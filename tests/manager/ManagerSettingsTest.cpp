// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/ManagerWebServer.h"
#include "manager/ManagerWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSocketNotifier>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>

using namespace codexui::manager;
class ManagerSettingsTest final : public QObject {
    Q_OBJECT
public:
    ManagerSettingsTest(ManagerWindow& window, QString path, QString directory)
        : window_(window)
        , path_(std::move(path))
        , directory_(std::move(directory)) {
    }

private:
    QByteArray contents() const {
        QFile file(path_);
        const bool opened = file.open(QIODevice::ReadOnly);
        return opened ? file.readAll() : QByteArray{};
    }
    void withDialog(const std::function<void(QDialog&)>& task) {
        window_.show();
        QTimer::singleShot(50, this, [&, task] {
            auto* dialog = window_.findChild<QDialog*>("managerWebSettings");
            QVERIFY(dialog);
            task(*dialog);
            dialog->reject();
        });
        auto* menu = window_.findChild<QToolButton*>();
        QVERIFY(menu);
        QAction* settings = nullptr;
        for (auto* action : menu->menu()->actions()) {
            if (action->text() == "Web interface settings…") {
                settings = action;
            }
        }
        QVERIFY(settings);
        settings->trigger();
    }
    void exercise(QDialog& dialog) {
        const auto close = qScopeGuard([&] {
            dialog.reject();
        });
        auto* boxes = dialog.findChild<QDialogButtonBox*>();
        auto* save = boxes->button(QDialogButtonBox::Save);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 10000);
        auto* http = dialog.findChild<QCheckBox*>("Enable HTTP");
        auto* https = dialog.findChild<QCheckBox*>("Enable HTTPS");
        auto* httpPort = dialog.findChild<QSpinBox*>("HTTP port");
        auto* httpsPort = dialog.findChild<QSpinBox*>("HTTPS port");
        auto* password = dialog.findChild<QLineEdit*>("Password file");
        auto* status = dialog.findChild<QLabel*>("webSettingsStatus");
        auto* running = dialog.findChild<QLabel*>("webSettingsRunning");
        QVERIFY(http && https && httpPort && httpsPort && password && status && running);
        QVERIFY(!http->isChecked() && !https->isChecked());
        QCOMPARE(httpPort->value(), 4510);
        QVERIFY(running->text().contains("disabled", Qt::CaseInsensitive));
        QVERIFY(dialog.findChildren<QPushButton*>().size() >= 2);
        for (auto* input : dialog.findChildren<QLineEdit*>()) {
            QVERIFY(!input->accessibleName().isEmpty() || input->parent()->inherits("QSpinBox"));
        }
        QSignalSpy responses(&window_, &ManagerWindow::webSettingsReceived);
        QTest::mouseClick(http, Qt::LeftButton, Qt::NoModifier, QPoint(8, http->height() / 2));
        QVERIFY(http->isChecked());
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(save->isEnabled());
        QVERIFY2(status->text().contains("owner-only password"), qPrintable(status->text()));
        QCOMPARE(contents(), QByteArray("log-level=5\n"));
        QFile secret(directory_ + "/password");
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("test-password\n");
        secret.close();
        QVERIFY(secret.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        password->setText(secret.fileName());
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(save->isEnabled());
        QVERIFY2(status->text().startsWith("Saved."), qPrintable(status->text()));
        QVERIFY(contents().contains("\n#manager-web.local.port=4510\nmanager-web.local.port=4510\n"));
        QVERIFY(contents().contains("log-level=5\n"));
        QVERIFY(contents().contains(secret.fileName().toUtf8()));
        QVERIFY(running->text().contains("disabled", Qt::CaseInsensitive));
        const auto token = responses.last().first().toJsonObject().value("token");
        auto snapshot = responses.last().first().toJsonObject();
        QJsonArray invalid;
        for (const auto& entry : snapshot.value("fields").toArray()) {
            const auto field = entry.toObject();
            invalid.append(field.value("label").toString() == "HTTP port" ? QJsonValue("99999") : field.value("value"));
        }
        const auto saved = contents();
        const auto count = responses.size();
        emit window_.webSettingsRequested({{"token", token}, {"values", invalid}});
        QTRY_COMPARE(responses.size(), count + 1);
        QVERIFY(status->text().contains("Invalid HTTP port"));
        QCOMPARE(contents(), saved);
        QVERIFY(QFile::rename(path_, path_ + ".backup"));
        QVERIFY(QDir().mkdir(path_));
        httpPort->setValue(4512);
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(save->isEnabled());
        QVERIFY(status->text().contains("Could not save"));
        QVERIFY(QDir().rmdir(path_));
        QVERIFY(QFile::rename(path_ + ".backup", path_));
        QCOMPARE(contents(), saved);
        QVERIFY(QFile::setPermissions(path_, QFile::ReadOwner));
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(save->isEnabled());
        QVERIFY(status->text().contains("Could not save"));
        QVERIFY(QFile::setPermissions(path_, QFile::ReadOwner | QFile::WriteOwner));
        QCOMPARE(contents(), saved);
        const auto beforeRead = responses.size();
        emit window_.webSettingsRequested({{"token", token}});
        QTRY_COMPARE(responses.size(), beforeRead + 1);
        snapshot = responses.last().first().toJsonObject();
        const auto fields = snapshot.value("fields").toArray();
        for (const auto& entry : fields) {
            const auto field = entry.toObject();
            if (field.value("label").toString() == "HTTP port") {
                QCOMPARE(field.value("value").toString(), QString("4510"));
            }
        }
        auto* cert = dialog.findChild<QLineEdit*>("HTTPS certificate");
        auto* key = dialog.findChild<QLineEdit*>("HTTPS private key");
        QVERIFY(cert && key);
        https->setChecked(true);
        cert->setText(directory_ + "/missing.pem");
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(save->isEnabled());
        QVERIFY(status->text().contains("Invalid HTTPS certificate"));
        QCOMPARE(contents(), saved);
        const auto certificate = directory_ + "/server.pem";
        const auto privateKey = directory_ + "/server.key";
        QProcess openssl;
        openssl.start("openssl",
                      {"req",
                       "-x509",
                       "-newkey",
                       "rsa:2048",
                       "-nodes",
                       "-keyout",
                       privateKey,
                       "-out",
                       certificate,
                       "-days",
                       "1",
                       "-subj",
                       "/CN=localhost"});
        QVERIFY(openssl.waitForFinished(10000));
        QCOMPARE(openssl.exitCode(), 0);
        cert->setText(certificate);
        key->setText(privateKey);
        QTcpServer first, second;
        QVERIFY(first.listen(QHostAddress::LocalHost, 0));
        QVERIFY(second.listen(QHostAddress::LocalHost, 0));
        port_ = first.serverPort();
        tlsPort_ = second.serverPort();
        first.close();
        second.close();
        httpPort->setValue(port_);
        httpsPort->setValue(tlsPort_);
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(save->isEnabled());
        QVERIFY2(status->text().startsWith("Saved."), qPrintable(status->text()));
        QVERIFY(running->text().contains("disabled", Qt::CaseInsensitive));
        // Restoring the previous configuration must not revalidate a certificate removed externally.
        QVERIFY(QFile::rename(certificate, certificate + ".away"));
        const auto beforeMissingCertificate = responses.size();
        emit window_.webSettingsRequested({{"token", token}, {"values", invalid}});
        QTRY_COMPARE(responses.size(), beforeMissingCertificate + 1);
        QVERIFY(status->text().contains("Invalid HTTP port"));
        QVERIFY(QFile::rename(certificate + ".away", certificate));
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(save->isEnabled());
        QVERIFY(status->text().startsWith("Saved."));
        dialog.resize(460, 520);
        QCoreApplication::processEvents();
        QCOMPARE(dialog.size(), QSize(460, 520));
        auto* scroll = dialog.findChild<QScrollArea*>();
        scroll->ensureWidgetVisible(key);
        QCoreApplication::processEvents();
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(!key->visibleRegion().isEmpty());
        QVERIFY(dialog.rect().contains(QRect(save->mapTo(&dialog, QPoint()), save->size())));
        const auto evidence = qEnvironmentVariable("CODEXMANAGER_SETTINGS_EVIDENCE");
        if (!evidence.isEmpty()) {
            QVERIFY(dialog.grab().save(evidence));
        }
        succeeded_ = true;
    }
private slots:
    void dialogPersistsUsingSnode() {
        withDialog([&](QDialog& dialog) {
            exercise(dialog);
        });
        QVERIFY(succeeded_);
    }
    void freshProcessResaves() {
        QVERIFY(succeeded_);
        QProcess reload;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("CODEXMANAGER_SETTINGS_CONFIG", path_);
        reload.setProcessEnvironment(environment);
        reload.start(QCoreApplication::applicationFilePath(), {"reloadAndSaveUnchanged", "-o", directory_ + "/roundtrip.log,txt"});
        QVERIFY(reload.waitForStarted());
        QVERIFY(reload.waitForFinished(15000));
        QFile log(directory_ + "/roundtrip.log");
        QVERIFY(log.open(QIODevice::ReadOnly));
        const auto output = log.readAll();
        QVERIFY2(reload.exitCode() == 0, output.constData());
        QVERIFY(output.contains("PASS   : ManagerSettingsTest::reloadAndSaveUnchanged()"));
    }
    void reloadAndSaveUnchanged() {
        if (qEnvironmentVariableIsEmpty("CODEXMANAGER_SETTINGS_CONFIG")) {
            QSKIP("Executed by freshProcessResaves in a new application process.");
        }
        bool saved = false;
        withDialog([&](QDialog& dialog) {
            auto* save = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
            QTRY_VERIFY(save->isEnabled());
            QVERIFY(dialog.findChild<QCheckBox*>("Enable HTTP")->isChecked());
            QVERIFY(dialog.findChild<QCheckBox*>("Enable HTTPS")->isChecked());
            const auto running = dialog.findChild<QLabel*>("webSettingsRunning")->text();
            QVERIFY(running.contains("HTTP: http://127.0.0.1:"));
            QVERIFY(running.contains("HTTPS: https://127.0.0.1:"));
            int links = 0;
            for (auto* button : dialog.findChildren<QPushButton*>()) {
                if (button->text().startsWith("Open ")) {
                    QVERIFY(button->isVisible() && button->isEnabled());
                    ++links;
                }
            }
            QCOMPARE(links, 2);
            const auto port = QString::number(dialog.findChild<QSpinBox*>("HTTP port")->value()).toUtf8();
            QTest::mouseClick(save, Qt::LeftButton);
            QTRY_VERIFY(save->isEnabled());
            QVERIFY(dialog.findChild<QLabel*>("webSettingsStatus")->text().startsWith("Saved."));
            QVERIFY(contents().contains("\n#manager-web.local.port=" + port + "\nmanager-web.local.port=" + port + "\n"));
            QVERIFY(contents().contains("log-level=5\n"));
            saved = true;
        });
        QVERIFY(saved);
    }
    void restartUsesSavedFile() {
        QVERIFY(succeeded_);
        QProcess manager;
        manager.start(CODEXMANAGER_EXECUTABLE, {"--config-file", path_});
        const auto cleanup = qScopeGuard([&] {
            manager.terminate();
            if (!manager.waitForFinished(10000)) {
                manager.kill();
                manager.waitForFinished(1000);
            }
        });
        QVERIFY(manager.waitForStarted());
        QTcpSocket probe;
        QElapsedTimer deadline;
        deadline.start();
        bool listening = false;
        while (!listening && deadline.elapsed() < 10000) {
            probe.connectToHost(QHostAddress::LocalHost, port_);
            listening = probe.waitForConnected(100);
            probe.abort();
            QTest::qWait(20);
        }
        QVERIFY2(listening, manager.readAll().constData());
        QNetworkAccessManager network;
        for (const auto& endpoint :
             {QString("http://127.0.0.1:%1/api/state").arg(port_), QString("https://127.0.0.1:%1/api/state").arg(tlsPort_)}) {
            QNetworkRequest request{QUrl(endpoint)};
            request.setRawHeader("Authorization", "Basic " + QByteArray("manager:test-password").toBase64());
            auto* reply = network.get(request);
            // Disposable self-signed certificate; system certificate trust is not qualified here.
            connect(reply, &QNetworkReply::sslErrors, reply, [reply] {
                reply->ignoreSslErrors();
            });
            QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 10000);
            QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
            QVERIFY(QJsonDocument::fromJson(reply->readAll()).object().contains("profiles"));
            reply->deleteLater();
        }
        manager.terminate();
        QVERIFY(manager.waitForFinished(10000));
        QCOMPARE(manager.exitCode(), 0);
    }

private:
    ManagerWindow& window_;
    QString path_, directory_;
    bool succeeded_ = false;
    quint16 port_ = 0, tlsPort_ = 0;
};
int main(int argc, char** argv) {
    prepareManagerSignals();
    QTemporaryDir directory;
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    QApplication app(argc, argv);
    app.setApplicationName("ManagerSettingsTest");
    const auto inherited = qEnvironmentVariable("CODEXMANAGER_SETTINGS_CONFIG");
    const auto path = inherited.isEmpty() ? directory.filePath("settings.conf") : inherited;
    if (inherited.isEmpty()) {
        QFile config(path);
        if (!config.open(QIODevice::WriteOnly)) {
            qFatal("Cannot create settings fixture");
        }
        config.write("log-level=5\n");
        config.close();
    }
    QByteArray program("codexmanager-settings-test"), option("--config-file"), filename(path.toUtf8());
    char* arguments[]{program.data(), option.data(), filename.data()};
    auto options = initializeManager(3, arguments, directory.filePath("profiles.json").toStdString(), CODEXMANAGER_TEST_ASSETS);
    ManagerWebServer web(options);
    ManagerController controller(QString::fromStdString(options.profiles));
    ManagerWindow window(controller);
    QSocketNotifier notifier(web.descriptor(), QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &window, [&] {
        web.drainWake();
        std::string result;
        while (web.configuration(result)) {
            emit window.webSettingsReceived(QJsonDocument::fromJson(QByteArray::fromStdString(result)).object());
        }
    });
    QObject::connect(&window, &ManagerWindow::webSettingsRequested, &window, [&](const QJsonObject& request) {
        if (!web.configure(QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString())) {
            qFatal("Unexpected settings queue overflow");
        }
    });
    web.start();
    ManagerSettingsTest test(window, path, directory.path());
    const int result = QTest::qExec(&test, argc, argv);
    return result;
}
#include "ManagerSettingsTest.moc"
