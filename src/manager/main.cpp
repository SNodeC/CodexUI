// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/ManagerWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("CodexManager");
    QGuiApplication::setDesktopFileName("codex-manager");
    app.setApplicationDisplayName("CodexManager");
    app.setApplicationVersion("1.0.0");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("Independent Codex profiles, local and remote");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"profiles", "Profile-store JSON file", "file"});
    parser.addOption({"screenshot", "Save a UI preview and exit without launching profile components", "file"});
    parser.process(app);
    const QString defaultFile = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/profiles.json";
    const QString path = parser.isSet("profiles") ? parser.value("profiles") : defaultFile;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QLockFile lock(path + ".lock");
    int result = 1;
    if (!lock.tryLock(0)) {
        QMessageBox::information(
            nullptr,
            "CodexManager already open",
            "Another manager is using this profile store. Open its existing window or use a different --profiles file.");
    } else {
        codexui::manager::ManagerWindow window(path);
        window.show();
        if (parser.isSet("screenshot")) {
            QTimer::singleShot(500, &app, [&] {
                app.exit(window.grab().save(parser.value("screenshot")) ? 0 : 1);
            });
        }
        result = app.exec();
    }
    return result;
}
