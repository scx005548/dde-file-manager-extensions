// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dfmplugin_disk_encrypt_global.h"
#include "diskencryptmenuscene.h"
#include "gui/encryptparamsinputdialog.h"
#include "gui/decryptparamsinputdialog.h"
#include "gui/chgpassphrasedialog.h"
#include "events/eventshandler.h"
#include "utils/encryptutils.h"

#include <dfm-base/dfm_menu_defines.h>
#include <dfm-base/base/schemefactory.h>
#include <dfm-base/interfaces/fileinfo.h>

#include <dfm-mount/dmount.h>

#include <QDebug>
#include <QMenu>
#include <QProcess>
#include <QFile>
#include <QStringList>
#include <QDBusInterface>
#include <QDBusReply>
#include <QApplication>

#include <ddialog.h>
#include <dconfig.h>

#include <fstab.h>

DFMBASE_USE_NAMESPACE
using namespace dfmplugin_diskenc;
using namespace disk_encrypt;

static constexpr char kActIDEncrypt[] { "de_0_encrypt" };
static constexpr char kActIDUnlock[] { "de_0_unlock" };
static constexpr char kActIDDecrypt[] { "de_1_decrypt" };
static constexpr char kActIDChangePwd[] { "de_2_changePwd" };

DiskEncryptMenuScene::DiskEncryptMenuScene(QObject *parent)
    : AbstractMenuScene(parent)
{
}

dfmbase::AbstractMenuScene *DiskEncryptMenuCreator::create()
{
    return new DiskEncryptMenuScene();
}

QString DiskEncryptMenuScene::name() const
{
    return DiskEncryptMenuCreator::name();
}

bool DiskEncryptMenuScene::initialize(const QVariantHash &params)
{
    QList<QUrl> selectedItems = params.value(MenuParamKey::kSelectFiles).value<QList<QUrl>>();
    if (selectedItems.isEmpty())
        return false;

    auto selectedItem = selectedItems.first();
    if (!selectedItem.path().endsWith("blockdev"))
        return false;

    QSharedPointer<FileInfo> info = InfoFactory::create<FileInfo>(selectedItem);
    if (!info)
        return false;
    info->refresh();

    selectedItemInfo = info->extraProperties();
    auto device = selectedItemInfo.value("Device", "").toString();
    if (device.isEmpty())
        return false;

    auto preferDev = selectedItemInfo.value("PreferredDevice", "").toString();
    if (preferDev.startsWith("/dev/mapper/") || device.startsWith("/dev/dm-")) {
        qInfo() << "mapper device is not supported to be encrypted yet." << device << preferDev;
        return false;
    }

    const QString &idType = selectedItemInfo.value("IdType").toString();
    const QStringList &supportedFS { "ext4", "ext3", "ext2" };
    if (idType == "crypto_LUKS") {
        if (selectedItemInfo.value("IdVersion").toString() == "1")
            return false;
        itemEncrypted = true;
    } else if (!supportedFS.contains(idType)) {
        return false;
    }

    QString devMpt = selectedItemInfo.value("MountPoint", "").toString();
    if (devMpt.isEmpty() && selectedItemInfo.contains("ClearBlockDeviceInfo"))
        devMpt = selectedItemInfo.value("ClearBlockDeviceInfo").toHash().value("MountPoint").toString();

    if (kDisabledEncryptPath.contains(devMpt, Qt::CaseInsensitive)) {
        qInfo() << devMpt << "doesn't support encrypt";
        return false;
    }

    selectionMounted = !devMpt.isEmpty();
    param.devDesc = device;
    param.initOnly = fstab_utils::isFstabItem(devMpt);
    param.mountPoint = devMpt;
    param.uuid = selectedItemInfo.value("IdUUID", "").toString();
    param.deviceDisplayName = info->displayOf(dfmbase::FileInfo::kFileDisplayName);
    param.type = SecKeyType::kPasswordOnly;
    param.backingDevUUID = param.uuid;
    param.clearDevUUID = selectedItemInfo.value("ClearBlockDeviceInfo").toHash().value("IdUUID", "").toString();

    if (itemEncrypted)
        param.type = static_cast<SecKeyType>(device_utils::encKeyType(device));

    return true;
}

bool DiskEncryptMenuScene::create(QMenu *)
{
    bool hasJob = EventsHandler::instance()->hasEnDecryptJob();
    if (itemEncrypted) {
        QAction *act = nullptr;

        act = new QAction(tr("Unlock encrypted partition"));
        act->setProperty(ActionPropertyKey::kActionID, kActIDUnlock);
        actions.insert(kActIDUnlock, act);

        act = new QAction(tr("Cancel partition encryption"));
        act->setProperty(ActionPropertyKey::kActionID, kActIDDecrypt);
        actions.insert(kActIDDecrypt, act);
        act->setEnabled(!hasJob);

        if (param.type == kTPMOnly)
            return true;

        QString keyType = tr("passphrase");
        if (param.type == kTPMAndPIN)
            keyType = "PIN";

        act = new QAction(tr("Changing the encryption %1").arg(keyType));
        act->setProperty(ActionPropertyKey::kActionID, kActIDChangePwd);
        actions.insert(kActIDChangePwd, act);
    } else {
        QAction *act = new QAction(tr("Enable partition encryption"));
        act->setProperty(ActionPropertyKey::kActionID, kActIDEncrypt);
        actions.insert(kActIDEncrypt, act);
        act->setEnabled(!hasJob);
    }

    return true;
}

bool DiskEncryptMenuScene::triggered(QAction *action)
{
    QString actID = action->property(ActionPropertyKey::kActionID).toString();

    if (actID == kActIDEncrypt)
        param.initOnly ? doEncryptDevice(param) : unmountBefore(encryptDevice);
    else if (actID == kActIDDecrypt)
        param.initOnly ? doDecryptDevice(param) : unmountBefore(deencryptDevice);
    else if (actID == kActIDChangePwd)
        changePassphrase(param);
    else if (actID == kActIDUnlock)
        unlockDevice(selectedItemInfo.value("Id").toString());
    else
        return false;
    return true;
}

void DiskEncryptMenuScene::updateState(QMenu *parent)
{
    Q_ASSERT(parent);
    QList<QAction *> acts = parent->actions();
    QAction *before { nullptr };
    for (int i = 0; i < acts.count(); ++i) {
        auto act = acts.at(i);
        QString actID = act->property(ActionPropertyKey::kActionID).toString();
        if (actID == "computer-rename"   // the encrypt actions should be under computer-rename
            && (i + 1) < acts.count()) {
            before = acts.at(i + 1);
            break;
        }
    }

    if (!before)
        before = acts.last();

    std::for_each(actions.begin(), actions.end(), [=](QAction *val) {
        parent->insertAction(before, val);
        val->setParent(parent);

        if (val->property(ActionPropertyKey::kActionID).toString() == kActIDUnlock
            && selectionMounted)
            val->setVisible(false);
    });
}

void DiskEncryptMenuScene::encryptDevice(const DeviceEncryptParam &param)
{
    EncryptParamsInputDialog dlg(param, qApp->activeWindow());
    int ret = dlg.exec();
    if (ret == QDialog::Accepted) {
        auto inputs = dlg.getInputs();
        doEncryptDevice(inputs);
    }
}

void DiskEncryptMenuScene::deencryptDevice(const DeviceEncryptParam &param)
{
    auto inputs = param;
    if (inputs.type == kTPMOnly) {
        QString passphrase = tpm_passphrase_utils::getPassphraseFromTPM(inputs.devDesc, "");
        inputs.key = passphrase;
        if (passphrase.isEmpty()) {
            dialog_utils::showDialog(tr("Error"),
                                     tr("Cannot resolve passphrase from TPM"),
                                     dialog_utils::DialogType::kError);
            return;
        }
        doDecryptDevice(inputs);
        return;
    }

    DecryptParamsInputDialog dlg(inputs.devDesc);
    if (inputs.type == kTPMAndPIN)
        dlg.setInputPIN(true);

    if (dlg.exec() != QDialog::Accepted)
        return;

    qDebug() << "start decrypting device" << inputs.devDesc;
    inputs.key = dlg.getKey();
    if (dlg.usingRecKey() || inputs.type == kPasswordOnly)
        doDecryptDevice(inputs);
    else {
        inputs.key = tpm_passphrase_utils::getPassphraseFromTPM(inputs.devDesc, inputs.key);
        if (inputs.key.isEmpty()) {
            dialog_utils::showDialog(tr("Error"), tr("PIN error"), dialog_utils::DialogType::kError);
            return;
        }
        doDecryptDevice(inputs);
    }
}

void DiskEncryptMenuScene::changePassphrase(DeviceEncryptParam param)
{
    QString dev = param.devDesc;
    ChgPassphraseDialog dlg(param.devDesc);
    if (dlg.exec() != 1)
        return;

    auto inputs = dlg.getPassphrase();
    QString oldKey = inputs.first;
    QString newKey = inputs.second;
    if (param.type == SecKeyType::kTPMAndPIN) {
        if (!dlg.validateByRecKey()) {
            oldKey = tpm_passphrase_utils::getPassphraseFromTPM(dev, oldKey);
            if (oldKey.isEmpty()) {
                dialog_utils::showDialog(tr("Error"), tr("PIN error"), dialog_utils::DialogType::kError);
                return;
            }
        }
        QString newPassphrase;
        int ret = tpm_passphrase_utils::genPassphraseFromTPM(dev, newKey, &newPassphrase);
        if (ret != tpm_passphrase_utils::kTPMNoError) {
            dialog_utils::showTPMError(tr("Change passphrase failed"), static_cast<tpm_passphrase_utils::TPMError>(ret));
            return;
        }
        newKey = newPassphrase;
    }
    param.validateByRecKey = dlg.validateByRecKey();
    param.key = oldKey;
    param.newKey = newKey;
    doChangePassphrase(param);
}

void DiskEncryptMenuScene::unlockDevice(const QString &devObjPath)
{
    auto blkDev = device_utils::createBlockDevice(devObjPath);
    if (!blkDev)
        return;

    QString pwd;
    bool cancel { false };
    bool ok = EventsHandler::instance()->onAcquireDevicePwd(blkDev->device(), &pwd, &cancel);
    if (pwd.isEmpty() && ok) {
        qWarning() << "acquire pwd faield!!!";
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    blkDev->unlockAsync(pwd, {}, onUnlocked);
}

void DiskEncryptMenuScene::doEncryptDevice(const DeviceEncryptParam &param)
{
    // if tpm selected, use tpm to generate the key
    QString tpmConfig, tpmToken;
    if (param.type != kPasswordOnly) {
        tpmConfig = generateTPMConfig();
        tpmToken = generateTPMToken(param.devDesc, param.type == kTPMAndPIN);
    }

    QDBusInterface iface(kDaemonBusName,
                         kDaemonBusPath,
                         kDaemonBusIface,
                         QDBusConnection::systemBus());
    if (iface.isValid()) {
        QVariantMap params {
            { encrypt_param_keys::kKeyDevice, param.devDesc },
            { encrypt_param_keys::kKeyUUID, param.uuid },
            { encrypt_param_keys::kKeyCipher, config_utils::cipherType() },
            { encrypt_param_keys::kKeyPassphrase, param.key },
            { encrypt_param_keys::kKeyInitParamsOnly, param.initOnly },
            { encrypt_param_keys::kKeyRecoveryExportPath, param.exportPath },
            { encrypt_param_keys::kKeyEncMode, static_cast<int>(param.type) },
            { encrypt_param_keys::kKeyDeviceName, param.deviceDisplayName },
            { encrypt_param_keys::kKeyMountPoint, param.mountPoint }
        };
        if (!tpmConfig.isEmpty()) params.insert(encrypt_param_keys::kKeyTPMConfig, tpmConfig);
        if (!tpmToken.isEmpty()) params.insert(encrypt_param_keys::kKeyTPMToken, tpmToken);

        QDBusReply<QString> reply = iface.call("PrepareEncryptDisk", params);
        qDebug() << "preencrypt device jobid:" << reply.value();
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }
}

void DiskEncryptMenuScene::doReencryptDevice(const DeviceEncryptParam &param)
{
    // if tpm selected, use tpm to generate the key
    QString tpmConfig, tpmToken;
    if (param.type != kPasswordOnly) {
        tpmConfig = generateTPMConfig();
        tpmToken = generateTPMToken(param.devDesc, param.type == kTPMAndPIN);
    }

    QDBusInterface iface(kDaemonBusName,
                         kDaemonBusPath,
                         kDaemonBusIface,
                         QDBusConnection::systemBus());
    if (iface.isValid()) {
        QVariantMap params {
            { encrypt_param_keys::kKeyDevice, param.devDesc },
            { encrypt_param_keys::kKeyUUID, param.uuid },
            { encrypt_param_keys::kKeyCipher, config_utils::cipherType() },
            { encrypt_param_keys::kKeyPassphrase, param.key },
            { encrypt_param_keys::kKeyInitParamsOnly, param.initOnly },
            { encrypt_param_keys::kKeyRecoveryExportPath, param.exportPath },
            { encrypt_param_keys::kKeyEncMode, static_cast<int>(param.type) },
            { encrypt_param_keys::kKeyDeviceName, param.deviceDisplayName },
            { encrypt_param_keys::kKeyMountPoint, param.mountPoint },
            { encrypt_param_keys::kKeyBackingDevUUID, param.backingDevUUID },
            { encrypt_param_keys::kKeyClearDevUUID, param.clearDevUUID }
        };
        if (!tpmConfig.isEmpty()) params.insert(encrypt_param_keys::kKeyTPMConfig, tpmConfig);
        if (!tpmToken.isEmpty()) params.insert(encrypt_param_keys::kKeyTPMToken, tpmToken);

        QDBusReply<void> reply = iface.call("SetEncryptParams", params);
        qDebug() << "start reencrypt device";
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }
}

void DiskEncryptMenuScene::doDecryptDevice(const DeviceEncryptParam &param)
{
    // if tpm selected, use tpm to generate the key
    QDBusInterface iface(kDaemonBusName,
                         kDaemonBusPath,
                         kDaemonBusIface,
                         QDBusConnection::systemBus());
    if (iface.isValid()) {
        QVariantMap params {
            { encrypt_param_keys::kKeyDevice, param.devDesc },
            { encrypt_param_keys::kKeyPassphrase, param.key },
            { encrypt_param_keys::kKeyInitParamsOnly, param.initOnly },
            { encrypt_param_keys::kKeyUUID, param.uuid },
            { encrypt_param_keys::kKeyDeviceName, param.deviceDisplayName }
        };
        QDBusReply<QString> reply = iface.call("DecryptDisk", params);
        qDebug() << "preencrypt device jobid:" << reply.value();
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }
}

void DiskEncryptMenuScene::doChangePassphrase(const DeviceEncryptParam &param)
{
    QString token;
    if (param.type != SecKeyType::kPasswordOnly) {
        // new tpm token should be setted.
        QFile f(kGlobalTPMConfigPath + param.devDesc + "/token.json");
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "cannot read old tpm token!!!";
            return;
        }
        QJsonDocument oldTokenDoc = QJsonDocument::fromJson(f.readAll());
        f.close();
        QJsonObject oldTokenObj = oldTokenDoc.object();

        QString newToken = generateTPMToken(param.devDesc, param.type == SecKeyType::kTPMAndPIN);
        QJsonDocument newTokenDoc = QJsonDocument::fromJson(newToken.toLocal8Bit());
        QJsonObject newTokenObj = newTokenDoc.object();

        oldTokenObj.insert("enc", newTokenObj.value("enc"));
        oldTokenObj.insert("kek-priv", newTokenObj.value("kek-priv"));
        oldTokenObj.insert("kek-pub", newTokenObj.value("kek-pub"));
        oldTokenObj.insert("iv", newTokenObj.value("iv"));
        newTokenDoc.setObject(oldTokenObj);
        token = newTokenDoc.toJson(QJsonDocument::Compact);
    }

    QDBusInterface iface(kDaemonBusName,
                         kDaemonBusPath,
                         kDaemonBusIface,
                         QDBusConnection::systemBus());
    if (iface.isValid()) {
        QVariantMap params {
            { encrypt_param_keys::kKeyDevice, param.devDesc },
            { encrypt_param_keys::kKeyPassphrase, param.newKey },
            { encrypt_param_keys::kKeyOldPassphrase, param.key },
            { encrypt_param_keys::kKeyValidateWithRecKey, param.validateByRecKey },
            { encrypt_param_keys::kKeyTPMToken, token },
            { encrypt_param_keys::kKeyDeviceName, param.deviceDisplayName }
        };
        QDBusReply<QString> reply = iface.call("ChangeEncryptPassphress", params);
        qDebug() << "modify device passphrase jobid:" << reply.value();
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }
}

QString DiskEncryptMenuScene::generateTPMConfig()
{
    QString sessionHashAlgo, sessionKeyAlgo, primaryHashAlgo, primaryKeyAlgo, minorHashAlgo, minorKeyAlgo;
    if (!tpm_passphrase_utils::getAlgorithm(&sessionHashAlgo, &sessionKeyAlgo, &primaryHashAlgo, &primaryKeyAlgo, &minorHashAlgo, &minorKeyAlgo)) {
        qWarning() << "cannot choose algorithm for tpm";
        primaryHashAlgo = "sha256";
        primaryKeyAlgo = "ecc";
    }

    QJsonObject tpmParams;
    tpmParams = { { "keyslot", "1" },
                  { "session-key-alg", sessionKeyAlgo },
                  { "session-hash-alg", sessionHashAlgo },
                  { "primary-key-alg", primaryKeyAlgo },
                  { "primary-hash-alg", primaryHashAlgo },
                  { "pcr", "7" },
                  { "pcr-bank", primaryHashAlgo } };
    return QJsonDocument(tpmParams).toJson();
}

QString DiskEncryptMenuScene::generateTPMToken(const QString &device, bool pin)
{
    QString tpmConfig = generateTPMConfig();
    QJsonDocument doc = QJsonDocument::fromJson(tpmConfig.toLocal8Bit());
    QJsonObject token = doc.object();

    // keep same with usec.
    // https://gerrit.uniontech.com/plugins/gitiles/usec-crypt-kit/+/refs/heads/master/src/boot-crypt/util.cpp
    // j["type"] = "usec-tpm2";
    // j["keyslots"] = {"0"};
    // j["kek-priv"] = encoded_priv_key;
    // j["kek-pub"] = encoded_pub_key;
    // j["primary-key-alg"] = primary_key_alg;
    // j["primary-hash-alg"] = primary_hash_alg;
    // j["iv"] = encoded_iv;
    // j["enc"] = encoded_cipher;
    // j["pin"] = pin;
    // j["pcr"] = pcr;
    // j["pcr-bank"] = pcr_bank;

    token.remove("keyslot");
    token.insert("type", "usec-tpm2");
    token.insert("keyslots", QJsonArray::fromStringList({ "0" }));
    token.insert("kek-priv", getBase64Of(kGlobalTPMConfigPath + device + "/key.priv"));
    token.insert("kek-pub", getBase64Of(kGlobalTPMConfigPath + device + "/key.pub"));
    token.insert("iv", getBase64Of(kGlobalTPMConfigPath + device + "/iv.bin"));
    token.insert("enc", getBase64Of(kGlobalTPMConfigPath + device + "/cipher.out"));
    token.insert("pin", pin ? "1" : "0");

    doc.setObject(token);
    return doc.toJson(QJsonDocument::Compact);
}

QString DiskEncryptMenuScene::getBase64Of(const QString &fileName)
{
    QFile f(fileName);
    if (!f.open(QIODevice::ReadOnly)) {
        qDebug() << "cannot read file of" << fileName;
        return "";
    }
    QByteArray contents = f.readAll();
    f.close();
    return QString(contents.toBase64());
}

void DiskEncryptMenuScene::onUnlocked(bool ok, dfmmount::OperationErrorInfo info, QString clearDev)
{
    QApplication::restoreOverrideCursor();
    if (!ok && info.code != dfmmount::DeviceError::kUDisksErrorNotAuthorizedDismissed) {
        qWarning() << "unlock device failed!" << info.message;
        dialog_utils::showDialog(tr("Unlock device failed"),
                                 tr("Wrong passphrase"),
                                 dialog_utils::kError);
        return;
    }

    auto dev = device_utils::createBlockDevice(clearDev);
    if (!dev)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    dev->mountAsync({}, onMounted);
}

void DiskEncryptMenuScene::onMounted(bool ok, dfmmount::OperationErrorInfo info, QString mountPoint)
{
    QApplication::restoreOverrideCursor();
    if (!ok && info.code != dfmmount::DeviceError::kUDisksErrorNotAuthorizedDismissed) {
        qWarning() << "mount device failed!" << info.message;
        dialog_utils::showDialog(tr("Mount device failed"), "", dialog_utils::kError);
        return;
    }
}

void DiskEncryptMenuScene::unmountBefore(const std::function<void(const DeviceEncryptParam &)> &after)
{
    using namespace dfmmount;
    auto blk = device_utils::createBlockDevice(selectedItemInfo.value("Id").toString());
    if (!blk)
        return;

    auto params = param;
    if (blk->isEncrypted()) {
        const QString &clearPath = blk->getProperty(Property::kEncryptedCleartextDevice).toString();
        if (clearPath.length() > 1) {
            auto lock = [=] {
                blk->lockAsync({}, [=](bool ok, OperationErrorInfo err) {
                    ok ? after(params) : onUnmountError(kLock, params.devDesc, err);
                });
            };
            auto onUnmounted = [=](bool ok, const OperationErrorInfo &err) {
                ok ? lock() : onUnmountError(kUnmount, params.devDesc, err);
            };

            // do unmount cleardev
            auto clearDev = device_utils::createBlockDevice(clearPath);
            clearDev->unmountAsync({}, onUnmounted);
        } else {
            after(params);
        }
    } else {
        blk->unmountAsync({}, [=](bool ok, OperationErrorInfo err) {
            ok ? after(params) : onUnmountError(kUnmount, params.devDesc, err);
        });
    }
}

void DiskEncryptMenuScene::onUnmountError(OpType t, const QString &dev, const dfmmount::OperationErrorInfo &err)
{
    qDebug() << "unmount device failed:"
             << dev
             << err.message;
    QString operation = (t == kUnmount) ? tr("unmount") : tr("lock");
    dialog_utils::showDialog(tr("Encrypt failed"),
                             tr("Cannot %1 device %2").arg(operation, dev),
                             dialog_utils::kError);
}
