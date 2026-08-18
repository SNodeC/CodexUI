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
#include <ai/openai/codex/frontend/Protocol.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

namespace codexui::detail {

[[nodiscard]] std::optional<QString> unixPeerCredentialError(qintptr socketDescriptor, uid_t expectedUserId) noexcept;

struct StateUpdateScope {
    struct ItemContentIdentity {
        QString threadId;
        QString turnId;
        QString itemId;

        bool operator==(const ItemContentIdentity&) const = default;
    };

    QStringList affectedThreadIds;
    QStringList fullyAffectedThreadIds;
    QStringList affectedInspectorThreadIds;
    std::vector<ItemContentIdentity> affectedItemContents;
    bool allThreadsAffected = false;
    bool allInspectorsAffected = false;
    bool sidebarAffected = false;
    bool hasPresentationChange = false;
};

[[nodiscard]] StateUpdateScope
stateUpdateScope(const ai::openai::codex::frontend::client::StateUpdate& update);

} // namespace codexui::detail

namespace codexui {

struct FrontendSessionTestAccess;

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
    void reconnectToBackend();
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
    void statusChanged();
    void stateChanged(const codexui::detail::StateUpdateScope& scope);

private:
    friend struct FrontendSessionTestAccess;

    static constexpr int initialReconnectDelayMs = 250;
    static constexpr int maximumReconnectDelayMs = 5'000;
    static constexpr int outboundDrainRetryMs = 10;
    static constexpr qint64 maximumBufferedOutboundBytes = static_cast<qint64>(
        4U * (ai::openai::codex::frontend::DefaultFrontendMaximumInboundMessageBytes + 1U));

    using Client = ai::openai::codex::frontend::client::Client;
    using Connection = ai::openai::codex::frontend::client::Connection;
    using OutboundMessage = ai::openai::codex::frontend::client::OutboundMessage;
    using SendResult = ai::openai::codex::frontend::client::SendResult;
    using OutboundWriter = std::function<qint64(const char*, qint64)>;

    struct PendingWrite
    {
        std::string frame;
        qint64 offset = 0;
    };

    enum class DrainResult { Progress, Blocked, Failed, Reset };

    static QString defaultSocketPath();
    void socketConnected();
    void socketReadyRead();
    void socketBytesWritten(qint64 bytes);
    void socketDisconnected();
    void socketFailed(QLocalSocket::LocalSocketError error);
    void handleConnectionStateChange(const ai::openai::codex::frontend::client::ConnectionStateChange& change);
    void reportDiagnostic(QString message);
    void scheduleSocketRead();
    void clearInbound() noexcept;
    [[nodiscard]] bool hasCompleteInboundFrame() const noexcept;
    void compactInbound() noexcept;
    void reconcileRequestedThreadReads();
    void startConnection();
    void scheduleReconnect();
    void retryConnection();
    void resetReconnectPolicy();
    void failWithoutReconnect(QString reason);
    [[nodiscard]] SendResult send(OutboundMessage&& message);
    [[nodiscard]] SendResult sendToTransport(OutboundMessage&& message,
                                             bool transportConnected,
                                             qint64 socketBufferedBytes,
                                             const OutboundWriter& writer) noexcept;
    [[nodiscard]] SendResult acceptOutbound(OutboundMessage&& message,
                                            qint64 socketBufferedBytes,
                                            const OutboundWriter& writer) noexcept;
    [[nodiscard]] DrainResult drainOutbound(const OutboundWriter& writer) noexcept;
    void drainSocketWrites();
    void scheduleOutboundDrain();
    void clearOutbound() noexcept;
    void closeTransport(QString reason) noexcept;
    void setLifecycle(Lifecycle value, QString detail = {});

    QLocalSocket socket;
    QTimer reconnectTimer;
    QTimer outboundDrainTimer;
    QByteArray inboundBuffer;
    qsizetype inboundOffset = 0;
    std::unique_ptr<Client> client;
    Connection connection;
    ai::openai::codex::frontend::client::State currentState;
    Lifecycle currentLifecycle = Lifecycle::Disconnected;
    QString detail;
    QString diagnosticDetail;
    std::set<std::string> requestedThreadReads;
    std::deque<PendingWrite> pendingWrites;
    qint64 pendingWriteBytes = 0;
    std::uint64_t outboundEpoch = 0;
    int reconnectDelayMs = initialReconnectDelayMs;
    bool receiveContinuationScheduled = false;
    bool drainingOutbound = false;
    bool outboundClearPending = false;
    bool automaticReconnectEnabled = true;
    bool localShutdown = false;
};

} // namespace codexui

#endif // CODEXUI_APP_FRONTENDSESSION_H
