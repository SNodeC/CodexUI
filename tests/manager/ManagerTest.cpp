// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "codex/ui/ProfileWindow.h"
#include "codex/ui/UiStyle.h"
#include "manager/LaunchPlan.h"
#include "manager/ManagerWindow.h"
#include "manager/ProfileEditor.h"
#include "manager/ProfileRuntime.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QMenu>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QtTest>
#include <algorithm>
#include <iostream>
#include <memory>

using namespace codexui::manager;
namespace {
    void writeFile(const QString& path, const QByteArray& data, bool executable = false) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            qFatal("Cannot write fixture");
        }
        if (executable) {
            f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
    }
    int fixture(const QStringList& arguments) {
        const auto mode = arguments.value(2);
        int result = 0;
        if (arguments.contains("--show-config")) {
            std::cout << "snodec.config.comment-meta\n";
            result = 2;
        } else if (arguments.contains("--help")) {
            if (qEnvironmentVariableIsSet("CODEXMANAGER_SLOW_HELP")) {
                QThread::msleep(200);
            }
            std::cout << "--listen\n";
            for (const auto& t : frontendTransports()) {
                std::cout << "codex-ui-" << t.toStdString() << "\ncodex-bridge-client-" << t.toStdString() << "\n";
            }
        } else if (mode == "probe") {
            std::cout << "{\"kind\":\"bridge.connection\",\"event\":\"opened\",\"connectionId\":\"probe\",\"role\":\"controller\"}\n"
                         "{\"kind\":\"bridge.provider\",\"state\":\"ready\"}\n"
                         "{\"kind\":\"bridge.controller\",\"controllerConnectionId\":\"probe\"}\n"
                      << std::flush;
            int releases = 0;
            bool signedIn = qEnvironmentVariable("CODEXMANAGER_AUTH_FIXTURE").startsWith("signed-in");
            QString loginId;
            QString loginType;
            const QString authMode = qEnvironmentVariable("CODEXMANAGER_AUTH_FIXTURE");
            const auto emitPayload = [](const QJsonObject& payload) {
                std::cout
                    << QJsonDocument(QJsonObject{{"kind", "appserver"}, {"payload", payload}}).toJson(QJsonDocument::Compact).constData()
                    << '\n'
                    << std::flush;
            };
            for (std::string line; std::getline(std::cin, line) && line != "quit";) {
                if (line == "acquire") {
                    std::cout << "{\"kind\":\"bridge.controller\",\"controllerConnectionId\":\"probe\"}\n" << std::flush;
                } else if (line.starts_with("raw ")) {
                    const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line.substr(4))).object();
                    const auto method = request.value("method").toString();
                    const auto params = request.value("params").toObject();
                    QJsonObject result;
                    if (method == "account/read") {
                        if (!loginId.isEmpty() && (authMode == "success" || authMode == "expired")) {
                            signedIn = authMode == "success";
                            emitPayload({{"method", "account/login/completed"},
                                         {"params", QJsonObject{{"loginId", loginId}, {"success", signedIn}}}});
                            loginId.clear();
                        }
                        result = {{"requiresOpenaiAuth", !authMode.isEmpty()},
                                  {"account",
                                   signedIn ? QJsonValue(QJsonObject{{"type", loginType == "apiKey" ? "apiKey" : "chatgpt"},
                                                                     {"email",
                                                                      authMode.startsWith("signed-in") ? QString(110, 'x') + "@example.test"
                                                                                                       : "fixture@example.test"},
                                                                     {"planType", "plus"}})
                                            : QJsonValue(QJsonValue::Null)}};
                    } else if (method == "account/login/start") {
                        loginType = params.value("type").toString();
                        if (authMode == "error") {
                            emitPayload({{"id", request.value("id")},
                                         {"error", QJsonObject{{"code", -32602}, {"message", "secret must not be logged"}}}});
                        } else if (authMode == "disconnect") {
                            std::cout << "{\"kind\":\"client.connection\",\"event\":\"disconnected\"}\n" << std::flush;
                        } else if (loginType == "apiKey") {
                            signedIn = true;
                            result = {{"type", "apiKey"}};
                        } else {
                            loginId = "fixture-login";
                            if (authMode == "early") {
                                signedIn = true;
                                emitPayload({{"method", "account/login/completed"},
                                             {"params", QJsonObject{{"loginId", loginId}, {"success", true}}}});
                            }
                            result = {{"type", loginType}, {"loginId", loginId}};
                            if (loginType == "chatgpt") {
                                result.insert("authUrl", "https://auth.openai.com/fixture-browser");
                            } else {
                                result.insert("verificationUrl", "https://auth.openai.com/codex/device");
                                result.insert("userCode", "TEST-CODE");
                            }
                        }
                    } else if (method == "account/login/cancel") {
                        result = {{"status", "canceled"}};
                        loginId.clear();
                    } else if (method == "account/logout") {
                        signedIn = false;
                    }
                    if (!(method == "account/login/start" && (authMode == "error" || authMode == "disconnect")) &&
                        !(method == "account/read" && authMode == "read-timeout")) {
                        emitPayload({{"id", request.value("id")}, {"result", result}});
                        if (authMode == "unauthorized" && method == "account/login/start") {
                            emitPayload(
                                {{"method", "error"}, {"params", QJsonObject{{"error", QJsonObject{{"codexErrorInfo", "unauthorized"}}}}}});
                        }
                    }
                } else if (line == "release") {
                    std::cout << (authMode.contains("controller")
                                      ? "{\"kind\":\"bridge.controller\",\"controllerConnectionId\":\"other-ui\"}\n"
                                      : "{\"kind\":\"bridge.controller\",\"controllerConnectionId\":null}\n")
                              << std::flush;
                    ++releases;
                    if (qEnvironmentVariableIsSet("CODEXMANAGER_RECLAIM_CONTROL")) {
                        if (releases >= 2) {
                            writeFile(qEnvironmentVariable("CODEXMANAGER_FIXTURES") + "/releases", QByteArray::number(releases));
                        }
                        if (releases == 1) {
                            std::cout << "{\"kind\":\"bridge.controller\",\"controllerConnectionId\":\"probe\"}\n" << std::flush;
                        }
                    }
                }
            }
        } else if (mode == "ssh") {
            if (qEnvironmentVariableIsSet("CODEXMANAGER_FAIL_SSH")) {
                result = 255;
            } else {
                QProcess process;
                process.start("sh", {"-c", arguments.last()});
                if (!process.waitForFinished(10000)) {
                    result = 1;
                } else {
                    std::cout << process.readAllStandardOutput().constData();
                    result = process.exitCode();
                }
            }
        } else {
            const auto root = qEnvironmentVariable("CODEXMANAGER_FIXTURES");
            QString unit;
            for (const auto& arg : arguments) {
                if (arg.startsWith("--unit=")) {
                    unit = arg.mid(7);
                } else if (arg.startsWith("codexmanager-") && arg.endsWith(".service")) {
                    unit = arg;
                }
            }
            if (mode == "systemd-run") {
                QFile f(root + "/starts");
                if (!f.open(QIODevice::Append)) {
                    qFatal("Cannot open fixture launch log");
                }
                f.write((unit + '\n').toUtf8());
                if (qEnvironmentVariableIsSet("CODEXMANAGER_FAIL_START")) {
                    result = 1;
                } else {
                    writeFile(root + '/' + unit, "running");
                }
            } else if (mode == "systemctl") {
                if (arguments.contains("stop")) {
                    QFile::remove(root + '/' + unit);
                } else {
                    const bool active = QFileInfo::exists(root + '/' + unit);
                    std::cout << "ActiveState=" << (active ? "active" : "inactive") << "\nMainPID=" << (active ? 123 : 0)
                              << "\nDescription="
                              << (qEnvironmentVariableIsSet("CODEXMANAGER_FOREIGN_UNIT") ? "Someone else " : "CodexManager ")
                              << unit.toStdString() << '\n';
                    result = active ? 0 : 4;
                }
            } else if (mode == "journalctl") {
                std::cout << "fixture log\n";
            } else {
                result = 1;
            }
        }
        return result;
    }
    class FixtureTools {
    public:
        QTemporaryDir dir;
        QByteArray oldPath = qgetenv("PATH");
        FixtureTools() {
            qputenv("CODEXMANAGER_FIXTURES", dir.path().toUtf8());
            for (const auto& tool : {"systemd-run", "systemctl", "journalctl", "ssh", "probe", "codex-bridge", "codex-ui", "codex"}) {
                const QString script =
                    "#!/bin/sh\nexec " + quote(QCoreApplication::applicationFilePath()) + " --fixture " + quote(tool) + " \"$@\"\n";
                writeFile(dir.filePath(tool), script.toUtf8(), true);
            }
            qputenv("PATH", dir.path().toUtf8() + ':' + oldPath);
        }
        ~FixtureTools() {
            qputenv("PATH", oldPath);
            qunsetenv("CODEXMANAGER_FIXTURES");
            qunsetenv("CODEXMANAGER_FAIL_SSH");
            qunsetenv("CODEXMANAGER_FAIL_START");
            qunsetenv("CODEXMANAGER_FOREIGN_UNIT");
            qunsetenv("CODEXMANAGER_RECLAIM_CONTROL");
            qunsetenv("CODEXMANAGER_AUTH_FIXTURE");
        }
    };
} // namespace
class ManagerTest final : public QObject {
    Q_OBJECT
private slots:
    void deletionRejectsSharedSettings() {
        FixtureTools tools;
        auto p = Profile::create("Owner");
        p.codexHome = tools.dir.filePath(".codex-" + p.id.left(8));
        auto other = Profile::create("Nested home");
        other.codexHome = p.settingsDirectory() + "/other-home";
        ProfileRuntime runtime(p);
        std::optional<DeletionPlan> plan;
        runtime.inspectDeletion({p, other}, [&](DeletionPlan checked) {
            plan = std::move(checked);
        });
        QTRY_VERIFY_WITH_TIMEOUT(plan.has_value(), 10000);
        QVERIFY(plan->error.contains("shares or overlaps"));
    }
    void profileDeletion_data() {
        QTest::addColumn<bool>("remote");
        QTest::addColumn<bool>("foreign");
        QTest::newRow("local") << false << false;
        QTest::newRow("ssh") << true << false;
        QTest::newRow("foreign-unit") << false << true;
    }
    void profileDeletion() {
        QFETCH(bool, remote);
        QFETCH(bool, foreign);
        FixtureTools tools;
        auto p = Profile::create("Delete fixture");
        p.codexHome = tools.dir.filePath(".codex-" + p.id.left(8));
        p.probeExecutable = tools.dir.filePath("probe");
        if (remote) {
            p.bridge.ownership = Ownership::Ssh;
            p.bridge.sshTarget = "fixture-host";
            p.frontend.transport = "ipv4";
        }
        QVERIFY(QDir().mkpath(p.codexHome));
        writeFile(p.codexHome + "/conversation", "fixture data");
        QVERIFY(QDir().mkpath(p.settingsDirectory()));
        const auto cleanup = qScopeGuard([&] {
            QDir(p.settingsDirectory()).removeRecursively();
        });
        writeFile(p.settingsDirectory() + "/settings", "fixture settings");
        writeFile(tools.dir.filePath(p.unit("bridge")), "running");
        if (foreign) {
            qputenv("CODEXMANAGER_FOREIGN_UNIT", "1");
        }
        ProfileRuntime runtime(p);
        std::optional<DeletionPlan> plan;
        runtime.inspectDeletion({p}, [&](DeletionPlan result) {
            plan = std::move(result);
        });
        QTRY_VERIFY_WITH_TIMEOUT(plan.has_value(), 10000);
        QVERIFY2(plan->error.isEmpty(), qPrintable(plan->error));
        QSignalSpy finished(&runtime, &ProfileRuntime::deletionFinished);
        runtime.deleteData(*plan);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().first().toBool(), !foreign);
        QCOMPARE(QFileInfo::exists(p.codexHome + "/conversation"), foreign);
        QCOMPARE(QFileInfo::exists(p.settingsDirectory()), foreign);
        QCOMPARE(QFileInfo::exists(tools.dir.filePath(p.unit("bridge"))), foreign);
    }
    void deletionDialog_data() {
        QTest::addColumn<bool>("confirm");
        QTest::newRow("cancel") << false;
        QTest::newRow("delete") << true;
    }
    void deletionDialog() {
        QFETCH(bool, confirm);
        FixtureTools tools;
        auto p = Profile::create("Disposable profile");
        p.codexHome = tools.dir.filePath(".codex-" + p.id.left(8));
        QVERIFY(QDir().mkpath(p.codexHome));
        writeFile(p.codexHome + "/data", "preserve until confirmed");
        const auto path = tools.dir.filePath("profiles.json");
        QString error;
        QVERIFY(ProfileStore(path).save({p}, &error));
        ManagerWindow window(path);
        window.show();
        auto* runtime = window.findChild<ProfileRuntime*>();
        QVERIFY(runtime);
        QTRY_VERIFY(!runtime->busy());
        QAction* deletion = nullptr;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->text() == "Delete profile and its data…") {
                deletion = action;
            }
        }
        QVERIFY(deletion);
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QPointer<QDialog> guard(dialog);
            const auto close = qScopeGuard([guard] {
                if (guard) {
                    guard->reject();
                }
            });
            auto* name = dialog->findChild<QLineEdit*>("deletionName");
            auto* exclusive = dialog->findChild<QCheckBox*>("deletionExclusive");
            auto* erase = dialog->findChild<QPushButton*>("deletePermanently");
            QVERIFY(name && exclusive && erase);
            QVERIFY(!erase->isEnabled());
            name->setText(p.name);
            QVERIFY(!erase->isEnabled());
            exclusive->setChecked(true);
            QTRY_VERIFY_WITH_TIMEOUT(erase->isEnabled(), 10000);
            name->setText("wrong name");
            QVERIFY(!erase->isEnabled());
            name->setText(p.name);
            for (auto* button : dialog->findChildren<QPushButton*>()) {
                if (button->isVisible()) {
                    QVERIFY(dialog->rect().contains(QRect(button->mapTo(dialog, QPoint{}), button->size())));
                    QVERIFY(button->height() >= 24);
                }
            }
            const auto evidence = qEnvironmentVariable("CODEXMANAGER_EVIDENCE_DIR");
            if (!evidence.isEmpty()) {
                QVERIFY(QDir().mkpath(evidence));
                QVERIFY(dialog->grab().save(evidence + "/delete-profile.png"));
            }
            if (confirm) {
                QTest::mouseClick(erase, Qt::LeftButton);
            }
        });
        deletion->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(ProfileStore(path).load(&error).size(), confirm ? 0 : 1, 10000);
        QCOMPARE(QFileInfo::exists(p.codexHome), !confirm);
    }
    void profileAuthentication_data() {
        QTest::addColumn<QString>("method");
        QTest::newRow("device-code") << "chatgptDeviceCode";
        QTest::newRow("browser") << "chatgpt";
        QTest::newRow("api-key") << "apiKey";
    }
    void profileAuthentication() {
        QFETCH(QString, method);
        FixtureTools tools;
        qputenv("CODEXMANAGER_AUTH_FIXTURE", "success");
        auto p = Profile::create("Authentication");
        p.codexHome = tools.dir.filePath("home");
        p.probeExecutable = tools.dir.filePath("probe");
        ProfileRuntime first(p);
        first.start();
        QTRY_COMPARE(first.accountState(), ProfileRuntime::AccountState::SignedOut);
        QTRY_VERIFY(first.authenticationAvailable());
        QVERIFY(!first.components().value("CodexUI").startsWith("Running"));
        auto secondProfile = p;
        secondProfile.id = Profile::create().id;
        secondProfile.codexHome = tools.dir.filePath("other-home");
        ProfileRuntime second(secondProfile);
        second.start(false);
        QTRY_COMPARE(second.accountState(), ProfileRuntime::AccountState::SignedOut);
        first.signIn(method, "sk-fixture-only-do-not-log");
        if (method != "apiKey") {
            QTRY_VERIFY(!first.loginUrl().isEmpty());
            QCOMPARE(first.loginCode(), method == "chatgptDeviceCode" ? QString("TEST-CODE") : QString{});
            first.refreshAccount();
        }
        QTRY_COMPARE(first.accountState(), ProfileRuntime::AccountState::SignedIn);
        QTRY_VERIFY(first.authenticationAvailable());
        QVERIFY(first.accountStatus().contains("fixture@example.test"));
        QCOMPARE(second.accountState(), ProfileRuntime::AccountState::SignedOut);
        QVERIFY(!first.logs().contains("sk-fixture"));
        QVERIFY(!first.logs().contains("TEST-CODE"));
        first.signOut();
        QTRY_COMPARE(first.accountState(), ProfileRuntime::AccountState::SignedOut);
        QTRY_VERIFY(first.authenticationAvailable());
        first.stop();
        second.stop();
        QTRY_VERIFY(!first.busy() && !second.busy());
    }
    void authenticationFailuresAndCancellation_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"pending", "error", "disconnect", "controller", "expired", "early"}) {
            QTest::newRow(mode) << QString(mode);
        }
    }
    void authenticationFailuresAndCancellation() {
        QFETCH(QString, mode);
        FixtureTools tools;
        qputenv("CODEXMANAGER_AUTH_FIXTURE", mode.toUtf8());
        auto p = Profile::create();
        p.bridge.ownership = Ownership::External;
        p.probeExecutable = tools.dir.filePath("probe");
        ProfileRuntime runtime(p);
        runtime.start(false);
        QTRY_COMPARE(runtime.accountState(), ProfileRuntime::AccountState::SignedOut);
        QTRY_VERIFY(runtime.ready());
        runtime.signIn("chatgptDeviceCode");
        if (mode == "early") {
            QTRY_VERIFY(!runtime.authenticating());
            QTRY_COMPARE(runtime.accountState(), ProfileRuntime::AccountState::SignedIn);
            QTRY_VERIFY(runtime.authenticationAvailable());
        } else if (mode == "expired") {
            QTRY_VERIFY(!runtime.loginCode().isEmpty());
            runtime.refreshAccount();
            QTRY_VERIFY(!runtime.authenticating());
            QTRY_COMPARE(runtime.accountState(), ProfileRuntime::AccountState::SignedOut);
            QVERIFY(runtime.authenticationMessage().contains("expired"));
        } else if (mode == "pending") {
            QTRY_VERIFY(!runtime.loginCode().isEmpty());
            runtime.cancelLogin();
            QTRY_VERIFY(!runtime.authenticating());
            QTRY_VERIFY(runtime.authenticationAvailable());
            QVERIFY(runtime.loginUrl().isEmpty());
            QCOMPARE(runtime.accountState(), ProfileRuntime::AccountState::SignedOut);
            // Cancel before the asynchronous claim/start response arrives as well.
            runtime.signIn("chatgpt");
            runtime.cancelLogin();
            QTRY_VERIFY(!runtime.authenticating() && runtime.authenticationAvailable());
        } else if (mode == "controller") {
            QVERIFY(!runtime.authenticating());
            QVERIFY(runtime.authenticationMessage().contains("control"));
        } else if (mode == "disconnect") {
            QTRY_VERIFY(!runtime.authenticating());
            QCOMPARE(runtime.accountState(), ProfileRuntime::AccountState::Unknown);
            QVERIFY(runtime.authenticationMessage().contains("unknown"));
        } else {
            QTRY_VERIFY(!runtime.authenticating());
            QVERIFY(runtime.authenticationMessage().contains("rejected"));
            QVERIFY(!runtime.logs().contains("secret"));
        }
        runtime.stop();
        QTRY_VERIFY(!runtime.busy());
    }
    void accountReadTimeoutPreservesLogin() {
        FixtureTools tools;
        qputenv("CODEXMANAGER_AUTH_FIXTURE", "read-timeout");
        auto p = Profile::create();
        p.bridge.ownership = Ownership::External;
        p.probeExecutable = tools.dir.filePath("probe");
        ProfileRuntime runtime(p);
        runtime.check();
        QTRY_VERIFY(runtime.authenticationAvailable());
        runtime.signIn("chatgptDeviceCode");
        QTRY_COMPARE(runtime.loginCode(), QString("TEST-CODE"));
        // Exercise the real public timeout contract, with the event loop still running.
        QTest::qWait(31000);
        runtime.refresh();
        QVERIFY(runtime.authenticating());
        QCOMPARE(runtime.loginCode(), QString("TEST-CODE"));
        QCOMPARE(runtime.accountState(), ProfileRuntime::AccountState::Unknown);
        runtime.cancelLogin();
        QTRY_VERIFY(!runtime.authenticating());
        runtime.stop();
        QTRY_VERIFY(!runtime.busy());
    }
    void authenticationRejection() {
        FixtureTools tools;
        qputenv("CODEXMANAGER_AUTH_FIXTURE", "unauthorized");
        auto p = Profile::create();
        p.bridge.ownership = Ownership::External;
        p.probeExecutable = tools.dir.filePath("probe");
        ProfileRuntime runtime(p);
        runtime.check();
        QTRY_VERIFY(runtime.authenticationAvailable());
        runtime.signIn("apiKey", "sk-fixture-only");
        QTRY_COMPARE(runtime.accountState(), ProfileRuntime::AccountState::Rejected);
        QVERIFY(runtime.accountStatus().contains("rejected"));
        QTRY_VERIFY(runtime.authenticationAvailable());
        runtime.stop();
        QTRY_VERIFY(!runtime.busy());
    }
    void wrappedLabelGeometry() {
        for (const auto* kind : {"body", "meta", "panelHeader"}) {
            std::unique_ptr<QLabel> label(
                codexui::UiStyle::makeLabel("Long text that must wrap across several lines in a narrow area", kind));
            QCOMPARE(label->hasHeightForWidth(), label->wordWrap());
            if (label->wordWrap()) {
                QVERIFY(label->heightForWidth(120) > label->heightForWidth(400));
            }
        }
    }
    void authenticationDialog_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<bool>("largeFont");
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"pending", "controller", "signed-in-controller", "signed-in-long", "disconnect"}) {
            QTest::newRow(qPrintable(QString(mode) + "-normal")) << 680 << 580 << false << QString(mode);
            QTest::newRow(qPrintable(QString(mode) + "-narrow")) << 480 << 420 << false << QString(mode);
            QTest::newRow(qPrintable(QString(mode) + "-large-font")) << 520 << 440 << true << QString(mode);
        }
    }
    void authenticationDialog() {
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(bool, largeFont);
        QFETCH(QString, mode);
        FixtureTools tools;
        qputenv("CODEXMANAGER_AUTH_FIXTURE", mode.toUtf8());
        auto p = Profile::create("Remote research with a long descriptive profile name");
        p.bridge.ownership = Ownership::External;
        p.codexHome = "/srv/codex/" + QString(100, 'x') + "/research";
        p.probeExecutable = tools.dir.filePath("probe");
        QString error;
        const auto path = tools.dir.filePath("profiles.json");
        QVERIFY(ProfileStore(path).save({p}, &error));
        ManagerWindow window(path);
        window.show();
        auto* runtime = window.findChild<ProfileRuntime*>();
        QVERIFY(runtime);
        runtime->check();
        QTRY_COMPARE(runtime->accountState(),
                     mode.startsWith("signed-in") ? ProfileRuntime::AccountState::SignedIn : ProfileRuntime::AccountState::SignedOut);
        QTRY_VERIFY(runtime->ready());
        const auto buttons = window.findChildren<QPushButton*>();
        auto account = std::find_if(buttons.cbegin(), buttons.cend(), [](QPushButton* b) {
            return b->isVisible() && b->text() == "Account / Sign in…";
        });
        QVERIFY(account != buttons.cend());
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] {
                dialog->reject();
            });
            if (largeFont) {
                dialog->setStyleSheet("QLabel, QPushButton, QComboBox, QLineEdit { font-size: 18pt; }");
            }
            dialog->resize(width, height);
            auto* area = dialog->findChild<QScrollArea*>();
            auto* method = dialog->findChild<QComboBox*>("authenticationMethod");
            auto* start = dialog->findChild<QPushButton*>("startAuthentication");
            auto* cancel = dialog->findChild<QPushButton*>("cancelAuthentication");
            auto* key = dialog->findChild<QLineEdit*>("authenticationApiKey");
            auto* code = dialog->findChild<QLineEdit*>("authenticationCode");
            auto* status = dialog->findChild<QLabel*>("accountStatus");
            QVERIFY(area && method && start && cancel && key && code && status);
            const auto checkGeometry = [&](const QString& stage) {
                QApplication::processEvents();
                QCOMPARE(dialog->width(), width);
                QCOMPARE(dialog->height(), height);
                QCOMPARE(area->horizontalScrollBar()->maximum(), 0);
                for (auto* label : area->findChildren<QLabel*>()) {
                    if (label->isVisible() && !label->text().isEmpty()) {
                        QVERIFY2(label->height() >= label->heightForWidth(label->width()), qPrintable(stage + ": " + label->text()));
                        area->ensureWidgetVisible(label, 0, 0);
                        QApplication::processEvents();
                        QVERIFY2(area->viewport()->rect().contains(QRect(label->mapTo(area->viewport(), QPoint{}), label->size())),
                                 qPrintable(stage + ": " + label->text()));
                    }
                }
                for (auto* field : dialog->findChildren<QLineEdit*>()) {
                    if (field->isVisible()) {
                        QCOMPARE(field->height(), field->sizeHint().height());
                        field->setFocus();
                        QApplication::processEvents();
                        const QRect target(field->mapTo(area->widget(), QPoint{}), field->size());
                        area->ensureVisible(target.center().x(), target.center().y(), target.width() / 2 + 1, target.height() / 2 + 1);
                        QApplication::processEvents();
                        const QRect visible(field->mapTo(area->viewport(), QPoint{}), field->size());
                        QVERIFY2(area->viewport()->rect().contains(visible),
                                 qPrintable(stage + ": " + field->accessibleName() + " " + QString::number(visible.y()) + "/" +
                                            QString::number(visible.height()) + " viewport " +
                                            QString::number(area->viewport()->height())));
                        if (field->isReadOnly()) {
                            field->setFocus();
                            QTest::keyClick(field, Qt::Key_A, Qt::ControlModifier);
                            QTest::keyClick(field, Qt::Key_C, Qt::ControlModifier);
                            QCOMPARE(QApplication::clipboard()->text(), field->text());
                            QTest::keyClick(field, Qt::Key_Home);
                            QCOMPARE(field->cursorPosition(), 0);
                            QTest::keyClick(field, Qt::Key_End);
                            QCOMPARE(field->cursorPosition(), field->text().size());
                        }
                    }
                }
                for (auto* button : dialog->findChildren<QPushButton*>()) {
                    if (button->text() == "Start / connect backend") {
                        QCOMPARE(button->isVisible(), !runtime->ready() && !runtime->authenticating());
                    }
                    if (button->isVisible()) {
                        area->ensureWidgetVisible(button, 0, 0);
                        QApplication::processEvents();
                        const QRect geometry(button->mapTo(dialog, QPoint{}), button->size());
                        QVERIFY2(dialog->rect().contains(geometry), qPrintable(stage + ": " + button->text()));
                        QVERIFY(button->height() >= button->sizeHint().height());
                        if (button->text() == "Refresh account") {
                            QCOMPARE(button->isEnabled(), runtime->ready());
                        } else if (button->text() == "Start / connect backend") {
                            QVERIFY(!runtime->ready());
                            QCOMPARE(button->isEnabled(), !runtime->busy());
                        } else if (button->text() == "Sign out…") {
                            QVERIFY(runtime->authenticationAvailable());
                            QCOMPARE(runtime->accountState(), ProfileRuntime::AccountState::SignedIn);
                            QVERIFY(button->isEnabled());
                        } else if (button->text() == "Open browser") {
                            QVERIFY(!runtime->loginUrl().isEmpty());
                            QVERIFY(button->isEnabled());
                        }
                    }
                }
                const QString evidence = qEnvironmentVariable("CODEXMANAGER_EVIDENCE_DIR");
                if (!evidence.isEmpty()) {
                    QDir().mkpath(evidence);
                    area->verticalScrollBar()->setValue(0);
                    QVERIFY(dialog->grab().save(evidence + "/" + QString(QTest::currentDataTag()) + "-" + stage + ".png"));
                }
            };
            QCOMPARE(method->count(), 3);
            QVERIFY(!cancel->isVisible());
            QVERIFY(!code->isVisible());
            for (auto* label : dialog->findChildren<QLabel*>()) {
                QVERIFY(!label->text().contains("code shown below"));
                QVERIFY(!label->text().startsWith("Sign in to"));
            }
            checkGeometry("initial");
            if (mode.contains("controller")) {
                QTRY_VERIFY(status->text().contains("Release control"));
                QVERIFY(!start->isEnabled());
                QVERIFY(!method->isEnabled());
                checkGeometry("handoff");
            } else if (mode.startsWith("signed-in")) {
                QCOMPARE(start->text(), QString("Switch account"));
                QVERIFY(start->isEnabled());
                const auto fields = dialog->findChildren<QLineEdit*>();
                auto identity = std::find_if(fields.cbegin(), fields.cend(), [](QLineEdit* field) {
                    return field->accessibleName() == "Account status";
                });
                QVERIFY(identity != fields.cend());
                QVERIFY((*identity)->text().contains(QString(110, 'x') + "@example.test"));
                QVERIFY((*identity)->isReadOnly());
                checkGeometry("signed-in");
            } else {
                method->setCurrentIndex(2);
                QVERIFY(key->isVisible());
                QCOMPARE(key->echoMode(), QLineEdit::Password);
                QVERIFY(!start->isEnabled());
                QTest::keyClicks(key, "fixture-key");
                QVERIFY(start->isEnabled());
                checkGeometry("api-key");
                method->setCurrentIndex(0);
                area->ensureWidgetVisible(start);
                QTest::mouseClick(start, Qt::LeftButton);
                if (mode == "disconnect") {
                    QTRY_VERIFY(!runtime->ready());
                    QVERIFY(!start->isVisible());
                    QVERIFY(!code->isVisible());
                    QVERIFY(!cancel->isVisible());
                    checkGeometry("disconnected");
                } else {
                    QTRY_COMPARE(runtime->loginCode(), QString("TEST-CODE"));
                    QVERIFY(key->text().isEmpty());
                    QVERIFY(code->isVisible());
                    QCOMPARE(code->text(), QString("TEST-CODE"));
                    QVERIFY(!start->isVisible());
                    QVERIFY(cancel->isVisible() && cancel->isEnabled());
                    checkGeometry("device-code");
                    area->ensureWidgetVisible(code);
                    code->setFocus();
                    QTest::keyClick(code, Qt::Key_A, Qt::ControlModifier);
                    QTest::keyClick(code, Qt::Key_C, Qt::ControlModifier);
                    QCOMPARE(QApplication::clipboard()->text(), QString("TEST-CODE"));
                    area->ensureWidgetVisible(cancel);
                    QTest::mouseClick(cancel, Qt::LeftButton);
                    QTRY_VERIFY(!runtime->authenticating());
                    QTRY_VERIFY(runtime->authenticationAvailable());
                    QVERIFY(!code->isVisible());
                    checkGeometry("cancelled");
                    method->setCurrentIndex(1);
                    area->ensureWidgetVisible(start);
                    QTest::mouseClick(start, Qt::LeftButton);
                    QTRY_VERIFY(!runtime->loginUrl().isEmpty());
                    QVERIFY(runtime->loginCode().isEmpty());
                    QVERIFY(!code->isVisible());
                    checkGeometry("browser-callback");
                    runtime->cancelLogin();
                    QTRY_VERIFY(!runtime->authenticating());
                    QTRY_VERIFY(runtime->authenticationAvailable());
                    method->setCurrentIndex(2);
                    QTest::keyClicks(key, "fixture-key");
                    area->ensureWidgetVisible(start);
                    QTest::mouseClick(start, Qt::LeftButton);
                    QTRY_COMPARE(runtime->accountState(), ProfileRuntime::AccountState::SignedIn);
                    QTRY_VERIFY(runtime->authenticationAvailable());
                    QCOMPARE(start->text(), QString("Switch account"));
                    checkGeometry("completed");
                }
            }
        });
        QTest::mouseClick(*account, Qt::LeftButton);
        runtime->stop();
        QTRY_VERIFY(!runtime->busy());
    }
    void profileStorageAndConflicts() {
        QTemporaryDir dir;
        Profile p = Profile::create("Primary");
        p.codexHome = dir.filePath("codex a");
        p.bridge.environment.insert("LANG", "en_US.UTF-8");
        ProfileStore store(dir.filePath("profiles.json"));
        QString error;
        QVERIFY2(store.save({p}, &error), qPrintable(error));
        const auto loaded = store.load(&error);
        QVERIFY(error.isEmpty());
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().toJson(), p.toJson());
        Profile collision = Profile::create("Different label");
        collision.codexHome = p.codexHome;
        QVERIFY(!collision.errors({p}).isEmpty());
        QVERIFY(!store.save({p, collision}, &error));
        QCOMPARE(store.load(&error).size(), 1);
        QVERIFY(QDir().mkpath(p.codexHome));
        QVERIFY(QFile::link(p.codexHome, dir.filePath("home-alias")));
        collision.codexHome = dir.filePath("home-alias");
        QVERIFY(!collision.errors({p}).isEmpty());
        collision.codexHome = p.codexHome;
        collision.codexHome += "-other";
        collision.frontend.socketPath = p.frontend.socketPath;
        QVERIFY(!collision.errors({p}).isEmpty());
        auto networkA = Profile::create(), networkB = Profile::create();
        networkA.frontend.transport = "ipv4";
        networkB.frontend.transport = "websocket-ipv4";
        networkB.frontend.bindHost = "127.0.0.2";
        QVERIFY(networkB.errors({networkA}).isEmpty());
        networkA.frontend.bindHost = "0.0.0.0";
        QVERIFY(!networkB.errors({networkA}).isEmpty());
        auto mixed = Profile::create();
        mixed.provider.transport = "stdio";
        mixed.appServer.ownership = Ownership::Ssh;
        mixed.appServer.sshTarget = "unused-old-host";
        QVERIFY(mixed.errors().isEmpty());
        mixed.provider.transport = "unix";
        QVERIFY(!mixed.errors().isEmpty());
        mixed.provider.transport = "websocket-ipv4";
        mixed.appServer = Host{};
        QVERIFY(!mixed.errors().isEmpty());
        mixed.appServer.ownership = Ownership::External;
        QVERIFY(mixed.errors().isEmpty());
        mixed.provider.transport = "unix";
        mixed.appServer = Host{};
        mixed.provider.socketPath = mixed.frontend.socketPath;
        QVERIFY(!mixed.errors().isEmpty());
        writeFile(store.filePath(), "{invalid}");
        QVERIFY(store.load(&error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void deploymentMatrix() {
        for (int scenario = 1; scenario <= 11; ++scenario) {
            Profile p = Profile::create(QString::number(scenario));
            const bool remoteBridge = scenario >= 8;
            if (remoteBridge) {
                p.bridge.ownership = Ownership::Ssh;
                p.bridge.sshTarget = "bridge-host";
            }
            if (scenario == 2 || scenario == 4 || remoteBridge) {
                p.frontend.transport = "ipv4";
            }
            if (scenario == 3 || scenario == 4 || scenario == 5 || scenario == 9) {
                p.provider.transport = "unix";
            }
            if (scenario == 6 || scenario == 7 || scenario == 10 || scenario == 11) {
                p.provider.transport = "websocket-ipv4";
            }
            if (scenario == 5 || scenario == 6) {
                p.appServer.ownership = Ownership::External;
            }
            if (scenario == 7 || scenario >= 9) {
                p.appServer.ownership = Ownership::Ssh;
                p.appServer.sshTarget = scenario == 9 || scenario == 10 ? "bridge-host" : "app-host";
            }
            QVERIFY2(p.errors().isEmpty(), qPrintable(p.errors().join('\n')));
            const auto plan = LaunchPlan::forProfile(p);
            QCOMPARE(plan.backends.size(), p.provider.transport == "stdio" ? 1 : 2);
            QVERIFY(plan.ui.command.arguments.contains("--config-file"));
            QCOMPARE(plan.ui.command.environment.value("CODEXUI_PROFILE_ID"), p.id);
            QVERIFY(plan.instructions(p).contains(p.codexHome));
            for (const auto& c : plan.backends) {
                const auto start = serviceStart(c);
                QCOMPARE(start.program, c.host.sshTarget.isEmpty() ? QString("systemd-run") : QString("ssh"));
                QVERIFY(start.shell().contains(p.id));
                QVERIFY(serviceCommand(c, "stop").shell().contains(p.id));
            }
        }
    }
    void shellQuotingAndTransportSelection() {
        QTemporaryDir dir;
        const auto dangerous = dir.filePath("a'; touch SHOULD_NOT_EXIST; $(false)");
        QProcess shell;
        shell.start("sh", {"-c", Command{"printf", {"%s", dangerous}, {}, {}}.shell()});
        QVERIFY(shell.waitForFinished());
        QCOMPARE(QString::fromUtf8(shell.readAllStandardOutput()), dangerous);
        QVERIFY(!QFileInfo::exists("SHOULD_NOT_EXIST"));
        for (const auto& transport : frontendTransports()) {
            auto p = Profile::create();
            p.frontend.transport = transport;
            const auto plan = LaunchPlan::forProfile(p);
            QVERIFY(plan.ui.command.arguments.contains("codex-ui-" + transport));
            QVERIFY(plan.probe.arguments.contains("codex-bridge-client-" + transport));
            if (transport != "unix") {
                QVERIFY(plan.ui.command.arguments.contains("--disabled=true"));
            }
        }
    }
    void independentHomesAndOwnedLifecycle() {
        FixtureTools tools;
        auto a = Profile::create("A");
        a.codexHome = tools.dir.filePath("home-a");
        a.probeExecutable = tools.dir.filePath("probe");
        auto b = Profile::create("B");
        b.codexHome = tools.dir.filePath("home-b");
        b.probeExecutable = a.probeExecutable;
        QVERIFY(b.errors({a}).isEmpty());
        QVERIFY(a.unit("bridge") != b.unit("bridge"));
        ProfileRuntime one(a), two(b);
        one.start();
        two.start();
        QTRY_VERIFY_WITH_TIMEOUT(one.ready() && two.ready(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!one.busy() && !two.busy(), 10000);
        QTRY_VERIFY(one.components().value("CodexUI").startsWith("Running"));
        QFile starts(tools.dir.filePath("starts"));
        QVERIFY(starts.open(QIODevice::ReadOnly));
        QCOMPARE(starts.readAll().count('\n'), 4);
        starts.close();
        one.start();
        QTRY_VERIFY_WITH_TIMEOUT(!one.busy(), 10000);
        QVERIFY(starts.open(QIODevice::ReadOnly));
        QCOMPARE(starts.readAll().count('\n'), 4);
        one.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!one.busy(), 10000);
        QVERIFY(!QFileInfo::exists(tools.dir.filePath(a.unit("bridge"))));
        QVERIFY(QFileInfo::exists(tools.dir.filePath(b.unit("bridge"))));
        two.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!two.busy(), 10000);
    }
    void externalComponentsAndSshLoss() {
        FixtureTools tools;
        auto p = Profile::create("External");
        p.bridge.ownership = Ownership::External;
        p.provider.transport = "websocket-ipv4";
        p.appServer.ownership = Ownership::External;
        p.probeExecutable = tools.dir.filePath("probe");
        p.codexHome = tools.dir.filePath("external-home");
        ProfileRuntime external(p);
        external.start(false);
        QTRY_VERIFY_WITH_TIMEOUT(external.ready(), 10000);
        QVERIFY(!QFileInfo::exists(tools.dir.filePath("starts")));
        QVERIFY(!QDir(p.codexHome).exists());
        external.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!external.busy(), 10000);
        p.bridge.ownership = Ownership::Ssh;
        p.bridge.sshTarget = "remote";
        p.frontend.transport = "ipv4";
        qputenv("CODEXMANAGER_FAIL_SSH", "1");
        ProfileRuntime remote(p);
        remote.start(false);
        QTRY_VERIFY_WITH_TIMEOUT(!remote.busy(), 10000);
        QVERIFY(remote.components().value("Bridge").contains("supervision unavailable"));
        QVERIFY(remote.status().contains("SSH supervision unavailable"));
    }
    void monitorRemainsObserver() {
        FixtureTools tools;
        qputenv("CODEXMANAGER_RECLAIM_CONTROL", "1");
        auto p = Profile::create();
        p.bridge.ownership = Ownership::External;
        p.probeExecutable = tools.dir.filePath("probe");
        ProfileRuntime runtime(p);
        runtime.start(false);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.ready() && !runtime.busy(), 5000);
        QFile file(tools.dir.filePath("releases"));
        QTRY_VERIFY_WITH_TIMEOUT(file.exists(), 5000);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("2"));
        runtime.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.busy(), 5000);
        QVERIFY(!runtime.ready());
    }
    void unixProviderOrderingRecoveryAndOwnership() {
        FixtureTools tools;
        auto p = Profile::create("Remote Unix");
        p.codexHome = tools.dir.filePath("home");
        p.bridge.ownership = Ownership::Ssh;
        p.bridge.sshTarget = "host-b";
        p.appServer.ownership = Ownership::Ssh;
        p.appServer.sshTarget = "host-b";
        p.provider.transport = "unix";
        p.frontend.transport = "ipv4";
        p.probeExecutable = tools.dir.filePath("probe");
        {
            ProfileRuntime runtime(p);
            runtime.start(false);
            QTRY_VERIFY_WITH_TIMEOUT(runtime.ready() && !runtime.busy(), 10000);
            QFile f(tools.dir.filePath("starts"));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), (p.unit("appserver") + '\n' + p.unit("bridge") + '\n').toUtf8());
            qputenv("CODEXMANAGER_FAIL_SSH", "1");
            runtime.refresh();
            QTRY_VERIFY_WITH_TIMEOUT(!runtime.busy(), 10000);
            QVERIFY(runtime.hasOwnedProcesses());
            QVERIFY(runtime.components().value("Bridge").contains("unknown"));
            qunsetenv("CODEXMANAGER_FAIL_SSH");
            runtime.start(false);
            QTRY_VERIFY_WITH_TIMEOUT(!runtime.busy() && runtime.ready(), 10000);
        }
        ProfileRuntime adopted(p);
        adopted.start(false);
        QTRY_VERIFY_WITH_TIMEOUT(adopted.ready() && !adopted.busy(), 10000);
        QFile f(tools.dir.filePath("starts"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll().count('\n'), 2);
        qputenv("CODEXMANAGER_FOREIGN_UNIT", "1");
        adopted.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!adopted.busy(), 10000);
        QVERIFY(QFileInfo::exists(tools.dir.filePath(p.unit("bridge"))));
        QVERIFY(adopted.components().value("Bridge").contains("conflict"));
        qunsetenv("CODEXMANAGER_FOREIGN_UNIT");
        adopted.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!adopted.busy(), 10000);
        QVERIFY(!QFileInfo::exists(tools.dir.filePath(p.unit("bridge"))));
    }
    void startupFailureAndCancellation() {
        FixtureTools tools;
        auto p = Profile::create();
        p.codexHome = tools.dir.filePath("home");
        p.probeExecutable = tools.dir.filePath("probe");
        qputenv("CODEXMANAGER_FAIL_START", "1");
        ProfileRuntime runtime(p);
        runtime.start();
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.busy(), 10000);
        QVERIFY(!runtime.ready());
        QVERIFY(runtime.status().contains("Could not start"));
        QVERIFY(!QFileInfo::exists(tools.dir.filePath(p.unit("ui"))));
        qunsetenv("CODEXMANAGER_FAIL_START");
        runtime.start();
        runtime.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.busy(), 10000);
        QVERIFY(!QFileInfo::exists(tools.dir.filePath(p.unit("bridge"))));
        QVERIFY(!QFileInfo::exists(tools.dir.filePath(p.unit("ui"))));
    }
    void installedCommandConfigurations() {
        QTemporaryDir dir;
        const QString bridge = QStandardPaths::findExecutable("codex-bridge");
        const QString probe = QStandardPaths::findExecutable("codex-bridge-client");
        QVERIFY(!bridge.isEmpty());
        QVERIFY(!probe.isEmpty());
        const QString certificate = dir.filePath("certificate.pem");
        writeFile(certificate, "configuration-only fixture\n");
        for (const auto& transport : frontendTransports()) {
            for (const auto& provider : {"stdio", "unix", "websocket-ipv4", "websocket-ipv6"}) {
                auto p = Profile::create();
                p.codexHome = dir.filePath("home");
                p.bridgeExecutable = bridge;
                p.probeExecutable = probe;
                p.uiExecutable = CODEXMANAGER_UI_EXECUTABLE;
                p.provider.transport = provider;
                p.frontend.transport = transport;
                if (transport.contains("ipv6")) {
                    p.frontend.host = "::1";
                    p.frontend.bindHost = "::1";
                }
                if (transport.startsWith("rfcomm")) {
                    p.frontend.host = "00:00:00:00:00:00";
                    p.frontend.bindHost = p.frontend.host;
                }
                p.frontend.certificate = certificate;
                p.frontend.privateKey = certificate;
                p.frontend.caCertificate = certificate;
                const auto plan = LaunchPlan::forProfile(p);
                QVector<Component> components{
                    plan.backends.last(), plan.ui, {"Probe", Host{}, {}, {p.probeExecutable, plan.probe.arguments, {}, {}}}};
                for (const auto& component : components) {
                    const auto check = component.configurationCheck();
                    QProcess process;
                    auto env = QProcessEnvironment::systemEnvironment();
                    env.insert("QT_QPA_PLATFORM", "offscreen");
                    process.setProcessEnvironment(env);
                    process.setProcessChannelMode(QProcess::MergedChannels);
                    process.start(check.program, check.arguments);
                    QVERIFY(process.waitForFinished(10000));
                    const auto output = process.readAllStandardOutput();
                    QVERIFY2(component.acceptsConfiguration(process.exitCode(), QString::fromUtf8(output)),
                             qPrintable(transport + '/' + provider + '/' + component.name + ": " + QString::fromUtf8(output)));
                }
            }
        }
    }
    void editorConfigurationAndCapabilities() {
        FixtureTools tools;
        auto p = Profile::create("Before");
        p.codexHome = tools.dir.filePath("home");
        ProfileEditor editor(p, {});
        editor.show();
        const auto combos = editor.findChildren<QComboBox*>();
        auto frontend = std::find_if(combos.cbegin(), combos.cend(), [](QComboBox* combo) {
            return combo->findData("rfcomm-tls") >= 0;
        });
        QVERIFY(frontend != combos.cend());
        QTRY_VERIFY_WITH_TIMEOUT((*frontend)->isEnabled(), 5000);
        auto fields = editor.findChildren<QLineEdit*>();
        auto name = std::find_if(fields.cbegin(), fields.cend(), [](QLineEdit* field) {
            return field->accessibleName() == "Name";
        });
        QVERIFY(name != fields.cend());
        (*name)->setText("After & independent");
        (*frontend)->setCurrentIndex((*frontend)->findData("websocket-ipv6"));
        auto* buttons = editor.findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        QCOMPARE(editor.result(), int(QDialog::Accepted));
        QCOMPARE(editor.profile().name, QString("After & independent"));
        QCOMPARE(editor.profile().frontend.transport, QString("websocket-ipv6"));
    }
    void closeDuringDiscoveryAndSupervision() {
        FixtureTools tools;
        auto p = Profile::create();
        p.codexHome = tools.dir.filePath("home");
        qputenv("CODEXMANAGER_SLOW_HELP", "1");
        {
            ProfileEditor editor(p, {});
            editor.show();
            QTest::qWait(10);
        }
        qunsetenv("CODEXMANAGER_SLOW_HELP");
        QString error;
        const auto path = tools.dir.filePath("profiles.json");
        QVERIFY(ProfileStore(path).save({p}, &error));
        {
            ManagerWindow window(path);
            window.show();
            const auto runtimes = window.findChildren<ProfileRuntime*>();
            QCOMPARE(runtimes.size(), 1);
            runtimes.first()->start(false);
            QApplication::processEvents();
        }
        QVERIFY(!QFileInfo::exists(tools.dir.filePath(p.unit("bridge"))));
    }
    void profileWindowActivationAndIdentity() {
        QTemporaryDir dir;
        qputenv("CODEXUI_MANAGER_SOCKET", dir.filePath("window.sock").toUtf8());
        qputenv("CODEXUI_PROFILE_NAME", "Example profile");
        QWidget window;
        window.setStyleSheet("QWidget { padding: 1px; }");
        qputenv("CODEXUI_PROFILE_ACCENT", "#5471a6");
        codexui::ProfileWindow first;
        QCOMPARE(first.prepare(), 0);
        QVERIFY(first.attach(window));
        QCOMPARE(window.windowTitle(), QString("Example profile · CodexUI"));
        QVERIFY(window.styleSheet().contains("padding: 1px"));
        QVERIFY(window.styleSheet().contains("#5471a6"));
        qunsetenv("CODEXUI_PROFILE_ACCENT");
        codexui::ProfileWindow second;
        QCOMPARE(second.prepare(), 1);
        QTRY_VERIFY(window.isVisible());
        qunsetenv("CODEXUI_MANAGER_SOCKET");
        qunsetenv("CODEXUI_PROFILE_NAME");
    }
    void liveLocalProfiles() {
        if (!qEnvironmentVariableIsSet("CODEXMANAGER_LIVE_QUALIFICATION")) {
            QSKIP("Run explicitly with isolated real systemd services; see profile-manager qualification.");
        }
        QTemporaryDir dir;
        auto a = Profile::create("Qualification stdio");
        auto b = Profile::create("Qualification Unix");
        a.codexHome = dir.filePath("home-a");
        b.codexHome = dir.filePath("home-b");
        b.provider.transport = "unix";
        for (auto* p : {&a, &b}) {
            p->bridgeExecutable = QStandardPaths::findExecutable("codex-bridge");
            p->appServerExecutable = QStandardPaths::findExecutable("codex");
            p->probeExecutable = QStandardPaths::findExecutable("codex-bridge-client");
            p->uiExecutable = CODEXMANAGER_UI_EXECUTABLE;
        }
        ProfileRuntime one(a), two(b);
        connect(&one, &ProfileRuntime::logAdded, this, [](const QString& line) {
            qInfo().noquote() << "stdio:" << line;
        });
        connect(&two, &ProfileRuntime::logAdded, this, [](const QString& line) {
            qInfo().noquote() << "unix:" << line;
        });
        connect(&one, &ProfileRuntime::changed, this, [&] {
            qInfo().noquote() << "stdio state:" << one.status();
        });
        connect(&two, &ProfileRuntime::changed, this, [&] {
            qInfo().noquote() << "unix state:" << two.status();
        });
        const auto cleanup = qScopeGuard([&] {
            one.stop();
            two.stop();
            QElapsedTimer timer;
            timer.start();
            while ((one.busy() || two.busy()) && timer.elapsed() < 25000) {
                QTest::qWait(20);
            }
            if (!one.hasOwnedProcesses() && !two.hasOwnedProcesses()) {
                for (const auto& p : {a, b}) {
                    for (const auto& socket : {p.frontend.socketPath, p.provider.socketPath, p.runtimeSocket()}) {
                        QFile::remove(socket);
                        QFile::remove(socket + ".lock");
                    }
                }
            }
        });
        one.start();
        two.start();
        QTRY_VERIFY2_WITH_TIMEOUT(
            one.ready() && two.ready(), qPrintable(one.status() + '\n' + one.logs() + '\n' + two.status() + '\n' + two.logs()), 12000);
        QTRY_VERIFY_WITH_TIMEOUT(!one.busy() && !two.busy(), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(one.accountState(), ProfileRuntime::AccountState::SignedOut, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(two.accountState(), ProfileRuntime::AccountState::SignedOut, 10000);
        // Explicit browsing remains available without authenticating or sending inference.
        one.openUi();
        two.openUi();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(a.runtimeSocket()) && QFileInfo::exists(b.runtimeSocket()), 20000);
        QLocalSocket first, second;
        first.connectToServer(a.runtimeSocket());
        second.connectToServer(b.runtimeSocket());
        QVERIFY(first.waitForConnected(1000));
        QVERIFY(second.waitForConnected(1000));
        first.disconnectFromServer();
        second.disconnectFromServer();
        one.openUi();
        QTRY_VERIFY_WITH_TIMEOUT(!one.busy(), 10000);
        std::optional<DeletionPlan> deletion;
        one.inspectDeletion({a, b}, [&](DeletionPlan plan) {
            deletion = std::move(plan);
        });
        QTRY_VERIFY_WITH_TIMEOUT(deletion.has_value(), 10000);
        QVERIFY2(deletion->error.isEmpty(), qPrintable(deletion->error));
        QSignalSpy deleted(&one, &ProfileRuntime::deletionFinished);
        one.deleteData(*deletion);
        QTRY_COMPARE_WITH_TIMEOUT(deleted.size(), 1, 20000);
        QVERIFY2(deleted.first().first().toBool(), qPrintable(deleted.first().last().toString()));
        QVERIFY(!QFileInfo::exists(a.codexHome));
        QVERIFY(!QFileInfo::exists(a.settingsDirectory()));
        QVERIFY(!one.hasOwnedProcesses());
        QVERIFY(two.ready());
        two.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!two.busy(), 20000);
        QVERIFY(!two.hasOwnedProcesses());
    }
    void managerOverviewAndCreationDialog() {
        FixtureTools tools;
        Profile p = Profile::create("Research");
        p.codexHome = tools.dir.filePath("home");
        p.bridge.ownership = Ownership::External;
        QString error;
        const auto file = tools.dir.filePath("profiles.json");
        QVERIFY(ProfileStore(file).save({p}, &error));
        ManagerWindow window(file);
        window.show();
        QTest::qWait(30);
        QVERIFY(window.isVisible());
        const auto labels = window.findChildren<QLabel*>();
        QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](QLabel* label) {
            return label->text() == "Research";
        }));
        const auto buttons = window.findChildren<QPushButton*>();
        auto it = std::find_if(buttons.cbegin(), buttons.cend(), [](QPushButton* b) {
            return b->text().contains("New profile");
        });
        QVERIFY(it != buttons.cend());
        QTimer::singleShot(30, [] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                dialog->reject();
            }
        });
        QTest::mouseClick(*it, Qt::LeftButton);
    }
    void managerGeometryAndDetails_data() {
        QTest::addColumn<int>("width");
        QTest::newRow("narrow") << 740;
        QTest::newRow("normal") << 1240;
        QTest::newRow("wide") << 1600;
    }
    void managerGeometryAndDetails() {
        QFETCH(int, width);
        FixtureTools tools;
        QVector<Profile> profiles;
        for (const auto& name : {"Daily development", "Research & experiments", "Remote lab"}) {
            auto p = Profile::create(name);
            p.codexHome = tools.dir.filePath(p.id);
            p.bridge.ownership = Ownership::External;
            p.description = "An independent environment for focused work.";
            p.accent = profiles.isEmpty() ? "blue" : profiles.size() == 1 ? "purple" : "teal";
            profiles << p;
        }
        QString error;
        const auto file = tools.dir.filePath("profiles.json");
        QVERIFY(ProfileStore(file).save(profiles, &error));
        ManagerWindow window(file);
        window.resize(width, 830);
        window.show();
        QTest::qWait(50);
        const QString evidence = qEnvironmentVariable("CODEXMANAGER_EVIDENCE_DIR");
        const auto capture = [&](const QString& name) {
            if (!evidence.isEmpty()) {
                QDir().mkpath(evidence);
                QVERIFY(window.grab().save(evidence + '/' + name + '-' + QString::number(width) + ".png"));
            }
        };
        capture("overview");
        const auto buttons = window.findChildren<QPushButton*>();
        auto detail = std::find_if(buttons.cbegin(), buttons.cend(), [](QPushButton* b) {
            return b->text() == "Details →";
        });
        QVERIFY(detail != buttons.cend());
        QTest::mouseClick(*detail, Qt::LeftButton);
        QTest::qWait(30);
        auto* tabs = window.findChild<QTabWidget*>();
        QVERIFY(tabs);
        for (int index = 0; index < tabs->count(); ++index) {
            tabs->setCurrentIndex(index);
            QTest::qWait(10);
            capture("detail-" + QString::number(index));
        }
        tabs->setCurrentIndex(0);
        const auto toolsButtons = window.findChildren<QToolButton*>();
        auto disclosure = std::find_if(toolsButtons.cbegin(), toolsButtons.cend(), [](QToolButton* b) {
            return b->text() == "Technical details";
        });
        QVERIFY(disclosure != toolsButtons.cend());
        QTest::mouseClick(*disclosure, Qt::LeftButton);
        QTest::qWait(10);
        capture("technical-expanded");
        QTimer::singleShot(50, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                auto* editorTabs = dialog->findChild<QTabWidget*>();
                if (editorTabs) {
                    for (int i = 0; i < editorTabs->count(); ++i) {
                        editorTabs->setCurrentIndex(i);
                        QApplication::processEvents();
                        if (!evidence.isEmpty()) {
                            QVERIFY(
                                dialog->grab().save(evidence + "/editor-" + QString::number(i) + '-' + QString::number(width) + ".png"));
                        }
                    }
                }
                dialog->reject();
            }
        });
        auto add = std::find_if(buttons.cbegin(), buttons.cend(), [](QPushButton* b) {
            return b->text().contains("New profile");
        });
        QVERIFY(add != buttons.cend());
        QTest::mouseClick(*add, Qt::LeftButton);
    }
};
int main(int argc, char** argv) {
    QCoreApplication::setApplicationName("CodexManagerTest");
    int result = 0;
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--fixture") {
        QStringList args;
        for (int i = 0; i < argc; ++i) {
            args << QString::fromLocal8Bit(argv[i]);
        }
        result = fixture(args);
    } else {
        QTemporaryDir configuration;
        qputenv("XDG_CONFIG_HOME", configuration.path().toUtf8());
        QApplication app(argc, argv);
        ManagerTest test;
        result = QTest::qExec(&test, argc, argv);
    }
    return result;
}
#include "ManagerTest.moc"
