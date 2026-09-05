// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_VALUE_H
#define CODEXUI_CODEX_NODEGRAPH_VALUE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace codexui::nodegraph {

// A directly owned representation of the protocol values retained by graph
// nodes. Accessors are deliberately exact and non-throwing: callers decide
// how an unexpected protocol alternative should be handled.
class Value final {
public:
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value, std::less<>>;

  Value() noexcept = default;
  Value(std::nullptr_t) noexcept;
  Value(bool value) noexcept;
  Value(std::int64_t value) noexcept;
  Value(std::uint64_t value) noexcept;
  Value(double value) noexcept;
  Value(const char *value);
  Value(std::string value);
  Value(std::string_view value);
  Value(Array value);
  Value(Object value);

  template <
      typename Integer,
      std::enable_if_t<std::is_integral_v<std::remove_cv_t<Integer>> &&
                           !std::is_same_v<std::remove_cv_t<Integer>, bool>,
                       int> = 0>
  Value(Integer value) noexcept {
    if constexpr (std::is_signed_v<Integer>)
      storage_.emplace<std::int64_t>(static_cast<std::int64_t>(value));
    else
      storage_.emplace<std::uint64_t>(static_cast<std::uint64_t>(value));
  }

  [[nodiscard]] bool isNull() const noexcept;
  [[nodiscard]] bool isBool() const noexcept;
  [[nodiscard]] bool isSigned() const noexcept;
  [[nodiscard]] bool isUnsigned() const noexcept;
  [[nodiscard]] bool isDouble() const noexcept;
  [[nodiscard]] bool isString() const noexcept;
  [[nodiscard]] bool isArray() const noexcept;
  [[nodiscard]] bool isObject() const noexcept;

  [[nodiscard]] bool *asBool() noexcept;
  [[nodiscard]] const bool *asBool() const noexcept;
  [[nodiscard]] std::int64_t *asInt64() noexcept;
  [[nodiscard]] const std::int64_t *asInt64() const noexcept;
  [[nodiscard]] std::uint64_t *asUInt64() noexcept;
  [[nodiscard]] const std::uint64_t *asUInt64() const noexcept;
  [[nodiscard]] double *asDouble() noexcept;
  [[nodiscard]] const double *asDouble() const noexcept;
  [[nodiscard]] std::string *asString() noexcept;
  [[nodiscard]] const std::string *asString() const noexcept;
  [[nodiscard]] Array *asArray() noexcept;
  [[nodiscard]] const Array *asArray() const noexcept;
  [[nodiscard]] Object *asObject() noexcept;
  [[nodiscard]] const Object *asObject() const noexcept;

  [[nodiscard]] Value *find(std::string_view key) noexcept;
  [[nodiscard]] const Value *find(std::string_view key) const noexcept;

  bool operator==(const Value &) const = default;

private:
  using Storage =
      std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
                   std::string, Array, Object>;

  Storage storage_;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_VALUE_H
