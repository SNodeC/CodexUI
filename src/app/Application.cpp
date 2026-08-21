// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/Application.h"

namespace codexui {

Application::~Application()
{
    // Complete/cancel frontend operations while MainWindow and its callback
    // targets are still alive. Member destruction happens after this body.
    frontendSession.shutdown();
}

void Application::show()
{
    mainWindow.show();
    frontendSession.connectToBackend();
}

} // namespace codexui
