// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/ManagerWebServer.h"
#include "manager/ManagerWindow.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QMessageBox>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char* argv[]) {
    codexui::manager::prepareManagerSignals();
    QApplication app(argc, argv);
    app.setApplicationName("CodexManager");
    QGuiApplication::setDesktopFileName("codex-manager");
    app.setApplicationDisplayName("CodexManager");
    app.setApplicationVersion("1.0.0");
    app.setQuitOnLastWindowClosed(false);
    const QString defaultFile = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/profiles.json";
    const auto options = codexui::manager::initializeManager(argc, argv, defaultFile.toStdString(), CODEXMANAGER_WEB_ROOT);
    const auto path = QString::fromStdString(options.profiles);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QLockFile lock(path + ".lock");
    int result = 1;
    if (options.configurationOnly) {
        codexui::manager::ManagerWebServer web(options);
        web.start();
        static_cast<void>(web.wait());
        result = 0;
    } else if (!lock.tryLock(0)) {
        QMessageBox::information(
            nullptr,
            "CodexManager already open",
            "Another manager is using this profile store. Open its existing window or use a different --profiles file.");
    } else {
        codexui::manager::ManagerController manager(path);
        codexui::manager::ManagerWindow window(manager);
        codexui::manager::ManagerWebServer web(options);
        QSocketNotifier notifier(web.descriptor(), QSocketNotifier::Read);
        QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
            web.drainWake();
            codexui::manager::ManagerWebServer::Request request;
            std::size_t processed = 0;
            while (processed < codexui::manager::ManagerWebServer::Capacity && web.take(request)) {
                ++processed;
                const auto body = QJsonDocument::fromJson(QByteArray::fromStdString(request.body)).object();
                const auto reply = manager.request(QString::fromStdString(request.action), body);
                web.reply({request.id,
                           reply.contains("error") && !reply.value("error").toString().isEmpty() ? 409 : 200,
                           QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString()});
            }
            std::string settings;
            while (web.configuration(settings)) {
                emit window.webSettingsReceived(QJsonDocument::fromJson(QByteArray::fromStdString(settings)).object());
            }
            if (web.stopped()) {
                app.exit(web.wait());
            }
        });
        QObject::connect(&manager, &codexui::manager::ManagerController::changed, &app, [&] {
            web.changed();
        });
        QObject::connect(&manager, &codexui::manager::ManagerController::profilesChanged, &app, [&] {
            web.changed();
        });
        QObject::connect(&window, &codexui::manager::ManagerWindow::webSettingsRequested, &app, [&](QJsonObject request) {
            if (!web.configure(QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString())) {
                request.insert("error", "Settings queue is full; no change was accepted.");
                emit window.webSettingsReceived(request);
            }
        });
        window.show();
        if (!options.screenshot.empty()) {
            QTimer::singleShot(500, &app, [&] {
                app.exit(window.grab().save(QString::fromStdString(options.screenshot)) ? 0 : 1);
            });
        }
        if (options.screenshot.empty()) {
            web.start();
        }
        result = app.exec();
    }
    return result;
}
