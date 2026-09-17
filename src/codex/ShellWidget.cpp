// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ShellWidget.h"

#include "codex/ConnectionDialog.h"
#include "codex/FileSelectionDialog.h"
#include "codex/ForkNaming.h"
#include "codex/FrontendSession.h"
#include "codex/NewThreadDialog.h"
#include "codex/NodeGraphJson.h"
#include "codex/PendingRequestDialog.h"
#include "codex/PendingRequestPolicy.h"
#include "codex/UiStatus.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/BrandMark.h"
#include "codex/ui/ExpandingPromptEditor.h"
#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/ui/UiStyle.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace codexui::codex {
namespace {

using nodegraph::boolFromValue;
using nodegraph::exactStringFromValue;
using nodegraph::signedIntegerFromValue;
using nodegraph::valueMember;

constexpr auto DraftThreadId = "draft:new-thread";
constexpr int GraphRetryDelayMilliseconds = 8;
constexpr std::size_t ConversationPresentationRowsPerPass = 8;

bool containsKind(const nodegraph::GraphChanged &change,
                  std::initializer_list<nodegraph::NodeKind> kinds) {
  const auto matches = [kinds](const nodegraph::NodeRef &node) {
    return node && std::ranges::find(kinds, node->id().kind) != kinds.end();
  };
  return std::ranges::any_of(change.affected, matches) ||
         std::ranges::any_of(change.removed, matches);
}

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string utf8(const QString &value) { return value.toUtf8().toStdString(); }

QString lastActivityText(std::int64_t timestamp) {
  const QDateTime activity =
      QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime();
  const QDateTime now = QDateTime::currentDateTime();
  const QString formatted =
      activity.date() == now.date()
          ? activity.toString(QStringLiteral("HH:mm:ss"))
          : activity.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  return QStringLiteral("Last activity: %1").arg(formatted);
}

void collectThreadTitles(const std::vector<ui::ThreadListRow> &rows,
                         std::vector<std::string> &titles) {
  for (const ui::ThreadListRow &row : rows) {
    titles.push_back(row.title);
    collectThreadTitles(row.children, titles);
  }
}

bool fieldChanged(const nodegraph::NodeGraph::ReadAccess &read,
                  const nodegraph::NodeRef &node, std::string_view field,
                  std::uint64_t revision) {
  return node && read.contains(node) &&
         read.fieldChangedRevision(node, field) == revision;
}

struct ThreadPaneRoute {
  bool affected = false;
  bool structural = false;
  std::vector<nodegraph::NodeRef> rows;
};

enum class ThreadSelectionOrigin { Graph, User };

ThreadPaneRoute
threadPaneRoute(const nodegraph::GraphChanged &change,
                const nodegraph::NodeGraph &graph,
                middle::ThreadPane::SortCriterion sortCriterion) {
  if (change.rescanRequired ||
      containsKind(change, {nodegraph::NodeKind::Connection,
                            nodegraph::NodeKind::Interaction}) ||
      std::ranges::any_of(change.removed, [](const auto &node) {
        return node && (node->id().kind == nodegraph::NodeKind::Runtime ||
                        node->id().kind == nodegraph::NodeKind::Connection ||
                        node->id().kind == nodegraph::NodeKind::Thread);
      }))
    return {true, true, {}};
  const auto read = graph.tryRead();
  if (!read)
    return containsKind(change, {nodegraph::NodeKind::Runtime,
                                 nodegraph::NodeKind::Thread})
               ? ThreadPaneRoute{true, true, {}}
               : ThreadPaneRoute{};
  constexpr std::array<std::string_view, 16> Fields{
      "name",
      "localNameOverlay",
      "title",
      "preview",
      "cwd",
      "workspace",
      "status",
      "createdAt",
      "updatedAt",
      "recencyAt",
      "lastActivityAt",
      "localActivityAt",
      "localPromptActivityAt",
      "confirmedLocalPromptActivityAt",
      "hydrationState",
      "archived"};
  ThreadPaneRoute route;
  for (const nodegraph::NodeRef &node : change.affected) {
    if (!node || !read->contains(node))
      continue;
    if (node->id().kind == nodegraph::NodeKind::Runtime) {
      if (read->structureChangedRevision(node) == change.revision)
        return {true, true, {}};
      continue;
    }
    if (node->id().kind == nodegraph::NodeKind::Item) {
      const auto state = read->state(node);
      if (!state ||
          exactStringFromValue(valueMember(*state, "type")) != "localPrompt")
        continue;
      nodegraph::NodeRef owner = read->parent(node);
      while (owner && owner->id().kind != nodegraph::NodeKind::Thread)
        owner = read->parent(owner);
      if (owner && std::ranges::find(route.rows, owner) == route.rows.end()) {
        route.affected = true;
        route.rows.push_back(std::move(owner));
      }
      continue;
    }
    if (node->id().kind != nodegraph::NodeKind::Thread)
      continue;
    const bool presentationChanged =
        read->statusChangedRevision(node) == change.revision ||
        std::ranges::any_of(Fields, [&](std::string_view field) {
          return fieldChanged(*read, node, field, change.revision);
        });
    if (!presentationChanged &&
        read->structureChangedRevision(node) != change.revision)
      continue;
    const bool sortChanged =
        (sortCriterion == middle::ThreadPane::SortCriterion::Alphanumeric &&
         (fieldChanged(*read, node, "name", change.revision) ||
          fieldChanged(*read, node, "localNameOverlay", change.revision) ||
          fieldChanged(*read, node, "title", change.revision) ||
          fieldChanged(*read, node, "preview", change.revision))) ||
        (sortCriterion == middle::ThreadPane::SortCriterion::Created &&
         fieldChanged(*read, node, "createdAt", change.revision)) ||
        (sortCriterion == middle::ThreadPane::SortCriterion::Recency &&
         (fieldChanged(*read, node, "recencyAt", change.revision) ||
          fieldChanged(*read, node, "localPromptActivityAt", change.revision) ||
          fieldChanged(*read, node, "confirmedLocalPromptActivityAt",
                       change.revision)));
    if (read->structureChangedRevision(node) == change.revision ||
        sortChanged || fieldChanged(*read, node, "archived", change.revision))
      return {true, true, {}};
    route.affected = true;
    if (std::ranges::find(route.rows, node) == route.rows.end())
      route.rows.push_back(node);
  }
  return route;
}

std::vector<AttachmentDraft>
graphAttachmentDrafts(const nodegraph::NodeState &state) {
  std::vector<AttachmentDraft> result;
  const nodegraph::Value *attachments = valueMember(state, "attachments");
  const nodegraph::Value::Array *array =
      attachments ? attachments->asArray() : nullptr;
  if (!array)
    return result;
  result.reserve(array->size());
  for (const nodegraph::Value &entry : *array) {
    const nodegraph::Value::Object *object = entry.asObject();
    if (!object)
      continue;
    AttachmentDraft attachment;
    attachment.path = exactStringFromValue(valueMember(*object, "path"));
    attachment.name = exactStringFromValue(valueMember(*object, "displayName"));
    attachment.mimeType =
        exactStringFromValue(valueMember(*object, "mimeType"));
    if (!attachment.path.empty())
      result.emplace_back(std::move(attachment));
  }
  return result;
}

bool shellChromeAffected(const nodegraph::GraphChanged &change,
                         const nodegraph::NodeGraph &graph,
                         const nodegraph::NodeRef &selectedThread) {
  if (change.rescanRequired)
    return true;
  constexpr std::size_t MaximumFilteredNodes = 64;
  if (change.affected.size() + change.removed.size() > MaximumFilteredNodes)
    return true;

  const auto read = graph.tryRead();
  if (!read)
    return true;
  const auto directlyAffectsChrome = [&](const auto &node) {
    if (!node)
      return false;
    if (!read->contains(node))
      return true;
    switch (node->id().kind) {
    case nodegraph::NodeKind::Connection:
      return true;
    case nodegraph::NodeKind::Catalog:
      return node->id().canonical == "model" ||
             node->id().canonical == "permissionProfile";
    case nodegraph::NodeKind::Thread: {
      if (!selectedThread || node != selectedThread)
        return false;
      constexpr std::array<std::string_view, 15> Fields{
          "name",
          "title",
          "cwd",
          "workspace",
          "status",
          "hydrationState",
          "recoveryOnly",
          "lastActivityAt",
          "recencyAt",
          "updatedAt",
          "localActivityAt",
          "localPromptActivityAt",
          "settingsRevision",
          "settings",
          "settingsAcknowledgements"};
      return read->statusChangedRevision(node) == change.revision ||
             read->structureChangedRevision(node) == change.revision ||
             std::ranges::any_of(Fields, [&](std::string_view field) {
               return fieldChanged(*read, node, field, change.revision);
             });
    }
    default:
      return false;
    }
  };
  if (std::ranges::any_of(change.affected, directlyAffectsChrome) ||
      std::ranges::any_of(change.removed, directlyAffectsChrome))
    return true;
  if (!selectedThread)
    return false;

  const auto turnBelongsToSelection = [&](const auto &node) {
    if (!node || node->id().kind != nodegraph::NodeKind::Turn)
      return false;
    if (read->statusChangedRevision(node) != change.revision &&
        read->structureChangedRevision(node) != change.revision)
      return false;
    if (read->parent(node) == selectedThread)
      return true;
    return exactStringFromValue(
               valueMember(*read->state(node), "protocolThreadId")) ==
           selectedThread->id().canonical;
  };
  return std::ranges::any_of(change.affected, turnBelongsToSelection) ||
         std::ranges::any_of(change.removed, turnBelongsToSelection);
}

bool graphUiFallbackAffected(const nodegraph::GraphChanged &change,
                             const nodegraph::NodeGraph &graph) {
  if (change.rescanRequired ||
      containsKind(change, {nodegraph::NodeKind::Notice}))
    return true;
  const auto read = graph.tryRead();
  if (!read)
    return containsKind(change, {nodegraph::NodeKind::Runtime});
  return std::ranges::any_of(change.affected, [&](const auto &node) {
    return node && read->contains(node) &&
           node->id().kind == nodegraph::NodeKind::Runtime &&
           (read->structureChangedRevision(node) == change.revision ||
            fieldChanged(*read, node, "uiSelectionSerial", change.revision));
  });
}

bool connectionSettingsContainSelection(const nlohmann::json &settings,
                                        const nlohmann::json &selection) {
  if (!settings.is_object() || !selection.is_object())
    return false;
  const std::string transport = selection.value("transport", std::string{});
  if (transport.empty() ||
      settings.value("selected", std::string{}) != transport)
    return false;
  const nlohmann::json available =
      settings.value("available", nlohmann::json::array());
  for (const auto &entry : available) {
    if (!entry.is_object() || entry.value("key", std::string{}) != transport)
      continue;
    for (auto field = selection.cbegin(); field != selection.cend(); ++field) {
      if (field.key() != "transport" &&
          (!entry.contains(field.key()) || entry[field.key()] != field.value()))
        return false;
    }
    return true;
  }
  return false;
}

void applyConnectionSelection(nlohmann::json &settings,
                              const nlohmann::json &selection) {
  if (!settings.is_object() || !selection.is_object())
    return;
  const std::string transport = selection.value("transport", std::string{});
  if (transport.empty())
    return;
  settings["selected"] = transport;
  auto available = settings.find("available");
  if (available == settings.end() || !available->is_array())
    return;
  for (auto &entry : *available) {
    if (!entry.is_object() || entry.value("key", std::string{}) != transport)
      continue;
    for (auto field = selection.cbegin(); field != selection.cend(); ++field)
      if (field.key() != "transport")
        entry[field.key()] = field.value();
    return;
  }
}

struct ShellChromeValues final {
  bool connected = false;
  bool retrying = false;
  bool canControl = false;
  bool activeTurn = false;
  bool threadAdmissionReady = true;
  bool conversationReadyForDisplay = true;
  bool hydrationFailed = false;
  bool recoveryOnly = false;
  std::string role;
  std::string providerState;
  std::string selectedTransport;
  std::string workspace;
  std::string title;
  UiStatus status;
  std::optional<std::int64_t> lastActivityAt;
  std::size_t totalPending = 0;
  std::optional<PendingRequestDescriptor> attention;

  bool operator==(const ShellChromeValues &) const = default;
};

struct LocalPendingRequest final {
  PendingRequestSubmission submission;
  std::optional<std::uint64_t> awaitingResponseAfter;
};

using UiStyle::makeLabel;

QLabel *makeStatusLabel(QString value, QString objectName, int maximumWidth,
                        const char *kind = "meta") {
  auto *label = makeLabel({}, kind);
  label->setObjectName(std::move(objectName));
  label->setWordWrap(false);
  label->setMaximumWidth(maximumWidth);
  label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  label->setToolTip(value);
  label->setText(label->fontMetrics().elidedText(value, Qt::ElideMiddle,
                                                 label->maximumWidth()));
  return label;
}

void setStatusLabelText(QLabel *label, QString value) {
  const QString displayed = label->fontMetrics().elidedText(
      value, Qt::ElideMiddle, label->maximumWidth());
  if (label->toolTip() != value)
    label->setToolTip(value);
  if (label->text() != displayed)
    label->setText(displayed);
}

void setStatusTone(QFrame *dot, const QString &tone, QLabel *label = nullptr) {
  for (QWidget *target :
       {static_cast<QWidget *>(dot), static_cast<QWidget *>(label)}) {
    if (!target || target->property("tone").toString() == tone)
      continue;
    target->setProperty("tone", tone);
    target->style()->unpolish(target);
    target->style()->polish(target);
    target->update();
  }
}

QFrame *statusDot() {
  auto *dot = new QFrame;
  dot->setFixedSize(10, 10);
  dot->setProperty("kind", "statusDot");
  return dot;
}

} // namespace

struct ShellWidget::Impl final {
  Impl(ShellWidget *owner, FrontendSession &session)
      : owner(owner), session(session), uiAdapter(session.nodeGraph()),
        alive(std::make_shared<bool>(true)) {
    owner->setProperty(
        "conversationPresentationRowsPerPassBudget",
        static_cast<qulonglong>(ConversationPresentationRowsPerPass));
    buildUi();
    connectUi();
    const auto token = alive;
    session.setGraphChangedHandler(
        [this, token](const nodegraph::GraphChanged &change) {
          if (*token)
            handleGraphChanged(change);
        });
    session.setGraphUiEffectHandler(
        [this, token](const nodegraph::UiEffect &effect) {
          if (*token)
            handleUiEffect(effect);
        });
    bindGraphPanes({});
    observedProviderAuthorityRevision = middleRegion->composer()
                                            .turnSettings()
                                            .context()
                                            .providerAuthorityRevision;
  }

  ~Impl() {
    *alive = false;
    session.setGraphChangedHandler({});
    session.setGraphUiEffectHandler({});
    if (qApp)
      qApp->removeEventFilter(owner);
  }

  void buildUi();
  void connectUi();
  void scheduleGraphBinding(bool immediate = true);
  void runGraphBinding();
  void bindGraphPanes(nodegraph::NodeRef selectedThread);
  [[nodiscard]] bool refreshConversation();
  [[nodiscard]] bool refreshInspector();
  void schedulePaneCommit(bool immediate = false);
  void commitPendingPanes();
  void handleGraphChanged(const nodegraph::GraphChanged &change);
  void handleUiEffect(const nodegraph::UiEffect &effect);
  void reconcileGraphUiFallback();
  void selectGraphThread(nodegraph::NodeRef thread,
                         ThreadSelectionOrigin origin,
                         std::uint64_t graphSerial = 0);
  void render(const ui::PendingRequestsSummary *requests = nullptr);
  void renderStatus(const ShellChromeValues &values,
                    bool updateWorkspace = true);
  void showNotice(QString message, bool error = true);
  [[nodiscard]] bool sendNodeAction(nodegraph::NodeAction action,
                                    QString rejection);
  [[nodiscard]] bool sendRuntimeAction(nodegraph::RuntimeAction action,
                                       QString rejection);
  [[nodiscard]] nodegraph::NodeRef activeTurn() const;
  [[nodiscard]] std::optional<ui::PendingRequestsSummary>
  pendingRequestSummary(bool *busy = nullptr);
  [[nodiscard]] std::optional<ui::InspectorPageSnapshot>
  pendingRequestPage(const ui::InspectorRowRequest &request,
                     bool *busy = nullptr);
  [[nodiscard]] std::optional<PendingRequestDescriptor>
  pendingRequest(const nodegraph::NodeRef &target);
  void applyLocalPending(PendingRequestDescriptor &request);
  [[nodiscard]] bool submitPending(const PendingRequestDescriptor &request,
                                   PendingRequestSubmission submission);
  void hydrateSelectedThreadIfNeeded(nodegraph::NodeRef thread);
  void beginNewThreadDialog();
  [[nodiscard]] std::optional<NewThreadDraft>
  suggestedForkDraft(const nodegraph::NodeRef &thread) const;
  void forkThread(const nodegraph::NodeRef &thread, NewThreadDraft draft,
                  bool includeOptions);
  void beginForkThreadDialog(const nodegraph::NodeRef &thread);
  void renameThreadDialog(const nodegraph::NodeRef &thread);
  void confirmDeleteThread(const nodegraph::NodeRef &thread);
  [[nodiscard]] bool submitPrompt(QString prompt,
                                  std::vector<AttachmentDraft> attachments);
  void chooseAttachments();
  void recoverPrompt(const nodegraph::NodeRef &prompt);
  void presentPending(PendingRequestDescriptor request);
  void reviewPending(const nodegraph::NodeRef &target);
  void acceptPending(const nodegraph::NodeRef &target);
  void rejectPending(const nodegraph::NodeRef &target);
  void
  respondToRenderedPending(void (Impl::*respond)(const nodegraph::NodeRef &));

  ShellWidget *owner = nullptr;
  FrontendSession &session;
  ui::NodeGraphUiAdapter uiAdapter;
  std::shared_ptr<bool> alive;
  std::optional<NewThreadDraft> newThreadDraft;
  std::string creationDraftCorrelation;
  std::uint64_t nextCreationDraftSerial = 1;
  std::uint64_t observedProviderAuthorityRevision = 0;
  std::string selectedGraphThreadId;
  nodegraph::NodeRef boundGraphThread;
  std::map<const nodegraph::Node *, std::pair<nodegraph::NodeRef, QString>>
      retainedRenames;
  std::map<nodegraph::NodeRef, LocalPendingRequest,
           std::owner_less<nodegraph::NodeRef>>
      localPendingRequests;
  std::optional<nlohmann::json> retainedConnectionSelection;
  bool graphPanesBound = false;
  bool graphBindingScheduled = false;
  bool paneCommitScheduled = false;
  bool pendingThreadPane = false;
  std::vector<nodegraph::NodeRef> pendingThreadRows;
  bool pendingConversation = false;
  bool pendingConversationAuthorityReplacement = false;
  std::deque<nodegraph::NodeRef> pendingConversationItems;
  bool pendingInspector = false;
  bool pendingChrome = false;
  bool graphFallbackScheduled = false;
  std::uint64_t lastNoticeSerial = 0;
  std::uint64_t lastSelectionSerial = 0;
  std::optional<ShellChromeValues> renderedChrome;
  std::uint64_t shellRenderCommits = 0;
  std::uint64_t threadPaneRoutes = 0;
  std::uint64_t conversationRoutes = 0;
  std::uint64_t inspectorRoutes = 0;

  middle::MiddleRegionWidget *middleRegion = nullptr;
  QPushButton *restoreSidebarButton = nullptr;
  QPushButton *restoreInspectorButton = nullptr;
  QLabel *workspaceBreadcrumb = nullptr;
  QPushButton *requestButton = nullptr;
  QFrame *connectionStatusDot = nullptr;
  QToolButton *connectionButton = nullptr;
  QAction *connectAction = nullptr;
  QAction *disconnectAction = nullptr;
  QAction *reconnectAction = nullptr;
  QPushButton *controllerButton = nullptr;
  QFrame *globalStatusDot = nullptr;
  QLabel *globalStatusLabel = nullptr;
};

void ShellWidget::Impl::buildUi() {
  owner->setObjectName(QStringLiteral("applicationShell"));
  auto *root = new QVBoxLayout(owner);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto *top = new QFrame;
  top->setObjectName(QStringLiteral("topBar"));
  top->setFixedHeight(64);
  auto *topLayout = new QHBoxLayout(top);
  topLayout->setContentsMargins(18, 0, 18, 0);
  topLayout->setSpacing(12);
  topLayout->addWidget(codexui::BrandMark::createLockup());

  restoreSidebarButton = new QPushButton(QStringLiteral("Show threads"));
  restoreSidebarButton->setProperty("kind", "subtle");
  restoreSidebarButton->setFixedHeight(32);
  restoreSidebarButton->hide();
  topLayout->addSpacing(12);
  topLayout->addWidget(restoreSidebarButton);
  topLayout->addSpacing(18);
  workspaceBreadcrumb = makeLabel(QStringLiteral("No workspace"), "muted");
  workspaceBreadcrumb->setObjectName(QStringLiteral("workspaceBreadcrumb"));
  workspaceBreadcrumb->setWordWrap(false);
  workspaceBreadcrumb->setMaximumWidth(280);
  topLayout->addWidget(workspaceBreadcrumb);
  topLayout->addStretch();

  restoreInspectorButton = new QPushButton(QStringLiteral("Show inspector"));
  restoreInspectorButton->setProperty("kind", "subtle");
  restoreInspectorButton->setFixedHeight(32);
  restoreInspectorButton->hide();
  requestButton = new QPushButton;
  requestButton->setProperty("kind", "request");
  requestButton->setFixedHeight(32);
  requestButton->hide();
  controllerButton = new QPushButton(QStringLiteral("Claim control"));
  controllerButton->setFixedHeight(32);
  topLayout->addWidget(restoreInspectorButton);
  topLayout->addWidget(requestButton);
  topLayout->addWidget(controllerButton);

  connectionStatusDot = statusDot();
  connectionStatusDot->setToolTip(QStringLiteral("Not connected"));
  connectionButton = new UiStyle::ChevronToolButton;
  connectionButton->setObjectName(QStringLiteral("transportButton"));
  connectionButton->setText(QStringLiteral("Connection"));
  connectionButton->setProperty("kind", "subtle");
  connectionButton->setProperty("codexChevron", true);
  connectionButton->setPopupMode(QToolButton::InstantPopup);
  connectionButton->setFixedHeight(32);
  auto *connectionMenu = new QMenu(connectionButton);
  connectionMenu->addAction(QStringLiteral("Configure..."), owner, [this] {
    nlohmann::json settings = nlohmann::json::object();
    {
      auto read = session.nodeGraph().tryRead();
      if (!read) {
        showNotice(QStringLiteral("Connection state is busy; try again."));
        return;
      }
      const nodegraph::NodeRef connection =
          read->find({nodegraph::NodeKind::Connection, "connection"});
      if (connection) {
        const auto state = read->state(connection);
        if (const nodegraph::Value *value = valueMember(*state, "settings"))
          settings = jsonFromValue(*value);
      }
    }
    if (!settings.is_object() || settings.empty()) {
      showNotice(QStringLiteral("Connection settings are not available yet."));
      return;
    }
    if (retainedConnectionSelection &&
        connectionSettingsContainSelection(settings,
                                           *retainedConnectionSelection))
      retainedConnectionSelection.reset();
    if (retainedConnectionSelection)
      applyConnectionSelection(settings, *retainedConnectionSelection);
    ConnectionDialog dialog(std::move(settings), owner);
    if (dialog.exec() != QDialog::Accepted)
      return;
    retainedConnectionSelection = dialog.selection();
    nodegraph::RuntimeAction action;
    action.kind = nodegraph::RuntimeActionKind::ConfigureConnection;
    action.payload = objectFromJson(*retainedConnectionSelection);
    static_cast<void>(sendRuntimeAction(
        std::move(action),
        QStringLiteral("Connection settings were not sent; try again.")));
  });
  connectionMenu->addSeparator();
  connectAction =
      connectionMenu->addAction(QStringLiteral("Connect"), owner, [this] {
        static_cast<void>(sendRuntimeAction(
            {nodegraph::RuntimeActionKind::Connect},
            QStringLiteral("Connect request was not admitted; try again.")));
      });
  disconnectAction =
      connectionMenu->addAction(QStringLiteral("Disconnect"), owner, [this] {
        static_cast<void>(sendRuntimeAction(
            {nodegraph::RuntimeActionKind::Disconnect},
            QStringLiteral("Disconnect request was not admitted; try again.")));
      });
  reconnectAction =
      connectionMenu->addAction(QStringLiteral("Reconnect"), owner, [this] {
        static_cast<void>(sendRuntimeAction(
            {nodegraph::RuntimeActionKind::Reconnect},
            QStringLiteral("Reconnect request was not admitted; try again.")));
      });
  connectionButton->setMenu(connectionMenu);
  auto *connectionControl = new QWidget;
  auto *connectionLayout = new QHBoxLayout(connectionControl);
  connectionLayout->setContentsMargins(0, 0, 0, 0);
  connectionLayout->setSpacing(6);
  connectionLayout->addWidget(connectionButton);
  connectionLayout->addWidget(connectionStatusDot);
  topLayout->addWidget(connectionControl);
  root->addWidget(top);

  middleRegion = new middle::MiddleRegionWidget;
  root->addWidget(middleRegion, 1);

  auto *statusBar = new QFrame;
  statusBar->setObjectName(QStringLiteral("customStatusBar"));
  statusBar->setFixedHeight(40);
  auto *statusLayout = new QHBoxLayout(statusBar);
  statusLayout->setContentsMargins(18, 0, 24, 0);
  statusLayout->setSpacing(10);
  auto *attribution = new QLabel(
      QStringLiteral(
          "<span style=\"color:%1;font-weight:600\">"
          "© Volker Christian &amp; Codex</span>  |  "
          "<a style=\"color:%1;text-decoration:none;font-weight:600\" "
          "href=\"https://github.com/SNodeC/CodexUI\">CodexUI</a>  •  "
          "<a style=\"color:%1;text-decoration:none;font-weight:600\" "
          "href=\"https://github.com/SNodeC/AISuite\">AISuite</a>  •  "
          "<span style=\"color:%2;font-size:8pt\">Powered by</span> "
          "<a style=\"color:%1;text-decoration:none;font-weight:600\" "
          "href=\"https://github.com/SNodeC/snode.c\">SNode.C</a>")
          .arg(QString::fromLatin1(UiStyle::strongText),
               QString::fromLatin1(UiStyle::placeholder)));
  attribution->setObjectName(QStringLiteral("statusAttribution"));
  attribution->setProperty("kind", "meta");
  attribution->setTextFormat(Qt::RichText);
  attribution->setOpenExternalLinks(true);
  attribution->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                       Qt::LinksAccessibleByKeyboard);
  attribution->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  statusLayout->addWidget(attribution);
  statusLayout->addStretch();
  auto *statusCaption = makeStatusLabel(
      QStringLiteral("Status:"), QStringLiteral("globalStatusCaption"), 60);
  statusLayout->addWidget(statusCaption);
  globalStatusDot = statusDot();
  globalStatusDot->setObjectName(QStringLiteral("shellGlobalStatusDot"));
  statusLayout->addWidget(globalStatusDot);
  globalStatusLabel =
      makeStatusLabel(QStringLiteral("Offline"),
                      QStringLiteral("globalStatusLabel"), 160, "settingLabel");
  statusLayout->addWidget(globalStatusLabel);
  root->addWidget(statusBar);
}

void ShellWidget::Impl::connectUi() {
  middle::ThreadPane::Actions threadActions;
  threadActions.newThread = [this] { beginNewThreadDialog(); };
  threadActions.loadMore = [this] {
    static_cast<void>(sendRuntimeAction(
        {nodegraph::RuntimeActionKind::LoadMoreThreads},
        QStringLiteral("More threads could not be requested; try again.")));
  };
  threadActions.hide = [this] { middleRegion->showSidebar(false); };
  threadActions.select = [this](const nodegraph::NodeRef &thread) {
    selectGraphThread(thread, ThreadSelectionOrigin::User);
  };
  threadActions.reload = [this](const nodegraph::NodeRef &thread) {
    if (!thread)
      return;
    nodegraph::NodeAction action{thread, nodegraph::NodeActionKind::Reload};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("Thread reload was not admitted; try again.")));
  };
  threadActions.rename = [this](const nodegraph::NodeRef &thread) {
    if (thread)
      renameThreadDialog(thread);
  };
  threadActions.fork = [this](const nodegraph::NodeRef &thread) {
    if (!thread)
      return;
    const std::optional<NewThreadDraft> draft = suggestedForkDraft(thread);
    if (!draft) {
      showNotice(QStringLiteral("Thread state is busy; try Quick fork again."));
      return;
    }
    forkThread(thread, *draft, false);
  };
  threadActions.forkWithOptions = [this](const nodegraph::NodeRef &thread) {
    if (thread)
      beginForkThreadDialog(thread);
  };
  threadActions.toggleArchive = [this](const nodegraph::NodeRef &thread) {
    if (!thread)
      return;
    bool archived = false;
    if (auto read = session.nodeGraph().tryRead(); read && read->live(thread))
      archived = boolFromValue(valueMember(*read->state(thread), "archived"));
    else {
      showNotice(
          QStringLiteral("Thread state is busy; no archive action was sent."));
      return;
    }
    nodegraph::NodeAction action{thread,
                                 archived ? nodegraph::NodeActionKind::Unarchive
                                          : nodegraph::NodeActionKind::Archive};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("Archive request was not admitted; try again.")));
  };
  threadActions.remove = [this](const nodegraph::NodeRef &thread) {
    if (thread)
      confirmDeleteThread(thread);
  };
  middleRegion->threads().setActions(std::move(threadActions));

  middle::ComposerPane::Actions composerActions;
  composerActions.submit = [this](QString prompt,
                                  std::vector<AttachmentDraft> attachments) {
    return submitPrompt(std::move(prompt), std::move(attachments));
  };
  composerActions.stop = [this] {
    nodegraph::NodeRef turn = activeTurn();
    if (!turn) {
      showNotice(QStringLiteral("No active turn is available to stop."));
      return;
    }
    nodegraph::NodeAction action{turn,
                                 nodegraph::NodeActionKind::InterruptTurn};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("Stop request was not admitted; try again.")));
  };
  composerActions.attach = [this] { chooseAttachments(); };
  composerActions.accept = [this] {
    respondToRenderedPending(&Impl::acceptPending);
  };
  composerActions.review = [this] {
    respondToRenderedPending(&Impl::reviewPending);
  };
  composerActions.deny = [this] {
    respondToRenderedPending(&Impl::rejectPending);
  };
  middleRegion->composer().setActions(std::move(composerActions));

  middleRegion->conversation().setLoadMoreAction([this] {
    if (!boundGraphThread || pendingConversation ||
        middleRegion->conversation().structuralStagingActive())
      return;
    const auto info = uiAdapter.conversationInfo(boundGraphThread);
    if (!info) {
      showNotice(QStringLiteral(
          "Conversation state is busy; no history request was sent."));
      return;
    }
    if (info->historyRequestPending || !info->providerHasMore)
      return;
    nodegraph::NodeAction action{boundGraphThread,
                                 nodegraph::NodeActionKind::LoadHistory};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("History request was not admitted; try again.")));
  });
  middleRegion->conversation().setPromptMaterializedAction(
      [this](nodegraph::NodeRef localPrompt) {
        nodegraph::NodeAction action{
            std::move(localPrompt),
            nodegraph::NodeActionKind::PromptMaterialized};
        return sendNodeAction(std::move(action), {});
      });
  middleRegion->conversation().setPromptRecoveryAction(
      [this](nodegraph::NodeRef prompt) { recoverPrompt(prompt); });
  middleRegion->conversation().setReconciliationFinishedAction(
      [this](const std::string &threadId,
             middle::ConversationView::ReconciliationResult result,
             bool selectionCommitted) {
        if (!boundGraphThread || boundGraphThread->id().canonical != threadId)
          return;
        if (result ==
            middle::ConversationView::ReconciliationResult::Rejected) {
          showNotice(QStringLiteral(
              "Conversation data could not be presented; select Reload to "
              "retry."));
          if (pendingConversation || !pendingConversationItems.empty())
            schedulePaneCommit(true);
          return;
        }
        if (pendingConversation || !pendingConversationItems.empty())
          schedulePaneCommit(true);
        if (!selectionCommitted)
          return;
        renderedChrome.reset();
        pendingInspector = true;
        pendingChrome = true;
        schedulePaneCommit();
      });
  middleRegion->inspector().setRequestActions(
      [this](const nodegraph::NodeRef &target) { reviewPending(target); },
      [this](const nodegraph::NodeRef &target) { acceptPending(target); },
      [this](const nodegraph::NodeRef &target) { rejectPending(target); });
  middleRegion->inspector().setRefreshRequestedAction([this] {
    pendingInspector = true;
    schedulePaneCommit();
  });
  middleRegion->setPaneVisibilityAction(
      [this](bool sidebarVisible, bool inspectorVisible) {
        restoreSidebarButton->setVisible(!sidebarVisible);
        restoreInspectorButton->setVisible(!inspectorVisible);
      });

  connect(restoreSidebarButton, &QPushButton::clicked, owner,
          [this] { middleRegion->showSidebar(true); });
  connect(restoreInspectorButton, &QPushButton::clicked, owner, [this] {
    middleRegion->showInspector(true);
    pendingInspector = true;
    schedulePaneCommit();
  });
  connect(requestButton, &QPushButton::clicked, owner, [this] {
    middleRegion->showInspector(true);
    middleRegion->inspector().tabs()->setCurrentIndex(3);
    pendingInspector = true;
    schedulePaneCommit();
  });
  connect(controllerButton, &QPushButton::clicked, owner, [this] {
    bool controller = false;
    auto read = session.nodeGraph().tryRead();
    if (!read) {
      showNotice(QStringLiteral(
          "Connection state is busy; no controller request was sent."));
      return;
    }
    const nodegraph::NodeRef connection =
        read->find({nodegraph::NodeKind::Connection, "connection"});
    if (connection)
      controller = exactStringFromValue(valueMember(*read->state(connection),
                                                    "role")) == "controller";
    read.reset();
    static_cast<void>(sendRuntimeAction(
        {controller ? nodegraph::RuntimeActionKind::ReleaseController
                    : nodegraph::RuntimeActionKind::ClaimController},
        QStringLiteral("Controller request was not admitted; try again.")));
  });
  qApp->installEventFilter(owner);
}

void ShellWidget::Impl::showNotice(QString message, bool error) {
  middleRegion->showNotice(std::move(message), error);
}

void ShellWidget::Impl::scheduleGraphBinding(bool immediate) {
  if (graphBindingScheduled)
    return;
  graphBindingScheduled = true;
  const auto token = alive;
  const int delay = immediate ? 0 : GraphRetryDelayMilliseconds;
  QTimer::singleShot(delay, owner, [this, token] {
    if (!*token)
      return;
    graphBindingScheduled = false;
    runGraphBinding();
  });
}

void ShellWidget::Impl::runGraphBinding() {
  nodegraph::NodeRef selectedThread;
  if (!selectedGraphThreadId.empty()) {
    {
      auto graphRead = session.nodeGraph().tryRead();
      if (!graphRead) {
        scheduleGraphBinding(false);
        return;
      }
      selectedThread =
          graphRead->find({nodegraph::NodeKind::Thread, selectedGraphThreadId});
    }
  }

  const bool reboundSelectedThread =
      selectedThread && selectedThread != boundGraphThread;
  bindGraphPanes(std::move(selectedThread));
  if (reboundSelectedThread) {
    nodegraph::NodeAction action{boundGraphThread,
                                 nodegraph::NodeActionKind::Hydrate};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral(
            "Thread loading was not admitted; select Reload to retry.")));
  }
}

void ShellWidget::Impl::bindGraphPanes(nodegraph::NodeRef selectedThread) {
  if (graphPanesBound && boundGraphThread == selectedThread) {
    render();
    return;
  }
  if (selectedThread && boundGraphThread != selectedThread)
    middleRegion->conversation().beginThreadSelection(
        selectedThread->id().canonical, selectedThread->incarnation());
  pendingConversationAuthorityReplacement = false;
  boundGraphThread = std::move(selectedThread);
  graphPanesBound = true;
  pendingConversation = false;
  pendingConversationItems.clear();
  if (auto threads = uiAdapter.threads(boundGraphThread)) {
    middleRegion->threads().refresh(*threads);
  }
  const bool conversationReady = refreshConversation();
  pendingInspector = false;
  const bool inspectorReady = refreshInspector();
  std::optional<ui::PendingRequestsSummary> requests = pendingRequestSummary();
  pendingChrome = !requests;
  if (requests)
    render(&*requests);
  // The immediate atomic bind already represents the newest graph state.
  // Any frame-coalesced work queued for the previous selection is obsolete.
  pendingThreadPane = false;
  pendingThreadRows.clear();
  if (!conversationReady) {
    pendingConversation = true;
    pendingConversationAuthorityReplacement = true;
  }
  pendingInspector |= !inspectorReady;
  if (pendingConversation || pendingInspector || pendingChrome)
    schedulePaneCommit();
}

bool ShellWidget::Impl::refreshConversation() {
  if (!boundGraphThread) {
    static_cast<void>(middleRegion->conversation().reconcile({}));
    return true;
  }

  const auto info = uiAdapter.conversationInfo(boundGraphThread);
  if (!info)
    return false;
  const std::string &threadId = boundGraphThread->id().canonical;
  middleRegion->conversation().setHistoryRequestPending(
      threadId, info->historyRequestPending);

  if (!info->readyForDisplay) {
    if (!info->hydrationFailed) {
      middleRegion->conversation().beginThreadSelection(
          threadId, boundGraphThread->incarnation());
      return true;
    }
    middleRegion->conversation().setEmptyMessage(
        QStringLiteral("Thread loading failed. Select Reload to retry."));
    middle::ConversationSnapshot failed;
    failed.threadId = threadId;
    static_cast<void>(middleRegion->conversation().reconcile(failed));
    return true;
  }

  auto snapshot = uiAdapter.conversation(boundGraphThread);
  if (!snapshot)
    return false;
  middleRegion->conversation().setEmptyMessage(
      QStringLiteral("No materialized activity."));
  const middle::ConversationView::SnapshotDisposition disposition =
      middleRegion->conversation().reconcileStaged(std::move(*snapshot));
  return disposition !=
         middle::ConversationView::SnapshotDisposition::Retryable;
}

bool ShellWidget::Impl::refreshInspector() {
  middle::InspectorPane &pane = middleRegion->inspector();
  const std::optional<ui::InspectorProjection> projection =
      pane.currentProjection();
  if (!projection)
    return true;
  if (*projection == ui::InspectorProjection::Requests) {
    ui::InspectorSnapshot snapshot;
    snapshot.threadIncarnation =
        boundGraphThread ? boundGraphThread->incarnation() : 0;
    auto requests = pendingRequestPage(pane.rowRequest(*projection));
    if (!requests)
      return false;
    snapshot.requests = std::move(*requests);
    pane.refresh(snapshot, *projection);
    return true;
  }
  nodegraph::NodeRef inspectorThread = boundGraphThread;
  if (inspectorThread) {
    const auto info = uiAdapter.conversationInfo(inspectorThread);
    if (!info)
      return false;
    const std::string &presentedThreadId =
        middleRegion->conversation().presentedThreadId();
    if (!presentedThreadId.empty() &&
        presentedThreadId != inspectorThread->id().canonical &&
        !info->hydrationFailed)
      return true;
    if (!info->readyForDisplay)
      inspectorThread.reset();
  }
  if (auto snapshot = uiAdapter.inspector(inspectorThread, *projection,
                                          pane.rowRequest(*projection))) {
    pane.refresh(*snapshot, *projection);
    return true;
  }
  return false;
}

void ShellWidget::Impl::schedulePaneCommit(bool immediate) {
  if (paneCommitScheduled)
    return;
  paneCommitScheduled = true;
  const auto token = alive;
  QTimer::singleShot(immediate ? 0 : 16, Qt::PreciseTimer, owner,
                     [this, token] {
                       if (!*token)
                         return;
                       paneCommitScheduled = false;
                       commitPendingPanes();
                     });
}

void ShellWidget::Impl::commitPendingPanes() {
  owner->setProperty("paneCommitInvocations",
                     owner->property("paneCommitInvocations").toULongLong() +
                         1);
  owner->setProperty("conversationPresentationRowsInLastPass", qulonglong{0});
  bool retry = false;
  bool waitingForConversationStage = false;
  if (!pendingThreadPane && !pendingThreadRows.empty()) {
    std::vector<nodegraph::NodeRef> rows = std::move(pendingThreadRows);
    pendingThreadRows.clear();
    bool structuralFallback = false;
    for (const nodegraph::NodeRef &thread : rows) {
      const auto row = uiAdapter.threadRow(thread);
      if (!row || !middleRegion->threads().applyRowPresentation(*row)) {
        structuralFallback = true;
        break;
      }
    }
    if (structuralFallback) {
      pendingThreadPane = true;
    } else {
      ++threadPaneRoutes;
      owner->setProperty("threadPaneRoutes",
                         static_cast<qulonglong>(threadPaneRoutes));
      owner->setProperty(
          "targetedThreadPaneRoutes",
          owner->property("targetedThreadPaneRoutes").toULongLong() + 1);
    }
  }
  if (pendingThreadPane) {
    if (auto threads = uiAdapter.threads(boundGraphThread)) {
      pendingThreadPane = false;
      pendingThreadRows.clear();
      ++threadPaneRoutes;
      owner->setProperty("threadPaneRoutes",
                         static_cast<qulonglong>(threadPaneRoutes));
      middleRegion->threads().refresh(*threads);
    } else {
      retry = true;
    }
  }
  if (!pendingConversation && !pendingConversationItems.empty() &&
      boundGraphThread) {
    const std::size_t requestedRows = std::min(
        ConversationPresentationRowsPerPass, pendingConversationItems.size());
    std::vector<nodegraph::NodeRef> items;
    items.reserve(requestedRows);
    auto pending = pendingConversationItems.begin();
    for (std::size_t index = 0; index < requestedRows; ++index, ++pending)
      items.push_back(*pending);
    auto delta = uiAdapter.conversationDelta(boundGraphThread, items, false);
    std::size_t appliedRows = delta ? delta->presentations.size() : 0;
    const bool projected = delta.has_value();
    const bool applied =
        delta &&
        middleRegion->conversation().applyConversationDelta(std::move(*delta));
    if (!applied)
      appliedRows = 0;
    if (applied) {
      for (std::size_t index = 0; index < requestedRows; ++index)
        pendingConversationItems.pop_front();
    } else if (projected) {
      pendingConversation = true;
      pendingConversationAuthorityReplacement = true;
      pendingConversationItems.clear();
    } else {
      retry = true;
    }
    owner->setProperty("conversationPresentationRowsInLastPass",
                       static_cast<qulonglong>(appliedRows));
    owner->setProperty(
        "conversationPresentationRowsProcessed",
        owner->property("conversationPresentationRowsProcessed").toULongLong() +
            static_cast<qulonglong>(appliedRows));
    owner->setProperty(
        "conversationPresentationMaxRowsPerPass",
        std::max(owner->property("conversationPresentationMaxRowsPerPass")
                     .toULongLong(),
                 static_cast<qulonglong>(appliedRows)));
    if (appliedRows != 0) {
      ++conversationRoutes;
      owner->setProperty("conversationRoutes",
                         static_cast<qulonglong>(conversationRoutes));
      owner->setProperty(
          "targetedConversationRoutes",
          owner->property("targetedConversationRoutes").toULongLong() + 1);
    }
    if (applied && !pendingConversationItems.empty())
      owner->setProperty(
          "conversationPresentationDeferredPasses",
          owner->property("conversationPresentationDeferredPasses")
                  .toULongLong() +
              1);
  }
  if (pendingConversation && !pendingConversationAuthorityReplacement &&
      !pendingConversationItems.empty() && boundGraphThread &&
      !middleRegion->conversation().structuralStagingActive()) {
    std::vector<nodegraph::NodeRef> items(pendingConversationItems.begin(),
                                          pendingConversationItems.end());
    auto delta = uiAdapter.conversationDelta(boundGraphThread, items, true);
    if (!delta) {
      retry = true;
    } else if (middleRegion->conversation().applyConversationDelta(
                   std::move(*delta))) {
      pendingConversation = false;
      pendingConversationAuthorityReplacement = false;
      pendingConversationItems.clear();
      ++conversationRoutes;
      owner->setProperty("conversationRoutes",
                         static_cast<qulonglong>(conversationRoutes));
      owner->setProperty(
          "targetedConversationRoutes",
          owner->property("targetedConversationRoutes").toULongLong() + 1);
    } else {
      pendingConversationAuthorityReplacement = true;
      pendingConversationItems.clear();
    }
  }
  if (pendingConversation && pendingConversationAuthorityReplacement) {
    const bool authorityReplacement = pendingConversationAuthorityReplacement;
    pendingConversation = false;
    pendingConversationAuthorityReplacement = false;
    pendingConversationItems.clear();
    if (refreshConversation()) {
      ++conversationRoutes;
      owner->setProperty("conversationRoutes",
                         static_cast<qulonglong>(conversationRoutes));
    } else {
      pendingConversation = true;
      pendingConversationAuthorityReplacement = authorityReplacement;
      retry = true;
    }
  }
  waitingForConversationStage =
      middleRegion->conversation().structuralStagingActive() &&
      (pendingConversation || !pendingConversationItems.empty());
  std::optional<ui::PendingRequestsSummary> projectedRequests;
  if (pendingChrome) {
    projectedRequests = pendingRequestSummary();
    if (!projectedRequests)
      retry = true;
  }
  if (pendingInspector) {
    pendingInspector = false;
    if (refreshInspector()) {
      ++inspectorRoutes;
      owner->setProperty("inspectorRoutes",
                         static_cast<qulonglong>(inspectorRoutes));
    } else {
      pendingInspector = true;
      retry = true;
    }
  }
  if (pendingChrome && projectedRequests) {
    pendingChrome = false;
    render(&*projectedRequests);
  }
  if (retry || pendingThreadPane || !pendingThreadRows.empty() ||
      (!waitingForConversationStage &&
       (pendingConversation || !pendingConversationItems.empty())) ||
      pendingInspector || pendingChrome)
    schedulePaneCommit();
}

void ShellWidget::Impl::handleGraphChanged(
    const nodegraph::GraphChanged &change) {
  const bool requestsChanged = uiAdapter.inspectorAffected(
      change, boundGraphThread, ui::InspectorProjection::Requests);
  const bool updateChrome =
      requestsChanged ||
      shellChromeAffected(change, session.nodeGraph(), boundGraphThread);
  const bool providerReset = change.providerAuthorityRevision != 0;
  const bool authorityAdvanced =
      change.providerAuthorityRevision > observedProviderAuthorityRevision;
  if (authorityAdvanced)
    observedProviderAuthorityRevision = change.providerAuthorityRevision;
  for (const nodegraph::NodeRef &removed : change.removed) {
    if (!removed)
      continue;
    if (removed->id().kind == nodegraph::NodeKind::Thread) {
      middleRegion->conversation().forgetThreadPresentation(
          removed->id().canonical, removed->incarnation());
      middleRegion->composer().turnSettings().forget(removed->id().canonical,
                                                     removed->incarnation());
    }
    if (removed->id().kind == nodegraph::NodeKind::Interaction)
      localPendingRequests.erase(removed);
    retainedRenames.erase(removed.get());
  }
  bool staleBoundThread = false;
  if (authorityAdvanced && change.rescanRequired && boundGraphThread) {
    auto read = session.nodeGraph().tryRead();
    staleBoundThread =
        read && read->find(boundGraphThread->id()) != boundGraphThread;
  }
  const nodegraph::NodeRef removedBoundThread =
      boundGraphThread &&
              (staleBoundThread ||
               std::ranges::find(change.removed, boundGraphThread) !=
                   change.removed.end())
          ? boundGraphThread
          : nodegraph::NodeRef{};
  const ui::NodeGraphUiAdapter::ConversationRoute conversation =
      uiAdapter.conversationRoute(change, boundGraphThread);
  if (conversation.historyRequestPending && boundGraphThread)
    middleRegion->conversation().setHistoryRequestPending(
        boundGraphThread->id().canonical, *conversation.historyRequestPending);
  middle::InspectorPane &inspectorPane = middleRegion->inspector();
  const std::optional<ui::InspectorProjection> inspectorProjection =
      inspectorPane.isVisible() ? inspectorPane.currentProjection()
                                : std::nullopt;
  const ThreadPaneRoute threads =
      requestsChanged && !change.childListsChanged.empty()
          ? ThreadPaneRoute{true, true, {}}
          : threadPaneRoute(change, session.nodeGraph(),
                            middleRegion->threads().currentSortCriterion());
  if (threads.structural) {
    pendingThreadPane = true;
    pendingThreadRows.clear();
  } else if (threads.affected && !pendingThreadPane) {
    for (const nodegraph::NodeRef &thread : threads.rows)
      if (std::ranges::find(pendingThreadRows, thread) ==
          pendingThreadRows.end())
        pendingThreadRows.push_back(thread);
  }
  if (conversation.authorityReplacement) {
    pendingConversation = true;
    pendingConversationAuthorityReplacement = true;
    pendingConversationItems.clear();
  } else {
    if (conversation.structural) {
      pendingConversation = true;
      pendingConversationAuthorityReplacement = false;
    }
    if (conversation.affected && !pendingConversationAuthorityReplacement) {
      for (const nodegraph::NodeRef &item : conversation.items)
        if (std::ranges::find(pendingConversationItems, item) ==
            pendingConversationItems.end())
          pendingConversationItems.push_back(item);
      if (pendingConversationItems.size() >
          ui::NodeGraphUiAdapter::MaximumConversationDeltaItems) {
        pendingConversation = true;
        pendingConversationAuthorityReplacement = true;
        pendingConversationItems.clear();
      }
    }
  }
  pendingInspector =
      pendingInspector ||
      (inspectorProjection &&
       (*inspectorProjection == ui::InspectorProjection::Requests
            ? requestsChanged
            : uiAdapter.inspectorAffected(change, boundGraphThread,
                                          *inspectorProjection)));
  pendingChrome = pendingChrome || updateChrome;
  if (graphUiFallbackAffected(change, session.nodeGraph()))
    reconcileGraphUiFallback();

  // Promotion may already have rebound a removed optimistic thread. Otherwise
  // clear all pane-held references synchronously before FrontendSession can
  // acknowledge retirement. Preserve the canonical selection only across a
  // provider generation reset so recreation can trigger one fresh hydration.
  if (removedBoundThread && boundGraphThread == removedBoundThread) {
    if (!providerReset)
      selectedGraphThreadId.clear();
    bindGraphPanes({});
  }

  // Retirement must not outlive presentation references. Ordinary state
  // traffic is merged to one old-UI reconciliation per display interval.
  if (!change.removed.empty() || authorityAdvanced)
    commitPendingPanes();
  else if (pendingThreadPane || !pendingThreadRows.empty() ||
           pendingConversation || !pendingConversationItems.empty() ||
           pendingInspector || pendingChrome)
    schedulePaneCommit();
  if (authorityAdvanced)
    observedProviderAuthorityRevision = std::max(
        observedProviderAuthorityRevision, middleRegion->composer()
                                               .turnSettings()
                                               .context()
                                               .providerAuthorityRevision);

  const bool selectedChanged =
      !graphPanesBound || change.rescanRequired ||
      (!boundGraphThread && !selectedGraphThreadId.empty() &&
       std::ranges::any_of(
           change.affected,
           [this](const auto &node) {
             return node && node->id().kind == nodegraph::NodeKind::Thread &&
                    node->id().canonical == selectedGraphThreadId;
           })) ||
      std::ranges::any_of(change.removed, [this](const auto &node) {
        return node && node->id().kind == nodegraph::NodeKind::Thread &&
               node->id().canonical == selectedGraphThreadId;
      });
  if (selectedChanged)
    scheduleGraphBinding();
}

void ShellWidget::Impl::reconcileGraphUiFallback() {
  std::string notice;
  bool noticeError = true;
  std::uint64_t noticeSerial = lastNoticeSerial;
  nodegraph::NodeRef selection;
  std::uint64_t selectionSerial = lastSelectionSerial;
  if (auto read = session.nodeGraph().tryRead()) {
    const auto considerNotice = [&](std::string_view id) {
      if (const nodegraph::NodeRef node =
              read->find({nodegraph::NodeKind::Notice, std::string(id)})) {
        const auto state = read->state(node);
        const std::int64_t rawSerial =
            signedIntegerFromValue(valueMember(*state, "noticeSerial"))
                .value_or(0);
        if (rawSerial > 0 &&
            static_cast<std::uint64_t>(rawSerial) > noticeSerial) {
          noticeSerial = static_cast<std::uint64_t>(rawSerial);
          notice = exactStringFromValue(valueMember(*state, "noticeText"));
          if (notice.empty())
            notice = exactStringFromValue(valueMember(*state, "message"));
          noticeError = exactStringFromValue(valueMember(*state, "severity")) !=
                        "warning";
        }
      }
    };
    considerNotice("local-worker-notice");
    considerNotice("provider-notice");
    if (const nodegraph::NodeRef runtime =
            read->find({nodegraph::NodeKind::Runtime, "runtime"})) {
      const auto state = read->state(runtime);
      const std::int64_t rawSerial =
          signedIntegerFromValue(valueMember(*state, "uiSelectionSerial"))
              .value_or(0);
      if (rawSerial > 0 &&
          static_cast<std::uint64_t>(rawSerial) > lastSelectionSerial) {
        const std::vector<nodegraph::NodeRef> targets =
            read->related(runtime, nodegraph::RelationKind::UiSelectionTarget);
        if (!targets.empty() && read->live(targets.front())) {
          selectionSerial = static_cast<std::uint64_t>(rawSerial);
          selection = targets.front();
        }
      }
    }
  } else {
    if (!graphFallbackScheduled) {
      graphFallbackScheduled = true;
      const auto token = alive;
      QTimer::singleShot(GraphRetryDelayMilliseconds, owner, [this, token] {
        if (!*token)
          return;
        graphFallbackScheduled = false;
        reconcileGraphUiFallback();
      });
    }
    return;
  }

  if (noticeSerial > lastNoticeSerial) {
    lastNoticeSerial = noticeSerial;
    if (!notice.empty())
      showNotice(text(notice), noticeError);
  }
  if (selection && selectionSerial > lastSelectionSerial) {
    lastSelectionSerial = selectionSerial;
    selectGraphThread(std::move(selection), ThreadSelectionOrigin::Graph,
                      selectionSerial);
  }
}

void ShellWidget::Impl::selectGraphThread(nodegraph::NodeRef thread,
                                          ThreadSelectionOrigin origin,
                                          std::uint64_t graphSerial) {
  if (!thread || thread->id().kind != nodegraph::NodeKind::Thread)
    return;
  std::string creationCorrelation;
  bool local = false;
  {
    auto read = session.nodeGraph().tryRead();
    if (!read) {
      QTimer::singleShot(
          GraphRetryDelayMilliseconds, owner,
          [this, thread = std::move(thread), origin, graphSerial]() mutable {
            selectGraphThread(std::move(thread), origin, graphSerial);
          });
      return;
    }
    if (!read->live(thread))
      return;
    const auto state = read->state(thread);
    creationCorrelation =
        exactStringFromValue(valueMember(*state, "creationCorrelation"));
    local = boolFromValue(valueMember(*state, "local"));
  }
  if (origin == ThreadSelectionOrigin::Graph && graphSerial != 0 &&
      graphSerial != lastSelectionSerial)
    return;
  if (origin == ThreadSelectionOrigin::User) {
    const auto visible = middleRegion->threads().visiblySelectedThread();
    if (!visible || visible->target != thread)
      return;
  }
  const std::string targetId = thread->id().canonical;
  const bool ownsCreation = !creationCorrelation.empty() &&
                            creationCorrelation == creationDraftCorrelation;
  if (!local && !creationCorrelation.empty())
    middleRegion->composer().turnSettings().promote(
        ui::threadPresentationKey(targetId, creationCorrelation), targetId,
        thread->incarnation());
  if (origin == ThreadSelectionOrigin::Graph && !creationCorrelation.empty()) {
    if (!ownsCreation && !creationDraftCorrelation.empty())
      return;
    if (ownsCreation) {
      if (!local)
        creationDraftCorrelation.clear();
      if (!selectedGraphThreadId.empty() && selectedGraphThreadId != targetId)
        return;
    }
  }
  if (origin == ThreadSelectionOrigin::User && newThreadDraft) {
    middleRegion->composer().turnSettings().forget(
        ui::threadPresentationKey(DraftThreadId, creationDraftCorrelation));
    middleRegion->threads().discardOptimisticThread(DraftThreadId);
    creationDraftCorrelation.clear();
  }
  newThreadDraft.reset();
  selectedGraphThreadId = targetId;
  bindGraphPanes(thread);
  if (origin == ThreadSelectionOrigin::User)
    hydrateSelectedThreadIfNeeded(std::move(thread));
}

void ShellWidget::Impl::handleUiEffect(const nodegraph::UiEffect &effect) {
  switch (effect.kind) {
  case nodegraph::UiEffectKind::ShowNotice: {
    const std::int64_t rawSerial =
        signedIntegerFromValue(valueMember(effect.details, "serial"))
            .value_or(0);
    if (rawSerial > 0) {
      const auto serial = static_cast<std::uint64_t>(rawSerial);
      if (serial <= lastNoticeSerial)
        break;
      lastNoticeSerial = serial;
    }
    showNotice(text(effect.text),
               exactStringFromValue(valueMember(effect.details, "severity")) !=
                   "warning");
    break;
  }
  case nodegraph::UiEffectKind::SelectThread: {
    const std::int64_t rawSerial =
        signedIntegerFromValue(valueMember(effect.details, "serial"))
            .value_or(0);
    std::uint64_t serial = 0;
    if (rawSerial > 0) {
      serial = static_cast<std::uint64_t>(rawSerial);
      if (serial <= lastSelectionSerial)
        break;
      lastSelectionSerial = serial;
    }
    if (effect.target &&
        (*effect.target)->id().kind == nodegraph::NodeKind::Thread)
      selectGraphThread(*effect.target, ThreadSelectionOrigin::Graph, serial);
    break;
  }
  case nodegraph::UiEffectKind::ProtocolDiagnostic:
    middleRegion->inspector().appendProtocolDiagnostic(effect);
    return;
  }
}

bool ShellWidget::Impl::sendNodeAction(nodegraph::NodeAction action,
                                       QString rejection) {
  const nodegraph::ChannelSendStatus status = session.sendNodeAction(action);
  if (!nodegraph::deliveryGuaranteed(status)) {
    if (!rejection.isEmpty())
      showNotice(std::move(rejection));
    return false;
  }
  if (nodegraph::wakeFailed(status))
    showNotice(QStringLiteral(
        "The action was admitted after a worker wake-up failure; bounded "
        "fallback delivery is active."));
  return true;
}

bool ShellWidget::Impl::sendRuntimeAction(nodegraph::RuntimeAction action,
                                          QString rejection) {
  const nodegraph::ChannelSendStatus status = session.sendRuntimeAction(action);
  if (!nodegraph::deliveryGuaranteed(status)) {
    if (!rejection.isEmpty())
      showNotice(std::move(rejection));
    return false;
  }
  if (nodegraph::wakeFailed(status))
    showNotice(QStringLiteral(
        "The action was admitted after a worker wake-up failure; bounded "
        "fallback delivery is active."));
  return true;
}

void ShellWidget::Impl::hydrateSelectedThreadIfNeeded(
    nodegraph::NodeRef thread) {
  if (!thread || boundGraphThread != thread)
    return;
  auto read = session.nodeGraph().tryRead();
  if (!read) {
    QTimer::singleShot(GraphRetryDelayMilliseconds, owner,
                       [this, thread = std::move(thread)]() mutable {
                         hydrateSelectedThreadIfNeeded(std::move(thread));
                       });
    return;
  }
  if (!read->live(thread))
    return;
  const bool recoveryOnly =
      boolFromValue(valueMember(*read->state(thread), "recoveryOnly"));
  read.reset();
  if (recoveryOnly)
    return;
  nodegraph::NodeAction action{std::move(thread),
                               nodegraph::NodeActionKind::Hydrate};
  static_cast<void>(sendNodeAction(
      std::move(action),
      QStringLiteral(
          "Thread loading was not admitted; select Reload to retry.")));
}

nodegraph::NodeRef ShellWidget::Impl::activeTurn() const {
  if (!boundGraphThread)
    return {};
  auto read = session.nodeGraph().tryRead();
  if (!read || !read->live(boundGraphThread))
    return {};
  nodegraph::NodeRef indexed =
      read->relatedAt(boundGraphThread, nodegraph::RelationKind::ActiveTurn, 0);
  if (indexed && indexed->id().kind == nodegraph::NodeKind::Turn)
    return indexed;
  return {};
}

void ShellWidget::Impl::applyLocalPending(PendingRequestDescriptor &request) {
  const auto local = localPendingRequests.find(request.target);
  if (local == localPendingRequests.end())
    return;
  if (local->second.awaitingResponseAfter &&
      request.responseRevision > *local->second.awaitingResponseAfter) {
    localPendingRequests.erase(local);
    return;
  }
  request.retainedSubmission = local->second.submission;
  if (local->second.awaitingResponseAfter)
    request.availability = PendingRequestAvailability::Submitting;
}

std::optional<ui::PendingRequestsSummary>
ShellWidget::Impl::pendingRequestSummary(bool *busy) {
  if (busy)
    *busy = false;
  std::vector<nodegraph::NodeRef> localTargets;
  localTargets.reserve(localPendingRequests.size());
  for (const auto &[target, local] : localPendingRequests) {
    static_cast<void>(local);
    localTargets.push_back(target);
  }
  auto summary = uiAdapter.pendingRequestSummary(
      boundGraphThread ? std::string_view(boundGraphThread->id().canonical)
                       : std::string_view{},
      localTargets);
  if (!summary) {
    if (busy)
      *busy = true;
    return std::nullopt;
  }
  for (PendingRequestDescriptor &request : summary->candidates)
    applyLocalPending(request);
  std::erase_if(localPendingRequests, [&](const auto &local) {
    return std::ranges::none_of(summary->candidates, [&](const auto &request) {
      return request.target == local.first;
    });
  });
  return summary;
}

std::optional<ui::InspectorPageSnapshot>
ShellWidget::Impl::pendingRequestPage(const ui::InspectorRowRequest &request,
                                      bool *busy) {
  if (busy)
    *busy = false;
  auto page = uiAdapter.pendingRequests(request);
  if (!page) {
    if (busy)
      *busy = true;
    return std::nullopt;
  }
  const auto apply = [this](ui::InspectorRow &row) {
    if (auto *pending = std::get_if<PendingRequestDescriptor>(&row.value))
      applyLocalPending(*pending);
  };
  for (ui::InspectorRow &row : page->rows)
    apply(row);
  if (page->focused)
    apply(*page->focused);
  return page;
}

std::optional<PendingRequestDescriptor>
ShellWidget::Impl::pendingRequest(const nodegraph::NodeRef &target) {
  bool busy = false;
  auto request = uiAdapter.pendingRequest(target, &busy);
  if (!request) {
    showNotice(
        busy ? QStringLiteral("Request state is busy; no response was sent.")
             : QStringLiteral("The pending request is no longer actionable."));
    return std::nullopt;
  }
  applyLocalPending(*request);
  return request;
}

void ShellWidget::Impl::render(
    const ui::PendingRequestsSummary *projectedRequests) {
  ShellChromeValues values;
  values.title = "Select a thread";
  values.workspace = "No workspace";

  {
    auto read = session.nodeGraph().tryRead();
    if (!read) {
      pendingChrome = true;
      schedulePaneCommit();
      return;
    }
    if (const nodegraph::NodeRef connection =
            read->find({nodegraph::NodeKind::Connection, "connection"})) {
      const auto state = read->state(connection);
      const std::string transport =
          exactStringFromValue(valueMember(*state, "transportState"));
      values.connected = state->status == nodegraph::NodeStatus::Connected;
      values.retrying = transport == "retrying" || transport == "connecting";
      values.role = exactStringFromValue(valueMember(*state, "role"));
      values.providerState =
          exactStringFromValue(valueMember(*state, "providerState"));
      values.canControl = values.connected && values.providerState == "ready" &&
                          values.role == "controller";
      if (const nodegraph::Value *settings = valueMember(*state, "settings")) {
        if (const auto *object = settings->asObject()) {
          const std::string selected =
              exactStringFromValue(valueMember(*object, "selected"));
          const nodegraph::Value *available = valueMember(*object, "available");
          if (available && available->asArray()) {
            for (const nodegraph::Value &entry : *available->asArray()) {
              const auto *transportEntry = entry.asObject();
              if (transportEntry && exactStringFromValue(valueMember(
                                        *transportEntry, "key")) == selected) {
                values.selectedTransport =
                    exactStringFromValue(valueMember(*transportEntry, "label"));
                break;
              }
            }
          }
        }
      }
    }

    nodegraph::NodeRef selected = boundGraphThread;
    if (selected && !read->live(selected))
      selected.reset();
    if (selected) {
      const auto state = read->state(selected);
      values.title = exactStringFromValue(valueMember(*state, "name"));
      if (values.title.empty())
        values.title = exactStringFromValue(valueMember(*state, "title"));
      if (values.title.empty())
        values.title = "Untitled thread";
      values.workspace = exactStringFromValue(valueMember(*state, "cwd"));
      if (values.workspace.empty())
        values.workspace =
            exactStringFromValue(valueMember(*state, "workspace"));
      if (values.workspace.empty())
        values.workspace = "No workspace";
      values.status = statusFromState(*state);
      values.recoveryOnly = boolFromValue(valueMember(*state, "recoveryOnly"));
      const std::string hydrationState =
          exactStringFromValue(valueMember(*state, "hydrationState"));
      const bool local = boolFromValue(valueMember(*state, "local"));
      values.conversationReadyForDisplay =
          hydrationState == "ready" || local || values.recoveryOnly;
      values.hydrationFailed = hydrationState == "failed";
      values.threadAdmissionReady =
          !values.recoveryOnly && hydrationState != "loading" &&
          hydrationState != "failed" &&
          (state->status != nodegraph::NodeStatus::NotLoaded ||
           hydrationState == "ready");
      values.lastActivityAt =
          signedIntegerFromValue(valueMember(*state, "lastActivityAt"));
      for (const std::string_view field :
           {std::string_view("recencyAt"), std::string_view("updatedAt"),
            std::string_view("localActivityAt"),
            std::string_view("localPromptActivityAt")}) {
        const std::optional<std::int64_t> timestamp =
            signedIntegerFromValue(valueMember(*state, field));
        if (timestamp &&
            (!values.lastActivityAt || *timestamp > *values.lastActivityAt))
          values.lastActivityAt = timestamp;
      }
      nodegraph::NodeRef turn =
          read->relatedAt(selected, nodegraph::RelationKind::ActiveTurn, 0);
      values.activeTurn = turn && turn->id().kind == nodegraph::NodeKind::Turn;
    } else if (newThreadDraft) {
      values.title = newThreadDraft->name.trimmed().isEmpty()
                         ? "New thread"
                         : utf8(newThreadDraft->name.trimmed());
      values.workspace = utf8(newThreadDraft->workspace);
    }
  }

  std::optional<ui::PendingRequestsSummary> ownedRequests;
  if (!projectedRequests) {
    ownedRequests = pendingRequestSummary();
    if (!ownedRequests) {
      pendingChrome = true;
      schedulePaneCommit();
      return;
    }
    projectedRequests = &*ownedRequests;
  }
  values.totalPending = projectedRequests->total;
  const std::optional<std::size_t> attention =
      PendingRequestPolicy::attentionIndex(
          projectedRequests->candidates,
          boundGraphThread ? std::string_view(boundGraphThread->id().canonical)
                           : std::string_view{});
  if (attention && *attention < projectedRequests->candidates.size())
    values.attention = projectedRequests->candidates[*attention];
  const std::string &presentedThreadId =
      middleRegion->conversation().presentedThreadId();
  const bool replacementHydrating =
      boundGraphThread && !presentedThreadId.empty() &&
      boundGraphThread->id().canonical != presentedThreadId &&
      !values.hydrationFailed;

  if (!replacementHydrating) {
    const std::string draftIdentity =
        newThreadDraft || !creationDraftCorrelation.empty()
            ? ui::threadPresentationKey(DraftThreadId, creationDraftCorrelation)
            : std::string{};
    const std::string draftWorkspace =
        newThreadDraft ? utf8(newThreadDraft->workspace) : std::string{};
    auto settingsContext =
        uiAdapter.turnSettings(boundGraphThread, draftIdentity, draftWorkspace);
    if (!settingsContext) {
      pendingChrome = true;
      schedulePaneCommit();
      return;
    }
    middleRegion->composer().setTurnSettingsContext(
        std::move(*settingsContext));
  }

  const bool chromeChanged = !renderedChrome || *renderedChrome != values;
  if (!chromeChanged)
    return;

  if (!replacementHydrating) {
    middleRegion->conversation().setEmptyMessage(
        values.recoveryOnly
            ? QStringLiteral(
                  "This thread preserves an unsent prompt. Restore it from "
                  "the failed prompt card before continuing.")
        : boundGraphThread && values.hydrationFailed
            ? QStringLiteral("Thread loading failed. Select Reload to retry.")
        : boundGraphThread && !values.conversationReadyForDisplay
            ? QStringLiteral("Loading conversation…")
        : boundGraphThread
            ? QStringLiteral("Conversation activity appears here.")
            : (newThreadDraft
                   ? QStringLiteral("Send a message to create this thread.")
                   : QStringLiteral("Conversation activity appears here.")));
    if (boundGraphThread) {
      const QString activity = values.lastActivityAt
                                   ? lastActivityText(*values.lastActivityAt)
                                   : QString{};
      const QString status = text(displayStatus(values.status));
      const std::string_view statusToneValue = statusTone(values.status);
      const QString tone =
          QString::fromLatin1(statusToneValue.data(), statusToneValue.size());
      middleRegion->setThreadHeading(text(values.title), text(values.workspace),
                                     activity, status, tone);
    } else {
      middleRegion->setThreadHeading(text(values.title),
                                     text(values.workspace));
    }
  }
  renderStatus(values, !replacementHydrating);
  renderedChrome = values;
  ++shellRenderCommits;
  owner->setProperty("shellRenderCommits",
                     static_cast<qulonglong>(shellRenderCommits));
}

void ShellWidget::Impl::renderStatus(const ShellChromeValues &status,
                                     bool updateWorkspace) {
  QString dotTone;
  QString dotTip;
  if (status.connected) {
    dotTone = QStringLiteral("success");
    dotTip = QStringLiteral("Connected");
  } else if (status.retrying) {
    dotTone = QStringLiteral("warning");
    dotTip = QStringLiteral("Disconnected, retrying");
  } else {
    dotTone = QStringLiteral("danger");
    dotTip = QStringLiteral("Disconnected");
  }
  setStatusTone(connectionStatusDot, dotTone);
  if (connectionStatusDot->toolTip() != dotTip)
    connectionStatusDot->setToolTip(dotTip);
  const QString connectionText = status.selectedTransport.empty()
                                     ? QStringLiteral("Connection")
                                     : text(status.selectedTransport);
  if (connectionButton->text() != connectionText)
    connectionButton->setText(connectionText);
  const QString connectionTip =
      status.connected ? QStringLiteral("Connected bridge transport")
                       : QStringLiteral("Disconnected bridge transport");
  if (connectionButton->toolTip() != connectionTip)
    connectionButton->setToolTip(connectionTip);
  connectAction->setEnabled(!status.connected);
  disconnectAction->setEnabled(status.connected);
  reconnectAction->setEnabled(status.connected);
  const QString controllerText = status.role == "controller"
                                     ? QStringLiteral("Release control")
                                     : QStringLiteral("Claim control");
  if (controllerButton->text() != controllerText)
    controllerButton->setText(controllerText);
  controllerButton->setEnabled(status.connected);

  const QString requestText =
      QStringLiteral("Requests (%1)")
          .arg(static_cast<qulonglong>(status.totalPending));
  if (requestButton->text() != requestText)
    requestButton->setText(requestText);
  requestButton->setVisible(status.totalPending != 0);
  PendingRequestControls controls;
  if (status.attention) {
    const PendingRequestDescriptor &request = *status.attention;
    const bool replacesTarget =
        !renderedChrome || !renderedChrome->attention ||
        renderedChrome->attention->target != request.target;
    controls = PendingRequestPolicy::controls(request);
    QString requestDetail = text(PendingRequestPolicy::detail(request));
    const QString requestStatus = text(PendingRequestPolicy::status(request));
    if (!requestStatus.isEmpty())
      requestDetail += QStringLiteral("  |  ") + requestStatus;
    middleRegion->composer().setAttentionRequest(
        text(PendingRequestPolicy::title(request.kind)),
        std::move(requestDetail), controls.positive.has_value(),
        controls.positive ? text(controls.positive->label) : QString{},
        controls.negative.has_value(),
        controls.negative ? text(controls.negative->label) : QString{},
        replacesTarget);
  }
  middleRegion->composer().setAttentionVisible(status.attention.has_value());
  middleRegion->composer().setAttentionActionEnabled(
      controls.directEnabled && controls.positive,
      controls.directEnabled && controls.negative, controls.reviewEnabled);

  QString globalStatus = QStringLiteral("Ready");
  QString globalTone = QStringLiteral("success");
  if (status.retrying) {
    globalStatus = QStringLiteral("Reconnecting");
    globalTone = QStringLiteral("warning");
  } else if (!status.connected) {
    globalStatus = QStringLiteral("Offline");
    globalTone = QStringLiteral("danger");
  } else if (status.providerState != "ready") {
    globalStatus = status.providerState.empty()
                       ? QStringLiteral("Waiting for provider")
                       : QStringLiteral("Provider unavailable");
    globalTone = status.providerState.empty() ? QStringLiteral("warning")
                                              : QStringLiteral("danger");
  } else if (status.totalPending != 0) {
    globalStatus = QStringLiteral("Attention required");
    globalTone = QStringLiteral("warning");
  }
  setStatusTone(globalStatusDot, globalTone, globalStatusLabel);
  setStatusLabelText(globalStatusLabel, globalStatus);

  if (updateWorkspace) {
    const QString workspace = text(status.workspace);
    if (workspaceBreadcrumb->toolTip() != workspace)
      workspaceBreadcrumb->setToolTip(workspace);
    const QString displayedWorkspace =
        workspaceBreadcrumb->fontMetrics().elidedText(
            workspace, Qt::ElideMiddle, workspaceBreadcrumb->maximumWidth());
    if (workspaceBreadcrumb->text() != displayedWorkspace)
      workspaceBreadcrumb->setText(displayedWorkspace);
  }

  middleRegion->composer().setActiveTurn(status.activeTurn);
  middleRegion->composer().setCanSubmit(
      status.canControl && (boundGraphThread || newThreadDraft) &&
      (status.activeTurn || status.threadAdmissionReady));
  middleRegion->composer().setSettingsEnabled(
      status.canControl && status.threadAdmissionReady &&
      (boundGraphThread || newThreadDraft) && !status.activeTurn);
}

void ShellWidget::Impl::beginNewThreadDialog() {
  if (!creationDraftCorrelation.empty()) {
    showNotice(QStringLiteral("A new-thread draft is already active. Select "
                              "another thread to abandon it before starting "
                              "another."),
               false);
    return;
  }
  const QString fallback = QDir::currentPath();
  const QString initial =
      text(middleRegion->composer().turnSettings().workspace(utf8(fallback)));
  NewThreadDialog dialog(initial, owner);
  if (dialog.exec() != QDialog::Accepted)
    return;
  NewThreadDraft draft = dialog.draft();
  newThreadDraft = draft;
  creationDraftCorrelation =
      "qt-draft:" + std::to_string(nextCreationDraftSerial++);
  selectedGraphThreadId.clear();
  bindGraphPanes({});
  middleRegion->threads().beginOptimisticThread(
      DraftThreadId,
      ui::threadPresentationKey(DraftThreadId, creationDraftCorrelation),
      draft.name.trimmed().isEmpty() ? "New thread"
                                     : utf8(draft.name.trimmed()),
      utf8(draft.workspace));
  middleRegion->composer().clearDraft();
  middleRegion->composer().promptEditor()->setFocus();
}

std::optional<NewThreadDraft>
ShellWidget::Impl::suggestedForkDraft(const nodegraph::NodeRef &thread) const {
  if (!thread || thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;
  const std::optional<ui::ThreadListRow> source = uiAdapter.threadRow(thread);
  if (!source)
    return std::nullopt;
  const std::optional<ui::ThreadListSnapshot> snapshot = uiAdapter.threads({});
  if (!snapshot)
    return std::nullopt;

  std::vector<std::string> titles;
  collectThreadTitles(snapshot->roots, titles);
  NewThreadDraft draft;
  draft.workspace = text(source->cwd);
  draft.name = text(suggestForkName(source->title, titles));

  if (auto read = session.nodeGraph().tryRead(); read && read->live(thread)) {
    const std::shared_ptr<const nodegraph::NodeState> state =
        read->state(thread);
    draft.baseInstructions =
        text(exactStringFromValue(valueMember(*state, "baseInstructions")));
    draft.developerInstructions = text(
        exactStringFromValue(valueMember(*state, "developerInstructions")));
    draft.ephemeral = boolFromValue(valueMember(*state, "ephemeral"));
  }
  return draft;
}

void ShellWidget::Impl::forkThread(const nodegraph::NodeRef &thread,
                                   NewThreadDraft draft, bool includeOptions) {
  if (!thread || thread->id().kind != nodegraph::NodeKind::Thread ||
      !uiAdapter.threadRow(thread)) {
    showNotice(QStringLiteral(
        "The source thread is no longer available; no fork was sent."));
    return;
  }
  nodegraph::NodeAction action{thread, nodegraph::NodeActionKind::Fork};
  const QString requestedName = draft.name.trimmed();
  if (!draft.ephemeral && !requestedName.isEmpty())
    action.payload.emplace("requestedName", utf8(requestedName));
  if (includeOptions) {
    const QString workspace = draft.workspace.trimmed();
    if (!workspace.isEmpty())
      action.payload.emplace("cwd", utf8(workspace));
    const QString baseInstructions = draft.baseInstructions.trimmed();
    if (!baseInstructions.isEmpty())
      action.payload.emplace("baseInstructions", utf8(baseInstructions));
    const QString developerInstructions = draft.developerInstructions.trimmed();
    if (!developerInstructions.isEmpty())
      action.payload.emplace("developerInstructions",
                             utf8(developerInstructions));
    action.payload.emplace("ephemeral", draft.ephemeral);
  }
  static_cast<void>(sendNodeAction(
      std::move(action),
      QStringLiteral("Thread fork was not admitted; try again.")));
}

void ShellWidget::Impl::beginForkThreadDialog(
    const nodegraph::NodeRef &thread) {
  std::optional<NewThreadDraft> draft = suggestedForkDraft(thread);
  if (!draft) {
    showNotice(
        QStringLiteral("Thread state is busy; try Fork with options again."));
    return;
  }
  NewThreadDialog dialog(*draft, NewThreadDialog::Purpose::Fork, owner);
  if (dialog.exec() != QDialog::Accepted)
    return;
  NewThreadDraft selected = dialog.draft();
  if (selected.name.trimmed().isEmpty())
    selected.name = draft->name;
  forkThread(thread, std::move(selected), true);
}

void ShellWidget::Impl::renameThreadDialog(const nodegraph::NodeRef &thread) {
  if (!thread || thread->id().kind != nodegraph::NodeKind::Thread)
    return;
  std::string currentName;
  if (auto read = session.nodeGraph().tryRead()) {
    if (!read->live(thread))
      return;
    const auto state = read->state(thread);
    currentName = exactStringFromValue(valueMember(*state, "name"));
    if (currentName.empty())
      currentName = exactStringFromValue(valueMember(*state, "title"));
  } else {
    showNotice(QStringLiteral("Thread state is busy; try again."));
    return;
  }
  const auto retained = retainedRenames.find(thread.get());
  const QString initialName = retained == retainedRenames.end()
                                  ? text(currentName)
                                  : retained->second.second;
  bool accepted = false;
  const QString name =
      QInputDialog::getText(owner, QStringLiteral("Rename thread"),
                            QStringLiteral("Name"), QLineEdit::Normal,
                            initialName, &accepted)
          .trimmed();
  if (!accepted || name.isEmpty())
    return;
  {
    auto read = session.nodeGraph().tryRead();
    if (!read) {
      showNotice(
          QStringLiteral("Thread state is busy; the rename was not sent."));
      return;
    }
    if (!read->live(thread)) {
      retainedRenames.erase(thread.get());
      showNotice(QStringLiteral(
          "The thread was removed while Rename was open; no rename was sent."));
      return;
    }
  }
  nodegraph::NodeAction action{thread, nodegraph::NodeActionKind::Rename};
  action.payload.emplace("name", utf8(name));
  const nodegraph::ChannelSendStatus status = session.sendNodeAction(action);
  if (!nodegraph::deliveryGuaranteed(status)) {
    retainedRenames.insert_or_assign(thread.get(), std::pair{thread, name});
    showNotice(QStringLiteral(
        "Rename request was not admitted. Reopen Rename to recover the "
        "name you entered."));
    return;
  }
  retainedRenames.erase(thread.get());
  if (nodegraph::wakeFailed(status))
    showNotice(QStringLiteral(
        "The rename was admitted after a worker wake-up failure; bounded "
        "fallback delivery is active."));
}

void ShellWidget::Impl::confirmDeleteThread(const nodegraph::NodeRef &thread) {
  if (!thread || thread->id().kind != nodegraph::NodeKind::Thread)
    return;
  if (QMessageBox::question(owner, QStringLiteral("Delete thread"),
                            QStringLiteral("Delete the selected thread?"),
                            QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel) != QMessageBox::Yes)
    return;
  if (!uiAdapter.threadRow(thread)) {
    showNotice(QStringLiteral(
        "The thread is no longer available; no delete was sent."));
    return;
  }
  static_cast<void>(sendNodeAction(
      {thread, nodegraph::NodeActionKind::Delete},
      QStringLiteral("Delete request was not admitted; try again.")));
}

bool ShellWidget::Impl::submitPrompt(QString prompt,
                                     std::vector<AttachmentDraft> attachments) {
  if (prompt.trimmed().isEmpty())
    return false;
  TurnSettingsPolicy &settings = middleRegion->composer().turnSettings();
  std::vector<nodegraph::Attachment> ownedAttachments;
  ownedAttachments.reserve(attachments.size());
  for (AttachmentDraft &attachment : attachments) {
    ownedAttachments.push_back({std::move(attachment.path),
                                std::move(attachment.name),
                                std::move(attachment.mimeType), std::nullopt});
  }

  bool admitted = false;
  const std::optional<middle::ThreadPane::VisibleThread> visible =
      middleRegion->threads().visiblySelectedThread();
  nodegraph::NodeRef target = visible && visible->target == boundGraphThread
                                  ? visible->target
                                  : nodegraph::NodeRef{};
  const bool visibleDraft =
      visible && !visible->target && visible->id == DraftThreadId &&
      visible->presentationKey ==
          ui::threadPresentationKey(DraftThreadId, creationDraftCorrelation);
  if (target) {
    QString graphRejection;
    if (auto read = session.nodeGraph().tryRead()) {
      if (!read->live(target)) {
        graphRejection =
            QStringLiteral("The selected thread is no longer available. Your "
                           "message was not sent.");
      } else {
        const auto state = read->state(target);
        const std::string hydration =
            exactStringFromValue(valueMember(*state, "hydrationState"));
        const nodegraph::NodeRef turn =
            read->relatedAt(target, nodegraph::RelationKind::ActiveTurn, 0);
        const bool steeringKnownActiveTurn =
            turn && turn->id().kind == nodegraph::NodeKind::Turn;
        if (!steeringKnownActiveTurn &&
            (hydration == "loading" || hydration == "failed" ||
             (state->status == nodegraph::NodeStatus::NotLoaded &&
              hydration != "ready"))) {
          const std::string detail =
              exactStringFromValue(valueMember(*state, "hydrationError"));
          graphRejection = text(
              detail.empty()
                  ? "This thread must finish loading before sending. Your "
                    "draft is still available."
                  : detail +
                        ". Reload the thread; your draft is still available.");
        }
      }
    } else {
      graphRejection = QStringLiteral(
          "Thread state is busy. Your draft is still available; try again.");
    }
    // The graph read guard above is gone before any QWidget is touched.
    if (!graphRejection.isEmpty()) {
      showNotice(std::move(graphRejection));
      return false;
    }
    auto settingsContext = uiAdapter.turnSettings(target);
    if (!settingsContext) {
      showNotice(QStringLiteral(
          "Thread settings are busy. Your message was not sent; try again."));
      return false;
    }
    middleRegion->composer().setTurnSettingsContext(
        std::move(*settingsContext));
    nodegraph::NodeAction action{target,
                                 nodegraph::NodeActionKind::SubmitPrompt};
    action.promptText = utf8(prompt);
    action.attachments = std::move(ownedAttachments);
    action.payload =
        objectFromJson(settings.startOptions(TurnSettingsScope::Turn));
    admitted = sendNodeAction(
        std::move(action),
        QStringLiteral("Your message was not sent; the worker queue is full."));
  } else if (visibleDraft && newThreadDraft) {
    auto settingsContext = uiAdapter.turnSettings(
        {}, ui::threadPresentationKey(DraftThreadId, creationDraftCorrelation),
        utf8(newThreadDraft->workspace));
    if (!settingsContext) {
      showNotice(QStringLiteral(
          "Thread settings are busy. Your message was not sent; try again."));
      return false;
    }
    middleRegion->composer().setTurnSettingsContext(
        std::move(*settingsContext));
    nodegraph::RuntimeAction action;
    action.kind = nodegraph::RuntimeActionKind::CreateThread;
    action.correlation = creationDraftCorrelation;
    action.promptText = utf8(prompt);
    action.attachments = std::move(ownedAttachments);
    nodegraph::Value::Object threadStart =
        objectFromJson(settings.startOptions(TurnSettingsScope::Thread));
    threadStart.insert_or_assign("cwd",
                                 settings.workspace(utf8(QDir::currentPath())));
    if (!newThreadDraft->ephemeral && !newThreadDraft->name.trimmed().isEmpty())
      action.payload.insert_or_assign("requestedName",
                                      utf8(newThreadDraft->name));
    if (!newThreadDraft->baseInstructions.isEmpty())
      threadStart.insert_or_assign("baseInstructions",
                                   utf8(newThreadDraft->baseInstructions));
    if (!newThreadDraft->developerInstructions.isEmpty())
      threadStart.insert_or_assign("developerInstructions",
                                   utf8(newThreadDraft->developerInstructions));
    if (newThreadDraft->ephemeral)
      threadStart.insert_or_assign("ephemeral", true);
    action.payload.insert_or_assign("threadStart",
                                    nodegraph::Value(std::move(threadStart)));
    action.payload.insert_or_assign(
        "turnStart", nodegraph::Value(objectFromJson(
                         settings.startOptions(TurnSettingsScope::Turn))));
    admitted = sendRuntimeAction(
        std::move(action),
        QStringLiteral("Your message was not sent; the worker queue is full."));
    if (admitted)
      newThreadDraft.reset();
  } else {
    showNotice(QStringLiteral(
        "No destination thread is selected. Your message was not sent."));
  }
  if (admitted)
    middleRegion->conversation().prepareForLocalPromptAdmission();
  return admitted;
}

void ShellWidget::Impl::chooseAttachments() {
  const QString initial =
      text(middleRegion->composer().turnSettings().workspace(
          utf8(QDir::currentPath())));
  FileSelectionDialog dialog(FileSelectionDialog::Mode::Attachments, initial,
                             middleRegion->composer().attachments(), owner);
  if (dialog.exec() == QDialog::Accepted)
    middleRegion->composer().setAttachments(dialog.selectedAttachments());
}

void ShellWidget::Impl::recoverPrompt(const nodegraph::NodeRef &prompt) {
  if (!prompt || prompt->id().kind != nodegraph::NodeKind::Item)
    return;

  std::shared_ptr<const nodegraph::NodeState> state;
  if (auto read = session.nodeGraph().tryRead()) {
    if (!read->live(prompt))
      return;
    state = read->state(prompt);
    if (!boolFromValue(valueMember(*state, "requiresExplicitRecovery")))
      return;
  } else {
    showNotice(QStringLiteral(
        "Prompt state is busy; the preserved prompt was not changed."));
    return;
  }
  std::string authoredText =
      exactStringFromValue(valueMember(*state, "authoredText"));
  if (authoredText.empty())
    authoredText = exactStringFromValue(valueMember(*state, "text"));
  std::vector<AttachmentDraft> attachments = graphAttachmentDrafts(*state);

  if (!middleRegion->composer().promptEditor()->toPlainText().isEmpty() ||
      !middleRegion->composer().attachments().empty()) {
    showNotice(QStringLiteral(
        "Clear or send the current draft before restoring this prompt. The "
        "preserved prompt was not changed."));
    return;
  }

  NewThreadDraft draft;
  const nodegraph::Value::Object *threadStart = nullptr;
  if (const nodegraph::Value *value = valueMember(*state, "threadStartOptions"))
    threadStart = value->asObject();
  const std::string recoveredWorkspace =
      threadStart ? exactStringFromValue(valueMember(*threadStart, "cwd"))
                  : std::string{};
  draft.workspace =
      recoveredWorkspace.empty()
          ? text(middleRegion->composer().turnSettings().workspace(
                utf8(QDir::currentPath())))
          : text(recoveredWorkspace);
  const std::string requestedName =
      exactStringFromValue(valueMember(*state, "requestedName"));
  draft.name = requestedName.empty() ? QStringLiteral("Recovered prompt")
                                     : text(requestedName);
  if (threadStart) {
    draft.baseInstructions = text(
        exactStringFromValue(valueMember(*threadStart, "baseInstructions")));
    draft.developerInstructions = text(exactStringFromValue(
        valueMember(*threadStart, "developerInstructions")));
    draft.ephemeral = boolFromValue(valueMember(*threadStart, "ephemeral"));
  }
  newThreadDraft = draft;
  creationDraftCorrelation =
      "qt-draft:" + std::to_string(nextCreationDraftSerial++);
  selectedGraphThreadId.clear();
  bindGraphPanes({});
  middleRegion->threads().beginOptimisticThread(
      DraftThreadId,
      ui::threadPresentationKey(DraftThreadId, creationDraftCorrelation),
      utf8(draft.name), utf8(draft.workspace));
  middleRegion->composer().promptEditor()->setPlainText(text(authoredText));
  middleRegion->composer().setAttachments(std::move(attachments));
  middleRegion->composer().promptEditor()->setFocus();
  showNotice(
      QStringLiteral("The unsent prompt was restored to a new-thread draft."),
      false);
}

void ShellWidget::Impl::respondToRenderedPending(
    void (Impl::*respond)(const nodegraph::NodeRef &)) {
  if (!renderedChrome || !renderedChrome->attention) {
    showNotice(QStringLiteral("The pending request is no longer actionable."));
    return;
  }
  (this->*respond)(renderedChrome->attention->target);
}

bool ShellWidget::Impl::submitPending(const PendingRequestDescriptor &request,
                                      PendingRequestSubmission submission) {
  if (request.availability == PendingRequestAvailability::Submitting) {
    showNotice(QStringLiteral("This request response is already submitting."),
               false);
    return false;
  }
  if (request.availability != PendingRequestAvailability::Actionable) {
    localPendingRequests.insert_or_assign(
        request.target,
        LocalPendingRequest{std::move(submission), std::nullopt});
    pendingChrome = true;
    pendingInspector = true;
    schedulePaneCommit();
    showNotice(
        request.availability == PendingRequestAvailability::RecoveryOnly
            ? QStringLiteral(
                  "The original request ended when the provider "
                  "changed. Your authored response remains preserved "
                  "for review, but was not sent.")
            : QStringLiteral("Controller access is unavailable. Your response "
                             "remains preserved for review."));
    return false;
  }

  nodegraph::NodeAction action{request.target,
                               nodegraph::NodeActionKind::ResolveInteraction};
  action.payload.emplace("choice", nodegraph::Value(submission.choice));
  if (!submission.input.is_null())
    action.payload.emplace("input", valueFromJson(submission.input));
  if (!submission.metadata.is_null())
    action.payload.emplace("metadata", valueFromJson(submission.metadata));
  auto [local, inserted] = localPendingRequests.insert_or_assign(
      request.target,
      LocalPendingRequest{std::move(submission), request.responseRevision});
  static_cast<void>(inserted);
  const nodegraph::ChannelSendStatus status = session.sendNodeAction(action);
  if (!nodegraph::deliveryGuaranteed(status)) {
    local->second.awaitingResponseAfter.reset();
    showNotice(QStringLiteral(
        "Your response was not admitted. Reopen Review to recover the input "
        "you entered; the request remains pending."));
  } else if (nodegraph::wakeFailed(status)) {
    showNotice(QStringLiteral(
        "The response was admitted after a worker wake-up failure; bounded "
        "fallback delivery is active."));
  }
  pendingChrome = true;
  pendingInspector = true;
  schedulePaneCommit();
  return nodegraph::deliveryGuaranteed(status);
}

void ShellWidget::Impl::presentPending(PendingRequestDescriptor request) {
  if (request.availability == PendingRequestAvailability::Submitting) {
    showNotice(QStringLiteral("This request response is already submitting."),
               false);
    return;
  }
  if (request.availability != PendingRequestAvailability::Actionable &&
      !request.retainedSubmission) {
    showNotice(QStringLiteral(
        "Controller access is unavailable; no response was sent."));
    return;
  }
  const auto submission = PendingRequestDialog::present(
      request, owner,
      request.retainedSubmission ? &*request.retainedSubmission : nullptr);
  if (!submission)
    return;
  const auto current = pendingRequest(request.target);
  if (current)
    static_cast<void>(submitPending(*current, *submission));
}

void ShellWidget::Impl::reviewPending(const nodegraph::NodeRef &target) {
  const auto request = pendingRequest(target);
  if (request)
    presentPending(*request);
}

void ShellWidget::Impl::acceptPending(const nodegraph::NodeRef &target) {
  const auto request = pendingRequest(target);
  if (!request)
    return;
  if (request->availability != PendingRequestAvailability::Actionable) {
    showNotice(QStringLiteral(
        "Controller access is unavailable; no response was sent."));
    return;
  }
  const auto positive = PendingRequestPolicy::controls(*request).positive;
  if (!positive) {
    presentPending(*request);
    return;
  }
  static_cast<void>(submitPending(
      *request, PendingRequestSubmission{positive->value, nullptr, nullptr}));
}

void ShellWidget::Impl::rejectPending(const nodegraph::NodeRef &target) {
  const auto request = pendingRequest(target);
  if (!request)
    return;
  if (request->availability != PendingRequestAvailability::Actionable) {
    showNotice(QStringLiteral(
        "Controller access is unavailable; no response was sent."));
    return;
  }
  const auto negative = PendingRequestPolicy::controls(*request).negative;
  if (!negative) {
    presentPending(*request);
    return;
  }
  static_cast<void>(submitPending(
      *request, PendingRequestSubmission{negative->value, nullptr, nullptr}));
}

ShellWidget::ShellWidget(FrontendSession &session, QWidget *parent)
    : QWidget(parent), impl(nullptr) {
  // Impl installs this widget as the application event filter. Keep the
  // member in a defined null state while Impl builds child widgets: their
  // construction can synchronously pass events through that filter.
  impl = std::make_unique<Impl>(this, session);
}

ShellWidget::~ShellWidget() = default;

bool ShellWidget::eventFilter(QObject *watched, QEvent *event) {
  if (watched == qApp && event &&
      event->type() == QEvent::ApplicationFontChange) {
    const QString styleSheet = UiStyle::applicationStyleSheet();
    if (qApp->styleSheet() != styleSheet)
      qApp->setStyleSheet(styleSheet);
  }
  if (impl && impl->middleRegion->routeScrollEvent(watched, event))
    return true;
  return QWidget::eventFilter(watched, event);
}

} // namespace codexui::codex
