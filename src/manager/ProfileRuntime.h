// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "manager/LaunchPlan.h"

#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QTimer>
#include <functional>
#include <optional>

namespace codexui::manager {
    class ProfileRuntime final : public QObject {
        Q_OBJECT
    public:
        explicit ProfileRuntime(Profile profile, QObject* parent = nullptr);
        ~ProfileRuntime() override;
        [[nodiscard]] const Profile& profile() const {
            return profile_;
        }
        [[nodiscard]] QString status() const {
            return status_;
        }
        [[nodiscard]] QString logs() const {
            return logs_;
        }
        [[nodiscard]] QMap<QString, QString> components() const;
        [[nodiscard]] bool busy() const;
        [[nodiscard]] bool ready() const {
            return ready_ && observer_;
        }
        [[nodiscard]] bool hasOwnedProcesses() const;
        void start(bool openUi = true);
        void check();
        void openUi();
        void stop();
        void refresh();
        void readLogs();
    signals:
        void changed();
        void logAdded(const QString& line);

    private:
        enum class State { Unknown, Inactive, Running, Starting, Foreign, External };
        static State serviceState(const Component& component, int code, const QString& output);
        void stopComponent(int index);
        struct Operation {
            Command command;
            std::function<void(int, QString)> done;
        };
        void enqueue(Command command, std::function<void(int, QString)> done);
        void runNext();
        void launch(const Component& component, std::function<void(int, QString)> done);
        void finishOperation(int code);
        void ensureComponent(int index);
        void ensureUi();
        void beginProbe();
        void readProbe();
        void updateReadiness();
        void setStatus(QString text);
        void log(QString text);
        void fail(const QString& message);
        bool prepareHome();
        [[nodiscard]] QVector<Component> ownedComponents() const;

        Profile profile_;
        LaunchPlan plan_;
        QString status_ = QStringLiteral("Not checked");
        QString logs_;
        QMap<QString, State> componentStates_;
        QQueue<Operation> operations_;
        std::optional<Operation> current_;
        QProcess operation_;
        QByteArray operationOutput_;
        QTimer operationTimeout_;
        QProcess probe_;
        QByteArray probeOutput_;
        QTimer readinessTimeout_;
        QTimer refreshTimer_;
        bool starting_ = false;
        bool stopping_ = false;
        bool openRequested_ = false;
        bool ready_ = false;
        bool observer_ = false;
        QString probeConnectionId_;
    };
} // namespace codexui::manager
