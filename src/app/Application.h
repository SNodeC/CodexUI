// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_APP_APPLICATION_H
#define CODEXUI_APP_APPLICATION_H

#include "ui/MainWindow.h"

namespace codexui {

class Application
{
public:
    void show();

private:
    MainWindow mainWindow;
};

} // namespace codexui

#endif // CODEXUI_APP_APPLICATION_H
