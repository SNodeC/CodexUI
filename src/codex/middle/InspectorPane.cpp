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

const nodegraph::Value *graphField(const nodegraph::Value::Object &object,
                                   std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

std::string graphString(const nodegraph::Value *value) {
  if (!value)
    return {};
  if (const auto *string = value->asString())
    return *string;
  if (const auto *number = value->asUInt64())
    return std::to_string(*number);
  if (const auto *number = value->asInt64())
    return std::to_string(*number);
  return {};
}

std::optional<std::uint64_t> graphUnsigned(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const auto *number = value->asUInt64())
    return *number;
  if (const auto *number = value->asInt64(); number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return std::nullopt;
}

bool sensitiveDiagnosticText(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const unsigned char character : value)
    lowered.push_back(static_cast<char>(std::tolower(character)));
  constexpr std::array markers{
      std::string_view("authorization"), std::string_view("bearer "),
      std::string_view("password"),      std::string_view("secret"),
      std::string_view("token="),        std::string_view("token:"),
      std::string_view("credential"),    std::string_view("cookie"),
      std::string_view("-----begin"),    std::string_view("github_pat_"),
      std::string_view("ghp_"),          std::string_view("xoxb-"),
      std::string_view("xoxp-")};
  return std::ranges::any_of(markers, [&lowered](std::string_view marker) {
    return lowered.find(marker) != std::string::npos;
  }) || lowered.find("sk-") != std::string::npos;
}

QString protocolMetadata(const nodegraph::Value *value,
                         qsizetype maximumCharacters = 240) {
  const std::string raw = graphString(value);
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
    QColor color(copied_ ? QString::fromLatin1(UiStyle::greenText)
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
    if (QLayout *child = item->layout()) {
      clearLayout(child);
      delete child;
      continue;
    }
    if (QWidget *widget = item->widget())
      delete widget;
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

QFrame *InspectorPane::planStepFrame(const ui::InspectorPlanStep &step) {
  auto *frame = new QFrame;
  patchPlanStepFrame(frame, step);
  setProperty("planRowConstructions",
              property("planRowConstructions").toULongLong() + 1);
  return frame;
}

void InspectorPane::patchPlanStepFrame(QFrame *frame,
                                       const ui::InspectorPlanStep &step) {
  frame->setObjectName(QStringLiteral("inspectorPlanStepFrame"));
  frame->setProperty("planStep", text(step.step));
  frame->setProperty("kind", "raised");
  auto *layout = qobject_cast<QVBoxLayout *>(frame->layout());
  if (layout)
    clearLayout(layout);
  else
    layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(6);
  layout->addWidget(makeLabel(text(step.step)));
  layout->addWidget(statusLabel(step.status));
  setProperty("planRowPatches",
              property("planRowPatches").toULongLong() + 1);
}

QFrame *InspectorPane::agentFrame(const ui::InspectorAgentRow &agent) {
  auto *frame = new QFrame;
  patchAgentFrame(frame, agent);
  setProperty("agentRowConstructions",
              property("agentRowConstructions").toULongLong() + 1);
  return frame;
}

void InspectorPane::patchAgentFrame(QFrame *frame,
                                    const ui::InspectorAgentRow &agent) {
  if (!frame)
    return;
  frame->setObjectName(QStringLiteral("inspectorAgentFrame"));
  frame->setProperty("logicalAgentId", text(agent.id));
  frame->setProperty("kind", "raised");
  frame->setMinimumWidth(0);
  auto *layout = qobject_cast<QVBoxLayout *>(frame->layout());
  if (layout)
    clearLayout(layout);
  else
    layout = new QVBoxLayout(frame);
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

  const std::string expansionKey = agentsSnapshot->threadId + '\n' + agent.id;
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
  setProperty("agentRowPatches",
              property("agentRowPatches").toULongLong() + 1);
}

QFrame *InspectorPane::requestFrame(
    const ui::InspectorRequestRow &request) {
  auto *frame = new QFrame;
  patchRequestFrame(frame, request);
  setProperty("requestRowConstructions",
              property("requestRowConstructions").toULongLong() + 1);
  return frame;
}

void InspectorPane::patchRequestFrame(
    QFrame *frame, const ui::InspectorRequestRow &request) {
  frame->setObjectName(QStringLiteral("inspectorRequestFrame"));
  frame->setProperty("requestId", text(request.id));
  frame->setProperty("kind", "raised");
  frame->setProperty("tone", "warning");
  auto *layout = qobject_cast<QVBoxLayout *>(frame->layout());
  if (layout)
    clearLayout(layout);
  else
    layout = new QVBoxLayout(frame);
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
  connect(reject, &QPushButton::clicked, frame, [this, id = request.id] {
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
    connect(accept, &QPushButton::clicked, frame, [this, id = request.id] {
      if (acceptRequest)
        acceptRequest(id);
    });
    actions->addWidget(accept);
  } else {
    auto *review = new QPushButton(QStringLiteral("Review"));
    review->setProperty("kind", "request");
    review->setFixedHeight(28);
    review->setEnabled(request.actionable);
    connect(review, &QPushButton::clicked, frame, [this, id = request.id] {
      if (reviewRequest)
        reviewRequest(id);
    });
    actions->addWidget(review);
  }
  layout->addLayout(actions);
  setProperty("requestRowPatches",
              property("requestRowPatches").toULongLong() + 1);
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
  planLayout->addStretch();
  agentsContent = new QWidget;
  agentsLayout = new QVBoxLayout(agentsContent);
  agentsLayout->setContentsMargins(12, 12, 12, 12);
  agentsLayout->setSpacing(8);
  agentsLayout->addStretch();
  requestsContent = new QWidget;
  requestsLayout = new QVBoxLayout(requestsContent);
  requestsLayout->setContentsMargins(12, 12, 12, 12);
  requestsLayout->setSpacing(8);
  requestsLayout->addStretch();
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
  });
  connect(protocolChoice, &QPushButton::clicked, this, [this] {
    infoStack->setCurrentIndex(ProtocolPage);
    showProtocolTail();
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

  const auto requestCurrentPage = [this](int) {
    if (refreshRequested)
      refreshRequested();
    else
      refreshCurrentTab();
  };
  connect(inspectorTabs, &QTabWidget::currentChanged, this,
          requestCurrentPage);
  connect(infoStack, &QStackedWidget::currentChanged, this,
          requestCurrentPage);
}

void InspectorPane::setHideAction(std::function<void()> hide) {
  hideAction = std::move(hide);
}

void InspectorPane::setRefreshRequestedAction(
    std::function<void()> refresh) {
  refreshRequested = std::move(refresh);
}

void InspectorPane::setRequestActions(RequestAction review,
                                      RequestAction accept,
                                      RequestAction reject) {
  reviewRequest = std::move(review);
  acceptRequest = std::move(accept);
  rejectRequest = std::move(reject);
}

void InspectorPane::refresh(const ui::InspectorSnapshot &snapshot) {
  currentSnapshot = snapshot;
  if (isVisible())
    refreshCurrentTab();
}

void InspectorPane::refresh(const ui::InspectorSnapshot &snapshot,
                            ui::InspectorProjection projection) {
  if (projection == ui::InspectorProjection::All || !currentSnapshot) {
    currentSnapshot = snapshot;
  } else {
    switch (projection) {
    case ui::InspectorProjection::Plan:
      currentSnapshot->plan = snapshot.plan;
      break;
    case ui::InspectorProjection::Agents:
      currentSnapshot->agents = snapshot.agents;
      break;
    case ui::InspectorProjection::Changes:
      currentSnapshot->changes = snapshot.changes;
      break;
    case ui::InspectorProjection::Requests:
      currentSnapshot->requests = snapshot.requests;
      break;
    case ui::InspectorProjection::State:
      currentSnapshot->state = snapshot.state;
      break;
    case ui::InspectorProjection::All:
      break;
    }
  }
  if (isVisible())
    refreshCurrentTab();
}

void InspectorPane::showEvent(QShowEvent *event) {
  QFrame::showEvent(event);
  if (refreshRequested)
    refreshRequested();
  else
    refreshCurrentTab();
}

void InspectorPane::refreshCurrentTab() {
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

void InspectorPane::refreshPlan() {
  const ui::InspectorPlanSnapshot &next = currentSnapshot->plan;
  if (planSnapshot && *planSnapshot == next)
    return;
  const std::optional<ui::InspectorPlanSnapshot> previous = planSnapshot;
  planSnapshot = next;
  const ui::InspectorPlanSnapshot &snapshot = *planSnapshot;
  const std::string beforeExplanation =
      previous && previous->plan ? previous->plan->explanation : std::string{};
  const std::string nextExplanation =
      snapshot.plan ? snapshot.plan->explanation : std::string{};
  if (beforeExplanation != nextExplanation ||
      static_cast<bool>(planExplanation) != !nextExplanation.empty()) {
    if (planExplanation) {
      planLayout->removeWidget(planExplanation);
      delete planExplanation;
      planExplanation = nullptr;
    }
    if (!nextExplanation.empty()) {
      planExplanation = makeMarkdownLabel(text(nextExplanation));
      planLayout->insertWidget(0, planExplanation);
    }
  }

  std::vector<std::pair<std::string, const ui::InspectorPlanStep *>> rows;
  if (snapshot.plan) {
    std::unordered_map<std::string, std::size_t> occurrences;
    rows.reserve(snapshot.plan->steps.size());
    for (const ui::InspectorPlanStep &step : snapshot.plan->steps) {
      const std::size_t occurrence = occurrences[step.step]++;
      rows.emplace_back(step.step + '\n' + std::to_string(occurrence), &step);
    }
  }
  std::unordered_set<std::string> desired;
  for (const auto &[key, step] : rows) {
    static_cast<void>(step);
    desired.insert(key);
  }
  for (auto iterator = planFrames.begin(); iterator != planFrames.end();) {
    if (desired.contains(iterator->first)) {
      ++iterator;
      continue;
    }
    planLayout->removeWidget(iterator->second);
    delete iterator->second;
    renderedPlanSteps.erase(iterator->first);
    iterator = planFrames.erase(iterator);
  }

  if (planMessage) {
    planLayout->removeWidget(planMessage);
    delete planMessage;
    planMessage = nullptr;
  }
  if (!snapshot.plan) {
    if (!snapshot.threadPresent) {
      planMessage = makeLabel(QStringLiteral("No selected thread."), "muted");
    } else if (snapshot.planItem) {
      const QString value = text(*snapshot.planItem);
      planMessage = value.isEmpty()
                        ? static_cast<QWidget *>(makeLabel(
                              QStringLiteral("Plan is being prepared."),
                              "muted"))
                        : static_cast<QWidget *>(makeMarkdownLabel(value));
    } else {
      planMessage =
          makeLabel(QStringLiteral("No plan for this thread."), "muted");
    }
    planLayout->insertWidget(0, planMessage);
  }

  const int firstRow = planExplanation ? 1 : 0;
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto &[key, step] = rows[index];
    QFrame *frame = nullptr;
    const auto retained = planFrames.find(key);
    if (retained == planFrames.end()) {
      frame = planStepFrame(*step);
      planFrames.emplace(key, frame);
    } else {
      frame = retained->second;
      const auto rendered = renderedPlanSteps.find(key);
      if (rendered == renderedPlanSteps.end() || rendered->second != *step)
        patchPlanStepFrame(frame, *step);
    }
    renderedPlanSteps.insert_or_assign(key, *step);
    const int position = firstRow + static_cast<int>(index);
    if (planLayout->indexOf(frame) != position)
      planLayout->insertWidget(position, frame);
  }
  setProperty("planTabCommits",
              property("planTabCommits").toULongLong() + 1);
}

void InspectorPane::refreshAgents() {
  const ui::InspectorAgentsSnapshot &next = currentSnapshot->agents;
  if (agentsSnapshot && *agentsSnapshot == next)
    return;
  const bool changedThread =
      agentsSnapshot && agentsSnapshot->threadId != next.threadId;
  agentsSnapshot = next;
  const ui::InspectorAgentsSnapshot &snapshot = *agentsSnapshot;
  if (changedThread) {
    for (auto &[id, frame] : agentFrames) {
      static_cast<void>(id);
      agentsLayout->removeWidget(frame);
      delete frame;
    }
    agentFrames.clear();
    renderedAgentRows.clear();
  }

  std::unordered_set<std::string> desired;
  desired.reserve(snapshot.agents.size());
  for (const ui::InspectorAgentRow &agent : snapshot.agents)
    desired.insert(agent.id);
  for (auto iterator = agentFrames.begin(); iterator != agentFrames.end();) {
    if (desired.contains(iterator->first)) {
      ++iterator;
      continue;
    }
    agentsLayout->removeWidget(iterator->second);
    delete iterator->second;
    renderedAgentRows.erase(iterator->first);
    iterator = agentFrames.erase(iterator);
    setProperty("agentRowRemovals",
                property("agentRowRemovals").toULongLong() + 1);
  }

  if (agentsMessage) {
    agentsLayout->removeWidget(agentsMessage);
    delete agentsMessage;
    agentsMessage = nullptr;
  }
  if (!snapshot.threadPresent) {
    agentsMessage = makeLabel(QStringLiteral("No selected thread."), "muted");
  } else if (snapshot.agents.empty()) {
    agentsMessage = makeLabel(
        QStringLiteral("No agent activity for this thread."), "muted");
  }
  if (agentsMessage)
    agentsLayout->insertWidget(0, agentsMessage);

  for (std::size_t index = 0; index < snapshot.agents.size(); ++index) {
    const ui::InspectorAgentRow &agent = snapshot.agents[index];
    QFrame *frame = nullptr;
    const auto retained = agentFrames.find(agent.id);
    if (retained == agentFrames.end()) {
      frame = agentFrame(agent);
      agentFrames.emplace(agent.id, frame);
    } else {
      frame = retained->second;
      const auto rendered = renderedAgentRows.find(agent.id);
      if (rendered == renderedAgentRows.end() || rendered->second != agent)
        patchAgentFrame(frame, agent);
    }
    renderedAgentRows.insert_or_assign(agent.id, agent);
    if (agentsLayout->indexOf(frame) != static_cast<int>(index))
      agentsLayout->insertWidget(static_cast<int>(index), frame);
  }
  setProperty("agentsTabCommits",
              property("agentsTabCommits").toULongLong() + 1);
}

void InspectorPane::refreshChanges() {
  const ui::InspectorChangesSnapshot &snapshot = currentSnapshot->changes;
  diffViewer->setRepositoryContext(
      text(snapshot.threadId), text(snapshot.cwd), texts(snapshot.commandCwds),
      texts(snapshot.changedPaths));
  diffViewer->refreshRepository();
}

void InspectorPane::refreshRequests() {
  const ui::InspectorRequestsSnapshot &next = currentSnapshot->requests;
  if (requestsSnapshot && *requestsSnapshot == next)
    return;
  requestsSnapshot = next;
  const std::vector<ui::InspectorRequestRow> &snapshot =
      requestsSnapshot->requests;
  std::unordered_set<std::string> desired;
  for (const ui::InspectorRequestRow &request : snapshot)
    desired.insert(request.id);
  for (auto iterator = requestFrames.begin(); iterator != requestFrames.end();) {
    if (desired.contains(iterator->first)) {
      ++iterator;
      continue;
    }
    requestsLayout->removeWidget(iterator->second);
    delete iterator->second;
    renderedRequests.erase(iterator->first);
    iterator = requestFrames.erase(iterator);
  }
  if (requestsMessage) {
    requestsLayout->removeWidget(requestsMessage);
    delete requestsMessage;
    requestsMessage = nullptr;
  }
  if (snapshot.empty()) {
    requestsMessage =
        makeLabel(QStringLiteral("No pending requests."), "muted");
    requestsLayout->insertWidget(0, requestsMessage);
  }
  for (std::size_t index = 0; index < snapshot.size(); ++index) {
    const ui::InspectorRequestRow &request = snapshot[index];
    QFrame *frame = nullptr;
    const auto retained = requestFrames.find(request.id);
    if (retained == requestFrames.end()) {
      frame = requestFrame(request);
      requestFrames.emplace(request.id, frame);
    } else {
      frame = retained->second;
      const auto rendered = renderedRequests.find(request.id);
      if (rendered == renderedRequests.end() || rendered->second != request)
        patchRequestFrame(frame, request);
    }
    renderedRequests.insert_or_assign(request.id, request);
    if (requestsLayout->indexOf(frame) != static_cast<int>(index))
      requestsLayout->insertWidget(static_cast<int>(index), frame);
  }
  setProperty("requestsTabCommits",
              property("requestsTabCommits").toULongLong() + 1);
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
          .arg(static_cast<qulonglong>(
              std::max(snapshot.telemetryCount, protocolTelemetryCount)));
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
  nodegraph::UiEffect effect;
  effect.kind = nodegraph::UiEffectKind::ProtocolDiagnostic;
  const auto copyUnsigned = [&frame, &effect](const char *from,
                                              const char *to) {
    const auto found = frame.find(from);
    if (found != frame.end() && found->is_number_unsigned())
      effect.details.emplace(to, nodegraph::Value(found->get<std::uint64_t>()));
  };
  const auto copyString = [&frame, &effect](const char *from,
                                            const char *to) {
    const auto found = frame.find(from);
    if (found != frame.end() && found->is_string())
      effect.details.emplace(to, nodegraph::Value(found->get<std::string>()));
  };
  copyUnsigned("sequence", "sequence");
  copyUnsigned("generation", "connectionGeneration");
  copyString("kind", "direction");
  copyString(frame.value("kind", std::string{}) == "result" ? "action"
                                                              : "type",
             "subject");
  copyString("authority", "authority");
  copyString("correlationId", "correlation");
  const auto scope = frame.find("scope");
  if (scope != frame.end() && scope->is_object()) {
    for (const char *key :
         {"threadId", "turnId", "itemId", "requestId", "processId"}) {
      const auto found = scope->find(key);
      if (found != scope->end() && found->is_string())
        effect.details.emplace(key,
                               nodegraph::Value(found->get<std::string>()));
    }
  }
  if (frame.value("kind", std::string{}) == "result") {
    effect.details.emplace("outcome",
                           nodegraph::Value(frame.value("ok", false)
                                                ? "ok"
                                                : "ERROR"));
    const auto error = frame.find("error");
    if (error != frame.end() && error->is_object()) {
      const auto message = error->find("message");
      if (message != error->end() && message->is_string())
        effect.details.emplace(
            "error", nodegraph::Value(message->get<std::string>()));
    }
  }
  appendProtocolDiagnostic(effect);
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
    protocolLines.push_back(line);
    recorded.push_back(std::move(line));
  };
  const std::optional<std::uint64_t> sequence =
      graphUnsigned(graphField(effect.details, "sequence"));
  if (sequence && *sequence != 0) {
    if (observedSequence != 0 && *sequence != observedSequence + 1) {
      record(QStringLiteral("[%1]  %2  expected=%3  received=%4")
                 .arg(timestamp, *sequence <= observedSequence
                                     ? QStringLiteral("NON-MONOTONIC")
                                     : QStringLiteral("SEQUENCE GAP"))
                 .arg(observedSequence + 1)
                 .arg(*sequence));
    }
    observedSequence = std::max(observedSequence, *sequence);
  }
  if (const auto dropped =
          graphUnsigned(graphField(effect.details, "droppedBefore"));
      dropped && *dropped != 0)
    record(QStringLiteral("[%1]  DROPPED %2 DIAGNOSTICS before #%3")
               .arg(timestamp)
               .arg(*dropped)
               .arg(sequence.value_or(0)));

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
  for (std::string_view key :
       {"authority", "outcome", "threadId", "turnId", "itemId",
        "requestId", "processId", "connectionId", "targetId", "role",
        "state", "event", "correlation", "error", "errorCategory",
        "errorCode"}) {
    const QString value = protocolMetadata(graphField(effect.details, key));
    if (!value.isEmpty())
      parts << QStringLiteral("%1=%2").arg(text(key), value);
  }
  record(parts.join(QStringLiteral("  ")));

  const std::string direction =
      graphString(graphField(effect.details, "direction"));
  const std::string authority =
      graphString(graphField(effect.details, "authority"));
  if (authority == "none" &&
      (direction.find("notification") != std::string::npos ||
       direction.find("event") != std::string::npos ||
       direction.ends_with("frame")))
    protocolTelemetryCount = std::min<std::size_t>(
        256, protocolTelemetryCount + 1);

  const bool visibleProtocol =
      isVisible() && inspectorTabs->currentIndex() == 4 &&
      infoStack->currentIndex() == ProtocolPage;
  if (visibleProtocol) {
    const ScrollPosition position{protocolFollowsTail,
                                  protocolPausedScrollValue};
    mutatingProtocolLog = true;
    for (const QString &line : recorded)
      protocolLog->appendPlainText(line);
    restoreProtocolScroll(position.followsTail, position.value);
    if (currentSnapshot)
      refreshProtocolStats();
  }
}

} // namespace codexui::codex::middle
