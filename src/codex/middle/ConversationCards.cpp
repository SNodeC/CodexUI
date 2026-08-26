// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"

#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int MaximumCommandOutputHeight = 220;
constexpr int MaximumCommandTextHeight = 90;
constexpr int CommandTextPadding = 7;
constexpr int PendingAnimationIntervalMilliseconds = 32;
constexpr qint64 PendingHalfCycleMilliseconds = 850;
constexpr int ThumbnailMaximumWidth = 280;
constexpr int ThumbnailMaximumHeight = 180;
constexpr int ViewerMaximumImageExtent = 4096;

void openImageViewer(const QString &path);

class ImageThumbnail final : public QLabel {
public:
  ImageThumbnail(QString path, QWidget *parent)
      : QLabel(parent), path_(std::move(path)) {
    setObjectName(QStringLiteral("messageImageThumbnail"));
    setProperty("kind", "imageThumbnail");
    setCursor(Qt::PointingHandCursor);
    setToolTip(QDir::toNativeSeparators(path_));
    setAlignment(Qt::AlignCenter);
    setMinimumSize(72, 48);
    setMaximumSize(ThumbnailMaximumWidth, ThumbnailMaximumHeight);

    QImageReader reader(path_);
    reader.setAutoTransform(true);
    const QSize source = reader.size();
    if (source.isValid())
      reader.setScaledSize(source.scaled(ThumbnailMaximumWidth - 8,
                                         ThumbnailMaximumHeight - 8,
                                         Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull()) {
      setText(QStringLiteral("Image unavailable\n%1")
                  .arg(QFileInfo(path_).fileName()));
      setProperty("imageAvailable", false);
      unsetCursor();
      path_.clear();
      return;
    }
    setProperty("imageAvailable", true);
    setPixmap(QPixmap::fromImage(image));
    setFixedSize(image.size() + QSize(8, 8));
  }

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && !path_.isEmpty()) {
      openImageViewer(path_);
      event->accept();
      return;
    }
    QLabel::mousePressEvent(event);
  }

private:
  QString path_;
};

class ImageViewer final : public QDialog {
public:
  explicit ImageViewer(const QString &path) : QDialog(nullptr, Qt::Window) {
    setObjectName(QStringLiteral("messageImageViewer"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    setWindowTitle(QFileInfo(path).fileName());
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    imageLabel_ = new QLabel(scroll_);
    imageLabel_->setObjectName(QStringLiteral("messageImageViewerImage"));
    imageLabel_->setAlignment(Qt::AlignCenter);

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize source = reader.size();
    if (source.isValid() && (source.width() > ViewerMaximumImageExtent ||
                             source.height() > ViewerMaximumImageExtent))
      reader.setScaledSize(source.scaled(ViewerMaximumImageExtent,
                                         ViewerMaximumImageExtent,
                                         Qt::KeepAspectRatio));
    image_ = reader.read();
    if (image_.isNull())
      imageLabel_->setText(QStringLiteral("Image unavailable"));
    scroll_->setWidget(imageLabel_);
    layout->addWidget(scroll_);
    resize(900, 650);
    updatePixmap();
  }

protected:
  void showEvent(QShowEvent *event) override {
    QDialog::showEvent(event);
    updatePixmap();
  }

  void resizeEvent(QResizeEvent *event) override {
    QDialog::resizeEvent(event);
    updatePixmap();
  }

private:
  void updatePixmap() {
    if (image_.isNull() || !scroll_)
      return;
    const QSize available = scroll_->viewport()->size() - QSize(8, 8);
    if (available.isEmpty())
      return;
    imageLabel_->setPixmap(QPixmap::fromImage(
        image_.scaled(available, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation)));
  }

  QImage image_;
  QScrollArea *scroll_ = nullptr;
  QLabel *imageLabel_ = nullptr;
};

void openImageViewer(const QString &path) {
  auto *viewer = new ImageViewer(path);
  viewer->show();
}

QLabel *makeLabel(const QString &value, const char *kind = "body",
                  QWidget *parent = nullptr) {
  auto *label = new QLabel(value, parent);
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QString markdownHtml(const QString &markdown) {
  QTextDocument document;
  document.setMarkdown(
      markdown,
      QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
          QTextDocument::MarkdownNoHTML);
  return document.toHtml();
}

QLabel *makeMarkdownLabel(const QString &value, QWidget *parent = nullptr) {
  auto *label = new QLabel(parent);
  label->setProperty("kind", "body");
  label->setTextFormat(Qt::RichText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setOpenExternalLinks(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::LinksAccessibleByMouse);
  label->setProperty("markdownSource", value);
  label->setText(markdownHtml(value));
  return label;
}

bool setVisibleText(QLabel *label, const QString &text) {
  const bool visible = !text.isEmpty();
  const bool explicitlyVisible = !label->isHidden();
  const bool changed = label->text() != text || explicitlyVisible != visible;
  if (label->text() != text)
    label->setText(text);
  if (explicitlyVisible != visible)
    label->setVisible(visible);
  return changed;
}

bool setVisibleMarkdown(QLabel *label, const QString &markdown) {
  const bool visible = !markdown.isEmpty();
  const bool contentChanged =
      label->property("markdownSource").toString() != markdown;
  const bool explicitlyVisible = !label->isHidden();
  const bool changed = contentChanged || explicitlyVisible != visible;
  if (contentChanged) {
    label->setProperty("markdownSource", markdown);
    label->setText(markdownHtml(markdown));
  }
  if (explicitlyVisible != visible)
    label->setVisible(visible);
  return changed;
}

QString displayStatus(const QString &status) {
  if (status == QStringLiteral("inProgress") ||
      status == QStringLiteral("active"))
    return QStringLiteral("Running");
  if (status == QStringLiteral("completed") || status == QStringLiteral("idle"))
    return QStringLiteral("Completed");
  if (status == QStringLiteral("failed") ||
      status == QStringLiteral("systemError"))
    return QStringLiteral("Failed");
  if (status.isEmpty())
    return QStringLiteral("Unknown");
  return status;
}

QString commandMetadata(const CommandExecutionData &command) {
  QStringList metadata{displayStatus(command.status)};
  if (command.exitCode)
    metadata << QStringLiteral("exit %1").arg(*command.exitCode);
  if (!command.cwd.isEmpty())
    metadata << command.cwd;
  return metadata.join(QStringLiteral("  |  "));
}

QString agentMetadata(const AgentActivityData &activity) {
  QStringList metadata;
  if (!activity.tool.isEmpty())
    metadata << activity.tool;
  metadata << displayStatus(activity.status.isEmpty() ? activity.kind
                                                      : activity.status);
  if (!activity.receivers.isEmpty())
    metadata << activity.receivers.join(QStringLiteral(", "));
  return metadata.join(QStringLiteral("  |  "));
}

bool acceptedTransitionActive(const LocalPromptData &prompt, qint64 now) {
  return prompt.acceptedTransitionActive(now);
}

bool presentationEquals(const VisibleCardData &left,
                        const VisibleCardData &right) {
  if (left.kind != right.kind || left.payload.index() != right.payload.index())
    return false;
  switch (left.kind) {
  case CardKind::UserMessage:
    return std::get<UserMessageData>(left.payload) ==
           std::get<UserMessageData>(right.payload);
  case CardKind::AgentMessage:
    return std::get<AgentMessageData>(left.payload) ==
           std::get<AgentMessageData>(right.payload);
  case CardKind::CommandExecution:
    return std::get<CommandExecutionData>(left.payload) ==
           std::get<CommandExecutionData>(right.payload);
  case CardKind::AgentActivity:
    return std::get<AgentActivityData>(left.payload) ==
           std::get<AgentActivityData>(right.payload);
  case CardKind::Reasoning:
    return std::get<ReasoningData>(left.payload) ==
           std::get<ReasoningData>(right.payload);
  case CardKind::FileChanges: {
    const auto &first = std::get<FileChangesData>(left.payload);
    const auto &second = std::get<FileChangesData>(right.payload);
    // The card presents the aggregate status and path count. The detailed
    // change JSON belongs to the Changes inspector and is deliberately not a
    // conversation-card invalidation source.
    return first.status == second.status && first.pathCount == second.pathCount;
  }
  case CardKind::Plan:
    return std::get<PlanData>(left.payload) ==
           std::get<PlanData>(right.payload);
  case CardKind::GenericActivity:
    return std::get<GenericActivityData>(left.payload) ==
           std::get<GenericActivityData>(right.payload);
  case CardKind::LocalPrompt: {
    const auto &first = std::get<LocalPromptData>(left.payload);
    const auto &second = std::get<LocalPromptData>(right.payload);
    return first.prompt == second.prompt &&
           first.attachmentCount == second.attachmentCount &&
           first.imagePaths == second.imagePaths &&
           first.state == second.state &&
           first.acceptedAtMilliseconds == second.acceptedAtMilliseconds &&
           first.error == second.error;
  }
  }
  return false;
}

} // namespace

ContentSizedTextView::ContentSizedTextView(int maximumContentHeight,
                                           QWidget *parent)
    : QTextEdit(parent) {
  setReadOnly(true);
  setAcceptRichText(false);
  setMinimumHeight(0);
  setMaximumHeight(maximumContentHeight);
  setLineWrapMode(QTextEdit::WidgetWidth);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  document()->setDocumentMargin(CommandTextPadding);
}

bool ContentSizedTextView::setContent(const QString &content) {
  if (toPlainText() == content)
    return false;
  setPlainText(content);
  measureAtCurrentWidth(true);
  return true;
}

QSize ContentSizedTextView::sizeHint() const {
  QSize result = QTextEdit::sizeHint();
  result.setHeight(preferredHeight_);
  return result;
}

QSize ContentSizedTextView::minimumSizeHint() const {
  QSize result = QTextEdit::minimumSizeHint();
  result.setHeight(0);
  return result;
}

void ContentSizedTextView::resizeEvent(QResizeEvent *event) {
  QTextEdit::resizeEvent(event);
  // Wrapping is authoritative only after QTextEdit has assigned its
  // viewport width. Propagate a changed hint immediately so a multiline view
  // cannot remain at an earlier one-line height with a premature scrollbar.
  measureAtCurrentWidth(true);
}

void ContentSizedTextView::measureAtCurrentWidth(bool notifyParent) {
  const QString content = toPlainText();
  int wantedHeight = 0;
  if (!content.isEmpty()) {
    const int frame = 2 * frameWidth();
    document()->setTextWidth(std::max(1, viewport()->width()));
    wantedHeight =
        frame + static_cast<int>(std::ceil(document()->size().height()));
  }
  wantedHeight = std::clamp(wantedHeight, 0, maximumHeight());
  if (wantedHeight == preferredHeight_)
    return;
  preferredHeight_ = wantedHeight;
  if (notifyParent)
    updateGeometry();
}

CommandOutputView::CommandOutputView(const QString &output, QWidget *parent)
    : ContentSizedTextView(MaximumCommandOutputHeight, parent) {
  setProperty("kind", "code");
  setObjectName(QStringLiteral("commandOutputView"));
  setStyleSheet(QStringLiteral(
      "QTextEdit#commandOutputView{background:#111827;color:#e5e7eb;"
      "border-radius:6px;}"));

  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            if (programmaticScroll_)
              return;
            preservedScrollValue_ = value;
            followsLatest_ = isAtBottom();
          });
  connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int) {
            if (!programmaticScroll_)
              settleScroll();
          });

  setOutput(output);
  measureAtCurrentWidth(false);
  settleScroll();
}

CommandOutputView::ScrollState CommandOutputView::scrollState() const {
  return {followsLatest_, preservedScrollValue_};
}

bool CommandOutputView::followsLatest() const noexcept {
  return followsLatest_;
}

bool CommandOutputView::setOutput(const QString &output) {
  const QString displayOutput = trimTrailingEmptyLines(output);
  if (currentOutput_ == displayOutput)
    return false;

  const bool retainedFollow = followsLatest_;
  const int retainedValue = preservedScrollValue_;
  const bool appendOnly =
      !currentOutput_.isEmpty() && displayOutput.startsWith(currentOutput_);
  programmaticScroll_ = true;
  if (appendOnly) {
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(displayOutput.sliced(currentOutput_.size()));
  } else {
    setPlainText(displayOutput);
  }
  currentOutput_ = displayOutput;
  followsLatest_ = retainedFollow;
  preservedScrollValue_ = retainedValue;
  programmaticScroll_ = false;
  // Asking the document layout for its size here completes wrapping at the
  // already assigned viewport width.  The enclosing conversation can then
  // account for the final card height in the same reconciliation transaction.
  measureAtCurrentWidth(true);
  settleScroll();
  return true;
}

void CommandOutputView::restoreScrollState(const ScrollState &state) {
  followsLatest_ = state.followsLatest;
  preservedScrollValue_ = std::max(0, state.value);
  settleScroll();
}

void CommandOutputView::wheelEvent(QWheelEvent *event) {
  QScrollBar *bar = verticalScrollBar();
  const int delta = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                  : event->angleDelta().y();
  if (bar->maximum() <= bar->minimum() ||
      (delta > 0 && bar->value() <= bar->minimum()) ||
      (delta < 0 && bar->value() >= bar->maximum())) {
    event->accept();
    return;
  }

  if (delta > 0)
    followsLatest_ = false;
  QTextEdit::wheelEvent(event);
  preservedScrollValue_ = bar->value();
  followsLatest_ = isAtBottom();
}

void CommandOutputView::settleScroll() {
  if (settlingScroll_)
    return;
  settlingScroll_ = true;
  QScrollBar *bar = verticalScrollBar();
  const bool wasProgrammatic = programmaticScroll_;
  programmaticScroll_ = true;
  if (followsLatest_) {
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
    ensureCursorVisible();
  }
  const int target =
      followsLatest_
          ? bar->maximum()
          : std::clamp(preservedScrollValue_, bar->minimum(), bar->maximum());
  bar->setValue(target);
  preservedScrollValue_ = target;
  programmaticScroll_ = wasProgrammatic;
  settlingScroll_ = false;
}

bool CommandOutputView::isAtBottom() const {
  return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
}

class ConversationCard::Impl final {
public:
  Impl(ConversationCard *owner, const VisibleCardData &initial)
      : owner(owner), current(initial) {
    owner->setObjectName(QStringLiteral("conversationCard"));
    owner->setProperty("conversationCardKey",
                       QString::fromStdString(stableKey(initial.key)));
    owner->setProperty("conversationCardKind", static_cast<int>(initial.kind));

    layout = new QVBoxLayout(owner);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    createChildren(initial.kind);
    applyPayload(initial);
  }

  bool apply(const VisibleCardData &next) {
    if (current.key != next.key || current.kind != next.kind) {
      Q_ASSERT_X(false, "ConversationCard::apply",
                 "a persistent conversation card cannot change key or kind");
      return false;
    }
    const bool presentationChanged = !presentationEquals(current, next);
    current = next;
    if (!presentationChanged)
      return false;
    applyPayload(next);
    owner->updateGeometry();
    owner->update();
    return true;
  }

  void createChildren(CardKind kind) {
    owner->setProperty("kind", "raised");
    switch (kind) {
    case CardKind::UserMessage:
      owner->setProperty("messageRole", "user");
      title = makeLabel(QStringLiteral("You"), "title", owner);
      body = makeMarkdownLabel({}, owner);
      layout->addWidget(title);
      layout->addWidget(body);
      createImageContainer();
      break;
    case CardKind::AgentMessage:
      owner->setProperty("messageRole", "agent");
      title = makeLabel({}, "title", owner);
      body = makeMarkdownLabel({}, owner);
      layout->addWidget(title);
      layout->addWidget(body);
      break;
    case CardKind::CommandExecution:
      title = makeLabel(QStringLiteral("Command execution"), "title", owner);
      command = new ContentSizedTextView(MaximumCommandTextHeight, owner);
      command->setProperty("kind", "command");
      command->setObjectName(QStringLiteral("commandTextView"));
      command->setStyleSheet(QStringLiteral(
          "QTextEdit#commandTextView{background:#f8fafc;"
          "border:1px solid #d7dee8;border-radius:6px;}"));
      output = new CommandOutputView({}, owner);
      output->hide();
      metadata = makeLabel({}, "meta", owner);
      metadata->setObjectName(QStringLiteral("commandMetadata"));
      layout->addWidget(title);
      layout->addWidget(command);
      layout->addWidget(output);
      layout->addWidget(metadata);
      break;
    case CardKind::AgentActivity:
      title = makeLabel(QStringLiteral("Agent activity"), "title", owner);
      metadata = makeLabel({}, "meta", owner);
      body = makeLabel({}, "body", owner);
      detail = makeMarkdownLabel({}, owner);
      layout->addWidget(title);
      layout->addWidget(metadata);
      layout->addWidget(body);
      layout->addWidget(detail);
      break;
    case CardKind::Reasoning:
      title = makeLabel(QStringLiteral("Reasoning"), "title", owner);
      body = makeMarkdownLabel({}, owner);
      layout->addWidget(title);
      layout->addWidget(body);
      break;
    case CardKind::FileChanges:
      title = makeLabel(QStringLiteral("File changes"), "title", owner);
      metadata = makeLabel({}, "meta", owner);
      layout->addWidget(title);
      layout->addWidget(metadata);
      break;
    case CardKind::Plan:
      title = makeLabel(QStringLiteral("plan"), "title", owner);
      body = makeMarkdownLabel({}, owner);
      layout->addWidget(title);
      layout->addWidget(body);
      break;
    case CardKind::GenericActivity:
      title = makeLabel({}, "title", owner);
      metadata = makeLabel({}, "meta", owner);
      layout->addWidget(title);
      layout->addWidget(metadata);
      break;
    case CardKind::LocalPrompt:
      owner->setObjectName(QStringLiteral("pendingPromptCard"));
      owner->setStyleSheet(QStringLiteral(
          "QFrame#pendingPromptCard{background:transparent;border:0;}"));
      title = makeLabel(QStringLiteral("You"), "title", owner);
      body = makeLabel({}, "body", owner);
      metadata = makeLabel({}, "meta", owner);
      layout->addWidget(title);
      layout->addWidget(body);
      layout->addWidget(metadata);
      createImageContainer();
      animationTimer = new QTimer(owner);
      animationTimer->setInterval(PendingAnimationIntervalMilliseconds);
      QObject::connect(animationTimer, &QTimer::timeout, owner, [this] {
        if (refreshPendingPresentation())
          owner->updateGeometry();
        owner->update();
      });
      break;
    }
  }

  void createImageContainer() {
    images = new QWidget(owner);
    images->setObjectName(QStringLiteral("messageImages"));
    imageLayout = new QVBoxLayout(images);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->setSpacing(8);
    imageLayout->setAlignment(Qt::AlignLeft);
    images->hide();
    layout->addWidget(images);
  }

  void setImages(const QStringList &paths) {
    while (QLayoutItem *item = imageLayout->takeAt(0)) {
      delete item->widget();
      delete item;
    }
    for (const QString &path : paths) {
      auto *thumbnail = new ImageThumbnail(path, images);
      imageLayout->addWidget(thumbnail, 0, Qt::AlignLeft);
    }
    images->setVisible(!paths.isEmpty());
  }

  void applyPayload(const VisibleCardData &data) {
    switch (data.kind) {
    case CardKind::UserMessage: {
      const auto &message = std::get<UserMessageData>(data.payload);
      setVisibleMarkdown(body, message.text);
      setImages(message.imagePaths);
      break;
    }
    case CardKind::AgentMessage: {
      const auto &message = std::get<AgentMessageData>(data.payload);
      title->setText(message.finalAnswer ? QStringLiteral("Codex")
                                         : QStringLiteral("Codex activity"));
      layout->setContentsMargins(12, message.finalAnswer ? 10 : 8, 12,
                                 message.finalAnswer ? 10 : 8);
      setVisibleMarkdown(body, message.text);
      break;
    }
    case CardKind::CommandExecution: {
      const auto &execution = std::get<CommandExecutionData>(data.payload);
      const QString displayCommand = trimTrailingEmptyLines(execution.command);
      command->setContent(displayCommand);
      command->setVisible(!displayCommand.isEmpty());
      const QString displayOutput = trimTrailingEmptyLines(execution.output);
      const bool visibleOutput = terminalOutputHasVisibleText(displayOutput);
      if (visibleOutput) {
        output->setOutput(displayOutput);
        output->show();
      } else {
        output->hide();
        output->setOutput({});
        // Once the surface ceases to exist there is no user-owned paused
        // position to retain. If visible output appears later it starts in
        // the documented follow-latest state.
        output->restoreScrollState({true, 0});
      }
      metadata->setText(commandMetadata(execution));
      metadata->show();
      break;
    }
    case CardKind::AgentActivity: {
      const auto &activity = std::get<AgentActivityData>(data.payload);
      metadata->setText(agentMetadata(activity));
      metadata->show();
      setVisibleText(body, activity.prompt);
      setVisibleMarkdown(detail, activity.resultText);
      break;
    }
    case CardKind::Reasoning: {
      const auto &reasoning = std::get<ReasoningData>(data.payload);
      setVisibleMarkdown(body, reasoning.summary);
      break;
    }
    case CardKind::FileChanges: {
      const auto &changes = std::get<FileChangesData>(data.payload);
      QStringList values{displayStatus(changes.status)};
      values << QStringLiteral("%1 paths").arg(changes.pathCount);
      metadata->setText(values.join(QStringLiteral("  |  ")));
      metadata->show();
      break;
    }
    case CardKind::Plan: {
      const auto &plan = std::get<PlanData>(data.payload);
      setVisibleMarkdown(body, plan.text);
      break;
    }
    case CardKind::GenericActivity: {
      const auto &activity = std::get<GenericActivityData>(data.payload);
      title->setText(activity.type.isEmpty() ? QStringLiteral("Activity")
                                             : activity.type);
      metadata->setText(QString::fromStdString(activity.raw.dump(2)));
      metadata->show();
      break;
    }
    case CardKind::LocalPrompt: {
      const auto &prompt = std::get<LocalPromptData>(data.payload);
      body->setText(prompt.prompt);
      body->show();
      setImages(prompt.imagePaths);
      refreshPendingPresentation();
      break;
    }
    }
  }

  bool refreshPendingPresentation() {
    const auto *prompt = std::get_if<LocalPromptData>(&current.payload);
    if (!prompt)
      return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool transitioning = acceptedTransitionActive(*prompt, now);
    const bool waiting = prompt->state == PromptState::Queued ||
                         prompt->state == PromptState::InFlight;
    const bool failed = prompt->state == PromptState::Failed;
    const QString foreground = waiting || transitioning
                                   ? QStringLiteral("#536b8f")
                               : failed ? QStringLiteral("#982f3d")
                                        : QStringLiteral("#1d2633");
    const QString style =
        QStringLiteral("background:transparent;color:%1;").arg(foreground);
    bool changed = false;
    for (QLabel *label : {title, body, metadata}) {
      if (label->styleSheet() != style) {
        label->setStyleSheet(style);
        changed = true;
      }
    }

    QString status;
    if (failed)
      status = prompt->error.isEmpty()
                   ? QStringLiteral("Not sent")
                   : QStringLiteral("Not sent: %1").arg(prompt->error);

    if (prompt->attachmentCount > 0) {
      const QString attachments =
          QStringLiteral("%1 attachment%2")
              .arg(prompt->attachmentCount)
              .arg(prompt->attachmentCount == 1 ? QString{}
                                                : QStringLiteral("s"));
      status = status.isEmpty()
                   ? attachments
                   : status + QStringLiteral("  |  ") + attachments;
    }
    changed = setVisibleText(metadata, status) || changed;

    if (waiting || transitioning) {
      if (!animationTimer->isActive())
        animationTimer->start();
    } else {
      animationTimer->stop();
    }
    return changed;
  }

  ConversationCard *owner = nullptr;
  VisibleCardData current;
  QVBoxLayout *layout = nullptr;
  QLabel *title = nullptr;
  QLabel *body = nullptr;
  QLabel *metadata = nullptr;
  QLabel *detail = nullptr;
  ContentSizedTextView *command = nullptr;
  CommandOutputView *output = nullptr;
  QTimer *animationTimer = nullptr;
  QWidget *images = nullptr;
  QVBoxLayout *imageLayout = nullptr;
};

ConversationCard::ConversationCard(const VisibleCardData &data, QWidget *parent)
    : QFrame(parent), impl_(std::make_unique<Impl>(this, data)) {}

ConversationCard::~ConversationCard() = default;

CardKind ConversationCard::cardKind() const noexcept {
  return impl_->current.kind;
}

const VisibleCardData &ConversationCard::data() const noexcept {
  return impl_->current;
}

std::optional<CommandOutputView::ScrollState>
ConversationCard::commandOutputScrollState() const {
  if (!impl_->output)
    return std::nullopt;
  return impl_->output->scrollState();
}

void ConversationCard::restoreCommandOutputScrollState(
    const CommandOutputView::ScrollState &state) {
  if (impl_->output)
    impl_->output->restoreScrollState(state);
}

bool ConversationCard::apply(const VisibleCardData &data) {
  return impl_->apply(data);
}

void ConversationCard::paintEvent(QPaintEvent *event) {
  QFrame::paintEvent(event);
  if (impl_->current.kind != CardKind::LocalPrompt)
    return;
  const auto *prompt = std::get_if<LocalPromptData>(&impl_->current.payload);
  if (!prompt)
    return;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF bounds = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool waiting = prompt->state == PromptState::Queued ||
                       prompt->state == PromptState::InFlight;
  const bool transitioning = acceptedTransitionActive(*prompt, now);
  const bool failed = prompt->state == PromptState::Failed;
  const QColor background = waiting || transitioning
                                ? QColor(QStringLiteral("#dbe7f8"))
                            : failed ? QColor(QStringLiteral("#fff0f2"))
                                     : QColor(QStringLiteral("#eaf2ff"));
  const QColor border = waiting || transitioning
                            ? QColor(QStringLiteral("#9eb9df"))
                        : failed ? QColor(QStringLiteral("#efb8c0"))
                                 : QColor(QStringLiteral("#bfd3f9"));
  painter.setBrush(background);
  painter.setPen(QPen(border, 1.0));
  painter.drawRoundedRect(bounds, 8.0, 8.0);

  if (!waiting && !transitioning)
    return;

  const qint64 phase = now % (2 * PendingHalfCycleMilliseconds);
  const qreal position =
      waiting ? phase <= PendingHalfCycleMilliseconds
                    ? qreal(phase) / PendingHalfCycleMilliseconds
                    : qreal(2 * PendingHalfCycleMilliseconds - phase) /
                          PendingHalfCycleMilliseconds
              : std::clamp(qreal(now - prompt->acceptedAtMilliseconds) /
                               AcknowledgementTransitionMilliseconds,
                           0.0, 1.0);
  const qreal center = bounds.left() + position * bounds.width();
  const qreal radius = std::max(28.0, bounds.width() * 0.24);
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

ConversationCard *createConversationCard(const VisibleCardData &data,
                                         QWidget *parent) {
  return new ConversationCard(data, parent);
}

} // namespace codexui::codex::middle
