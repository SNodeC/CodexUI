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
          bool complete =
              appendAccessibleLine(lines, payload.explanation, false);
          for (const PlanStepData &step : payload.steps) {
            if (!complete ||
                !appendAccessibleLine(lines, displayStatus(step.status),
                                      true) ||
                !appendAccessibleLine(lines, ": ", false) ||
                !appendAccessibleLine(lines, step.text, false)) {
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

bool structurallyCompatible(const CardKey &beforeKey, CardKind beforeKind,
                            const nodegraph::NodeRef &beforeTarget,
                            const VisibleCardData &after) noexcept {
  return beforeKey == after.key &&
         ((beforeKind == after.kind && beforeTarget == after.target) ||
          (beforeKind == CardKind::LocalPrompt &&
           after.kind == CardKind::UserMessage));
}

bool validSectionPlacement(bool turnRoot, bool nested,
                           std::size_t remainingSectionRows, bool remainingRoot,
                           bool previousInSection,
                           bool nextInSection) noexcept {
  if (turnRoot && nested)
    return false;
  if (remainingSectionRows != 0 && !previousInSection && !nextInSection)
    return false;
  if (turnRoot)
    return !remainingRoot;
  return true;
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

ConversationItemModel::StructuralChangeResult
ConversationItemModel::replaceConversation(ConversationSnapshot snapshot) {
  const std::string nextThreadId = snapshot.threadId;
  const bool nextHasMore = snapshot.hasMore;
  std::vector<Row> desired = flatten(std::move(snapshot));
  if (!rowsAreUnique(desired))
    return StructuralChangeResult::Rejected;

  bool identical = nextThreadId == threadId_ && nextHasMore == hasMore_ &&
                   desired.size() == nodeCount(rows_);
  for (std::size_t position = 0; identical && position < desired.size();
       ++position) {
    const RowNode *current = nodeAt(position);
    identical = current && current->value == desired[position];
  }
  if (identical)
    return StructuralChangeResult::Unchanged;

  beginResetModel();
  clearRows();
  rows_ = buildRows(desired, 0, desired.size());
  threadId_ = nextThreadId;
  hasMore_ = nextHasMore;
  rebuildIndexes();
  endResetModel();
  incrementProperty("modelResetCount");
  incrementProperty("modelReplacementCount");
  return StructuralChangeResult::Changed;
}

ConversationItemModel::StructuralChangeResult
ConversationItemModel::reconcile(ConversationSnapshot snapshot) {
  if (snapshot.threadId != threadId_)
    return replaceConversation(std::move(snapshot));
  const bool nextHasMore = snapshot.hasMore;
  std::vector<Row> desired = flatten(std::move(snapshot));
  if (!rowsAreUnique(desired))
    return StructuralChangeResult::Rejected;

  const bool chromeChanged = nextHasMore != hasMore_;
  hasMore_ = nextHasMore;
  const std::size_t currentCount = nodeCount(rows_);
  if (currentCount != 0 && desired.size() >= currentCount) {
    struct InsertionRun {
      std::size_t first = 0;
      std::size_t count = 0;
    };
    std::vector<InsertionRun> insertions;
    std::vector<RowNode *> retainedNodes;
    std::vector<std::size_t> retainedPositions;
    retainedNodes.reserve(currentCount);
    retainedPositions.reserve(currentCount);
    RowNode *current = nodeAt(0);
    std::size_t desiredPosition = 0;
    bool insertionOnly = true;
    while (current && insertionOnly) {
      const std::size_t insertionFirst = desiredPosition;
      while (desiredPosition < desired.size() &&
             desired[desiredPosition].stableKey != current->value.stableKey)
        ++desiredPosition;
      if (desiredPosition == desired.size()) {
        insertionOnly = false;
        break;
      }
      if (desiredPosition != insertionFirst)
        insertions.push_back(
            {insertionFirst, desiredPosition - insertionFirst});
      const Row &replacement = desired[desiredPosition];
      insertionOnly =
          structurallyCompatible(current->value.card.key,
                                 current->value.card.kind,
                                 current->value.card.target, replacement.card);
      retainedNodes.push_back(current);
      retainedPositions.push_back(desiredPosition);
      ++desiredPosition;
      current = nextNode(current);
    }
    if (insertionOnly && retainedPositions.size() == currentCount) {
      if (desiredPosition != desired.size())
        insertions.push_back(
            {desiredPosition, desired.size() - desiredPosition});
      for (const InsertionRun &insertion : insertions) {
        const int firstRow = static_cast<int>(insertion.first);
        beginInsertRows({}, firstRow,
                        firstRow + static_cast<int>(insertion.count) - 1);
        auto [before, after] =
            splitRows(std::move(rows_), insertion.first);
        rows_ = mergeRows(
            mergeRows(std::move(before),
                      buildRows(desired, insertion.first,
                                insertion.first + insertion.count)),
            std::move(after));
        rebuildIndexes();
        endInsertRows();
        incrementProperty("modelInsertCount");
      }
      bool rowsUpdated = false;
      for (std::size_t index = 0; index < currentCount; ++index) {
        const std::size_t position = retainedPositions[index];
        RowNode *retained = retainedNodes[index];
        if (retained->value == desired[position])
          continue;
        updateRow(retained, static_cast<int>(position),
                  std::move(desired[position]));
        rowsUpdated = true;
      }
      return !insertions.empty() || rowsUpdated || chromeChanged
                 ? StructuralChangeResult::Changed
                 : StructuralChangeResult::Unchanged;
    }
  }
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
        insertRow(position + inserted, std::move(desired[position + inserted]));
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

    beginMoveRows({}, *source, *source, {}, static_cast<int>(position));
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
  return changed ? StructuralChangeResult::Changed
                 : StructuralChangeResult::Unchanged;
}

ConversationItemModel::CardUpdateResult
ConversationItemModel::cardUpdateResult(const VisibleCardData &card) const {
  const std::string key = stableKey(card.key);
  const auto found = stableRows_.find(key);
  if (found == stableRows_.end())
    return CardUpdateResult::Missing;
  const std::optional<int> modelRow = rowOf(found->second);
  if (!modelRow)
    return CardUpdateResult::Missing;
  const Row &current = found->second->value;
  if (current.card.key != card.key || current.card.kind != card.kind ||
      current.card.target != card.target)
    return CardUpdateResult::Incompatible;
  if (current.card == card)
    return CardUpdateResult::Unchanged;
  return CardUpdateResult::Changed;
}

ConversationItemModel::CardUpdateResult
ConversationItemModel::updateCard(VisibleCardData card) {
  const CardUpdateResult admission = cardUpdateResult(card);
  if (admission != CardUpdateResult::Changed)
    return admission;

  const std::string key = stableKey(card.key);
  const auto found = stableRows_.find(key);
  const std::optional<int> modelRow = rowOf(found->second);
  Row &current = found->second->value;
  const bool wasPresented = current.presented;
  current.card = std::move(card);
  current.presented = isPresented(current.card);
  QList<int> roles{PresentationRole, Qt::AccessibleTextRole};
  if (wasPresented != current.presented)
    roles.push_back(PresentedRole);
  emit dataChanged(index(*modelRow), index(*modelRow), roles);
  incrementProperty("modelDataChangeCount");
  return CardUpdateResult::Changed;
}

std::optional<ConversationItemModel::StructuralDeltaPlan>
ConversationItemModel::planStructuralDelta(
    std::span<const ConversationRowChange> rows,
    std::span<const nodegraph::NodeRef> removals) const {
  const auto reject = [&](const char *) -> std::optional<StructuralDeltaPlan> {
    return std::nullopt;
  };
  struct VirtualRow {
    CardKey key;
    CardKind kind = CardKind::GenericActivity;
    nodegraph::NodeRef target;
    std::string stableKey;
    std::string sectionKey;
    bool turnRoot = false;
    bool nested = false;
  };
  struct VirtualSequence {
    struct Segment {
      std::size_t originalFirst = 0;
      std::size_t originalCount = 0;
      std::optional<VirtualRow> value;

      [[nodiscard]] std::size_t size() const noexcept {
        return value ? 1 : originalCount;
      }
    };

    explicit VirtualSequence(const ConversationItemModel &source)
        : source(source) {
      if (source.rowCount() != 0)
        segments.push_back(
            {0, static_cast<std::size_t>(source.rowCount()), std::nullopt});
    }

    [[nodiscard]] static VirtualRow project(const Row &row) {
      return {row.card.key,   row.card.kind, row.card.target, row.stableKey,
              row.sectionKey, row.turnRoot,  row.nested};
    }

    [[nodiscard]] int size() const noexcept {
      std::size_t result = 0;
      for (const Segment &segment : segments)
        result += segment.size();
      return static_cast<int>(result);
    }

    [[nodiscard]] std::optional<VirtualRow> at(int position) const {
      if (position < 0)
        return std::nullopt;
      std::size_t offset = static_cast<std::size_t>(position);
      for (const Segment &segment : segments) {
        if (offset >= segment.size()) {
          offset -= segment.size();
          continue;
        }
        if (segment.value)
          return *segment.value;
        const Row *row =
            source.row(static_cast<int>(segment.originalFirst + offset));
        return row ? std::optional<VirtualRow>(project(*row)) : std::nullopt;
      }
      return std::nullopt;
    }

    [[nodiscard]] std::optional<int>
    locateStable(const std::string &key) const {
      int position = 0;
      for (const Segment &segment : segments) {
        if (segment.value && segment.value->stableKey == key)
          return position;
        position += static_cast<int>(segment.size());
      }
      const QModelIndex original = source.indexForStableKey(key);
      return original.isValid() ? locateOriginal(original.row()) : std::nullopt;
    }

    [[nodiscard]] std::optional<int>
    locateTarget(const nodegraph::NodeRef &target) const {
      if (!target)
        return std::nullopt;
      int position = 0;
      for (const Segment &segment : segments) {
        if (segment.value && segment.value->target == target)
          return position;
        position += static_cast<int>(segment.size());
      }
      const QModelIndex original = source.indexForTarget(target);
      return original.isValid() ? locateOriginal(original.row()) : std::nullopt;
    }

    [[nodiscard]] std::optional<VirtualRow> erase(int position) {
      const std::optional<VirtualRow> removed = at(position);
      if (!removed)
        return std::nullopt;
      const std::size_t first = splitAt(static_cast<std::size_t>(position));
      const std::size_t after = splitAt(static_cast<std::size_t>(position + 1));
      segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(first),
                     segments.begin() + static_cast<std::ptrdiff_t>(after));
      return removed;
    }

    void insert(int position, VirtualRow row) {
      const std::size_t at = splitAt(static_cast<std::size_t>(position));
      segments.insert(segments.begin() + static_cast<std::ptrdiff_t>(at),
                      Segment{0, 0, std::move(row)});
    }

  private:
    [[nodiscard]] std::optional<int> locateOriginal(int original) const {
      int position = 0;
      for (const Segment &segment : segments) {
        if (!segment.value &&
            original >= static_cast<int>(segment.originalFirst) &&
            original <
                static_cast<int>(segment.originalFirst + segment.originalCount))
          return position + original - static_cast<int>(segment.originalFirst);
        position += static_cast<int>(segment.size());
      }
      return std::nullopt;
    }

    std::size_t splitAt(std::size_t position) {
      std::size_t offset = 0;
      for (std::size_t index = 0; index < segments.size(); ++index) {
        Segment &segment = segments[index];
        if (position == offset)
          return index;
        if (position < offset + segment.size()) {
          const std::size_t leftCount = position - offset;
          Segment right{segment.originalFirst + leftCount,
                        segment.originalCount - leftCount, std::nullopt};
          segment.originalCount = leftCount;
          segments.insert(segments.begin() +
                              static_cast<std::ptrdiff_t>(index + 1),
                          std::move(right));
          return index + 1;
        }
        offset += segment.size();
      }
      return segments.size();
    }

    const ConversationItemModel &source;
    std::vector<Segment> segments;
  } sequence(*this);

  struct VirtualSection {
    std::size_t count = 0;
    bool hasRoot = false;
    std::string firstKey;
  };
  std::unordered_map<std::string, VirtualSection> sections;
  const auto section = [&](const std::string &key) -> VirtualSection & {
    if (const auto found = sections.find(key); found != sections.end())
      return found->second;
    VirtualSection value;
    if (const auto found = sectionRows_.find(key);
        found != sectionRows_.end()) {
      value.count = found->second.count;
      value.hasRoot = found->second.root != nullptr;
      if (found->second.first)
        value.firstKey = found->second.first->value.stableKey;
    }
    return sections.emplace(key, value).first->second;
  };
  const auto removeVirtual = [&](int position) {
    const std::optional<VirtualRow> removed = sequence.erase(position);
    if (!removed)
      return false;
    VirtualSection &owner = section(removed->sectionKey);
    if (owner.count == 0)
      return false;
    --owner.count;
    if (removed->turnRoot)
      owner.hasRoot = false;
    if (owner.firstKey == removed->stableKey) {
      const std::optional<VirtualRow> next = sequence.at(position);
      owner.firstKey = next && next->sectionKey == removed->sectionKey
                           ? next->stableKey
                           : std::string{};
    }
    return true;
  };
  const auto insertVirtual = [&](int position, VirtualRow row) {
    VirtualSection &owner = section(row.sectionKey);
    const std::optional<int> first = sequence.locateStable(owner.firstKey);
    if (owner.count == 0 || (first && position <= *first))
      owner.firstKey = row.stableKey;
    ++owner.count;
    owner.hasRoot = owner.hasRoot || row.turnRoot;
    sequence.insert(position, std::move(row));
  };

  std::unordered_map<std::string, std::size_t> desiredRows;
  std::unordered_set<const nodegraph::Node *> desiredTargets;
  std::unordered_set<const nodegraph::Node *> removedTargets;
  desiredRows.reserve(rows.size());
  desiredTargets.reserve(rows.size());
  removedTargets.reserve(removals.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const ConversationRowChange &change = rows[index];
    const VisibleCardData &card = change.placement.card;
    const std::string key = stableKey(card.key);
    if (key.empty() || card.threadId != threadId_ ||
        change.placement.sectionKey.empty() ||
        !desiredRows.emplace(key, index).second ||
        (card.target && !desiredTargets.insert(card.target.get()).second))
      return std::nullopt;
  }
  for (const nodegraph::NodeRef &target : removals) {
    if (!target || !removedTargets.insert(target.get()).second ||
        desiredTargets.contains(target.get()))
      return std::nullopt;
  }

  StructuralDeltaPlan result;
  std::vector<bool> settled(rows.size(), false);
  std::vector<bool> replacedRoots(removals.size(), false);
  for (std::size_t removalIndex = 0; removalIndex < removals.size();
       ++removalIndex) {
    const std::optional<int> position =
        sequence.locateTarget(removals[removalIndex]);
    const std::optional<VirtualRow> current =
        position ? sequence.at(*position) : std::nullopt;
    if (!current || !current->turnRoot ||
        section(current->sectionKey).count < 2)
      continue;

    std::optional<std::size_t> replacementIndex;
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const ConversationRowPlacement &placement = rows[index].placement;
      const std::string key = stableKey(placement.card.key);
      if (settled[index] || !placement.turnRoot || placement.nested ||
          placement.sectionKey != current->sectionKey ||
          key == current->stableKey || sequence.locateStable(key) ||
          sequence.locateTarget(placement.card.target))
        continue;
      if (replacementIndex) {
        replacementIndex.reset();
        break;
      }
      replacementIndex = index;
    }
    if (!replacementIndex)
      continue;

    const ConversationRowPlacement &placement =
        rows[*replacementIndex].placement;
    VirtualRow replacement{placement.card.key,
                           placement.card.kind,
                           placement.card.target,
                           stableKey(placement.card.key),
                           placement.sectionKey,
                           true,
                           false};
    if (!removeVirtual(*position))
      return std::nullopt;
    insertVirtual(*position, std::move(replacement));

    StructuralDeltaPlan::Operation operation;
    operation.kind = StructuralDeltaPlan::Operation::Kind::ReplaceRoot;
    operation.deltaIndex = *replacementIndex;
    operation.removalIndex = removalIndex;
    operation.source = *position;
    operation.destination = *position;
    operation.oldStableKey = current->stableKey;
    operation.stableKey = stableKey(placement.card.key);
    operation.oldSection = current->sectionKey;
    operation.section = placement.sectionKey;
    result.operations.push_back(std::move(operation));
    settled[*replacementIndex] = true;
    replacedRoots[removalIndex] = true;
  }

  for (std::size_t index = 0; index < removals.size(); ++index) {
    if (replacedRoots[index])
      continue;
    const std::optional<int> position = sequence.locateTarget(removals[index]);
    const std::optional<VirtualRow> current =
        position ? sequence.at(*position) : std::nullopt;
    if (!current)
      continue;
    const auto desired = desiredRows.find(current->stableKey);
    const bool replaced =
        desired != desiredRows.end() &&
        rows[desired->second].placement.card.target != removals[index];
    if (replaced)
      continue;
    if (!removeVirtual(*position))
      return std::nullopt;
    StructuralDeltaPlan::Operation operation;
    operation.kind = StructuralDeltaPlan::Operation::Kind::Remove;
    operation.removalIndex = index;
    operation.source = *position;
    operation.oldStableKey = current->stableKey;
    operation.oldSection = current->sectionKey;
    result.operations.push_back(std::move(operation));
  }

  for (std::size_t attempt = 0; attempt < 2 * rows.size(); ++attempt) {
    const std::size_t index =
        attempt < rows.size() ? attempt : 2 * rows.size() - attempt - 1;
    if (settled[index])
      continue;
    const ConversationRowChange &change = rows[index];
    const VisibleCardData &card = change.placement.card;
    const std::string key = stableKey(card.key);
    const std::optional<int> stable = sequence.locateStable(key);
    const std::optional<int> target = sequence.locateTarget(card.target);
    if (stable && target && *stable != *target)
      return reject("stable-target-disagree");
    if (!stable && target)
      return reject("target-without-stable");

    const int source = stable ? *stable : -1;
    const std::optional<VirtualRow> current =
        source >= 0 ? sequence.at(source) : std::nullopt;
    if (current && (!current->target ||
                    !structurallyCompatible(current->key, current->kind,
                                            current->target, card)))
      return reject("incompatible-current");
    const bool materializesLocalPrompt =
        current && current->kind == CardKind::LocalPrompt &&
        card.kind == CardKind::UserMessage;
    const bool rehomesLocalPrompt =
        current && current->kind == CardKind::LocalPrompt &&
        card.kind == CardKind::LocalPrompt && current->target == card.target;
    if (current && !materializesLocalPrompt && !rehomesLocalPrompt &&
        (current->turnRoot != change.placement.turnRoot ||
         (current->turnRoot &&
          current->sectionKey != change.placement.sectionKey)))
      return reject("section-change");

    const auto pending = [&](const std::optional<CardKey> &neighbor) {
      if (!neighbor)
        return false;
      const auto found = desiredRows.find(stableKey(*neighbor));
      return found != desiredRows.end() && !settled[found->second];
    };
    const std::optional<int> previous =
        change.previousCardKey && !pending(change.previousCardKey)
            ? sequence.locateStable(stableKey(*change.previousCardKey))
            : std::nullopt;
    const std::optional<int> next =
        change.nextCardKey && !pending(change.nextCardKey)
            ? sequence.locateStable(stableKey(*change.nextCardKey))
            : std::nullopt;
    const bool connected = source >= 0 || previous || next ||
                           !change.nextCardKey || sequence.size() == 0;
    if (!connected)
      continue;

    int destination = -1;
    if (!change.previousCardKey) {
      destination = 0;
    } else if (!change.nextCardKey) {
      destination = sequence.size() - static_cast<int>(source >= 0);
    } else if (previous && *previous != source) {
      destination = *previous + 1;
      if (source >= 0 && source < destination)
        --destination;
    } else if (next && *next != source) {
      destination = *next;
      if (source >= 0 && source < destination)
        --destination;
    }
    if (destination < 0) {
      if (sequence.size() == 0)
        destination = 0;
      else
        continue;
    }

    if (source >= 0 && !removeVirtual(source))
      return reject("invalid-destination");
    if (destination < 0 || destination > sequence.size() ||
        sequence.locateStable(key) || sequence.locateTarget(card.target))
      return reject("invalid-current-placement");

    VirtualRow replacement{card.key,
                           card.kind,
                           card.target,
                           key,
                           change.placement.sectionKey,
                           change.placement.turnRoot,
                           change.placement.nested};
    const std::optional<VirtualRow> previousRow = sequence.at(destination - 1);
    const std::optional<VirtualRow> nextRow = sequence.at(destination);
    const VirtualSection &owner = section(replacement.sectionKey);
    const bool previousInSection =
        previousRow && previousRow->sectionKey == replacement.sectionKey;
    const bool nextInSection =
        nextRow && nextRow->sectionKey == replacement.sectionKey;
    const bool exactTail =
        source < 0 && !change.nextCardKey && destination == sequence.size();
    const bool valid =
        validSectionPlacement(replacement.turnRoot, replacement.nested,
                              owner.count, owner.hasRoot, previousInSection,
                              nextInSection);
    if (!valid)
      return std::nullopt;
    if (current && current->turnRoot && source != destination)
      return std::nullopt;
    insertVirtual(destination, std::move(replacement));
    StructuralDeltaPlan::Operation operation;
    operation.kind = StructuralDeltaPlan::Operation::Kind::Place;
    operation.deltaIndex = index;
    operation.source = source;
    operation.destination = destination;
    operation.tailAppend = exactTail;
    operation.oldStableKey = current ? current->stableKey : std::string{};
    operation.stableKey = key;
    operation.oldSection = current ? current->sectionKey : std::string{};
    operation.section = change.placement.sectionKey;
    result.operations.push_back(std::move(operation));
    settled[index] = true;
  }

  for (std::size_t index = 0; index < rows.size(); ++index) {
    if (!settled[index]) {
      return reject("unsettled");
    }
  }

  for (const StructuralDeltaPlan::Operation &operation : result.operations) {
    if (operation.kind == StructuralDeltaPlan::Operation::Kind::Remove)
      continue;
    const ConversationRowChange &change = rows[operation.deltaIndex];
    const std::optional<int> position =
        sequence.locateStable(stableKey(change.placement.card.key));
    if (!position)
      return reject("missing-after-plan");
    if ((!change.previousCardKey && *position != 0) ||
        (!change.nextCardKey && *position != sequence.size() - 1))
      return reject("edge-mismatch");
    const auto adjacent = [&](const std::optional<CardKey> &neighbor,
                              int offset) {
      if (!neighbor)
        return true;
      const std::string neighborKey = stableKey(*neighbor);
      const std::optional<int> neighborPosition =
          sequence.locateStable(neighborKey);
      return neighborPosition &&
             *neighborPosition + offset == *position;
    };
    if (!adjacent(change.previousCardKey, 1) ||
        !adjacent(change.nextCardKey, -1)) {
      return reject("adjacency-mismatch");
    }
  }

  return result;
}

std::optional<ConversationItemModel::StructuralDeltaPlan>
ConversationItemModel::applyStructuralDelta(
    std::vector<ConversationRowChange> &rows,
    std::span<const nodegraph::NodeRef> removals) {
  std::optional<StructuralDeltaPlan> plan = planStructuralDelta(rows, removals);
  if (!plan)
    return std::nullopt;
  commitStructuralDelta(*plan, rows, removals);
  return plan;
}

void ConversationItemModel::commitStructuralDelta(
    StructuralDeltaPlan &plan, std::vector<ConversationRowChange> &rows,
    std::span<const nodegraph::NodeRef> removals) {
  struct SectionChange {
    SectionStructure before;
    std::vector<std::string> changedKeys;
  };
  std::unordered_map<std::string, SectionChange> changedSections;
  const auto touch = [this, &changedSections](const std::string &section,
                                              const std::string &key) {
    if (section.empty())
      return;
    auto [found, inserted] = changedSections.try_emplace(section);
    if (inserted)
      found->second.before = sectionStructure(section);
    if (!key.empty() && std::ranges::find(found->second.changedKeys, key) ==
                            found->second.changedKeys.end())
      found->second.changedKeys.push_back(key);
  };

  for (StructuralDeltaPlan::Operation &operation : plan.operations) {
    if (operation.kind == StructuralDeltaPlan::Operation::Kind::ReplaceRoot) {
      const QModelIndex sourceIndex =
          indexForTarget(removals[operation.removalIndex]);
      Q_ASSERT(sourceIndex.isValid() && sourceIndex.row() == operation.source);
      const Row *current = row(sourceIndex.row());
      Q_ASSERT(current && current->turnRoot &&
               current->sectionKey == operation.oldSection);
      touch(operation.oldSection, operation.oldStableKey);
      touch(operation.section, operation.stableKey);

      Row replacement =
          rowFromPlacement(std::move(rows[operation.deltaIndex].placement));
      const bool finalFirstInTurn = current->firstInTurn;
      replacement.lastInTurn = current->lastInTurn;
      const bool finalActive = replacement.activeTurn;
      replacement.turnRoot = false;
      replacement.nested = true;
      replacement.firstInTurn = false;
      replacement.activeTurn = false;

      const int insertedRow = sourceIndex.row() + 1;
      beginInsertRows({}, insertedRow, insertedRow);
      insertRow(static_cast<std::size_t>(insertedRow), std::move(replacement));
      endInsertRows();
      incrementProperty("modelInsertCount");
      incrementProperty("modelExactInsertCount");

      beginRemoveRows({}, sourceIndex.row(), sourceIndex.row());
      (void)takeRow(static_cast<std::size_t>(sourceIndex.row()));
      RowNode *promoted = nodeAt(static_cast<std::size_t>(sourceIndex.row()));
      Q_ASSERT(promoted && promoted->value.stableKey == operation.stableKey);
      removeSectionIdentity(promoted);
      promoted->value.turnRoot = true;
      promoted->value.nested = false;
      promoted->value.firstInTurn = finalFirstInTurn;
      promoted->value.activeTurn = finalActive;
      addSectionIdentity(promoted);
      endRemoveRows();
      incrementProperty("modelRemoveCount");
      incrementProperty("modelExactRemoveCount");
      emit dataChanged(
          index(sourceIndex.row()), index(sourceIndex.row()),
          {TurnRootRole, NestedCardRole, FirstInTurnRole, ActiveTurnRole});
      incrementProperty("modelDataChangeCount");
      continue;
    }

    if (operation.kind == StructuralDeltaPlan::Operation::Kind::Remove) {
      const QModelIndex sourceIndex =
          indexForTarget(removals[operation.removalIndex]);
      Q_ASSERT(sourceIndex.isValid() && sourceIndex.row() == operation.source);
      touch(operation.oldSection, operation.oldStableKey);
      beginRemoveRows({}, sourceIndex.row(), sourceIndex.row());
      (void)takeRow(static_cast<std::size_t>(sourceIndex.row()));
      endRemoveRows();
      incrementProperty("modelRemoveCount");
      incrementProperty("modelExactRemoveCount");
      continue;
    }

    Row replacement =
        rowFromPlacement(std::move(rows[operation.deltaIndex].placement));
    if (operation.source < 0) {
      touch(operation.section, operation.stableKey);
      const Row *previous = row(operation.destination - 1);
      const Row *next = row(operation.destination);
      replacement.firstInTurn =
          !previous || previous->sectionKey != replacement.sectionKey;
      replacement.lastInTurn =
          !next || next->sectionKey != replacement.sectionKey;
      beginInsertRows({}, operation.destination, operation.destination);
      insertRow(static_cast<std::size_t>(operation.destination),
                std::move(replacement));
      endInsertRows();
      incrementProperty("modelInsertCount");
      incrementProperty(operation.tailAppend ? "modelTailAppendCount"
                                             : "modelExactInsertCount");
      continue;
    }

    const QModelIndex sourceIndex = indexForStableKey(operation.oldStableKey);
    Q_ASSERT(sourceIndex.isValid() && sourceIndex.row() == operation.source);
    const Row *current = row(sourceIndex.row());
    Q_ASSERT(current);
    replacement.firstInTurn = current->firstInTurn;
    replacement.lastInTurn = current->lastInTurn;
    const bool movedRows = operation.source != operation.destination;
    const bool presentationChanged = *current != replacement;
    operation.changed = movedRows || presentationChanged;
    if (!operation.changed)
      continue;
    touch(operation.oldSection, operation.oldStableKey);
    touch(operation.section, operation.stableKey);
    if (movedRows) {
      const int destinationChild = operation.destination > operation.source
                                       ? operation.destination + 1
                                       : operation.destination;
      beginMoveRows({}, operation.source, operation.source, {},
                    destinationChild);
      std::unique_ptr<RowNode> moved =
          takeRow(static_cast<std::size_t>(operation.source));
      insertRow(static_cast<std::size_t>(operation.destination),
                std::move(moved->value));
      endMoveRows();
      incrementProperty("modelMoveCount");
    }

    Row &moved = nodeAt(static_cast<std::size_t>(operation.destination))->value;
    replacement.firstInTurn = moved.firstInTurn;
    replacement.lastInTurn = moved.lastInTurn;
    if (presentationChanged || movedRows)
      updateRow(operation.destination, std::move(replacement));
    incrementProperty(movedRows ? "modelExactMoveCount"
                                : "modelExactPlacementUpdateCount");
  }

  for (auto &[section, change] : changedSections)
    refreshSectionStructure(section, change.before, change.changedKeys);
}

bool ConversationItemModel::setProviderHasMore(bool providerHasMore) noexcept {
  if (hasMore_ == providerHasMore)
    return false;
  hasMore_ = providerHasMore;
  return true;
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
        emit dataChanged(index(first), index(position - 1), {PresentedRole});
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
    const auto representedRoot =
        root ? std::ranges::find_if(section.cards, [&](const auto &card) {
          return stableKey(card.key) == *root;
        }) : section.cards.end();
    const std::optional<std::size_t> rootPosition =
        representedRoot == section.cards.end()
            ? std::nullopt
            : std::optional<std::size_t>{static_cast<std::size_t>(
                  std::distance(section.cards.begin(), representedRoot))};
    for (std::size_t position = 0; position < section.cards.size();
         ++position) {
      VisibleCardData card = std::move(section.cards[position]);
      const std::string key = stableKey(card.key);
      const bool turnRoot = rootPosition && position == *rootPosition;
      result.push_back(Row{std::move(card), key, section.key, turnRoot,
                           isNestedTurnCard(rootPosition, position),
                           position == 0,
                           position + 1 == section.cards.size(), false, false});
      Row &row = result.back();
      row.presented = isPresented(row.card);
      row.activeTurn = turnRoot && snapshot.activeTurnId &&
                       row.card.turnId == *snapshot.activeTurnId;
    }
  }
  return result;
}

bool ConversationItemModel::rowsAreUnique(const std::vector<Row> &rows) const {
  std::unordered_set<std::string> stableKeys;
  std::unordered_set<const nodegraph::Node *> targets;
  stableKeys.reserve(rows.size());
  targets.reserve(rows.size());
  for (const Row &row : rows) {
    if (row.stableKey.empty() || !stableKeys.insert(row.stableKey).second)
      return false;
    if (row.card.target && !targets.insert(row.card.target.get()).second)
      return false;
  }
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
  return result;
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
    std::span<const std::string> changedKeys) {
  if (sectionKey.empty())
    return;
  const auto found = sectionRows_.find(sectionKey);
  if (found == sectionRows_.end())
    return;

  const SectionStructure after = sectionStructure(sectionKey);
  const std::optional<int> rootPosition =
      found->second.root ? rowOf(found->second.root) : std::nullopt;
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
  for (const std::string &key : changedKeys)
    retain(key);

  if (before.root.empty() != after.root.empty()) {
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
    const bool nested =
        rootPosition && isNestedTurnCard(
                            static_cast<std::size_t>(*rootPosition),
                            static_cast<std::size_t>(*rowPosition));
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
    addSectionIdentity(current);
    current = current->right.get();
  }
  incrementProperty("modelIndexRebuildCount");
}

std::unique_ptr<ConversationItemModel::RowNode>
ConversationItemModel::buildRows(std::vector<Row> &rows, std::size_t first,
                                 std::size_t last, std::uint64_t depth) {
  if (first >= last)
    return {};
  const std::size_t middle = first + (last - first) / 2;
  auto node = std::make_unique<RowNode>(
      std::move(rows[middle]),
      std::numeric_limits<std::uint64_t>::max() - depth);
  node->left = buildRows(rows, first, middle, depth + 1);
  node->right = buildRows(rows, middle + 1, last, depth + 1);
  updateNode(node.get());
  node->parent = nullptr;
  return node;
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
  removed->parent = nullptr;
  return std::move(removed);
}

void ConversationItemModel::clearRows() noexcept {
  stableRows_.clear();
  targetRows_.clear();
  sectionRows_.clear();
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
    section.first = next && next->value.sectionKey == node->value.sectionKey
                        ? next
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
    if (!replacement || replacement->value.sectionKey != node->value.sectionKey)
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
  updateRow(node, rowIndex, std::move(replacement));
}

void ConversationItemModel::updateRow(RowNode *node, int rowIndex,
                                      Row replacement) {
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
      before.card.status != replacement.card.status) {
    roles.push_back(PresentationRole);
    roles.push_back(Qt::AccessibleTextRole);
  }
  const std::string oldStableKey = before.stableKey;
  const std::string oldSectionKey = before.sectionKey;
  const bool oldTurnRoot = before.turnRoot;
  const nodegraph::Node *oldTarget =
      before.card.target ? before.card.target.get() : nullptr;
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
    const auto old = targetRows_.find(oldTarget);
    if (old != targetRows_.end() && old->second == node)
      targetRows_.erase(old);
    if (newTarget)
      targetRows_.insert_or_assign(newTarget, node);
  }
  if (roles.empty())
    return;
  emit dataChanged(index(rowIndex), index(rowIndex), roles);
  incrementProperty("modelDataChangeCount");
}

} // namespace codexui::codex::middle
