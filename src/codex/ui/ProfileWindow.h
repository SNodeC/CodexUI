// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <memory>
#include <qobject.h>
#include <qstring.h>

class QWidget;

namespace codexui {
    // A manager-launched window exposes only activation, never conversation operations.
    class ProfileWindow final : public QObject {
    public:
        explicit ProfileWindow(QObject* parent = nullptr);
        // 0: normal startup, 1: activated an existing window, -1: endpoint is owned elsewhere.
        [[nodiscard]] int prepare();
        [[nodiscard]] bool attach(QWidget& window);

    private:
        QString socketPath_;
        std::unique_ptr<QLockFile> lock_;
        QLocalServer server_;
    };
} // namespace codexui
