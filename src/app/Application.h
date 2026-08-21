// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_APP_APPLICATION_H
#define CODEXUI_APP_APPLICATION_H

#include "app/FrontendSession.h"
#include "ui/MainWindow.h"

namespace codexui {

class Application
{
public:
    ~Application();
    void show();

private:
    FrontendSession frontendSession;
    MainWindow mainWindow{frontendSession};
};

} // namespace codexui

#endif // CODEXUI_APP_APPLICATION_H
