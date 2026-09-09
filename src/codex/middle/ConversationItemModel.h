// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H

#include "codex/middle/MiddleTypes.h"

#include <QAbstractListModel>

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
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

  [[nodiscard]] int
  rowCount(const QModelIndex &parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex &index,
                              int role = Qt::DisplayRole) const override;
  [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  // A different thread is one complete authority replacement and therefore a
  // model reset. Same-thread order is reconciled with exact row operations.
  [[nodiscard]] bool reconcile(ConversationSnapshot snapshot);
  [[nodiscard]] CardUpdateResult updateCard(VisibleCardData card);
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
  [[nodiscard]] bool hasMore() const noexcept { return hasMore_; }

private:
  [[nodiscard]] std::vector<Row> flatten(ConversationSnapshot &&snapshot) const;
  [[nodiscard]] bool isPresented(const VisibleCardData &card) const noexcept;
  void rebuildIndexes();
  void incrementProperty(const char *name);
  void updateRow(int row, Row replacement);
  [[nodiscard]] std::optional<int> logicalRow(std::size_t ordinal) const;
  void eraseRowIdentity(const Row &row);

  std::deque<Row> rows_;
  // Absolute ordinals let a bounded front trim avoid rewriting every stable
  // identity in the retained suffix.
  std::unordered_map<std::string, std::size_t> stableRows_;
  std::unordered_map<const nodegraph::Node *, std::size_t> targetRows_;
  std::size_t rowBase_ = 0;
  std::size_t historyActivityCount_ = 0;
  std::string threadId_;
  std::size_t hiddenAuthoritativeItemCount_ = 0;
  bool hasMore_ = false;
  Visibility visibility_;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H
