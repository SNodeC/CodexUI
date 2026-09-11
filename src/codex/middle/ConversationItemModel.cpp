// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationItemModel.h"

#include <QString>
#include <algorithm>
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

constexpr qsizetype MaximumAccessibleCharacters = 8192;

bool appendAccessibleLine(QString &destination, std::string_view value,
                          bool prependNewline) {
  qsizetype remaining = MaximumAccessibleCharacters - destination.size();
  if (prependNewline) {
    if (remaining <= 0)
      return false;
    destination += QLatin1Char('\n');
    --remaining;
  }
  if (remaining <= 0)
    return value.empty();

  const std::size_t byteCount =
      std::min(value.size(), static_cast<std::size_t>(remaining) * 4);
  QString rendered =
      QString::fromUtf8(value.data(), static_cast<qsizetype>(byteCount));
  if (rendered.size() > remaining)
    rendered.truncate(remaining);
  destination += rendered;
  return byteCount == value.size();
}

void markAccessibleTextTruncated(QString &value) {
  value.truncate(MaximumAccessibleCharacters);
  value += QStringLiteral("…");
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
          QString paths;
          bool first = true;
          for (const FileChangeData &change : payload.changes) {
            if (!appendAccessibleLine(paths, change.path, !first)) {
              markAccessibleTextTruncated(paths);
              break;
            }
            first = false;
          }
          return paths;
        }
        if constexpr (std::is_same_v<Payload, ImageGenerationData>)
          return QStringLiteral("%1\n%2").arg(
              boundedAccessibleText(payload.revisedPrompt),
              boundedAccessibleText(payload.path));
        if constexpr (std::is_same_v<Payload, PlanData>) {
          QString lines;
          bool complete = appendAccessibleLine(lines, payload.explanation,
                                               false);
          for (const PlanStepData &step : payload.steps) {
            if (!complete ||
                !appendAccessibleLine(lines, step.text, true)) {
              complete = false;
              break;
            }
          }
          if (complete && !payload.legacyText.empty())
            complete = appendAccessibleLine(lines, payload.legacyText, true);
          if (!complete)
            markAccessibleTextTruncated(lines);
          return lines;
        }
        if constexpr (std::is_same_v<Payload, GenericActivityData>)
          return boundedAccessibleText(payload.displayDetail);
        if constexpr (std::is_same_v<Payload, LocalPromptData>)
          return boundedAccessibleText(payload.prompt);
        return {};
      },
      card.payload);
  if (detail.size() > MaximumAccessibleCharacters) {
    markAccessibleTextTruncated(detail);
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

ConversationItemModel::~ConversationItemModel() = default;

int ConversationItemModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : static_cast<int>(nodeCount(rows_));
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

bool ConversationItemModel::replaceConversation(ConversationSnapshot snapshot) {
  const std::string nextThreadId = snapshot.threadId;
  const std::size_t nextHiddenCount = snapshot.hiddenAuthoritativeItemCount;
  const bool nextHasMore = snapshot.hasMore;
  std::vector<Row> desired = flatten(std::move(snapshot));
  if (!rowsAreUnique(desired))
    return false;

  bool identical = nextThreadId == threadId_ &&
                   nextHiddenCount == hiddenAuthoritativeItemCount_ &&
                   nextHasMore == hasMore_ &&
                   desired.size() == nodeCount(rows_);
  for (std::size_t position = 0; identical && position < desired.size();
       ++position) {
    const RowNode *current = nodeAt(position);
    identical = current && current->value == desired[position];
  }
  if (identical)
    return false;

  beginResetModel();
  clearRows();
  for (Row &row : desired)
    insertRow(nodeCount(rows_), std::move(row));
  threadId_ = nextThreadId;
  hiddenAuthoritativeItemCount_ = nextHiddenCount;
  hasMore_ = nextHasMore;
  rebuildIndexes();
  endResetModel();
  incrementProperty("modelResetCount");
  incrementProperty("modelReplacementCount");
  return true;
}

bool ConversationItemModel::prependHistoryPage(ConversationSnapshot snapshot) {
  if (snapshot.threadId != threadId_)
    return false;
  const std::size_t nextHiddenCount = snapshot.hiddenAuthoritativeItemCount;
  const bool nextHasMore = snapshot.hasMore;
  std::vector<Row> desired = flatten(std::move(snapshot));
  if (!rowsAreUnique(desired) || desired.size() < nodeCount(rows_))
    return false;

  std::size_t retained = 0;
  for (const Row &candidate : desired) {
    const RowNode *current = nodeAt(retained);
    if (current && candidate.stableKey == current->value.stableKey) {
      ++retained;
      continue;
    }
    if (stableRows_.contains(candidate.stableKey))
      return false;
  }
  if (retained != nodeCount(rows_))
    return false;

  bool changed = nextHiddenCount != hiddenAuthoritativeItemCount_ ||
                 nextHasMore != hasMore_;
  std::size_t desiredPosition = 0;
  std::size_t modelPosition = 0;
  while (desiredPosition < desired.size()) {
    const RowNode *current = nodeAt(modelPosition);
    if (current && desired[desiredPosition].stableKey ==
                       current->value.stableKey) {
      if (current->value != desired[desiredPosition]) {
        updateRow(static_cast<int>(modelPosition),
                  std::move(desired[desiredPosition]));
        changed = true;
      }
      ++desiredPosition;
      ++modelPosition;
      continue;
    }

    const std::size_t firstDesired = desiredPosition;
    while (
        desiredPosition < desired.size() &&
        !([&] {
          const RowNode *retainedRow = nodeAt(modelPosition);
          return retainedRow && desired[desiredPosition].stableKey ==
                                    retainedRow->value.stableKey;
        })())
      ++desiredPosition;
    const std::size_t count = desiredPosition - firstDesired;
    const int firstRow = static_cast<int>(modelPosition);
    const int lastRow = static_cast<int>(modelPosition + count - 1);
    beginInsertRows({}, firstRow, lastRow);
    for (std::size_t inserted = firstDesired; inserted < desiredPosition;
         ++inserted)
      insertRow(modelPosition++, std::move(desired[inserted]));
    endInsertRows();
    incrementProperty("modelInsertCount");
    changed = true;
  }

  hiddenAuthoritativeItemCount_ = nextHiddenCount;
  hasMore_ = nextHasMore;
  if (changed)
    incrementProperty("modelHistoryPrependCount");
  return changed;
}

bool ConversationItemModel::reconcile(ConversationSnapshot snapshot) {
  if (snapshot.threadId != threadId_)
    return replaceConversation(std::move(snapshot));
  const std::size_t nextHiddenCount = snapshot.hiddenAuthoritativeItemCount;
  const bool nextHasMore = snapshot.hasMore;
  std::vector<Row> desired = flatten(std::move(snapshot));
  if (!rowsAreUnique(desired))
    return false;

  const bool chromeChanged = nextHiddenCount != hiddenAuthoritativeItemCount_ ||
                             nextHasMore != hasMore_;
  hiddenAuthoritativeItemCount_ = nextHiddenCount;
  hasMore_ = nextHasMore;
  std::unordered_set<std::string> desiredKeys;
  desiredKeys.reserve(desired.size());
  for (const Row &row : desired)
    desiredKeys.insert(row.stableKey);

  bool changed = chromeChanged;
  for (std::size_t offset = nodeCount(rows_); offset > 0;) {
    std::size_t last = offset - 1;
    const RowNode *lastNode = nodeAt(last);
    if (lastNode && desiredKeys.contains(lastNode->value.stableKey)) {
      offset = last;
      continue;
    }
    std::size_t first = last;
    while (first > 0 &&
           !desiredKeys.contains(nodeAt(first - 1)->value.stableKey))
      --first;
    beginRemoveRows({}, static_cast<int>(first), static_cast<int>(last));
    for (std::size_t position = last + 1; position > first; --position)
      (void)takeRow(position - 1);
    endRemoveRows();
    incrementProperty("modelRemoveCount");
    changed = true;
    offset = first;
  }

  std::vector<bool> inserted(desired.size(), false);
  for (std::size_t position = 0; position < desired.size(); ++position) {
    const RowNode *current = nodeAt(position);
    if (current && current->value.stableKey == desired[position].stableKey)
      continue;
    const auto found = stableRows_.find(desired[position].stableKey);
    const std::optional<int> source =
        found == stableRows_.end() ? std::nullopt : rowOf(found->second);
    if (!source) {
      std::size_t count = 1;
      while (position + count < desired.size() &&
             !stableRows_.contains(desired[position + count].stableKey))
        ++count;
      beginInsertRows({}, static_cast<int>(position),
                      static_cast<int>(position + count - 1));
      for (std::size_t inserted = 0; inserted < count; ++inserted)
        insertRow(position + inserted,
                  std::move(desired[position + inserted]));
      endInsertRows();
      std::fill(inserted.begin() + static_cast<std::ptrdiff_t>(position),
                inserted.begin() +
                    static_cast<std::ptrdiff_t>(position + count),
                true);
      incrementProperty("modelInsertCount");
      changed = true;
      position += count - 1;
      continue;
    }

    beginMoveRows({}, *source, *source, {},
                  static_cast<int>(position));
    std::unique_ptr<RowNode> moved = takeRow(static_cast<std::size_t>(*source));
    insertRow(position, std::move(moved->value));
    endMoveRows();
    incrementProperty("modelMoveCount");
    changed = true;
  }

  for (std::size_t position = 0; position < desired.size(); ++position) {
    if (inserted[position])
      continue;
    const RowNode *current = nodeAt(position);
    if (current && current->value == desired[position])
      continue;
    updateRow(static_cast<int>(position), std::move(desired[position]));
    changed = true;
  }
  return changed;
}

ConversationItemModel::CardUpdateResult
ConversationItemModel::updateCard(VisibleCardData card) {
  const std::string key = stableKey(card.key);
  const auto found = stableRows_.find(key);
  if (found == stableRows_.end())
    return CardUpdateResult::Missing;
  const std::optional<int> modelRow = rowOf(found->second);
  if (!modelRow)
    return CardUpdateResult::Missing;
  Row &current = found->second->value;
  if (!compatible(current.card, card))
    return CardUpdateResult::Incompatible;
  if (current.card == card)
    return CardUpdateResult::Unchanged;

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
  return CardUpdateResult::Changed;
}

ConversationItemModel::StructuralChangeResult
ConversationItemModel::insertCard(int rowIndex,
                                  ConversationRowPlacement placement) {
  Row candidate = rowFromPlacement(std::move(placement));
  if (candidate.card.threadId != threadId_ || candidate.stableKey.empty() ||
      !sectionPlacementIsValid(rowIndex, candidate))
    return StructuralChangeResult::Invalid;
  if (stableRows_.contains(candidate.stableKey) ||
      (candidate.card.target &&
       targetRows_.contains(candidate.card.target.get())))
    return StructuralChangeResult::Duplicate;
  const SectionStructure before = sectionStructure(candidate.sectionKey);
  const std::string changedKey = candidate.stableKey;

  const bool previousInSection =
      rowIndex > 0 &&
      row(rowIndex - 1)->sectionKey == candidate.sectionKey;
  const bool nextInSection =
      rowIndex < rowCount() &&
      row(rowIndex)->sectionKey == candidate.sectionKey;
  candidate.firstInTurn = !previousInSection;
  candidate.lastInTurn = !nextInSection;

  beginInsertRows({}, rowIndex, rowIndex);
  RowNode *inserted = insertRow(static_cast<std::size_t>(rowIndex),
                                std::move(candidate));
  endInsertRows();
  incrementProperty("modelInsertCount");
  incrementProperty("modelExactInsertCount");
  refreshSectionStructure(inserted->value.sectionKey, before, changedKey);
  return StructuralChangeResult::Changed;
}

ConversationItemModel::StructuralChangeResult
ConversationItemModel::removeTarget(const nodegraph::NodeRef &target) {
  const QModelIndex targetIndex = indexForTarget(target);
  if (!targetIndex.isValid())
    return StructuralChangeResult::Missing;
  const int rowIndex = targetIndex.row();
  const std::string sectionKey = row(rowIndex)->sectionKey;
  const std::string changedKey = row(rowIndex)->stableKey;
  const SectionStructure before = sectionStructure(sectionKey);

  beginRemoveRows({}, rowIndex, rowIndex);
  (void)takeRow(static_cast<std::size_t>(rowIndex));
  endRemoveRows();
  incrementProperty("modelRemoveCount");
  incrementProperty("modelExactRemoveCount");
  refreshSectionStructure(sectionKey, before, changedKey);
  return StructuralChangeResult::Changed;
}

ConversationItemModel::StructuralChangeResult
ConversationItemModel::moveTarget(const nodegraph::NodeRef &target,
                                  int destinationRow,
                                  ConversationRowPlacement placement) {
  const QModelIndex targetIndex = indexForTarget(target);
  if (!targetIndex.isValid())
    return StructuralChangeResult::Missing;
  if (destinationRow < 0 || destinationRow >= rowCount() ||
      placement.card.threadId != threadId_ || placement.card.target != target ||
      placement.sectionKey.empty())
    return StructuralChangeResult::Invalid;

  const int sourceRow = targetIndex.row();
  const Row &current = targetRows_.at(target.get())->value;
  const std::string changedKey = current.stableKey;
  const SectionStructure oldBefore = sectionStructure(current.sectionKey);
  Row replacement = rowFromPlacement(std::move(placement));
  if (replacement.stableKey != current.stableKey ||
      !compatible(current.card, replacement.card) ||
      (replacement.turnRoot && replacement.nested))
    return StructuralChangeResult::Invalid;

  const auto rowAfterRemoval = [&](int position) -> const RowNode * {
    if (position < 0 || position >= rowCount() - 1)
      return nullptr;
    const int original = position < sourceRow ? position : position + 1;
    return nodeAt(static_cast<std::size_t>(original));
  };
  const RowNode *previous = rowAfterRemoval(destinationRow - 1);
  const RowNode *next = rowAfterRemoval(destinationRow);
  const bool previousInSection =
      previous && previous->value.sectionKey == replacement.sectionKey;
  const bool nextInSection =
      next && next->value.sectionKey == replacement.sectionKey;
  const auto section = sectionRows_.find(replacement.sectionKey);
  const std::size_t remainingSectionRows =
      section == sectionRows_.end()
          ? 0
          : section->second.count -
                static_cast<std::size_t>(current.sectionKey ==
                                         replacement.sectionKey);
  const bool remainingRoot =
      section != sectionRows_.end() && section->second.root &&
      section->second.root != targetRows_.at(target.get());
  if ((remainingSectionRows != 0 && !previousInSection && !nextInSection) ||
      (replacement.turnRoot && (remainingRoot || previousInSection)) ||
      (!replacement.turnRoot && nextInSection && next->value.turnRoot))
    return StructuralChangeResult::Invalid;

  const std::string oldSection = current.sectionKey;
  const SectionStructure newBefore =
      replacement.sectionKey == oldSection
          ? oldBefore
          : sectionStructure(replacement.sectionKey);
  const bool movedRows = sourceRow != destinationRow;
  if (movedRows) {
    const int destinationChild =
        destinationRow > sourceRow ? destinationRow + 1 : destinationRow;
    beginMoveRows({}, sourceRow, sourceRow, {}, destinationChild);
    std::unique_ptr<RowNode> moved =
        takeRow(static_cast<std::size_t>(sourceRow));
    insertRow(static_cast<std::size_t>(destinationRow),
              std::move(moved->value));
    endMoveRows();
    incrementProperty("modelMoveCount");
  }

  Row &moved = nodeAt(static_cast<std::size_t>(destinationRow))->value;
  replacement.firstInTurn = moved.firstInTurn;
  replacement.lastInTurn = moved.lastInTurn;
  const bool presentationChanged = moved != replacement;
  if (presentationChanged)
    updateRow(destinationRow, std::move(replacement));
  if (sourceRow == destinationRow && !presentationChanged)
    return StructuralChangeResult::Unchanged;

  refreshSectionStructure(oldSection, oldBefore, changedKey);
  const std::string newSection = row(destinationRow)->sectionKey;
  if (newSection != oldSection)
    refreshSectionStructure(newSection, newBefore, changedKey);
  incrementProperty(movedRows ? "modelExactMoveCount"
                              : "modelExactPlacementUpdateCount");
  return StructuralChangeResult::Changed;
}

bool ConversationItemModel::appendTail(ConversationTailCard tail) {
  if (tail.card.threadId != threadId_ || tail.sectionKey.empty())
    return false;
  const std::string key = stableKey(tail.card.key);
  if (key.empty() || stableRows_.contains(key))
    return false;

  const bool startsSection =
      rowCount() == 0 || row(rowCount() - 1)->sectionKey != tail.sectionKey;
  if ((!startsSection && tail.turnRoot) || (startsSection && tail.nested))
    return false;

  if (rowCount() != 0 && !startsSection) {
    Row &previous = nodeAt(static_cast<std::size_t>(rowCount() - 1))->value;
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
  beginInsertRows({}, insertedRow, insertedRow);
  insertRow(static_cast<std::size_t>(insertedRow), std::move(row));
  endInsertRows();
  incrementProperty("modelInsertCount");
  incrementProperty("modelTailAppendCount");
  return true;
}

ConversationItemModel::HistoryTrim
ConversationItemModel::trimHistoryTo(std::size_t activityLimit) {
  HistoryTrim result;
  if (historyActivityCount_ <= activityLimit || rowCount() == 0)
    return result;

  Row &first = nodeAt(0)->value;
  result.sectionKey = first.sectionKey;
  if (first.historyActivity && first.turnRoot && rowCount() > 1 &&
      row(1)->sectionKey == first.sectionKey) {
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
  if (!first.historyActivity &&
      (!first.turnRoot || rowCount() == 1 ||
       row(1)->sectionKey != first.sectionKey))
    return result;

  int removeCount = 1;
  int removeRow = 0;
  if (!first.historyActivity && first.turnRoot && rowCount() > 1 &&
      row(1)->sectionKey == first.sectionKey) {
    result.sectionKey = first.sectionKey;
    if (rowCount() > 2 && row(2)->sectionKey == first.sectionKey) {
      // Retain the pinned owner at logical row zero while dropping the oldest
      // nested activity. The order-statistic tree removes that one row without
      // changing the identity nodes of the retained suffix.
      removeRow = 1;
    } else {
      removeCount = 2;
    }
  }

  for (int offset = 0; offset < removeCount; ++offset) {
    const Row &removed = *row(removeRow + offset);
    result.removedStableKeys.push_back(removed.stableKey);
    if (removed.historyActivity)
      ++result.hiddenIncrement;
  }
  result.row = removeRow;
  result.count = removeCount;

  beginRemoveRows({}, removeRow, removeRow + removeCount - 1);
  for (int offset = 0; offset < removeCount; ++offset)
    (void)takeRow(static_cast<std::size_t>(removeRow));
  endRemoveRows();
  incrementProperty("modelRemoveCount");
  incrementProperty("modelBoundedFrontTrimCount");
  return result;
}

bool ConversationItemModel::setActiveTurn(int rowIndex, bool active) {
  RowNode *node = rowIndex >= 0 ? nodeAt(static_cast<std::size_t>(rowIndex))
                                : nullptr;
  Row *value = node ? &node->value : nullptr;
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
  for (int position = 0; position < rowCount(); ++position) {
    Row &current = nodeAt(static_cast<std::size_t>(position))->value;
    const bool presented = isPresented(current.card);
    if (presented == current.presented) {
      if (first >= 0) {
        emit dataChanged(index(first), index(position - 1),
                         {PresentedRole});
        incrementProperty("modelDataChangeCount");
        first = -1;
      }
      continue;
    }
    current.presented = presented;
    changed = true;
    if (first < 0)
      first = position;
  }
  if (first < 0)
    return changed;
  emit dataChanged(index(first), index(rowCount() - 1), {PresentedRole});
  incrementProperty("modelDataChangeCount");
  return true;
}

const ConversationItemModel::Row *
ConversationItemModel::row(int rowIndex) const noexcept {
  const RowNode *node =
      rowIndex >= 0 ? nodeAt(static_cast<std::size_t>(rowIndex)) : nullptr;
  return node ? &node->value : nullptr;
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
  const std::optional<int> row = rowOf(found->second);
  return row ? index(*row) : QModelIndex{};
}

QModelIndex
ConversationItemModel::indexForTarget(const nodegraph::NodeRef &target) const {
  if (!target)
    return {};
  const auto found = targetRows_.find(target.get());
  if (found == targetRows_.end())
    return {};
  const std::optional<int> modelRow = rowOf(found->second);
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

bool ConversationItemModel::rowsAreUnique(const std::vector<Row> &rows) const {
  std::unordered_set<std::string> unique;
  unique.reserve(rows.size());
  for (const Row &row : rows)
    if (row.stableKey.empty() || !unique.insert(row.stableKey).second)
      return false;
  return true;
}

ConversationItemModel::Row ConversationItemModel::rowFromPlacement(
    ConversationRowPlacement placement) const {
  Row result;
  result.card = std::move(placement.card);
  result.stableKey = stableKey(result.card.key);
  result.sectionKey = std::move(placement.sectionKey);
  result.turnRoot = placement.turnRoot;
  result.nested = placement.nested;
  result.presented = isPresented(result.card);
  result.activeTurn = placement.turnRoot && placement.activeTurn;
  result.historyActivity = placement.historyActivity;
  return result;
}

bool ConversationItemModel::sectionPlacementIsValid(
    int rowIndex, const Row &candidate) const {
  if (rowIndex < 0 || rowIndex > rowCount() || candidate.sectionKey.empty() ||
      (candidate.turnRoot && candidate.nested))
    return false;

  const RowNode *previous =
      rowIndex > 0 ? nodeAt(static_cast<std::size_t>(rowIndex - 1)) : nullptr;
  const RowNode *next = rowIndex < rowCount()
                            ? nodeAt(static_cast<std::size_t>(rowIndex))
                            : nullptr;
  const bool previousInSection =
      previous && previous->value.sectionKey == candidate.sectionKey;
  const bool nextInSection =
      next && next->value.sectionKey == candidate.sectionKey;
  const auto found = sectionRows_.find(candidate.sectionKey);
  if (found != sectionRows_.end() &&
      !previousInSection && !nextInSection)
    return false;
  if (candidate.turnRoot)
    return (found == sectionRows_.end() || !found->second.root) &&
           !previousInSection;
  return !nextInSection || !next->value.turnRoot;
}

ConversationItemModel::SectionStructure
ConversationItemModel::sectionStructure(const std::string &sectionKey) const {
  SectionStructure result;
  const auto found = sectionRows_.find(sectionKey);
  if (found == sectionRows_.end())
    return result;
  if (found->second.first)
    result.first = found->second.first->value.stableKey;
  if (found->second.last)
    result.last = found->second.last->value.stableKey;
  if (found->second.root)
    result.root = found->second.root->value.stableKey;
  return result;
}

void ConversationItemModel::refreshSectionStructure(
    const std::string &sectionKey, const SectionStructure &before,
    const std::string &changedKey) {
  if (sectionKey.empty())
    return;
  const auto found = sectionRows_.find(sectionKey);
  if (found == sectionRows_.end())
    return;

  const SectionStructure after = sectionStructure(sectionKey);
  std::vector<RowNode *> affected;
  affected.reserve(7);
  const auto retain = [this, &affected](const std::string &key) {
    if (key.empty())
      return;
    const auto found = stableRows_.find(key);
    if (found != stableRows_.end() &&
        std::ranges::find(affected, found->second) == affected.end())
      affected.push_back(found->second);
  };
  retain(before.first);
  retain(before.last);
  retain(before.root);
  retain(after.first);
  retain(after.last);
  retain(after.root);
  retain(changedKey);

  if (before.root != after.root) {
    affected.clear();
    RowNode *node = found->second.first;
    while (node && node->value.sectionKey == sectionKey) {
      affected.push_back(node);
      node = nextNode(node);
    }
  }

  for (RowNode *node : affected) {
    incrementProperty("modelSectionStructureRowsTouched");
    const std::optional<int> rowPosition = rowOf(node);
    if (!rowPosition || node->value.sectionKey != sectionKey)
      continue;
    Row &current = node->value;
    QList<int> roles;
    const bool firstInTurn = node == found->second.first;
    const bool lastInTurn = node == found->second.last;
    const bool nested = found->second.root && node != found->second.root;
    if (current.firstInTurn != firstInTurn) {
      current.firstInTurn = firstInTurn;
      roles.push_back(FirstInTurnRole);
    }
    if (current.lastInTurn != lastInTurn) {
      current.lastInTurn = lastInTurn;
      roles.push_back(LastInTurnRole);
    }
    if (current.nested != nested) {
      current.nested = nested;
      roles.push_back(NestedCardRole);
    }
    if (current.activeTurn && !current.turnRoot) {
      current.activeTurn = false;
      roles.push_back(ActiveTurnRole);
    }
    if (roles.empty())
      continue;
    emit dataChanged(index(*rowPosition), index(*rowPosition), roles);
    incrementProperty("modelDataChangeCount");
  }
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
  sectionRows_.clear();
  stableRows_.reserve(nodeCount(rows_));
  targetRows_.reserve(nodeCount(rows_));
  historyActivityCount_ = 0;
  std::vector<RowNode *> pending;
  RowNode *current = rows_.get();
  while (current || !pending.empty()) {
    while (current) {
      pending.push_back(current);
      current = current->left.get();
    }
    current = pending.back();
    pending.pop_back();
    stableRows_.emplace(current->value.stableKey, current);
    if (current->value.card.target)
      targetRows_.emplace(current->value.card.target.get(), current);
    if (current->value.historyActivity)
      ++historyActivityCount_;
    addSectionIdentity(current);
    current = current->right.get();
  }
  incrementProperty("modelIndexRebuildCount");
}

ConversationItemModel::RowNode *
ConversationItemModel::nodeAt(std::size_t row) const noexcept {
  RowNode *current = rows_.get();
  while (current) {
    const std::size_t leftCount = nodeCount(current->left);
    if (row < leftCount) {
      current = current->left.get();
      continue;
    }
    if (row == leftCount)
      return current;
    row -= leftCount + 1;
    current = current->right.get();
  }
  return nullptr;
}

std::optional<int>
ConversationItemModel::rowOf(const RowNode *node) const noexcept {
  if (!node)
    return std::nullopt;
  std::size_t result = nodeCount(node->left);
  while (node->parent) {
    if (node == node->parent->right.get())
      result += nodeCount(node->parent->left) + 1;
    node = node->parent;
  }
  if (node != rows_.get() ||
      result > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return std::nullopt;
  return static_cast<int>(result);
}

ConversationItemModel::RowNode *
ConversationItemModel::insertRow(std::size_t row, Row value) {
  auto inserted = std::make_unique<RowNode>(std::move(value), nextPriority());
  RowNode *result = inserted.get();
  auto [left, right] = splitRows(std::move(rows_), row);
  rows_ = mergeRows(mergeRows(std::move(left), std::move(inserted)),
                    std::move(right));
  stableRows_.insert_or_assign(result->value.stableKey, result);
  if (result->value.card.target)
    targetRows_.insert_or_assign(result->value.card.target.get(), result);
  if (result->value.historyActivity)
    ++historyActivityCount_;
  addSectionIdentity(result);
  return result;
}

std::unique_ptr<ConversationItemModel::RowNode>
ConversationItemModel::takeRow(std::size_t row) {
  RowNode *candidate = nodeAt(row);
  if (!candidate)
    return {};
  removeSectionIdentity(candidate);
  auto [left, suffix] = splitRows(std::move(rows_), row);
  auto [removed, right] = splitRows(std::move(suffix), 1);
  rows_ = mergeRows(std::move(left), std::move(right));
  if (!removed)
    return {};
  eraseRowIdentity(removed->value);
  if (removed->value.historyActivity)
    --historyActivityCount_;
  removed->parent = nullptr;
  return removed;
}

void ConversationItemModel::clearRows() noexcept {
  stableRows_.clear();
  targetRows_.clear();
  sectionRows_.clear();
  historyActivityCount_ = 0;
  rows_.reset();
}

std::uint64_t ConversationItemModel::nextPriority() noexcept {
  priorityState_ ^= priorityState_ >> 12;
  priorityState_ ^= priorityState_ << 25;
  priorityState_ ^= priorityState_ >> 27;
  return priorityState_ * 0x2545f4914f6cdd1dULL;
}

std::size_t ConversationItemModel::nodeCount(
    const std::unique_ptr<RowNode> &node) noexcept {
  return node ? node->count : 0;
}

void ConversationItemModel::updateNode(RowNode *node) noexcept {
  if (!node)
    return;
  node->count = nodeCount(node->left) + 1 + nodeCount(node->right);
  if (node->left)
    node->left->parent = node;
  if (node->right)
    node->right->parent = node;
}

std::pair<std::unique_ptr<ConversationItemModel::RowNode>,
          std::unique_ptr<ConversationItemModel::RowNode>>
ConversationItemModel::splitRows(std::unique_ptr<RowNode> root,
                                 std::size_t leftCount) {
  if (!root)
    return {};
  if (nodeCount(root->left) >= leftCount) {
    auto [left, middle] = splitRows(std::move(root->left), leftCount);
    root->left = std::move(middle);
    updateNode(root.get());
    root->parent = nullptr;
    if (left)
      left->parent = nullptr;
    return {std::move(left), std::move(root)};
  }
  const std::size_t remaining = leftCount - nodeCount(root->left) - 1;
  auto [middle, right] = splitRows(std::move(root->right), remaining);
  root->right = std::move(middle);
  updateNode(root.get());
  root->parent = nullptr;
  if (right)
    right->parent = nullptr;
  return {std::move(root), std::move(right)};
}

std::unique_ptr<ConversationItemModel::RowNode>
ConversationItemModel::mergeRows(std::unique_ptr<RowNode> left,
                                 std::unique_ptr<RowNode> right) {
  if (!left) {
    if (right)
      right->parent = nullptr;
    return right;
  }
  if (!right) {
    left->parent = nullptr;
    return left;
  }
  if (left->priority >= right->priority) {
    left->right = mergeRows(std::move(left->right), std::move(right));
    updateNode(left.get());
    left->parent = nullptr;
    return left;
  }
  right->left = mergeRows(std::move(left), std::move(right->left));
  updateNode(right.get());
  right->parent = nullptr;
  return right;
}

ConversationItemModel::RowNode *
ConversationItemModel::previousNode(RowNode *node) noexcept {
  if (!node)
    return nullptr;
  if (node->left) {
    node = node->left.get();
    while (node->right)
      node = node->right.get();
    return node;
  }
  while (node->parent && node == node->parent->left.get())
    node = node->parent;
  return node->parent;
}

ConversationItemModel::RowNode *
ConversationItemModel::nextNode(RowNode *node) noexcept {
  if (!node)
    return nullptr;
  if (node->right) {
    node = node->right.get();
    while (node->left)
      node = node->left.get();
    return node;
  }
  while (node->parent && node == node->parent->right.get())
    node = node->parent;
  return node->parent;
}

void ConversationItemModel::addSectionIdentity(RowNode *node) {
  if (!node || node->value.sectionKey.empty())
    return;
  SectionIndex &section = sectionRows_[node->value.sectionKey];
  const std::optional<int> position = rowOf(node);
  if (!section.member) {
    section.member = node;
    section.first = node;
    section.last = node;
  } else if (position) {
    const std::optional<int> first = rowOf(section.first);
    const std::optional<int> last = rowOf(section.last);
    if (!first || *position < *first)
      section.first = node;
    if (!last || *position > *last)
      section.last = node;
  }
  if (node->value.turnRoot)
    section.root = node;
  ++section.count;
}

void ConversationItemModel::removeSectionIdentity(RowNode *node) {
  if (!node || node->value.sectionKey.empty())
    return;
  const auto found = sectionRows_.find(node->value.sectionKey);
  if (found == sectionRows_.end())
    return;
  SectionIndex &section = found->second;
  RowNode *previous = previousNode(node);
  RowNode *next = nextNode(node);
  if (section.root == node)
    section.root = nullptr;
  if (section.first == node)
    section.first =
        next && next->value.sectionKey == node->value.sectionKey ? next
                                                                 : nullptr;
  if (section.last == node)
    section.last =
        previous && previous->value.sectionKey == node->value.sectionKey
            ? previous
            : nullptr;
  if (section.count > 0)
    --section.count;
  if (section.count == 0) {
    sectionRows_.erase(found);
    return;
  }
  if (section.member == node) {
    RowNode *replacement = previous;
    if (!replacement ||
        replacement->value.sectionKey != node->value.sectionKey)
      replacement = next;
    section.member = replacement;
  }
  if (!section.first && section.member) {
    section.first = section.member;
    while (RowNode *candidate = previousNode(section.first)) {
      if (candidate->value.sectionKey != node->value.sectionKey)
        break;
      section.first = candidate;
    }
  }
  if (!section.last && section.member) {
    section.last = section.member;
    while (RowNode *candidate = nextNode(section.last)) {
      if (candidate->value.sectionKey != node->value.sectionKey)
        break;
      section.last = candidate;
    }
  }
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
  RowNode *node = nodeAt(static_cast<std::size_t>(rowIndex));
  if (!node)
    return;
  Row &before = node->value;
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
  const std::string oldStableKey = before.stableKey;
  const std::string oldSectionKey = before.sectionKey;
  const bool oldTurnRoot = before.turnRoot;
  const nodegraph::Node *oldTarget =
      before.card.target ? before.card.target.get() : nullptr;
  const bool oldHistoryActivity = before.historyActivity;
  if (oldSectionKey != replacement.sectionKey ||
      oldTurnRoot != replacement.turnRoot)
    removeSectionIdentity(node);
  before = std::move(replacement);
  if (oldSectionKey != before.sectionKey || oldTurnRoot != before.turnRoot)
    addSectionIdentity(node);
  if (oldStableKey != before.stableKey) {
    stableRows_.erase(oldStableKey);
    stableRows_.insert_or_assign(before.stableKey, node);
  }
  const nodegraph::Node *newTarget =
      before.card.target ? before.card.target.get() : nullptr;
  if (oldTarget != newTarget) {
    if (oldTarget)
      targetRows_.erase(oldTarget);
    if (newTarget)
      targetRows_.insert_or_assign(newTarget, node);
  }
  if (oldHistoryActivity != before.historyActivity) {
    if (before.historyActivity)
      ++historyActivityCount_;
    else
      --historyActivityCount_;
  }
  if (roles.empty())
    return;
  emit dataChanged(index(rowIndex), index(rowIndex), roles);
  incrementProperty("modelDataChangeCount");
}

} // namespace codexui::codex::middle
