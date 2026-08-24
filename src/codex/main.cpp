// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/Configuration.h"
#include "codex/FrontendSession.h"

#include <core/SNodeC.h>
#include <utils/Config.h>

#include "codex/MainWindow.h"

#include <QApplication>

#include <string_view>

namespace {

bool isConfigurationOnlyInvocation(int argc, char *argv[]) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "-h" || argument.starts_with("--help") ||
        argument == "-s" || argument == "--show-config" || argument == "-w" ||
        argument == "--write-config" || argument == "-v" ||
        argument == "--version" || argument == "-k" || argument == "--kill" ||
        argument.starts_with("--command-line"))
      return true;
  }
  return false;
}

} // namespace

int main(int argc, char *argv[]) {
  const bool configurationOnly = isConfigurationOnlyInvocation(argc, argv);
  auto *configuration =
      utils::Config::configRoot.newSubCommand<codexui::codex::Configuration>();
  QApplication application(argc, argv);
  core::SNodeC::init(argc, argv);

  codexui::codex::FrontendSession session(*configuration);
  codexui::codex::MainWindow window(session);
  session.setRuntimeStoppedHandler([&application] { application.quit(); });
  session.start(!configurationOnly);

  if (configurationOnly) {
    session.wait();
    return 0;
  }

  window.show();
  const int result = application.exec();
  session.shutdown();
  return result;
}
