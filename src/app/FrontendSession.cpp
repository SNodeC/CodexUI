// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <ai/openai/codex/frontend/Security.h>
#include <ai/openai/codex/frontend/client/Controller.h>
#include <ai/openai/codex/frontend/client/Threads.h>
#include <ai/openai/codex/frontend/client/Turns.h>

#include <QByteArray>
#include <QDir>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace codexui {
namespace sdk = ai::openai::codex::frontend::client;
namespace frontend = ai::openai::codex::frontend;

namespace {

constexpr qsizetype maximumFrameBytes = 16 * 1024 * 1024;

QString operationError(const std::optional<sdk::Error>& error, const QString& fallback)
{
    if (error && !error->message.empty())
        return QString::fromStdString(error->message);
    return fallback;
}

std::optional<QString> submissionError(const sdk::Submission& submission, const QString& fallback)
{
    if (submission)
        return std::nullopt;
    return operationError(submission.error, fallback);
}

} // namespace

FrontendSession::FrontendSession(QObject* parent)
    : QObject(parent)
{
    sdk::ClientOptions options;
    options.credentialProvider = [] {
        return sdk::AuthenticationContext{
            frontend::NoCredential{},
            std::string{"verified-local:"} + std::to_string(::geteuid()),
        };
    };

    sdk::ClientCallbacks callbacks;
    callbacks.onConnectionStateChanged = [this](const sdk::ConnectionStateChange& change) {
        switch (change.current) {
            case sdk::ConnectionState::Connecting:
                setLifecycle(Lifecycle::Connecting);
                break;
            case sdk::ConnectionState::Authenticating:
                setLifecycle(Lifecycle::Authenticating);
                break;
            case sdk::ConnectionState::Synchronizing:
                setLifecycle(Lifecycle::Synchronizing);
                break;
            case sdk::ConnectionState::Ready:
                setLifecycle(Lifecycle::Ready);
                break;
            case sdk::ConnectionState::Disconnected:
            case sdk::ConnectionState::Closed:
                setLifecycle(Lifecycle::Disconnected,
                             change.error ? QString::fromStdString(change.error->message) : QString{});
                break;
            case sdk::ConnectionState::Closing:
                break;
        }
        if (change.error)
            setLifecycle(Lifecycle::Failed, QString::fromStdString(change.error->message));
    };
    callbacks.onStateUpdated = [this](const sdk::StateUpdate& update) {
        currentState = update.state;
        emit stateChanged();
    };
    callbacks.onSynchronized = [this](const sdk::SynchronizationInfo& info) {
        currentState = info.state;
        setLifecycle(Lifecycle::Ready);
        emit stateChanged();
    };
    callbacks.onDiagnostic = [this](const sdk::Diagnostic& diagnostic) {
        if (diagnostic.severity == sdk::Diagnostic::Severity::Error)
            setLifecycle(Lifecycle::Failed, QString::fromStdString(diagnostic.message));
    };
    client = std::make_unique<Client>(std::move(options), std::move(callbacks));

    connect(&socket, &QLocalSocket::connected, this, &FrontendSession::socketConnected);
    connect(&socket, &QLocalSocket::readyRead, this, &FrontendSession::socketReadyRead);
    connect(&socket, &QLocalSocket::disconnected, this, &FrontendSession::socketDisconnected);
    connect(&socket, &QLocalSocket::errorOccurred, this, &FrontendSession::socketFailed);
}

FrontendSession::~FrontendSession()
{
    localShutdown = true;
    if (connection.isOpen())
        connection.close("CodexUI is closing");
    client->close("CodexUI is closing");
    socket.abort();
}

void FrontendSession::connectToBackend()
{
    if (socket.state() != QLocalSocket::UnconnectedState || connection.isOpen())
        return;
    localShutdown = false;
    setLifecycle(Lifecycle::Connecting);
    socket.connectToServer(defaultSocketPath(), QIODevice::ReadWrite);
}

FrontendSession::Lifecycle FrontendSession::lifecycle() const noexcept
{
    return currentLifecycle;
}

QString FrontendSession::statusText() const
{
    if (!detail.isEmpty())
        return detail;
    switch (currentLifecycle) {
        case Lifecycle::Disconnected:
            return QStringLiteral("Disconnected");
        case Lifecycle::Connecting:
            return QStringLiteral("Connecting…");
        case Lifecycle::Authenticating:
            return QStringLiteral("Authenticating…");
        case Lifecycle::Synchronizing:
            return QStringLiteral("Synchronizing…");
        case Lifecycle::Ready:
            return QStringLiteral("State synced");
        case Lifecycle::Failed:
            return QStringLiteral("Connection failed");
    }
    return QStringLiteral("Disconnected");
}

const sdk::State& FrontendSession::state() const noexcept
{
    return currentState;
}

bool FrontendSession::ownsController() const noexcept
{
    const auto& projection = currentState.controller();
    return projection.value && projection.value->ownedByThisClient;
}

void FrontendSession::loadThread(const QString& threadId)
{
    if (currentLifecycle != Lifecycle::Ready || threadId.isEmpty())
        return;
    const std::string id = threadId.toStdString();
    const auto* thread = currentState.thread(id);
    if (!thread || thread->fullyLoaded || requestedThreadReads.contains(id))
        return;

    requestedThreadReads.insert(id);
    sdk::Submission submission = client->threads().read(
        {ai::openai::codex::typed::ThreadId{id}, true},
        [this, id](const sdk::OperationResult<sdk::ThreadReadResult>&) {
            requestedThreadReads.erase(id);
        });
    if (!submission)
        requestedThreadReads.erase(id);
}

std::optional<QString> FrontendSession::acquireController(OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    sdk::Submission submission = client->controller().acquire(
        [completion = std::move(completion)](const sdk::OperationResult<sdk::ControllerResult>& result) {
            if (!result) {
                completion(operationError(result.error, QStringLiteral("Controller acquisition failed")));
                return;
            }
            if (!result.value->ownedByThisClient) {
                completion(QStringLiteral("Controller is owned by another frontend"));
                return;
            }
            completion({});
        });
    return submissionError(submission, QStringLiteral("Controller acquisition was not accepted"));
}

std::optional<QString> FrontendSession::startThread(ThreadStartCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    sdk::Submission submission = client->threads().start(
        ai::openai::codex::typed::ThreadStartParams{},
        [completion = std::move(completion)](const sdk::OperationResult<sdk::ThreadStartResult>& result) {
            if (!result) {
                completion({}, operationError(result.error, QStringLiteral("New thread could not be created")));
                return;
            }
            if (result.value->threadId.value.empty()) {
                completion({}, QStringLiteral("New thread response did not contain a stable thread ID"));
                return;
            }
            completion(QString::fromStdString(result.value->threadId.value), {});
        });
    return submissionError(submission, QStringLiteral("New thread submission was not accepted"));
}

std::optional<QString> FrontendSession::resumeThread(const QString& threadId, ThreadStartCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    const std::string requestedId = threadId.toStdString();
    ai::openai::codex::typed::ThreadResumeParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{requestedId};
    sdk::Submission submission = client->threads().resume(
        std::move(parameters),
        [completion = std::move(completion), requestedId](const sdk::OperationResult<sdk::ThreadResumeResult>& result) {
            if (!result) {
                completion({}, operationError(result.error, QStringLiteral("Thread could not be attached")));
                return;
            }
            if (result.value->threadId.value != requestedId) {
                completion({}, QStringLiteral("Thread attach returned an unexpected thread ID"));
                return;
            }
            completion(QString::fromStdString(result.value->threadId.value), {});
        });
    return submissionError(submission, QStringLiteral("Thread attach submission was not accepted"));
}

std::optional<QString> FrontendSession::startTurn(const QString& threadId,
                                                  const QString& prompt,
                                                  OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    ai::openai::codex::typed::TurnStartParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    ai::openai::codex::typed::TextInput input;
    input.text = prompt.toStdString();
    parameters.input.emplace_back(std::move(input));
    sdk::Submission submission = client->turns().start(
        std::move(parameters),
        [completion = std::move(completion)](const sdk::OperationResult<sdk::TurnStartResult>& result) {
            completion(result ? QString{} : operationError(result.error, QStringLiteral("Turn could not be started")));
        });
    return submissionError(submission, QStringLiteral("Turn submission was not accepted"));
}

std::optional<QString> FrontendSession::interruptTurn(const QString& threadId,
                                                      const QString& turnId,
                                                      OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    ai::openai::codex::typed::TurnInterruptParams parameters{
        ai::openai::codex::typed::ThreadId{threadId.toStdString()},
        ai::openai::codex::typed::TurnId{turnId.toStdString()},
    };
    sdk::Submission submission = client->turns().interrupt(
        std::move(parameters),
        [completion = std::move(completion)](const sdk::OperationResult<ai::openai::codex::typed::Unit>& result) {
            completion(result ? QString{} : operationError(result.error, QStringLiteral("Turn could not be interrupted")));
        });
    return submissionError(submission, QStringLiteral("Interrupt submission was not accepted"));
}

QString FrontendSession::defaultSocketPath()
{
    const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDirectory.isEmpty())
        return QDir(runtimeDirectory).filePath(QStringLiteral("snodec-codex-backend.sock"));
    return QStringLiteral("/tmp/snodec-codex-backend-%1.sock").arg(::getuid());
}

void FrontendSession::socketConnected()
{
    connection = client->openConnection({
        [this](OutboundMessage message) { return send(std::move(message)); },
        [this](std::string reason) { closeTransport(QString::fromStdString(reason)); },
    });
    if (!connection.isOpen()) {
        setLifecycle(Lifecycle::Failed, QStringLiteral("Frontend SDK rejected the Unix connection"));
        socket.abort();
        return;
    }
    connection.transportConnected();
}

void FrontendSession::socketReadyRead()
{
    const auto rejectOversizedFrame = [this] {
        setLifecycle(Lifecycle::Failed, QStringLiteral("Backend frame exceeds the SDK input limit"));
        socket.abort();
    };

    while (socket.canReadLine()) {
        const QByteArray prefix = socket.peek(maximumFrameBytes + 2);
        const qsizetype newline = prefix.indexOf('\n');
        if (newline < 0) {
            rejectOversizedFrame();
            return;
        }
        const qsizetype payloadBytes = newline > 0 && prefix.at(newline - 1) == '\r' ? newline - 1 : newline;
        if (payloadBytes > maximumFrameBytes) {
            rejectOversizedFrame();
            return;
        }

        QByteArray frame = socket.read(newline + 1);
        if (frame.endsWith('\n'))
            frame.chop(1);
        if (frame.endsWith('\r'))
            frame.chop(1);
        if (frame.isEmpty())
            continue;
        const sdk::ReceiveResult result = connection.receive(std::string_view(frame.constData(), static_cast<std::size_t>(frame.size())));
        if (!result.accepted) {
            setLifecycle(Lifecycle::Failed,
                         result.error ? QString::fromStdString(result.error->message)
                                      : QStringLiteral("Frontend SDK rejected a server message"));
            socket.abort();
            return;
        }
    }
    if (socket.bytesAvailable() > maximumFrameBytes) {
        rejectOversizedFrame();
    }
}

void FrontendSession::socketDisconnected()
{
    if (connection.isOpen()) {
        if (localShutdown)
            connection.transportDisconnected();
        else
            connection.transportDisconnected(sdk::TransportError{"Unix backend disconnected", true});
    }
    connection = Connection{};
    if (!localShutdown && currentLifecycle != Lifecycle::Failed)
        setLifecycle(Lifecycle::Disconnected);
}

void FrontendSession::socketFailed(QLocalSocket::LocalSocketError)
{
    if (localShutdown)
        return;
    if (connection.isOpen())
        connection.transportDisconnected(sdk::TransportError{socket.errorString().toStdString(), true});
    connection = Connection{};
    setLifecycle(Lifecycle::Failed, socket.errorString());
}

FrontendSession::SendResult FrontendSession::send(OutboundMessage message) noexcept
{
    if (socket.state() != QLocalSocket::ConnectedState)
        return {sdk::SendStatus::Closed, sdk::TransportError{"Unix backend socket is closed", true}};
    QByteArray frame = QByteArray::fromStdString(message.compactJson);
    frame.append('\n');
    const qint64 written = socket.write(frame);
    if (written != frame.size())
        return {sdk::SendStatus::Failed, sdk::TransportError{socket.errorString().toStdString(), true}};
    return {sdk::SendStatus::Accepted, std::nullopt};
}

void FrontendSession::closeTransport(QString reason) noexcept
{
    if (!reason.isEmpty())
        detail = std::move(reason);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.abort();
}

void FrontendSession::setLifecycle(Lifecycle value, QString newDetail)
{
    if (value != Lifecycle::Failed && newDetail.isEmpty())
        detail.clear();
    else
        detail = std::move(newDetail);
    if (currentLifecycle == value) {
        emit lifecycleChanged();
        return;
    }
    currentLifecycle = value;
    emit lifecycleChanged();
}

} // namespace codexui
