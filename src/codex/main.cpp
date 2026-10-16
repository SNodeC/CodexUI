// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

// SNode.C declares emit(); parse its headers before any Qt header defines the emit macro.
// clang-format off
#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include <core/SNodeC.h>
#include <utils/Config.h>
#include "codex/MainWindow.h"
#include "codex/ui/ProfileWindow.h"
#include <QApplication>
#include <string_view>
// clang-format on

namespace {

    bool isConfigurationOnlyInvocation(int argc, char* argv[]) {
        bool result = false;
        for (int index = 1; !result && index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "-h" || argument.starts_with("--help") || argument == "-s" || argument == "--show-config" || argument == "-w" ||
                argument == "--write-config" || argument == "-v" || argument == "--version" || argument == "-k" || argument == "--kill" ||
                argument.starts_with("--command-line")) {
                result = true;
            }
        }
        return result;
    }

} // namespace

int main(int argc, char* argv[]) {
    const bool configurationOnly = isConfigurationOnlyInvocation(argc, argv);
    auto* configuration = utils::Config::configRoot.newSubCommand<codexui::codex::Configuration>();
    QApplication application(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("codex-ui"));
    QCoreApplication::setApplicationName(QStringLiteral("CodexUI"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("CodexUI"));
    codexui::ProfileWindow profileWindow;
    int result = 0;
    const int activation = configurationOnly ? 0 : profileWindow.prepare();
    if (activation < 0) {
        result = 1;
    } else if (activation == 0) {
        core::SNodeC::init(argc, argv);
        codexui::codex::FrontendSession session(*configuration);
        if (configurationOnly) {
            session.start(false);
            session.wait();
        } else {
            codexui::codex::MainWindow window(session);
            if (!profileWindow.attach(window)) {
                result = 1;
            } else {
                session.setRuntimeStoppedHandler([&application] {
                    application.quit();
                });
                session.start(true);
                window.show();
                result = application.exec();
                session.shutdown();
            }
        }
    }
    return result;
}
