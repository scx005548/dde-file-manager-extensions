// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "diskencrypt.h"
#include "fsresize/fsresize.h"
#include "notification/notifications.h"

#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QJsonDocument>
#include <QJsonObject>

#include <dfm-base/utils/finallyutil.h>
#include <dfm-mount/dmount.h>

#include <libcryptsetup.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <functional>

FILE_ENCRYPT_USE_NS
using namespace disk_encrypt;

#define CHECK_INT(checkVal, msg, retVal)   \
    if ((checkVal) < 0) {                  \
        qWarning() << (msg) << (checkVal); \
        return retVal;                     \
    }
#define CHECK_BOOL(checkVal, msg, retVal) \
    if (!(checkVal)) {                    \
        qWarning() << (msg);              \
        return retVal;                    \
    }

// used to record current reencrypting device.
QString gCurrReencryptingDevice;
QString gCurrDecryptintDevice;
bool gInterruptEncFlag { false };

struct crypt_params_reencrypt *encryptParams()
{
    static struct crypt_params_luks2 reencLuks2
    {
        .sector_size = 512
    };

    static struct crypt_params_reencrypt reencParams
    {
        .mode = CRYPT_REENCRYPT_ENCRYPT,
        .direction = CRYPT_REENCRYPT_BACKWARD,
        .resilience = "datashift",
        .hash = "sha256",
        .data_shift = 32 * 1024,
        .max_hotzone_size = 0,
        .device_size = 0,
        .luks2 = &reencLuks2,
        .flags = CRYPT_REENCRYPT_INITIALIZE_ONLY | CRYPT_REENCRYPT_MOVE_FIRST_SEGMENT
    };
    return &reencParams;
}
struct crypt_params_reencrypt *decryptParams()
{
    static struct crypt_params_reencrypt params
    {
        .mode = CRYPT_REENCRYPT_DECRYPT,
        .direction = CRYPT_REENCRYPT_BACKWARD,
        .resilience = "checksum",
        .hash = "sha256",
        .data_shift = 0,
        .max_hotzone_size = 0,
        .device_size = 0
    };
    return &params;
}
struct crypt_params_reencrypt *resumeParams()
{
    static struct crypt_params_reencrypt params
    {
        .mode = CRYPT_REENCRYPT_REENCRYPT,
        .direction = CRYPT_REENCRYPT_FORWARD,
        .resilience = "checksum",
        .hash = "sha256",
        .max_hotzone_size = 0,
        .device_size = 0,
        .flags = CRYPT_REENCRYPT_RESUME_ONLY
    };
    return &params;
}
void parseCipher(const QString &fullCipher, QString *cipher, QString *mode, int *len)
{
    Q_ASSERT(cipher && mode && len);
    *cipher = fullCipher;
    *mode = "xts-plain64";
    *len = 256;
}

EncryptParams disk_encrypt_utils::bcConvertParams(const QVariantMap &params)
{
    auto toString = [&params](const QString &key) { return params.value(key).toString(); };
    return {
        .device = toString(encrypt_param_keys::kKeyDevice),
        .passphrase = toString(encrypt_param_keys::kKeyPassphrase),   // decode()
        .cipher = toString(encrypt_param_keys::kKeyCipher),
        .recoveryPath = toString(encrypt_param_keys::kKeyRecoveryExportPath),
        .tpmToken = toString(encrypt_param_keys::kKeyTPMToken),
    };
}

bool disk_encrypt_utils::bcValidateParams(const EncryptParams &params)
{
    if (!params.isValid()) {
        qWarning() << "params is not valid!";
        return false;
    }

    // check whether device exists
    struct stat blkStat;
    if (stat(params.device.toStdString().c_str(), &blkStat) != 0) {
        int errCode = errno;
        qWarning() << "query stat of device failed:"
                   << params.device
                   << strerror(errCode)
                   << errCode;
        return false;
    }
    if (!S_ISBLK(blkStat.st_mode)) {
        qWarning() << "device is not a block!"
                   << params.device
                   << blkStat.st_mode;
        return false;
    }

    // check if is valid path.
    if (!params.recoveryPath.isEmpty()
        && access(params.recoveryPath.toStdString().c_str(), F_OK) != 0) {
        qWarning() << "recovery export path is not valid!"
                   << params.recoveryPath;
        return false;
    }

    return true;
}

QString disk_encrypt_utils::bcExpRecFile(const EncryptParams &params)
{
    if (params.recoveryPath.isEmpty())
        return "";

    while (1) {
        if (!QDir(params.recoveryPath).exists()) {
            qWarning() << "the recovery key path does not exists!"
                       << params.recoveryPath;
            break;
        }
        QString recKey = bcGenRecKey();
        if (recKey.isEmpty()) {
            qWarning() << "no recovery key generated, give up export.";
            break;
        }

        QString recFileName = QString("%1/%2_recovery_key.txt")
                                      .arg(params.recoveryPath)
                                      .arg(params.device.mid(5));
        QFile recFile(recFileName);
        if (!recFile.open(QIODevice::ReadWrite)) {
            qWarning() << "cannot create recovery file!";
            break;
        }

        recFile.write(recKey.toLocal8Bit());
        recFile.flush();
        recFile.close();

        return QString(recKey);
    }

    return "";
}

QString disk_encrypt_utils::bcGenRecKey()
{
    QString recKey;
    QLibrary lib("usec-recoverykey");
    dfmbase::FinallyUtil finalClear([&] { if (lib.isLoaded()) lib.unload(); });

    if (!lib.load()) {
        qWarning() << "libusec-recoverykey load failed. use uuid as recovery key";
        return recKey;
    }

    typedef int (*FnGenKey)(char *, const size_t, const size_t);
    FnGenKey fn = (FnGenKey)(lib.resolve("usec_get_recovery_key"));
    if (!fn) {
        qWarning() << "libusec-recoverykey resolve failed. use uuid as recovery key";
        return recKey;
    }

    static const size_t kRecoveryKeySize = 24;
    char genKey[kRecoveryKeySize + 1];
    int ret = fn(genKey, kRecoveryKeySize, 1);
    if (ret != 0) {
        qWarning() << "libusec-recoverykey generate failed. use uuid as recovery key";
        return recKey;
    }

    recKey = genKey;
    return recKey;
}

int disk_encrypt_funcs::bcInitHeaderFile(const EncryptParams &params,
                                         QString &headerPath, int *keyslotCipher, int *keyslotRecKey)
{
    if (!disk_encrypt_utils::bcValidateParams(params))
        return -kErrorParamsInvalid;

    auto status = block_device_utils::bcDevEncryptVersion(params.device);
    if (status != kNotEncrypted) {
        qWarning() << "cannot encrypt device:"
                   << params.device
                   << status;
        return -kErrorDeviceEncrypted;
    }

    if (block_device_utils::bcIsMounted(params.device)) {
        qWarning() << "device is already mounted, cannot encrypt";
        return -kErrorDeviceMounted;
    }

    int err = bcDoSetupHeader(params, &headerPath, keyslotCipher, keyslotRecKey);
    return err;
}

int disk_encrypt_funcs::bcDoSetupHeader(const EncryptParams &params, QString *headerPath, int *keyslotCipher, int *keyslotRecKey)
{
    Q_ASSERT(headerPath && keyslotCipher && keyslotRecKey);

    QString localPath;
    int ret = 0;
    ret = bcPrepareHeaderFile(params.device, &localPath);
    if (localPath.isEmpty())
        return -kErrorCreateHeader;

    fs_resize::shrinkFileSystem_ext(params.device);

    struct crypt_device *cdev { nullptr };

    dfmbase::FinallyUtil finalClear([&] {
        if (cdev) crypt_free(cdev);
        if (ret < 0) {
            ::remove(localPath.toStdString().c_str());
            fs_resize::expandFileSystem_ext(params.device);
        }
    });

    ret = crypt_init(&cdev, localPath.toStdString().c_str());
    CHECK_INT(ret, "init crypt failed " + params.device, -kErrorInitCrypt);

    crypt_set_rng_type(cdev, CRYPT_RNG_RANDOM);

    ret = crypt_set_data_offset(cdev, 32 * 1024);   // offset 32M
    CHECK_INT(ret, "cannot set offset " + params.device, -kErrorSetOffset);

    QString cipher, mode;
    int keyLen;
    parseCipher(params.cipher, &cipher, &mode, &keyLen);
    qDebug() << "encrypt with cipher:" << cipher << mode << keyLen;

    std::string cDevice = params.device.toStdString();
    struct crypt_params_luks2 luks2Params = {
        .data_alignment = 0,
        .data_device = cDevice.c_str(),
        .sector_size = 512,
        .label = nullptr,
        .subsystem = nullptr
    };
    ret = crypt_format(cdev,
                       CRYPT_LUKS2,
                       cipher.toStdString().c_str(),
                       mode.toStdString().c_str(),
                       nullptr,
                       nullptr,
                       keyLen / 8,
                       &luks2Params);
    CHECK_INT(ret, "format failed " + params.device, -kErrorFormatLuks);

    ret = crypt_keyslot_add_by_volume_key(cdev,
                                          CRYPT_ANY_SLOT,
                                          nullptr,
                                          0,
                                          params.passphrase.toStdString().c_str(),
                                          params.passphrase.length());
    CHECK_INT(ret, "add key failed " + params.device, -kErrorAddKeyslot);
    *keyslotCipher = ret;

    QString recKey = disk_encrypt_utils::bcExpRecFile(params);
    if (!recKey.isEmpty()) {
        ret = crypt_keyslot_add_by_volume_key(cdev,
                                              CRYPT_ANY_SLOT,
                                              nullptr,
                                              0,
                                              recKey.toStdString().c_str(),
                                              recKey.length());
        if (ret < 0) {
            qWarning() << "add recovery key failed:"
                       << params.device
                       << ret;
        }
        *keyslotRecKey = ret;
    }

    ret = crypt_reencrypt_init_by_passphrase(cdev,
                                             nullptr,
                                             params.passphrase.toStdString().c_str(),
                                             params.passphrase.length(),
                                             CRYPT_ANY_SLOT,
                                             0,
                                             cipher.toStdString().c_str(),
                                             mode.toStdString().c_str(),
                                             encryptParams());
    CHECK_INT(ret, "init reencryption failed " + params.device, -kErrorInitReencrypt);

    // active device for expanding fs.
    QString activeDev = QString("dm-%1").arg(params.device.mid(5));
    ret = crypt_activate_by_passphrase(cdev,
                                       activeDev.toStdString().c_str(),
                                       CRYPT_ANY_SLOT,
                                       params.passphrase.toStdString().c_str(),
                                       params.passphrase.length(),
                                       CRYPT_ACTIVATE_NO_JOURNAL);
    CHECK_INT(ret, "acitve device failed " + params.device + activeDev, -kErrorActive);

    fs_resize::expandFileSystem_ext(QString("/dev/mapper/%1").arg(activeDev));
    ret = crypt_deactivate(nullptr, activeDev.toStdString().c_str());
    CHECK_INT(ret, "deacitvi device failed " + params.device, -kErrorDeactive);

    *headerPath = localPath;
    return kSuccess;
}

int disk_encrypt_funcs::bcInitHeaderDevice(const QString &device,
                                           const QString &passphrase,
                                           const QString &headerPath)
{
    Q_ASSERT_X(!headerPath.isEmpty() && !device.isEmpty(),
               "input params cannot be empty!", "");

    struct crypt_device *cdev { nullptr };
    dfmbase::FinallyUtil finalClear([&] {
        if (cdev) crypt_free(cdev);
        if (!headerPath.isEmpty()) ::remove(headerPath.toStdString().c_str());
    });

    int ret = crypt_init(&cdev, device.toStdString().c_str());
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_header_restore(cdev,
                               CRYPT_LUKS2,
                               headerPath.toStdString().c_str());
    CHECK_INT(ret, "restore header failed " + device + headerPath, -kErrorRestoreFromFile);
    return kSuccess;
}

int disk_encrypt_funcs::bcPrepareHeaderFile(const QString &device, QString *headerPath)
{
    Q_ASSERT(headerPath);
    QString localPath = QString("/tmp/%1_luks2_pre_enc").arg(device.mid(5));
    int fd = open(localPath.toStdString().c_str(),
                  O_CREAT | O_EXCL | O_WRONLY,
                  S_IRUSR | S_IWUSR);
    CHECK_INT(fd, "create tmp file failed " + device + strerror(errno), -kErrorOpenFileFailed);

    int ret = posix_fallocate(fd, 0, 32 * 1024 * 1024);
    close(fd);
    CHECK_BOOL(ret == 0, "allocate file failed " + localPath, -kErrorCreateHeader);
    *headerPath = localPath;
    return kSuccess;
}

int disk_encrypt_funcs::bcDecryptDevice(const QString &device,
                                        const QString &passphrase)
{
    // backup header first
    QString headerPath;
    uint32_t flags;
    struct crypt_device *cdev = nullptr;
    dfmbase::FinallyUtil finalClear([&] {
        if (cdev) crypt_free(cdev);
        if (!headerPath.isEmpty()) ::remove(headerPath.toStdString().c_str());
        gCurrDecryptintDevice.clear();
    });
    gCurrDecryptintDevice = device;

    int ret = bcBackupCryptHeader(device, headerPath);
    CHECK_INT(ret, "backup header failed " + device, -kErrorBackupHeader);

    ret = crypt_init_data_device(&cdev,
                                 headerPath.toStdString().c_str(),
                                 device.toStdString().c_str());
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_load(cdev, CRYPT_LUKS, nullptr);
    CHECK_INT(ret, "load device failed " + device, -kErrorLoadCrypt);

    ret = crypt_persistent_flags_get(cdev,
                                     CRYPT_FLAGS_REQUIREMENTS,
                                     &flags);
    CHECK_INT(ret, "get device flag failed " + device, -kErrorGetReencryptFlag);
    bool underEncrypting = (flags & CRYPT_REQUIREMENT_OFFLINE_REENCRYPT) || (flags & CRYPT_REQUIREMENT_ONLINE_REENCRYPT);
    CHECK_BOOL(!underEncrypting,
               "device is under encrypting... " + device + " the flags are: " + QString::number(flags),
               -kErrorWrongFlags);

    ret = crypt_reencrypt_init_by_passphrase(cdev,
                                             nullptr,
                                             passphrase.toStdString().c_str(),
                                             passphrase.length(),
                                             CRYPT_ANY_SLOT,
                                             CRYPT_ANY_SLOT,
                                             nullptr,
                                             nullptr,
                                             decryptParams());
    CHECK_INT(ret, "init reencrypt failed " + device, -kErrorWrongPassphrase);

    ret = crypt_reencrypt(cdev, bcDecryptProgress);
    CHECK_INT(ret, "decrypt failed" + device, -kErrorReencryptFailed);

    bool res = fs_resize::recoverySuperblock_ext(device, headerPath);
    CHECK_BOOL(res, "recovery fs failed " + device, -kErrorResizeFs);
    return 0;
}

int disk_encrypt_funcs::bcBackupCryptHeader(const QString &device, QString &headerPath)
{
    headerPath = "/tmp/dm_header_" + device.mid(5);
    struct crypt_device *cdev = nullptr;
    dfmbase::FinallyUtil finalClear([&] { if (cdev) crypt_free(cdev); });

    int ret = crypt_init(&cdev, device.toStdString().c_str());
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_header_backup(cdev,
                              nullptr,
                              headerPath.toStdString().c_str());
    CHECK_INT(ret, "backup header failed " + device, -kErrorBackupHeader);
    return 0;
}

int disk_encrypt_funcs::bcResumeReencrypt(const QString &device,
                                          const QString &passphrase,
                                          const QString &clearDev,
                                          bool expandFs)
{
    qDebug() << "start resume encryption for device"
             << device;
    gCurrReencryptingDevice = device;
    struct crypt_device *cdev { nullptr };
    dfmbase::FinallyUtil finalClear([&] {
        if (cdev) crypt_free(cdev);
        gCurrDecryptintDevice.clear();
    });

    int ret = crypt_init_data_device(&cdev,
                                     device.toStdString().c_str(),
                                     device.toStdString().c_str());
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_load(cdev, CRYPT_LUKS, nullptr);
    CHECK_INT(ret, "load device failed " + device, -kErrorLoadCrypt);

    uint32_t flags;
    ret = crypt_persistent_flags_get(cdev,
                                     CRYPT_FLAGS_REQUIREMENTS,
                                     &flags);
    CHECK_INT(ret, "read flags failed " + device, -kErrorGetReencryptFlag);
    CHECK_BOOL(flags & CRYPT_REQUIREMENT_ONLINE_REENCRYPT,
               "wrong flags " + device + " flags " + QString::number(flags),
               -kErrorWrongFlags);

    std::string _clearDev = clearDev.toStdString();
    const char *__clearDev = clearDev.isEmpty() ? nullptr : _clearDev.c_str();
    ret = crypt_reencrypt_init_by_passphrase(cdev,
                                             __clearDev,
                                             passphrase.toStdString().c_str(),
                                             passphrase.length(),
                                             CRYPT_ANY_SLOT,
                                             CRYPT_ANY_SLOT,
                                             nullptr,
                                             nullptr,
                                             resumeParams());
    CHECK_INT(ret, "init reencrypt failed " + device, -kErrorInitReencrypt);

    ret = crypt_reencrypt(cdev, bcEncryptProgress);
    CHECK_INT(ret, "start resume failed " + device, -kErrorReencryptFailed);

    if (!expandFs)
        return kSuccess;

    // active device for expanding fs.
    QString activeDev = QString("dm-%1").arg(device.mid(5));
    ret = crypt_activate_by_passphrase(cdev,
                                       activeDev.toStdString().c_str(),
                                       CRYPT_ANY_SLOT,
                                       passphrase.toStdString().c_str(),
                                       passphrase.length(),
                                       CRYPT_ACTIVATE_NO_JOURNAL);
    CHECK_INT(ret, "acitve device failed " + device + activeDev, -kErrorActive);

    fs_resize::expandFileSystem_ext(QString("/dev/mapper/%1").arg(activeDev));

    ret = crypt_deactivate(nullptr,
                           activeDev.toStdString().c_str());
    CHECK_INT(ret, "deacitvi device failed " + device, -kErrorDeactive);
    return kSuccess;
}

int disk_encrypt_funcs::bcEncryptProgress(uint64_t size, uint64_t offset, void *)
{
    Q_EMIT SignalEmitter::instance()->updateEncryptProgress(gCurrReencryptingDevice,
                                                            double(offset) / size);
    return 0;
}

int disk_encrypt_funcs::bcDecryptProgress(uint64_t size, uint64_t offset, void *)
{
    Q_EMIT SignalEmitter::instance()->updateDecryptProgress(gCurrDecryptintDevice,
                                                            double(offset) / size);
    return 0;
}

int disk_encrypt_funcs::bcChangePassphrase(const QString &device, const QString &oldPassphrase, const QString &newPassphrase, int *keyslot)
{
    struct crypt_device *cdev { nullptr };
    dfmbase::FinallyUtil finalClear([&] {if (cdev) crypt_free(cdev); });

    int ret = crypt_init_data_device(&cdev, device.toStdString().c_str(), nullptr);
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_load(cdev, CRYPT_LUKS, nullptr);
    CHECK_INT(ret, "load device failed " + device, -kErrorLoadCrypt);

    ret = crypt_keyslot_change_by_passphrase(cdev,
                                             CRYPT_ANY_SLOT,
                                             CRYPT_ANY_SLOT,
                                             oldPassphrase.toStdString().c_str(),
                                             oldPassphrase.length(),
                                             newPassphrase.toStdString().c_str(),
                                             newPassphrase.length());
    CHECK_INT(ret, "change passphrase failed " + device, -kErrorChangePassphraseFailed);
    Q_ASSERT(keyslot);
    *keyslot = ret;
    return kSuccess;
}

int disk_encrypt_funcs::bcChangePassphraseByRecKey(const QString &device, const QString &recoveryKey, const QString &newPassphrase, int *keyslot)
{
    struct crypt_device *cdev { nullptr };
    dfmbase::FinallyUtil finalClear([&] {if (cdev) crypt_free(cdev); });

    int ret = crypt_init_data_device(&cdev,
                                     device.toStdString().c_str(),
                                     /*device.toStdString().c_str()*/ nullptr);
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_load(cdev, CRYPT_LUKS, nullptr);
    CHECK_INT(ret, "load device failed " + device, -kErrorLoadCrypt);

    ret = crypt_keyslot_add_by_passphrase(cdev,
                                          CRYPT_ANY_SLOT,
                                          recoveryKey.toStdString().c_str(),
                                          recoveryKey.length(),
                                          newPassphrase.toStdString().c_str(),
                                          newPassphrase.length());
    CHECK_INT(ret, "change passphrase by rec key failed " + device, -kErrorAddKeyslot);
    Q_ASSERT(keyslot);
    *keyslot = ret;
    return kSuccess;
}

int disk_encrypt_funcs::bcGetToken(const QString &device, QString *tokenJson)
{
    Q_ASSERT(tokenJson);
    struct crypt_device *cdev { nullptr };
    dfmbase::FinallyUtil finalClear([&] {if (cdev) crypt_free(cdev); });

    int ret = crypt_init(&cdev,
                         device.toStdString().c_str());
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_load(cdev, CRYPT_LUKS, nullptr);
    CHECK_INT(ret, "load device failed " + device, -kErrorLoadCrypt);

    for (int i = 0; i < 32 /* LUKS2_TOKENS_MAX */; ++i) {
        const char *token { nullptr };
        if ((ret = crypt_token_json_get(cdev, i, &token)) < 0)
            continue;
        QString json(token);
        if (json.contains("usec-tpm2")) {
            // qInfo() << "found token:" << json;
            QJsonDocument doc = QJsonDocument::fromJson(token);
            QJsonObject obj = doc.object();
            obj.insert("token_index", i);
            doc.setObject(obj);
            *tokenJson = doc.toJson();
            return kSuccess;
        }
    }

    qInfo() << "token not found." << device;
    return kSuccess;
}

int disk_encrypt_funcs::bcSetToken(const QString &device, const QString &token)
{
    if (token.isEmpty())
        return 0;

    QJsonDocument doc = QJsonDocument::fromJson(token.toLocal8Bit());
    QJsonObject obj = doc.object();
    int tokenIndex = obj.value("token_index").toInt(CRYPT_ANY_TOKEN);

    struct crypt_device *cdev { nullptr };
    dfmbase::FinallyUtil finalClear([&] {if (cdev) crypt_free(cdev); });

    int ret = crypt_init(&cdev,
                         device.toStdString().c_str());
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_load(cdev, CRYPT_LUKS, nullptr);
    CHECK_INT(ret, "load device failed " + device, -kErrorLoadCrypt);

    ret = crypt_token_json_set(cdev,
                               tokenIndex,
                               token.toStdString().c_str());
    CHECK_INT(ret, "set token failed " + device, -kErrorSetTokenFailed);
    return 0;
}

EncryptVersion block_device_utils::bcDevEncryptVersion(const QString &device)
{
    auto blkDev = block_device_utils::bcCreateBlkDev(device);
    if (!blkDev) {
        qWarning() << "cannot create block device handler:"
                   << device;
        return kVersionUnknown;
    }

    const QString &idType = blkDev->getProperty(dfmmount::Property::kBlockIDType).toString();
    const QString &idVersion = blkDev->getProperty(dfmmount::Property::kBlockIDVersion).toString();

    if (idType == "crypto_LUKS") {
        if (idVersion == "1")
            return kVersionLUKS1;
        if (idVersion == "2")
            return kVersionLUKS2;
        return kVersionLUKSUnknown;
    }

    if (blkDev->isEncrypted())
        return kVersionUnknown;

    // TODO: this should be completed, not only LUKS encrypt.

    return kNotEncrypted;
}

DevPtr block_device_utils::bcCreateBlkDev(const QString &device)
{
    auto mng = dfmmount::DDeviceManager::instance();
    Q_ASSERT_X(mng, "cannot create device manager", "");
    auto blkMonitor = mng->getRegisteredMonitor(dfmmount::DeviceType::kBlockDevice)
                              .objectCast<dfmmount::DBlockMonitor>();
    Q_ASSERT_X(blkMonitor, "cannot get valid device monitor", "");

    auto blkDevs = blkMonitor->resolveDeviceNode(device, {});
    if (blkDevs.isEmpty()) {
        qWarning() << "cannot resolve device from" << device;
        return nullptr;
    }

    auto blkDev = blkMonitor->createDeviceById(blkDevs.constFirst());
    if (!blkDev) {
        qWarning() << "cannot create device by" << blkDevs.constFirst();
        return nullptr;
    }
    return blkDev.objectCast<dfmmount::DBlockDevice>();
}

bool block_device_utils::bcIsMounted(const QString &device)
{
    auto blkDev = block_device_utils::bcCreateBlkDev(device);
    if (!blkDev) {
        qWarning() << "cannot create block device handler:"
                   << device;
        return false;
    }
    return !blkDev->mountPoints().isEmpty();
}

int block_device_utils::bcDevEncryptStatus(const QString &device, EncryptStatus *status)
{
    Q_ASSERT(status);

    struct crypt_device *cdev { nullptr };
    dfmbase::FinallyUtil finalClear([&] {if (cdev) crypt_free(cdev); });

    int ret = crypt_init(&cdev,
                         device.toStdString().c_str());
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_load(cdev, CRYPT_LUKS, nullptr);
    CHECK_INT(ret, "load device failed " + device, -kErrorLoadCrypt);

    uint32_t flags;
    ret = crypt_persistent_flags_get(cdev,
                                     CRYPT_FLAGS_REQUIREMENTS,
                                     &flags);
    CHECK_INT(ret, "get device flag failed " + device, -kErrorGetReencryptFlag);

    *status = kStatusFinished;
    if (flags & CRYPT_REQUIREMENT_OFFLINE_REENCRYPT)
        *status = kStatusOfflineUnfinished;
    if (flags & CRYPT_REQUIREMENT_ONLINE_REENCRYPT)
        *status = kStatusOnlineUnfinished;
    if (flags & CRYPT_REQUIREMENT_UNKNOWN)
        *status = kStatusUnknown;
    return kSuccess;
}

int disk_encrypt_funcs::bcSetLabel(const QString &device, const QString &label)
{

    struct crypt_device *cdev { nullptr };
    dfmbase::FinallyUtil finalClear([&] {if (cdev) crypt_free(cdev); });

    int ret = crypt_init(&cdev,
                         device.toStdString().c_str());
    CHECK_INT(ret, "init device failed " + device, -kErrorInitCrypt);

    ret = crypt_load(cdev, CRYPT_LUKS, nullptr);
    CHECK_INT(ret, "load device failed " + device, -kErrorLoadCrypt);

    ret = crypt_set_label(cdev, label.toStdString().c_str(), nullptr);
    CHECK_INT(ret, "set label failed " + device, -kErrorSetLabel);

    return kSuccess;
}

bool disk_encrypt_utils::bcReadEncryptConfig(disk_encrypt::EncryptConfig *config)
{
    Q_ASSERT(config);

    QFile encConfig(kEncConfigPath);
    if (!encConfig.exists()) {
        qInfo() << "the encrypt config file doesn't exist";
        return false;
    }

    if (!encConfig.open(QIODevice::ReadOnly)) {
        qWarning() << "encrypt config file open failed!";
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(encConfig.readAll());
    encConfig.close();
    auto obj = doc.object();

    config->cipher = obj.value("cipher").toString();
    config->device = obj.value("device").toString();
    config->mountPoint = obj.value("device-mountpoint").toString();
    config->deviceName = obj.value("device-name").toString();
    config->devicePath = obj.value("device-path").toString();
    config->keySize = obj.value("key-size").toString();
    config->mode = obj.value("mode").toString();
    config->recoveryPath = obj.value("recoverykey-path").toString();
    // config->tpmConfig = obj.value("tpm-config");// no tpmconfig will be set in pre-encrypt phase
    config->clearDev = obj.value("volume").toString();

    return true;
}
