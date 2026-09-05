// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/QtNodeAttachment.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace codexui::codex::middle {
namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

std::string utf8(const QString &value) { return value.toUtf8().toStdString(); }

class LayoutRequestProbe final : public QObject {
public:
  explicit LayoutRequestProbe(QWidget *root) : root_(root) {
    qApp->installEventFilter(this);
  }

  ~LayoutRequestProbe() override { qApp->removeEventFilter(this); }

  void start() {
    count = 0;
    active = true;
  }

  int count = 0;
  bool active = false;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (active && event->type() == QEvent::LayoutRequest && widget &&
        (widget == root_ || root_->isAncestorOf(widget)))
      ++count;
    return false;
  }

private:
  QWidget *root_ = nullptr;
};

void spin(int milliseconds = 0) {
  if (milliseconds == 0) {
    // One selected/load-more page is admitted in eight-card slices. Drain a
    // bounded page worth of zero-delay continuations without turning every
    // test settle into an arbitrary wall-clock delay.
    for (int pass = 0; pass < 16; ++pass)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    return;
  }
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (milliseconds > 0)
      QThread::msleep(1);
  } while (timer.elapsed() < milliseconds);
}

template <typename Predicate>
bool spinUntil(Predicate &&predicate, int maximumPasses = 64) {
  for (int pass = 0; pass < maximumPasses; ++pass) {
    if (predicate())
      return true;
    // Zero-delay continuations normally drain in one event dispatch, while
    // contention recovery deliberately uses a small nonzero timer. Give both
    // paths a real event-loop tick without assuming synchronous completion.
    spin(1);
  }
  return predicate();
}

template <typename Predicate>
bool dispatchUntil(Predicate &&predicate, int maximumPasses = 512) {
  for (int pass = 0; pass < maximumPasses; ++pass) {
    if (predicate())
      return true;
    QCoreApplication::processEvents(QEventLoop::AllEvents);
  }
  return predicate();
}

void dispatchPasses(int count) {
  for (int pass = 0; pass < count; ++pass)
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

VisibleCardData agentCard(const std::string &threadId,
                          const std::string &turnId, int index,
                          QString text = {}) {
  const std::string itemId = "agent-" + std::to_string(index);
  if (text.isEmpty())
    text = QStringLiteral("Codex output line %1 with enough text to wrap a "
                          "little in the viewport.")
               .arg(index);
  return {AuthoritativeItemKey{threadId, turnId, itemId},
          CardKind::AgentMessage,
          threadId,
          turnId,
          itemId,
          AgentMessageData{utf8(text), index % 3 == 0}};
}

VisibleCardData cardForAppearanceAudit(const std::string &threadId,
                                       CardKind kind, int index) {
  const std::string itemId = "appearance-" + std::to_string(index);
  CardKey key =
      kind == CardKind::LocalPrompt
          ? CardKey{LocalPromptKey{9000U + static_cast<std::uint64_t>(index)}}
          : CardKey{AuthoritativeItemKey{threadId, "turn-2", itemId}};
  CardPayload payload = GenericActivityData{};
  switch (kind) {
  case CardKind::UserMessage:
    payload = UserMessageData{"User appearance audit", {}};
    break;
  case CardKind::AgentMessage:
    payload = AgentMessageData{"Agent appearance audit", true};
    break;
  case CardKind::CommandExecution:
    payload = CommandExecutionData{"printf audit", {}, "inProgress",
                                   "/workspace",   {}, {}};
    break;
  case CardKind::AgentActivity:
    payload = AgentActivityData{"spawn_agent",
                                "inProgress",
                                "tool",
                                "Inspect appearance",
                                {},
                                {},
                                {},
                                {},
                                {},
                                {},
                                {}};
    break;
  case CardKind::Reasoning:
    payload = ReasoningData{"Initial reasoning summary"};
    break;
  case CardKind::FileChanges:
    payload = FileChangesData{"inProgress", {{"src/a.cpp", "update", 1, 0}}};
    break;
  case CardKind::ImageGeneration:
    payload = ImageGenerationData{{}, "inProgress", "Initial image prompt"};
    break;
  case CardKind::Plan:
    payload = PlanData{"Initial plan", {{"Inspect", "inProgress"}}, {}};
    break;
  case CardKind::GenericActivity:
    payload = GenericActivityData{
        "unknownActivity",
        {{"type", "unknownActivity"}, {"status", "inProgress"}}};
    break;
  case CardKind::LocalPrompt:
    payload = LocalPromptData{9000U + static_cast<std::uint64_t>(index),
                              "Local prompt appearance audit",
                              PromptState::InFlight,
                              0,
                              {},
                              {}};
    break;
  }
  return {std::move(key), kind, threadId, "turn-2", itemId, std::move(payload)};
}

// ConversationView is graph-only. These concise fixture records keep the
// presentation-oriented test cases readable while applyConversation writes
// their current facts into the same NodeGraph shape used by the application.
struct TurnGraphSpec {
  std::string key;
  std::string turnId;
  std::vector<VisibleCardData> cards;
  std::optional<CardKey> rootCardKey;
};

struct FixtureGraph final {
  nodegraph::NodeGraph graph;
};

struct ConversationGraphSpec {
  std::string threadId;
  std::vector<TurnGraphSpec> sections;
  std::size_t hiddenAuthoritativeItemCount = 0;
  bool hasMore = false;
  std::optional<std::string> activeTurnId;
  mutable std::shared_ptr<FixtureGraph> storage =
      std::make_shared<FixtureGraph>();
};

nodegraph::Value graphValue(const nlohmann::json &value) {
  if (value.is_null())
    return nullptr;
  if (value.is_boolean())
    return value.get<bool>();
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>();
  if (value.is_number_integer())
    return value.get<std::int64_t>();
  if (value.is_number_float())
    return value.get<double>();
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_array()) {
    nodegraph::Value::Array result;
    result.reserve(value.size());
    for (const nlohmann::json &entry : value)
      result.push_back(graphValue(entry));
    return result;
  }
  nodegraph::Value::Object result;
  for (const auto &[key, entry] : value.items())
    result.emplace(key, graphValue(entry));
  return result;
}

nodegraph::NodeStatus graphStatus(std::string_view status) {
  if (status == "pending" || status == "inProgress" || status == "running")
    return nodegraph::NodeStatus::Running;
  if (status == "completed")
    return nodegraph::NodeStatus::Completed;
  if (status == "failed")
    return nodegraph::NodeStatus::Failed;
  if (status == "interrupted")
    return nodegraph::NodeStatus::Interrupted;
  return nodegraph::NodeStatus::Unknown;
}

nodegraph::Value stringArray(const std::vector<std::string> &values) {
  nodegraph::Value::Array result;
  result.reserve(values.size());
  for (const std::string &value : values)
    result.emplace_back(value);
  return result;
}

std::string fixtureNodeId(const VisibleCardData &card) {
  if (const auto *authoritative = std::get_if<AuthoritativeItemKey>(&card.key))
    return authoritative->itemId;
  if (const auto *prompt = std::get_if<LocalPromptKey>(&card.key))
    return "fixture-local-prompt:" + std::to_string(prompt->submissionId);
  return stableKey(card.key);
}

nodegraph::NodeState fixtureNodeState(const VisibleCardData &card) {
  nodegraph::NodeState state;
  auto &fields = state.fields;
  switch (card.kind) {
  case CardKind::UserMessage: {
    const auto &data = std::get<UserMessageData>(card.payload);
    fields.emplace("type", "userMessage");
    fields.emplace("text", data.text);
    nodegraph::Value::Array content;
    for (const std::string &path : data.imagePaths) {
      nodegraph::Value::Object image;
      image.emplace("type", "localImage");
      image.emplace("path", path);
      content.emplace_back(std::move(image));
    }
    fields.emplace("content", std::move(content));
    state.status = nodegraph::NodeStatus::Completed;
    break;
  }
  case CardKind::AgentMessage: {
    const auto &data = std::get<AgentMessageData>(card.payload);
    fields.emplace("type", "agentMessage");
    fields.emplace("text", data.text);
    fields.emplace("phase", data.finalAnswer ? "final_answer" : "commentary");
    state.status = nodegraph::NodeStatus::Completed;
    break;
  }
  case CardKind::CommandExecution: {
    const auto &data = std::get<CommandExecutionData>(card.payload);
    fields.emplace("type", "commandExecution");
    fields.emplace("command", data.command);
    fields.emplace("aggregatedOutput", data.output);
    fields.emplace("status", data.status);
    fields.emplace("cwd", data.cwd);
    if (data.exitCode)
      fields.emplace("exitCode", *data.exitCode);
    if (data.durationMilliseconds)
      fields.emplace("durationMs", *data.durationMilliseconds);
    state.status = graphStatus(data.status);
    break;
  }
  case CardKind::AgentActivity: {
    const auto &data = std::get<AgentActivityData>(card.payload);
    fields.emplace("type", "collabAgentToolCall");
    fields.emplace("tool", data.tool);
    fields.emplace("status", data.status);
    fields.emplace("kind", data.kind);
    fields.emplace("prompt", data.prompt);
    fields.emplace("resultText", data.resultText);
    fields.emplace("receiverThreadIds", stringArray(data.receivers));
    fields.emplace("model", data.model);
    fields.emplace("reasoningEffort", data.reasoningEffort);
    fields.emplace("agentThreadId", data.childThreadId);
    fields.emplace("agentPath", data.agentPath);
    fields.emplace("senderThreadId", data.senderThreadId);
    state.status = graphStatus(data.status);
    break;
  }
  case CardKind::Reasoning: {
    fields.emplace("type", "reasoning");
    fields.emplace("summary", std::get<ReasoningData>(card.payload).summary);
    state.status = nodegraph::NodeStatus::Completed;
    break;
  }
  case CardKind::FileChanges: {
    const auto &data = std::get<FileChangesData>(card.payload);
    fields.emplace("type", "fileChange");
    fields.emplace("status", data.status);
    nodegraph::Value::Array changes;
    for (const FileChangeData &change : data.changes) {
      nodegraph::Value::Object entry;
      entry.emplace("path", change.path);
      entry.emplace("kind", change.kind);
      std::string diff;
      for (int index = 0; index < change.additions.value_or(0); ++index)
        diff += "+added\n";
      for (int index = 0; index < change.deletions.value_or(0); ++index)
        diff += "-removed\n";
      entry.emplace("diff", std::move(diff));
      changes.emplace_back(std::move(entry));
    }
    fields.emplace("changes", std::move(changes));
    state.status = graphStatus(data.status);
    break;
  }
  case CardKind::ImageGeneration: {
    const auto &data = std::get<ImageGenerationData>(card.payload);
    fields.emplace("type", "imageGeneration");
    fields.emplace("path", data.path);
    fields.emplace("status", data.status);
    fields.emplace("revisedPrompt", data.revisedPrompt);
    state.status = graphStatus(data.status);
    break;
  }
  case CardKind::Plan: {
    const auto &data = std::get<PlanData>(card.payload);
    fields.emplace("type", "plan");
    fields.emplace("text", data.legacyText);
    fields.emplace("planExplanation", data.explanation);
    nodegraph::Value::Array steps;
    for (const PlanStepData &step : data.steps) {
      nodegraph::Value::Object entry;
      entry.emplace("step", step.text);
      entry.emplace("status", step.status);
      steps.emplace_back(std::move(entry));
    }
    fields.emplace("plan", std::move(steps));
    state.status = nodegraph::NodeStatus::Running;
    break;
  }
  case CardKind::GenericActivity: {
    const auto &data = std::get<GenericActivityData>(card.payload);
    const nodegraph::Value raw = graphValue(data.raw);
    if (const auto *object = raw.asObject())
      fields = *object;
    fields.insert_or_assign("type", data.type);
    fields.insert_or_assign("status", data.status);
    if (!data.displayDetail.empty())
      fields.insert_or_assign("detail", data.displayDetail);
    state.status = graphStatus(data.status);
    break;
  }
  case CardKind::LocalPrompt: {
    const auto &data = std::get<LocalPromptData>(card.payload);
    fields.emplace("type", "localPrompt");
    fields.emplace("submissionId", data.submissionId);
    fields.emplace("text", data.prompt);
    fields.emplace("error", data.error);
    const char *dispatch = "queued";
    switch (data.state) {
    case PromptState::Queued:
      break;
    case PromptState::InFlight:
      dispatch = "inFlight";
      break;
    case PromptState::Accepted:
      dispatch = "awaitingMaterialization";
      break;
    case PromptState::Failed:
      dispatch = "failed";
      break;
    }
    fields.emplace("dispatchState", dispatch);
    fields.emplace("showPendingAnimation", data.showPendingAnimation);
    if (data.admittedAtMs)
      fields.emplace("admittedAtMs", *data.admittedAtMs);
    nodegraph::Value::Array attachments;
    for (const std::string &path : data.imagePaths) {
      nodegraph::Value::Object attachment;
      attachment.emplace("path", path);
      attachment.emplace("mimeType", "image/test");
      attachments.emplace_back(std::move(attachment));
    }
    fields.emplace("attachments", std::move(attachments));
    state.status = data.state == PromptState::Failed
                       ? nodegraph::NodeStatus::Failed
                       : nodegraph::NodeStatus::Running;
    break;
  }
  }
  return state;
}

struct BoundFixture final {
  std::shared_ptr<FixtureGraph> storage;
  nodegraph::NodeRef thread;
};

std::unordered_map<ConversationView *, BoundFixture> &fixtureBindings() {
  static std::unordered_map<ConversationView *, BoundFixture> bindings;
  return bindings;
}

bool applyConversation(ConversationView &view,
                       const ConversationGraphSpec &snapshot) {
  ConversationSnapshot projected;
  projected.threadId = snapshot.threadId;
  projected.hiddenAuthoritativeItemCount =
      snapshot.hiddenAuthoritativeItemCount;
  projected.hasMore = snapshot.hasMore;
  projected.activeTurnId = snapshot.activeTurnId;
  projected.sections.reserve(snapshot.sections.size());
  for (const TurnGraphSpec &section : snapshot.sections)
    projected.sections.push_back(
        {section.key, section.turnId, section.cards, section.rootCardKey});
  return view.reconcile(projected);
}

ConversationGraphSpec conversation(const std::string &threadId, int count) {
  ConversationGraphSpec result;
  result.threadId = threadId;
  TurnGraphSpec first{"turn:" + threadId + ":1", "turn-1", {}};
  TurnGraphSpec second{"turn:" + threadId + ":2", "turn-2", {}};
  for (int index = 0; index < count; ++index)
    (index < count / 2 ? first : second)
        .cards.push_back(agentCard(
            threadId, index < count / 2 ? "turn-1" : "turn-2", index));
  result.sections.push_back(std::move(first));
  result.sections.push_back(std::move(second));
  return result;
}

struct GraphConversationFixture final {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef reasoning;
  std::vector<nodegraph::NodeRef> messages;

  GraphConversationFixture() {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{65});
    threadState.fields.emplace("hydrationState", "ready");
    thread = write.upsert({nodegraph::NodeKind::Thread, "graph-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "graph-turn"});
    write.setParent(thread, turn);

    nodegraph::NodeState reasoningState;
    reasoningState.status = nodegraph::NodeStatus::Completed;
    reasoningState.fields.emplace("type", "reasoning");
    reasoningState.fields.emplace("summary", "latest hidden reasoning");
    reasoning = write.upsert({nodegraph::NodeKind::Item, "reasoning-item"},
                             std::move(reasoningState));
    write.setParent(turn, reasoning);

    messages.reserve(64);
    for (int index = 0; index < 64; ++index) {
      nodegraph::NodeState state;
      state.status = nodegraph::NodeStatus::Completed;
      state.fields.emplace("type", "agentMessage");
      state.fields.emplace("phase", "final_answer");
      state.fields.emplace("text", "Graph message " + std::to_string(index));
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item, "graph-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
      messages.push_back(std::move(item));
    }
    static_cast<void>(write.finish());
  }
};

ui::QtNodeAttachment *graphAttachment(const nodegraph::NodeRef &node) {
  return node ? static_cast<ui::QtNodeAttachment *>(node->uiAttachment())
              : nullptr;
}

nodegraph::NodeState graphMessageState(std::string type, std::string text) {
  nodegraph::NodeState state;
  state.status = nodegraph::NodeStatus::Completed;
  state.fields.emplace("type", std::move(type));
  state.fields.emplace("text", std::move(text));
  return state;
}

QPushButton *historyButton(ConversationView &view) {
  const auto buttons = view.findChildren<QPushButton *>();
  const auto found = std::ranges::find_if(buttons, [](QPushButton *button) {
    return button->property("kind").toString() == QStringLiteral("history");
  });
  return found == buttons.end() ? nullptr : *found;
}

QLabel *conversationEmptyLabel(ConversationView &view) {
  const auto labels = view.findChildren<QLabel *>();
  const auto found = std::ranges::find_if(labels, [](QLabel *label) {
    return label->text() ==
           QStringLiteral("Conversation activity appears here.");
  });
  return found == labels.end() ? nullptr : *found;
}

bool testMessageIdentityPalette() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  ConversationCard user(
      VisibleCardData{AuthoritativeItemKey{"identity-palette", "turn", "user"},
                      CardKind::UserMessage, "identity-palette", "turn", "user",
                      UserMessageData{"Prompt", {}}});
  ConversationCard update(VisibleCardData{
      AuthoritativeItemKey{"identity-palette", "turn", "update"},
      CardKind::AgentMessage, "identity-palette", "turn", "update",
      AgentMessageData{"Working", false}});
  ConversationCard final(
      VisibleCardData{AuthoritativeItemKey{"identity-palette", "turn", "final"},
                      CardKind::AgentMessage, "identity-palette", "turn",
                      "final", AgentMessageData{"Response", true}});
  for (ConversationCard *card : {&user, &update, &final}) {
    card->resize(600, card->sizeHint().height());
    card->show();
  }
  spin();

  const auto titleColor = [](ConversationCard &card) {
    for (QLabel *label : card.findChildren<QLabel *>())
      if (label->property("kind").toString() == QStringLiteral("title"))
        return label->palette().color(QPalette::WindowText);
    return QColor{};
  };
  const auto surfaceColor = [](ConversationCard &card) {
    const QImage rendered = card.grab().toImage();
    return rendered.pixelColor(rendered.width() - 10, rendered.height() - 10);
  };
  const bool result = expect(
      titleColor(user) ==
              QColor(QString::fromLatin1(codexui::UiStyle::blueHover)) &&
          surfaceColor(user) == QColor(QStringLiteral("#eaf2ff")) &&
          surfaceColor(update) ==
              QColor(QString::fromLatin1(codexui::UiStyle::panel)) &&
          titleColor(final) ==
              QColor(QString::fromLatin1(codexui::UiStyle::purpleText)) &&
          surfaceColor(final) ==
              QColor(QString::fromLatin1(codexui::UiStyle::purpleSurface)),
      "You is blue, interim Codex is neutral, and final Codex is violet");
  qApp->setStyleSheet(originalStyleSheet);
  return result;
}

bool testActiveWorkBordersFollowStatus() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  VisibleCardData command{
      AuthoritativeItemKey{"active-border", "turn", "command"},
      CardKind::CommandExecution,
      "active-border",
      "turn",
      "command",
      CommandExecutionData{"sleep 1", {}, "inProgress", {}, {}, {}}};
  ConversationCard commandCard(command);
  commandCard.resize(560, commandCard.sizeHint().height());
  commandCard.show();
  spin();
  auto *commandStatus =
      commandCard.findChild<QLabel *>(QStringLiteral("commandStatus"));
  const auto emphasizedAtMidpoint = [](ConversationCard &card) {
    const QImage frame = card.grab().toImage();
    return frame.pixelColor(1, frame.height() / 2).red() < 180;
  };
  bool result = expect(
      commandCard.property("activeWork").toBool() && commandStatus &&
          emphasizedAtMidpoint(commandCard) &&
          commandStatus->property("tone").toString() ==
              QStringLiteral("active"),
      "a running command uses the emphasized card border and active header "
      "status");
  std::get<CommandExecutionData>(command.payload).status = "completed";
  result &= expect(commandCard.apply(command) &&
                       !commandCard.property("activeWork").toBool() &&
                       !emphasizedAtMidpoint(commandCard),
                   "a completed command returns to the normal card border");

  VisibleCardData image{AuthoritativeItemKey{"active-border", "turn", "image"},
                        CardKind::ImageGeneration,
                        "active-border",
                        "turn",
                        "image",
                        ImageGenerationData{{}, "inProgress", {}}};
  ConversationCard imageCard(image);
  auto *imageStatus =
      imageCard.findChild<QLabel *>(QStringLiteral("imageGenerationStatus"));
  result &= expect(
      imageCard.property("activeWork").toBool() && imageStatus &&
          imageStatus->font().capitalization() == QFont::MixedCase &&
          imageStatus->text() == QStringLiteral("running") &&
          imageStatus->property("tone").toString() == QStringLiteral("active"),
      "a loading figure uses the emphasized card border and active header "
      "status");
  std::get<ImageGenerationData>(image.payload).status = "completed";
  result &= expect(
      imageCard.apply(image) && !imageCard.property("activeWork").toBool() &&
          imageStatus->text() == QStringLiteral("completed") &&
          imageStatus->property("tone").toString() == QStringLiteral("success"),
      "a loaded figure returns to the normal card border and success header "
      "status");
  qApp->setStyleSheet(originalStyleSheet);
  return result;
}

ConversationCard *card(ConversationView &view, const std::string &key) {
  for (QWidget *widget : view.findChildren<QWidget *>()) {
    auto *candidate = dynamic_cast<ConversationCard *>(widget);
    if (!candidate)
      continue;
    if (candidate->property("conversationAnchorKey").toString() ==
        QString::fromStdString(key))
      return candidate;
  }
  return nullptr;
}

QString cardTitle(const ConversationCard *card) {
  if (!card)
    return {};
  const auto labels = card->findChildren<QLabel *>();
  const auto title = std::ranges::find_if(labels, [](QLabel *label) {
    return label->property("kind").toString() == QStringLiteral("title");
  });
  return title == labels.end() ? QString{} : (*title)->text();
}

QColor cardTitleColor(const ConversationCard *card) {
  if (!card)
    return {};
  const auto labels = card->findChildren<QLabel *>();
  const auto title = std::ranges::find_if(labels, [](QLabel *label) {
    return label->property("kind").toString() == QStringLiteral("title");
  });
  return title == labels.end()
             ? QColor{}
             : (*title)->palette().color(QPalette::WindowText);
}

std::vector<std::string> visualCardKeys(ConversationView &view) {
  std::vector<ConversationCard *> cards;
  for (QWidget *widget : view.findChildren<QWidget *>())
    if (auto *candidate = dynamic_cast<ConversationCard *>(widget))
      cards.push_back(candidate);
  std::ranges::sort(cards, [&view](QWidget *left, QWidget *right) {
    return left->mapTo(view.viewport(), QPoint{}).y() <
           right->mapTo(view.viewport(), QPoint{}).y();
  });

  std::vector<std::string> keys;
  keys.reserve(cards.size());
  for (ConversationCard *candidate : cards)
    keys.push_back(
        candidate->property("conversationAnchorKey").toString().toStdString());
  return keys;
}

bool hasConversationItem(ConversationView &view, const std::string &key) {
  return std::ranges::any_of(
      view.findChildren<QWidget *>(), [&key](QWidget *widget) {
        return widget->property("conversationAnchorKey").toString() ==
               QString::fromStdString(key);
      });
}

struct LiveConversationWidgetCounts final {
  int cards = 0;
  int itemPlaceholders = 0;
  int turnSections = 0;

  [[nodiscard]] int itemRepresentations() const noexcept {
    return cards + itemPlaceholders;
  }
};

LiveConversationWidgetCounts
liveConversationWidgetCounts(ConversationView &view) {
  LiveConversationWidgetCounts result;
  for (QWidget *widget : view.findChildren<QWidget *>()) {
    if (dynamic_cast<ConversationCard *>(widget))
      ++result.cards;
    if (widget->objectName() == QStringLiteral("conversationCardPlaceholder"))
      ++result.itemPlaceholders;
    if (widget->property("turnSectionKey").isValid())
      ++result.turnSections;
  }
  return result;
}

bool graphPassBudgetsWereRespected(const ConversationView &view) {
  const QVariant structure = view.property("graphMaxStructureReadsPerPass");
  const QVariant geometry = view.property("graphMaxGeometryRecordsPerPass");
  const QVariant cards = view.property("graphMaxCardOperationsPerPass");
  return structure.isValid() && geometry.isValid() && cards.isValid() &&
         structure.toULongLong() > 0 && structure.toULongLong() <= 64 &&
         geometry.toULongLong() > 0 && geometry.toULongLong() <= 32 &&
         cards.toULongLong() > 0 && cards.toULongLong() <= 8;
}

bool graphRefreshWasConstantBounded(const ConversationView &view) {
  const QVariant reads = view.property("graphLastRefreshStructureReads");
  return reads.isValid() && reads.toULongLong() <= 64;
}

std::size_t graphLiveRecordBound(const ConversationView &view) {
  constexpr int EstimatedItemExtent = 66;
  const std::size_t viewportItems = static_cast<std::size_t>(
      std::max(1, view.viewport()->height()) / EstimatedItemExtent + 1);
  // The visible viewport plus one bounded viewport of overscan on each side.
  // A co-visible explicit root is part of this same record budget.
  return std::max<std::size_t>(8, viewportItems * 3);
}

bool graphViewportWidgetsAreBounded(ConversationView &view) {
  const LiveConversationWidgetCounts widgets =
      liveConversationWidgetCounts(view);
  const QVariant live = view.property("graphLiveRecordCount");
  const QVariant sections = view.property("graphLiveSectionCount");
  const std::size_t bound = graphLiveRecordBound(view);
  return live.isValid() && sections.isValid() && live.toULongLong() <= bound &&
         sections.toULongLong() <= bound &&
         static_cast<std::size_t>(widgets.itemRepresentations()) <= bound &&
         static_cast<std::size_t>(widgets.turnSections) <= bound;
}

QToolButton *disclosure(ConversationCard *card) {
  return card ? card->findChild<QToolButton *>(
                    QStringLiteral("cardDisclosureButton"))
              : nullptr;
}

QToolButton *copyButton(ConversationCard *card) {
  if (!card)
    return nullptr;
  QWidget *header = card->findChild<QWidget *>(
      QStringLiteral("conversationCardHeader"), Qt::FindDirectChildrenOnly);
  return header
             ? header->findChild<QToolButton *>(
                   QStringLiteral("cardCopyButton"), Qt::FindDirectChildrenOnly)
             : nullptr;
}

QRect paintedDisclosureBounds(QToolButton *button) {
  if (!button)
    return {};
  QImage image(button->size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  button->render(&image, QPoint{}, QRegion{}, QWidget::DrawChildren);
  QRect bounds;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (qAlpha(image.pixel(x, y)) > 0)
        bounds |= QRect(x, y, 1, 1);
    }
  }
  return bounds;
}

bool setFolded(ConversationCard *card, bool collapsed) {
  if (!card)
    return false;
  if (card->isCollapsed() == collapsed)
    return true;
  QPointer<ConversationCard> guard(card);
  QToolButton *button = disclosure(card);
  if (!button)
    return false;
  button->click();
  spin();
  return guard && guard->isCollapsed() == collapsed;
}

std::pair<std::string, int> firstVisible(ConversationView &view) {
  std::vector<ConversationCard *> cards;
  for (QWidget *widget : view.findChildren<QWidget *>())
    if (auto *candidate = dynamic_cast<ConversationCard *>(widget))
      cards.push_back(candidate);
  std::ranges::sort(cards, [&view](QWidget *left, QWidget *right) {
    return left->mapTo(view.viewport(), QPoint{}).y() <
           right->mapTo(view.viewport(), QPoint{}).y();
  });
  for (ConversationCard *candidate : cards) {
    const int top = candidate->mapTo(view.viewport(), QPoint{}).y();
    if (top + candidate->height() >= 0)
      return {
          candidate->property("conversationAnchorKey").toString().toStdString(),
          top};
  }
  return {};
}

class PaintAnchorProbe final : public QObject {
public:
  explicit PaintAnchorProbe(ConversationView &view) : view_(view) {
    view_.viewport()->installEventFilter(this);
  }

  ~PaintAnchorProbe() override { view_.viewport()->removeEventFilter(this); }

  void start(QWidget *tracked = nullptr) {
    anchors.clear();
    trackedGeometries.clear();
    representationCounts.clear();
    tracked_ = tracked;
    active = true;
  }

  void trackOwnership(QWidget *owner, QWidget *child) {
    owner_ = owner;
    child_ = child;
    ownership.clear();
  }

  std::vector<std::pair<std::string, int>> anchors;
  std::vector<QRect> trackedGeometries;
  std::vector<int> representationCounts;
  std::vector<bool> ownership;
  bool active = false;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (active && watched == view_.viewport() &&
        event->type() == QEvent::Paint) {
      anchors.push_back(firstVisible(view_));
      representationCounts.push_back(static_cast<int>(std::ranges::count_if(
          view_.findChildren<QWidget *>(), [](QWidget *widget) {
            return dynamic_cast<ConversationCard *>(widget) ||
                   widget->objectName() ==
                       QStringLiteral("conversationCardPlaceholder");
          })));
      if (tracked_)
        trackedGeometries.emplace_back(
            tracked_->mapTo(view_.viewport(), QPoint{}), tracked_->size());
      if (owner_ && child_)
        ownership.push_back(owner_->property("turnContainer").toBool() &&
                            owner_->isAncestorOf(child_));
    }
    return false;
  }

private:
  ConversationView &view_;
  QPointer<QWidget> tracked_;
  QPointer<QWidget> owner_;
  QPointer<QWidget> child_;
};

void wheel(ConversationView &view, int pixelDelta) {
  const QPointF local(view.viewport()->rect().center());
  QWheelEvent event(local, view.viewport()->mapToGlobal(local.toPoint()),
                    QPoint(), QPoint(0, pixelDelta), Qt::NoButton,
                    Qt::NoModifier, Qt::ScrollUpdate, false);
  QApplication::sendEvent(view.viewport(), &event);
  spin();
}

void mouseWheelNotch(ConversationView &view, int angleDelta) {
  const QPointF local(view.viewport()->rect().center());
  QWheelEvent event(local, view.viewport()->mapToGlobal(local.toPoint()),
                    QPoint(), QPoint(0, angleDelta), Qt::NoButton,
                    Qt::NoModifier, Qt::ScrollUpdate, false);
  QApplication::sendEvent(view.viewport(), &event);
  spin();
}

bool testStructuralOrderAndIdentity() {
  ConversationView view;
  view.resize(620, 420);
  view.show();
  ConversationGraphSpec snapshot = conversation("structural-order", 8);
  applyConversation(view, snapshot);
  spin();

  std::unordered_map<std::string, ConversationCard *> identities;
  for (const TurnGraphSpec &section : snapshot.sections)
    for (const VisibleCardData &value : section.cards)
      identities.emplace(stableKey(value.key),
                         card(view, stableKey(value.key)));

  for (TurnGraphSpec &section : snapshot.sections)
    std::ranges::reverse(section.cards);
  std::ranges::reverse(snapshot.sections);
  std::vector<std::string> expectedKeys;
  for (const TurnGraphSpec &section : snapshot.sections)
    for (const VisibleCardData &value : section.cards)
      expectedKeys.push_back(stableKey(value.key));

  bool result = expect(applyConversation(view, snapshot),
                       "structural order changes reconcile");
  spin();
  result &= expect(visualCardKeys(view) == expectedKeys,
                   "section and card order follows the projection exactly");
  bool retainedIdentity = true;
  for (const auto &[key, identity] : identities) {
    ConversationCard *current = card(view, key);
    retainedIdentity = retainedIdentity && current == identity;
  }
  result &= expect(retainedIdentity,
                   "structural moves preserve same-kind card identity");

  const std::string pagingThread = "turn-root-paging";
  VisibleCardData laterPrompt{
      AuthoritativeItemKey{pagingThread, "turn", "later-user"},
      CardKind::UserMessage,
      pagingThread,
      "turn",
      "later-user",
      UserMessageData{"Later prompt", {}}};
  VisibleCardData activity = agentCard(pagingThread, "turn", 50);
  ConversationGraphSpec paged{
      pagingThread,
      {{"turn:paged", "turn", {laterPrompt, activity}, laterPrompt.key}},
      0,
      false};
  ConversationView pagedView;
  pagedView.resize(620, 420);
  pagedView.show();
  applyConversation(pagedView, paged);
  spin();
  ConversationCard *laterRoot = card(pagedView, stableKey(laterPrompt.key));
  ConversationCard *activityCard = card(pagedView, stableKey(activity.key));
  VisibleCardData earlierPrompt{
      AuthoritativeItemKey{pagingThread, "turn", "earlier-user"},
      CardKind::UserMessage,
      pagingThread,
      "turn",
      "earlier-user",
      UserMessageData{"Earlier prompt", {}}};
  paged.sections.front().cards.insert(paged.sections.front().cards.begin(),
                                      earlierPrompt);
  paged.sections.front().rootCardKey = earlierPrompt.key;
  result &= expect(applyConversation(pagedView, paged),
                   "older history can introduce the real turn prompt");
  spin();
  ConversationCard *earlierRoot = card(pagedView, stableKey(earlierPrompt.key));
  result &=
      expect(earlierRoot && laterRoot && activityCard &&
                 earlierRoot->isAncestorOf(laterRoot) &&
                 earlierRoot->isAncestorOf(activityCard) &&
                 !laterRoot->isAncestorOf(activityCard) &&
                 earlierRoot->property("turnContainer").toBool() &&
                 !laterRoot->property("turnContainer").toBool(),
             "history paging replaces and flattens the visible turn root");

  paged.sections.front().cards.erase(paged.sections.front().cards.begin());
  result &= expect(applyConversation(pagedView, paged),
                   "a transient projection can omit the declared root");
  spin();
  result &=
      expect(card(pagedView, stableKey(laterPrompt.key)) == laterRoot &&
                 card(pagedView, stableKey(activity.key)) == activityCard &&
                 !laterRoot->property("turnContainer").toBool() &&
                 !laterRoot->isAncestorOf(activityCard),
             "a retained steering message never becomes an inferred turn root");

  paged.sections.front().cards.insert(paged.sections.front().cards.begin(),
                                      earlierPrompt);
  result &= expect(applyConversation(pagedView, paged),
                   "the declared turn root can return");
  spin();
  ConversationCard *restoredRoot =
      card(pagedView, stableKey(earlierPrompt.key));
  result &=
      expect(restoredRoot && restoredRoot->property("turnContainer").toBool() &&
                 restoredRoot->isAncestorOf(laterRoot) &&
                 restoredRoot->isAncestorOf(activityCard) &&
                 card(pagedView, stableKey(laterPrompt.key)) == laterRoot &&
                 card(pagedView, stableKey(activity.key)) == activityCard,
             "root restoration reparents retained cards without changing their "
             "identity");
  return result;
}

bool testFollowPauseAndStableAnchor() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationGraphSpec snapshot = conversation("thread-a", 34);
  bool result =
      expect(applyConversation(view, snapshot), "initial projection renders");
  spin();
  QScrollArea nativeReference;
  nativeReference.setWidgetResizable(true);
  auto *nativeContent = new QWidget;
  nativeContent->setMinimumHeight(5000);
  nativeReference.setWidget(nativeContent);
  nativeReference.resize(view.size());
  nativeReference.show();
  spin();
  result &=
      expect(view.verticalScrollBar()->singleStep() ==
                 nativeReference.verticalScrollBar()->singleStep(),
             "conversation line-step matches the previous native QScrollArea");
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom(),
                   "a new thread starts following at its real bottom");

  const int oldValue = view.verticalScrollBar()->value();
  snapshot.sections.back().cards.push_back(agentCard("thread-a", "turn-2", 34));
  result &=
      expect(applyConversation(view, snapshot), "a new card materializes");
  int previous = view.verticalScrollBar()->value();
  bool monotonic = previous >= oldValue;
  QElapsedTimer animation;
  animation.start();
  while (animation.elapsed() < 400 && !view.isAtBottom()) {
    spin(8);
    const int current = view.verticalScrollBar()->value();
    monotonic = monotonic && current >= previous;
    previous = current;
  }
  result &= expect(monotonic && view.isAtBottom(),
                   "follow animation is monotonic and reaches the new bottom");

  const int beforeWheelNotch = view.verticalScrollBar()->value();
  mouseWheelNotch(view, 120);
  result &= expect(
      view.mode() == ConversationView::Mode::Paused && !view.isAtBottom() &&
          beforeWheelNotch - view.verticalScrollBar()->value() ==
              std::min(beforeWheelNotch,
                       view.verticalScrollBar()->singleStep() *
                           std::max(1, QApplication::wheelScrollLines())),
      "native mouse-wheel handling uses the configured line "
      "distance and "
      "pauses following immediately");
  const auto anchor = firstVisible(view);
  result &=
      expect(!anchor.first.empty(), "paused view has a visible card anchor");

  // Reflow a card above the anchor and append another card in one projection.
  const auto anchorPosition =
      std::ranges::find_if(snapshot.sections.front().cards,
                           [&anchor](const VisibleCardData &candidate) {
                             return stableKey(candidate.key) == anchor.first;
                           });
  if (anchorPosition != snapshot.sections.front().cards.begin() &&
      anchorPosition != snapshot.sections.front().cards.end()) {
    auto &message = std::get<AgentMessageData>((anchorPosition - 1)->payload);
    message.text +=
        "\nA reflowing upstream update.\nA second line.\nA third line.";
  }
  snapshot.sections.back().cards.push_back(agentCard("thread-a", "turn-2", 35));
  LayoutRequestProbe layoutRequests(&view);
  result &= expect(applyConversation(view, snapshot),
                   "paused incoming changes still materialize");
  layoutRequests.start();
  spin();
  result &= expect(layoutRequests.count <= 24,
                   "a paused append leaves only bounded ancestor/new-card "
                   "layout settlement across sliced graph rendering");
  const auto after = firstVisible(view);
  result &= expect(after.first == anchor.first &&
                       std::abs(after.second - anchor.second) <= 1,
                   "paused reconciliation preserves key and pixel anchor");
  result &=
      expect(card(view, stableKey(snapshot.sections.back().cards.back().key)),
             "paused mode never withholds a later card");

  const int unchangedValue = view.verticalScrollBar()->value();
  const auto unchangedAnchor = firstVisible(view);
  result &= expect(!applyConversation(view, snapshot),
                   "an identical visible projection is a true no-op");
  spin();
  result &= expect(view.verticalScrollBar()->value() == unchangedValue &&
                       firstVisible(view) == unchangedAnchor,
                   "a no-op changes neither scroll nor visible geometry");

  while (!view.isAtBottom())
    wheel(view, -300);
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
  spin();
  const auto pageStepAnchor = firstVisible(view);
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       !pageStepAnchor.first.empty(),
                   "a scrollbar page action pauses at its resulting anchor");
  auto &upstream = std::get<AgentMessageData>(
      snapshot.sections.front().cards.front().payload);
  upstream.text += "\nTrack-action upstream reflow.\nSecond line.\nThird line.";
  result &= expect(applyConversation(view, snapshot),
                   "page-action coverage applies an upstream reflow");
  spin();
  const auto afterPageStepReflow = firstVisible(view);
  result &= expect(
      afterPageStepReflow.first == pageStepAnchor.first &&
          std::abs(afterPageStepReflow.second - pageStepAnchor.second) <= 1,
      "page-action scroll ownership survives later reflow");
  return result;
}

bool testPausedExpandedCommandStaysPainted() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  const std::string thread = "paused-expanded-command";
  ConversationGraphSpec snapshot = conversation(thread, 24);
  QString output;
  for (int line = 0; line < 48; ++line)
    output += QStringLiteral("completed command output %1\n").arg(line);
  VisibleCardData completedCommand{
      AuthoritativeItemKey{thread, "turn-2", "completed-command"},
      CardKind::CommandExecution,
      thread,
      "turn-2",
      "completed-command",
      CommandExecutionData{"run completed command", utf8(output), "completed",
                           "/workspace", 0, 1250}};
  snapshot.sections.back().cards.insert(
      snapshot.sections.back().cards.begin(),
      cardForAppearanceAudit(thread, CardKind::UserMessage, 99));
  snapshot.sections.back().cards.push_back(completedCommand);

  ConversationView view;
  view.resize(620, 420);
  view.show();
  bool result = expect(applyConversation(view, snapshot),
                       "expanded-command audit renders its conversation");
  spin();
  QPointer<ConversationCard> commandCard =
      card(view, stableKey(completedCommand.key));
  result &= expect(setFolded(commandCard, false),
                   "completed command is expanded before incoming cards");
  QPointer<CommandOutputView> outputView =
      commandCard ? dynamic_cast<CommandOutputView *>(
                        commandCard->findChild<QTextEdit *>(
                            QStringLiteral("commandOutputView")))
                  : nullptr;
  if (outputView && outputView->verticalScrollBar()->maximum() > 0) {
    outputView->verticalScrollBar()->setValue(
        outputView->verticalScrollBar()->maximum() / 2);
    spin();
  }
  result &= expect(commandCard && outputView &&
                       view.mode() == ConversationView::Mode::Paused,
                   "expanded completed command owns a paused viewport");

  PaintAnchorProbe paintProbe(view);
  const std::vector<CardKind> incomingKinds{
      CardKind::UserMessage,      CardKind::AgentMessage,
      CardKind::CommandExecution, CardKind::AgentActivity,
      CardKind::Reasoning,        CardKind::FileChanges,
      CardKind::ImageGeneration,  CardKind::Plan,
      CardKind::GenericActivity,  CardKind::LocalPrompt};
  const auto stableAgainst = [](const std::pair<std::string, int> &reference,
                                const std::pair<std::string, int> &candidate) {
    return candidate.first == reference.first &&
           std::abs(candidate.second - reference.second) <= 1;
  };
  bool allIncomingCardsMaterialized = true;
  for (std::size_t index = 0; index < incomingKinds.size(); ++index) {
    if (!commandCard) {
      result &= expect(false, "incoming activity retains the visible expanded "
                              "command QWidget");
      break;
    }
    const auto anchorBefore = firstVisible(view);
    const QRect commandBefore(commandCard->mapTo(view.viewport(), QPoint{}),
                              commandCard->size());
    const auto outputStateBefore = commandCard->commandOutputScrollState();
    const qulonglong fullGeometryBefore =
        view.property("conversationGeometryPasses").toULongLong();
    const qulonglong localGeometryBefore =
        view.property("conversationLocalGeometryPasses").toULongLong();
    const qulonglong structuralCommitsBefore =
        view.property("incrementalStructuralCommits").toULongLong();
    snapshot.sections.back().cards.push_back(cardForAppearanceAudit(
        thread, incomingKinds[index], 100 + static_cast<int>(index)));
    paintProbe.start(commandCard);
    const bool changed = applyConversation(view, snapshot);
    const auto immediateAnchor = firstVisible(view);
    const QRect immediateCommand(commandCard->mapTo(view.viewport(), QPoint{}),
                                 commandCard->size());
    const std::string incomingKey =
        stableKey(snapshot.sections.back().cards.back().key);
    QPointer<ConversationCard> incomingCard = card(view, incomingKey);
    const int immediateIncomingHeight =
        incomingCard ? incomingCard->height() : -1;
    if (!incomingCard)
      allIncomingCardsMaterialized = false;
    spin(80);
    paintProbe.active = false;
    if (!commandCard) {
      result &= expect(false, "incoming activity retains the visible expanded "
                              "command QWidget through settlement");
      break;
    }
    const auto settledAnchor = firstVisible(view);
    const QRect settledCommand(commandCard->mapTo(view.viewport(), QPoint{}),
                               commandCard->size());
    const int settledIncomingHeight =
        incomingCard ? incomingCard->height() : -1;
    const bool paintedAnchorStable =
        std::ranges::all_of(paintProbe.anchors, [&](const auto &anchor) {
          return stableAgainst(anchorBefore, anchor);
        });
    const bool paintedStable = std::ranges::all_of(
        paintProbe.trackedGeometries, [&commandBefore](const QRect &geometry) {
          return geometry == commandBefore;
        });
    const bool incomingWidgetStable =
        immediateIncomingHeight < 0
            ? !incomingCard
            : incomingCard && immediateIncomingHeight == settledIncomingHeight;
    const bool auditPass =
        changed && card(view, stableKey(completedCommand.key)) == commandCard &&
        view.mode() == ConversationView::Mode::Paused &&
        stableAgainst(anchorBefore, immediateAnchor) &&
        stableAgainst(anchorBefore, settledAnchor) &&
        immediateCommand == commandBefore && settledCommand == commandBefore &&
        paintedAnchorStable && paintedStable && incomingWidgetStable &&
        commandCard->commandOutputScrollState() == outputStateBefore &&
        view.property("conversationGeometryPasses").toULongLong() ==
            fullGeometryBefore &&
        view.property("conversationLocalGeometryPasses").toULongLong() ==
            localGeometryBefore + 1 &&
        view.property("incrementalStructuralCommits").toULongLong() ==
            structuralCommitsBefore + 1;
    result &= expect(
        auditPass,
        "incoming card preserves a visible expanded command in every paint "
        "and settles only its affected Turn");
  }
  result &= expect(allIncomingCardsMaterialized,
                   "selected-thread incoming cards materialize immediately");

  qApp->setStyleSheet(originalStyleSheet);
  spin();
  return result;
}

bool testCommandCompletionWithoutGeometryWork() {
  const std::string thread = "command-completion-paint-only";
  VisibleCardData prompt{
      AuthoritativeItemKey{thread, "turn", "prompt"}, CardKind::UserMessage,
      thread, "turn", "prompt", UserMessageData{"Run the command", {}}};
  QString output;
  for (int line = 0; line < 80; ++line)
    output += QStringLiteral("streamed output line %1\n").arg(line);
  VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{"run long command", utf8(output), "inProgress",
                           "/workspace", {}, {}},
      true};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.activeTurnId = "turn";
  snapshot.sections.push_back(
      {"turn-section", "turn", {prompt, command}, prompt.key});

  ConversationView view;
  view.resize(760, 420);
  view.show();
  bool result = expect(view.reconcile(snapshot),
                       "running command completion audit renders");
  spin();
  ConversationCard *commandCard = card(view, stableKey(command.key));
  if (!commandCard)
    return expect(false, "running command completion audit owns its card");
  const int heightBefore = commandCard->height();
  const int rangeBefore = view.verticalScrollBar()->maximum();
  const qulonglong fullGeometryBefore =
      view.property("conversationGeometryPasses").toULongLong();
  const qulonglong localGeometryBefore =
      view.property("conversationLocalGeometryPasses").toULongLong();

  auto &completed = std::get<CommandExecutionData>(command.payload);
  completed.status = "completed";
  completed.exitCode = 0;
  completed.durationMilliseconds = 12'000;
  command.activeWork = false;
  const std::optional<PresentationImpact> impact =
      view.applyCardPresentation(command);
  spin();

  auto *status = commandCard->findChild<QLabel *>(
      QStringLiteral("commandStatus"));
  const bool completionStayedLocal =
      impact == PresentationImpact::PaintOnly && status &&
          status->text() == QStringLiteral("completed") &&
          !commandCard->property("activeWork").toBool() &&
          commandCard->height() == heightBefore &&
          view.verticalScrollBar()->maximum() == rangeBefore &&
          view.property("conversationGeometryPasses").toULongLong() ==
              fullGeometryBefore &&
          view.property("conversationLocalGeometryPasses").toULongLong() ==
              localGeometryBefore;
  if (!completionStayedLocal)
    std::cerr << "completion impact="
              << (impact ? static_cast<int>(*impact) : -1)
              << " height=" << heightBefore << "->" << commandCard->height()
              << " range=" << rangeBefore << "->"
              << view.verticalScrollBar()->maximum() << " full="
              << fullGeometryBefore << "->"
              << view.property("conversationGeometryPasses").toULongLong()
              << " local=" << localGeometryBefore << "->"
              << view.property("conversationLocalGeometryPasses")
                     .toULongLong()
              << '\n';
  result &= expect(
      completionStayedLocal,
      "running-to-completed patches lifecycle paint without conversation "
      "geometry or scroll-range work");
  return result;
}

bool testStreamingAgentBecomesVisibleWithoutReselection() {
  const std::string thread = "streaming-final-visibility";
  VisibleCardData prompt{
      AuthoritativeItemKey{thread, "turn", "prompt"}, CardKind::UserMessage,
      thread, "turn", "prompt", UserMessageData{"Prompt", {}}};
  VisibleCardData response{
      AuthoritativeItemKey{thread, "turn", "streaming-response"},
      CardKind::AgentMessage,
      thread,
      "turn",
      "streaming-response",
      AgentMessageData{"The completed response must appear immediately.",
                       false}};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.sections.push_back(
      {"turn-section", "turn", {prompt, response}, prompt.key});

  ConversationView view;
  view.setPresentationOptions({true, false, false, false});
  view.resize(620, 420);
  view.show();
  bool result = expect(view.reconcile(snapshot),
                       "a filtered streaming response is retained");
  spin();
  QPointer<ConversationCard> responseCard =
      card(view, stableKey(response.key));
  ConversationCard *rootCard =
      card(view, stableKey(*snapshot.sections.front().rootCardKey));
  result &= expect(responseCard && responseCard->isHidden() && rootCard &&
                       rootCard->isAncestorOf(responseCard),
                   "the streaming response performs no visible work while "
                   "updates are filtered");

  std::get<AgentMessageData>(snapshot.sections.front().cards.back().payload)
      .finalAnswer = true;
  result &= expect(view.reconcile(snapshot),
                   "completion makes the retained response visible");
  spin();
  responseCard = card(view, stableKey(response.key));
  rootCard = card(view, stableKey(*snapshot.sections.front().rootCardKey));
  result &= expect(responseCard && !responseCard->isHidden() && rootCard &&
                       rootCard->isAncestorOf(responseCard) &&
                       responseCard->height() > 0 &&
                       rootCard->contentsRect().contains(
                           responseCard->mapTo(rootCard, QPoint{})) &&
                       responseCard
                               ->mapTo(rootCard,
                                       QPoint(0, responseCard->height()))
                               .y() <= rootCard->contentsRect().bottom() + 1,
                   "the final response and its settled owner appear without "
                   "thread reselection");

  ConversationView optimisticView;
  optimisticView.setPresentationOptions({true, false, false, false});
  optimisticView.resize(620, 420);
  optimisticView.show();
  ConversationSnapshot liveSnapshot;
  liveSnapshot.threadId = "optimistic-live-final";
  static_cast<void>(optimisticView.reconcile(liveSnapshot));
  VisibleCardData localPrompt{
      LocalPromptKey{1}, CardKind::LocalPrompt, liveSnapshot.threadId,
      "live-turn", {},
      LocalPromptData{1, "Live prompt", PromptState::InFlight, 0, {}, {}}};
  liveSnapshot.sections.push_back(
      {"live-turn-section", "live-turn", {localPrompt}, localPrompt.key});
  result &= expect(optimisticView.reconcile(liveSnapshot),
                   "an optimistic Turn/You owner inserts immediately");
  liveSnapshot.sections.front().cards.front().kind = CardKind::UserMessage;
  liveSnapshot.sections.front().cards.front().payload =
      UserMessageData{"Live prompt", {}};
  result &= expect(optimisticView.reconcile(liveSnapshot),
                   "the optimistic Turn/You owner acknowledges in place");
  VisibleCardData liveResponse{
      AuthoritativeItemKey{liveSnapshot.threadId, "live-turn", "live-answer"},
      CardKind::AgentMessage,
      liveSnapshot.threadId,
      "live-turn",
      "live-answer",
      AgentMessageData{"Live final answer", true}};
  liveSnapshot.sections.front().cards.push_back(liveResponse);
  result &= expect(optimisticView.reconcile(liveSnapshot),
                   "the final response inserts into the acknowledged Turn");
  spin();
  ConversationCard *liveRoot =
      card(optimisticView, stableKey(localPrompt.key));
  ConversationCard *liveAnswer =
      card(optimisticView, stableKey(liveResponse.key));
  result &= expect(liveRoot && liveAnswer && !liveAnswer->isHidden() &&
                       liveRoot->isAncestorOf(liveAnswer) &&
                       liveAnswer
                               ->mapTo(liveRoot,
                                       QPoint(0, liveAnswer->height()))
                               .y() <= liveRoot->contentsRect().bottom() + 1,
                   "the optimistic live sequence exposes the final answer in "
                   "its settled Turn without reselection");
  return result;
}

bool testThreadLocalScrollAndComposerExtent() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationGraphSpec first = conversation("thread-a", 30);
  ConversationGraphSpec second = conversation("thread-b", 26);
  applyConversation(view, first);
  spin();
  wheel(view, 220);
  const auto saved = firstVisible(view);
  bool result = expect(view.mode() == ConversationView::Mode::Paused,
                       "first thread is paused before switching");

  applyConversation(view, second);
  spin();
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom(),
                   "a new thread does not inherit another thread's pause");
  applyConversation(view, first);
  spin();
  const auto restored = firstVisible(view);
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       restored.first == saved.first &&
                       std::abs(restored.second - saved.second) <= 1,
                   "switching back restores that thread's own visual anchor");

  const int beforeExtent = view.verticalScrollBar()->maximum();
  const int beforeValue = view.verticalScrollBar()->value();
  view.setTrailingSpaceHeight(137);
  spin();
  result &=
      expect(view.trailingSpaceHeight() == 137 &&
                 view.verticalScrollBar()->maximum() == beforeExtent + 137 &&
                 view.verticalScrollBar()->value() == beforeValue &&
                 view.mode() == ConversationView::Mode::Paused,
             "composer growth adds exact scroll extent without moving content");
  while (!view.isAtBottom())
    wheel(view, -240);
  result &= expect(view.mode() == ConversationView::Mode::Following,
                   "reaching the extended bottom restores following");
  view.setTrailingSpaceHeight(0);
  spin();
  result &=
      expect(view.isAtBottom() && view.trailingSpaceHeight() == 0,
             "composer contraction removes extent and accepts bottom clamp");
  return result;
}

bool testPromptAdmissionFollowOwnership() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationGraphSpec snapshot = conversation("prompt-follow", 30);
  applyConversation(view, snapshot);
  spin();

  view.setTrailingSpaceHeight(120);
  bool result = expect(view.mode() == ConversationView::Mode::Paused,
                       "composer growth preserves the painted viewport");
  view.prepareForLocalPromptAdmission();
  VisibleCardData pending{LocalPromptKey{1001},
                          CardKind::LocalPrompt,
                          "prompt-follow",
                          {},
                          {},
                          LocalPromptData{1001,
                                          "a newly admitted pending prompt",
                                          PromptState::InFlight,
                                          0,
                                          {}}};
  snapshot.sections.back().cards.push_back(pending);
  applyConversation(view, snapshot);
  view.setTrailingSpaceHeight(0);
  ConversationCard *pendingCard = nullptr;
  const bool admittedPromptReady = spinUntil([&] {
    pendingCard = card(view, stableKey(pending.key));
    return view.isAtBottom() && pendingCard &&
           pendingCard->mapTo(view.viewport(), QPoint{}).y() +
                   pendingCard->height() <=
               view.viewport()->height();
  }, 512);
  result &= expect(
      admittedPromptReady &&
          view.mode() == ConversationView::Mode::Following,
      "composer-owned pause resumes and reveals the complete admitted prompt");

  wheel(view, 180);
  const auto userAnchor = firstVisible(view);
  view.setTrailingSpaceHeight(120);
  view.prepareForLocalPromptAdmission();
  VisibleCardData later = pending;
  later.key = LocalPromptKey{1002};
  std::get<LocalPromptData>(later.payload).submissionId = 1002;
  std::get<LocalPromptData>(later.payload).prompt =
      "must not displace a user-owned reading position";
  snapshot.sections.back().cards.push_back(later);
  applyConversation(view, snapshot);
  view.setTrailingSpaceHeight(0);
  spin(40);
  const auto retainedAnchor = firstVisible(view);
  result &=
      expect(view.mode() == ConversationView::Mode::Paused &&
                 retainedAnchor.first == userAnchor.first &&
                 std::abs(retainedAnchor.second - userAnchor.second) <= 1,
             "local admission never overrides an explicit user scroll pause");
  return result;
}

bool testCardCopyControls() {
  const std::string thread = "copy-controls";
  struct CopyCase {
    VisibleCardData card;
    QString expected;
    bool markdown = false;
  };
  const std::vector<CopyCase> cases{
      {{AuthoritativeItemKey{thread, "turn", "user"}, CardKind::UserMessage,
        thread, "turn", "user",
        UserMessageData{"# Prompt\n\n**bold**",
                        {"/tmp/first.png", "/tmp/second.png"}}},
       QStringLiteral("# Prompt\n\n**bold**"),
       true},
      {{AuthoritativeItemKey{thread, "turn", "image-only-user"},
        CardKind::UserMessage, thread, "turn", "image-only-user",
        UserMessageData{{}, {"/tmp/only-image.png"}}},
       QStringLiteral("/tmp/only-image.png"),
       false},
      {{AuthoritativeItemKey{thread, "turn", "agent"}, CardKind::AgentMessage,
        thread, "turn", "agent", AgentMessageData{"## Answer\n\n- item", true}},
       QStringLiteral("## Answer\n\n- item"),
       true},
      {{AuthoritativeItemKey{thread, "turn", "command"},
        CardKind::CommandExecution, thread, "turn", "command",
        CommandExecutionData{"printf copy\n\n", "one\n\n", "completed", {}, 0}},
       QStringLiteral("printf copy\n\none"),
       false},
      {{AuthoritativeItemKey{thread, "turn", "activity"},
        CardKind::AgentActivity, thread, "turn", "activity",
        AgentActivityData{"tool", "completed", {}, "Inspect", "**result**"}},
       QStringLiteral("Inspect\n\n**result**"),
       true},
      {{AuthoritativeItemKey{thread, "turn", "reasoning"}, CardKind::Reasoning,
        thread, "turn", "reasoning", ReasoningData{"Reasoning *summary*"}},
       QStringLiteral("Reasoning *summary*"),
       true},
      {{AuthoritativeItemKey{thread, "turn", "files"}, CardKind::FileChanges,
        thread, "turn", "files",
        FileChangesData{"completed", {{"src/card.cpp", "update", 2, 1}}}},
       QStringLiteral("src/card.cpp  ·  Update  +2 −1"),
       false},
      {{TurnPlanKey{thread, "turn"},
        CardKind::Plan,
        thread,
        "turn",
        {},
        PlanData{"Plan explanation",
                 {{"Inspect", "completed"}, {"Implement", "inProgress"}},
                 {}}},
       QStringLiteral("Plan explanation\n\n✓ Inspect  \n◉ Implement  "),
       true},
      {{AuthoritativeItemKey{thread, "turn", "image"},
        CardKind::ImageGeneration, thread, "turn", "image",
        ImageGenerationData{"/tmp/generated.png", "completed",
                            "A revised prompt"}},
       QStringLiteral("A revised prompt\n\n/tmp/generated.png"),
       false},
      {{AuthoritativeItemKey{thread, "turn", "generic"},
        CardKind::GenericActivity, thread, "turn", "generic",
        GenericActivityData{"custom", {{"detail", "value"}}}},
       QStringLiteral("{\n  \"detail\": \"value\"\n}"),
       false},
      {{LocalPromptKey{99},
        CardKind::LocalPrompt,
        thread,
        {},
        {},
        LocalPromptData{99,
                        "Pending `prompt`",
                        PromptState::InFlight,
                        0,
                        {},
                        {"/tmp/pending.png"}}},
       QStringLiteral("Pending `prompt`"),
       true},
  };

  bool result = true;
  for (std::size_t index = 0; index < cases.size(); ++index) {
    ConversationCard card(cases[index].card);
    card.show();
    spin();
    QToolButton *button = copyButton(&card);
    if (index == 0)
      card.setCollapsed(true);
    QApplication::clipboard()->clear();
    if (button)
      button->click();
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    result &= expect(
        button && !button->isHidden() && mime &&
            mime->text() == cases[index].expected &&
            mime->hasFormat("text/markdown") == cases[index].markdown &&
            (!cases[index].markdown ||
             mime->data("text/markdown") == cases[index].expected.toUtf8()),
        "each content card copies its canonical source while collapsed or "
        "expanded");
    if (index == 0) {
      auto *morph = button->findChild<QVariantAnimation *>();
      const QImage copyIcon = button->grab().toImage();
      spin(220);
      const QImage checkIcon = button->grab().toImage();
      result &= expect(
          morph && button->property("copyFeedbackActive").toBool() &&
              button->property("copyIconState") == QStringLiteral("check") &&
              copyIcon != checkIcon && QToolTip::isVisible() &&
              QToolTip::text() == QStringLiteral("Copied"),
          "Copy quickly morphs into a visible success check while showing "
          "the canonical transient Copied overlay");
      const QSize cardSize = card.size();
      spin(1700);
      result &= expect(!button->property("copyFeedbackActive").toBool() &&
                           button->property("copyIconState") ==
                               QStringLiteral("copy") &&
                           card.size() == cardSize,
                       "the held check morphs back to Copy without changing "
                       "card geometry");
    }
    if (index == 0)
      result &= expect(
          button->parentWidget()->layout()->indexOf(button) <
              button->parentWidget()->layout()->indexOf(disclosure(&card)),
          "Copy precedes the disclosure control in the card header");
    if (index == 0) {
      QToolButton *fold = disclosure(&card);
      const QRect copyInk = paintedDisclosureBounds(button).translated(
          button->mapTo(button->parentWidget(), QPoint{}));
      const QRect foldInk = paintedDisclosureBounds(fold).translated(
          fold->mapTo(fold->parentWidget(), QPoint{}));
      result &= expect(
          button->text().isEmpty() && button->height() == fold->height() &&
              std::abs(copyInk.center().y() - foldInk.center().y()) <= 1 &&
              foldInk.left() - copyInk.right() - 1 <= 14 &&
              button->parentWidget()->layout()->spacing() == 4,
          "copy and disclosure are backgroundless, vertically aligned, and "
          "use canonical compact spacing");
    }
  }

  VisibleCardData mutableMessage = cases.front().card;
  ConversationCard mutableCard(mutableMessage);
  std::get<UserMessageData>(mutableMessage.payload).text =
      "Updated **Markdown**";
  result &= expect(mutableCard.apply(mutableMessage),
                   "copy fixture accepts an in-place content update");
  QApplication::clipboard()->clear();
  copyButton(&mutableCard)->click();
  result &= expect(
      QApplication::clipboard()->text() ==
          QStringLiteral("Updated **Markdown**"),
      "copy reads the latest retained card data after an in-place update");

  ConversationCard emptyReasoning(VisibleCardData{
      AuthoritativeItemKey{thread, "turn", "empty"}, CardKind::Reasoning,
      thread, "turn", "empty", ReasoningData{}});
  emptyReasoning.show();
  spin();
  result &= expect(copyButton(&emptyReasoning) &&
                       copyButton(&emptyReasoning)->isHidden(),
                   "contentless cards omit the Copy control");
  VisibleCardData populatedReasoning = emptyReasoning.data();
  std::get<ReasoningData>(populatedReasoning.payload).summary =
      "Late **summary**";
  result &= expect(emptyReasoning.apply(populatedReasoning) &&
                       !copyButton(&emptyReasoning)->isHidden(),
                   "Copy appears when retained card content arrives later");
  QApplication::clipboard()->clear();
  copyButton(&emptyReasoning)->click();
  result &= expect(
      QApplication::clipboard()->text() == QStringLiteral("Late **summary**") &&
          QApplication::clipboard()->mimeData()->hasFormat("text/markdown"),
      "late Markdown content copies from the updated source");
  return result;
}

bool testMutableCardsAndCommandOutput() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  const std::string thread = "card-thread";
  TurnGraphSpec section{"turn:cards", "turn", {}};
  section.cards = {
      {AuthoritativeItemKey{thread, "turn", "user"}, CardKind::UserMessage,
       thread, "turn", "user",
       UserMessageData{"hello **Markdown**\n\n| Value | Rating "
                       "|\n|---|---|\n| State | 10 |\n\n"
                       "[Docs](https://example.com)"}},
      {AuthoritativeItemKey{thread, "turn", "agent"}, CardKind::AgentMessage,
       thread, "turn", "agent", AgentMessageData{"answer", false}},
      {AuthoritativeItemKey{thread, "turn", "command"},
       CardKind::CommandExecution, thread, "turn", "command",
       CommandExecutionData{
           "printf test\n\n \t", " \n\t", "inProgress", {}, std::nullopt}},
      {AuthoritativeItemKey{thread, "turn", "activity"},
       CardKind::AgentActivity, thread, "turn", "activity",
       AgentActivityData{"tool", "inProgress", {}, "prompt", {}, {}}},
      {AuthoritativeItemKey{thread, "turn", "reasoning"}, CardKind::Reasoning,
       thread, "turn", "reasoning", ReasoningData{"summary"}},
      {AuthoritativeItemKey{thread, "turn", "files"}, CardKind::FileChanges,
       thread, "turn", "files",
       FileChangesData{"inProgress", {{"src/card.cpp", "update", 2, 1}}}},
      {AuthoritativeItemKey{thread, "turn", "plan"}, CardKind::Plan, thread,
       "turn", "plan",
       PlanData{"Keep the card compact",
                {{"Inspect data", "completed"}, {"Render cards", "inProgress"}},
                {}}},
      {AuthoritativeItemKey{thread, "turn", "generic"},
       CardKind::GenericActivity, thread, "turn", "generic",
       GenericActivityData{"custom activity", {{"detail", "initial"}}}},
      {LocalPromptKey{77},
       CardKind::LocalPrompt,
       thread,
       {},
       {},
       LocalPromptData{77,
                       "pending\n\nAttached files:\n"
                       "- [report.pdf](file:///tmp/report.pdf)",
                       PromptState::InFlight,
                       0,
                       {}}},
  };
  section.rootCardKey = section.cards.front().key;
  ConversationGraphSpec snapshot{thread, {section}, 0, false};
  ConversationView view;
  view.resize(650, 520);
  view.show();
  applyConversation(view, snapshot);
  const bool allCoVisibleCardsReady = spinUntil([&] {
    return std::ranges::all_of(
        snapshot.sections.front().cards, [&view](const VisibleCardData &value) {
          return card(view, stableKey(value.key)) != nullptr;
        });
  });

  bool result =
      expect(allCoVisibleCardsReady,
             "bounded render continuations materialize every co-visible card");
  if (!allCoVisibleCardsReady) {
    qApp->setStyleSheet(originalStyleSheet);
    spin();
    return false;
  }

  std::unordered_map<std::string, ConversationCard *> identities;
  for (const auto &value : snapshot.sections.front().cards)
    identities[stableKey(value.key)] = card(view, stableKey(value.key));
  auto containsLabelText = [](QWidget *parent, const QString &needle) {
    return std::ranges::any_of(
        parent->findChildren<QLabel *>(), [&needle](QLabel *label) {
          return label->text().contains(needle) ||
                 label->property("markdownSource").toString().contains(needle);
        });
  };
  auto titleText = [](QWidget *parent) {
    const auto labels = parent->findChildren<QLabel *>();
    const auto title = std::ranges::find_if(labels, [](QLabel *label) {
      return label->property("kind").toString() == QStringLiteral("title");
    });
    return title == labels.end() ? QString{} : (*title)->text();
  };
  auto *commandCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "command"}})];
  auto *output = dynamic_cast<CommandOutputView *>(
      commandCard->findChild<QTextEdit *>(QStringLiteral("commandOutputView")));
  auto *commandText = dynamic_cast<ContentSizedTextView *>(
      commandCard->findChild<QTextEdit *>(QStringLiteral("commandTextView")));
  auto *commandStatus =
      commandCard->findChild<QLabel *>(QStringLiteral("commandStatus"));
  auto *commandMeta =
      commandCard->findChild<QLabel *>(QStringLiteral("commandMetadata"));
  result &= expect(
      output && output->isHidden() && commandStatus && commandMeta &&
          commandMeta->isHidden() &&
          commandStatus->property("tone").toString() ==
              QStringLiteral("active") &&
          commandStatus->font().capitalization() == QFont::MixedCase &&
          commandStatus->text() == QStringLiteral("running") &&
          commandStatus->parentWidget()->layout()->indexOf(commandStatus) <
              commandStatus->parentWidget()->layout()->indexOf(
                  copyButton(commandCard)),
      "empty-line command output has no black surface and exposes its "
      "lowercase status before Copy");
  auto *userCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "user"}})];
  auto *agentCardWidget = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "agent"}})];
  auto *agentPhase =
      agentCardWidget->findChild<QLabel *>(QStringLiteral("agentMessagePhase"));
  auto *activityCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "activity"}})];
  auto *activityStatus =
      activityCard->findChild<QLabel *>(QStringLiteral("agentActivityStatus"));
  const auto userLabels = userCard->findChildren<QLabel *>();
  result &=
      expect(std::ranges::any_of(
                 userLabels,
                 [](QLabel *label) {
                   return label->property("markdownSource").toString() ==
                              QStringLiteral(
                                  "hello **Markdown**\n\n| Value | Rating |\n"
                                  "|---|---|\n| State | 10 |\n\n"
                                  "[Docs](https://example.com)") &&
                          label->textFormat() == Qt::RichText &&
                          label->text().contains(QStringLiteral("<table")) &&
                          label->textInteractionFlags().testFlag(
                              Qt::LinksAccessibleByKeyboard);
                 }),
             "authoritative user messages render GitHub Markdown tables");
  result &= expect(
      titleText(agentCardWidget) == QStringLiteral("Codex") && agentPhase &&
          agentPhase->text() == QStringLiteral("update") &&
          agentPhase->property("tone").toString() == QStringLiteral("active") &&
          agentPhase->font().weight() == QFont::Normal &&
          agentPhase->parentWidget()->layout()->indexOf(agentPhase) <
              agentPhase->parentWidget()->layout()->indexOf(
                  copyButton(agentCardWidget)),
      "interim agent messages show a right-aligned normal-weight update "
      "phase before Copy");
  result &= expect(
      activityStatus &&
          activityStatus->font().capitalization() == QFont::MixedCase &&
          activityStatus->text() == QStringLiteral("running") &&
          activityStatus->property("tone").toString() ==
              QStringLiteral("active"),
      "agent activity exposes its canonical lowercase status in the header");
  auto *filesCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "files"}})];
  auto *filesStatus =
      filesCard->findChild<QLabel *>(QStringLiteral("fileChangesStatus"));
  auto *planCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "plan"}})];
  result &= expect(
      containsLabelText(filesCard,
                        QStringLiteral("src/card.cpp  ·  Update  +2 −1")) &&
          containsLabelText(filesCard, QStringLiteral("+2 −1")) &&
          filesStatus &&
          filesStatus->font().capitalization() == QFont::MixedCase &&
          filesStatus->text() == QStringLiteral("running") &&
          filesStatus->property("tone").toString() == QStringLiteral("active"),
      "file-change cards keep counts below and expose status in the "
      "header");
  result &= expect(
      containsLabelText(planCard, QStringLiteral("Keep the card compact")) &&
          containsLabelText(planCard, QStringLiteral("✓ Inspect data")) &&
          containsLabelText(planCard, QStringLiteral("◉ Render cards")),
      "structured plan cards show explanation and step status");
  result &= expect(
      commandText &&
          commandText->toPlainText() == QStringLiteral("printf test") &&
          commandText->height() < commandText->maximumHeight() &&
          commandText->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
      "short command text trims empty lines and uses its content height");
  auto *pendingCard = identities[stableKey(CardKey{LocalPromptKey{77}})];
  result &= expect(
      std::ranges::any_of(
          pendingCard->findChildren<QLabel *>(),
          [](QLabel *label) {
            return label->property("markdownSource")
                       .toString()
                       .contains(QStringLiteral(
                           "[report.pdf](file:///tmp/report.pdf)")) &&
                   label->textFormat() == Qt::RichText;
          }),
      "pending prompts render file links before authoritative replacement");

  commandCard->setCollapsed(false);
  spin();
  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      commandCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  auto &cards = snapshot.sections.front().cards;
  std::get<UserMessageData>(cards[0].payload).text += " updated";
  auto &agent = std::get<AgentMessageData>(cards[1].payload);
  agent.text += " updated";
  agent.finalAnswer = true;
  auto &command = std::get<CommandExecutionData>(cards[2].payload);
  QString longCommand;
  for (int line = 0; line < 30; ++line)
    longCommand += QStringLiteral("command argument line %1\n").arg(line);
  command.command = utf8(longCommand);
  command.output =
      utf8(QString(120, QLatin1Char('x')) + QStringLiteral("\nvisible\n\n \t"));
  command.status = "completed";
  command.exitCode = 0;
  command.cwd = "/workspace";
  command.durationMilliseconds = 1500;
  std::get<AgentActivityData>(cards[3].payload).resultText = "result";
  std::get<ReasoningData>(cards[4].payload).summary += " more";
  std::get<FileChangesData>(cards[5].payload)
      .changes.push_back({"tests/card.cpp", "add", 3, 0});
  std::get<PlanData>(cards[6].payload).steps[1].status = "completed";
  auto &generic = std::get<GenericActivityData>(cards[7].payload);
  generic.type = "updated custom activity";
  generic.raw["detail"] = "updated";
  std::get<LocalPromptData>(cards[8].payload).state = PromptState::Failed;
  std::get<LocalPromptData>(cards[8].payload).error = "error";
  result &= expect(applyConversation(view, snapshot),
                   "all card types accept visible updates");
  result &= spinUntil([&] {
    const auto *presented =
        std::get_if<CommandExecutionData>(&commandCard->data().payload);
    return presented && presented->output.ends_with("visible\n\n \t");
  });
  const int immediateCommandHeight = commandCard->height();
  const int immediatePreferredOutputHeight = output->sizeHint().height();
  spin();
  result &=
      expect(commandCard->height() == immediateCommandHeight &&
                 output->sizeHint().height() == immediatePreferredOutputHeight,
             "command output has no delayed card geometry settlement while "
             "other graph renders remain sliced");
  const bool longCommandStartsAtTop =
      commandText->verticalScrollBar()->maximum() > 0 &&
      commandText->verticalScrollBar()->value() ==
          commandText->verticalScrollBar()->minimum();
  if (!longCommandStartsAtTop)
    std::cerr << "long command scroll: value="
              << commandText->verticalScrollBar()->value() << " minimum="
              << commandText->verticalScrollBar()->minimum() << " maximum="
              << commandText->verticalScrollBar()->maximum() << " height="
              << commandText->height() << " hint="
              << commandText->sizeHint().height() << " cursor="
              << commandText->textCursor().position() << " focus="
              << commandText->hasFocus() << '\n';
  result &= expect(longCommandStartsAtTop,
                   "long executed-command text opens at its beginning");
  for (const auto &value : cards)
    result &= expect(card(view, stableKey(value.key)) ==
                         identities[stableKey(value.key)],
                     "same-key same-kind card updates in place");
  // The graph update above deliberately does no QWidget projection for this
  // offscreen card. Bringing the already-materialized card into view applies
  // its latest canonical state without reconstructing it.
  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      agentCardWidget->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  result &= expect(
      titleText(agentCardWidget) == QStringLiteral("Codex") && agentPhase &&
          agentPhase->text() == QStringLiteral("final answer") &&
          agentPhase->property("tone").toString() ==
              QStringLiteral("success") &&
          agentPhase->font().weight() == QFont::Normal,
      "final agent messages show a normal-weight success answer phase");
  result &= expect(
      commandStatus->text() == QStringLiteral("completed") &&
          commandStatus->property("tone").toString() ==
              QStringLiteral("success") &&
          commandMeta->text() ==
              QStringLiteral("exit 0  |  /workspace  |  1.5 s") &&
          !commandMeta->text().contains(QStringLiteral("completed")),
      "command completion moves only lifecycle status while retaining exit, "
      "cwd, and duration below output");
  result &=
      expect(!output->isHidden() && output->minimumHeight() == 0 &&
                 output->maximumHeight() == 220 &&
                 output->toPlainText().endsWith(QStringLiteral("visible")) &&
                 output->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
             "visible output trims empty lines and grows with the 220px cap");

  QString longOutput;
  for (int line = 0; line < 80; ++line)
    longOutput += QStringLiteral("line %1 with terminal output\n").arg(line);
  output->setOutput(longOutput);
  view.resize(650, 520);
  spin(40);
  result &= expect(output->verticalScrollBar()->maximum() > 0 &&
                       output->followsLatest(),
                   "long command output exposes its own scrollbar and follows");

  // A scrollbar move immediately after an output update is user-owned.  It
  // must not be overwritten by a deferred follow-latest settlement.
  output->setOutput(longOutput + QStringLiteral("new output before gesture\n"));
  output->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  spin();
  const int immediateGestureValue = output->verticalScrollBar()->value();
  result &=
      expect(!output->followsLatest() &&
                 output->verticalScrollBar()->value() == immediateGestureValue,
             "an immediate inner-scroll gesture supersedes following");

  output->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  spin();
  const int preserved = output->verticalScrollBar()->value();
  output->setOutput(longOutput + QStringLiteral("one more line\n"));
  spin();
  result &= expect(!output->followsLatest() &&
                       output->verticalScrollBar()->value() == preserved,
                   "paused command output preserves its inner scroll value");
  output->verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  spin();
  result &= expect(output->followsLatest(),
                   "inner output following resumes at its real bottom");

  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      commandCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  command.output = "\x1b]0;terminal title\x07\x1b[0m \n\t";
  result &= expect(applyConversation(view, snapshot),
                   "non-presentable replacement updates the command card");
  result &= spinUntil([&] { return output->isHidden(); });
  const int hiddenOuterRange = view.verticalScrollBar()->maximum();
  const int hiddenCommandHeight = commandCard->height();
  result &= expect(output->isHidden(),
                   "non-presentable replacement removes the black surface");
  spin();
  result &=
      expect(output->isHidden() &&
                 view.verticalScrollBar()->maximum() == hiddenOuterRange &&
                 commandCard->height() == hiddenCommandHeight,
             "hidden command output causes no delayed outer reflow");
  qApp->setStyleSheet(originalStyleSheet);
  spin();
  return result;
}

bool testCardFoldingGeometryAndRetention() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  const std::string thread = "folding-thread";
  const VisibleCardData user{
      AuthoritativeItemKey{thread, "turn", "user"},
      CardKind::UserMessage,
      thread,
      "turn",
      "user",
      UserMessageData{"Keep this message initially expanded.", {}}};
  const VisibleCardData agent{
      AuthoritativeItemKey{thread, "turn", "agent"},
      CardKind::AgentMessage,
      thread,
      "turn",
      "agent",
      AgentMessageData{"Codex also starts expanded.", true}};
  const VisibleCardData reasoning{
      AuthoritativeItemKey{thread, "turn", "reasoning"},
      CardKind::Reasoning,
      thread,
      "turn",
      "reasoning",
      ReasoningData{"A retained public summary with enough "
                    "detail to create real height.\n\n"
                    "The second paragraph proves expansion uses "
                    "the final wrapped size."}};
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{"produce output", "initial output", "completed",
                           "/workspace", 0}};
  const VisibleCardData files{
      AuthoritativeItemKey{thread, "turn", "files"},
      CardKind::FileChanges,
      thread,
      "turn",
      "files",
      FileChangesData{"completed", {{"src/card.cpp", "update", 4, 1}}}};
  const VisibleCardData activity{
      AuthoritativeItemKey{thread, "turn", "activity"},
      CardKind::AgentActivity,
      thread,
      "turn",
      "activity",
      AgentActivityData{"spawn_agent",
                        "completed",
                        {},
                        "Inspect folding",
                        "Inspection complete",
                        {}}};
  const VisibleCardData image{AuthoritativeItemKey{thread, "turn", "image"},
                              CardKind::ImageGeneration,
                              thread,
                              "turn",
                              "image",
                              ImageGenerationData{"/tmp/folding-preview.png",
                                                  "completed",
                                                  "A folding preview"}};
  const VisibleCardData plan{
      AuthoritativeItemKey{thread, "turn", "plan"},
      CardKind::Plan,
      thread,
      "turn",
      "plan",
      PlanData{"Verify folding", {{"Inspect geometry", "completed"}}, {}}};
  const VisibleCardData generic{
      AuthoritativeItemKey{thread, "turn", "generic"},
      CardKind::GenericActivity,
      thread,
      "turn",
      "generic",
      GenericActivityData{"Unknown activity", {{"detail", "bounded"}}}};
  const VisibleCardData emptyReasoning{
      AuthoritativeItemKey{thread, "turn", "empty-reasoning"},
      CardKind::Reasoning,
      thread,
      "turn",
      "empty-reasoning",
      ReasoningData{}};
  ConversationGraphSpec snapshot{
      thread,
      {{"turn:folding",
        "turn",
        {user, agent, reasoning, command, files, activity, image, plan, generic,
         emptyReasoning},
        user.key}},
      0,
      false};
  snapshot.activeTurnId = "turn";

  ConversationView view;
  view.resize(700, 820);
  view.setTrailingSpaceHeight(500);
  view.show();
  ConversationGraphSpec promptOnly = snapshot;
  promptOnly.sections.front().cards = {user};
  bool result = expect(applyConversation(view, promptOnly),
                       "prompt-only folding fixture renders");
  spin();

  ConversationCard *promptOnlyCard = card(view, stableKey(user.key));
  QWidget *promptOnlyNestedCards =
      promptOnlyCard ? promptOnlyCard->findChild<QWidget *>(
                           QStringLiteral("conversationNestedCards"),
                           Qt::FindDirectChildrenOnly)
                     : nullptr;
  result &= expect(promptOnlyCard && promptOnlyNestedCards &&
                       promptOnlyNestedCards->isHidden(),
                   "an initial turn prompt reserves no nested-card gap");
  result &= expect(applyConversation(view, snapshot),
                   "first nested activity extends the folding fixture");
  const bool foldingCardsReady = spinUntil([&] {
    return std::ranges::all_of(
        snapshot.sections.front().cards, [&view](const VisibleCardData &value) {
          return card(view, stableKey(value.key)) != nullptr;
        });
  });
  result &= expect(
      foldingCardsReady,
      "bounded render continuations materialize every co-visible folding "
      "card");
  if (!foldingCardsReady) {
    qApp->setStyleSheet(originalStyleSheet);
    spin();
    return false;
  }

  QPointer<ConversationCard> userCard = card(view, stableKey(user.key));
  QPointer<ConversationCard> agentCardWidget = card(view, stableKey(agent.key));
  QPointer<ConversationCard> reasoningCard =
      card(view, stableKey(reasoning.key));
  QPointer<ConversationCard> commandCard = card(view, stableKey(command.key));
  QPointer<ConversationCard> filesCard = card(view, stableKey(files.key));
  const std::vector<QPointer<ConversationCard>> additionalActionCards{
      card(view, stableKey(activity.key)), card(view, stableKey(image.key)),
      card(view, stableKey(plan.key)), card(view, stableKey(generic.key))};
  QPointer<ConversationCard> emptyReasoningCard =
      card(view, stableKey(emptyReasoning.key));
  result &= expect(
      userCard && agentCardWidget && reasoningCard && commandCard &&
          filesCard && !userCard->isCollapsed() &&
          !agentCardWidget->isCollapsed() && reasoningCard->isCollapsed() &&
          commandCard->isCollapsed() && filesCard->isCollapsed() &&
          disclosure(userCard) && disclosure(agentCardWidget) &&
          disclosure(reasoningCard) && disclosure(commandCard) &&
          disclosure(filesCard) &&
          disclosure(userCard)->property("chevronDirection") == "down" &&
          disclosure(reasoningCard)->property("chevronDirection") == "left",
      "all cards share disclosure controls with role-correct initial state");
  result &= expect(
      userCard && userCard == promptOnlyCard &&
          userCard->property("authoritativeTurnActive").toBool() &&
          agentCardWidget &&
          !agentCardWidget->property("authoritativeTurnActive").toBool() &&
          !userCard->findChild<QTimer *>(QStringLiteral("activeTurnAnimation")),
      "the retained running outer You card receives a static emphasized "
      "border");
  snapshot.activeTurnId.reset();
  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      userCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  result &= expect(applyConversation(view, snapshot) &&
                       spinUntil([&] {
                         return !userCard
                                     ->property("authoritativeTurnActive")
                                     .toBool();
                       }) &&
                       userCard == card(view, stableKey(user.key)) &&
                       !userCard->property("authoritativeTurnActive").toBool(),
                   "turn completion restores the same card's canonical border");
  const QRect collapsedDisclosure =
      paintedDisclosureBounds(disclosure(reasoningCard));
  result &= expect(
      collapsedDisclosure.isValid() && collapsedDisclosure.width() <= 8 &&
          collapsedDisclosure.right() >= disclosure(reasoningCard)->width() - 3,
      "collapsed disclosure paints only a right-inset left chevron");
  result &= expect(
      std::ranges::all_of(additionalActionCards,
                          [](const QPointer<ConversationCard> &value) {
                            return value && value->isCollapsed() &&
                                   disclosure(value);
                          }),
      "agent, image, plan, and fallback activity cards also start collapsed");
  result &= expect(emptyReasoningCard && emptyReasoningCard->isCollapsed() &&
                       disclosure(emptyReasoningCard) &&
                       disclosure(emptyReasoningCard)->isHidden(),
                   "title-only reasoning omits a meaningless disclosure");
  if (!userCard || !agentCardWidget || !reasoningCard || !commandCard ||
      !filesCard || !emptyReasoningCard)
    return false;

  result &=
      expect(userCard->property("turnContainer").toBool() &&
                 userCard->isAncestorOf(agentCardWidget) &&
                 userCard->isAncestorOf(reasoningCard) &&
                 agentCardWidget->property("nestedConversationCard").toBool(),
             "the first You card structurally owns its turn activity");
  QWidget *promptContent = userCard->findChild<QWidget *>(
      QStringLiteral("conversationCardContent"), Qt::FindDirectChildrenOnly);
  const int promptContentBottom =
      promptContent ? promptContent->geometry().y() + promptContent->height()
                    : -1;
  const int firstNestedTop = agentCardWidget->mapTo(userCard, QPoint{}).y();
  QWidget *nestedCards = userCard->findChild<QWidget *>(
      QStringLiteral("conversationNestedCards"), Qt::FindDirectChildrenOnly);
  result &= expect(
      promptContent && nestedCards && nestedCards->layout() &&
          nestedCards->layout()->contentsMargins().top() == 8 &&
          firstNestedTop - promptContentBottom == 14,
      "turn prompt content adds a visible canonical 8 px section boundary "
      "before its first nested card");

  const LocalPromptKey steeringKey{4343};
  VisibleCardData steering{
      steeringKey,
      CardKind::LocalPrompt,
      thread,
      "turn",
      {},
      LocalPromptData{
          4343, "A steering prompt", PromptState::InFlight, true, {}, {}}};
  snapshot.sections.front().cards.push_back(steering);
  result &= expect(applyConversation(view, snapshot),
                   "a steering prompt joins the active turn");
  spin(40);
  ConversationCard *steeringCard = card(view, stableKey(steeringKey));
  auto *steeringPhase = steeringCard
                            ? steeringCard->findChild<QLabel *>(
                                  QStringLiteral("steeringMessagePhase"))
                            : nullptr;
  auto *steeringAnimation = steeringCard
                                ? steeringCard->findChild<QTimer *>(
                                      QString{}, Qt::FindDirectChildrenOnly)
                                : nullptr;
  result &= expect(
      steeringCard && userCard->isAncestorOf(steeringCard) &&
          steeringCard->property("nestedConversationCard").toBool() &&
          cardTitle(steeringCard) == QStringLiteral("You") && steeringPhase &&
          steeringPhase->text() == QStringLiteral("steering · pending") &&
          steeringPhase->font().weight() == QFont::Normal &&
          steeringPhase->parentWidget()->layout()->indexOf(steeringPhase) <
              steeringPhase->parentWidget()->layout()->indexOf(
                  copyButton(steeringCard)) &&
          cardTitleColor(steeringCard) == QColor(QStringLiteral("#146f73")) &&
          steeringAnimation && steeringAnimation->isActive(),
      "a pending steering You card is nested and keeps its animation");

  snapshot.sections.front().cards.back() = {
      steeringKey,     CardKind::UserMessage,
      thread,          "turn",
      "steering-user", UserMessageData{"A steering prompt", {}}};
  result &= expect(applyConversation(view, snapshot),
                   "the steering prompt receives authoritative content");
  spin();
  ConversationCard *authoritativeSteering = card(view, stableKey(steeringKey));
  result &=
      expect(authoritativeSteering == steeringCard &&
                 userCard->isAncestorOf(authoritativeSteering) &&
                 authoritativeSteering->cardKind() == CardKind::UserMessage &&
                 cardTitle(authoritativeSteering) == QStringLiteral("You") &&
                 steeringPhase->text() == QStringLiteral("steering") &&
                 authoritativeSteering->palette().color(QPalette::Window) ==
                     QColor(QStringLiteral("#eefafa")) &&
                 steeringAnimation && !steeringAnimation->isActive(),
             "steering acknowledgement morphs the same nested card");

  auto retainedEmptyReasoning = std::ranges::find_if(
      snapshot.sections.front().cards, [&emptyReasoning](const auto &value) {
        return value.key == emptyReasoning.key;
      });
  std::get<ReasoningData>(retainedEmptyReasoning->payload).summary =
      "Public reasoning summary arrived";
  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      emptyReasoningCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  result &= expect(applyConversation(view, snapshot),
                   "empty reasoning accepts later public content");
  result &= spinUntil([&] { return !disclosure(emptyReasoningCard)->isHidden(); });
  result &=
      expect(!disclosure(emptyReasoningCard)->isHidden() &&
                 disclosure(emptyReasoningCard)->property("chevronDirection") ==
                     "left",
             "reasoning disclosure appears collapsed when detail arrives");

  wheel(view, 10000);

  const int userTop = userCard->mapTo(view.viewport(), QPoint{}).y();
  const int reasoningTop = reasoningCard->mapTo(view.viewport(), QPoint{}).y();
  const int filesTop = filesCard->mapTo(view.viewport(), QPoint{}).y();
  const int foldedReasoningHeight = reasoningCard->height();
  result &= expect(setFolded(reasoningCard, false),
                   "reasoning expands through its disclosure control");
  const int expandedReasoningHeight = reasoningCard->height();
  result &= expect(
      reasoningCard->mapTo(view.viewport(), QPoint{}).y() == reasoningTop &&
          userCard->mapTo(view.viewport(), QPoint{}).y() == userTop &&
          expandedReasoningHeight > foldedReasoningHeight &&
          disclosure(reasoningCard)->property("chevronDirection") == "down" &&
          filesCard->mapTo(view.viewport(), QPoint{}).y() ==
              filesTop + expandedReasoningHeight - foldedReasoningHeight,
      "expansion fixes the affected title and grows only downward");
  const QRect expandedDisclosure =
      paintedDisclosureBounds(disclosure(reasoningCard));
  result &= expect(
      expandedDisclosure.isValid() && expandedDisclosure.width() <= 10 &&
          expandedDisclosure.right() >= disclosure(reasoningCard)->width() - 3,
      "expanded disclosure paints only a right-inset down chevron");

  const int commandHeight = commandCard->height();
  auto &execution = std::get<CommandExecutionData>(
      snapshot.sections.front().cards[3].payload);
  execution.output =
      "streamed line 1\nstreamed line 2\nstreamed line 3\nstreamed line 4";
  const int scrollBeforeCommandUpdate = view.verticalScrollBar()->value();
  view.verticalScrollBar()->setValue(
      scrollBeforeCommandUpdate +
      commandCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  result &= expect(applyConversation(view, snapshot),
                   "folded command accepts a streamed content update");
  auto *output = dynamic_cast<CommandOutputView *>(
      commandCard->findChild<QTextEdit *>(QStringLiteral("commandOutputView")));
  result &= spinUntil([&] {
    return output &&
           output->toPlainText().contains(QStringLiteral("streamed line 4"));
  });
  view.verticalScrollBar()->setValue(scrollBeforeCommandUpdate);
  spin(20);
  result &= expect(
      commandCard->isCollapsed() && commandCard->height() == commandHeight &&
          output &&
          output->toPlainText().contains(QStringLiteral("streamed line 4")),
      "streaming updates folded content without changing height");

  const int userHeight = userCard->height();
  result &= expect(setFolded(userCard, true),
                   "You can be folded from its expanded default");
  const bool foldedTurnPass =
      userCard->mapTo(view.viewport(), QPoint{}).y() == userTop &&
      userCard->height() < userHeight &&
      (!agentCardWidget || !agentCardWidget->isVisibleTo(userCard)) &&
      (!reasoningCard || !reasoningCard->isVisibleTo(userCard)) &&
      (!commandCard || !commandCard->isVisibleTo(userCard));
  result &= expect(
      foldedTurnPass,
      "folding You fixes its title and hides or releases nested widgets");

  applyConversation(view, conversation("folding-other-thread", 4));
  spin();
  applyConversation(view, snapshot);
  spin(40);
  userCard = card(view, stableKey(user.key));
  reasoningCard = card(view, stableKey(reasoning.key));
  commandCard = card(view, stableKey(command.key));
  result &= expect(userCard && userCard->isCollapsed(),
                   "the root fold survives thread switching and updates");
  result &= expect(setFolded(userCard, false),
                   "the restored root can rematerialize its nested cards");
  spin(80);
  reasoningCard = card(view, stableKey(reasoning.key));
  commandCard = card(view, stableKey(command.key));
  result &=
      expect(reasoningCard && commandCard && !reasoningCard->isCollapsed() &&
                 commandCard->isCollapsed(),
             "rematerialized nested cards restore user-owned folds");

  const std::string promptThread = "folding-prompt-replacement";
  const LocalPromptKey promptKey{4242};
  VisibleCardData localPrompt{
      promptKey,
      CardKind::LocalPrompt,
      promptThread,
      {},
      {},
      LocalPromptData{
          4242, "A temporary prompt", PromptState::InFlight, 0, {}, {}}};
  VisibleCardData promptActivity = agentCard(promptThread, "turn", 77);
  ConversationGraphSpec promptSnapshot{
      promptThread,
      {{"local:folding-prompt", {}, {localPrompt, promptActivity}, promptKey}},
      0,
      false};
  applyConversation(view, promptSnapshot);
  spin();
  ConversationCard *promptCard = card(view, stableKey(promptKey));
  QPointer<ConversationCard> promptActivityCard =
      card(view, stableKey(promptActivity.key));
  result &=
      expect(promptCard && !promptCard->isCollapsed() && promptActivityCard &&
                 promptCard->isAncestorOf(promptActivityCard) &&
                 setFolded(promptCard, true),
             "temporary You prompts start expanded and can be folded");
  ConversationCard *const admittedPromptCard = promptCard;
  QWidget *const admittedPromptHeader =
      admittedPromptCard ? admittedPromptCard->findChild<QWidget *>(
                               QStringLiteral("conversationCardHeader"))
                         : nullptr;
  const QSize admittedPromptSize =
      admittedPromptCard ? admittedPromptCard->size() : QSize{};
  const QRect admittedPromptHeaderGeometry =
      admittedPromptHeader ? admittedPromptHeader->geometry() : QRect{};
  const int admittedPromptFrameWidth =
      admittedPromptCard ? admittedPromptCard->frameWidth() : -1;
  promptSnapshot.sections.front().cards.front() = {
      promptKey,    CardKind::UserMessage,
      promptThread, "turn",
      "user",       UserMessageData{"A temporary prompt", {}}};
  promptSnapshot.sections.front().key = "turn:folding-prompt";
  promptSnapshot.sections.front().turnId = "turn";
  applyConversation(view, promptSnapshot);
  spin();
  promptCard = card(view, stableKey(promptKey));
  auto *promptAnimation = admittedPromptCard->findChild<QTimer *>(
      QString{}, Qt::FindDirectChildrenOnly);
  const bool promptMorphPass =
      promptCard && promptCard == admittedPromptCard &&
      promptCard->cardKind() == CardKind::UserMessage &&
      promptCard->isCollapsed() && promptAnimation &&
      !promptAnimation->isActive() &&
      promptCard->property("messageRole") == QStringLiteral("user") &&
      promptCard->property("conversationCardKind").toInt() ==
          static_cast<int>(CardKind::UserMessage) &&
      promptCard->objectName() == QStringLiteral("conversationCard") &&
      promptCard->styleSheet().isEmpty() &&
      promptCard->size() == admittedPromptSize && admittedPromptHeader &&
      admittedPromptHeader->geometry() == admittedPromptHeaderGeometry &&
      promptCard->frameWidth() == admittedPromptFrameWidth;
  result &= expect(
      promptMorphPass,
      "acknowledgement morphs the retained You card without geometry drift");
  result &= expect(setFolded(promptCard, false),
                   "acknowledged prompt can reveal current turn activity");
  spin(40);
  ConversationCard *rematerializedPromptActivity =
      card(view, stableKey(promptActivity.key));
  result &=
      expect(rematerializedPromptActivity &&
                 promptCard->isAncestorOf(rematerializedPromptActivity) &&
                 (!promptActivityCard ||
                  rematerializedPromptActivity == promptActivityCard),
             "prompt activity remains structurally nested across lazy release");

  const std::string edgeThread = "folding-bottom-edge";
  ConversationGraphSpec edge = conversation(edgeThread, 12);
  QString longOutput;
  for (int line = 0; line < 70; ++line)
    longOutput += QStringLiteral("bottom-edge line %1\n").arg(line);
  VisibleCardData edgeCommand{
      AuthoritativeItemKey{edgeThread, "turn-2", "edge-command"},
      CardKind::CommandExecution,
      edgeThread,
      "turn-2",
      "edge-command",
      CommandExecutionData{
          "produce capped output", utf8(longOutput), "completed", {}, 0}};
  edge.sections.back().cards.push_back(edgeCommand);
  ConversationView edgeView;
  edgeView.resize(650, 520);
  edgeView.show();
  applyConversation(edgeView, edge);
  spin();
  ConversationCard *edgeCard = card(edgeView, stableKey(edgeCommand.key));
  const int collapsedTop =
      edgeCard ? edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() : 0;
  result &= expect(setFolded(edgeCard, false),
                   "bottom-edge command expands from its compact default");
  result &= expect(
      edgeCard &&
          edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() < collapsedTop &&
          edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() +
                  edgeCard->height() <=
              edgeView.viewport()->height(),
      "bottom-edge expansion shifts upward to reveal the complete card");
  wheel(edgeView, -10000);
  const int followedTitleTop =
      edgeCard ? edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() : 0;
  const int expandedScrollMaximum = edgeView.verticalScrollBar()->maximum();
  result &= expect(edgeView.isAtBottom() && followedTitleTop >= 0,
                   "expanded lower-limit fixture exposes its title at bottom");
  result &= expect(setFolded(edgeCard, true),
                   "expanded bottom-edge command collapses");
  spin(120);
  result &= expect(
      edgeCard &&
          edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() >
              followedTitleTop &&
          edgeView.verticalScrollBar()->maximum() < expandedScrollMaximum &&
          edgeView.isAtBottom() &&
          edgeView.mode() == ConversationView::Mode::Paused,
      "bottom-edge collapse accepts the natural range without a blank tail");
  constexpr int ComposerOverlayHeight = 80;
  edgeView.setTrailingSpaceHeight(ComposerOverlayHeight);
  result &=
      expect(setFolded(edgeCard, false), "bottom-edge command expands again");
  spin(120);
  result &= expect(
      edgeView.verticalScrollBar()->maximum() ==
              expandedScrollMaximum + ComposerOverlayHeight &&
          edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() +
                  edgeCard->height() <=
              edgeView.viewport()->height() - ComposerOverlayHeight,
      "fold round trip reveals the complete card above a grown composer");
  qApp->setStyleSheet(originalStyleSheet);
  spin();
  return result;
}

bool testPresentationOptionsRetainCardsAndInitialFolding() {
  {
    const std::string nestedThread = "nested-presentation-options";
    const AuthoritativeItemKey nestedUserKey{nestedThread, "turn", "user"};
    const AuthoritativeItemKey nestedReasoningKey{nestedThread, "turn",
                                                  "reasoning"};
    ConversationGraphSpec nestedSnapshot;
    nestedSnapshot.threadId = nestedThread;
    nestedSnapshot.sections.push_back(
        {"turn:nested-presentation-options:turn",
         "turn",
         {{nestedUserKey, CardKind::UserMessage, nestedThread, "turn", "user",
           UserMessageData{"Prompt", {}}}},
         nestedUserKey});
    ConversationView nestedView;
    nestedView.setPresentationOptions({false, true, true, true});
    nestedView.resize(700, 700);
    nestedView.show();
    bool nestedResult = applyConversation(nestedView, nestedSnapshot);
    spin();
    nestedSnapshot.sections.front().cards.push_back(
        {nestedReasoningKey, CardKind::Reasoning, nestedThread, "turn",
         "reasoning", ReasoningData{"Hidden reasoning"}});
    nestedResult &= applyConversation(nestedView, nestedSnapshot);
    spin();
    ConversationCard *nestedReasoning =
        card(nestedView, stableKey(nestedReasoningKey));
    if (!expect(nestedResult && nestedReasoning &&
                    !nestedReasoning->isVisible(),
                "filtered nested reasoning is retained without painting"))
      return false;
  }

  {
    const std::string followingThread = "following-nested-insertion";
    ConversationGraphSpec followingSnapshot;
    followingSnapshot.threadId = followingThread;
    TurnGraphSpec followingSection{
        "turn:following-nested-insertion:turn", "turn", {}};
    followingSection.cards.push_back(
        {AuthoritativeItemKey{followingThread, "turn", "user"},
         CardKind::UserMessage, followingThread, "turn", "user",
         UserMessageData{"Prompt", {}}});
    followingSection.rootCardKey = followingSection.cards.front().key;
    for (int index = 0; index < 12; ++index)
      followingSection.cards.push_back(
          agentCard(followingThread, "turn", index));
    followingSection.cards.insert(
        followingSection.cards.begin() + 6,
        {AuthoritativeItemKey{followingThread, "turn", "reasoning"},
         CardKind::Reasoning, followingThread, "turn", "reasoning",
         ReasoningData{"Initially hidden reasoning detail"}});
    followingSnapshot.sections.push_back(std::move(followingSection));

    ConversationView followingView;
    followingView.setPresentationOptions({false, true, true, true});
    followingView.resize(520, 320);
    followingView.show();
    bool followingResult = applyConversation(followingView, followingSnapshot);
    spin();
    const AuthoritativeItemKey incomingKey{followingThread, "turn", "incoming"};
    followingSnapshot.sections.front().cards.push_back(
        {incomingKey, CardKind::AgentActivity, followingThread, "turn",
         "incoming",
         AgentActivityData{"tool",
                           "completed",
                           "tool",
                           "New nested activity",
                           {},
                           {},
                           {},
                           {},
                           {},
                           {},
                           {}}});
    followingResult &= applyConversation(followingView, followingSnapshot);
    followingResult &= spinUntil(
        [&] { return card(followingView, stableKey(incomingKey)) != nullptr; });
    ConversationCard *incoming = card(followingView, stableKey(incomingKey));
    const int materializedTop =
        incoming ? incoming->mapTo(followingView.viewport(), QPoint{}).y() : -1;
    const bool materializedAtBottom = followingView.isAtBottom();
    spin(320);
    const int settledTop =
        incoming ? incoming->mapTo(followingView.viewport(), QPoint{}).y() : -1;
    if (!expect(followingResult && incoming && materializedAtBottom &&
                    materializedTop == settledTop,
                "a new nested card reaches its final followed position in "
                "the bounded materialization continuation"))
      return false;

    followingView.setPresentationOptions({true, true, true, true});
    const int immediateToggleTop =
        incoming->mapTo(followingView.viewport(), QPoint{}).y();
    const bool toggleImmediatelyAtBottom = followingView.isAtBottom();
    spin(320);
    const int settledToggleTop =
        incoming->mapTo(followingView.viewport(), QPoint{}).y();
    if (!expect(toggleImmediatelyAtBottom &&
                    immediateToggleTop == settledToggleTop,
                "presentation toggles relayout atomically without a follow "
                "animation"))
      return false;
  }

  const std::string thread = "presentation-options";
  const AuthoritativeItemKey updateKey{thread, "turn", "update"};
  const AuthoritativeItemKey finalKey{thread, "turn", "final"};
  const AuthoritativeItemKey reasoningKey{thread, "turn", "reasoning"};
  const AuthoritativeItemKey firstCommandKey{thread, "turn", "command-1"};
  const AuthoritativeItemKey firstImageKey{thread, "turn", "image-1"};
  ConversationGraphSpec snapshot;
  snapshot.threadId = thread;
  snapshot.sections.push_back(
      {"turn:presentation-options:turn",
       "turn",
       {{updateKey, CardKind::AgentMessage, thread, "turn", "update",
         AgentMessageData{"First retained update", false}},
        {finalKey, CardKind::AgentMessage, thread, "turn", "final",
         AgentMessageData{"Final answer remains visible", true}},
        {reasoningKey, CardKind::Reasoning, thread, "turn", "reasoning",
         ReasoningData{"First retained reasoning"}},
        {firstCommandKey, CardKind::CommandExecution, thread, "turn",
         "command-1",
         CommandExecutionData{"printf first", {}, "completed", {}, 0}},
        {firstImageKey, CardKind::ImageGeneration, thread, "turn", "image-1",
         ImageGenerationData{"/missing/image-1.png", "completed",
                             "First image"}}}});
  const auto containsText = [](QWidget *widget, const QString &needle) {
    return std::ranges::any_of(
        widget->findChildren<QLabel *>(),
        [&needle](QLabel *label) { return label->text().contains(needle); });
  };

  ConversationView view;
  view.setPresentationOptions({false, true, true, true});
  view.resize(700, 700);
  view.show();
  bool result = expect(applyConversation(view, snapshot),
                       "presentation-options fixture renders");
  result &= spinUntil([&] {
    return card(view, stableKey(updateKey)) &&
           card(view, stableKey(finalKey)) &&
           card(view, stableKey(firstCommandKey)) &&
           card(view, stableKey(firstImageKey));
  });
  QPointer<ConversationCard> update = card(view, stableKey(updateKey));
  QPointer<ConversationCard> final = card(view, stableKey(finalKey));
  QPointer<ConversationCard> reasoning = card(view, stableKey(reasoningKey));
  QPointer<ConversationCard> firstCommand =
      card(view, stableKey(firstCommandKey));
  QPointer<ConversationCard> firstImage = card(view, stableKey(firstImageKey));
  result &= expect(update && final && reasoning && firstCommand && firstImage &&
                       !update->isHidden() && !final->isHidden() &&
                       reasoning->isHidden() &&
                       !firstCommand->isCollapsed() &&
                       !firstImage->isCollapsed(),
                   "default presentation retains hidden reasoning and opens "
                   "commands and images");
  if (!update || !final || !firstCommand || !firstImage)
    return false;

  view.setPresentationOptions({false, false, false, false});
  spin();
  result &= expect(update && update->isHidden() && reasoning &&
                       reasoning->isHidden() && final && !final->isHidden() &&
                       firstCommand && !firstCommand->isCollapsed(),
                   "filters hide retained reasoning and update widgets without "
                   "changing final answers or existing folds");

  std::get<AgentMessageData>(snapshot.sections.front().cards[0].payload).text =
      "Updated while hidden";
  std::get<ReasoningData>(snapshot.sections.front().cards[2].payload).summary =
      "Reasoning updated while hidden";
  const AuthoritativeItemKey secondCommandKey{thread, "turn", "command-2"};
  const AuthoritativeItemKey secondImageKey{thread, "turn", "image-2"};
  snapshot.sections.front().cards.push_back(
      {secondCommandKey, CardKind::CommandExecution, thread, "turn",
       "command-2",
       CommandExecutionData{"printf second", {}, "completed", {}, 0}});
  snapshot.sections.front().cards.push_back(
      {secondImageKey, CardKind::ImageGeneration, thread, "turn", "image-2",
       ImageGenerationData{"/missing/image-2.png", "completed",
                           "Second image"}});
  result &= expect(applyConversation(view, snapshot),
                   "hidden cards and a new command accept updates");
  result &= spinUntil([&] {
    return card(view, stableKey(secondCommandKey)) &&
           card(view, stableKey(secondImageKey));
  });
  QPointer<ConversationCard> secondCommand =
      card(view, stableKey(secondCommandKey));
  QPointer<ConversationCard> secondImage =
      card(view, stableKey(secondImageKey));
  result &= expect(update && update->isHidden() && reasoning &&
                       reasoning->isHidden() && secondCommand &&
                       secondCommand->isCollapsed() && secondImage &&
                       secondImage->isCollapsed(),
                   "filtered nodes remain hidden while new commands and images "
                   "use current initial preferences");

  result &= expect(setFolded(firstCommand, true),
                   "an existing command records a user-owned collapsed state");
  view.setPresentationOptions({true, true, true, true});
  result &= spinUntil([&] {
    return card(view, stableKey(updateKey)) &&
           card(view, stableKey(reasoningKey));
  });
  update = card(view, stableKey(updateKey));
  reasoning = card(view, stableKey(reasoningKey));
  result &= expect(
      update && reasoning && !update->isHidden() && !reasoning->isHidden() &&
          containsText(update, QStringLiteral("Updated while hidden")) &&
          containsText(reasoning,
                       QStringLiteral("Reasoning updated while hidden")) &&
          firstCommand && firstCommand->isCollapsed() && secondCommand &&
          secondCommand->isCollapsed() && firstImage &&
          !firstImage->isCollapsed() && secondImage &&
          secondImage->isCollapsed(),
      "restoring visibility reveals latest content and preserves existing "
      "folds");

  const AuthoritativeItemKey thirdCommandKey{thread, "turn", "command-3"};
  snapshot.sections.front().cards.push_back(
      {thirdCommandKey, CardKind::CommandExecution, thread, "turn", "command-3",
       CommandExecutionData{"printf third", {}, "completed", {}, 0}});
  result &= expect(applyConversation(view, snapshot),
                   "a command arrives after restoring expanded-by-default");
  result &= spinUntil(
      [&] { return card(view, stableKey(thirdCommandKey)) != nullptr; });
  QPointer<ConversationCard> thirdCommand =
      card(view, stableKey(thirdCommandKey));
  result &=
      expect(thirdCommand && !thirdCommand->isCollapsed() && firstCommand &&
                 firstCommand->isCollapsed() && secondCommand &&
                 secondCommand->isCollapsed(),
             "only newly appearing commands use the changed initial folding "
             "preference");
  return result;
}

bool testInitialCommandGeometrySettlement() {
  const std::string thread = "initial-command-thread";
  QString output;
  for (int word = 0; word < 32; ++word)
    output += QStringLiteral("width-sensitive-output ");
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{"printf output", utf8(output), "completed", {}, 0}};
  ConversationGraphSpec snapshot{
      thread, {{"turn:initial-command", "turn", {command}}}, 0, false};

  ConversationView view;
  view.resize(650, 520);
  view.show();
  spin();
  bool result = expect(applyConversation(view, snapshot),
                       "initial visible command output is inserted");
  ConversationCard *commandCard = card(view, stableKey(command.key));
  result &= expect(setFolded(commandCard, false),
                   "initially folded command can be expanded for inspection");
  auto *outputView = commandCard ? dynamic_cast<CommandOutputView *>(
                                       commandCard->findChild<QTextEdit *>(
                                           QStringLiteral("commandOutputView")))
                                 : nullptr;
  result &= expect(commandCard && outputView && !outputView->isHidden() &&
                       outputView->height() < outputView->maximumHeight(),
                   "initial output is visible and below its height cap");
  if (!commandCard || !outputView)
    return false;
  const int immediateRange = view.verticalScrollBar()->maximum();
  const int immediateCardHeight = commandCard->height();
  const int immediateOutputHeight = outputView->height();
  const int immediateHint = outputView->sizeHint().height();
  spin();
  result &= expect(view.verticalScrollBar()->maximum() == immediateRange &&
                       commandCard->height() == immediateCardHeight &&
                       outputView->height() == immediateOutputHeight &&
                       outputView->sizeHint().height() == immediateHint,
                   "initial wrapped output has no delayed geometry settlement");

  const int glyphWidth = std::max(
      1, outputView->fontMetrics().horizontalAdvance(QLatin1Char('W')));
  const int charactersPerLine =
      std::max(1, outputView->viewport()->width() / glyphWidth);
  auto &execution = std::get<CommandExecutionData>(
      snapshot.sections.front().cards.front().payload);
  execution.output = utf8(QString(charactersPerLine + 1, QLatin1Char('W')));
  result &= expect(applyConversation(view, snapshot),
                   "single logical output line changes to two visual lines");
  spin();
  const QTextBlock wrappedBlock = outputView->document()->firstBlock();
  result &= expect(
      wrappedBlock.layout() && wrappedBlock.layout()->lineCount() == 2 &&
          outputView->verticalScrollBar()->maximum() == 0 &&
          outputView->viewport()->height() >=
              static_cast<int>(
                  std::ceil(outputView->document()->size().height())),
      "two visual output lines are fully visible without inner scrolling");
  return result;
}

bool testRootlessFinalAnswerGeometrySettlement() {
  const std::string thread = "rootless-child-thread";
  const VisibleCardData activity{
      AuthoritativeItemKey{thread, "turn", "activity"},
      CardKind::AgentActivity,
      thread,
      "turn",
      "activity",
      AgentActivityData{"spawn_agent",
                        "completed",
                        "tool",
                        "Child work",
                        {},
                        {},
                        {},
                        {},
                        {},
                        {},
                        {}}};
  VisibleCardData answer{
      AuthoritativeItemKey{thread, "turn", "answer"},
      CardKind::AgentMessage,
      thread,
      "turn",
      "answer",
      AgentMessageData{"Implemented the requested child-thread change.", true}};
  ConversationGraphSpec snapshot{
      thread, {{"turn:rootless-child", "turn", {activity, answer}}}, 0, false};

  ConversationView view;
  view.resize(700, 700);
  view.show();
  bool result = expect(applyConversation(view, snapshot),
                       "rootless child activity and final answer appear");
  spin();
  ConversationCard *answerCard = card(view, stableKey(answer.key));
  if (!answerCard)
    return false;
  answerCard->setMinimumHeight(600);
  answerCard->resize(answerCard->width(), 600);
  std::get<AgentMessageData>(answer.payload).text += "\n\nValidation passed.";
  snapshot.sections.front().cards.back() = answer;
  result &= expect(applyConversation(view, snapshot),
                   "rootless final answer accepts an authoritative update");
  spin();
  QLabel *answerBody = nullptr;
  for (QLabel *label : answerCard->findChildren<QLabel *>())
    if (!label->property("markdownSource").toString().isEmpty()) {
      answerBody = label;
      break;
    }
  result &= expect(answerBody &&
                       answerCard->height() == answerCard->minimumHeight() &&
                       answerCard->height() < 200 &&
                       answerBody->height() >=
                           answerBody->heightForWidth(answerBody->width()),
                   "rootless final answer settles to its natural final-width "
                   "height instead of retaining stale viewport space");
  return result;
}

bool testRetainedNestedFinalAnswerGeometrySettlement() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  const std::string thread = "retained-nested-final-answer";
  const VisibleCardData prompt{
      AuthoritativeItemKey{thread, "turn", "prompt"},
      CardKind::UserMessage,
      thread,
      "turn",
      "prompt",
      UserMessageData{"Please provide the complete retained report.", {}}};
  QString markdown = QStringLiteral(
      "The retained report contains enough Markdown to require its final "
      "nested width before height calculation.\n\n"
      "Its complete list must remain inside the final-answer border:\n\n");
  for (int index = 1; index <= 48; ++index)
    markdown += QStringLiteral(
                    "- Retained result %1 with explanatory text, **emphasis**, "
                    "and enough detail to wrap naturally at the nested card "
                    "width.\n")
                    .arg(index);
  markdown += QStringLiteral(
      "\nRenamed:\n\n"
      "- `src/codex/PresentationStatus.h` → `src/codex/UiStatus.h`\n\n"
      "</details>\n\n"
      "No remote operation was performed; the final line must remain fully "
      "visible.\n");
  const VisibleCardData answer{
      AuthoritativeItemKey{thread, "turn", "answer"},
      CardKind::AgentMessage,
      thread,
      "turn",
      "answer",
      AgentMessageData{"Retained final answer is materializing.", true}};
  TurnGraphSpec section{"turn:retained", "turn", {prompt}, prompt.key};
  for (int index = 0; index < 4; ++index)
    section.cards.push_back(
        agentCard(thread, "turn", index,
                  QStringLiteral("Retained update %1 preceding the final "
                                 "answer with enough text to wrap.")
                      .arg(index)));
  section.cards.push_back(answer);
  ConversationGraphSpec snapshot{thread, {std::move(section)}, 0, false};

  ConversationView view;
  view.resize(980, 420);
  bool result =
      expect(applyConversation(view, snapshot),
             "retained prompt and partial final answer materialize initially");
  std::get<AgentMessageData>(snapshot.sections.front().cards.back().payload)
      .text = utf8(markdown);
  result &= expect(applyConversation(view, snapshot),
                   "retained hydration completes before first exposure");
  view.resize(560, 420);
  view.show();
  result &= spinUntil([&] {
    return view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  });
  spin(160);
  ConversationCard *promptCard = card(view, stableKey(prompt.key));
  ConversationCard *answerCard = card(view, stableKey(answer.key));
  QLabel *answerBody = nullptr;
  if (answerCard)
    for (QLabel *label : answerCard->findChildren<QLabel *>())
      if (label->property("markdownSource").toString() == markdown) {
        answerBody = label;
        break;
      }
  int documentHeight = 0;
  if (answerBody) {
    QTextDocument document;
    document.setDefaultFont(answerBody->font());
    document.setDocumentMargin(0);
    document.setHtml(answerBody->text());
    document.setTextWidth(answerBody->width());
    documentHeight = static_cast<int>(std::ceil(document.size().height()));
  }
  if (!(promptCard && answerCard && answerBody &&
        promptCard->isAncestorOf(answerCard) &&
        answerBody->height() >=
            documentHeight + answerBody->fontMetrics().descent() &&
        answerBody->mapTo(answerCard, QPoint(0, answerBody->height())).y() <=
            answerCard->contentsRect().bottom() + 1))
    std::cerr << "nested final settle: prompt=" << bool(promptCard)
              << " answer=" << bool(answerCard)
              << " body=" << bool(answerBody) << " bodyHeight="
              << (answerBody ? answerBody->height() : -1)
              << " documentHeight=" << documentHeight << " cardBottom="
              << (answerCard ? answerCard->contentsRect().bottom() : -1)
              << " bodyBottom="
              << (answerBody ? answerBody
                                    ->mapTo(answerCard,
                                            QPoint(0, answerBody->height()))
                                    .y()
                             : -1)
              << " frozen="
              << view.property("bulkMaterializationUpdatesSuppressed").toBool()
              << '\n';
  const int answerBottomInPrompt =
      promptCard && answerCard
          ? answerCard->mapTo(promptCard, QPoint(0, answerCard->height())).y()
          : -1;
  result &= expect(
      promptCard && answerCard && answerBody &&
          promptCard->isAncestorOf(answerCard) &&
          answerBody->height() >=
              documentHeight + answerBody->fontMetrics().descent() &&
          answerBody->mapTo(answerCard, QPoint(0, answerBody->height())).y() <=
              answerCard->contentsRect().bottom() + 1 &&
          answerBottomInPrompt <= promptCard->contentsRect().bottom() + 1,
      "an initially retained nested final answer fully fits its rendered "
      "document, inner card, and canonical Turn/You owner");

  QPointer<ConversationCard> retainedPrompt = promptCard;
  QPointer<ConversationCard> retainedAnswer = answerCard;
  const VisibleCardData laterPrompt{
      AuthoritativeItemKey{thread, "later-turn", "later-prompt"},
      CardKind::UserMessage,
      thread,
      "later-turn",
      "later-prompt",
      UserMessageData{"A later prompt arrives after the long answer.", {}}};
  const VisibleCardData laterAnswer{
      AuthoritativeItemKey{thread, "later-turn", "later-answer"},
      CardKind::AgentMessage,
      thread,
      "later-turn",
      "later-answer",
      AgentMessageData{"The later result is complete.", true}};
  snapshot.sections.push_back(
      {"turn:later", "later-turn", {laterPrompt, laterAnswer}, laterPrompt.key});
  result &= expect(applyConversation(view, snapshot),
                   "a later completed Turn is appended after the long answer");
  spin(160);
  promptCard = card(view, stableKey(prompt.key));
  answerCard = card(view, stableKey(answer.key));
  ConversationCard *laterPromptCard = card(view, stableKey(laterPrompt.key));
  QWidget *retainedSection = promptCard ? promptCard->parentWidget() : nullptr;
  while (retainedSection &&
         retainedSection->property("turnSectionKey").toString().isEmpty())
    retainedSection = retainedSection->parentWidget();
  const int retainedAnswerBottom =
      promptCard && answerCard
          ? answerCard->mapTo(promptCard, QPoint(0, answerCard->height())).y()
          : -1;
  const int promptBottomInSection =
      retainedSection && promptCard
          ? promptCard->mapTo(retainedSection,
                              QPoint(0, promptCard->height()))
                .y()
          : -1;
  const int promptBottomInViewport =
      promptCard
          ? promptCard->mapTo(view.viewport(),
                              QPoint(0, promptCard->height()))
                .y()
          : -1;
  const int laterTopInViewport =
      laterPromptCard
          ? laterPromptCard->mapTo(view.viewport(), QPoint()).y()
          : -1;
  result &= expect(
      promptCard && answerCard && laterPromptCard && retainedSection &&
          retainedPrompt == promptCard &&
          retainedAnswer == answerCard && promptCard->isAncestorOf(answerCard) &&
          retainedAnswerBottom <= promptCard->contentsRect().bottom() + 1 &&
          promptBottomInSection <= retainedSection->contentsRect().bottom() + 1 &&
          laterTopInViewport >= promptBottomInViewport + 8,
      "appending a later conversation card cannot clip the retained long "
      "answer through its Turn/You owner or section boundary");
  spin();
  qApp->setStyleSheet(originalStyleSheet);
  return result;
}

bool testBottomAnchoredCommandOutputGrowth() {
  const std::string thread = "bottom-anchored-output";
  ConversationGraphSpec snapshot = conversation(thread, 14);
  VisibleCardData command{
      AuthoritativeItemKey{thread, "turn-2", "live-command"},
      CardKind::CommandExecution,
      thread,
      "turn-2",
      "live-command",
      CommandExecutionData{
          "run live command", {}, "inProgress", {}, std::nullopt}};
  snapshot.sections.back().cards.push_back(command);

  ConversationView view;
  view.resize(620, 360);
  view.show();
  applyConversation(view, snapshot);
  spinUntil([&] {
    return view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  });
  ConversationCard *commandCard = card(view, stableKey(command.key));
  bool result = expect(setFolded(commandCard, false),
                       "live command expands from its compact default");
  wheel(view, -10000);
  auto *metadata =
      commandCard
          ? commandCard->findChild<QLabel *>(QStringLiteral("commandMetadata"))
          : nullptr;
  auto *status =
      commandCard
          ? commandCard->findChild<QLabel *>(QStringLiteral("commandStatus"))
          : nullptr;
  auto *output = commandCard ? dynamic_cast<CommandOutputView *>(
                                   commandCard->findChild<QTextEdit *>(
                                       QStringLiteral("commandOutputView")))
                             : nullptr;
  result &= expect(commandCard && metadata && metadata->isHidden() && status &&
                       output && output->isHidden() && view.isAtBottom() &&
                       status->property("tone") == "active",
                   "live command starts with a hidden zero-line output");
  if (!commandCard || !metadata || !status || !output)
    return false;
  const int cardBottomBefore =
      commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height())).y();

  auto &live = std::get<CommandExecutionData>(
      snapshot.sections.back().cards.back().payload);
  live.output =
      "first wrapped output line with enough words to use real width\n"
      "second output line\nthird output line\n\n";
  result &=
      expect(applyConversation(view, snapshot), "live output becomes visible");
  spinUntil([&] {
    return !output->isHidden() && output->height() > 2 * 20 &&
           output->height() == output->sizeHint().height();
  });
  const int cardBottomAfter =
      commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height())).y();
  result &= expect(!output->isHidden() && output->height() > 2 * 20 &&
                       output->height() == output->sizeHint().height() &&
                       cardBottomAfter == cardBottomBefore && view.isAtBottom(),
                   "multiline output takes its needed height and grows upward");

  QString cappedOutput;
  for (int line = 0; line < 80; ++line)
    cappedOutput += QStringLiteral("scrollable line %1\n").arg(line);
  live.output = utf8(cappedOutput);
  result &=
      expect(applyConversation(view, snapshot), "live output reaches its cap");
  spinUntil([&] {
    return output->height() == 220 &&
           output->verticalScrollBar()->maximum() > 0;
  });
  if (!(output->height() == 220 &&
        output->verticalScrollBar()->maximum() > 0 &&
        commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height()))
                .y() == cardBottomBefore))
    std::cerr << "capped output: height=" << output->height()
              << " maximum=" << output->verticalScrollBar()->maximum()
              << " presentedBytes="
              << std::get<CommandExecutionData>(commandCard->data().payload)
                     .output.size()
              << " expectedBytes=" << utf8(cappedOutput).size()
              << " bottom="
              << commandCard
                     ->mapTo(view.viewport(), QPoint(0, commandCard->height()))
                     .y()
              << " expectedBottom=" << cardBottomBefore << " frozen="
              << view.property("bulkMaterializationUpdatesSuppressed").toBool()
              << " blocker="
              << view.property("bulkMaterializationBlocker")
                     .toString()
                     .toStdString()
              << '\n';
  result &= expect(
      output->height() == 220 && output->verticalScrollBar()->maximum() > 0 &&
          commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height()))
                  .y() == cardBottomBefore,
      "capped output keeps its scrollbar and fixed card bottom");

  const qulonglong geometryBeforeAppend =
      view.property("conversationGeometryPasses").toULongLong();
  QPointer<ConversationCard> retainedCommand = commandCard;
  live.output += "one more append-only streaming line\n";
  result &= expect(applyConversation(view, snapshot),
                   "capped output accepts another streaming append");
  spin();
  result &= expect(
      retainedCommand == commandCard && output->height() == 220 &&
          view.property("conversationGeometryPasses").toULongLong() ==
              geometryBeforeAppend &&
          commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height()))
                  .y() == cardBottomBefore,
      "append-only capped output repaints its retained card without a "
      "conversation geometry pass");
  return result;
}

bool testCommandOutputStateAcrossNavigation() {
  const std::string thread = "command-navigation-thread";
  QString output;
  for (int line = 0; line < 80; ++line)
    output += QStringLiteral("retained line %1\n").arg(line);
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{"produce output", utf8(output), "completed", {}, 0}};
  const ConversationGraphSpec commandThread{
      thread, {{"turn:command-navigation", "turn", {command}}}, 0, false};

  ConversationView view;
  view.resize(650, 520);
  view.show();
  applyConversation(view, commandThread);
  spin();
  ConversationCard *commandCard = card(view, stableKey(command.key));
  bool result = expect(setFolded(commandCard, false),
                       "navigation command expands from its compact default");
  auto *initialOutput = commandCard
                            ? dynamic_cast<CommandOutputView *>(
                                  commandCard->findChild<QTextEdit *>(
                                      QStringLiteral("commandOutputView")))
                            : nullptr;
  result &=
      expect(initialOutput && initialOutput->verticalScrollBar()->maximum() > 0,
             "navigation test has independently scrollable output");
  if (!initialOutput)
    return false;
  applyConversation(view, conversation("other-thread", 8));
  spin();
  applyConversation(view, commandThread);
  spin();
  commandCard = card(view, stableKey(command.key));
  initialOutput = commandCard ? dynamic_cast<CommandOutputView *>(
                                    commandCard->findChild<QTextEdit *>(
                                        QStringLiteral("commandOutputView")))
                              : nullptr;
  result &= expect(initialOutput && initialOutput->followsLatest() &&
                       initialOutput->verticalScrollBar()->value() ==
                           initialOutput->verticalScrollBar()->maximum(),
                   "framework geometry during navigation does not pause a "
                   "following command output");
  if (!initialOutput)
    return false;
  initialOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  spin();
  const int pausedValue = initialOutput->verticalScrollBar()->value();
  result &= expect(!initialOutput->followsLatest(),
                   "command output is paused before thread navigation");

  applyConversation(view, conversation("other-thread", 8));
  spin();
  applyConversation(view, commandThread);
  spin();
  commandCard = card(view, stableKey(command.key));
  auto *restoredOutput = commandCard
                             ? dynamic_cast<CommandOutputView *>(
                                   commandCard->findChild<QTextEdit *>(
                                       QStringLiteral("commandOutputView")))
                             : nullptr;
  result &=
      expect(restoredOutput && !restoredOutput->followsLatest() &&
                 restoredOutput->verticalScrollBar()->value() == pausedValue,
             "thread navigation restores paused command output state");
  return result;
}

#if defined(CODEXUI_DIRECT_GRAPH_WIDGET_TESTS)
bool testLoadedWindowMaterializesOnce() {
  const std::string thread = "viewport-lazy";
  ConversationGraphSpec snapshot = conversation(thread, 240);
  ConversationView view;
  view.resize(620, 360);
  view.show();

  bool result = expect(applyConversation(view, snapshot),
                       "a large conversation creates its lazy geometry");
  const int immediateCards = liveConversationWidgetCounts(view).cards;
  result &= expect(immediateCards <= 8,
                   "the first loaded-window pass remains card-budgeted");
  const bool loadedWindowReady = spinUntil([&] {
    const LiveConversationWidgetCounts widgets =
        liveConversationWidgetCounts(view);
    return widgets.cards == static_cast<int>(AuthoritativeHistoryPageSize) &&
           widgets.itemPlaceholders == 0;
  }, 512);
  const LiveConversationWidgetCounts settled =
      liveConversationWidgetCounts(view);
  result &= expect(
      loadedWindowReady &&
          settled.cards == static_cast<int>(AuthoritativeHistoryPageSize) &&
          settled.turnSections <= 2 && view.isAtBottom() &&
          historyButton(view)->isVisible(),
      "thread selection materializes exactly the loaded 80-card window in "
      "bounded continuations");

  const std::string firstKey =
      stableKey(snapshot.sections.back().cards[40].key);
  const std::string lastKey =
      stableKey(snapshot.sections.back().cards.back().key);
  const QVariant firstRetainedScroll =
      view.property("graphFirstRetainedScrollValue");
  QPointer<ConversationCard> firstIdentity = card(view, firstKey);
  QPointer<ConversationCard> lastIdentity = card(view, lastKey);
  const qulonglong geometryBeforeScroll =
      view.property("conversationGeometryPasses").toULongLong();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  if (firstRetainedScroll.isValid())
    view.verticalScrollBar()->setValue(firstRetainedScroll.toInt());
  spin(100);
  result &= expect(view.mode() == ConversationView::Mode::Paused,
                   "scrolling a large conversation pauses following");
  result &= expect(card(view, firstKey) == firstIdentity &&
                       card(view, lastKey) == lastIdentity,
                   "scrolling retains both ends of the loaded card window");
  const LiveConversationWidgetCounts scrolled =
      liveConversationWidgetCounts(view);
  result &= expect(
      firstRetainedScroll.isValid() &&
          scrolled.cards == settled.cards && scrolled.turnSections <= 2 &&
          view.property("conversationGeometryPasses").toULongLong() ==
              geometryBeforeScroll &&
          graphPassBudgetsWereRespected(view),
      "viewport movement performs no card construction, destruction, or "
      "geometry pass while initial materialization remains pass-budgeted");
  return result;
}

// Retained only as a record of the discarded direct-graph QWidget scanner.
// The production widget API intentionally has no bindGraph/graphChanged seam;
// canonical graph-to-snapshot coverage lives in NodeGraphConversationUiTest.
bool testGraphBackedLazyRenderingAndLifetime() {
  GraphConversationFixture fixture;
  ConversationView view;
  view.resize(620, 360);
  view.show();
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.showReasoning = false;
  view.setPresentationOptions(options);
  view.bindGraph(fixture.graph, fixture.thread);

  const auto materializedCount = [&view] {
    return static_cast<int>(std::ranges::count_if(
        view.findChildren<QWidget *>(), [](QWidget *widget) {
          return dynamic_cast<ConversationCard *>(widget) != nullptr;
        }));
  };
  bool result = expect(
      materializedCount() <= 8 && graphAttachment(fixture.reasoning) == nullptr,
      "graph binding performs at most eight immediate renders and leaves a "
      "filtered item unmaterialized");
  const bool loadedWindowReady = spinUntil([&] {
    return view.property("graphStructureScanComplete").toBool() &&
           view.property("graphLiveRecordCount").toULongLong() ==
               fixture.messages.size() &&
           std::ranges::all_of(
               fixture.messages, [](const nodegraph::NodeRef &message) {
                 const auto *attachment = graphAttachment(message);
                 return attachment && attachment->widget;
               }) &&
           view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 1024);
  spin();

  nodegraph::NodeRef offscreen = fixture.messages.front();
  nodegraph::NodeRef visible = fixture.messages.back();
  ui::QtNodeAttachment *offscreenInitialAttachment =
      graphAttachment(offscreen);
  const std::uint64_t offscreenInitialRevision =
      offscreenInitialAttachment
          ? offscreenInitialAttachment->renderedRevision
          : 0;
  result &= expect(loadedWindowReady && offscreenInitialAttachment,
                   "every card in the selected loaded window materializes "
                   "once, including its initially off-screen cards");
  ui::QtNodeAttachment *visibleAttachment = graphAttachment(visible);
  QPointer<QWidget> visibleIdentity =
      visibleAttachment ? visibleAttachment->widget : nullptr;
  result &= expect(visibleAttachment && visibleIdentity,
                   "a viewport graph node owns its Qt attachment");

  nodegraph::GraphChange visibleChange;
  {
    auto graphWrite = fixture.graph.write();
    graphWrite.setField(visible, "text", "Visible graph revision");
    visibleChange = graphWrite.finish();
  }
  view.graphChanged(visibleChange.removed);
  const bool visibleRevisionRendered = spinUntil([&] {
    const auto *attachment = graphAttachment(visible);
    return attachment &&
           attachment->renderedRevision == visibleChange.revision;
  });
  visibleAttachment = graphAttachment(visible);
  auto *visibleCard =
      visibleAttachment
          ? qobject_cast<ConversationCard *>(visibleAttachment->widget.data())
          : nullptr;
  const auto *visibleMessage =
      visibleCard ? std::get_if<AgentMessageData>(&visibleCard->data().payload)
                  : nullptr;
  if (!(visibleAttachment && visibleAttachment->widget == visibleIdentity &&
        visibleAttachment->renderedRevision == visibleChange.revision &&
        visibleMessage && visibleMessage->text == "Visible graph revision"))
    std::cerr << "visible revision: attachment=" << bool(visibleAttachment)
              << " viewport="
              << (visibleAttachment ? visibleAttachment->viewportVisible : 0)
              << " rendered="
              << (visibleAttachment ? visibleAttachment->renderedRevision : 0)
              << " expected=" << visibleChange.revision << " text="
              << (visibleMessage ? visibleMessage->text : "<none>")
              << " scroll=" << view.verticalScrollBar()->value() << '/'
              << view.verticalScrollBar()->maximum() << " frozen="
              << view.property("bulkMaterializationUpdatesSuppressed").toBool()
              << '\n';
  result &= expect(
      visibleRevisionRendered && visibleAttachment &&
          visibleAttachment->widget == visibleIdentity &&
          visibleAttachment->renderedRevision == visibleChange.revision &&
          visibleMessage && visibleMessage->text == "Visible graph revision",
      "a visible node revision updates the existing attached card");

  nodegraph::GraphChange deferredChange;
  {
    auto graphWrite = fixture.graph.write();
    graphWrite.setField(offscreen, "text", "Deferred off-screen revision");
    graphWrite.setField(fixture.reasoning, "summary",
                        "Deferred filtered reasoning revision");
    deferredChange = graphWrite.finish();
  }
  view.graphChanged(deferredChange.removed);
  spin(40);
  result &= expect(
      graphAttachment(offscreen) == offscreenInitialAttachment &&
          graphAttachment(offscreen)->renderedRevision ==
              offscreenInitialRevision &&
          graphAttachment(fixture.reasoning) == nullptr,
      "off-screen loaded and filtered node updates perform no QWidget "
      "projection while retaining existing card identity");

  options.showReasoning = true;
  view.setPresentationOptions(options);
  spin(20);
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  const bool newlyVisibleRendered = spinUntil([&] {
    const auto *offscreenCurrent = graphAttachment(offscreen);
    const auto *reasoningCurrent = graphAttachment(fixture.reasoning);
    return offscreenCurrent && reasoningCurrent &&
           offscreenCurrent->renderedRevision == deferredChange.revision;
  });
  ui::QtNodeAttachment *offscreenAttachment = graphAttachment(offscreen);
  auto *offscreenCard =
      offscreenAttachment
          ? qobject_cast<ConversationCard *>(offscreenAttachment->widget.data())
          : nullptr;
  const auto *offscreenMessage =
      offscreenCard
          ? std::get_if<AgentMessageData>(&offscreenCard->data().payload)
          : nullptr;
  ui::QtNodeAttachment *reasoningAttachment =
      graphAttachment(fixture.reasoning);
  auto *reasoningCard =
      reasoningAttachment
          ? qobject_cast<ConversationCard *>(reasoningAttachment->widget.data())
          : nullptr;
  const auto *reasoningData =
      reasoningCard ? std::get_if<ReasoningData>(&reasoningCard->data().payload)
                    : nullptr;
  result &= expect(
      newlyVisibleRendered && offscreenAttachment &&
          offscreenAttachment->renderedRevision == deferredChange.revision &&
          offscreenMessage &&
          offscreenMessage->text == "Deferred off-screen revision" &&
          reasoningAttachment && reasoningData &&
          reasoningData->summary == "Deferred filtered reasoning revision",
      "newly visible nodes render once from their latest graph state");

  const std::uint64_t renderedBeforeContention =
      offscreenAttachment ? offscreenAttachment->renderedRevision : 0;
  const qulonglong retriesBeforeContention =
      view.property("graphContentionRetryCount").toULongLong();
  auto contendedWrite = fixture.graph.write();
  contendedWrite.setField(offscreen, "text", "Rendered after lock retry");
  view.graphChanged();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  result &= expect(
      view.property("graphContentionRetryDelayMs").toInt() > 0 &&
          view.property("graphContentionRetryDelayMs").toInt() <= 16 &&
          view.property("graphContentionRetryCount").toULongLong() >
              retriesBeforeContention &&
          graphAttachment(offscreen) &&
          graphAttachment(offscreen)->renderedRevision ==
              renderedBeforeContention,
      "a contended graph read returns to Qt and schedules a nonzero bounded "
      "retry without rendering stale state");
  const nodegraph::GraphChange contentionChange = contendedWrite.finish();
  spin(60);
  offscreenAttachment = graphAttachment(offscreen);
  offscreenCard =
      offscreenAttachment
          ? qobject_cast<ConversationCard *>(offscreenAttachment->widget.data())
          : nullptr;
  offscreenMessage =
      offscreenCard
          ? std::get_if<AgentMessageData>(&offscreenCard->data().payload)
          : nullptr;
  result &= expect(
      offscreenAttachment &&
          offscreenAttachment->renderedRevision == contentionChange.revision &&
          offscreenMessage &&
          offscreenMessage->text == "Rendered after lock retry",
      "the already-scheduled retry renders the current revision after "
      "contention clears without another notification");

  QPointer<QWidget> removedWidget =
      offscreenAttachment ? offscreenAttachment->widget : nullptr;
  auto removalWrite = fixture.graph.write();
  removalWrite.remove(offscreen);
  const nodegraph::GraphChange removal = removalWrite.finish();
  view.graphChanged(removal.removed);
  result &=
      expect(offscreen->uiAttachment() == nullptr && removedWidget.isNull(),
             "removal synchronously clears the node attachment and "
             "deletes its widget");
  spin(20);
  return result;
}

bool testGraphStructureScanSurvivesUnrelatedRevisionChurn() {
  constexpr std::size_t ItemCount = 400;
  constexpr std::size_t AdditionalPages = 3;
  constexpr std::size_t ExpectedScanTarget =
      (AdditionalPages + 1) * AuthoritativeHistoryPageSize;
  constexpr int ChurnRevisions = 96;
  constexpr int EventDispatchLimit = 512;

  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef unrelated;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", ItemCount);
    thread = write.upsert({nodegraph::NodeKind::Thread, "scan-thread"},
                          std::move(threadState));
    nodegraph::NodeRef turn =
        write.upsert({nodegraph::NodeKind::Turn, "scan-turn"});
    write.setParent(thread, turn);
    for (std::size_t index = 0; index < ItemCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          "agentMessage", "Scanned history " + std::to_string(index));
      state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item, "scan-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
    }

    nodegraph::NodeRef unrelatedThread =
        write.upsert({nodegraph::NodeKind::Thread, "unrelated-scan-thread"});
    nodegraph::NodeRef unrelatedTurn =
        write.upsert({nodegraph::NodeKind::Turn, "unrelated-scan-turn"});
    unrelated =
        write.upsert({nodegraph::NodeKind::Item, "unrelated-scan-item"},
                     graphMessageState("agentMessage", "Unrelated activity"));
    write.setParent(unrelatedThread, unrelatedTurn);
    write.setParent(unrelatedTurn, unrelated);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 360);
  view.show();
  view.bindGraph(graph, thread);

  QPushButton *loadMore = historyButton(view);
  const qulonglong retainedAtFirstYield =
      view.property("graphRetainedGeometryRecordCount").toULongLong();
  bool result = expect(
      loadMore && retainedAtFirstYield > 32 &&
          retainedAtFirstYield < AuthoritativeHistoryPageSize &&
          !view.property("graphStructureScanComplete").toBool(),
      "the fixture yields during an actually incomplete selected-history "
      "structure scan after more than thirty-two records");
  if (!loadMore)
    return false;
  for (std::size_t page = 0; page < AdditionalPages; ++page)
    loadMore->click();

  int churnCount = 0;
  int completedAtChurn = -1;
  std::function<void()> churn;
  churn = [&] {
    if (completedAtChurn < 0 &&
        view.property("graphStructureScanComplete").toBool() &&
        view.property("graphStructureScanTarget").toULongLong() >=
            ExpectedScanTarget)
      completedAtChurn = churnCount;
    if (churnCount >= ChurnRevisions)
      return;

    nodegraph::GraphChange change;
    {
      auto write = graph.write();
      write.setField(unrelated, "unrelatedRevision",
                     static_cast<std::uint64_t>(churnCount + 1));
      change = write.finish();
    }
    ++churnCount;
    view.graphChangedDeferred(change.affected, change.removed);
    if (churnCount < ChurnRevisions)
      QTimer::singleShot(0, &view, churn);
  };
  QTimer::singleShot(0, &view, churn);

  int dispatches = 0;
  while (churnCount < ChurnRevisions && dispatches < EventDispatchLimit) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    ++dispatches;
  }

  result &= expect(
      churnCount == ChurnRevisions && completedAtChurn >= 0 &&
          completedAtChurn < ChurnRevisions &&
          view.property("graphStructureScanComplete").toBool() &&
          view.property("graphStructureScanTarget").toULongLong() >=
              ExpectedScanTarget &&
          view.property("graphRetainedGeometryRecordCount").toULongLong() >=
              ExpectedScanTarget &&
          graphPassBudgetsWereRespected(view),
      "continuous unrelated Item field revisions cannot restart or starve the "
      "bounded selected-history structure scan");
  return result;
}

bool testGraphGeometryScanSurvivesVisibleHeightChurn() {
  constexpr int FilteredItems = 78;
  constexpr int ChurnRevisions = 48;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef streaming;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", FilteredItems + 2);
    thread =
        write.upsert({nodegraph::NodeKind::Thread, "geometry-churn-thread"},
                     std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "geometry-churn-turn"});
    write.setParent(thread, turn);

    nodegraph::NodeState sentinel =
        graphMessageState("agentMessage", "Visible sentinel");
    sentinel.fields.emplace("phase", "final_answer");
    nodegraph::NodeRef sentinelNode =
        write.upsert({nodegraph::NodeKind::Item, "geometry-churn-sentinel"},
                     std::move(sentinel));
    write.setParent(turn, sentinelNode);
    for (int index = 0; index < FilteredItems; ++index) {
      nodegraph::NodeState hidden;
      hidden.status = nodegraph::NodeStatus::Completed;
      hidden.fields.emplace("type", "reasoning");
      hidden.fields.emplace("summary",
                            "Filtered geometry " + std::to_string(index));
      nodegraph::NodeRef item =
          write.upsert({nodegraph::NodeKind::Item,
                        "geometry-churn-hidden-" + std::to_string(index)},
                       std::move(hidden));
      write.setParent(turn, item);
    }
    nodegraph::NodeState initial =
        graphMessageState("agentMessage", "Initial visible stream");
    initial.fields.emplace("phase", "final_answer");
    streaming =
        write.upsert({nodegraph::NodeKind::Item, "geometry-churn-stream"},
                     std::move(initial));
    write.setParent(turn, streaming);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 360);
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.showReasoning = false;
  view.setPresentationOptions(options);
  view.show();
  view.bindGraph(graph, thread);
  const bool initiallyVisible = dispatchUntil([&] {
    ui::QtNodeAttachment *attachment = graphAttachment(streaming);
    return attachment && attachment->widget && attachment->viewportVisible &&
           view.property("graphStructureScanComplete").toBool() &&
           view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  });
  std::uint64_t latestRevision = 0;
  std::string latestText;
  int churnCount = 0;
  int renderedDuringChurn = -1;
  view.resize(430, 360);
  const qulonglong fullGeometryPassesBeforeChurn =
      view.property("conversationFullGeometryPasses").toULongLong();
  std::function<void()> churn;
  churn = [&] {
    if (latestRevision != 0) {
      ui::QtNodeAttachment *attachment = graphAttachment(streaming);
      if (attachment && attachment->renderedRevision >= latestRevision &&
          renderedDuringChurn < 0)
        renderedDuringChurn = churnCount;
    }
    if (churnCount >= ChurnRevisions)
      return;

    ++churnCount;
    latestText = "Visible stream revision " + std::to_string(churnCount);
    const int lines = churnCount % 2 == 0 ? 14 : 2;
    for (int line = 0; line < lines; ++line)
      latestText += "\nheight-changing selected text " + std::to_string(line);
    nodegraph::GraphChange change;
    {
      auto write = graph.write();
      write.setField(streaming, "text", latestText);
      change = write.finish();
    }
    latestRevision = change.revision;
    view.graphChangedDeferred(change.affected, change.removed);
    if (churnCount < ChurnRevisions)
      QTimer::singleShot(0, &view, churn);
  };
  churn();

  const bool churnCompleted = dispatchUntil(
      [&] { return churnCount == ChurnRevisions; }, ChurnRevisions * 8);
  const bool finalRevisionRendered = dispatchUntil([&] {
    ui::QtNodeAttachment *attachment = graphAttachment(streaming);
    auto *widget =
        attachment ? qobject_cast<ConversationCard *>(attachment->widget.data())
                   : nullptr;
    const auto *message =
        widget ? std::get_if<AgentMessageData>(&widget->data().payload)
               : nullptr;
    return attachment && attachment->renderedRevision >= latestRevision &&
           message && message->text == latestText;
  });

  return expect(
      initiallyVisible && churnCompleted && renderedDuringChurn >= 0 &&
          renderedDuringChurn < ChurnRevisions && finalRevisionRendered &&
          view.property("conversationFullGeometryPasses").toULongLong() ==
              fullGeometryPassesBeforeChurn &&
          view.property("graphMaxGeometryRecordsPerPass").toULongLong() > 0 &&
          view.property("graphMaxGeometryRecordsPerPass").toULongLong() <= 32 &&
          view.property("graphMaxCardOperationsPerPass").toULongLong() <= 8,
      "continuous visible height revisions cannot restart and starve the "
      "bounded geometry frontier");
}

bool testFocusedGraphCardSurvivesViewportReconciliation() {
  constexpr int HistoryItems = 240;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef command;
  std::vector<nodegraph::NodeRef> messages;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", HistoryItems + 1);
    thread = write.upsert({nodegraph::NodeKind::Thread, "focused-card-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "focused-card-turn"});
    write.setParent(thread, turn);
    messages.reserve(HistoryItems);
    for (int index = 0; index < HistoryItems; ++index) {
      nodegraph::NodeState state = graphMessageState(
          "agentMessage", "Focus history " + std::to_string(index));
      state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item =
          write.upsert({nodegraph::NodeKind::Item,
                        "focused-card-item-" + std::to_string(index)},
                       std::move(state));
      write.setParent(turn, item);
      messages.push_back(std::move(item));
    }
    nodegraph::NodeState commandState;
    commandState.status = nodegraph::NodeStatus::Completed;
    commandState.fields.emplace("type", "commandExecution");
    commandState.fields.emplace("command", "retain focused output");
    std::string output;
    for (int line = 0; line < 60; ++line)
      output += "focused output " + std::to_string(line) + "\n";
    commandState.fields.emplace("output", std::move(output));
    commandState.fields.emplace("status", "completed");
    command = write.upsert({nodegraph::NodeKind::Item, "focused-card-command"},
                           std::move(commandState));
    write.setParent(turn, command);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 360);
  view.show();
  view.bindGraph(graph, thread);
  const bool commandReady = dispatchUntil([&] {
    ui::QtNodeAttachment *attachment = graphAttachment(command);
    return attachment && attachment->widget && attachment->viewportVisible;
  });
  QPointer<ConversationCard> commandCard;
  if (ui::QtNodeAttachment *attachment = graphAttachment(command))
    commandCard = qobject_cast<ConversationCard *>(attachment->widget.data());
  bool result = expect(commandReady && setFolded(commandCard, false),
                       "the focus-pinning fixture exposes its command output");
  QPointer<CommandOutputView> output =
      commandCard ? dynamic_cast<CommandOutputView *>(
                        commandCard->findChild<QTextEdit *>(
                            QStringLiteral("commandOutputView")))
                  : nullptr;
  if (!commandCard || !output)
    return false;
  view.raise();
  view.activateWindow();
  output->setFocus(Qt::OtherFocusReason);
  dispatchPasses(2);
  const bool focusEstablished =
      QApplication::focusWidget() == output ||
      commandCard->isAncestorOf(QApplication::focusWidget());

  const std::size_t retainedStart =
      messages.size() - (AuthoritativeHistoryPageSize - 1);
  nodegraph::NodeRef earliestRetained = messages[retainedStart];
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.verticalScrollBar()->setValue(
      view.property("graphFirstRetainedScrollValue").toInt());
  const bool distantViewportReady = dispatchUntil(
      [&] { return graphAttachment(earliestRetained) != nullptr; });
  ui::QtNodeAttachment *pinnedAttachment = graphAttachment(command);
  const bool retainedWhileFocused =
      commandCard && pinnedAttachment &&
      pinnedAttachment->widget == commandCard &&
      (QApplication::focusWidget() == output ||
       commandCard->isAncestorOf(QApplication::focusWidget()));
  if (!(focusEstablished && distantViewportReady && retainedWhileFocused)) {
    std::cerr << "focus pin: established=" << focusEstablished
              << " distant=" << distantViewportReady
              << " retained=" << retainedWhileFocused
              << " command=" << static_cast<bool>(commandCard)
              << " attachment=" << static_cast<bool>(pinnedAttachment)
              << " same="
              << (pinnedAttachment && pinnedAttachment->widget == commandCard)
              << " focus="
              << (QApplication::focusWidget()
                      ? QApplication::focusWidget()->metaObject()->className()
                      : "null")
              << '\n';
  }
  result &= expect(
      focusEstablished && distantViewportReady && retainedWhileFocused,
      "viewport reconciliation pins a focused card while materializing a "
      "distant viewport");

  output->clearFocus();
  view.graphChangedDeferred();
  dispatchPasses(16);
  result &= expect(graphAttachment(command) &&
                       graphAttachment(command)->widget == commandCard,
                   "a loaded card retains its QWidget and local output state "
                   "after focus leaves and it becomes off-screen");
  return result;
}

bool testGraphRootFoldSuppressesAndRestoresChildExtent() {
  const std::string thread = "root-fold-extent";
  TurnGraphSpec preceding{"turn:root-fold-preceding", "preceding", {}};
  for (int index = 0; index < 12; ++index)
    preceding.cards.push_back(agentCard(thread, "preceding", index));

  const VisibleCardData root{
      AuthoritativeItemKey{thread, "fold-turn", "root"},
      CardKind::UserMessage,
      thread,
      "fold-turn",
      "root",
      UserMessageData{"Fold this complete turn without losing child state.",
                      {}}};
  TurnGraphSpec folded{"turn:root-fold-target", "fold-turn", {root}, root.key};
  for (int index = 0; index < 5; ++index)
    folded.cards.push_back(agentCard(thread, "fold-turn", 100 + index));
  QString outputText;
  for (int line = 0; line < 70; ++line)
    outputText += QStringLiteral("retained child output %1\n").arg(line);
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "fold-turn", "command"},
      CardKind::CommandExecution,
      thread,
      "fold-turn",
      "command",
      CommandExecutionData{"preserve child interaction", utf8(outputText),
                           "completed", "/workspace", 0}};
  folded.cards.push_back(command);
  ConversationGraphSpec snapshot{
      thread, {std::move(preceding), std::move(folded)}, 0, false};

  ConversationView view;
  view.resize(650, 520);
  view.show();
  const bool applied = applyConversation(view, snapshot);
  const bool cardsReady = dispatchUntil([&] {
    ui::QtNodeAttachment *commandAttachment = nullptr;
    if (snapshot.storage) {
      auto read = snapshot.storage->graph.tryRead();
      if (read) {
        nodegraph::NodeRef commandNode =
            read->find({nodegraph::NodeKind::Item, fixtureNodeId(command)});
        commandAttachment = graphAttachment(commandNode);
      }
    }
    return card(view, stableKey(root.key)) &&
           card(view, stableKey(command.key)) && commandAttachment &&
           commandAttachment->viewportVisible;
  });
  // Measure both ends of the retained window before comparing the fold
  // round-trip. Otherwise previously unseen cards in the preceding turn can
  // legitimately replace their estimated heights while the target turn is
  // folded, obscuring the target turn's exact effective-extent invariant.
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  const bool precedingMeasured = dispatchUntil([&] {
    return card(view, stableKey(snapshot.sections.front().cards.front().key));
  });
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  const bool targetRestored = dispatchUntil([&] {
    return card(view, stableKey(root.key)) &&
           card(view, stableKey(command.key));
  });
  QPointer<ConversationCard> rootCard = card(view, stableKey(root.key));
  QPointer<ConversationCard> commandCard = card(view, stableKey(command.key));
  QPointer<QToolButton> commandDisclosure = disclosure(commandCard);
  if (commandDisclosure) {
    commandDisclosure->setFocus(Qt::OtherFocusReason);
    dispatchPasses(1);
  }
  bool result =
      expect(applied && cardsReady && precedingMeasured && targetRestored &&
                 rootCard && commandCard && setFolded(commandCard, false),
             "the root-fold fixture materializes an expanded child");
  QPointer<CommandOutputView> output =
      commandCard ? dynamic_cast<CommandOutputView *>(
                        commandCard->findChild<QTextEdit *>(
                            QStringLiteral("commandOutputView")))
                  : nullptr;
  if (!rootCard || !commandCard || !output)
    return false;
  if (commandDisclosure)
    commandDisclosure->clearFocus();
  if (output->verticalScrollBar()->maximum() > 0)
    output->verticalScrollBar()->setValue(
        output->verticalScrollBar()->maximum() / 2);
  dispatchPasses(64);
  const auto childStateBefore = commandCard->commandOutputScrollState();
  const int expandedExtent = view.verticalScrollBar()->maximum();
  result &= expect(setFolded(rootCard, true),
                   "the authoritative root folds its child activity");
  dispatchPasses(64);
  const int foldedExtent = view.verticalScrollBar()->maximum();
  const bool childSuppressed =
      !commandCard || !commandCard->isVisibleTo(view.viewport());
  result &= expect(
      foldedExtent < expandedExtent && childSuppressed,
      "a folded root removes child geometry from the effective scroll extent");

  result &= expect(setFolded(rootCard, false),
                   "the authoritative root expands after suppression");
  const bool childRestored = dispatchUntil([&] {
    return card(view, stableKey(command.key)) != nullptr &&
           view.verticalScrollBar()->maximum() == expandedExtent;
  });
  commandCard = card(view, stableKey(command.key));
  output = commandCard ? dynamic_cast<CommandOutputView *>(
                             commandCard->findChild<QTextEdit *>(
                                 QStringLiteral("commandOutputView")))
                       : nullptr;
  const auto childStateAfter =
      commandCard ? commandCard->commandOutputScrollState() : std::nullopt;
  result &= expect(
      childRestored && commandCard && output && !commandCard->isCollapsed() &&
          childStateBefore && childStateAfter &&
          childStateAfter->value == childStateBefore->value &&
          childStateAfter->followsLatest == childStateBefore->followsLatest &&
          view.verticalScrollBar()->maximum() == expandedExtent,
      "expanding a root restores the exact extent and child interaction state");
  return result;
}

bool testLargeGraphResetUsesBoundedRetiredCleanup() {
  constexpr std::size_t ItemCount = 640;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", ItemCount);
    thread =
        write.upsert({nodegraph::NodeKind::Thread, "retired-cleanup-thread"},
                     std::move(threadState));
    nodegraph::NodeRef turn =
        write.upsert({nodegraph::NodeKind::Turn, "retired-cleanup-turn"});
    write.setParent(thread, turn);
    for (std::size_t index = 0; index < ItemCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          "agentMessage", "Retired geometry " + std::to_string(index));
      state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item =
          write.upsert({nodegraph::NodeKind::Item,
                        "retired-cleanup-item-" + std::to_string(index)},
                       std::move(state));
      write.setParent(turn, item);
    }
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 360);
  view.show();
  view.bindGraph(graph, thread);
  QPushButton *loadMore = historyButton(view);
  bool result = expect(loadMore && loadMore->isVisible(),
                       "the cleanup fixture exposes retained history paging");
  if (!loadMore)
    return false;
  for (std::size_t page = AuthoritativeHistoryPageSize; page < ItemCount;
       page += AuthoritativeHistoryPageSize)
    loadMore->click();
  const bool retained = dispatchUntil([&] {
    return view.property("graphStructureScanComplete").toBool() &&
           view.property("graphRetainedGeometryRecordCount").toULongLong() >=
               ItemCount;
  });
  result &= expect(retained, "the cleanup fixture retains its large geometry");

  nodegraph::GraphChange removal;
  {
    auto write = graph.write();
    write.remove(thread);
    removal = write.finish();
  }
  view.graphChanged(removal.removed);
  const QVariant retiredCount =
      view.property("graphRetiredGeometryRecordCount");
  const QVariant lastCleanup =
      view.property("graphLastRetiredCleanupOperations");
  const QVariant maxCleanup = view.property("graphMaxRetiredCleanupOperations");
  result &= expect(
      retiredCount.isValid() && lastCleanup.isValid() && maxCleanup.isValid() &&
          retiredCount.toULongLong() >= ItemCount &&
          lastCleanup.toULongLong() <= 64 && maxCleanup.toULongLong() <= 64 &&
          view.property("graphRetainedGeometryRecordCount").toULongLong() == 0,
      "large selected-thread removal retires geometry immediately without an "
      "unbounded cleanup pass");

  const bool cleanupFinished = dispatchUntil(
      [&] {
        return view.property("graphRetiredGeometryRecordCount").toULongLong() ==
               0;
      },
      128);
  result &= expect(
      cleanupFinished &&
          view.property("graphLastRetiredCleanupOperations").toULongLong() <=
              64 &&
          view.property("graphMaxRetiredCleanupOperations").toULongLong() <= 64,
      "retired geometry drains completely in fixed-size Qt cleanup slices");
  return result;
}

bool testDeferredRefreshSurvivesRetirementAcknowledgement() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", 1);
    thread = write.upsert(
        {nodegraph::NodeKind::Thread, "deferred-retirement-thread"},
        std::move(threadState));
    nodegraph::NodeRef turn = write.upsert(
        {nodegraph::NodeKind::Turn, "deferred-retirement-turn"});
    nodegraph::NodeRef item = write.upsert(
        {nodegraph::NodeKind::Item, "deferred-retirement-item"},
        graphMessageState("agentMessage", "Visible before retirement"));
    write.setParent(thread, turn);
    write.setParent(turn, item);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 360);
  view.show();
  view.bindGraph(graph, thread);
  bool result = expect(
      dispatchUntil([&] {
        return card(view,
                    stableKey(AuthoritativeItemKey{
                        "deferred-retirement-thread",
                        "deferred-retirement-turn",
                        "deferred-retirement-item"})) != nullptr;
      }),
      "the deferred-retirement fixture materializes its selected thread");

  nodegraph::GraphChange removal;
  {
    auto write = graph.write();
    write.remove(thread);
    removal = write.finish();
  }
  // FrontendSession acknowledges detached UI nodes immediately after the
  // GraphChanged callback, before this view's deferred refresh executes.
  view.graphChangedDeferred(removal.affected, removal.removed);
  {
    auto write = graph.write();
    write.releaseRetired(removal.removed);
    static_cast<void>(write.finish());
  }
  dispatchPasses(8);
  result &= expect(
      card(view,
           stableKey(AuthoritativeItemKey{"deferred-retirement-thread",
                                          "deferred-retirement-turn",
                                          "deferred-retirement-item"})) ==
          nullptr,
      "a deferred Qt pass safely discards purged NodeRefs after detachment");
  return result;
}

bool testAffectedNodeRequeuedAfterCursorConsumption() {
  constexpr int ItemCount = 64;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef target;
  std::vector<nodegraph::NodeRef> fillers;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", ItemCount);
    thread =
        write.upsert({nodegraph::NodeKind::Thread, "affected-cursor-thread"},
                     std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "affected-cursor-turn"});
    write.setParent(thread, turn);
    fillers.reserve(ItemCount - 1);
    for (int index = 0; index < ItemCount - 1; ++index) {
      nodegraph::NodeState state = graphMessageState(
          "agentMessage", "Affected filler " + std::to_string(index));
      state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item =
          write.upsert({nodegraph::NodeKind::Item,
                        "affected-cursor-filler-" + std::to_string(index)},
                       std::move(state));
      write.setParent(turn, item);
      fillers.push_back(std::move(item));
    }
    nodegraph::NodeState hidden;
    hidden.status = nodegraph::NodeStatus::Completed;
    hidden.fields.emplace("type", "reasoning");
    hidden.fields.emplace("summary", "Initially filtered target");
    target = write.upsert({nodegraph::NodeKind::Item, "affected-cursor-target"},
                          std::move(hidden));
    write.setParent(turn, target);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 360);
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.showReasoning = false;
  view.setPresentationOptions(options);
  view.show();
  view.bindGraph(graph, thread);
  const bool initialScanComplete = dispatchUntil([&] {
    return view.property("graphStructureScanComplete").toBool() &&
           view.property("graphRetainedGeometryRecordCount").toULongLong() >=
               ItemCount;
  });

  std::vector<nodegraph::NodeRef> affected;
  affected.reserve(ItemCount);
  affected.push_back(target);
  affected.insert(affected.end(), fillers.begin(), fillers.end());
  view.graphChangedDeferred(affected, {});
  bool lateChangeSent = false;
  std::uint64_t lateRevision = 0;
  QTimer::singleShot(0, &view, [&] {
    nodegraph::GraphChange change;
    {
      auto write = graph.write();
      write.setField(target, "type", "agentMessage");
      write.setField(target, "phase", "final_answer");
      write.setField(target, "text", "Late change after cursor consumption");
      change = write.finish();
    }
    lateRevision = change.revision;
    lateChangeSent = true;
    view.graphChangedDeferred(change.affected, change.removed);
  });

  const bool lateProjectionRendered = dispatchUntil([&] {
    if (!lateChangeSent)
      return false;
    ui::QtNodeAttachment *attachment = graphAttachment(target);
    auto *widget =
        attachment ? qobject_cast<ConversationCard *>(attachment->widget.data())
                   : nullptr;
    const auto *message =
        widget ? std::get_if<AgentMessageData>(&widget->data().payload)
               : nullptr;
    return attachment && attachment->renderedRevision >= lateRevision &&
           message && message->text == "Late change after cursor consumption";
  });
  return expect(
      initialScanComplete && graphAttachment(target) && lateChangeSent &&
          lateProjectionRendered && graphPassBudgetsWereRespected(view),
      "an affected node changed after cursor consumption is requeued and "
      "rendered exactly from its latest graph state");
}

bool testGraphStreamTruncationNotices() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef agent;
  nodegraph::NodeRef command;
  nodegraph::NodeRef reasoning;
  nodegraph::NodeRef plan;
  const auto addRetention = [](nodegraph::NodeState &state, std::string field,
                               std::uint64_t omitted, std::uint64_t retained) {
    nodegraph::Value::Object entry{
        {"discardedBytes", nodegraph::Value(omitted)},
        {"retainedBytes", nodegraph::Value(retained)}};
    nodegraph::Value::Object retention;
    retention.emplace(std::move(field), nodegraph::Value(std::move(entry)));
    state.fields.emplace("textRetention",
                         nodegraph::Value(std::move(retention)));
  };
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{4});
    thread = write.upsert({nodegraph::NodeKind::Thread, "truncation-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "truncation-turn"});
    write.setParent(thread, turn);

    nodegraph::NodeState agentState =
        graphMessageState("agentMessage", "retained response");
    agentState.fields.emplace("phase", "final_answer");
    addRetention(agentState, "text", 123, 17);
    agent = write.upsert({nodegraph::NodeKind::Item, "truncation-agent"},
                         std::move(agentState));

    nodegraph::NodeState commandState;
    commandState.status = nodegraph::NodeStatus::Completed;
    commandState.fields = {{"type", "commandExecution"},
                           {"command", "printf retained"},
                           {"aggregatedOutput", "retained output"}};
    addRetention(commandState, "aggregatedOutput", 456, 15);
    command = write.upsert({nodegraph::NodeKind::Item, "truncation-command"},
                           std::move(commandState));

    nodegraph::NodeState reasoningState;
    reasoningState.status = nodegraph::NodeStatus::Completed;
    reasoningState.fields = {
        {"type", "reasoning"},
        {"summary",
         nodegraph::Value::Array{nodegraph::Value("retained reasoning")}}};
    addRetention(reasoningState, "summary", 789, 18);
    reasoning =
        write.upsert({nodegraph::NodeKind::Item, "truncation-reasoning"},
                     std::move(reasoningState));

    nodegraph::NodeState planState =
        graphMessageState("plan", "retained plan text");
    addRetention(planState, "text", 42, 18);
    plan = write.upsert({nodegraph::NodeKind::Item, "truncation-plan"},
                        std::move(planState));

    for (const nodegraph::NodeRef &item : {agent, command, reasoning, plan})
      write.setParent(turn, item);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(720, 1000);
  view.show();
  view.bindGraph(graph, thread);
  spin(160);

  const auto attachedCard = [](const nodegraph::NodeRef &node) {
    ui::QtNodeAttachment *attachment = graphAttachment(node);
    return attachment
               ? qobject_cast<ConversationCard *>(attachment->widget.data())
               : nullptr;
  };
  ConversationCard *agentCard = attachedCard(agent);
  ConversationCard *commandCard = attachedCard(command);
  ConversationCard *reasoningCard = attachedCard(reasoning);
  ConversationCard *planCard = attachedCard(plan);
  const auto *agentData =
      agentCard ? std::get_if<AgentMessageData>(&agentCard->data().payload)
                : nullptr;
  const auto *commandData =
      commandCard
          ? std::get_if<CommandExecutionData>(&commandCard->data().payload)
          : nullptr;
  const auto *reasoningData =
      reasoningCard ? std::get_if<ReasoningData>(&reasoningCard->data().payload)
                    : nullptr;
  const auto *planData =
      planCard ? std::get_if<PlanData>(&planCard->data().payload) : nullptr;
  return expect(
      agentData &&
          agentData->text.starts_with(
              "> Earlier Codex response was truncated (123 bytes omitted).") &&
          commandData &&
          commandData->output.starts_with(
              "[Earlier command output was truncated (456 "
              "bytes omitted).]") &&
          reasoningData &&
          reasoningData->summary.starts_with(
              "> Earlier reasoning was truncated (789 bytes "
              "omitted).") &&
          planData &&
          planData->legacyText.starts_with(
              "> Earlier plan text was truncated (42 bytes "
              "omitted)."),
      "visible graph cards disclose every bounded protocol text tail");
}

bool testGraphBoundedHistoryAndExplicitRoot() {
  constexpr std::size_t ItemCount = 5000;
  constexpr std::size_t RevealedPages = 2;
  constexpr std::size_t FirstRevealedIndex =
      ItemCount - (RevealedPages + 1) * AuthoritativeHistoryPageSize;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef root;
  nodegraph::NodeRef firstRevealed;
  nodegraph::NodeRef distant;
  nodegraph::NodeRef steering;
  nodegraph::NodeRef tail;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", ItemCount);
    threadState.fields.emplace("historyHasMore", true);
    threadState.fields.emplace("historyNextCursor", "older-page");
    thread = write.upsert({nodegraph::NodeKind::Thread, "bounded-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "bounded-turn"});
    write.setParent(thread, turn);
    for (std::size_t index = 0; index < ItemCount; ++index) {
      const bool isRoot = index == 0;
      const bool isSteering = index == ItemCount - 2;
      nodegraph::NodeState state = graphMessageState(
          isRoot || isSteering ? "userMessage" : "agentMessage",
          isRoot ? "Original prompt"
                 : (isSteering ? "Later steering prompt"
                               : "History " + std::to_string(index)));
      if (!isRoot && !isSteering)
        state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item, "bounded-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
      if (isRoot)
        root = item;
      if (index == FirstRevealedIndex)
        firstRevealed = item;
      if (index == 3100)
        distant = item;
      if (isSteering)
        steering = item;
      if (index + 1 == ItemCount)
        tail = item;
    }
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    // History suffix replacement may unparent the original prompt while its
    // explicit protocol relation remains authoritative.
    write.clearParent(root);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  int providerPageRequests = 0;
  view.setLoadMoreAction([&providerPageRequests] { ++providerPageRequests; });
  view.resize(620, 420);
  view.show();
  PaintAnchorProbe initialPaints(view);
  initialPaints.start();
  view.bindGraph(graph, thread);
  const bool selectionMaterializationFrozen =
      view.property("bulkMaterializationUpdatesSuppressed").toBool() &&
      !view.viewport()->updatesEnabled();
  const bool initialLoadedWindowReady = spinUntil([&] {
    const LiveConversationWidgetCounts widgets =
        liveConversationWidgetCounts(view);
    return view.property("graphLiveRecordCount").toULongLong() ==
               AuthoritativeHistoryPageSize + 1 &&
           widgets.cards ==
               static_cast<int>(AuthoritativeHistoryPageSize + 1) &&
           widgets.itemPlaceholders == 0 &&
           graphAttachment(root) && graphAttachment(steering) &&
           graphAttachment(tail) && view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 2048);
  initialPaints.active = false;
  const bool initializationPaintedOnlyCompleteWindow =
      std::ranges::all_of(initialPaints.representationCounts, [](int count) {
        return count ==
               static_cast<int>(AuthoritativeHistoryPageSize + 1);
      });

  const auto representationCount = [&view] {
    return static_cast<int>(std::ranges::count_if(
        view.findChildren<QWidget *>(), [](QWidget *widget) {
          return dynamic_cast<ConversationCard *>(widget) ||
                 widget->objectName() ==
                     QStringLiteral("conversationCardPlaceholder");
        }));
  };
  const auto hiddenItemCount = [&view] {
    qulonglong count = 0;
    for (QWidget *widget : view.findChildren<QWidget *>())
      if (widget->objectName() ==
          QStringLiteral("conversationHistoryPlaceholder"))
        count += widget->property("hiddenItemCount").toULongLong();
    return count;
  };
  QPushButton *loadMore = historyButton(view);
  const QVariant retainedGeometry =
      view.property("graphRetainedGeometryRecordCount");
  bool result = expect(
      initialLoadedWindowReady && selectionMaterializationFrozen &&
          initializationPaintedOnlyCompleteWindow &&
          view.viewport()->updatesEnabled() &&
          !view.property("bulkMaterializationUpdatesSuppressed").toBool() &&
          representationCount() ==
              static_cast<int>(AuthoritativeHistoryPageSize + 1) &&
          retainedGeometry.isValid() && loadMore &&
          loadMore->isVisible() &&
          loadMore->text() == QStringLiteral("Load 80 more activities") &&
          hiddenItemCount() == ItemCount - AuthoritativeHistoryPageSize - 1,
      "five thousand graph items retain compact graph geometry while thread "
      "selection materializes the loaded 80-card window plus its root once");

  int heartbeatCount = 0;
  qint64 longestHeartbeatGap = 0;
  QElapsedTimer heartbeatGap;
  heartbeatGap.start();
  QTimer heartbeat;
  heartbeat.setInterval(1);
  QObject::connect(&heartbeat, &QTimer::timeout, &view, [&] {
    longestHeartbeatGap = std::max(longestHeartbeatGap, heartbeatGap.restart());
    ++heartbeatCount;
  });
  heartbeat.start();
  spin(80);
  heartbeat.stop();
  result &= expect(heartbeatCount >= 5 && longestHeartbeatGap < 50,
                   "visibility continuations for five thousand graph items "
                   "preserve the Qt heartbeat");
  ui::QtNodeAttachment *steeringAttachment = graphAttachment(steering);
  auto *steeringCard =
      steeringAttachment
          ? qobject_cast<ConversationCard *>(steeringAttachment->widget.data())
          : nullptr;
  result &=
      expect(steeringCard && !steeringCard->property("turnContainer").toBool() &&
                 qobject_cast<ConversationCard *>(
                     graphAttachment(root)->widget.data())
                     ->isAncestorOf(steeringCard),
             "a loaded steering message remains a child of the canonical "
             "Turn/You card and is never inferred to be the root");

  const qulonglong hiddenBeforePaging = hiddenItemCount();
  bool everyPageMaterializedAtomically = true;
  for (std::size_t page = 0; page < RevealedPages; ++page) {
    PaintAnchorProbe pagePaints(view);
    pagePaints.start();
    loadMore->click();
    everyPageMaterializedAtomically =
        everyPageMaterializedAtomically &&
        view.property("bulkMaterializationUpdatesSuppressed").toBool() &&
        !view.viewport()->updatesEnabled();
    spinUntil([&] {
      const std::size_t expected =
          (page + 2) * AuthoritativeHistoryPageSize + 1;
      const LiveConversationWidgetCounts widgets =
          liveConversationWidgetCounts(view);
      return view.property("graphLiveRecordCount").toULongLong() == expected &&
             widgets.cards == static_cast<int>(expected) &&
             widgets.itemPlaceholders == 0 &&
             view.viewport()->updatesEnabled() &&
             !view.property("bulkMaterializationUpdatesSuppressed").toBool();
    }, 2048);
    pagePaints.active = false;
    const int expectedRepresentations = static_cast<int>(
        (page + 2) * AuthoritativeHistoryPageSize + 1);
    const bool pagePaintedOnlyCompleteWindow =
        std::ranges::all_of(pagePaints.representationCounts,
                            [expectedRepresentations](int count) {
                              return count == expectedRepresentations;
                            });
    everyPageMaterializedAtomically =
        everyPageMaterializedAtomically && view.viewport()->updatesEnabled() &&
        !view.property("bulkMaterializationUpdatesSuppressed").toBool() &&
        pagePaintedOnlyCompleteWindow;
    if (!pagePaintedOnlyCompleteWindow) {
      std::cerr << "load-more page " << page << " expected "
                << expectedRepresentations << " representations; paints:";
      for (const int count : pagePaints.representationCounts)
        std::cerr << ' ' << count;
      std::cerr << '\n';
    }
  }
  const QVariant retainedAfterPaging =
      view.property("graphRetainedGeometryRecordCount");
  result &= expect(
      providerPageRequests == 0 && everyPageMaterializedAtomically &&
          hiddenItemCount() + RevealedPages * AuthoritativeHistoryPageSize ==
              hiddenBeforePaging &&
          retainedAfterPaging.isValid() &&
          retainedAfterPaging.toULongLong() >=
              RevealedPages * AuthoritativeHistoryPageSize &&
          view.property("graphLiveRecordCount").toULongLong() ==
              (RevealedPages + 1) * AuthoritativeHistoryPageSize + 1 &&
          graphPassBudgetsWereRespected(view),
      "each Load 80 more action materializes that admitted batch exactly once "
      "while every construction pass remains budgeted");

  ui::QtNodeAttachment *tailAttachment = graphAttachment(tail);
  QPointer<QWidget> tailIdentity =
      tailAttachment ? tailAttachment->widget : nullptr;
  const LiveConversationWidgetCounts beforeVisibleDelta =
      liveConversationWidgetCounts(view);
  const qulonglong fullGeometryBeforeVisibleDelta =
      view.property("conversationFullGeometryPasses").toULongLong();
  nodegraph::GraphChange visibleDelta;
  {
    auto write = graph.write();
    write.setField(tail, "text", "Targeted visible revision after many pages");
    visibleDelta = write.finish();
  }
  view.graphChangedDeferred(visibleDelta.affected, visibleDelta.removed);
  spin(80);
  tailAttachment = graphAttachment(tail);
  auto *tailCard =
      tailAttachment
          ? qobject_cast<ConversationCard *>(tailAttachment->widget.data())
          : nullptr;
  const auto *tailMessage =
      tailCard ? std::get_if<AgentMessageData>(&tailCard->data().payload)
               : nullptr;
  result &= expect(
      tailIdentity && tailAttachment &&
          tailAttachment->widget == tailIdentity &&
          tailAttachment->renderedRevision == visibleDelta.revision &&
          tailMessage &&
          tailMessage->text == "Targeted visible revision after many pages" &&
          liveConversationWidgetCounts(view).itemRepresentations() ==
              beforeVisibleDelta.itemRepresentations() &&
          view.property("conversationFullGeometryPasses").toULongLong() ==
              fullGeometryBeforeVisibleDelta &&
          view.property("conversationSectionsLaidOutLastPass").toULongLong() ==
              1 &&
          graphRefreshWasConstantBounded(view) &&
          graphPassBudgetsWereRespected(view),
      "a visible targeted delta after many revealed pages updates the stable "
      "card inside only its TurnSection with constant bounded graph and "
      "QWidget work");

  const LiveConversationWidgetCounts beforeOffscreenDelta =
      liveConversationWidgetCounts(view);
  result &= expect(graphAttachment(distant) == nullptr,
                   "an item outside the loaded history window has no QWidget "
                   "attachment");
  nodegraph::GraphChange offscreenDelta;
  {
    auto write = graph.write();
    write.setField(distant, "text", "Off-screen revision stays in NodeGraph");
    offscreenDelta = write.finish();
  }
  view.graphChangedDeferred(offscreenDelta.affected, offscreenDelta.removed);
  spin(80);
  const LiveConversationWidgetCounts afterOffscreenDelta =
      liveConversationWidgetCounts(view);
  result &= expect(
      graphAttachment(distant) == nullptr &&
          afterOffscreenDelta.cards == beforeOffscreenDelta.cards &&
          afterOffscreenDelta.itemPlaceholders ==
              beforeOffscreenDelta.itemPlaceholders &&
          afterOffscreenDelta.turnSections ==
              beforeOffscreenDelta.turnSections &&
          graphRefreshWasConstantBounded(view) &&
          graphPassBudgetsWereRespected(view),
      "a delta outside the loaded history window creates no QWidget and keeps "
      "all work bounded");

  ui::QtNodeAttachment *firstAttachment = graphAttachment(firstRevealed);
  auto *firstCard =
      firstAttachment
          ? qobject_cast<ConversationCard *>(firstAttachment->widget.data())
          : nullptr;
  ui::QtNodeAttachment *rootAttachment = graphAttachment(root);
  auto *rootCard =
      rootAttachment
          ? qobject_cast<ConversationCard *>(rootAttachment->widget.data())
          : nullptr;
  result &= expect(
      rootCard && firstCard &&
          rootCard->property("turnContainer").toBool() &&
          rootCard->isAncestorOf(firstCard) &&
          firstCard->property("nestedConversationCard").toBool(),
      "every loaded child is materialized once under its actual canonical "
      "Turn/You card even when both are initially offscreen");

  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      firstCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(80);
  const int firstScrollValue = view.verticalScrollBar()->value();

  std::string tallText;
  for (int line = 0; line < 18; ++line)
    tallText += "Measured retained line " + std::to_string(line) + "\n";
  nodegraph::GraphChange tallChange;
  {
    auto write = graph.write();
    write.setField(firstRevealed, "text", tallText);
    tallChange = write.finish();
  }
  view.graphChangedDeferred(tallChange.affected, tallChange.removed);
  spin(100);
  firstAttachment = graphAttachment(firstRevealed);
  firstCard =
      firstAttachment
          ? qobject_cast<ConversationCard *>(firstAttachment->widget.data())
          : nullptr;
  QPointer<QWidget> retainedFirst = firstCard;
  const int retainedHeight = firstCard ? firstCard->height() : 0;
  const int retainedViewportTop =
      firstCard ? firstCard->mapTo(view.viewport(), QPoint{}).y() : 0;
  const int nestedViewportX =
      firstCard ? firstCard->mapTo(view.viewport(), QPoint{}).x() : -1;
  const int maximumWithMeasuredHeight = view.verticalScrollBar()->maximum();
  result &= expect(
      firstCard && retainedHeight > 66 &&
          firstAttachment->renderedRevision == tallChange.revision,
      "a visible tall record publishes and retains its measured final height");

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  spin(160);
  rootAttachment = graphAttachment(root);
  rootCard =
      rootAttachment
          ? qobject_cast<ConversationCard *>(rootAttachment->widget.data())
          : nullptr;
  result &= expect(
      rootCard && graphAttachment(firstRevealed) == firstAttachment &&
          retainedFirst == firstCard && rootCard->isAncestorOf(firstCard) &&
          std::abs(view.verticalScrollBar()->maximum() -
                   maximumWithMeasuredHeight) <= 2,
      "scrolling retains the loaded child, its canonical owner, and its "
      "measured scroll extent without reconstruction");
  steeringAttachment = graphAttachment(steering);
  steeringCard =
      steeringAttachment
          ? qobject_cast<ConversationCard *>(steeringAttachment->widget.data())
          : nullptr;
  result &= expect(
      rootCard && steeringCard && rootCard->isAncestorOf(steeringCard) &&
          steeringCard->property("nestedConversationCard").toBool() &&
          steeringCard->mapTo(view.viewport(), QPoint{}).x() ==
              nestedViewportX && retainedFirst == firstCard,
      "a retained child remains owned by its canonical Turn/You card and "
      "identically indented after scrolling");

  view.verticalScrollBar()->setValue(firstScrollValue);
  spin(160);
  firstAttachment = graphAttachment(firstRevealed);
  firstCard =
      firstAttachment
          ? qobject_cast<ConversationCard *>(firstAttachment->widget.data())
          : nullptr;
  rootAttachment = graphAttachment(root);
  rootCard =
      rootAttachment
          ? qobject_cast<ConversationCard *>(rootAttachment->widget.data())
          : nullptr;
  result &= expect(
      rootCard && firstCard && retainedFirst == firstCard &&
          rootCard->isAncestorOf(firstCard) &&
          firstCard->property("nestedConversationCard").toBool() &&
          firstCard->height() == retainedHeight &&
          std::abs(firstCard->mapTo(view.viewport(), QPoint{}).y() -
                   retainedViewportTop) <= 2 &&
          liveConversationWidgetCounts(view).cards ==
              static_cast<int>((RevealedPages + 1) *
                                   AuthoritativeHistoryPageSize +
                               1) &&
          graphPassBudgetsWereRespected(view),
      "returning to a retained child preserves identity, canonical ownership, "
      "nested presentation, measured height, and exact scroll anchor");
  return result;
}

bool testLoadedCardsMaterializeOnceWithoutScrollChurn() {
  constexpr std::size_t LoadedCount = 80;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  std::vector<nodegraph::NodeRef> items;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", LoadedCount);
    thread = write.upsert({nodegraph::NodeKind::Thread, "once-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "once-turn"});
    write.setParent(thread, turn);
    items.reserve(LoadedCount);
    for (std::size_t index = 0; index < LoadedCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          index == 0 ? "userMessage" : "agentMessage",
          "Retained card " + std::to_string(index));
      if (index != 0)
        state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "once-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
      items.push_back(item);
    }
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, items.front());
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 420);
  view.show();
  view.bindGraph(graph, thread);
  const bool allMaterialized = spinUntil(
      [&items, &view] {
        return std::ranges::all_of(
                   items, [](const nodegraph::NodeRef &item) {
                     ui::QtNodeAttachment *attachment = graphAttachment(item);
                     return attachment && attachment->widget;
                   }) &&
               view.viewport()->updatesEnabled() &&
               !view.property("bulkMaterializationUpdatesSuppressed").toBool();
      },
      1024);

  std::vector<QPointer<QWidget>> identities;
  identities.reserve(items.size());
  for (const nodegraph::NodeRef &item : items) {
    ui::QtNodeAttachment *attachment = graphAttachment(item);
    identities.push_back(attachment ? attachment->widget : nullptr);
  }
  const qulonglong geometryBefore =
      view.property("conversationGeometryPasses").toULongLong();
  const int maximumBefore = view.verticalScrollBar()->maximum();
  for (int pass = 0; pass < 4; ++pass) {
    view.verticalScrollBar()->setValue(0);
    spin(10);
    view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
    spin(10);
  }

  bool sameCards = allMaterialized;
  for (std::size_t index = 0; index < items.size(); ++index) {
    ui::QtNodeAttachment *attachment = graphAttachment(items[index]);
    sameCards = sameCards && attachment && attachment->widget == identities[index];
  }
  ConversationCard *rootCard =
      qobject_cast<ConversationCard *>(identities.front().data());
  bool owned = rootCard && rootCard->property("turnContainer").toBool();
  for (std::size_t index = 1; owned && index < identities.size(); ++index)
    owned = identities[index] && rootCard->isAncestorOf(identities[index]);
  if (!(allMaterialized && sameCards && owned &&
        view.verticalScrollBar()->maximum() == maximumBefore &&
        view.property("conversationGeometryPasses").toULongLong() ==
            geometryBefore))
    std::cerr << "loaded-window churn: materialized=" << allMaterialized
              << " same=" << sameCards << " owned=" << owned
              << " maximum=" << maximumBefore << "->"
              << view.verticalScrollBar()->maximum() << " geometry="
              << geometryBefore << "->"
              << view.property("conversationGeometryPasses").toULongLong()
              << " frozen="
              << view.property("bulkMaterializationUpdatesSuppressed").toBool()
              << " blocker="
              << view.property("bulkMaterializationBlocker")
                     .toString()
                     .toStdString()
              << '\n';
  return expect(
      allMaterialized && sameCards && owned &&
          view.verticalScrollBar()->maximum() == maximumBefore &&
          view.property("conversationGeometryPasses").toULongLong() ==
              geometryBefore,
      "the selected 80-card loaded window materializes once, retains exact "
      "Turn/You ownership, and performs no reconstruction or relayout while "
      "scrolling");
}

bool testDelayedInitialHistoryMaterializesAtomically() {
  constexpr std::size_t LoadedCount = AuthoritativeHistoryPageSize;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{0});
    threadState.fields.emplace("hydrationState", "loading");
    thread = write.upsert(
        {nodegraph::NodeKind::Thread, "delayed-initial-history"},
        std::move(threadState));
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 420);
  view.show();
  spin(20);
  PaintAnchorProbe paints(view);
  paints.start();
  view.bindGraph(graph, thread);
  const bool emptyBindingHeld = spinUntil([&] {
    return !view.viewport()->updatesEnabled() &&
           view.property("bulkMaterializationUpdatesSuppressed").toBool() &&
           view.property("bulkMaterializationBlocker").toString() ==
               QStringLiteral("hydration");
  });
  auto *loadingCover = view.findChild<QLabel *>(
      QStringLiteral("conversationAtomicTransitionOverlay"));
  const bool loadingCoverVisible =
      loadingCover && loadingCover->isVisible() &&
      loadingCover->pixmap().isNull() &&
      loadingCover->text() == QStringLiteral("Loading conversation…");

  std::vector<nodegraph::NodeRef> items;
  nodegraph::GraphChange hydrated;
  {
    auto write = graph.write();
    const nodegraph::NodeRef turn =
        write.upsert({nodegraph::NodeKind::Turn, "delayed-initial-turn"});
    write.setParent(thread, turn);
    items.reserve(LoadedCount);
    for (std::size_t index = 0; index < LoadedCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          index == 0 ? "userMessage" : "agentMessage",
          "Hydrated retained card " + std::to_string(index));
      if (index != 0)
        state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "delayed-initial-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
      items.push_back(item);
    }
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, items.front());
    write.setField(thread, "historyLoadedItemCount", LoadedCount);
    write.setField(thread, "hydrationState", "ready");
    hydrated = write.finish();
  }
  view.graphChangedDeferred(hydrated.affected, hydrated.removed);
  const bool hydratedWindowReady = spinUntil(
      [&] {
        return std::ranges::all_of(
                   items, [](const nodegraph::NodeRef &item) {
                     const auto *attachment = graphAttachment(item);
                     return attachment && attachment->widget;
                   }) &&
               view.viewport()->updatesEnabled() &&
               !view.property("bulkMaterializationUpdatesSuppressed")
                    .toBool() &&
               loadingCover && !loadingCover->isVisible();
      },
      1024);
  paints.active = false;
  const bool noPartialHistoryFrame =
      std::ranges::all_of(paints.representationCounts, [](int count) {
        return count == 0 || count == static_cast<int>(LoadedCount);
      }) &&
      std::ranges::find(paints.representationCounts,
                        static_cast<int>(LoadedCount)) !=
          paints.representationCounts.end();

  auto *rootCard = items.empty() || !graphAttachment(items.front())
                       ? nullptr
                       : qobject_cast<ConversationCard *>(
                             graphAttachment(items.front())->widget.data());
  QWidget *nested = rootCard
                        ? rootCard->findChild<QWidget *>(
                              QStringLiteral("conversationNestedCards"),
                              Qt::FindDirectChildrenOnly)
                        : nullptr;
  const int rootHeight = rootCard ? rootCard->height() : -1;
  const int nestedHeight = nested ? nested->height() : -1;
  const int scrollMaximum = view.verticalScrollBar()->maximum();
  const qulonglong geometryPasses =
      view.property("conversationGeometryPasses").toULongLong();
  spin(80);
  const bool finalLayoutStable =
      rootCard && nested && rootCard->property("turnContainer").toBool() &&
      rootCard->height() == rootHeight && nested->height() == nestedHeight &&
      view.verticalScrollBar()->maximum() == scrollMaximum &&
      view.property("conversationGeometryPasses").toULongLong() ==
          geometryPasses;

  return expect(
      emptyBindingHeld && loadingCoverVisible && hydratedWindowReady &&
          noPartialHistoryFrame && finalLayoutStable,
      "history arriving after an empty selection remains invisible until all "
      "retained cards have their stable final old-UI layout");
}

bool testPartialLiveTailWaitsForAuthoritativeInitialHistory() {
  constexpr std::size_t LoadedCount = AuthoritativeHistoryPageSize;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  std::vector<nodegraph::NodeRef> items;
  {
    auto write = graph.write();
    thread = write.upsert(
        {nodegraph::NodeKind::Thread, "partial-live-tail-thread"});
    turn =
        write.upsert({nodegraph::NodeKind::Turn, "partial-live-tail-turn"});
    write.setParent(thread, turn);
    items.reserve(LoadedCount);
    for (std::size_t index = 0; index < 2; ++index) {
      nodegraph::NodeState state = graphMessageState(
          index == 0 ? "userMessage" : "agentMessage",
          "Partial live tail " + std::to_string(index));
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "partial-live-tail-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
      items.push_back(item);
    }
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, items.front());
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 420);
  view.show();
  spin(20);
  PaintAnchorProbe paints(view);
  paints.start();
  view.bindGraph(graph, thread);
  const bool partialTailHeld = spinUntil([&] {
    return !view.viewport()->updatesEnabled() &&
           view.property("bulkMaterializationUpdatesSuppressed").toBool() &&
           view.property("bulkMaterializationBlocker").toString() ==
               QStringLiteral("hydration");
  });

  nodegraph::GraphChange loading;
  {
    auto write = graph.write();
    write.setField(thread, "hydrationState", "loading");
    loading = write.finish();
  }
  view.graphChangedDeferred(loading.affected, loading.removed);
  spin(20);

  nodegraph::GraphChange hydrated;
  {
    auto write = graph.write();
    for (std::size_t index = items.size(); index < LoadedCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          "agentMessage", "Hydrated retained card " + std::to_string(index));
      state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "partial-live-tail-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
      items.push_back(item);
    }
    write.setField(thread, "historyLoadedItemCount", LoadedCount);
    write.setField(thread, "hydrationState", "ready");
    hydrated = write.finish();
  }
  view.graphChangedDeferred(hydrated.affected, hydrated.removed);
  const bool hydratedWindowReady = spinUntil(
      [&] {
        return std::ranges::all_of(
                   items, [](const nodegraph::NodeRef &item) {
                     const auto *attachment = graphAttachment(item);
                     return attachment && attachment->widget;
                   }) &&
               view.viewport()->updatesEnabled() &&
               !view.property("bulkMaterializationUpdatesSuppressed")
                    .toBool();
      },
      1024);
  paints.active = false;

  const bool noPartialTailFrame =
      std::ranges::all_of(paints.representationCounts, [](int count) {
        return count == 0 || count == static_cast<int>(LoadedCount);
      }) &&
      std::ranges::find(paints.representationCounts,
                        static_cast<int>(LoadedCount)) !=
          paints.representationCounts.end();
  auto *rootCard = graphAttachment(items.front())
                       ? qobject_cast<ConversationCard *>(
                             graphAttachment(items.front())->widget.data())
                       : nullptr;
  bool allOwned = rootCard && rootCard->property("turnContainer").toBool();
  for (std::size_t index = 1; allOwned && index < items.size(); ++index) {
    const auto *attachment = graphAttachment(items[index]);
    allOwned = attachment && attachment->widget &&
               rootCard->isAncestorOf(attachment->widget);
  }

  return expect(
      partialTailHeld && hydratedWindowReady && noPartialTailFrame && allOwned,
      "a thread-list live tail never paints before authoritative history and "
      "the first exposed frame has the complete canonically owned window");
}

bool testThreadSwitchCoversOldFrameUntilAtomicCommit() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef sourceThread;
  nodegraph::NodeRef sourceItem;
  nodegraph::NodeRef targetThread;
  {
    auto write = graph.write();
    nodegraph::NodeState ready;
    ready.fields.emplace("historyLoadedItemCount", std::uint64_t{1});
    ready.fields.emplace("hydrationState", "ready");
    sourceThread = write.upsert(
        {nodegraph::NodeKind::Thread, "atomic-frame-source"},
        std::move(ready));
    const auto sourceTurn =
        write.upsert({nodegraph::NodeKind::Turn, "atomic-frame-source-turn"});
    sourceItem = write.upsert(
        {nodegraph::NodeKind::Item, "atomic-frame-source-item"},
        graphMessageState("userMessage", "Previous painted conversation"));
    write.setParent(sourceThread, sourceTurn);
    write.setParent(sourceTurn, sourceItem);
    write.relate(sourceTurn, nodegraph::RelationKind::TurnRootItem,
                 sourceItem);

    nodegraph::NodeState loading;
    loading.fields.emplace("historyLoadedItemCount", std::uint64_t{0});
    loading.fields.emplace("hydrationState", "loading");
    targetThread = write.upsert(
        {nodegraph::NodeKind::Thread, "atomic-frame-target"},
        std::move(loading));
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 420);
  view.show();
  view.bindGraph(graph, sourceThread);
  const bool sourceReady = spinUntil([&] {
    const auto *attachment = graphAttachment(sourceItem);
    return attachment && attachment->widget && view.viewport()->updatesEnabled();
  });
  view.bindGraph(graph, targetThread);
  const bool targetHeld = spinUntil([&] {
    return !view.viewport()->updatesEnabled() &&
           view.property("bulkMaterializationBlocker").toString() ==
               QStringLiteral("hydration");
  });
  auto *overlay = view.findChild<QLabel *>(
      QStringLiteral("conversationAtomicTransitionOverlay"));
  const bool oldFrameCovered =
      overlay && overlay->isVisible() && overlay->pixmap().isNull() &&
      overlay->text() == QStringLiteral("Loading conversation…") &&
      graphAttachment(sourceItem) == nullptr;

  nodegraph::NodeRef targetItem;
  nodegraph::GraphChange hydrated;
  {
    auto write = graph.write();
    const auto targetTurn =
        write.upsert({nodegraph::NodeKind::Turn, "atomic-frame-target-turn"});
    targetItem = write.upsert(
        {nodegraph::NodeKind::Item, "atomic-frame-target-item"},
        graphMessageState("userMessage", "Complete incoming conversation"));
    write.setParent(targetThread, targetTurn);
    write.setParent(targetTurn, targetItem);
    write.relate(targetTurn, nodegraph::RelationKind::TurnRootItem,
                 targetItem);
    write.setField(targetThread, "historyLoadedItemCount", std::uint64_t{1});
    write.setField(targetThread, "hydrationState", "ready");
    hydrated = write.finish();
  }
  view.graphChangedDeferred(hydrated.affected, hydrated.removed);
  const bool targetReady = spinUntil([&] {
    const auto *attachment = graphAttachment(targetItem);
    return attachment && attachment->widget && view.viewport()->updatesEnabled() &&
           overlay && !overlay->isVisible();
  });

  return expect(sourceReady && targetHeld && oldFrameCovered && targetReady,
                "thread switching covers the outgoing conversation until the "
                "incoming history can be exposed in one atomic commit");
}

bool testPausedIncomingCardMaterializesWithoutAnchorJump() {
  constexpr std::size_t InitialCount = 40;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef root;
  std::vector<nodegraph::NodeRef> initial;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", InitialCount);
    thread = write.upsert({nodegraph::NodeKind::Thread, "paused-append-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "paused-append-turn"});
    write.setParent(thread, turn);
    for (std::size_t index = 0; index < InitialCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          index == 0 ? "userMessage" : "agentMessage",
          "Initial retained card " + std::to_string(index));
      if (index != 0)
        state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "paused-append-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
      initial.push_back(item);
    }
    root = initial.front();
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 420);
  view.show();
  view.bindGraph(graph, thread);
  const bool initialReady = spinUntil([&] {
    return std::ranges::all_of(initial, [](const nodegraph::NodeRef &item) {
      ui::QtNodeAttachment *attachment = graphAttachment(item);
      return attachment && attachment->widget;
    }) && view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 512);
  view.verticalScrollBar()->setValue(0);
  spin(40);
  const auto anchorBefore = firstVisible(view);
  const int scrollBefore = view.verticalScrollBar()->value();
  std::vector<QPointer<QWidget>> identities;
  identities.reserve(initial.size());
  for (const nodegraph::NodeRef &item : initial) {
    ui::QtNodeAttachment *attachment = graphAttachment(item);
    identities.push_back(attachment ? attachment->widget : nullptr);
  }

  nodegraph::NodeRef incoming;
  nodegraph::GraphChange change;
  const qulonglong atomicAttemptsBefore =
      view.property("bulkMaterializationCommitAttempts").toULongLong();
  {
    auto write = graph.write();
    nodegraph::NodeState state =
        graphMessageState("agentMessage", "Incoming while reading above");
    state.fields.emplace("phase", "update");
    incoming = write.upsert(
        {nodegraph::NodeKind::Item, "paused-append-incoming"},
        std::move(state));
    write.setParent(turn, incoming);
    write.setField(thread, "historyLoadedItemCount", InitialCount + 1);
    change = write.finish();
  }
  view.graphChangedDeferred(change.affected, change.removed);
  const bool incomingReady = spinUntil([&] {
    ui::QtNodeAttachment *attachment = graphAttachment(incoming);
    return attachment && attachment->widget &&
           view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 32);
  const auto anchorAfter = firstVisible(view);
  ui::QtNodeAttachment *rootAttachment = graphAttachment(root);
  ui::QtNodeAttachment *incomingAttachment = graphAttachment(incoming);
  auto *rootCard = rootAttachment
                       ? qobject_cast<ConversationCard *>(
                             rootAttachment->widget.data())
                       : nullptr;
  QWidget *incomingCard =
      incomingAttachment ? incomingAttachment->widget.data() : nullptr;
  bool oldIdentitiesRetained = true;
  for (std::size_t index = 0; index < initial.size(); ++index)
    oldIdentitiesRetained =
        oldIdentitiesRetained && graphAttachment(initial[index]) &&
        graphAttachment(initial[index])->widget == identities[index];

  return expect(
      initialReady && !anchorBefore.first.empty() && incomingReady &&
          view.property("bulkMaterializationCommitAttempts").toULongLong() >
              atomicAttemptsBefore &&
          view.viewport()->updatesEnabled() &&
          !view.property("bulkMaterializationUpdatesSuppressed").toBool() &&
          view.mode() == ConversationView::Mode::Paused &&
          view.verticalScrollBar()->value() == scrollBefore &&
          anchorAfter.first == anchorBefore.first &&
          std::abs(anchorAfter.second - anchorBefore.second) <= 1 &&
          oldIdentitiesRetained && rootCard && incomingCard &&
          rootCard->isAncestorOf(incomingCard) &&
          incomingCard->mapTo(view.viewport(), QPoint{}).y() >=
              view.viewport()->height() &&
          graphPassBudgetsWereRespected(view),
      "a selected thread materializes one new offscreen card promptly while "
      "a paused viewport retains its exact anchor and existing card identity");
}

bool testPausedMixedCardBurstKeepsLeafAnchorAndParents() {
  constexpr std::size_t InitialCount = 40;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef root;
  std::vector<nodegraph::NodeRef> initial;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", InitialCount);
    thread = write.upsert(
        {nodegraph::NodeKind::Thread, "mixed-card-anchor-thread"},
        std::move(threadState));
    turn = write.upsert(
        {nodegraph::NodeKind::Turn, "mixed-card-anchor-turn"});
    write.setParent(thread, turn);
    initial.reserve(InitialCount);
    for (std::size_t index = 0; index < InitialCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          index == 0 ? "userMessage" : "agentMessage",
          "Retained mixed-card history " + std::to_string(index));
      if (index != 0)
        state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "mixed-card-anchor-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(turn, item);
      initial.push_back(item);
    }
    root = initial.front();
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 420);
  view.show();
  view.bindGraph(graph, thread);
  const bool initialReady = spinUntil([&] {
    return std::ranges::all_of(initial, [](const nodegraph::NodeRef &item) {
      const auto *attachment = graphAttachment(item);
      return attachment && attachment->widget;
    }) && view.viewport()->updatesEnabled();
  }, 512);
  QWidget *anchorWidget =
      initialReady ? graphAttachment(initial[12])->widget.data() : nullptr;
  if (anchorWidget)
    view.verticalScrollBar()->setValue(std::clamp(
        anchorWidget->mapTo(view.viewport(), QPoint{}).y() +
            view.verticalScrollBar()->value() - 24,
        view.verticalScrollBar()->minimum(),
        view.verticalScrollBar()->maximum()));
  spin(24);
  const int anchorTopBefore =
      anchorWidget ? anchorWidget->mapTo(view.viewport(), QPoint{}).y() : 0;
  const int scrollBefore = view.verticalScrollBar()->value();
  QPointer<QWidget> rootIdentity =
      graphAttachment(root) ? graphAttachment(root)->widget : nullptr;
  PaintAnchorProbe paintProbe(view);
  paintProbe.start(anchorWidget);

  std::vector<nodegraph::NodeRef> incoming;
  nodegraph::NodeRef review;
  nodegraph::GraphChange burst;
  {
    auto write = graph.write();
    std::vector<nodegraph::NodeState> states;
    states.push_back(graphMessageState("userMessage", "Steering user card"));
    nodegraph::NodeState agent =
        graphMessageState("agentMessage", "Agent response card");
    agent.fields.emplace("phase", "commentary");
    states.push_back(std::move(agent));
    nodegraph::NodeState command;
    command.status = nodegraph::NodeStatus::Running;
    command.fields = {{"type", "commandExecution"},
                      {"command", "printf mixed-card"},
                      {"aggregatedOutput", "one line"},
                      {"status", "inProgress"}};
    states.push_back(std::move(command));
    nodegraph::NodeState activity;
    activity.status = nodegraph::NodeStatus::Running;
    activity.fields = {{"type", "collabAgentToolCall"},
                       {"tool", "spawn_agent"},
                       {"status", "inProgress"},
                       {"prompt", "mixed-card agent activity"}};
    states.push_back(std::move(activity));
    nodegraph::NodeState reasoning;
    reasoning.status = nodegraph::NodeStatus::Running;
    reasoning.fields = {{"type", "reasoning"},
                        {"summary", "mixed-card reasoning"}};
    states.push_back(std::move(reasoning));
    nodegraph::NodeState fileChange;
    fileChange.status = nodegraph::NodeStatus::Running;
    fileChange.fields = {
        {"type", "fileChange"},
        {"status", "inProgress"},
        {"changes",
         nodegraph::Value::Array{nodegraph::Value(nodegraph::Value::Object{
             {"path", "src/mixed.cpp"}, {"kind", "update"}})}}};
    states.push_back(std::move(fileChange));
    nodegraph::NodeState image;
    image.status = nodegraph::NodeStatus::Running;
    image.fields = {{"type", "imageGeneration"},
                    {"status", "inProgress"},
                    {"revisedPrompt", "mixed-card image"}};
    states.push_back(std::move(image));
    nodegraph::NodeState plan;
    plan.status = nodegraph::NodeStatus::Running;
    plan.fields = {{"type", "plan"}, {"text", "mixed-card plan"}};
    states.push_back(std::move(plan));
    nodegraph::NodeState autoReview;
    autoReview.status = nodegraph::NodeStatus::Running;
    autoReview.fields = {{"type", "autoApprovalReview"},
                         {"phase", "started"},
                         {"detail", "mixed-card approval review"}};
    states.push_back(std::move(autoReview));
    nodegraph::NodeState generic;
    generic.status = nodegraph::NodeStatus::Running;
    generic.fields = {{"type", "contextCompaction"},
                      {"detail", "mixed-card generic activity"}};
    states.push_back(std::move(generic));
    nodegraph::NodeState local;
    local.status = nodegraph::NodeStatus::Running;
    local.fields = {{"type", "localPrompt"},
                    {"submissionId", std::uint64_t{4100}},
                    {"text", "Optimistic steering card"},
                    {"dispatchState", "inFlight"},
                    {"showPendingAnimation", true},
                    {"startsTurn", false}};
    states.push_back(std::move(local));

    incoming.reserve(states.size());
    for (std::size_t index = 0; index < states.size(); ++index) {
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "mixed-card-incoming-" + std::to_string(index)},
          std::move(states[index]));
      write.setParent(turn, item);
      incoming.push_back(item);
    }
    review = incoming[8];
    write.setField(thread, "historyLoadedItemCount",
                   InitialCount + incoming.size());
    burst = write.finish();
  }
  view.graphChangedDeferred(burst.affected, burst.removed);
  const bool burstReady = spinUntil([&] {
    return std::ranges::all_of(incoming, [](const nodegraph::NodeRef &item) {
      const auto *attachment = graphAttachment(item);
      return attachment && attachment->widget;
    }) && view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 256);
  paintProbe.active = false;

  bool allNested = rootIdentity;
  for (const nodegraph::NodeRef &item : incoming)
    allNested = allNested && graphAttachment(item) &&
                rootIdentity->isAncestorOf(graphAttachment(item)->widget);
  auto *runningCommandCard =
      graphAttachment(incoming[2])
          ? qobject_cast<ConversationCard *>(
                graphAttachment(incoming[2])->widget.data())
          : nullptr;
  const bool everyDelayedWorkCardEmphasized = std::ranges::all_of(
      incoming.begin() + 2, incoming.begin() + 10,
      [](const nodegraph::NodeRef &item) {
        const auto *attachment = graphAttachment(item);
        const auto *card =
            attachment
                ? qobject_cast<ConversationCard *>(attachment->widget.data())
                : nullptr;
        return card && card->property("activeWork").toBool();
      });
  const int anchorTopAfter =
      anchorWidget ? anchorWidget->mapTo(view.viewport(), QPoint{}).y() : 0;
  const bool paintedStable = std::ranges::all_of(
      paintProbe.trackedGeometries, [anchorTopBefore](const QRect &geometry) {
        return geometry.top() == anchorTopBefore;
      });

  // Completion of the same approval-review node is a state/geometry change,
  // not another structural row. It must retain identity and the paused view.
  QPointer<QWidget> reviewIdentity =
      graphAttachment(review) ? graphAttachment(review)->widget : nullptr;
  nodegraph::GraphChange completed;
  {
    auto write = graph.write();
    write.setField(review, "phase", "completed");
    write.setField(review, "detail",
                   std::string(1200, 'r') + " completed review");
    write.setStatus(review, nodegraph::NodeStatus::Completed);
    completed = write.finish();
  }
  view.graphChangedDeferred(completed.affected, completed.removed);
  spin(64);

  const bool pausedViewportContract =
      initialReady && anchorWidget && burstReady && allNested &&
          runningCommandCard &&
          runningCommandCard->property("activeWork").toBool() &&
          everyDelayedWorkCardEmphasized &&
          rootIdentity == graphAttachment(root)->widget &&
          reviewIdentity && graphAttachment(review) &&
          graphAttachment(review)->widget == reviewIdentity &&
          view.mode() == ConversationView::Mode::Paused &&
          view.verticalScrollBar()->value() == scrollBefore &&
          anchorTopAfter == anchorTopBefore && paintedStable &&
          anchorWidget->mapTo(view.viewport(), QPoint{}).y() ==
              anchorTopBefore &&
          graphPassBudgetsWereRespected(view);
  if (reviewIdentity)
    view.verticalScrollBar()->setValue(std::clamp(
        view.verticalScrollBar()->value() +
            reviewIdentity->mapTo(view.viewport(), QPoint{}).y() - 24,
        view.verticalScrollBar()->minimum(),
        view.verticalScrollBar()->maximum()));
  const bool terminalReviewSettled = spinUntil([&] {
    return reviewIdentity &&
           !reviewIdentity->property("activeWork").toBool();
  });

  return expect(
      pausedViewportContract && terminalReviewSettled,
      "a coalesced burst covering every conversation card kind, including "
      "approval review and optimistic steering, materializes atomically under "
      "the canonical You parent without moving a scrolled-up leaf anchor, "
      "and every delayed-work border follows canonical lifecycle state");
}

bool testPausedNormalPromptTurnMaterializesWithoutAnchorJump() {
  constexpr std::size_t InitialCount = 40;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef oldTurn;
  std::vector<nodegraph::NodeRef> initial;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", InitialCount);
    thread = write.upsert(
        {nodegraph::NodeKind::Thread, "paused-new-prompt-thread"},
        std::move(threadState));
    oldTurn =
        write.upsert({nodegraph::NodeKind::Turn, "paused-new-prompt-old-turn"});
    write.setParent(thread, oldTurn);
    for (std::size_t index = 0; index < InitialCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          index == 0 ? "userMessage" : "agentMessage",
          "Retained history " + std::to_string(index));
      if (index != 0)
        state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "paused-new-prompt-item-" + std::to_string(index)},
          std::move(state));
      write.setParent(oldTurn, item);
      initial.push_back(item);
    }
    write.relate(oldTurn, nodegraph::RelationKind::TurnRootItem,
                 initial.front());
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 420);
  view.show();
  view.bindGraph(graph, thread);
  const bool initialReady = spinUntil([&] {
    return std::ranges::all_of(initial, [](const nodegraph::NodeRef &item) {
      const ui::QtNodeAttachment *attachment = graphAttachment(item);
      return attachment && attachment->widget;
    }) && view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 512);
  view.verticalScrollBar()->setValue(
      std::min(600, view.verticalScrollBar()->maximum() / 2));
  spin(40);
  const auto anchorBefore = firstVisible(view);
  const int verticalBefore = view.verticalScrollBar()->value();
  const int horizontalBefore = view.horizontalScrollBar()->value();
  std::vector<QPointer<QWidget>> identities;
  identities.reserve(initial.size());
  for (const nodegraph::NodeRef &item : initial)
    identities.push_back(graphAttachment(item)->widget);
  PaintAnchorProbe paints(view);
  paints.start();
  const qulonglong atomicAttemptsBefore =
      view.property("bulkMaterializationCommitAttempts").toULongLong();

  nodegraph::NodeRef prompt;
  nodegraph::GraphChange appended;
  {
    auto write = graph.write();
    nodegraph::NodeState turnState;
    turnState.status = nodegraph::NodeStatus::Pending;
    turnState.fields.emplace("type", "localTurn");
    turnState.fields.emplace("local", true);
    nodegraph::NodeRef turn = write.upsert(
        {nodegraph::NodeKind::Turn, "paused-new-prompt-current-turn"},
        std::move(turnState));
    nodegraph::NodeState promptState;
    promptState.status = nodegraph::NodeStatus::Pending;
    promptState.fields.emplace("type", "localPrompt");
    promptState.fields.emplace("submissionId", std::uint64_t{9001});
    promptState.fields.emplace("text", "A normal new prompt");
    promptState.fields.emplace("dispatchState", "inFlight");
    promptState.fields.emplace("startsTurn", true);
    prompt = write.upsert(
        {nodegraph::NodeKind::Item, "paused-new-prompt-local"},
        std::move(promptState));
    write.setParent(thread, turn);
    write.setParent(turn, prompt);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, prompt);
    write.setField(thread, "historyLoadedItemCount", InitialCount + 1);
    appended = write.finish();
  }
  view.graphChangedDeferred(appended.affected, appended.removed);
  const bool promptReady = spinUntil([&] {
    const ui::QtNodeAttachment *attachment = graphAttachment(prompt);
    return attachment && attachment->widget &&
           view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 64);
  spin(40);
  paints.active = false;
  const auto anchorAfter = firstVisible(view);
  auto *promptCard =
      graphAttachment(prompt)
          ? qobject_cast<ConversationCard *>(
                graphAttachment(prompt)->widget.data())
          : nullptr;
  auto *promptStatus =
      promptCard
          ? promptCard->findChild<QLabel *>(
                QStringLiteral("pendingPromptStatus"))
          : nullptr;
  const bool paintedStable = std::ranges::all_of(
      paints.anchors, [&anchorBefore](const auto &anchor) {
        return anchor.first.empty() ||
               (anchor.first == anchorBefore.first &&
                std::abs(anchor.second - anchorBefore.second) <= 1);
      });
  bool retained = true;
  for (std::size_t index = 0; index < initial.size(); ++index)
    retained = retained && graphAttachment(initial[index]) &&
               graphAttachment(initial[index])->widget == identities[index];

  if (!(initialReady && !anchorBefore.first.empty() && promptReady &&
        retained &&
        view.mode() == ConversationView::Mode::Paused &&
        view.verticalScrollBar()->value() == verticalBefore &&
        view.horizontalScrollBar()->value() == horizontalBefore &&
        anchorAfter.first == anchorBefore.first &&
        std::abs(anchorAfter.second - anchorBefore.second) <= 1 &&
        paintedStable && graphPassBudgetsWereRespected(view)))
    std::cerr << "normal prompt anchor: before=" << anchorBefore.first << ':'
              << anchorBefore.second << " after=" << anchorAfter.first << ':'
              << anchorAfter.second << " scroll=" << verticalBefore << "->"
              << view.verticalScrollBar()->value() << " horizontal="
              << horizontalBefore << "->"
              << view.horizontalScrollBar()->value() << " mode="
              << static_cast<int>(view.mode()) << " ready=" << promptReady
              << " retained=" << retained << " paints=" << paints.anchors.size()
              << " stable=" << paintedStable << " frozen="
              << view.property("bulkMaterializationUpdatesSuppressed").toBool()
              << " updates=" << view.viewport()->updatesEnabled()
              << " retainedGeometry="
              << view.property("graphRetainedGeometryRecordCount").toULongLong()
              << " target="
              << view.property("graphStructureScanTarget").toULongLong()
              << " live="
              << view.property("graphLiveRecordCount").toULongLong()
              << " cards=" << liveConversationWidgetCounts(view).cards
              << " placeholders="
              << liveConversationWidgetCounts(view).itemPlaceholders
              << " attempts="
              << view.property("bulkMaterializationCommitAttempts").toULongLong()
              << " blocker="
              << view.property("bulkMaterializationBlocker")
                     .toString()
                     .toStdString()
              << '\n';

  return expect(
      initialReady && !anchorBefore.first.empty() && promptReady && retained &&
          promptCard &&
          promptCard->property("authoritativeTurnActive").toBool() &&
          promptStatus && promptStatus->text() == QStringLiteral("pending") &&
          view.property("bulkMaterializationCommitAttempts").toULongLong() >
              atomicAttemptsBefore &&
          paints.anchors.size() <= 1 && view.viewport()->updatesEnabled() &&
          !view.property("bulkMaterializationUpdatesSuppressed").toBool() &&
          view.mode() == ConversationView::Mode::Paused &&
          view.verticalScrollBar()->value() == verticalBefore &&
          view.horizontalScrollBar()->value() == horizontalBefore &&
          anchorAfter.first == anchorBefore.first &&
          std::abs(anchorAfter.second - anchorBefore.second) <= 1 &&
          paintedStable && graphPassBudgetsWereRespected(view),
      "a normal prompt appends and immediately materializes its active, "
      "emphasized Turn/You card without moving or repaint-jumping a paused "
      "history viewport");
}

bool testGraphHistoryPagingAndPausedTailGrowth() {
  constexpr std::size_t InitialItemCount = 100;
  constexpr std::size_t PrependedItemCount = 5;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef currentTurn;
  nodegraph::NodeRef root;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", InitialItemCount);
    threadState.fields.emplace("historyHasMore", true);
    threadState.fields.emplace("historyNextCursor", "older-page");
    thread = write.upsert({nodegraph::NodeKind::Thread, "paging-thread"},
                          std::move(threadState));
    currentTurn =
        write.upsert({nodegraph::NodeKind::Turn, "paging-current-turn"});
    write.setParent(thread, currentTurn);
    for (std::size_t index = 0; index < InitialItemCount; ++index) {
      nodegraph::NodeState state = graphMessageState(
          index == 0 ? "userMessage" : "agentMessage",
          index == 0 ? "Opening prompt"
                     : "Current history " + std::to_string(index));
      if (index != 0)
        state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item =
          write.upsert({nodegraph::NodeKind::Item,
                        "paging-current-item-" + std::to_string(index)},
                       std::move(state));
      write.setParent(currentTurn, item);
      if (index == 0)
        root = item;
    }
    write.relate(currentTurn, nodegraph::RelationKind::TurnRootItem, root);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  int providerPageRequests = 0;
  view.setLoadMoreAction([&providerPageRequests] { ++providerPageRequests; });
  view.resize(620, 420);
  view.show();
  view.bindGraph(graph, thread);
  spin(80);

  const auto hiddenItemCount = [&view] {
    qulonglong count = 0;
    for (QWidget *widget : view.findChildren<QWidget *>())
      if (widget->objectName() ==
          QStringLiteral("conversationHistoryPlaceholder"))
        count += widget->property("hiddenItemCount").toULongLong();
    return count;
  };

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  spin(80);
  const auto anchorBeforePrepend = firstVisible(view);
  const qulonglong hiddenBeforePrepend = hiddenItemCount();
  bool result = expect(view.mode() == ConversationView::Mode::Paused &&
                           !anchorBeforePrepend.first.empty(),
                       "the retained graph history can be paused at a stable "
                       "anchor");

  nodegraph::GraphChange prepended;
  {
    auto write = graph.write();
    nodegraph::NodeRef olderTurn =
        write.upsert({nodegraph::NodeKind::Turn, "paging-older-turn"});
    for (std::size_t index = 0; index < PrependedItemCount; ++index) {
      nodegraph::NodeState state =
          graphMessageState(index == 0 ? "userMessage" : "agentMessage",
                            "Older history " + std::to_string(index));
      if (index != 0)
        state.fields.emplace("phase", "final_answer");
      nodegraph::NodeRef item =
          write.upsert({nodegraph::NodeKind::Item,
                        "paging-older-item-" + std::to_string(index)},
                       std::move(state));
      write.setParent(olderTurn, item);
    }
    std::vector<nodegraph::NodeRef> turns{olderTurn, currentTurn};
    write.replaceChildren(thread, turns);
    write.setField(thread, "historyLoadedItemCount",
                   InitialItemCount + PrependedItemCount);
    prepended = write.finish();
  }
  view.graphChanged(prepended.removed);
  spin(100);
  const auto anchorAfterPrepend = firstVisible(view);
  const qulonglong hiddenAfterPrepend = hiddenItemCount();
  result &= expect(
      hiddenAfterPrepend == hiddenBeforePrepend + PrependedItemCount &&
          anchorAfterPrepend.first == anchorBeforePrepend.first &&
          std::abs(anchorAfterPrepend.second - anchorBeforePrepend.second) <= 1,
      "an older provider page stays above the paused requested window without "
      "double-expanding it");

  nodegraph::GraphChange appended;
  {
    auto write = graph.write();
    nodegraph::NodeState state =
        graphMessageState("agentMessage", "New activity at the tail");
    state.fields.emplace("phase", "final_answer");
    nodegraph::NodeRef item = write.upsert(
        {nodegraph::NodeKind::Item, "paging-newest-item"}, std::move(state));
    write.setParent(currentTurn, item);
    write.setField(thread, "historyLoadedItemCount",
                   InitialItemCount + PrependedItemCount + 1);
    appended = write.finish();
  }
  view.graphChanged(appended.removed);
  spin(100);
  const auto anchorAfterAppend = firstVisible(view);
  result &= expect(
      hiddenItemCount() == hiddenAfterPrepend &&
          anchorAfterAppend.first == anchorBeforePrepend.first &&
          std::abs(anchorAfterAppend.second - anchorBeforePrepend.second) <= 1,
      "a true paused tail append temporarily expands the effective window and "
      "preserves its painted anchor");

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  const bool requestedWindowRestored = spinUntil([&] {
    return view.mode() == ConversationView::Mode::Following &&
           hiddenItemCount() == hiddenAfterPrepend + 1;
  });
  if (!requestedWindowRestored)
    std::cerr << "history resume: mode=" << static_cast<int>(view.mode())
              << " hidden=" << hiddenItemCount() << " expected="
              << hiddenAfterPrepend + 1 << " live="
              << view.property("graphLiveRecordCount").toULongLong()
              << " target="
              << view.property("graphStructureScanTarget").toULongLong()
              << " retained="
              << view.property("graphRetainedGeometryRecordCount")
                     .toULongLong()
              << " scroll=" << view.verticalScrollBar()->value() << '/'
              << view.verticalScrollBar()->maximum()
              << " frozen="
              << view.property("bulkMaterializationUpdatesSuppressed").toBool()
              << '\n';
  result &= expect(
      requestedWindowRestored &&
          view.mode() == ConversationView::Mode::Following &&
          hiddenItemCount() == hiddenAfterPrepend + 1,
      "resuming following restores the requested history bound after its "
      "temporary paused expansion");

  QPushButton *loadMore = historyButton(view);
  if (loadMore)
    loadMore->click();
  result &= expect(
      loadMore && providerPageRequests == 1,
      "the final retained page requests the provider continuation exactly "
      "once");
  return result;
}

bool testGraphRootReplacementAndAttachmentRecovery() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef root;
  nodegraph::NodeRef sibling;
  nodegraph::NodeRef steering;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{3});
    thread = write.upsert({nodegraph::NodeKind::Thread, "replacement-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "replacement-turn"});
    write.setParent(thread, turn);
    root = write.upsert({nodegraph::NodeKind::Item, "replacement-root"},
                        graphMessageState("userMessage", "Prompt"));
    sibling = write.upsert({nodegraph::NodeKind::Item, "replacement-sibling"},
                           graphMessageState("agentMessage", "Answer"));
    steering = write.upsert({nodegraph::NodeKind::Item, "replacement-steering"},
                            graphMessageState("userMessage", "Steer"));
    write.setParent(turn, root);
    write.setParent(turn, sibling);
    write.setParent(turn, steering);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 720);
  view.show();
  view.bindGraph(graph, thread);
  spin(100);
  ui::QtNodeAttachment *rootAttachment = graphAttachment(root);
  ui::QtNodeAttachment *siblingAttachment = graphAttachment(sibling);
  ui::QtNodeAttachment *steeringAttachment = graphAttachment(steering);
  QPointer<QWidget> oldRoot = rootAttachment ? rootAttachment->widget : nullptr;
  QPointer<QWidget> siblingIdentity =
      siblingAttachment ? siblingAttachment->widget : nullptr;
  QPointer<QWidget> steeringIdentity =
      steeringAttachment ? steeringAttachment->widget : nullptr;
  bool result =
      expect(oldRoot && siblingIdentity && steeringIdentity &&
                 oldRoot->isAncestorOf(siblingIdentity) &&
                 oldRoot->isAncestorOf(steeringIdentity),
             "the materialized root initially owns its nested turn cards");

  nodegraph::GraphChange replacementChange;
  {
    auto write = graph.write();
    nodegraph::NodeState replacement =
        graphMessageState("agentMessage", "Replacement root");
    replacement.fields.emplace("phase", "final_answer");
    write.replaceState(root, std::move(replacement));
    replacementChange = write.finish();
  }
  view.graphChanged(replacementChange.removed);
  spin(100);
  rootAttachment = graphAttachment(root);
  auto *replacementRoot =
      rootAttachment
          ? qobject_cast<ConversationCard *>(rootAttachment->widget.data())
          : nullptr;
  result &= expect(
      oldRoot.isNull() && replacementRoot && siblingIdentity &&
          steeringIdentity && graphAttachment(sibling) == siblingAttachment &&
          graphAttachment(steering) == steeringAttachment &&
          replacementRoot->isAncestorOf(siblingIdentity) &&
          replacementRoot->isAncestorOf(steeringIdentity),
      "replacing a root card detaches nested widgets before deleting their "
      "former QObject owner");

  nodegraph::GraphChange recoveryChange;
  {
    auto write = graph.write();
    write.setField(sibling, "text", "Recovered current revision");
    recoveryChange = write.finish();
  }
  delete siblingIdentity.data();
  result &= expect(siblingIdentity.isNull() && sibling->uiAttachment(),
                   "external QObject deletion leaves a detectable stale "
                   "opaque attachment");
  view.graphChanged(recoveryChange.removed);
  spin(100);
  siblingAttachment = graphAttachment(sibling);
  auto *recoveredSibling =
      siblingAttachment
          ? qobject_cast<ConversationCard *>(siblingAttachment->widget.data())
          : nullptr;
  const auto *recoveredMessage =
      recoveredSibling
          ? std::get_if<AgentMessageData>(&recoveredSibling->data().payload)
          : nullptr;
  result &= expect(
      recoveredSibling && recoveredMessage &&
          recoveredMessage->text == "Recovered current revision" &&
          replacementRoot->isAncestorOf(recoveredSibling) &&
          siblingAttachment->renderedRevision == recoveryChange.revision,
      "a null external QPointer clears the stale attachment and rematerializes "
      "the latest node revision");
  return result;
}

bool testGraphLastItemRemovalUpdatesChromeSynchronously() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef item;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{1});
    thread = write.upsert({nodegraph::NodeKind::Thread, "removal-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "removal-turn"});
    item = write.upsert({nodegraph::NodeKind::Item, "removal-item"},
                        graphMessageState("agentMessage", "Only item"));
    write.setParent(thread, turn);
    write.setParent(turn, item);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 360);
  view.show();
  view.bindGraph(graph, thread);
  spin(60);
  ui::QtNodeAttachment *attachment = graphAttachment(item);
  QPointer<QWidget> removedWidget = attachment ? attachment->widget : nullptr;
  QPushButton *loadMore = historyButton(view);
  QLabel *empty = conversationEmptyLabel(view);
  bool result = expect(removedWidget && loadMore && !loadMore->isVisible() &&
                           empty && !empty->isVisible(),
                       "one graph item hides the empty state");

  nodegraph::GraphChange removal;
  {
    auto write = graph.write();
    write.setField(thread, "historyLoadedItemCount", std::uint64_t{0});
    write.remove(item);
    removal = write.finish();
  }
  view.graphChanged(removal.removed);
  result &= expect(
      item->uiAttachment() == nullptr && removedWidget.isNull() &&
          !loadMore->isVisible() && empty->isVisible(),
      "removing the last graph item synchronously deletes its widget and "
      "recomputes empty and Load More chrome");
  spin(20);
  return result;
}

bool testGraphLocalPromptMorphsWithoutReplacingItsWidget() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef localPrompt;
  nodegraph::NodeRef activity;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{2});
    thread = write.upsert({nodegraph::NodeKind::Thread, "prompt-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "prompt-turn"});
    nodegraph::NodeState promptState;
    promptState.status = nodegraph::NodeStatus::Pending;
    promptState.fields.emplace("type", "localPrompt");
    promptState.fields.emplace("submissionId", std::uint64_t{42});
    promptState.fields.emplace("text", "Exact authored prompt");
    promptState.fields.emplace("dispatchState", "inFlight");
    promptState.fields.emplace(
        "attachments",
        nodegraph::Value::Array{nodegraph::Value(
            nodegraph::Value::Object{{"path", "/tmp/prompt.png"},
                                     {"displayName", "prompt.png"},
                                     {"mimeType", "image/png"}})});
    localPrompt = write.upsert({nodegraph::NodeKind::Item, "local-prompt:42"},
                               std::move(promptState));
    activity = write.upsert(
        {nodegraph::NodeKind::Item, "prompt-activity"},
        graphMessageState("agentMessage", "Nested turn activity"));
    write.setParent(thread, turn);
    write.setParent(turn, localPrompt);
    write.setParent(turn, activity);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, localPrompt);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 360);
  view.show();
  std::vector<nodegraph::NodeRef> acknowledgements;
  bool callbackGraphWriteCompleted = false;
  view.setPromptMaterializedAction([&acknowledgements,
                                    &callbackGraphWriteCompleted, &graph,
                                    &thread](nodegraph::NodeRef prompt) {
    // This deliberately takes the exclusive graph lock synchronously. If
    // ConversationView crosses the callback boundary with a read guard,
    // this test deadlocks instead of masking the lock-order defect.
    auto callbackWrite = graph.write();
    callbackWrite.setField(thread, "materializationCallbackObserved", true);
    static_cast<void>(callbackWrite.finish());
    callbackGraphWriteCompleted = true;
    acknowledgements.emplace_back(std::move(prompt));
    return true;
  });
  view.bindGraph(graph, thread);
  spin(60);

  ui::QtNodeAttachment *localAttachment = graphAttachment(localPrompt);
  auto *localCard =
      localAttachment
          ? qobject_cast<ConversationCard *>(localAttachment->widget.data())
          : nullptr;
  QPointer<ConversationCard> stableCard(localCard);
  ui::QtNodeAttachment *activityAttachment = graphAttachment(activity);
  auto *activityCard = activityAttachment
                           ? qobject_cast<ConversationCard *>(
                                 activityAttachment->widget.data())
                           : nullptr;
  bool result = expect(
      localCard && localCard->data().kind == CardKind::LocalPrompt &&
          localCard->property("turnContainer").toBool() && activityCard &&
          localCard->isAncestorOf(activityCard) &&
          std::get<LocalPromptData>(localCard->data().payload).prompt ==
              "Exact authored prompt" &&
          std::get<LocalPromptData>(localCard->data().payload).imagePaths ==
              std::vector<std::string>{"/tmp/prompt.png"},
      "a starting graph prompt is the owning You card for its nested turn "
      "activity while rendering exact authored content");
  const qulonglong retiredBefore =
      view.property("graphRetiredGeometryRecordCount").toULongLong();
  PaintAnchorProbe ownershipProbe(view);
  ownershipProbe.start(localCard);
  ownershipProbe.trackOwnership(localCard, activityCard);

  nodegraph::NodeRef authoritative;
  nodegraph::GraphChange materialized;
  {
    auto write = graph.write();
    authoritative =
        write.upsert({nodegraph::NodeKind::Item, "provider-user-message"},
                     graphMessageState("userMessage", "Exact authored prompt"));
    write.setParent(turn, authoritative);
    write.relate(authoritative, nodegraph::RelationKind::PromptMaterialization,
                 localPrompt);
    write.replaceRelated(turn, nodegraph::RelationKind::TurnRootItem,
                         std::array<nodegraph::NodeRef, 1>{authoritative});
    write.setField(thread, "historyLoadedItemCount", std::uint64_t{3});
    materialized = write.finish();
  }
  view.graphChangedDeferred(materialized.affected, materialized.removed);
  spin(80);

  result &= expect(
      graphAttachment(localPrompt) == localAttachment &&
          graphAttachment(authoritative) == nullptr && stableCard &&
          stableCard->data().kind == CardKind::LocalPrompt &&
          acknowledgements.empty(),
      "a correlated authoritative item stays hidden behind the one stable "
      "local card until exact prompt acknowledgement");

  nodegraph::GraphChange failed;
  {
    auto write = graph.write();
    write.setField(localPrompt, "dispatchState", "failed");
    write.setField(localPrompt, "error", "result failed");
    write.setStatus(localPrompt, nodegraph::NodeStatus::Failed);
    failed = write.finish();
  }
  view.graphChangedDeferred(failed.affected, failed.removed);
  spin(40);
  result &=
      expect(stableCard && graphAttachment(authoritative) == nullptr &&
                 stableCard->data().kind == CardKind::LocalPrompt &&
                 std::get<LocalPromptData>(stableCard->data().payload).state ==
                     PromptState::Failed,
             "a failed exact result keeps one recoverable authored card and no "
             "correlated duplicate");

  nodegraph::GraphChange acknowledged;
  {
    auto write = graph.write();
    write.setField(localPrompt, "dispatchState", "awaitingMaterialization");
    write.setStatus(localPrompt, nodegraph::NodeStatus::Running);
    acknowledged = write.finish();
  }
  view.graphChangedDeferred(acknowledged.affected, acknowledged.removed);
  spin(80);
  ownershipProbe.active = false;

  ui::QtNodeAttachment *authoritativeAttachment =
      graphAttachment(authoritative);
  auto *authoritativeCard = authoritativeAttachment
                                ? qobject_cast<ConversationCard *>(
                                      authoritativeAttachment->widget.data())
                                : nullptr;
  result &= expect(
      localPrompt->uiAttachment() == nullptr && stableCard &&
          authoritativeCard == stableCard &&
          authoritativeCard->data().kind == CardKind::UserMessage &&
          std::get<UserMessageData>(authoritativeCard->data().payload).text ==
              "Exact authored prompt" &&
          authoritativeCard->property("conversationAnchorKey").toString() ==
              QStringLiteral("prompt:42") &&
          callbackGraphWriteCompleted &&
          acknowledgements == std::vector<nodegraph::NodeRef>{localPrompt},
      "correlation plus exact result acknowledgement runs after releasing "
      "the graph read guard, then transfers and morphs the same You card");
  result &= expect(
      authoritativeCard &&
          authoritativeCard->property("turnContainer").toBool() &&
          activityCard && authoritativeCard->isAncestorOf(activityCard),
      "authoritative root transfer keeps nested activity owned by the same "
      "morphed You card");
  result &= expect(
      view.property("graphRetiredGeometryRecordCount").toULongLong() ==
              retiredBefore &&
          graphAttachment(activity) == activityAttachment && activityCard &&
          !ownershipProbe.ownership.empty() &&
          std::ranges::all_of(ownershipProbe.ownership, std::identity{}),
      "normal prompt root promotion retains existing history geometry and "
      "widgets, and no painted frame exposes a parentless nested card");

  nodegraph::GraphChange removed;
  {
    auto write = graph.write();
    write.remove(localPrompt);
    write.setField(thread, "historyLoadedItemCount", std::uint64_t{2});
    removed = write.finish();
  }
  view.graphChangedDeferred(removed.affected, removed.removed);
  spin(30);
  result &= expect(stableCard && graphAttachment(authoritative) &&
                       graphAttachment(authoritative)->widget == stableCard,
                   "retiring the acknowledged local node preserves the "
                   "authoritative card attachment");
  return result;
}

bool testGraphSteeringPromptAcknowledgementKeepsCanonicalParent() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef root;
  nodegraph::NodeRef steering;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{2});
    thread = write.upsert({nodegraph::NodeKind::Thread, "steering-ack-thread"},
                          std::move(threadState));
    turn = write.upsert({nodegraph::NodeKind::Turn, "steering-ack-turn"});
    root = write.upsert({nodegraph::NodeKind::Item, "steering-ack-root"},
                        graphMessageState("userMessage", "Opening prompt"));
    nodegraph::NodeState steeringState;
    steeringState.status = nodegraph::NodeStatus::Running;
    steeringState.fields.emplace("type", "localPrompt");
    steeringState.fields.emplace("submissionId", std::uint64_t{77});
    steeringState.fields.emplace("text", "A steering prompt");
    steeringState.fields.emplace("dispatchState", "inFlight");
    steeringState.fields.emplace("startsTurn", false);
    steering = write.upsert(
        {nodegraph::NodeKind::Item, "steering-ack-local"},
        std::move(steeringState));
    write.setParent(thread, turn);
    write.setParent(turn, root);
    write.setParent(turn, steering);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(620, 420);
  view.show();
  std::vector<nodegraph::NodeRef> acknowledgements;
  view.setPromptMaterializedAction(
      [&acknowledgements](nodegraph::NodeRef prompt) {
        acknowledgements.push_back(std::move(prompt));
        return true;
      });
  view.bindGraph(graph, thread);
  const bool initialReady = spinUntil([&] {
    const auto *rootAttachment = graphAttachment(root);
    const auto *steeringAttachment = graphAttachment(steering);
    return rootAttachment && rootAttachment->widget && steeringAttachment &&
           steeringAttachment->widget && view.viewport()->updatesEnabled();
  }, 128);
  auto *rootCard = graphAttachment(root)
                       ? qobject_cast<ConversationCard *>(
                             graphAttachment(root)->widget.data())
                       : nullptr;
  auto *steeringCard = graphAttachment(steering)
                           ? qobject_cast<ConversationCard *>(
                                 graphAttachment(steering)->widget.data())
                           : nullptr;
  QPointer<ConversationCard> stableSteering(steeringCard);
  PaintAnchorProbe ownership(view);
  ownership.start(steeringCard);
  ownership.trackOwnership(rootCard, steeringCard);

  nodegraph::NodeRef authoritative;
  nodegraph::GraphChange arrived;
  {
    auto write = graph.write();
    authoritative = write.upsert(
        {nodegraph::NodeKind::Item, "steering-ack-authoritative"},
        graphMessageState("userMessage", "A steering prompt"));
    write.setParent(turn, authoritative);
    write.relate(authoritative,
                 nodegraph::RelationKind::PromptMaterialization, steering);
    write.setField(thread, "historyLoadedItemCount", std::uint64_t{3});
    arrived = write.finish();
  }
  view.graphChangedDeferred(arrived.affected, arrived.removed);
  spin(32);
  const bool retainedBeforeResult =
      stableSteering && graphAttachment(steering) &&
      graphAttachment(steering)->widget == stableSteering &&
      graphAttachment(authoritative) == nullptr && rootCard &&
      rootCard->isAncestorOf(stableSteering);

  // The app-server may emit other canonical items before the exact steer
  // request result. Coalescing more than one tail append must not be mistaken
  // for a provider reorder or rebuild the retained Turn/You hierarchy.
  nodegraph::NodeRef review;
  nodegraph::NodeRef progress;
  nodegraph::GraphChange interleaved;
  const qulonglong retiredBeforeInterleave =
      view.property("graphRetiredGeometryRecordCount").toULongLong();
  {
    auto write = graph.write();
    nodegraph::NodeState reviewState;
    reviewState.status = nodegraph::NodeStatus::Running;
    reviewState.fields.emplace("type", "autoApprovalReview");
    reviewState.fields.emplace("phase", "started");
    reviewState.fields.emplace("detail", "Reviewing the requested action");
    review = write.upsert(
        {nodegraph::NodeKind::Item, "steering-ack-review"},
        std::move(reviewState));
    nodegraph::NodeState progressState =
        graphMessageState("agentMessage", "Interleaved progress");
    progressState.fields.emplace("phase", "commentary");
    progress = write.upsert(
        {nodegraph::NodeKind::Item, "steering-ack-progress"},
        std::move(progressState));
    write.setParent(turn, review);
    write.setParent(turn, progress);
    write.setField(thread, "historyLoadedItemCount", std::uint64_t{5});
    interleaved = write.finish();
  }
  view.graphChangedDeferred(interleaved.affected, interleaved.removed);
  const bool interleavedReady = spinUntil([&] {
    const auto *reviewAttachment = graphAttachment(review);
    const auto *progressAttachment = graphAttachment(progress);
    return reviewAttachment && reviewAttachment->widget &&
           progressAttachment && progressAttachment->widget &&
           view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 128);
  const bool stableDuringInterleave =
      stableSteering && rootCard && rootCard->isAncestorOf(stableSteering) &&
      graphAttachment(steering) &&
      graphAttachment(steering)->widget == stableSteering &&
      graphAttachment(authoritative) == nullptr &&
      graphAttachment(review) &&
      rootCard->isAncestorOf(graphAttachment(review)->widget) &&
      graphAttachment(progress) &&
      rootCard->isAncestorOf(graphAttachment(progress)->widget) &&
      view.property("graphRetiredGeometryRecordCount").toULongLong() ==
          retiredBeforeInterleave;

  const qulonglong commitAttemptsBefore =
      view.property("bulkMaterializationCommitAttempts").toULongLong();
  nodegraph::GraphChange acknowledged;
  {
    auto write = graph.write();
    write.setField(steering, "dispatchState", "awaitingMaterialization");
    acknowledged = write.finish();
  }
  view.graphChangedDeferred(acknowledged.affected, acknowledged.removed);
  const bool transferred = spinUntil([&] {
    const auto *attachment = graphAttachment(authoritative);
    return attachment && attachment->widget == stableSteering &&
           view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  }, 128);
  spin(16);
  ownership.active = false;
  const auto *authoritativeAttachment = graphAttachment(authoritative);
  auto *authoritativeCard =
      authoritativeAttachment
          ? qobject_cast<ConversationCard *>(
                authoritativeAttachment->widget.data())
          : nullptr;

  return expect(
      initialReady && retainedBeforeResult && interleavedReady &&
          stableDuringInterleave && transferred && stableSteering &&
          authoritativeCard == stableSteering && rootCard &&
          rootCard->property("turnContainer").toBool() &&
          rootCard->isAncestorOf(stableSteering) &&
          stableSteering->property("nestedConversationCard").toBool() &&
          steering->uiAttachment() == nullptr &&
          acknowledgements == std::vector<nodegraph::NodeRef>{steering} &&
          view.property("bulkMaterializationCommitAttempts").toULongLong() >
              commitAttemptsBefore &&
          !ownership.ownership.empty() &&
          std::ranges::all_of(ownership.ownership, std::identity{}),
      "steering acknowledgement transfers one stable nested card atomically "
      "without any painted parentless frame");
}

bool testStructuredTurnPlanRemainsInspectorOnly() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{0});
    threadState.fields.emplace("hydrationState", "ready");
    thread = write.upsert({nodegraph::NodeKind::Thread, "plan-thread"},
                          std::move(threadState));
    nodegraph::NodeState planState;
    planState.status = nodegraph::NodeStatus::Running;
    planState.fields.emplace("planExplanation", "Current graph plan");
    nodegraph::Value::Array steps;
    nodegraph::Value::Object first;
    first.emplace("step", "Inspect graph");
    first.emplace("status", "completed");
    steps.emplace_back(std::move(first));
    nodegraph::Value::Object second;
    second.emplace("step", "Render plan");
    second.emplace("status", "inProgress");
    steps.emplace_back(std::move(second));
    planState.fields.emplace("plan", std::move(steps));
    turn = write.upsert({nodegraph::NodeKind::Turn, "plan-turn"},
                        std::move(planState));
    write.setParent(thread, turn);
    static_cast<void>(write.finish());
  }

  ConversationView view;
  view.resize(640, 400);
  view.show();
  view.bindGraph(graph, thread);
  spin();
  const std::string key =
      stableKey(CardKey{TurnPlanKey{"plan-thread", "plan-turn"}});
  QPointer<ConversationCard> planCard = card(view, key);
  bool result = expect(!planCard && graphAttachment(turn) == nullptr,
                       "structured turn plans remain Inspector-only and do "
                       "not create a duplicate conversation card");

  nodegraph::GraphChange cleared;
  {
    auto write = graph.write();
    nodegraph::NodeState completed;
    completed.status = nodegraph::NodeStatus::Completed;
    write.replaceState(turn, std::move(completed));
    cleared = write.finish();
  }
  view.graphChanged(cleared.removed);
  spin();
  result &= expect(!planCard && graphAttachment(turn) == nullptr &&
                       !hasConversationItem(view, key),
                   "clearing Inspector-only plan state leaves conversation "
                   "geometry unchanged");
  return result;
}

bool testOverduePromptStartsOnlyWhenVisible() {
  constexpr int promptCount = 32;
  const std::int64_t admittedAt = QDateTime::currentMSecsSinceEpoch() -
                                  PendingAnimationDelayMilliseconds - 250;
  ConversationGraphSpec snapshot;
  snapshot.threadId = "overdue-prompts";
  TurnGraphSpec turn{"turn:overdue-prompts", "turn", {}};
  turn.cards.reserve(promptCount);
  for (int index = 0; index < promptCount; ++index) {
    const std::uint64_t submission = 7000U + static_cast<std::uint64_t>(index);
    turn.cards.push_back(
        {LocalPromptKey{submission},
         CardKind::LocalPrompt,
         snapshot.threadId,
         turn.turnId,
         {},
         LocalPromptData{submission,
                         "retained overdue prompt " + std::to_string(index),
                         PromptState::InFlight,
                         false,
                         {},
                         {},
                         admittedAt}});
  }
  snapshot.sections.push_back(std::move(turn));

  ConversationView view;
  view.resize(620, 240);
  view.show();
  applyConversation(view, snapshot);

  std::vector<nodegraph::NodeRef> prompts;
  prompts.reserve(promptCount);
  {
    auto read = snapshot.storage->graph.tryRead();
    if (read) {
      for (int index = 0; index < promptCount; ++index) {
        prompts.push_back(read->find(
            {nodegraph::NodeKind::Item,
             "fixture-local-prompt:" + std::to_string(7000 + index)}));
      }
    }
  }
  const bool loadedWindowReady = spinUntil([&] {
    return prompts.size() == promptCount &&
           std::ranges::all_of(prompts, [](const nodegraph::NodeRef &prompt) {
             const auto *attachment = graphAttachment(prompt);
             return attachment && attachment->widget;
           }) &&
           view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  });

  bool foundDormantOverscan = false;
  int attachedPrompts = 0;
  int viewportPrompts = 0;
  int activeAnimations = 0;
  int activeDelays = 0;
  for (const nodegraph::NodeRef &prompt : prompts) {
    ui::QtNodeAttachment *attachment = graphAttachment(prompt);
    if (!attachment || !attachment->widget)
      continue;
    ++attachedPrompts;
    viewportPrompts += attachment->viewportVisible ? 1 : 0;
    auto *promptCard =
        qobject_cast<ConversationCard *>(attachment->widget.data());
    QTimer *animation = promptCard
                            ? promptCard->findChild<QTimer *>(
                                  QStringLiteral("pendingAnimationTimer"))
                            : nullptr;
    QTimer *delay = promptCard ? promptCard->findChild<QTimer *>(
                                     QStringLiteral("pendingDelayTimer"))
                               : nullptr;
    activeAnimations += animation && animation->isActive() ? 1 : 0;
    activeDelays += delay && delay->isActive() ? 1 : 0;
    if (!attachment->viewportVisible && promptCard && animation && delay &&
        !animation->isActive() &&
        !delay->isActive()) {
      foundDormantOverscan = true;
      break;
    }
  }
  if (!(loadedWindowReady && foundDormantOverscan)) {
    std::cerr << "overdue visibility: ready=" << loadedWindowReady
              << " prompts=" << prompts.size()
              << " attached=" << attachedPrompts
              << " viewport=" << viewportPrompts
              << " animations=" << activeAnimations
              << " delays=" << activeDelays
              << " scroll=" << view.verticalScrollBar()->value() << '/'
              << view.verticalScrollBar()->maximum()
              << " blocked="
              << view.property("bulkMaterializationUpdatesSuppressed").toBool()
              << '\n';
  }
  bool result = expect(
      loadedWindowReady && foundDormantOverscan,
      "an overdue materialized overscan prompt performs no timer work while "
      "its node attachment is outside the viewport");

  const nodegraph::NodeRef first = prompts.empty() ? nullptr : prompts.front();
  ui::QtNodeAttachment *firstDormantAttachment = graphAttachment(first);
  auto *firstDormantCard =
      firstDormantAttachment
          ? qobject_cast<ConversationCard *>(
                firstDormantAttachment->widget.data())
          : nullptr;
  QTimer *firstDormantAnimation =
      firstDormantCard
          ? firstDormantCard->findChild<QTimer *>(
                QStringLiteral("pendingAnimationTimer"))
          : nullptr;
  QTimer *firstDormantDelay =
      firstDormantCard
          ? firstDormantCard->findChild<QTimer *>(
                QStringLiteral("pendingDelayTimer"))
          : nullptr;
  result &= expect(
      firstDormantAttachment && firstDormantAttachment->widget &&
          !firstDormantAttachment->viewportVisible && firstDormantAnimation &&
          !firstDormantAnimation->isActive() && firstDormantDelay &&
          !firstDormantDelay->isActive(),
      "a loaded distant overdue prompt retains its one materialized card but "
      "performs no animation work until its viewport is requested");
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  spin(160);

  ui::QtNodeAttachment *attachment = graphAttachment(first);
  auto *promptCard =
      attachment ? qobject_cast<ConversationCard *>(attachment->widget.data())
                 : nullptr;
  QTimer *animation = promptCard ? promptCard->findChild<QTimer *>(
                                       QStringLiteral("pendingAnimationTimer"))
                                 : nullptr;
  QTimer *delay =
      promptCard
          ? promptCard->findChild<QTimer *>(QStringLiteral("pendingDelayTimer"))
          : nullptr;
  const auto *promptData =
      promptCard ? std::get_if<LocalPromptData>(&promptCard->data().payload)
                 : nullptr;
  result &= expect(
      attachment && attachment->viewportVisible && promptCard && promptData &&
          promptData->admittedAtMs == admittedAt &&
          promptCard->property("pendingFeedbackVisible").toBool() &&
          animation && animation->isActive() && delay && !delay->isActive(),
      "materializing an already-overdue graph prompt starts feedback "
      "immediately from its retained admission deadline");
  return result;
}
#endif

bool testPendingPromptAnimation() {
  VisibleCardData pending{
      LocalPromptKey{901},
      CardKind::LocalPrompt,
      "prompt-thread",
      {},
      {},
      LocalPromptData{
          901, "pending prompt", PromptState::InFlight, false, {}, {}}};
  ConversationCard card(pending);
  card.resize(560, 92);
  card.show();
  spin(40);
  const QImage first = card.grab().toImage();
  auto *pendingStatus =
      card.findChild<QLabel *>(QStringLiteral("pendingPromptStatus"));
  spin(110);
  const QImage second = card.grab().toImage();
  bool result = expect(pendingStatus &&
                           pendingStatus->text() == QStringLiteral("pending") &&
                           first == second,
                       "a newly admitted prompt begins as a calm static card");
  result &= expect(first.pixelColor(10, first.height() - 10).blue() >
                           first.pixelColor(10, first.height() - 10).red() &&
                       first.pixelColor(10, first.height() - 10).blue() >
                           first.pixelColor(10, first.height() - 10).green(),
                   "the temporary You card stays in the blue identity family");

  spin(950);
  const QImage delayedFirst = card.grab().toImage();
  spin(110);
  result &= expect(delayedFirst != card.grab().toImage(),
                   "pending feedback starts locally after one second without "
                   "a worker or graph timer update");

  auto &prompt = std::get<LocalPromptData>(pending.payload);
  prompt.showPendingAnimation = true;
  result &= expect(card.apply(pending),
                   "the delayed pending state starts the feedback sweep");
  const QImage animatedFirst = card.grab().toImage();
  spin(110);
  result &= expect(animatedFirst != card.grab().toImage(),
                   "an overdue unacknowledged prompt visibly animates");

  result &= expect(card.setAuthoritativeTurnActive(true),
                   "the retained prompt immediately owns the active border");
  const auto activeBorderVisible = [&card] {
    const QImage frame = card.grab().toImage();
    return frame.pixelColor(1, frame.height() / 2).red() < 175;
  };
  for (int frame = 0; frame < 10; ++frame) {
    spin(50);
    result &= expect(activeBorderVisible(),
                     "pending feedback never weakens the active border");
  }

  prompt.state = PromptState::Accepted;
  prompt.showPendingAnimation = false;
  result &= expect(card.apply(pending),
                   "the correlated acknowledgement stops pending feedback");
  const QImage settled = card.grab().toImage();
  spin(100);
  result &= expect(settled == card.grab().toImage(),
                   "acknowledgement leaves a stable retained card");

  VisibleCardData steering{
      LocalPromptKey{902},
      CardKind::LocalPrompt,
      "prompt-thread",
      "turn",
      {},
      LocalPromptData{
          902, "steering prompt", PromptState::InFlight, false, {}, {}}};
  ConversationCard steeringCard(steering);
  steeringCard.setProperty("nestedConversationCard", true);
  steeringCard.resize(520, 92);
  steeringCard.show();
  spin(40);
  const QImage steeringStatic = steeringCard.grab().toImage();
  spin(100);
  result &= expect(steeringStatic == steeringCard.grab().toImage(),
                   "steering uses the same calm initial timing");
  auto &steeringPrompt = std::get<LocalPromptData>(steering.payload);
  steeringPrompt.showPendingAnimation = true;
  result &= expect(steeringCard.apply(steering),
                   "overdue steering starts its teal feedback sweep");
  auto *steeringStatus = steeringCard.findChild<QLabel *>(
      QStringLiteral("steeringMessagePhase"));
  result &= expect(
      steeringStatus &&
          steeringStatus->text() == QStringLiteral("steering · pending"),
      "pending steering retains its identity and shows its lifecycle state");
  const QImage steeringAnimated = steeringCard.grab().toImage();
  spin(110);
  result &= expect(steeringAnimated != steeringCard.grab().toImage(),
                   "the delayed steering sweep is visibly animated");
  result &= expect(
      steeringAnimated.pixelColor(10, steeringAnimated.height() - 10).green() >
          steeringAnimated.pixelColor(10, steeringAnimated.height() - 10).red(),
      "the steering feedback stays in the teal identity family");
  steeringPrompt.state = PromptState::Accepted;
  steeringPrompt.showPendingAnimation = false;
  result &= expect(steeringCard.apply(steering),
                   "steering acknowledgement stops its feedback sweep");
  result &= expect(steeringStatus &&
                       steeringStatus->text() == QStringLiteral("steering"),
                   "acknowledgement clears pending from the steering header");
  spin(40);
  const QImage steeringSettled = steeringCard.grab().toImage();
  spin(100);
  result &= expect(steeringSettled == steeringCard.grab().toImage(),
                   "acknowledged steering remains visually stable");

  pending = {LocalPromptKey{901},
             CardKind::UserMessage,
             "prompt-thread",
             "turn",
             "user",
             UserMessageData{"pending prompt", {}}};
  result &= expect(card.apply(pending) && activeBorderVisible(),
                   "authoritative promotion retains the same active border");
  return result;
}

bool testMessageImagePresentation() {
  QTemporaryDir directory;
  const QString path = directory.filePath(QStringLiteral("sample.png"));
  const QString portraitPath =
      directory.filePath(QStringLiteral("portrait.png"));
  const QString squarePath = directory.filePath(QStringLiteral("square.png"));
  QImage source(640, 360, QImage::Format_ARGB32_Premultiplied);
  source.fill(QColor(QStringLiteral("#2f6feb")));
  QImage portrait(320, 640, QImage::Format_ARGB32_Premultiplied);
  portrait.fill(QColor(QStringLiteral("#6941c6")));
  QImage square(420, 420, QImage::Format_ARGB32_Premultiplied);
  square.fill(QColor(QStringLiteral("#18865e")));
  bool result =
      expect(directory.isValid() && source.save(path) &&
                 portrait.save(portraitPath) && square.save(squarePath),
             "image test fixtures are real readable images");

  VisibleCardData message{
      AuthoritativeItemKey{"images", "turn", "message"},
      CardKind::UserMessage,
      "images",
      "turn",
      "message",
      UserMessageData{"attached images",
                      {utf8(path), utf8(portraitPath), utf8(squarePath)}}};
  auto *card = new ConversationCard(message);
  card->resize(430, 400);
  card->show();
  spin();
  auto *ribbon =
      card->findChild<QScrollArea *>(QStringLiteral("messageImages"));
  auto thumbnails =
      card->findChildren<QLabel *>(QStringLiteral("messageImageThumbnail"));
  std::ranges::sort(thumbnails, [ribbon](QLabel *left, QLabel *right) {
    return left->mapTo(ribbon, QPoint{}).x() <
           right->mapTo(ribbon, QPoint{}).x();
  });
  auto *thumbnail = thumbnails.empty() ? nullptr : thumbnails.front();
  const QPixmap thumbnailPixmap = thumbnail ? thumbnail->pixmap() : QPixmap{};
  const auto hasEvenVerticalGap = [ribbon](QLabel *image) {
    if (!ribbon || !image)
      return false;
    const int top = image->mapTo(ribbon->viewport(), QPoint{}).y();
    const int bottom = ribbon->viewport()->height() - top - image->height();
    return std::abs(top - bottom) <= 1;
  };
  const QImage ribbonImage = ribbon ? ribbon->grab().toImage() : QImage{};
  result &= expect(
      ribbon && thumbnails.size() == 3 && thumbnail &&
          thumbnail->property("imageAvailable").toBool() &&
          !thumbnailPixmap.isNull() && thumbnailPixmap.width() <= 280 &&
          thumbnailPixmap.height() <= 180 &&
          std::ranges::all_of(thumbnails, hasEvenVerticalGap) &&
          !ribbonImage.isNull() &&
          ribbonImage.pixelColor(2, 2) == QColor(QStringLiteral("#111827")) &&
          thumbnails[0]->mapTo(ribbon, QPoint{}).x() <
              thumbnails[1]->mapTo(ribbon, QPoint{}).x() &&
          thumbnails[1]->mapTo(ribbon, QPoint{}).x() <
              thumbnails[2]->mapTo(ribbon, QPoint{}).x() &&
          ribbon->horizontalScrollBar()->maximum() > 0 &&
          ribbon->verticalScrollBar()->maximum() == 0 &&
          ribbon->frameWidth() == 1 && ribbon->widget() &&
          ribbon->widget()->layout() &&
          ribbon->widget()->layout()->contentsMargins() == QMargins(4, 4, 4, 4),
      "multiple bounded thumbnails form one horizontally scrollable "
      "and canonically bounded ribbon");
  const int narrowRibbonHeight = ribbon ? ribbon->height() : 0;
  card->resize(1000, card->height());
  spin();
  result &= expect(ribbon && ribbon->horizontalScrollBar()->maximum() == 0 &&
                       ribbon->height() < narrowRibbonHeight,
                   "a wide ribbon removes unnecessary horizontal overflow");
  card->resize(430, card->height());
  spin();
  result &= expect(ribbon && ribbon->horizontalScrollBar()->maximum() > 0,
                   "narrowing restores accessible horizontal overflow");
  auto &payload = std::get<UserMessageData>(message.payload);
  payload.text = "attached image with edited text";
  result &= expect(card->apply(message),
                   "message text updates with an unchanged attachment");
  auto *retainedThumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &= expect(retainedThumbnail == thumbnail,
                   "an unchanged attachment retains its decoded thumbnail");
  thumbnail = retainedThumbnail;
  result &= expect(thumbnail && thumbnail->focusPolicy() == Qt::StrongFocus &&
                       !thumbnail->accessibleName().isEmpty(),
                   "available image thumbnails expose a named keyboard target");
  if (thumbnail) {
    QKeyEvent activate(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &activate);
    spin();
  }
  QWidget *viewer = nullptr;
  for (QWidget *candidate : QApplication::topLevelWidgets())
    if (candidate->objectName() == QStringLiteral("messageImageViewer"))
      viewer = candidate;
  result &= expect(viewer && viewer->isVisible(),
                   "keyboard activation opens the non-modal image viewer");
  const auto *viewerImage = viewer ? viewer->findChild<QLabel *>(QStringLiteral(
                                         "messageImageViewerImage"))
                                   : nullptr;
  result &= expect(viewerImage && !viewerImage->pixmap().isNull(),
                   "the shown viewer contains a fitted image pixmap");
  if (viewer)
    viewer->close();
  spin();

  const QString missingPath = directory.filePath(QStringLiteral("missing.png"));
  QPointer<QLabel> retainedGuard(retainedThumbnail);
  payload.imagePaths = {utf8(missingPath)};
  result &= expect(card->apply(message),
                   "changing the image list invalidates card presentation");
  auto *missingThumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &= expect(
      retainedGuard.isNull() && missingThumbnail &&
          !missingThumbnail->property("imageAvailable").toBool() &&
          missingThumbnail->text().contains(QStringLiteral("unavailable")) &&
          missingThumbnail->focusPolicy() == Qt::NoFocus &&
          !missingThumbnail->accessibleName().isEmpty(),
      "an unreadable image has a stable restrained placeholder");

  result &= expect(source.save(missingPath),
                   "the missing attachment can be recreated");
  QPointer<QLabel> missingGuard(missingThumbnail);
  payload.text += " after recreation";
  card->apply(message);
  auto *recreatedThumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &= expect(missingGuard.isNull() && recreatedThumbnail &&
                       recreatedThumbnail->property("imageAvailable").toBool(),
                   "recreating an attachment replaces its placeholder");

  result &= expect(QFile::remove(missingPath),
                   "the recreated attachment can be deleted");
  QPointer<QLabel> recreatedGuard(recreatedThumbnail);
  payload.text += " after deletion";
  card->apply(message);
  auto *deletedThumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &= expect(recreatedGuard.isNull() && deletedThumbnail &&
                       !deletedThumbnail->property("imageAvailable").toBool(),
                   "deleting an attachment restores its placeholder");

  payload.imagePaths = {utf8(path)};
  card->apply(message);
  thumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, local, local,
                      thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &press);
    spin();
  }
  viewer = nullptr;
  for (QWidget *candidate : QApplication::topLevelWidgets())
    if (candidate->objectName() == QStringLiteral("messageImageViewer") &&
        candidate->isVisible())
      viewer = candidate;
  result &= expect(!viewer, "mouse-down does not open the image viewer");
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent release(QEvent::MouseButtonRelease, local, local,
                        thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &release);
    spin();
  }
  for (QWidget *candidate : QApplication::topLevelWidgets())
    if (candidate->objectName() == QStringLiteral("messageImageViewer") &&
        candidate->isVisible())
      viewer = candidate;
  delete card;
  spin();
  result &= expect(viewer && viewer->isVisible(),
                   "mouse-up opens a viewer that remains independent of its "
                   "originating card");
  if (viewer)
    viewer->close();
  spin();
  return result;
}

bool testGeneratedImagePresentationAndGenericBound() {
  QTemporaryDir directory;
  const QString path = directory.filePath(QStringLiteral("generated.png"));
  QImage source(800, 450, QImage::Format_ARGB32_Premultiplied);
  source.fill(QColor(QStringLiteral("#e9f7f0")));
  bool result = expect(directory.isValid() && source.save(path),
                       "generated-image fixture is readable");

  VisibleCardData generated{
      AuthoritativeItemKey{"generated", "turn", "image"},
      CardKind::ImageGeneration,
      "generated",
      "turn",
      "image",
      ImageGenerationData{utf8(path), "completed", "A generated UI proposal"}};
  ConversationCard generatedCard(generated);
  generatedCard.show();
  spin();
  auto *thumbnail = generatedCard.findChild<QLabel *>(
      QStringLiteral("messageImageThumbnail"));
  result &= expect(thumbnail && thumbnail->property("imageAvailable").toBool(),
                   "generated-image card reuses the bounded thumbnail");
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, local, local,
                      thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, local,
                        thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &release);
    spin();
  }
  QWidget *viewer = nullptr;
  for (QWidget *candidate : QApplication::topLevelWidgets())
    if (candidate->objectName() == QStringLiteral("messageImageViewer"))
      viewer = candidate;
  result &= expect(viewer && viewer->isVisible(),
                   "generated-image thumbnail opens the shared image viewer");
  if (viewer)
    viewer->close();
  spin();

  VisibleCardData viewed{AuthoritativeItemKey{"generated", "turn", "view"},
                         CardKind::ImageGeneration,
                         "generated",
                         "turn",
                         "view",
                         ImageGenerationData{utf8(path), {}, {}}};
  ConversationCard viewedCard(viewed);
  viewedCard.show();
  spin();
  const auto viewedLabels = viewedCard.findChildren<QLabel *>();
  result &= expect(
      std::ranges::any_of(viewedLabels,
                          [](QLabel *label) {
                            return label->property("kind").toString() ==
                                       QStringLiteral("title") &&
                                   label->text() == QStringLiteral("Image");
                          }) &&
          std::ranges::any_of(
              viewedLabels,
              [](QLabel *label) {
                return label->objectName() ==
                           QStringLiteral("messageImageThumbnail") &&
                       label->property("imageAvailable").toBool();
              }),
      "plain image-view cards use a neutral title and the shared thumbnail");

  VisibleCardData generic{
      AuthoritativeItemKey{"generated", "turn", "unknown"},
      CardKind::GenericActivity,
      "generated",
      "turn",
      "unknown",
      GenericActivityData{"contextCompaction",
                          {{"type", "contextCompaction"},
                           {"large", std::string(100000, 'x')}}}};
  ConversationCard genericCard(generic);
  genericCard.show();
  spin();
  auto *details = genericCard.findChild<QLabel *>(
      QStringLiteral("genericActivityMetadata"));
  const auto genericLabels = genericCard.findChildren<QLabel *>();
  result &= expect(
      std::ranges::any_of(genericLabels,
                          [](QLabel *label) {
                            return label->property("kind").toString() ==
                                       QStringLiteral("title") &&
                                   label->text() ==
                                       QStringLiteral("Context compaction");
                          }) &&
          std::get<GenericActivityData>(generic.payload).type ==
              "contextCompaction" &&
          details && details->text().size() < 4200 &&
          details->text().endsWith(
              QStringLiteral("[Activity details truncated]")),
      "protocol labels are humanized without changing bounded raw details");
  auto &genericData = std::get<GenericActivityData>(generic.payload);
  genericData.displayDetail = "field: direct graph detail";
  result &= expect(genericCard.apply(generic) && details &&
                       details->text() ==
                           QStringLiteral("field: direct graph detail"),
                   "graph generic activity detail renders without JSON "
                   "construction");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;
  if (qEnvironmentVariableIsSet("CODEXUI_MUTABLE_CARD_TESTS"))
    return testMutableCardsAndCommandOutput() ? 0 : 1;
  if (qEnvironmentVariableIsSet("CODEXUI_FOLLOW_TESTS"))
    return testFollowPauseAndStableAnchor() ? 0 : 1;
  if (qEnvironmentVariableIsSet("CODEXUI_BORDER_TESTS"))
    return testActiveWorkBordersFollowStatus() && testPendingPromptAnimation()
               ? 0
               : 1;
  if (qEnvironmentVariableIsSet("CODEXUI_SETTLEMENT_TESTS")) {
    bool focused = testRetainedNestedFinalAnswerGeometrySettlement();
    focused &= testBottomAnchoredCommandOutputGrowth();
    if (focused)
      std::cout << "Conversation settlement tests passed\n";
    return focused ? 0 : 1;
  }
  bool result = testMessageIdentityPalette();
  result &= testActiveWorkBordersFollowStatus();
  result &= testStructuralOrderAndIdentity();
  result &= testFollowPauseAndStableAnchor();
  result &= testPausedExpandedCommandStaysPainted();
  result &= testCommandCompletionWithoutGeometryWork();
  result &= testStreamingAgentBecomesVisibleWithoutReselection();
  result &= testThreadLocalScrollAndComposerExtent();
  result &= testPromptAdmissionFollowOwnership();
  result &= testCardCopyControls();
  result &= testMutableCardsAndCommandOutput();
  result &= testCardFoldingGeometryAndRetention();
  result &= testPresentationOptionsRetainCardsAndInitialFolding();
  result &= testInitialCommandGeometrySettlement();
  result &= testRootlessFinalAnswerGeometrySettlement();
  result &= testRetainedNestedFinalAnswerGeometrySettlement();
  result &= testBottomAnchoredCommandOutputGrowth();
  result &= testCommandOutputStateAcrossNavigation();
  result &= testPendingPromptAnimation();
  result &= testMessageImagePresentation();
  result &= testGeneratedImagePresentationAndGenericBound();
  if (result)
    std::cout << "Conversation card tests passed\n";
  return result ? 0 : 1;
}
