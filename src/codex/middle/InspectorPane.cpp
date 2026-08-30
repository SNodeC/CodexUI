// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/InspectorPane.h"

#include "codex/DiffViewer.h"
#include "codex/PresentationModel.h"
#include "codex/PresentationStatus.h"
#include "codex/ui/UiStyle.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {

constexpr int MaximumProtocolLines = 2000;
constexpr int InfoChoicePage = 0;
constexpr int StatePage = 1;
constexpr int ProtocolPage = 2;

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QStringList texts(const std::vector<std::string> &values) {
  QStringList result;
  result.reserve(static_cast<qsizetype>(values.size()));
  for (const std::string &value : values)
    result.push_back(text(value));
  return result;
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string{};
}

bool supportsDirectAccept(std::string_view kind) {
  return kind == "command-approval" || kind == "file-change-approval" ||
         kind == "permissions-approval" || kind == "legacy-patch-approval" ||
         kind == "legacy-command-approval";
}

QString directAcceptText(std::string_view kind) {
  if (kind == "permissions-approval")
    return QStringLiteral("Allow this turn");
  return QStringLiteral("Accept");
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

QLabel *statusLabel(const std::string &status) {
  const PresentationStatus classified = classifyStatus(status);
  auto *label = makeLabel(text(classified.text), "meta");
  if (!classified.tone.empty())
    label->setProperty("tone", classified.tone.data());
  return label;
}

std::string effectivePlanStepStatus(const std::string &stepStatus,
                                    const std::string &turnStatus,
                                    const std::string &threadStatus) {
  if (!isActiveStatus(stepStatus))
    return stepStatus;
  StatusKind outcome = classifyStatus(turnStatus).kind;
  if (outcome != StatusKind::Completed && outcome != StatusKind::Failed &&
      outcome != StatusKind::Interrupted)
    outcome = classifyStatus(threadStatus).kind;
  if (outcome == StatusKind::Completed)
    return "completed";
  if (outcome == StatusKind::Failed)
    return "failed";
  if (outcome == StatusKind::Interrupted)
    return "interrupted";
  return stepStatus;
}

QLabel *makeMarkdownLabel(const QString &value) {
  QTextDocument document;
  document.setMarkdown(
      value,
      QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
          QTextDocument::MarkdownNoHTML);
  auto *label = new QLabel(document.toHtml());
  label->setProperty("kind", "body");
  label->setTextFormat(Qt::RichText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setOpenExternalLinks(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::LinksAccessibleByMouse |
                                 Qt::LinksAccessibleByKeyboard);
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

class InfoChoiceButton final : public QPushButton {
protected:
  void paintEvent(QPaintEvent *event) override {
    QPushButton::paintEvent(event);
    QStyleOptionButton option;
    initStyleOption(&option);
    const QRect contents = style()->subElementRect(
        QStyle::SE_PushButtonContents, &option, this);
    const QRect indicator(contents.right() - 18, contents.top(), 18,
                          contents.height());
    UiStyle::drawChevron(
        this, indicator, option.state & QStyle::State_Enabled,
        option.state & (QStyle::State_MouseOver | QStyle::State_HasFocus),
        UiStyle::ChevronDirection::Right);
  }
};

QPushButton *infoChoice(const QString &title, const QString &description) {
  auto *button = new InfoChoiceButton;
  button->setProperty("kind", "infoChoice");
  button->setMinimumHeight(64);
  button->setCursor(Qt::PointingHandCursor);

  auto *layout = new QHBoxLayout(button);
  layout->setContentsMargins(12, 9, 30, 9);
  layout->setSpacing(8);
  auto *copy = new QVBoxLayout;
  copy->setSpacing(2);
  auto *titleLabel = makeLabel(title, "title");
  auto *descriptionLabel = makeLabel(description, "meta");
  titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  descriptionLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
  descriptionLabel->setTextInteractionFlags(Qt::NoTextInteraction);
  copy->addWidget(titleLabel);
  copy->addWidget(descriptionLabel);
  layout->addLayout(copy, 1);
  return button;
}

QWidget *infoDetail(const QString &title, QWidget *content,
                    QPushButton **backButton) {
  auto *page = new QWidget;
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(8);
  auto *heading = new QHBoxLayout;
  *backButton = new QPushButton(QStringLiteral("‹  Info"));
  (*backButton)->setProperty("kind", "subtle");
  (*backButton)->setFixedHeight(28);
  heading->addWidget(*backButton);
  heading->addStretch();
  heading->addWidget(makeLabel(title, "title"));
  layout->addLayout(heading);
  layout->addWidget(content, 1);
  return page;
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

QFrame *InspectorPane::agentFrame(const AgentSnapshot &agent) {
  auto *frame = new QFrame;
  frame->setProperty("kind", "raised");
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(6);
  const QString agentPath = text(agent.agentPath);
  const QStringList pathParts = agentPath.split('/', Qt::SkipEmptyParts);
  QString agentName;
  if (!pathParts.isEmpty())
    agentName = pathParts.back();
  else if (!agent.tool.empty())
    agentName = text(agent.tool);
  auto *heading = new QHBoxLayout;
  heading->setContentsMargins(0, 0, 0, 0);
  heading->setSpacing(6);
  auto *titleLabel = makeLabel(QStringLiteral("Agent"), "title");
  titleLabel->setObjectName(QStringLiteral("agentTitle"));
  titleLabel->setWordWrap(false);
  titleLabel->setContentsMargins(0, 0, 0, 1);
  titleLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  heading->addWidget(titleLabel, 0, Qt::AlignBaseline);
  if (!agentName.isEmpty()) {
    auto *nameLabel = makeLabel(agentName, "code");
    nameLabel->setObjectName(QStringLiteral("agentName"));
    nameLabel->setWordWrap(false);
    // The fixed-width font's descent sits one pixel below the proportional
    // labels. Preserve their visual baseline without changing its font.
    nameLabel->setContentsMargins(0, 0, 0, 1);
    nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    if (!agentPath.isEmpty())
      nameLabel->setToolTip(agentPath);
    heading->addWidget(nameLabel, 0, Qt::AlignBottom);
  }
  heading->addStretch();
  auto *status = statusLabel(agent.status);
  status->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  heading->addWidget(status, 0, Qt::AlignBaseline);
  layout->addLayout(heading);
  QStringList metadata;
  if (!agent.tool.empty() && !agentPath.isEmpty())
    metadata << text(agent.tool);
  for (const std::string *value : {&agent.model, &agent.reasoningEffort}) {
    if (!value->empty())
      metadata << text(*value);
  }
  if (!metadata.isEmpty())
    layout->addWidget(makeLabel(metadata.join(QStringLiteral("  ·  ")), "meta"));
  if (!agent.prompt.empty())
    layout->addWidget(makeLabel(text(agent.prompt)));
  if (!agent.resultText.empty()) {
    auto *result = makeMarkdownLabel(text(agent.resultText));
    result->setObjectName(QStringLiteral("agentResult"));
    result->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(result);
  }
  QStringList identities;
  if (!agent.childThreadId.empty())
    identities << QStringLiteral("thread %1").arg(text(agent.childThreadId));
  if (!agent.senderThreadId.empty())
    identities << QStringLiteral("sender %1").arg(text(agent.senderThreadId));
  const QString receivers =
      texts(agent.receiverThreadIds).join(QStringLiteral(", "));
  if (!receivers.isEmpty())
    identities << QStringLiteral("receivers %1").arg(receivers);
  if (!identities.isEmpty())
    layout->addWidget(
        makeLabel(identities.join(QStringLiteral("  |  ")), "meta"));
  return frame;
}

InspectorPane::InspectorPane(QWidget *parent) : QFrame(parent) {
  setObjectName(QStringLiteral("inspector"));
  setMinimumWidth(300);
  setMaximumWidth(520);

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(18, 14, 20, 0);
  outer->setSpacing(0);
  auto *heading = new QHBoxLayout;
  heading->addStrut(24);
  auto *sectionTitle = makeLabel(QStringLiteral("INSPECTOR"), "panelHeader");
  sectionTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  sectionTitle->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  heading->addWidget(sectionTitle);
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
  auto *headerDivider = new QFrame;
  headerDivider->setProperty("kind", "standardDivider");
  headerDivider->setFixedHeight(1);
  outer->addWidget(headerDivider);
  outer->addSpacing(8);

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
    scroll->setProperty("kind", "inspectorScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidget(content);
    return scroll;
  };

  stateView = new QPlainTextEdit;
  stateView->setObjectName(QStringLiteral("stateInfoView"));
  stateView->setProperty("kind", "infoViewer");
  stateView->setReadOnly(true);
  stateView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  stateView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stateView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  auto *protocolContent = new QWidget;
  auto *protocolLayout = new QVBoxLayout(protocolContent);
  protocolLayout->setContentsMargins(0, 0, 0, 0);
  protocolLayout->setSpacing(6);
  protocolLog = new QPlainTextEdit;
  protocolLog->setObjectName(QStringLiteral("protocolInfoLog"));
  protocolLog->setProperty("kind", "infoViewer");
  protocolLog->setReadOnly(true);
  protocolLog->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  protocolLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  protocolLog->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
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

  infoStack = new QStackedWidget;
  infoStack->setObjectName(QStringLiteral("infoStack"));
  auto *choices = new QWidget;
  auto *choicesLayout = new QVBoxLayout(choices);
  choicesLayout->setContentsMargins(8, 8, 8, 8);
  choicesLayout->setSpacing(8);
  auto *stateChoice = infoChoice(
      QStringLiteral("State"), QStringLiteral("Current application state"));
  stateChoice->setObjectName(QStringLiteral("stateInfoChoice"));
  auto *protocolChoice = infoChoice(
      QStringLiteral("Protocol"), QStringLiteral("App-server protocol messages"));
  protocolChoice->setObjectName(QStringLiteral("protocolInfoChoice"));
  choicesLayout->addWidget(stateChoice);
  choicesLayout->addWidget(protocolChoice);
  choicesLayout->addStretch();

  QPushButton *stateBack = nullptr;
  QPushButton *protocolBack = nullptr;
  infoStack->addWidget(choices);
  infoStack->addWidget(infoDetail(QStringLiteral("State"), stateView,
                                  &stateBack));
  infoStack->addWidget(infoDetail(QStringLiteral("Protocol"), protocolContent,
                                  &protocolBack));
  connect(stateChoice, &QPushButton::clicked, this, [this] {
    infoStack->setCurrentIndex(StatePage);
    refreshCurrentTab();
  });
  connect(protocolChoice, &QPushButton::clicked, this, [this] {
    infoStack->setCurrentIndex(ProtocolPage);
    showProtocolTail();
    refreshCurrentTab();
  });
  const auto showInfoChoices = [this] {
    infoStack->setCurrentIndex(InfoChoicePage);
  };
  connect(stateBack, &QPushButton::clicked, this, showInfoChoices);
  connect(protocolBack, &QPushButton::clicked, this, showInfoChoices);

  inspectorTabs->addTab(makeScroll(planContent), QStringLiteral("Plan"));
  inspectorTabs->addTab(makeScroll(agentsContent), QStringLiteral("Agents"));
  inspectorTabs->addTab(diffViewer, QStringLiteral("Changes"));
  inspectorTabs->addTab(makeScroll(requestsContent),
                        QStringLiteral("Requests"));
  inspectorTabs->addTab(infoStack, QStringLiteral("Info"));
  outer->addWidget(inspectorTabs, 1);

  connect(inspectorTabs, &QTabWidget::currentChanged, this,
          [this](int) { refreshCurrentTab(); });
}

void InspectorPane::setHideAction(std::function<void()> hide) {
  hideAction = std::move(hide);
}

void InspectorPane::setRequestActions(RequestAction review,
                                      RequestAction accept,
                                      RequestAction reject,
                                      RequestEligibility eligible) {
  reviewRequest = std::move(review);
  acceptRequest = std::move(accept);
  rejectRequest = std::move(reject);
  requestEligible = std::move(eligible);
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
    if (infoStack->currentIndex() == StatePage)
      refreshState();
    else if (infoStack->currentIndex() == ProtocolPage) {
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
  PlanSnapshot next;
  next.threadId = currentThreadId;
  next.threadPresent = thread != nullptr;
  if (thread) {
    for (auto id = thread->turnOrder.rbegin(); id != thread->turnOrder.rend();
         ++id) {
      const auto turn = thread->turns.find(*id);
      if (turn == thread->turns.end())
        continue;
      if (turn->second.plan.is_object() &&
          turn->second.plan.contains("steps")) {
        PlanContentSnapshot plan;
        plan.explanation = stringValue(turn->second.plan, "explanation");
        for (const auto &step :
             turn->second.plan.value("steps", nlohmann::json::array())) {
          const std::string status = stringValue(step, "status");
          plan.steps.push_back({
              stringValue(step, "step"),
              effectivePlanStepStatus(status, turn->second.status,
                                      thread->status)});
        }
        next.plan = std::move(plan);
        break;
      }
      for (auto itemId = turn->second.itemOrder.rbegin();
           itemId != turn->second.itemOrder.rend(); ++itemId) {
        const auto item = turn->second.items.find(*itemId);
        if (item != turn->second.items.end() &&
            stringValue(item->second.raw, "type") == "plan") {
          next.planItem = stringValue(item->second.raw, "text");
          break;
        }
      }
      if (next.planItem)
        break;
    }
  }
  if (planSnapshot && *planSnapshot == next)
    return;
  planSnapshot = std::move(next);
  const PlanSnapshot &snapshot = *planSnapshot;
  setUpdatesEnabled(false);
  clearLayout(planLayout);
  if (!snapshot.threadPresent) {
    planLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
  } else if (snapshot.plan) {
    const QString explanation = text(snapshot.plan->explanation);
    if (!explanation.isEmpty())
      planLayout->addWidget(makeMarkdownLabel(explanation));
    for (const PlanStepSnapshot &step : snapshot.plan->steps) {
      auto *row = new QFrame;
      row->setProperty("kind", "raised");
      auto *layout = new QVBoxLayout(row);
      layout->setContentsMargins(12, 10, 12, 10);
      layout->setSpacing(6);
      layout->addWidget(makeLabel(text(step.step)));
      layout->addWidget(statusLabel(step.status));
      planLayout->addWidget(row);
    }
  } else if (snapshot.planItem) {
    const QString value = text(*snapshot.planItem);
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
  AgentsSnapshot next;
  next.threadId = currentThreadId;
  next.threadPresent = thread != nullptr;
  if (thread) {
    next.agents.reserve(thread->agentOrder.size());
    for (const std::string &id : thread->agentOrder) {
      const auto agent = thread->agents.find(id);
      if (agent == thread->agents.end())
        continue;
      AgentSnapshot snapshot;
      snapshot.id = id;
      snapshot.status = agent->second.status;
      snapshot.childThreadId = agent->second.childThreadId;
      snapshot.agentPath = stringValue(agent->second.raw, "agentPath");
      snapshot.tool = stringValue(agent->second.raw, "tool");
      snapshot.model = stringValue(agent->second.raw, "model");
      snapshot.reasoningEffort =
          stringValue(agent->second.raw, "reasoningEffort");
      snapshot.prompt = stringValue(agent->second.raw, "prompt");
      snapshot.resultText = stringValue(agent->second.raw, "resultText");
      snapshot.senderThreadId =
          stringValue(agent->second.raw, "senderThreadId");
      const auto receivers = agent->second.raw.find("receiverThreadIds");
      if (receivers != agent->second.raw.end() && receivers->is_array()) {
        for (const auto &receiver : *receivers) {
          if (receiver.is_string())
            snapshot.receiverThreadIds.push_back(
                receiver.get<std::string>());
        }
      }
      next.agents.push_back(std::move(snapshot));
    }
  }
  if (agentsSnapshot && *agentsSnapshot == next)
    return;
  agentsSnapshot = std::move(next);
  const AgentsSnapshot &snapshot = *agentsSnapshot;
  setUpdatesEnabled(false);
  clearLayout(agentsLayout);
  if (!snapshot.threadPresent)
    agentsLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
  else if (snapshot.agents.empty())
    agentsLayout->addWidget(makeLabel(
        QStringLiteral("No agent activity for this thread."), "muted"));
  else
    for (const AgentSnapshot &agent : snapshot.agents)
      agentsLayout->addWidget(agentFrame(agent));
  agentsLayout->addStretch();
  setUpdatesEnabled(true);
}

void InspectorPane::refreshChanges() {
  const ThreadPresentation *thread = currentModel->thread(currentThreadId);
  diffViewer->setRepositoryContext(
      text(currentThreadId), thread ? text(thread->cwd) : QString{},
      thread ? texts(thread->commandCwds) : QStringList{},
      thread ? texts(thread->changedPaths) : QStringList{});
  diffViewer->refreshRepository();
}

void InspectorPane::refreshRequests() {
  std::vector<RequestSnapshot> next;
  next.reserve(currentModel->pendingRequestCount());
  for (const auto &[id, request] :
       currentModel->pendingRequestPresentations()) {
    RequestSnapshot snapshot;
    snapshot.id = id;
    snapshot.kind = request.kind;
    snapshot.threadContext = request.threadId;
    if (const ThreadPresentation *thread =
            currentModel->thread(request.threadId);
        thread && !thread->title.empty())
      snapshot.threadContext = thread->title;
    snapshot.generation = request.generation;
    snapshot.command = stringValue(request.raw, "command");
    snapshot.reason = stringValue(request.raw, "reason");
    snapshot.message = stringValue(request.raw, "message");
    const auto questions = request.raw.find("questions");
    if (questions != request.raw.end() && questions->is_array())
      snapshot.questionCount = questions->size();
    snapshot.actionable = requestEligible && requestEligible(id);
    next.push_back(std::move(snapshot));
  }
  if (requestsSnapshot && *requestsSnapshot == next)
    return;
  requestsSnapshot = std::move(next);
  const std::vector<RequestSnapshot> &snapshot = *requestsSnapshot;
  setUpdatesEnabled(false);
  clearLayout(requestsLayout);
  for (const RequestSnapshot &request : snapshot) {
    auto *frame = new QFrame;
    frame->setProperty("kind", "raised");
    frame->setProperty("tone", "warning");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    layout->addWidget(
        makeLabel(UiStyle::humanizeLabel(text(request.kind)), "title"));
    layout->addWidget(
        makeLabel(QStringLiteral("thread %1  |  generation %2  |  request %3")
                      .arg(text(request.threadContext))
                      .arg(static_cast<qulonglong>(request.generation))
                      .arg(text(request.id)),
                  "meta"));
    const auto addMetadata = [layout](const std::string &value,
                                      const char *prefix) {
      const QString displayed = text(value);
      if (!displayed.isEmpty())
        layout->addWidget(
            makeLabel(QString::fromLatin1(prefix) + displayed, "meta"));
    };
    addMetadata(request.command, "Command: ");
    addMetadata(request.reason, "Reason: ");
    addMetadata(request.message, "");
    if (request.questionCount)
      layout->addWidget(
          makeLabel(QStringLiteral("%1 questions")
                        .arg(static_cast<qulonglong>(*request.questionCount)),
                    "meta"));
    if (request.command.empty() && request.reason.empty() &&
        request.message.empty() && !request.questionCount)
      layout->addWidget(
          makeLabel(QStringLiteral("Request %1 needs a decision.")
                        .arg(text(request.id)),
                    "meta"));
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 2, 0, 0);
    auto *reject = new QPushButton(QStringLiteral("Reject"));
    reject->setProperty("kind", "destructive");
    reject->setFixedHeight(28);
    reject->setEnabled(request.actionable);
    connect(reject, &QPushButton::clicked, this, [this, id = request.id] {
      if (rejectRequest)
        rejectRequest(id);
    });
    actions->addStretch();
    actions->addWidget(reject);
    if (supportsDirectAccept(request.kind)) {
      auto *accept = new QPushButton(directAcceptText(request.kind));
      accept->setProperty("kind", "request");
      accept->setFixedHeight(28);
      accept->setEnabled(request.actionable);
      connect(accept, &QPushButton::clicked, this, [this, id = request.id] {
        if (acceptRequest)
          acceptRequest(id);
      });
      actions->addWidget(accept);
    } else {
      auto *review = new QPushButton(QStringLiteral("Review"));
      review->setProperty("kind", "request");
      review->setFixedHeight(28);
      review->setEnabled(request.actionable);
      connect(review, &QPushButton::clicked, this, [this, id = request.id] {
        if (reviewRequest)
          reviewRequest(id);
      });
      actions->addWidget(review);
    }
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
