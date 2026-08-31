// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"

#include "codex/PresentationStatus.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariant>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <type_traits>
#include <unordered_set>
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
constexpr qsizetype MaximumGenericActivityCharacters = 4096;
constexpr int CardHeaderActionSpacing = 4;
constexpr int CopyMorphDurationMilliseconds = 160;
constexpr int CopyCheckHoldMilliseconds = 1000;

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QStringList textList(const std::vector<std::string> &values) {
  QStringList result;
  result.reserve(static_cast<qsizetype>(values.size()));
  for (const std::string &value : values)
    result.push_back(text(value));
  return result;
}

std::string utf8(const QString &value) { return value.toUtf8().toStdString(); }

QString trimmedTrailingLines(const QString &value) {
  return text(trimTrailingEmptyLines(utf8(value)));
}

bool initiallyCollapsed(CardKind kind, bool commandInitiallyCollapsed,
                        bool imageInitiallyCollapsed) {
  if (kind == CardKind::CommandExecution)
    return commandInitiallyCollapsed;
  if (kind == CardKind::ImageGeneration)
    return imageInitiallyCollapsed;
  return kind != CardKind::UserMessage && kind != CardKind::AgentMessage &&
         kind != CardKind::LocalPrompt;
}

class CardDisclosureButton final : public QToolButton {
public:
  explicit CardDisclosureButton(QWidget *parent = nullptr)
      : QToolButton(parent) {
    setObjectName(QStringLiteral("cardDisclosureButton"));
    setProperty("kind", "subtle");
    setFixedSize(14, 24);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Expand card"));
    setToolTip(accessibleName());
    setProperty("chevronDirection", "left");
  }

  void setExpanded(bool expanded) {
    if (expanded_ == expanded)
      return;
    expanded_ = expanded;
    setAccessibleName(expanded ? QStringLiteral("Collapse card")
                               : QStringLiteral("Expand card"));
    setToolTip(accessibleName());
    setProperty("chevronDirection", expanded ? "down" : "left");
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    static_cast<void>(event);
    QRect indicator(0, 3, 12, height() - 6);
    // Keep the visible stroke at the accepted card-right inset. The narrower
    // left glyph needs two pixels more optical compensation than the down
    // glyph, while the compact control width avoids artificial action gaps.
    indicator.translate(expanded_ ? 3 : 5, 0);
    UiStyle::drawChevron(this, indicator, isEnabled(),
                         underMouse() || hasFocus(),
                         expanded_ ? UiStyle::ChevronDirection::Down
                                   : UiStyle::ChevronDirection::Left);
  }

private:
  bool expanded_ = false;
};

class CardCopyButton final : public QToolButton {
public:
  explicit CardCopyButton(QWidget *parent = nullptr) : QToolButton(parent) {
    setObjectName(QStringLiteral("cardCopyButton"));
    setFixedSize(16, 24);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Copy card content"));
    setToolTip(accessibleName());

    morph_ = new QVariantAnimation(this);
    morph_->setDuration(CopyMorphDurationMilliseconds);
    morph_->setEasingCurve(QEasingCurve::InOutCubic);
    QObject::connect(
        morph_, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &value) {
          morphProgress_ = value.toReal();
          update();
        });
    QObject::connect(morph_, &QVariantAnimation::finished, this, [this] {
      if (returningToCopy_) {
        finishFeedback();
        return;
      }
      setProperty("copyIconState", QStringLiteral("check"));
      hold_->start(CopyCheckHoldMilliseconds);
    });
    hold_ = new QTimer(this);
    hold_->setSingleShot(true);
    QObject::connect(hold_, &QTimer::timeout, this, [this] {
      returningToCopy_ = true;
      if (animationsEnabled()) {
        morph_->setStartValue(morphProgress_);
        morph_->setEndValue(0.0);
        morph_->start();
      } else {
        finishFeedback();
      }
    });
  }

  void showCopiedFeedback() {
    morph_->stop();
    hold_->stop();
    returningToCopy_ = false;
    setProperty("copyFeedbackActive", true);
    setProperty("copyIconState", QStringLiteral("morphing"));
    if (animationsEnabled()) {
      morph_->setStartValue(morphProgress_);
      morph_->setEndValue(1.0);
      morph_->start();
    } else {
      morphProgress_ = 1.0;
      setProperty("copyIconState", QStringLiteral("check"));
      hold_->start(CopyCheckHoldMilliseconds);
    }
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, height())),
                       QStringLiteral("Copied"), this, rect(),
                       CopyCheckHoldMilliseconds);
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    static_cast<void>(event);
    QColor color(QStringLiteral("#667085"));
    if (!isEnabled())
      color = QColor(QStringLiteral("#98a2b3"));
    else if (underMouse() || hasFocus())
      color = QColor(QStringLiteral("#1d2633"));
    if (property("copyFeedbackActive").toBool())
      color = QColor(QStringLiteral("#176b45"));

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(
        QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.save();
    painter.setOpacity(1.0 - morphProgress_);
    painter.drawRoundedRect(QRectF(4.5, 5.5, 8.0, 9.0), 1.2, 1.2);
    painter.drawRoundedRect(QRectF(7.5, 8.5, 8.0, 9.0), 1.2, 1.2);
    painter.restore();
    painter.setOpacity(morphProgress_);
    QPainterPath check;
    check.moveTo(4.0, 12.0);
    check.lineTo(7.5, 15.5);
    check.lineTo(15.0, 7.5);
    painter.drawPath(check);
  }

private:
  [[nodiscard]] bool animationsEnabled() const {
    return style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr,
                              this) > 0;
  }

  void finishFeedback() {
    returningToCopy_ = false;
    morphProgress_ = 0.0;
    setProperty("copyFeedbackActive", false);
    setProperty("copyIconState", QStringLiteral("copy"));
    update();
  }

  QVariantAnimation *morph_ = nullptr;
  QTimer *hold_ = nullptr;
  qreal morphProgress_ = 0.0;
  bool returningToCopy_ = false;
};

void openImageViewer(const QString &path);

class ImageThumbnail final : public QLabel {
public:
  ImageThumbnail(QString path, QWidget *parent)
      : QLabel(parent), path_(std::move(path)) {
    setObjectName(QStringLiteral("messageImageThumbnail"));
    setProperty("kind", "imageThumbnail");
    setCursor(Qt::PointingHandCursor);
    setToolTip(QDir::toNativeSeparators(path_));
    setAccessibleDescription(QDir::toNativeSeparators(path_));
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
      setAccessibleName(QStringLiteral("Image unavailable: %1")
                            .arg(QFileInfo(path_).fileName()));
      setText(QStringLiteral("Image unavailable\n%1")
                  .arg(QFileInfo(path_).fileName()));
      setProperty("imageAvailable", false);
      setFocusPolicy(Qt::NoFocus);
      unsetCursor();
      return;
    }
    setAccessibleName(
        QStringLiteral("Open image: %1").arg(QFileInfo(path_).fileName()));
    setFocusPolicy(Qt::StrongFocus);
    setProperty("imageAvailable", true);
    setPixmap(QPixmap::fromImage(image));
    setFixedSize(image.size() + QSize(8, 8));
  }

  [[nodiscard]] bool represents(const QString &path) const {
    return path_ == path &&
           property("imageAvailable").toBool() == QFileInfo(path).isFile();
  }

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton &&
        property("imageAvailable").toBool()) {
      leftPressArmed_ = true;
      setFocus(Qt::MouseFocusReason);
      event->accept();
      return;
    }
    leftPressArmed_ = false;
    QLabel::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && leftPressArmed_) {
      leftPressArmed_ = false;
      if (rect().contains(event->position().toPoint()))
        activate();
      event->accept();
      return;
    }
    QLabel::mouseReleaseEvent(event);
  }

  void keyPressEvent(QKeyEvent *event) override {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
         event->key() == Qt::Key_Space) &&
        activate()) {
      event->accept();
      return;
    }
    QLabel::keyPressEvent(event);
  }

private:
  bool activate() {
    if (!property("imageAvailable").toBool())
      return false;
    openImageViewer(path_);
    return true;
  }

  QString path_;
  bool leftPressArmed_ = false;
};

class ImageRibbon final : public QScrollArea {
public:
  explicit ImageRibbon(QWidget *parent = nullptr) : QScrollArea(parent) {
    setObjectName(QStringLiteral("messageImages"));
    setFrameShape(QFrame::StyledPanel);
    setWidgetResizable(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setStyleSheet(
        QStringLiteral("QScrollArea#messageImages{background:#111827;"
                       "border:1px solid #d7dee8;border-radius:6px;}"
                       "QWidget#messageImageStrip{background:#111827;}"));

    strip_ = new QWidget;
    strip_->setObjectName(QStringLiteral("messageImageStrip"));
    layout_ = new QHBoxLayout(strip_);
    layout_->setContentsMargins(4, 4, 4, 4);
    layout_->setSpacing(8);
    layout_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    setWidget(strip_);
    hide();
  }

  void setPaths(const QStringList &paths, bool forceRebuild = false) {
    bool matches = !forceRebuild && layout_->count() == paths.size();
    for (qsizetype index = 0; matches && index < paths.size(); ++index) {
      const auto *thumbnail = dynamic_cast<ImageThumbnail *>(
          layout_->itemAt(static_cast<int>(index))->widget());
      matches = thumbnail && thumbnail->represents(paths.at(index));
    }
    if (matches) {
      setVisible(!paths.isEmpty());
      return;
    }

    while (QLayoutItem *item = layout_->takeAt(0)) {
      delete item->widget();
      delete item;
    }
    for (const QString &path : paths)
      layout_->addWidget(new ImageThumbnail(path, strip_), 0, Qt::AlignVCenter);

    layout_->activate();
    naturalSize_ = layout_->sizeHint().expandedTo(QSize(0, 0));
    strip_->setFixedSize(naturalSize_);
    horizontalScrollBar()->setValue(0);
    refreshHeight();
    setVisible(!paths.isEmpty());
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QScrollArea::resizeEvent(event);
    refreshHeight();
  }

private:
  void refreshHeight() {
    const int availableWidth = std::max(0, viewport()->width());
    const bool overflows = naturalSize_.width() > availableWidth;
    const int scrollBarHeight =
        overflows ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0;
    const int target =
        std::max(0, naturalSize_.height() + scrollBarHeight + 2 * frameWidth());
    if (height() != target)
      setFixedHeight(target);
  }

  QWidget *strip_ = nullptr;
  QHBoxLayout *layout_ = nullptr;
  QSize naturalSize_;
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
    imageLabel_->setPixmap(QPixmap::fromImage(image_.scaled(
        available, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
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
  document.setMarkdown(markdown, QTextDocument::MarkdownFeatures(
                                     QTextDocument::MarkdownDialectGitHub) |
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
                                 Qt::LinksAccessibleByMouse |
                                 Qt::LinksAccessibleByKeyboard);
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
  const QByteArray encoded = status.toUtf8();
  return text(codexui::codex::displayStatus(std::string_view(
      encoded.constData(), static_cast<std::size_t>(encoded.size()))));
}

QString statusTone(const QString &status) {
  const QByteArray encoded = status.toUtf8();
  const std::string_view tone =
      classifyStatus(std::string_view(encoded.constData(),
                                      static_cast<std::size_t>(encoded.size())))
          .tone;
  return QString::fromLatin1(tone.data(), static_cast<qsizetype>(tone.size()));
}

void setStatusTone(QLabel *label, const QString &status) {
  const QString tone = statusTone(status);
  if (label->property("tone").toString() == tone)
    return;
  label->setProperty("tone", tone);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->update();
}

QString commandMetadata(const CommandExecutionData &command) {
  QStringList metadata;
  if (command.exitCode)
    metadata << QStringLiteral("exit %1").arg(*command.exitCode);
  if (!command.cwd.empty())
    metadata << text(command.cwd);
  if (command.durationMilliseconds) {
    const qreal seconds = qreal(*command.durationMilliseconds) / 1000.0;
    metadata << QStringLiteral("%1 s").arg(seconds, 0, 'f',
                                           seconds < 10.0 ? 1 : 0);
  }
  return metadata.join(QStringLiteral("  |  "));
}

QString agentMetadata(const AgentActivityData &activity) {
  QStringList metadata;
  if (!activity.tool.empty())
    metadata << text(activity.tool);
  if (activity.status.empty() && !activity.kind.empty())
    metadata << displayStatus(text(activity.kind));
  if (!activity.receivers.empty())
    metadata << textList(activity.receivers).join(QStringLiteral(", "));
  if (!activity.model.empty())
    metadata << text(activity.model);
  if (!activity.reasoningEffort.empty())
    metadata << text(activity.reasoningEffort);
  if (!activity.childThreadId.empty())
    metadata << QStringLiteral("thread %1").arg(text(activity.childThreadId));
  if (!activity.agentPath.empty())
    metadata << text(activity.agentPath);
  if (!activity.senderThreadId.empty())
    metadata << QStringLiteral("sender %1").arg(text(activity.senderThreadId));
  return metadata.join(QStringLiteral("  |  "));
}

QString displayChangeKind(std::string_view kind) {
  if (kind.empty())
    return QStringLiteral("Changed");
  return UiStyle::humanizeLabel(text(kind));
}

struct DiffCounts {
  int additions = 0;
  int deletions = 0;
};

struct CardCopyContent {
  QString text;
  bool markdown = false;
};

QString joinedCopyText(QStringList parts) {
  parts.removeAll(QString{});
  return parts.join(QStringLiteral("\n\n"));
}

QString fileChangesText(const FileChangesData &data) {
  QStringList rows;
  for (const FileChangeData &change : data.changes) {
    if (change.path.empty())
      continue;
    QString row = QStringLiteral("%1  ·  %2")
                      .arg(text(change.path), displayChangeKind(change.kind));
    if (change.additions && change.deletions)
      row += QStringLiteral("  +%1 −%2")
                 .arg(*change.additions)
                 .arg(*change.deletions);
    rows << row;
  }
  return rows.join(QLatin1Char('\n'));
}

std::optional<DiffCounts> totalDiffCounts(const FileChangesData &data) {
  DiffCounts total;
  bool available = false;
  for (const FileChangeData &change : data.changes) {
    if (!change.additions || !change.deletions)
      continue;
    available = true;
    total.additions += *change.additions;
    total.deletions += *change.deletions;
  }
  return available ? std::optional<DiffCounts>{total} : std::nullopt;
}

QString planMarkdown(const PlanData &plan) {
  if (!plan.legacyText.empty())
    return text(plan.legacyText);
  QStringList rows;
  if (!plan.explanation.empty())
    rows << text(plan.explanation);
  if (!plan.steps.empty() && !rows.empty())
    rows << QString{};
  for (const PlanStepData &step : plan.steps) {
    const QString marker = step.status == "completed"    ? QStringLiteral("✓")
                           : step.status == "inProgress" ? QStringLiteral("◉")
                                                         : QStringLiteral("○");
    rows << QStringLiteral("%1 %2  ").arg(marker, text(step.text));
  }
  return rows.join(QLatin1Char('\n'));
}

QString boundedGenericActivity(const nlohmann::json &raw) {
  QString rendered = QString::fromStdString(raw.dump(2));
  if (rendered.size() <= MaximumGenericActivityCharacters)
    return rendered;
  rendered.truncate(MaximumGenericActivityCharacters);
  return rendered + QStringLiteral("\n\n[Activity details truncated]");
}

CardCopyContent cardCopyContent(const VisibleCardData &card) {
  return std::visit(
      [](const auto &payload) -> CardCopyContent {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, UserMessageData>) {
          return payload.text.empty()
                     ? CardCopyContent{textList(payload.imagePaths)
                                           .join(QLatin1Char('\n')),
                                       false}
                     : CardCopyContent{text(payload.text), true};
        } else if constexpr (std::is_same_v<Payload, AgentMessageData>) {
          return {text(payload.text), true};
        } else if constexpr (std::is_same_v<Payload, CommandExecutionData>) {
          return {
              joinedCopyText({text(trimTrailingEmptyLines(payload.command)),
                              text(trimTrailingEmptyLines(payload.output))}),
              false};
        } else if constexpr (std::is_same_v<Payload, AgentActivityData>) {
          return {
              joinedCopyText({text(payload.prompt), text(payload.resultText)}),
              true};
        } else if constexpr (std::is_same_v<Payload, ReasoningData>) {
          return {text(payload.summary), true};
        } else if constexpr (std::is_same_v<Payload, FileChangesData>) {
          return {fileChangesText(payload), false};
        } else if constexpr (std::is_same_v<Payload, PlanData>) {
          return {planMarkdown(payload), true};
        } else if constexpr (std::is_same_v<Payload, ImageGenerationData>) {
          return {
              joinedCopyText({text(payload.revisedPrompt), text(payload.path)}),
              false};
        } else if constexpr (std::is_same_v<Payload, GenericActivityData>) {
          return {boundedGenericActivity(payload.raw), false};
        } else {
          return payload.prompt.empty()
                     ? CardCopyContent{textList(payload.imagePaths)
                                           .join(QLatin1Char('\n')),
                                       false}
                     : CardCopyContent{text(payload.prompt), true};
        }
      },
      card.payload);
}

bool presentationEquals(const VisibleCardData &left,
                        const VisibleCardData &right) {
  if (left.kind != right.kind)
    return false;
  const auto *first = std::get_if<LocalPromptData>(&left.payload);
  const auto *second = std::get_if<LocalPromptData>(&right.payload);
  if (first && second) {
    return first->prompt == second->prompt &&
           first->imagePaths == second->imagePaths &&
           first->state == second->state &&
           first->showPendingAnimation == second->showPendingAnimation &&
           first->error == second->error;
  }
  return left.payload == right.payload;
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

bool ContentSizedTextView::retainsWheelGesture(QWheelEvent *event) {
  if (!event)
    return false;
  const int delta = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                  : event->angleDelta().y();
  QScrollBar *bar = verticalScrollBar();
  const bool canScroll = bar->maximum() > bar->minimum() &&
                         ((delta > 0 && bar->value() > bar->minimum()) ||
                          (delta < 0 && bar->value() < bar->maximum()));

  const bool hasDirection = delta != 0;
  if (event->phase() == Qt::ScrollBegin) {
    wheelGestureActive_ = true;
    wheelGestureDecided_ = hasDirection;
    wheelGestureOwned_ = hasDirection && canScroll;
  } else if (event->phase() == Qt::ScrollEnd) {
    const bool retained =
        wheelGestureActive_ && wheelGestureDecided_ && wheelGestureOwned_;
    wheelGestureActive_ = false;
    wheelGestureDecided_ = false;
    wheelGestureOwned_ = false;
    return retained;
  } else if (event->phase() == Qt::NoScrollPhase) {
    // A discrete mouse-wheel notch is a complete gesture. At an existing
    // boundary it may therefore scroll the enclosing conversation.
    return canScroll;
  } else if (!wheelGestureActive_) {
    // Some platforms omit ScrollBegin and start with ScrollUpdate.
    wheelGestureActive_ = true;
    wheelGestureDecided_ = hasDirection;
    wheelGestureOwned_ = hasDirection && canScroll;
  } else if (!wheelGestureDecided_ && hasDirection) {
    wheelGestureDecided_ = true;
    wheelGestureOwned_ = canScroll;
  }
  return wheelGestureDecided_ && wheelGestureOwned_;
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

void ContentSizedTextView::wheelEvent(QWheelEvent *event) {
  QScrollBar *bar = verticalScrollBar();
  const int delta = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                  : event->angleDelta().y();
  const bool atBoundary = bar->maximum() <= bar->minimum() ||
                          (delta > 0 && bar->value() <= bar->minimum()) ||
                          (delta < 0 && bar->value() >= bar->maximum());
  if (atBoundary) {
    // If this nested view owned the gesture when it began, reaching an edge
    // must not leak the remaining updates into the conversation viewport.
    event->accept();
    return;
  }
  QTextEdit::wheelEvent(event);
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
            if (userScrollActive_) {
              preservedScrollValue_ = value;
              followsLatest_ = isAtBottom();
            }
          });
  connect(verticalScrollBar(), &QScrollBar::sliderPressed, this,
          [this] { userScrollActive_ = true; });
  connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [this] {
    userScrollActive_ = false;
    preservedScrollValue_ = verticalScrollBar()->value();
    followsLatest_ = isAtBottom();
  });
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
          [this](int) {
            preservedScrollValue_ = verticalScrollBar()->sliderPosition();
            followsLatest_ =
                preservedScrollValue_ >= verticalScrollBar()->maximum() - 1;
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
  const QString displayOutput = trimmedTrailingLines(output);
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
  if (delta > 0)
    followsLatest_ = false;
  ContentSizedTextView::wheelEvent(event);
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
  if (followsLatest_)
    preservedScrollValue_ = target;
  programmaticScroll_ = wasProgrammatic;
  settlingScroll_ = false;
}

bool CommandOutputView::isAtBottom() const {
  return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
}

class ConversationCard::Impl final {
public:
  Impl(ConversationCard *owner, const VisibleCardData &initial,
       bool commandInitiallyCollapsed, bool imageInitiallyCollapsed)
      : owner(owner), current(initial),
        collapsed(initiallyCollapsed(initial.kind, commandInitiallyCollapsed,
                                     imageInitiallyCollapsed)) {
    owner->setObjectName(QStringLiteral("conversationCard"));
    owner->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    owner->setProperty("conversationCardKey",
                       QString::fromStdString(stableKey(initial.key)));
    owner->setProperty("conversationCardKind", static_cast<int>(initial.kind));
    layout = new QVBoxLayout(owner);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);

    header = new QWidget(owner);
    header->setObjectName(QStringLiteral("conversationCardHeader"));
    headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(CardHeaderActionSpacing);
    title = makeLabel({}, "title", header);
    title->setWordWrap(false);
    copy = new CardCopyButton(header);
    disclosure = new CardDisclosureButton(header);
    headerLayout->addWidget(title, 1);
    headerLayout->addWidget(copy, 0, Qt::AlignRight | Qt::AlignVCenter);
    headerLayout->addWidget(disclosure, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(header);

    content = new QWidget(owner);
    content->setObjectName(QStringLiteral("conversationCardContent"));
    contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(6);
    layout->addWidget(content);

    nestedCards = new QWidget(owner);
    nestedCards->setObjectName(QStringLiteral("conversationNestedCards"));
    nestedLayout = new QVBoxLayout(nestedCards);
    nestedLayout->setContentsMargins(0, 0, 0, 0);
    nestedLayout->setSpacing(8);
    nestedCards->hide();
    layout->addWidget(nestedCards);

    QObject::connect(disclosure, &QToolButton::clicked, owner,
                     [this] { emit this->owner->foldRequested(!collapsed); });
    QObject::connect(copy, &QToolButton::clicked, owner, [this] {
      const CardCopyContent content = cardCopyContent(current);
      if (content.text.isEmpty())
        return;
      auto *mime = new QMimeData;
      mime->setText(content.text);
      if (content.markdown)
        mime->setData("text/markdown", content.text.toUtf8());
      QApplication::clipboard()->setMimeData(mime);
      copy->showCopiedFeedback();
    });
    owner->setProperty("kind", "raised");
    std::visit([this](const auto &payload) { createComposition(payload); },
               initial.payload);
    refreshCopyPresentation();
    refreshFoldPresentation();
  }

  [[nodiscard]] bool canApply(const VisibleCardData &next) const noexcept {
    return current.key == next.key && (current.kind == next.kind ||
                                       (current.kind == CardKind::LocalPrompt &&
                                        next.kind == CardKind::UserMessage));
  }

  bool apply(const VisibleCardData &next) {
    if (!canApply(next)) {
      Q_ASSERT_X(false, "ConversationCard::apply",
                 "a persistent conversation card received an incompatible "
                 "key or kind");
      return false;
    }
    const bool becomingAuthoritative = current.kind == CardKind::LocalPrompt &&
                                       next.kind == CardKind::UserMessage;
    const bool presentationChanged =
        becomingAuthoritative || !presentationEquals(current, next);
    if (becomingAuthoritative)
      promoteToAuthoritativeUserMessage();
    current = next;
    if (!presentationChanged)
      return false;
    std::visit([this](const auto &payload) { updateComposition(payload); },
               next.payload);
    refreshCopyPresentation();
    refreshFoldPresentation();
    owner->updateGeometry();
    owner->update();
    return true;
  }

  void promoteToAuthoritativeUserMessage() {
    if (animationTimer)
      animationTimer->stop();
    owner->setObjectName(QStringLiteral("conversationCard"));
    owner->setProperty("conversationCardKind",
                       static_cast<int>(CardKind::UserMessage));
    owner->setProperty("messageRole", "user");
    owner->setStyleSheet(QString{});
    for (QLabel *label : {title, body, metadata})
      if (label)
        label->setStyleSheet(QString{});
    if (metadata) {
      metadata->clear();
      metadata->hide();
    }
  }

  void setCollapsed(bool next) {
    if (collapsed == next)
      return;
    collapsed = next;
    refreshFoldPresentation();
    owner->updateGeometry();
    owner->update();
  }

  bool setAuthoritativeTurnActive(bool active) {
    const bool next = active && (current.kind == CardKind::LocalPrompt ||
                                 current.kind == CardKind::UserMessage);
    if (authoritativeTurnActive == next)
      return false;
    authoritativeTurnActive = next;
    owner->setProperty("authoritativeTurnActive", next);
    owner->update();
    return true;
  }

  void setNestedConversationCard(bool nested) {
    owner->setProperty("nestedConversationCard", nested);
    if (current.kind == CardKind::UserMessage ||
        current.kind == CardKind::LocalPrompt) {
      title->setText(QStringLiteral("You"));
      if (nested) {
        showPhase(QStringLiteral("steering"),
                  QStringLiteral("steeringMessagePhase"));
        setPhaseTone(QStringLiteral("steering"));
      } else if (phase) {
        phase->hide();
      }
    }
    if (current.kind == CardKind::LocalPrompt)
      refreshPendingPresentation();
    owner->style()->unpolish(owner);
    owner->style()->polish(owner);
    owner->update();
  }

  void setNestedCards(const std::vector<ConversationCard *> &cards) {
    const std::unordered_set<ConversationCard *> retained(cards.begin(),
                                                          cards.end());
    for (int index = nestedLayout->count() - 1; index >= 0; --index) {
      auto *card = dynamic_cast<ConversationCard *>(
          nestedLayout->itemAt(index)->widget());
      if (!card || retained.contains(card))
        continue;
      const bool explicitlyHidden = card->isHidden();
      nestedLayout->removeWidget(card);
      card->setParent(owner->parentWidget());
      card->setVisible(!explicitlyHidden);
      card->impl_->setNestedConversationCard(false);
    }
    for (std::size_t index = 0; index < cards.size(); ++index) {
      ConversationCard *card = cards[index];
      if (!card)
        continue;
      const bool explicitlyHidden = card->isHidden();
      const int position = static_cast<int>(index);
      if (nestedLayout->indexOf(card) != position)
        nestedLayout->insertWidget(position, card);
      card->setVisible(!explicitlyHidden);
      card->impl_->setNestedConversationCard(true);
    }
    hasVisibleNestedCards =
        std::ranges::any_of(cards, [](const ConversationCard *card) {
          return card && !card->isHidden();
        });
    refreshFoldPresentation();
  }

  [[nodiscard]] bool hasVisibleContent() const {
    for (int index = 0; index < contentLayout->count(); ++index) {
      if (QWidget *widget = contentLayout->itemAt(index)->widget();
          widget && !widget->isHidden())
        return true;
    }
    return false;
  }

  void refreshFoldPresentation() {
    const bool expandable = hasVisibleContent() || hasVisibleNestedCards;
    disclosure->setExpanded(!collapsed);
    disclosure->setVisible(expandable);
    content->setVisible(expandable && !collapsed);
    nestedCards->setVisible(hasVisibleNestedCards && !collapsed);
  }

  void refreshCopyPresentation() {
    copy->setVisible(!cardCopyContent(current).text.isEmpty());
  }

  void showPhase(const QString &value, const QString &objectName) {
    if (!phase) {
      phase = makeLabel({}, "messagePhase", header);
      phase->setWordWrap(false);
      phase->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
      headerLayout->insertWidget(headerLayout->indexOf(copy), phase, 0,
                                 Qt::AlignVCenter);
    }
    phase->setObjectName(objectName);
    phase->setText(value);
    phase->show();
  }

  void showStatus(const QString &status, const QString &objectName) {
    if (status.isEmpty()) {
      if (phase)
        phase->hide();
      return;
    }
    showPhase(displayStatus(status), objectName);
    setStatusTone(phase, status);
  }

  void setPhaseTone(const QString &tone) {
    if (!phase || phase->property("tone").toString() == tone)
      return;
    phase->setProperty("tone", tone);
    phase->style()->unpolish(phase);
    phase->style()->polish(phase);
    phase->update();
  }

  void setActiveWork(bool active) {
    if (owner->property("activeWork").toBool() == active)
      return;
    owner->setProperty("activeWork", active);
    owner->update();
  }

  void createImageContainer() {
    images = new ImageRibbon(content);
    contentLayout->addWidget(images);
  }

  void setImages(const QStringList &paths, bool forceRebuild = false) {
    images->setPaths(paths, forceRebuild);
  }

  void createComposition(const UserMessageData &message) {
    owner->setProperty("messageRole", "user");
    title->setText(QStringLiteral("You"));
    body = makeMarkdownLabel({}, content);
    contentLayout->addWidget(body);
    createImageContainer();
    updateComposition(message);
  }

  void updateComposition(const UserMessageData &message) {
    setVisibleMarkdown(body, text(message.text));
    setImages(textList(message.imagePaths));
  }

  void createComposition(const AgentMessageData &message) {
    owner->setProperty("messageRole", "agent");
    title->setText(QStringLiteral("Codex"));
    showPhase({}, QStringLiteral("agentMessagePhase"));
    body = makeMarkdownLabel({}, content);
    contentLayout->addWidget(body);
    updateComposition(message);
  }

  void updateComposition(const AgentMessageData &message) {
    const QString messagePhase = message.finalAnswer ? QStringLiteral("final")
                                                     : QStringLiteral("update");
    if (owner->property("messagePhase").toString() != messagePhase) {
      owner->setProperty("messagePhase", messagePhase);
      owner->style()->unpolish(owner);
      owner->style()->polish(owner);
    }
    showPhase(message.finalAnswer ? QStringLiteral("final answer")
                                  : QStringLiteral("update"),
              QStringLiteral("agentMessagePhase"));
    const QString phaseStatus = message.finalAnswer
                                    ? QStringLiteral("completed")
                                    : QStringLiteral("inProgress");
    setStatusTone(phase, phaseStatus);
    layout->setContentsMargins(12, message.finalAnswer ? 10 : 8, 12,
                               message.finalAnswer ? 10 : 8);
    setVisibleMarkdown(body, text(message.text));
  }

  void createComposition(const CommandExecutionData &execution) {
    title->setText(QStringLiteral("Command execution"));
    command = new ContentSizedTextView(MaximumCommandTextHeight, content);
    command->setProperty("kind", "command");
    command->setObjectName(QStringLiteral("commandTextView"));
    command->setStyleSheet(
        QStringLiteral("QTextEdit#commandTextView{background:#f8fafc;"
                       "border:1px solid #d7dee8;border-radius:6px;}"));
    output = new CommandOutputView({}, content);
    output->hide();
    metadata = makeLabel({}, "meta", content);
    metadata->setObjectName(QStringLiteral("commandMetadata"));
    contentLayout->addWidget(command);
    contentLayout->addWidget(output);
    contentLayout->addWidget(metadata);
    updateComposition(execution);
  }

  void updateComposition(const CommandExecutionData &execution) {
    setActiveWork(isActiveStatus(execution.status));
    showStatus(text(execution.status), QStringLiteral("commandStatus"));
    const std::string trimmedCommand =
        trimTrailingEmptyLines(execution.command);
    const QString displayCommand = text(trimmedCommand);
    command->setContent(displayCommand);
    command->setVisible(!displayCommand.isEmpty());
    const std::string trimmedOutput = trimTrailingEmptyLines(execution.output);
    const QString displayOutput = text(trimmedOutput);
    const bool visibleOutput = terminalOutputHasVisibleText(trimmedOutput);
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
    setVisibleText(metadata, commandMetadata(execution));
  }

  void createComposition(const AgentActivityData &activity) {
    title->setText(QStringLiteral("Agent activity"));
    metadata = makeLabel({}, "meta", content);
    body = makeLabel({}, "body", content);
    detail = makeMarkdownLabel({}, content);
    contentLayout->addWidget(metadata);
    contentLayout->addWidget(body);
    contentLayout->addWidget(detail);
    updateComposition(activity);
  }

  void updateComposition(const AgentActivityData &activity) {
    showStatus(text(activity.status), QStringLiteral("agentActivityStatus"));
    setVisibleText(metadata, agentMetadata(activity));
    setVisibleText(body, text(activity.prompt));
    setVisibleMarkdown(detail, text(activity.resultText));
  }

  void createComposition(const ReasoningData &reasoning) {
    title->setText(QStringLiteral("Reasoning"));
    body = makeMarkdownLabel({}, content);
    contentLayout->addWidget(body);
    updateComposition(reasoning);
  }

  void updateComposition(const ReasoningData &reasoning) {
    setVisibleMarkdown(body, text(reasoning.summary));
  }

  void createComposition(const FileChangesData &changes) {
    title->setText(QStringLiteral("File changes"));
    metadata = makeLabel({}, "meta", content);
    body = makeLabel({}, "body", content);
    contentLayout->addWidget(body);
    contentLayout->addWidget(metadata);
    updateComposition(changes);
  }

  void updateComposition(const FileChangesData &changes) {
    setVisibleText(body, fileChangesText(changes));
    showStatus(text(changes.status), QStringLiteral("fileChangesStatus"));
    QStringList values{QStringLiteral("%1 paths").arg(changes.changes.size())};
    if (const auto counts = totalDiffCounts(changes))
      values << QStringLiteral("+%1 −%2")
                    .arg(counts->additions)
                    .arg(counts->deletions);
    metadata->setText(values.join(QStringLiteral("  |  ")));
    metadata->show();
  }

  void createComposition(const PlanData &plan) {
    title->setText(QStringLiteral("Plan"));
    body = makeMarkdownLabel({}, content);
    contentLayout->addWidget(body);
    updateComposition(plan);
  }

  void updateComposition(const PlanData &plan) {
    setVisibleMarkdown(body, planMarkdown(plan));
  }

  void createComposition(const ImageGenerationData &image) {
    title->setText(QStringLiteral("Generated image"));
    body = makeLabel({}, "body", content);
    contentLayout->addWidget(body);
    createImageContainer();
    updateComposition(image);
  }

  void updateComposition(const ImageGenerationData &image) {
    setActiveWork(isActiveStatus(image.status));
    showStatus(text(image.status), QStringLiteral("imageGenerationStatus"));
    const bool generated =
        !image.status.empty() || !image.revisedPrompt.empty();
    title->setText(generated ? QStringLiteral("Generated image")
                             : QStringLiteral("Image"));
    setVisibleText(body, text(image.revisedPrompt));
    // A generated image can become readable at the same path as its status
    // advances, so its update remains the authoritative reload boundary.
    setImages(image.path.empty() ? QStringList{}
                                 : QStringList{text(image.path)},
              true);
  }

  void createComposition(const GenericActivityData &activity) {
    metadata = makeLabel({}, "meta", content);
    contentLayout->addWidget(metadata);
    updateComposition(activity);
  }

  void updateComposition(const GenericActivityData &activity) {
    title->setText(activity.type.empty()
                       ? QStringLiteral("Activity")
                       : UiStyle::humanizeLabel(text(activity.type)));
    showStatus(text(activity.status), QStringLiteral("genericActivityStatus"));
    metadata->setText(boundedGenericActivity(activity.raw));
    metadata->setObjectName(QStringLiteral("genericActivityMetadata"));
    metadata->show();
  }

  void createComposition(const LocalPromptData &prompt) {
    owner->setObjectName(QStringLiteral("pendingPromptCard"));
    owner->setStyleSheet(
        QStringLiteral("QFrame#pendingPromptCard{background:transparent;"
                       "border:1px solid transparent;border-radius:8px;}"));
    title->setText(QStringLiteral("You"));
    body = makeMarkdownLabel({}, content);
    metadata = makeLabel({}, "meta", content);
    contentLayout->addWidget(body);
    contentLayout->addWidget(metadata);
    createImageContainer();
    animationTimer = new QTimer(owner);
    animationTimer->setInterval(PendingAnimationIntervalMilliseconds);
    QObject::connect(animationTimer, &QTimer::timeout, owner, [this] {
      if (refreshPendingPresentation())
        owner->updateGeometry();
      owner->update();
    });
    updateComposition(prompt);
  }

  void updateComposition(const LocalPromptData &prompt) {
    setVisibleMarkdown(body, text(prompt.prompt));
    setImages(textList(prompt.imagePaths));
    refreshPendingPresentation();
  }

  bool refreshPendingPresentation() {
    const auto *prompt = std::get_if<LocalPromptData>(&current.payload);
    if (!prompt)
      return false;
    const bool waiting = prompt->state == PromptState::Queued ||
                         prompt->state == PromptState::InFlight;
    const bool failed = prompt->state == PromptState::Failed;
    const bool steering = owner->property("nestedConversationCard").toBool();
    const QString foreground =
        waiting
            ? steering ? QStringLiteral("#146f73") : QStringLiteral("#536b8f")
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
      status = prompt->error.empty()
                   ? QStringLiteral("Not sent")
                   : QStringLiteral("Not sent: %1").arg(text(prompt->error));

    changed = setVisibleText(metadata, status) || changed;

    if (waiting && prompt->showPendingAnimation) {
      if (!animationTimer->isActive())
        animationTimer->start();
    } else {
      animationTimer->stop();
    }
    return changed;
  }

  ConversationCard *owner = nullptr;
  VisibleCardData current;
  bool collapsed = false;
  QVBoxLayout *layout = nullptr;
  QWidget *header = nullptr;
  QHBoxLayout *headerLayout = nullptr;
  QLabel *title = nullptr;
  QLabel *phase = nullptr;
  CardCopyButton *copy = nullptr;
  CardDisclosureButton *disclosure = nullptr;
  QWidget *content = nullptr;
  QVBoxLayout *contentLayout = nullptr;
  QLabel *body = nullptr;
  QLabel *metadata = nullptr;
  QLabel *detail = nullptr;
  ContentSizedTextView *command = nullptr;
  CommandOutputView *output = nullptr;
  QTimer *animationTimer = nullptr;
  ImageRibbon *images = nullptr;
  QWidget *nestedCards = nullptr;
  QVBoxLayout *nestedLayout = nullptr;
  bool hasVisibleNestedCards = false;
  bool authoritativeTurnActive = false;
};

ConversationCard::ConversationCard(const VisibleCardData &data, QWidget *parent,
                                   bool commandInitiallyCollapsed,
                                   bool imageInitiallyCollapsed)
    : QFrame(parent),
      impl_(std::make_unique<Impl>(this, data, commandInitiallyCollapsed,
                                   imageInitiallyCollapsed)) {}

ConversationCard::~ConversationCard() = default;

CardKind ConversationCard::cardKind() const noexcept {
  return impl_->current.kind;
}

const VisibleCardData &ConversationCard::data() const noexcept {
  return impl_->current;
}

bool ConversationCard::isCollapsed() const noexcept { return impl_->collapsed; }

void ConversationCard::setCollapsed(bool collapsed) {
  impl_->setCollapsed(collapsed);
}

bool ConversationCard::setAuthoritativeTurnActive(bool active) {
  return impl_->setAuthoritativeTurnActive(active);
}

void ConversationCard::setNestedCards(
    const std::vector<ConversationCard *> &cards) {
  impl_->setNestedCards(cards);
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

bool ConversationCard::canApply(const VisibleCardData &data) const noexcept {
  return impl_->canApply(data);
}

void ConversationCard::paintEvent(QPaintEvent *event) {
  QFrame::paintEvent(event);
  if (property("activeWork").toBool()) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(QStringLiteral("#98a2b3")), 1.5));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 9.0,
                            9.0);
    return;
  }
  const auto paintActiveTurnBorder = [this] {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(QStringLiteral("#6f98e8")), 1.5));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 8.0,
                            8.0);
  };
  if (impl_->current.kind == CardKind::UserMessage) {
    if (impl_->authoritativeTurnActive)
      paintActiveTurnBorder();
    return;
  }
  if (impl_->current.kind != CardKind::LocalPrompt)
    return;
  const auto *prompt = std::get_if<LocalPromptData>(&impl_->current.payload);
  if (!prompt)
    return;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF bounds = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
  const bool waiting = prompt->state == PromptState::Queued ||
                       prompt->state == PromptState::InFlight;
  const bool failed = prompt->state == PromptState::Failed;
  const bool steering = property("nestedConversationCard").toBool();
  const bool animated = waiting && prompt->showPendingAnimation;
  const QColor background = failed
                                ? QColor(QStringLiteral("#fff0f2"))
                                : QColor(steering ? QStringLiteral("#eefafa")
                                                  : QStringLiteral("#eaf2ff"));
  const QColor border = failed
                            ? QColor(QStringLiteral("#efb8c0"))
                        : waiting
                            ? QColor(steering ? QStringLiteral("#5caeb1")
                                              : QStringLiteral("#79a0d7"))
                            : QColor(steering ? QStringLiteral("#9fd7d8")
                                              : QStringLiteral("#bfd3f9"));
  painter.setBrush(background);
  painter.setPen(QPen(border, waiting ? 1.5 : 1.0));
  painter.drawRoundedRect(bounds, 8.0, 8.0);

  if (!animated) {
    if (impl_->authoritativeTurnActive)
      paintActiveTurnBorder();
    return;
  }

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const qint64 phase = now % (2 * PendingHalfCycleMilliseconds);
  const qreal position = phase <= PendingHalfCycleMilliseconds
                             ? qreal(phase) / PendingHalfCycleMilliseconds
                             : qreal(2 * PendingHalfCycleMilliseconds - phase) /
                                   PendingHalfCycleMilliseconds;
  const qreal center = bounds.left() + position * bounds.width();
  const qreal radius = std::max(28.0, bounds.width() * 0.24);
  QLinearGradient sweep(center - radius, 0.0, center + radius, 0.0);
  sweep.setColorAt(0.0, steering ? QColor(22, 123, 128, 0)
                                 : QColor(47, 111, 235, 0));
  sweep.setColorAt(0.5, steering ? QColor(92, 180, 184, 105)
                                 : QColor(117, 160, 239, 105));
  sweep.setColorAt(1.0, steering ? QColor(22, 123, 128, 0)
                                 : QColor(47, 111, 235, 0));
  QPainterPath clip;
  clip.addRoundedRect(bounds, 8.0, 8.0);
  painter.save();
  painter.setClipPath(clip);
  painter.fillRect(bounds, sweep);
  painter.restore();

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(border, 1.5));
  painter.drawRoundedRect(bounds, 8.0, 8.0);
  if (impl_->authoritativeTurnActive)
    paintActiveTurnBorder();
}

ConversationCard *createConversationCard(const VisibleCardData &data,
                                         QWidget *parent,
                                         bool commandInitiallyCollapsed,
                                         bool imageInitiallyCollapsed) {
  return new ConversationCard(data, parent, commandInitiallyCollapsed,
                              imageInitiallyCollapsed);
}

} // namespace codexui::codex::middle
