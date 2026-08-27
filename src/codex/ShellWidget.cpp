// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ShellWidget.h"

#include "codex/ConnectionDialog.h"
#include "codex/FileSelectionDialog.h"
#include "codex/FrontendSession.h"
#include "codex/NewThreadDialog.h"
#include "codex/PendingRequestDialog.h"
#include "codex/PresentationModel.h"
#include "codex/PresentationStatus.h"
#include "codex/TurnSettingsWidget.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationProjection.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/middle/PromptCoordinator.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/BrandMark.h"
#include "codex/ui/ExpandingPromptEditor.h"
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
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace codexui::codex {
namespace {

constexpr auto DraftThreadId = "draft:new-thread";

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string{};
}

std::string safeMessage(const nlohmann::json &value) {
  std::string message = stringValue(value, "message");
  if (message.empty())
    message = stringValue(value, "detail");
  if (!message.empty())
    return message;
  const auto error = value.find("error");
  return error != value.end() && error->is_object()
             ? stringValue(*error, "message")
             : std::string{};
}

bool isThreadNotFoundResult(const nlohmann::json &result) {
  if (result.value("ok", false))
    return false;
  const QString message =
      text(safeMessage(result.value("error", nlohmann::json::object())))
          .toLower();
  return message.contains(QStringLiteral("thread")) &&
         message.contains(QStringLiteral("not found"));
}

std::optional<std::string> resultTurnId(const nlohmann::json &result) {
  const nlohmann::json scope = result.value("scope", nlohmann::json::object());
  std::string id = stringValue(scope, "turnId");
  if (!id.empty())
    return id;
  const nlohmann::json data = result.value("data", nlohmann::json::object());
  id = stringValue(data, "turnId");
  if (!id.empty())
    return id;
  const nlohmann::json turn = data.value("turn", nlohmann::json::object());
  id = stringValue(turn, "id");
  return id.empty() ? std::nullopt : std::optional<std::string>(std::move(id));
}

QLabel *makeLabel(QString value, const char *kind = "body") {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QFrame *statusDot() {
  auto *dot = new QFrame;
  dot->setFixedSize(10, 10);
  dot->setStyleSheet(QStringLiteral("background:#98a2b3;border-radius:5px;"));
  return dot;
}

} // namespace

struct ShellWidget::Impl final {
  enum class Hydration { NotHydrated, InFlight, Hydrated, Failed };
  enum class SettingsHydration {
    Unknown,
    WaitingForRead,
    InFlight,
    Hydrated,
    Failed
  };
  struct ThreadRuntimeState {
    Hydration hydration = Hydration::NotHydrated;
    SettingsHydration settingsHydration = SettingsHydration::Unknown;
    std::uint64_t readRevision = 0;
    bool operationReady = false;
    bool resumeInFlight = false;
    bool dispatchScheduled = false;
    std::unordered_set<std::uint64_t> recoveryAttemptedSubmissions;

    void resetForConnection() noexcept {
      hydration = Hydration::NotHydrated;
      settingsHydration = SettingsHydration::Unknown;
      readRevision = 0;
      operationReady = false;
      dispatchScheduled = false;
    }
  };
  struct SettingsUiSnapshot {
    std::string identity;
    nlohmann::json canonical;
    nlohmann::json modelCatalog;
    nlohmann::json permissionProfiles;
    std::uint64_t settingsRevision = 0;
    nlohmann::json settingsUpdate;

    bool operator==(const SettingsUiSnapshot &) const = default;
  };
  struct StatusUiSnapshot {
    bool connected = false;
    bool retrying = false;
    std::string role;
    QString selectedTransport;
    QString workspace;
    QString threadContext;
    QString agentActivity;
    bool active = false;
    std::size_t selectedPending = 0;
    std::size_t totalPending = 0;

    bool operator==(const StatusUiSnapshot &) const = default;
  };
  struct HistoryWindow {
    std::size_t requested =
        middle::ConversationProjection::DefaultAuthoritativeItemLimit;
    std::size_t effective =
        middle::ConversationProjection::DefaultAuthoritativeItemLimit;
    std::size_t lastAuthoritativeCount = 0;
  };

  Impl(ShellWidget *owner, FrontendSession &session)
      : owner(owner), session(session), alive(std::make_shared<bool>(true)) {
    buildUi();
    connectUi();
    const auto token = alive;
    session.setEventHandler([this, token](const nlohmann::json &event) {
      if (*token)
        handleEvent(event);
    });
    render();
  }

  ~Impl() {
    *alive = false;
    session.setEventHandler({});
    qApp->removeEventFilter(owner);
  }

  void buildUi();
  void connectUi();
  void handleEvent(const nlohmann::json &event);
  void scheduleRender();
  void render();
  void renderConversation();
  void refreshSettings();
  void refreshStatus();
  void hydrateHistoricalChildren(const std::string &parentThreadId);
  void showNotice(QString message, bool error = true);
  void resetRuntimeForConnection();

  void selectThread(std::string threadId);
  void beginNewThread();
  void readThread(const std::string &threadId, bool forced = false);
  void ensureThreadHydrated(const std::string &threadId);
  void ensureThreadSettingsHydrated(const std::string &threadId);
  void resumeThreadForSettings(const std::string &threadId);
  void renameThread(const std::string &threadId);
  void forkThread(const std::string &threadId);
  void toggleThreadArchive(const std::string &threadId);
  void deleteThread(const std::string &threadId);

  [[nodiscard]] bool submitPrompt(QString prompt,
                                  std::vector<AttachmentDraft> attachments);
  void startThreadForDraft();
  void dispatchNextPrompt(const std::string &threadId);
  void dispatchPrompt(middle::PromptDispatch dispatch);
  void resumePromptQueue(const std::string &threadId);
  void completePrompt(const std::string &threadId, std::uint64_t submissionId,
                      const nlohmann::json &result);
  [[nodiscard]] bool attemptThreadRecovery(const std::string &threadId,
                                           std::uint64_t submissionId,
                                           const nlohmann::json &result);
  void scheduleAcceptedTransition(const std::string &threadId,
                                  std::uint64_t submissionId);

  void chooseAttachments();
  void interruptTurn();
  void reviewPending(const std::string &requestKey);
  void rejectPending(const std::string &requestKey);
  void respondToFirstPending(bool approve);

  ShellWidget *owner = nullptr;
  FrontendSession &session;
  PresentationModel model;
  middle::PromptCoordinator prompts;
  std::shared_ptr<bool> alive;

  std::string selectedThreadId;
  bool newThreadIntent = false;
  bool newThreadCreationInFlight = false;
  nlohmann::json newThreadOptions = nlohmann::json::object();
  QString newThreadName;
  QString newThreadWorkspace;

  std::unordered_map<std::string, ThreadRuntimeState> runtimeByThread;
  std::unordered_set<std::string> staleReadResultCorrelations;
  std::uint64_t nextReadRevision = 1;
  std::unordered_map<std::string, HistoryWindow> historyWindows;
  std::uint64_t observedConnectionGeneration = 0;
  std::uint64_t observedProviderGeneration = 0;
  std::optional<SettingsUiSnapshot> settingsSnapshot;
  std::optional<StatusUiSnapshot> statusSnapshot;
  bool renderScheduled = false;

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
  QLabel *threadContextStatus = nullptr;
  QLabel *agentActivityStatus = nullptr;
  QLabel *controllerLabel = nullptr;
};

void ShellWidget::Impl::buildUi() {
  owner->setObjectName(QStringLiteral("applicationShell"));
  auto *root = new QVBoxLayout(owner);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto *top = new QFrame;
  top->setObjectName(QStringLiteral("topBar"));
  top->setStyleSheet(QStringLiteral(
      "QFrame#topBar{background:#ffffff;border-bottom:1px solid #d7dee8;}"));
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
  workspaceBreadcrumb->setWordWrap(false);
  workspaceBreadcrumb->setMaximumWidth(280);
  workspaceBreadcrumb->setStyleSheet(
      QStringLiteral("color:#667085;font-weight:500;"));
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
    if (!model.connection().settings.is_object() ||
        model.connection().settings.empty()) {
      showNotice(QStringLiteral("Connection settings are not available yet."));
      return;
    }
    ConnectionDialog dialog(model.connection().settings, owner);
    if (dialog.exec() != QDialog::Accepted)
      return;
    const auto token = alive;
    session.configureConnection(
        dialog.selection(), [this, token](const nlohmann::json &result) {
          if (!*token || result.value("ok", false))
            return;
          const std::string message =
              safeMessage(result.value("error", nlohmann::json::object()));
          showNotice(text(message.empty()
                              ? std::string("Connection configuration failed")
                              : message));
        });
  });
  connectionMenu->addSeparator();
  connectAction = connectionMenu->addAction(
      QStringLiteral("Connect"), owner, [this] { session.connectTransport(); });
  disconnectAction =
      connectionMenu->addAction(QStringLiteral("Disconnect"), owner,
                                [this] { session.disconnectTransport(); });
  reconnectAction = connectionMenu->addAction(
      QStringLiteral("Reconnect"), owner, [this] { session.reconnect(); });
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
  statusBar->setStyleSheet(QStringLiteral(
      "QFrame#customStatusBar{background:#f8fafc;border-top:1px solid "
      "#d7dee8;}"));
  statusBar->setFixedHeight(40);
  auto *statusLayout = new QHBoxLayout(statusBar);
  statusLayout->setContentsMargins(18, 0, 24, 0);
  statusLayout->setSpacing(8);
  threadContextStatus = makeLabel(QStringLiteral("No thread context"), "meta");
  statusLayout->addWidget(threadContextStatus);
  statusLayout->addSpacing(42);
  agentActivityStatus = makeLabel(QStringLiteral("No agent activity"), "meta");
  statusLayout->addWidget(agentActivityStatus);
  statusLayout->addStretch();
  controllerLabel = makeLabel(QStringLiteral("Observer"), "meta");
  statusLayout->addWidget(controllerLabel);
  root->addWidget(statusBar);
}

void ShellWidget::Impl::connectUi() {
  middle::ThreadPane::Actions threadActions;
  threadActions.newThread = [this] { beginNewThread(); };
  threadActions.refresh = [this] { session.listThreads(); };
  threadActions.hide = [this] { middleRegion->showSidebar(false); };
  threadActions.select = [this](const std::string &id) {
    if (id != selectedThreadId)
      selectThread(id);
  };
  threadActions.reload = [this](const std::string &id) {
    runtimeByThread[id].settingsHydration = SettingsHydration::Unknown;
    readThread(id, true);
    ensureThreadSettingsHydrated(id);
  };
  threadActions.rename = [this](const std::string &id) { renameThread(id); };
  threadActions.fork = [this](const std::string &id) { forkThread(id); };
  threadActions.toggleArchive = [this](const std::string &id) {
    toggleThreadArchive(id);
  };
  threadActions.remove = [this](const std::string &id) { deleteThread(id); };
  middleRegion->threads().setActions(std::move(threadActions));

  middle::ComposerPane::Actions composerActions;
  composerActions.submit = [this](QString prompt,
                                  std::vector<AttachmentDraft> attachments) {
    return submitPrompt(std::move(prompt), std::move(attachments));
  };
  composerActions.stop = [this] { interruptTurn(); };
  composerActions.attach = [this] { chooseAttachments(); };
  composerActions.review = [this] { respondToFirstPending(true); };
  composerActions.deny = [this] { respondToFirstPending(false); };
  middleRegion->composer().setActions(std::move(composerActions));

  middleRegion->conversation().setLoadMoreAction([this] {
    const std::string key = selectedThreadId.empty()
                                ? std::string(DraftThreadId)
                                : selectedThreadId;
    HistoryWindow &history = historyWindows[key];
    history.requested +=
        middle::ConversationProjection::DefaultAuthoritativeItemLimit;
    history.effective +=
        middle::ConversationProjection::DefaultAuthoritativeItemLimit;
    renderConversation();
  });
  middleRegion->inspector().setRequestActions(
      [this](const std::string &id) { reviewPending(id); },
      [this](const std::string &id) { rejectPending(id); });
  middleRegion->setPaneVisibilityAction(
      [this](bool sidebarVisible, bool inspectorVisible) {
        restoreSidebarButton->setVisible(!sidebarVisible);
        restoreInspectorButton->setVisible(!inspectorVisible);
      });

  connect(restoreSidebarButton, &QPushButton::clicked, owner,
          [this] { middleRegion->showSidebar(true); });
  connect(restoreInspectorButton, &QPushButton::clicked, owner,
          [this] { middleRegion->showInspector(true); });
  connect(requestButton, &QPushButton::clicked, owner, [this] {
    middleRegion->showInspector(true);
    middleRegion->inspector().tabs()->setCurrentIndex(3);
  });
  connect(controllerButton, &QPushButton::clicked, owner, [this] {
    if (model.connection().role == "controller")
      session.releaseController();
    else
      session.claimController();
  });
  qApp->installEventFilter(owner);
}

void ShellWidget::Impl::showNotice(QString message, bool error) {
  middleRegion->showNotice(std::move(message), error);
}

void ShellWidget::Impl::resetRuntimeForConnection() {
  for (auto &[threadId, runtime] : runtimeByThread) {
    static_cast<void>(threadId);
    runtime.resetForConnection();
  }
}

void ShellWidget::Impl::handleEvent(const nlohmann::json &event) {
  middleRegion->inspector().appendProtocolFrame(event);

  const std::string kind = stringValue(event, "kind");
  const std::string action = stringValue(event, "action");
  const std::string correlationId = stringValue(event, "correlationId");
  const bool staleReadResult =
      kind == "result" && action == "thread.read" && !correlationId.empty() &&
      staleReadResultCorrelations.erase(correlationId) > 0;
  if (!staleReadResult)
    model.applyEvent(event);

  const ConnectionPresentation &connection = model.connection();
  if (connection.generation != observedConnectionGeneration) {
    observedConnectionGeneration = connection.generation;
    resetRuntimeForConnection();
  }
  if (connection.providerGeneration != observedProviderGeneration) {
    observedProviderGeneration = connection.providerGeneration;
    resetRuntimeForConnection();
  }

  const std::string type = stringValue(event, "type");
  const nlohmann::json data = event.value("data", nlohmann::json::object());
  const nlohmann::json scope = event.value("scope", nlohmann::json::object());
  const std::string eventThreadId = stringValue(scope, "threadId");
  if (kind == "event" && type == "connection.provider" &&
      stringValue(data, "state") == "disconnected") {
    resetRuntimeForConnection();
  }

  if (kind == "result" && !event.value("ok", false) && action != "turn.start" &&
      action != "turn.steer" && action != "thread.read" &&
      action != "thread.resume") {
    const std::string message =
        safeMessage(event.value("error", nlohmann::json::object()));
    showNotice(text(message.empty() ? std::string("Codex operation failed")
                                    : message));
  } else if (kind == "event" && type == "notice.added") {
    const nlohmann::json notice =
        data.value("notice", nlohmann::json::object());
    const std::string message = safeMessage(notice);
    if (!message.empty())
      showNotice(text(message), stringValue(data, "severity") == "error");
  } else if (kind == "event" && type == "system.diagnostic") {
    const std::string message = safeMessage(data);
    if (!message.empty())
      showNotice(QStringLiteral("Protocol diagnostic: %1").arg(text(message)));
  } else if (kind == "event" && type == "connection.lifecycle" &&
             (stringValue(data, "state") == "failure" ||
              stringValue(data, "state") == "disconnected")) {
    const std::string detail = stringValue(data, "detail");
    if (!detail.starts_with("local-"))
      showNotice(detail.empty() ? QStringLiteral("Codex bridge disconnected")
                                : text(detail));
  }

  if (kind == "event" && type == "connection.bridge" &&
      stringValue(data, "state") == "opened") {
    session.listThreads();
    session.listModels();
    ensureThreadHydrated(selectedThreadId);
    ensureThreadSettingsHydrated(selectedThreadId);
    for (const std::string &threadId : prompts.queuedThreadIds()) {
      if (threadId == DraftThreadId) {
        if (newThreadIntent)
          startThreadForDraft();
      } else {
        dispatchNextPrompt(threadId);
      }
    }
    session.listPermissionProfiles(
        {{"cwd", QDir::currentPath().toStdString()}});
  }

  if (type == "thread.removed" && !eventThreadId.empty()) {
    prompts.clearThread(eventThreadId);
    runtimeByThread.erase(eventThreadId);
    historyWindows.erase(eventThreadId);
    if (selectedThreadId == eventThreadId) {
      selectedThreadId.clear();
      middleRegion->composer().clearDraft();
    }
  } else if (!eventThreadId.empty()) {
    if (const ThreadPresentation *thread = model.thread(eventThreadId)) {
      prompts.reconcile(eventThreadId, *thread,
                        QDateTime::currentMSecsSinceEpoch());
    }
  } else if (kind == "event" && type == "connection.provider" &&
             stringValue(data, "state") == "ready") {
    session.listThreads();
    session.listModels();
    readThread(selectedThreadId, true);
    ensureThreadSettingsHydrated(selectedThreadId);
  }

  if (kind == "result" && action == "thread.read" &&
      event.value("ok", false))
    hydrateHistoricalChildren(eventThreadId);
  else if (kind == "event" && type == "agents.activity.upsert")
    hydrateHistoricalChildren(eventThreadId);
  scheduleRender();
}

void ShellWidget::Impl::scheduleRender() {
  if (renderScheduled)
    return;
  renderScheduled = true;
  const auto token = alive;
  // A streamed response may deliver many deltas in one display interval.
  // Reconcile once per frame instead of rebuilding rich text and layout for
  // every transport chunk.
  QTimer::singleShot(16, Qt::PreciseTimer, owner, [this, token] {
    if (!*token)
      return;
    renderScheduled = false;
    render();
  });
}

void ShellWidget::Impl::render() {
  middleRegion->threads().refresh(model, selectedThreadId);
  renderConversation();
  middleRegion->inspector().refresh(model, selectedThreadId);
  refreshSettings();
  refreshStatus();
}

void ShellWidget::Impl::renderConversation() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  const std::string projectionId = selectedThreadId.empty() && newThreadIntent
                                       ? std::string(DraftThreadId)
                                       : selectedThreadId;
  middle::AuthoritativeItemIndex authoritativeItems =
      middle::indexAuthoritativeItems(projectionId, thread);
  if (thread)
    prompts.reconcile(selectedThreadId, authoritativeItems, now);
  const auto submissions = prompts.submissions(projectionId);
  const std::size_t authoritativeCount = authoritativeItems.ordered.size();
  HistoryWindow &history = historyWindows[projectionId];
  const middle::ConversationView::Mode viewportMode =
      middleRegion->conversation().modeForThread(projectionId);
  if (viewportMode == middle::ConversationView::Mode::Paused &&
      authoritativeCount > history.lastAuthoritativeCount) {
    // Do not evict the paused visual anchor merely because newer items were
    // appended. The hidden prefix stays constant until following resumes.
    history.effective += authoritativeCount - history.lastAuthoritativeCount;
  } else if (viewportMode == middle::ConversationView::Mode::Following) {
    history.effective = history.requested;
  }
  history.lastAuthoritativeCount = authoritativeCount;
  const middle::ConversationSnapshot snapshot =
      middle::ConversationProjection::project(
          authoritativeItems, thread, submissions, history.effective, now);
  if (!thread && newThreadIntent)
    middleRegion->conversation().setEmptyMessage(
        QStringLiteral("Send a message to create this thread."));
  else if (thread)
    middleRegion->conversation().setEmptyMessage(
        QStringLiteral("No materialized activity."));
  else
    middleRegion->conversation().setEmptyMessage(
        QStringLiteral("Conversation activity appears here."));
  middleRegion->conversation().reconcile(snapshot);

  if (thread) {
    middleRegion->setThreadHeading(
        text(thread->title), text(thread->cwd) + QStringLiteral("  |  ") +
                                 text(classifyStatus(thread->status).text));
  } else if (newThreadIntent) {
    middleRegion->setThreadHeading(QStringLiteral("New thread"),
                                   newThreadWorkspace.isEmpty()
                                       ? QDir::currentPath()
                                       : newThreadWorkspace);
  } else {
    middleRegion->setThreadHeading(QStringLiteral("Select a thread"), {});
  }
}

void ShellWidget::Impl::refreshSettings() {
  nlohmann::json canonical = nlohmann::json::object();
  nlohmann::json settingsUpdate = nlohmann::json::object();
  std::uint64_t settingsRevision = 0;
  std::string identity = "no-thread";
  if (const ThreadPresentation *thread = model.thread(selectedThreadId)) {
    identity = thread->id;
    settingsUpdate = thread->latestSettingsUpdate;
    settingsRevision = thread->settingsRevision;
    canonical = thread->raw;
    const auto settings = thread->domains.find("thread.settings.changed");
    if (settings != thread->domains.end() && settings->second.is_object()) {
      nlohmann::json update = settings->second;
      if (update.contains("threadSettings") &&
          update["threadSettings"].is_object())
        update = update["threadSettings"];
      if (update.contains("effort"))
        canonical.erase("reasoningEffort");
      if (update.contains("sandboxPolicy"))
        canonical.erase("sandbox");
      canonical.merge_patch(update);
    }
  } else if (newThreadIntent) {
    identity = DraftThreadId;
    canonical["cwd"] = (newThreadWorkspace.isEmpty() ? QDir::currentPath()
                                                     : newThreadWorkspace)
                           .toStdString();
  } else {
    canonical["cwd"] = QDir::currentPath().toStdString();
  }
  nlohmann::json profiles = nlohmann::json::array();
  const auto found =
      model.globalDomains().find("operation.permission-profiles.list");
  if (found != model.globalDomains().end())
    profiles = found->second;
  SettingsUiSnapshot next{std::move(identity),
                          std::move(canonical),
                          model.modelCatalog(),
                          std::move(profiles),
                          settingsRevision,
                          std::move(settingsUpdate)};
  if (settingsSnapshot && *settingsSnapshot == next)
    return;
  settingsSnapshot = std::move(next);
  const SettingsUiSnapshot &snapshot = *settingsSnapshot;
  middleRegion->composer().turnSettings()->setContext(
      snapshot.identity, snapshot.canonical, snapshot.modelCatalog,
      snapshot.permissionProfiles, snapshot.settingsRevision,
      snapshot.settingsUpdate);
}

void ShellWidget::Impl::refreshStatus() {
  const ConnectionPresentation &connection = model.connection();
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  std::size_t runningAgents = 0;
  if (thread) {
    for (const auto &[id, agent] : thread->agents) {
      static_cast<void>(id);
      if (isActiveStatus(agent.status))
        ++runningAgents;
    }
  }
  const bool active = model.activeTurnId(selectedThreadId).has_value();
  const std::size_t selectedPending = static_cast<std::size_t>(std::count_if(
      model.pendingRequestPresentations().begin(),
      model.pendingRequestPresentations().end(),
      [this](const auto &entry) {
        return entry.second.threadId == selectedThreadId;
      }));
  const std::size_t totalPending = model.pendingRequestCount();
  QString selectedTransport;
  const std::string selectedKey = stringValue(connection.settings, "selected");
  const nlohmann::json available =
      connection.settings.value("available", nlohmann::json::array());
  if (available.is_array()) {
    for (const auto &entry : available) {
      if (stringValue(entry, "key") == selectedKey) {
        selectedTransport = text(stringValue(entry, "label"));
        break;
      }
    }
  }
  QString workspace = QStringLiteral("No workspace");
  QString threadContext = QStringLiteral("No thread context");
  QString agentActivity = QStringLiteral("No agent activity");
  if (thread) {
    workspace = text(thread->cwd);
    threadContext =
        QStringLiteral("%1  |  %2")
            .arg(text(thread->title),
                 text(classifyStatus(thread->status).text));
    if (!thread->agents.empty()) {
      agentActivity = QStringLiteral("%1 agents  |  %2 active")
                          .arg(static_cast<qulonglong>(thread->agents.size()))
                          .arg(static_cast<qulonglong>(runningAgents));
    }
  } else if (newThreadIntent) {
    workspace = text(middleRegion->composer().turnSettings()->workspace(
        QDir::currentPath().toStdString()));
    threadContext = QStringLiteral("New thread");
  }
  StatusUiSnapshot next{connection.connected,
                        connection.retrying,
                        connection.role,
                        std::move(selectedTransport),
                        std::move(workspace),
                        std::move(threadContext),
                        std::move(agentActivity),
                        active,
                        selectedPending,
                        totalPending};
  if (statusSnapshot && *statusSnapshot == next)
    return;
  statusSnapshot = std::move(next);
  const StatusUiSnapshot &snapshot = *statusSnapshot;
  QString dotStyle;
  QString dotTip;
  if (snapshot.connected) {
    dotStyle = QStringLiteral("background:#18865e;border-radius:5px;");
    dotTip = QStringLiteral("Connected");
  } else if (snapshot.retrying) {
    dotStyle = QStringLiteral("background:#a85d0c;border-radius:5px;");
    dotTip = QStringLiteral("Disconnected, retrying");
  } else {
    dotStyle = QStringLiteral("background:#c43d4d;border-radius:5px;");
    dotTip = QStringLiteral("Disconnected");
  }
  connectionStatusDot->setStyleSheet(dotStyle);
  connectionStatusDot->setToolTip(dotTip);
  connectionButton->setText(snapshot.selectedTransport.isEmpty()
                                ? QStringLiteral("Connection")
                                : snapshot.selectedTransport);
  connectionButton->setToolTip(
      snapshot.connected ? QStringLiteral("Connected bridge transport")
                         : QStringLiteral("Disconnected bridge transport"));
  connectAction->setEnabled(!snapshot.connected);
  disconnectAction->setEnabled(snapshot.connected);
  reconnectAction->setEnabled(snapshot.connected);
  controllerLabel->setText(snapshot.role.empty() ? QStringLiteral("No role")
                                                 : text(snapshot.role));
  controllerButton->setText(snapshot.role == "controller"
                                ? QStringLiteral("Release control")
                                : QStringLiteral("Claim control"));
  controllerButton->setEnabled(snapshot.connected);

  requestButton->setText(
      QStringLiteral("Requests (%1)")
          .arg(static_cast<qulonglong>(snapshot.totalPending)));
  requestButton->setVisible(snapshot.totalPending != 0);
  middleRegion->composer().setAttentionVisible(snapshot.selectedPending != 0);

  threadContextStatus->setText(snapshot.threadContext);
  agentActivityStatus->setText(snapshot.agentActivity);
  workspaceBreadcrumb->setToolTip(snapshot.workspace);
  workspaceBreadcrumb->setText(workspaceBreadcrumb->fontMetrics().elidedText(
      snapshot.workspace, Qt::ElideMiddle,
      workspaceBreadcrumb->maximumWidth()));

  const bool canSubmit = snapshot.connected && snapshot.role == "controller";
  middleRegion->composer().setActiveTurn(snapshot.active);
  middleRegion->composer().setCanSubmit(canSubmit);
  middleRegion->composer().setSettingsEnabled(canSubmit && !snapshot.active);
}

void ShellWidget::Impl::hydrateHistoricalChildren(
    const std::string &parentThreadId) {
  const ThreadPresentation *thread = model.thread(parentThreadId);
  if (!thread)
    return;
  for (const std::string &childThreadId : thread->childThreadOrder) {
    const ChildThreadOwnership *ownership =
        model.childOwnership(childThreadId);
    if (!ownership || ownership->parentThreadId != parentThreadId)
      continue;
    const auto agent = thread->agents.find(ownership->agentId);
    if (agent == thread->agents.end() ||
        !isActiveStatus(agent->second.status))
      continue;
    // Historical child hydration shares the same monotonic read boundary as
    // user-selected threads, so a pre-reconnect result cannot replace newer
    // child/agent presentation state.
    readThread(childThreadId);
  }
}

void ShellWidget::Impl::selectThread(std::string threadId) {
  if (threadId.empty())
    return;
  if (threadId == selectedThreadId) {
    ensureThreadHydrated(threadId);
    ensureThreadSettingsHydrated(threadId);
    return;
  }
  selectedThreadId = std::move(threadId);
  newThreadIntent = false;
  newThreadOptions = nlohmann::json::object();
  newThreadName.clear();
  newThreadWorkspace.clear();
  historyWindows.try_emplace(selectedThreadId);
  ensureThreadHydrated(selectedThreadId);
  ensureThreadSettingsHydrated(selectedThreadId);
  render();
}

void ShellWidget::Impl::beginNewThread() {
  if (newThreadCreationInFlight) {
    showNotice(QStringLiteral("The current new thread is still being created."),
               false);
    return;
  }
  const QString initial =
      text(middleRegion->composer().turnSettings()->workspace(
          QDir::currentPath().toStdString()));
  NewThreadDialog dialog(initial, owner);
  if (dialog.exec() != QDialog::Accepted)
    return;
  const NewThreadDraft draft = dialog.draft();
  prompts.clearThread(DraftThreadId);
  selectedThreadId.clear();
  newThreadIntent = true;
  newThreadName = draft.name;
  newThreadWorkspace = draft.workspace;
  newThreadOptions = nlohmann::json::object();
  if (!draft.baseInstructions.isEmpty())
    newThreadOptions["baseInstructions"] = draft.baseInstructions.toStdString();
  if (!draft.developerInstructions.isEmpty())
    newThreadOptions["developerInstructions"] =
        draft.developerInstructions.toStdString();
  if (draft.ephemeral)
    newThreadOptions["ephemeral"] = true;
  settingsSnapshot.reset();
  middleRegion->composer().clearDraft();
  middleRegion->composer().turnSettings()->setWorkspace(draft.workspace);
  middleRegion->composer().promptEditor()->setFocus();
  render();
}

void ShellWidget::Impl::readThread(const std::string &threadId, bool forced) {
  if (threadId.empty())
    return;
  ThreadRuntimeState &runtime = runtimeByThread[threadId];
  if (runtime.resumeInFlight)
    return;
  if (!forced) {
    if (runtime.hydration == Hydration::InFlight ||
        runtime.hydration == Hydration::Hydrated ||
        runtime.hydration == Hydration::Failed)
      return;
  }
  runtime.hydration = Hydration::InFlight;
  const auto token = alive;
  const std::uint64_t revision = nextReadRevision++;
  runtime.readRevision = revision;
  session.readThread(threadId, [this, token, threadId,
                                revision](const nlohmann::json &result) {
    if (!*token)
      return;
    const auto current = runtimeByThread.find(threadId);
    if (current == runtimeByThread.end() ||
        current->second.readRevision != revision) {
      const std::string correlationId = stringValue(result, "correlationId");
      if (!correlationId.empty())
        staleReadResultCorrelations.insert(correlationId);
      return;
    }
    ThreadRuntimeState &runtime = current->second;
    if (result.value("ok", false)) {
      runtime.hydration = Hydration::Hydrated;
      if (runtime.settingsHydration == SettingsHydration::WaitingForRead)
        resumeThreadForSettings(threadId);
      QTimer::singleShot(0, owner,
                         [this, threadId] { dispatchNextPrompt(threadId); });
      return;
    }
    // A non-forced hydration is attempted once per connection generation.
    // Explicit Reload bypasses this terminal state, while a new generation
    // clears it together with the other hydration bookkeeping.
    runtime.hydration = Hydration::Failed;
    if (runtime.settingsHydration == SettingsHydration::WaitingForRead)
      runtime.settingsHydration = SettingsHydration::Failed;
    const std::string message =
        safeMessage(result.value("error", nlohmann::json::object()));
    const QString displayed =
        text(message.empty() ? std::string("Thread loading failed") : message);
    static_cast<void>(prompts.failQueued(threadId, displayed));
    showNotice(displayed);
    render();
  });
}

void ShellWidget::Impl::ensureThreadSettingsHydrated(
    const std::string &threadId) {
  if (threadId.empty() || !model.connection().connected ||
      model.connection().role != "controller")
    return;
  ThreadRuntimeState &runtime = runtimeByThread[threadId];
  if (runtime.settingsHydration == SettingsHydration::WaitingForRead ||
      runtime.settingsHydration == SettingsHydration::InFlight ||
      runtime.settingsHydration == SettingsHydration::Hydrated)
    return;
  if (runtime.hydration != Hydration::Hydrated) {
    runtime.settingsHydration = SettingsHydration::WaitingForRead;
    ensureThreadHydrated(threadId);
    return;
  }
  resumeThreadForSettings(threadId);
}

void ShellWidget::Impl::resumeThreadForSettings(
    const std::string &threadId) {
  ThreadRuntimeState &runtime = runtimeByThread[threadId];
  if (runtime.resumeInFlight)
    return;
  runtime.settingsHydration = SettingsHydration::InFlight;
  const auto token = alive;
  session.resumeThread(
      threadId, {{"excludeTurns", true}},
      [this, token, threadId](const nlohmann::json &result) {
        if (!*token)
          return;
        const auto found = runtimeByThread.find(threadId);
        if (found == runtimeByThread.end())
          return;
        ThreadRuntimeState &runtime = found->second;
        if (!result.value("ok", false)) {
          runtime.settingsHydration = SettingsHydration::Failed;
          if (selectedThreadId == threadId) {
            const std::string message =
                safeMessage(result.value("error", nlohmann::json::object()));
            showNotice(text(message.empty()
                                ? std::string("Thread settings refresh failed")
                                : message));
          }
        } else {
          runtime.settingsHydration = SettingsHydration::Hydrated;
          runtime.hydration = Hydration::Hydrated;
          runtime.operationReady = true;
        }
        QTimer::singleShot(0, owner,
                           [this, threadId] { dispatchNextPrompt(threadId); });
      });
}

void ShellWidget::Impl::ensureThreadHydrated(const std::string &threadId) {
  if (threadId.empty() || !model.connection().connected)
    return;
  const auto found = runtimeByThread.find(threadId);
  if (found != runtimeByThread.end() &&
      (found->second.hydration == Hydration::Hydrated ||
       found->second.hydration == Hydration::InFlight))
    return;
  readThread(threadId);
}

void ShellWidget::Impl::renameThread(const std::string &threadId) {
  const ThreadPresentation *thread = model.thread(threadId);
  if (!thread)
    return;
  bool accepted = false;
  const QString name =
      QInputDialog::getText(owner, QStringLiteral("Rename thread"),
                            QStringLiteral("Name"), QLineEdit::Normal,
                            text(thread->title), &accepted)
          .trimmed();
  if (accepted && !name.isEmpty())
    session.renameThread(threadId, name.toStdString());
}

void ShellWidget::Impl::forkThread(const std::string &threadId) {
  if (threadId.empty())
    return;
  const auto token = alive;
  session.forkThread(threadId, nlohmann::json::object(),
                     [this, token](const nlohmann::json &result) {
                       if (!*token || !result.value("ok", false))
                         return;
                       const std::string id = stringValue(
                           result.value("data", nlohmann::json::object())
                               .value("thread", nlohmann::json::object()),
                           "id");
                       if (!id.empty())
                         selectThread(id);
                     });
}

void ShellWidget::Impl::toggleThreadArchive(const std::string &threadId) {
  const ThreadPresentation *thread = model.thread(threadId);
  if (!thread)
    return;
  if (thread->archived)
    session.unarchiveThread(threadId);
  else
    session.archiveThread(threadId);
}

void ShellWidget::Impl::deleteThread(const std::string &threadId) {
  if (threadId.empty())
    return;
  if (QMessageBox::question(owner, QStringLiteral("Delete thread"),
                            QStringLiteral("Delete the selected thread?"),
                            QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel) == QMessageBox::Yes)
    session.deleteThread(threadId);
}

bool ShellWidget::Impl::submitPrompt(QString prompt,
                                     std::vector<AttachmentDraft> attachments) {
  prompt = prompt.trimmed();
  if (prompt.isEmpty())
    return false;
  prompt = middle::promptWithFileLinks(std::move(prompt), attachments);
  const std::string visiblySelected =
      middleRegion->threads().visiblySelectedThreadId();
  if (!visiblySelected.empty() && visiblySelected != selectedThreadId) {
    if (!model.thread(visiblySelected)) {
      showNotice(QStringLiteral("The visibly selected thread is no longer "
                                "available. Your message was not sent."));
      return false;
    }
    selectThread(visiblySelected);
  }

  std::string destination = selectedThreadId;
  const ThreadPresentation *thread = model.thread(destination);
  if (destination.empty()) {
    if (!newThreadIntent) {
      showNotice(QStringLiteral("No destination thread is selected. Your "
                                "message was not sent; select a thread or use "
                                "New thread."));
      middleRegion->composer().promptEditor()->setFocus();
      return false;
    }
    destination = DraftThreadId;
    thread = nullptr;
  }

  if (destination != DraftThreadId) {
    const auto runtime = runtimeByThread.find(destination);
    if (runtime != runtimeByThread.end() &&
        runtime->second.hydration == Hydration::Failed) {
      showNotice(QStringLiteral("Thread loading failed. Reload the thread "
                                "before sending; your message was not sent."));
      middleRegion->composer().promptEditor()->setFocus();
      return false;
    }
  }

  const auto activeTurn = destination == DraftThreadId
                              ? std::optional<std::string>{}
                              : model.activeTurnId(destination);
  const std::uint64_t submissionId =
      prompts.admit(destination, prompt, std::move(attachments),
                    middleRegion->composer().turnSettings()->turnStartOptions(),
                    thread, activeTurn, QDateTime::currentMSecsSinceEpoch());
  static_cast<void>(submissionId);

  // Admission is a synchronous UI fact. Transport dispatch is queued below so
  // this awaiting projection is committed without forcing paint reentrancy.
  middleRegion->conversation().prepareForLocalPromptAdmission();
  renderConversation();

  if (destination == DraftThreadId)
    startThreadForDraft();
  else
    dispatchNextPrompt(destination);
  return true;
}

void ShellWidget::Impl::startThreadForDraft() {
  if (newThreadCreationInFlight || prompts.submissions(DraftThreadId).empty())
    return;
  newThreadCreationInFlight = true;
  nlohmann::json options =
      middleRegion->composer().turnSettings()->threadStartOptions();
  options.update(newThreadOptions);
  options["cwd"] = middleRegion->composer().turnSettings()->workspace(
      QDir::currentPath().toStdString());
  const QString requestedName = newThreadName;
  const auto token = alive;
  session.createThread(std::move(options), [this, token, requestedName](
                                               const nlohmann::json &result) {
    if (!*token)
      return;
    newThreadCreationInFlight = false;
    if (!result.value("ok", false)) {
      const std::string message =
          safeMessage(result.value("error", nlohmann::json::object()));
      const QString error = text(
          message.empty() ? std::string("Thread creation failed") : message);
      const auto pending = prompts.submissions(DraftThreadId);
      std::vector<std::uint64_t> ids;
      for (const auto &submission : pending)
        ids.push_back(submission.id);
      for (const std::uint64_t id : ids)
        static_cast<void>(prompts.fail(DraftThreadId, id, error));
      showNotice(error);
      render();
      return;
    }
    const std::string threadId =
        stringValue(result.value("data", nlohmann::json::object())
                        .value("thread", nlohmann::json::object()),
                    "id");
    if (threadId.empty()) {
      const QString error =
          QStringLiteral("Thread creation returned no thread identifier");
      const auto pending = prompts.submissions(DraftThreadId);
      std::vector<std::uint64_t> ids;
      for (const auto &submission : pending)
        ids.push_back(submission.id);
      for (const std::uint64_t id : ids)
        static_cast<void>(prompts.fail(DraftThreadId, id, error));
      showNotice(error);
      render();
      return;
    }

    if (!prompts.reassignThread(DraftThreadId, threadId)) {
      showNotice(QStringLiteral("Could not attach the draft prompts to "
                                "the created thread."));
      render();
      return;
    }
    ThreadRuntimeState &runtime = runtimeByThread[threadId];
    runtime.hydration = Hydration::Hydrated;
    runtime.settingsHydration = SettingsHydration::Hydrated;
    runtime.operationReady = true;
    const bool viewingDraft = selectedThreadId.empty() && newThreadIntent;
    if (viewingDraft) {
      selectedThreadId = threadId;
      newThreadIntent = false;
    }
    newThreadOptions = nlohmann::json::object();
    newThreadName.clear();
    newThreadWorkspace.clear();
    settingsSnapshot.reset();
    if (!requestedName.isEmpty())
      session.renameThread(threadId, requestedName.toStdString());
    render();
    QTimer::singleShot(0, owner,
                       [this, threadId] { dispatchNextPrompt(threadId); });
  });
}

void ShellWidget::Impl::dispatchNextPrompt(const std::string &threadId) {
  if (threadId.empty() || !model.connection().connected)
    return;
  auto runtime = runtimeByThread.find(threadId);
  if (runtime != runtimeByThread.end() &&
      runtime->second.settingsHydration == SettingsHydration::InFlight)
    return;
  const auto submissions = prompts.submissions(threadId);
  if (std::ranges::none_of(
          submissions, [](const middle::PromptSubmission &submission) {
            return submission.state == middle::PromptState::Queued;
          }))
    return;
  if (runtime != runtimeByThread.end() && runtime->second.resumeInFlight)
    return;
  if (runtime == runtimeByThread.end() ||
      runtime->second.hydration != Hydration::Hydrated) {
    ensureThreadHydrated(threadId);
    return;
  }
  if (prompts.hasInFlight(threadId))
    return;
  const ThreadPresentation *thread = model.thread(threadId);
  if (!runtime->second.operationReady && thread &&
      thread->status == "notLoaded") {
    resumePromptQueue(threadId);
    return;
  }
  if (runtime->second.dispatchScheduled)
    return;
  runtime->second.dispatchScheduled = true;

  // The admitted card already presents the awaiting state. Queueing transport
  // gives Qt one normal paint turn, then samples start-versus-steer at the
  // actual send boundary without a forced repaint or reentrant event drain.
  const std::uint64_t generation = observedConnectionGeneration;
  QTimer::singleShot(0, owner, [this, threadId, generation] {
    const auto runtime = runtimeByThread.find(threadId);
    if (runtime == runtimeByThread.end())
      return;
    runtime->second.dispatchScheduled = false;
    if (observedConnectionGeneration != generation)
      return;
    if (!model.connection().connected || runtime->second.resumeInFlight)
      return;
    const ThreadPresentation *thread = model.thread(threadId);
    if (runtime->second.hydration != Hydration::Hydrated ||
        (!runtime->second.operationReady && thread &&
         thread->status == "notLoaded")) {
      dispatchNextPrompt(threadId);
      return;
    }
    const auto dispatch =
        prompts.beginNext(threadId, model.activeTurnId(threadId));
    if (dispatch)
      dispatchPrompt(*dispatch);
  });
}

void ShellWidget::Impl::dispatchPrompt(middle::PromptDispatch dispatch) {
  nlohmann::json input =
      nlohmann::json::array({{{"type", "text"},
                              {"text", dispatch.prompt.toStdString()},
                              {"text_elements", nlohmann::json::array()}}});
  for (const AttachmentDraft &attachment : dispatch.attachments) {
    if (attachment.mimeType.startsWith(QStringLiteral("image/")))
      input.push_back(
          {{"type", "localImage"}, {"path", attachment.path.toStdString()}});
    else if (attachment.mimeType.startsWith(QStringLiteral("audio/")))
      input.push_back(
          {{"type", "localAudio"}, {"path", attachment.path.toStdString()}});
  }

  const std::string threadId = dispatch.threadId;
  const std::uint64_t submissionId = dispatch.id;
  const auto token = alive;
  auto completed = [this, token, threadId,
                    submissionId](const nlohmann::json &result) {
    if (*token)
      completePrompt(threadId, submissionId, result);
  };
  if (dispatch.expectedTurnId) {
    session.request("turn.steer",
                    {{"threadId", dispatch.threadId},
                     {"expectedTurnId", *dispatch.expectedTurnId},
                     {"clientUserMessageId", dispatch.clientUserMessageId},
                     {"input", std::move(input)}},
                    std::move(completed));
  } else {
    dispatch.turnOptions["clientUserMessageId"] = dispatch.clientUserMessageId;
    session.startTurn(dispatch.threadId, std::move(input),
                      std::move(dispatch.turnOptions), std::move(completed));
  }
}

void ShellWidget::Impl::resumePromptQueue(const std::string &threadId) {
  ThreadRuntimeState &runtime = runtimeByThread[threadId];
  if (runtime.resumeInFlight)
    return;
  runtime.resumeInFlight = true;
  const auto token = alive;
  session.resumeThread(
      threadId, nlohmann::json::object(),
      [this, token, threadId](const nlohmann::json &result) {
        if (!*token)
          return;
        const auto found = runtimeByThread.find(threadId);
        if (found == runtimeByThread.end())
          return;
        ThreadRuntimeState &runtime = found->second;
        runtime.resumeInFlight = false;
        if (!result.value("ok", false)) {
          runtime.settingsHydration = SettingsHydration::Failed;
          const std::string message =
              safeMessage(result.value("error", nlohmann::json::object()));
          const QString displayed = text(
              message.empty() ? std::string("Thread resume failed") : message);
          static_cast<void>(prompts.failQueued(threadId, displayed));
          showNotice(displayed);
          render();
          return;
        }
        runtime.hydration = Hydration::Hydrated;
        runtime.settingsHydration = SettingsHydration::Hydrated;
        runtime.operationReady = true;
        QTimer::singleShot(0, owner,
                           [this, threadId] { dispatchNextPrompt(threadId); });
      });
}

void ShellWidget::Impl::completePrompt(const std::string &threadId,
                                       std::uint64_t submissionId,
                                       const nlohmann::json &result) {
  if (attemptThreadRecovery(threadId, submissionId, result))
    return;
  const auto runtime = runtimeByThread.find(threadId);
  if (runtime != runtimeByThread.end())
    runtime->second.recoveryAttemptedSubmissions.erase(submissionId);
  if (result.value("ok", false)) {
    if (runtime != runtimeByThread.end())
      runtime->second.operationReady = true;
    static_cast<void>(prompts.acknowledge(threadId, submissionId,
                                          resultTurnId(result),
                                          QDateTime::currentMSecsSinceEpoch()));
    scheduleAcceptedTransition(threadId, submissionId);
  } else {
    const std::string message =
        safeMessage(result.value("error", nlohmann::json::object()));
    const QString displayed =
        text(message.empty() ? std::string("Submission failed") : message);
    static_cast<void>(prompts.fail(threadId, submissionId, displayed));
    showNotice(text(message.empty() ? std::string("Turn submission failed")
                                    : message));
  }
  render();
  QTimer::singleShot(0, owner,
                     [this, threadId] { dispatchNextPrompt(threadId); });
}

bool ShellWidget::Impl::attemptThreadRecovery(const std::string &threadId,
                                              std::uint64_t submissionId,
                                              const nlohmann::json &result) {
  if (!isThreadNotFoundResult(result))
    return false;
  const auto found = runtimeByThread.find(threadId);
  if (found == runtimeByThread.end())
    return false;
  ThreadRuntimeState &runtime = found->second;
  if (!runtime.recoveryAttemptedSubmissions.insert(submissionId).second)
    return false;
  if (!prompts.requeue(threadId, submissionId))
    return false;
  runtime.hydration = Hydration::NotHydrated;
  runtime.operationReady = false;
  render();
  runtime.resumeInFlight = true;
  const auto token = alive;
  session.resumeThread(
      threadId, nlohmann::json::object(),
      [this, token, threadId](const nlohmann::json &resumeResult) {
        if (!*token)
          return;
        const auto found = runtimeByThread.find(threadId);
        if (found == runtimeByThread.end())
          return;
        ThreadRuntimeState &runtime = found->second;
        runtime.resumeInFlight = false;
        if (!resumeResult.value("ok", false)) {
          runtime.settingsHydration = SettingsHydration::Failed;
          const std::string message = safeMessage(
              resumeResult.value("error", nlohmann::json::object()));
          const QString displayed =
              text(message.empty() ? std::string("Thread recovery failed")
                                   : message);
          static_cast<void>(prompts.failQueued(threadId, displayed));
          showNotice(displayed);
          render();
          return;
        }
        runtime.hydration = Hydration::Hydrated;
        runtime.settingsHydration = SettingsHydration::Hydrated;
        runtime.operationReady = true;
        QTimer::singleShot(0, owner,
                           [this, threadId] { dispatchNextPrompt(threadId); });
      });
  return true;
}

void ShellWidget::Impl::scheduleAcceptedTransition(const std::string &threadId,
                                                   std::uint64_t submissionId) {
  const middle::PromptSubmission *submission =
      prompts.submission(threadId, submissionId);
  if (!submission || submission->state != middle::PromptState::Accepted)
    return;
  const qint64 elapsed =
      QDateTime::currentMSecsSinceEpoch() - submission->acceptedAtMilliseconds;
  const int remaining = static_cast<int>(std::max<qint64>(
      1, middle::AcknowledgementTransitionMilliseconds - elapsed));
  QTimer::singleShot(
      remaining, Qt::PreciseTimer, owner, [this, threadId, submissionId] {
        const middle::PromptSubmission *current =
            prompts.submission(threadId, submissionId);
        if (!current || current->state != middle::PromptState::Accepted)
          return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (current->acceptedTransitionActive(now)) {
          scheduleAcceptedTransition(threadId, submissionId);
          return;
        }
        if (const ThreadPresentation *thread = model.thread(threadId))
          prompts.reconcile(threadId, *thread, now);
        render();
      });
}

void ShellWidget::Impl::chooseAttachments() {
  const QString initial =
      text(middleRegion->composer().turnSettings()->workspace(
          QDir::currentPath().toStdString()));
  FileSelectionDialog dialog(FileSelectionDialog::Mode::Attachments, initial,
                             middleRegion->composer().attachments(), owner);
  if (dialog.exec() == QDialog::Accepted)
    middleRegion->composer().setAttachments(dialog.selectedAttachments());
}

void ShellWidget::Impl::interruptTurn() {
  const auto turn = model.activeTurnId(selectedThreadId);
  if (turn)
    session.interruptTurn(selectedThreadId, *turn);
}

void ShellWidget::Impl::respondToFirstPending(bool approve) {
  const auto &pending = model.pendingRequestPresentations();
  const auto request = std::ranges::find_if(pending, [this](const auto &entry) {
    return entry.second.threadId == selectedThreadId;
  });
  if (request == pending.end())
    return;
  if (approve)
    reviewPending(request->first);
  else
    rejectPending(request->first);
}

void ShellWidget::Impl::reviewPending(const std::string &requestKey) {
  const auto request = model.pendingRequestPresentations().find(requestKey);
  if (request == model.pendingRequestPresentations().end())
    return;
  const auto response = PendingRequestDialog::present(request->second, owner);
  if (!response)
    return;
  session.respondToServerRequest(nlohmann::json::parse(requestKey),
                                 response->result, response->error);
}

void ShellWidget::Impl::rejectPending(const std::string &requestKey) {
  const auto request = model.pendingRequestPresentations().find(requestKey);
  if (request == model.pendingRequestPresentations().end())
    return;
  PendingRequestResponse response =
      PendingRequestDialog::negativeResponse(request->second);
  session.respondToServerRequest(nlohmann::json::parse(requestKey),
                                 std::move(response.result),
                                 std::move(response.error));
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
  if (impl && impl->middleRegion->routeScrollEvent(watched, event))
    return true;
  return QWidget::eventFilter(watched, event);
}

} // namespace codexui::codex
