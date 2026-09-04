// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/InspectorPane.h"

#include "codex/DiffViewer.h"
#include "codex/UiStatus.h"
#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {

constexpr int MaximumProtocolLines = 2000;
constexpr std::size_t MaximumRetainedTelemetry = 256;
constexpr int InfoChoicePage = 0;
constexpr int StatePage = 1;
constexpr int ProtocolPage = 2;
constexpr qsizetype MaximumGraphDiagnosticCharacters = 32 * 1024;
constexpr std::size_t MaximumInspectorWidgetChangesPerPass = 12;
constexpr std::size_t MaximumInspectorGraphReadsPerPass = 64;
constexpr int InspectorGraphLockRetryMilliseconds = 4;
constexpr std::size_t MaximumInspectorChangeNodesToInspect = 64;
constexpr std::size_t MaximumInspectorAncestryDepth = 16;
constexpr std::size_t MaximumInspectorMaterializedRows = 48;
constexpr std::size_t MaximumStateDomainEntries = 96;
constexpr std::size_t MaximumStatePendingEntries = 64;
constexpr std::size_t InspectorRowOverscan = 2;
constexpr int PlanEstimatedRowHeight = 72;
constexpr int AgentEstimatedRowHeight = 56;
constexpr int RequestEstimatedRowHeight = 144;

struct InspectorRowWindow final {
  std::size_t first = 0;
  std::size_t end = 0;
};

std::size_t planComponentCount(const InspectorPlanData &snapshot) {
  return snapshot.totalRows;
}

std::size_t agentsComponentCount(const InspectorAgentsData &snapshot) {
  return snapshot.totalRows;
}

std::size_t requestsComponentCount(const InspectorRequestsData &snapshot) {
  return snapshot.totalRows;
}

std::size_t renderedPlanComponentCount(const InspectorPlanData &snapshot) {
  if (!snapshot.threadPresent || !snapshot.plan)
    return 1;
  return snapshot.plan->steps.size() +
         (snapshot.plan->includesExplanation ? 1U : 0U);
}

std::size_t renderedAgentsComponentCount(const InspectorAgentsData &snapshot) {
  return snapshot.agents.empty() && snapshot.totalRows == 1
             ? 1
             : snapshot.agents.size();
}

std::size_t
renderedRequestsComponentCount(const InspectorRequestsData &snapshot) {
  return snapshot.requests.empty() && snapshot.totalRows == 1
             ? 1
             : snapshot.requests.size();
}

InspectorRowWindow visibleRowWindow(QScrollArea *scroll, std::size_t rowCount,
                                    int estimatedRowHeight, int scrollValue) {
  if (rowCount == 0)
    return {};
  const std::size_t viewportRows = std::max<std::size_t>(
      1, (static_cast<std::size_t>(std::max(1, scroll->viewport()->height())) +
          static_cast<std::size_t>(estimatedRowHeight) - 1) /
             static_cast<std::size_t>(estimatedRowHeight));
  const std::size_t scrollableRows =
      rowCount > viewportRows ? rowCount - viewportRows : 0;
  const int scrollMaximum = scroll->verticalScrollBar()->maximum();
  const double scrollFraction =
      scrollMaximum > 0
          ? static_cast<double>(std::clamp(scrollValue, 0, scrollMaximum)) /
                static_cast<double>(scrollMaximum)
          : 0.0;
  const std::size_t firstVisible = static_cast<std::size_t>(
      scrollFraction * static_cast<double>(scrollableRows));
  const std::size_t first = firstVisible > InspectorRowOverscan
                                ? firstVisible - InspectorRowOverscan
                                : 0;
  const std::size_t desiredEnd = std::min(
      rowCount, firstVisible + viewportRows + InspectorRowOverscan + 1);
  return {first,
          std::min(desiredEnd, first + MaximumInspectorMaterializedRows)};
}

int estimatedSpacerHeight(std::size_t rows, int estimatedRowHeight) {
  constexpr std::size_t MaximumSpacerHeight =
      static_cast<std::size_t>(std::numeric_limits<int>::max() / 4);
  const std::size_t height = static_cast<std::size_t>(estimatedRowHeight);
  return static_cast<int>(rows > MaximumSpacerHeight / height
                              ? MaximumSpacerHeight
                              : rows * height);
}

bool materializedRowsCoverViewport(QVBoxLayout *layout, QScrollArea *scroll,
                                   std::size_t firstRow, std::size_t endRow,
                                   std::size_t totalRows) {
  QWidget *firstWidget = nullptr;
  QWidget *lastWidget = nullptr;
  for (int index = 0; index < layout->count(); ++index) {
    QWidget *widget = layout->itemAt(index)->widget();
    if (!widget)
      continue;
    if (!firstWidget)
      firstWidget = widget;
    lastWidget = widget;
  }
  if (!firstWidget || !lastWidget)
    return false;
  const int viewportTop = scroll->verticalScrollBar()->value();
  const int viewportBottom = viewportTop + scroll->viewport()->height();
  const bool coversTop =
      firstRow == 0 || viewportTop >= firstWidget->geometry().top();
  const bool coversBottom =
      endRow == totalRows || viewportBottom <= lastWidget->geometry().bottom();
  return coversTop && coversBottom;
}

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
  const UiStatus classified = classifyStatus(status);
  auto *label = makeLabel(text(displayStatus(status)), "meta");
  if (!classified.tone.empty())
    label->setProperty("tone", classified.tone.data());
  return label;
}

QLabel *makeMarkdownLabel(const QString &value) {
  QTextDocument document;
  document.setMarkdown(value, QTextDocument::MarkdownFeatures(
                                  QTextDocument::MarkdownDialectGitHub) |
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

class AgentDisclosureButton final : public QToolButton {
public:
  explicit AgentDisclosureButton(QWidget *parent = nullptr)
      : QToolButton(parent) {
    setObjectName(QStringLiteral("agentDisclosureButton"));
    setFixedSize(14, 24);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setExpanded(false);
  }

  void setExpanded(bool expanded) {
    expanded_ = expanded;
    setAccessibleName(expanded ? QStringLiteral("Collapse agent")
                               : QStringLiteral("Expand agent"));
    setToolTip(accessibleName());
    update();
  }

  [[nodiscard]] bool isExpanded() const noexcept { return expanded_; }

protected:
  void paintEvent(QPaintEvent *event) override {
    static_cast<void>(event);
    QRect indicator(0, 3, 12, height() - 6);
    indicator.translate(expanded_ ? 3 : 5, 0);
    UiStyle::drawChevron(this, indicator, isEnabled(),
                         underMouse() || hasFocus(),
                         expanded_ ? UiStyle::ChevronDirection::Down
                                   : UiStyle::ChevronDirection::Left);
  }

private:
  bool expanded_ = false;
};

class AgentCopyButton final : public QToolButton {
public:
  explicit AgentCopyButton(QWidget *parent = nullptr) : QToolButton(parent) {
    setObjectName(QStringLiteral("agentCopyButton"));
    setFixedSize(16, 24);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Copy agent content"));
    setToolTip(accessibleName());
  }

  void showCopiedFeedback() {
    copied_ = true;
    update();
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, height())),
                       QStringLiteral("Copied"), this, rect(), 500);
    QTimer::singleShot(500, this, [this] {
      copied_ = false;
      update();
    });
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    static_cast<void>(event);
    QColor color(copied_ ? QStringLiteral("#176b45")
                         : QStringLiteral("#667085"));
    if (!copied_ && (underMouse() || hasFocus()))
      color = QColor(QStringLiteral("#1d2633"));
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(
        QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (copied_) {
      QPainterPath check;
      check.moveTo(3.0, 12.0);
      check.lineTo(6.5, 15.5);
      check.lineTo(14.0, 7.5);
      painter.drawPath(check);
      return;
    }
    painter.drawRoundedRect(QRectF(3.5, 4.5, 8.0, 9.0), 1.2, 1.2);
    painter.drawRoundedRect(QRectF(6.5, 7.5, 8.0, 9.0), 1.2, 1.2);
  }

private:
  bool copied_ = false;
};

QString agentCopyText(const InspectorAgentRender &agent) {
  QStringList lines{QStringLiteral("Agent")};
  if (!agent.agentPath.empty())
    lines << QStringLiteral("Path: %1").arg(text(agent.agentPath));
  if (!agent.status.empty())
    lines
        << QStringLiteral("Status: %1").arg(text(displayStatus(agent.status)));
  if (!agent.tool.empty())
    lines << QStringLiteral("Tool: %1").arg(text(agent.tool));
  if (!agent.model.empty())
    lines << QStringLiteral("Model: %1").arg(text(agent.model));
  if (!agent.reasoningEffort.empty())
    lines << QStringLiteral("Reasoning: %1").arg(text(agent.reasoningEffort));
  if (!agent.prompt.empty())
    lines << QString{} << text(agent.prompt);
  if (!agent.resultText.empty())
    lines << QString{} << text(agent.resultText);
  if (!agent.childThreadId.empty())
    lines << QString{}
          << QStringLiteral("Thread: %1").arg(text(agent.childThreadId));
  if (!agent.senderThreadId.empty())
    lines << QStringLiteral("Sender: %1").arg(text(agent.senderThreadId));
  if (!agent.receiverThreadIds.empty())
    lines << QStringLiteral("Receivers: %1")
                 .arg(
                     texts(agent.receiverThreadIds).join(QStringLiteral(", ")));
  return lines.join(QLatin1Char('\n'));
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

bool deleteLastLayoutItem(QLayout *layout) {
  if (!layout || layout->count() == 0)
    return false;
  QLayoutItem *item = layout->takeAt(layout->count() - 1);
  if (QWidget *widget = item->widget())
    delete widget;
  if (QLayout *child = item->layout()) {
    clearLayout(child);
    delete child;
  }
  delete item;
  return true;
}

class InfoChoiceButton final : public QPushButton {
protected:
  void paintEvent(QPaintEvent *event) override {
    QPushButton::paintEvent(event);
    QStyleOptionButton option;
    initStyleOption(&option);
    const QRect contents =
        style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
    const QRect indicator(contents.right() - 18, contents.top(), 18,
                          contents.height());
    UiStyle::drawChevron(this, indicator, option.state & QStyle::State_Enabled,
                         option.state &
                             (QStyle::State_MouseOver | QStyle::State_HasFocus),
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

const nodegraph::Value *graphField(const nodegraph::NodeState &state,
                                   std::string_view name) {
  const auto found = state.fields.find(name);
  return found == state.fields.end() ? nullptr : &found->second;
}

const nodegraph::Value *graphField(const nodegraph::Value::Object &object,
                                   std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

std::string graphString(const nodegraph::Value *value) {
  if (!value)
    return {};
  if (const std::string *string = value->asString())
    return *string;
  if (const nodegraph::Value::Object *object = value->asObject()) {
    if (const nodegraph::Value *type = graphField(*object, "type"))
      return graphString(type);
  }
  return {};
}

bool sensitiveDiagnosticText(std::string_view value);

QString diagnosticIdentifier(std::string_view value,
                             qsizetype maximumCharacters = 160) {
  if (value.empty())
    return {};
  if (sensitiveDiagnosticText(value))
    return QStringLiteral("<redacted identifier>");
  QString result = text(value);
  for (qsizetype index = 0; index < result.size(); ++index) {
    const QChar character = result.at(index);
    if (character.unicode() < 0x20U || character.unicode() == 0x7fU)
      result[index] = QLatin1Char(' ');
  }
  if (result.size() > maximumCharacters) {
    result.truncate(maximumCharacters);
    result += QStringLiteral("...");
  }
  return result;
}

std::optional<std::uint64_t> graphUnsigned(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const std::uint64_t *number = value->asUInt64())
    return *number;
  if (const std::int64_t *number = value->asInt64(); number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return std::nullopt;
}

QString protocolMetadata(const nodegraph::Value *value,
                         qsizetype maximumCharacters = 240) {
  std::string raw = graphString(value);
  if (raw.empty())
    return {};
  if (sensitiveDiagnosticText(raw))
    return QStringLiteral("<redacted sensitive text>");
  QString result = text(raw);
  for (qsizetype index = 0; index < result.size(); ++index) {
    const QChar character = result.at(index);
    if (character.unicode() < 0x20U || character.unicode() == 0x7fU)
      result[index] = QLatin1Char(' ');
  }
  if (result.size() > maximumCharacters) {
    result.truncate(maximumCharacters);
    result += QStringLiteral("...");
  }
  return result;
}

bool graphBool(const nodegraph::Value *value) {
  return value && value->asBool() && *value->asBool();
}

std::string graphStatus(const nodegraph::NodeState &state) {
  if (std::string status = graphString(graphField(state, "status"));
      !status.empty())
    return status;
  switch (state.status) {
  case nodegraph::NodeStatus::Pending:
    return "pending";
  case nodegraph::NodeStatus::Running:
    return "running";
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

bool hasStructuredPlan(const nodegraph::NodeState &state) {
  const nodegraph::Value *planValue = graphField(state, "plan");
  return (planValue && planValue->asArray()) ||
         (planValue && planValue->asObject() &&
          graphField(*planValue->asObject(), "steps"));
}

bool terminalStatus(std::string_view status) {
  const StatusKind kind = classifyStatus(status).kind;
  return kind == StatusKind::Completed || kind == StatusKind::Failed ||
         kind == StatusKind::Interrupted;
}

bool spawnAgentTool(std::string_view tool) {
  return tool == "spawn_agent" || tool == "spawnAgent" ||
         tool == "spawn_agents_on_csv" || tool == "spawnAgentsOnCsv";
}

std::string agentActivityStatus(const nodegraph::NodeState &state) {
  const std::string kind = graphString(graphField(state, "kind"));
  if (kind == "completed" || kind == "interrupted" || kind == "failed")
    return kind;
  if (kind == "interacted")
    return {};
  if (std::string status = graphString(graphField(state, "status"));
      !status.empty())
    return status;
  const std::string publishedStatus = graphStatus(state);
  if (terminalStatus(publishedStatus))
    return publishedStatus;
  if (kind == "started" || kind == "progress")
    return "inProgress";
  return publishedStatus;
}

void updateAgentStatus(std::string &current, std::string candidate) {
  if (candidate.empty() ||
      (terminalStatus(current) && isActiveStatus(candidate)))
    return;
  current = std::move(candidate);
}

std::string_view nodeKindName(nodegraph::NodeKind kind) {
  switch (kind) {
  case nodegraph::NodeKind::Runtime:
    return "Runtime";
  case nodegraph::NodeKind::Connection:
    return "Connection";
  case nodegraph::NodeKind::Thread:
    return "Thread";
  case nodegraph::NodeKind::Turn:
    return "Turn";
  case nodegraph::NodeKind::Item:
    return "Item";
  case nodegraph::NodeKind::Interaction:
    return "Interaction";
  case nodegraph::NodeKind::Operation:
    return "Operation";
  case nodegraph::NodeKind::Catalog:
    return "Catalog";
  case nodegraph::NodeKind::CatalogEntry:
    return "CatalogEntry";
  case nodegraph::NodeKind::Account:
    return "Account";
  case nodegraph::NodeKind::Configuration:
    return "Configuration";
  case nodegraph::NodeKind::PermissionProfile:
    return "PermissionProfile";
  case nodegraph::NodeKind::Skill:
    return "Skill";
  case nodegraph::NodeKind::Hook:
    return "Hook";
  case nodegraph::NodeKind::Plugin:
    return "Plugin";
  case nodegraph::NodeKind::App:
    return "App";
  case nodegraph::NodeKind::McpServer:
    return "McpServer";
  case nodegraph::NodeKind::Project:
    return "Project";
  case nodegraph::NodeKind::ThreadSection:
    return "ThreadSection";
  case nodegraph::NodeKind::Process:
    return "Process";
  case nodegraph::NodeKind::RealtimeSession:
    return "RealtimeSession";
  case nodegraph::NodeKind::FilesystemWatch:
    return "FilesystemWatch";
  case nodegraph::NodeKind::ExternalAgentImport:
    return "ExternalAgentImport";
  case nodegraph::NodeKind::FuzzyFileSearchSession:
    return "FuzzyFileSearchSession";
  case nodegraph::NodeKind::LoginAttempt:
    return "LoginAttempt";
  case nodegraph::NodeKind::Notice:
    return "Notice";
  case nodegraph::NodeKind::UnknownProtocol:
    return "UnknownProtocol";
  }
  return "UnknownProtocol";
}

std::string requestKind(std::string_view method) {
  if (method == "item/commandExecution/requestApproval")
    return "command-approval";
  if (method == "item/fileChange/requestApproval")
    return "file-change-approval";
  if (method == "item/tool/requestUserInput")
    return "user-input";
  if (method == "mcpServer/elicitation/request")
    return "mcp-elicitation";
  if (method == "item/permissions/requestApproval")
    return "permissions-approval";
  if (method == "item/tool/call")
    return "dynamic-tool-call";
  if (method == "account/chatgptAuthTokens/refresh")
    return "authentication-refresh";
  if (method == "attestation/generate")
    return "attestation";
  if (method == "applyPatchApproval")
    return "legacy-patch-approval";
  if (method == "execCommandApproval")
    return "legacy-command-approval";
  return "unsupported";
}

void appendUniqueBounded(std::vector<std::string> &values, std::string value,
                         std::size_t maximum) {
  if (value.empty() ||
      std::find(values.begin(), values.end(), value) != values.end())
    return;
  if (values.size() == maximum)
    values.erase(values.begin());
  values.emplace_back(std::move(value));
}

void appendDiagnostic(QString &target, QString value) {
  if (target.size() >= MaximumGraphDiagnosticCharacters)
    return;
  const qsizetype remaining = MaximumGraphDiagnosticCharacters - target.size();
  if (value.size() > remaining)
    value.truncate(remaining);
  target += value;
}

std::string normalizedDiagnosticKey(std::string_view key) {
  std::string normalized;
  normalized.reserve(key.size());
  for (const unsigned char character : key) {
    if (std::isalnum(character))
      normalized.push_back(static_cast<char>(std::tolower(character)));
  }
  return normalized;
}

bool redactedGraphField(std::string_view key) {
  const std::string normalized = normalizedDiagnosticKey(key);
  if (normalized == "payload" || normalized == "requestpayload" ||
      normalized == "responsepayload" ||
      normalized == "retainedresponsepayload" || normalized == "raw" ||
      normalized == "private" || normalized == "bytes" ||
      normalized == "command" || normalized == "prompt" ||
      normalized == "input" || normalized == "output" ||
      normalized == "delta" || normalized == "error" || normalized == "message")
    return true;
  constexpr std::array sensitive{
      std::string_view("password"),      std::string_view("secret"),
      std::string_view("authorization"), std::string_view("cookie"),
      std::string_view("credential"),    std::string_view("apikey"),
      std::string_view("accesskey"),     std::string_view("privatekey"),
      std::string_view("accesstoken"),   std::string_view("refreshtoken"),
      std::string_view("idtoken"),       std::string_view("authtoken")};
  if (normalized == "token" || normalized.ends_with("token"))
    return true;
  return std::ranges::any_of(sensitive, [&normalized](std::string_view marker) {
    return normalized.find(marker) != std::string::npos;
  });
}

bool sensitiveDiagnosticText(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const unsigned char character : value)
    lowered.push_back(static_cast<char>(std::tolower(character)));
  constexpr std::array sensitive{
      std::string_view("authorization"), std::string_view("bearer "),
      std::string_view("password="),     std::string_view("password:"),
      std::string_view("password "),     std::string_view("secret="),
      std::string_view("secret:"),       std::string_view("secret "),
      std::string_view("token="),        std::string_view("token:"),
      std::string_view("credential="),   std::string_view("credential:"),
      std::string_view("cookie="),       std::string_view("cookie:"),
      std::string_view("-----begin"),    std::string_view("github_pat_"),
      std::string_view("ghp_"),          std::string_view("xoxb-"),
      std::string_view("xoxp-")};
  if (std::ranges::any_of(sensitive, [&lowered](std::string_view marker) {
        return lowered.find(marker) != std::string::npos;
      }))
    return true;
  if (lowered.find("sk-") != std::string::npos)
    return true;
  const std::size_t jwt = value.find("eyJ");
  if (jwt != std::string_view::npos) {
    const std::size_t firstDot = value.find('.', jwt);
    if (firstDot != std::string_view::npos &&
        value.find('.', firstDot + 1) != std::string_view::npos)
      return true;
  }
  return false;
}

bool stateDomainKind(nodegraph::NodeKind kind) {
  return kind != nodegraph::NodeKind::Thread &&
         kind != nodegraph::NodeKind::Turn &&
         kind != nodegraph::NodeKind::Item &&
         kind != nodegraph::NodeKind::Interaction &&
         kind != nodegraph::NodeKind::Operation &&
         kind != nodegraph::NodeKind::UnknownProtocol;
}

void appendGraphObject(QString &target, const nodegraph::Value::Object &object,
                       int indentation, int depth);

void appendGraphValue(QString &target, const nodegraph::Value &value,
                      int indentation = 0, int depth = 0) {
  if (target.size() >= MaximumGraphDiagnosticCharacters)
    return;
  if (depth > 12) {
    appendDiagnostic(target, QStringLiteral("<nested value>"));
    return;
  }
  if (value.isNull()) {
    appendDiagnostic(target, QStringLiteral("null"));
  } else if (const bool *boolean = value.asBool()) {
    appendDiagnostic(target, *boolean ? QStringLiteral("true")
                                      : QStringLiteral("false"));
  } else if (const std::int64_t *number = value.asInt64()) {
    appendDiagnostic(target, QString::number(*number));
  } else if (const std::uint64_t *number = value.asUInt64()) {
    appendDiagnostic(target, QString::number(*number));
  } else if (const double *number = value.asDouble()) {
    appendDiagnostic(target, QString::number(*number, 'g', 15));
  } else if (const std::string *string = value.asString()) {
    if (sensitiveDiagnosticText(*string)) {
      appendDiagnostic(target, QStringLiteral("<redacted sensitive text>"));
      return;
    }
    const qsizetype remaining = std::max<qsizetype>(
        0, MaximumGraphDiagnosticCharacters - target.size() - 2);
    const std::size_t maximumBytes = static_cast<std::size_t>(remaining);
    const std::size_t bytes = std::min(string->size(), maximumBytes);
    QString displayed =
        QString::fromUtf8(string->data(), static_cast<qsizetype>(bytes));
    displayed.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    displayed.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    if (displayed.size() > remaining)
      displayed.truncate(remaining);
    appendDiagnostic(target,
                     QStringLiteral("\"") + displayed + QStringLiteral("\""));
  } else if (const nodegraph::Value::Array *array = value.asArray()) {
    if (array->empty()) {
      appendDiagnostic(target, QStringLiteral("[]"));
      return;
    }
    appendDiagnostic(target, QStringLiteral("[\n"));
    for (const nodegraph::Value &entry : *array) {
      appendDiagnostic(target, QString(indentation + 2, QLatin1Char(' ')) +
                                   QStringLiteral("- "));
      appendGraphValue(target, entry, indentation + 2, depth + 1);
      appendDiagnostic(target, QStringLiteral("\n"));
      if (target.size() >= MaximumGraphDiagnosticCharacters)
        break;
    }
    appendDiagnostic(target, QString(indentation, QLatin1Char(' ')) +
                                 QStringLiteral("]"));
  } else if (const nodegraph::Value::Object *object = value.asObject())
    appendGraphObject(target, *object, indentation, depth);
}

void appendGraphObject(QString &target, const nodegraph::Value::Object &object,
                       int indentation, int depth) {
  if (object.empty()) {
    appendDiagnostic(target, QStringLiteral("{}"));
    return;
  }
  bool namedSensitiveValue = false;
  for (std::string_view label : {"name", "key"}) {
    const auto found = object.find(label);
    if (found == object.end() || !found->second.asString())
      continue;
    if (redactedGraphField(*found->second.asString()) ||
        sensitiveDiagnosticText(*found->second.asString())) {
      namedSensitiveValue = true;
      break;
    }
  }
  appendDiagnostic(target, QStringLiteral("{\n"));
  for (const auto &[key, entry] : object) {
    appendDiagnostic(target, QString(indentation + 2, QLatin1Char(' ')) +
                                 diagnosticIdentifier(key) +
                                 QStringLiteral(": "));
    if (redactedGraphField(key) ||
        (namedSensitiveValue && normalizedDiagnosticKey(key) == "value"))
      appendDiagnostic(target, QStringLiteral("<redacted>"));
    else
      appendGraphValue(target, entry, indentation + 2, depth + 1);
    appendDiagnostic(target, QStringLiteral("\n"));
    if (target.size() >= MaximumGraphDiagnosticCharacters)
      break;
  }
  appendDiagnostic(target, QString(indentation, QLatin1Char(' ')) +
                               QStringLiteral("}"));
}

} // namespace

struct InspectorPane::PlanGraphScan final {
  std::uint64_t revision = 0;
  nodegraph::NodeRef thread;
  std::shared_ptr<const nodegraph::NodeState> threadState;
  std::unordered_set<const nodegraph::Node *> dependencies;
  std::size_t turnCursor = 0;
  nodegraph::NodeRef currentTurn;
  std::shared_ptr<const nodegraph::NodeState> currentTurnState;
  std::size_t itemCursor = 0;
  nodegraph::NodeRef sourceTurn;
  std::shared_ptr<const nodegraph::NodeState> sourceTurnState;
  nodegraph::NodeRef sourceItem;
  std::shared_ptr<const nodegraph::NodeState> sourceItemState;
};

struct InspectorPane::AgentsGraphScan final {
  enum class Phase {
    MainTurns,
    MainItems,
    ChildRelations,
    SourceChildren,
    PrepareChild,
    ChildTurns,
    ChildItems,
  };

  struct SourceChild final {
    std::string key;
    std::string id;
    nodegraph::NodeRef thread;
    std::string status;
    std::string resultText;
    bool hasCanonicalThreadId = true;
  };

  struct ChildFacts final {
    std::string status;
    std::string resultText;
  };

  struct LogicalAgent final {
    std::size_t rowIndex = 0;
    std::optional<std::size_t> renderIndex;
    std::string status;
  };

  std::uint64_t revision = 0;
  nodegraph::NodeRef thread;
  std::unordered_set<const nodegraph::Node *> dependencies;
  InspectorAgentsData snapshot;
  std::size_t renderEnd = 0;
  std::size_t matchingCount = 0;
  Phase phase = Phase::MainTurns;
  std::size_t turnCursor = 0;
  std::size_t turnCount = 0;
  nodegraph::NodeRef currentTurn;
  std::size_t itemCursor = 0;
  std::size_t itemCount = 0;
  nodegraph::NodeRef sourceItem;
  std::shared_ptr<const nodegraph::NodeState> sourceState;
  bool sourceCanCreate = false;
  std::vector<SourceChild> sourceChildren;
  std::unordered_map<std::string, std::size_t> sourceChildIndexes;
  std::vector<std::string> sourceReceiverIds;
  std::size_t sourceFieldPhase = 0;
  std::size_t receiverCursor = 0;
  nodegraph::Value::Object::const_iterator agentStateCursor;
  bool agentStateCursorInitialized = false;
  std::size_t sourceChildCursor = 0;
  nodegraph::NodeRef childThread;
  std::shared_ptr<const nodegraph::NodeState> childState;
  std::string childResultText;
  std::size_t relationCursor = 0;
  std::size_t relationCount = 0;
  std::size_t childTurnCursor = 0;
  nodegraph::NodeRef currentChildTurn;
  std::size_t childItemCursor = 0;
  std::size_t logicalAgentCount = 0;
  std::unordered_map<std::string, LogicalAgent> logicalAgents;
  // Full child result text is retained only for the requested viewport rows.
  std::unordered_map<std::string, ChildFacts> visibleChildFacts;
};

struct InspectorPane::RequestsGraphScan final {
  std::uint64_t revision = 0;
  nodegraph::NodeRef runtime;
  nodegraph::NodeRef connection;
  std::unordered_set<const nodegraph::Node *> dependencies;
  InspectorRequestsData snapshot;
  bool canControl = false;
  std::uint64_t generation = 0;
  std::size_t pendingCount = 0;
  std::size_t pendingCursor = 0;
  std::size_t matchingCount = 0;
  std::size_t renderEnd = 0;
  nodegraph::NodeRef currentInteraction;
  std::optional<InspectorRequestRender> currentRow;
  std::size_t targetCount = 0;
  std::size_t targetCursor = 0;
  nodegraph::NodeRef targetAncestor;
  std::size_t targetAncestryDepth = 0;
};

struct InspectorPane::ChangesGraphScan final {
  enum class Phase { Turns, Items, ChangeEntries };

  std::uint64_t revision = 0;
  nodegraph::NodeRef thread;
  std::unordered_set<const nodegraph::Node *> dependencies;
  InspectorChangesData snapshot;
  Phase phase = Phase::Turns;
  std::size_t turnCursor = 0;
  std::size_t turnCount = 0;
  nodegraph::NodeRef currentTurn;
  std::size_t itemCursor = 0;
  std::size_t itemCount = 0;
  std::shared_ptr<const nodegraph::NodeState> changeState;
  std::size_t changeCursor = 0;
};

struct InspectorPane::StateGraphScan final {
  struct DomainEntry final {
    nodegraph::NodeKind kind = nodegraph::NodeKind::Runtime;
    std::string id;
    std::string status;
    std::uint64_t changedRevision = 0;
    std::shared_ptr<const nodegraph::NodeState> state;
  };
  struct PendingEntry final {
    std::string id;
    std::string method;
    std::string category;
    std::string threadId;
    std::string status;
    std::uint64_t connectionGeneration = 0;
    std::uint64_t providerGeneration = 0;
  };

  std::uint64_t revision = 0;
  std::uint64_t sampledRevision = 0;
  std::uint64_t maximumInsertionOrder = 0;
  std::uint64_t nextInsertionOrder = 1;
  std::size_t nodeCount = 0;
  std::map<std::string, std::size_t, std::less<>> kindCounts;
  std::size_t threadCount = 0;
  std::size_t turnCount = 0;
  std::size_t itemCount = 0;
  std::size_t modelCount = 0;
  std::size_t pendingInteractions = 0;
  std::size_t omittedDomainEntries = 0;
  std::size_t omittedPendingEntries = 0;
  std::vector<DomainEntry> domains;
  std::vector<PendingEntry> pending;
  std::unordered_set<const nodegraph::Node *> dependencies;
  nodegraph::NodeRef selectedThread;
  std::string selectedId;
  std::string parentId;
  std::uint64_t selectedRevision = 0;
  std::size_t childCount = 0;
  std::size_t selectedItemCount = 0;
  bool selectedItemCountKnown = false;
  bool dirty = false;
  std::shared_ptr<const nodegraph::NodeState> selectedState;
};

struct InspectorPane::ProtocolGraphScan final {
  std::uint64_t revision = 0;
  std::uint64_t sampledRevision = 0;
  std::uint64_t maximumInsertionOrder = 0;
  std::uint64_t nextInsertionOrder = 1;
  std::size_t threadCount = 0;
  std::size_t selectedTurnCount = 0;
  std::size_t selectedItemCount = 0;
  std::size_t modelCount = 0;
  std::size_t unknownCount = 0;
  std::size_t pendingInteractionCount = 0;
  nodegraph::NodeRef selectedThread;
  bool dirty = false;
  std::unordered_set<const nodegraph::Node *> dependencies;
};

QFrame *InspectorPane::agentFrame(const InspectorAgentRender &agent,
                                  std::string_view threadId) {
  auto *frame = new QFrame;
  frame->setObjectName(QStringLiteral("inspectorAgentFrame"));
  frame->setProperty("nodeCanonicalId", text(agent.id));
  frame->setProperty("kind", "raised");
  frame->setMinimumWidth(0);
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
  heading->setSpacing(4);
  auto *titleLabel = makeLabel(QStringLiteral("Agent"), "title");
  titleLabel->setObjectName(QStringLiteral("agentTitle"));
  titleLabel->setWordWrap(false);
  titleLabel->setContentsMargins(0, 0, 0, 0);
  titleLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  heading->addWidget(titleLabel, 0, Qt::AlignBaseline);
  if (!agentName.isEmpty()) {
    auto *nameLabel = makeLabel(agentName, "code");
    nameLabel->setObjectName(QStringLiteral("agentName"));
    nameLabel->setWordWrap(false);
    nameLabel->setContentsMargins(0, 0, 0, 0);
    nameLabel->setMinimumWidth(0);
    nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (!agentPath.isEmpty())
      nameLabel->setToolTip(agentPath);
    heading->addWidget(nameLabel, 1, Qt::AlignBaseline);
  } else {
    heading->addStretch(1);
  }
  auto *status = statusLabel(agent.status);
  status->setContentsMargins(0, 0, 0, 0);
  status->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  heading->addWidget(status, 0, Qt::AlignBaseline);
  auto *copy = new AgentCopyButton(frame);
  auto *disclosure = new AgentDisclosureButton(frame);
  heading->addWidget(copy, 0, Qt::AlignRight | Qt::AlignVCenter);
  heading->addWidget(disclosure, 0, Qt::AlignRight | Qt::AlignVCenter);
  layout->addLayout(heading);
  auto *content = new QWidget(frame);
  content->setObjectName(QStringLiteral("agentCardContent"));
  auto *contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(6);
  QStringList metadata;
  if (!agent.tool.empty() && !agentPath.isEmpty())
    metadata << text(agent.tool);
  for (const std::string *value : {&agent.model, &agent.reasoningEffort}) {
    if (!value->empty())
      metadata << text(*value);
  }
  if (!metadata.isEmpty())
    contentLayout->addWidget(
        makeLabel(metadata.join(QStringLiteral("  ·  ")), "meta"));
  if (!agent.prompt.empty())
    contentLayout->addWidget(makeLabel(text(agent.prompt)));
  if (!agent.resultText.empty()) {
    auto *result = makeMarkdownLabel(text(agent.resultText));
    result->setObjectName(QStringLiteral("agentResult"));
    result->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    contentLayout->addWidget(result);
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
    contentLayout->addWidget(
        makeLabel(identities.join(QStringLiteral("  |  ")), "meta"));
  layout->addWidget(content);

  const std::string expansionKey =
      std::string(threadId) + '\n' +
      (agent.logicalKey.empty() ? agent.id : agent.logicalKey);
  const bool expanded = expandedAgents.contains(expansionKey);
  disclosure->setExpanded(expanded);
  content->setVisible(expanded);
  QObject::connect(disclosure, &QToolButton::clicked, frame,
                   [this, expansionKey, content, disclosure] {
                     const bool expanded = !disclosure->isExpanded();
                     disclosure->setExpanded(expanded);
                     content->setVisible(expanded);
                     if (expanded)
                       expandedAgents.insert(expansionKey);
                     else
                       expandedAgents.erase(expansionKey);
                   });
  QObject::connect(copy, &QToolButton::clicked, frame, [copy, agent] {
    const QString value = agentCopyText(agent);
    auto *mime = new QMimeData;
    mime->setText(value);
    mime->setData("text/markdown", value.toUtf8());
    QApplication::clipboard()->setMimeData(mime);
    copy->showCopiedFeedback();
  });
  return frame;
}

QFrame *InspectorPane::requestFrame(const InspectorRequestRender &request) {
  auto *frame = new QFrame;
  frame->setObjectName(QStringLiteral("inspectorRequestFrame"));
  frame->setProperty("nodeCanonicalId", text(request.id));
  frame->setProperty("kind", "raised");
  frame->setProperty("tone", "warning");
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(6);
  const std::string &displayId =
      request.displayId.empty() ? request.id : request.displayId;
  layout->addWidget(
      makeLabel(UiStyle::humanizeLabel(text(request.kind)), "title"));
  layout->addWidget(
      makeLabel(QStringLiteral("thread %1  |  generation %2  |  request %3")
                    .arg(text(request.threadContext))
                    .arg(static_cast<qulonglong>(request.generation))
                    .arg(text(displayId)),
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
    layout->addWidget(makeLabel(
        QStringLiteral("Request %1 needs a decision.").arg(text(displayId)),
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
    review->setEnabled(request.actionable || request.recoverable);
    connect(review, &QPushButton::clicked, this, [this, id = request.id] {
      if (reviewRequest)
        reviewRequest(id);
    });
    actions->addWidget(review);
  }
  layout->addLayout(actions);
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
            ++protocolScrollRevision;
            QScrollBar *scrollBar = protocolLog->verticalScrollBar();
            protocolFollowsTail = value >= scrollBar->maximum() - 1;
            if (!protocolFollowsTail)
              protocolPausedScrollValue = value;
          });
  connect(protocolLog->verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int maximum) {
            if (protocolFollowsTail) {
              const bool wasMutating = mutatingProtocolLog;
              mutatingProtocolLog = true;
              protocolLog->verticalScrollBar()->setValue(maximum);
              mutatingProtocolLog = wasMutating;
            }
          });
  protocolStats = makeLabel({}, "meta");
  protocolStats->setObjectName(QStringLiteral("protocolInfoStats"));
  auto *protocolAuthority = makeLabel(
      QStringLiteral("Non-authoritative, metadata-only diagnostic history."),
      "meta");
  protocolAuthority->setObjectName(QStringLiteral("protocolInfoAuthority"));
  protocolLayout->addWidget(protocolAuthority);
  protocolLayout->addWidget(protocolLog, 1);
  protocolLayout->addWidget(protocolStats);

  infoStack = new QStackedWidget;
  infoStack->setObjectName(QStringLiteral("infoStack"));
  auto *choices = new QWidget;
  auto *choicesLayout = new QVBoxLayout(choices);
  choicesLayout->setContentsMargins(8, 8, 8, 8);
  choicesLayout->setSpacing(8);
  auto *stateChoice = infoChoice(QStringLiteral("State"),
                                 QStringLiteral("Current application state"));
  stateChoice->setObjectName(QStringLiteral("stateInfoChoice"));
  auto *protocolChoice =
      infoChoice(QStringLiteral("Protocol"),
                 QStringLiteral("App-server protocol messages"));
  protocolChoice->setObjectName(QStringLiteral("protocolInfoChoice"));
  choicesLayout->addWidget(stateChoice);
  choicesLayout->addWidget(protocolChoice);
  choicesLayout->addStretch();

  QPushButton *stateBack = nullptr;
  QPushButton *protocolBack = nullptr;
  infoStack->addWidget(choices);
  infoStack->addWidget(
      infoDetail(QStringLiteral("State"), stateView, &stateBack));
  infoStack->addWidget(
      infoDetail(QStringLiteral("Protocol"), protocolContent, &protocolBack));
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
    refreshCurrentTab();
  };
  connect(stateBack, &QPushButton::clicked, this, showInfoChoices);
  connect(protocolBack, &QPushButton::clicked, this, showInfoChoices);

  planScroll = makeScroll(planContent);
  agentsScroll = makeScroll(agentsContent);
  requestsScroll = makeScroll(requestsContent);
  connect(planScroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            QScrollBar *bar = planScroll->verticalScrollBar();
            if (!pendingPlanSnapshot)
              planScrollFollowsTail =
                  bar->maximum() > 0 && value == bar->maximum();
            refreshGraphPlanViewport();
          });
  connect(planScroll->verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int maximum) {
            if (planScrollFollowsTail)
              planScroll->verticalScrollBar()->setValue(maximum);
            QTimer::singleShot(0, this, [this] { refreshGraphPlanViewport(); });
          });
  connect(agentsScroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            QScrollBar *bar = agentsScroll->verticalScrollBar();
            if (!pendingAgentsSnapshot)
              agentsScrollFollowsTail =
                  bar->maximum() > 0 && value == bar->maximum();
            refreshGraphAgentsViewport();
          });
  connect(agentsScroll->verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int maximum) {
            if (agentsScrollFollowsTail)
              agentsScroll->verticalScrollBar()->setValue(maximum);
            QTimer::singleShot(0, this,
                               [this] { refreshGraphAgentsViewport(); });
          });
  connect(requestsScroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            QScrollBar *bar = requestsScroll->verticalScrollBar();
            if (!pendingRequestsSnapshot)
              requestsScrollFollowsTail =
                  bar->maximum() > 0 && value == bar->maximum();
            refreshGraphRequestsViewport();
          });
  connect(requestsScroll->verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int maximum) {
            if (requestsScrollFollowsTail)
              requestsScroll->verticalScrollBar()->setValue(maximum);
            QTimer::singleShot(0, this,
                               [this] { refreshGraphRequestsViewport(); });
          });
  inspectorTabs->addTab(planScroll, QStringLiteral("Plan"));
  inspectorTabs->addTab(agentsScroll, QStringLiteral("Agents"));
  inspectorTabs->addTab(diffViewer, QStringLiteral("Changes"));
  inspectorTabs->addTab(requestsScroll, QStringLiteral("Requests"));
  inspectorTabs->addTab(infoStack, QStringLiteral("Info"));
  outer->addWidget(inspectorTabs, 1);

  connect(inspectorTabs, &QTabWidget::currentChanged, this,
          [this](int) { refreshCurrentTab(); });
}

InspectorPane::~InspectorPane() = default;

void InspectorPane::showEvent(QShowEvent *event) {
  graphRefreshSuspended = false;
  QFrame::showEvent(event);
  if (inspectorTabs->currentIndex() == 4 &&
      infoStack->currentIndex() == ProtocolPage)
    showProtocolTail();
  if (graphRefreshDirty)
    scheduleGraphRefresh();
}

void InspectorPane::hideEvent(QHideEvent *event) {
  graphRefreshSuspended = true;
  graphRefreshDirty = graph != nullptr;
  cancelGraphScans();
  cancelGraphRowRenders();
  QFrame::hideEvent(event);
}

void InspectorPane::setHideAction(std::function<void()> hide) {
  hideAction = std::move(hide);
}

void InspectorPane::setRequestActions(RequestAction review,
                                      RequestAction accept,
                                      RequestAction reject) {
  reviewRequest = std::move(review);
  acceptRequest = std::move(accept);
  rejectRequest = std::move(reject);
}

void InspectorPane::refresh(const nodegraph::NodeGraph &nextGraph,
                            nodegraph::NodeRef selectedThread) {
  if (graph != &nextGraph) {
    cancelGraphRowRenders();
    cancelGraphScans();
    graph = &nextGraph;
    changesSnapshot.reset();
    planKnownRows = 1;
    agentsKnownRows = 0;
    requestsKnownRows = 0;
    stateSnapshot.clear();
    protocolStatsSnapshot.clear();
    stateInsertionFrontier = 0;
  }
  if (selectedGraphThread != selectedThread) {
    cancelGraphRowRenders();
    cancelGraphScans();
    activeGraphDependencies.clear();
    graphDependenciesTab = -1;
    graphDependenciesInfoPage = -1;
  }
  selectedGraphThread = std::move(selectedThread);
  scheduleGraphRefresh();
}

void InspectorPane::graphChanged(const nodegraph::GraphChanged &change) {
  if (!graph)
    return;
  if (graphRefreshSuspended) {
    graphRefreshDirty = true;
    cancelGraphScans();
    cancelGraphRowRenders();
    return;
  }
  if (graphChangeAffectsCurrentTab(change)) {
    bool continuedBoundedInfoScan = false;
    if (inspectorTabs->currentIndex() == 4 &&
        infoStack->currentIndex() == StatePage && stateGraphScan) {
      // The immutable insertion frontier lets this scan finish even when
      // unrelated insertions/removals continuously shift orderedNodes(). A
      // delayed follow-up samples the newer frontier afterwards.
      if (protocolSelectedStructureAffected(change)) {
        stateGraphScan.reset();
      } else {
        stateGraphScan->dirty = true;
        continuedBoundedInfoScan = true;
      }
    } else if (inspectorTabs->currentIndex() == 4 &&
               infoStack->currentIndex() == ProtocolPage && protocolGraphScan) {
      if (protocolSelectedStructureAffected(change)) {
        protocolGraphScan.reset();
      } else {
        protocolGraphScan->dirty = true;
        continuedBoundedInfoScan = true;
      }
    }
    // Field streaming cannot invalidate the stable node order. Finish the
    // bounded sample, then take at most one delayed follow-up sample instead
    // of restarting or sustaining a zero-delay scan loop.
    if (!continuedBoundedInfoScan && !stateGraphScan && !protocolGraphScan)
      cancelGraphScans();
    scheduleGraphRefresh();
  }
}

void InspectorPane::appendProtocolDiagnostic(
    const nodegraph::UiEffect &effect) {
  if (effect.kind != nodegraph::UiEffectKind::ProtocolDiagnostic)
    return;

  const QString timestamp =
      QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
  std::vector<QString> recorded;
  const auto record = [this, &recorded](QString line) {
    while (protocolLines.size() >= MaximumProtocolLines)
      protocolLines.pop_front();
    protocolLines.emplace_back(line);
    recorded.emplace_back(std::move(line));
  };
  const std::optional<std::uint64_t> sequence =
      graphUnsigned(graphField(effect.details, "sequence"));
  if (sequence && *sequence != 0) {
    if (observedProtocolSequence != 0 &&
        *sequence != observedProtocolSequence + 1) {
      record(QStringLiteral("[%1]  %2  expected=%3  received=%4")
                 .arg(timestamp, *sequence <= observedProtocolSequence
                                     ? QStringLiteral("NON-MONOTONIC")
                                     : QStringLiteral("SEQUENCE GAP"))
                 .arg(observedProtocolSequence + 1)
                 .arg(*sequence));
    }
    observedProtocolSequence = std::max(observedProtocolSequence, *sequence);
  }
  if (const std::optional<std::uint64_t> dropped =
          graphUnsigned(graphField(effect.details, "droppedBefore"));
      dropped && *dropped != 0) {
    record(QStringLiteral("[%1]  DROPPED %2 DIAGNOSTIC%3 before #%4")
               .arg(timestamp)
               .arg(*dropped)
               .arg(*dropped == 1 ? QString{} : QStringLiteral("S"))
               .arg(sequence.value_or(0)));
  }

  QStringList parts{QStringLiteral("[%1]").arg(timestamp)};
  if (sequence && *sequence != 0)
    parts << QStringLiteral("#%1").arg(*sequence);
  if (const auto connection =
          graphUnsigned(graphField(effect.details, "connectionGeneration")))
    parts << QStringLiteral("g%1").arg(*connection);
  if (const auto provider =
          graphUnsigned(graphField(effect.details, "providerGeneration")))
    parts << QStringLiteral("p%1").arg(*provider);
  for (std::string_view key : {"direction", "subject", "source"}) {
    const QString value = protocolMetadata(graphField(effect.details, key));
    if (!value.isEmpty())
      parts << value;
  }
  if (const QString authority =
          protocolMetadata(graphField(effect.details, "authority"));
      !authority.isEmpty())
    parts << QStringLiteral("authority=%1").arg(authority);
  if (const QString outcome =
          protocolMetadata(graphField(effect.details, "outcome"));
      !outcome.isEmpty())
    parts << outcome;
  for (std::string_view key :
       {"threadId", "turnId", "itemId", "requestId", "processId",
        "connectionId", "targetId", "role", "state", "event"}) {
    const QString value = protocolMetadata(graphField(effect.details, key));
    if (!value.isEmpty())
      parts << QStringLiteral("%1=%2").arg(text(key), value);
  }
  if (const QString correlation =
          protocolMetadata(graphField(effect.details, "correlation"));
      !correlation.isEmpty())
    parts << QStringLiteral("correlation=%1").arg(correlation);
  if (const QString error =
          protocolMetadata(graphField(effect.details, "error"));
      !error.isEmpty())
    parts << QStringLiteral("error=%1").arg(error);
  if (const QString category =
          protocolMetadata(graphField(effect.details, "errorCategory"));
      !category.isEmpty())
    parts << QStringLiteral("error-category=%1").arg(category);
  if (const QString code =
          protocolMetadata(graphField(effect.details, "errorCode"));
      !code.isEmpty())
    parts << QStringLiteral("error-code=%1").arg(code);
  record(parts.join(QStringLiteral("  ")));
  ++receivedProtocolDiagnostics;
  const std::string direction =
      graphString(graphField(effect.details, "direction"));
  const std::string authority =
      graphString(graphField(effect.details, "authority"));
  if (authority == "none" &&
      (direction.find("notification") != std::string::npos ||
       direction.find("event") != std::string::npos ||
       direction.ends_with("frame")))
    protocolTelemetryCount =
        std::min(MaximumRetainedTelemetry, protocolTelemetryCount + 1);

  if (isVisible() && inspectorTabs->currentIndex() == 4 &&
      infoStack->currentIndex() == ProtocolPage) {
    if (!protocolLogSynchronized) {
      showProtocolTail();
    } else {
      QScrollBar *scrollBar = protocolLog->verticalScrollBar();
      const bool atVisibleTail = scrollBar->value() >= scrollBar->maximum() - 1;
      const ScrollPosition position{protocolFollowsTail || atVisibleTail,
                                    protocolPausedScrollValue};
      mutatingProtocolLog = true;
      for (const QString &line : recorded)
        protocolLog->appendPlainText(line);
      restoreProtocolScroll(position.followsTail, position.value);
    }
    refreshProtocolStatistics();
  } else {
    protocolLogSynchronized = false;
  }
}

bool InspectorPane::graphChangeAffectsCurrentTab(
    const nodegraph::GraphChanged &change) {
  if (change.rescanRequired)
    return true;
  if (change.affected.size() > MaximumInspectorChangeNodesToInspect ||
      change.removed.size() >
          MaximumInspectorChangeNodesToInspect - change.affected.size())
    return true;

  const int tab = inspectorTabs->currentIndex();
  const int infoPage = tab == 4 ? infoStack->currentIndex() : InfoChoicePage;
  if (tab == 4) {
    if (infoPage == InfoChoicePage)
      return false;
    if (infoPage == StatePage) {
      // Any retirement can change the aggregate counts. For insertions, the
      // immutable order identifies a genuinely new node without treating
      // ordinary streaming updates to old Items as topology changes.
      if (!change.removed.empty())
        return true;
      const auto stateNode = [this](const nodegraph::NodeRef &node) {
        return node && (node == selectedGraphThread ||
                        node->id().kind == nodegraph::NodeKind::Thread ||
                        node->id().kind == nodegraph::NodeKind::Turn ||
                        node->id().kind == nodegraph::NodeKind::Interaction ||
                        stateDomainKind(node->id().kind));
      };
      if (std::ranges::any_of(change.affected, stateNode))
        return true;
      auto read = graph->tryRead();
      if (!read)
        return true;
      const std::uint64_t frontier = stateGraphScan
                                         ? stateGraphScan->maximumInsertionOrder
                                         : stateInsertionFrontier;
      return std::ranges::any_of(
          change.affected, [&read, frontier](const nodegraph::NodeRef &node) {
            return node && !read->removed(node) &&
                   read->insertionOrder(node) > frontier;
          });
    }
    if (protocolSelectedStructureAffected(change))
      return true;
    const auto protocolNode = [](const nodegraph::NodeRef &node) {
      if (!node)
        return false;
      switch (node->id().kind) {
      case nodegraph::NodeKind::Catalog:
      case nodegraph::NodeKind::Interaction:
      case nodegraph::NodeKind::Thread:
      case nodegraph::NodeKind::UnknownProtocol:
        return true;
      default:
        return false;
      }
    };
    return std::ranges::any_of(change.affected, protocolNode) ||
           std::ranges::any_of(change.removed, protocolNode);
  }

  const bool dependenciesCurrent =
      graphDependenciesTab == tab && graphDependenciesInfoPage == infoPage;
  const auto knownDependency =
      [this, dependenciesCurrent](const nodegraph::NodeRef &node) {
        return dependenciesCurrent && node &&
               activeGraphDependencies.contains(node.get());
      };
  const auto selectedThread = [this](const nodegraph::NodeRef &node) {
    return node && selectedGraphThread && node == selectedGraphThread;
  };
  const auto directMatch = [&](const nodegraph::NodeRef &node) {
    if (!node)
      return false;
    if (knownDependency(node) || selectedThread(node))
      return true;
    if (tab == 3)
      return node->id().kind == nodegraph::NodeKind::Runtime ||
             node->id().kind == nodegraph::NodeKind::Connection ||
             node->id().kind == nodegraph::NodeKind::Interaction;
    return false;
  };
  if (std::ranges::any_of(change.affected, directMatch) ||
      std::ranges::any_of(change.removed, directMatch))
    return true;

  // Parent links are graph-owned, so inspect ancestry only behind a
  // non-blocking read. If the worker is publishing, conservatively refresh on
  // the next Qt pass instead of waiting on it.
  auto read = graph->tryRead();
  if (!read)
    return true;
  nodegraph::NodeRef thread;
  if (selectedGraphThread &&
      read->find(selectedGraphThread->id()) == selectedGraphThread)
    thread = selectedGraphThread;

  for (const nodegraph::NodeRef &node : change.affected) {
    if (!node || read->removed(node))
      continue;
    bool belowSelectedThread = false;
    bool belowActiveDependency = false;
    std::size_t ancestryDepth = 0;
    for (nodegraph::NodeRef ancestor = node; ancestor;
         ancestor = read->parent(ancestor)) {
      if (++ancestryDepth > MaximumInspectorAncestryDepth)
        return true;
      belowSelectedThread = belowSelectedThread || ancestor == thread;
      belowActiveDependency =
          belowActiveDependency || knownDependency(ancestor);
    }
    if (!belowSelectedThread && !belowActiveDependency)
      continue;

    if (node->id().kind == nodegraph::NodeKind::Thread)
      return belowActiveDependency || node == thread;
    if (node->id().kind == nodegraph::NodeKind::Turn)
      return tab == 0 || tab == 2 || (tab == 1 && knownDependency(node));
    if (node->id().kind != nodegraph::NodeKind::Item)
      continue;

    const std::shared_ptr<const nodegraph::NodeState> state = read->state(node);
    const std::string type = graphString(graphField(*state, "type"));
    if (tab == 0 && type == "plan")
      return true;
    if (tab == 1) {
      if (type == "subAgentActivity" || type == "collabAgentToolCall")
        return true;
      if (type == "agentMessage" && knownDependency(node))
        return true;
    }
    if (tab == 2 && (type == "commandExecution" || type == "fileChange"))
      return true;
  }
  return false;
}

bool InspectorPane::protocolSelectedStructureAffected(
    const nodegraph::GraphChanged &change) const {
  if (!graph || !selectedGraphThread)
    return false;
  const auto kindChanged = [&change](nodegraph::NodeKind kind) {
    const auto hasKind = [kind](const nodegraph::NodeRef &node) {
      return node && node->id().kind == kind;
    };
    return std::ranges::any_of(change.affected, hasKind) ||
           std::ranges::any_of(change.removed, hasKind);
  };
  // Hierarchy mutations always affect the child plus its old/new parent.
  // A Turn-only update is ordinary protocol state and must not restart a scan.
  if (!kindChanged(nodegraph::NodeKind::Turn) ||
      (!kindChanged(nodegraph::NodeKind::Thread) &&
       !kindChanged(nodegraph::NodeKind::Item) && change.removed.empty()))
    return false;
  const auto direct = [this](const nodegraph::NodeRef &node) {
    return node && node->id().kind == nodegraph::NodeKind::Turn &&
           (activeGraphDependencies.contains(node.get()) ||
            (protocolGraphScan &&
             protocolGraphScan->dependencies.contains(node.get())));
  };
  if (std::ranges::any_of(change.affected, direct) ||
      std::ranges::any_of(change.removed, direct))
    return true;

  const auto structuralCandidate = [](const nodegraph::NodeRef &node) {
    return node && node->id().kind == nodegraph::NodeKind::Turn;
  };
  if (!std::ranges::any_of(change.affected, structuralCandidate) &&
      !std::ranges::any_of(change.removed, structuralCandidate))
    return false;

  auto read = graph->tryRead();
  if (!read)
    return true;
  const std::string &selectedId = selectedGraphThread->id().canonical;
  const auto belongsToSelected = [&read, &selectedId, &structuralCandidate,
                                  this](const nodegraph::NodeRef &node) {
    if (!structuralCandidate(node))
      return false;
    const std::shared_ptr<const nodegraph::NodeState> state = read->state(node);
    if (state &&
        graphString(graphField(*state, "protocolThreadId")) == selectedId)
      return true;
    if (read->removed(node))
      return false;
    for (nodegraph::NodeRef ancestor = read->parent(node); ancestor;
         ancestor = read->parent(ancestor)) {
      if (ancestor == selectedGraphThread)
        return true;
    }
    return false;
  };
  return std::ranges::any_of(change.affected, belongsToSelected) ||
         std::ranges::any_of(change.removed, belongsToSelected);
}

void InspectorPane::cancelGraphRowRenders() {
  if (pendingPlanSnapshot)
    planRowsMaterialized = false;
  if (pendingAgentsSnapshot)
    agentsRowsMaterialized = false;
  if (pendingRequestsSnapshot)
    requestsRowsMaterialized = false;
  pendingPlanSnapshot.reset();
  pendingAgentsSnapshot.reset();
  pendingRequestsSnapshot.reset();
  planRenderClearing = false;
  agentsRenderClearing = false;
  requestsRenderClearing = false;
  planRenderCursor = 0;
  agentsRenderCursor = 0;
  requestsRenderCursor = 0;
}

void InspectorPane::cancelGraphScans() {
  planGraphScan.reset();
  agentsGraphScan.reset();
  requestsGraphScan.reset();
  changesGraphScan.reset();
  stateGraphScan.reset();
  protocolGraphScan.reset();
}

void InspectorPane::refreshCurrentTab() {
  cancelGraphScans();
  activeGraphDependencies.clear();
  graphDependenciesTab = -1;
  graphDependenciesInfoPage = -1;
  scheduleGraphRefresh();
}

void InspectorPane::scheduleGraphRefresh(bool lockRetry) {
  graphRefreshDirty = graph != nullptr;
  if (!graph || graphRefreshScheduled || graphRefreshSuspended)
    return;
  graphRefreshScheduled = true;
  QTimer::singleShot(lockRetry ? InspectorGraphLockRetryMilliseconds : 0, this,
                     [this] {
                       graphRefreshScheduled = false;
                       if (graphRefreshSuspended)
                         return;
                       graphRefreshDirty = false;
                       runGraphRefresh();
                     });
}

void InspectorPane::runGraphRefresh() {
  if (!graph)
    return;

  const int tab = inspectorTabs->currentIndex();
  const int infoPage = tab == 4 ? infoStack->currentIndex() : InfoChoicePage;
  if (tab == 0) {
    runPlanGraphScan();
  } else if (tab == 1) {
    runAgentsGraphScan();
  } else if (tab == 2) {
    runChangesGraphScan();
  } else if (tab == 3) {
    runRequestsGraphRefresh();
  } else if (tab == 4 && infoPage == StatePage) {
    runStateGraphScan();
  } else if (tab == 4 && infoPage == ProtocolPage) {
    runProtocolGraphScan();
  }
}

void InspectorPane::runPlanGraphScan() {
  auto read = graph ? graph->tryRead() : std::nullopt;
  if (!read) {
    scheduleGraphRefresh(true);
    return;
  }

  const std::uint64_t revision = read->revision();
  if (!planGraphScan || planGraphScan->revision != revision) {
    planGraphScan = std::make_unique<PlanGraphScan>();
    PlanGraphScan &scan = *planGraphScan;
    scan.revision = revision;
    if (selectedGraphThread &&
        selectedGraphThread->id().kind == nodegraph::NodeKind::Thread &&
        read->find(selectedGraphThread->id()) == selectedGraphThread)
      scan.thread = selectedGraphThread;
    if (scan.thread) {
      scan.dependencies.insert(scan.thread.get());
      scan.threadState = read->state(scan.thread);
      scan.turnCursor = read->childCount(scan.thread);
    }
  }

  PlanGraphScan &scan = *planGraphScan;
  bool complete = false;
  std::size_t work = 0;
  while (work < MaximumInspectorGraphReadsPerPass && !complete) {
    if (scan.currentTurn) {
      if (scan.itemCursor == 0) {
        scan.currentTurn.reset();
        scan.currentTurnState.reset();
        continue;
      }
      const nodegraph::NodeRef item =
          read->childAt(scan.currentTurn, --scan.itemCursor);
      ++work;
      if (!item || item->id().kind != nodegraph::NodeKind::Item)
        continue;
      const std::shared_ptr<const nodegraph::NodeState> itemState =
          read->state(item);
      if (graphString(graphField(*itemState, "type")) != "plan")
        continue;
      scan.dependencies.insert(item.get());
      scan.sourceTurn = scan.currentTurn;
      scan.sourceTurnState = scan.currentTurnState;
      scan.sourceItem = item;
      scan.sourceItemState = itemState;
      complete = true;
      continue;
    }

    if (scan.turnCursor == 0) {
      complete = true;
      continue;
    }
    const nodegraph::NodeRef turn =
        read->childAt(scan.thread, --scan.turnCursor);
    ++work;
    if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
      continue;
    scan.dependencies.insert(turn.get());
    const std::shared_ptr<const nodegraph::NodeState> turnState =
        read->state(turn);
    if (hasStructuredPlan(*turnState)) {
      scan.sourceTurn = turn;
      scan.sourceTurnState = turnState;
      complete = true;
      continue;
    }
    scan.currentTurn = turn;
    scan.currentTurnState = turnState;
    scan.itemCursor = read->childCount(turn);
  }
  read.reset();

  if (!complete) {
    scheduleGraphRefresh();
    return;
  }
  std::unique_ptr<PlanGraphScan> finished = std::move(planGraphScan);
  if (graph->publishedRevision() != finished->revision) {
    scheduleGraphRefresh();
    return;
  }

  activeGraphDependencies = std::move(finished->dependencies);
  graphDependenciesTab = 0;
  graphDependenciesInfoPage = InfoChoicePage;

  InspectorPlanData snapshot;
  snapshot.threadId =
      finished->thread ? finished->thread->id().canonical : std::string{};
  snapshot.threadPresent = static_cast<bool>(finished->thread);
  if (finished->threadState && finished->sourceTurnState) {
    const std::string threadStatus = graphStatus(*finished->threadState);
    const nodegraph::Value *planValue =
        graphField(*finished->sourceTurnState, "plan");
    const nodegraph::Value::Array *steps = nullptr;
    const nodegraph::Value *explanationValue =
        graphField(*finished->sourceTurnState, "planExplanation");
    bool structured = false;
    if (const nodegraph::Value::Array *array =
            planValue ? planValue->asArray() : nullptr) {
      structured = true;
      steps = array;
    } else if (const nodegraph::Value::Object *plan =
                   planValue ? planValue->asObject() : nullptr) {
      const nodegraph::Value *nestedSteps = graphField(*plan, "steps");
      structured = nestedSteps != nullptr;
      steps = nestedSteps ? nestedSteps->asArray() : nullptr;
      const std::string *explanation =
          explanationValue ? explanationValue->asString() : nullptr;
      if (!explanation || explanation->empty())
        explanationValue = graphField(*plan, "explanation");
    }
    if (structured) {
      InspectorPlanRender plan;
      const std::string *explanation =
          explanationValue ? explanationValue->asString() : nullptr;
      plan.hasExplanation = explanation && !explanation->empty();
      snapshot.totalRows =
          (steps ? steps->size() : 0) + (plan.hasExplanation ? 1U : 0U);
      const InspectorRowWindow window = visibleRowWindow(
          planScroll, snapshot.totalRows, PlanEstimatedRowHeight,
          planScroll->verticalScrollBar()->value());
      snapshot.firstRow = window.first;
      plan.includesExplanation =
          plan.hasExplanation && window.first == 0 && window.end != 0;
      if (plan.includesExplanation)
        plan.explanation = *explanation;
      if (steps) {
        const std::size_t explanationRows = plan.hasExplanation ? 1U : 0U;
        plan.firstStep =
            window.first > explanationRows ? window.first - explanationRows : 0;
        const std::size_t endStep =
            window.end > explanationRows
                ? std::min(steps->size(), window.end - explanationRows)
                : 0;
        plan.steps.reserve(endStep - plan.firstStep);
        const std::string turnStatus = graphStatus(*finished->sourceTurnState);
        for (std::size_t index = plan.firstStep; index < endStep; ++index) {
          const nodegraph::Value &entry = (*steps)[index];
          const nodegraph::Value::Object *step = entry.asObject();
          if (!step) {
            plan.steps.emplace_back();
            continue;
          }
          const std::string status = graphString(graphField(*step, "status"));
          plan.steps.push_back(
              {graphString(graphField(*step, "step")),
               effectivePlanStepStatus(status, turnStatus, threadStatus)});
        }
      }
      snapshot.plan = std::move(plan);
    } else if (finished->sourceItemState) {
      snapshot.planItem =
          graphString(graphField(*finished->sourceItemState, "text"));
    }
  }
  renderGraphPlan(std::move(snapshot));
}

void InspectorPane::runAgentsGraphScan() {
  const InspectorRowWindow requestedWindow = visibleRowWindow(
      agentsScroll,
      agentsKnownRows == 0 ? MaximumInspectorMaterializedRows : agentsKnownRows,
      AgentEstimatedRowHeight, agentsScroll->verticalScrollBar()->value());
  auto read = graph ? graph->tryRead() : std::nullopt;
  if (!read) {
    scheduleGraphRefresh(true);
    return;
  }

  const std::uint64_t revision = read->revision();
  if (!agentsGraphScan || agentsGraphScan->revision != revision) {
    agentsGraphScan = std::make_unique<AgentsGraphScan>();
    AgentsGraphScan &scan = *agentsGraphScan;
    scan.revision = revision;
    if (selectedGraphThread &&
        selectedGraphThread->id().kind == nodegraph::NodeKind::Thread &&
        read->find(selectedGraphThread->id()) == selectedGraphThread)
      scan.thread = selectedGraphThread;
    scan.snapshot.threadId =
        scan.thread ? scan.thread->id().canonical : std::string{};
    scan.snapshot.threadPresent = static_cast<bool>(scan.thread);
    scan.snapshot.firstRow = requestedWindow.first;
    scan.renderEnd = requestedWindow.end;
    if (scan.thread)
      scan.turnCount = read->childCount(scan.thread);
  }

  AgentsGraphScan &scan = *agentsGraphScan;
  const auto clearSource = [&scan] {
    scan.sourceItem.reset();
    scan.sourceState.reset();
    scan.sourceCanCreate = false;
    scan.sourceChildren.clear();
    scan.sourceChildIndexes.clear();
    scan.sourceReceiverIds.clear();
    scan.sourceFieldPhase = 0;
    scan.receiverCursor = 0;
    scan.agentStateCursorInitialized = false;
    scan.sourceChildCursor = 0;
    scan.childThread.reset();
    scan.childState.reset();
    scan.childResultText.clear();
    scan.relationCursor = 0;
    scan.relationCount = 0;
    scan.childTurnCursor = 0;
    scan.currentChildTurn.reset();
    scan.childItemCursor = 0;
    scan.phase = AgentsGraphScan::Phase::MainItems;
  };
  const auto addSourceChild = [&scan](std::string id,
                                      nodegraph::NodeRef thread = {},
                                      std::string status = {},
                                      std::string resultText = {}) {
    if (id.empty())
      return;
    const std::string key = "child\n" + id;
    const auto [found, inserted] =
        scan.sourceChildIndexes.try_emplace(key, scan.sourceChildren.size());
    if (inserted) {
      scan.sourceChildren.push_back({key, std::move(id), std::move(thread),
                                     std::move(status), std::move(resultText),
                                     true});
      return;
    }
    AgentsGraphScan::SourceChild &child = scan.sourceChildren.at(found->second);
    if (thread)
      child.thread = std::move(thread);
    updateAgentStatus(child.status, std::move(status));
    if (!resultText.empty())
      child.resultText = std::move(resultText);
  };
  const auto mergeSource = [&scan](const AgentsGraphScan::SourceChild &child,
                                   const AgentsGraphScan::ChildFacts &facts) {
    auto found = scan.logicalAgents.find(child.key);
    if (found == scan.logicalAgents.end()) {
      if (!scan.sourceCanCreate)
        return;
      AgentsGraphScan::LogicalAgent logical;
      logical.rowIndex = scan.logicalAgentCount++;
      if (logical.rowIndex >= scan.snapshot.firstRow &&
          logical.rowIndex < scan.renderEnd) {
        logical.renderIndex = scan.snapshot.agents.size();
        scan.snapshot.agents.push_back(InspectorAgentRender{});
        scan.snapshot.agents.back().logicalKey = child.key;
        scan.snapshot.agents.back().id = child.id;
      }
      found = scan.logicalAgents.emplace(child.key, std::move(logical)).first;
    }

    AgentsGraphScan::LogicalAgent &logical = found->second;
    const std::string type = graphString(graphField(*scan.sourceState, "type"));
    if (type == "subAgentActivity" || scan.sourceCanCreate)
      updateAgentStatus(logical.status, agentActivityStatus(*scan.sourceState));
    updateAgentStatus(logical.status, child.status);
    updateAgentStatus(logical.status, facts.status);
    if (!logical.renderIndex)
      return;

    InspectorAgentRender &row = scan.snapshot.agents.at(*logical.renderIndex);
    const auto update = [](std::string &target, std::string value) {
      if (!value.empty())
        target = std::move(value);
    };
    const bool sourceCarriesAgentFields =
        type == "subAgentActivity" || scan.sourceCanCreate;
    if (sourceCarriesAgentFields) {
      update(row.agentPath,
             graphString(graphField(*scan.sourceState, "agentPath")));
      update(row.tool, graphString(graphField(*scan.sourceState, "tool")));
      update(row.model, graphString(graphField(*scan.sourceState, "model")));
      update(row.reasoningEffort,
             graphString(graphField(*scan.sourceState, "reasoningEffort")));
      update(row.prompt, graphString(graphField(*scan.sourceState, "prompt")));
      update(row.resultText,
             graphString(graphField(*scan.sourceState, "resultText")));
      update(row.senderThreadId,
             graphString(graphField(*scan.sourceState, "senderThreadId")));
      if (!scan.sourceReceiverIds.empty())
        row.receiverThreadIds = scan.sourceReceiverIds;
    }
    if (child.hasCanonicalThreadId)
      row.childThreadId = child.id;
    row.status = logical.status;
    update(row.resultText, child.resultText);
    update(row.resultText, facts.resultText);
  };
  const auto finishChild = [&scan, &mergeSource] {
    const AgentsGraphScan::SourceChild &child =
        scan.sourceChildren.at(scan.sourceChildCursor);
    AgentsGraphScan::ChildFacts facts{
        scan.childState ? graphStatus(*scan.childState) : std::string{},
        scan.childResultText};
    if (child.hasCanonicalThreadId)
      scan.visibleChildFacts.insert_or_assign(child.key, facts);
    mergeSource(child, facts);
    ++scan.sourceChildCursor;
    scan.childThread.reset();
    scan.childState.reset();
    scan.childResultText.clear();
    scan.childTurnCursor = 0;
    scan.currentChildTurn.reset();
    scan.childItemCursor = 0;
    scan.phase = AgentsGraphScan::Phase::PrepareChild;
  };

  bool complete = false;
  std::size_t work = 0;
  while (work < MaximumInspectorGraphReadsPerPass && !complete) {
    switch (scan.phase) {
    case AgentsGraphScan::Phase::MainTurns: {
      if (scan.turnCursor == scan.turnCount) {
        complete = true;
        break;
      }
      const nodegraph::NodeRef turn =
          read->childAt(scan.thread, scan.turnCursor++);
      ++work;
      if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
        break;
      scan.currentTurn = turn;
      scan.itemCursor = 0;
      scan.itemCount = read->childCount(turn);
      scan.phase = AgentsGraphScan::Phase::MainItems;
      break;
    }
    case AgentsGraphScan::Phase::MainItems: {
      if (scan.itemCursor == scan.itemCount) {
        scan.currentTurn.reset();
        scan.phase = AgentsGraphScan::Phase::MainTurns;
        break;
      }
      const nodegraph::NodeRef item =
          read->childAt(scan.currentTurn, scan.itemCursor++);
      ++work;
      if (!item || item->id().kind != nodegraph::NodeKind::Item)
        break;
      const std::shared_ptr<const nodegraph::NodeState> state =
          read->state(item);
      const std::string type = graphString(graphField(*state, "type"));
      if (type != "subAgentActivity" && type != "collabAgentToolCall")
        break;
      scan.sourceItem = item;
      scan.sourceState = state;
      scan.dependencies.insert(item.get());
      scan.sourceCanCreate =
          type == "subAgentActivity"
              ? (graphString(graphField(*state, "kind")).empty() ||
                 graphString(graphField(*state, "kind")) == "started")
              : spawnAgentTool(graphString(graphField(*state, "tool")));
      scan.relationCursor = 0;
      scan.relationCount =
          read->relatedCount(item, nodegraph::RelationKind::AgentChildThread);
      scan.phase = AgentsGraphScan::Phase::ChildRelations;
      break;
    }
    case AgentsGraphScan::Phase::ChildRelations: {
      if (scan.relationCursor < scan.relationCount) {
        const nodegraph::NodeRef candidate = read->relatedAt(
            scan.sourceItem, nodegraph::RelationKind::AgentChildThread,
            scan.relationCursor++);
        ++work;
        if (candidate && candidate->id().kind == nodegraph::NodeKind::Thread) {
          addSourceChild(candidate->id().canonical, candidate);
          scan.dependencies.insert(candidate.get());
        }
        break;
      }
      scan.phase = AgentsGraphScan::Phase::SourceChildren;
      break;
    }
    case AgentsGraphScan::Phase::SourceChildren: {
      if (scan.sourceFieldPhase == 0) {
        addSourceChild(
            graphString(graphField(*scan.sourceState, "agentThreadId")));
        scan.sourceFieldPhase = 1;
        break;
      }
      if (scan.sourceFieldPhase == 1) {
        const nodegraph::Value *value =
            graphField(*scan.sourceState, "receiverThreadIds");
        const nodegraph::Value::Array *receivers =
            value ? value->asArray() : nullptr;
        if (receivers && scan.receiverCursor < receivers->size()) {
          std::string id = graphString(&receivers->at(scan.receiverCursor++));
          if (!id.empty() && std::ranges::find(scan.sourceReceiverIds, id) ==
                                 scan.sourceReceiverIds.end())
            scan.sourceReceiverIds.push_back(id);
          addSourceChild(std::move(id));
          ++work;
          break;
        }
        scan.sourceFieldPhase = 2;
      }
      if (scan.sourceFieldPhase == 2) {
        const nodegraph::Value *value =
            graphField(*scan.sourceState, "agentsStates");
        const nodegraph::Value::Object *states =
            value ? value->asObject() : nullptr;
        if (states && !scan.agentStateCursorInitialized) {
          scan.agentStateCursor = states->begin();
          scan.agentStateCursorInitialized = true;
        }
        if (states && scan.agentStateCursor != states->end()) {
          const auto &[id, value] = *scan.agentStateCursor++;
          const nodegraph::Value::Object *state = value.asObject();
          addSourceChild(id, {},
                         state ? graphString(graphField(*state, "status"))
                               : std::string{},
                         state ? graphString(graphField(*state, "message"))
                               : std::string{});
          ++work;
          break;
        }
        scan.sourceFieldPhase = 3;
      }
      if (scan.sourceChildren.empty() && scan.sourceCanCreate) {
        std::string displayId =
            nodegraph::protocolCanonicalId(*scan.sourceState, scan.sourceItem);
        if (!displayId.empty()) {
          std::string key = "source\n" + scan.sourceItem->id().canonical;
          scan.sourceChildIndexes.emplace(key, 0);
          scan.sourceChildren.push_back(
              {std::move(key), std::move(displayId), {}, {}, {}, false});
        }
      }
      if (scan.sourceChildren.empty()) {
        clearSource();
        break;
      }
      scan.sourceChildCursor = 0;
      scan.phase = AgentsGraphScan::Phase::PrepareChild;
      break;
    }
    case AgentsGraphScan::Phase::PrepareChild: {
      if (scan.sourceChildCursor == scan.sourceChildren.size()) {
        clearSource();
        break;
      }
      const AgentsGraphScan::SourceChild &child =
          scan.sourceChildren.at(scan.sourceChildCursor);
      mergeSource(child, {});
      const auto logical = scan.logicalAgents.find(child.key);
      if (logical == scan.logicalAgents.end() || !logical->second.renderIndex) {
        ++scan.sourceChildCursor;
        if (child.hasCanonicalThreadId)
          ++work;
        break;
      }
      if (child.hasCanonicalThreadId) {
        if (const auto cached = scan.visibleChildFacts.find(child.key);
            cached != scan.visibleChildFacts.end()) {
          mergeSource(child, cached->second);
          ++scan.sourceChildCursor;
          ++work;
          break;
        }
      }
      scan.childThread = child.thread;
      if (!scan.childThread && child.hasCanonicalThreadId) {
        scan.childThread = read->find({nodegraph::NodeKind::Thread, child.id});
        ++work;
      }
      if (!scan.childThread || read->removed(scan.childThread)) {
        finishChild();
        break;
      }
      scan.dependencies.insert(scan.childThread.get());
      scan.childState = read->state(scan.childThread);
      scan.childTurnCursor = read->childCount(scan.childThread);
      ++work;
      scan.phase = AgentsGraphScan::Phase::ChildTurns;
      break;
    }
    case AgentsGraphScan::Phase::ChildTurns: {
      if (scan.childTurnCursor == 0) {
        finishChild();
        break;
      }
      const nodegraph::NodeRef childTurn =
          read->childAt(scan.childThread, --scan.childTurnCursor);
      ++work;
      if (!childTurn || childTurn->id().kind != nodegraph::NodeKind::Turn)
        break;
      scan.dependencies.insert(childTurn.get());
      scan.currentChildTurn = childTurn;
      scan.childItemCursor = read->childCount(childTurn);
      scan.phase = AgentsGraphScan::Phase::ChildItems;
      break;
    }
    case AgentsGraphScan::Phase::ChildItems: {
      if (scan.childItemCursor == 0) {
        scan.currentChildTurn.reset();
        scan.phase = AgentsGraphScan::Phase::ChildTurns;
        break;
      }
      const nodegraph::NodeRef childItem =
          read->childAt(scan.currentChildTurn, --scan.childItemCursor);
      ++work;
      if (!childItem || childItem->id().kind != nodegraph::NodeKind::Item)
        break;
      const std::shared_ptr<const nodegraph::NodeState> childItemState =
          read->state(childItem);
      if (graphString(graphField(*childItemState, "type")) != "agentMessage")
        break;
      if (graphString(graphField(*childItemState, "text")).empty())
        break;
      scan.dependencies.insert(childItem.get());
      scan.childResultText = graphString(graphField(*childItemState, "text"));
      finishChild();
      break;
    }
    }
  }
  read.reset();

  if (!complete) {
    scheduleGraphRefresh();
    return;
  }
  std::unique_ptr<AgentsGraphScan> finished = std::move(agentsGraphScan);
  if (graph->publishedRevision() != finished->revision) {
    scheduleGraphRefresh();
    return;
  }
  activeGraphDependencies = std::move(finished->dependencies);
  graphDependenciesTab = 1;
  graphDependenciesInfoPage = InfoChoicePage;
  finished->matchingCount = finished->logicalAgentCount;
  if (finished->matchingCount == 0) {
    finished->snapshot.firstRow = 0;
    finished->snapshot.totalRows = 1;
  } else if (finished->snapshot.firstRow >= finished->matchingCount) {
    agentsKnownRows = finished->matchingCount;
    scheduleGraphRefresh();
    return;
  } else {
    finished->snapshot.totalRows = finished->matchingCount;
  }
  renderGraphAgents(std::move(finished->snapshot));
}

void InspectorPane::runChangesGraphScan() {
  auto read = graph ? graph->tryRead() : std::nullopt;
  if (!read) {
    scheduleGraphRefresh(true);
    return;
  }

  const std::uint64_t revision = read->revision();
  if (!changesGraphScan || changesGraphScan->revision != revision) {
    changesGraphScan = std::make_unique<ChangesGraphScan>();
    ChangesGraphScan &scan = *changesGraphScan;
    scan.revision = revision;
    if (selectedGraphThread &&
        selectedGraphThread->id().kind == nodegraph::NodeKind::Thread &&
        read->find(selectedGraphThread->id()) == selectedGraphThread)
      scan.thread = selectedGraphThread;
    scan.snapshot.threadId =
        scan.thread ? scan.thread->id().canonical : std::string{};
    if (scan.thread) {
      scan.dependencies.insert(scan.thread.get());
      const std::shared_ptr<const nodegraph::NodeState> threadState =
          read->state(scan.thread);
      scan.snapshot.cwd = graphString(graphField(*threadState, "cwd"));
      scan.turnCount = read->childCount(scan.thread);
    }
  }

  ChangesGraphScan &scan = *changesGraphScan;
  bool complete = false;
  std::size_t work = 0;
  while (work < MaximumInspectorGraphReadsPerPass && !complete) {
    switch (scan.phase) {
    case ChangesGraphScan::Phase::Turns: {
      if (scan.turnCursor == scan.turnCount) {
        complete = true;
        break;
      }
      const nodegraph::NodeRef turn =
          read->childAt(scan.thread, scan.turnCursor++);
      ++work;
      if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
        break;
      scan.dependencies.insert(turn.get());
      scan.currentTurn = turn;
      scan.itemCursor = 0;
      scan.itemCount = read->childCount(turn);
      scan.phase = ChangesGraphScan::Phase::Items;
      break;
    }
    case ChangesGraphScan::Phase::Items: {
      if (scan.itemCursor == scan.itemCount) {
        scan.currentTurn.reset();
        scan.phase = ChangesGraphScan::Phase::Turns;
        break;
      }
      const nodegraph::NodeRef item =
          read->childAt(scan.currentTurn, scan.itemCursor++);
      ++work;
      if (!item || item->id().kind != nodegraph::NodeKind::Item)
        break;
      const std::shared_ptr<const nodegraph::NodeState> state =
          read->state(item);
      const std::string type = graphString(graphField(*state, "type"));
      if (type != "commandExecution" && type != "fileChange")
        break;
      scan.dependencies.insert(item.get());
      if (type == "commandExecution") {
        appendUniqueBounded(scan.snapshot.commandCwds,
                            graphString(graphField(*state, "cwd")), 64);
        break;
      }
      scan.changeState = state;
      scan.changeCursor = 0;
      scan.phase = ChangesGraphScan::Phase::ChangeEntries;
      break;
    }
    case ChangesGraphScan::Phase::ChangeEntries: {
      const nodegraph::Value *changesValue =
          graphField(*scan.changeState, "changes");
      const nodegraph::Value::Array *changes =
          changesValue ? changesValue->asArray() : nullptr;
      if (!changes || scan.changeCursor == changes->size()) {
        scan.changeState.reset();
        scan.changeCursor = 0;
        scan.phase = ChangesGraphScan::Phase::Items;
        break;
      }
      const nodegraph::Value &change = (*changes)[scan.changeCursor++];
      ++work;
      const nodegraph::Value::Object *object = change.asObject();
      if (object)
        appendUniqueBounded(scan.snapshot.changedPaths,
                            graphString(graphField(*object, "path")), 512);
      break;
    }
    }
  }
  read.reset();

  if (!complete) {
    scheduleGraphRefresh();
    return;
  }
  std::unique_ptr<ChangesGraphScan> finished = std::move(changesGraphScan);
  if (graph->publishedRevision() != finished->revision) {
    scheduleGraphRefresh();
    return;
  }
  activeGraphDependencies = std::move(finished->dependencies);
  graphDependenciesTab = 2;
  graphDependenciesInfoPage = InfoChoicePage;
  if (!changesSnapshot || *changesSnapshot != finished->snapshot) {
    changesSnapshot = std::move(finished->snapshot);
    renderChanges(*changesSnapshot);
  }
}

void InspectorPane::runRequestsGraphRefresh() {
  const InspectorRowWindow requestedWindow = visibleRowWindow(
      requestsScroll,
      requestsKnownRows == 0 ? MaximumInspectorMaterializedRows
                             : requestsKnownRows,
      RequestEstimatedRowHeight, requestsScroll->verticalScrollBar()->value());
  auto read = graph ? graph->tryRead() : std::nullopt;
  if (!read) {
    scheduleGraphRefresh(true);
    return;
  }

  const std::uint64_t revision = read->revision();
  if (!requestsGraphScan || requestsGraphScan->revision != revision) {
    requestsGraphScan = std::make_unique<RequestsGraphScan>();
    RequestsGraphScan &scan = *requestsGraphScan;
    scan.revision = revision;
    if (selectedGraphThread &&
        selectedGraphThread->id().kind == nodegraph::NodeKind::Thread &&
        read->find(selectedGraphThread->id()) == selectedGraphThread)
      scan.dependencies.insert(selectedGraphThread.get());

    scan.connection =
        read->find({nodegraph::NodeKind::Connection, "connection"});
    if (scan.connection) {
      scan.dependencies.insert(scan.connection.get());
      const std::shared_ptr<const nodegraph::NodeState> connectionState =
          read->state(scan.connection);
      const std::string transport =
          graphString(graphField(*connectionState, "transportState"));
      const std::string provider =
          graphString(graphField(*connectionState, "providerState"));
      const std::string role =
          graphString(graphField(*connectionState, "role"));
      scan.canControl =
          (transport == "connected" ||
           connectionState->status == nodegraph::NodeStatus::Connected) &&
          provider == "ready" && role == "controller";
      scan.generation =
          graphUnsigned(graphField(*connectionState, "connectionGeneration"))
              .value_or(graphUnsigned(
                            graphField(*connectionState, "providerGeneration"))
                            .value_or(0));
    }

    scan.runtime = read->find({nodegraph::NodeKind::Runtime, "runtime"});
    if (scan.runtime) {
      scan.dependencies.insert(scan.runtime.get());
      scan.pendingCount = read->relatedCount(
          scan.runtime, nodegraph::RelationKind::PendingInteraction);
    }
    scan.snapshot.firstRow = requestedWindow.first;
    scan.renderEnd = requestedWindow.end;
    scan.snapshot.requests.reserve(requestedWindow.end - requestedWindow.first);
  }

  RequestsGraphScan &scan = *requestsGraphScan;
  const auto clearCurrent = [&scan] {
    scan.currentInteraction.reset();
    scan.currentRow.reset();
    scan.targetCount = 0;
    scan.targetCursor = 0;
    scan.targetAncestor.reset();
    scan.targetAncestryDepth = 0;
  };
  const auto appendCurrent = [&scan, &clearCurrent] {
    scan.snapshot.requests.emplace_back(std::move(*scan.currentRow));
    clearCurrent();
  };

  bool complete = false;
  std::size_t work = 0;
  while (work < MaximumInspectorGraphReadsPerPass && !complete) {
    if (scan.currentRow) {
      if (scan.targetAncestor) {
        if (scan.targetAncestor->id().kind == nodegraph::NodeKind::Thread) {
          scan.dependencies.insert(scan.targetAncestor.get());
          const std::shared_ptr<const nodegraph::NodeState> targetState =
              read->state(scan.targetAncestor);
          scan.currentRow->threadContext = scan.targetAncestor->id().canonical;
          std::string title = graphString(graphField(*targetState, "name"));
          if (title.empty())
            title = graphString(graphField(*targetState, "title"));
          if (!title.empty())
            scan.currentRow->threadContext = std::move(title);
          appendCurrent();
          continue;
        }
        if (++scan.targetAncestryDepth > MaximumInspectorAncestryDepth) {
          scan.targetAncestor.reset();
          continue;
        }
        scan.targetAncestor = read->parent(scan.targetAncestor);
        ++work;
        continue;
      }
      if (scan.targetCursor < scan.targetCount) {
        scan.targetAncestor = read->relatedAt(
            scan.currentInteraction, nodegraph::RelationKind::InteractionTarget,
            scan.targetCursor++);
        scan.targetAncestryDepth = 0;
        ++work;
        continue;
      }
      appendCurrent();
      continue;
    }

    if (scan.pendingCursor == scan.pendingCount) {
      complete = true;
      continue;
    }
    const nodegraph::NodeRef candidate = read->relatedAt(
        scan.runtime, nodegraph::RelationKind::PendingInteraction,
        scan.pendingCursor++);
    ++work;
    if (!candidate || candidate->id().kind != nodegraph::NodeKind::Interaction)
      continue;
    const std::shared_ptr<const nodegraph::NodeState> state =
        read->state(candidate);
    if (state->status != nodegraph::NodeStatus::Pending &&
        state->status != nodegraph::NodeStatus::Failed)
      continue;
    const std::size_t rowIndex = scan.matchingCount++;
    if (rowIndex < scan.snapshot.firstRow || rowIndex >= scan.renderEnd)
      continue;

    scan.dependencies.insert(candidate.get());
    scan.currentInteraction = candidate;
    scan.currentRow.emplace();
    InspectorRequestRender &row = *scan.currentRow;
    row.id = candidate->id().canonical;
    row.displayId = graphString(graphField(*state, "requestId"));
    row.kind = requestKind(graphString(graphField(*state, "method")));
    row.generation = scan.generation;
    const bool recoveryOnly = graphBool(graphField(*state, "recoveryOnly"));
    row.actionable = scan.canControl && !recoveryOnly;
    const nodegraph::Value *retained =
        graphField(*state, "retainedResponsePayload");
    row.recoverable = recoveryOnly && retained && retained->asObject();
    const nodegraph::Value *payloadValue = graphField(*state, "payload");
    const nodegraph::Value::Object *payload =
        payloadValue ? payloadValue->asObject() : nullptr;
    if (payload) {
      row.command = graphString(graphField(*payload, "command"));
      row.reason = graphString(graphField(*payload, "reason"));
      row.message = graphString(graphField(*payload, "message"));
      row.threadContext = graphString(graphField(*payload, "threadId"));
      const nodegraph::Value *questionsValue =
          graphField(*payload, "questions");
      if (const nodegraph::Value::Array *questions =
              questionsValue ? questionsValue->asArray() : nullptr)
        row.questionCount = questions->size();
    }
    if (row.message.empty())
      row.message = graphString(graphField(*state, "error"));
    scan.targetCount = read->relatedCount(
        candidate, nodegraph::RelationKind::InteractionTarget);
  }
  read.reset();

  if (!complete) {
    scheduleGraphRefresh();
    return;
  }
  std::unique_ptr<RequestsGraphScan> finished = std::move(requestsGraphScan);
  if (graph->publishedRevision() != finished->revision) {
    scheduleGraphRefresh();
    return;
  }
  if (finished->matchingCount == 0) {
    finished->snapshot.firstRow = 0;
    finished->snapshot.totalRows = 1;
  } else if (finished->snapshot.firstRow >= finished->matchingCount) {
    requestsKnownRows = finished->matchingCount;
    scheduleGraphRefresh();
    return;
  } else {
    finished->snapshot.totalRows = finished->matchingCount;
  }
  activeGraphDependencies = std::move(finished->dependencies);
  graphDependenciesTab = 3;
  graphDependenciesInfoPage = InfoChoicePage;
  renderGraphRequests(std::move(finished->snapshot));
}

void InspectorPane::runStateGraphScan() {
  auto read = graph ? graph->tryRead() : std::nullopt;
  if (!read) {
    scheduleGraphRefresh(true);
    return;
  }

  const std::uint64_t revision = read->revision();
  if (!stateGraphScan) {
    stateGraphScan = std::make_unique<StateGraphScan>();
    StateGraphScan &scan = *stateGraphScan;
    scan.revision = revision;
    scan.sampledRevision = revision;
    const std::vector<nodegraph::NodeRef> &nodes = read->orderedNodes();
    if (!nodes.empty())
      scan.maximumInsertionOrder = read->insertionOrder(nodes.back());
    nodegraph::NodeRef thread;
    if (selectedGraphThread &&
        selectedGraphThread->id().kind == nodegraph::NodeKind::Thread &&
        read->find(selectedGraphThread->id()) == selectedGraphThread)
      thread = selectedGraphThread;
    if (thread) {
      scan.selectedThread = thread;
      scan.dependencies.insert(thread.get());
      scan.selectedId = thread->id().canonical;
      scan.selectedState = read->state(thread);
      scan.selectedRevision = read->changedRevision(thread);
      scan.childCount = read->childCount(thread);
      if (const std::optional<std::uint64_t> itemCount = graphUnsigned(
              graphField(*scan.selectedState, "historyLoadedItemCount"))) {
        scan.selectedItemCount = *itemCount;
        scan.selectedItemCountKnown = true;
      }
      if (const nodegraph::NodeRef parent = read->parent(thread))
        scan.parentId = parent->id().canonical;
    }
  }

  StateGraphScan &scan = *stateGraphScan;
  scan.sampledRevision = revision;
  const std::vector<nodegraph::NodeRef> &nodes = read->orderedNodes();
  auto cursor = std::lower_bound(
      nodes.begin(), nodes.end(), scan.nextInsertionOrder,
      [&read](const nodegraph::NodeRef &node, std::uint64_t order) {
        return read->insertionOrder(node) < order;
      });
  std::size_t work = 0;
  while (cursor != nodes.end() && work < MaximumInspectorGraphReadsPerPass) {
    const nodegraph::NodeRef &node = *cursor++;
    const std::uint64_t order = read->insertionOrder(node);
    if (order > scan.maximumInsertionOrder)
      break;
    scan.nextInsertionOrder = order + 1;
    ++scan.nodeCount;
    ++work;
    if (!node)
      continue;
    ++scan.kindCounts[std::string(nodeKindName(node->id().kind))];
    switch (node->id().kind) {
    case nodegraph::NodeKind::Thread:
      ++scan.threadCount;
      break;
    case nodegraph::NodeKind::Turn:
      ++scan.turnCount;
      break;
    case nodegraph::NodeKind::Item:
      ++scan.itemCount;
      if (!scan.selectedItemCountKnown && scan.selectedThread) {
        const nodegraph::NodeRef turn = read->parent(node);
        if (turn && read->parent(turn) == scan.selectedThread)
          ++scan.selectedItemCount;
      }
      break;
    default:
      break;
    }

    if (node->id().kind == nodegraph::NodeKind::Interaction) {
      const std::shared_ptr<const nodegraph::NodeState> state =
          read->state(node);
      if (state->status != nodegraph::NodeStatus::Pending)
        continue;
      ++scan.pendingInteractions;
      if (scan.pending.size() == MaximumStatePendingEntries) {
        ++scan.omittedPendingEntries;
        continue;
      }
      scan.dependencies.insert(node.get());
      const std::string method = graphString(graphField(*state, "method"));
      std::string threadId;
      if (const nodegraph::Value *payload = graphField(*state, "payload")) {
        if (const nodegraph::Value::Object *object = payload->asObject())
          threadId = graphString(graphField(*object, "threadId"));
      }
      if (threadId.empty()) {
        nodegraph::NodeRef target = read->relatedAt(
            node, nodegraph::RelationKind::InteractionTarget, 0);
        for (std::size_t depth = 0; target && depth < 3; ++depth) {
          if (target->id().kind == nodegraph::NodeKind::Thread) {
            threadId = target->id().canonical;
            break;
          }
          target = read->parent(target);
        }
      }
      scan.pending.push_back(
          {node->id().canonical, method, requestKind(method),
           std::move(threadId), graphStatus(*state),
           graphUnsigned(graphField(*state, "connectionGeneration"))
               .value_or(0),
           graphUnsigned(graphField(*state, "providerGeneration"))
               .value_or(0)});
      continue;
    }
    if (!stateDomainKind(node->id().kind))
      continue;

    const std::shared_ptr<const nodegraph::NodeState> state = read->state(node);
    if (node->id().kind == nodegraph::NodeKind::Catalog &&
        node->id().canonical == "model") {
      const nodegraph::Value *models = graphField(*state, "data");
      if (const nodegraph::Value::Array *array =
              models ? models->asArray() : nullptr)
        scan.modelCount = array->size();
    }
    if (scan.domains.size() == MaximumStateDomainEntries) {
      ++scan.omittedDomainEntries;
      continue;
    }
    scan.dependencies.insert(node.get());
    scan.domains.push_back({node->id().kind, node->id().canonical,
                            graphStatus(*state), read->changedRevision(node),
                            state});
  }
  const bool complete =
      cursor == nodes.end() ||
      (cursor != nodes.end() &&
       read->insertionOrder(*cursor) > scan.maximumInsertionOrder);
  read.reset();

  if (!complete) {
    scheduleGraphRefresh();
    return;
  }
  std::unique_ptr<StateGraphScan> finished = std::move(stateGraphScan);
  const bool needsFreshSample = finished->dirty;
  stateInsertionFrontier = finished->maximumInsertionOrder;
  setProperty("stateScanCompletions",
              property("stateScanCompletions").toULongLong() + 1);
  activeGraphDependencies = std::move(finished->dependencies);
  graphDependenciesTab = 4;
  graphDependenciesInfoPage = StatePage;

  const std::string selectedStatus = finished->selectedState
                                         ? graphStatus(*finished->selectedState)
                                         : std::string{};
  QString value = QStringLiteral("Shared NodeGraph\nLatest sampled revision: "
                                 "%1\nInsertion-order frontier: %2\nNodes: %3\n"
                                 "Threads: %4\nTurns: %5\nItems: %6\n"
                                 "Models: %7\nPending interactions: %8\n")
                      .arg(finished->sampledRevision)
                      .arg(finished->maximumInsertionOrder)
                      .arg(finished->nodeCount)
                      .arg(finished->threadCount)
                      .arg(finished->turnCount)
                      .arg(finished->itemCount)
                      .arg(finished->modelCount)
                      .arg(finished->pendingInteractions);
  if (finished->revision != finished->sampledRevision) {
    appendDiagnostic(
        value,
        QStringLiteral("Bounded sample crossed field revisions %1-%2; node "
                       "order remained stable\n")
            .arg(finished->revision)
            .arg(finished->sampledRevision));
  }
  if (needsFreshSample)
    appendDiagnostic(value, QStringLiteral("Relevant refresh queued\n"));
  appendDiagnostic(value, QStringLiteral("Node kinds:\n"));
  for (const auto &[kind, count] : finished->kindCounts) {
    appendDiagnostic(value,
                     QStringLiteral("  %1: %2\n").arg(text(kind)).arg(count));
  }
  appendDiagnostic(value, QStringLiteral("\nSelected thread:\n"));
  if (!finished->selectedState) {
    appendDiagnostic(value, QStringLiteral("  <none>\n"));
  } else {
    appendDiagnostic(value,
                     QStringLiteral("  id: %1\n  status: %2\n"
                                    "  changed revision: %3\n"
                                    "  parent: %4\n  turns: %5\n  items: %6\n"
                                    "  fields: ")
                         .arg(diagnosticIdentifier(finished->selectedId),
                              text(selectedStatus))
                         .arg(finished->selectedRevision)
                         .arg(finished->parentId.empty()
                                  ? QStringLiteral("<root>")
                                  : diagnosticIdentifier(finished->parentId))
                         .arg(finished->childCount)
                         .arg(finished->selectedItemCount));
    appendGraphObject(value, finished->selectedState->fields, 2, 0);
    appendDiagnostic(value, QStringLiteral("\n"));
  }
  appendDiagnostic(value, QStringLiteral("\nCurrent domains (redacted):\n"));
  if (finished->domains.empty()) {
    appendDiagnostic(value, QStringLiteral("  <none>\n"));
  } else {
    for (const StateGraphScan::DomainEntry &domain : finished->domains) {
      appendDiagnostic(
          value, QStringLiteral("  %1 %2  status=%3  revision=%4\n    fields: ")
                     .arg(text(nodeKindName(domain.kind)),
                          diagnosticIdentifier(domain.id),
                          domain.status.empty() ? QStringLiteral("unknown")
                                                : text(domain.status))
                     .arg(domain.changedRevision));
      appendGraphObject(value, domain.state->fields, 4, 0);
      appendDiagnostic(value, QStringLiteral("\n"));
    }
  }
  if (finished->omittedDomainEntries != 0)
    appendDiagnostic(value, QStringLiteral("  %1 more domains omitted\n")
                                .arg(finished->omittedDomainEntries));

  appendDiagnostic(value,
                   QStringLiteral("\nPending interactions (metadata only):\n"));
  if (finished->pending.empty()) {
    appendDiagnostic(value, QStringLiteral("  <none>\n"));
  } else {
    for (const StateGraphScan::PendingEntry &pending : finished->pending) {
      appendDiagnostic(value,
                       QStringLiteral("  %1  %2  %3  category=%4  thread=%5  "
                                      "generation=%6/%7\n")
                           .arg(text(pending.status),
                                diagnosticIdentifier(pending.id),
                                diagnosticIdentifier(pending.method),
                                diagnosticIdentifier(pending.category),
                                pending.threadId.empty()
                                    ? QStringLiteral("<global>")
                                    : diagnosticIdentifier(pending.threadId))
                           .arg(pending.connectionGeneration)
                           .arg(pending.providerGeneration));
    }
  }
  if (finished->omittedPendingEntries != 0)
    appendDiagnostic(value, QStringLiteral("  %1 more interactions omitted\n")
                                .arg(finished->omittedPendingEntries));
  if (value.size() >= MaximumGraphDiagnosticCharacters) {
    value.truncate(MaximumGraphDiagnosticCharacters - 34);
    value += QStringLiteral("\n[Graph diagnostic truncated]\n");
  }
  renderGraphState(std::move(value));
  if (needsFreshSample)
    scheduleGraphRefresh(true);
}

void InspectorPane::runProtocolGraphScan() {
  auto read = graph ? graph->tryRead() : std::nullopt;
  if (!read) {
    scheduleGraphRefresh(true);
    return;
  }

  const std::uint64_t revision = read->revision();
  if (!protocolGraphScan) {
    protocolGraphScan = std::make_unique<ProtocolGraphScan>();
    ProtocolGraphScan &scan = *protocolGraphScan;
    scan.revision = revision;
    scan.sampledRevision = revision;
    const std::vector<nodegraph::NodeRef> &nodes = read->orderedNodes();
    if (!nodes.empty())
      scan.maximumInsertionOrder = read->insertionOrder(nodes.back());
    if (selectedGraphThread &&
        selectedGraphThread->id().kind == nodegraph::NodeKind::Thread &&
        read->find(selectedGraphThread->id()) == selectedGraphThread) {
      scan.selectedThread = selectedGraphThread;
      scan.dependencies.insert(selectedGraphThread.get());
    }
  }

  ProtocolGraphScan &scan = *protocolGraphScan;
  scan.sampledRevision = revision;
  const std::vector<nodegraph::NodeRef> &nodes = read->orderedNodes();
  auto cursor = std::lower_bound(
      nodes.begin(), nodes.end(), scan.nextInsertionOrder,
      [&read](const nodegraph::NodeRef &node, std::uint64_t order) {
        return read->insertionOrder(node) < order;
      });
  std::size_t work = 0;
  while (cursor != nodes.end() && work < MaximumInspectorGraphReadsPerPass) {
    const nodegraph::NodeRef &node = *cursor++;
    const std::uint64_t order = read->insertionOrder(node);
    if (order > scan.maximumInsertionOrder)
      break;
    scan.nextInsertionOrder = order + 1;
    ++work;
    if (!node)
      continue;
    switch (node->id().kind) {
    case nodegraph::NodeKind::Thread:
      ++scan.threadCount;
      break;
    case nodegraph::NodeKind::Turn:
      if (scan.selectedThread && read->parent(node) == scan.selectedThread) {
        ++scan.selectedTurnCount;
        scan.dependencies.insert(node.get());
      }
      break;
    case nodegraph::NodeKind::Item: {
      const nodegraph::NodeRef turn = read->parent(node);
      if (scan.selectedThread && turn &&
          read->parent(turn) == scan.selectedThread) {
        ++scan.selectedItemCount;
        scan.dependencies.insert(turn.get());
        scan.dependencies.insert(node.get());
      }
      break;
    }
    default:
      break;
    }
    if (node->id().kind == nodegraph::NodeKind::Interaction) {
      scan.dependencies.insert(node.get());
      const std::shared_ptr<const nodegraph::NodeState> state =
          read->state(node);
      if (state->status == nodegraph::NodeStatus::Pending)
        ++scan.pendingInteractionCount;
    } else if (node->id().kind == nodegraph::NodeKind::UnknownProtocol) {
      ++scan.unknownCount;
    } else if (node->id().kind == nodegraph::NodeKind::Catalog &&
               node->id().canonical == "model") {
      const std::shared_ptr<const nodegraph::NodeState> state =
          read->state(node);
      const nodegraph::Value *models = graphField(*state, "data");
      if (const nodegraph::Value::Array *array =
              models ? models->asArray() : nullptr)
        scan.modelCount = array->size();
    }
  }
  const bool complete =
      cursor == nodes.end() ||
      (cursor != nodes.end() &&
       read->insertionOrder(*cursor) > scan.maximumInsertionOrder);
  read.reset();

  if (!complete) {
    scheduleGraphRefresh();
    return;
  }
  std::unique_ptr<ProtocolGraphScan> finished = std::move(protocolGraphScan);
  const bool needsFreshSample = finished->dirty;
  setProperty("protocolScanCompletions",
              property("protocolScanCompletions").toULongLong() + 1);
  activeGraphDependencies = std::move(finished->dependencies);
  graphDependenciesTab = 4;
  graphDependenciesInfoPage = ProtocolPage;

  protocolThreadCount = finished->threadCount;
  protocolModelCount = finished->modelCount;
  protocolTurnCount = finished->selectedTurnCount;
  protocolItemCount = finished->selectedItemCount;
  protocolPendingCount = finished->pendingInteractionCount;
  protocolUnknownCount = finished->unknownCount;
  refreshProtocolStatistics();
  if (needsFreshSample)
    scheduleGraphRefresh(true);
}

void InspectorPane::renderGraphPlan(InspectorPlanData snapshot) {
  if (pendingPlanSnapshot && *pendingPlanSnapshot == snapshot) {
    scheduleGraphPlanRender();
    return;
  }
  pendingPlanSnapshot = std::move(snapshot);
  planKnownRows = pendingPlanSnapshot->totalRows;
  planRenderFirst = pendingPlanSnapshot->firstRow;
  planRenderEnd =
      planRenderFirst + renderedPlanComponentCount(*pendingPlanSnapshot);
  planRenderCursor = planRenderFirst;
  planRenderClearing = true;
  QScrollBar *planScrollBar = planScroll->verticalScrollBar();
  planScrollValue = planScrollBar->value();
  planScrollFollowsTail = planScrollBar->maximum() > 0 &&
                          planScrollValue == planScrollBar->maximum();
  scheduleGraphPlanRender();
}

void InspectorPane::renderGraphAgents(InspectorAgentsData snapshot) {
  if (pendingAgentsSnapshot && *pendingAgentsSnapshot == snapshot) {
    scheduleGraphAgentsRender();
    return;
  }
  pendingAgentsSnapshot = std::move(snapshot);
  agentsKnownRows = pendingAgentsSnapshot->totalRows;
  agentsRenderFirst = pendingAgentsSnapshot->firstRow;
  agentsRenderEnd =
      agentsRenderFirst + renderedAgentsComponentCount(*pendingAgentsSnapshot);
  agentsRenderCursor = agentsRenderFirst;
  agentsRenderClearing = true;
  QScrollBar *agentsScrollBar = agentsScroll->verticalScrollBar();
  agentsScrollValue = agentsScrollBar->value();
  agentsScrollFollowsTail = agentsScrollBar->maximum() > 0 &&
                            agentsScrollValue == agentsScrollBar->maximum();
  scheduleGraphAgentsRender();
}

void InspectorPane::renderGraphRequests(InspectorRequestsData snapshot) {
  if (pendingRequestsSnapshot && *pendingRequestsSnapshot == snapshot) {
    scheduleGraphRequestsRender();
    return;
  }
  pendingRequestsSnapshot = std::move(snapshot);
  requestsKnownRows = pendingRequestsSnapshot->totalRows;
  requestsRenderFirst = pendingRequestsSnapshot->firstRow;
  requestsRenderEnd = requestsRenderFirst +
                      renderedRequestsComponentCount(*pendingRequestsSnapshot);
  requestsRenderCursor = requestsRenderFirst;
  requestsRenderClearing = true;
  QScrollBar *requestsScrollBar = requestsScroll->verticalScrollBar();
  requestsScrollValue = requestsScrollBar->value();
  requestsScrollFollowsTail =
      requestsScrollBar->maximum() > 0 &&
      requestsScrollValue == requestsScrollBar->maximum();
  scheduleGraphRequestsRender();
}

void InspectorPane::refreshGraphPlanViewport() {
  if (pendingPlanSnapshot || graphRefreshSuspended ||
      inspectorTabs->currentIndex() != 0)
    return;
  if (planRowsMaterialized && materializedRowsCoverViewport(
                                  planLayout, planScroll, planMaterializedFirst,
                                  planMaterializedEnd, planKnownRows))
    return;
  const int scrollValue = planScroll->verticalScrollBar()->value();
  const InspectorRowWindow window = visibleRowWindow(
      planScroll, planKnownRows, PlanEstimatedRowHeight, scrollValue);
  if (planRowsMaterialized && window.first == planMaterializedFirst &&
      window.end == planMaterializedEnd)
    return;
  planGraphScan.reset();
  scheduleGraphRefresh();
}

void InspectorPane::refreshGraphAgentsViewport() {
  if (pendingAgentsSnapshot || graphRefreshSuspended ||
      inspectorTabs->currentIndex() != 1)
    return;
  if (agentsRowsMaterialized &&
      materializedRowsCoverViewport(agentsLayout, agentsScroll,
                                    agentsMaterializedFirst,
                                    agentsMaterializedEnd, agentsKnownRows))
    return;
  const int scrollValue = agentsScroll->verticalScrollBar()->value();
  const InspectorRowWindow window = visibleRowWindow(
      agentsScroll, agentsKnownRows, AgentEstimatedRowHeight, scrollValue);
  if (agentsRowsMaterialized && window.first == agentsMaterializedFirst &&
      window.end == agentsMaterializedEnd)
    return;
  agentsGraphScan.reset();
  scheduleGraphRefresh();
}

void InspectorPane::refreshGraphRequestsViewport() {
  if (pendingRequestsSnapshot || graphRefreshSuspended ||
      inspectorTabs->currentIndex() != 3)
    return;
  if (requestsRowsMaterialized &&
      materializedRowsCoverViewport(requestsLayout, requestsScroll,
                                    requestsMaterializedFirst,
                                    requestsMaterializedEnd, requestsKnownRows))
    return;
  const int scrollValue = requestsScroll->verticalScrollBar()->value();
  const InspectorRowWindow window =
      visibleRowWindow(requestsScroll, requestsKnownRows,
                       RequestEstimatedRowHeight, scrollValue);
  if (requestsRowsMaterialized && window.first == requestsMaterializedFirst &&
      window.end == requestsMaterializedEnd)
    return;
  requestsGraphScan.reset();
  scheduleGraphRefresh();
}

void InspectorPane::scheduleGraphPlanRender() {
  if (!pendingPlanSnapshot || planRenderScheduled || graphRefreshSuspended)
    return;
  planRenderScheduled = true;
  QTimer::singleShot(0, this, [this] {
    planRenderScheduled = false;
    runGraphPlanRender();
  });
}

void InspectorPane::scheduleGraphAgentsRender() {
  if (!pendingAgentsSnapshot || agentsRenderScheduled || graphRefreshSuspended)
    return;
  agentsRenderScheduled = true;
  QTimer::singleShot(0, this, [this] {
    agentsRenderScheduled = false;
    runGraphAgentsRender();
  });
}

void InspectorPane::scheduleGraphRequestsRender() {
  if (!pendingRequestsSnapshot || requestsRenderScheduled ||
      graphRefreshSuspended)
    return;
  requestsRenderScheduled = true;
  QTimer::singleShot(0, this, [this] {
    requestsRenderScheduled = false;
    runGraphRequestsRender();
  });
}

void InspectorPane::runGraphPlanRender() {
  if (!pendingPlanSnapshot || !graph || graphRefreshSuspended ||
      inspectorTabs->currentIndex() != 0)
    return;

  std::size_t work = 0;
  while (planRenderClearing && planLayout->count() != 0 &&
         work < MaximumInspectorWidgetChangesPerPass) {
    static_cast<void>(deleteLastLayoutItem(planLayout));
    ++work;
  }
  if (planRenderClearing && planLayout->count() != 0) {
    scheduleGraphPlanRender();
    return;
  }
  planRenderClearing = false;

  const InspectorPlanData &snapshot = *pendingPlanSnapshot;
  const std::size_t componentCount = planComponentCount(snapshot);
  if (planLayout->count() == 0 && planRenderFirst != 0) {
    planLayout->addSpacing(
        estimatedSpacerHeight(planRenderFirst, PlanEstimatedRowHeight));
  }
  while (planRenderCursor < planRenderEnd &&
         work < MaximumInspectorWidgetChangesPerPass) {
    QWidget *component = nullptr;
    if (!snapshot.threadPresent) {
      component = makeLabel(QStringLiteral("No selected thread."), "muted");
    } else if (snapshot.plan) {
      if (snapshot.plan->includesExplanation && planRenderCursor == 0) {
        component = makeMarkdownLabel(text(snapshot.plan->explanation));
      } else {
        const std::size_t explanationRows =
            snapshot.plan->hasExplanation ? 1U : 0U;
        const std::size_t stepIndex =
            planRenderCursor - explanationRows - snapshot.plan->firstStep;
        const InspectorPlanStepRender &step =
            snapshot.plan->steps.at(stepIndex);
        auto *row = new QFrame;
        row->setObjectName(QStringLiteral("inspectorPlanStep"));
        row->setProperty("rowIndex", static_cast<qulonglong>(planRenderCursor));
        row->setProperty("kind", "raised");
        auto *layout = new QVBoxLayout(row);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(6);
        layout->addWidget(makeLabel(text(step.step)));
        layout->addWidget(statusLabel(step.status));
        component = row;
      }
    } else if (snapshot.planItem) {
      const QString value = text(*snapshot.planItem);
      component = value.isEmpty()
                      ? static_cast<QWidget *>(makeLabel(
                            QStringLiteral("Plan is being prepared."), "muted"))
                      : static_cast<QWidget *>(makeMarkdownLabel(value));
    } else {
      component =
          makeLabel(QStringLiteral("No plan for this thread."), "muted");
    }
    planLayout->addWidget(component);
    ++planRenderCursor;
    ++work;
  }
  if (planRenderCursor != planRenderEnd) {
    scheduleGraphPlanRender();
    return;
  }

  if (planRenderEnd < componentCount) {
    planLayout->addSpacing(estimatedSpacerHeight(componentCount - planRenderEnd,
                                                 PlanEstimatedRowHeight));
  }
  planLayout->addStretch();
  planMaterializedFirst = planRenderFirst;
  planMaterializedEnd = planRenderEnd;
  planRowsMaterialized = true;
  pendingPlanSnapshot.reset();
  QTimer::singleShot(
      0, planScroll,
      [this, scroll = planScroll, value = planScrollValue,
       followsTail = planScrollFollowsTail] {
        QScrollBar *bar = scroll->verticalScrollBar();
        bar->setValue(followsTail
                          ? bar->maximum()
                          : std::clamp(value, bar->minimum(), bar->maximum()));
        refreshGraphPlanViewport();
      });
}

void InspectorPane::runGraphAgentsRender() {
  if (!pendingAgentsSnapshot || !graph || graphRefreshSuspended ||
      inspectorTabs->currentIndex() != 1)
    return;

  std::size_t work = 0;
  while (agentsRenderClearing && agentsLayout->count() != 0 &&
         work < MaximumInspectorWidgetChangesPerPass) {
    static_cast<void>(deleteLastLayoutItem(agentsLayout));
    ++work;
  }
  if (agentsRenderClearing && agentsLayout->count() != 0) {
    scheduleGraphAgentsRender();
    return;
  }
  agentsRenderClearing = false;

  const InspectorAgentsData &snapshot = *pendingAgentsSnapshot;
  const std::size_t componentCount = agentsComponentCount(snapshot);
  if (agentsLayout->count() == 0 && agentsRenderFirst != 0) {
    agentsLayout->addSpacing(
        estimatedSpacerHeight(agentsRenderFirst, AgentEstimatedRowHeight));
  }
  while (agentsRenderCursor < agentsRenderEnd &&
         work < MaximumInspectorWidgetChangesPerPass) {
    if (!snapshot.threadPresent) {
      agentsLayout->addWidget(
          makeLabel(QStringLiteral("No selected thread."), "muted"));
    } else if (snapshot.agents.empty()) {
      agentsLayout->addWidget(makeLabel(
          QStringLiteral("No agent activity for this thread."), "muted"));
    } else {
      agentsLayout->addWidget(
          agentFrame(snapshot.agents.at(agentsRenderCursor - snapshot.firstRow),
                     snapshot.threadId));
    }
    ++agentsRenderCursor;
    ++work;
  }
  if (agentsRenderCursor != agentsRenderEnd) {
    scheduleGraphAgentsRender();
    return;
  }

  if (agentsRenderEnd < componentCount) {
    agentsLayout->addSpacing(estimatedSpacerHeight(
        componentCount - agentsRenderEnd, AgentEstimatedRowHeight));
  }
  agentsLayout->addStretch();
  agentsMaterializedFirst = agentsRenderFirst;
  agentsMaterializedEnd = agentsRenderEnd;
  agentsRowsMaterialized = true;
  pendingAgentsSnapshot.reset();
  QTimer::singleShot(
      0, agentsScroll,
      [this, scroll = agentsScroll, value = agentsScrollValue,
       followsTail = agentsScrollFollowsTail] {
        QScrollBar *bar = scroll->verticalScrollBar();
        bar->setValue(followsTail
                          ? bar->maximum()
                          : std::clamp(value, bar->minimum(), bar->maximum()));
        refreshGraphAgentsViewport();
      });
}

void InspectorPane::runGraphRequestsRender() {
  if (!pendingRequestsSnapshot || !graph || graphRefreshSuspended ||
      inspectorTabs->currentIndex() != 3)
    return;

  std::size_t work = 0;
  while (requestsRenderClearing && requestsLayout->count() != 0 &&
         work < MaximumInspectorWidgetChangesPerPass) {
    static_cast<void>(deleteLastLayoutItem(requestsLayout));
    ++work;
  }
  if (requestsRenderClearing && requestsLayout->count() != 0) {
    scheduleGraphRequestsRender();
    return;
  }
  requestsRenderClearing = false;

  const InspectorRequestsData &snapshot = *pendingRequestsSnapshot;
  const std::size_t componentCount = requestsComponentCount(snapshot);
  if (requestsLayout->count() == 0 && requestsRenderFirst != 0) {
    requestsLayout->addSpacing(
        estimatedSpacerHeight(requestsRenderFirst, RequestEstimatedRowHeight));
  }
  while (requestsRenderCursor < requestsRenderEnd &&
         work < MaximumInspectorWidgetChangesPerPass) {
    if (snapshot.requests.empty()) {
      requestsLayout->addWidget(
          makeLabel(QStringLiteral("No pending requests."), "muted"));
    } else {
      requestsLayout->addWidget(requestFrame(
          snapshot.requests.at(requestsRenderCursor - snapshot.firstRow)));
    }
    ++requestsRenderCursor;
    ++work;
  }
  if (requestsRenderCursor != requestsRenderEnd) {
    scheduleGraphRequestsRender();
    return;
  }

  if (requestsRenderEnd < componentCount) {
    requestsLayout->addSpacing(estimatedSpacerHeight(
        componentCount - requestsRenderEnd, RequestEstimatedRowHeight));
  }
  requestsLayout->addStretch();
  requestsMaterializedFirst = requestsRenderFirst;
  requestsMaterializedEnd = requestsRenderEnd;
  requestsRowsMaterialized = true;
  pendingRequestsSnapshot.reset();
  QTimer::singleShot(
      0, requestsScroll,
      [this, scroll = requestsScroll, value = requestsScrollValue,
       followsTail = requestsScrollFollowsTail] {
        QScrollBar *bar = scroll->verticalScrollBar();
        bar->setValue(followsTail
                          ? bar->maximum()
                          : std::clamp(value, bar->minimum(), bar->maximum()));
        refreshGraphRequestsViewport();
      });
}

void InspectorPane::renderChanges(const InspectorChangesData &snapshot) {
  diffViewer->setRepositoryContext(text(snapshot.threadId), text(snapshot.cwd),
                                   texts(snapshot.commandCwds),
                                   texts(snapshot.changedPaths));
}

void InspectorPane::renderGraphState(QString value) {
  const QByteArray next = value.toUtf8();
  if (next == stateSnapshot)
    return;
  stateSnapshot = next;
  stateView->setPlainText(std::move(value));
}

void InspectorPane::showProtocolTail() {
  if (protocolLogSynchronized)
    return;
  QStringList lines;
  lines.reserve(static_cast<qsizetype>(protocolLines.size()));
  for (const QString &line : protocolLines)
    lines.push_back(line);
  const QString value = lines.join(QLatin1Char('\n'));
  const ScrollPosition position{protocolFollowsTail, protocolPausedScrollValue};
  mutatingProtocolLog = true;
  protocolLog->setPlainText(value);
  protocolLogSynchronized = true;
  restoreProtocolScroll(position.followsTail, position.value);
}

void InspectorPane::restoreProtocolScroll(bool followsTail, int pausedValue) {
  const ScrollPosition position{followsTail, pausedValue};
  const std::uint64_t revision = ++protocolScrollRevision;
  protocolFollowsTail = position.followsTail;
  mutatingProtocolLog = true;
  restoreScrollPosition(protocolLog, position);
  if (!position.followsTail)
    protocolPausedScrollValue = protocolLog->verticalScrollBar()->value();
  mutatingProtocolLog = false;
  QTimer::singleShot(0, this, [this, position, revision] {
    if (revision != protocolScrollRevision)
      return;
    mutatingProtocolLog = true;
    restoreScrollPosition(protocolLog, position);
    protocolFollowsTail = position.followsTail;
    if (!position.followsTail)
      protocolPausedScrollValue = protocolLog->verticalScrollBar()->value();
    mutatingProtocolLog = false;
  });
}

void InspectorPane::refreshProtocolStatistics() {
  const QString statistics =
      QStringLiteral("seq %1  |  diagnostics %2  |  threads %3  |  models %4  "
                     "|  turns %5  |  items %6  |  pending %7  |  telemetry "
                     "%8  |  unknown %9")
          .arg(observedProtocolSequence)
          .arg(receivedProtocolDiagnostics)
          .arg(protocolThreadCount)
          .arg(protocolModelCount)
          .arg(protocolTurnCount)
          .arg(protocolItemCount)
          .arg(protocolPendingCount)
          .arg(protocolTelemetryCount)
          .arg(protocolUnknownCount);
  const QByteArray next = statistics.toUtf8();
  if (next != protocolStatsSnapshot) {
    protocolStatsSnapshot = next;
    protocolStats->setText(std::move(statistics));
  }
}

} // namespace codexui::codex::middle
