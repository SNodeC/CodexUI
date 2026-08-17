// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <QCoreApplication>

#include <iostream>
#include <limits>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>

namespace codexui {

struct FrontendSessionTestAccess
{
    static void setLifecycle(FrontendSession& session, FrontendSession::Lifecycle lifecycle, QString detail = {})
    {
        session.setLifecycle(lifecycle, std::move(detail));
    }

    static void handleConnectionStateChange(
        FrontendSession& session,
        const ai::openai::codex::frontend::client::ConnectionStateChange& change)
    {
        session.handleConnectionStateChange(change);
    }

    static void reportDiagnostic(FrontendSession& session, QString message)
    {
        session.reportDiagnostic(std::move(message));
    }

    static bool automaticReconnectEnabled(const FrontendSession& session)
    {
        return session.automaticReconnectEnabled;
    }

    static void prepareReconnectReset(FrontendSession& session)
    {
        session.automaticReconnectEnabled = false;
        session.reconnectDelayMs = FrontendSession::maximumReconnectDelayMs;
        session.reconnectTimer.start(60'000);
    }

    static void installConnectionWithTerminalClose(FrontendSession& session, bool& closeObserved)
    {
        session.connection = session.client->openConnection({
            [](FrontendSession::OutboundMessage) {
                return FrontendSession::SendResult{
                    ai::openai::codex::frontend::client::SendStatus::Accepted,
                    std::nullopt};
            },
            [&session, &closeObserved](std::string) {
                closeObserved = true;
                ai::openai::codex::frontend::client::Error terminalError;
                terminalError.message = "terminal close callback";
                terminalError.retryable = false;
                session.handleConnectionStateChange(
                    {ai::openai::codex::frontend::client::ConnectionState::Connecting,
                     ai::openai::codex::frontend::client::ConnectionState::Closed,
                     terminalError});
            },
        });
    }
};

} // namespace codexui

namespace {

namespace sdk = ai::openai::codex::frontend::client;

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

bool testPeerCredentials()
{
    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        std::cerr << "could not create the Unix peer-credential fixture\n";
        return false;
    }

    const uid_t currentUser = ::geteuid();
    const uid_t differentUser = currentUser == std::numeric_limits<uid_t>::max() ? currentUser - 1 : currentUser + 1;
    bool passed = true;
    passed &= expect(!codexui::detail::unixPeerCredentialError(sockets[0], currentUser),
                     "the connected process UID must be accepted");
    passed &= expect(codexui::detail::unixPeerCredentialError(sockets[0], differentUser).has_value(),
                     "a different process UID must be rejected");
    passed &= expect(codexui::detail::unixPeerCredentialError(-1, currentUser).has_value(),
                     "an invalid socket descriptor must fail closed");

    ::close(sockets[0]);
    ::close(sockets[1]);
    return passed;
}

bool testLifecycleAndDiagnostics()
{
    int lifecycleChanges = 0;
    int statusChanges = 0;
    bool reconnectCloseObserved = false;
    codexui::FrontendSession session;
    QObject::connect(&session, &codexui::FrontendSession::lifecycleChanged, [&lifecycleChanges] { ++lifecycleChanges; });
    QObject::connect(&session, &codexui::FrontendSession::statusChanged, [&statusChanges] { ++statusChanges; });

    codexui::FrontendSessionTestAccess::setLifecycle(session, codexui::FrontendSession::Lifecycle::Ready);
    codexui::FrontendSessionTestAccess::setLifecycle(session, codexui::FrontendSession::Lifecycle::Ready);
    bool passed = expect(lifecycleChanges == 1, "an identical lifecycle and detail must not emit a duplicate transition");

    codexui::FrontendSessionTestAccess::reportDiagnostic(session, QStringLiteral("projection diagnostic"));
    codexui::FrontendSessionTestAccess::reportDiagnostic(session, QStringLiteral("projection diagnostic"));
    codexui::FrontendSessionTestAccess::setLifecycle(session, codexui::FrontendSession::Lifecycle::Ready);
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Ready
                         && session.statusText() == QStringLiteral("projection diagnostic")
                         && lifecycleChanges == 1 && statusChanges == 1,
                     "an error diagnostic must update status once without changing a ready lifecycle");

    sdk::Error retryableError;
    retryableError.message = "temporary backend failure";
    retryableError.retryable = true;
    const sdk::ConnectionStateChange retryableChange{
        sdk::ConnectionState::Ready, sdk::ConnectionState::Disconnected, retryableError};
    codexui::FrontendSessionTestAccess::handleConnectionStateChange(session, retryableChange);
    const int retryableSignalCount = lifecycleChanges;
    codexui::FrontendSessionTestAccess::handleConnectionStateChange(session, retryableChange);
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && lifecycleChanges == retryableSignalCount && retryableSignalCount == 2,
                     "a retryable connection error must produce one failed transition and retain automatic reconnect");

    sdk::Error terminalError;
    terminalError.message = "terminal protocol failure";
    terminalError.retryable = false;
    codexui::FrontendSessionTestAccess::handleConnectionStateChange(
        session,
        {sdk::ConnectionState::Disconnected, sdk::ConnectionState::Closed, terminalError});
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && !codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && lifecycleChanges == 3,
                     "a nonretryable connection error must produce one failed transition and disable automatic reconnect");
    codexui::FrontendSessionTestAccess::handleConnectionStateChange(
        session,
        {sdk::ConnectionState::Closed, sdk::ConnectionState::Disconnected, std::nullopt});
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && lifecycleChanges == 3,
                     "a following physical close must preserve the terminal failure");

    codexui::FrontendSessionTestAccess::prepareReconnectReset(session);
    codexui::FrontendSessionTestAccess::installConnectionWithTerminalClose(session, reconnectCloseObserved);
    session.reconnectToBackend();
    passed &= expect(reconnectCloseObserved
                         && codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session),
                     "the public reconnect path must override a terminal old-transport close callback");
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    return testPeerCredentials() && testLifecycleAndDiagnostics() ? 0 : 1;
}
