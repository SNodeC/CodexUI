// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationView.h"

#include "codex/middle/ConversationCards.h"
#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/ui/QtNodeAttachment.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int CardSpacing = 8;
constexpr int NativeScrollLineStep = 20;
constexpr int MaxCardOperationsPerPass = 8;
constexpr std::size_t MaxVisibilitySlotChecksPerPass = 64;
constexpr std::size_t MaxVisibilitySectionChecksPerPass = 64;
constexpr int EstimatedGraphHistoryItemExtent = 66;

class GraphHistoryPlaceholder final : public QWidget {
public:
  explicit GraphHistoryPlaceholder(QWidget *parent = nullptr)
      : QWidget(parent) {
    setObjectName(QStringLiteral("conversationHistoryPlaceholder"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setItemCount(0);
  }

  void setItemCount(std::size_t count) {
    setProperty("hiddenItemCount", QVariant::fromValue<qulonglong>(
                                       static_cast<qulonglong>(count)));
    const std::size_t maximum = static_cast<std::size_t>(
        std::numeric_limits<int>::max() / EstimatedGraphHistoryItemExtent);
    const int height = static_cast<int>(std::min(count, maximum)) *
                       EstimatedGraphHistoryItemExtent;
    setFixedHeight(height);
    setVisible(count != 0);
  }
};

class MeasuredCardPlaceholder final : public QWidget {
public:
  MeasuredCardPlaceholder(const std::string &key, int height, QWidget *parent)
      : QWidget(parent) {
    setObjectName(QStringLiteral("conversationCardPlaceholder"));
    setProperty("conversationAnchorKey", QString::fromStdString(key));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMeasuredHeight(height);
  }

  void setMeasuredHeight(int height) {
    height_ = std::max(0, height);
    setFixedHeight(height_);
  }

private:
  int height_ = 0;
};

int initialCardHeight(CardKind kind) {
  switch (kind) {
  case CardKind::UserMessage:
  case CardKind::AgentMessage:
  case CardKind::LocalPrompt:
    return 88;
  case CardKind::CommandExecution:
  case CardKind::AgentActivity:
  case CardKind::Reasoning:
  case CardKind::FileChanges:
  case CardKind::ImageGeneration:
  case CardKind::Plan:
  case CardKind::GenericActivity:
    return 58;
  }
  return 72;
}

const nodegraph::Value *graphField(const nodegraph::NodeState &state,
                                   std::string_view name) {
  const auto found = state.fields.find(name);
  return found == state.fields.end() ? nullptr : &found->second;
}

const nodegraph::Value *graphMember(const nodegraph::Value::Object &object,
                                    std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

std::string graphString(const nodegraph::Value *value) {
  if (!value)
    return {};
  if (const auto *text = value->asString())
    return *text;
  if (const auto *number = value->asInt64())
    return std::to_string(*number);
  if (const auto *number = value->asUInt64())
    return std::to_string(*number);
  return {};
}

bool graphBool(const nodegraph::Value *value) {
  if (!value)
    return false;
  if (const auto *boolean = value->asBool())
    return *boolean;
  return false;
}

std::optional<std::size_t> graphSize(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const auto *number = value->asUInt64();
      number && *number <= std::numeric_limits<std::size_t>::max())
    return static_cast<std::size_t>(*number);
  if (const auto *number = value->asInt64(); number && *number >= 0)
    return static_cast<std::size_t>(*number);
  return std::nullopt;
}

bool graphProviderHasMoreHistory(const nodegraph::NodeState &state) {
  if (graphBool(graphField(state, "historyHasMore")))
    return true;
  if (!graphString(graphField(state, "historyNextCursor")).empty())
    return true;
  const nodegraph::Value *history = graphField(state, "history");
  const auto *object = history ? history->asObject() : nullptr;
  return object && (graphBool(graphMember(*object, "hasMore")) ||
                    !graphString(graphMember(*object, "nextCursor")).empty());
}

std::string graphStatus(const nodegraph::NodeState &state) {
  if (const nodegraph::Value *value = graphField(state, "status")) {
    if (const auto *object = value->asObject())
      return graphString(graphMember(*object, "type"));
    if (std::string status = graphString(value); !status.empty())
      return status;
  }
  switch (state.status) {
  case nodegraph::NodeStatus::Pending:
    return "pending";
  case nodegraph::NodeStatus::Running:
    return "inProgress";
  case nodegraph::NodeStatus::Completed:
    return "completed";
  case nodegraph::NodeStatus::Failed:
    return "failed";
  case nodegraph::NodeStatus::Interrupted:
    return "interrupted";
  case nodegraph::NodeStatus::NotLoaded:
    return "notLoaded";
  case nodegraph::NodeStatus::Connected:
    return "connected";
  case nodegraph::NodeStatus::Disconnected:
    return "disconnected";
  case nodegraph::NodeStatus::Unknown:
    return {};
  }
  return {};
}

std::optional<std::int64_t> graphInteger(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const auto *number = value->asInt64())
    return *number;
  if (const auto *number = value->asUInt64();
      number && *number <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
    return static_cast<std::int64_t>(*number);
  return std::nullopt;
}

std::vector<std::string> graphStrings(const nodegraph::Value *value) {
  std::vector<std::string> result;
  const auto *array = value ? value->asArray() : nullptr;
  if (!array)
    return result;
  result.reserve(array->size());
  for (const nodegraph::Value &entry : *array)
    if (const auto *text = entry.asString())
      result.push_back(*text);
  return result;
}

std::uint64_t graphOmittedTextBytes(const nodegraph::NodeState &state,
                                    std::string_view field) {
  const nodegraph::Value *retention = graphField(state, "textRetention");
  const auto *retentionObject = retention ? retention->asObject() : nullptr;
  const nodegraph::Value *entry =
      retentionObject ? graphMember(*retentionObject, field) : nullptr;
  const auto *entryObject = entry ? entry->asObject() : nullptr;
  const nodegraph::Value *discarded =
      entryObject ? graphMember(*entryObject, "discardedBytes") : nullptr;
  if (const auto *number = discarded ? discarded->asUInt64() : nullptr)
    return *number;
  if (const auto *number = discarded ? discarded->asInt64() : nullptr;
      number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return 0;
}

std::string withTruncationNotice(std::string value, std::uint64_t omitted,
                                 std::string_view subject, bool markdown) {
  if (omitted == 0)
    return value;
  const std::string notice = "Earlier " + std::string(subject) +
                             " was truncated (" + std::to_string(omitted) +
                             " bytes omitted).";
  return markdown ? "> " + notice + "\n\n" + value
                  : '[' + notice + "]\n" + value;
}

std::string graphMessageText(const nodegraph::NodeState &state) {
  std::string result;
  if (const auto *content = graphField(state, "content");
      content && content->asArray()) {
    for (const nodegraph::Value &entry : *content->asArray()) {
      const auto *object = entry.asObject();
      const std::string text =
          object ? graphString(graphMember(*object, "text")) : std::string{};
      if (text.empty())
        continue;
      if (!result.empty())
        result.push_back('\n');
      result += text;
    }
  }
  return result.empty() ? graphString(graphField(state, "text")) : result;
}

std::vector<std::string> graphImagePaths(const nodegraph::NodeState &state) {
  std::vector<std::string> result;
  const auto *content = graphField(state, "content");
  const auto *array = content ? content->asArray() : nullptr;
  if (!array)
    return result;
  for (const nodegraph::Value &entry : *array) {
    const auto *object = entry.asObject();
    if (!object || graphString(graphMember(*object, "type")) != "localImage")
      continue;
    if (std::string path = graphString(graphMember(*object, "path"));
        !path.empty())
      result.push_back(std::move(path));
  }
  return result;
}

std::vector<std::string>
graphLocalPromptImagePaths(const nodegraph::NodeState &state) {
  std::vector<std::string> result;
  const nodegraph::Value *attachments = graphField(state, "attachments");
  const auto *array = attachments ? attachments->asArray() : nullptr;
  if (!array)
    return result;
  for (const nodegraph::Value &entry : *array) {
    const auto *object = entry.asObject();
    if (!object ||
        !graphString(graphMember(*object, "mimeType")).starts_with("image/"))
      continue;
    if (std::string path = graphString(graphMember(*object, "path"));
        !path.empty())
      result.emplace_back(std::move(path));
  }
  return result;
}

std::string graphJoinedText(const nodegraph::Value *value) {
  const auto *array = value ? value->asArray() : nullptr;
  if (!array)
    return graphString(value);
  std::string result;
  for (const nodegraph::Value &entry : *array) {
    std::string text = graphString(&entry);
    if (text.empty())
      if (const auto *object = entry.asObject())
        text = graphString(graphMember(*object, "text"));
    if (text.empty())
      continue;
    if (!result.empty())
      result += ", ";
    result += text;
  }
  return result;
}

std::pair<int, int> graphDiffCounts(std::string_view diff) {
  int additions = 0;
  int deletions = 0;
  for (std::size_t offset = 0; offset <= diff.size();) {
    const std::size_t end = diff.find('\n', offset);
    const std::string_view line =
        diff.substr(offset, end == std::string_view::npos ? diff.size() - offset
                                                          : end - offset);
    if (!line.starts_with("+++ ") && !line.starts_with("--- ")) {
      if (line.starts_with('+'))
        ++additions;
      else if (line.starts_with('-'))
        ++deletions;
    }
    if (end == std::string_view::npos)
      break;
    offset = end + 1;
  }
  return {additions, deletions};
}

constexpr std::size_t MaximumGraphActivityDetailCharacters = 4000;

class GraphDetailBuilder final {
public:
  void append(std::string_view text) {
    if (text.empty() || truncated_)
      return;
    const std::size_t remaining =
        MaximumGraphActivityDetailCharacters - value_.size();
    if (text.size() <= remaining) {
      value_.append(text);
      return;
    }
    value_.append(text.substr(0, remaining));
    truncated_ = true;
  }

  void indent(int depth) {
    for (int index = 0; index < depth; ++index)
      append("  ");
  }

  [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  [[nodiscard]] std::string finish() && {
    if (truncated_)
      value_ += "\n\n[Activity details truncated]";
    return std::move(value_);
  }

private:
  std::string value_;
  bool truncated_ = false;
};

void appendGraphDetail(GraphDetailBuilder &builder,
                       const nodegraph::Value &value, int depth) {
  if (builder.truncated())
    return;
  if (value.isNull()) {
    builder.append("none");
    return;
  }
  if (const auto *boolean = value.asBool()) {
    builder.append(*boolean ? "true" : "false");
    return;
  }
  if (const auto *number = value.asInt64()) {
    builder.append(std::to_string(*number));
    return;
  }
  if (const auto *number = value.asUInt64()) {
    builder.append(std::to_string(*number));
    return;
  }
  if (const auto *number = value.asDouble()) {
    builder.append(std::to_string(*number));
    return;
  }
  if (const auto *text = value.asString()) {
    builder.append(*text);
    return;
  }
  if (depth >= 4) {
    builder.append("nested detail omitted");
    return;
  }
  if (const auto *array = value.asArray()) {
    if (array->empty()) {
      builder.append("none");
      return;
    }
    for (const nodegraph::Value &entry : *array) {
      builder.append("\n");
      builder.indent(depth + 1);
      builder.append("- ");
      appendGraphDetail(builder, entry, depth + 1);
      if (builder.truncated())
        return;
    }
    return;
  }
  const auto *object = value.asObject();
  if (!object || object->empty()) {
    builder.append("none");
    return;
  }
  for (const auto &[key, entry] : *object) {
    builder.append("\n");
    builder.indent(depth + 1);
    builder.append(key);
    builder.append(": ");
    appendGraphDetail(builder, entry, depth + 1);
    if (builder.truncated())
      return;
  }
}

std::string graphDisplayDetail(const nodegraph::NodeState &state) {
  GraphDetailBuilder builder;
  for (const auto &[key, value] : state.fields) {
    builder.append(key);
    builder.append(": ");
    appendGraphDetail(builder, value, 0);
    if (builder.truncated())
      break;
    builder.append("\n");
  }
  return std::move(builder).finish();
}

bool graphHasStructuredPlan(const nodegraph::NodeState &state) {
  if (!graphString(graphField(state, "planExplanation")).empty())
    return true;
  const nodegraph::Value *plan = graphField(state, "plan");
  if (!plan)
    return false;
  if (const auto *steps = plan->asArray())
    return !steps->empty();
  const auto *object = plan->asObject();
  if (!object)
    return false;
  if (!graphString(graphMember(*object, "explanation")).empty())
    return true;
  const nodegraph::Value *steps = graphMember(*object, "steps");
  return steps && steps->asArray() && !steps->asArray()->empty();
}

PlanData graphPlanData(const nodegraph::NodeState &state) {
  PlanData result;
  result.explanation = graphString(graphField(state, "planExplanation"));
  const nodegraph::Value *plan = graphField(state, "plan");
  const nodegraph::Value::Array *steps = plan ? plan->asArray() : nullptr;
  if (const auto *object = plan ? plan->asObject() : nullptr) {
    if (result.explanation.empty())
      result.explanation = graphString(graphMember(*object, "explanation"));
    const nodegraph::Value *nested = graphMember(*object, "steps");
    steps = nested ? nested->asArray() : nullptr;
  }
  if (!steps)
    return result;
  result.steps.reserve(steps->size());
  for (const nodegraph::Value &entry : *steps) {
    const auto *object = entry.asObject();
    if (!object)
      continue;
    std::string text = graphString(graphMember(*object, "step"));
    if (text.empty())
      text = graphString(graphMember(*object, "text"));
    if (!text.empty())
      result.steps.push_back(
          {std::move(text), graphString(graphMember(*object, "status"))});
  }
  return result;
}

CardKind graphCardKind(const nodegraph::NodeState &state) {
  const std::string type = graphString(graphField(state, "type"));
  if (type == "localPrompt")
    return CardKind::LocalPrompt;
  if (type == "userMessage")
    return CardKind::UserMessage;
  if (type == "agentMessage")
    return CardKind::AgentMessage;
  if (type == "commandExecution")
    return CardKind::CommandExecution;
  if (type == "collabAgentToolCall" || type == "subAgentActivity")
    return CardKind::AgentActivity;
  if (type == "reasoning")
    return CardKind::Reasoning;
  if (type == "fileChange")
    return CardKind::FileChanges;
  if (type == "imageGeneration" || type == "imageView")
    return CardKind::ImageGeneration;
  if ((type == "plan" && !graphMessageText(state).empty()) ||
      graphHasStructuredPlan(state))
    return CardKind::Plan;
  return CardKind::GenericActivity;
}

bool graphCardVisible(const nodegraph::NodeState &state,
                      const ConversationView::PresentationOptions &options) {
  const CardKind kind = graphCardKind(state);
  if (kind == CardKind::Reasoning)
    return options.showReasoning;
  if (kind != CardKind::AgentMessage)
    return true;
  return graphString(graphField(state, "phase")) == "final_answer" ||
         options.showCodexUpdates;
}

VisibleCardData graphCardData(const nodegraph::NodeRef &item,
                              std::string threadId, std::string turnId,
                              const nodegraph::NodeState &state) {
  const std::string itemId =
      item ? nodegraph::protocolCanonicalId(state, item) : std::string{};
  const std::string type = graphString(graphField(state, "type"));
  GenericActivityData generic;
  generic.type = type;
  generic.status = graphStatus(state);
  generic.displayDetail = graphDisplayDetail(state);
  VisibleCardData result{AuthoritativeItemKey{threadId, turnId, itemId},
                         CardKind::GenericActivity,
                         std::move(threadId),
                         std::move(turnId),
                         itemId,
                         std::move(generic)};

  result.kind = graphCardKind(state);
  switch (result.kind) {
  case CardKind::UserMessage:
    result.payload =
        UserMessageData{graphMessageText(state), graphImagePaths(state)};
    break;
  case CardKind::AgentMessage:
    result.payload = AgentMessageData{
        withTruncationNotice(graphMessageText(state),
                             graphOmittedTextBytes(state, "text"),
                             "Codex response", true),
        graphString(graphField(state, "phase")) == "final_answer"};
    break;
  case CardKind::CommandExecution: {
    std::string outputField = "aggregatedOutput";
    std::string output = graphString(graphField(state, "aggregatedOutput"));
    if (output.empty()) {
      outputField = "output";
      output = graphString(graphField(state, "output"));
    }
    output = withTruncationNotice(std::move(output),
                                  graphOmittedTextBytes(state, outputField),
                                  "command output", false);
    if (!terminalOutputHasVisibleText(output))
      output.clear();
    const auto exit = graphInteger(graphField(state, "exitCode"));
    auto duration = graphInteger(graphField(state, "durationMs"));
    if (!duration)
      duration = graphInteger(graphField(state, "duration_ms"));
    result.payload = CommandExecutionData{
        graphString(graphField(state, "command")),
        std::move(output),
        graphStatus(state),
        graphString(graphField(state, "cwd")),
        exit ? std::optional<int>(static_cast<int>(*exit)) : std::nullopt,
        duration};
    break;
  }
  case CardKind::AgentActivity:
    result.payload =
        AgentActivityData{graphString(graphField(state, "tool")),
                          graphStatus(state),
                          graphString(graphField(state, "kind")),
                          graphString(graphField(state, "prompt")),
                          graphString(graphField(state, "resultText")),
                          graphStrings(graphField(state, "receiverThreadIds")),
                          graphString(graphField(state, "model")),
                          graphString(graphField(state, "reasoningEffort")),
                          graphString(graphField(state, "agentThreadId")),
                          graphString(graphField(state, "agentPath")),
                          graphString(graphField(state, "senderThreadId"))};
    break;
  case CardKind::Reasoning:
    result.payload = ReasoningData{withTruncationNotice(
        graphJoinedText(graphField(state, "summary")),
        graphOmittedTextBytes(state, "summary"), "reasoning", true)};
    break;
  case CardKind::FileChanges: {
    FileChangesData projected{graphStatus(state), {}};
    const auto *changes = graphField(state, "changes");
    const auto *array = changes ? changes->asArray() : nullptr;
    if (array) {
      projected.changes.reserve(array->size());
      for (const nodegraph::Value &value : *array) {
        const auto *change = value.asObject();
        if (!change)
          continue;
        FileChangeData entry{graphString(graphMember(*change, "path")),
                             graphString(graphMember(*change, "kind")),
                             std::nullopt, std::nullopt};
        if (std::string diff = graphString(graphMember(*change, "diff"));
            !diff.empty()) {
          const auto [additions, deletions] = graphDiffCounts(diff);
          entry.additions = additions;
          entry.deletions = deletions;
        }
        projected.changes.push_back(std::move(entry));
      }
    }
    result.payload = std::move(projected);
    break;
  }
  case CardKind::ImageGeneration: {
    std::string path = graphString(graphField(state, "path"));
    if (path.empty())
      path = graphString(graphField(state, "savedPath"));
    if (path.empty())
      path = graphString(graphField(state, "saved_path"));
    std::string prompt = graphString(graphField(state, "revisedPrompt"));
    if (prompt.empty())
      prompt = graphString(graphField(state, "revised_prompt"));
    result.payload = ImageGenerationData{
        std::move(path), type == "imageView" ? "completed" : graphStatus(state),
        std::move(prompt)};
    break;
  }
  case CardKind::Plan:
    if (graphHasStructuredPlan(state))
      result.payload = graphPlanData(state);
    else
      result.payload =
          PlanData{{},
                   {},
                   withTruncationNotice(graphMessageText(state),
                                        graphOmittedTextBytes(state, "text"),
                                        "plan text", true)};
    if (item && item->id().kind == nodegraph::NodeKind::Turn) {
      result.key = TurnPlanKey{result.threadId, result.turnId};
      result.itemId.clear();
    }
    break;
  case CardKind::GenericActivity:
    break;
  case CardKind::LocalPrompt: {
    const std::string dispatch =
        graphString(graphField(state, "dispatchState"));
    PromptState promptState = PromptState::Queued;
    if (dispatch == "dispatching" || dispatch == "inFlight")
      promptState = PromptState::InFlight;
    else if (dispatch == "awaitingMaterialization")
      promptState = PromptState::Accepted;
    else if (dispatch == "failed" || dispatch == "uncertain")
      promptState = PromptState::Failed;
    const std::int64_t rawId =
        graphInteger(graphField(state, "submissionId")).value_or(0);
    const std::uint64_t submissionId =
        rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
    result.key = LocalPromptKey{submissionId};
    result.itemId.clear();
    result.payload = LocalPromptData{
        submissionId,
        graphString(graphField(state, "text")),
        promptState,
        graphField(state, "showPendingAnimation")
            ? graphBool(graphField(state, "showPendingAnimation"))
            : false,
        graphString(graphField(state, "error")),
        graphLocalPromptImagePaths(state),
        graphInteger(graphField(state, "admittedAtMs")),
        graphBool(graphField(state, "requiresExplicitRecovery"))};
    break;
  }
  }
  return result;
}

void setAttachmentViewportVisibility(ui::QtNodeAttachment &attachment,
                                     bool visible) {
  attachment.viewportVisible = visible;
  attachment.materialization = visible
                                   ? ui::NodeMaterialization::ViewportVisible
                                   : ui::NodeMaterialization::Overscan;
  if (auto *card = qobject_cast<ConversationCard *>(attachment.widget.data()))
    card->setViewportVisible(visible);
}

QLabel *makeEmptyLabel() {
  auto *label =
      new QLabel(QStringLiteral("Conversation activity appears here."));
  label->setProperty("kind", "muted");
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}

} // namespace

class ConversationView::TurnSectionWidget final : public QWidget {
public:
  struct CardSlot {
    std::string key;
    nodegraph::NodeRef graphNode;
    QWidget *item = nullptr;
    QPointer<QWidget> itemGuard;
    std::unique_ptr<ui::QtNodeAttachment> attachment;
    std::optional<std::uint64_t> promptVisualId;
    int measuredHeight = 0;
    bool projectionVisible = true;
  };

  explicit TurnSectionWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, false);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    cards = new QVBoxLayout(this);
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(CardSpacing);
  }

  QVBoxLayout *cards = nullptr;
  std::vector<CardSlot> cardSlots;
  QPointer<GraphHistoryPlaceholder> historyPlaceholder;
  std::size_t hiddenItemCount = 0;
  std::string rootKey;
  std::string protocolId;
  nodegraph::NodeRef graphNode;
  bool graphActive = false;
};

ConversationView::ConversationView(QWidget *parent)
    : QAbstractScrollArea(parent) {
  setObjectName(QStringLiteral("conversationScroll"));
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
  verticalScrollBar()->setSingleStep(NativeScrollLineStep);
  viewport()->setAutoFillBackground(false);

  content_ = new QWidget(viewport());
  content_->setObjectName(QStringLiteral("conversationContent"));
  content_->setAttribute(Qt::WA_StyledBackground, false);
  content_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  content_->installEventFilter(this);

  contentLayout_ = new QVBoxLayout(content_);
  contentLayout_->setContentsMargins(0, 0, 0, 0);
  contentLayout_->setSpacing(CardSpacing);
  contentLayout_->setAlignment(Qt::AlignTop);

  loadMore_ = new QPushButton(QStringLiteral("Load more activities"), content_);
  loadMore_->setProperty("kind", "history");
  loadMore_->setFixedHeight(32);
  loadMore_->hide();
  connect(loadMore_, &QPushButton::clicked, this, [this] {
    if (graph_) {
      const bool requestProviderPage =
          graphProviderHasMore_ &&
          graphHiddenItemCount_ <= AuthoritativeHistoryPageSize;
      const std::size_t requestedAvailable =
          std::numeric_limits<std::size_t>::max() - graphRequestedHistoryLimit_;
      graphRequestedHistoryLimit_ +=
          std::min(AuthoritativeHistoryPageSize, requestedAvailable);
      const std::size_t effectiveAvailable =
          std::numeric_limits<std::size_t>::max() - graphHistoryLimit_;
      graphHistoryLimit_ +=
          std::min(AuthoritativeHistoryPageSize, effectiveAvailable);
      storeCurrentThreadState();
      scheduleGraphRefresh();
      if (requestProviderPage && loadMoreAction_)
        loadMoreAction_();
      return;
    }
    if (loadMoreAction_)
      loadMoreAction_();
  });
  contentLayout_->addWidget(loadMore_, 0, Qt::AlignHCenter);

  graphLeadingPlaceholder_ = new GraphHistoryPlaceholder(content_);
  contentLayout_->addWidget(graphLeadingPlaceholder_);

  empty_ = makeEmptyLabel();
  emptyMessage_ = empty_->text();
  empty_->setParent(content_);
  contentLayout_->addWidget(empty_);
  trailingSpace_ =
      new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
  contentLayout_->addItem(trailingSpace_);

  followAnimation_ = new QVariantAnimation(this);
  followAnimation_->setEasingCurve(QEasingCurve::OutCubic);
  connect(followAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            if (mode_ != Mode::Following || applying_) {
              followAnimation_->stop();
              return;
            }
            // Never let a retargeted animation move an already-following view
            // backwards.
            setScrollValue(
                std::max(verticalScrollBar()->value(), value.toInt()));
          });
  connect(followAnimation_, &QVariantAnimation::finished, this, [this] {
    if (mode_ == Mode::Following)
      setScrollValue(verticalScrollBar()->maximum());
  });

  connect(verticalScrollBar(), &QScrollBar::sliderPressed, this, [this] {
    sliderDown_ = true;
    pausedByComposerGrowth_ = false;
    stopFollowingAnimation();
  });
  connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [this] {
    sliderDown_ = false;
    handleUserScrollValue(verticalScrollBar()->value());
  });
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
          [this](int action) {
            userActionPending_ = true;
            pausedByComposerGrowth_ = false;
            stopFollowingAnimation();
            if (action == QAbstractSlider::SliderSingleStepSub ||
                action == QAbstractSlider::SliderPageStepSub ||
                action == QAbstractSlider::SliderToMinimum) {
              mode_ = Mode::Paused;
            }
          });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            positionContent();
            scheduleVisibilityPass();
            if (programmaticScroll_ || applying_)
              return;
            if (sliderDown_ || userActionPending_) {
              handleUserScrollValue(value);
            }
            userActionPending_ = false;
          });

  recomputeGeometry();
}

ConversationView::~ConversationView() { clearGraph(); }

void ConversationView::bindGraph(const nodegraph::NodeGraph &graph,
                                 nodegraph::NodeRef selectedThread) {
  if (graph_ == &graph && graphThread_ == selectedThread) {
    graphChanged();
    return;
  }
  // Capture the outgoing graph's painted anchor before detaching its widgets.
  // setThread also restores the incoming thread's independent follow mode.
  const std::string nextThreadId =
      selectedThread ? selectedThread->id().canonical : std::string{};
  setThread(nextThreadId);
  const auto saved = threadStates_.find(nextThreadId);
  const std::optional<ThreadScrollState> restored =
      saved == threadStates_.end()
          ? std::nullopt
          : std::optional<ThreadScrollState>{saved->second};
  if (graph_)
    clearGraph();

  graph_ = &graph;
  graphThread_ = std::move(selectedThread);
  graphHiddenItemCount_ = 0;
  graphWindowItemCount_ = 0;
  graphProviderHasMore_ = false;
  runGraphRefresh();
  if (restored) {
    mode_ = restored->mode;
    pausedByComposerGrowth_ = restored->pausedByComposerGrowth;
    restoreAnchor(restored->anchor);
    storeCurrentThreadState();
  }
}

void ConversationView::graphChanged(
    std::span<const nodegraph::NodeRef> removed) {
  if (!graph_)
    return;
  detachRemovedNodes(removed);
  runGraphRefresh();
}

void ConversationView::graphChangedDeferred(
    std::span<const nodegraph::NodeRef> removed) {
  if (!graph_)
    return;
  detachRemovedNodes(removed);
  // A single eventfd drain may carry hundreds of streaming revisions. Keep
  // removal detachment synchronous for node lifetime, but coalesce ordinary
  // structural/render reconciliation into one later Qt event-loop pass.
  scheduleGraphRefresh();
}

void ConversationView::detachRemovedNodes(
    std::span<const nodegraph::NodeRef> removed) {
  if (graph_ && !removed.empty())
    detachGraphWidgets(removed);
}

void ConversationView::clearGraph() {
  if (!graph_ && graphSections_.empty())
    return;
  for (TurnSectionWidget *section : graphSections_) {
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
      auto *card =
          slot.attachment
              ? qobject_cast<ConversationCard *>(slot.attachment->widget.data())
              : nullptr;
      const auto outputState =
          card ? card->commandOutputScrollState() : std::nullopt;
      if (outputState && !outputState->followsLatest)
        commandOutputStates_[slot.key] = *outputState;
      else if (card)
        commandOutputStates_.erase(slot.key);
      if (slot.graphNode && slot.attachment &&
          slot.graphNode->uiAttachment() == slot.attachment.get())
        slot.graphNode->setUiAttachment(nullptr);
      slot.attachment.reset();
    }
    contentLayout_->removeWidget(section);
    delete section;
  }
  graphSections_.clear();
  static_cast<GraphHistoryPlaceholder *>(graphLeadingPlaceholder_)
      ->setItemCount(0);
  graphThread_.reset();
  graph_ = nullptr;
  graphHiddenItemCount_ = 0;
  graphWindowItemCount_ = 0;
  graphProviderHasMore_ = false;
  visibilitySectionCursor_ = 0;
  visibilitySlotCursor_ = 0;
  visibilitySlotsRemaining_ = 0;
  visibilityScanScrollTop_ = -1;
  visibilityScanViewportHeight_ = -1;
  visibilityScanViewportWidth_ = -1;
  visibilityScanContentHeight_ = -1;
  graphRefreshScheduled_ = false;
  visibilityPassScheduled_ = false;
}

void ConversationView::scheduleGraphRefresh() {
  if (!graph_ || graphRefreshScheduled_)
    return;
  graphRefreshScheduled_ = true;
  QTimer::singleShot(0, this, [this] {
    graphRefreshScheduled_ = false;
    runGraphRefresh();
  });
}

void ConversationView::runGraphRefresh() {
  if (!graph_)
    return;

  struct ItemPin final {
    nodegraph::NodeRef node;
    std::shared_ptr<const nodegraph::NodeState> state;
    nodegraph::NodeRef materializedPrompt;
    std::shared_ptr<const nodegraph::NodeState> materializedPromptState;
    bool suppressedByLocalPrompt = false;
  };
  struct TurnPin final {
    nodegraph::NodeRef node;
    std::shared_ptr<const nodegraph::NodeState> state;
    nodegraph::NodeRef root;
    std::vector<ItemPin> items;
    std::size_t hiddenBeforeItems = 0;
  };
  struct Structure final {
    bool threadRemoved = false;
    bool providerHasMore = false;
    std::size_t totalItems = 0;
    std::size_t leadingHiddenItems = 0;
    std::size_t hiddenItems = 0;
    std::string newestItemKey;
    std::vector<TurnPin> turns;
  } structure;

  {
    std::optional<nodegraph::NodeGraph::ReadAccess> read = graph_->tryRead();
    if (!read) {
      scheduleGraphRefresh();
      return;
    }
    if (!graphThread_ || read->removed(graphThread_)) {
      structure.threadRemoved = true;
    } else {
      const std::shared_ptr<const nodegraph::NodeState> threadState =
          read->state(graphThread_);
      std::optional<std::size_t> loadedItemCount;
      if (threadState) {
        structure.providerHasMore = graphProviderHasMoreHistory(*threadState);
        loadedItemCount =
            graphSize(graphField(*threadState, "historyLoadedItemCount"));
      }

      const auto turnRoot = [&read](const nodegraph::NodeRef &turn) {
        for (const nodegraph::NodeRef &candidate :
             read->related(turn, nodegraph::RelationKind::TurnRootItem))
          if (candidate && candidate->id().kind == nodegraph::NodeKind::Item &&
              !read->removed(candidate))
            return candidate;
        return nodegraph::NodeRef{};
      };
      const auto newestItem = [&read, &turnRoot, this]() -> nodegraph::NodeRef {
        for (std::size_t turnIndex = read->childCount(graphThread_);
             turnIndex > 0;) {
          nodegraph::NodeRef turn = read->childAt(graphThread_, --turnIndex);
          if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
              read->removed(turn))
            continue;
          for (std::size_t itemIndex = read->childCount(turn); itemIndex > 0;) {
            nodegraph::NodeRef item = read->childAt(turn, --itemIndex);
            if (item && item->id().kind == nodegraph::NodeKind::Item &&
                !read->removed(item))
              return item;
          }
          if (nodegraph::NodeRef root = turnRoot(turn))
            return root;
        }
        return {};
      }();
      if (newestItem)
        structure.newestItemKey = newestItem->id().canonical;
      if (mode_ == Mode::Paused && graphKnownItemCount_ != 0 &&
          loadedItemCount && *loadedItemCount > graphKnownItemCount_ &&
          !graphNewestItemKey_.empty() && !structure.newestItemKey.empty() &&
          structure.newestItemKey != graphNewestItemKey_) {
        // A paused view grows its retained tail only for genuinely new
        // activity. Older provider pages increase the loaded count while the
        // newest item stays identical and must remain hidden above the anchor.
        const std::size_t appended = *loadedItemCount - graphKnownItemCount_;
        const std::size_t available =
            std::numeric_limits<std::size_t>::max() - graphHistoryLimit_;
        graphHistoryLimit_ += std::min(appended, available);
      }
      const auto pinItem = [&read](const nodegraph::NodeRef &item) {
        nodegraph::NodeRef localPrompt;
        std::shared_ptr<const nodegraph::NodeState> localPromptState;
        bool suppressedByLocalPrompt = false;
        for (const nodegraph::NodeRef &candidate : read->related(
                 item, nodegraph::RelationKind::PromptMaterialization)) {
          if (!candidate || candidate->id().kind != nodegraph::NodeKind::Item ||
              read->removed(candidate))
            continue;
          const auto state = read->state(candidate);
          if (graphString(graphField(*state, "type")) != "localPrompt")
            continue;
          if (graphString(graphField(*state, "dispatchState")) ==
              "awaitingMaterialization") {
            localPrompt = candidate;
            localPromptState = state;
          } else {
            // Correlation is not delivery acknowledgement, but it does prove
            // both nodes represent the same authored message. Keep the local
            // card visible until the exact request result permits handoff.
            suppressedByLocalPrompt = true;
          }
          break;
        }
        return ItemPin{item, read->state(item), std::move(localPrompt),
                       std::move(localPromptState), suppressedByLocalPrompt};
      };

      if (loadedItemCount) {
        // App-server ingestion maintains this exact count. Walk backward only
        // through the turns and items needed by the current UI history window;
        // older storage is represented by fixed-geometry spacers.
        structure.totalItems = *loadedItemCount;
        std::size_t remaining =
            std::min(graphHistoryLimit_, structure.totalItems);
        std::size_t representedItems = 0;
        for (std::size_t turnIndex = read->childCount(graphThread_);
             turnIndex > 0 && remaining > 0;) {
          nodegraph::NodeRef turn = read->childAt(graphThread_, --turnIndex);
          if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
              read->removed(turn))
            continue;

          const std::size_t directItemCount = read->childCount(turn);
          nodegraph::NodeRef root = turnRoot(turn);
          const bool rootOutsideChildren = root && read->parent(root) != turn;
          const std::size_t logicalItemCount =
              directItemCount + (rootOutsideChildren ? 1U : 0U);

          std::vector<nodegraph::NodeRef> selected;
          selected.reserve(std::min(directItemCount, remaining));
          for (std::size_t itemIndex = directItemCount;
               itemIndex > 0 && remaining > 0;) {
            nodegraph::NodeRef item = read->childAt(turn, --itemIndex);
            if (!item || item->id().kind != nodegraph::NodeKind::Item ||
                read->removed(item))
              continue;
            selected.push_back(std::move(item));
            --remaining;
          }
          std::ranges::reverse(selected);

          const bool selectedDirectItem = !selected.empty();
          bool selectedOutsideRoot = false;
          if (rootOutsideChildren && remaining > 0) {
            --remaining;
            selectedOutsideRoot = true;
          }
          if (!selectedDirectItem && !selectedOutsideRoot)
            continue;

          TurnPin pinned{turn, read->state(turn), root, {}, 0};
          const bool selectedRoot =
              root && std::ranges::find(selected, root) != selected.end();
          if (root && !selectedRoot)
            pinned.items.push_back(pinItem(root));
          for (nodegraph::NodeRef &item : selected)
            pinned.items.push_back(pinItem(item));
          pinned.hiddenBeforeItems =
              logicalItemCount > pinned.items.size()
                  ? logicalItemCount - pinned.items.size()
                  : 0;
          structure.hiddenItems += pinned.hiddenBeforeItems;
          representedItems += logicalItemCount;
          structure.turns.push_back(std::move(pinned));
        }
        std::ranges::reverse(structure.turns);
        structure.leadingHiddenItems =
            structure.totalItems > representedItems
                ? structure.totalItems - representedItems
                : 0;
        structure.hiddenItems += structure.leadingHiddenItems;
      } else {
        // Compatibility fallback for manually constructed graphs that predate
        // the worker-maintained history count.
        struct TurnCount final {
          nodegraph::NodeRef node;
          std::size_t items = 0;
        };
        std::vector<TurnCount> counts;
        for (nodegraph::NodeRef turn : read->children(graphThread_)) {
          if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
              read->removed(turn))
            continue;
          const std::vector<nodegraph::NodeRef> children = read->children(turn);
          const std::size_t itemCount = static_cast<std::size_t>(
              std::ranges::count_if(children, [&read](const auto &item) {
                return item && item->id().kind == nodegraph::NodeKind::Item &&
                       !read->removed(item);
              }));
          counts.push_back({std::move(turn), itemCount});
          structure.totalItems += itemCount;
        }

        const std::size_t firstRetained =
            structure.totalItems > graphHistoryLimit_
                ? structure.totalItems - graphHistoryLimit_
                : 0;
        std::size_t offset = 0;
        for (const TurnCount &count : counts) {
          const std::size_t turnEnd = offset + count.items;
          if (turnEnd <= firstRetained) {
            structure.leadingHiddenItems += count.items;
            offset = turnEnd;
            continue;
          }

          std::vector<nodegraph::NodeRef> items = read->children(count.node);
          std::erase_if(items, [&read](const nodegraph::NodeRef &item) {
            return !item || item->id().kind != nodegraph::NodeKind::Item ||
                   read->removed(item);
          });
          const std::size_t selectedStart =
              firstRetained > offset ? firstRetained - offset : 0;
          TurnPin pinned{count.node,
                         read->state(count.node),
                         turnRoot(count.node),
                         {},
                         selectedStart};

          const auto root =
              pinned.root ? std::ranges::find(items, pinned.root) : items.end();
          if (pinned.root && root == items.end()) {
            pinned.items.push_back(pinItem(pinned.root));
          } else if (root != items.end()) {
            const std::size_t rootIndex =
                static_cast<std::size_t>(std::distance(items.begin(), root));
            if (rootIndex < selectedStart) {
              pinned.items.push_back(pinItem(pinned.root));
              --pinned.hiddenBeforeItems;
            }
          }

          for (std::size_t index = selectedStart; index < items.size(); ++index)
            pinned.items.push_back(pinItem(items[index]));
          structure.hiddenItems += pinned.hiddenBeforeItems;
          structure.turns.push_back(std::move(pinned));
          offset = turnEnd;
        }
        structure.hiddenItems += structure.leadingHiddenItems;
      }

      std::unordered_set<const nodegraph::Node *> materializedLocals;
      for (const TurnPin &turn : structure.turns)
        for (const ItemPin &item : turn.items)
          if (item.materializedPrompt)
            materializedLocals.insert(item.materializedPrompt.get());
      std::size_t hiddenDuplicates = 0;
      for (TurnPin &turn : structure.turns) {
        const std::size_t before = turn.items.size();
        std::erase_if(turn.items, [&materializedLocals](const ItemPin &item) {
          return item.suppressedByLocalPrompt ||
                 materializedLocals.contains(item.node.get());
        });
        hiddenDuplicates += before - turn.items.size();
      }
      std::erase_if(structure.turns, [](const TurnPin &turn) {
        return turn.items.empty() && turn.hiddenBeforeItems == 0;
      });
      structure.totalItems -= std::min(structure.totalItems, hiddenDuplicates);
    }
  }

  if (structure.threadRemoved) {
    std::array<nodegraph::NodeRef, 1> removed{graphThread_};
    detachGraphWidgets(removed);
    return;
  }

  std::vector<const nodegraph::Node *> previousOrder;
  for (TurnSectionWidget *section : graphSections_)
    for (const TurnSectionWidget::CardSlot &slot : section->cardSlots)
      previousOrder.push_back(slot.graphNode.get());
  std::vector<const nodegraph::Node *> nextOrder;
  for (const TurnPin &turn : structure.turns)
    for (const ItemPin &item : turn.items)
      nextOrder.push_back(item.node.get());
  const bool appended =
      structure.totalItems > graphKnownItemCount_ && !nextOrder.empty() &&
      (previousOrder.empty() || previousOrder.back() != nextOrder.back());

  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());

  std::unordered_map<const nodegraph::Node *, TurnSectionWidget *>
      retainedSections;
  std::unordered_map<const nodegraph::Node *, TurnSectionWidget::CardSlot>
      retainedSlots;
  for (TurnSectionWidget *section : graphSections_) {
    if (ConversationCard *root = cardForStableKey(section->rootKey))
      root->setNestedItems({});
    retainedSections.emplace(section->graphNode.get(), section);
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
      retainedSlots.emplace(slot.graphNode.get(), std::move(slot));
    section->cardSlots.clear();
  }

  std::vector<TurnSectionWidget *> nextSections;
  nextSections.reserve(structure.turns.size());
  std::vector<nodegraph::NodeRef> materializedPrompts;

  for (const TurnPin &turn : structure.turns) {
    TurnSectionWidget *section = nullptr;
    if (auto retained = retainedSections.find(turn.node.get());
        retained != retainedSections.end()) {
      section = retained->second;
      retainedSections.erase(retained);
    } else {
      section = new TurnSectionWidget(content_);
      section->setProperty("turnSectionKey",
                           QString::fromStdString(turn.node->id().canonical));
    }
    section->graphNode = turn.node;
    section->protocolId =
        turn.state ? nodegraph::protocolCanonicalId(*turn.state, turn.node)
                   : turn.node->id().canonical;
    section->setProperty("turnSectionKey",
                         QString::fromStdString(section->protocolId));
    section->setProperty("turnId", QString::fromStdString(section->protocolId));
    section->graphActive =
        turn.state && turn.state->status == nodegraph::NodeStatus::Running;
    section->rootKey.clear();
    section->hiddenItemCount = turn.hiddenBeforeItems;
    if (!section->historyPlaceholder)
      section->historyPlaceholder = new GraphHistoryPlaceholder(section);
    section->historyPlaceholder->setItemCount(turn.hiddenBeforeItems);
    section->cardSlots.reserve(turn.items.size());

    for (const ItemPin &item : turn.items) {
      TurnSectionWidget::CardSlot slot;
      auto retained = item.materializedPrompt
                          ? retainedSlots.find(item.materializedPrompt.get())
                          : retainedSlots.end();
      const bool transferringPrompt = retained != retainedSlots.end();
      if (!transferringPrompt)
        retained = retainedSlots.find(item.node.get());
      if (retained != retainedSlots.end()) {
        slot = std::move(retained->second);
        retainedSlots.erase(retained);
        if (transferringPrompt) {
          // The provider item may already have been independently visible
          // before the exact request result acknowledged the local prompt.
          // Prefer the authored card's stable identity and retire that
          // short-lived duplicate before moving the local attachment.
          if (auto duplicate = retainedSlots.find(item.node.get());
              duplicate != retainedSlots.end()) {
            TurnSectionWidget::CardSlot &displaced = duplicate->second;
            if (displaced.attachment &&
                item.node->uiAttachment() == displaced.attachment.get())
              item.node->setUiAttachment(nullptr);
            displaced.attachment.reset();
            delete displaced.item;
            retainedSlots.erase(duplicate);
          }
          if (slot.attachment &&
              item.materializedPrompt->uiAttachment() == slot.attachment.get())
            item.materializedPrompt->setUiAttachment(nullptr);
          if (slot.attachment) {
            slot.attachment->renderedRevision = 0;
            Q_ASSERT(item.node->uiAttachment() == nullptr);
            item.node->setUiAttachment(slot.attachment.get());
          }
        }
      } else {
        slot.graphNode = item.node;
        if (item.materializedPromptState) {
          const std::int64_t rawId =
              graphInteger(
                  graphField(*item.materializedPromptState, "submissionId"))
                  .value_or(0);
          slot.promptVisualId =
              rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
          slot.key = stableKey(LocalPromptKey{*slot.promptVisualId});
        } else if (item.node->id().kind == nodegraph::NodeKind::Turn) {
          slot.key = stableKey(
              TurnPlanKey{graphThread_->id().canonical, section->protocolId});
        } else if (graphCardKind(*item.state) == CardKind::LocalPrompt) {
          const std::int64_t rawId =
              graphInteger(graphField(*item.state, "submissionId")).value_or(0);
          slot.promptVisualId =
              rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
          slot.key = stableKey(LocalPromptKey{*slot.promptVisualId});
        } else {
          slot.key = stableKey(AuthoritativeItemKey{
              graphThread_->id().canonical, section->protocolId,
              nodegraph::protocolCanonicalId(*item.state, item.node)});
        }
        slot.measuredHeight = initialCardHeight(graphCardKind(*item.state));
        slot.item =
            new MeasuredCardPlaceholder(slot.key, slot.measuredHeight, section);
        slot.itemGuard = slot.item;
      }
      if (item.materializedPrompt &&
          std::find(materializedPrompts.begin(), materializedPrompts.end(),
                    item.materializedPrompt) == materializedPrompts.end())
        materializedPrompts.emplace_back(item.materializedPrompt);
      slot.graphNode = item.node;
      if (!slot.itemGuard) {
        if (slot.attachment &&
            slot.graphNode->uiAttachment() == slot.attachment.get())
          slot.graphNode->setUiAttachment(nullptr);
        slot.attachment.reset();
        slot.item =
            new MeasuredCardPlaceholder(slot.key, slot.measuredHeight, section);
        slot.itemGuard = slot.item;
      } else {
        slot.item = slot.itemGuard.data();
      }
      slot.projectionVisible =
          graphCardVisible(*item.state, presentationOptions_);
      if (item.node == turn.root)
        section->rootKey = slot.key;
      if (auto *placeholder =
              dynamic_cast<MeasuredCardPlaceholder *>(slot.item))
        placeholder->setMeasuredHeight(
            slot.projectionVisible ? slot.measuredHeight : 0);
      if (slot.item->isHidden() == slot.projectionVisible)
        slot.item->setVisible(slot.projectionVisible);
      section->cardSlots.push_back(std::move(slot));
    }
    arrangeSection(section);
    section->setVisible(
        turn.hiddenBeforeItems != 0 ||
        std::ranges::any_of(section->cardSlots,
                            [](const TurnSectionWidget::CardSlot &slot) {
                              return slot.projectionVisible;
                            }));
    nextSections.push_back(section);
  }

  for (auto &[node, slot] : retainedSlots) {
    static_cast<void>(node);
    if (slot.graphNode && slot.attachment &&
        slot.graphNode->uiAttachment() == slot.attachment.get())
      slot.graphNode->setUiAttachment(nullptr);
    slot.attachment.reset();
    delete slot.item;
  }
  for (auto &[node, section] : retainedSections) {
    static_cast<void>(node);
    contentLayout_->removeWidget(section);
    delete section;
  }

  for (TurnSectionWidget *section : graphSections_)
    contentLayout_->removeWidget(section);
  graphSections_ = std::move(nextSections);
  static_cast<GraphHistoryPlaceholder *>(graphLeadingPlaceholder_)
      ->setItemCount(structure.leadingHiddenItems);
  for (std::size_t index = 0; index < graphSections_.size(); ++index)
    contentLayout_->insertWidget(2 + static_cast<int>(index),
                                 graphSections_[index]);
  graphKnownItemCount_ = structure.totalItems;
  graphNewestItemKey_ = std::move(structure.newestItemKey);
  graphHiddenItemCount_ = structure.hiddenItems;
  graphWindowItemCount_ = nextOrder.size();
  graphProviderHasMore_ = structure.providerHasMore;
  updateGraphChrome();

  recomputeGeometry();
  if (follow && (previousOrder.empty() || appended))
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
  visibilitySlotsRemaining_ = 0;
  if (runGraphVisibilityPass())
    scheduleVisibilityPass();
  storeCurrentThreadState();
  if (promptMaterializedAction_) {
    for (nodegraph::NodeRef &prompt : materializedPrompts) {
      if (!promptMaterializedAction_(prompt)) {
        QTimer::singleShot(16, this, [this] { scheduleGraphRefresh(); });
        break;
      }
    }
  }
}

void ConversationView::detachGraphWidgets(
    std::span<const nodegraph::NodeRef> removed) {
  if (!graph_ || removed.empty())
    return;
  const auto isRemoved = [removed](const nodegraph::NodeRef &node) {
    return node && std::ranges::any_of(
                       removed, [&node](const nodegraph::NodeRef &candidate) {
                         return candidate == node;
                       });
  };
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());

  const bool removeThread = isRemoved(graphThread_);
  std::size_t removedWindowItems = 0;
  for (auto section = graphSections_.begin();
       section != graphSections_.end();) {
    TurnSectionWidget *widget = *section;
    if (ConversationCard *root = cardForStableKey(widget->rootKey))
      root->setNestedItems({});
    const bool removeSection = removeThread || isRemoved(widget->graphNode);
    for (auto slot = widget->cardSlots.begin();
         slot != widget->cardSlots.end();) {
      if (!removeSection && !isRemoved(slot->graphNode)) {
        ++slot;
        continue;
      }
      if (slot->graphNode && slot->attachment &&
          slot->graphNode->uiAttachment() == slot->attachment.get())
        slot->graphNode->setUiAttachment(nullptr);
      slot->attachment.reset();
      delete slot->item;
      ++removedWindowItems;
      slot = widget->cardSlots.erase(slot);
    }
    if (removeSection) {
      contentLayout_->removeWidget(widget);
      delete widget;
      section = graphSections_.erase(section);
      continue;
    }
    if (std::ranges::none_of(widget->cardSlots, [widget](const auto &slot) {
          return slot.key == widget->rootKey;
        }))
      widget->rootKey.clear();
    arrangeSection(widget);
    widget->setVisible(
        (widget->historyPlaceholder &&
         !widget->historyPlaceholder->isHidden()) ||
        std::ranges::any_of(widget->cardSlots,
                            [](const TurnSectionWidget::CardSlot &slot) {
                              return slot.projectionVisible;
                            }));
    ++section;
  }
  if (removeThread) {
    // The removed NodeRef must not survive into the already-scheduled refresh:
    // FrontendSession may acknowledge UI detachment as soon as this Qt pass
    // returns, after which retired graph membership can be released.
    graphThread_.reset();
    graphKnownItemCount_ = 0;
    graphNewestItemKey_.clear();
    graphHiddenItemCount_ = 0;
    graphWindowItemCount_ = 0;
    graphProviderHasMore_ = false;
    static_cast<GraphHistoryPlaceholder *>(graphLeadingPlaceholder_)
        ->setItemCount(0);
  } else {
    graphKnownItemCount_ -= std::min(graphKnownItemCount_, removedWindowItems);
    graphWindowItemCount_ -=
        std::min(graphWindowItemCount_, removedWindowItems);
    if (graphKnownItemCount_ == 0)
      graphHiddenItemCount_ = 0;
  }
  visibilitySlotsRemaining_ = 0;
  updateGraphChrome();
  recomputeGeometry();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
}

void ConversationView::setLoadMoreAction(std::function<void()> action) {
  loadMoreAction_ = std::move(action);
}

void ConversationView::setPromptMaterializedAction(
    std::function<bool(nodegraph::NodeRef)> action) {
  promptMaterializedAction_ = std::move(action);
}

void ConversationView::setPromptRecoveryAction(
    std::function<void(nodegraph::NodeRef)> action) {
  promptRecoveryAction_ = std::move(action);
}

void ConversationView::updateGraphChrome() {
  if (!graph_)
    return;
  const bool hasMore = graphHiddenItemCount_ != 0 || graphProviderHasMore_;
  loadMore_->setVisible(hasMore);
  if (graphHiddenItemCount_ != 0) {
    const std::size_t page =
        std::min(AuthoritativeHistoryPageSize, graphHiddenItemCount_);
    loadMore_->setText(QStringLiteral("Load %1 more activities")
                           .arg(static_cast<qulonglong>(page)));
    loadMore_->setToolTip(
        QStringLiteral("%1 earlier activities are retained")
            .arg(static_cast<qulonglong>(graphHiddenItemCount_)));
  } else {
    loadMore_->setText(QStringLiteral("Load more activities"));
    loadMore_->setToolTip({});
  }

  const bool hasVisibleCard =
      std::ranges::any_of(graphSections_, [](TurnSectionWidget *section) {
        return std::ranges::any_of(section->cardSlots,
                                   [](const TurnSectionWidget::CardSlot &slot) {
                                     return slot.projectionVisible;
                                   });
      });
  empty_->setVisible(!hasVisibleCard && graphHiddenItemCount_ == 0);
}

void ConversationView::setEmptyMessage(QString message) {
  if (message == emptyMessage_)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  emptyMessage_ = std::move(message);
  empty_->setText(emptyMessage_);
  recomputeGeometry();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  applying_ = false;
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
}

void ConversationView::setPresentationOptions(PresentationOptions options) {
  if (presentationOptions_ == options)
    return;
  presentationOptions_ = options;
  if (graph_)
    scheduleGraphRefresh();
}

void ConversationView::storeCurrentThreadState() {
  if (threadId_.empty())
    return;
  threadStates_[threadId_] = {mode_,
                              captureAnchor(),
                              pausedByComposerGrowth_,
                              graphRequestedHistoryLimit_,
                              graphHistoryLimit_,
                              graphKnownItemCount_,
                              graphNewestItemKey_};
}

void ConversationView::setThread(const std::string &threadId) {
  if (threadId == threadId_)
    return;
  storeCurrentThreadState();
  stopFollowingAnimation();
  threadId_ = threadId;
  const auto saved = threadStates_.find(threadId_);
  mode_ = saved == threadStates_.end() ? Mode::Following : saved->second.mode;
  pausedByComposerGrowth_ =
      saved != threadStates_.end() && saved->second.pausedByComposerGrowth;
  graphRequestedHistoryLimit_ = saved == threadStates_.end()
                                    ? AuthoritativeHistoryPageSize
                                    : saved->second.graphRequestedHistoryLimit;
  graphHistoryLimit_ = saved == threadStates_.end() || mode_ == Mode::Following
                           ? graphRequestedHistoryLimit_
                           : saved->second.graphHistoryLimit;
  graphKnownItemCount_ =
      saved == threadStates_.end() ? 0 : saved->second.graphKnownItemCount;
  graphNewestItemKey_ = saved == threadStates_.end()
                            ? std::string{}
                            : saved->second.graphNewestItemKey;
}

void ConversationView::arrangeSection(TurnSectionWidget *section) {
  if (!section)
    return;
  const auto slotCard = [this](TurnSectionWidget::CardSlot &slot) {
    if (graph_ && slot.attachment)
      return qobject_cast<ConversationCard *>(slot.attachment->widget.data());
    return dynamic_cast<ConversationCard *>(slot.item);
  };

  TurnSectionWidget::CardSlot *rootSlot = nullptr;
  if (!section->rootKey.empty()) {
    const auto root = std::ranges::find_if(
        section->cardSlots, [section](const TurnSectionWidget::CardSlot &slot) {
          return slot.key == section->rootKey;
        });
    if (root != section->cardSlots.end())
      rootSlot = &*root;
  }
  auto *prompt = rootSlot ? slotCard(*rootSlot) : nullptr;

  std::unordered_set<ConversationCard *> obsoleteOwners;
  if (section->historyPlaceholder) {
    for (QWidget *parent = section->historyPlaceholder->parentWidget(); parent;
         parent = parent->parentWidget()) {
      auto *owner = dynamic_cast<ConversationCard *>(parent);
      if (!owner)
        continue;
      if (owner != prompt)
        obsoleteOwners.insert(owner);
      break;
    }
  }
  for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
    for (QWidget *parent = slot.item->parentWidget(); parent;
         parent = parent->parentWidget()) {
      auto *owner = dynamic_cast<ConversationCard *>(parent);
      if (!owner)
        continue;
      if (owner != prompt)
        obsoleteOwners.insert(owner);
      break;
    }
  }
  for (ConversationCard *owner : obsoleteOwners)
    owner->setNestedItems({});

  // A formerly materialized root can be replaced by its placeholder in the
  // same pass. Ensure no other card keeps an obsolete nested layout first.
  for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
    auto *card = slotCard(slot);
    if (!card || card == prompt)
      continue;
    if (card->property("turnContainer").toBool())
      card->setNestedItems({});
    card->setProperty("turnContainer", false);
    card->setAuthoritativeTurnActive(false);
  }

  if (prompt) {
    std::vector<QWidget *> nestedItems;
    nestedItems.reserve(section->cardSlots.size());
    if (section->historyPlaceholder)
      nestedItems.push_back(section->historyPlaceholder);
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
      if (&slot != rootSlot)
        nestedItems.push_back(slot.item);
    prompt->setProperty("nestedConversationCard", false);
    prompt->setProperty("turnContainer", true);
    prompt->setNestedItems(nestedItems);
    prompt->setAuthoritativeTurnActive(section->graphActive);
    if (section->cards->indexOf(prompt) != 0)
      section->cards->insertWidget(0, prompt);
    return;
  }

  struct OrderedItem final {
    QWidget *widget = nullptr;
    TurnSectionWidget::CardSlot *slot = nullptr;
  };
  std::vector<OrderedItem> ordered;
  ordered.reserve(section->cardSlots.size() + 1);
  if (rootSlot)
    ordered.push_back({rootSlot->item, rootSlot});
  if (section->historyPlaceholder)
    ordered.push_back({section->historyPlaceholder, nullptr});
  for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
    if (&slot != rootSlot)
      ordered.push_back({slot.item, &slot});
  for (std::size_t position = 0; position < ordered.size(); ++position) {
    QWidget *item = ordered[position].widget;
    if (ordered[position].slot) {
      auto *card = slotCard(*ordered[position].slot);
      if (card) {
        if (card->property("turnContainer").toBool())
          card->setNestedItems({});
        card->setProperty("nestedConversationCard", false);
        card->setProperty("turnContainer", false);
        card->setAuthoritativeTurnActive(false);
        card->setMinimumHeight(0);
      }
    }
    if (section->cards->indexOf(item) != static_cast<int>(position))
      section->cards->insertWidget(static_cast<int>(position), item);
  }
}

void ConversationView::scheduleVisibilityPass() {
  if (applying_ || visibilityPassScheduled_ || !graph_)
    return;
  visibilityPassScheduled_ = true;
  QTimer::singleShot(0, this, [this] {
    visibilityPassScheduled_ = false;
    if (runVisibilityPass())
      scheduleVisibilityPass();
  });
}

void ConversationView::resetGraphVisibilityScan() {
  visibilitySectionCursor_ = 0;
  visibilitySlotCursor_ = 0;
  visibilitySlotsRemaining_ = graphWindowItemCount_;
  visibilityScanScrollTop_ = verticalScrollBar()->value();
  visibilityScanViewportHeight_ = viewport() ? viewport()->height() : 0;
  visibilityScanViewportWidth_ = viewport() ? viewport()->width() : 0;
  visibilityScanContentHeight_ = contentHeight_;
  if (visibilitySlotsRemaining_ == 0 || graphSections_.empty() || !content_ ||
      !viewport())
    return;

  const int targetY =
      visibilityScanScrollTop_ + std::max(1, visibilityScanViewportHeight_) / 2;
  std::size_t lower = 0;
  std::size_t upper = graphSections_.size();
  while (lower < upper) {
    const std::size_t middle = lower + (upper - lower) / 2;
    TurnSectionWidget *section = graphSections_[middle];
    const int bottom =
        section->mapTo(content_, QPoint{}).y() + std::max(1, section->height());
    if (bottom < targetY)
      lower = middle + 1;
    else
      upper = middle;
  }
  visibilitySectionCursor_ = std::min(lower, graphSections_.size() - 1);
  TurnSectionWidget *section = graphSections_[visibilitySectionCursor_];
  if (section->cardSlots.empty())
    return;
  const int top = section->mapTo(content_, QPoint{}).y();
  const int height = std::max(1, section->height());
  const double relative = std::clamp(static_cast<double>(targetY - top) /
                                         static_cast<double>(height),
                                     0.0, 1.0);
  const std::size_t estimated =
      std::min(section->cardSlots.size() - 1,
               static_cast<std::size_t>(
                   relative * static_cast<double>(section->cardSlots.size())));
  constexpr std::size_t LookBehind = MaxVisibilitySlotChecksPerPass / 4;
  visibilitySlotCursor_ = estimated > LookBehind ? estimated - LookBehind : 0;
}

bool ConversationView::runVisibilityPass() { return runGraphVisibilityPass(); }

bool ConversationView::runGraphVisibilityPass() {
  if (applying_ || !graph_ || !content_ || !viewport())
    return false;

  enum class Operation { Materialize, Render, Release };
  struct ScannedSlot final {
    TurnSectionWidget *section = nullptr;
    TurnSectionWidget::CardSlot *slot = nullptr;
  };
  struct Candidate final {
    TurnSectionWidget *section = nullptr;
    TurnSectionWidget::CardSlot *slot = nullptr;
    Operation operation = Operation::Materialize;
    bool inViewport = false;
    int distance = 0;
    std::uint64_t renderedRevision = 0;
    std::uint64_t nodeRevision = 0;
    std::shared_ptr<const nodegraph::NodeState> state;
  };

  const int scrollTop = verticalScrollBar()->value();
  const int viewportHeight = std::max(1, viewport()->height());
  if (visibilitySlotsRemaining_ == 0 || visibilityScanScrollTop_ != scrollTop ||
      visibilityScanViewportHeight_ != viewport()->height() ||
      visibilityScanViewportWidth_ != viewport()->width() ||
      visibilityScanContentHeight_ != contentHeight_)
    resetGraphVisibilityScan();
  if (visibilitySlotsRemaining_ == 0)
    return false;

  const std::size_t batchStartSection = visibilitySectionCursor_;
  const std::size_t batchStartSlot = visibilitySlotCursor_;
  const std::size_t batchStartRemaining = visibilitySlotsRemaining_;
  std::vector<TurnSectionWidget *> scannedSections;
  scannedSections.reserve(MaxVisibilitySectionChecksPerPass);
  std::vector<ScannedSlot> scanned;
  scanned.reserve(
      std::min(MaxVisibilitySlotChecksPerPass, visibilitySlotsRemaining_));
  const std::size_t scanLimit =
      std::min(MaxVisibilitySlotChecksPerPass, visibilitySlotsRemaining_);
  std::size_t sectionChecks = 0;
  while (scanned.size() < scanLimit &&
         sectionChecks < MaxVisibilitySectionChecksPerPass) {
    if (visibilitySectionCursor_ >= graphSections_.size())
      visibilitySectionCursor_ = 0;
    TurnSectionWidget *section = graphSections_[visibilitySectionCursor_];
    if (std::ranges::find(scannedSections, section) == scannedSections.end())
      scannedSections.push_back(section);
    if (visibilitySlotCursor_ >= section->cardSlots.size()) {
      visibilitySectionCursor_ =
          (visibilitySectionCursor_ + 1) % graphSections_.size();
      visibilitySlotCursor_ = 0;
      ++sectionChecks;
      continue;
    }
    scanned.push_back({section, &section->cardSlots[visibilitySlotCursor_]});
    ++visibilitySlotCursor_;
    if (visibilitySlotCursor_ >= section->cardSlots.size()) {
      visibilitySectionCursor_ =
          (visibilitySectionCursor_ + 1) % graphSections_.size();
      visibilitySlotCursor_ = 0;
      ++sectionChecks;
    }
    --visibilitySlotsRemaining_;
  }

  std::vector<TurnSectionWidget *> recoveredSections;
  const auto rememberSection = [](std::vector<TurnSectionWidget *> &sections,
                                  TurnSectionWidget *section) {
    if (section && std::ranges::find(sections, section) == sections.end())
      sections.push_back(section);
  };
  for (TurnSectionWidget *section : scannedSections) {
    if (section->historyPlaceholder)
      continue;
    section->historyPlaceholder = new GraphHistoryPlaceholder(section);
    section->historyPlaceholder->setItemCount(section->hiddenItemCount);
    rememberSection(recoveredSections, section);
  }
  for (const ScannedSlot &entry : scanned) {
    TurnSectionWidget *section = entry.section;
    TurnSectionWidget::CardSlot &slot = *entry.slot;
    if (slot.itemGuard) {
      slot.item = slot.itemGuard.data();
      continue;
    }
    if (slot.graphNode && slot.attachment &&
        slot.graphNode->uiAttachment() == slot.attachment.get())
      slot.graphNode->setUiAttachment(nullptr);
    slot.attachment.reset();
    slot.item = new MeasuredCardPlaceholder(
        slot.key, slot.projectionVisible ? slot.measuredHeight : 0, section);
    slot.itemGuard = slot.item;
    slot.item->setVisible(slot.projectionVisible);
    rememberSection(recoveredSections, section);
  }
  if (!recoveredSections.empty()) {
    for (TurnSectionWidget *section : recoveredSections)
      arrangeSection(section);
    recomputeGeometry();
  }

  const QRect visibleRect(0, scrollTop, std::max(1, content_->width()),
                          viewportHeight);
  const QRect materializationRect(0, std::max(0, scrollTop - viewportHeight),
                                  std::max(1, content_->width()),
                                  viewportHeight * 3);
  const QRect retentionRect(0, std::max(0, scrollTop - 2 * viewportHeight),
                            std::max(1, content_->width()), viewportHeight * 5);
  std::vector<Candidate> candidates;
  candidates.reserve(scanned.size());
  QWidget *focus = QApplication::focusWidget();
  const auto updateAttachmentVisibility = [this](const ScannedSlot &entry) {
    TurnSectionWidget::CardSlot &slot = *entry.slot;
    if (!slot.attachment)
      return;
    QWidget *widget = slot.attachment->widget.data();
    const bool visible =
        widget && widget->isVisibleTo(viewport()) &&
        QRect(widget->mapTo(viewport(), QPoint{}), widget->size())
            .intersects(viewport()->rect());
    setAttachmentViewportVisibility(*slot.attachment, visible);
  };
  for (const ScannedSlot &entry : scanned) {
    TurnSectionWidget::CardSlot &slot = *entry.slot;
    QWidget *item = slot.itemGuard.data();
    if (!item)
      continue;
    slot.item = item;
    const QRect itemRect(item->mapTo(content_, QPoint{}), item->size());
    const bool presented =
        slot.projectionVisible && item->isVisibleTo(content_);
    const bool wanted = presented && itemRect.intersects(materializationRect);
    const bool retained = presented && itemRect.intersects(retentionRect);
    const bool inViewport = presented && itemRect.intersects(visibleRect);
    const int center = itemRect.center().y();
    const int distance =
        center < visibleRect.top()
            ? visibleRect.top() - center
            : (center > visibleRect.bottom() ? center - visibleRect.bottom()
                                             : 0);
    QWidget *widget =
        slot.attachment ? slot.attachment->widget.data() : nullptr;
    if (widget) {
      const bool ownsFocus =
          focus && (focus == widget || widget->isAncestorOf(focus));
      if (!retained && !ownsFocus) {
        candidates.push_back(
            {entry.section, &slot, Operation::Release, false, distance});
      } else if (wanted) {
        candidates.push_back({entry.section, &slot, Operation::Render,
                              inViewport, distance,
                              slot.attachment->renderedRevision});
      }
    } else if (wanted) {
      candidates.push_back(
          {entry.section, &slot, Operation::Materialize, inViewport, distance});
    }
    updateAttachmentVisibility(entry);
  }

  const bool needsGraphRead =
      std::ranges::any_of(candidates, [](const Candidate &candidate) {
        return candidate.operation != Operation::Release;
      });
  if (needsGraphRead) {
    std::optional<nodegraph::NodeGraph::ReadAccess> read = graph_->tryRead();
    if (!read) {
      visibilitySectionCursor_ = batchStartSection;
      visibilitySlotCursor_ = batchStartSlot;
      visibilitySlotsRemaining_ = batchStartRemaining;
      return true;
    }
    for (Candidate &candidate : candidates) {
      if (candidate.operation == Operation::Release ||
          !candidate.slot->graphNode)
        continue;
      if (read->removed(candidate.slot->graphNode)) {
        candidate.operation = Operation::Release;
        continue;
      }
      candidate.nodeRevision = read->changedRevision(candidate.slot->graphNode);
      if (candidate.operation == Operation::Render &&
          candidate.renderedRevision >= candidate.nodeRevision)
        continue;
      candidate.state = read->state(candidate.slot->graphNode);
    }
  }
  std::erase_if(candidates, [](const Candidate &candidate) {
    return candidate.operation == Operation::Render && !candidate.state;
  });

  std::ranges::stable_sort(
      candidates, [](const Candidate &left, const Candidate &right) {
        if (left.inViewport != right.inViewport)
          return left.inViewport > right.inViewport;
        const auto urgency = [](Operation operation) {
          return operation == Operation::Release ? 1 : 0;
        };
        if (urgency(left.operation) != urgency(right.operation))
          return urgency(left.operation) < urgency(right.operation);
        return left.distance < right.distance;
      });
  if (candidates.empty())
    return !recoveredSections.empty() || visibilitySlotsRemaining_ != 0;

  Anchor anchor;
  anchor.absoluteValue = verticalScrollBar()->value();
  TurnSectionWidget::CardSlot *anchorSlot = nullptr;
  int anchorContentTop = std::numeric_limits<int>::max();
  for (const ScannedSlot &entry : scanned) {
    TurnSectionWidget::CardSlot &slot = *entry.slot;
    QWidget *item = slot.itemGuard.data();
    if (!item || !slot.projectionVisible || !item->isVisibleTo(content_))
      continue;
    const int viewportTop = item->mapTo(viewport(), QPoint{}).y();
    if (viewportTop + item->height() < 0)
      continue;
    const int contentTop = item->mapTo(content_, QPoint{}).y();
    if (contentTop >= anchorContentTop)
      continue;
    anchorContentTop = contentTop;
    anchorSlot = &slot;
    anchor.stableKey = slot.key;
    anchor.pixelOffset = viewportTop;
  }

  const bool follow = mode_ == Mode::Following;
  const bool followedBottom = follow && isAtBottom();
  const int previousValue = verticalScrollBar()->value();
  stopFollowingAnimation();
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  std::vector<std::pair<ConversationCard *, CommandOutputView::ScrollState>>
      outputRestorations;
  const auto configureCard =
      [this, &outputRestorations](ConversationCard *card,
                                  TurnSectionWidget::CardSlot &slot) {
        card->setProperty("conversationAnchorKey",
                          QString::fromStdString(slot.key));
        if (const auto collapsed = cardCollapsedStates_.find(slot.key);
            collapsed != cardCollapsedStates_.end())
          card->setCollapsed(collapsed->second);
        connect(card, &ConversationCard::foldRequested, this,
                [this, key = slot.key, card](bool collapsed) {
                  if (cardForStableKey(key) == card)
                    setCardCollapsed(key, card, collapsed);
                });
        connect(card, &ConversationCard::recoveryRequested, this,
                [this, node = slot.graphNode] {
                  if (promptRecoveryAction_)
                    promptRecoveryAction_(node);
                });
        if (const auto saved = commandOutputStates_.find(slot.key);
            saved != commandOutputStates_.end()) {
          outputRestorations.emplace_back(card, saved->second);
          commandOutputStates_.erase(saved);
        }
      };
  int operations = 0;
  bool workRemaining = false;
  std::vector<TurnSectionWidget *> changedSections;

  for (Candidate &candidate : candidates) {
    if (operations >= MaxCardOperationsPerPass) {
      workRemaining = true;
      break;
    }
    TurnSectionWidget::CardSlot &slot = *candidate.slot;
    if (candidate.operation == Operation::Materialize) {
      if (!candidate.state)
        continue;
      auto *placeholder = dynamic_cast<MeasuredCardPlaceholder *>(slot.item);
      if (!placeholder)
        continue;
      VisibleCardData data =
          graphCardData(slot.graphNode, graphThread_->id().canonical,
                        candidate.section->protocolId, *candidate.state);
      if (slot.promptVisualId)
        data.key = LocalPromptKey{*slot.promptVisualId};
      auto *card = createConversationCard(
          data, candidate.section,
          !presentationOptions_.commandsInitiallyExpanded,
          !presentationOptions_.imagesInitiallyExpanded);
      configureCard(card, slot);
      card->setVisible(slot.projectionVisible);
      slot.item = card;
      slot.itemGuard = card;
      slot.attachment = std::make_unique<ui::QtNodeAttachment>();
      slot.attachment->widget = card;
      slot.attachment->renderedRevision = candidate.nodeRevision;
      setAttachmentViewportVisibility(*slot.attachment, candidate.inViewport);
      Q_ASSERT(slot.graphNode->uiAttachment() == nullptr);
      slot.graphNode->setUiAttachment(slot.attachment.get());
      delete placeholder;
      rememberSection(changedSections, candidate.section);
      ++operations;
      continue;
    }

    auto *card =
        slot.attachment
            ? qobject_cast<ConversationCard *>(slot.attachment->widget.data())
            : nullptr;
    if (candidate.operation == Operation::Render) {
      if (!card || !candidate.state)
        continue;
      VisibleCardData data =
          graphCardData(slot.graphNode, graphThread_->id().canonical,
                        candidate.section->protocolId, *candidate.state);
      if (slot.promptVisualId)
        data.key = LocalPromptKey{*slot.promptVisualId};
      if (!card->canApply(data)) {
        if (operations + 2 > MaxCardOperationsPerPass) {
          workRemaining = true;
          break;
        }
        const auto outputState = card->commandOutputScrollState();
        if (outputState && !outputState->followsLatest)
          commandOutputStates_[slot.key] = *outputState;
        if (slot.key == candidate.section->rootKey)
          card->setNestedItems({});
        if (slot.graphNode->uiAttachment() == slot.attachment.get())
          slot.graphNode->setUiAttachment(nullptr);
        slot.attachment.reset();
        delete card;
        auto *replacement = createConversationCard(
            data, candidate.section,
            !presentationOptions_.commandsInitiallyExpanded,
            !presentationOptions_.imagesInitiallyExpanded);
        configureCard(replacement, slot);
        slot.item = replacement;
        slot.itemGuard = replacement;
        slot.attachment = std::make_unique<ui::QtNodeAttachment>();
        slot.attachment->widget = replacement;
        Q_ASSERT(slot.graphNode->uiAttachment() == nullptr);
        slot.graphNode->setUiAttachment(slot.attachment.get());
        card = replacement;
        operations += 2;
      } else {
        card->apply(data);
        ++operations;
      }
      card->setVisible(slot.projectionVisible);
      slot.attachment->renderedRevision = candidate.nodeRevision;
      setAttachmentViewportVisibility(*slot.attachment, candidate.inViewport);
      rememberSection(changedSections, candidate.section);
      continue;
    }

    if (!card)
      continue;
    const auto outputState = card->commandOutputScrollState();
    if (outputState && !outputState->followsLatest)
      commandOutputStates_[slot.key] = *outputState;
    else
      commandOutputStates_.erase(slot.key);
    if (slot.key == candidate.section->rootKey) {
      card->setNestedItems({});
      card->setMinimumHeight(0);
      if (card->layout()) {
        card->layout()->invalidate();
        card->layout()->activate();
      }
      slot.measuredHeight =
          std::max(slot.measuredHeight, card->minimumSizeHint().height());
    } else {
      slot.measuredHeight = std::max(1, card->height());
    }
    auto *placeholder = new MeasuredCardPlaceholder(
        slot.key, slot.projectionVisible ? slot.measuredHeight : 0,
        candidate.section);
    placeholder->setVisible(slot.projectionVisible);
    if (slot.graphNode->uiAttachment() == slot.attachment.get())
      slot.graphNode->setUiAttachment(nullptr);
    slot.attachment.reset();
    slot.item = placeholder;
    slot.itemGuard = placeholder;
    delete card;
    rememberSection(changedSections, candidate.section);
    ++operations;
  }

  if (workRemaining) {
    visibilitySectionCursor_ = batchStartSection;
    visibilitySlotCursor_ = batchStartSlot;
    visibilitySlotsRemaining_ = batchStartRemaining;
  }
  for (TurnSectionWidget *section : changedSections)
    arrangeSection(section);
  recomputeGeometry();
  for (const auto &[card, state] : outputRestorations)
    card->restoreCommandOutputScrollState(state);
  if (followedBottom)
    setScrollValue(verticalScrollBar()->maximum());
  else {
    int value = anchor.absoluteValue;
    if (anchorSlot && !anchor.stableKey.empty()) {
      if (QWidget *item = anchorSlot->itemGuard.data())
        value = item->mapTo(content_, QPoint{}).y() - anchor.pixelOffset;
    }
    setScrollValue(std::clamp(value, verticalScrollBar()->minimum(),
                              verticalScrollBar()->maximum()));
  }
  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();

  for (const ScannedSlot &entry : scanned)
    updateAttachmentVisibility(entry);
  if (follow && !followedBottom) {
    const int stableValue = verticalScrollBar()->value();
    if (verticalScrollBar()->maximum() > stableValue + 3)
      animateToBottom(std::min(previousValue, stableValue));
  }
  return workRemaining || visibilitySlotsRemaining_ != 0 || operations > 0;
}

void ConversationView::setCardCollapsed(const std::string &key,
                                        ConversationCard *card,
                                        bool collapsed) {
  if (!card || card->isCollapsed() == collapsed)
    return;

  const int titleTop = card->mapTo(viewport(), QPoint{}).y();
  stopFollowingAnimation();
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());

  mode_ = Mode::Paused;
  pausedByComposerGrowth_ = false;
  cardCollapsedStates_[key] = collapsed;
  ConversationCard *turnContainer =
      card->property("turnContainer").toBool() ? card : nullptr;
  for (QWidget *parent = card->parentWidget(); !turnContainer && parent;
       parent = parent->parentWidget())
    if (auto *candidate = dynamic_cast<ConversationCard *>(parent);
        candidate && candidate->property("turnContainer").toBool())
      turnContainer = candidate;
  if (turnContainer)
    turnContainer->setMinimumHeight(0);
  card->setCollapsed(collapsed);
  recomputeGeometry();
  const auto updateMeasuredHeight = [&key, card](TurnSectionWidget *section) {
    const auto slot = std::ranges::find_if(
        section->cardSlots,
        [&key](const TurnSectionWidget::CardSlot &candidate) {
          return candidate.key == key;
        });
    if (slot != section->cardSlots.end() && key != section->rootKey)
      slot->measuredHeight = std::max(1, card->height());
  };
  for (TurnSectionWidget *section : graphSections_)
    updateMeasuredHeight(section);
  const int visibleHeight =
      std::max(0, viewport()->height() - trailingSpaceHeight_);
  const int visibleTop =
      collapsed ? titleTop
                : std::clamp(titleTop, 0,
                             std::max(0, visibleHeight - card->height()));
  setScrollValue(card->mapTo(content_, QPoint{}).y() - visibleTop);

  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
  scheduleVisibilityPass();
  storeCurrentThreadState();
}

void ConversationView::setTrailingSpaceHeight(int height) {
  height = std::max(0, height);
  if (height == trailingSpaceHeight_)
    return;

  const bool grew = height > trailingSpaceHeight_;
  const Anchor anchor = captureAnchor();
  const int previousValue = verticalScrollBar()->value();
  stopFollowingAnimation();

  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());

  if (grew) {
    pausedByComposerGrowth_ =
        pausedByComposerGrowth_ || mode_ == Mode::Following;
    mode_ = Mode::Paused;
  }
  trailingSpaceHeight_ = height;
  QScrollBar *conversationScrollBar = verticalScrollBar();
  conversationScrollBar->setProperty("composerBottomInset", height);
  conversationScrollBar->setStyleSheet(
      height == 0
          ? QString{}
          : QStringLiteral("QScrollBar:vertical{margin:2px 2px %1px 2px;}")
                .arg(height + 2));
  recomputeGeometry();
  if (mode_ == Mode::Following)
    setScrollValue(verticalScrollBar()->maximum());
  else if (grew && anchor.stableKey.empty())
    setScrollValue(std::min(previousValue, verticalScrollBar()->maximum()));
  else
    restoreAnchor(anchor);
  if (!grew && isAtBottom()) {
    mode_ = Mode::Following;
    pausedByComposerGrowth_ = false;
    restoreRequestedGraphHistoryLimit();
  }

  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
  storeCurrentThreadState();
}

void ConversationView::prepareForLocalPromptAdmission() {
  if (mode_ != Mode::Paused || !pausedByComposerGrowth_)
    return;
  mode_ = Mode::Following;
  pausedByComposerGrowth_ = false;
  restoreRequestedGraphHistoryLimit();
  storeCurrentThreadState();
}

bool ConversationView::forwardWheelEvent(QWheelEvent *event) {
  return event && applyWheel(event);
}

bool ConversationView::isAtBottom() const noexcept {
  return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
}

ConversationView::Mode
ConversationView::modeForThread(const std::string &threadId) const noexcept {
  if (threadId == threadId_)
    return mode_;
  const auto saved = threadStates_.find(threadId);
  return saved == threadStates_.end() ? Mode::Following : saved->second.mode;
}

bool ConversationView::eventFilter(QObject *watched, QEvent *event) {
  if (watched == content_ && event->type() == QEvent::LayoutRequest &&
      !applying_) {
    Anchor anchor = captureAnchor();
    if (mode_ == Mode::Paused) {
      const auto retained = threadStates_.find(threadId_);
      if (retained != threadStates_.end() &&
          !retained->second.anchor.stableKey.empty())
        anchor = retained->second.anchor;
    }
    const bool follow = mode_ == Mode::Following;
    stopFollowingAnimation();
    applying_ = true;
    viewport()->setUpdatesEnabled(false);
    const QSignalBlocker scrollSignals(verticalScrollBar());
    recomputeGeometry();
    restoreAnchor(anchor);
    applying_ = false;
    viewport()->setUpdatesEnabled(true);
    viewport()->update();
    const int stableValue = verticalScrollBar()->value();
    if (follow && verticalScrollBar()->maximum() > stableValue + 3)
      animateToBottom(stableValue);
    else if (follow)
      setScrollValue(verticalScrollBar()->maximum());
    scheduleVisibilityPass();
    storeCurrentThreadState();
    return true;
  }
  return QAbstractScrollArea::eventFilter(watched, event);
}

void ConversationView::resizeEvent(QResizeEvent *event) {
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  stopFollowingAnimation();
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  QAbstractScrollArea::resizeEvent(event);
  recomputeGeometry();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  applying_ = false;
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
  scheduleVisibilityPass();
  storeCurrentThreadState();
}

void ConversationView::wheelEvent(QWheelEvent *event) {
  if (!applyWheel(event))
    QAbstractScrollArea::wheelEvent(event);
}

ConversationView::Anchor ConversationView::captureAnchor() const {
  Anchor anchor;
  anchor.absoluteValue = verticalScrollBar()->value();
  const auto capture = [this, &anchor](const std::string &key) {
    QWidget *item = itemForStableKey(key);
    if (!item || !item->isVisible())
      return false;
    const int viewportTop = item->mapTo(viewport(), QPoint(0, 0)).y();
    if (viewportTop + item->height() < 0)
      return false;
    anchor.stableKey = key;
    // The contract is visual stability. Capture the actual painted offset
    // instead of deriving it from content coordinates while a layout/range
    // transaction may temporarily be between those coordinate systems.
    anchor.pixelOffset = viewportTop;
    return true;
  };
  for (TurnSectionWidget *section : graphSections_) {
    if (!section->rootKey.empty() && capture(section->rootKey))
      return anchor;
    for (const TurnSectionWidget::CardSlot &slot : section->cardSlots)
      if (slot.key != section->rootKey && slot.projectionVisible &&
          capture(slot.key))
        return anchor;
  }
  return anchor;
}

void ConversationView::restoreAnchor(const Anchor &anchor) {
  int value = anchor.absoluteValue;
  if (!anchor.stableKey.empty()) {
    if (QWidget *item = itemForStableKey(anchor.stableKey)) {
      const int top = item->mapTo(content_, QPoint(0, 0)).y();
      value = top - anchor.pixelOffset;
    }
  }
  setScrollValue(std::clamp(value, verticalScrollBar()->minimum(),
                            verticalScrollBar()->maximum()));
}

void ConversationView::setScrollValue(int value) {
  value = std::clamp(value, verticalScrollBar()->minimum(),
                     verticalScrollBar()->maximum());
  programmaticScroll_ = true;
  verticalScrollBar()->setValue(value);
  programmaticScroll_ = false;
  positionContent();
  scheduleVisibilityPass();
}

void ConversationView::stopFollowingAnimation() {
  if (followAnimation_->state() != QAbstractAnimation::Stopped)
    followAnimation_->stop();
}

void ConversationView::restoreRequestedGraphHistoryLimit() {
  if (graphHistoryLimit_ == graphRequestedHistoryLimit_)
    return;
  graphHistoryLimit_ = graphRequestedHistoryLimit_;
  scheduleGraphRefresh();
}

void ConversationView::animateToBottom(int previousValue) {
  if (mode_ != Mode::Following)
    return;
  const int destination = verticalScrollBar()->maximum();
  const int start =
      std::clamp(std::max(verticalScrollBar()->value(), previousValue),
                 verticalScrollBar()->minimum(), destination);
  const int distance = destination - start;
  stopFollowingAnimation();
  if (distance <= 3) {
    setScrollValue(destination);
    return;
  }
  setScrollValue(start);
  followAnimation_->setDuration(std::clamp(110 + distance / 3, 130, 260));
  followAnimation_->setStartValue(start);
  followAnimation_->setEndValue(destination);
  followAnimation_->start();
}

void ConversationView::recomputeGeometry() {
  if (!content_ || !viewport())
    return;
  std::vector<TurnSectionWidget *> layoutSections = graphSections_;
  std::vector<ConversationCard *> layoutCards;
  for (TurnSectionWidget *section : graphSections_)
    for (const TurnSectionWidget::CardSlot &slot : section->cardSlots)
      if (slot.attachment)
        if (auto *card = qobject_cast<ConversationCard *>(
                slot.attachment->widget.data()))
          layoutCards.push_back(card);
  const int width = std::max(0, viewport()->width());
  trailingSpace_->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
  contentLayout_->invalidate();
  for (TurnSectionWidget *section : layoutSections)
    section->setMinimumHeight(0);

  // Give every nested layout its final width before asking for height.  This
  // makes wrapped labels and command output contribute to the same range
  // transaction as their insertion/update.
  content_->resize(width, std::max(viewport()->height(), contentHeight_));
  contentLayout_->setGeometry(content_->rect());
  for (TurnSectionWidget *section : layoutSections)
    section->layout()->activate();
  const auto activateCard = [](ConversationCard *card) {
    if (!card)
      return;
    if (QWidget *cardContent = card->findChild<QWidget *>(
            QStringLiteral("conversationCardContent"),
            Qt::FindDirectChildrenOnly);
        cardContent && cardContent->layout())
      cardContent->layout()->activate();
    if (card->layout())
      card->layout()->activate();
  };
  const auto settleCardHeight = [&activateCard](ConversationCard *card,
                                                int cardWidth) {
    if (!card || !card->layout())
      return;
    cardWidth = std::max(0, cardWidth);
    card->setMinimumHeight(0);
    // Retained rich text is created and nested in one transaction. Establish
    // its real width before measuring so QLabel cannot reuse pre-nesting
    // document geometry until a later streamed update.
    card->resize(cardWidth, card->height());
    card->layout()->invalidate();
    card->layout()->setGeometry(card->contentsRect());
    activateCard(card);
    card->updateGeometry();
    const int cardHeight =
        card->layout()->hasHeightForWidth()
            ? card->layout()->heightForWidth(cardWidth) + 2 * card->frameWidth()
            : card->sizeHint().height();
    card->setMinimumHeight(cardHeight);
    card->resize(cardWidth, cardHeight);
    card->layout()->setGeometry(card->contentsRect());
    activateCard(card);
  };
  for (ConversationCard *card : layoutCards)
    activateCard(card);
  // Child/subagent threads may have no visible You root. Their cards live
  // directly in a turn section, so settle them at the final section width
  // just as deliberately as cards nested inside a normal turn container.
  for (ConversationCard *card : layoutCards) {
    if (card->property("turnContainer").toBool() ||
        card->property("nestedConversationCard").toBool())
      continue;
    const int cardWidth = card->parentWidget()
                              ? card->parentWidget()->contentsRect().width()
                              : card->width();
    settleCardHeight(card, cardWidth);
  }
  // A You turn container adds one real layout depth. Settle that depth in
  // dependency order so newly nested cards reach their final height inside
  // this transaction instead of posting a second visible LayoutRequest.
  for (ConversationCard *card : layoutCards) {
    if (!card->property("turnContainer").toBool())
      continue;
    card->setMinimumHeight(0);
    QWidget *nested = card->findChild<QWidget *>(
        QStringLiteral("conversationNestedCards"), Qt::FindDirectChildrenOnly);
    if (!nested || !nested->layout())
      continue;
    const int cardWidth = card->parentWidget()
                              ? card->parentWidget()->contentsRect().width()
                              : card->width();
    card->resize(cardWidth, card->height());
    if (card->layout()) {
      card->layout()->invalidate();
      card->layout()->setGeometry(card->contentsRect());
      card->layout()->activate();
    }
    nested->layout()->activate();
    for (int index = 0; index < nested->layout()->count(); ++index) {
      auto *nestedCard = dynamic_cast<ConversationCard *>(
          nested->layout()->itemAt(index)->widget());
      if (!nestedCard)
        continue;
      const int nestedWidth = nested->contentsRect().width();
      settleCardHeight(nestedCard, nestedWidth);
    }
    nested->layout()->invalidate();
    const int nestedHeight =
        nested->isHidden() ? 0 : nested->layout()->minimumSize().height();
    nested->setFixedHeight(nestedHeight);
    nested->layout()->setGeometry(nested->contentsRect());
    nested->updateGeometry();
    nested->layout()->invalidate();
    nested->layout()->activate();
    settleCardHeight(card, cardWidth);
  }
  for (TurnSectionWidget *section : layoutSections) {
    section->layout()->invalidate();
    const int sectionHeight = section->layout()->minimumSize().height();
    section->setMinimumHeight(sectionHeight);
    section->resize(section->width(), sectionHeight);
    section->layout()->setGeometry(section->contentsRect());
    section->updateGeometry();
    section->layout()->activate();
  }
  contentLayout_->invalidate();
  contentLayout_->setGeometry(content_->rect());
  contentLayout_->activate();

  int wanted = contentLayout_->hasHeightForWidth()
                   ? contentLayout_->heightForWidth(width)
                   : contentLayout_->sizeHint().height();
  wanted = std::max(wanted, contentLayout_->minimumSize().height());
  naturalContentHeight_ = wanted;
  trailingSpace_->changeSize(0, trailingSpaceHeight_, QSizePolicy::Minimum,
                             QSizePolicy::Fixed);
  contentLayout_->invalidate();
  wanted += trailingSpaceHeight_;
  contentHeight_ = std::max(viewport()->height(), wanted);
  content_->resize(width, contentHeight_);
  contentLayout_->setGeometry(QRect(0, 0, width, contentHeight_));
  for (TurnSectionWidget *section : layoutSections)
    section->layout()->activate();
  contentLayout_->activate();

  verticalScrollBar()->setPageStep(viewport()->height());
  verticalScrollBar()->setRange(
      0, std::max(0, contentHeight_ - viewport()->height()));
  positionContent();

  for (ConversationCard *card : layoutCards) {
    if (QWidget *nested = card->findChild<QWidget *>(
            QStringLiteral("conversationNestedCards"),
            Qt::FindDirectChildrenOnly))
      QCoreApplication::sendPostedEvents(nested, QEvent::LayoutRequest);
    QCoreApplication::sendPostedEvents(card, QEvent::LayoutRequest);
  }
  for (TurnSectionWidget *section : layoutSections)
    QCoreApplication::sendPostedEvents(section, QEvent::LayoutRequest);
  QCoreApplication::sendPostedEvents(content_, QEvent::LayoutRequest);
}

void ConversationView::positionContent() {
  if (content_)
    content_->move(0, -verticalScrollBar()->value());
}

void ConversationView::handleUserScrollValue(int value) {
  stopFollowingAnimation();
  pausedByComposerGrowth_ = false;
  mode_ = value >= verticalScrollBar()->maximum() - 1 ? Mode::Following
                                                      : Mode::Paused;
  if (mode_ == Mode::Following)
    restoreRequestedGraphHistoryLimit();
  storeCurrentThreadState();
}

bool ConversationView::applyWheel(QWheelEvent *event) {
  if (!event)
    return false;
  const int intent = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                   : event->angleDelta().y();
  if (intent == 0)
    return false;

  pausedByComposerGrowth_ = false;
  const int oldValue = verticalScrollBar()->value();
  if (intent > 0) {
    // An upward wheel/touchpad gesture pauses before any subsequent layout or
    // incoming frame can move the viewport.
    stopFollowingAnimation();
    mode_ = Mode::Paused;
  }
  // Keep Qt's native wheel/touchpad interpretation, but deliver it directly
  // to the scrollbar. Calling QAbstractScrollArea::wheelEvent() here would
  // redispatch through ShellWidget's application event filter, which routes
  // the same gesture back into this method recursively.
  QScrollBar *bar = verticalScrollBar();
  const QPointF local = bar->mapFromGlobal(event->globalPosition().toPoint());
  QWheelEvent forwarded(local, event->globalPosition(), event->pixelDelta(),
                        event->angleDelta(), event->buttons(),
                        event->modifiers(), event->phase(), event->inverted());
  const QScopedValueRollback nativeDispatch(dispatchingNativeWheel_, true);
  QApplication::sendEvent(bar, &forwarded);
  positionContent();
  if (verticalScrollBar()->value() < oldValue)
    mode_ = Mode::Paused;
  if (verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1) {
    mode_ = Mode::Following;
    restoreRequestedGraphHistoryLimit();
  }
  storeCurrentThreadState();
  event->accept();
  return true;
}

ConversationCard *
ConversationView::cardForStableKey(const std::string &key) const {
  for (TurnSectionWidget *section : graphSections_)
    for (const TurnSectionWidget::CardSlot &slot : section->cardSlots)
      if (slot.key == key && slot.attachment)
        return qobject_cast<ConversationCard *>(slot.attachment->widget.data());
  return nullptr;
}

QWidget *ConversationView::itemForStableKey(const std::string &key) const {
  for (TurnSectionWidget *section : graphSections_)
    for (const TurnSectionWidget::CardSlot &slot : section->cardSlots)
      if (slot.key == key) {
        if (slot.attachment && slot.attachment->widget)
          return slot.attachment->widget.data();
        return slot.itemGuard.data();
      }
  return nullptr;
}

} // namespace codexui::codex::middle
