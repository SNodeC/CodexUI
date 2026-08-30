// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ShellWidget.h"

#include "codex/ConnectionDialog.h"
#include "codex/FileSelectionDialog.h"
#include "codex/FrontendSession.h"
#include "codex/NewThreadDialog.h"
#include "codex/PendingRequestDialog.h"
#include "codex/PendingRequestPolicy.h"
#include "codex/TurnSettingsWidget.h"
#include "codex/UiSession.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/MiddleRegionWidget.h"
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
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace codexui::codex {
namespace {

constexpr auto DraftThreadId = "draft:new-thread";

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string utf8(const QString &value) {
  return value.toUtf8().toStdString();
}

QString lastActivityText(std::int64_t timestamp) {
  const QDateTime activity =
      QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime();
  const QDateTime now = QDateTime::currentDateTime();
  const QString formatted = activity.date() == now.date()
                                ? activity.toString(QStringLiteral("HH:mm:ss"))
                                : activity.toString(
                                      QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  return QStringLiteral("Last activity: %1").arg(formatted);
}

const ui::ThreadListRow *findThread(const ui::ThreadListRow &row,
                                    std::string_view id) {
  if (row.id == id)
    return &row;
  for (const ui::ThreadListRow &child : row.children) {
    if (const ui::ThreadListRow *found = findThread(child, id))
      return found;
  }
  return nullptr;
}

const ui::ThreadListRow *findThread(const ui::ThreadListSnapshot &snapshot,
                                    std::string_view id) {
  for (const ui::ThreadListRow &root : snapshot.roots) {
    if (const ui::ThreadListRow *found = findThread(root, id))
      return found;
  }
  return nullptr;
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
  label->setToolTip(value);
  label->setText(label->fontMetrics().elidedText(value, Qt::ElideMiddle,
                                                 label->maximumWidth()));
}

QString statusToneColor(QStringView tone) {
  if (tone == QStringLiteral("active"))
    return QStringLiteral("#2f6feb");
  if (tone == QStringLiteral("success"))
    return QStringLiteral("#18865e");
  if (tone == QStringLiteral("warning"))
    return QStringLiteral("#a85d0c");
  if (tone == QStringLiteral("danger"))
    return QStringLiteral("#c43d4d");
  return QStringLiteral("#98a2b3");
}

void setStatusTone(QFrame *dot, QLabel *label, const QString &tone) {
  dot->setStyleSheet(QStringLiteral("background:%1;border-radius:5px;")
                         .arg(statusToneColor(tone)));
  if (label->property("tone").toString() == tone)
    return;
  label->setProperty("tone", tone);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->update();
}

QFrame *statusDot() {
  auto *dot = new QFrame;
  dot->setFixedSize(10, 10);
  dot->setStyleSheet(QStringLiteral("background:#98a2b3;border-radius:5px;"));
  return dot;
}

} // namespace

struct ShellWidget::Impl final {
  Impl(ShellWidget *owner, FrontendSession &session)
      : owner(owner), session(session),
        uiSession(session.presentationClient(), utf8(QDir::currentPath())),
        alive(std::make_shared<bool>(true)) {
    buildUi();
    connectUi();
    const auto token = alive;
    uiSession.setChangedHandler([this, token] {
      if (*token)
        scheduleRender();
    });
    uiSession.setWakeupHandler([this, token](std::int64_t atMilliseconds) {
      if (*token)
        scheduleLogicWakeup(atMilliseconds);
    });
    uiSession.setProtocolFrameObserver(
        [this, token](const nlohmann::json &frame) {
          if (*token)
            middleRegion->inspector().appendProtocolFrame(frame);
        });
    session.setEventHandler([this, token](const nlohmann::json &frame) {
      if (*token)
        uiSession.onPresentationFrame(frame);
    });
    session.setActivityHandler([this, token](const std::string &threadId) {
      if (*token)
        uiSession.noteThreadActivity(threadId);
    });
    render();
  }

  ~Impl() {
    *alive = false;
    uiSession.setChangedHandler({});
    uiSession.setWakeupHandler({});
    uiSession.setProtocolFrameObserver({});
    session.setEventHandler({});
    session.setActivityHandler({});
    if (qApp)
      qApp->removeEventFilter(owner);
  }

  void buildUi();
  void connectUi();
  void scheduleLogicWakeup(std::int64_t atMilliseconds);
  void scheduleRender();
  void render();
  void renderStatus(const UiSessionView &view);
  void synchronizeOptimisticThread(
      const std::optional<UiOptimisticThreadView> &optimistic);
  void showNotice(QString message, bool error = true);
  void beginNewThreadDialog();
  void renameThreadDialog(const std::string &threadId);
  void confirmDeleteThread(const std::string &threadId);
  [[nodiscard]] bool submitPrompt(QString prompt,
                                  std::vector<AttachmentDraft> attachments);
  void chooseAttachments();
  void reviewPending(const std::string &requestKey);
  void acceptPending(const std::string &requestKey);
  void rejectPending(const std::string &requestKey);
  void respondToFirstPending(bool approve);
  [[nodiscard]] const UiPendingRequestView *
  pendingRequest(const std::string &requestKey) const;

  ShellWidget *owner = nullptr;
  FrontendSession &session;
  UiSession uiSession;
  std::shared_ptr<bool> alive;
  const UiSessionView *renderedView = nullptr;
  std::optional<UiSettingsView> settingsSnapshot;
  std::optional<UiStatusView> statusSnapshot;
  std::optional<UiPendingRequestView> attentionSnapshot;
  std::optional<UiOptimisticThreadView> optimisticSnapshot;
  std::optional<std::int64_t> scheduledLogicWakeup;
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
    if (!renderedView ||
        !renderedView->status.connectionSettings.is_object() ||
        renderedView->status.connectionSettings.empty()) {
      showNotice(QStringLiteral("Connection settings are not available yet."));
      return;
    }
    ConnectionDialog dialog(renderedView->status.connectionSettings, owner);
    if (dialog.exec() != QDialog::Accepted)
      return;
    uiSession.configureConnection(dialog.selection());
  });
  connectionMenu->addSeparator();
  connectAction = connectionMenu->addAction(
      QStringLiteral("Connect"), owner,
      [this] { uiSession.connectTransport(); });
  disconnectAction =
      connectionMenu->addAction(QStringLiteral("Disconnect"), owner,
                                [this] { uiSession.disconnectTransport(); });
  reconnectAction = connectionMenu->addAction(
      QStringLiteral("Reconnect"), owner,
      [this] { uiSession.reconnectTransport(); });
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
  statusLayout->setSpacing(10);
  auto *attribution = new QLabel(QStringLiteral(
      "<span style=\"color:#344054;font-weight:600\">"
      "© Volker Christian &amp; Codex</span>  |  "
      "<a style=\"color:#344054;text-decoration:none;font-weight:600\" "
      "href=\"https://github.com/SNodeC/CodexUI\">CodexUI</a>  •  "
      "<a style=\"color:#344054;text-decoration:none;font-weight:600\" "
      "href=\"https://github.com/SNodeC/AISuite\">AISuite</a>  •  "
      "<span style=\"color:#98a2b3;font-size:8pt\">Powered by</span> "
      "<a style=\"color:#344054;text-decoration:none;font-weight:600\" "
      "href=\"https://github.com/SNodeC/snode.c\">SNode.C</a>"));
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
  threadActions.refresh = [this] { uiSession.refreshThreads(); };
  threadActions.hide = [this] { middleRegion->showSidebar(false); };
  threadActions.select = [this](const std::string &id) {
    if (id == DraftThreadId && renderedView &&
        renderedView->newThreadIntent) {
      render();
      return;
    }
    uiSession.selectThread(id);
  };
  threadActions.reload =
      [this](const std::string &id) { uiSession.reloadThread(id); };
  threadActions.rename =
      [this](const std::string &id) { renameThreadDialog(id); };
  threadActions.fork =
      [this](const std::string &id) { uiSession.forkThread(id); };
  threadActions.toggleArchive = [this](const std::string &id) {
    uiSession.toggleThreadArchive(id);
  };
  threadActions.remove =
      [this](const std::string &id) { confirmDeleteThread(id); };
  middleRegion->threads().setActions(std::move(threadActions));

  middle::ComposerPane::Actions composerActions;
  composerActions.submit = [this](QString prompt,
                                  std::vector<AttachmentDraft> attachments) {
    return submitPrompt(std::move(prompt), std::move(attachments));
  };
  composerActions.stop = [this] { uiSession.interruptTurn(); };
  composerActions.attach = [this] { chooseAttachments(); };
  composerActions.accept = [this] { respondToFirstPending(true); };
  composerActions.review = [this] { respondToFirstPending(true); };
  composerActions.deny = [this] { respondToFirstPending(false); };
  middleRegion->composer().setActions(std::move(composerActions));

  middleRegion->conversation().setLoadMoreAction(
      [this] { uiSession.loadEarlierConversation(); });
  middleRegion->inspector().setRequestActions(
      [this](const std::string &id) { reviewPending(id); },
      [this](const std::string &id) { acceptPending(id); },
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
  connect(controllerButton, &QPushButton::clicked, owner,
          [this] { uiSession.toggleController(); });
  qApp->installEventFilter(owner);
}

void ShellWidget::Impl::showNotice(QString message, bool error) {
  middleRegion->showNotice(std::move(message), error);
}

void ShellWidget::Impl::scheduleLogicWakeup(std::int64_t atMilliseconds) {
  if (scheduledLogicWakeup && *scheduledLogicWakeup <= atMilliseconds)
    return;
  scheduledLogicWakeup = atMilliseconds;
  const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
  const std::int64_t requestedDelay =
      atMilliseconds > now ? atMilliseconds - now : 0;
  const int delay =
      requestedDelay > std::numeric_limits<int>::max()
          ? std::numeric_limits<int>::max()
          : static_cast<int>(requestedDelay);
  const auto token = alive;
  QTimer::singleShot(delay, Qt::PreciseTimer, owner,
                     [this, token, atMilliseconds] {
                       if (!*token || scheduledLogicWakeup != atMilliseconds)
                         return;
                       scheduledLogicWakeup.reset();
                       uiSession.tick();
                     });
}

void ShellWidget::Impl::scheduleRender() {
  if (renderScheduled)
    return;
  renderScheduled = true;
  const auto token = alive;
  // Stream deltas can arrive in bursts. Reconcile the toolkit once per display
  // interval while UiSession still observes every presentation frame.
  QTimer::singleShot(16, Qt::PreciseTimer, owner, [this, token] {
    if (!*token)
      return;
    renderScheduled = false;
    render();
  });
}

void ShellWidget::Impl::synchronizeOptimisticThread(
    const std::optional<UiOptimisticThreadView> &optimistic) {
  if (optimisticSnapshot == optimistic)
    return;

  if (!optimistic) {
    if (optimisticSnapshot) {
      const std::string id = optimisticSnapshot->threadId.empty()
                                 ? optimisticSnapshot->key
                                 : optimisticSnapshot->threadId;
      middleRegion->threads().confirmOptimisticThread(id);
    }
    optimisticSnapshot.reset();
    return;
  }

  if (!optimisticSnapshot || optimisticSnapshot->key != optimistic->key) {
    if (optimisticSnapshot) {
      const std::string previousId = optimisticSnapshot->threadId.empty()
                                         ? optimisticSnapshot->key
                                         : optimisticSnapshot->threadId;
      middleRegion->threads().confirmOptimisticThread(previousId);
    }
    middleRegion->threads().beginOptimisticThread(
        optimistic->key, optimistic->title, optimistic->workspace);
  }

  std::string renderedId = optimistic->key;
  if (!optimistic->threadId.empty()) {
    middleRegion->threads().promoteOptimisticThread(optimistic->key,
                                                     optimistic->threadId);
    renderedId = optimistic->threadId;
  }
  if (optimistic->phase == UiOptimisticThreadPhase::Failed)
    middleRegion->threads().failOptimisticThread(renderedId);
  else if (optimistic->phase == UiOptimisticThreadPhase::Confirmed)
    middleRegion->threads().confirmOptimisticThread(renderedId);
  optimisticSnapshot = optimistic;
}

void ShellWidget::Impl::render() {
  bool focusComposer = false;
  for (const UiEffect effect : uiSession.takeEffects()) {
    switch (effect) {
    case UiEffect::ClearComposerDraft:
      middleRegion->composer().clearDraft();
      break;
    case UiEffect::FocusComposer:
      focusComposer = true;
      break;
    case UiEffect::PrepareLocalPromptAdmission:
      middleRegion->conversation().prepareForLocalPromptAdmission();
      break;
    }
  }
  for (UiNotice &notice : uiSession.takeNotices())
    showNotice(text(notice.message), notice.error);

  const std::string fallbackWorkspace = utf8(QDir::currentPath());
  const std::string draftWorkspace =
      middleRegion->composer().turnSettings()->workspace(fallbackWorkspace);
  const std::string conversationKey = uiSession.conversationKey();
  const bool following =
      middleRegion->conversation().modeForThread(conversationKey) ==
      middle::ConversationView::Mode::Following;
  const UiSessionView &view =
      uiSession.refreshView(following, draftWorkspace);
  renderedView = &view;

  synchronizeOptimisticThread(view.optimisticThread);
  middleRegion->threads().refresh(view.threads);

  middleRegion->conversation().setEmptyMessage(
      text(view.conversation.emptyMessage));
  middleRegion->conversation().reconcile(view.conversation.snapshot);
  if (view.conversation.mode == UiConversationMode::Thread) {
    QStringList metadata;
    if (!view.conversation.workspace.empty())
      metadata << text(view.conversation.workspace);
    if (!view.conversation.status.empty())
      metadata << text(view.conversation.status);
    const QString activity = view.conversation.lastActivityAt
                                 ? lastActivityText(
                                       *view.conversation.lastActivityAt)
                                 : QString{};
    middleRegion->setThreadHeading(
        text(view.conversation.title),
        metadata.join(QStringLiteral("  |  ")), activity);
  } else if (view.conversation.mode == UiConversationMode::NewThread) {
    middleRegion->setThreadHeading(text(view.conversation.title),
                                   text(view.conversation.workspace));
  } else {
    middleRegion->setThreadHeading(text(view.conversation.title), {});
  }

  middleRegion->inspector().refresh(view.inspector);
  if (!settingsSnapshot || *settingsSnapshot != view.settings) {
    settingsSnapshot = view.settings;
    middleRegion->composer().turnSettings()->setContext(
        view.settings.identity, view.settings.canonical,
        view.settings.modelCatalog, view.settings.permissionProfiles,
        view.settings.settingsRevision, view.settings.settingsUpdate);
  }
  renderStatus(view);

  if (focusComposer)
    middleRegion->composer().promptEditor()->setFocus();
}

void ShellWidget::Impl::renderStatus(const UiSessionView &view) {
  if (statusSnapshot && *statusSnapshot == view.status &&
      attentionSnapshot == view.selectedPendingRequest)
    return;
  statusSnapshot = view.status;
  attentionSnapshot = view.selectedPendingRequest;
  const UiStatusView &status = view.status;

  QString dotStyle;
  QString dotTip;
  if (status.connected) {
    dotStyle = QStringLiteral("background:#18865e;border-radius:5px;");
    dotTip = QStringLiteral("Connected");
  } else if (status.retrying) {
    dotStyle = QStringLiteral("background:#a85d0c;border-radius:5px;");
    dotTip = QStringLiteral("Disconnected, retrying");
  } else {
    dotStyle = QStringLiteral("background:#c43d4d;border-radius:5px;");
    dotTip = QStringLiteral("Disconnected");
  }
  connectionStatusDot->setStyleSheet(dotStyle);
  connectionStatusDot->setToolTip(dotTip);
  connectionButton->setText(
      status.selectedTransport.empty() ? QStringLiteral("Connection")
                                       : text(status.selectedTransport));
  connectionButton->setToolTip(
      status.connected ? QStringLiteral("Connected bridge transport")
                       : QStringLiteral("Disconnected bridge transport"));
  connectAction->setEnabled(!status.connected);
  disconnectAction->setEnabled(status.connected);
  reconnectAction->setEnabled(status.connected);
  controllerButton->setText(status.role == "controller"
                                ? QStringLiteral("Release control")
                                : QStringLiteral("Claim control"));
  controllerButton->setEnabled(status.connected);

  requestButton->setText(
      QStringLiteral("Requests (%1)")
          .arg(static_cast<qulonglong>(status.totalPending)));
  requestButton->setVisible(status.totalPending != 0);
  if (view.selectedPendingRequest) {
    const UiPendingRequestView &request = *view.selectedPendingRequest;
    middleRegion->composer().setAttentionRequest(
        text(request.title), text(request.detail), request.supportsDirectAccept,
        text(request.directAcceptLabel));
  }
  middleRegion->composer().setAttentionVisible(
      view.selectedPendingRequest.has_value());
  middleRegion->composer().setAttentionEnabled(
      view.selectedPendingRequest && view.selectedPendingRequest->actionable);

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
  setStatusTone(globalStatusDot, globalStatusLabel, globalTone);
  setStatusLabelText(globalStatusLabel, globalStatus);

  const QString workspace = text(status.workspace);
  workspaceBreadcrumb->setToolTip(workspace);
  workspaceBreadcrumb->setText(
      workspaceBreadcrumb->fontMetrics().elidedText(
          workspace, Qt::ElideMiddle, workspaceBreadcrumb->maximumWidth()));

  middleRegion->composer().setActiveTurn(status.activeTurn);
  middleRegion->composer().setCanSubmit(status.canSubmit);
  middleRegion->composer().setSettingsEnabled(status.canEditSettings);
}

void ShellWidget::Impl::beginNewThreadDialog() {
  const QString fallback = QDir::currentPath();
  const QString initial =
      text(middleRegion->composer().turnSettings()->workspace(utf8(fallback)));
  NewThreadDialog dialog(initial, owner);
  if (dialog.exec() != QDialog::Accepted)
    return;
  const NewThreadDraft draft = dialog.draft();
  middleRegion->composer().turnSettings()->setWorkspace(draft.workspace);
  uiSession.beginNewThread(
      {utf8(draft.workspace), utf8(draft.name),
       utf8(draft.baseInstructions), utf8(draft.developerInstructions),
       draft.ephemeral});
}

void ShellWidget::Impl::renameThreadDialog(
    const std::string &threadId) {
  if (!renderedView || !renderedView->threads.canControl)
    return;
  const ui::ThreadListRow *thread =
      findThread(renderedView->threads, threadId);
  if (!thread)
    return;
  bool accepted = false;
  const QString name =
      QInputDialog::getText(owner, QStringLiteral("Rename thread"),
                            QStringLiteral("Name"), QLineEdit::Normal,
                            text(thread->title), &accepted)
          .trimmed();
  if (accepted && !name.isEmpty())
    uiSession.renameThread(threadId, utf8(name));
}

void ShellWidget::Impl::confirmDeleteThread(
    const std::string &threadId) {
  if (threadId.empty() || !renderedView ||
      !renderedView->threads.canControl)
    return;
  if (QMessageBox::question(owner, QStringLiteral("Delete thread"),
                            QStringLiteral("Delete the selected thread?"),
                            QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel) == QMessageBox::Yes)
    uiSession.deleteThread(threadId);
}

bool ShellWidget::Impl::submitPrompt(
    QString prompt, std::vector<AttachmentDraft> attachments) {
  prompt = prompt.trimmed();
  if (prompt.isEmpty())
    return false;
  TurnSettingsWidget *settings =
      middleRegion->composer().turnSettings();
  const std::string visibleThreadId =
      middleRegion->threads().visiblySelectedThreadId();
  UiPromptDraft draft;
  draft.text = utf8(prompt);
  draft.attachments = std::move(attachments);
  draft.turnStartOptions = settings->turnStartOptions();
  draft.threadStartOptions = settings->threadStartOptions();
  draft.workspace = settings->workspace(utf8(QDir::currentPath()));
  draft.visiblySelectedThreadId = visibleThreadId;
  const bool admitted = uiSession.submitPrompt(std::move(draft));
  if (admitted && !visibleThreadId.empty() &&
      visibleThreadId != DraftThreadId)
    middleRegion->threads().promotePromptedThread(visibleThreadId);
  return admitted;
}

void ShellWidget::Impl::chooseAttachments() {
  const QString initial =
      text(middleRegion->composer().turnSettings()->workspace(
          utf8(QDir::currentPath())));
  FileSelectionDialog dialog(FileSelectionDialog::Mode::Attachments, initial,
                             middleRegion->composer().attachments(), owner);
  if (dialog.exec() == QDialog::Accepted)
    middleRegion->composer().setAttachments(dialog.selectedAttachments());
}

const UiPendingRequestView *ShellWidget::Impl::pendingRequest(
    const std::string &requestKey) const {
  if (!renderedView)
    return nullptr;
  for (const UiPendingRequestView &request : renderedView->pendingRequests) {
    if (request.id == requestKey)
      return &request;
  }
  return nullptr;
}

void ShellWidget::Impl::respondToFirstPending(bool approve) {
  if (!renderedView || !renderedView->selectedPendingRequest)
    return;
  const std::string id = renderedView->selectedPendingRequest->id;
  if (approve)
    acceptPending(id);
  else
    rejectPending(id);
}

void ShellWidget::Impl::reviewPending(const std::string &requestKey) {
  const UiPendingRequestView *current = pendingRequest(requestKey);
  if (!current || !current->actionable)
    return;
  const UiPendingRequestView request = *current;
  const PendingRequestDescriptor presented{
      request.id, request.kind, request.threadId, request.generation,
      request.raw};
  const auto response = PendingRequestDialog::present(presented, owner);
  if (response)
    static_cast<void>(
        uiSession.resolvePending(request, std::move(*response)));
}

void ShellWidget::Impl::acceptPending(const std::string &requestKey) {
  const UiPendingRequestView *current = pendingRequest(requestKey);
  if (!current || !current->actionable)
    return;
  const UiPendingRequestView request = *current;
  if (!request.supportsDirectAccept) {
    reviewPending(requestKey);
    return;
  }
  static_cast<void>(uiSession.resolvePending(
      request,
      PendingRequestPolicy::positiveResponse(request.kind, request.raw)));
}

void ShellWidget::Impl::rejectPending(const std::string &requestKey) {
  const UiPendingRequestView *current = pendingRequest(requestKey);
  if (!current || !current->actionable)
    return;
  const UiPendingRequestView request = *current;
  static_cast<void>(uiSession.resolvePending(
      request,
      PendingRequestPolicy::negativeResponse(request.kind, request.raw)));
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
