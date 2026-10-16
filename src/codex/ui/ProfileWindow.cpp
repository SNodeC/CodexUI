// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "codex/ui/ProfileWindow.h"

#include <QApplication>
#include <QColor>
#include <QFileInfo>
#include <QLocalSocket>
#include <QSettings>
#include <QWindow>

namespace codexui {
    ProfileWindow::ProfileWindow(QObject* parent)
        : QObject(parent)
        , socketPath_(qEnvironmentVariable("CODEXUI_MANAGER_SOCKET")) {
    }
    int ProfileWindow::prepare() {
        int result = 0;
        if (!socketPath_.isEmpty()) {
            if (!QFileInfo(socketPath_).isAbsolute()) {
                result = -1;
            } else {
                QLocalSocket existing;
                existing.connectToServer(socketPath_);
                if (existing.waitForConnected(200)) {
                    existing.write("activate\n");
                    existing.waitForBytesWritten(200);
                    result = 1;
                } else {
                    lock_ = std::make_unique<QLockFile>(socketPath_ + ".lock");
                    if (!lock_->tryLock(0)) {
                        result = -1;
                    } else {
                        QLocalServer::removeServer(socketPath_);
                        const QString directory = qEnvironmentVariable("CODEXUI_SETTINGS_DIRECTORY");
                        if (!directory.isEmpty()) {
                            QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, directory);
                        }
                    }
                }
            }
        }
        return result;
    }
    bool ProfileWindow::attach(QWidget& window) {
        const QString name = qEnvironmentVariable("CODEXUI_PROFILE_NAME");
        if (!name.isEmpty()) {
            window.setWindowTitle(name + QStringLiteral(" · CodexUI"));
        }
        const QColor accent(qEnvironmentVariable("CODEXUI_PROFILE_ACCENT"));
        if (accent.isValid()) {
            window.setStyleSheet(window.styleSheet() + QString("QMainWindow { border-top: 4px solid %1; }").arg(accent.name()));
        }
        bool result = true;
        if (!socketPath_.isEmpty()) {
            server_.setSocketOptions(QLocalServer::UserAccessOption);
            connect(&server_, &QLocalServer::newConnection, &window, [this, &window] {
                while (auto* socket = server_.nextPendingConnection()) {
                    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                    socket->close();
                    window.showNormal();
                    window.raise();
                    window.activateWindow();
                    if (auto* handle = window.windowHandle()) {
                        handle->requestActivate();
                    }
                }
            });
            result = server_.listen(socketPath_);
        }
        return result;
    }
} // namespace codexui
