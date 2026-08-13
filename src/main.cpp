// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/Application.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication qtApplication(argc, argv);
    qtApplication.setApplicationName("CodexUI");
    qtApplication.setOrganizationName("SNodeC");

    codexui::Application application;
    application.show();
    return qtApplication.exec();
}
