// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H

#include "codex/middle/MiddleTypes.h"

#include <QAbstractListModel>

#include <memory>
#include <optional>
#include <span>
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
    Rejected,
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

    bool operator==(const Row &) const = default;
  };

  struct StructuralDeltaPlan {
    struct Operation {
      enum class Kind { Remove, Place, ReplaceRoot };

      Kind kind = Kind::Place;
      std::size_t deltaIndex = 0;
      std::size_t removalIndex = 0;
      int source = -1;
      int destination = -1;
      bool tailAppend = false;
      bool changed = true;
      std::string oldStableKey;
      std::string stableKey;
      std::string oldSection;
      std::string section;
    };

    std::vector<Operation> operations;
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
  [[nodiscard]] StructuralChangeResult
  replaceConversation(ConversationSnapshot snapshot);
  // Reconciles authoritative ordering with exact inserts, moves, removals, and
  // row updates. History uses the same mechanism with a different view anchor.
  [[nodiscard]] StructuralChangeResult reconcile(ConversationSnapshot snapshot);
  [[nodiscard]] CardUpdateResult
  cardUpdateResult(const VisibleCardData &card) const;
  [[nodiscard]] CardUpdateResult updateCard(VisibleCardData card);
  [[nodiscard]] std::optional<StructuralDeltaPlan>
  planStructuralDelta(std::span<const ConversationRowChange> rows,
                      std::span<const nodegraph::NodeRef> removals) const;
  [[nodiscard]] std::optional<StructuralDeltaPlan>
  applyStructuralDelta(std::vector<ConversationRowChange> &rows,
                       std::span<const nodegraph::NodeRef> removals);
  [[nodiscard]] bool setProviderHasMore(bool providerHasMore) noexcept;
  [[nodiscard]] bool setVisibility(Visibility visibility);
  [[nodiscard]] bool isPresented(const VisibleCardData &card) const noexcept;

  [[nodiscard]] const Row *row(int row) const noexcept;
  [[nodiscard]] const VisibleCardData *card(int row) const noexcept;
  [[nodiscard]] QModelIndex indexForStableKey(const std::string &key) const;
  [[nodiscard]] QModelIndex
  indexForTarget(const nodegraph::NodeRef &target) const;
  [[nodiscard]] const std::string &threadId() const noexcept {
    return threadId_;
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
  [[nodiscard]] SectionStructure
  sectionStructure(const std::string &sectionKey) const;
  void refreshSectionStructure(const std::string &sectionKey,
                               const SectionStructure &before,
                               std::span<const std::string> changedKeys);
  void commitStructuralDelta(StructuralDeltaPlan &plan,
                             std::vector<ConversationRowChange> &rows,
                             std::span<const nodegraph::NodeRef> removals);
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
  static std::unique_ptr<RowNode> mergeRows(std::unique_ptr<RowNode> left,
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
  std::uint64_t priorityState_ = 0x9e3779b97f4a7c15ULL;
  std::string threadId_;
  bool hasMore_ = false;
  Visibility visibility_;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H
