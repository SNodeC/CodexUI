// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

namespace codexui::testing {

// CI accepts elapsed-time overruns, not correctness or bounded-work failures.
inline bool
timingLimit(bool met,
            std::source_location where = std::source_location::current()) {
  if (met)
    return true;
  const char *policy = std::getenv("CODEXUI_TIMING_POLICY");
  if (!policy || std::string_view(policy) != "report")
    return false;
  std::clog << "TIMING WARNING: accepted elapsed-time overrun at "
            << where.file_name() << ':' << where.line() << '\n';
  return true;
}

} // namespace codexui::testing
