// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_APP_FRONTENDSESSION_H
#define CODEXUI_APP_FRONTENDSESSION_H

#include <QByteArray>
#include <QObject>
#include <QLocalSocket>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <ai/openai/codex/frontend/client/Client.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace codexui {

class FrontendSession : public QObject
{
    Q_OBJECT

public:
    enum class Lifecycle { Disconnected, Connecting, Authenticating, Synchronizing, Ready, Failed };

    using OperationCompletion = std::function<void(const QString& error)>;
    using ThreadStartCompletion = std::function<void(const QString& threadId, const QString& error)>;

    explicit FrontendSession(QObject* parent = nullptr);
    ~FrontendSession() override;

    void connectToBackend();
    [[nodiscard]] Lifecycle lifecycle() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] static std::optional<QString> promptValidationError(const QString& prompt);
    [[nodiscard]] const ai::openai::codex::frontend::client::State& state() const noexcept;
    [[nodiscard]] bool ownsController() const noexcept;
    void loadThread(const QString& threadId);
    [[nodiscard]] std::optional<QString> acquireController(OperationCompletion completion);
    [[nodiscard]] std::optional<QString> startThread(ThreadStartCompletion completion);
    [[nodiscard]] std::optional<QString> resumeThread(const QString& threadId, ThreadStartCompletion completion);
    [[nodiscard]] std::optional<QString> startTurn(const QString& threadId,
                                                   const QString& prompt,
                                                   OperationCompletion completion);
    [[nodiscard]] std::optional<QString> interruptTurn(const QString& threadId,
                                                       const QString& turnId,
                                                       OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    respondApproval(const ai::openai::codex::frontend::client::PendingRequestId& requestId,
                    ai::openai::codex::typed::ApprovalDecision decision,
                    OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    respondApplyPatchApproval(const ai::openai::codex::frontend::client::PendingRequestId& requestId,
                              ai::openai::codex::typed::ApplyPatchApprovalResponse response,
                              OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    respondExecCommandApproval(const ai::openai::codex::frontend::client::PendingRequestId& requestId,
                               ai::openai::codex::typed::ExecCommandApprovalResponse response,
                               OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    respondUserInput(const ai::openai::codex::frontend::client::PendingRequestId& requestId,
                     std::vector<ai::openai::codex::typed::UserInputAnswer> answers,
                     OperationCompletion completion);

signals:
    void lifecycleChanged();
    void stateChanged(const QStringList& affectedThreadIds,
                      bool allThreadsAffected,
                      bool inspectorAffected);

private:
    static constexpr int initialReconnectDelayMs = 250;
    static constexpr int maximumReconnectDelayMs = 5'000;

    using Client = ai::openai::codex::frontend::client::Client;
    using Connection = ai::openai::codex::frontend::client::Connection;
    using OutboundMessage = ai::openai::codex::frontend::client::OutboundMessage;
    using SendResult = ai::openai::codex::frontend::client::SendResult;

    static QString defaultSocketPath();
    void socketConnected();
    void socketReadyRead();
    void socketDisconnected();
    void socketFailed(QLocalSocket::LocalSocketError error);
    void scheduleSocketRead();
    void reconcileRequestedThreadReads();
    void startConnection();
    void scheduleReconnect();
    void retryConnection();
    void failWithoutReconnect(QString reason);
    [[nodiscard]] SendResult send(OutboundMessage message) noexcept;
    void closeTransport(QString reason) noexcept;
    void setLifecycle(Lifecycle value, QString detail = {});

    QLocalSocket socket;
    QTimer reconnectTimer;
    QByteArray inboundBuffer;
    std::unique_ptr<Client> client;
    Connection connection;
    ai::openai::codex::frontend::client::State currentState;
    Lifecycle currentLifecycle = Lifecycle::Disconnected;
    QString detail;
    std::set<std::string> requestedThreadReads;
    int reconnectDelayMs = initialReconnectDelayMs;
    bool receiveContinuationScheduled = false;
    bool automaticReconnectEnabled = true;
    bool localShutdown = false;
};

} // namespace codexui

#endif // CODEXUI_APP_FRONTENDSESSION_H
