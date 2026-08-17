// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <iostream>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

} // namespace

int main()
{
    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        std::cerr << "could not create the Unix peer-credential fixture\n";
        return 1;
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
    return passed ? 0 : 1;
}
