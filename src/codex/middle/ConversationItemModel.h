// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H

#include "codex/middle/MiddleTypes.h"

#include <QAbstractListModel>

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

    bool operator==(const Row &) const = default;
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

  std::vector<Row> rows_;
  std::unordered_map<std::string, int> stableRows_;
  std::unordered_map<const nodegraph::Node *, int> targetRows_;
  std::string threadId_;
  std::size_t hiddenAuthoritativeItemCount_ = 0;
  bool hasMore_ = false;
  Visibility visibility_;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONITEMMODEL_H
