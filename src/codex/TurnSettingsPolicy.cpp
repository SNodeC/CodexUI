// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/TurnSettingsPolicy.h"

#include <algorithm>
#include <ranges>

namespace codexui::codex {
namespace {

constexpr std::size_t index(TurnSettingField field) noexcept {
  return static_cast<std::size_t>(field);
}

std::string trimmed(std::string_view value) {
  const std::size_t first = value.find_first_not_of(" \t\r\n\f\v");
  if (first == std::string_view::npos)
    return {};
  const std::size_t last = value.find_last_not_of(" \t\r\n\f\v");
  return std::string(value.substr(first, last - first + 1));
}

void copyChoice(nlohmann::json &result, const TurnSettingsPolicy &policy,
                TurnSettingField field, const char *name) {
  if (!policy.touched(field))
    return;
  const std::string &selected = policy.values()[field];
  result[name] = selected == DefaultTurnSetting ? nlohmann::json(nullptr)
                                                : nlohmann::json(selected);
}

nlohmann::json sandboxPolicy(const TurnSettingValues &values) {
  const std::string &access = values[TurnSettingField::Sandbox];
  const bool network = values[TurnSettingField::Network] == "enabled";
  if (access == DefaultTurnSetting)
    return nullptr;
  if (access == "danger-full-access")
    return {{"type", "dangerFullAccess"}};
  if (access == "external")
    return {{"type", "externalSandbox"},
            {"networkAccess", network ? "enabled" : "restricted"}};
  if (access == "read-only")
    return {{"type", "readOnly"}, {"networkAccess", network}};
  return {{"type", "workspaceWrite"},
          {"writableRoots", nlohmann::json::array()},
          {"networkAccess", network},
          {"excludeTmpdirEnvVar", false},
          {"excludeSlashTmp", false}};
}

} // namespace

TurnSettingsPolicy::Draft *TurnSettingsPolicy::draft() noexcept {
  const auto found = drafts_.find(context_.identity);
  return found == drafts_.end() ? nullptr : &found->second;
}

const TurnSettingsPolicy::Draft *TurnSettingsPolicy::draft() const noexcept {
  const auto found = drafts_.find(context_.identity);
  return found == drafts_.end() ? nullptr : &found->second;
}

bool TurnSettingsPolicy::setContext(TurnSettingsContext context) {
  if (context.providerAuthorityRevision <
      context_.providerAuthorityRevision)
    return false;
  const TurnSettingValues oldValues = values();
  const TurnSettingMask oldTouched =
      draft() ? draft()->touched : TurnSettingMask{};
  const std::string oldIdentity = context_.identity;
  const bool authorityChanged =
      context.providerAuthorityRevision > context_.providerAuthorityRevision;
  if (authorityChanged)
    drafts_.clear();
  if (context.identity.empty()) {
    const bool changed = context_ != context || oldTouched.any();
    context_ = std::move(context);
    return changed;
  }
  if (!authorityChanged && context == context_ && draft())
    return false;

  context_ = std::move(context);

  auto [entry, inserted] = drafts_.try_emplace(context_.identity);
  Draft &current = entry->second;
  const bool replaced =
      !inserted && current.threadIncarnation != 0 &&
      context_.threadIncarnation != 0 &&
      current.threadIncarnation != context_.threadIncarnation;
  if (inserted || replaced) {
    current = {};
    current.values = context_.canonical;
    current.acknowledged = context_.acknowledged;
  } else {
    for (std::size_t field = 0; field < TurnSettingFieldCount; ++field) {
      if (context_.acknowledged[field] > current.acknowledged[field] &&
          current.values.fields[field] == context_.canonical.fields[field])
        current.touched.reset(field);
      if (!current.touched.test(field))
        current.values.fields[field] = context_.canonical.fields[field];
      current.acknowledged[field] =
          std::max(current.acknowledged[field], context_.acknowledged[field]);
    }
  }
  if (context_.threadIncarnation != 0)
    current.threadIncarnation = context_.threadIncarnation;
  normalize(current);
  return authorityChanged || oldIdentity != context_.identity ||
         oldValues != current.values || oldTouched != current.touched;
}

void TurnSettingsPolicy::change(TurnSettingField field, std::string value) {
  Draft *current = draft();
  if (!current)
    return;
  if ((field == TurnSettingField::Model ||
       field == TurnSettingField::PermissionProfile ||
       field == TurnSettingField::ServiceTier ||
       field == TurnSettingField::Collaboration) &&
      value.empty())
    value = std::string(DefaultTurnSetting);
  current->values[field] = std::move(value);
  current->touched.set(index(field));
  if (field == TurnSettingField::Sandbox ||
      field == TurnSettingField::Network) {
    current->values[TurnSettingField::PermissionProfile] =
        std::string(DefaultTurnSetting);
    current->touched.set(index(TurnSettingField::PermissionProfile));
  } else if (field == TurnSettingField::PermissionProfile &&
             current->values[field] != DefaultTurnSetting) {
    current->values[TurnSettingField::Sandbox] =
        context_.canonical[TurnSettingField::Sandbox];
    current->values[TurnSettingField::Network] =
        context_.canonical[TurnSettingField::Network];
    current->touched.reset(index(TurnSettingField::Sandbox));
    current->touched.reset(index(TurnSettingField::Network));
  }
  normalize(*current);
}

void TurnSettingsPolicy::promote(std::string_view sourceIdentity,
                                 std::string_view canonicalIdentity,
                                 std::uint64_t canonicalIncarnation) {
  if (sourceIdentity == canonicalIdentity)
    return;
  auto source = drafts_.extract(std::string(sourceIdentity));
  if (source.empty())
    return;
  drafts_.erase(std::string(canonicalIdentity));
  source.key() = canonicalIdentity;
  source.mapped().threadIncarnation = canonicalIncarnation;
  drafts_.insert(std::move(source));
  if (context_.identity == sourceIdentity)
    context_.identity = canonicalIdentity;
}

void TurnSettingsPolicy::forget(std::string_view identity) {
  drafts_.erase(std::string(identity));
}

void TurnSettingsPolicy::forget(std::string_view identity,
                                std::uint64_t incarnation) {
  const auto found = drafts_.find(std::string(identity));
  if (found != drafts_.end() &&
      found->second.threadIncarnation == incarnation)
    drafts_.erase(found);
}

const TurnSettingValues &TurnSettingsPolicy::values() const noexcept {
  if (const Draft *current = draft())
    return current->values;
  return context_.canonical;
}

bool TurnSettingsPolicy::touched(TurnSettingField field) const noexcept {
  const Draft *current = draft();
  return current && current->touched.test(index(field));
}

const TurnSettingModel *TurnSettingsPolicy::selectedModel() const noexcept {
  const std::string &selected = values()[TurnSettingField::Model];
  const auto found = std::ranges::find_if(
      context_.models, [&selected](const TurnSettingModel &candidate) {
        return selected == DefaultTurnSetting
                   ? candidate.isDefault
                   : candidate.choice.value == selected;
      });
  return found == context_.models.end() ? nullptr : &*found;
}

bool TurnSettingsPolicy::personalityEnabled() const noexcept {
  const TurnSettingModel *model = selectedModel();
  return !model || model->supportsPersonality;
}

bool TurnSettingsPolicy::networkEnabled() const noexcept {
  const std::string &sandbox = values()[TurnSettingField::Sandbox];
  return sandbox != DefaultTurnSetting && sandbox != "danger-full-access";
}

std::string TurnSettingsPolicy::workspace(std::string_view fallback) const {
  const std::string selected = trimmed(values()[TurnSettingField::Workspace]);
  return selected.empty() ? std::string(fallback) : selected;
}

nlohmann::json TurnSettingsPolicy::startOptions(TurnSettingsScope scope) const {
  const bool turn = scope == TurnSettingsScope::Turn;
  nlohmann::json result = nlohmann::json::object();
  copyChoice(result, *this, TurnSettingField::Model, "model");
  copyChoice(result, *this, TurnSettingField::Personality, "personality");
  copyChoice(result, *this, TurnSettingField::Approval, "approvalPolicy");
  copyChoice(result, *this, TurnSettingField::Reviewer, "approvalsReviewer");
  copyChoice(result, *this, TurnSettingField::ServiceTier, "serviceTier");
  if (turn) {
    copyChoice(result, *this, TurnSettingField::Effort, "effort");
    copyChoice(result, *this, TurnSettingField::Summary, "summary");
  }
  if (touched(TurnSettingField::Workspace)) {
    const std::string cwd = trimmed(values()[TurnSettingField::Workspace]);
    result["cwd"] = cwd.empty() ? nlohmann::json(nullptr) : nlohmann::json(cwd);
  }
  const std::string &profile = values()[TurnSettingField::PermissionProfile];
  if (touched(TurnSettingField::PermissionProfile))
    result["permissions"] = profile == DefaultTurnSetting
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(profile);
  if (profile == DefaultTurnSetting && turn &&
      (touched(TurnSettingField::Sandbox) ||
       touched(TurnSettingField::Network))) {
    result["sandboxPolicy"] = sandboxPolicy(values());
  } else if (profile == DefaultTurnSetting &&
             touched(TurnSettingField::Sandbox)) {
    const std::string &sandbox = values()[TurnSettingField::Sandbox];
    result["sandbox"] = sandbox == DefaultTurnSetting || sandbox == "external"
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(sandbox);
  }

  if (turn) {
    std::string selected = values()[TurnSettingField::Model];
    if (const TurnSettingModel *definition = selectedModel();
        selected == DefaultTurnSetting && definition)
      selected = definition->choice.value;
    if (selected.empty() || selected == DefaultTurnSetting)
      return result;
    const std::string &effort = values()[TurnSettingField::Effort];
    result["collaborationMode"] = {
        {"mode", values()[TurnSettingField::Collaboration]},
        {"settings",
         {{"model", selected},
          {"developer_instructions", nullptr},
          {"reasoning_effort", effort == DefaultTurnSetting
                                   ? nlohmann::json(nullptr)
                                   : nlohmann::json(effort)}}}};
  }
  return result;
}

void TurnSettingsPolicy::normalize(Draft &current) const {
  std::string &sandbox = current.values[TurnSettingField::Sandbox];
  if (sandbox != DefaultTurnSetting && sandbox != "workspace-write" &&
      sandbox != "read-only" && sandbox != "danger-full-access" &&
      sandbox != "external")
    sandbox = std::string(DefaultTurnSetting);
  if (sandbox == DefaultTurnSetting)
    current.values[TurnSettingField::Network] = std::string(DefaultTurnSetting);
  else if (sandbox == "danger-full-access")
    current.values[TurnSettingField::Network] = "enabled";

  const TurnSettingModel *model = selectedModel();
  if (!model)
    return;
  std::string &effort = current.values[TurnSettingField::Effort];
  if (effort != DefaultTurnSetting && !model->reasoningEfforts.empty() &&
      std::ranges::find(model->reasoningEfforts, effort) ==
          model->reasoningEfforts.end())
    effort = std::string(DefaultTurnSetting);
  if (!model->supportsPersonality)
    current.values[TurnSettingField::Personality] =
        std::string(DefaultTurnSetting);
}

} // namespace codexui::codex
