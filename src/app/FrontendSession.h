// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_APP_FRONTENDSESSION_H
#define CODEXUI_APP_FRONTENDSESSION_H

#include <QObject>
#include <QLocalSocket>
#include <QString>

#include <ai/openai/codex/frontend/client/Client.h>

#include <memory>
#include <set>
#include <string>

namespace codexui {

class FrontendSession : public QObject
{
    Q_OBJECT

public:
    enum class Lifecycle { Disconnected, Connecting, Authenticating, Synchronizing, Ready, Failed };

    explicit FrontendSession(QObject* parent = nullptr);
    ~FrontendSession() override;

    void connectToBackend();
    [[nodiscard]] Lifecycle lifecycle() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] const ai::openai::codex::frontend::client::State& state() const noexcept;
    void loadThread(const QString& threadId);

signals:
    void lifecycleChanged();
    void stateChanged();

private:
    using Client = ai::openai::codex::frontend::client::Client;
    using Connection = ai::openai::codex::frontend::client::Connection;
    using OutboundMessage = ai::openai::codex::frontend::client::OutboundMessage;
    using SendResult = ai::openai::codex::frontend::client::SendResult;

    static QString defaultSocketPath();
    void socketConnected();
    void socketReadyRead();
    void socketDisconnected();
    void socketFailed(QLocalSocket::LocalSocketError error);
    [[nodiscard]] SendResult send(OutboundMessage message) noexcept;
    void closeTransport(QString reason) noexcept;
    void setLifecycle(Lifecycle value, QString detail = {});

    QLocalSocket socket;
    std::unique_ptr<Client> client;
    Connection connection;
    ai::openai::codex::frontend::client::State currentState;
    Lifecycle currentLifecycle = Lifecycle::Disconnected;
    QString detail;
    std::set<std::string> requestedThreadReads;
    bool localShutdown = false;
};

} // namespace codexui

#endif // CODEXUI_APP_FRONTENDSESSION_H
