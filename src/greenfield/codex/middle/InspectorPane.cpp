// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/InspectorPane.h"

#include "codex/DiffViewer.h"
#include "codex/PresentationModel.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {

constexpr int MaximumProtocolLines = 2000;

QString text(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
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

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string{};
}

QString displayStatus(const std::string &status) {
  if (status == "inProgress" || status == "active")
    return QStringLiteral("Running");
  if (status == "completed" || status == "idle")
    return QStringLiteral("Completed");
  if (status == "failed" || status == "systemError")
    return QStringLiteral("Failed");
  return status.empty() ? QStringLiteral("Unknown") : text(status);
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

QLabel *makeMarkdownLabel(const QString &value) {
  QTextDocument document;
  document.setMarkdown(value, QTextDocument::MarkdownNoHTML);
  auto *label = new QLabel(document.toHtml());
  label->setProperty("kind", "body");
  label->setTextFormat(Qt::RichText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setOpenExternalLinks(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::LinksAccessibleByMouse);
  return label;
}

void clearLayout(QLayout *layout) {
  while (QLayoutItem *item = layout->takeAt(0)) {
    if (QWidget *widget = item->widget())
      delete widget;
    if (QLayout *child = item->layout()) {
      clearLayout(child);
      delete child;
    }
    delete item;
  }
}

QByteArray bytes(const nlohmann::json &value) {
  const std::string serialized = value.dump();
  return QByteArray(serialized.data(),
                    static_cast<qsizetype>(serialized.size()));
}

QFrame *agentFrame(const AgentPresentation &agent) {
  auto *frame = new QFrame;
  frame->setProperty("kind", "raised");
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(6);
  const std::string tool = stringValue(agent.raw, "tool");
  const QString title =
      !agent.childThreadId.empty() ? QStringLiteral("Subagent")
      : tool.empty()               ? QStringLiteral("Agent activity")
                                   : QStringLiteral("Agent %1").arg(text(tool));
  layout->addWidget(makeLabel(title, "title"));
  QStringList metadata{displayStatus(agent.status)};
  for (const char *key : {"agentPath", "tool", "model", "reasoningEffort"}) {
    const QString value = text(stringValue(agent.raw, key));
    if (!value.isEmpty())
      metadata << value;
  }
  layout->addWidget(makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));
  const QString prompt = text(stringValue(agent.raw, "prompt"));
  if (!prompt.isEmpty())
    layout->addWidget(makeLabel(prompt));
  const QString result = text(stringValue(agent.raw, "resultText"));
  if (!result.isEmpty())
    layout->addWidget(makeMarkdownLabel(result));
  QStringList identities;
  if (!agent.childThreadId.empty())
    identities << QStringLiteral("thread %1").arg(text(agent.childThreadId));
  const QString sender = text(stringValue(agent.raw, "senderThreadId"));
  if (!sender.isEmpty())
    identities << QStringLiteral("sender %1").arg(sender);
  const QString receivers = joinedStrings(
      agent.raw.value("receiverThreadIds", nlohmann::json::array()));
  if (!receivers.isEmpty())
    identities << QStringLiteral("receivers %1").arg(receivers);
  if (!identities.isEmpty())
    layout->addWidget(
        makeLabel(identities.join(QStringLiteral("  |  ")), "meta"));
  return frame;
}

struct ScrollPosition {
  bool followsTail = true;
  int value = 0;
};

void restoreScrollPosition(QPlainTextEdit *view,
                           const ScrollPosition &position) {
  QScrollBar *scrollBar = view->verticalScrollBar();
  if (position.followsTail) {
    scrollBar->setValue(scrollBar->maximum());
    return;
  }
  scrollBar->setValue(
      std::clamp(position.value, scrollBar->minimum(), scrollBar->maximum()));
}

} // namespace

InspectorPane::InspectorPane(QWidget *parent) : QFrame(parent) {
  setObjectName(QStringLiteral("inspector"));
  setStyleSheet(QStringLiteral("QFrame#inspector{background:#fbfcfe;}"));
  setMinimumWidth(300);
  setMaximumWidth(520);

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(18, 14, 20, 0);
  outer->setSpacing(0);
  auto *heading = new QHBoxLayout;
  heading->addWidget(makeLabel(QStringLiteral("INSPECTOR"), "section"));
  heading->addStretch();
  auto *hide = new QPushButton(QStringLiteral("Hide"));
  hide->setProperty("kind", "subtle");
  hide->setFixedSize(58, 24);
  connect(hide, &QPushButton::clicked, this, [this] {
    if (hideAction)
      hideAction();
  });
  heading->addWidget(hide);
  outer->addLayout(heading);
  outer->addSpacing(7);

  inspectorTabs = new QTabWidget;
  inspectorTabs->setDocumentMode(true);
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
  diffViewer = new DiffViewer;

  const auto makeScroll = [](QWidget *content) {
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
  };

  auto *stateContent = new QWidget;
  auto *stateLayout = new QVBoxLayout(stateContent);
  stateLayout->setContentsMargins(8, 8, 8, 8);
  stateView = new QPlainTextEdit;
  stateView->setObjectName(QStringLiteral("stateInfoView"));
  stateView->setProperty("kind", "infoViewer");
  stateView->setReadOnly(true);
  stateView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  stateView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stateView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  stateView->verticalScrollBar()->setProperty("kind", "infoViewer");
  stateLayout->addWidget(stateView);

  auto *protocolContent = new QWidget;
  auto *protocolLayout = new QVBoxLayout(protocolContent);
  protocolLayout->setContentsMargins(8, 8, 8, 8);
  protocolLayout->setSpacing(6);
  protocolLog = new QPlainTextEdit;
  protocolLog->setObjectName(QStringLiteral("protocolInfoLog"));
  protocolLog->setProperty("kind", "infoViewer");
  protocolLog->setReadOnly(true);
  protocolLog->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  protocolLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  protocolLog->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  protocolLog->verticalScrollBar()->setProperty("kind", "infoViewer");
  protocolLog->document()->setMaximumBlockCount(MaximumProtocolLines);
  connect(protocolLog->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            if (mutatingProtocolLog)
              return;
            QScrollBar *scrollBar = protocolLog->verticalScrollBar();
            protocolFollowsTail = value >= scrollBar->maximum() - 1;
            if (!protocolFollowsTail)
              protocolPausedScrollValue = value;
          });
  protocolStats = makeLabel({}, "meta");
  protocolStats->setObjectName(QStringLiteral("protocolInfoStats"));
  protocolLayout->addWidget(protocolLog, 1);
  protocolLayout->addWidget(protocolStats);

  infoTabs = new QTabWidget;
  infoTabs->setObjectName(QStringLiteral("infoTabs"));
  infoTabs->setDocumentMode(true);
  infoTabs->addTab(stateContent, QStringLiteral("State"));
  infoTabs->addTab(protocolContent, QStringLiteral("Protocol"));

  inspectorTabs->addTab(makeScroll(planContent), QStringLiteral("Plan"));
  inspectorTabs->addTab(makeScroll(agentsContent), QStringLiteral("Agents"));
  inspectorTabs->addTab(diffViewer, QStringLiteral("Changes"));
  inspectorTabs->addTab(makeScroll(requestsContent),
                        QStringLiteral("Requests"));
  inspectorTabs->addTab(infoTabs, QStringLiteral("Info"));
  outer->addWidget(inspectorTabs, 1);

  connect(inspectorTabs, &QTabWidget::currentChanged, this,
          [this](int) { refreshCurrentTab(); });
  connect(infoTabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == 1)
      showProtocolTail();
    refreshCurrentTab();
  });
}

void InspectorPane::setHideAction(std::function<void()> hide) {
  hideAction = std::move(hide);
}

void InspectorPane::setRequestActions(RequestAction review,
                                      RequestAction reject) {
  reviewRequest = std::move(review);
  rejectRequest = std::move(reject);
}

void InspectorPane::refresh(const PresentationModel &model,
                            const std::string &selectedThreadId) {
  currentModel = &model;
  currentThreadId = selectedThreadId;
  refreshCurrentTab();
}

void InspectorPane::refreshCurrentTab() {
  if (!currentModel)
    return;
  switch (inspectorTabs->currentIndex()) {
  case 0:
    refreshPlan();
    break;
  case 1:
    refreshAgents();
    break;
  case 2:
    refreshChanges();
    break;
  case 3:
    refreshRequests();
    break;
  case 4:
    if (infoTabs->currentIndex() == 0)
      refreshState();
    else {
      showProtocolTail();
      refreshProtocolStats();
    }
    break;
  default:
    break;
  }
}

void InspectorPane::refreshPlan() {
  const ThreadPresentation *thread = currentModel->thread(currentThreadId);
  nlohmann::json snapshot{{"threadId", currentThreadId}};
  const TurnPresentation *planTurn = nullptr;
  const ItemPresentation *planItem = nullptr;
  if (thread) {
    for (auto id = thread->turnOrder.rbegin(); id != thread->turnOrder.rend();
         ++id) {
      const auto turn = thread->turns.find(*id);
      if (turn == thread->turns.end())
        continue;
      if (turn->second.plan.is_object() &&
          turn->second.plan.contains("steps")) {
        planTurn = &turn->second;
        snapshot["plan"]["explanation"] =
            stringValue(planTurn->plan, "explanation");
        snapshot["plan"]["steps"] = nlohmann::json::array();
        for (const auto &step :
             planTurn->plan.value("steps", nlohmann::json::array()))
          snapshot["plan"]["steps"].push_back(
              {{"step", stringValue(step, "step")},
               {"status", stringValue(step, "status")}});
        break;
      }
      for (auto itemId = turn->second.itemOrder.rbegin();
           itemId != turn->second.itemOrder.rend(); ++itemId) {
        const auto item = turn->second.items.find(*itemId);
        if (item != turn->second.items.end() &&
            stringValue(item->second.raw, "type") == "plan") {
          planItem = &item->second;
          snapshot["planItem"] = stringValue(planItem->raw, "text");
          break;
        }
      }
      if (planItem)
        break;
    }
  }
  const QByteArray next = bytes(snapshot);
  if (next == planSnapshot)
    return;
  planSnapshot = next;
  setUpdatesEnabled(false);
  clearLayout(planLayout);
  if (!thread) {
    planLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
  } else if (planTurn) {
    const QString explanation =
        text(stringValue(planTurn->plan, "explanation"));
    if (!explanation.isEmpty())
      planLayout->addWidget(makeMarkdownLabel(explanation));
    for (const auto &step :
         planTurn->plan.value("steps", nlohmann::json::array())) {
      auto *row = new QFrame;
      row->setProperty("kind", "summary");
      auto *layout = new QVBoxLayout(row);
      layout->setContentsMargins(9, 7, 9, 7);
      layout->addWidget(makeLabel(text(stringValue(step, "step"))));
      layout->addWidget(
          makeLabel(displayStatus(stringValue(step, "status")), "meta"));
      planLayout->addWidget(row);
    }
  } else if (planItem) {
    const QString value = text(stringValue(planItem->raw, "text"));
    planLayout->addWidget(
        value.isEmpty()
            ? makeLabel(QStringLiteral("Plan is being prepared."), "muted")
            : makeMarkdownLabel(value));
  } else {
    planLayout->addWidget(
        makeLabel(QStringLiteral("No plan for this thread."), "muted"));
  }
  planLayout->addStretch();
  setUpdatesEnabled(true);
}

void InspectorPane::refreshAgents() {
  const ThreadPresentation *thread = currentModel->thread(currentThreadId);
  nlohmann::json snapshot = nlohmann::json::array();
  if (thread) {
    for (const std::string &id : thread->agentOrder) {
      const auto agent = thread->agents.find(id);
      if (agent != thread->agents.end())
        snapshot.push_back(
            {{"id", id},
             {"status", agent->second.status},
             {"childThreadId", agent->second.childThreadId},
             {"agentPath", stringValue(agent->second.raw, "agentPath")},
             {"tool", stringValue(agent->second.raw, "tool")},
             {"model", stringValue(agent->second.raw, "model")},
             {"reasoningEffort",
              stringValue(agent->second.raw, "reasoningEffort")},
             {"prompt", stringValue(agent->second.raw, "prompt")},
             {"resultText", stringValue(agent->second.raw, "resultText")},
             {"senderThreadId",
              stringValue(agent->second.raw, "senderThreadId")},
             {"receiverThreadIds",
              agent->second.raw.value("receiverThreadIds",
                                      nlohmann::json::array())}});
    }
  }
  const QByteArray next =
      bytes({{"threadId", currentThreadId}, {"agents", snapshot}});
  if (next == agentsSnapshot)
    return;
  agentsSnapshot = next;
  setUpdatesEnabled(false);
  clearLayout(agentsLayout);
  if (!thread)
    agentsLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
  else if (snapshot.empty())
    agentsLayout->addWidget(makeLabel(
        QStringLiteral("No agent activity for this thread."), "muted"));
  else
    for (const std::string &id : thread->agentOrder) {
      const auto agent = thread->agents.find(id);
      if (agent != thread->agents.end())
        agentsLayout->addWidget(agentFrame(agent->second));
    }
  agentsLayout->addStretch();
  setUpdatesEnabled(true);
}

void InspectorPane::refreshChanges() {
  const ThreadPresentation *thread = currentModel->thread(currentThreadId);
  QString liveDiff;
  std::vector<DiffFilePresentation> retained;
  if (thread) {
    for (auto id = thread->turnOrder.rbegin();
         id != thread->turnOrder.rend() && liveDiff.isEmpty(); ++id) {
      const auto turn = thread->turns.find(*id);
      if (turn == thread->turns.end())
        continue;
      const auto domain = turn->second.domains.find("turn.diff.changed");
      if (domain != turn->second.domains.end())
        liveDiff = text(stringValue(domain->second, "diff"));
    }
    if (liveDiff.isEmpty()) {
      for (auto id = thread->turnOrder.rbegin();
           id != thread->turnOrder.rend() && retained.empty(); ++id) {
        const auto turn = thread->turns.find(*id);
        if (turn == thread->turns.end())
          continue;
        for (auto itemId = turn->second.itemOrder.rbegin();
             itemId != turn->second.itemOrder.rend(); ++itemId) {
          const auto item = turn->second.items.find(*itemId);
          if (item == turn->second.items.end() ||
              stringValue(item->second.raw, "type") != "fileChange")
            continue;
          for (const auto &change :
               item->second.raw.value("changes", nlohmann::json::array())) {
            QString kind = text(stringValue(change, "kind"));
            if (kind.isEmpty() && change.contains("kind") &&
                change["kind"].is_object())
              kind = text(stringValue(change["kind"], "type"));
            retained.push_back({text(stringValue(change, "path")),
                                std::move(kind),
                                text(stringValue(change, "diff"))});
          }
          if (!retained.empty())
            break;
        }
      }
    }
  }
  nlohmann::json signature{{"threadId", currentThreadId},
                           {"live", liveDiff.toStdString()}};
  for (const auto &change : retained)
    signature["retained"].push_back({change.path.toStdString(),
                                     change.kind.toStdString(),
                                     change.diff.toStdString()});
  const QByteArray next = bytes(signature);
  if (next == changesSnapshot)
    return;
  changesSnapshot = next;
  diffViewer->setChanges(std::move(liveDiff), std::move(retained));
}

void InspectorPane::refreshRequests() {
  nlohmann::json snapshot = nlohmann::json::array();
  for (const auto &[id, request] :
       currentModel->pendingRequestPresentations()) {
    const nlohmann::json questions =
        request.raw.value("questions", nlohmann::json::array());
    snapshot.push_back(
        {{"id", id},
         {"kind", request.kind},
         {"threadId", request.threadId},
         {"generation", request.generation},
         {"command", stringValue(request.raw, "command")},
         {"reason", stringValue(request.raw, "reason")},
         {"message", stringValue(request.raw, "message")},
         {"questionCount", questions.is_array() ? questions.size() : 0U}});
  }
  const QByteArray next = bytes(snapshot);
  if (next == requestsSnapshot)
    return;
  requestsSnapshot = next;
  setUpdatesEnabled(false);
  clearLayout(requestsLayout);
  for (const auto &[id, request] :
       currentModel->pendingRequestPresentations()) {
    auto *frame = new QFrame;
    frame->setProperty("kind", "summary");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(9, 7, 9, 7);
    layout->setSpacing(5);
    layout->addWidget(makeLabel(text(request.kind), "title"));
    QString threadContext = text(request.threadId);
    if (const ThreadPresentation *thread =
            currentModel->thread(request.threadId);
        thread && !thread->title.empty())
      threadContext = text(thread->title);
    layout->addWidget(
        makeLabel(QStringLiteral("thread %1  |  generation %2  |  request %3")
                      .arg(threadContext)
                      .arg(static_cast<qulonglong>(request.generation))
                      .arg(text(id)),
                  "meta"));
    for (const auto &[key, prefix] :
         std::array<std::pair<const char *, const char *>, 3>{
             {{"command", "Command: "},
              {"reason", "Reason: "},
              {"message", ""}}}) {
      const QString value = text(stringValue(request.raw, key));
      if (!value.isEmpty())
        layout->addWidget(
            makeLabel(QString::fromLatin1(prefix) + value, "meta"));
    }
    const auto questions = request.raw.find("questions");
    if (questions != request.raw.end() && questions->is_array())
      layout->addWidget(
          makeLabel(QStringLiteral("%1 questions")
                        .arg(static_cast<qulonglong>(questions->size())),
                    "meta"));
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 2, 0, 0);
    auto *deny = new QPushButton(QStringLiteral("Deny"));
    auto *review = new QPushButton(QStringLiteral("Review"));
    review->setProperty("kind", "primary");
    deny->setFixedHeight(28);
    review->setFixedHeight(28);
    connect(deny, &QPushButton::clicked, this, [this, id] {
      if (rejectRequest)
        rejectRequest(id);
    });
    connect(review, &QPushButton::clicked, this, [this, id] {
      if (reviewRequest)
        reviewRequest(id);
    });
    actions->addStretch();
    actions->addWidget(deny);
    actions->addWidget(review);
    layout->addLayout(actions);
    requestsLayout->addWidget(frame);
  }
  if (snapshot.empty())
    requestsLayout->addWidget(
        makeLabel(QStringLiteral("No pending requests."), "muted"));
  requestsLayout->addStretch();
  setUpdatesEnabled(true);
}

void InspectorPane::refreshState() {
  nlohmann::json domains = nlohmann::json::object();
  for (const auto &[name, value] : currentModel->globalDomains())
    domains[name] = value;
  nlohmann::json pending = nlohmann::json::object();
  for (const auto &[id, request] : currentModel->pendingRequestPresentations())
    pending[id] = {{"category", request.kind},
                   {"threadId", request.threadId},
                   {"generation", request.generation}};
  nlohmann::json state{{"models", currentModel->modelCatalog()},
                       {"pendingRequests", std::move(pending)},
                       {"domains", std::move(domains)}};
  std::string rendered = state.dump(2);
  constexpr std::size_t MaximumBytes = 32U * 1024U;
  if (rendered.size() > MaximumBytes) {
    const std::size_t total = rendered.size();
    rendered.resize(MaximumBytes);
    rendered += "\n\n[State display truncated at 32 KiB; retained bytes: " +
                std::to_string(total) + "]";
  }
  const QByteArray next(rendered.data(),
                        static_cast<qsizetype>(rendered.size()));
  if (next == stateSnapshot)
    return;
  stateSnapshot = next;
  stateView->setPlainText(text(rendered));
}

void InspectorPane::refreshProtocolStats() {
  std::size_t turns = 0;
  std::size_t items = 0;
  if (const ThreadPresentation *thread =
          currentModel->thread(currentThreadId)) {
    turns = thread->turnOrder.size();
    for (const auto &[id, turn] : thread->turns) {
      static_cast<void>(id);
      items += turn.itemOrder.size();
    }
  }
  const QString value =
      QStringLiteral("seq %1  |  threads %2  |  models %3  |  turns %4  |  "
                     "items %5  |  pending %6  |  telemetry %7")
          .arg(static_cast<qulonglong>(observedSequence))
          .arg(static_cast<qulonglong>(currentModel->threadOrder().size()))
          .arg(static_cast<qulonglong>(currentModel->modelCatalog().size()))
          .arg(static_cast<qulonglong>(turns))
          .arg(static_cast<qulonglong>(items))
          .arg(static_cast<qulonglong>(currentModel->pendingRequestCount()))
          .arg(static_cast<qulonglong>(currentModel->telemetry().size()));
  if (value.toUtf8() == protocolStatsSnapshot)
    return;
  protocolStatsSnapshot = value.toUtf8();
  protocolStats->setText(value);
}

void InspectorPane::showProtocolTail() {
  QStringList lines;
  lines.reserve(static_cast<qsizetype>(protocolLines.size()));
  for (const QString &line : protocolLines)
    lines << line;
  const QString value = lines.join(QLatin1Char('\n'));
  if (protocolLog->toPlainText() == value)
    return;
  const ScrollPosition position{protocolFollowsTail, protocolPausedScrollValue};
  mutatingProtocolLog = true;
  protocolLog->setPlainText(value);
  restoreProtocolScroll(position.followsTail, position.value);
}

void InspectorPane::restoreProtocolScroll(bool followsTail, int pausedValue) {
  const ScrollPosition position{followsTail, pausedValue};
  restoreScrollPosition(protocolLog, position);
  const std::uint64_t revision = ++protocolScrollRevision;
  QTimer::singleShot(0, this, [this, position, revision] {
    if (revision != protocolScrollRevision)
      return;
    restoreScrollPosition(protocolLog, position);
    protocolFollowsTail = position.followsTail;
    if (!position.followsTail)
      protocolPausedScrollValue = protocolLog->verticalScrollBar()->value();
    mutatingProtocolLog = false;
  });
}

void InspectorPane::appendProtocolFrame(const nlohmann::json &frame) {
  const auto record = [this](QString line) {
    if (protocolLines.size() >= MaximumProtocolLines)
      protocolLines.pop_front();
    protocolLines.push_back(line);
    const ScrollPosition position{protocolFollowsTail,
                                  protocolPausedScrollValue};
    mutatingProtocolLog = true;
    protocolLog->appendPlainText(line);
    restoreProtocolScroll(position.followsTail, position.value);
  };
  const std::uint64_t sequence = frame.value("sequence", 0ULL);
  if (sequence != 0) {
    if (observedSequence != 0 && sequence != observedSequence + 1) {
      record(QStringLiteral("[%1] %2 expected=%3 received=%4")
                 .arg(QDateTime::currentDateTime().toString(
                          QStringLiteral("HH:mm:ss.zzz")),
                      sequence <= observedSequence
                          ? QStringLiteral("NON-MONOTONIC")
                          : QStringLiteral("SEQUENCE GAP"))
                 .arg(static_cast<qulonglong>(observedSequence + 1))
                 .arg(static_cast<qulonglong>(sequence)));
    }
    observedSequence = std::max(observedSequence, sequence);
  }
  const std::string kind = stringValue(frame, "kind");
  const std::string subject = kind == "result" ? stringValue(frame, "action")
                                               : stringValue(frame, "type");
  const nlohmann::json scope = frame.value("scope", nlohmann::json::object());
  QStringList parts{QStringLiteral("[%1]").arg(
      QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))};
  if (sequence != 0)
    parts << QStringLiteral("#%1").arg(static_cast<qulonglong>(sequence));
  parts << QStringLiteral("g%1").arg(
               static_cast<qulonglong>(frame.value("generation", 0ULL)))
        << text(kind) << text(subject) << text(stringValue(frame, "authority"));
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
  const std::string correlation = stringValue(frame, "correlationId");
  if (!correlation.empty())
    parts << QStringLiteral("correlation=%1").arg(text(correlation));
  if (kind == "result" && !frame.value("ok", false)) {
    const std::string message =
        stringValue(frame.value("error", nlohmann::json::object()), "message");
    if (!message.empty())
      parts << text(message);
  }
  record(parts.join(QStringLiteral("  ")));
}

} // namespace codexui::codex::middle
