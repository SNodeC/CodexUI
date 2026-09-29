// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#ifndef CODEXUI_CODEX_THREADBROWSER_H
#define CODEXUI_CODEX_THREADBROWSER_H

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace codexui::codex {

// Query identity excludes transport pagination. Used by the worker and its
// read-only projection; never infer membership from a filtered page's absence.
inline nlohmann::json threadBrowserParameters(std::string_view method,
                                              nlohmann::json parameters) {
  parameters.erase("cursor");
  parameters.erase("useStateDbOnly");
  parameters["limit"] = 100;
  if (method == "thread/list") {
    if (!parameters.contains("archived") || parameters["archived"].is_null())
      parameters["archived"] = false;
    if (!parameters.contains("sortKey"))
      parameters["sortKey"] = "recency_at";
    if (!parameters.contains("sortDirection"))
      parameters["sortDirection"] = "desc";
  }
  return parameters;
}

inline std::string threadBrowserKey(std::string_view method,
                                    const nlohmann::json &parameters) {
  return "thread-browser:" + std::string(method) + ":" +
         threadBrowserParameters(method, parameters).dump();
}

inline constexpr auto ProjectDescriptionKey = "codexui.description";

} // namespace codexui::codex
#endif
