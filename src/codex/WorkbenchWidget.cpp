// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/WorkbenchWidget.h"

#include "codex/FrontendSession.h"
#include "codex/ui/BrandMark.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>

namespace codexui::codex {
namespace {

QString text(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto iterator = object.find(key);
  return iterator != object.end() && iterator->is_string()
             ? iterator->get<std::string>()
             : std::string{};
}

QString displayStatus(const std::string &status) {
  if (status == "inProgress" || status == "active")
    return QStringLiteral("Running");
  if (status == "completed" || status == "idle")
    return QStringLiteral("Completed");
  if (status == "failed" || status == "systemError")
    return QStringLiteral("Failed");
  if (status.empty())
    return QStringLiteral("Unknown");
  return text(status);
}

QLabel *makeLabel(QString value, const char *kind = "body") {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

void clearLayout(QLayout *layout) {
  while (QLayoutItem *item = layout->takeAt(0)) {
    if (QWidget *widget = item->widget())
      widget->deleteLater();
    if (QLayout *child = item->layout()) {
      clearLayout(child);
      delete child;
    }
    delete item;
  }
}

QString joinedStrings(const nlohmann::json &value) {
  if (!value.is_array())
    return {};
  QStringList result;
  for (const auto &item : value) {
    if (item.is_string())
      result.push_back(text(item.get<std::string>()));
  }
  return result.join(QStringLiteral(", "));
}

QString messageText(const nlohmann::json &item) {
  const std::string type = stringValue(item, "type");
  if (type == "agentMessage" || type == "plan")
    return text(stringValue(item, "text"));
  if (type == "userMessage") {
    QStringList parts;
    const nlohmann::json content =
        item.value("content", nlohmann::json::array());
    if (content.is_array()) {
      for (const auto &entry : content) {
        const std::string value = stringValue(entry, "text");
        if (!value.empty())
          parts.push_back(text(value));
      }
    }
    return parts.join(QStringLiteral("\n"));
  }
  return {};
}

QFrame *itemFrame(const ItemPresentation &presentation) {
  const nlohmann::json &item = presentation.raw;
  const std::string typeName = stringValue(item, "type");
  auto *frame = new QFrame;
  frame->setProperty("kind", "raised");
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(6);

  QString title;
  if (typeName == "userMessage")
    title = QStringLiteral("You");
  else if (typeName == "agentMessage")
    title = stringValue(item, "phase") == "final_answer"
                ? QStringLiteral("Codex")
                : QStringLiteral("Codex activity");
  else if (typeName == "commandExecution")
    title = QStringLiteral("Command execution");
  else if (typeName == "collabAgentToolCall" || typeName == "subAgentActivity")
    title = QStringLiteral("Agent activity");
  else if (typeName == "reasoning")
    title = QStringLiteral("Reasoning");
  else if (typeName == "fileChange")
    title = QStringLiteral("File changes");
  else
    title = text(typeName.empty() ? std::string("Activity") : typeName);
  layout->addWidget(makeLabel(title, "title"));

  const QString body = messageText(item);
  if (!body.isEmpty())
    layout->addWidget(makeLabel(body));

  if (typeName == "commandExecution") {
    const QString command = text(stringValue(item, "command"));
    if (!command.isEmpty()) {
      auto *commandView = new QPlainTextEdit(command);
      commandView->setReadOnly(true);
      commandView->setMaximumHeight(90);
      commandView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
      commandView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      commandView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      commandView->setProperty("kind", "command");
      layout->addWidget(commandView);
    }
    const QString output = text(stringValue(item, "aggregatedOutput"));
    if (!output.trimmed().isEmpty()) {
      auto *outputView = new QPlainTextEdit(output);
      outputView->setReadOnly(true);
      outputView->setMaximumHeight(220);
      outputView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
      outputView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      outputView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      layout->addWidget(outputView);
    }
    QStringList metadata;
    metadata << displayStatus(stringValue(item, "status"));
    if (item.contains("exitCode") && item["exitCode"].is_number_integer())
      metadata << QStringLiteral("exit %1").arg(item["exitCode"].get<int>());
    const QString cwd = text(stringValue(item, "cwd"));
    if (!cwd.isEmpty())
      metadata << cwd;
    layout->addWidget(
        makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));
  } else if (typeName == "collabAgentToolCall") {
    QStringList metadata;
    metadata << text(stringValue(item, "tool"));
    metadata << displayStatus(stringValue(item, "status"));
    const QString receivers =
        joinedStrings(item.value("receiverThreadIds", nlohmann::json::array()));
    if (!receivers.isEmpty())
      metadata << receivers;
    layout->addWidget(
        makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));
    const QString prompt = text(stringValue(item, "prompt"));
    if (!prompt.isEmpty())
      layout->addWidget(makeLabel(prompt));
  } else if (typeName == "reasoning") {
    const QString summaries =
        joinedStrings(item.value("summary", nlohmann::json::array()));
    if (!summaries.isEmpty())
      layout->addWidget(makeLabel(summaries));
  } else if (body.isEmpty()) {
    layout->addWidget(makeLabel(text(item.dump(2)), "meta"));
  }
  return frame;
}

QFrame *agentFrame(const AgentPresentation &agent) {
  const nlohmann::json &activity = agent.raw;
  auto *frame = new QFrame;
  frame->setProperty("kind", "raised");
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(6);

  const std::string tool = stringValue(activity, "tool");
  const bool childAgent = !agent.childThreadId.empty();
  const QString title = childAgent ? QStringLiteral("Subagent")
                        : tool.empty()
                            ? QStringLiteral("Agent activity")
                            : QStringLiteral("Agent %1").arg(text(tool));
  layout->addWidget(makeLabel(title, "title"));

  QStringList metadata;
  metadata << displayStatus(agent.status);
  const QString path = text(stringValue(activity, "agentPath"));
  if (!path.isEmpty())
    metadata << path;
  if (!tool.empty())
    metadata << text(tool);
  const QString model = text(stringValue(activity, "model"));
  if (!model.isEmpty())
    metadata << model;
  const QString effort = text(stringValue(activity, "reasoningEffort"));
  if (!effort.isEmpty())
    metadata << effort;
  layout->addWidget(makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));

  const QString prompt = text(stringValue(activity, "prompt"));
  if (!prompt.isEmpty())
    layout->addWidget(makeLabel(prompt));

  const QString result = text(stringValue(activity, "resultText"));
  if (!result.isEmpty())
    layout->addWidget(makeLabel(result));

  QStringList identities;
  if (!agent.childThreadId.empty())
    identities << QStringLiteral("thread %1").arg(text(agent.childThreadId));
  const QString sender = text(stringValue(activity, "senderThreadId"));
  if (!sender.isEmpty())
    identities << QStringLiteral("sender %1").arg(sender);
  const QString receivers = joinedStrings(
      activity.value("receiverThreadIds", nlohmann::json::array()));
  if (!receivers.isEmpty())
    identities << QStringLiteral("receivers %1").arg(receivers);
  if (!identities.isEmpty())
    layout->addWidget(
        makeLabel(identities.join(QStringLiteral("  |  ")), "meta"));
  return frame;
}

} // namespace

WorkbenchWidget::WorkbenchWidget(FrontendSession &session, QWidget *parent)
    : QWidget(parent), session(session) {
  setObjectName(QStringLiteral("workbench"));
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto *top = new QFrame;
  top->setProperty("kind", "panel");
  top->setFixedHeight(64);
  auto *topLayout = new QHBoxLayout(top);
  topLayout->setContentsMargins(18, 0, 18, 0);
  topLayout->addWidget(codexui::BrandMark::createLockup());
  topLayout->addStretch();
  attentionLabel = makeLabel({}, "attentionSection");
  connectionLabel = makeLabel(QStringLiteral("Disconnected"), "meta");
  controllerLabel = makeLabel(QStringLiteral("No role"), "meta");
  controllerButton = new QPushButton(QStringLiteral("Claim control"));
  auto *reconnectButton = new QPushButton(QStringLiteral("Reconnect"));
  connect(controllerButton, &QPushButton::clicked, this, [this] {
    if (model.connection().role == "controller")
      this->session.releaseController();
    else
      this->session.claimController();
  });
  connect(reconnectButton, &QPushButton::clicked, this,
          [this] { this->session.reconnect(); });
  topLayout->addWidget(attentionLabel);
  topLayout->addWidget(connectionLabel);
  topLayout->addWidget(controllerLabel);
  topLayout->addWidget(controllerButton);
  topLayout->addWidget(reconnectButton);
  root->addWidget(top);

  auto *splitter = new QSplitter;
  splitter->setChildrenCollapsible(false);

  auto *sidebar = new QFrame;
  sidebar->setProperty("kind", "panel");
  sidebar->setMinimumWidth(230);
  sidebar->setMaximumWidth(390);
  auto *sidebarLayout = new QVBoxLayout(sidebar);
  sidebarLayout->setContentsMargins(10, 10, 10, 10);
  auto *sidebarHeader = new QHBoxLayout;
  sidebarHeader->addWidget(makeLabel(QStringLiteral("Threads"), "section"));
  sidebarHeader->addStretch();
  auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
  auto *newButton = new QPushButton(QStringLiteral("New"));
  threadActionsButton = new QToolButton;
  threadActionsButton->setText(QStringLiteral("More"));
  threadActionsButton->setPopupMode(QToolButton::InstantPopup);
  auto *threadActions = new QMenu(threadActionsButton);
  threadActions->addAction(QStringLiteral("Reload"), this,
                           [this] { readSelectedThread(); });
  threadActions->addAction(QStringLiteral("Rename"), this,
                           [this] { renameSelectedThread(); });
  threadActions->addAction(QStringLiteral("Fork"), this,
                           [this] { forkSelectedThread(); });
  threadActions->addAction(QStringLiteral("Archive / unarchive"), this,
                           [this] { toggleSelectedThreadArchive(); });
  threadActions->addSeparator();
  threadActions->addAction(QStringLiteral("Delete"), this,
                           [this] { deleteSelectedThread(); });
  threadActionsButton->setMenu(threadActions);
  sidebarHeader->addWidget(refreshButton);
  sidebarHeader->addWidget(newButton);
  sidebarHeader->addWidget(threadActionsButton);
  sidebarLayout->addLayout(sidebarHeader);
  threadList = new QListWidget;
  threadList->setSelectionMode(QAbstractItemView::SingleSelection);
  threadList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  threadList->setTextElideMode(Qt::ElideRight);
  sidebarLayout->addWidget(threadList);
  connect(refreshButton, &QPushButton::clicked, this,
          [this] { requestThreads(); });
  connect(newButton, &QPushButton::clicked, this, [this] { beginNewThread(); });
  connect(threadList, &QListWidget::itemClicked, this,
          [this](QListWidgetItem *item) {
            selectThread(item->data(Qt::UserRole).toString().toStdString());
          });
  splitter->addWidget(sidebar);

  auto *center = new QFrame;
  center->setProperty("kind", "panel");
  auto *centerLayout = new QVBoxLayout(center);
  centerLayout->setContentsMargins(16, 12, 16, 12);
  centerLayout->setSpacing(8);
  conversationTitle = makeLabel(QStringLiteral("Select a thread"), "heading");
  conversationMeta = makeLabel({}, "meta");
  centerLayout->addWidget(conversationTitle);
  centerLayout->addWidget(conversationMeta);

  conversationScroll = new QScrollArea;
  conversationScroll->setWidgetResizable(true);
  conversationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  conversationContent = new QWidget;
  conversationContent->setMinimumWidth(0);
  conversationContent->setSizePolicy(QSizePolicy::Ignored,
                                     QSizePolicy::Preferred);
  conversationLayout = new QVBoxLayout(conversationContent);
  conversationLayout->setContentsMargins(4, 6, 4, 6);
  conversationLayout->setSpacing(8);
  emptyConversation =
      makeLabel(QStringLiteral("Conversation activity appears here."), "muted");
  conversationLayout->addWidget(emptyConversation);
  conversationLayout->addStretch();
  conversationScroll->setWidget(conversationContent);
  centerLayout->addWidget(conversationScroll, 1);

  auto *attention = new QFrame;
  attention->setProperty("kind", "amberBadge");
  auto *attentionLayout = new QHBoxLayout(attention);
  attentionLayout->setContentsMargins(10, 6, 10, 6);
  attentionLayout->addWidget(makeLabel(
      QStringLiteral("A Codex request needs attention"), "attentionSection"));
  attentionLayout->addStretch();
  approveButton = new QPushButton(QStringLiteral("Approve"));
  denyButton = new QPushButton(QStringLiteral("Deny"));
  attentionLayout->addWidget(denyButton);
  attentionLayout->addWidget(approveButton);
  connect(approveButton, &QPushButton::clicked, this,
          [this] { respondToFirstPending(true); });
  connect(denyButton, &QPushButton::clicked, this,
          [this] { respondToFirstPending(false); });
  centerLayout->addWidget(attention);

  auto *composer = new QFrame;
  composer->setProperty("kind", "composer");
  auto *composerLayout = new QHBoxLayout(composer);
  composerLayout->setContentsMargins(10, 8, 8, 8);
  promptEditor = new codexui::ExpandingPromptEditor;
  sendButton = new QPushButton(QStringLiteral("Send"));
  sendButton->setProperty("kind", "primary");
  interruptButton = new QPushButton(QStringLiteral("Stop"));
  interruptButton->setProperty("kind", "stop");
  composerLayout->addWidget(promptEditor, 1);
  composerLayout->addWidget(interruptButton);
  composerLayout->addWidget(sendButton);
  centerLayout->addWidget(composer);
  connect(sendButton, &QPushButton::clicked, this, [this] { submitPrompt(); });
  connect(promptEditor, &codexui::ExpandingPromptEditor::submitRequested, this,
          [this] { submitPrompt(); });
  connect(interruptButton, &QPushButton::clicked, this,
          [this] { interruptActiveTurn(); });
  splitter->addWidget(center);

  inspectorTabs = new QTabWidget;
  inspectorTabs->setMinimumWidth(260);
  inspectorTabs->setMaximumWidth(430);
  planContent = new QWidget;
  planLayout = new QVBoxLayout(planContent);
  planLayout->setContentsMargins(12, 12, 12, 12);
  planLayout->setSpacing(8);
  agentsContent = new QWidget;
  agentsLayout = new QVBoxLayout(agentsContent);
  agentsLayout->setContentsMargins(12, 12, 12, 12);
  agentsLayout->setSpacing(8);
  requestsContent = new QWidget;
  requestsLayout = new QVBoxLayout(requestsContent);
  requestsLayout->setContentsMargins(12, 12, 12, 12);
  requestsLayout->setSpacing(8);
  auto *planScroll = new QScrollArea;
  planScroll->setWidgetResizable(true);
  planScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  planScroll->setWidget(planContent);
  auto *agentsScroll = new QScrollArea;
  agentsScroll->setWidgetResizable(true);
  agentsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  agentsScroll->setWidget(agentsContent);
  auto *requestsScroll = new QScrollArea;
  requestsScroll->setWidgetResizable(true);
  requestsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  requestsScroll->setWidget(requestsContent);
  auto *protocolContent = new QWidget;
  auto *protocolLayout = new QVBoxLayout(protocolContent);
  protocolLayout->setContentsMargins(8, 8, 8, 8);
  protocolLayout->setSpacing(6);
  protocolStats = makeLabel({}, "meta");
  protocolLog = new QPlainTextEdit;
  protocolLog->setProperty("kind", "code");
  protocolLog->setReadOnly(true);
  protocolLog->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  protocolLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  protocolLog->document()->setMaximumBlockCount(2000);
  protocolLayout->addWidget(protocolStats);
  protocolLayout->addWidget(protocolLog, 1);
  auto *stateContent = new QWidget;
  auto *stateLayout = new QVBoxLayout(stateContent);
  stateLayout->setContentsMargins(8, 8, 8, 8);
  stateView = new QPlainTextEdit;
  stateView->setProperty("kind", "code");
  stateView->setReadOnly(true);
  stateView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  stateView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stateLayout->addWidget(stateView);
  inspectorTabs->addTab(planScroll, QStringLiteral("Plan"));
  inspectorTabs->addTab(agentsScroll, QStringLiteral("Agents"));
  inspectorTabs->addTab(requestsScroll, QStringLiteral("Requests"));
  inspectorTabs->addTab(stateContent, QStringLiteral("State"));
  inspectorTabs->addTab(protocolContent, QStringLiteral("Protocol"));
  connect(inspectorTabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == 3)
      requestEnvironment();
  });
  splitter->addWidget(inspectorTabs);
  splitter->setSizes({270, 900, 320});
  root->addWidget(splitter, 1);

  refreshTimer = new QTimer(this);
  refreshTimer->setSingleShot(true);
  refreshTimer->setInterval(16);
  connect(refreshTimer, &QTimer::timeout, this, [this] { refresh(); });

  session.setEventHandler(
      [this](const nlohmann::json &event) { handleEvent(event); });
  refresh();
}

void WorkbenchWidget::handleEvent(const nlohmann::json &event) {
  appendProtocolFrame(event);
  model.applyEvent(event);
  if (event.value("kind", std::string{}) == "event" &&
      event.value("type", std::string{}) == "connection.bridge" &&
      event.value("data", nlohmann::json::object())
              .value("state", std::string{}) == "opened") {
    environmentRequested = false;
    requestThreads();
    if (inspectorTabs->currentIndex() == 3)
      requestEnvironment();
  }

  hydrateHistoricalAgents();

  if (!selectedThreadId.empty() && !model.thread(selectedThreadId))
    selectedThreadId.clear();
  scheduleRefresh();
}

void WorkbenchWidget::hydrateHistoricalAgents() {
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread)
    return;
  for (const std::string &agentId : thread->agentOrder) {
    const auto agent = thread->agents.find(agentId);
    if (agent == thread->agents.end() || agent->second.childThreadId.empty() ||
        agent->second.status != "started")
      continue;
    if (!requestedAgentThreads.insert(agent->second.childThreadId).second)
      continue;
    session.readThread(agent->second.childThreadId);
  }
}

void WorkbenchWidget::scheduleRefresh() {
  if (!refreshTimer->isActive())
    refreshTimer->start();
}

void WorkbenchWidget::refresh() {
  refreshThreads();
  refreshConversation();
  refreshInspector();
  refreshStateInspector();
  refreshProtocolStats();
  refreshStatus();
}

void WorkbenchWidget::refreshProtocolStats() {
  std::size_t turns = 0;
  std::size_t items = 0;
  if (const ThreadPresentation *thread = model.thread(selectedThreadId)) {
    turns = thread->turnOrder.size();
    for (const auto &[turnId, turn] : thread->turns) {
      static_cast<void>(turnId);
      items += turn.itemOrder.size();
    }
  }
  protocolStats->setText(
      QStringLiteral("seq %1  |  threads %2  |  models %3  |  turns %4  |  "
                     "items %5  |  pending %6  |  telemetry %7")
          .arg(static_cast<qulonglong>(observedPresentationSequence))
          .arg(static_cast<qulonglong>(model.threadOrder().size()))
          .arg(static_cast<qulonglong>(model.modelCatalog().size()))
          .arg(static_cast<qulonglong>(turns))
          .arg(static_cast<qulonglong>(items))
          .arg(static_cast<qulonglong>(model.pendingRequestCount()))
          .arg(static_cast<qulonglong>(model.telemetry().size())));
}

void WorkbenchWidget::appendProtocolFrame(const nlohmann::json &frame) {
  if (!protocolLog)
    return;

  const std::uint64_t sequence = frame.value("sequence", 0ULL);
  if (sequence != 0) {
    if (observedPresentationSequence != 0 &&
        sequence != observedPresentationSequence + 1) {
      const QString relation = sequence <= observedPresentationSequence
                                   ? QStringLiteral("NON-MONOTONIC")
                                   : QStringLiteral("SEQUENCE GAP");
      protocolLog->appendPlainText(
          QStringLiteral("[%1] %2 expected=%3 received=%4")
              .arg(QDateTime::currentDateTime().toString(
                       QStringLiteral("HH:mm:ss.zzz")),
                   relation)
              .arg(static_cast<qulonglong>(observedPresentationSequence + 1))
              .arg(static_cast<qulonglong>(sequence)));
    }
    observedPresentationSequence =
        std::max(observedPresentationSequence, sequence);
  }

  const std::string kind = stringValue(frame, "kind");
  const std::string subject = kind == "result" ? stringValue(frame, "action")
                                               : stringValue(frame, "type");
  const nlohmann::json scope = frame.value("scope", nlohmann::json::object());
  QStringList parts;
  parts << QStringLiteral("[%1]").arg(
      QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")));
  if (sequence != 0)
    parts << QStringLiteral("#%1").arg(static_cast<qulonglong>(sequence));
  parts << QStringLiteral("g%1").arg(
      static_cast<qulonglong>(frame.value("generation", 0ULL)));
  parts << text(kind);
  parts << text(subject);
  parts << text(stringValue(frame, "authority"));
  if (kind == "result")
    parts << (frame.value("ok", false) ? QStringLiteral("ok")
                                       : QStringLiteral("ERROR"));
  for (const char *key :
       {"threadId", "turnId", "itemId", "requestId", "processId"}) {
    const std::string value = stringValue(scope, key);
    if (!value.empty())
      parts << QStringLiteral("%1=%2").arg(QString::fromLatin1(key),
                                           text(value));
  }
  const std::string correlationId = stringValue(frame, "correlationId");
  if (!correlationId.empty())
    parts << QStringLiteral("correlation=%1").arg(text(correlationId));
  if (kind == "result" && !frame.value("ok", false)) {
    const nlohmann::json error = frame.value("error", nlohmann::json::object());
    const std::string message = stringValue(error, "message");
    if (!message.empty())
      parts << text(message);
  }
  protocolLog->appendPlainText(parts.join(QStringLiteral("  ")));
}

void WorkbenchWidget::refreshThreads() {
  threadList->blockSignals(true);
  threadList->clear();
  for (const std::string &threadId : model.threadOrder()) {
    const ThreadPresentation *thread = model.thread(threadId);
    if (!thread)
      continue;
    QString title = text(thread->title);
    if (title.isEmpty())
      title = text(threadId.substr(0, 12));
    if (model.pendingRequestCount(threadId) != 0)
      title.prepend(QStringLiteral("! "));
    auto *item = new QListWidgetItem(title, threadList);
    if (model.pendingRequestCount(threadId) != 0)
      item->setForeground(QColor(QStringLiteral("#8a5a00")));
    item->setData(Qt::UserRole, text(threadId));
    item->setToolTip(text(thread->cwd));
    if (threadId == selectedThreadId)
      threadList->setCurrentItem(item);
  }
  threadList->blockSignals(false);
}

void WorkbenchWidget::refreshConversation() {
  const int previousMaximum =
      conversationScroll->verticalScrollBar()->maximum();
  const bool followLatest =
      conversationScroll->verticalScrollBar()->value() >= previousMaximum - 12;
  clearLayout(conversationLayout);

  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread) {
    conversationTitle->setText(localNewThreadIntent
                                   ? QStringLiteral("New thread")
                                   : QStringLiteral("Select a thread"));
    conversationMeta->setText(localNewThreadIntent ? QDir::currentPath()
                                                   : QString{});
    emptyConversation =
        makeLabel(localNewThreadIntent
                      ? QStringLiteral("Send a message to create this thread.")
                      : QStringLiteral("Conversation activity appears here."),
                  "muted");
    conversationLayout->addWidget(emptyConversation);
    conversationLayout->addStretch();
    return;
  }

  conversationTitle->setText(text(thread->title));
  conversationMeta->setText(text(thread->cwd) + QStringLiteral("  |  ") +
                            displayStatus(thread->status));
  std::size_t count = 0;
  for (const std::string &turnId : thread->turnOrder) {
    const auto turn = thread->turns.find(turnId);
    if (turn == thread->turns.end())
      continue;
    for (const std::string &itemId : turn->second.itemOrder) {
      const auto item = turn->second.items.find(itemId);
      if (item == turn->second.items.end())
        continue;
      conversationLayout->addWidget(itemFrame(item->second));
      ++count;
    }
  }
  if (count == 0)
    conversationLayout->addWidget(
        makeLabel(QStringLiteral("No materialized activity."), "muted"));
  conversationLayout->addStretch();
  if (followLatest)
    QTimer::singleShot(0, conversationScroll, [scroll = conversationScroll] {
      scroll->verticalScrollBar()->setValue(
          scroll->verticalScrollBar()->maximum());
    });
}

void WorkbenchWidget::refreshInspector() {
  clearLayout(planLayout);
  clearLayout(agentsLayout);
  clearLayout(requestsLayout);
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread) {
    planLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
    planLayout->addStretch();
    agentsLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
    agentsLayout->addStretch();
    requestsLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
    requestsLayout->addStretch();
    return;
  }

  const TurnPresentation *planTurn = nullptr;
  for (auto turnId = thread->turnOrder.rbegin();
       turnId != thread->turnOrder.rend(); ++turnId) {
    const auto turn = thread->turns.find(*turnId);
    if (turn != thread->turns.end() && turn->second.plan.is_object() &&
        turn->second.plan.contains("steps")) {
      planTurn = &turn->second;
      break;
    }
  }
  if (planTurn) {
    const QString explanation =
        text(stringValue(planTurn->plan, "explanation"));
    if (!explanation.isEmpty())
      planLayout->addWidget(makeLabel(explanation));
    const nlohmann::json steps =
        planTurn->plan.value("steps", nlohmann::json::array());
    for (const auto &step : steps) {
      auto *row = new QFrame;
      row->setProperty("kind", "summary");
      auto *rowLayout = new QVBoxLayout(row);
      rowLayout->setContentsMargins(9, 7, 9, 7);
      rowLayout->addWidget(makeLabel(text(stringValue(step, "step"))));
      rowLayout->addWidget(
          makeLabel(displayStatus(stringValue(step, "status")), "meta"));
      planLayout->addWidget(row);
    }
  } else {
    planLayout->addWidget(
        makeLabel(QStringLiteral("No plan for this thread."), "muted"));
  }
  planLayout->addStretch();

  std::size_t agentCount = 0;
  for (const std::string &agentId : thread->agentOrder) {
    const auto agent = thread->agents.find(agentId);
    if (agent == thread->agents.end())
      continue;
    agentsLayout->addWidget(agentFrame(agent->second));
    ++agentCount;
  }
  if (agentCount == 0)
    agentsLayout->addWidget(makeLabel(
        QStringLiteral("No agent activity for this thread."), "muted"));
  agentsLayout->addStretch();

  std::size_t requestCount = 0;
  for (const auto &[id, request] : model.pendingRequestPresentations()) {
    if (request.threadId != selectedThreadId)
      continue;
    auto *frame = new QFrame;
    frame->setProperty("kind", "summary");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(9, 7, 9, 7);
    layout->setSpacing(5);
    layout->addWidget(makeLabel(text(request.kind), "title"));
    layout->addWidget(
        makeLabel(QStringLiteral("generation %1  |  request %2")
                      .arg(static_cast<qulonglong>(request.generation))
                      .arg(text(id)),
                  "meta"));
    layout->addWidget(makeLabel(
        QStringLiteral("Review and answer this request using the action bar."),
        "meta"));
    requestsLayout->addWidget(frame);
    ++requestCount;
  }
  if (requestCount == 0)
    requestsLayout->addWidget(makeLabel(
        QStringLiteral("No pending requests for this thread."), "muted"));
  requestsLayout->addStretch();
}

void WorkbenchWidget::refreshStatus() {
  const ConnectionPresentation &connection = model.connection();
  connectionLabel->setText(connection.connected
                               ? QStringLiteral("Connected")
                               : QStringLiteral("Disconnected"));
  controllerLabel->setText(connection.role.empty() ? QStringLiteral("No role")
                                                   : text(connection.role));
  controllerButton->setText(connection.role == "controller"
                                ? QStringLiteral("Release control")
                                : QStringLiteral("Claim control"));
  controllerButton->setEnabled(connection.connected);
  const std::size_t pending = model.pendingRequestCount(selectedThreadId);
  attentionLabel->setText(
      pending == 0
          ? QString{}
          : QStringLiteral("%1 pending").arg(static_cast<qulonglong>(pending)));
  approveButton->parentWidget()->setVisible(pending != 0);
  const bool active = model.activeTurnId(selectedThreadId).has_value();
  interruptButton->setVisible(active);
  sendButton->setText(active ? QStringLiteral("Steer")
                             : QStringLiteral("Send"));
  sendButton->setEnabled(connection.connected &&
                         connection.role == "controller");
  threadActionsButton->setEnabled(!selectedThreadId.empty() &&
                                  connection.connected &&
                                  connection.role == "controller");
}

void WorkbenchWidget::refreshStateInspector() {
  if (!stateView)
    return;
  nlohmann::json domains = nlohmann::json::object();
  for (const auto &[name, value] : model.globalDomains())
    domains[name] = value;

  nlohmann::json pending = nlohmann::json::object();
  for (const auto &[id, request] : model.pendingRequestPresentations()) {
    pending[id] = {{"category", request.kind},
                   {"threadId", request.threadId},
                   {"generation", request.generation}};
  }

  const nlohmann::json state{{"models", model.modelCatalog()},
                             {"pendingRequests", std::move(pending)},
                             {"domains", std::move(domains)}};
  stateView->setPlainText(text(state.dump(2)));
}

void WorkbenchWidget::selectThread(std::string threadId) {
  selectedThreadId = std::move(threadId);
  localNewThreadIntent = false;
  readSelectedThread();
  refresh();
}

void WorkbenchWidget::beginNewThread() {
  selectedThreadId.clear();
  localNewThreadIntent = true;
  threadList->clearSelection();
  promptEditor->setFocus();
  refresh();
}

void WorkbenchWidget::requestThreads() { session.listThreads(); }

void WorkbenchWidget::requestModels() { session.listModels(); }

void WorkbenchWidget::requestEnvironment() {
  if (environmentRequested)
    return;
  environmentRequested = true;
  const std::string cwd = QDir::currentPath().toStdString();
  requestModels();
  session.readModelProviderCapabilities();
  session.readAccount({{"refreshToken", false}});
  session.readAccountRateLimits();
  session.readAccountTokenUsage();
  session.readConfig({{"cwd", cwd}, {"includeLayers", true}});
  session.listPermissionProfiles({{"cwd", cwd}});
  session.listExperimentalFeatures();
  session.listSkills(
      {{"cwds", nlohmann::json::array({cwd})}, {"forceReload", false}});
  session.listHooks({{"cwds", nlohmann::json::array({cwd})}});
  session.listPlugins(
      {{"cwds", nlohmann::json::array({cwd})}, {"forceRefetch", false}});
  session.listApps();
  session.listMcpServers();
}

void WorkbenchWidget::readSelectedThread() {
  if (selectedThreadId.empty())
    return;
  session.readThread(selectedThreadId);
}

void WorkbenchWidget::renameSelectedThread() {
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread)
    return;
  bool accepted = false;
  const QString name =
      QInputDialog::getText(this, QStringLiteral("Rename thread"),
                            QStringLiteral("Name"), QLineEdit::Normal,
                            text(thread->title), &accepted)
          .trimmed();
  if (accepted && !name.isEmpty())
    session.renameThread(selectedThreadId, name.toStdString());
}

void WorkbenchWidget::forkSelectedThread() {
  if (selectedThreadId.empty())
    return;
  session.forkThread(selectedThreadId, nlohmann::json::object(),
                     [this](const nlohmann::json &result) {
                       if (!result.value("ok", false))
                         return;
                       const std::string threadId = stringValue(
                           result.value("data", nlohmann::json::object())
                               .value("thread", nlohmann::json::object()),
                           "id");
                       if (!threadId.empty())
                         selectThread(threadId);
                     });
}

void WorkbenchWidget::toggleSelectedThreadArchive() {
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread)
    return;
  if (thread->archived)
    session.unarchiveThread(selectedThreadId);
  else
    session.archiveThread(selectedThreadId);
}

void WorkbenchWidget::deleteSelectedThread() {
  if (selectedThreadId.empty())
    return;
  if (QMessageBox::question(this, QStringLiteral("Delete thread"),
                            QStringLiteral("Delete the selected thread?"),
                            QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel) == QMessageBox::Yes) {
    session.deleteThread(selectedThreadId);
  }
}

void WorkbenchWidget::submitPrompt() {
  const QString promptValue = promptEditor->toPlainText().trimmed();
  if (promptValue.isEmpty())
    return;
  const std::string prompt = promptValue.toStdString();
  promptEditor->clear();

  if (!selectedThreadId.empty()) {
    submitPromptToThread(selectedThreadId, prompt);
    return;
  }
  localNewThreadIntent = true;
  session.createThread({{"cwd", QDir::currentPath().toStdString()}},
                       [this, prompt](const nlohmann::json &result) {
                         if (!result.value("ok", false))
                           return;
                         const std::string threadId = stringValue(
                             result.value("data", nlohmann::json::object())
                                 .value("thread", nlohmann::json::object()),
                             "id");
                         if (threadId.empty())
                           return;
                         selectedThreadId = threadId;
                         localNewThreadIntent = false;
                         submitPromptToThread(threadId, prompt);
                         scheduleRefresh();
                       });
}

void WorkbenchWidget::submitPromptToThread(std::string threadId,
                                           std::string prompt) {
  const nlohmann::json input =
      nlohmann::json::array({{{"type", "text"},
                              {"text", std::move(prompt)},
                              {"text_elements", nlohmann::json::array()}}});
  const auto activeTurn = model.activeTurnId(threadId);
  if (activeTurn) {
    session.steerTurn(threadId, *activeTurn, input);
  } else {
    session.startTurn(threadId, input);
  }
}

void WorkbenchWidget::interruptActiveTurn() {
  const auto turnId = model.activeTurnId(selectedThreadId);
  if (!turnId)
    return;
  session.interruptTurn(selectedThreadId, *turnId);
}

void WorkbenchWidget::respondToFirstPending(bool approve) {
  const auto &pending = model.pendingRequestPresentations();
  const auto request =
      std::find_if(pending.begin(), pending.end(), [this](const auto &entry) {
        return entry.second.threadId == selectedThreadId;
      });
  if (request == pending.end())
    return;

  nlohmann::json result;
  const std::string &type = request->second.kind;
  if (type == "command-approval" || type == "file-change-approval") {
    result = {{"decision", approve ? "accept" : "decline"}};
  } else if (type == "legacy-patch-approval" ||
             type == "legacy-command-approval") {
    result = {{"decision",
               approve ? nlohmann::json("approved")
                       : nlohmann::json{
                             {"denied", {{"rejection", "Denied by user"}}}}}};
  } else if (type == "mcp-elicitation") {
    result = {{"action", approve ? "accept" : "decline"},
              {"content", nullptr},
              {"_meta", nullptr}};
  } else {
    return;
  }
  session.respondToServerRequest(nlohmann::json::parse(request->first),
                                 std::move(result));
}

} // namespace codexui::codex
