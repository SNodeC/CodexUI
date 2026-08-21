// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_APP_FRONTENDSESSION_H
#define CODEXUI_APP_FRONTENDSESSION_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <ai/openai/codex/frontend/client/Client.h>
#include <ai/openai/codex/typed/Models.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace codexui::detail {

// This bounds only optional GUI presentation metadata. Exceeding it is
// lossless: the newest immutable State remains in the mailbox and the view
// performs an authoritative replacement refresh instead of retaining deltas.
inline constexpr std::uint64_t maximumCoalescedContentDeltaBytes =
    1024U * 1024U;
inline constexpr qsizetype maximumCoalescedPresentationIdentities = 1'024;

struct StateUpdateScope {
    struct ItemContentAppend {
        std::uint64_t baseContentBytes = 0;
        std::uint64_t discardPrefixBytes = 0;
        QByteArray deltaUtf8;

        bool operator==(const ItemContentAppend&) const = default;
    };

    struct ItemContentIdentity {
        QString threadId;
        QString turnId;
        QString itemId;
        ai::openai::codex::frontend::client::ItemContentChannel channel =
            ai::openai::codex::frontend::client::ItemContentChannel::AgentText;
        std::optional<ItemContentAppend> append;

        bool operator==(const ItemContentIdentity&) const = default;
    };

    QStringList affectedThreadIds;
    QStringList fullyAffectedThreadIds;
    QStringList affectedInspectorThreadIds;
    QStringList affectedSidebarThreadIds;
    std::vector<ItemContentIdentity> affectedItemContents;
    std::uint64_t coalescedContentDeltaBytes = 0;
    bool allThreadsAffected = false;
    bool allInspectorsAffected = false;
    bool allSidebarThreadsAffected = false;
    bool sidebarAffected = false;
    bool hasPresentationChange = false;
};

[[nodiscard]] StateUpdateScope
stateUpdateScope(const ai::openai::codex::frontend::client::StateUpdate& update);

} // namespace codexui::detail

namespace codexui {

struct FrontendSessionFacadeTestAccess;

class FrontendSession : public QObject
{
    Q_OBJECT

public:
    enum class Lifecycle { Disconnected, Connecting, Authenticating, Synchronizing, Ready, Failed };
    enum class ArchivedThreadDiscoveryStatus {
        InProgress,
        Complete,
        CompleteWithTruncation,
        Failed,
    };

    using OperationCompletion = std::function<void(const QString& error)>;
    using ThreadStartCompletion = std::function<void(const QString& threadId, const QString& error)>;
    using TurnStartCompletion = std::function<void(const QString& turnId, const QString& error)>;

    explicit FrontendSession(QObject* parent = nullptr);
    ~FrontendSession() override;

    FrontendSession(const FrontendSession&) = delete;
    FrontendSession& operator=(const FrontendSession&) = delete;

    void connectToBackend();
    void reconnectToBackend();
    void shutdown();
    [[nodiscard]] Lifecycle lifecycle() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] static std::optional<QString> promptValidationError(const QString& prompt);
    [[nodiscard]] const ai::openai::codex::frontend::client::State& state() const noexcept;
    [[nodiscard]] const std::vector<ai::openai::codex::typed::Model>& modelCatalog() const noexcept;
    [[nodiscard]] bool archivedThreadDiscoveryComplete() const noexcept;
    [[nodiscard]] bool archivedThreadDiscoveryTerminal() const noexcept;
    [[nodiscard]] ArchivedThreadDiscoveryStatus archivedThreadDiscoveryStatus() const noexcept;
    [[nodiscard]] bool ownsController() const noexcept;
    void loadThread(const QString& threadId);
    [[nodiscard]] std::optional<QString> acquireController(OperationCompletion completion);
    [[nodiscard]] std::optional<QString> startThread(ThreadStartCompletion completion);
    [[nodiscard]] std::optional<QString>
    startThread(ai::openai::codex::typed::ThreadStartParams parameters,
                ThreadStartCompletion completion);
    [[nodiscard]] std::optional<QString>
    resumeThread(const QString& threadId, ThreadStartCompletion completion);
    [[nodiscard]] std::optional<QString>
    resumeThread(ai::openai::codex::typed::ThreadResumeParams parameters,
                 ThreadStartCompletion completion);
    [[nodiscard]] std::optional<QString>
    startTurn(const QString& threadId, const QString& prompt, OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    startTurn(ai::openai::codex::typed::TurnStartParams parameters,
              const QString& prompt,
              TurnStartCompletion completion);
    [[nodiscard]] std::optional<QString>
    startTurn(ai::openai::codex::typed::TurnStartParams parameters,
              const QString& prompt,
              const QStringList& localImagePaths,
              TurnStartCompletion completion);
    [[nodiscard]] std::optional<QString>
    steerTurn(const QString& threadId,
              const QString& expectedTurnId,
              const QString& prompt,
              OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    steerTurn(const QString& threadId,
              const QString& expectedTurnId,
              const QString& prompt,
              const QStringList& localImagePaths,
              OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    forkThread(ai::openai::codex::typed::ThreadForkParams parameters,
               ThreadStartCompletion completion);
    [[nodiscard]] std::optional<QString>
    renameThread(const QString& threadId, const QString& name, OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    archiveThread(const QString& threadId, OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    unarchiveThread(const QString& threadId, OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    deleteThread(const QString& threadId, OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    interruptTurn(const QString& threadId, const QString& turnId, OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    respondApproval(const ai::openai::codex::frontend::client::PendingRequestId& requestId,
                    ai::openai::codex::typed::ApprovalDecision decision,
                    OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    respondApplyPatchApproval(
        const ai::openai::codex::frontend::client::PendingRequestId& requestId,
        ai::openai::codex::typed::ApplyPatchApprovalResponse response,
        OperationCompletion completion);
    [[nodiscard]] std::optional<QString>
    respondExecCommandApproval(
        const ai::openai::codex::frontend::client::PendingRequestId& requestId,
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
    void modelCatalogChanged();

private:
    friend struct FrontendSessionFacadeTestAccess;

    void enqueueStateForTest(std::uint64_t generation,
                             detail::StateUpdateScope scope);
    void enqueueStatusForTest(std::uint64_t generation, QString status);
    void enqueueStatusForTest(std::uint64_t generation,
                              Lifecycle lifecycle,
                              QString status);
    void enqueueLifecycleForTest(std::uint64_t generation,
                                 Lifecycle lifecycle,
                                 QString status);
    void enqueueModelsForTest(
        std::uint64_t generation,
        std::vector<ai::openai::codex::typed::Model> models);
    [[nodiscard]] std::size_t pendingStateCountForTest() const;
    [[nodiscard]] std::size_t pendingControlCountForTest() const;
    [[nodiscard]] std::size_t postedWakeCountForTest() const noexcept;
    [[nodiscard]] bool workerAffinityValidatedForTest() const noexcept;
    void trackOperationForTest(OperationCompletion completion);
    void completeOperationForTest(std::uint64_t generation,
                                  OperationCompletion completion,
                                  QString error);

    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace codexui

#endif // CODEXUI_APP_FRONTENDSESSION_H
