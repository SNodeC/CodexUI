// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"
#include "app/FrontendSessionWorker.h"

#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>
#include <variant>

namespace codexui {

namespace sdk = ai::openai::codex::frontend::client;

namespace {

bool appendUniqueBounded(QStringList& destination, const QStringList& source)
{
    for (const QString& value : source) {
        if (destination.contains(value))
            continue;
        if (destination.size()
            >= detail::maximumCoalescedPresentationIdentities)
            return false;
        destination.push_back(value);
    }
    return true;
}

void mergeScope(detail::StateUpdateScope& destination,
                const detail::StateUpdateScope& source)
{
    // A newer exact update for an identity supersedes an older removal. Apply
    // this before appending the source tombstones so remove-after-upsert still
    // wins while upsert-after-remove cannot clear a live selection.
    for (const QString& threadId : source.affectedThreadIds) {
        if (!source.removedThreadIds.contains(threadId))
            destination.removedThreadIds.removeAll(threadId);
    }
    destination.allThreadsAffected |= source.allThreadsAffected;
    destination.allInspectorsAffected |= source.allInspectorsAffected;
    destination.allSidebarThreadsAffected |= source.allSidebarThreadsAffected;
    destination.sidebarAffected |= source.sidebarAffected;
    destination.hasPresentationChange |= source.hasPresentationChange;
    destination.removedThreadIdsOverflowed |=
        source.removedThreadIdsOverflowed;
    if (!appendUniqueBounded(destination.removedThreadIds,
                             source.removedThreadIds)) {
        destination.removedThreadIdsOverflowed = true;
        destination.allThreadsAffected = true;
    }
    if (!destination.allThreadsAffected) {
        if (!appendUniqueBounded(destination.affectedThreadIds,
                                 source.affectedThreadIds)
            || !appendUniqueBounded(destination.fullyAffectedThreadIds,
                                    source.fullyAffectedThreadIds))
            destination.allThreadsAffected = true;
    }
    if (!destination.allInspectorsAffected
        && !appendUniqueBounded(destination.affectedInspectorThreadIds,
                                source.affectedInspectorThreadIds))
        destination.allInspectorsAffected = true;
    if (!destination.allSidebarThreadsAffected
        && !appendUniqueBounded(destination.affectedSidebarThreadIds,
                                source.affectedSidebarThreadIds))
        destination.allSidebarThreadsAffected = true;

    const auto sameContent = [](const auto& left, const auto& right) {
        return left.threadId == right.threadId && left.turnId == right.turnId
               && left.itemId == right.itemId && left.channel == right.channel;
    };
    if (!destination.allThreadsAffected) {
        for (const auto& identity : source.affectedItemContents) {
            auto existing = std::ranges::find_if(
                destination.affectedItemContents,
                [&identity, &sameContent](const auto& candidate) {
                    return sameContent(candidate, identity);
                });
            if (existing == destination.affectedItemContents.end()) {
                if (static_cast<qsizetype>(
                        destination.affectedItemContents.size())
                    >= detail::maximumCoalescedPresentationIdentities) {
                    destination.allThreadsAffected = true;
                    break;
                }
                auto bounded = identity;
                if (bounded.append) {
                    const std::uint64_t bytes =
                        static_cast<std::uint64_t>(
                            bounded.append->deltaUtf8.size());
                    if (destination.coalescedContentDeltaBytes
                            > detail::maximumCoalescedContentDeltaBytes
                        || bytes
                               > detail::maximumCoalescedContentDeltaBytes
                                     - destination.coalescedContentDeltaBytes) {
                        bounded.append.reset();
                    } else {
                        destination.coalescedContentDeltaBytes += bytes;
                    }
                }
                destination.affectedItemContents.push_back(
                    std::move(bounded));
                continue;
            }

            // A replacement is already authoritative. Two exact append hints
            // remain mergeable only while their byte bases form one contiguous
            // no-discard range; every ambiguous sequence degrades to a bounded
            // replacement refresh of the newest State.
            const auto discardAccumulated = [&destination, &existing] {
                if (existing->append) {
                    const std::uint64_t bytes =
                        static_cast<std::uint64_t>(
                            existing->append->deltaUtf8.size());
                    destination.coalescedContentDeltaBytes =
                        bytes <= destination.coalescedContentDeltaBytes
                            ? destination.coalescedContentDeltaBytes - bytes
                            : 0;
                }
                existing->append.reset();
            };
            if (!existing->append || !identity.append)
                discardAccumulated();
            else {
                auto& accumulated = *existing->append;
                const auto& next = *identity.append;
                const std::uint64_t accumulatedBytes =
                    static_cast<std::uint64_t>(accumulated.deltaUtf8.size());
                const bool baseFits =
                    accumulated.baseContentBytes
                    <= std::numeric_limits<std::uint64_t>::max()
                           - accumulatedBytes;
                const bool contiguous =
                    accumulated.discardPrefixBytes == 0
                    && next.discardPrefixBytes == 0 && baseFits
                    && next.baseContentBytes
                           == accumulated.baseContentBytes + accumulatedBytes;
                const std::uint64_t nextBytes =
                    static_cast<std::uint64_t>(next.deltaUtf8.size());
                const bool deltaFits =
                    destination.coalescedContentDeltaBytes
                        <= detail::maximumCoalescedContentDeltaBytes
                    && nextBytes
                           <= detail::maximumCoalescedContentDeltaBytes
                                  - destination.coalescedContentDeltaBytes;
                if (contiguous && deltaFits) {
                    accumulated.deltaUtf8.append(next.deltaUtf8);
                    destination.coalescedContentDeltaBytes += nextBytes;
                } else {
                    discardAccumulated();
                }
            }
        }
    }

    if (destination.affectedThreadIds.size()
            > detail::maximumCoalescedPresentationIdentities
        || destination.fullyAffectedThreadIds.size()
               > detail::maximumCoalescedPresentationIdentities
        || destination.removedThreadIds.size()
               > detail::maximumCoalescedPresentationIdentities
        || static_cast<qsizetype>(destination.affectedItemContents.size())
               > detail::maximumCoalescedPresentationIdentities) {
        destination.allThreadsAffected = true;
    }
    if (destination.affectedInspectorThreadIds.size()
        > detail::maximumCoalescedPresentationIdentities) {
        destination.allInspectorsAffected = true;
    }
    if (destination.affectedSidebarThreadIds.size()
        > detail::maximumCoalescedPresentationIdentities) {
        destination.allSidebarThreadsAffected = true;
    }
    if (destination.allThreadsAffected) {
        destination.affectedThreadIds.clear();
        destination.fullyAffectedThreadIds.clear();
        // Keep exact removals even when the rest of the presentation scope
        // degrades to an all-thread refresh. Global omission provenance makes
        // a missing selected ID ambiguous without this bounded evidence.
        destination.affectedItemContents.clear();
        destination.coalescedContentDeltaBytes = 0;
    }
    if (destination.allInspectorsAffected)
        destination.affectedInspectorThreadIds.clear();
    if (destination.allSidebarThreadsAffected)
        destination.affectedSidebarThreadIds.clear();
}

template<typename Completion>
struct CompletionGate
{
    explicit CompletionGate(Completion callback)
        : completion(std::move(callback))
    {
    }

    std::atomic_bool completed = false;
    std::uint64_t token = 0;
    Completion completion;
};

} // namespace

class FrontendSession::Impl
{
public:
    using WorkerCommand = std::function<void(FrontendSessionWorker&)>;

    struct StatePublication {
        std::uint64_t generation = 0;
        sdk::State state;
        detail::StateUpdateScope scope;
        ArchivedThreadDiscoveryStatus archivedStatus =
            ArchivedThreadDiscoveryStatus::InProgress;
    };

    struct StatusPublication {
        std::uint64_t generation = 0;
        Lifecycle lifecycle = Lifecycle::Disconnected;
        QString status;
        bool lifecycleChanged = false;
    };

    struct ModelPublication {
        std::uint64_t generation = 0;
        std::vector<ai::openai::codex::typed::Model> models;
    };

    struct CompletionPublication {
        std::uint64_t generation = 0;
        std::function<void()> invoke;
    };

    using Control = std::variant<StatusPublication,
                                 ModelPublication,
                                 CompletionPublication>;

    class WorkerThread final : public QThread
    {
    public:
        explicit WorkerThread(Impl& impl)
            : impl(impl)
        {
        }

        bool post(WorkerCommand command)
        {
            std::lock_guard lock(mutex);
            if (stopping)
                return false;
            if (!worker) {
                pending.push_back(std::move(command));
                return true;
            }
            auto shared = std::make_shared<WorkerCommand>(std::move(command));
            return QMetaObject::invokeMethod(
                worker,
                [target = worker, shared] { (*shared)(*target); },
                Qt::QueuedConnection);
        }

        void stopAndJoin()
        {
            FrontendSessionWorker* target = nullptr;
            bool alreadyStopping = false;
            {
                std::lock_guard lock(mutex);
                alreadyStopping = stopping;
                if (!alreadyStopping) {
                    stopping = true;
                    target = worker;
                }
            }
            if (alreadyStopping) {
                wait();
                return;
            }
            if (target) {
                const bool shutdownQueued = QMetaObject::invokeMethod(
                    target,
                    [target] {
                        target->shutdown();
                        QThread::currentThread()->quit();
                    },
                    Qt::QueuedConnection);
                // The local worker performs the same idempotent shutdown after
                // its event loop exits. Do not let a failed queued invocation
                // turn facade destruction into an unbounded join.
                if (!shutdownQueued)
                    quit();
            }
            wait();
        }

    protected:
        void run() override
        {
            FrontendSessionWorker localWorker;
            impl.attach(localWorker);

            std::deque<WorkerCommand> initial;
            bool stopImmediately = false;
            {
                std::lock_guard lock(mutex);
                worker = &localWorker;
                initial.swap(pending);
                stopImmediately = stopping;
            }
            if (!stopImmediately) {
                for (auto& command : initial)
                    command(localWorker);
                {
                    std::lock_guard lock(mutex);
                    stopImmediately = stopping;
                }
                if (!stopImmediately)
                    exec();
            }

            localWorker.shutdown();
            {
                std::lock_guard lock(mutex);
                worker = nullptr;
            }
        }

    private:
        Impl& impl;
        std::mutex mutex;
        FrontendSessionWorker* worker = nullptr;
        std::deque<WorkerCommand> pending;
        bool stopping = false;
    };

    explicit Impl(FrontendSession& owner)
        : owner(owner)
        , workerThread(*this)
    {
        workerThread.setObjectName(QStringLiteral("CodexUI frontend session"));
        workerThread.start();
    }

    ~Impl()
    {
        shutdown();
    }

    void shutdown()
    {
        if (shutdownComplete.exchange(true))
            return;
        workerThread.stopAndJoin();
        {
            std::lock_guard lock(publicationMutex);
            acceptingPublications = false;
        }
        drainPublications();
        failPendingCompletions(
            QStringLiteral("Frontend session closed before the operation completed"));
        {
            std::lock_guard lock(publicationMutex);
            latestState.reset();
            controls.clear();
            wakeScheduled = false;
        }
    }

    bool post(WorkerCommand command)
    {
        return workerThread.post(std::move(command));
    }

    template<typename Completion, typename Failure, typename Invocation>
    std::optional<QString> postOperation(
        std::optional<QString> validation,
        Completion completion,
        Failure failure,
        Invocation invocation)
    {
        if (currentLifecycle != Lifecycle::Ready)
            return QStringLiteral("Backend is not ready");
        if (validation)
            return validation;

        auto gate = std::make_shared<CompletionGate<Completion>>(
            std::move(completion));
        trackCompletion(gate, std::move(failure));
        if (!post([this, gate, invocation = std::move(invocation)](
                      FrontendSessionWorker& worker) mutable {
                const std::uint64_t generation = worker.generation();
                auto done = [this, gate, generation](auto&&... values) {
                    enqueueCompletion(generation, gate,
                                      std::forward<decltype(values)>(values)...);
                };
                invocation(worker, std::move(done));
            })) {
            cancelCompletion(gate);
            return QStringLiteral("Frontend worker is shutting down");
        }
        return std::nullopt;
    }

    template<typename Invocation>
    std::optional<QString> postUnitOperation(
        std::optional<QString> validation,
        OperationCompletion completion,
        Invocation invocation)
    {
        return postOperation(
            std::move(validation),
            std::move(completion),
            [](auto& callback, const QString& error) { callback(error); },
            [invocation = std::move(invocation)](
                FrontendSessionWorker& worker, auto done) mutable {
                if (const auto error = invocation(worker, done))
                    done(*error);
            });
    }

    template<typename Completion, typename Failure>
    void trackCompletion(
        const std::shared_ptr<CompletionGate<Completion>>& gate,
        Failure failure)
    {
        std::lock_guard lock(completionMutex);
        gate->token = ++nextCompletionToken;
        pendingCompletions.emplace(
            gate->token,
            [gate, failure = std::move(failure)](const QString& error) mutable {
                if (gate->completed.exchange(true))
                    return;
                failure(gate->completion, error);
            });
    }

    template<typename Completion>
    void cancelCompletion(
        const std::shared_ptr<CompletionGate<Completion>>& gate)
    {
        gate->completed.store(true);
        std::lock_guard lock(completionMutex);
        pendingCompletions.erase(gate->token);
    }

    void untrackCompletion(std::uint64_t token)
    {
        std::lock_guard lock(completionMutex);
        pendingCompletions.erase(token);
    }

    void failPendingCompletions(const QString& error)
    {
        std::map<std::uint64_t, std::function<void(const QString&)>> pending;
        {
            std::lock_guard lock(completionMutex);
            pending.swap(pendingCompletions);
        }
        for (auto& [token, fail] : pending) {
            Q_UNUSED(token);
            fail(error);
        }
    }

    void attach(FrontendSessionWorker& worker)
    {
        workerAffinityValidated.store(
            worker.transportAffinityIsCurrentThread(),
            std::memory_order_release);
        QObject::connect(
            &worker,
            &FrontendSessionWorker::stateChanged,
            &worker,
            [this, &worker](const detail::StateUpdateScope& scope) {
                enqueueState(StatePublication{
                    worker.generation(),
                    worker.state(),
                    scope,
                    worker.archivedThreadDiscoveryStatus(),
                });
            },
            Qt::DirectConnection);
        QObject::connect(
            &worker,
            &FrontendSessionWorker::lifecycleChanged,
            &worker,
            [this, &worker] {
                enqueueControl(StatusPublication{
                    worker.generation(),
                    worker.lifecycle(),
                    worker.statusText(),
                    true,
                });
            },
            Qt::DirectConnection);
        QObject::connect(
            &worker,
            &FrontendSessionWorker::statusChanged,
            &worker,
            [this, &worker] {
                enqueueControl(StatusPublication{
                    worker.generation(),
                    worker.lifecycle(),
                    worker.statusText(),
                    false,
                });
            },
            Qt::DirectConnection);
        QObject::connect(
            &worker,
            &FrontendSessionWorker::modelCatalogChanged,
            &worker,
            [this, &worker] {
                enqueueControl(ModelPublication{
                    worker.generation(),
                    worker.modelCatalog(),
                });
            },
            Qt::DirectConnection);
    }

    void scheduleWakeIfNeeded(bool& scheduleWake)
    {
        if (!wakeScheduled) {
            wakeScheduled = true;
            scheduleWake = true;
        }
    }

    void postWake(bool scheduleWake)
    {
        if (scheduleWake) {
            postedWakeCount.fetch_add(1, std::memory_order_relaxed);
            QMetaObject::invokeMethod(
                &owner,
                [this] { drainPublications(); },
                Qt::QueuedConnection);
        }
    }

    void enqueueState(StatePublication publication)
    {
        bool scheduleWake = false;
        {
            std::lock_guard lock(publicationMutex);
            if (!acceptingPublications)
                return;
            if (latestState
                && latestState->generation == publication.generation) {
                latestState->state = std::move(publication.state);
                latestState->archivedStatus = publication.archivedStatus;
                mergeScope(latestState->scope, publication.scope);
                // The newest immutable State is the final authority for any
                // identity it actually retains. Capacity-omitted identities
                // remain ambiguous and therefore keep their exact tombstone.
                for (auto iterator = latestState->scope.removedThreadIds.begin();
                     iterator != latestState->scope.removedThreadIds.end();) {
                    if (latestState->state.thread(iterator->toStdString()))
                        iterator = latestState->scope.removedThreadIds.erase(iterator);
                    else
                        ++iterator;
                }
            } else {
                latestState = std::move(publication);
            }
            scheduleWakeIfNeeded(scheduleWake);
        }
        postWake(scheduleWake);
    }

    [[nodiscard]] static bool isControlBarrier(const Control& control)
    {
        if (std::holds_alternative<CompletionPublication>(control))
            return true;
        const auto* status = std::get_if<StatusPublication>(&control);
        return status && status->lifecycleChanged;
    }

    template<typename Value>
    void appendReplaceableControl(Value value)
    {
        auto existing = controls.end();
        while (existing != controls.begin()) {
            --existing;
            if (isControlBarrier(*existing))
                break;
            const auto* candidate = std::get_if<Value>(&*existing);
            if (!candidate)
                continue;
            if (candidate->generation > value.generation)
                return;
            controls.erase(existing);
            break;
        }
        // Append instead of replacing in place so the surviving publication
        // keeps its last-occurrence ordering relative to the other
        // replaceable control kind.
        controls.push_back(Control{std::move(value)});
    }

    void appendControl(StatusPublication publication)
    {
        if (publication.lifecycleChanged) {
            controls.push_back(Control{std::move(publication)});
            return;
        }
        appendReplaceableControl(std::move(publication));
    }

    void appendControl(ModelPublication publication)
    {
        appendReplaceableControl(std::move(publication));
    }

    void appendControl(CompletionPublication publication)
    {
        controls.push_back(Control{std::move(publication)});
    }

    template<typename Value>
    void enqueueControl(Value value)
    {
        bool scheduleWake = false;
        {
            std::lock_guard lock(publicationMutex);
            if (!acceptingPublications)
                return;
            appendControl(std::move(value));
            scheduleWakeIfNeeded(scheduleWake);
        }
        postWake(scheduleWake);
    }

    void drainPublications()
    {
        std::optional<StatePublication> state;
        std::deque<Control> readyControls;
        {
            std::lock_guard lock(publicationMutex);
            state.swap(latestState);
            readyControls.swap(controls);
            wakeScheduled = false;
        }
        // A one-slot State mailbox cannot replay superseded intermediate
        // States around controls. Publish the newest authoritative State
        // first, then deliver every control/result in original FIFO order, so
        // no callback can observe State older than the worker boundary at
        // which the GUI caught up.
        if (state)
            apply(*state);
        for (Control& publication : readyControls)
            std::visit([this](auto& value) { apply(value); }, publication);
    }

    template<typename Completion, typename... Args>
    void enqueueCompletion(std::uint64_t generation,
                           const std::shared_ptr<CompletionGate<Completion>>& gate,
                           Args&&... args)
    {
        if (gate->completed.exchange(true))
            return;
        untrackCompletion(gate->token);
        auto arguments =
            std::make_tuple(std::forward<Args>(args)...);
        enqueueControl(CompletionPublication{
            generation,
            [gate, arguments = std::move(arguments)]() mutable {
                std::apply(gate->completion, std::move(arguments));
            },
        });
    }

    void apply(StatePublication& publication)
    {
        if (publication.generation < appliedGeneration)
            return;
        appliedGeneration = publication.generation;
        currentState = std::move(publication.state);
        archivedStatus = publication.archivedStatus;
        if (publication.scope.hasPresentationChange)
            emit owner.stateChanged(publication.scope);
    }

    void apply(StatusPublication& publication)
    {
        if (publication.generation < appliedGeneration)
            return;
        appliedGeneration = std::max(appliedGeneration, publication.generation);
        const Lifecycle previousLifecycle = currentLifecycle;
        const QString previousStatus = status;
        currentLifecycle = publication.lifecycle;
        status = std::move(publication.status);
        if (publication.lifecycleChanged && previousLifecycle != currentLifecycle)
            emit owner.lifecycleChanged();
        else if (publication.lifecycleChanged && previousStatus != status)
            emit owner.lifecycleChanged();
        if (!publication.lifecycleChanged && previousStatus != status)
            emit owner.statusChanged();
    }

    void apply(ModelPublication& publication)
    {
        if (publication.generation < appliedGeneration)
            return;
        appliedGeneration = publication.generation;
        if (models == publication.models)
            return;
        models = std::move(publication.models);
        emit owner.modelCatalogChanged();
    }

    void apply(CompletionPublication& publication)
    {
        // Operation results are deliberately never discarded merely because a
        // reconnect advanced the generation. They own user-visible completion.
        appliedGeneration = std::max(appliedGeneration, publication.generation);
        if (publication.invoke)
            publication.invoke();
    }

    FrontendSession& owner;
    WorkerThread workerThread;

    std::mutex publicationMutex;
    std::optional<StatePublication> latestState;
    std::deque<Control> controls;
    bool wakeScheduled = false;
    bool acceptingPublications = true;
    std::atomic_size_t postedWakeCount = 0;
    std::atomic_bool workerAffinityValidated = false;
    std::atomic_bool shutdownComplete = false;

    std::mutex completionMutex;
    std::map<std::uint64_t, std::function<void(const QString&)>>
        pendingCompletions;
    std::uint64_t nextCompletionToken = 0;

    sdk::State currentState;
    std::vector<ai::openai::codex::typed::Model> models;
    Lifecycle currentLifecycle = Lifecycle::Disconnected;
    ArchivedThreadDiscoveryStatus archivedStatus =
        ArchivedThreadDiscoveryStatus::InProgress;
    QString status = QStringLiteral("Disconnected");
    std::uint64_t appliedGeneration = 0;
};

FrontendSession::FrontendSession(QObject* parent)
    : QObject(parent)
    , impl(std::make_unique<Impl>(*this))
{
}

FrontendSession::~FrontendSession() = default;

void FrontendSession::shutdown()
{
    impl->shutdown();
}

void FrontendSession::enqueueStateForTest(
    std::uint64_t generation,
    detail::StateUpdateScope scope)
{
    impl->enqueueState(Impl::StatePublication{
        generation,
        impl->currentState,
        std::move(scope),
        impl->archivedStatus,
    });
}

void FrontendSession::enqueueStatusForTest(std::uint64_t generation,
                                           QString status)
{
    enqueueStatusForTest(
        generation, impl->currentLifecycle, std::move(status));
}

void FrontendSession::enqueueStatusForTest(std::uint64_t generation,
                                           Lifecycle lifecycle,
                                           QString status)
{
    impl->enqueueControl(Impl::StatusPublication{
        generation,
        lifecycle,
        std::move(status),
        false,
    });
}

void FrontendSession::enqueueLifecycleForTest(std::uint64_t generation,
                                              Lifecycle lifecycle,
                                              QString status)
{
    impl->enqueueControl(Impl::StatusPublication{
        generation,
        lifecycle,
        std::move(status),
        true,
    });
}

void FrontendSession::enqueueModelsForTest(
    std::uint64_t generation,
    std::vector<ai::openai::codex::typed::Model> models)
{
    impl->enqueueControl(Impl::ModelPublication{
        generation,
        std::move(models),
    });
}

std::size_t FrontendSession::pendingStateCountForTest() const
{
    std::lock_guard lock(impl->publicationMutex);
    return impl->latestState ? 1U : 0U;
}

std::size_t FrontendSession::pendingControlCountForTest() const
{
    std::lock_guard lock(impl->publicationMutex);
    return impl->controls.size();
}

std::size_t FrontendSession::postedWakeCountForTest() const noexcept
{
    return impl->postedWakeCount.load(std::memory_order_relaxed);
}

bool FrontendSession::workerAffinityValidatedForTest() const noexcept
{
    return impl->workerAffinityValidated.load(std::memory_order_acquire);
}

void FrontendSession::trackOperationForTest(OperationCompletion completion)
{
    auto gate = std::make_shared<CompletionGate<OperationCompletion>>(
        std::move(completion));
    impl->trackCompletion(gate, [](auto& callback, const QString& error) {
        callback(error);
    });
}

void FrontendSession::completeOperationForTest(
    std::uint64_t generation,
    OperationCompletion completion,
    QString error)
{
    auto gate = std::make_shared<CompletionGate<OperationCompletion>>(
        std::move(completion));
    impl->trackCompletion(gate, [](auto& callback, const QString& value) {
        callback(value);
    });
    impl->enqueueCompletion(generation, gate, error);
    // Exercise the same gate against a duplicate provider callback: exactly
    // one GUI publication owns the completion.
    impl->enqueueCompletion(generation, gate, std::move(error));
}

void FrontendSession::connectToBackend()
{
    impl->post([](FrontendSessionWorker& worker) {
        worker.connectToBackend();
    });
}

void FrontendSession::reconnectToBackend()
{
    impl->post([](FrontendSessionWorker& worker) {
        worker.reconnectToBackend();
    });
}

FrontendSession::Lifecycle FrontendSession::lifecycle() const noexcept
{
    return impl->currentLifecycle;
}

QString FrontendSession::statusText() const
{
    return impl->status;
}

std::optional<QString> FrontendSession::promptValidationError(const QString& prompt)
{
    return FrontendSessionWorker::promptValidationError(prompt);
}

const sdk::State& FrontendSession::state() const noexcept
{
    return impl->currentState;
}

const std::vector<ai::openai::codex::typed::Model>&
FrontendSession::modelCatalog() const noexcept
{
    return impl->models;
}

bool FrontendSession::archivedThreadDiscoveryComplete() const noexcept
{
    return impl->archivedStatus == ArchivedThreadDiscoveryStatus::Complete;
}

bool FrontendSession::archivedThreadDiscoveryTerminal() const noexcept
{
    return impl->archivedStatus != ArchivedThreadDiscoveryStatus::InProgress;
}

FrontendSession::ArchivedThreadDiscoveryStatus
FrontendSession::archivedThreadDiscoveryStatus() const noexcept
{
    return impl->archivedStatus;
}

bool FrontendSession::ownsController() const noexcept
{
    const auto& projection = impl->currentState.controller();
    return projection.value && projection.value->ownedByThisClient;
}

void FrontendSession::loadThread(const QString& threadId, bool retryIncomplete)
{
    if (impl->currentLifecycle != Lifecycle::Ready || threadId.isEmpty())
        return;
    impl->post([threadId, retryIncomplete](FrontendSessionWorker& worker) {
        worker.loadThread(threadId, retryIncomplete);
    });
}

std::optional<QString>
FrontendSession::acquireController(OperationCompletion completion)
{
    return impl->postOperation(
        std::nullopt,
        std::move(completion),
        [](auto& callback, const QString& error) { callback(error); },
        [](FrontendSessionWorker& worker, auto done) {
            if (const auto error = worker.acquireController(done))
                done(*error);
        });
}

std::optional<QString>
FrontendSession::startThread(ThreadStartCompletion completion)
{
    return startThread(ai::openai::codex::typed::ThreadStartParams{},
                       std::move(completion));
}

std::optional<QString>
FrontendSession::startThread(ai::openai::codex::typed::ThreadStartParams parameters,
                             ThreadStartCompletion completion)
{
    return impl->postOperation(
        std::nullopt,
        std::move(completion),
        [](auto& callback, const QString& error) { callback({}, error); },
        [parameters = std::move(parameters)](
            FrontendSessionWorker& worker, auto done) mutable {
                if (const auto error =
                        worker.startThread(std::move(parameters), done))
                    done(QString{}, *error);
        });
}

std::optional<QString>
FrontendSession::resumeThread(const QString& threadId,
                              ThreadStartCompletion completion)
{
    if (threadId.isEmpty())
        return QStringLiteral("Thread attach requires a thread ID");
    ai::openai::codex::typed::ThreadResumeParams parameters;
    parameters.threadId =
        ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    return resumeThread(std::move(parameters), std::move(completion));
}

std::optional<QString>
FrontendSession::resumeThread(
    ai::openai::codex::typed::ThreadResumeParams parameters,
    ThreadStartCompletion completion)
{
    const auto validation = parameters.threadId.value.empty()
                                ? std::optional<QString>(QStringLiteral(
                                      "Thread attach requires a thread ID"))
                                : std::nullopt;
    return impl->postOperation(
        validation,
        std::move(completion),
        [](auto& callback, const QString& error) { callback({}, error); },
        [parameters = std::move(parameters)](
            FrontendSessionWorker& worker, auto done) mutable {
                if (const auto error =
                        worker.resumeThread(std::move(parameters), done))
                    done(QString{}, *error);
        });
}

std::optional<QString>
FrontendSession::startTurn(const QString& threadId,
                           const QString& prompt,
                           OperationCompletion completion)
{
    ai::openai::codex::typed::TurnStartParams parameters;
    parameters.threadId =
        ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    return startTurn(
        std::move(parameters),
        prompt,
        [completion = std::move(completion)](const QString&,
                                             const QString& error) {
            completion(error);
        });
}

std::optional<QString>
FrontendSession::startTurn(ai::openai::codex::typed::TurnStartParams parameters,
                           const QString& prompt,
                           TurnStartCompletion completion)
{
    return startTurn(std::move(parameters), prompt, {},
                     std::move(completion));
}

std::optional<QString>
FrontendSession::startTurn(ai::openai::codex::typed::TurnStartParams parameters,
                           const QString& prompt,
                           const QStringList& localImagePaths,
                           TurnStartCompletion completion)
{
    std::optional<QString> validation = promptValidationError(prompt);
    if (!validation && parameters.threadId.value.empty())
        validation = QStringLiteral("Turn submission requires a thread ID");
    if (!validation && prompt.trimmed().isEmpty() && localImagePaths.isEmpty())
        validation = QStringLiteral(
            "Turn submission requires a prompt or attachment");
    return impl->postOperation(
        std::move(validation),
        std::move(completion),
        [](auto& callback, const QString& error) { callback({}, error); },
        [parameters = std::move(parameters), prompt, localImagePaths](
            FrontendSessionWorker& worker, auto done) mutable {
                if (const auto error = worker.startTurn(
                        std::move(parameters), prompt, localImagePaths, done))
                    done(QString{}, *error);
        });
}

std::optional<QString>
FrontendSession::steerTurn(const QString& threadId,
                           const QString& expectedTurnId,
                           const QString& prompt,
                           OperationCompletion completion)
{
    return steerTurn(threadId, expectedTurnId, prompt, {},
                     std::move(completion));
}

std::optional<QString>
FrontendSession::steerTurn(const QString& threadId,
                           const QString& expectedTurnId,
                           const QString& prompt,
                           const QStringList& localImagePaths,
                           OperationCompletion completion)
{
    std::optional<QString> validation = promptValidationError(prompt);
    if (!validation && (threadId.isEmpty() || expectedTurnId.isEmpty()))
        validation = QStringLiteral(
            "Steering requires the active thread and turn identities");
    if (!validation && prompt.trimmed().isEmpty() && localImagePaths.isEmpty())
        validation = QStringLiteral("Steering requires a prompt or attachment");
    return impl->postOperation(
        std::move(validation),
        std::move(completion),
        [](auto& callback, const QString& error) { callback(error); },
        [threadId, expectedTurnId, prompt, localImagePaths](
            FrontendSessionWorker& worker, auto done) mutable {
                if (const auto error = worker.steerTurn(
                        threadId, expectedTurnId, prompt, localImagePaths, done))
                    done(*error);
        });
}

std::optional<QString>
FrontendSession::forkThread(ai::openai::codex::typed::ThreadForkParams parameters,
                            ThreadStartCompletion completion)
{
    const auto validation = parameters.threadId.value.empty()
                                ? std::optional<QString>(QStringLiteral(
                                      "Fork requires a source thread ID"))
                                : std::nullopt;
    return impl->postOperation(
        validation,
        std::move(completion),
        [](auto& callback, const QString& error) { callback({}, error); },
        [parameters = std::move(parameters)](
            FrontendSessionWorker& worker, auto done) mutable {
                if (const auto error =
                        worker.forkThread(std::move(parameters), done))
                    done(QString{}, *error);
        });
}

std::optional<QString>
FrontendSession::renameThread(const QString& threadId,
                              const QString& name,
                              OperationCompletion completion)
{
    const auto validation =
        threadId.isEmpty() || name.trimmed().isEmpty()
            ? std::optional<QString>(
                  QStringLiteral("Rename requires a thread ID and non-empty name"))
            : std::nullopt;
    auto invocation = [threadId, name](FrontendSessionWorker& worker,
                                      OperationCompletion done) {
        return worker.renameThread(threadId, name, std::move(done));
    };
    return impl->postUnitOperation(validation, std::move(completion),
                                   std::move(invocation));
}

std::optional<QString>
FrontendSession::archiveThread(const QString& threadId,
                               OperationCompletion completion)
{
    const auto validation =
        threadId.isEmpty()
            ? std::optional<QString>(QStringLiteral("Archive requires a thread ID"))
            : std::nullopt;
    auto invocation = [threadId](FrontendSessionWorker& worker,
                                 OperationCompletion done) {
        return worker.archiveThread(threadId, std::move(done));
    };
    return impl->postUnitOperation(validation, std::move(completion),
                                   std::move(invocation));
}

std::optional<QString>
FrontendSession::unarchiveThread(const QString& threadId,
                                 OperationCompletion completion)
{
    const auto validation =
        threadId.isEmpty()
            ? std::optional<QString>(
                  QStringLiteral("Unarchive requires a thread ID"))
            : std::nullopt;
    auto invocation = [threadId](FrontendSessionWorker& worker,
                                 OperationCompletion done) {
        return worker.unarchiveThread(threadId, std::move(done));
    };
    return impl->postUnitOperation(validation, std::move(completion),
                                   std::move(invocation));
}

std::optional<QString>
FrontendSession::deleteThread(const QString& threadId,
                              OperationCompletion completion)
{
    const auto validation =
        threadId.isEmpty()
            ? std::optional<QString>(QStringLiteral("Delete requires a thread ID"))
            : std::nullopt;
    auto invocation = [threadId](FrontendSessionWorker& worker,
                                 OperationCompletion done) {
        return worker.deleteThread(threadId, std::move(done));
    };
    return impl->postUnitOperation(validation, std::move(completion),
                                   std::move(invocation));
}

std::optional<QString>
FrontendSession::interruptTurn(const QString& threadId,
                               const QString& turnId,
                               OperationCompletion completion)
{
    const auto validation =
        threadId.isEmpty() || turnId.isEmpty()
            ? std::optional<QString>(
                  QStringLiteral("Interrupt requires thread and turn IDs"))
            : std::nullopt;
    auto invocation = [threadId, turnId](FrontendSessionWorker& worker,
                                         OperationCompletion done) {
        return worker.interruptTurn(threadId, turnId, std::move(done));
    };
    return impl->postUnitOperation(validation, std::move(completion),
                                   std::move(invocation));
}

std::optional<QString>
FrontendSession::respondApproval(const sdk::PendingRequestId& requestId,
                                 ai::openai::codex::typed::ApprovalDecision decision,
                                 OperationCompletion completion)
{
    auto invocation =
        [requestId, decision = std::move(decision)](
            FrontendSessionWorker& worker, OperationCompletion done) mutable {
            return worker.respondApproval(requestId, std::move(decision),
                                          std::move(done));
        };
    return impl->postUnitOperation(std::nullopt, std::move(completion),
                                   std::move(invocation));
}

std::optional<QString>
FrontendSession::respondApplyPatchApproval(
    const sdk::PendingRequestId& requestId,
    ai::openai::codex::typed::ApplyPatchApprovalResponse response,
    OperationCompletion completion)
{
    auto invocation =
        [requestId, response = std::move(response)](
            FrontendSessionWorker& worker, OperationCompletion done) mutable {
            return worker.respondApplyPatchApproval(
                requestId, std::move(response), std::move(done));
        };
    return impl->postUnitOperation(std::nullopt, std::move(completion),
                                   std::move(invocation));
}

std::optional<QString>
FrontendSession::respondExecCommandApproval(
    const sdk::PendingRequestId& requestId,
    ai::openai::codex::typed::ExecCommandApprovalResponse response,
    OperationCompletion completion)
{
    auto invocation =
        [requestId, response = std::move(response)](
            FrontendSessionWorker& worker, OperationCompletion done) mutable {
            return worker.respondExecCommandApproval(
                requestId, std::move(response), std::move(done));
        };
    return impl->postUnitOperation(std::nullopt, std::move(completion),
                                   std::move(invocation));
}

std::optional<QString>
FrontendSession::respondUserInput(
    const sdk::PendingRequestId& requestId,
    std::vector<ai::openai::codex::typed::UserInputAnswer> answers,
    OperationCompletion completion)
{
    auto invocation =
        [requestId, answers = std::move(answers)](
            FrontendSessionWorker& worker, OperationCompletion done) mutable {
            return worker.respondUserInput(requestId, std::move(answers),
                                           std::move(done));
        };
    return impl->postUnitOperation(std::nullopt, std::move(completion),
                                   std::move(invocation));
}

} // namespace codexui
