// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/InspectorPane.h"

#include "codex/DiffViewer.h"
#include "codex/PresentationStatus.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {

constexpr int MaximumProtocolLines = 2000;
constexpr int InfoChoicePage = 0;
constexpr int StatePage = 1;
constexpr int ProtocolPage = 2;
constexpr qsizetype MaximumGraphDiagnosticCharacters = 32 * 1024;
constexpr std::size_t MaximumInspectorWidgetChangesPerPass = 12;

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
  auto *label = makeLabel(text(displayStatus(status)), "meta");
  if (!classified.tone.empty())
    label->setProperty("tone", classified.tone.data());
  return label;
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

QString agentCopyText(const ui::InspectorAgentRow &agent) {
  QStringList lines{QStringLiteral("Agent")};
  if (!agent.agentPath.empty())
    lines << QStringLiteral("Path: %1").arg(text(agent.agentPath));
  if (!agent.status.empty())
    lines << QStringLiteral("Status: %1").arg(text(displayStatus(agent.status)));
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
    lines << QString{} << QStringLiteral("Thread: %1").arg(text(agent.childThreadId));
  if (!agent.senderThreadId.empty())
    lines << QStringLiteral("Sender: %1").arg(text(agent.senderThreadId));
  if (!agent.receiverThreadIds.empty())
    lines << QStringLiteral("Receivers: %1")
                 .arg(texts(agent.receiverThreadIds).join(QStringLiteral(", ")));
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

std::optional<std::uint64_t> graphUnsigned(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const std::uint64_t *number = value->asUInt64())
    return *number;
  if (const std::int64_t *number = value->asInt64(); number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return std::nullopt;
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

bool terminalStatus(std::string_view status) {
  const StatusKind kind = classifyStatus(status).kind;
  return kind == StatusKind::Completed || kind == StatusKind::Failed ||
         kind == StatusKind::Interrupted;
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
  case nodegraph::NodeKind::Notice:
    return "Notice";
  case nodegraph::NodeKind::UnknownProtocol:
    return "UnknownProtocol";
  }
  return "UnknownProtocol";
}

QString protocolDirection(const nodegraph::Value *value) {
  const std::optional<std::uint64_t> direction = graphUnsigned(value);
  if (!direction)
    return QStringLiteral("unknown direction");
  switch (*direction) {
  case 0:
    return QStringLiteral("client request");
  case 1:
    return QStringLiteral("server request");
  case 2:
    return QStringLiteral("server notification");
  case 3:
    return QStringLiteral("client notification");
  default:
    return QStringLiteral("direction %1").arg(*direction);
  }
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

void appendGraphObject(QString &target,
                       const nodegraph::Value::Object &object,
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
    const qsizetype remaining =
        std::max<qsizetype>(0, MaximumGraphDiagnosticCharacters -
                                  target.size() - 2);
    const std::size_t maximumBytes = static_cast<std::size_t>(remaining);
    const std::size_t bytes = std::min(string->size(), maximumBytes);
    QString displayed = QString::fromUtf8(
        string->data(), static_cast<qsizetype>(bytes));
    displayed.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    displayed.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    if (displayed.size() > remaining)
      displayed.truncate(remaining);
    appendDiagnostic(target, QStringLiteral("\"") + displayed +
                                 QStringLiteral("\""));
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
    appendDiagnostic(target,
                     QString(indentation, QLatin1Char(' ')) +
                         QStringLiteral("]"));
  } else if (const nodegraph::Value::Object *object = value.asObject())
    appendGraphObject(target, *object, indentation, depth);
}

void appendGraphObject(QString &target,
                       const nodegraph::Value::Object &object,
                       int indentation, int depth) {
  if (object.empty()) {
    appendDiagnostic(target, QStringLiteral("{}"));
    return;
  }
  appendDiagnostic(target, QStringLiteral("{\n"));
  for (const auto &[key, entry] : object) {
    appendDiagnostic(target, QString(indentation + 2, QLatin1Char(' ')) +
                                 text(key) + QStringLiteral(": "));
    appendGraphValue(target, entry, indentation + 2, depth + 1);
    appendDiagnostic(target, QStringLiteral("\n"));
    if (target.size() >= MaximumGraphDiagnosticCharacters)
      break;
  }
  appendDiagnostic(target,
                   QString(indentation, QLatin1Char(' ')) +
                       QStringLiteral("}"));
}

} // namespace

QFrame *InspectorPane::agentFrame(const ui::InspectorAgentRow &agent,
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
      std::string(threadId) + '\n' + agent.id;
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

QFrame *InspectorPane::requestFrame(
    const ui::InspectorRequestRow &request) {
  auto *frame = new QFrame;
  frame->setObjectName(QStringLiteral("inspectorRequestFrame"));
  frame->setProperty("nodeCanonicalId", text(request.id));
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
    if (!graph)
      showProtocolTail();
    refreshCurrentTab();
  });
  const auto showInfoChoices = [this] {
    infoStack->setCurrentIndex(InfoChoicePage);
  };
  connect(stateBack, &QPushButton::clicked, this, showInfoChoices);
  connect(protocolBack, &QPushButton::clicked, this, showInfoChoices);

  planScroll = makeScroll(planContent);
  agentsScroll = makeScroll(agentsContent);
  requestsScroll = makeScroll(requestsContent);
  inspectorTabs->addTab(planScroll, QStringLiteral("Plan"));
  inspectorTabs->addTab(agentsScroll, QStringLiteral("Agents"));
  inspectorTabs->addTab(diffViewer, QStringLiteral("Changes"));
  inspectorTabs->addTab(requestsScroll, QStringLiteral("Requests"));
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
                                      RequestAction reject) {
  reviewRequest = std::move(review);
  acceptRequest = std::move(accept);
  rejectRequest = std::move(reject);
}

void InspectorPane::refresh(const ui::InspectorSnapshot &snapshot) {
  if (graph) {
    cancelGraphRowRenders();
    graph = nullptr;
    selectedGraphThread.reset();
    graphRefreshScheduled = false;
    planSnapshot.reset();
    agentsSnapshot.reset();
    requestsSnapshot.reset();
    changesSnapshot.reset();
    activeGraphDependencies.clear();
    graphDependenciesTab = -1;
    graphDependenciesInfoPage = -1;
    stateSnapshot.clear();
    protocolStatsSnapshot.clear();
  }
  currentSnapshot = snapshot;
  refreshCurrentTab();
}

void InspectorPane::refresh(nodegraph::NodeGraph &nextGraph,
                            nodegraph::NodeRef selectedThread) {
  if (graph != &nextGraph) {
    cancelGraphRowRenders();
    graph = &nextGraph;
    currentSnapshot.reset();
    planSnapshot.reset();
    agentsSnapshot.reset();
    requestsSnapshot.reset();
    changesSnapshot.reset();
    stateSnapshot.clear();
    protocolStatsSnapshot.clear();
  }
  if (selectedGraphThread != selectedThread) {
    cancelGraphRowRenders();
    activeGraphDependencies.clear();
    graphDependenciesTab = -1;
    graphDependenciesInfoPage = -1;
  }
  selectedGraphThread = std::move(selectedThread);
  scheduleGraphRefresh();
}

void InspectorPane::graphChanged(const nodegraph::GraphChanged &change) {
  if (graph && graphChangeAffectsCurrentTab(change))
    scheduleGraphRefresh();
}

bool InspectorPane::graphChangeAffectsCurrentTab(
    const nodegraph::GraphChanged &change) {
  if (change.rescanRequired)
    return true;

  const int tab = inspectorTabs->currentIndex();
  const int infoPage = tab == 4 ? infoStack->currentIndex() : InfoChoicePage;
  if (tab == 4) {
    if (infoPage == InfoChoicePage)
      return false;
    if (infoPage == StatePage)
      return !change.affected.empty() || !change.removed.empty();
    const auto protocolNode = [](const nodegraph::NodeRef &node) {
      return node &&
             (node->id().kind == nodegraph::NodeKind::Operation ||
              node->id().kind == nodegraph::NodeKind::UnknownProtocol);
    };
    return std::ranges::any_of(change.affected, protocolNode) ||
           std::ranges::any_of(change.removed, protocolNode);
  }

  const bool dependenciesCurrent =
      graphDependenciesTab == tab && graphDependenciesInfoPage == infoPage;
  const auto knownDependency = [this, dependenciesCurrent](
                                   const nodegraph::NodeRef &node) {
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
    for (nodegraph::NodeRef ancestor = node; ancestor;
         ancestor = read->parent(ancestor)) {
      belowSelectedThread = belowSelectedThread || ancestor == thread;
      belowActiveDependency =
          belowActiveDependency || knownDependency(ancestor);
    }
    if (!belowSelectedThread && !belowActiveDependency)
      continue;

    if (node->id().kind == nodegraph::NodeKind::Thread)
      return belowActiveDependency || node == thread;
    if (node->id().kind == nodegraph::NodeKind::Turn)
      return true;
    if (node->id().kind != nodegraph::NodeKind::Item)
      continue;

    const std::shared_ptr<const nodegraph::NodeState> state = read->state(node);
    const std::string type = graphString(graphField(*state, "type"));
    if (tab == 0 && type == "plan")
      return true;
    if (tab == 1) {
      if (type == "subAgentActivity" || type == "collabAgentToolCall")
        return true;
      if (type == "agentMessage" && belowActiveDependency)
        return true;
    }
    if (tab == 2 &&
        (type == "commandExecution" || type == "fileChange"))
      return true;
  }
  return false;
}

void InspectorPane::cancelGraphRowRenders() {
  pendingPlanSnapshot.reset();
  pendingAgentsSnapshot.reset();
  pendingRequestsSnapshot.reset();
  planRenderClearing = false;
  agentsRenderClearing = false;
  requestsRenderClearing = false;
  planRenderCursor = 0;
  agentsRenderCursor = 0;
  requestsRenderCursor = 0;
  agentsPreservedRows = 0;
  requestsPreservedRows = 0;
}

void InspectorPane::refreshCurrentTab() {
  if (graph) {
    activeGraphDependencies.clear();
    graphDependenciesTab = -1;
    graphDependenciesInfoPage = -1;
    scheduleGraphRefresh();
    return;
  }
  if (!currentSnapshot)
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

void InspectorPane::scheduleGraphRefresh() {
  if (!graph || graphRefreshScheduled)
    return;
  graphRefreshScheduled = true;
  QTimer::singleShot(0, this, [this] {
    graphRefreshScheduled = false;
    runGraphRefresh();
  });
}

void InspectorPane::runGraphRefresh() {
  if (!graph)
    return;

  const int tab = inspectorTabs->currentIndex();
  const int infoPage = tab == 4 ? infoStack->currentIndex() : InfoChoicePage;
  if (tab == 4 && infoPage == InfoChoicePage)
    return;

  auto read = graph->tryRead();
  if (!read) {
    scheduleGraphRefresh();
    return;
  }

  nodegraph::NodeRef thread;
  if (selectedGraphThread &&
      selectedGraphThread->id().kind == nodegraph::NodeKind::Thread &&
      read->find(selectedGraphThread->id()) == selectedGraphThread)
    thread = selectedGraphThread;

  std::unordered_set<const nodegraph::Node *> dependencies;
  if (thread)
    dependencies.insert(thread.get());
  const auto publishDependencies = [this, tab, infoPage](
                                       auto nextDependencies) {
    activeGraphDependencies = std::move(nextDependencies);
    graphDependenciesTab = tab;
    graphDependenciesInfoPage = infoPage;
  };

  if (tab == 0) {
    struct PlanSource final {
      nodegraph::NodeRef turn;
      std::shared_ptr<const nodegraph::NodeState> turnState;
      nodegraph::NodeRef item;
      std::shared_ptr<const nodegraph::NodeState> itemState;
    };

    const std::shared_ptr<const nodegraph::NodeState> threadState =
        thread ? read->state(thread) : nullptr;
    PlanSource source;
    if (thread) {
      const std::vector<nodegraph::NodeRef> turns = read->children(thread);
      for (auto turnIterator = turns.rbegin(); turnIterator != turns.rend();
           ++turnIterator) {
        if ((*turnIterator)->id().kind != nodegraph::NodeKind::Turn)
          continue;
        dependencies.insert(turnIterator->get());
        const std::shared_ptr<const nodegraph::NodeState> turnState =
            read->state(*turnIterator);
        const nodegraph::Value *planValue = graphField(*turnState, "plan");
        const bool hasStructuredPlan =
            (planValue && planValue->asArray()) ||
            (planValue && planValue->asObject() &&
             graphField(*planValue->asObject(), "steps"));
        if (hasStructuredPlan) {
          source.turn = *turnIterator;
          source.turnState = turnState;
          break;
        }

        const std::vector<nodegraph::NodeRef> items =
            read->children(*turnIterator);
        for (auto itemIterator = items.rbegin(); itemIterator != items.rend();
             ++itemIterator) {
          if ((*itemIterator)->id().kind != nodegraph::NodeKind::Item)
            continue;
          const std::shared_ptr<const nodegraph::NodeState> itemState =
              read->state(*itemIterator);
          if (graphString(graphField(*itemState, "type")) != "plan")
            continue;
          dependencies.insert(itemIterator->get());
          source.turn = *turnIterator;
          source.turnState = turnState;
          source.item = *itemIterator;
          source.itemState = itemState;
          break;
        }
        if (source.item)
          break;
      }
    }
    read.reset();
    publishDependencies(std::move(dependencies));

    ui::InspectorPlanSnapshot snapshot;
    snapshot.threadId = thread ? thread->id().canonical : std::string{};
    snapshot.threadPresent = static_cast<bool>(thread);
    if (threadState && source.turnState) {
      const std::string threadStatus = graphStatus(*threadState);
      const nodegraph::Value *planValue =
          graphField(*source.turnState, "plan");
      const nodegraph::Value::Array *steps = nullptr;
      std::string explanation =
          graphString(graphField(*source.turnState, "planExplanation"));
      bool hasStructuredPlan = false;
      if (const nodegraph::Value::Array *array =
              planValue ? planValue->asArray() : nullptr) {
        hasStructuredPlan = true;
        steps = array;
      } else if (const nodegraph::Value::Object *plan =
                     planValue ? planValue->asObject() : nullptr) {
        const nodegraph::Value *nestedSteps = graphField(*plan, "steps");
        hasStructuredPlan = nestedSteps != nullptr;
        steps = nestedSteps ? nestedSteps->asArray() : nullptr;
        if (explanation.empty())
          explanation = graphString(graphField(*plan, "explanation"));
      }
      if (hasStructuredPlan) {
        ui::InspectorPlan plan;
        plan.explanation = std::move(explanation);
        if (steps) {
          plan.steps.reserve(steps->size());
          const std::string turnStatus = graphStatus(*source.turnState);
          for (const nodegraph::Value &entry : *steps) {
            const nodegraph::Value::Object *step = entry.asObject();
            if (!step)
              continue;
            const std::string status =
                graphString(graphField(*step, "status"));
            plan.steps.push_back(
                {graphString(graphField(*step, "step")),
                 effectivePlanStepStatus(status, turnStatus, threadStatus)});
          }
        }
        snapshot.plan = std::move(plan);
      } else if (source.itemState) {
        snapshot.planItem =
            graphString(graphField(*source.itemState, "text"));
      }
    }
    renderGraphPlan(std::move(snapshot));
    return;
  }

  if (tab == 1) {
    struct AgentSource final {
      nodegraph::NodeRef item;
      std::shared_ptr<const nodegraph::NodeState> state;
      nodegraph::NodeRef childThread;
      std::shared_ptr<const nodegraph::NodeState> childState;
      std::shared_ptr<const nodegraph::NodeState> latestResultState;
    };
    std::vector<AgentSource> sources;
    if (thread) {
      for (const nodegraph::NodeRef &turn : read->children(thread)) {
        if (turn->id().kind != nodegraph::NodeKind::Turn)
          continue;
        dependencies.insert(turn.get());
        for (const nodegraph::NodeRef &item : read->children(turn)) {
          if (item->id().kind != nodegraph::NodeKind::Item)
            continue;
          const std::shared_ptr<const nodegraph::NodeState> state =
              read->state(item);
          const std::string type = graphString(graphField(*state, "type"));
          if (type != "subAgentActivity" && type != "collabAgentToolCall")
            continue;
          dependencies.insert(item.get());

          nodegraph::NodeRef childThread;
          for (const nodegraph::NodeRef &candidate : read->related(
                   item, nodegraph::RelationKind::AgentChildThread)) {
            if (candidate->id().kind == nodegraph::NodeKind::Thread) {
              childThread = candidate;
              break;
            }
          }
          if (type == "collabAgentToolCall") {
            const std::string tool = graphString(graphField(*state, "tool"));
            const bool spawn =
                tool == "spawn_agent" || tool == "spawnAgent" ||
                tool == "spawn_agents_on_csv" || tool == "spawnAgentsOnCsv";
            if (!spawn || !childThread)
              continue;
          }

          AgentSource source{item, state, childThread};
          if (childThread) {
            dependencies.insert(childThread.get());
            source.childState = read->state(childThread);
            const std::vector<nodegraph::NodeRef> childTurns =
                read->children(childThread);
            for (auto childTurn = childTurns.rbegin();
                 childTurn != childTurns.rend() && !source.latestResultState;
                 ++childTurn) {
              if ((*childTurn)->id().kind != nodegraph::NodeKind::Turn)
                continue;
              dependencies.insert(childTurn->get());
              const std::vector<nodegraph::NodeRef> childItems =
                  read->children(*childTurn);
              for (auto childItem = childItems.rbegin();
                   childItem != childItems.rend(); ++childItem) {
                if ((*childItem)->id().kind != nodegraph::NodeKind::Item)
                  continue;
                const std::shared_ptr<const nodegraph::NodeState>
                    childItemState = read->state(*childItem);
                if (graphString(graphField(*childItemState, "type")) !=
                    "agentMessage")
                  continue;
                dependencies.insert(childItem->get());
                if (!graphString(graphField(*childItemState, "text")).empty()) {
                  source.latestResultState = childItemState;
                  break;
                }
              }
            }
          }
          sources.emplace_back(std::move(source));
        }
      }
    }
    read.reset();
    publishDependencies(std::move(dependencies));

    ui::InspectorAgentsSnapshot snapshot;
    snapshot.threadId = thread ? thread->id().canonical : std::string{};
    snapshot.threadPresent = static_cast<bool>(thread);
    snapshot.agents.reserve(sources.size());
    for (const AgentSource &source : sources) {
      ui::InspectorAgentRow row;
      row.id = source.item->id().canonical;
      row.status = graphStatus(*source.state);
      row.agentPath = graphString(graphField(*source.state, "agentPath"));
      row.tool = graphString(graphField(*source.state, "tool"));
      row.model = graphString(graphField(*source.state, "model"));
      row.reasoningEffort =
          graphString(graphField(*source.state, "reasoningEffort"));
      row.prompt = graphString(graphField(*source.state, "prompt"));
      row.resultText = graphString(graphField(*source.state, "resultText"));
      row.senderThreadId =
          graphString(graphField(*source.state, "senderThreadId"));
      if (const nodegraph::Value::Array *receivers =
              graphField(*source.state, "receiverThreadIds")
                  ? graphField(*source.state, "receiverThreadIds")->asArray()
                  : nullptr) {
        row.receiverThreadIds.reserve(receivers->size());
        for (const nodegraph::Value &receiver : *receivers) {
          std::string id = graphString(&receiver);
          if (!id.empty())
            row.receiverThreadIds.emplace_back(std::move(id));
        }
      }
      if (source.childThread) {
        row.childThreadId = source.childThread->id().canonical;
        const std::string childStatus = graphStatus(*source.childState);
        if (row.status.empty() || terminalStatus(childStatus))
          row.status = childStatus;
        if (source.latestResultState)
          row.resultText =
              graphString(graphField(*source.latestResultState, "text"));
      } else {
        row.childThreadId =
            graphString(graphField(*source.state, "agentThreadId"));
      }
      snapshot.agents.emplace_back(std::move(row));
    }
    renderGraphAgents(std::move(snapshot));
    return;
  }

  if (tab == 2) {
    struct ItemSource final {
      nodegraph::NodeRef item;
      std::shared_ptr<const nodegraph::NodeState> state;
    };
    const std::shared_ptr<const nodegraph::NodeState> threadState =
        thread ? read->state(thread) : nullptr;
    std::vector<ItemSource> sources;
    if (thread) {
      for (const nodegraph::NodeRef &turn : read->children(thread)) {
        if (turn->id().kind != nodegraph::NodeKind::Turn)
          continue;
        dependencies.insert(turn.get());
        for (const nodegraph::NodeRef &item : read->children(turn)) {
          if (item->id().kind != nodegraph::NodeKind::Item)
            continue;
          const std::shared_ptr<const nodegraph::NodeState> state =
              read->state(item);
          const std::string type = graphString(graphField(*state, "type"));
          if (type != "commandExecution" && type != "fileChange")
            continue;
          dependencies.insert(item.get());
          sources.push_back({item, state});
        }
      }
    }
    read.reset();
    publishDependencies(std::move(dependencies));

    ui::InspectorChangesSnapshot snapshot;
    snapshot.threadId = thread ? thread->id().canonical : std::string{};
    if (threadState) {
      snapshot.cwd = graphString(graphField(*threadState, "cwd"));
      for (const ItemSource &source : sources) {
        const std::string type = graphString(graphField(*source.state, "type"));
        if (type == "commandExecution") {
          appendUniqueBounded(
              snapshot.commandCwds,
              graphString(graphField(*source.state, "cwd")), 64);
        } else if (const nodegraph::Value::Array *changes =
                       graphField(*source.state, "changes")
                           ? graphField(*source.state, "changes")->asArray()
                           : nullptr) {
          for (const nodegraph::Value &change : *changes) {
            const nodegraph::Value::Object *object = change.asObject();
            if (!object)
              continue;
            appendUniqueBounded(snapshot.changedPaths,
                                graphString(graphField(*object, "path")), 512);
          }
        }
      }
    }
    if (!changesSnapshot || *changesSnapshot != snapshot) {
      changesSnapshot = snapshot;
      renderChanges(*changesSnapshot);
    }
    return;
  }

  if (tab == 3) {
    struct RequestSource final {
      nodegraph::NodeRef interaction;
      std::shared_ptr<const nodegraph::NodeState> state;
      nodegraph::NodeRef targetThread;
      std::shared_ptr<const nodegraph::NodeState> targetState;
    };
    nodegraph::NodeRef connection;
    std::shared_ptr<const nodegraph::NodeState> connectionState;
    if ((connection =
             read->find({nodegraph::NodeKind::Connection, "connection"}))) {
      dependencies.insert(connection.get());
      connectionState = read->state(connection);
    }
    const nodegraph::NodeRef runtime =
        read->find({nodegraph::NodeKind::Runtime, "runtime"});
    if (runtime)
      dependencies.insert(runtime.get());
    std::vector<RequestSource> sources;
    const std::vector<nodegraph::NodeRef> pendingInteractions =
        runtime ? read->related(runtime,
                                nodegraph::RelationKind::PendingInteraction)
                : std::vector<nodegraph::NodeRef>{};
    sources.reserve(pendingInteractions.size());
    for (const nodegraph::NodeRef &candidate : pendingInteractions) {
      if (!candidate ||
          candidate->id().kind != nodegraph::NodeKind::Interaction)
        continue;
      dependencies.insert(candidate.get());
      const std::shared_ptr<const nodegraph::NodeState> state =
          read->state(candidate);
      if (state->status != nodegraph::NodeStatus::Pending &&
          state->status != nodegraph::NodeStatus::Failed)
        continue;
      RequestSource source{candidate, state};
      for (nodegraph::NodeRef target : read->related(
               candidate, nodegraph::RelationKind::InteractionTarget)) {
        while (target && target->id().kind != nodegraph::NodeKind::Thread) {
          dependencies.insert(target.get());
          target = read->parent(target);
        }
        if (target) {
          dependencies.insert(target.get());
          source.targetThread = target;
          source.targetState = read->state(target);
          break;
        }
      }
      sources.emplace_back(std::move(source));
    }
    read.reset();
    publishDependencies(std::move(dependencies));

    ui::InspectorRequestsSnapshot snapshot;
    bool canControl = false;
    std::uint64_t generation = 0;
    if (connectionState) {
      const std::string transport =
          graphString(graphField(*connectionState, "transportState"));
      const std::string provider =
          graphString(graphField(*connectionState, "providerState"));
      const std::string role =
          graphString(graphField(*connectionState, "role"));
      canControl =
          (transport == "connected" ||
           connectionState->status == nodegraph::NodeStatus::Connected) &&
          provider == "ready" && role == "controller";
      generation =
          graphUnsigned(graphField(*connectionState, "connectionGeneration"))
              .value_or(graphUnsigned(graphField(*connectionState,
                                                 "providerGeneration"))
                            .value_or(0));
    }

    snapshot.requests.reserve(sources.size());
    for (const RequestSource &source : sources) {
      const std::string method =
          graphString(graphField(*source.state, "method"));
      const nodegraph::Value *payloadValue =
          graphField(*source.state, "payload");
      const nodegraph::Value::Object *payload =
          payloadValue ? payloadValue->asObject() : nullptr;

      ui::InspectorRequestRow row;
      row.id = source.interaction->id().canonical;
      row.kind = requestKind(method);
      row.generation = generation;
      row.actionable =
          canControl && source.state->status == nodegraph::NodeStatus::Pending;
      if (payload) {
        row.command = graphString(graphField(*payload, "command"));
        row.reason = graphString(graphField(*payload, "reason"));
        row.message = graphString(graphField(*payload, "message"));
        const nodegraph::Value *questionsValue =
            graphField(*payload, "questions");
        if (const nodegraph::Value::Array *questions =
                questionsValue ? questionsValue->asArray() : nullptr)
          row.questionCount = questions->size();
      }
      if (row.message.empty())
        row.message = graphString(graphField(*source.state, "error"));

      if (source.targetThread) {
        row.threadContext = source.targetThread->id().canonical;
        std::string title = graphString(graphField(*source.targetState, "name"));
        if (title.empty())
          title = graphString(graphField(*source.targetState, "title"));
        if (!title.empty())
          row.threadContext = std::move(title);
      } else if (payload) {
        row.threadContext = graphString(graphField(*payload, "threadId"));
      }
      snapshot.requests.emplace_back(std::move(row));
    }
    renderGraphRequests(std::move(snapshot));
    return;
  }

  if (tab == 4 && infoPage == StatePage) {
    struct StateNodeSource final {
      nodegraph::NodeRef node;
      std::shared_ptr<const nodegraph::NodeState> state;
    };
    const std::uint64_t revision = read->revision();
    const std::size_t nodeCount = read->orderedNodes().size();
    std::vector<StateNodeSource> nodes;
    nodes.reserve(nodeCount);
    for (const nodegraph::NodeRef &node : read->orderedNodes()) {
      nodes.push_back(
          {node, node->id().kind == nodegraph::NodeKind::Interaction
                     ? read->state(node)
                     : nullptr});
    }

    std::string selectedId;
    std::string parentId;
    std::uint64_t selectedRevision = 0;
    std::size_t childCount = 0;
    std::shared_ptr<const nodegraph::NodeState> selectedState;
    if (thread) {
      selectedId = thread->id().canonical;
      selectedState = read->state(thread);
      selectedRevision = read->changedRevision(thread);
      childCount = read->childCount(thread);
      if (const nodegraph::NodeRef parent = read->parent(thread))
        parentId = parent->id().canonical;
    }
    read.reset();
    publishDependencies(std::move(dependencies));

    std::map<std::string, std::size_t, std::less<>> kindCounts;
    std::size_t pendingInteractions = 0;
    for (const StateNodeSource &source : nodes) {
      ++kindCounts[std::string(nodeKindName(source.node->id().kind))];
      if (source.node->id().kind == nodegraph::NodeKind::Interaction &&
          source.state &&
          source.state->status == nodegraph::NodeStatus::Pending)
        ++pendingInteractions;
    }
    const std::string selectedStatus =
        selectedState ? graphStatus(*selectedState) : std::string{};

    QString value = QStringLiteral("Shared NodeGraph\nRevision: %1\nNodes: %2\n"
                                   "Pending interactions: %3\n")
                        .arg(revision)
                        .arg(nodeCount)
                        .arg(pendingInteractions);
    appendDiagnostic(value, QStringLiteral("Node kinds:\n"));
    for (const auto &[kind, count] : kindCounts) {
      appendDiagnostic(value, QStringLiteral("  %1: %2\n")
                                  .arg(text(kind))
                                  .arg(count));
    }
    appendDiagnostic(value, QStringLiteral("\nSelected thread:\n"));
    if (!selectedState) {
      appendDiagnostic(value, QStringLiteral("  <none>\n"));
    } else {
      appendDiagnostic(value,
                       QStringLiteral("  id: %1\n  status: %2\n"
                                      "  changed revision: %3\n"
                                      "  parent: %4\n  turns: %5\n  fields: ")
                           .arg(text(selectedId), text(selectedStatus))
                           .arg(selectedRevision)
                           .arg(parentId.empty() ? QStringLiteral("<root>")
                                                 : text(parentId))
                           .arg(childCount));
      appendGraphObject(value, selectedState->fields, 2, 0);
      appendDiagnostic(value, QStringLiteral("\n"));
    }
    if (value.size() >= MaximumGraphDiagnosticCharacters) {
      value.truncate(MaximumGraphDiagnosticCharacters - 34);
      value += QStringLiteral("\n[Graph diagnostic truncated]\n");
    }
    renderGraphState(std::move(value));
    return;
  }

  if (tab == 4 && infoPage == ProtocolPage) {
    struct DiagnosticSource final {
      nodegraph::NodeKind kind = nodegraph::NodeKind::Operation;
      std::string id;
      std::shared_ptr<const nodegraph::NodeState> state;
    };
    const std::uint64_t revision = read->revision();
    const std::size_t nodeCount = read->orderedNodes().size();
    std::vector<DiagnosticSource> sources;
    for (const nodegraph::NodeRef &node : read->orderedNodes()) {
      if (node->id().kind != nodegraph::NodeKind::Operation &&
          node->id().kind != nodegraph::NodeKind::UnknownProtocol)
        continue;
      dependencies.insert(node.get());
      sources.push_back({node->id().kind, node->id().canonical,
                         read->state(node)});
    }
    read.reset();
    publishDependencies(std::move(dependencies));

    struct DiagnosticEntry final {
      nodegraph::NodeKind kind = nodegraph::NodeKind::Operation;
      std::string id;
      std::string method;
      std::string status;
      QString direction;
    };
    std::size_t operationCount = 0;
    std::size_t unknownCount = 0;
    std::size_t pendingCount = 0;
    std::vector<DiagnosticEntry> entries;
    entries.reserve(std::min<std::size_t>(sources.size(),
                                          MaximumProtocolLines));
    for (const DiagnosticSource &source : sources) {
      if (source.kind == nodegraph::NodeKind::Operation) {
        ++operationCount;
        if (source.state->status == nodegraph::NodeStatus::Pending)
          ++pendingCount;
      } else {
        ++unknownCount;
      }
      if (entries.size() == MaximumProtocolLines)
        continue;
      entries.push_back(
          {source.kind, source.id,
           graphString(graphField(*source.state, "method")),
           graphStatus(*source.state),
           protocolDirection(graphField(*source.state, "direction"))});
    }

    QString log;
    appendDiagnostic(log, QStringLiteral("Current worker operations\n"));
    bool wroteOperation = false;
    for (const DiagnosticEntry &entry : entries) {
      if (entry.kind != nodegraph::NodeKind::Operation)
        continue;
      wroteOperation = true;
      appendDiagnostic(log, QStringLiteral("%1  %2  %3\n")
                                .arg(text(entry.status), text(entry.id),
                                     text(entry.method)));
    }
    if (!wroteOperation)
      appendDiagnostic(log, QStringLiteral("<none>\n"));
    appendDiagnostic(log,
                     QStringLiteral("\nUnknown protocol alternatives\n"));
    bool wroteUnknown = false;
    for (const DiagnosticEntry &entry : entries) {
      if (entry.kind != nodegraph::NodeKind::UnknownProtocol)
        continue;
      wroteUnknown = true;
      appendDiagnostic(log, QStringLiteral("%1  %2  %3\n")
                                .arg(entry.direction, text(entry.id),
                                     text(entry.method)));
    }
    if (!wroteUnknown)
      appendDiagnostic(log, QStringLiteral("<none>\n"));
    if (operationCount + unknownCount > entries.size())
      appendDiagnostic(log, QStringLiteral("\n%1 more entries omitted\n")
                                .arg(operationCount + unknownCount -
                                     entries.size()));
    const QString statistics =
        QStringLiteral("revision %1  |  nodes %2  |  operations %3  |  "
                       "pending %4  |  unknown %5")
            .arg(revision)
            .arg(nodeCount)
            .arg(operationCount)
            .arg(pendingCount)
            .arg(unknownCount);
    renderGraphProtocol(std::move(log), statistics);
  }
}

void InspectorPane::renderGraphPlan(ui::InspectorPlanSnapshot snapshot) {
  if (pendingPlanSnapshot && *pendingPlanSnapshot == snapshot) {
    scheduleGraphPlanRender();
    return;
  }
  if (!pendingPlanSnapshot && planSnapshot && *planSnapshot == snapshot)
    return;
  pendingPlanSnapshot = std::move(snapshot);
  planRenderCursor = 0;
  planRenderClearing = true;
  planScrollValue = planScroll->verticalScrollBar()->value();
  scheduleGraphPlanRender();
}

void InspectorPane::renderGraphAgents(ui::InspectorAgentsSnapshot snapshot) {
  if (pendingAgentsSnapshot && *pendingAgentsSnapshot == snapshot) {
    scheduleGraphAgentsRender();
    return;
  }
  if (!pendingAgentsSnapshot && agentsSnapshot && *agentsSnapshot == snapshot)
    return;

  const bool interruptedRender = pendingAgentsSnapshot.has_value();
  std::size_t preserved = 0;
  if (!interruptedRender && agentsSnapshot &&
      agentsSnapshot->threadId == snapshot.threadId &&
      agentsSnapshot->threadPresent == snapshot.threadPresent &&
      agentsSnapshot->threadPresent) {
    const std::size_t common =
        std::min(agentsSnapshot->agents.size(), snapshot.agents.size());
    while (preserved < common &&
           agentsSnapshot->agents[preserved] == snapshot.agents[preserved])
      ++preserved;
  }
  pendingAgentsSnapshot = std::move(snapshot);
  agentsPreservedRows = preserved;
  agentsRenderCursor = preserved;
  agentsRenderClearing = true;
  agentsScrollValue = agentsScroll->verticalScrollBar()->value();
  scheduleGraphAgentsRender();
}

void InspectorPane::renderGraphRequests(
    ui::InspectorRequestsSnapshot snapshot) {
  if (pendingRequestsSnapshot && *pendingRequestsSnapshot == snapshot) {
    scheduleGraphRequestsRender();
    return;
  }
  if (!pendingRequestsSnapshot && requestsSnapshot &&
      *requestsSnapshot == snapshot)
    return;

  const bool interruptedRender = pendingRequestsSnapshot.has_value();
  std::size_t preserved = 0;
  if (!interruptedRender && requestsSnapshot) {
    const std::size_t common =
        std::min(requestsSnapshot->requests.size(), snapshot.requests.size());
    while (preserved < common &&
           requestsSnapshot->requests[preserved] == snapshot.requests[preserved])
      ++preserved;
  }
  pendingRequestsSnapshot = std::move(snapshot);
  requestsPreservedRows = preserved;
  requestsRenderCursor = preserved;
  requestsRenderClearing = true;
  requestsScrollValue = requestsScroll->verticalScrollBar()->value();
  scheduleGraphRequestsRender();
}

void InspectorPane::scheduleGraphPlanRender() {
  if (!pendingPlanSnapshot || planRenderScheduled)
    return;
  planRenderScheduled = true;
  QTimer::singleShot(0, this, [this] {
    planRenderScheduled = false;
    runGraphPlanRender();
  });
}

void InspectorPane::scheduleGraphAgentsRender() {
  if (!pendingAgentsSnapshot || agentsRenderScheduled)
    return;
  agentsRenderScheduled = true;
  QTimer::singleShot(0, this, [this] {
    agentsRenderScheduled = false;
    runGraphAgentsRender();
  });
}

void InspectorPane::scheduleGraphRequestsRender() {
  if (!pendingRequestsSnapshot || requestsRenderScheduled)
    return;
  requestsRenderScheduled = true;
  QTimer::singleShot(0, this, [this] {
    requestsRenderScheduled = false;
    runGraphRequestsRender();
  });
}

void InspectorPane::runGraphPlanRender() {
  if (!pendingPlanSnapshot || !graph || inspectorTabs->currentIndex() != 0)
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

  const ui::InspectorPlanSnapshot &snapshot = *pendingPlanSnapshot;
  const bool hasExplanation =
      snapshot.plan && !snapshot.plan->explanation.empty();
  const std::size_t componentCount =
      !snapshot.threadPresent
          ? 1
          : snapshot.plan
                ? snapshot.plan->steps.size() + (hasExplanation ? 1U : 0U)
                : 1;
  while (planRenderCursor < componentCount &&
         work < MaximumInspectorWidgetChangesPerPass) {
    QWidget *component = nullptr;
    if (!snapshot.threadPresent) {
      component = makeLabel(QStringLiteral("No selected thread."), "muted");
    } else if (snapshot.plan) {
      if (hasExplanation && planRenderCursor == 0) {
        component = makeMarkdownLabel(text(snapshot.plan->explanation));
      } else {
        const std::size_t stepIndex =
            planRenderCursor - (hasExplanation ? 1U : 0U);
        const ui::InspectorPlanStep &step = snapshot.plan->steps[stepIndex];
        auto *row = new QFrame;
        row->setObjectName(QStringLiteral("inspectorPlanStep"));
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
  if (planRenderCursor != componentCount) {
    scheduleGraphPlanRender();
    return;
  }

  planLayout->addStretch();
  planSnapshot = std::move(*pendingPlanSnapshot);
  pendingPlanSnapshot.reset();
  QTimer::singleShot(0, planScroll, [scroll = planScroll,
                                    value = planScrollValue] {
    scroll->verticalScrollBar()->setValue(
        std::clamp(value, scroll->verticalScrollBar()->minimum(),
                   scroll->verticalScrollBar()->maximum()));
  });
}

void InspectorPane::runGraphAgentsRender() {
  if (!pendingAgentsSnapshot || !graph || inspectorTabs->currentIndex() != 1)
    return;

  std::size_t work = 0;
  while (agentsRenderClearing &&
         agentsLayout->count() > static_cast<int>(agentsPreservedRows) &&
         work < MaximumInspectorWidgetChangesPerPass) {
    static_cast<void>(deleteLastLayoutItem(agentsLayout));
    ++work;
  }
  if (agentsRenderClearing &&
      agentsLayout->count() > static_cast<int>(agentsPreservedRows)) {
    scheduleGraphAgentsRender();
    return;
  }
  agentsRenderClearing = false;

  const ui::InspectorAgentsSnapshot &snapshot = *pendingAgentsSnapshot;
  const std::size_t componentCount =
      !snapshot.threadPresent || snapshot.agents.empty()
          ? 1
          : snapshot.agents.size();
  while (agentsRenderCursor < componentCount &&
         work < MaximumInspectorWidgetChangesPerPass) {
    if (!snapshot.threadPresent) {
      agentsLayout->addWidget(
          makeLabel(QStringLiteral("No selected thread."), "muted"));
    } else if (snapshot.agents.empty()) {
      agentsLayout->addWidget(makeLabel(
          QStringLiteral("No agent activity for this thread."), "muted"));
    } else {
      agentsLayout->addWidget(
          agentFrame(snapshot.agents[agentsRenderCursor], snapshot.threadId));
    }
    ++agentsRenderCursor;
    ++work;
  }
  if (agentsRenderCursor != componentCount) {
    scheduleGraphAgentsRender();
    return;
  }

  agentsLayout->addStretch();
  agentsSnapshot = std::move(*pendingAgentsSnapshot);
  pendingAgentsSnapshot.reset();
  QTimer::singleShot(0, agentsScroll, [scroll = agentsScroll,
                                      value = agentsScrollValue] {
    scroll->verticalScrollBar()->setValue(
        std::clamp(value, scroll->verticalScrollBar()->minimum(),
                   scroll->verticalScrollBar()->maximum()));
  });
}

void InspectorPane::runGraphRequestsRender() {
  if (!pendingRequestsSnapshot || !graph || inspectorTabs->currentIndex() != 3)
    return;

  std::size_t work = 0;
  while (requestsRenderClearing &&
         requestsLayout->count() > static_cast<int>(requestsPreservedRows) &&
         work < MaximumInspectorWidgetChangesPerPass) {
    static_cast<void>(deleteLastLayoutItem(requestsLayout));
    ++work;
  }
  if (requestsRenderClearing &&
      requestsLayout->count() > static_cast<int>(requestsPreservedRows)) {
    scheduleGraphRequestsRender();
    return;
  }
  requestsRenderClearing = false;

  const ui::InspectorRequestsSnapshot &snapshot = *pendingRequestsSnapshot;
  const std::size_t componentCount =
      snapshot.requests.empty() ? 1 : snapshot.requests.size();
  while (requestsRenderCursor < componentCount &&
         work < MaximumInspectorWidgetChangesPerPass) {
    if (snapshot.requests.empty()) {
      requestsLayout->addWidget(
          makeLabel(QStringLiteral("No pending requests."), "muted"));
    } else {
      requestsLayout->addWidget(
          requestFrame(snapshot.requests[requestsRenderCursor]));
    }
    ++requestsRenderCursor;
    ++work;
  }
  if (requestsRenderCursor != componentCount) {
    scheduleGraphRequestsRender();
    return;
  }

  requestsLayout->addStretch();
  requestsSnapshot = std::move(*pendingRequestsSnapshot);
  pendingRequestsSnapshot.reset();
  QTimer::singleShot(0, requestsScroll, [scroll = requestsScroll,
                                        value = requestsScrollValue] {
    scroll->verticalScrollBar()->setValue(
        std::clamp(value, scroll->verticalScrollBar()->minimum(),
                   scroll->verticalScrollBar()->maximum()));
  });
}

void InspectorPane::refreshPlan() {
  const ui::InspectorPlanSnapshot &next = currentSnapshot->plan;
  if (planSnapshot && *planSnapshot == next)
    return;
  planSnapshot = next;
  renderPlan(*planSnapshot);
}

void InspectorPane::renderPlan(const ui::InspectorPlanSnapshot &snapshot) {
  setUpdatesEnabled(false);
  clearLayout(planLayout);
  if (!snapshot.threadPresent) {
    planLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
  } else if (snapshot.plan) {
    const QString explanation = text(snapshot.plan->explanation);
    if (!explanation.isEmpty())
      planLayout->addWidget(makeMarkdownLabel(explanation));
    for (const ui::InspectorPlanStep &step : snapshot.plan->steps) {
      auto *row = new QFrame;
      row->setObjectName(QStringLiteral("inspectorPlanStep"));
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
  const ui::InspectorAgentsSnapshot &next = currentSnapshot->agents;
  if (agentsSnapshot && *agentsSnapshot == next)
    return;
  agentsSnapshot = next;
  renderAgents(*agentsSnapshot);
}

void InspectorPane::renderAgents(
    const ui::InspectorAgentsSnapshot &snapshot) {
  setUpdatesEnabled(false);
  clearLayout(agentsLayout);
  if (!snapshot.threadPresent)
    agentsLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
  else if (snapshot.agents.empty())
    agentsLayout->addWidget(makeLabel(
        QStringLiteral("No agent activity for this thread."), "muted"));
  else
    for (const ui::InspectorAgentRow &agent : snapshot.agents)
      agentsLayout->addWidget(agentFrame(agent, snapshot.threadId));
  agentsLayout->addStretch();
  setUpdatesEnabled(true);
}

void InspectorPane::refreshChanges() {
  const ui::InspectorChangesSnapshot &next = currentSnapshot->changes;
  if (changesSnapshot && *changesSnapshot == next)
    return;
  changesSnapshot = next;
  renderChanges(*changesSnapshot);
}

void InspectorPane::renderChanges(
    const ui::InspectorChangesSnapshot &snapshot) {
  diffViewer->setRepositoryContext(
      text(snapshot.threadId), text(snapshot.cwd), texts(snapshot.commandCwds),
      texts(snapshot.changedPaths));
}

void InspectorPane::refreshRequests() {
  const ui::InspectorRequestsSnapshot &next = currentSnapshot->requests;
  if (requestsSnapshot && *requestsSnapshot == next)
    return;
  requestsSnapshot = next;
  renderRequests(*requestsSnapshot);
}

void InspectorPane::renderRequests(
    const ui::InspectorRequestsSnapshot &requestSnapshot) {
  const std::vector<ui::InspectorRequestRow> &snapshot =
      requestSnapshot.requests;
  setUpdatesEnabled(false);
  clearLayout(requestsLayout);
  for (const ui::InspectorRequestRow &request : snapshot)
    requestsLayout->addWidget(requestFrame(request));
  if (snapshot.empty())
    requestsLayout->addWidget(
        makeLabel(QStringLiteral("No pending requests."), "muted"));
  requestsLayout->addStretch();
  setUpdatesEnabled(true);
}

void InspectorPane::refreshState() {
  std::string rendered = currentSnapshot->state.state.dump(2);
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
  const ui::InspectorStateSnapshot &snapshot = currentSnapshot->state;
  const QString value =
      QStringLiteral("seq %1  |  threads %2  |  models %3  |  turns %4  |  "
                     "items %5  |  pending %6  |  telemetry %7")
          .arg(static_cast<qulonglong>(observedSequence))
          .arg(static_cast<qulonglong>(snapshot.threadCount))
          .arg(static_cast<qulonglong>(snapshot.modelCount))
          .arg(static_cast<qulonglong>(snapshot.selectedThreadTurnCount))
          .arg(static_cast<qulonglong>(snapshot.selectedThreadItemCount))
          .arg(static_cast<qulonglong>(snapshot.pendingRequestCount))
          .arg(static_cast<qulonglong>(snapshot.telemetryCount));
  if (value.toUtf8() == protocolStatsSnapshot)
    return;
  protocolStatsSnapshot = value.toUtf8();
  protocolStats->setText(value);
}

void InspectorPane::renderGraphState(QString value) {
  const QByteArray next = value.toUtf8();
  if (next == stateSnapshot)
    return;
  stateSnapshot = next;
  stateView->setPlainText(std::move(value));
}

void InspectorPane::renderGraphProtocol(QString log, QString statistics) {
  if (protocolLog->toPlainText() != log) {
    const ScrollPosition position{protocolFollowsTail,
                                  protocolPausedScrollValue};
    mutatingProtocolLog = true;
    protocolLog->setPlainText(std::move(log));
    restoreProtocolScroll(position.followsTail, position.value);
  }
  const QByteArray next = statistics.toUtf8();
  if (next != protocolStatsSnapshot) {
    protocolStatsSnapshot = next;
    protocolStats->setText(std::move(statistics));
  }
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
  if (graph)
    return;
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
