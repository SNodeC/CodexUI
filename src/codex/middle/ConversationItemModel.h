// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H

#include "codex/middle/MiddleTypes.h"

#include <QAbstractListModel>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace codexui::codex::middle {

class ConversationItemModel final : public QAbstractListModel {
  Q_OBJECT

public:
  enum Role {
    StableKeyRole = Qt::UserRole + 1,
    ThreadIdRole,
    TurnIdRole,
    ItemIdRole,
    CardKindRole,
    TargetIdentityRole,
    TurnSectionRole,
    TurnRootRole,
    NestedCardRole,
    FirstInTurnRole,
    LastInTurnRole,
    PresentedRole,
    ActiveTurnRole,
    PresentationRole,
  };

  enum class CardUpdateResult {
    Missing,
    Incompatible,
    Unchanged,
    Changed,
  };

  enum class StructuralChangeResult {
    Missing,
    Invalid,
    Duplicate,
    Unchanged,
    Changed,
  };

  struct Visibility {
    bool showReasoning = true;
    bool showCodexUpdates = true;

    bool operator==(const Visibility &) const = default;
  };

  struct Row {
    VisibleCardData card;
    std::string stableKey;
    std::string sectionKey;
    bool turnRoot = false;
    bool nested = false;
    bool firstInTurn = false;
    bool lastInTurn = false;
    bool presented = true;
    bool activeTurn = false;
    // Local prompts and roots retained only as a turn owner do not consume the
    // bounded authoritative history activity window.
    bool historyActivity = true;

    bool operator==(const Row &) const = default;
  };

  struct HistoryTrim {
    int row = -1;
    int count = 0;
    std::vector<std::string> removedStableKeys;
    bool pinnedRoot = false;
    std::string sectionKey;
    std::size_t hiddenIncrement = 0;
  };

  explicit ConversationItemModel(QObject *parent = nullptr);
  ~ConversationItemModel() override;

  [[nodiscard]] int
  rowCount(const QModelIndex &parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex &index,
                              int role = Qt::DisplayRole) const override;
  [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  // Complete replacement is explicit and is reserved for a different thread
  // or a genuine full rescan where no narrower operation is correct.
  [[nodiscard]] bool replaceConversation(ConversationSnapshot snapshot);
  // A history page is a same-thread superset that retains every existing row
  // in order. It inserts only the missing ranges and updates changed row facts.
  [[nodiscard]] bool prependHistoryPage(ConversationSnapshot snapshot);
  // Preserves the established direct ConversationView API. Shell graph
  // routing never uses this whole-snapshot ordered diff as a delta fallback.
  [[nodiscard]] bool reconcile(ConversationSnapshot snapshot);
  [[nodiscard]] CardUpdateResult updateCard(VisibleCardData card);
  [[nodiscard]] StructuralChangeResult
  insertCard(int row, ConversationRowPlacement placement);
  [[nodiscard]] StructuralChangeResult
  removeTarget(const nodegraph::NodeRef &target);
  // destinationRow is the row's final logical position after the move.
  [[nodiscard]] StructuralChangeResult
  moveTarget(const nodegraph::NodeRef &target, int destinationRow,
             ConversationRowPlacement placement);
  [[nodiscard]] bool appendTail(ConversationTailCard tail);
  [[nodiscard]] HistoryTrim trimHistoryTo(std::size_t activityLimit);
  [[nodiscard]] bool setActiveTurn(int row, bool active);
  void setHistoryChrome(std::size_t hiddenAuthoritativeItemCount,
                        bool providerHasMore);
  [[nodiscard]] bool setVisibility(Visibility visibility);

  [[nodiscard]] const Row *row(int row) const noexcept;
  [[nodiscard]] const VisibleCardData *card(int row) const noexcept;
  [[nodiscard]] QModelIndex indexForStableKey(const std::string &key) const;
  [[nodiscard]] QModelIndex
  indexForTarget(const nodegraph::NodeRef &target) const;
  [[nodiscard]] const std::string &threadId() const noexcept {
    return threadId_;
  }
  [[nodiscard]] std::size_t hiddenAuthoritativeItemCount() const noexcept {
    return hiddenAuthoritativeItemCount_;
  }
  [[nodiscard]] std::size_t historyActivityCount() const noexcept {
    return historyActivityCount_;
  }
  [[nodiscard]] bool hasMore() const noexcept { return hasMore_; }

private:
  struct RowNode {
    explicit RowNode(Row value, std::uint64_t priority)
        : value(std::move(value)), priority(priority) {}

    Row value;
    std::uint64_t priority = 0;
    std::size_t count = 1;
    std::unique_ptr<RowNode> left;
    std::unique_ptr<RowNode> right;
    RowNode *parent = nullptr;
  };

  struct SectionIndex {
    RowNode *member = nullptr;
    RowNode *first = nullptr;
    RowNode *last = nullptr;
    RowNode *root = nullptr;
    std::size_t count = 0;
  };

  struct SectionStructure {
    std::string first;
    std::string last;
    std::string root;
  };

  [[nodiscard]] std::vector<Row> flatten(ConversationSnapshot &&snapshot) const;
  [[nodiscard]] bool rowsAreUnique(const std::vector<Row> &rows) const;
  [[nodiscard]] Row rowFromPlacement(ConversationRowPlacement placement) const;
  [[nodiscard]] bool sectionPlacementIsValid(int row,
                                             const Row &candidate) const;
  [[nodiscard]] SectionStructure
  sectionStructure(const std::string &sectionKey) const;
  void refreshSectionStructure(const std::string &sectionKey,
                               const SectionStructure &before,
                               const std::string &changedKey);
  [[nodiscard]] bool isPresented(const VisibleCardData &card) const noexcept;
  void rebuildIndexes();
  [[nodiscard]] RowNode *nodeAt(std::size_t row) const noexcept;
  [[nodiscard]] std::optional<int> rowOf(const RowNode *node) const noexcept;
  RowNode *insertRow(std::size_t row, Row value);
  [[nodiscard]] std::unique_ptr<RowNode> takeRow(std::size_t row);
  void clearRows() noexcept;
  [[nodiscard]] std::uint64_t nextPriority() noexcept;
  static std::size_t nodeCount(const std::unique_ptr<RowNode> &node) noexcept;
  static void updateNode(RowNode *node) noexcept;
  static std::pair<std::unique_ptr<RowNode>, std::unique_ptr<RowNode>>
  splitRows(std::unique_ptr<RowNode> root, std::size_t leftCount);
  static std::unique_ptr<RowNode>
  mergeRows(std::unique_ptr<RowNode> left,
            std::unique_ptr<RowNode> right);
  static RowNode *previousNode(RowNode *node) noexcept;
  static RowNode *nextNode(RowNode *node) noexcept;
  void addSectionIdentity(RowNode *node);
  void removeSectionIdentity(RowNode *node);
  void incrementProperty(const char *name);
  void updateRow(int row, Row replacement);
  void eraseRowIdentity(const Row &row);

  std::unique_ptr<RowNode> rows_;
  std::unordered_map<std::string, RowNode *> stableRows_;
  std::unordered_map<const nodegraph::Node *, RowNode *> targetRows_;
  std::unordered_map<std::string, SectionIndex> sectionRows_;
  std::size_t historyActivityCount_ = 0;
  std::uint64_t priorityState_ = 0x9e3779b97f4a7c15ULL;
  std::string threadId_;
  std::size_t hiddenAuthoritativeItemCount_ = 0;
  bool hasMore_ = false;
  Visibility visibility_;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H
