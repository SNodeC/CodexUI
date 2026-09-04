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
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int CardSpacing = 8;
constexpr int NativeScrollLineStep = 20;
constexpr int MaxCardOperationsPerPass = 8;
constexpr std::size_t MaxStructureRecordsPerPass = 64;
constexpr std::size_t MaxGeometryRecordsPerPass = 32;
constexpr std::size_t MaxGeometryEvictionsPerPass = 64;
constexpr std::size_t MaxPendingAffectedNodes = 64;
constexpr std::size_t MaxVisibilitySlotChecksPerPass = 64;
constexpr std::size_t MaxVisibilitySectionChecksPerPass = 64;
constexpr int GraphContentionRetryMilliseconds = 4;
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
    Q_ASSERT(count == 0);
    setPixelExtent(0, 0);
  }

  void setPixelExtent(std::size_t historyCount, std::int64_t pixelExtent) {
    setProperty("hiddenItemCount", QVariant::fromValue<qulonglong>(
                                       static_cast<qulonglong>(historyCount)));
    const int height = static_cast<int>(std::clamp<std::int64_t>(
        pixelExtent, 0, std::numeric_limits<int>::max()));
    setFixedHeight(height);
    setVisible(height != 0);
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
  case CardKind::LocalPrompt:
    return 72;
  case CardKind::AgentMessage:
    return 58;
  case CardKind::CommandExecution:
  case CardKind::AgentActivity:
  case CardKind::Reasoning:
  case CardKind::FileChanges:
  case CardKind::ImageGeneration:
  case CardKind::Plan:
  case CardKind::GenericActivity:
    return 48;
  }
  return 48;
}

int intrinsicGraphCardHeight(ConversationCard *card) {
  if (!card)
    return 1;
  int height = std::max(1, card->height());
  if (!card->property("turnContainer").toBool())
    return height;
  QWidget *nested = card->findChild<QWidget *>(
      QStringLiteral("conversationNestedCards"), Qt::FindDirectChildrenOnly);
  if (!nested || nested->isHidden())
    return height;
  const int spacing =
      card->layout() ? std::max(0, card->layout()->spacing()) : 0;
  return std::max(1, height - nested->height() - spacing);
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
    bool cardGeometryDirty = true;
  };

  explicit TurnSectionWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("conversationTurnSection"));
    setAttribute(Qt::WA_StyledBackground, false);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    cards = new QVBoxLayout(this);
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(CardSpacing);
  }

  QVBoxLayout *cards = nullptr;
  std::vector<CardSlot> cardSlots;
  QPointer<GraphHistoryPlaceholder> historyPlaceholder;
  std::string rootKey;
  std::string authoritativeRootKey;
  std::string protocolId;
  nodegraph::NodeRef graphNode;
  bool graphActive = false;
  bool hasAuthoritativeRoot = false;
  bool layoutDirty = true;
  bool geometryDirty = true;
};

class ConversationView::GraphViewportGeometry final {
public:
  struct TurnGeometry;

  struct ItemGeometry final {
    nodegraph::NodeRef node;
    nodegraph::NodeRef materializedPrompt;
    TurnGeometry *turn = nullptr;
    std::string key;
    std::optional<std::uint64_t> promptVisualId;
    int measuredHeight = 0;
    bool projectionVisible = true;
    bool countedOutsideHistory = false;
  };

  struct TurnGeometry final {
    nodegraph::NodeRef node;
    nodegraph::NodeRef root;
    std::string protocolId;
    std::unique_ptr<ItemGeometry> rootItem;
    std::deque<std::unique_ptr<ItemGeometry>> items;
    std::deque<std::unique_ptr<ItemGeometry>> appendedItems;
    std::size_t hiddenBeforeItems = 0;
    std::size_t knownChildCount = 0;
    nodegraph::NodeRef newestChild;
    std::int64_t pixelExtent = 0;
    std::int64_t rootPixelExtent = 0;
    std::uint64_t structureRevision = 0;
    bool active = false;
    bool rootIsChild = false;
    bool rootCollapsed = false;
  };

  struct ScanFrontier final {
    std::size_t nextTurnIndex = 0;
    TurnGeometry *turn = nullptr;
    std::size_t nextItemIndex = 0;
    std::size_t selectedInTurn = 0;
    std::size_t directItemsInTurn = 0;
    bool rootConsumed = false;
    bool initialized = false;
    bool complete = false;
  };

  struct DesiredGeometry final {
    ItemGeometry *record = nullptr;
    std::int64_t top = 0;
    std::int64_t bottom = 0;
  };

  struct GeometryFrontier final {
    bool active = false;
    bool valid = false;
    bool reverse = false;
    std::uint64_t generation = 0;
    std::int64_t targetTop = 0;
    std::int64_t targetBottom = 0;
    std::int64_t position = 0;
    std::size_t turnIndex = 0;
    std::size_t itemIndex = 0;
    std::uint8_t phase = 0;
    std::vector<DesiredGeometry> found;
  };

  // Reset storage is retired in O(1) after releasing NodeGraph::ReadAccess,
  // then destroyed in small Qt-event-loop slices. It is cleanup state only;
  // no rendering or lookup ever consults it.
  struct RetiredStorage final {
    std::deque<std::unique_ptr<TurnGeometry>> turns;
    std::map<const nodegraph::Node *, ItemGeometry *> itemIndex;
    std::map<const nodegraph::Node *, TurnGeometry *> turnIndex;
  };

  std::deque<std::unique_ptr<TurnGeometry>> turns;
  std::map<const nodegraph::Node *, ItemGeometry *> itemIndex;
  std::map<const nodegraph::Node *, TurnGeometry *> turnIndex;
  std::vector<nodegraph::NodeRef> affected;
  std::size_t affectedCursor = 0;
  ScanFrontier scan;
  GeometryFrontier geometryScan;
  std::deque<RetiredStorage> retiredStorage;
  std::uint64_t geometryGeneration = 1;
  std::uint64_t threadStructureRevision = 0;
  std::size_t threadChildCount = 0;
  nodegraph::NodeRef newestTurn;
  std::size_t retainedHistoryItems = 0;
  std::size_t retainedGeometryRecords = 0;
  std::size_t extraPinnedRoots = 0;
  std::size_t targetHistoryItems = 0;
  std::size_t totalItems = 0;
  std::int64_t totalPixelExtent = 0;
  std::int64_t leadingPixelExtent = 0;
  std::int64_t trailingPixelExtent = 0;
  std::size_t lastStructureReadsPerPass = 0;
  std::size_t lastGeometryRecordsPerPass = 0;
  std::size_t lastCardOperationsPerPass = 0;
  std::size_t maxStructureReadsPerPass = 0;
  std::size_t maxGeometryRecordsPerPass = 0;
  std::size_t maxCardOperationsPerPass = 0;
  std::size_t contentionRetryCount = 0;
  std::size_t retiredRecordCount = 0;
  std::size_t lastRetiredCleanupOperations = 0;
  std::size_t maxRetiredCleanupOperations = 0;
  std::size_t refreshStructureReads = 0;
  bool targetDirty = true;
  bool forceStructureCheck = false;
  bool forceSelectedReset = false;
  std::size_t structureValidationCursor = 0;
  std::size_t evictionTurnCursor = 0;
  std::uint64_t structureRequestGeneration = 0;
  std::uint64_t validationGeneration = 0;
  bool projectionRefreshPending = false;
  const nodegraph::Node *projectionCursor = nullptr;
  bool geometryRerunRequired = false;

  [[nodiscard]] static std::int64_t
  rawItemExtent(const ItemGeometry &record) noexcept {
    return record.projectionVisible
               ? std::max(1, record.measuredHeight) + CardSpacing
               : 0;
  }

  [[nodiscard]] static bool
  suppressesChildren(const TurnGeometry &turn) noexcept {
    return turn.rootCollapsed && turn.rootItem && turn.rootItem->node &&
           turn.rootItem->projectionVisible;
  }

  [[nodiscard]] static std::int64_t
  effectiveTurnExtent(const TurnGeometry &turn) noexcept {
    return suppressesChildren(turn) ? turn.rootPixelExtent : turn.pixelExtent;
  }

  void addRecordExtent(ItemGeometry &record) {
    TurnGeometry &turn = *record.turn;
    const std::int64_t before = effectiveTurnExtent(turn);
    const std::int64_t extent = rawItemExtent(record);
    turn.pixelExtent += extent;
    if (turn.rootItem.get() == &record)
      turn.rootPixelExtent += extent;
    totalPixelExtent += effectiveTurnExtent(turn) - before;
    ++geometryGeneration;
  }

  void setRecordProjectionVisible(ItemGeometry &record, bool visible) {
    if (record.projectionVisible == visible)
      return;
    TurnGeometry &turn = *record.turn;
    const std::int64_t before = effectiveTurnExtent(turn);
    const std::int64_t oldExtent = rawItemExtent(record);
    record.projectionVisible = visible;
    const std::int64_t delta = rawItemExtent(record) - oldExtent;
    turn.pixelExtent += delta;
    if (turn.rootItem.get() == &record)
      turn.rootPixelExtent += delta;
    totalPixelExtent += effectiveTurnExtent(turn) - before;
    ++geometryGeneration;
  }

  void setRecordMeasuredHeight(ItemGeometry &record, int height) {
    height = std::max(1, height);
    if (record.measuredHeight == height)
      return;
    TurnGeometry &turn = *record.turn;
    const std::int64_t before = effectiveTurnExtent(turn);
    const std::int64_t oldExtent = rawItemExtent(record);
    record.measuredHeight = height;
    const std::int64_t delta = rawItemExtent(record) - oldExtent;
    turn.pixelExtent += delta;
    if (turn.rootItem.get() == &record)
      turn.rootPixelExtent += delta;
    totalPixelExtent += effectiveTurnExtent(turn) - before;
    ++geometryGeneration;
  }

  void removeRecordExtent(ItemGeometry &record) {
    setRecordProjectionVisible(record, false);
  }

  void setHiddenBeforeItems(TurnGeometry &turn, std::size_t count) {
    if (turn.hiddenBeforeItems == count)
      return;
    const std::int64_t before = effectiveTurnExtent(turn);
    turn.pixelExtent += (static_cast<std::int64_t>(count) -
                         static_cast<std::int64_t>(turn.hiddenBeforeItems)) *
                        EstimatedGraphHistoryItemExtent;
    turn.hiddenBeforeItems = count;
    totalPixelExtent += effectiveTurnExtent(turn) - before;
    ++geometryGeneration;
  }

  void setRootCollapsed(TurnGeometry &turn, bool collapsed) {
    if (turn.rootCollapsed == collapsed)
      return;
    const std::int64_t before = effectiveTurnExtent(turn);
    turn.rootCollapsed = collapsed;
    totalPixelExtent += effectiveTurnExtent(turn) - before;
    ++geometryGeneration;
  }

  void retireCurrentStorage() {
    if (!turns.empty() || !itemIndex.empty() || !turnIndex.empty()) {
      retiredRecordCount += retainedGeometryRecords;
      retiredStorage.emplace_back();
      turns.swap(retiredStorage.back().turns);
      itemIndex.swap(retiredStorage.back().itemIndex);
      turnIndex.swap(retiredStorage.back().turnIndex);
    }
    scan = {};
    geometryScan = {};
    geometryRerunRequired = false;
    threadStructureRevision = 0;
    threadChildCount = 0;
    newestTurn.reset();
    retainedHistoryItems = 0;
    retainedGeometryRecords = 0;
    extraPinnedRoots = 0;
    totalPixelExtent = 0;
    leadingPixelExtent = 0;
    trailingPixelExtent = 0;
    structureValidationCursor = 0;
    evictionTurnCursor = 0;
    validationGeneration = structureRequestGeneration;
    projectionCursor = nullptr;
    ++geometryGeneration;
  }

  [[nodiscard]] bool drainRetiredStorage(std::size_t budget) {
    const std::size_t originalBudget = budget;
    while (budget != 0 && !retiredStorage.empty()) {
      RetiredStorage &storage = retiredStorage.front();
      if (!storage.itemIndex.empty()) {
        storage.itemIndex.erase(storage.itemIndex.begin());
        --budget;
        continue;
      }
      if (!storage.turnIndex.empty()) {
        storage.turnIndex.erase(storage.turnIndex.begin());
        --budget;
        continue;
      }
      if (storage.turns.empty()) {
        retiredStorage.pop_front();
        --budget;
        continue;
      }
      TurnGeometry &turn = *storage.turns.front();
      if (turn.rootItem) {
        turn.rootItem.reset();
        retiredRecordCount -= std::min<std::size_t>(1, retiredRecordCount);
      } else if (!turn.items.empty()) {
        turn.items.pop_front();
        retiredRecordCount -= std::min<std::size_t>(1, retiredRecordCount);
      } else if (!turn.appendedItems.empty()) {
        turn.appendedItems.pop_front();
        retiredRecordCount -= std::min<std::size_t>(1, retiredRecordCount);
      } else {
        storage.turns.pop_front();
      }
      --budget;
    }
    lastRetiredCleanupOperations = originalBudget - budget;
    maxRetiredCleanupOperations =
        std::max(maxRetiredCleanupOperations, lastRetiredCleanupOperations);
    return !retiredStorage.empty();
  }
};

ConversationView::ConversationView(QWidget *parent)
    : QAbstractScrollArea(parent),
      graphGeometry_(std::make_unique<GraphViewportGeometry>()) {
  setObjectName(QStringLiteral("conversationScroll"));
  setProperty("graphContentionRetryDelayMs", GraphContentionRetryMilliseconds);
  setProperty("graphRetiredGeometryRecordCount", 0);
  setProperty("graphLastRetiredCleanupOperations", 0);
  setProperty("graphMaxRetiredCleanupOperations", 0);
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

  graphTrailingPlaceholder_ = new GraphHistoryPlaceholder(content_);
  contentLayout_->addWidget(graphTrailingPlaceholder_);

  empty_ = makeEmptyLabel();
  emptyMessage_ = empty_->text();
  empty_->setParent(content_);
  contentLayout_->addWidget(empty_);
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
            handleUserScrollValue(value);
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
  pendingGraphAnchorRestore_.reset();
  if (restored && restored->mode == Mode::Paused &&
      !restored->anchor.stableKey.empty())
    pendingGraphAnchorRestore_ = restored->anchor;
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
  graphGeometry_->affected.clear();
  graphGeometry_->affectedCursor = 0;
  graphGeometry_->refreshStructureReads = 0;
  graphGeometry_->forceStructureCheck = true;
  ++graphGeometry_->structureRequestGeneration;
  runGraphRefresh();
}

void ConversationView::graphChangedDeferred(
    std::span<const nodegraph::NodeRef> removed) {
  graphChangedDeferred({}, removed);
}

void ConversationView::graphChangedDeferred(
    std::span<const nodegraph::NodeRef> affected,
    std::span<const nodegraph::NodeRef> removed) {
  if (!graph_)
    return;
  detachRemovedNodes(removed);
  graphGeometry_->refreshStructureReads = 0;
  bool structureCheckRequested = affected.empty() || !removed.empty();
  if (graphGeometry_->affectedCursor != 0) {
    graphGeometry_->affected.erase(
        graphGeometry_->affected.begin(),
        graphGeometry_->affected.begin() +
            static_cast<std::ptrdiff_t>(graphGeometry_->affectedCursor));
    graphGeometry_->affectedCursor = 0;
  }
  std::unordered_set<const nodegraph::Node *> pending;
  pending.reserve(graphGeometry_->affected.size());
  for (const nodegraph::NodeRef &node : graphGeometry_->affected)
    if (node)
      pending.insert(node.get());
  bool affectedOverflow = false;
  std::size_t inspectedAffected = 0;
  for (const nodegraph::NodeRef &node : affected) {
    if (inspectedAffected++ >= MaxPendingAffectedNodes) {
      affectedOverflow = true;
      break;
    }
    if (!node || pending.contains(node.get()))
      continue;
    if (graphGeometry_->affected.size() >= MaxPendingAffectedNodes) {
      affectedOverflow = true;
      break;
    }
    pending.insert(node.get());
    graphGeometry_->affected.push_back(node);
    if (node == graphThread_ || node->id().kind == nodegraph::NodeKind::Turn ||
        (node->id().kind == nodegraph::NodeKind::Item &&
         !graphGeometry_->itemIndex.contains(node.get())))
      structureCheckRequested = true;
  }
  if (affectedOverflow) {
    graphGeometry_->affected.clear();
    graphGeometry_->affectedCursor = 0;
    structureCheckRequested = true;
    graphGeometry_->forceSelectedReset = true;
    graphGeometry_->projectionRefreshPending = true;
    graphGeometry_->projectionCursor = nullptr;
  }
  if (structureCheckRequested) {
    graphGeometry_->forceStructureCheck = true;
    ++graphGeometry_->structureRequestGeneration;
  }
  // A single eventfd drain may carry hundreds of streaming revisions. Keep
  // removal detachment synchronous for node lifetime, but coalesce ordinary
  // structural/render reconciliation into one later Qt event-loop pass.
  scheduleGraphRefresh();
}

void ConversationView::detachRemovedNodes(
    std::span<const nodegraph::NodeRef> removed) {
  if (!graph_ || removed.empty())
    return;

  // A recursive graph retirement can contain thousands of descendants. Qt
  // only needs to detach its bounded live window; the next graph read detects
  // a selected-thread retirement directly. Small notifications can still be
  // handled immediately without turning removal delivery into an unbounded
  // main-thread pass.
  if (removed.size() > MaxPendingAffectedNodes)
    return;
  if (graphThread_ &&
      std::ranges::find(removed, graphThread_) != removed.end()) {
    std::array<nodegraph::NodeRef, 1> selected{graphThread_};
    detachGraphWidgets(selected);
    return;
  }
  detachGraphWidgets(removed);
}

void ConversationView::clearGraph() {
  if (!graph_ && graphSections_.empty() && graphGeometry_->turns.empty())
    return;
  ++graphBindingEpoch_;
  pendingGraphAnchorRestore_.reset();
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
  static_cast<GraphHistoryPlaceholder *>(graphTrailingPlaceholder_)
      ->setItemCount(0);
  graphGeometry_->retireCurrentStorage();
  graphGeometry_->affected.clear();
  graphGeometry_->affectedCursor = 0;
  graphGeometry_->geometryGeneration = 1;
  graphGeometry_->threadStructureRevision = 0;
  graphGeometry_->threadChildCount = 0;
  graphGeometry_->retainedHistoryItems = 0;
  graphGeometry_->retainedGeometryRecords = 0;
  graphGeometry_->extraPinnedRoots = 0;
  graphGeometry_->targetHistoryItems = 0;
  graphGeometry_->totalItems = 0;
  graphGeometry_->totalPixelExtent = 0;
  graphGeometry_->leadingPixelExtent = 0;
  graphGeometry_->trailingPixelExtent = 0;
  graphGeometry_->lastStructureReadsPerPass = 0;
  graphGeometry_->lastGeometryRecordsPerPass = 0;
  graphGeometry_->lastCardOperationsPerPass = 0;
  graphGeometry_->maxStructureReadsPerPass = 0;
  graphGeometry_->maxGeometryRecordsPerPass = 0;
  graphGeometry_->maxCardOperationsPerPass = 0;
  graphGeometry_->contentionRetryCount = 0;
  graphGeometry_->refreshStructureReads = 0;
  graphGeometry_->targetDirty = true;
  graphGeometry_->forceStructureCheck = false;
  graphGeometry_->forceSelectedReset = false;
  graphGeometry_->structureValidationCursor = 0;
  graphGeometry_->projectionRefreshPending = false;
  graphGeometry_->projectionCursor = nullptr;
  graphGeometry_->structureRequestGeneration = 0;
  graphGeometry_->validationGeneration = 0;
  graphGeometry_->geometryRerunRequired = false;
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
  graphPassCardOperations_ = 0;
  setProperty("graphLiveRecordCount", 0);
  setProperty("graphLiveSectionCount", 0);
  setProperty("graphRetainedGeometryRecordCount", 0);
  setProperty("graphLastStructureReadsPerPass", 0);
  setProperty("graphLastGeometryRecordsPerPass", 0);
  setProperty("graphLastCardOperationsPerPass", 0);
  setProperty("graphLastRefreshStructureReads", 0);
  setProperty("graphMaxStructureReadsPerPass", 0);
  setProperty("graphMaxGeometryRecordsPerPass", 0);
  setProperty("graphMaxCardOperationsPerPass", 0);
  setProperty("graphLeadingPixelExtent", 0);
  setProperty("graphFirstRetainedScrollValue", 0);
  setProperty("graphTrailingPixelExtent", 0);
  setProperty("graphStructureScanComplete", false);
  setProperty("graphStructureScanTarget", 0);
  setProperty("graphContentionRetryDelayMs", GraphContentionRetryMilliseconds);
  setProperty("graphContentionRetryCount", 0);
  publishRetiredGeometryCleanupMetrics();
  scheduleRetiredGeometryCleanup();
}

void ConversationView::scheduleGraphRefresh() {
  if (!graph_ || graphRefreshScheduled_)
    return;
  graphRefreshScheduled_ = true;
  const std::uint64_t epoch = graphBindingEpoch_;
  QTimer::singleShot(0, this, [this, epoch] {
    if (epoch != graphBindingEpoch_)
      return;
    graphRefreshScheduled_ = false;
    runGraphRefresh();
  });
}

void ConversationView::scheduleGraphContentionRetry() {
  if (!graph_ || graphRefreshScheduled_)
    return;
  graphRefreshScheduled_ = true;
  ++graphGeometry_->contentionRetryCount;
  setProperty("graphContentionRetryCount",
              static_cast<qulonglong>(graphGeometry_->contentionRetryCount));
  const std::uint64_t epoch = graphBindingEpoch_;
  QTimer::singleShot(GraphContentionRetryMilliseconds, this, [this, epoch] {
    if (epoch != graphBindingEpoch_)
      return;
    graphRefreshScheduled_ = false;
    runGraphRefresh();
  });
}

void ConversationView::scheduleVisibilityContentionRetry() {
  if (!graph_ || visibilityPassScheduled_)
    return;
  visibilityPassScheduled_ = true;
  ++graphGeometry_->contentionRetryCount;
  setProperty("graphContentionRetryCount",
              static_cast<qulonglong>(graphGeometry_->contentionRetryCount));
  const std::uint64_t epoch = graphBindingEpoch_;
  QTimer::singleShot(GraphContentionRetryMilliseconds, this, [this, epoch] {
    if (epoch != graphBindingEpoch_)
      return;
    visibilityPassScheduled_ = false;
    scheduleVisibilityPass();
  });
}

void ConversationView::scheduleRetiredGeometryCleanup() {
  if (retiredGeometryCleanupScheduled_ ||
      graphGeometry_->retiredStorage.empty())
    return;
  retiredGeometryCleanupScheduled_ = true;
  QTimer::singleShot(0, this, [this] {
    retiredGeometryCleanupScheduled_ = false;
    const bool cleanupRemaining =
        graphGeometry_->drainRetiredStorage(MaxStructureRecordsPerPass);
    publishRetiredGeometryCleanupMetrics();
    if (cleanupRemaining)
      scheduleRetiredGeometryCleanup();
  });
}

void ConversationView::publishRetiredGeometryCleanupMetrics() {
  setProperty("graphRetiredGeometryRecordCount",
              static_cast<qulonglong>(graphGeometry_->retiredRecordCount));
  setProperty(
      "graphLastRetiredCleanupOperations",
      static_cast<qulonglong>(graphGeometry_->lastRetiredCleanupOperations));
  setProperty(
      "graphMaxRetiredCleanupOperations",
      static_cast<qulonglong>(graphGeometry_->maxRetiredCleanupOperations));
}

void ConversationView::runGraphRefresh() {
  if (!graph_ || !graphThread_)
    return;
  const bool cleanupRemaining =
      graphGeometry_->drainRetiredStorage(MaxStructureRecordsPerPass);
  publishRetiredGeometryCleanupMetrics();
  if (cleanupRemaining)
    scheduleRetiredGeometryCleanup();
  graphPassCardOperations_ = 0;

  std::size_t structureReads = 0;
  bool threadRemoved = false;
  bool scanHasMore = false;
  bool resetRequested = false;
  bool evictionRemaining = false;
  std::vector<nodegraph::NodeRef> materializedPrompts;
  std::vector<std::pair<nodegraph::NodeRef, nodegraph::NodeRef>>
      promptTransfers;
  std::vector<std::pair<nodegraph::NodeRef, bool>> turnActivityChanges;
  const std::uint64_t epoch = graphBindingEpoch_;

  std::optional<nodegraph::NodeGraph::ReadAccess> read = graph_->tryRead();
  if (!read) {
    scheduleGraphContentionRetry();
    return;
  }

  if (!read->contains(graphThread_) || read->removed(graphThread_)) {
    threadRemoved = true;
  } else {
    const std::shared_ptr<const nodegraph::NodeState> threadState =
        read->state(graphThread_);
    const std::optional<std::size_t> loadedItemCount =
        threadState
            ? graphSize(graphField(*threadState, "historyLoadedItemCount"))
            : std::nullopt;
    graphProviderHasMore_ =
        threadState && graphProviderHasMoreHistory(*threadState);

    const std::size_t childCount = read->childCount(graphThread_);
    const std::uint64_t threadStructure =
        read->structureChangedRevision(graphThread_);
    const bool requestedStructureCheck = graphGeometry_->forceStructureCheck;
    const auto resetRetainedGeometry = [&resetRequested] {
      resetRequested = true;
    };
    if (graphGeometry_->forceSelectedReset)
      resetRetainedGeometry();
    graphGeometry_->forceSelectedReset = false;

    if (!resetRequested && graphGeometry_->forceStructureCheck &&
        graphGeometry_->scan.initialized) {
      if (graphGeometry_->structureValidationCursor == 0)
        graphGeometry_->validationGeneration =
            graphGeometry_->structureRequestGeneration;
      while (graphGeometry_->structureValidationCursor <
                 graphGeometry_->turns.size() &&
             structureReads < MaxStructureRecordsPerPass) {
        GraphViewportGeometry::TurnGeometry *turn =
            graphGeometry_->turns[graphGeometry_->structureValidationCursor++]
                .get();
        ++structureReads;
        if (!read->contains(turn->node) || read->removed(turn->node)) {
          resetRetainedGeometry();
          break;
        }
        const std::shared_ptr<const nodegraph::NodeState> turnState =
            read->state(turn->node);
        const bool active =
            turnState && turnState->status == nodegraph::NodeStatus::Running;
        if (turn->active != active) {
          turn->active = active;
          turnActivityChanges.emplace_back(turn->node, active);
        }
        const std::uint64_t revision =
            read->structureChangedRevision(turn->node);
        if (revision == turn->structureRevision)
          continue;
        const std::size_t currentCount = read->childCount(turn->node);
        if (currentCount > turn->knownChildCount) {
          if (structureReads >= MaxStructureRecordsPerPass) {
            --graphGeometry_->structureValidationCursor;
            break;
          }
          nodegraph::NodeRef currentNewest =
              read->childAt(turn->node, currentCount - 1);
          ++structureReads;
          // A tail append leaves the previous tail in place and is admitted
          // through the affected/newest-node path below. If the current tail
          // did not change, the added records were prepended (provider history)
          // and the backward frontier has shifted, so rebuild it incrementally.
          if (currentCount > turn->knownChildCount + 1 ||
              (turn->newestChild && currentNewest == turn->newestChild)) {
            resetRetainedGeometry();
            break;
          }
          turn->knownChildCount = currentCount;
          turn->newestChild = std::move(currentNewest);
          turn->structureRevision = revision;
          continue;
        }
        resetRetainedGeometry();
        break;
      }
      if (graphGeometry_->structureValidationCursor >=
          graphGeometry_->turns.size()) {
        graphGeometry_->structureValidationCursor = 0;
        if (graphGeometry_->validationGeneration ==
            graphGeometry_->structureRequestGeneration)
          graphGeometry_->forceStructureCheck = false;
      }
    }
    if (!resetRequested) {
      nodegraph::NodeRef newestTurn;
      nodegraph::NodeRef newestItem;
      for (std::size_t index = childCount;
           index > 0 && structureReads < MaxStructureRecordsPerPass;) {
        nodegraph::NodeRef turn = read->childAt(graphThread_, --index);
        ++structureReads;
        if (!turn || !read->contains(turn) ||
            turn->id().kind != nodegraph::NodeKind::Turn ||
            read->removed(turn))
          continue;
        newestTurn = turn;
        for (std::size_t itemIndex = read->childCount(turn);
             itemIndex > 0 && structureReads < MaxStructureRecordsPerPass;) {
          nodegraph::NodeRef item = read->childAt(turn, --itemIndex);
          ++structureReads;
          if (item && read->contains(item) &&
              item->id().kind == nodegraph::NodeKind::Item &&
              !read->removed(item)) {
            newestItem = std::move(item);
            break;
          }
        }
        if (newestItem)
          break;
      }

      const std::string newestKey =
          newestItem ? newestItem->id().canonical : std::string{};
      if (mode_ == Mode::Paused && loadedItemCount &&
          graphKnownItemCount_ != 0 &&
          *loadedItemCount > graphKnownItemCount_ &&
          !graphNewestItemKey_.empty() && !newestKey.empty() &&
          newestKey != graphNewestItemKey_) {
        const std::size_t appended = *loadedItemCount - graphKnownItemCount_;
        graphHistoryLimit_ +=
            std::min(appended, std::numeric_limits<std::size_t>::max() -
                                   graphHistoryLimit_);
      }

      graphGeometry_->totalItems = loadedItemCount.value_or(std::max(
          graphGeometry_->totalItems, graphGeometry_->retainedHistoryItems));
      graphGeometry_->targetHistoryItems =
          loadedItemCount ? std::min(graphHistoryLimit_, *loadedItemCount)
                          : graphHistoryLimit_;
      if (newestItem && !graphGeometry_->itemIndex.contains(newestItem.get()))
        graphGeometry_->targetHistoryItems =
            std::max(graphGeometry_->targetHistoryItems,
                     graphGeometry_->retainedHistoryItems + 1);

      if (!graphGeometry_->scan.initialized) {
        graphGeometry_->scan.initialized = true;
        graphGeometry_->scan.nextTurnIndex = childCount;
        graphGeometry_->threadChildCount = childCount;
        graphGeometry_->threadStructureRevision = threadStructure;
        graphGeometry_->newestTurn = newestTurn;
      } else if (threadStructure != graphGeometry_->threadStructureRevision) {
        // Older provider pages are prepended. Their insertion shifts the
        // retained backward frontier but does not invalidate the already-read
        // suffix. Appended turns stay after the frontier and are picked up by
        // the bounded newest-item probe above.
        if (childCount >= graphGeometry_->threadChildCount &&
            newestTurn == graphGeometry_->newestTurn) {
          graphGeometry_->scan.nextTurnIndex +=
              childCount - graphGeometry_->threadChildCount;
        } else if (childCount > graphGeometry_->threadChildCount &&
                   newestTurn != graphGeometry_->newestTurn) {
          // A newly appended turn sits beyond a completed backward frontier.
          // Reopen that frontier at the new tail; existing records are
          // identity-deduplicated below, so no retained count is duplicated.
          graphGeometry_->scan.nextTurnIndex = childCount;
          graphGeometry_->scan.complete = false;
        }
        if (requestedStructureCheck &&
            childCount == graphGeometry_->threadChildCount)
          graphGeometry_->forceSelectedReset = true;
        graphGeometry_->threadChildCount = childCount;
        graphGeometry_->threadStructureRevision = threadStructure;
        graphGeometry_->newestTurn = newestTurn;
      }

      const auto turnRoot = [&read](const nodegraph::NodeRef &turn) {
        const std::size_t count =
            read->relatedCount(turn, nodegraph::RelationKind::TurnRootItem);
        for (std::size_t index = 0; index < count; ++index) {
          nodegraph::NodeRef candidate = read->relatedAt(
              turn, nodegraph::RelationKind::TurnRootItem, index);
          if (candidate && read->contains(candidate) &&
              candidate->id().kind == nodegraph::NodeKind::Item &&
              !read->removed(candidate))
            return candidate;
        }
        return nodegraph::NodeRef{};
      };

      const auto makeRecord =
          [this, &read, &materializedPrompts,
           &promptTransfers](const nodegraph::NodeRef &item,
                             GraphViewportGeometry::TurnGeometry *turn)
          -> std::unique_ptr<GraphViewportGeometry::ItemGeometry> {
        if (!item || !turn || !read->contains(item) || read->removed(item))
          return {};
        const std::shared_ptr<const nodegraph::NodeState> state =
            read->state(item);
        if (!state)
          return {};

        nodegraph::NodeRef localPrompt;
        std::shared_ptr<const nodegraph::NodeState> localPromptState;
        bool suppressedByLocalPrompt = false;
        const std::size_t relationCount = read->relatedCount(
            item, nodegraph::RelationKind::PromptMaterialization);
        for (std::size_t index = 0; index < relationCount; ++index) {
          nodegraph::NodeRef candidate = read->relatedAt(
              item, nodegraph::RelationKind::PromptMaterialization, index);
          if (!candidate || !read->contains(candidate) ||
              candidate->id().kind != nodegraph::NodeKind::Item ||
              read->removed(candidate))
            continue;
          const std::shared_ptr<const nodegraph::NodeState> candidateState =
              read->state(candidate);
          if (!candidateState ||
              graphString(graphField(*candidateState, "type")) != "localPrompt")
            continue;
          if (graphString(graphField(*candidateState, "dispatchState")) ==
              "awaitingMaterialization") {
            localPrompt = candidate;
            localPromptState = candidateState;
          } else {
            suppressedByLocalPrompt = true;
          }
          break;
        }
        if (suppressedByLocalPrompt)
          return {};
        if (graphGeometry_->itemIndex.contains(item.get()))
          return {};
        if (localPrompt) {
          const auto retained =
              graphGeometry_->itemIndex.find(localPrompt.get());
          if (retained != graphGeometry_->itemIndex.end()) {
            GraphViewportGeometry::ItemGeometry *record = retained->second;
            graphGeometry_->itemIndex.erase(record->node.get());
            record->node = item;
            record->materializedPrompt = localPrompt;
            const std::int64_t rawId =
                graphInteger(graphField(*localPromptState, "submissionId"))
                    .value_or(0);
            record->promptVisualId =
                rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
            record->key = stableKey(LocalPromptKey{*record->promptVisualId});
            graphGeometry_->itemIndex[item.get()] = record;
            graphGeometry_->itemIndex[localPrompt.get()] = record;
            materializedPrompts.push_back(localPrompt);
            promptTransfers.emplace_back(localPrompt, item);
            return {};
          }
        }

        auto record = std::make_unique<GraphViewportGeometry::ItemGeometry>();
        record->node = item;
        record->materializedPrompt = localPrompt;
        record->turn = turn;
        if (localPromptState) {
          const std::int64_t rawId =
              graphInteger(graphField(*localPromptState, "submissionId"))
                  .value_or(0);
          record->promptVisualId =
              rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
          record->key = stableKey(LocalPromptKey{*record->promptVisualId});
          materializedPrompts.push_back(localPrompt);
        } else if (graphCardKind(*state) == CardKind::LocalPrompt) {
          const std::int64_t rawId =
              graphInteger(graphField(*state, "submissionId")).value_or(0);
          record->promptVisualId =
              rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
          record->key = stableKey(LocalPromptKey{*record->promptVisualId});
        } else {
          record->key = stableKey(AuthoritativeItemKey{
              graphThread_->id().canonical, turn->protocolId,
              nodegraph::protocolCanonicalId(*state, item)});
        }
        record->measuredHeight = initialCardHeight(graphCardKind(*state));
        for (TurnSectionWidget *section : graphSections_) {
          const auto retained = std::ranges::find_if(
              section->cardSlots,
              [&item, &localPrompt](const TurnSectionWidget::CardSlot &slot) {
                return slot.graphNode == item ||
                       (localPrompt && slot.graphNode == localPrompt);
              });
          if (retained != section->cardSlots.end()) {
            record->measuredHeight = retained->measuredHeight;
            break;
          }
        }
        record->projectionVisible =
            graphCardVisible(*state, presentationOptions_);
        return record;
      };

      const auto indexRecord =
          [this](GraphViewportGeometry::ItemGeometry *record) {
            if (!record)
              return;
            graphGeometry_->itemIndex[record->node.get()] = record;
            if (record->materializedPrompt)
              graphGeometry_->itemIndex[record->materializedPrompt.get()] =
                  record;
            ++graphGeometry_->retainedGeometryRecords;
            graphGeometry_->addRecordExtent(*record);
          };

      if (graphGeometry_->projectionRefreshPending) {
        auto projection = graphGeometry_->projectionCursor
                              ? graphGeometry_->itemIndex.upper_bound(
                                    graphGeometry_->projectionCursor)
                              : graphGeometry_->itemIndex.begin();
        while (projection != graphGeometry_->itemIndex.end() &&
               structureReads < MaxStructureRecordsPerPass) {
          const nodegraph::Node *indexKey = projection->first;
          GraphViewportGeometry::ItemGeometry *record = projection->second;
          ++projection;
          graphGeometry_->projectionCursor = indexKey;
          if (!record || record->node.get() != indexKey)
            continue;
          ++structureReads;
          const std::shared_ptr<const nodegraph::NodeState> state =
              read->state(record->node);
          if (!state)
            continue;
          const bool visible = graphCardVisible(*state, presentationOptions_);
          if (visible == record->projectionVisible)
            continue;
          graphGeometry_->setRecordProjectionVisible(*record, visible);
          for (TurnSectionWidget *section : graphSections_)
            for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
              if (slot.graphNode == record->node) {
                slot.projectionVisible = visible;
                section->geometryDirty = true;
              }
        }
        if (projection == graphGeometry_->itemIndex.end()) {
          graphGeometry_->projectionRefreshPending = false;
          graphGeometry_->projectionCursor = nullptr;
        }
      }

      while (graphGeometry_->affectedCursor < graphGeometry_->affected.size() &&
             structureReads < MaxStructureRecordsPerPass) {
        nodegraph::NodeRef affected =
            graphGeometry_->affected[graphGeometry_->affectedCursor++];
        ++structureReads;
        if (!affected || !read->contains(affected) || read->removed(affected) ||
            affected->id().kind != nodegraph::NodeKind::Item)
          continue;
        nodegraph::NodeRef turnNode = read->parent(affected);
        if (const auto existing =
                graphGeometry_->itemIndex.find(affected.get());
            existing != graphGeometry_->itemIndex.end()) {
          GraphViewportGeometry::ItemGeometry *record = existing->second;
          if (turnNode && turnNode != record->turn->node) {
            graphGeometry_->forceSelectedReset = true;
            continue;
          }
          const std::shared_ptr<const nodegraph::NodeState> currentState =
              read->state(record->node);
          if (!currentState)
            continue;
          const bool visible =
              graphCardVisible(*currentState, presentationOptions_);
          if (visible != record->projectionVisible) {
            graphGeometry_->setRecordProjectionVisible(*record, visible);
            for (TurnSectionWidget *section : graphSections_)
              for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
                if (slot.graphNode == record->node ||
                    slot.graphNode == affected) {
                  slot.projectionVisible = visible;
                  section->geometryDirty = true;
                }
          }
          continue;
        }
        if (!turnNode || !read->contains(turnNode) ||
            turnNode->id().kind != nodegraph::NodeKind::Turn ||
            read->removed(turnNode) || read->parent(turnNode) != graphThread_)
          continue;

        GraphViewportGeometry::TurnGeometry *turn = nullptr;
        if (const auto retained =
                graphGeometry_->turnIndex.find(turnNode.get());
            retained != graphGeometry_->turnIndex.end()) {
          turn = retained->second;
        } else {
          // A targeted state change to an item outside the retained suffix must
          // not manufacture geometry. New turns are discovered by the bounded
          // thread frontier below.
          continue;
        }
        const std::size_t currentCount = read->childCount(turnNode);
        if (currentCount < turn->knownChildCount ||
            currentCount > turn->knownChildCount + 1) {
          graphGeometry_->forceSelectedReset = true;
          continue;
        }
        if (currentCount == turn->knownChildCount) {
          // This is a state-only delta for an off-screen node. A replacement of
          // the explicit root changes the turn structure and is rebuilt by the
          // validation frontier rather than appended out of order here.
          continue;
        }
        if (structureReads >= MaxStructureRecordsPerPass) {
          --graphGeometry_->affectedCursor;
          break;
        }
        nodegraph::NodeRef currentNewest =
            read->childAt(turnNode, currentCount - 1);
        ++structureReads;
        if (currentNewest != affected) {
          graphGeometry_->forceSelectedReset = true;
          continue;
        }
        turn->knownChildCount = currentCount;
        turn->newestChild = currentNewest;
        turn->structureRevision = read->structureChangedRevision(turnNode);
        if (affected == turn->root && !turn->rootItem) {
          if (auto record = makeRecord(affected, turn)) {
            GraphViewportGeometry::ItemGeometry *raw = record.get();
            turn->rootItem = std::move(record);
            indexRecord(raw);
            const auto collapsed = cardCollapsedStates_.find(raw->key);
            graphGeometry_->setRootCollapsed(
                *turn,
                collapsed != cardCollapsedStates_.end() && collapsed->second);
          }
        } else if (auto record = makeRecord(affected, turn)) {
          GraphViewportGeometry::ItemGeometry *raw = record.get();
          turn->appendedItems.push_back(std::move(record));
          indexRecord(raw);
        }
        ++graphGeometry_->retainedHistoryItems;
        graphGeometry_->targetDirty = true;
      }
      if (graphGeometry_->affectedCursor == graphGeometry_->affected.size()) {
        graphGeometry_->affected.clear();
        graphGeometry_->affectedCursor = 0;
      }

      // A normal stream update appends to the current turn. Admit the newest
      // node directly without rebuilding the retained history window.
      if (newestItem && !graphGeometry_->turns.empty() &&
          !graphGeometry_->itemIndex.contains(newestItem.get())) {
        auto turn = graphGeometry_->turnIndex.find(newestTurn.get());
        if (turn != graphGeometry_->turnIndex.end()) {
          if (auto record = makeRecord(newestItem, turn->second)) {
            GraphViewportGeometry::ItemGeometry *raw = record.get();
            turn->second->appendedItems.push_back(std::move(record));
            indexRecord(raw);
            ++graphGeometry_->retainedHistoryItems;
            graphGeometry_->targetDirty = true;
          }
        }
      }
      if (!newestKey.empty())
        graphNewestItemKey_ = newestKey;

      auto &scan = graphGeometry_->scan;
      while (!threadRemoved &&
             graphGeometry_->retainedHistoryItems <
                 graphGeometry_->targetHistoryItems &&
             structureReads < MaxStructureRecordsPerPass) {
        if (!scan.turn) {
          if (scan.nextTurnIndex == 0) {
            scan.complete = true;
            break;
          }
          nodegraph::NodeRef turnNode =
              read->childAt(graphThread_, --scan.nextTurnIndex);
          ++structureReads;
          if (!turnNode || !read->contains(turnNode) ||
              turnNode->id().kind != nodegraph::NodeKind::Turn ||
              read->removed(turnNode))
            continue;

          GraphViewportGeometry::TurnGeometry *turn = nullptr;
          if (const auto retained =
                  graphGeometry_->turnIndex.find(turnNode.get());
              retained != graphGeometry_->turnIndex.end()) {
            turn = retained->second;
          } else {
            auto inserted =
                std::make_unique<GraphViewportGeometry::TurnGeometry>();
            inserted->node = turnNode;
            const std::shared_ptr<const nodegraph::NodeState> turnState =
                read->state(turnNode);
            inserted->protocolId =
                turnState ? nodegraph::protocolCanonicalId(*turnState, turnNode)
                          : turnNode->id().canonical;
            inserted->active = turnState && turnState->status ==
                                                nodegraph::NodeStatus::Running;
            inserted->structureRevision =
                read->structureChangedRevision(turnNode);
            inserted->knownChildCount = read->childCount(turnNode);
            inserted->root = turnRoot(turnNode);
            inserted->rootIsChild =
                inserted->root && read->parent(inserted->root) == turnNode;
            turn = inserted.get();
            graphGeometry_->turns.push_front(std::move(inserted));
            graphGeometry_->turnIndex.emplace(turnNode.get(), turn);
          }

          scan.turn = turn;
          scan.nextItemIndex = read->childCount(turnNode);
          turn->knownChildCount = scan.nextItemIndex;
          scan.directItemsInTurn = scan.nextItemIndex;
          scan.selectedInTurn = turn->items.size() + turn->appendedItems.size();
          scan.rootConsumed = false;
          turn->structureRevision = read->structureChangedRevision(turnNode);

          if (turn->root && !turn->rootItem &&
              structureReads < MaxStructureRecordsPerPass) {
            ++structureReads;
            if (auto rootRecord = makeRecord(turn->root, turn)) {
              GraphViewportGeometry::ItemGeometry *raw = rootRecord.get();
              raw->countedOutsideHistory = true;
              turn->rootItem = std::move(rootRecord);
              indexRecord(raw);
              const auto collapsed = cardCollapsedStates_.find(raw->key);
              graphGeometry_->setRootCollapsed(
                  *turn,
                  collapsed != cardCollapsedStates_.end() && collapsed->second);
              ++graphGeometry_->extraPinnedRoots;
            }
          }
        }

        GraphViewportGeometry::TurnGeometry *turn = scan.turn;
        const std::uint64_t currentStructure =
            read->structureChangedRevision(turn->node);
        if (currentStructure != turn->structureRevision) {
          const std::size_t currentCount = read->childCount(turn->node);
          // Appending children does not move the older backward frontier.
          if (currentCount < scan.nextItemIndex) {
            scan.nextItemIndex = currentCount;
            scan.selectedInTurn = 0;
          }
          scan.directItemsInTurn = currentCount;
          turn->structureRevision = currentStructure;
        }

        if (scan.nextItemIndex == 0) {
          graphGeometry_->setHiddenBeforeItems(*turn, 0);
          scan.turn = nullptr;
          continue;
        }

        nodegraph::NodeRef item =
            read->childAt(turn->node, --scan.nextItemIndex);
        ++structureReads;
        if (!item || !read->contains(item) ||
            item->id().kind != nodegraph::NodeKind::Item ||
            read->removed(item))
          continue;
        if (scan.nextItemIndex + 1 == scan.directItemsInTurn)
          turn->newestChild = item;

        const bool alreadyRetained =
            graphGeometry_->itemIndex.contains(item.get());
        if (!alreadyRetained) {
          ++scan.selectedInTurn;
          ++graphGeometry_->retainedHistoryItems;
        }
        if (item == turn->root) {
          scan.rootConsumed = true;
          if (turn->rootItem && turn->rootItem->countedOutsideHistory) {
            turn->rootItem->countedOutsideHistory = false;
            --graphGeometry_->extraPinnedRoots;
          }
        } else if (!alreadyRetained) {
          if (auto record = makeRecord(item, turn)) {
            GraphViewportGeometry::ItemGeometry *raw = record.get();
            turn->items.push_front(std::move(record));
            indexRecord(raw);
          }
        }
        std::size_t hidden = scan.nextItemIndex;
        if (turn->rootIsChild && turn->rootItem && !scan.rootConsumed &&
            hidden != 0)
          --hidden;
        graphGeometry_->setHiddenBeforeItems(*turn, hidden);
      }

      scanHasMore = graphGeometry_->retainedHistoryItems <
                        graphGeometry_->targetHistoryItems &&
                    !graphGeometry_->scan.complete;
      if (!loadedItemCount && graphGeometry_->scan.complete)
        graphGeometry_->totalItems = graphGeometry_->retainedHistoryItems;
      graphKnownItemCount_ = graphGeometry_->totalItems;
    }
  }

  read.reset();

  if (resetRequested) {
    graphGeometry_->retireCurrentStorage();
    publishRetiredGeometryCleanupMetrics();
    graphGeometry_->affected.clear();
    graphGeometry_->affectedCursor = 0;
    graphGeometry_->forceSelectedReset = false;
    graphGeometry_->forceStructureCheck = true;
    graphGeometry_->projectionRefreshPending = true;
    graphGeometry_->projectionCursor = nullptr;
    scheduleRetiredGeometryCleanup();
    scheduleGraphRefresh();
    return;
  }

  for (const auto &[turn, active] : turnActivityChanges)
    for (TurnSectionWidget *section : graphSections_)
      if (section->graphNode == turn) {
        section->graphActive = active;
        section->layoutDirty = true;
      }

  for (const auto &[localPrompt, authoritative] : promptTransfers) {
    for (TurnSectionWidget *section : graphSections_) {
      for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
        if (slot.graphNode != localPrompt)
          continue;
        if (slot.attachment &&
            localPrompt->uiAttachment() == slot.attachment.get())
          localPrompt->setUiAttachment(nullptr);
        slot.graphNode = authoritative;
        if (slot.attachment) {
          slot.attachment->renderedRevision = 0;
          authoritative->setUiAttachment(slot.attachment.get());
        }
      }
    }
  }

  std::size_t evictionWork = 0;
  while (mode_ == Mode::Following &&
         graphGeometry_->retainedHistoryItems >
             graphGeometry_->targetHistoryItems &&
         evictionWork < MaxGeometryEvictionsPerPass) {
    while (graphGeometry_->evictionTurnCursor < graphGeometry_->turns.size() &&
           graphGeometry_->turns[graphGeometry_->evictionTurnCursor]
               ->items.empty() &&
           graphGeometry_->turns[graphGeometry_->evictionTurnCursor]
               ->appendedItems.empty() &&
           evictionWork < MaxGeometryEvictionsPerPass) {
      ++graphGeometry_->evictionTurnCursor;
      ++evictionWork;
    }
    if (graphGeometry_->evictionTurnCursor >= graphGeometry_->turns.size() ||
        evictionWork >= MaxGeometryEvictionsPerPass)
      break;
    GraphViewportGeometry::TurnGeometry *oldestTurn =
        graphGeometry_->turns[graphGeometry_->evictionTurnCursor].get();
    std::unique_ptr<GraphViewportGeometry::ItemGeometry> evicted;
    if (!oldestTurn->items.empty()) {
      evicted = std::move(oldestTurn->items.front());
      oldestTurn->items.pop_front();
    } else if (!oldestTurn->appendedItems.empty()) {
      evicted = std::move(oldestTurn->appendedItems.front());
      oldestTurn->appendedItems.pop_front();
    }
    if (!evicted)
      break;
    ++evictionWork;
    // GeometryFrontier contains raw pointers into retained records. Cancel it
    // before the bounded eviction destroys one of those records.
    graphGeometry_->geometryScan = {};
    graphGeometry_->geometryRerunRequired = false;
    graphGeometry_->projectionCursor = nullptr;
    graphGeometry_->removeRecordExtent(*evicted);
    graphGeometry_->itemIndex.erase(evicted->node.get());
    if (evicted->materializedPrompt)
      graphGeometry_->itemIndex.erase(evicted->materializedPrompt.get());
    --graphGeometry_->retainedGeometryRecords;
    --graphGeometry_->retainedHistoryItems;
    if (graphGeometry_->scan.turn == oldestTurn)
      ++graphGeometry_->scan.nextItemIndex;
  }
  const bool hasEvictionCandidate =
      graphGeometry_->evictionTurnCursor < graphGeometry_->turns.size() &&
      (!graphGeometry_->turns[graphGeometry_->evictionTurnCursor]
            ->items.empty() ||
       !graphGeometry_->turns[graphGeometry_->evictionTurnCursor]
            ->appendedItems.empty());
  evictionRemaining =
      mode_ == Mode::Following &&
      (hasEvictionCandidate || evictionWork >= MaxGeometryEvictionsPerPass) &&
      graphGeometry_->retainedHistoryItems > graphGeometry_->targetHistoryItems;
  if (!evictionRemaining)
    graphGeometry_->evictionTurnCursor = 0;

  graphGeometry_->lastStructureReadsPerPass = structureReads;
  graphGeometry_->maxStructureReadsPerPass =
      std::max(graphGeometry_->maxStructureReadsPerPass, structureReads);
  graphGeometry_->refreshStructureReads += structureReads;
  setProperty("graphLastStructureReadsPerPass",
              static_cast<qulonglong>(structureReads));
  setProperty(
      "graphMaxStructureReadsPerPass",
      static_cast<qulonglong>(graphGeometry_->maxStructureReadsPerPass));
  setProperty("graphLastRefreshStructureReads",
              static_cast<qulonglong>(graphGeometry_->refreshStructureReads));
  setProperty("graphRetainedGeometryRecordCount",
              static_cast<qulonglong>(graphGeometry_->retainedGeometryRecords));
  setProperty("graphStructureScanComplete",
              graphGeometry_->scan.complete || !scanHasMore);
  setProperty("graphStructureScanTarget",
              static_cast<qulonglong>(graphGeometry_->targetHistoryItems));

  if (threadRemoved) {
    std::array<nodegraph::NodeRef, 1> removed{graphThread_};
    detachGraphWidgets(removed);
    return;
  }

  const std::size_t represented = std::max(graphGeometry_->retainedHistoryItems,
                                           graphGeometry_->targetHistoryItems) +
                                  graphGeometry_->extraPinnedRoots;
  graphHiddenItemCount_ = graphGeometry_->totalItems > represented
                              ? graphGeometry_->totalItems - represented
                              : 0;
  graphGeometry_->targetDirty = true;
  updateGraphChrome();
  const bool reconciliationRemaining = reconcileGraphViewport();
  resetGraphVisibilityScan();
  if (runGraphVisibilityPass())
    scheduleVisibilityPass();
  if (reconciliationRemaining)
    scheduleGraphRefresh();
  storeCurrentThreadState();

  if (promptMaterializedAction_) {
    for (nodegraph::NodeRef &prompt : materializedPrompts) {
      if (!promptMaterializedAction_(prompt)) {
        const std::uint64_t retryEpoch = epoch;
        QTimer::singleShot(16, this, [this, retryEpoch] {
          if (retryEpoch == graphBindingEpoch_)
            scheduleGraphRefresh();
        });
        break;
      }
    }
  }
  if (scanHasMore || evictionRemaining || !graphGeometry_->affected.empty() ||
      graphGeometry_->forceStructureCheck ||
      graphGeometry_->forceSelectedReset ||
      graphGeometry_->projectionRefreshPending)
    scheduleGraphRefresh();
}

bool ConversationView::reconcileGraphViewport() {
  if (!graph_ || !content_ || !viewport())
    return false;
  const std::int64_t viewportHeight = std::max(1, viewport()->height());
  const std::int64_t scrollTop =
      mode_ == Mode::Following
          ? std::max<std::int64_t>(0, graphGeometry_->totalPixelExtent -
                                          viewportHeight)
      : pendingGraphAnchorRestore_ ? pendingGraphAnchorRestore_->absoluteValue
                                   : verticalScrollBar()->value();
  const std::int64_t targetTop =
      std::max<std::int64_t>(0, scrollTop - viewportHeight);
  const std::int64_t targetBottom = std::min(graphGeometry_->totalPixelExtent,
                                             scrollTop + 2 * viewportHeight);
  const bool reverse =
      targetTop > graphGeometry_->totalPixelExtent - targetBottom;
  auto &geometryScan = graphGeometry_->geometryScan;
  const bool sameViewportTarget = geometryScan.targetTop == targetTop &&
                                  geometryScan.targetBottom == targetBottom &&
                                  geometryScan.reverse == reverse;
  if (geometryScan.active && sameViewportTarget &&
      geometryScan.generation != graphGeometry_->geometryGeneration) {
    // Appends, visibility changes, and measured-height corrections preserve
    // record addresses. Let the bounded frontier finish so visible widgets
    // make progress, then run one fresh geometry pass.
    geometryScan.generation = graphGeometry_->geometryGeneration;
    graphGeometry_->geometryRerunRequired = true;
  }
  const bool sameTarget =
      geometryScan.generation == graphGeometry_->geometryGeneration &&
      sameViewportTarget;
  if (!sameTarget || (!geometryScan.active && !geometryScan.valid)) {
    geometryScan = {};
    geometryScan.active = true;
    geometryScan.reverse = reverse;
    geometryScan.generation = graphGeometry_->geometryGeneration;
    geometryScan.targetTop = targetTop;
    geometryScan.targetBottom = targetBottom;
    geometryScan.position = reverse ? graphGeometry_->totalPixelExtent : 0;
    geometryScan.turnIndex = reverse ? graphGeometry_->turns.size() : 0;
  }

  const auto recordExtent =
      [](const GraphViewportGeometry::ItemGeometry *record) -> std::int64_t {
    if (!record || !record->projectionVisible)
      return 0;
    if (record->turn &&
        GraphViewportGeometry::suppressesChildren(*record->turn) &&
        record->turn->rootItem.get() != record)
      return 0;
    return GraphViewportGeometry::rawItemExtent(*record);
  };
  const auto appendIfWanted = [&geometryScan](
                                  GraphViewportGeometry::ItemGeometry *record,
                                  std::int64_t top, std::int64_t bottom) {
    if (record && record->node && record->projectionVisible && bottom > top &&
        bottom > geometryScan.targetTop && top < geometryScan.targetBottom)
      geometryScan.found.push_back({record, top, bottom});
  };

  std::size_t geometryReads = 0;
  while (geometryScan.active && geometryReads < MaxGeometryRecordsPerPass) {
    if (!geometryScan.reverse) {
      if (geometryScan.turnIndex >= graphGeometry_->turns.size() ||
          geometryScan.position >= geometryScan.targetBottom) {
        geometryScan.active = false;
        geometryScan.valid = true;
        break;
      }
      auto *turn = graphGeometry_->turns[geometryScan.turnIndex].get();
      if (geometryScan.phase == 0) {
        ++geometryReads;
        const std::int64_t turnExtent =
            GraphViewportGeometry::effectiveTurnExtent(*turn);
        if (geometryScan.position + turnExtent <= geometryScan.targetTop) {
          geometryScan.position += turnExtent;
          ++geometryScan.turnIndex;
          continue;
        }
        geometryScan.phase = 1;
      } else if (geometryScan.phase == 1) {
        ++geometryReads;
        const std::int64_t extent = recordExtent(turn->rootItem.get());
        appendIfWanted(turn->rootItem.get(), geometryScan.position,
                       geometryScan.position + extent);
        geometryScan.position += extent;
        if (GraphViewportGeometry::suppressesChildren(*turn)) {
          geometryScan.phase = 0;
          geometryScan.itemIndex = 0;
          ++geometryScan.turnIndex;
        } else {
          geometryScan.phase = 2;
        }
      } else if (geometryScan.phase == 2) {
        ++geometryReads;
        if (!GraphViewportGeometry::suppressesChildren(*turn))
          geometryScan.position +=
              static_cast<std::int64_t>(turn->hiddenBeforeItems) *
              EstimatedGraphHistoryItemExtent;
        geometryScan.phase = 3;
        geometryScan.itemIndex = 0;
      } else if (geometryScan.phase == 3) {
        if (geometryScan.itemIndex >= turn->items.size()) {
          geometryScan.phase = 4;
          geometryScan.itemIndex = 0;
          continue;
        }
        ++geometryReads;
        auto *record = turn->items[geometryScan.itemIndex++].get();
        const std::int64_t extent = recordExtent(record);
        appendIfWanted(record, geometryScan.position,
                       geometryScan.position + extent);
        geometryScan.position += extent;
      } else {
        if (geometryScan.itemIndex >= turn->appendedItems.size()) {
          geometryScan.phase = 0;
          geometryScan.itemIndex = 0;
          ++geometryScan.turnIndex;
          continue;
        }
        ++geometryReads;
        auto *record = turn->appendedItems[geometryScan.itemIndex++].get();
        const std::int64_t extent = recordExtent(record);
        appendIfWanted(record, geometryScan.position,
                       geometryScan.position + extent);
        geometryScan.position += extent;
      }
      continue;
    }

    if (geometryScan.turnIndex == 0 ||
        geometryScan.position <= geometryScan.targetTop) {
      geometryScan.active = false;
      geometryScan.valid = true;
      std::ranges::reverse(geometryScan.found);
      break;
    }
    auto *turn = graphGeometry_->turns[geometryScan.turnIndex - 1].get();
    if (geometryScan.phase == 0) {
      ++geometryReads;
      const std::int64_t turnExtent =
          GraphViewportGeometry::effectiveTurnExtent(*turn);
      if (geometryScan.position - turnExtent >= geometryScan.targetBottom) {
        geometryScan.position -= turnExtent;
        --geometryScan.turnIndex;
        continue;
      }
      if (GraphViewportGeometry::suppressesChildren(*turn)) {
        geometryScan.phase = 4;
        geometryScan.itemIndex = 0;
      } else {
        geometryScan.phase = 1;
        geometryScan.itemIndex = turn->appendedItems.size();
      }
    } else if (geometryScan.phase == 1) {
      if (geometryScan.itemIndex == 0) {
        geometryScan.phase = 2;
        geometryScan.itemIndex = turn->items.size();
        continue;
      }
      ++geometryReads;
      auto *record = turn->appendedItems[--geometryScan.itemIndex].get();
      const std::int64_t extent = recordExtent(record);
      appendIfWanted(record, geometryScan.position - extent,
                     geometryScan.position);
      geometryScan.position -= extent;
    } else if (geometryScan.phase == 2) {
      if (geometryScan.itemIndex == 0) {
        geometryScan.phase = 3;
        continue;
      }
      ++geometryReads;
      auto *record = turn->items[--geometryScan.itemIndex].get();
      const std::int64_t extent = recordExtent(record);
      appendIfWanted(record, geometryScan.position - extent,
                     geometryScan.position);
      geometryScan.position -= extent;
    } else if (geometryScan.phase == 3) {
      ++geometryReads;
      if (!GraphViewportGeometry::suppressesChildren(*turn))
        geometryScan.position -=
            static_cast<std::int64_t>(turn->hiddenBeforeItems) *
            EstimatedGraphHistoryItemExtent;
      geometryScan.phase = 4;
    } else {
      ++geometryReads;
      const std::int64_t extent = recordExtent(turn->rootItem.get());
      appendIfWanted(turn->rootItem.get(), geometryScan.position - extent,
                     geometryScan.position);
      geometryScan.position -= extent;
      geometryScan.phase = 0;
      --geometryScan.turnIndex;
    }
  }
  if (geometryScan.active && geometryReads >= MaxGeometryRecordsPerPass) {
    graphGeometry_->lastGeometryRecordsPerPass = geometryReads;
    graphGeometry_->maxGeometryRecordsPerPass =
        std::max(graphGeometry_->maxGeometryRecordsPerPass, geometryReads);
    setProperty("graphLastGeometryRecordsPerPass",
                static_cast<qulonglong>(geometryReads));
    setProperty(
        "graphMaxGeometryRecordsPerPass",
        static_cast<qulonglong>(graphGeometry_->maxGeometryRecordsPerPass));
    return true;
  }

  const std::vector<GraphViewportGeometry::DesiredGeometry> &positioned =
      geometryScan.found;
  graphGeometry_->lastGeometryRecordsPerPass = geometryReads;
  graphGeometry_->maxGeometryRecordsPerPass =
      std::max(graphGeometry_->maxGeometryRecordsPerPass, geometryReads);
  setProperty("graphLastGeometryRecordsPerPass",
              static_cast<qulonglong>(geometryReads));
  setProperty(
      "graphMaxGeometryRecordsPerPass",
      static_cast<qulonglong>(graphGeometry_->maxGeometryRecordsPerPass));

  std::unordered_set<const nodegraph::Node *> wanted;
  wanted.reserve(positioned.size());
  for (const auto &entry : positioned)
    wanted.insert(entry.record->node.get());

  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  std::size_t operations = graphPassCardOperations_;
  bool widgetRemaining = false;
  std::vector<TurnSectionWidget *> changedSections;
  std::unordered_set<const nodegraph::Node *> focusPinned;
  QWidget *focusedWidget = QApplication::focusWidget();

  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());

  for (TurnSectionWidget *section : graphSections_) {
    for (auto slot = section->cardSlots.begin();
         slot != section->cardSlots.end();) {
      if (wanted.contains(slot->graphNode.get())) {
        ++slot;
        continue;
      }
      QWidget *item = slot->itemGuard.data();
      if (focusedWidget && item &&
          (focusedWidget == item || item->isAncestorOf(focusedWidget))) {
        focusPinned.insert(slot->graphNode.get());
        ++slot;
        continue;
      }
      if (operations >= MaxCardOperationsPerPass) {
        widgetRemaining = true;
        break;
      }
      if (auto *card = slot->attachment ? qobject_cast<ConversationCard *>(
                                              slot->attachment->widget.data())
                                        : nullptr) {
        const auto outputState = card->commandOutputScrollState();
        if (outputState && !outputState->followsLatest)
          commandOutputStates_[slot->key] = *outputState;
        else
          commandOutputStates_.erase(slot->key);
        slot->measuredHeight = intrinsicGraphCardHeight(card);
        if (slot->key == section->rootKey)
          card->setNestedItems({});
      }
      if (const auto geometry =
              graphGeometry_->itemIndex.find(slot->graphNode.get());
          geometry != graphGeometry_->itemIndex.end()) {
        auto *record = geometry->second;
        graphGeometry_->setRecordMeasuredHeight(*record, slot->measuredHeight);
      }
      if (slot->graphNode && slot->attachment &&
          slot->graphNode->uiAttachment() == slot->attachment.get())
        slot->graphNode->setUiAttachment(nullptr);
      slot->attachment.reset();
      delete slot->item;
      slot = section->cardSlots.erase(slot);
      ++operations;
      if (std::ranges::find(changedSections, section) == changedSections.end())
        changedSections.push_back(section);
      section->geometryDirty = true;
    }
    if (widgetRemaining)
      break;
  }

  for (auto section = graphSections_.begin();
       section != graphSections_.end();) {
    if (!(*section)->cardSlots.empty()) {
      ++section;
      continue;
    }
    const std::size_t cost = (*section)->historyPlaceholder ? 2U : 1U;
    if (operations + cost > MaxCardOperationsPerPass) {
      widgetRemaining = true;
      ++section;
      continue;
    }
    contentLayout_->removeWidget(*section);
    delete *section;
    section = graphSections_.erase(section);
    operations += cost;
  }

  if (!widgetRemaining) {
    std::vector<const GraphViewportGeometry::DesiredGeometry *> insertionOrder;
    insertionOrder.reserve(positioned.size());
    for (const auto &entry : positioned)
      insertionOrder.push_back(&entry);
    const std::int64_t viewportBottom = scrollTop + viewportHeight;
    const auto viewportDistance = [scrollTop,
                                   viewportBottom](const auto *entry) {
      if (entry->bottom <= scrollTop)
        return scrollTop - entry->bottom;
      if (entry->top >= viewportBottom)
        return entry->top - viewportBottom;
      return std::int64_t{0};
    };
    std::ranges::stable_sort(
        insertionOrder,
        [follow, &viewportDistance](const auto *left, const auto *right) {
          const std::int64_t leftDistance = viewportDistance(left);
          const std::int64_t rightDistance = viewportDistance(right);
          if (leftDistance != rightDistance)
            return leftDistance < rightDistance;
          const bool leftRoot = left->record->turn->root == left->record->node;
          const bool rightRoot =
              right->record->turn->root == right->record->node;
          if (leftDistance == 0 && leftRoot != rightRoot)
            return leftRoot;
          // On initial following paint, give the newest bottom-visible record
          // the first materialization opportunity. Final visual order is
          // restored by desiredOrder below.
          if (follow && leftDistance == 0 && left->top != right->top)
            return left->top > right->top;
          return false;
        });
    for (const auto *positionedEntry : insertionOrder) {
      const auto &entry = *positionedEntry;
      auto existing = std::ranges::find_if(
          graphSections_, [&entry](TurnSectionWidget *section) {
            return std::ranges::any_of(
                section->cardSlots,
                [&entry](const TurnSectionWidget::CardSlot &slot) {
                  return slot.graphNode == entry.record->node;
                });
          });
      if (existing != graphSections_.end())
        continue;

      TurnSectionWidget *section = nullptr;
      const auto retained = std::ranges::find_if(
          graphSections_, [&entry](TurnSectionWidget *candidate) {
            return candidate->graphNode == entry.record->turn->node;
          });
      if (retained != graphSections_.end()) {
        section = *retained;
      } else {
        if (operations + 1 > MaxCardOperationsPerPass) {
          widgetRemaining = true;
          break;
        }
        section = new TurnSectionWidget(content_);
        section->graphNode = entry.record->turn->node;
        section->protocolId = entry.record->turn->protocolId;
        section->graphActive = entry.record->turn->active;
        section->hasAuthoritativeRoot =
            static_cast<bool>(entry.record->turn->root);
        section->authoritativeRootKey = entry.record->turn->rootItem
                                            ? entry.record->turn->rootItem->key
                                            : std::string{};
        section->setProperty("turnSectionKey",
                             QString::fromStdString(section->protocolId));
        section->setProperty("turnId",
                             QString::fromStdString(section->protocolId));
        graphSections_.push_back(section);
        ++operations;
      }

      // Preserve enough of the one shared pass budget to arrange/insert the
      // section and replace one viewport placeholder with its real card.
      constexpr std::size_t VisibilityOperationReserve = 4;
      if (operations + 1 + VisibilityOperationReserve >
          MaxCardOperationsPerPass) {
        widgetRemaining = true;
        break;
      }
      TurnSectionWidget::CardSlot slot;
      slot.graphNode = entry.record->node;
      slot.key = entry.record->key;
      slot.promptVisualId = entry.record->promptVisualId;
      slot.measuredHeight = entry.record->measuredHeight;
      slot.projectionVisible = entry.record->projectionVisible;
      slot.item = new MeasuredCardPlaceholder(
          slot.key, slot.projectionVisible ? slot.measuredHeight : 0, section);
      slot.itemGuard = slot.item;
      slot.item->setVisible(slot.projectionVisible);
      if (entry.record->node == entry.record->turn->root)
        section->rootKey = slot.key;
      section->cardSlots.push_back(std::move(slot));
      section->geometryDirty = true;
      ++operations;
      if (std::ranges::find(changedSections, section) == changedSections.end())
        changedSections.push_back(section);
    }
  }

  std::unordered_map<const nodegraph::Node *, std::size_t> desiredOrder;
  desiredOrder.reserve(positioned.size());
  for (std::size_t index = 0; index < positioned.size(); ++index)
    desiredOrder.emplace(positioned[index].record->node.get(), index);
  for (TurnSectionWidget *section : graphSections_) {
    const bool pinnedSection = std::ranges::any_of(
        section->cardSlots, [&focusPinned](const auto &slot) {
          return focusPinned.contains(slot.graphNode.get());
        });
    if (pinnedSection)
      continue;
    std::vector<const nodegraph::Node *> previousOrder;
    previousOrder.reserve(section->cardSlots.size());
    for (const TurnSectionWidget::CardSlot &slot : section->cardSlots)
      previousOrder.push_back(slot.graphNode.get());
    std::ranges::stable_sort(
        section->cardSlots,
        [&desiredOrder](const TurnSectionWidget::CardSlot &left,
                        const TurnSectionWidget::CardSlot &right) {
          const auto order = [&desiredOrder](const nodegraph::Node *node) {
            const auto found = desiredOrder.find(node);
            return found == desiredOrder.end()
                       ? std::numeric_limits<std::size_t>::max()
                       : found->second;
          };
          return order(left.graphNode.get()) < order(right.graphNode.get());
        });
    if (!std::ranges::equal(previousOrder, section->cardSlots,
                            std::ranges::equal_to{}, std::identity{},
                            [](const TurnSectionWidget::CardSlot &slot) {
                              return slot.graphNode.get();
                            }))
      section->layoutDirty = true;
    section->rootKey.clear();
    for (const TurnSectionWidget::CardSlot &slot : section->cardSlots)
      if (const auto record =
              graphGeometry_->itemIndex.find(slot.graphNode.get());
          record != graphGeometry_->itemIndex.end() &&
          record->second->turn->root == slot.graphNode)
        section->rootKey = slot.key;
  }
  if (focusPinned.empty())
    std::ranges::stable_sort(
        graphSections_,
        [&desiredOrder](TurnSectionWidget *left, TurnSectionWidget *right) {
          const auto firstOrder = [&desiredOrder](TurnSectionWidget *section) {
            std::size_t result = std::numeric_limits<std::size_t>::max();
            for (const TurnSectionWidget::CardSlot &slot : section->cardSlots)
              if (const auto found = desiredOrder.find(slot.graphNode.get());
                  found != desiredOrder.end())
                result = std::min(result, found->second);
            return result;
          };
          return firstOrder(left) < firstOrder(right);
        });

  for (TurnSectionWidget *section : changedSections)
    if (std::ranges::find(graphSections_, section) != graphSections_.end())
      section->layoutDirty = true;
  for (TurnSectionWidget *section : graphSections_) {
    if (!section->layoutDirty)
      continue;
    if (operations >= MaxCardOperationsPerPass) {
      widgetRemaining = true;
      break;
    }
    arrangeSection(section);
    section->setVisible(!section->cardSlots.empty());
    section->layoutDirty = false;
    section->geometryDirty = true;
    ++operations;
  }
  for (std::size_t index = 0; index < graphSections_.size(); ++index) {
    if (!focusPinned.empty())
      break;
    const int wantedIndex = 2 + static_cast<int>(index);
    if (contentLayout_->indexOf(graphSections_[index]) == wantedIndex)
      continue;
    if (operations >= MaxCardOperationsPerPass) {
      widgetRemaining = true;
      break;
    }
    contentLayout_->insertWidget(wantedIndex, graphSections_[index]);
    ++operations;
  }

  std::size_t liveRecordCount = 0;
  std::size_t firstCommitted = positioned.size();
  std::size_t lastCommitted = 0;
  bool committedSubset = true;
  for (TurnSectionWidget *section : graphSections_)
    for (const TurnSectionWidget::CardSlot &slot : section->cardSlots) {
      if (focusPinned.contains(slot.graphNode.get()))
        continue;
      ++liveRecordCount;
      const auto order = desiredOrder.find(slot.graphNode.get());
      if (order == desiredOrder.end()) {
        committedSubset = false;
        continue;
      }
      firstCommitted = std::min(firstCommitted, order->second);
      lastCommitted = std::max(lastCommitted, order->second);
    }
  const bool contiguousCommittedWindow =
      committedSubset && liveRecordCount != 0 &&
      firstCommitted < positioned.size() &&
      lastCommitted - firstCommitted + 1 == liveRecordCount;
  if (contiguousCommittedWindow) {
    graphGeometry_->leadingPixelExtent =
        std::max<std::int64_t>(0, positioned[firstCommitted].top);
    graphGeometry_->trailingPixelExtent = std::max<std::int64_t>(
        0, graphGeometry_->totalPixelExtent - positioned[lastCommitted].bottom);
  } else if (wanted.empty() && liveRecordCount == 0) {
    std::int64_t leading = graphGeometry_->totalPixelExtent;
    graphGeometry_->leadingPixelExtent = std::max<std::int64_t>(0, leading);
    graphGeometry_->trailingPixelExtent = 0;
  }
  const auto placeholderExtent = [](std::int64_t extent) {
    return extent == 0 ? std::int64_t{0}
                       : std::max<std::int64_t>(0, extent - CardSpacing);
  };
  static_cast<GraphHistoryPlaceholder *>(graphLeadingPlaceholder_)
      ->setPixelExtent(graphHiddenItemCount_,
                       placeholderExtent(graphGeometry_->leadingPixelExtent));
  static_cast<GraphHistoryPlaceholder *>(graphTrailingPlaceholder_)
      ->setPixelExtent(0,
                       placeholderExtent(graphGeometry_->trailingPixelExtent));
  setProperty("graphLeadingPixelExtent",
              static_cast<qlonglong>(graphGeometry_->leadingPixelExtent));
  std::int64_t firstRetained = 0;
  if (!graphGeometry_->turns.empty()) {
    const auto *firstTurn = graphGeometry_->turns.front().get();
    if (firstTurn->rootItem && firstTurn->rootItem->projectionVisible)
      firstRetained +=
          std::max(1, firstTurn->rootItem->measuredHeight) + CardSpacing;
    if (!GraphViewportGeometry::suppressesChildren(*firstTurn) &&
        (!firstTurn->items.empty() || !firstTurn->appendedItems.empty()))
      firstRetained += static_cast<std::int64_t>(firstTurn->hiddenBeforeItems) *
                       EstimatedGraphHistoryItemExtent;
  }
  if (loadMore_->isVisible())
    firstRetained += loadMore_->height() + CardSpacing;
  setProperty("graphFirstRetainedScrollValue",
              static_cast<qlonglong>(std::clamp<std::int64_t>(
                  firstRetained, 0, std::numeric_limits<int>::max())));
  setProperty("graphTrailingPixelExtent",
              static_cast<qlonglong>(graphGeometry_->trailingPixelExtent));

  graphWindowItemCount_ = 0;
  for (TurnSectionWidget *section : graphSections_)
    graphWindowItemCount_ += section->cardSlots.size();
  graphGeometry_->lastCardOperationsPerPass = operations;
  graphGeometry_->maxCardOperationsPerPass =
      std::max(graphGeometry_->maxCardOperationsPerPass, operations);
  setProperty("graphLastCardOperationsPerPass",
              static_cast<qulonglong>(operations));
  setProperty(
      "graphMaxCardOperationsPerPass",
      static_cast<qulonglong>(graphGeometry_->maxCardOperationsPerPass));
  setProperty("graphLiveRecordCount",
              static_cast<qulonglong>(graphWindowItemCount_));
  setProperty("graphLiveSectionCount",
              static_cast<qulonglong>(graphSections_.size()));
  graphPassCardOperations_ = operations;

  if (geometryScan.generation != graphGeometry_->geometryGeneration)
    graphGeometry_->geometryRerunRequired = true;
  const bool rerunGeometry = graphGeometry_->geometryRerunRequired;
  if (rerunGeometry) {
    geometryScan.active = false;
    geometryScan.valid = false;
    graphGeometry_->geometryRerunRequired = false;
  }

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
  resetGraphVisibilityScan();

  return widgetRemaining || liveRecordCount != wanted.size() || rerunGeometry;
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
  graphGeometry_->geometryScan = {};
  graphGeometry_->geometryRerunRequired = false;
  graphGeometry_->projectionCursor = nullptr;

  for (const nodegraph::NodeRef &node : removed) {
    if (!node)
      continue;
    const auto found = graphGeometry_->itemIndex.find(node.get());
    if (found == graphGeometry_->itemIndex.end())
      continue;
    GraphViewportGeometry::ItemGeometry *record = found->second;
    if (record->node != node && record->materializedPrompt == node) {
      graphGeometry_->itemIndex.erase(found);
      record->materializedPrompt.reset();
      continue;
    }
    graphGeometry_->removeRecordExtent(*record);
    graphGeometry_->itemIndex.erase(record->node.get());
    if (record->materializedPrompt)
      graphGeometry_->itemIndex.erase(record->materializedPrompt.get());
    record->node.reset();
    record->materializedPrompt.reset();
    if (record->countedOutsideHistory) {
      record->countedOutsideHistory = false;
      --graphGeometry_->extraPinnedRoots;
    }
    --graphGeometry_->retainedGeometryRecords;
    graphGeometry_->retainedHistoryItems -=
        std::min<std::size_t>(1, graphGeometry_->retainedHistoryItems);
  }
  graphGeometry_->targetDirty = true;
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
    graphGeometry_->retireCurrentStorage();
    publishRetiredGeometryCleanupMetrics();
    graphGeometry_->affected.clear();
    graphGeometry_->affectedCursor = 0;
    graphGeometry_->newestTurn.reset();
    graphGeometry_->threadStructureRevision = 0;
    graphGeometry_->threadChildCount = 0;
    graphGeometry_->totalItems = 0;
    graphGeometry_->targetHistoryItems = 0;
    graphGeometry_->forceStructureCheck = false;
    graphGeometry_->forceSelectedReset = false;
    graphGeometry_->structureValidationCursor = 0;
    graphGeometry_->structureRequestGeneration = 0;
    graphGeometry_->validationGeneration = 0;
    graphGeometry_->projectionRefreshPending = false;
    graphGeometry_->projectionCursor = nullptr;
    graphGeometry_->leadingPixelExtent = 0;
    graphGeometry_->trailingPixelExtent = 0;
    pendingGraphAnchorRestore_.reset();
    static_cast<GraphHistoryPlaceholder *>(graphLeadingPlaceholder_)
        ->setItemCount(0);
    static_cast<GraphHistoryPlaceholder *>(graphTrailingPlaceholder_)
        ->setItemCount(0);
    visibilitySectionCursor_ = 0;
    visibilitySlotCursor_ = 0;
    visibilitySlotsRemaining_ = 0;
    visibilityScanScrollTop_ = -1;
    visibilityScanViewportHeight_ = -1;
    visibilityScanViewportWidth_ = -1;
    visibilityScanContentHeight_ = -1;
    scheduleRetiredGeometryCleanup();
  } else {
    graphKnownItemCount_ -= std::min(graphKnownItemCount_, removedWindowItems);
    graphWindowItemCount_ -=
        std::min(graphWindowItemCount_, removedWindowItems);
    if (graphKnownItemCount_ == 0)
      graphHiddenItemCount_ = 0;
  }
  visibilitySlotsRemaining_ = 0;
  setProperty("graphRetainedGeometryRecordCount",
              static_cast<qulonglong>(graphGeometry_->retainedGeometryRecords));
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
  if (graph_) {
    graphGeometry_->projectionRefreshPending = true;
    graphGeometry_->projectionCursor = nullptr;
    scheduleGraphRefresh();
  }
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
  section->geometryDirty = true;
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
  if (rootSlot)
    rootSlot->cardGeometryDirty = true;
  const std::string &authoritativeRootKey =
      section->authoritativeRootKey.empty() ? section->rootKey
                                            : section->authoritativeRootKey;
  const auto savedRootFold = cardCollapsedStates_.find(authoritativeRootKey);
  const bool authoritativeRootCollapsed =
      savedRootFold != cardCollapsedStates_.end() && savedRootFold->second;

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
    if (section->historyPlaceholder) {
      section->historyPlaceholder->setVisible(
          section->historyPlaceholder->height() != 0);
      nestedItems.push_back(section->historyPlaceholder);
    }
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
      if (&slot != rootSlot) {
        slot.item->setVisible(slot.projectionVisible);
        if (auto *nestedCard = slotCard(slot);
            nestedCard &&
            !nestedCard->property("nestedConversationCard").toBool())
          slot.cardGeometryDirty = true;
        nestedItems.push_back(slot.item);
      }
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
  if (section->historyPlaceholder) {
    section->historyPlaceholder->setVisible(
        !authoritativeRootCollapsed &&
        section->historyPlaceholder->height() != 0);
    ordered.push_back({section->historyPlaceholder, nullptr});
  }
  for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
    if (&slot != rootSlot)
      ordered.push_back({slot.item, &slot});
  for (std::size_t position = 0; position < ordered.size(); ++position) {
    QWidget *item = ordered[position].widget;
    if (ordered[position].slot) {
      const bool isRoot = ordered[position].slot == rootSlot;
      item->setVisible(ordered[position].slot->projectionVisible &&
                       (isRoot || !authoritativeRootCollapsed));
      auto *card = slotCard(*ordered[position].slot);
      if (card) {
        if (card->property("turnContainer").toBool())
          card->setNestedItems({});
        const bool detachedNested = !rootSlot && section->hasAuthoritativeRoot;
        if (card->property("nestedConversationCard").toBool() != detachedNested)
          ordered[position].slot->cardGeometryDirty = true;
        card->setNestedPresentation(detachedNested);
        card->setProperty("turnContainer", false);
        card->setAuthoritativeTurnActive(false);
        card->setMinimumHeight(0);
      }
      item->setProperty("nestedConversationCard",
                        !rootSlot && section->hasAuthoritativeRoot);
    }
    if (section->cards->indexOf(item) != static_cast<int>(position))
      section->cards->insertWidget(static_cast<int>(position), item);
  }
}

void ConversationView::scheduleVisibilityPass() {
  if (applying_ || visibilityPassScheduled_ || !graph_)
    return;
  visibilityPassScheduled_ = true;
  const std::uint64_t epoch = graphBindingEpoch_;
  QTimer::singleShot(0, this, [this, epoch] {
    if (epoch != graphBindingEpoch_)
      return;
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

bool ConversationView::runVisibilityPass() {
  graphPassCardOperations_ = 0;
  if (reconcileGraphViewport())
    return true;
  return runGraphVisibilityPass();
}

bool ConversationView::runGraphVisibilityPass() {
  if (applying_ || !graph_ || !content_ || !viewport())
    return false;

  const auto finishPendingAnchorRestore = [this](bool settled) {
    if (!settled || mode_ != Mode::Paused || !pendingGraphAnchorRestore_ ||
        pendingGraphAnchorRestore_->stableKey.empty() ||
        !cardForStableKey(pendingGraphAnchorRestore_->stableKey))
      return;
    const Anchor anchor = *pendingGraphAnchorRestore_;
    pendingGraphAnchorRestore_.reset();
    restoreAnchor(anchor);
    storeCurrentThreadState();
  };

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
  const bool follow = mode_ == Mode::Following;
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
  std::size_t recoveryOperations = 0;
  bool recoveryRemaining = false;
  const auto rememberSection = [](std::vector<TurnSectionWidget *> &sections,
                                  TurnSectionWidget *section) {
    if (section && std::ranges::find(sections, section) == sections.end())
      sections.push_back(section);
  };
  for (const ScannedSlot &entry : scanned) {
    TurnSectionWidget *section = entry.section;
    TurnSectionWidget::CardSlot &slot = *entry.slot;
    if (slot.itemGuard) {
      slot.item = slot.itemGuard.data();
      continue;
    }
    if (graphPassCardOperations_ + recoveryOperations >=
        MaxCardOperationsPerPass) {
      recoveryRemaining = true;
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
    ++recoveryOperations;
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
  const QRect retentionRect = materializationRect;
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
      graphPassCardOperations_ += recoveryOperations;
      visibilitySectionCursor_ = batchStartSection;
      visibilitySlotCursor_ = batchStartSlot;
      visibilitySlotsRemaining_ = batchStartRemaining;
      scheduleVisibilityContentionRetry();
      return false;
    }
    for (Candidate &candidate : candidates) {
      if (candidate.operation == Operation::Release ||
          !candidate.slot->graphNode)
        continue;
      if (!read->contains(candidate.slot->graphNode) ||
          read->removed(candidate.slot->graphNode)) {
        candidate.operation = Operation::Release;
        continue;
      }
      candidate.nodeRevision = read->changedRevision(candidate.slot->graphNode);
      if (candidate.operation == Operation::Render &&
          candidate.renderedRevision >= candidate.nodeRevision)
        continue;
      candidate.state = read->state(candidate.slot->graphNode);
    }
    read.reset();
  }
  std::erase_if(candidates, [](const Candidate &candidate) {
    return candidate.operation == Operation::Render && !candidate.state;
  });

  std::ranges::stable_sort(candidates, [this, follow](const Candidate &left,
                                                      const Candidate &right) {
    if (left.inViewport != right.inViewport)
      return left.inViewport > right.inViewport;
    const auto urgency = [](Operation operation) {
      return operation == Operation::Release ? 1 : 0;
    };
    if (urgency(left.operation) != urgency(right.operation))
      return urgency(left.operation) < urgency(right.operation);
    if (left.distance != right.distance)
      return left.distance < right.distance;
    const bool leftRoot =
        left.section && left.slot && left.slot->key == left.section->rootKey;
    const bool rightRoot = right.section && right.slot &&
                           right.slot->key == right.section->rootKey;
    if (left.inViewport && leftRoot != rightRoot)
      return leftRoot;
    if (follow && left.inViewport && left.slot && right.slot &&
        left.slot->itemGuard && right.slot->itemGuard)
      return left.slot->itemGuard->mapTo(content_, QPoint{}).y() >
             right.slot->itemGuard->mapTo(content_, QPoint{}).y();
    return false;
  });
  if (candidates.empty()) {
    graphPassCardOperations_ += recoveryOperations;
    graphGeometry_->lastCardOperationsPerPass = graphPassCardOperations_;
    graphGeometry_->maxCardOperationsPerPass = std::max(
        graphGeometry_->maxCardOperationsPerPass, graphPassCardOperations_);
    setProperty("graphLastCardOperationsPerPass",
                static_cast<qulonglong>(graphPassCardOperations_));
    setProperty(
        "graphMaxCardOperationsPerPass",
        static_cast<qulonglong>(graphGeometry_->maxCardOperationsPerPass));
    const bool remaining = recoveryRemaining || !recoveredSections.empty() ||
                           visibilitySlotsRemaining_ != 0 ||
                           graphGeometry_->geometryScan.active ||
                           !graphGeometry_->geometryScan.valid;
    finishPendingAnchorRestore(!remaining);
    return remaining;
  }

  const Anchor anchor = captureAnchor();

  const bool followedBottom = follow && verticalScrollBar()->value() >=
                                            verticalScrollBar()->maximum() - 1;
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
  int operations =
      static_cast<int>(graphPassCardOperations_ + recoveryOperations);
  bool workRemaining = false;
  bool geometryChanged = false;
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
      slot.cardGeometryDirty = true;
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
      slot.cardGeometryDirty = true;
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
    slot.measuredHeight = intrinsicGraphCardHeight(card);
    if (slot.key == candidate.section->rootKey) {
      card->setNestedItems({});
      card->setMinimumHeight(0);
      if (card->layout()) {
        card->layout()->invalidate();
        card->layout()->activate();
      }
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
  for (TurnSectionWidget *section : changedSections) {
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
      auto *card =
          slot.attachment
              ? qobject_cast<ConversationCard *>(slot.attachment->widget.data())
              : nullptr;
      if (!card)
        continue;
      const int measured = intrinsicGraphCardHeight(card);
      if (measured == slot.measuredHeight)
        continue;
      const auto geometry =
          graphGeometry_->itemIndex.find(slot.graphNode.get());
      if (geometry != graphGeometry_->itemIndex.end()) {
        GraphViewportGeometry::ItemGeometry *record = geometry->second;
        if (record->measuredHeight != measured) {
          graphGeometry_->setRecordMeasuredHeight(*record, measured);
          geometryChanged = true;
        }
      }
      slot.measuredHeight = measured;
    }
  }
  for (const auto &[card, state] : outputRestorations)
    card->restoreCommandOutputScrollState(state);
  if (followedBottom)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
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
  graphGeometry_->lastCardOperationsPerPass = operations;
  graphGeometry_->maxCardOperationsPerPass =
      std::max(graphGeometry_->maxCardOperationsPerPass,
               static_cast<std::size_t>(operations));
  setProperty("graphLastCardOperationsPerPass", operations);
  setProperty(
      "graphMaxCardOperationsPerPass",
      static_cast<qulonglong>(graphGeometry_->maxCardOperationsPerPass));
  graphPassCardOperations_ = static_cast<std::size_t>(operations);
  if (geometryChanged)
    scheduleGraphRefresh();
  return workRemaining || recoveryRemaining || visibilitySlotsRemaining_ != 0 ||
         operations > 0;
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
  pendingGraphAnchorRestore_.reset();
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
  for (TurnSectionWidget *section : graphSections_) {
    bool changed = false;
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
      if (slot.key == key ||
          (!section->rootKey.empty() && slot.key == section->rootKey)) {
        slot.cardGeometryDirty = true;
        changed = true;
      }
    }
    section->geometryDirty = section->geometryDirty || changed;
  }
  recomputeGeometry();
  const auto updateMeasuredHeight = [this, &key, card,
                                     collapsed](TurnSectionWidget *section) {
    const auto slot = std::ranges::find_if(
        section->cardSlots,
        [&key](const TurnSectionWidget::CardSlot &candidate) {
          return candidate.key == key;
        });
    if (slot == section->cardSlots.end())
      return;
    const int measured = intrinsicGraphCardHeight(card);
    slot->measuredHeight = measured;
    const auto geometry = graphGeometry_->itemIndex.find(slot->graphNode.get());
    if (geometry == graphGeometry_->itemIndex.end())
      return;
    GraphViewportGeometry::ItemGeometry *record = geometry->second;
    graphGeometry_->setRecordMeasuredHeight(*record, measured);
    if (key == section->rootKey)
      graphGeometry_->setRootCollapsed(*record->turn, collapsed);
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
    for (TurnSectionWidget *section : graphSections_) {
      section->geometryDirty = true;
      for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
        slot.cardGeometryDirty = true;
    }
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
  for (TurnSectionWidget *section : graphSections_) {
    section->geometryDirty = true;
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
      slot.cardGeometryDirty = true;
  }
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
  if (pendingGraphAnchorRestore_)
    return *pendingGraphAnchorRestore_;
  Anchor anchor;
  anchor.absoluteValue = verticalScrollBar()->value();
  const auto capture = [this, &anchor](const std::string &key) {
    QWidget *item = itemForStableKey(key);
    if (!item || !item->isVisible())
      return false;
    const int viewportTop = item->mapTo(viewport(), QPoint(0, 0)).y();
    if (viewportTop + item->height() < 0 || viewportTop >= viewport()->height())
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
    QWidget *nested = card->findChild<QWidget *>(
        QStringLiteral("conversationNestedCards"), Qt::FindDirectChildrenOnly);
    if (!nested || !nested->layout())
      continue;
    const int cardWidth = card->parentWidget()
                              ? card->parentWidget()->contentsRect().width()
                              : card->width();
    card->setMinimumHeight(0);
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
    QCoreApplication::removePostedEvents(card, QEvent::LayoutRequest);
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
  pendingGraphAnchorRestore_.reset();
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

  pendingGraphAnchorRestore_.reset();
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
