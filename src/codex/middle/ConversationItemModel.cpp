// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationItemModel.h"

#include <QString>
#include <QStringList>

#include <algorithm>
#include <iterator>
#include <limits>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

QString cardLabel(CardKind kind) {
  switch (kind) {
  case CardKind::UserMessage:
    return QStringLiteral("You");
  case CardKind::AgentMessage:
    return QStringLiteral("Codex");
  case CardKind::CommandExecution:
    return QStringLiteral("Command execution");
  case CardKind::AgentActivity:
    return QStringLiteral("Agent activity");
  case CardKind::Reasoning:
    return QStringLiteral("Reasoning");
  case CardKind::FileChanges:
    return QStringLiteral("File changes");
  case CardKind::ImageGeneration:
    return QStringLiteral("Generated image");
  case CardKind::Plan:
    return QStringLiteral("Plan");
  case CardKind::GenericActivity:
    return QStringLiteral("Activity");
  case CardKind::LocalPrompt:
    return QStringLiteral("You");
  }
  return QStringLiteral("Activity");
}

QString boundedAccessibleText(std::string_view value) {
  constexpr std::size_t MaximumAccessibleBytes = 8192;
  const std::size_t length = std::min(value.size(), MaximumAccessibleBytes);
  QString result =
      QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
  if (length != value.size())
    result += QStringLiteral("…");
  return result;
}

QString accessibleCardText(const VisibleCardData &card) {
  QString detail = std::visit(
      [](const auto &payload) -> QString {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, UserMessageData>)
          return boundedAccessibleText(payload.text);
        if constexpr (std::is_same_v<Payload, AgentMessageData>)
          return boundedAccessibleText(payload.text);
        if constexpr (std::is_same_v<Payload, CommandExecutionData>)
          return QStringLiteral("%1\n%2").arg(
              boundedAccessibleText(payload.command),
              boundedAccessibleText(payload.output));
        if constexpr (std::is_same_v<Payload, AgentActivityData>)
          return QStringLiteral("%1\n%2").arg(
              boundedAccessibleText(payload.prompt),
              boundedAccessibleText(payload.resultText));
        if constexpr (std::is_same_v<Payload, ReasoningData>)
          return boundedAccessibleText(payload.summary);
        if constexpr (std::is_same_v<Payload, FileChangesData>) {
          QStringList paths;
          for (const FileChangeData &change : payload.changes)
            paths.push_back(boundedAccessibleText(change.path));
          return paths.join(QLatin1Char('\n'));
        }
        if constexpr (std::is_same_v<Payload, ImageGenerationData>)
          return QStringLiteral("%1\n%2").arg(
              boundedAccessibleText(payload.revisedPrompt),
              boundedAccessibleText(payload.path));
        if constexpr (std::is_same_v<Payload, PlanData>) {
          QStringList lines{boundedAccessibleText(payload.explanation)};
          for (const PlanStepData &step : payload.steps)
            lines.push_back(boundedAccessibleText(step.text));
          if (!payload.legacyText.empty())
            lines.push_back(boundedAccessibleText(payload.legacyText));
          return lines.join(QLatin1Char('\n'));
        }
        if constexpr (std::is_same_v<Payload, GenericActivityData>)
          return boundedAccessibleText(payload.displayDetail);
        if constexpr (std::is_same_v<Payload, LocalPromptData>)
          return boundedAccessibleText(payload.prompt);
        return {};
      },
      card.payload);
  constexpr qsizetype MaximumAccessibleCharacters = 8192;
  if (detail.size() > MaximumAccessibleCharacters) {
    detail.truncate(MaximumAccessibleCharacters);
    detail += QStringLiteral("…");
  }
  const QString label = cardLabel(card.kind);
  return detail.isEmpty() ? label : label + QStringLiteral("\n") + detail;
}

bool compatible(const VisibleCardData &before,
                const VisibleCardData &after) noexcept {
  return before.key == after.key &&
         (before.kind == after.kind || (before.kind == CardKind::LocalPrompt &&
                                        after.kind == CardKind::UserMessage));
}

} // namespace

ConversationItemModel::ConversationItemModel(QObject *parent)
    : QAbstractListModel(parent) {}

int ConversationItemModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ConversationItemModel::data(const QModelIndex &index, int role) const {
  const Row *value = row(index.row());
  if (!value || index.column() != 0)
    return {};
  switch (role) {
  case Qt::DisplayRole:
    return cardLabel(value->card.kind);
  case Qt::AccessibleTextRole:
    return accessibleCardText(value->card);
  case StableKeyRole:
    return QString::fromStdString(value->stableKey);
  case ThreadIdRole:
    return QString::fromStdString(value->card.threadId);
  case TurnIdRole:
    return QString::fromStdString(value->card.turnId);
  case ItemIdRole:
    return QString::fromStdString(value->card.itemId);
  case CardKindRole:
    return static_cast<int>(value->card.kind);
  case TargetIdentityRole:
    return value->card.target
               ? QString::fromStdString(value->card.target->id().canonical)
               : QString{};
  case TurnSectionRole:
    return QString::fromStdString(value->sectionKey);
  case TurnRootRole:
    return value->turnRoot;
  case NestedCardRole:
    return value->nested;
  case FirstInTurnRole:
    return value->firstInTurn;
  case LastInTurnRole:
    return value->lastInTurn;
  case PresentedRole:
    return value->presented;
  case ActiveTurnRole:
    return value->activeTurn;
  case PresentationRole:
    // Large card values intentionally remain available only through card().
    // Returning them through QVariant would copy streamed text.
    return {};
  default:
    return {};
  }
}

Qt::ItemFlags ConversationItemModel::flags(const QModelIndex &index) const {
  const Row *value = row(index.row());
  if (!value || !value->presented)
    return Qt::NoItemFlags;
  return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QHash<int, QByteArray> ConversationItemModel::roleNames() const {
  return {
      {StableKeyRole, "stableKey"},     {ThreadIdRole, "threadId"},
      {TurnIdRole, "turnId"},           {ItemIdRole, "itemId"},
      {CardKindRole, "cardKind"},       {TargetIdentityRole, "targetIdentity"},
      {TurnSectionRole, "turnSection"}, {TurnRootRole, "turnRoot"},
      {NestedCardRole, "nestedCard"},   {FirstInTurnRole, "firstInTurn"},
      {LastInTurnRole, "lastInTurn"},   {PresentedRole, "presented"},
      {ActiveTurnRole, "activeTurn"},   {PresentationRole, "presentation"}};
}

bool ConversationItemModel::reconcile(ConversationSnapshot snapshot) {
  const bool authorityReplacement = snapshot.threadId != threadId_;
  const std::string nextThreadId = snapshot.threadId;
  const std::size_t nextHiddenCount = snapshot.hiddenAuthoritativeItemCount;
  const bool nextHasMore = snapshot.hasMore;
  std::vector<Row> desired = flatten(std::move(snapshot));
  std::unordered_set<std::string> unique;
  unique.reserve(desired.size());
  for (const Row &row : desired)
    if (!unique.insert(row.stableKey).second)
      return false;

  const bool chromeChanged = nextHiddenCount != hiddenAuthoritativeItemCount_ ||
                             nextHasMore != hasMore_;
  hiddenAuthoritativeItemCount_ = nextHiddenCount;
  hasMore_ = nextHasMore;
  if (authorityReplacement) {
    beginResetModel();
    rows_.clear();
    rows_.insert(rows_.end(), std::make_move_iterator(desired.begin()),
                 std::make_move_iterator(desired.end()));
    threadId_ = nextThreadId;
    rebuildIndexes();
    endResetModel();
    incrementProperty("modelResetCount");
    return true;
  }

  std::unordered_set<std::string> desiredKeys;
  desiredKeys.reserve(desired.size());
  for (const Row &row : desired)
    desiredKeys.insert(row.stableKey);

  bool changed = chromeChanged;
  bool indexesDirty = false;
  for (std::size_t offset = rows_.size(); offset > 0;) {
    std::size_t last = offset - 1;
    if (desiredKeys.contains(rows_[last].stableKey)) {
      offset = last;
      continue;
    }
    std::size_t first = last;
    while (first > 0 && !desiredKeys.contains(rows_[first - 1].stableKey))
      --first;
    beginRemoveRows({}, static_cast<int>(first), static_cast<int>(last));
    rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(first),
                rows_.begin() + static_cast<std::ptrdiff_t>(last + 1));
    endRemoveRows();
    incrementProperty("modelRemoveCount");
    changed = true;
    indexesDirty = true;
    offset = first;
  }

  std::vector<bool> inserted(desired.size(), false);
  for (std::size_t position = 0; position < desired.size(); ++position) {
    if (position < rows_.size() &&
        rows_[position].stableKey == desired[position].stableKey)
      continue;
    const auto found =
        std::find_if(rows_.begin() + static_cast<std::ptrdiff_t>(
                                         std::min(position, rows_.size())),
                     rows_.end(), [&](const Row &row) {
                       return row.stableKey == desired[position].stableKey;
                     });
    if (found == rows_.end()) {
      std::size_t count = 1;
      while (position + count < desired.size() &&
             std::ranges::none_of(
                 rows_,
                 [&](const Row &row) {
                   return row.stableKey == desired[position + count].stableKey;
                 }))
        ++count;
      beginInsertRows({}, static_cast<int>(position),
                      static_cast<int>(position + count - 1));
      rows_.insert(
          rows_.begin() + static_cast<std::ptrdiff_t>(position),
          std::make_move_iterator(desired.begin() +
                                  static_cast<std::ptrdiff_t>(position)),
          std::make_move_iterator(
              desired.begin() + static_cast<std::ptrdiff_t>(position + count)));
      endInsertRows();
      std::fill(inserted.begin() + static_cast<std::ptrdiff_t>(position),
                inserted.begin() +
                    static_cast<std::ptrdiff_t>(position + count),
                true);
      incrementProperty("modelInsertCount");
      changed = true;
      indexesDirty = true;
      position += count - 1;
      continue;
    }

    const std::size_t source =
        static_cast<std::size_t>(std::distance(rows_.begin(), found));
    beginMoveRows({}, static_cast<int>(source), static_cast<int>(source), {},
                  static_cast<int>(position));
    Row moved = std::move(rows_[source]);
    rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(source));
    rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(position),
                 std::move(moved));
    endMoveRows();
    incrementProperty("modelMoveCount");
    changed = true;
    indexesDirty = true;
  }

  for (std::size_t position = 0; position < desired.size(); ++position) {
    if (inserted[position])
      continue;
    if (rows_[position] == desired[position])
      continue;
    indexesDirty = indexesDirty ||
                   rows_[position].card.target != desired[position].card.target;
    updateRow(static_cast<int>(position), std::move(desired[position]));
    changed = true;
  }
  if (indexesDirty)
    rebuildIndexes();
  return changed;
}

ConversationItemModel::CardUpdateResult
ConversationItemModel::updateCard(VisibleCardData card) {
  const std::string key = stableKey(card.key);
  const auto found = stableRows_.find(key);
  if (found == stableRows_.end())
    return CardUpdateResult::Missing;
  const std::optional<int> modelRow = logicalRow(found->second);
  if (!modelRow)
    return CardUpdateResult::Missing;
  Row &current = rows_[static_cast<std::size_t>(*modelRow)];
  if (!compatible(current.card, card))
    return CardUpdateResult::Incompatible;
  if (current.card == card)
    return CardUpdateResult::Unchanged;

  const nodegraph::Node *oldTarget =
      current.card.target ? current.card.target.get() : nullptr;
  const nodegraph::Node *newTarget = card.target ? card.target.get() : nullptr;
  Row replacement;
  replacement.card = std::move(card);
  replacement.stableKey = current.stableKey;
  replacement.sectionKey = current.sectionKey;
  replacement.turnRoot = current.turnRoot;
  replacement.nested = current.nested;
  replacement.firstInTurn = current.firstInTurn;
  replacement.lastInTurn = current.lastInTurn;
  replacement.presented = isPresented(replacement.card);
  replacement.activeTurn = current.activeTurn;
  replacement.historyActivity = current.historyActivity;
  updateRow(*modelRow, std::move(replacement));
  if (oldTarget != newTarget) {
    if (oldTarget)
      targetRows_.erase(oldTarget);
    if (newTarget)
      targetRows_.insert_or_assign(newTarget, found->second);
  }
  return CardUpdateResult::Changed;
}

bool ConversationItemModel::appendTail(ConversationTailCard tail) {
  if (tail.card.threadId != threadId_ || tail.sectionKey.empty())
    return false;
  const std::string key = stableKey(tail.card.key);
  if (key.empty() || stableRows_.contains(key))
    return false;

  const bool startsSection =
      rows_.empty() || rows_.back().sectionKey != tail.sectionKey;
  if ((!startsSection && tail.turnRoot) || (startsSection && tail.nested))
    return false;

  if (!rows_.empty() && !startsSection) {
    Row &previous = rows_.back();
    previous.lastInTurn = false;
    emit dataChanged(index(rowCount() - 1), index(rowCount() - 1),
                     {LastInTurnRole});
    incrementProperty("modelDataChangeCount");
  }

  Row row;
  row.card = std::move(tail.card);
  row.stableKey = key;
  row.sectionKey = std::move(tail.sectionKey);
  row.turnRoot = tail.turnRoot;
  row.nested = tail.nested;
  row.firstInTurn = startsSection;
  row.lastInTurn = true;
  row.presented = isPresented(row.card);
  row.activeTurn = tail.turnRoot && tail.activeTurn;
  row.historyActivity = tail.historyActivity;

  const int insertedRow = rowCount();
  const std::size_t ordinal = rowBase_ + rows_.size();
  beginInsertRows({}, insertedRow, insertedRow);
  rows_.push_back(std::move(row));
  stableRows_.emplace(key, ordinal);
  if (rows_.back().card.target)
    targetRows_.emplace(rows_.back().card.target.get(), ordinal);
  if (rows_.back().historyActivity)
    ++historyActivityCount_;
  endInsertRows();
  incrementProperty("modelInsertCount");
  incrementProperty("modelTailAppendCount");
  return true;
}

ConversationItemModel::HistoryTrim
ConversationItemModel::trimHistoryTo(std::size_t activityLimit) {
  HistoryTrim result;
  if (historyActivityCount_ <= activityLimit || rows_.empty())
    return result;

  Row &first = rows_.front();
  result.sectionKey = first.sectionKey;
  if (first.historyActivity && first.turnRoot && rows_.size() > 1 &&
      rows_[1].sectionKey == first.sectionKey) {
    first.historyActivity = false;
    --historyActivityCount_;
    result.pinnedRoot = true;
    result.sectionKey = first.sectionKey;
    incrementProperty("modelHistoryRootPins");
    return result;
  }

  // Pending/recovery prompts are protected independently of the history
  // suffix. If one is the complete leading section, leave the window one row
  // over budget until its authoritative acknowledgement or a complete
  // reconciliation can place it without changing optimistic ordering.
  if (!first.historyActivity && (!first.turnRoot || rows_.size() == 1 ||
                                 rows_[1].sectionKey != first.sectionKey))
    return result;

  int removeCount = 1;
  int removeRow = 0;
  if (!first.historyActivity && first.turnRoot && rows_.size() > 1 &&
      rows_[1].sectionKey == first.sectionKey) {
    result.sectionKey = first.sectionKey;
    if (rows_.size() > 2 && rows_[2].sectionKey == first.sectionKey) {
      // Retain the pinned owner at logical row zero while dropping the oldest
      // nested activity. Moving that one row across the deque prefix keeps all
      // later absolute identity ordinals unchanged.
      removeRow = 1;
    } else {
      removeCount = 2;
    }
  }

  for (int offset = 0; offset < removeCount; ++offset) {
    const Row &removed = rows_[static_cast<std::size_t>(removeRow + offset)];
    result.removedStableKeys.push_back(removed.stableKey);
    if (removed.historyActivity) {
      --historyActivityCount_;
      ++result.hiddenIncrement;
    }
  }
  result.row = removeRow;
  result.count = removeCount;

  beginRemoveRows({}, removeRow, removeRow + removeCount - 1);
  if (removeRow == 1) {
    Row retainedRoot = std::move(rows_.front());
    eraseRowIdentity(rows_[1]);
    rows_.pop_front();
    rows_.front() = std::move(retainedRoot);
    ++rowBase_;
    stableRows_.insert_or_assign(rows_.front().stableKey, rowBase_);
    if (rows_.front().card.target)
      targetRows_.insert_or_assign(rows_.front().card.target.get(), rowBase_);
  } else {
    for (int offset = 0; offset < removeCount; ++offset) {
      eraseRowIdentity(rows_.front());
      rows_.pop_front();
      ++rowBase_;
    }
  }
  endRemoveRows();
  incrementProperty("modelRemoveCount");
  incrementProperty("modelBoundedFrontTrimCount");
  return result;
}

bool ConversationItemModel::setActiveTurn(int rowIndex, bool active) {
  Row *value = rowIndex >= 0 && rowIndex < rowCount()
                   ? &rows_[static_cast<std::size_t>(rowIndex)]
                   : nullptr;
  if (!value || !value->turnRoot || value->activeTurn == active)
    return false;
  value->activeTurn = active;
  emit dataChanged(index(rowIndex), index(rowIndex), {ActiveTurnRole});
  incrementProperty("modelDataChangeCount");
  return true;
}

void ConversationItemModel::setHistoryChrome(
    std::size_t hiddenAuthoritativeItemCount, bool providerHasMore) {
  hiddenAuthoritativeItemCount_ = hiddenAuthoritativeItemCount;
  hasMore_ = hiddenAuthoritativeItemCount != 0 || providerHasMore;
}

bool ConversationItemModel::setVisibility(Visibility visibility) {
  if (visibility_ == visibility)
    return false;
  visibility_ = visibility;
  int first = -1;
  bool changed = false;
  for (std::size_t position = 0; position < rows_.size(); ++position) {
    Row &row = rows_[position];
    const bool presented = isPresented(row.card);
    if (presented == row.presented) {
      if (first >= 0) {
        emit dataChanged(index(first), index(static_cast<int>(position) - 1),
                         {PresentedRole});
        incrementProperty("modelDataChangeCount");
        first = -1;
      }
      continue;
    }
    row.presented = presented;
    changed = true;
    if (first < 0)
      first = static_cast<int>(position);
  }
  if (first < 0)
    return changed;
  emit dataChanged(index(first), index(rowCount() - 1), {PresentedRole});
  incrementProperty("modelDataChangeCount");
  return true;
}

const ConversationItemModel::Row *
ConversationItemModel::row(int rowIndex) const noexcept {
  return rowIndex >= 0 && static_cast<std::size_t>(rowIndex) < rows_.size()
             ? &rows_[static_cast<std::size_t>(rowIndex)]
             : nullptr;
}

const VisibleCardData *
ConversationItemModel::card(int rowIndex) const noexcept {
  const Row *value = row(rowIndex);
  return value ? &value->card : nullptr;
}

QModelIndex
ConversationItemModel::indexForStableKey(const std::string &key) const {
  const auto found = stableRows_.find(key);
  if (found == stableRows_.end())
    return {};
  const std::optional<int> row = logicalRow(found->second);
  return row ? index(*row) : QModelIndex{};
}

QModelIndex
ConversationItemModel::indexForTarget(const nodegraph::NodeRef &target) const {
  if (!target)
    return {};
  const auto found = targetRows_.find(target.get());
  if (found == targetRows_.end())
    return {};
  const std::optional<int> modelRow = logicalRow(found->second);
  const Row *candidate = modelRow ? row(*modelRow) : nullptr;
  return candidate && candidate->card.target == target ? index(*modelRow)
                                                       : QModelIndex{};
}

std::vector<ConversationItemModel::Row>
ConversationItemModel::flatten(ConversationSnapshot &&snapshot) const {
  std::size_t count = 0;
  for (const TurnSection &section : snapshot.sections)
    count += section.cards.size();
  std::vector<Row> result;
  result.reserve(count);
  for (TurnSection &section : snapshot.sections) {
    std::optional<std::string> root;
    if (section.rootCardKey)
      root = stableKey(*section.rootCardKey);
    const bool representedRoot =
        root && std::ranges::any_of(section.cards, [&](const auto &card) {
          return stableKey(card.key) == *root;
        });
    for (std::size_t position = 0; position < section.cards.size();
         ++position) {
      VisibleCardData card = std::move(section.cards[position]);
      const std::string key = stableKey(card.key);
      const bool turnRoot = representedRoot && key == *root;
      result.push_back(Row{std::move(card), key, section.key, turnRoot,
                           representedRoot && !turnRoot, position == 0,
                           position + 1 == section.cards.size(), false, false,
                           false});
      Row &row = result.back();
      row.presented = isPresented(row.card);
      row.activeTurn = turnRoot && snapshot.activeTurnId &&
                       row.card.turnId == *snapshot.activeTurnId;
      row.historyActivity = row.card.kind != CardKind::LocalPrompt &&
                            !(turnRoot && section.rootPinned);
    }
  }
  return result;
}

bool ConversationItemModel::isPresented(
    const VisibleCardData &card) const noexcept {
  if (card.kind == CardKind::Reasoning)
    return visibility_.showReasoning;
  if (card.kind != CardKind::AgentMessage)
    return true;
  const auto *message = std::get_if<AgentMessageData>(&card.payload);
  return !message || message->finalAnswer || visibility_.showCodexUpdates;
}

void ConversationItemModel::rebuildIndexes() {
  stableRows_.clear();
  targetRows_.clear();
  stableRows_.reserve(rows_.size());
  targetRows_.reserve(rows_.size());
  rowBase_ = 0;
  historyActivityCount_ = 0;
  for (std::size_t position = 0; position < rows_.size(); ++position) {
    Row &row = rows_[position];
    stableRows_.emplace(row.stableKey, position);
    if (row.card.target)
      targetRows_.emplace(row.card.target.get(), position);
    if (row.historyActivity)
      ++historyActivityCount_;
  }
  incrementProperty("modelIndexRebuildCount");
}

std::optional<int>
ConversationItemModel::logicalRow(std::size_t ordinal) const {
  if (ordinal < rowBase_ || ordinal - rowBase_ >= rows_.size())
    return std::nullopt;
  const std::size_t value = ordinal - rowBase_;
  if (value > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return std::nullopt;
  return static_cast<int>(value);
}

void ConversationItemModel::eraseRowIdentity(const Row &row) {
  stableRows_.erase(row.stableKey);
  if (row.card.target)
    targetRows_.erase(row.card.target.get());
}

void ConversationItemModel::incrementProperty(const char *name) {
  setProperty(name, property(name).toULongLong() + 1);
}

void ConversationItemModel::updateRow(int rowIndex, Row replacement) {
  Row &before = rows_[static_cast<std::size_t>(rowIndex)];
  QList<int> roles;
  if (before.card.threadId != replacement.card.threadId)
    roles.push_back(ThreadIdRole);
  if (before.card.turnId != replacement.card.turnId)
    roles.push_back(TurnIdRole);
  if (before.card.itemId != replacement.card.itemId)
    roles.push_back(ItemIdRole);
  if (before.card.kind != replacement.card.kind) {
    roles.push_back(CardKindRole);
    roles.push_back(Qt::DisplayRole);
    roles.push_back(Qt::AccessibleTextRole);
  }
  if (before.card.target != replacement.card.target)
    roles.push_back(TargetIdentityRole);
  if (before.sectionKey != replacement.sectionKey)
    roles.push_back(TurnSectionRole);
  if (before.turnRoot != replacement.turnRoot)
    roles.push_back(TurnRootRole);
  if (before.nested != replacement.nested)
    roles.push_back(NestedCardRole);
  if (before.firstInTurn != replacement.firstInTurn)
    roles.push_back(FirstInTurnRole);
  if (before.lastInTurn != replacement.lastInTurn)
    roles.push_back(LastInTurnRole);
  if (before.presented != replacement.presented)
    roles.push_back(PresentedRole);
  if (before.activeTurn != replacement.activeTurn)
    roles.push_back(ActiveTurnRole);
  if (before.card.payload != replacement.card.payload ||
      before.card.activeWork != replacement.card.activeWork) {
    roles.push_back(PresentationRole);
    roles.push_back(Qt::AccessibleTextRole);
  }
  before = std::move(replacement);
  if (roles.empty())
    roles.push_back(PresentationRole);
  emit dataChanged(index(rowIndex), index(rowIndex), roles);
  incrementProperty("modelDataChangeCount");
}

} // namespace codexui::codex::middle
