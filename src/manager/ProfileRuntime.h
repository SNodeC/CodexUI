// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "manager/DeletionPlan.h"

#include <QDeadlineTimer>
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
        enum class AccountState { Unknown, SignedOut, SignedIn, Rejected, NotRequired };
        [[nodiscard]] AccountState accountState() const {
            return accountState_;
        }
        [[nodiscard]] QString accountStatus() const;
        [[nodiscard]] QString authenticationMessage() const {
            return authenticationMessage_;
        }
        [[nodiscard]] QString loginUrl() const {
            return login_ ? login_->url : QString{};
        }
        [[nodiscard]] QString loginCode() const {
            return login_ ? login_->code : QString{};
        }
        [[nodiscard]] bool authenticating() const {
            return login_.has_value();
        }
        [[nodiscard]] bool authenticationAvailable() const;
        void refreshAccount();
        void signIn(const QString& type, const QString& apiKey = {});
        void signOut();
        void cancelLogin();
        void start(bool openUi = true);
        void check();
        void openUi();
        void stop();
        void refresh();
        void readLogs();
        void inspectDeletion(const QVector<Profile>& profiles, std::function<void(DeletionPlan)> done);
        void deleteData(DeletionPlan plan);
    signals:
        void changed();
        void logAdded(const QString& line);
        void deletionFinished(bool success, const QString& message);

    private:
        enum class State { Unknown, Inactive, Running, Starting, Foreign, External };
        static State serviceState(const Component& component, int code, const QString& output);
        void stopComponent(int index);
        struct Operation {
            Command command;
            std::function<void(int, QString)> done;
            int timeoutMs = 20000;
        };
        void enqueue(Command command, std::function<void(int, QString)> done, int timeoutMs = 20000);
        void runNext();
        void inspectDeletionTarget(DeletionPlan plan, int index, std::function<void(DeletionPlan)> done);
        void deleteTarget(int index);
        void finishDeletion(const QString& error);
        std::optional<DeletionPlan> deletion_;
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

        void accountRequest(const QString& method, const QJsonObject& parameters = {});
        void accountMessage(const QJsonObject& message);
        void beginAuthentication(QJsonObject parameters);
        void finishAuthentication(const QString& message);
        void forgetAccount();
        enum class LoginPhase { Claiming, Starting, Waiting, Cancelling, SigningOut };
        struct Login {
            LoginPhase phase = LoginPhase::Claiming;
            QJsonObject parameters;
            QJsonObject completion;
            QString id, url, code;
            QDeadlineTimer deadline{30000};
        };
        struct AccountRequest {
            QString method;
            QDeadlineTimer deadline{30000};
        };
        QMap<QString, AccountRequest> accountRequests_;
        quint64 accountSequence_ = 0;
        AccountState accountState_ = AccountState::Unknown;
        QString accountIdentity_;
        QString authenticationMessage_;
        QString controllerId_;
        std::optional<Login> login_;
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
