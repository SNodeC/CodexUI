// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <ai/openai/codex/frontend/Security.h>

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
