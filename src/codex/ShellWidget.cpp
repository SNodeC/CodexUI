// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ShellWidget.h"

#include "codex/ConnectionDialog.h"
#include "codex/DiffViewer.h"
#include "codex/FileSelectionDialog.h"
#include "codex/FrontendSession.h"
#include "codex/NewThreadDialog.h"
#include "codex/PendingRequestDialog.h"
#include "codex/TurnSettingsWidget.h"
#include "codex/ui/BrandMark.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <optional>

namespace codexui::codex {
namespace {

constexpr int UpcomingControlHeight = 32;
constexpr int MaximumCommandOutputHeight = 220;

using CommandOutputScrollState = std::pair<bool, int>;

QLabel *makeLabel(QString value, const char *kind = "body");

class PendingPromptCard final : public QFrame {
public:
  PendingPromptCard(const QString &prompt, int attachmentCount, bool awaiting,
                    bool failed, const QString &error) {
    setObjectName(QStringLiteral("pendingPromptCard"));
    setStyleSheet(QStringLiteral(
        "QFrame#pendingPromptCard{background:transparent;border:0;}"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);

    const QString foreground = awaiting ? QStringLiteral("#536b8f")
                               : failed ? QStringLiteral("#9b2c2c")
                                        : QStringLiteral("#1d2633");
    auto *title = makeLabel(QStringLiteral("You"), "title");
    title->setStyleSheet(
        QStringLiteral("background:transparent;color:%1;").arg(foreground));
    layout->addWidget(title);
    auto *body = makeLabel(prompt);
    body->setStyleSheet(
        QStringLiteral("background:transparent;color:%1;").arg(foreground));
    layout->addWidget(body);

    QString status;
    if (awaiting)
      status = QStringLiteral("Waiting for app-server acknowledgment");
    else if (failed)
      status = error.isEmpty() ? QStringLiteral("Not sent")
                               : QStringLiteral("Not sent: %1").arg(error);
    if (attachmentCount > 0) {
      const QString attachments =
          QStringLiteral("%1 attachment%2")
              .arg(attachmentCount)
              .arg(attachmentCount == 1 ? QString{} : QStringLiteral("s"));
      status = status.isEmpty()
                   ? attachments
                   : status + QStringLiteral("  |  ") + attachments;
    }
    if (!status.isEmpty()) {
      auto *metadata = makeLabel(status, "meta");
      metadata->setStyleSheet(
          QStringLiteral("background:transparent;color:%1;").arg(foreground));
      layout->addWidget(metadata);
    }

    if (awaiting) {
      animationTimer.setInterval(32);
      connect(&animationTimer, &QTimer::timeout, this,
              qOverload<>(&PendingPromptCard::update));
      animationTimer.start();
    }
    isAwaiting = awaiting;
    hasFailed = failed;
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    QFrame::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bounds = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    const QColor background = isAwaiting  ? QColor(QStringLiteral("#dbe7f8"))
                              : hasFailed ? QColor(QStringLiteral("#fff1f1"))
                                          : QColor(QStringLiteral("#eaf2ff"));
    const QColor border = isAwaiting  ? QColor(QStringLiteral("#9eb9df"))
                          : hasFailed ? QColor(QStringLiteral("#e5a3a3"))
                                      : QColor(QStringLiteral("#bfd3f9"));
    painter.setBrush(background);
    painter.setPen(QPen(border, 1.0));
    painter.drawRoundedRect(bounds, 8.0, 8.0);

    if (!isAwaiting)
      return;
    constexpr qreal HalfSweepWidth = 0.24;
    constexpr qint64 HalfCycleMilliseconds = 850;
    const qint64 phase =
        QDateTime::currentMSecsSinceEpoch() % (2 * HalfCycleMilliseconds);
    const qreal progress =
        phase <= HalfCycleMilliseconds
            ? qreal(phase) / HalfCycleMilliseconds
            : qreal(2 * HalfCycleMilliseconds - phase) / HalfCycleMilliseconds;
    const qreal center = bounds.left() + progress * bounds.width();
    const qreal radius = std::max(28.0, bounds.width() * HalfSweepWidth);
    QLinearGradient sweep(center - radius, 0.0, center + radius, 0.0);
    sweep.setColorAt(0.0, QColor(47, 111, 235, 0));
    sweep.setColorAt(0.5, QColor(117, 160, 239, 105));
    sweep.setColorAt(1.0, QColor(47, 111, 235, 0));
    QPainterPath clip;
    clip.addRoundedRect(bounds, 8.0, 8.0);
    painter.save();
    painter.setClipPath(clip);
    painter.fillRect(bounds, sweep);
    painter.restore();

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(QStringLiteral("#79a0d7")), 1.5));
    painter.drawRoundedRect(bounds, 8.0, 8.0);
  }

private:
  QTimer animationTimer;
  bool isAwaiting = false;
  bool hasFailed = false;
};

class CommandOutputView final : public QPlainTextEdit {
public:
  explicit CommandOutputView(
      const QString &output,
      std::optional<CommandOutputScrollState> restoredState = std::nullopt)
      : followsLatest(restoredState ? restoredState->first : true),
        preservedScrollValue(restoredState ? restoredState->second : 0) {
    setReadOnly(true);
    setMinimumHeight(0);
    setMaximumHeight(MaximumCommandOutputHeight);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setProperty("kind", "code");
    setStyleSheet(QStringLiteral(
        "background:#111827;color:#e5e7eb;border-radius:6px;padding:7px;"
        "font-family:monospace;"));
    setPlainText(output);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) {
              if (adjustingScroll)
                return;
              preservedScrollValue = value;
              followsLatest = value >= verticalScrollBar()->maximum() - 1;
            });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int) { scheduleScrollSettlement(); });
    connect(document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this](const QSizeF &) { remeasure(); });
    // Establish a content-derived height before the card enters the
    // conversation layout. Otherwise it first appears at zero height and then
    // changes the outer scroll range in a visibly separate pass.
    remeasure();
    QTimer::singleShot(0, this, [this] { settleScroll(); });
  }

  [[nodiscard]] CommandOutputScrollState scrollState() const {
    return {followsLatest, verticalScrollBar()->value()};
  }

  QSize sizeHint() const override {
    QSize result = QPlainTextEdit::sizeHint();
    result.setHeight(preferredHeight);
    return result;
  }

  QSize minimumSizeHint() const override {
    QSize result = QPlainTextEdit::minimumSizeHint();
    result.setHeight(0);
    return result;
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QPlainTextEdit::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { remeasure(); });
  }

private:
  void remeasure() {
    const int contentHeight = static_cast<int>(
        std::ceil(document()->documentLayout()->documentSize().height()));
    const int wantedHeight = std::clamp(contentHeight + 2 * frameWidth() + 14,
                                        0, MaximumCommandOutputHeight);
    if (wantedHeight != preferredHeight) {
      preferredHeight = wantedHeight;
      updateGeometry();
    }
    scheduleScrollSettlement();
  }

  void scheduleScrollSettlement() {
    if (scrollSettlementPending)
      return;
    scrollSettlementPending = true;
    QTimer::singleShot(0, this, [this] {
      scrollSettlementPending = false;
      settleScroll();
    });
  }

  void settleScroll() {
    adjustingScroll = true;
    QScrollBar *scrollBar = verticalScrollBar();
    scrollBar->setValue(
        followsLatest ? scrollBar->maximum()
                      : std::min(preservedScrollValue, scrollBar->maximum()));
    adjustingScroll = false;
  }

  bool followsLatest = true;
  bool adjustingScroll = false;
  bool scrollSettlementPending = false;
  int preservedScrollValue = 0;
  int preferredHeight = 0;
};

std::optional<CommandOutputScrollState>
commandOutputScrollState(QWidget *card) {
  if (!card)
    return std::nullopt;
  for (QPlainTextEdit *editor : card->findChildren<QPlainTextEdit *>()) {
    if (auto *output = dynamic_cast<CommandOutputView *>(editor))
      return output->scrollState();
  }
  return std::nullopt;
}

class BottomOverlayDock final : public QWidget {
public:
  BottomOverlayDock(QWidget *anchor, std::function<void(int)> heightChanged)
      : QWidget(anchor), anchor(anchor),
        heightChanged(std::move(heightChanged)) {
    anchor->installEventFilter(this);
  }

  void synchronizeGeometry() {
    if (!layout())
      return;
    layout()->activate();
    constexpr int HorizontalInset = 24;
    constexpr int BottomInset = 12;
    const int availableHeight = std::max(0, anchor->height() - BottomInset);
    const int wantedHeight = std::min(sizeHint().height(), availableHeight);
    if (wantedHeight != reportedHeight) {
      reportedHeight = wantedHeight;
      if (heightChanged)
        heightChanged(wantedHeight);
    }
    setGeometry(HorizontalInset, availableHeight - wantedHeight,
                std::max(0, anchor->width() - 2 * HorizontalInset),
                wantedHeight);
    raise();
  }

protected:
  bool event(QEvent *event) override {
    const bool accepted = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show)
      scheduleSynchronization();
    return accepted;
  }

  bool eventFilter(QObject *watched, QEvent *event) override {
    if (watched == anchor &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
         event->type() == QEvent::LayoutRequest))
      scheduleSynchronization();
    return QWidget::eventFilter(watched, event);
  }

private:
  void scheduleSynchronization() {
    if (synchronizationPending)
      return;
    synchronizationPending = true;
    QTimer::singleShot(0, this, [this] {
      synchronizationPending = false;
      synchronizeGeometry();
    });
  }

  QWidget *anchor = nullptr;
  std::function<void(int)> heightChanged;
  int reportedHeight = -1;
  bool synchronizationPending = false;
};

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

QLabel *makeLabel(QString value, const char *kind) {
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

QFrame *makeDivider() {
  auto *divider = new QFrame;
  divider->setFixedHeight(1);
  divider->setStyleSheet(QStringLiteral("background:#d7dee8;"));
  return divider;
}

QFrame *makeStatusDot() {
  auto *dot = new QFrame;
  dot->setFixedSize(8, 8);
  dot->setStyleSheet(QStringLiteral("background:#98a2b3;border-radius:4px;"));
  return dot;
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

std::string safeMessage(const nlohmann::json &value) {
  std::string message = stringValue(value, "message");
  if (message.empty())
    message = stringValue(value, "detail");
  if (!message.empty())
    return message;
  const auto error = value.find("error");
  return error != value.end() && error->is_object()
             ? stringValue(*error, "message")
             : std::string{};
}

QFrame *itemFrame(
    const ItemPresentation &presentation,
    std::optional<CommandOutputScrollState> outputScrollState = std::nullopt) {
  const nlohmann::json &item = presentation.raw;
  const std::string typeName = stringValue(item, "type");
  auto *frame = new QFrame;
  frame->setProperty("kind", "raised");
  if (typeName == "userMessage")
    frame->setProperty("messageRole", "user");
  else if (typeName == "agentMessage")
    frame->setProperty("messageRole", "agent");
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
  if (!body.isEmpty()) {
    layout->addWidget(typeName == "agentMessage" || typeName == "plan"
                          ? makeMarkdownLabel(body)
                          : makeLabel(body));
  }

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
      commandView->setStyleSheet(QStringLiteral(
          "background:#f8fafc;border:1px solid #d7dee8;border-radius:6px;"
          "padding:7px;font-family:monospace;"));
      layout->addWidget(commandView);
    }
    const QString output = text(stringValue(item, "aggregatedOutput"));
    if (!output.trimmed().isEmpty()) {
      layout->addWidget(new CommandOutputView(output, outputScrollState));
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
  } else if (typeName == "collabAgentToolCall" ||
             typeName == "subAgentActivity") {
    QStringList metadata;
    const QString tool = text(stringValue(item, "tool"));
    if (!tool.isEmpty())
      metadata << tool;
    std::string status = stringValue(item, "status");
    if (status.empty())
      status = stringValue(item, "kind");
    metadata << displayStatus(status);
    const QString receivers =
        joinedStrings(item.value("receiverThreadIds", nlohmann::json::array()));
    if (!receivers.isEmpty())
      metadata << receivers;
    layout->addWidget(
        makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));
    const QString prompt = text(stringValue(item, "prompt"));
    if (!prompt.isEmpty())
      layout->addWidget(makeLabel(prompt));
    const QString result = text(stringValue(item, "resultText"));
    if (!result.isEmpty())
      layout->addWidget(makeMarkdownLabel(result));
  } else if (typeName == "reasoning") {
    const QString summaries =
        joinedStrings(item.value("summary", nlohmann::json::array()));
    if (!summaries.isEmpty())
      layout->addWidget(makeMarkdownLabel(summaries));
  } else if (typeName == "fileChange") {
    QStringList metadata;
    metadata << displayStatus(stringValue(item, "status"));
    const nlohmann::json changes =
        item.value("changes", nlohmann::json::array());
    if (changes.is_array())
      metadata << QStringLiteral("%1 paths")
                      .arg(static_cast<qulonglong>(changes.size()));
    layout->addWidget(
        makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));
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
    layout->addWidget(makeMarkdownLabel(result));

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

ShellWidget::ShellWidget(FrontendSession &session, QWidget *parent)
    : QWidget(parent), session(session) {
  setObjectName(QStringLiteral("workbench"));
  auto *root = new QVBoxLayout(this);
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
  requestButton = new QPushButton;
  requestButton->setProperty("kind", "request");
  requestButton->setFixedHeight(32);
  requestButton->hide();
  connect(requestButton, &QPushButton::clicked, this, [this] {
    inspector->show();
    restoreInspectorButton->hide();
    inspectorTabs->setCurrentIndex(3);
  });
  topLayout->addStretch();
  connectionStatusDot = makeStatusDot();
  connectionStatusDot->setToolTip(QStringLiteral("Not connected"));
  connectionButton = new QToolButton;
  connectionButton->setText(QStringLiteral("Connection"));
  connectionButton->setProperty("kind", "subtle");
  connectionButton->setPopupMode(QToolButton::InstantPopup);
  connectionButton->setFixedHeight(32);
  auto *connectionMenu = new QMenu(connectionButton);
  connectionMenu->addAction(QStringLiteral("Configure..."), this, [this] {
    if (!model.connection().settings.is_object() ||
        model.connection().settings.empty()) {
      showNotice(QStringLiteral("Connection settings are not available yet."));
      return;
    }
    ConnectionDialog dialog(model.connection().settings, this);
    if (dialog.exec() != QDialog::Accepted)
      return;
    this->session.configureConnection(
        dialog.selection(), [this](const nlohmann::json &result) {
          if (result.value("ok", false))
            return;
          const std::string message =
              safeMessage(result.value("error", nlohmann::json::object()));
          showNotice(text(message.empty()
                              ? std::string("Connection configuration failed")
                              : message));
        });
  });
  connectionMenu->addSeparator();
  connectAction =
      connectionMenu->addAction(QStringLiteral("Connect"), this,
                                [this] { this->session.connectTransport(); });
  disconnectAction =
      connectionMenu->addAction(QStringLiteral("Disconnect"), this, [this] {
        this->session.disconnectTransport();
      });
  reconnectAction = connectionMenu->addAction(
      QStringLiteral("Reconnect"), this, [this] { this->session.reconnect(); });
  connectionButton->setMenu(connectionMenu);
  controllerButton = new QPushButton(QStringLiteral("Claim control"));
  controllerButton->setFixedHeight(32);
  restoreInspectorButton = new QPushButton(QStringLiteral("Show inspector"));
  restoreInspectorButton->setProperty("kind", "subtle");
  restoreInspectorButton->setFixedHeight(32);
  restoreInspectorButton->hide();
  connect(controllerButton, &QPushButton::clicked, this, [this] {
    if (model.connection().role == "controller")
      this->session.releaseController();
    else
      this->session.claimController();
  });
  topLayout->addWidget(restoreInspectorButton);
  topLayout->addWidget(requestButton);
  topLayout->addWidget(controllerButton);
  auto *connectionControl = new QWidget;
  auto *connectionLayout = new QHBoxLayout(connectionControl);
  connectionLayout->setContentsMargins(0, 0, 0, 0);
  connectionLayout->setSpacing(6);
  connectionLayout->addWidget(connectionButton);
  connectionLayout->addWidget(connectionStatusDot);
  topLayout->addWidget(connectionControl);
  root->addWidget(top);

  splitter = new QSplitter(Qt::Horizontal);
  splitter->setChildrenCollapsible(false);
  splitter->setHandleWidth(8);

  sidebar = new QFrame;
  sidebar->setObjectName(QStringLiteral("sidebar"));
  sidebar->setStyleSheet(QStringLiteral("QFrame#sidebar{background:#f8fafc;}"));
  sidebar->setMinimumWidth(220);
  sidebar->setMaximumWidth(440);
  auto *sidebarLayout = new QVBoxLayout(sidebar);
  sidebarLayout->setContentsMargins(10, 14, 10, 17);
  sidebarLayout->setSpacing(0);
  auto *sidebarHeader = new QHBoxLayout;
  sidebarHeader->setContentsMargins(8, 0, 6, 8);
  sidebarHeader->addWidget(makeLabel(QStringLiteral("WORK"), "section"));
  sidebarHeader->addStretch();
  auto *hideSidebarButton = new QPushButton(QStringLiteral("Hide"));
  hideSidebarButton->setProperty("kind", "subtle");
  hideSidebarButton->setFixedSize(52, 24);
  sidebarHeader->addWidget(hideSidebarButton);
  sidebarLayout->addLayout(sidebarHeader);

  auto *newButton = new QPushButton(QStringLiteral("+  New thread"));
  newButton->setFixedHeight(36);
  newButton->setStyleSheet(QStringLiteral(
      "QPushButton{background:#ffffff;color:#2f6feb;border:1px solid #bfd3f9;"
      "border-radius:8px;text-align:left;padding-left:14px;font-weight:600;}"
      "QPushButton:hover{background:#e5eeff;border-color:#2f6feb;}"
      "QPushButton:disabled{background:#f6f8fb;color:#98a2b3;"
      "border-color:#d7dee8;}"));
  sidebarLayout->addWidget(newButton);
  sidebarLayout->addSpacing(8);

  auto *threadToolbar = new QHBoxLayout;
  threadToolbar->setContentsMargins(4, 0, 4, 6);
  auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
  refreshButton->setProperty("kind", "subtle");
  refreshButton->setFixedHeight(28);
  threadToolbar->addWidget(refreshButton);
  threadToolbar->addStretch();
  sidebarLayout->addLayout(threadToolbar);
  threadList = new QListWidget;
  threadList->setObjectName(QStringLiteral("threadList"));
  threadList->setSelectionMode(QAbstractItemView::SingleSelection);
  threadList->setContextMenuPolicy(Qt::CustomContextMenu);
  threadList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  threadList->setTextElideMode(Qt::ElideRight);
  threadList->setStyleSheet(QStringLiteral(
      "QListWidget#threadList{background:transparent;border:0;outline:0;}"
      "QListWidget#threadList::item{min-height:30px;border:0;border-radius:5px;"
      "padding:2px 8px;color:#344054;}"
      "QListWidget#threadList::item:hover{background:#eef3fa;}"
      "QListWidget#threadList::item:selected{background:#e5eeff;"
      "color:#1d2633;font-weight:600;}"));
  sidebarLayout->addWidget(threadList);
  connect(refreshButton, &QPushButton::clicked, this,
          [this] { requestThreads(); });
  connect(newButton, &QPushButton::clicked, this, [this] { beginNewThread(); });
  connect(hideSidebarButton, &QPushButton::clicked, this, [this] {
    sidebar->hide();
    restoreSidebarButton->show();
  });
  connect(restoreSidebarButton, &QPushButton::clicked, this, [this] {
    sidebar->show();
    restoreSidebarButton->hide();
  });
  connect(threadList, &QListWidget::itemSelectionChanged, this, [this] {
    const std::string threadId = visiblySelectedThreadId();
    if (!threadId.empty() && threadId != selectedThreadId)
      selectThread(threadId);
  });
  connect(threadList, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint &position) {
            QListWidgetItem *item = threadList->itemAt(position);
            if (!item)
              return;
            const std::string threadId =
                item->data(Qt::UserRole).toString().toStdString();
            const ThreadPresentation *thread = model.thread(threadId);
            if (!thread)
              return;
            QMenu menu(threadList);
            menu.addAction(QStringLiteral("Reload"), this,
                           [this, threadId] { readThread(threadId); });
            const bool canControl = model.connection().connected &&
                                    model.connection().role == "controller";
            QAction *rename =
                menu.addAction(QStringLiteral("Rename"), this,
                               [this, threadId] { renameThread(threadId); });
            QAction *fork =
                menu.addAction(QStringLiteral("Fork"), this,
                               [this, threadId] { forkThread(threadId); });
            QAction *archive = menu.addAction(
                thread->archived ? QStringLiteral("Unarchive")
                                 : QStringLiteral("Archive"),
                this, [this, threadId] { toggleThreadArchive(threadId); });
            menu.addSeparator();
            QAction *remove =
                menu.addAction(QStringLiteral("Delete"), this,
                               [this, threadId] { deleteThread(threadId); });
            rename->setEnabled(canControl);
            fork->setEnabled(canControl);
            archive->setEnabled(canControl);
            remove->setEnabled(canControl);
            menu.exec(threadList->viewport()->mapToGlobal(position));
          });
  splitter->addWidget(sidebar);

  conversationRegion = new QFrame;
  conversationRegion->setObjectName(QStringLiteral("conversation"));
  conversationRegion->setStyleSheet(
      QStringLiteral("QFrame#conversation{background:#f6f8fb;}"));
  conversationRegion->setMinimumWidth(480);
  auto *centerLayout = new QVBoxLayout(conversationRegion);
  centerLayout->setContentsMargins(24, 14, 24, 12);
  centerLayout->setSpacing(0);
  auto *context = new QHBoxLayout;
  auto *threadBadge = makeLabel(QStringLiteral("THREAD"), "small");
  threadBadge->setAlignment(Qt::AlignCenter);
  threadBadge->setFixedSize(58, 20);
  threadBadge->setStyleSheet(
      QStringLiteral("background:#e5eeff;color:#2f6feb;border-radius:5px;"
                     "font-weight:600;"));
  context->addWidget(threadBadge);
  context->addStretch();
  centerLayout->addLayout(context);
  centerLayout->addSpacing(2);
  conversationTitle =
      makeLabel(QStringLiteral("No synchronized thread"), "heading");
  conversationMeta = makeLabel({}, "meta");
  centerLayout->addWidget(conversationTitle);
  centerLayout->addSpacing(2);
  centerLayout->addWidget(conversationMeta);
  centerLayout->addSpacing(7);
  centerLayout->addWidget(makeDivider());
  centerLayout->addSpacing(7);

  noticeBar = new QFrame;
  noticeBar->setStyleSheet(QStringLiteral(
      "background:#fff4f2;border:1px solid #efc2bc;border-radius:6px;"));
  auto *noticeLayout = new QHBoxLayout(noticeBar);
  noticeLayout->setContentsMargins(10, 6, 8, 6);
  noticeLabel = makeLabel({}, "meta");
  noticeLabel->setStyleSheet(QStringLiteral("color:#9d2e2e;"));
  auto *dismissNotice = new QPushButton(QStringLiteral("Dismiss"));
  dismissNotice->setProperty("kind", "subtle");
  dismissNotice->setFixedHeight(28);
  noticeLayout->addWidget(noticeLabel, 1);
  noticeLayout->addWidget(dismissNotice);
  noticeBar->hide();
  connect(dismissNotice, &QPushButton::clicked, noticeBar, &QWidget::hide);
  centerLayout->addWidget(noticeBar);

  conversationScroll = new QScrollArea;
  conversationScroll->setWidgetResizable(true);
  conversationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  conversationContent = new QWidget;
  conversationContent->setMinimumWidth(0);
  conversationContent->setSizePolicy(QSizePolicy::Ignored,
                                     QSizePolicy::Preferred);
  conversationLayout = new QVBoxLayout(conversationContent);
  conversationLayout->setContentsMargins(0, 0, 0, 16);
  conversationLayout->setSpacing(8);
  emptyConversation =
      makeLabel(QStringLiteral("Conversation activity appears here."), "muted");
  conversationLayout->addWidget(emptyConversation);
  addConversationTrailingSpace();
  conversationLayout->addStretch();
  conversationScroll->setWidget(conversationContent);
  QScrollBar *conversationScrollBar = conversationScroll->verticalScrollBar();
  connect(conversationScrollBar, &QScrollBar::valueChanged, this,
          [this, conversationScrollBar](int value) {
            if (conversationScrollRebuilding ||
                conversationScrollProgrammatic || conversationSpacerAdjusting)
              return;
            conversationFollowsLatest =
                value >= conversationScrollBar->maximum() - 1;
          });
  connect(conversationScrollBar, &QScrollBar::rangeChanged, this,
          [this](int, int) {
            if (conversationScrollRebuilding || conversationSpacerAdjusting)
              return;
            if (conversationFollowsLatest)
              scheduleConversationFollowLatest();
          });
  centerLayout->addWidget(conversationScroll, 1);

  composerReserve = new QWidget;
  composerReserve->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  composerReserve->setFixedHeight(0);
  centerLayout->addWidget(composerReserve);

  auto *composerDock =
      new BottomOverlayDock(conversationRegion, [this](int height) {
        updateComposerDockHeight(height);
      });
  auto *composerDockLayout = new QVBoxLayout(composerDock);
  composerDockLayout->setContentsMargins(0, 8, 0, 0);
  composerDockLayout->setSpacing(0);

  auto *attention = new QFrame;
  attention->setProperty("kind", "amberBadge");
  auto *attentionLayout = new QHBoxLayout(attention);
  attentionLayout->setContentsMargins(10, 6, 10, 6);
  attentionLayout->addWidget(makeLabel(
      QStringLiteral("A Codex request needs attention"), "attentionSection"));
  attentionLayout->addStretch();
  approveButton = new QPushButton(QStringLiteral("Review"));
  denyButton = new QPushButton(QStringLiteral("Deny"));
  attentionLayout->addWidget(denyButton);
  attentionLayout->addWidget(approveButton);
  connect(approveButton, &QPushButton::clicked, this,
          [this] { respondToFirstPending(true); });
  connect(denyButton, &QPushButton::clicked, this,
          [this] { respondToFirstPending(false); });
  attention->hide();
  composerDockLayout->addWidget(attention);

  turnSettings = new TurnSettingsWidget;
  composerDockLayout->addWidget(turnSettings);

  auto *composer = new QFrame;
  composer->setProperty("kind", "composer");
  auto *composerLayout = new QVBoxLayout(composer);
  composerLayout->setContentsMargins(10, 8, 8, 8);
  composerLayout->setSpacing(6);
  attachmentPanel = new QFrame;
  attachmentPanel->setProperty("kind", "summary");
  auto *attachmentPanelLayout = new QVBoxLayout(attachmentPanel);
  attachmentPanelLayout->setContentsMargins(6, 6, 6, 6);
  attachmentPanelLayout->setSpacing(4);
  attachmentListScroll = new QScrollArea;
  attachmentListScroll->setWidgetResizable(true);
  attachmentListScroll->setFrameShape(QFrame::NoFrame);
  attachmentListScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *attachmentListContent = new QWidget;
  attachmentListLayout = new QVBoxLayout(attachmentListContent);
  attachmentListLayout->setContentsMargins(0, 0, 0, 0);
  attachmentListLayout->setSpacing(4);
  attachmentListScroll->setWidget(attachmentListContent);
  attachmentPanelLayout->addWidget(attachmentListScroll);
  attachmentPanel->hide();
  composerLayout->addWidget(attachmentPanel);

  composerBody = new QWidget;
  composerBody->installEventFilter(this);
  composerGrid = new QGridLayout(composerBody);
  composerGrid->setContentsMargins(0, 0, 0, 0);
  composerGrid->setHorizontalSpacing(8);
  composerGrid->setVerticalSpacing(6);
  composerGrid->setColumnStretch(1, 1);

  attachmentButton = new QToolButton;
  attachmentButton->setProperty("kind", "composerAction");
  attachmentButton->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MailAttachment));
  attachmentButton->setIconSize(QSize(16, 16));
  attachmentButton->setToolTip(QStringLiteral("Attach files"));
  attachmentButton->setAccessibleName(QStringLiteral("Attach files"));
  attachmentButton->setFixedSize(UpcomingControlHeight, UpcomingControlHeight);
  promptEditor = new codexui::ExpandingPromptEditor;
  sendButton = new QPushButton(QStringLiteral("Send"));
  sendButton->setProperty("kind", "primary");
  sendButton->setFixedSize(62, UpcomingControlHeight);
  interruptButton = new QPushButton(QStringLiteral("Stop"));
  interruptButton->setProperty("kind", "stop");
  interruptButton->setFixedSize(54, UpcomingControlHeight);
  interruptButton->hide();
  composerGrid->addWidget(attachmentButton, 0, 0);
  composerGrid->addWidget(promptEditor, 0, 1);
  composerGrid->addWidget(sendButton, 0, 2);
  composerLayout->addWidget(composerBody);
  composerDockLayout->addWidget(composer);
  connect(promptEditor, &codexui::ExpandingPromptEditor::editorHeightChanged,
          composerDock,
          [composerDock] { composerDock->synchronizeGeometry(); });
  QTimer::singleShot(0, composerDock, [composerDock] {
    composerDock->layout()->activate();
    composerDock->synchronizeGeometry();
  });
  connect(sendButton, &QPushButton::clicked, this, [this] { submitPrompt(); });
  connect(promptEditor, &codexui::ExpandingPromptEditor::submitRequested, this,
          [this] { submitPrompt(); });
  connect(promptEditor, &QPlainTextEdit::textChanged, this,
          [this] { scheduleComposerLayout(); });
  connect(interruptButton, &QPushButton::clicked, this,
          [this] { interruptActiveTurn(); });
  connect(attachmentButton, &QPushButton::clicked, this,
          [this] { chooseAttachments(); });
  splitter->addWidget(conversationRegion);

  inspector = new QFrame;
  inspector->setObjectName(QStringLiteral("inspector"));
  inspector->setStyleSheet(
      QStringLiteral("QFrame#inspector{background:#fbfcfe;}"));
  inspector->setMinimumWidth(300);
  inspector->setMaximumWidth(520);
  auto *inspectorLayout = new QVBoxLayout(inspector);
  inspectorLayout->setContentsMargins(18, 14, 20, 0);
  inspectorLayout->setSpacing(0);
  auto *inspectorHeader = new QHBoxLayout;
  inspectorHeader->addWidget(makeLabel(QStringLiteral("INSPECTOR"), "section"));
  inspectorHeader->addStretch();
  auto *hideInspectorButton = new QPushButton(QStringLiteral("Hide"));
  hideInspectorButton->setProperty("kind", "subtle");
  hideInspectorButton->setFixedSize(58, 24);
  inspectorHeader->addWidget(hideInspectorButton);
  inspectorLayout->addLayout(inspectorHeader);
  inspectorLayout->addSpacing(7);
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
  diffViewer = new DiffViewer;
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
  protocolLog->setProperty("kind", "infoViewer");
  protocolLog->setReadOnly(true);
  protocolLog->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  protocolLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  protocolLog->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  protocolLog->verticalScrollBar()->setProperty("kind", "infoViewer");
  protocolLog->document()->setMaximumBlockCount(200);
  protocolLayout->addWidget(protocolLog, 1);
  protocolLayout->addWidget(protocolStats);
  auto *stateContent = new QWidget;
  auto *stateLayout = new QVBoxLayout(stateContent);
  stateLayout->setContentsMargins(8, 8, 8, 8);
  stateView = new QPlainTextEdit;
  stateView->setProperty("kind", "infoViewer");
  stateView->setReadOnly(true);
  stateView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  stateView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stateView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  stateView->verticalScrollBar()->setProperty("kind", "infoViewer");
  stateLayout->addWidget(stateView);
  infoTabs = new QTabWidget;
  infoTabs->setDocumentMode(true);
  infoTabs->addTab(stateContent, QStringLiteral("State"));
  infoTabs->addTab(protocolContent, QStringLiteral("Protocol"));
  connect(infoTabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == 0) {
      scheduleRefresh(RefreshState);
      return;
    }
    showProtocolTail();
    scheduleRefresh(RefreshProtocolStats);
  });
  inspectorTabs->addTab(planScroll, QStringLiteral("Plan"));
  inspectorTabs->addTab(agentsScroll, QStringLiteral("Agents"));
  inspectorTabs->addTab(diffViewer, QStringLiteral("Changes"));
  inspectorTabs->addTab(requestsScroll, QStringLiteral("Requests"));
  inspectorTabs->addTab(infoTabs, QStringLiteral("Info"));
  connect(inspectorTabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == 4) {
      if (infoTabs && infoTabs->currentIndex() == 1)
        showProtocolTail();
    }
    scheduleRefresh(RefreshInspector | RefreshState | RefreshProtocolStats);
  });
  inspectorLayout->addWidget(inspectorTabs, 1);
  connect(hideInspectorButton, &QPushButton::clicked, this, [this] {
    inspector->hide();
    restoreInspectorButton->show();
  });
  connect(restoreInspectorButton, &QPushButton::clicked, this, [this] {
    inspector->show();
    restoreInspectorButton->hide();
  });
  splitter->addWidget(inspector);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setStretchFactor(2, 0);
  splitter->setSizes({282, 834, 404});
  qApp->installEventFilter(this);
  root->addWidget(splitter, 1);

  auto *statusBar = new QFrame;
  statusBar->setObjectName(QStringLiteral("customStatusBar"));
  statusBar->setStyleSheet(
      QStringLiteral("QFrame#customStatusBar{background:#f8fafc;"
                     "border-top:1px solid #d7dee8;}"));
  statusBar->setFixedHeight(40);
  auto *statusLayout = new QHBoxLayout(statusBar);
  statusLayout->setContentsMargins(18, 0, 24, 0);
  statusLayout->setSpacing(8);
  threadContextStatus = makeLabel(QStringLiteral("No thread context"), "meta");
  statusLayout->addWidget(threadContextStatus);
  statusLayout->addSpacing(42);
  agentActivityStatus = makeLabel(QStringLiteral("No agent activity"), "meta");
  statusLayout->addWidget(agentActivityStatus);
  statusLayout->addStretch();
  controllerLabel = makeLabel(QStringLiteral("Observer"), "meta");
  statusLayout->addWidget(controllerLabel);
  root->addWidget(statusBar);

  refreshTimer = new QTimer(this);
  refreshTimer->setSingleShot(true);
  refreshTimer->setInterval(32);
  connect(refreshTimer, &QTimer::timeout, this, [this] { refresh(); });

  session.setEventHandler(
      [this](const nlohmann::json &event) { handleEvent(event); });
  refresh();
}

bool ShellWidget::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::Wheel && conversationRegion &&
      conversationScroll) {
    auto *target = qobject_cast<QWidget *>(watched);
    const bool inConversationRegion =
        target && (target == conversationRegion ||
                   conversationRegion->isAncestorOf(target));
    const bool onSplitterHandle =
        target && splitter &&
        (target == splitter->handle(1) || target == splitter->handle(2));
    if (inConversationRegion || onSplitterHandle) {
      bool insideScrollableChild = false;
      if (inConversationRegion) {
        for (QWidget *ancestor = target;
             ancestor && ancestor != conversationRegion;
             ancestor = ancestor->parentWidget()) {
          if (qobject_cast<QAbstractScrollArea *>(ancestor)) {
            insideScrollableChild = true;
            break;
          }
        }
      }
      if (!insideScrollableChild) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        QWidget *viewport = conversationScroll->viewport();
        const QPointF localPosition =
            viewport->mapFromGlobal(wheel->globalPosition().toPoint());
        QWheelEvent forwarded(localPosition, wheel->globalPosition(),
                              wheel->pixelDelta(), wheel->angleDelta(),
                              wheel->buttons(), wheel->modifiers(),
                              wheel->phase(), wheel->inverted(),
                              wheel->source(), wheel->pointingDevice());
        QApplication::sendEvent(viewport, &forwarded);
        event->accept();
        return true;
      }
    }
  }
  if (watched == composerBody &&
      (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
       event->type() == QEvent::LayoutRequest))
    scheduleComposerLayout();
  return QWidget::eventFilter(watched, event);
}

void ShellWidget::scheduleComposerLayout() {
  if (composerLayoutRefreshPending)
    return;
  composerLayoutRefreshPending = true;
  QTimer::singleShot(0, this, [this] {
    composerLayoutRefreshPending = false;
    refreshComposerLayout();
  });
}

void ShellWidget::refreshComposerLayout() {
  if (!composerBody || !composerGrid || composerBody->width() <= 0)
    return;

  const bool active = interruptButton->isVisible();
  const int visibleControls = active ? 3 : 2;
  const int controlsWidth = attachmentButton->width() + sendButton->width() +
                            (active ? interruptButton->width() : 0);
  const int compactEditorWidth =
      composerBody->contentsRect().width() - controlsWidth -
      (visibleControls * composerGrid->horizontalSpacing());
  const bool expand = promptEditor->requiresExpandedLayout(compactEditorWidth);
  if (expand == composerExpanded && active == composerActive)
    return;

  composerExpanded = expand;
  composerActive = active;
  composerGrid->removeWidget(attachmentButton);
  composerGrid->removeWidget(promptEditor);
  composerGrid->removeWidget(sendButton);
  composerGrid->removeWidget(interruptButton);
  if (composerExpanded) {
    composerGrid->addWidget(promptEditor, 0, 0, 1, 4);
    composerGrid->addWidget(attachmentButton, 1, 0);
    composerGrid->addWidget(sendButton, 1, 2);
    if (active)
      composerGrid->addWidget(interruptButton, 1, 3);
  } else {
    composerGrid->addWidget(attachmentButton, 0, 0);
    composerGrid->addWidget(promptEditor, 0, 1);
    composerGrid->addWidget(sendButton, 0, 2);
    if (active)
      composerGrid->addWidget(interruptButton, 0, 3);
  }
  composerGrid->invalidate();
}

void ShellWidget::handleEvent(const nlohmann::json &event) {
  appendProtocolFrame(event);
  model.applyEvent(event);
  const std::string kind = stringValue(event, "kind");
  const std::string type = stringValue(event, "type");
  const nlohmann::json data = event.value("data", nlohmann::json::object());
  if (kind == "result" && !event.value("ok", false)) {
    const nlohmann::json error = event.value("error", nlohmann::json::object());
    const std::string message = safeMessage(error);
    showNotice(text(message.empty() ? std::string("Codex operation failed")
                                    : message));
  } else if (kind == "event" && type == "notice.added") {
    const nlohmann::json notice =
        data.value("notice", nlohmann::json::object());
    const std::string message = safeMessage(notice);
    if (!message.empty())
      showNotice(text(message), stringValue(data, "severity") == "error");
  } else if (kind == "event" && type == "system.diagnostic") {
    const std::string message = safeMessage(data);
    if (!message.empty())
      showNotice(QStringLiteral("Protocol diagnostic: %1").arg(text(message)));
  } else if (kind == "event" && type == "connection.lifecycle" &&
             (stringValue(data, "state") == "failure" ||
              stringValue(data, "state") == "disconnected")) {
    const std::string detail = stringValue(data, "detail");
    if (!detail.starts_with("local-"))
      showNotice(detail.empty() ? QStringLiteral("Codex bridge disconnected")
                                : text(detail));
  }
  if (event.value("kind", std::string{}) == "event" &&
      event.value("type", std::string{}) == "connection.bridge" &&
      event.value("data", nlohmann::json::object())
              .value("state", std::string{}) == "opened") {
    requestThreads();
    requestModels();
    session.listPermissionProfiles(
        {{"cwd", QDir::currentPath().toStdString()}});
  }

  hydrateHistoricalAgents();

  if (!selectedThreadId.empty() && !model.thread(selectedThreadId)) {
    selectedThreadId.clear();
    resetComposer();
  }

  const nlohmann::json scope = event.value("scope", nlohmann::json::object());
  const std::string eventThreadId = stringValue(scope, "threadId");
  const std::string turnId = stringValue(scope, "turnId");
  const std::string itemId = stringValue(scope, "itemId");
  if (type == "thread.removed" && !eventThreadId.empty()) {
    pendingPrompts.erase(eventThreadId);
    materializedPromptItemIds.erase(eventThreadId);
  } else if (!eventThreadId.empty()) {
    reconcileAcknowledgedPrompts(eventThreadId);
  }
  if (eventThreadId == selectedThreadId && !turnId.empty() && !itemId.empty() &&
      (type == "conversation.item.upsert" ||
       type == "conversation.item.append" ||
       type == "conversation.reasoning.part-added" ||
       type == "conversation.file-change.output-appended" ||
       type == "conversation.file-change.patch-replaced" ||
       type == "conversation.mcp.progress")) {
    const std::string key = turnId + '\x1f' + itemId;
    dirtyConversationItems[key] = {turnId, itemId};
  }

  const std::string action = stringValue(event, "action");
  if ((event.value("kind", std::string{}) == "result" &&
       action == "thread.read" && eventThreadId == selectedThreadId) ||
      type == "thread.removed") {
    conversationRebuildPending = true;
  }
  scheduleRefresh(refreshAreasForEvent(event));
}

std::uint32_t
ShellWidget::refreshAreasForEvent(const nlohmann::json &event) const {
  const std::string kind = stringValue(event, "kind");
  if (kind == "result") {
    const std::string action = stringValue(event, "action");
    if (action == "thread.read")
      return RefreshAll;
    if (action == "threads.list")
      return RefreshThreads | RefreshProtocolStats | RefreshStatus;
    if (action == "thread.create" || action == "thread.resume" ||
        action == "thread.fork")
      return RefreshThreads | RefreshTurnSettings | RefreshStatus;
    if (action == "turn.start")
      return RefreshThreads | RefreshInspector | RefreshProtocolStats |
             RefreshStatus;
    if (action == "models.list" || action == "permission-profiles.list")
      return RefreshState | RefreshProtocolStats | RefreshTurnSettings;
    return RefreshState | RefreshProtocolStats;
  }

  const std::string type = stringValue(event, "type");
  if (type.starts_with("connection."))
    return RefreshThreads | RefreshInspector | RefreshProtocolStats |
           RefreshStatus;
  if (type == "thread.upsert" || type == "thread.name.changed" ||
      type == "thread.status.changed" || type == "thread.lifecycle")
    return RefreshThreads | RefreshTurnSettings | RefreshProtocolStats |
           RefreshStatus;
  if (type == "thread.removed")
    return RefreshThreads | RefreshConversation | RefreshInspector |
           RefreshTurnSettings | RefreshProtocolStats | RefreshStatus;
  if (type == "turn.upsert")
    return RefreshThreads | RefreshInspector | RefreshProtocolStats |
           RefreshStatus;
  if (type == "plan.replaced")
    return RefreshInspector | RefreshProtocolStats;
  if (type.starts_with("conversation."))
    return RefreshConversation | RefreshInspector | RefreshProtocolStats;
  if (type == "agents.activity.upsert")
    return RefreshInspector | RefreshProtocolStats | RefreshStatus;
  if (type.starts_with("pending-request."))
    return RefreshThreads | RefreshInspector | RefreshProtocolStats |
           RefreshStatus;
  if (type == "thread.token-usage.changed")
    return RefreshProtocolStats | RefreshStatus;
  return RefreshState | RefreshProtocolStats;
}

void ShellWidget::hydrateHistoricalAgents() {
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

void ShellWidget::scheduleRefresh(std::uint32_t areas) {
  pendingRefreshAreas |= areas;
  if (!refreshTimer->isActive())
    refreshTimer->start();
}

void ShellWidget::refresh() {
  const std::uint32_t areas = pendingRefreshAreas;
  pendingRefreshAreas = RefreshNone;
  if ((areas & RefreshThreads) != 0)
    refreshThreads();
  if ((areas & RefreshConversation) != 0) {
    if (conversationRebuildPending) {
      refreshConversation();
    } else {
      bool requiresRebuild = false;
      for (const auto &[key, identity] : dirtyConversationItems) {
        if (!refreshConversationItem(key, identity.first, identity.second)) {
          requiresRebuild = true;
          break;
        }
      }
      if (requiresRebuild)
        refreshConversation();
    }
    dirtyConversationItems.clear();
    conversationRebuildPending = false;
  }
  if ((areas & RefreshInspector) != 0)
    refreshInspector();
  if ((areas & RefreshState) != 0)
    refreshStateInspector();
  if ((areas & RefreshProtocolStats) != 0)
    refreshProtocolStats();
  if ((areas & RefreshTurnSettings) != 0)
    refreshTurnSettings();
  if ((areas & RefreshStatus) != 0)
    refreshStatus();
}

void ShellWidget::showNotice(QString message, bool error) {
  if (message.trimmed().isEmpty())
    return;
  noticeLabel->setText(std::move(message));
  noticeBar->setStyleSheet(
      error ? QStringLiteral("background:#fff4f2;border:1px solid #efc2bc;"
                             "border-radius:6px;")
            : QStringLiteral("background:#fff8e8;border:1px solid #e5c77d;"
                             "border-radius:6px;"));
  noticeLabel->setStyleSheet(error ? QStringLiteral("color:#9d2e2e;")
                                   : QStringLiteral("color:#8a5a00;"));
  noticeBar->show();
}

void ShellWidget::refreshProtocolStats() {
  if (!infoTabs || inspectorTabs->currentIndex() != 4 ||
      infoTabs->currentIndex() != 1)
    return;
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

void ShellWidget::showProtocolTail() {
  if (!protocolLog)
    return;
  QStringList lines;
  lines.reserve(static_cast<qsizetype>(protocolLines.size()));
  for (const QString &line : protocolLines)
    lines.push_back(line);
  protocolLog->setPlainText(lines.join(QLatin1Char('\n')));
  protocolLog->moveCursor(QTextCursor::End);
}

void ShellWidget::refreshTurnSettings() {
  nlohmann::json canonical = nlohmann::json::object();
  std::string identity = "new-thread";
  if (const ThreadPresentation *thread = model.thread(selectedThreadId)) {
    identity = thread->id;
    canonical = thread->raw;
    const auto settings = thread->domains.find("thread.settings.changed");
    if (settings != thread->domains.end() && settings->second.is_object()) {
      nlohmann::json update = settings->second;
      if (update.contains("threadSettings") &&
          update["threadSettings"].is_object())
        update = update["threadSettings"];
      canonical.merge_patch(update);
    }
  } else {
    canonical["cwd"] =
        (localNewThreadIntent && !newThreadDraftWorkspace.isEmpty()
             ? newThreadDraftWorkspace
             : QDir::currentPath())
            .toStdString();
  }

  nlohmann::json permissionProfiles = nlohmann::json::array();
  const auto profiles =
      model.globalDomains().find("operation.permission-profiles.list");
  if (profiles != model.globalDomains().end())
    permissionProfiles = profiles->second;
  turnSettings->setContext(identity, canonical, model.modelCatalog(),
                           permissionProfiles);
}

std::string ShellWidget::visiblySelectedThreadId() const {
  if (!threadList)
    return {};
  const QList<QListWidgetItem *> selected = threadList->selectedItems();
  if (selected.size() != 1 || !selected.front())
    return {};
  return selected.front()->data(Qt::UserRole).toString().toStdString();
}

void ShellWidget::addConversationTrailingSpace() {
  // The spacer belongs to the scroll-area content so QScrollArea derives its
  // extended range from normal layout geometry. It is recreated with the
  // conversation and never replaces the canonical composer reservation.
  conversationTrailingSpace = new QWidget;
  conversationTrailingSpace->setObjectName(
      QStringLiteral("conversationTrailingSpace"));
  conversationTrailingSpace->setSizePolicy(QSizePolicy::Preferred,
                                           QSizePolicy::Fixed);
  conversationTrailingSpace->setFixedHeight(conversationTrailingSpaceHeight);
  conversationLayout->addWidget(conversationTrailingSpace);
}

void ShellWidget::updateComposerDockHeight(int height) {
  if (!composerReserve || !conversationScroll || !conversationContent ||
      height <= 0)
    return;

  if (composerCanonicalHeight == 0) {
    // Only the compact surface participates in the center layout. Later
    // growth remains an overlay and is represented by trailing scroll space.
    composerCanonicalHeight = height;
    composerReserve->setFixedHeight(composerCanonicalHeight);
    return;
  }

  const int trailingHeight = std::max(0, height - composerCanonicalHeight);
  if (trailingHeight == conversationTrailingSpaceHeight)
    return;

  QScrollBar *scrollBar = conversationScroll->verticalScrollBar();
  const int preservedValue = scrollBar->value();
  const bool spacerGrew = trailingHeight > conversationTrailingSpaceHeight;
  conversationTrailingSpaceHeight = trailingHeight;
  const std::uint64_t revision = ++conversationSpacerRevision;
  conversationSpacerAdjusting = true;
  // A larger range must not pull content toward the newly exposed bottom. The
  // user explicitly reaching that bottom will restore follow-latest below.
  if (spacerGrew)
    conversationFollowsLatest = false;

  if (conversationTrailingSpace)
    conversationTrailingSpace->setFixedHeight(trailingHeight);
  conversationLayout->invalidate();
  conversationContent->updateGeometry();

  const auto settle = [this, revision, preservedValue] {
    if (revision != conversationSpacerRevision)
      return;
    QScrollBar *currentScrollBar = conversationScroll->verticalScrollBar();
    conversationScrollProgrammatic = true;
    currentScrollBar->setValue(
        std::min(preservedValue, currentScrollBar->maximum()));
    conversationScrollProgrammatic = false;
  };
  QTimer::singleShot(0, this, [this, revision, settle] {
    if (revision != conversationSpacerRevision)
      return;
    settle();
    QTimer::singleShot(0, this, [this, revision, settle] {
      if (revision != conversationSpacerRevision)
        return;
      settle();
      conversationSpacerAdjusting = false;
      QScrollBar *currentScrollBar = conversationScroll->verticalScrollBar();
      conversationFollowsLatest =
          currentScrollBar->value() >= currentScrollBar->maximum() - 1;
    });
  });
}

void ShellWidget::scrollConversationToLatest() {
  if (!conversationScroll)
    return;
  QScrollBar *scrollBar = conversationScroll->verticalScrollBar();
  conversationScrollProgrammatic = true;
  scrollBar->setValue(scrollBar->maximum());
  conversationScrollProgrammatic = false;
}

void ShellWidget::scheduleConversationFollowLatest() {
  if (conversationFollowScrollPending)
    return;
  conversationFollowScrollPending = true;
  // Shell-output documents and wrapping labels can report several closely
  // spaced geometry changes while a new card is admitted. Follow once after
  // that burst instead of moving the viewport for every intermediate range.
  QTimer::singleShot(16, this, [this] {
    conversationFollowScrollPending = false;
    if (conversationFollowsLatest && !conversationScrollRebuilding &&
        !conversationSpacerAdjusting)
      scrollConversationToLatest();
  });
}

void ShellWidget::settleConversationScroll(bool followLatest,
                                           int preservedValue) {
  QTimer::singleShot(0, this, [this, followLatest, preservedValue] {
    QScrollBar *scrollBar = conversationScroll->verticalScrollBar();
    conversationScrollProgrammatic = true;
    scrollBar->setValue(followLatest
                            ? scrollBar->maximum()
                            : std::min(preservedValue, scrollBar->maximum()));
    conversationScrollProgrammatic = false;
    QTimer::singleShot(0, this, [this, followLatest, preservedValue] {
      QScrollBar *scrollBar = conversationScroll->verticalScrollBar();
      conversationScrollProgrammatic = true;
      scrollBar->setValue(followLatest
                              ? scrollBar->maximum()
                              : std::min(preservedValue, scrollBar->maximum()));
      conversationScrollProgrammatic = false;
      conversationScrollRebuilding = false;
      conversationFollowsLatest =
          scrollBar->value() >= scrollBar->maximum() - 1;
    });
  });
}

void ShellWidget::appendProtocolFrame(const nlohmann::json &frame) {
  if (!protocolLog)
    return;

  const auto recordLine = [this](QString line) {
    if (protocolLines.size() == 200)
      protocolLines.pop_front();
    protocolLines.push_back(line);
    if (inspectorTabs->currentIndex() == 4 && infoTabs &&
        infoTabs->currentIndex() == 1)
      protocolLog->appendPlainText(std::move(line));
  };

  const std::uint64_t sequence = frame.value("sequence", 0ULL);
  if (sequence != 0) {
    if (observedPresentationSequence != 0 &&
        sequence != observedPresentationSequence + 1) {
      const QString relation = sequence <= observedPresentationSequence
                                   ? QStringLiteral("NON-MONOTONIC")
                                   : QStringLiteral("SEQUENCE GAP");
      recordLine(
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
  recordLine(parts.join(QStringLiteral("  ")));
}

void ShellWidget::refreshThreads() {
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
    auto *item = new QListWidgetItem(threadList);
    item->setSizeHint(QSize(0, 48));
    item->setData(Qt::UserRole, text(threadId));
    item->setToolTip(text(thread->cwd));
    auto *row = new QWidget;
    row->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(5, 2, 5, 2);
    rowLayout->setSpacing(8);
    auto *dot = makeStatusDot();
    QString dotColor = QStringLiteral("#98a2b3");
    if (model.pendingRequestCount(threadId) != 0)
      dotColor = QStringLiteral("#a76812");
    else if (thread->status == "active" || thread->status == "inProgress")
      dotColor = QStringLiteral("#2f6feb");
    else if (thread->status == "failed" || thread->status == "systemError")
      dotColor = QStringLiteral("#b83a3a");
    dot->setStyleSheet(
        QStringLiteral("background:%1;border-radius:4px;").arg(dotColor));
    rowLayout->addWidget(dot);
    auto *copy = new QVBoxLayout;
    copy->setContentsMargins(0, 0, 0, 0);
    copy->setSpacing(1);
    auto *titleLabel = makeLabel(title, "title");
    titleLabel->setStyleSheet(QStringLiteral("font-weight:500;"));
    copy->addWidget(titleLabel);
    copy->addWidget(makeLabel(displayStatus(thread->status), "meta"));
    rowLayout->addLayout(copy, 1);
    threadList->setItemWidget(item, row);
    if (threadId == selectedThreadId)
      threadList->setCurrentItem(item);
  }
  threadList->blockSignals(false);
}

void ShellWidget::refreshConversation() {
  QScrollBar *scrollBar = conversationScroll->verticalScrollBar();
  const bool followLatest = conversationFollowsLatest;
  const int preservedValue = scrollBar->value();
  ++conversationSpacerRevision;
  conversationSpacerAdjusting = false;
  conversationScrollRebuilding = true;
  for (const auto &[key, card] : conversationCards) {
    if (const auto state = commandOutputScrollState(card))
      commandOutputScrollStates[key] = *state;
  }
  conversationCards.clear();
  conversationTrailingSpace = nullptr;
  clearLayout(conversationLayout);

  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread) {
    conversationTitle->setText(localNewThreadIntent
                                   ? QStringLiteral("New thread")
                                   : QStringLiteral("Select a thread"));
    conversationMeta->setText(localNewThreadIntent ? QDir::currentPath()
                                                   : QString{});
    if (localNewThreadIntent && !newThreadPendingPrompts.empty()) {
      emptyConversation = nullptr;
      for (const PendingPrompt &pending : newThreadPendingPrompts) {
        conversationLayout->addWidget(new PendingPromptCard(
            pending.prompt, static_cast<int>(pending.attachments.size()),
            pending.status == PendingPromptStatus::Awaiting,
            pending.status == PendingPromptStatus::Failed, pending.error));
      }
    } else {
      emptyConversation = makeLabel(
          localNewThreadIntent
              ? QStringLiteral("Send a message to create this thread.")
              : QStringLiteral("Conversation activity appears here."),
          "muted");
      conversationLayout->addWidget(emptyConversation);
    }
    addConversationTrailingSpace();
    conversationLayout->addStretch();
    settleConversationScroll(followLatest, preservedValue);
    return;
  }

  reconcileAcknowledgedPrompts(selectedThreadId);
  conversationTitle->setText(text(thread->title));
  conversationMeta->setText(text(thread->cwd) + QStringLiteral("  |  ") +
                            displayStatus(thread->status));
  struct VisibleItem {
    std::string key;
    const ItemPresentation *item = nullptr;
  };
  std::vector<VisibleItem> items;
  for (const std::string &turnId : thread->turnOrder) {
    const auto turn = thread->turns.find(turnId);
    if (turn == thread->turns.end())
      continue;
    for (const std::string &itemId : turn->second.itemOrder) {
      const auto item = turn->second.items.find(itemId);
      if (item == turn->second.items.end())
        continue;
      items.push_back({turnId + '\x1f' + itemId, &item->second});
    }
  }
  const std::size_t first = items.size() > conversationItemLimit
                                ? items.size() - conversationItemLimit
                                : 0;
  if (first != 0) {
    const std::size_t page = std::min<std::size_t>(80, first);
    auto *loadEarlier =
        new QPushButton(QStringLiteral("Load %1 more activities")
                            .arg(static_cast<qulonglong>(page)));
    loadEarlier->setProperty("kind", "history");
    loadEarlier->setFixedHeight(UpcomingControlHeight);
    loadEarlier->setToolTip(QStringLiteral("%1 earlier activities are retained")
                                .arg(static_cast<qulonglong>(first)));
    connect(loadEarlier, &QPushButton::clicked, this, [this, page] {
      conversationItemLimit += page;
      conversationRebuildPending = true;
      scheduleRefresh(RefreshConversation);
    });
    conversationLayout->addWidget(loadEarlier, 0, Qt::AlignHCenter);
  }
  for (std::size_t index = first; index < items.size(); ++index) {
    std::optional<CommandOutputScrollState> outputScrollState;
    if (const auto retained = commandOutputScrollStates.find(items[index].key);
        retained != commandOutputScrollStates.end())
      outputScrollState = retained->second;
    QWidget *card = itemFrame(*items[index].item, outputScrollState);
    conversationCards[items[index].key] = card;
    conversationLayout->addWidget(card);
  }
  const auto pending = pendingPrompts.find(selectedThreadId);
  if (items.empty() &&
      (pending == pendingPrompts.end() || pending->second.empty()))
    conversationLayout->addWidget(
        makeLabel(QStringLiteral("No materialized activity."), "muted"));
  if (pending != pendingPrompts.end()) {
    for (const PendingPrompt &submission : pending->second) {
      conversationLayout->addWidget(new PendingPromptCard(
          submission.prompt, static_cast<int>(submission.attachments.size()),
          submission.status == PendingPromptStatus::Awaiting,
          submission.status == PendingPromptStatus::Failed, submission.error));
    }
  }
  addConversationTrailingSpace();
  conversationLayout->addStretch();
  settleConversationScroll(followLatest, preservedValue);
}

bool ShellWidget::refreshConversationItem(const std::string &key,
                                          const std::string &turnId,
                                          const std::string &itemId) {
  const auto existing = conversationCards.find(key);
  if (existing == conversationCards.end())
    return false;
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread)
    return false;
  const auto turn = thread->turns.find(turnId);
  if (turn == thread->turns.end())
    return false;
  const auto item = turn->second.items.find(itemId);
  if (item == turn->second.items.end())
    return false;

  const bool followLatest = conversationFollowsLatest;
  std::optional<CommandOutputScrollState> outputScrollState =
      commandOutputScrollState(existing->second);
  if (outputScrollState)
    commandOutputScrollStates[key] = *outputScrollState;
  QWidget *replacement = itemFrame(item->second, outputScrollState);
  QLayoutItem *replaced =
      conversationLayout->replaceWidget(existing->second, replacement);
  if (!replaced) {
    replacement->deleteLater();
    return false;
  }
  delete replaced;
  existing->second->deleteLater();
  existing->second = replacement;
  if (followLatest)
    scheduleConversationFollowLatest();
  return true;
}

void ShellWidget::refreshInspector() {
  const int activeTab = inspectorTabs->currentIndex();
  QVBoxLayout *activeLayout = nullptr;
  if (activeTab == 0)
    activeLayout = planLayout;
  else if (activeTab == 1)
    activeLayout = agentsLayout;
  else if (activeTab == 3)
    activeLayout = requestsLayout;
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (activeTab == 2) {
    QString liveDiff;
    std::vector<DiffFilePresentation> retained;
    if (thread) {
      for (auto turnId = thread->turnOrder.rbegin();
           turnId != thread->turnOrder.rend() && liveDiff.isEmpty(); ++turnId) {
        const auto turn = thread->turns.find(*turnId);
        if (turn == thread->turns.end())
          continue;
        const auto domain = turn->second.domains.find("turn.diff.changed");
        if (domain != turn->second.domains.end())
          liveDiff = text(stringValue(domain->second, "diff"));
      }
      if (liveDiff.isEmpty()) {
        for (auto turnId = thread->turnOrder.rbegin();
             turnId != thread->turnOrder.rend() && retained.empty(); ++turnId) {
          const auto turn = thread->turns.find(*turnId);
          if (turn == thread->turns.end())
            continue;
          for (auto itemId = turn->second.itemOrder.rbegin();
               itemId != turn->second.itemOrder.rend(); ++itemId) {
            const auto item = turn->second.items.find(*itemId);
            if (item == turn->second.items.end() ||
                stringValue(item->second.raw, "type") != "fileChange")
              continue;
            const nlohmann::json changes =
                item->second.raw.value("changes", nlohmann::json::array());
            if (!changes.is_array())
              continue;
            for (const auto &change : changes) {
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
    diffViewer->setChanges(std::move(liveDiff), std::move(retained));
    return;
  }
  if (!activeLayout)
    return;

  clearLayout(activeLayout);
  if (!thread && activeTab != 3) {
    activeLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
    activeLayout->addStretch();
    return;
  }

  if (activeTab == 0) {
    const TurnPresentation *planTurn = nullptr;
    const ItemPresentation *planItem = nullptr;
    for (auto turnId = thread->turnOrder.rbegin();
         turnId != thread->turnOrder.rend(); ++turnId) {
      const auto turn = thread->turns.find(*turnId);
      if (turn == thread->turns.end())
        continue;
      if (turn->second.plan.is_object() &&
          turn->second.plan.contains("steps")) {
        planTurn = &turn->second;
        break;
      }
      for (auto itemId = turn->second.itemOrder.rbegin();
           itemId != turn->second.itemOrder.rend(); ++itemId) {
        const auto item = turn->second.items.find(*itemId);
        if (item != turn->second.items.end() &&
            stringValue(item->second.raw, "type") == "plan") {
          planItem = &item->second;
          break;
        }
      }
      if (planItem)
        break;
    }
    if (planTurn) {
      const QString explanation =
          text(stringValue(planTurn->plan, "explanation"));
      if (!explanation.isEmpty())
        planLayout->addWidget(makeMarkdownLabel(explanation));
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
    } else if (planItem) {
      const QString planText = text(stringValue(planItem->raw, "text"));
      if (planText.isEmpty())
        planLayout->addWidget(
            makeLabel(QStringLiteral("Plan is being prepared."), "muted"));
      else
        planLayout->addWidget(makeMarkdownLabel(planText));
    } else {
      planLayout->addWidget(
          makeLabel(QStringLiteral("No plan for this thread."), "muted"));
    }
    planLayout->addStretch();
    return;
  }

  if (activeTab == 1) {
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
    return;
  }

  std::size_t requestCount = 0;
  for (const auto &[id, request] : model.pendingRequestPresentations()) {
    auto *frame = new QFrame;
    frame->setProperty("kind", "summary");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(9, 7, 9, 7);
    layout->setSpacing(5);
    layout->addWidget(makeLabel(text(request.kind), "title"));
    QString threadContext = text(request.threadId);
    if (const ThreadPresentation *requestThread =
            model.thread(request.threadId);
        requestThread && !requestThread->title.empty())
      threadContext = text(requestThread->title);
    layout->addWidget(
        makeLabel(QStringLiteral("thread %1  |  generation %2  |  request %3")
                      .arg(threadContext)
                      .arg(static_cast<qulonglong>(request.generation))
                      .arg(text(id)),
                  "meta"));
    const std::string command = stringValue(request.raw, "command");
    const std::string reason = stringValue(request.raw, "reason");
    const std::string message = stringValue(request.raw, "message");
    if (!command.empty())
      layout->addWidget(
          makeLabel(QStringLiteral("Command: %1").arg(text(command)), "meta"));
    if (!reason.empty())
      layout->addWidget(
          makeLabel(QStringLiteral("Reason: %1").arg(text(reason)), "meta"));
    if (!message.empty())
      layout->addWidget(makeLabel(text(message), "meta"));
    if (request.raw.contains("questions") &&
        request.raw["questions"].is_array())
      layout->addWidget(makeLabel(
          QStringLiteral("%1 questions")
              .arg(static_cast<qulonglong>(request.raw["questions"].size())),
          "meta"));
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 2, 0, 0);
    auto *deny = new QPushButton(QStringLiteral("Deny"));
    auto *review = new QPushButton(QStringLiteral("Review"));
    review->setProperty("kind", "primary");
    deny->setFixedHeight(28);
    review->setFixedHeight(28);
    connect(deny, &QPushButton::clicked, this,
            [this, id] { rejectPending(id); });
    connect(review, &QPushButton::clicked, this,
            [this, id] { reviewPending(id); });
    actions->addStretch();
    actions->addWidget(deny);
    actions->addWidget(review);
    layout->addLayout(actions);
    requestsLayout->addWidget(frame);
    ++requestCount;
  }
  if (requestCount == 0)
    requestsLayout->addWidget(
        makeLabel(QStringLiteral("No pending requests."), "muted"));
  requestsLayout->addStretch();
}

void ShellWidget::resetComposer() {
  promptEditor->clear();
  attachmentDrafts.clear();
  ++attachmentRevision;
  refreshAttachments();
  refreshComposerEnabledState();
}

void ShellWidget::refreshComposerEnabledState() {
  if (!promptEditor || !sendButton || !attachmentButton)
    return;
  const ConnectionPresentation &connection = model.connection();
  const bool canSubmit =
      connection.connected && connection.role == "controller";
  promptEditor->setEnabled(true);
  sendButton->setEnabled(canSubmit);
  attachmentButton->setEnabled(canSubmit);
  for (QPushButton *button : attachmentPanel->findChildren<QPushButton *>())
    button->setEnabled(true);
}

void ShellWidget::completePromptSubmission(const std::string &threadId,
                                           std::uint64_t submissionId,
                                           const nlohmann::json &result) {
  const auto prompts = pendingPrompts.find(threadId);
  if (prompts == pendingPrompts.end())
    return;
  const auto submission =
      std::find_if(prompts->second.begin(), prompts->second.end(),
                   [submissionId](const PendingPrompt &candidate) {
                     return candidate.id == submissionId;
                   });
  if (submission == prompts->second.end())
    return;
  if (result.value("ok", false))
    submission->status = PendingPromptStatus::Acknowledged;
  else {
    submission->status = PendingPromptStatus::Failed;
    const nlohmann::json error =
        result.value("error", nlohmann::json::object());
    const std::string message = safeMessage(error);
    submission->error =
        text(message.empty() ? std::string("Submission failed") : message);
    showNotice(text(message.empty() ? std::string("Turn submission failed")
                                    : message));
  }
  conversationRebuildPending = true;
  scheduleRefresh(RefreshConversation | RefreshStatus);
  QTimer::singleShot(0, this,
                     [this, threadId] { dispatchNextPrompt(threadId); });
}

std::unordered_set<std::string>
ShellWidget::materializedUserMessageIds(const std::string &threadId) const {
  std::unordered_set<std::string> result;
  const ThreadPresentation *thread = model.thread(threadId);
  if (!thread)
    return result;
  for (const std::string &turnId : thread->turnOrder) {
    const auto turn = thread->turns.find(turnId);
    if (turn == thread->turns.end())
      continue;
    for (const std::string &itemId : turn->second.itemOrder) {
      const auto item = turn->second.items.find(itemId);
      if (item != turn->second.items.end() &&
          stringValue(item->second.raw, "type") == "userMessage")
        result.insert(turnId + '\x1f' + itemId);
    }
  }
  return result;
}

void ShellWidget::reconcileAcknowledgedPrompts(const std::string &threadId) {
  const auto prompts = pendingPrompts.find(threadId);
  const ThreadPresentation *thread = model.thread(threadId);
  if (prompts == pendingPrompts.end() || !thread)
    return;
  auto &claimed = materializedPromptItemIds[threadId];
  for (auto submission = prompts->second.begin();
       submission != prompts->second.end();) {
    if (submission->status != PendingPromptStatus::Acknowledged) {
      ++submission;
      continue;
    }
    std::string matchedId;
    for (const std::string &turnId : thread->turnOrder) {
      const auto turn = thread->turns.find(turnId);
      if (turn == thread->turns.end())
        continue;
      for (const std::string &itemId : turn->second.itemOrder) {
        const auto item = turn->second.items.find(itemId);
        if (item == turn->second.items.end() ||
            stringValue(item->second.raw, "type") != "userMessage")
          continue;
        const std::string identity = turnId + '\x1f' + itemId;
        if (claimed.contains(identity) ||
            submission->knownUserMessageIds.contains(identity))
          continue;
        if (messageText(item->second.raw).trimmed() ==
            submission->prompt.trimmed()) {
          matchedId = identity;
          break;
        }
      }
      if (!matchedId.empty())
        break;
    }
    if (matchedId.empty()) {
      ++submission;
      continue;
    }
    claimed.insert(matchedId);
    submission = prompts->second.erase(submission);
  }
  if (prompts->second.empty())
    pendingPrompts.erase(prompts);
}

void ShellWidget::refreshStatus() {
  const ConnectionPresentation &connection = model.connection();
  QString dotStyle;
  QString dotToolTip;
  if (connection.connected) {
    dotStyle = QStringLiteral("background:#23845a;border-radius:4px;");
    dotToolTip = QStringLiteral("Connected");
  } else if (connection.retrying) {
    dotStyle = QStringLiteral("background:#d98e1c;border-radius:4px;");
    dotToolTip = QStringLiteral("Disconnected, retrying");
  } else {
    dotStyle = QStringLiteral("background:#b83a3a;border-radius:4px;");
    dotToolTip = QStringLiteral("Disconnected");
  }
  connectionStatusDot->setStyleSheet(dotStyle);
  connectionStatusDot->setToolTip(dotToolTip);
  QString selectedTransport;
  const std::string selectedKey = stringValue(connection.settings, "selected");
  const nlohmann::json available =
      connection.settings.value("available", nlohmann::json::array());
  if (available.is_array()) {
    for (const auto &entry : available) {
      if (stringValue(entry, "key") == selectedKey) {
        selectedTransport = text(stringValue(entry, "label"));
        break;
      }
    }
  }
  connectionButton->setText(selectedTransport.isEmpty()
                                ? QStringLiteral("Connection")
                                : selectedTransport);
  connectionButton->setToolTip(
      connection.connected ? QStringLiteral("Connected bridge transport")
                           : QStringLiteral("Disconnected bridge transport"));
  connectAction->setEnabled(!connection.connected);
  disconnectAction->setEnabled(connection.connected);
  reconnectAction->setEnabled(connection.connected);
  controllerLabel->setText(connection.role.empty() ? QStringLiteral("No role")
                                                   : text(connection.role));
  controllerButton->setText(connection.role == "controller"
                                ? QStringLiteral("Release control")
                                : QStringLiteral("Claim control"));
  controllerButton->setEnabled(connection.connected);
  const std::size_t pending = model.pendingRequestCount(selectedThreadId);
  const std::size_t totalPending = model.pendingRequestCount();
  requestButton->setText(QStringLiteral("Requests (%1)")
                             .arg(static_cast<qulonglong>(totalPending)));
  requestButton->setVisible(totalPending != 0);
  approveButton->parentWidget()->setVisible(pending != 0);

  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (thread) {
    const QString workspace = text(thread->cwd);
    workspaceBreadcrumb->setToolTip(workspace);
    workspaceBreadcrumb->setText(workspaceBreadcrumb->fontMetrics().elidedText(
        workspace, Qt::ElideMiddle, workspaceBreadcrumb->maximumWidth()));
    threadContextStatus->setText(
        QStringLiteral("%1  |  %2")
            .arg(text(thread->title), displayStatus(thread->status)));
    std::size_t runningAgents = 0;
    for (const auto &[agentId, agent] : thread->agents) {
      static_cast<void>(agentId);
      if (agent.status == "inProgress" || agent.status == "running" ||
          agent.status == "started")
        ++runningAgents;
    }
    agentActivityStatus->setText(
        thread->agents.empty()
            ? QStringLiteral("No agent activity")
            : QStringLiteral("%1 agents  |  %2 active")
                  .arg(static_cast<qulonglong>(thread->agents.size()))
                  .arg(static_cast<qulonglong>(runningAgents)));
  } else {
    const QString workspace =
        localNewThreadIntent
            ? text(turnSettings->workspace(QDir::currentPath().toStdString()))
            : QStringLiteral("No workspace");
    workspaceBreadcrumb->setToolTip(workspace);
    workspaceBreadcrumb->setText(workspaceBreadcrumb->fontMetrics().elidedText(
        workspace, Qt::ElideMiddle, workspaceBreadcrumb->maximumWidth()));
    threadContextStatus->setText(localNewThreadIntent
                                     ? QStringLiteral("New thread")
                                     : QStringLiteral("No thread context"));
    agentActivityStatus->setText(QStringLiteral("No agent activity"));
  }
  const bool active = model.activeTurnId(selectedThreadId).has_value();
  interruptButton->setVisible(active);
  sendButton->setText(active ? QStringLiteral("Steer")
                             : QStringLiteral("Send"));
  const QString actionKind =
      active ? QStringLiteral("steer") : QStringLiteral("primary");
  if (sendButton->property("kind").toString() != actionKind) {
    sendButton->setProperty("kind", actionKind);
    sendButton->style()->unpolish(sendButton);
    sendButton->style()->polish(sendButton);
  }
  scheduleComposerLayout();
  refreshComposerEnabledState();
  turnSettings->setControlsEnabled(connection.connected &&
                                   connection.role == "controller" && !active);
}

void ShellWidget::refreshStateInspector() {
  if (!stateView || !infoTabs || inspectorTabs->currentIndex() != 4 ||
      infoTabs->currentIndex() != 0)
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
  std::string rendered = state.dump(2);
  constexpr std::size_t MaximumRenderedStateBytes = 32U * 1024U;
  if (rendered.size() > MaximumRenderedStateBytes) {
    const std::size_t totalBytes = rendered.size();
    rendered.resize(MaximumRenderedStateBytes);
    rendered += "\n\n[State display truncated at 32 KiB; retained bytes: " +
                std::to_string(totalBytes) + "]";
  }
  stateView->setPlainText(text(rendered));
}

void ShellWidget::selectThread(std::string threadId) {
  if (threadId != selectedThreadId)
    conversationItemLimit = 80;
  selectedThreadId = std::move(threadId);
  localNewThreadIntent = false;
  newThreadDraftOptions = nlohmann::json::object();
  newThreadDraftName.clear();
  newThreadDraftWorkspace.clear();
  conversationRebuildPending = true;
  resetComposer();
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread || thread->turnOrder.empty())
    readThread(selectedThreadId);
  scheduleRefresh();
}

void ShellWidget::beginNewThread() {
  if (newThreadCreationInFlight) {
    showNotice(QStringLiteral("The current new thread is still being created."),
               false);
    return;
  }
  NewThreadDialog dialog(
      text(turnSettings->workspace(QDir::currentPath().toStdString())), this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  const NewThreadDraft draft = dialog.draft();
  newThreadPendingPrompts.clear();
  selectedThreadId.clear();
  localNewThreadIntent = true;
  newThreadDraftName = draft.name;
  newThreadDraftWorkspace = draft.workspace;
  newThreadDraftOptions = nlohmann::json::object();
  if (!draft.baseInstructions.isEmpty())
    newThreadDraftOptions["baseInstructions"] =
        draft.baseInstructions.toStdString();
  if (!draft.developerInstructions.isEmpty())
    newThreadDraftOptions["developerInstructions"] =
        draft.developerInstructions.toStdString();
  if (draft.ephemeral)
    newThreadDraftOptions["ephemeral"] = true;
  turnSettings->setWorkspace(draft.workspace);
  conversationItemLimit = 80;
  conversationRebuildPending = true;
  threadList->clearSelection();
  resetComposer();
  promptEditor->setFocus();
  scheduleRefresh();
}

void ShellWidget::requestThreads() { session.listThreads(); }

void ShellWidget::requestModels() { session.listModels(); }

void ShellWidget::readThread(const std::string &threadId) {
  if (threadId.empty())
    return;
  session.readThread(threadId);
}

void ShellWidget::renameThread(const std::string &threadId) {
  const ThreadPresentation *thread = model.thread(threadId);
  if (!thread)
    return;
  bool accepted = false;
  const QString name =
      QInputDialog::getText(this, QStringLiteral("Rename thread"),
                            QStringLiteral("Name"), QLineEdit::Normal,
                            text(thread->title), &accepted)
          .trimmed();
  if (accepted && !name.isEmpty())
    session.renameThread(threadId, name.toStdString());
}

void ShellWidget::forkThread(const std::string &threadId) {
  if (threadId.empty())
    return;
  session.forkThread(
      threadId, nlohmann::json::object(), [this](const nlohmann::json &result) {
        if (!result.value("ok", false))
          return;
        const std::string threadId =
            stringValue(result.value("data", nlohmann::json::object())
                            .value("thread", nlohmann::json::object()),
                        "id");
        if (!threadId.empty())
          selectThread(threadId);
      });
}

void ShellWidget::toggleThreadArchive(const std::string &threadId) {
  const ThreadPresentation *thread = model.thread(threadId);
  if (!thread)
    return;
  if (thread->archived)
    session.unarchiveThread(threadId);
  else
    session.archiveThread(threadId);
}

void ShellWidget::deleteThread(const std::string &threadId) {
  if (threadId.empty())
    return;
  if (QMessageBox::question(this, QStringLiteral("Delete thread"),
                            QStringLiteral("Delete the selected thread?"),
                            QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel) == QMessageBox::Yes) {
    session.deleteThread(threadId);
  }
}

void ShellWidget::submitPrompt() {
  const QString promptValue = promptEditor->toPlainText().trimmed();
  if (promptValue.isEmpty())
    return;
  const std::string visibleThreadId = visiblySelectedThreadId();
  if (!visibleThreadId.empty() && visibleThreadId != selectedThreadId) {
    if (!model.thread(visibleThreadId)) {
      showNotice(QStringLiteral("The visibly selected thread is no longer "
                                "available. Your message was not sent."));
      return;
    }
    selectThread(visibleThreadId);
  }

  PendingPrompt submission;
  submission.id = nextPendingPromptId++;
  submission.prompt = promptValue;
  submission.attachments = attachmentDrafts;
  submission.turnOptions = turnSettings->turnStartOptions();

  if (!selectedThreadId.empty()) {
    submission.knownUserMessageIds =
        materializedUserMessageIds(selectedThreadId);
    const std::string destination = selectedThreadId;
    pendingPrompts[destination].push_back(std::move(submission));
    resetComposer();
    conversationRebuildPending = true;
    scheduleRefresh(RefreshConversation);
    dispatchNextPrompt(destination);
    return;
  }
  if (!localNewThreadIntent) {
    showNotice(QStringLiteral("No destination thread is selected. Your "
                              "message was not sent; select a thread or use "
                              "New thread."));
    promptEditor->setFocus();
    return;
  }
  newThreadPendingPrompts.push_back(std::move(submission));
  resetComposer();
  conversationRebuildPending = true;
  scheduleRefresh(RefreshConversation);
  startThreadForPendingPrompts();
}

void ShellWidget::startThreadForPendingPrompts() {
  if (newThreadCreationInFlight || newThreadPendingPrompts.empty())
    return;
  newThreadCreationInFlight = true;
  nlohmann::json threadOptions = turnSettings->threadStartOptions();
  threadOptions.update(newThreadDraftOptions);
  threadOptions["cwd"] =
      turnSettings->workspace(QDir::currentPath().toStdString());
  const QString requestedName = newThreadDraftName;
  session.createThread(
      std::move(threadOptions),
      [this, requestedName](const nlohmann::json &result) {
        newThreadCreationInFlight = false;
        if (!result.value("ok", false)) {
          const nlohmann::json error =
              result.value("error", nlohmann::json::object());
          const std::string message = safeMessage(error);
          const QString displayedError =
              text(message.empty() ? std::string("Thread creation failed")
                                   : message);
          for (PendingPrompt &pending : newThreadPendingPrompts) {
            if (pending.status == PendingPromptStatus::Awaiting) {
              pending.status = PendingPromptStatus::Failed;
              pending.error = displayedError;
            }
          }
          showNotice(text(message.empty()
                              ? std::string("Thread creation failed")
                              : message));
          conversationRebuildPending = true;
          scheduleRefresh(RefreshConversation);
          return;
        }
        const std::string threadId =
            stringValue(result.value("data", nlohmann::json::object())
                            .value("thread", nlohmann::json::object()),
                        "id");
        if (threadId.empty()) {
          for (PendingPrompt &pending : newThreadPendingPrompts) {
            if (pending.status == PendingPromptStatus::Awaiting) {
              pending.status = PendingPromptStatus::Failed;
              pending.error = QStringLiteral(
                  "Thread creation returned no thread identifier");
            }
          }
          showNotice(QStringLiteral("Thread creation returned no thread."));
          conversationRebuildPending = true;
          scheduleRefresh(RefreshConversation);
          return;
        }
        auto &threadPrompts = pendingPrompts[threadId];
        threadPrompts.insert(
            threadPrompts.end(),
            std::make_move_iterator(newThreadPendingPrompts.begin()),
            std::make_move_iterator(newThreadPendingPrompts.end()));
        newThreadPendingPrompts.clear();
        const bool viewingNewThreadDraft =
            selectedThreadId.empty() && localNewThreadIntent;
        if (viewingNewThreadDraft) {
          selectedThreadId = threadId;
          localNewThreadIntent = false;
        }
        newThreadDraftOptions = nlohmann::json::object();
        newThreadDraftName.clear();
        newThreadDraftWorkspace.clear();
        conversationItemLimit = 80;
        conversationRebuildPending = true;
        if (!requestedName.isEmpty())
          session.renameThread(threadId, requestedName.toStdString());
        scheduleRefresh();
        QTimer::singleShot(0, this,
                           [this, threadId] { dispatchNextPrompt(threadId); });
      });
}

void ShellWidget::dispatchNextPrompt(const std::string &threadId) {
  const auto prompts = pendingPrompts.find(threadId);
  if (prompts == pendingPrompts.end())
    return;
  if (std::any_of(prompts->second.begin(), prompts->second.end(),
                  [](const PendingPrompt &candidate) {
                    return candidate.status == PendingPromptStatus::Awaiting &&
                           candidate.dispatched;
                  }))
    return;
  const auto next =
      std::find_if(prompts->second.begin(), prompts->second.end(),
                   [](const PendingPrompt &candidate) {
                     return candidate.status == PendingPromptStatus::Awaiting &&
                            !candidate.dispatched;
                   });
  if (next == prompts->second.end())
    return;
  next->dispatched = true;
  submitPromptToThread(threadId, next->id, next->prompt.toStdString(),
                       next->turnOptions, next->attachments);
}

void ShellWidget::submitPromptToThread(
    std::string threadId, std::uint64_t submissionId, std::string prompt,
    nlohmann::json options, std::vector<AttachmentDraft> attachments) {
  nlohmann::json input =
      nlohmann::json::array({{{"type", "text"},
                              {"text", std::move(prompt)},
                              {"text_elements", nlohmann::json::array()}}});
  for (const AttachmentDraft &attachment : attachments) {
    if (attachment.mimeType.startsWith(QStringLiteral("image/"))) {
      input.push_back(
          {{"type", "localImage"}, {"path", attachment.path.toStdString()}});
    } else if (attachment.mimeType.startsWith(QStringLiteral("audio/"))) {
      input.push_back(
          {{"type", "localAudio"}, {"path", attachment.path.toStdString()}});
    } else {
      input.push_back({{"type", "mention"},
                       {"name", attachment.name.toStdString()},
                       {"path", attachment.path.toStdString()}});
    }
  }
  const auto completed = [this, threadId,
                          submissionId](const nlohmann::json &result) {
    completePromptSubmission(threadId, submissionId, result);
  };
  const auto activeTurn = model.activeTurnId(threadId);
  if (activeTurn) {
    session.steerTurn(threadId, *activeTurn, std::move(input), completed);
  } else {
    auto startTurn = [this, threadId, input = std::move(input),
                      options = std::move(options), completed]() mutable {
      session.startTurn(threadId, std::move(input), std::move(options),
                        completed);
    };
    const ThreadPresentation *thread = model.thread(threadId);
    if (thread && thread->status == "notLoaded") {
      session.resumeThread(threadId, nlohmann::json::object(),
                           [startTurn = std::move(startTurn),
                            completed](const nlohmann::json &result) mutable {
                             if (!result.value("ok", false)) {
                               completed(result);
                               return;
                             }
                             startTurn();
                           });
    } else {
      startTurn();
    }
  }
}

void ShellWidget::chooseAttachments() {
  const QString initialDirectory =
      text(turnSettings->workspace(QDir::currentPath().toStdString()));
  FileSelectionDialog dialog(FileSelectionDialog::Mode::Attachments,
                             initialDirectory, attachmentDrafts, this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  attachmentDrafts = dialog.selectedAttachments();
  ++attachmentRevision;
  refreshAttachments();
}

void ShellWidget::refreshAttachments() {
  const bool hasAttachments = !attachmentDrafts.empty();
  attachmentPanel->setVisible(hasAttachments);
  clearLayout(attachmentListLayout);
  if (!hasAttachments) {
    attachmentListScroll->setFixedHeight(0);
    return;
  }
  constexpr int AttachmentRowHeight = 28;
  constexpr int MaximumVisibleAttachments = 4;
  for (std::size_t index = 0; index < attachmentDrafts.size(); ++index) {
    const AttachmentDraft &attachment = attachmentDrafts[index];
    auto *row = new QWidget;
    row->setFixedHeight(AttachmentRowHeight);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 2, 0, 2);
    rowLayout->setSpacing(5);
    auto *remove = new QPushButton(QStringLiteral("X"));
    remove->setAccessibleName(QStringLiteral("Remove %1").arg(attachment.name));
    remove->setToolTip(QStringLiteral("Remove attachment"));
    remove->setFixedSize(18, 18);
    remove->setStyleSheet(
        QStringLiteral("QPushButton{background:#b83a3a;color:#ffffff;border:0;"
                       "border-radius:4px;padding:0;"
                       "font-weight:700;}"
                       "QPushButton:hover{background:#9f2f2f;}"
                       "QPushButton:pressed{background:#842626;}"));
    connect(remove, &QPushButton::clicked, this, [this, index] {
      attachmentDrafts.erase(attachmentDrafts.begin() +
                             static_cast<std::ptrdiff_t>(index));
      ++attachmentRevision;
      refreshAttachments();
    });
    auto *fileBox = new QFrame;
    fileBox->setObjectName(QStringLiteral("attachmentFileBox"));
    fileBox->setStyleSheet(
        QStringLiteral("QFrame#attachmentFileBox{background:#ffffff;"
                       "border:1px solid #d7dee8;border-radius:6px;}"));
    auto *fileLayout = new QHBoxLayout(fileBox);
    fileLayout->setContentsMargins(8, 1, 8, 1);
    auto *name = makeLabel(attachment.name, "meta");
    name->setToolTip(QDir::toNativeSeparators(attachment.path));
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    fileLayout->addWidget(name);
    rowLayout->addWidget(fileBox, 1);
    rowLayout->addWidget(remove, 0, Qt::AlignVCenter);
    attachmentListLayout->addWidget(row);
  }
  const int visibleRows = std::min<int>(
      static_cast<int>(attachmentDrafts.size()), MaximumVisibleAttachments);
  attachmentListScroll->setFixedHeight(visibleRows * AttachmentRowHeight +
                                       (visibleRows - 1) * 4);
}

void ShellWidget::interruptActiveTurn() {
  const auto turnId = model.activeTurnId(selectedThreadId);
  if (!turnId)
    return;
  session.interruptTurn(selectedThreadId, *turnId);
}

void ShellWidget::respondToFirstPending(bool approve) {
  const auto &pending = model.pendingRequestPresentations();
  const auto request =
      std::find_if(pending.begin(), pending.end(), [this](const auto &entry) {
        return entry.second.threadId == selectedThreadId;
      });
  if (request == pending.end())
    return;

  if (approve)
    reviewPending(request->first);
  else
    rejectPending(request->first);
}

void ShellWidget::reviewPending(const std::string &requestKey) {
  const auto request = model.pendingRequestPresentations().find(requestKey);
  if (request == model.pendingRequestPresentations().end())
    return;
  const auto response = PendingRequestDialog::present(request->second, this);
  if (!response)
    return;
  session.respondToServerRequest(nlohmann::json::parse(requestKey),
                                 response->result, response->error);
}

void ShellWidget::rejectPending(const std::string &requestKey) {
  const auto request = model.pendingRequestPresentations().find(requestKey);
  if (request == model.pendingRequestPresentations().end())
    return;
  PendingRequestResponse response =
      PendingRequestDialog::negativeResponse(request->second);
  session.respondToServerRequest(nlohmann::json::parse(requestKey),
                                 std::move(response.result),
                                 std::move(response.error));
}

} // namespace codexui::codex
