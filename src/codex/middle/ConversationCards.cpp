// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"

#include "codex/middle/ConversationPresentation.h"

#include "codex/UiStatus.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
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
#include <QPixmapCache>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int MaximumCommandOutputHeight = 220;
constexpr int MaximumCommandTextHeight = 90;
constexpr int CommandTextPadding = 7;
constexpr int CommandOutputHorizontalPadding = 7;
constexpr int CommandOutputVerticalPadding = 4;
constexpr int PendingAnimationIntervalMilliseconds = 32;
constexpr qint64 PendingHalfCycleMilliseconds = 850;
constexpr int ThumbnailMaximumWidth = 280;
constexpr int ThumbnailMaximumHeight = 180;
constexpr int CardHeaderActionSpacing = 0;
constexpr int CopyMorphDurationMilliseconds = 160;
constexpr int CopyCheckHoldMilliseconds = 500;
constexpr int MarkdownBottomPaintGuard = 4;

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
  qsizetype end = value.size();
  while (end > 0) {
    while (end > 0 &&
           (value.at(end - 1) == QLatin1Char('\n') ||
            value.at(end - 1) == QLatin1Char('\r')))
      --end;
    if (end == 0)
      break;

    qsizetype lineStart = end;
    while (lineStart > 0 && value.at(lineStart - 1) != QLatin1Char('\n') &&
           value.at(lineStart - 1) != QLatin1Char('\r'))
      --lineStart;
    bool whitespaceOnly = true;
    for (qsizetype offset = lineStart; offset < end; ++offset) {
      if (!value.at(offset).isSpace()) {
        whitespaceOnly = false;
        break;
      }
    }
    if (!whitespaceOnly)
      break;
    end = lineStart;
  }
  return end == value.size() ? value : value.first(end);
}

bool initiallyCollapsed(CardKind kind, bool commandInitiallyCollapsed,
                        bool imageInitiallyCollapsed,
                        bool fileChangesInitiallyCollapsed) {
  if (kind == CardKind::CommandExecution)
    return commandInitiallyCollapsed;
  if (kind == CardKind::ImageGeneration)
    return imageInitiallyCollapsed;
  if (kind == CardKind::FileChanges)
    return fileChangesInitiallyCollapsed;
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
    QObject::connect(morph_, &QVariantAnimation::valueChanged, this,
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
      color = QColor(QString::fromLatin1(UiStyle::greenText));

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

bool openLocalFile(const QString &path) {
  if (path.isEmpty())
    return false;
  return QDesktopServices::openUrl(
      QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
}

struct ImageFileIdentity {
  QString absolutePath;
  qint64 size = -1;
  qint64 modifiedMilliseconds = -1;
  qint64 metadataChangedMilliseconds = -1;
  bool file = false;

  bool operator==(const ImageFileIdentity &) const = default;
};

ImageFileIdentity imageFileIdentity(const QString &path) {
  const QFileInfo info(path);
  return {info.absoluteFilePath(), info.size(),
          info.lastModified().toMSecsSinceEpoch(),
          info.metadataChangeTime().toMSecsSinceEpoch(), info.isFile()};
}

QString thumbnailCacheKey(const ImageFileIdentity &identity) {
  return QStringLiteral("codexui-thumbnail:%1:%2:%3:%4")
      .arg(identity.absolutePath)
      .arg(identity.size)
      .arg(identity.modifiedMilliseconds)
      .arg(identity.metadataChangedMilliseconds);
}

class ImageThumbnail final : public QLabel {
public:
  ImageThumbnail(QString path, QWidget *parent)
      : QLabel(parent), path_(std::move(path)),
        identity_(imageFileIdentity(path_)) {
    setObjectName(QStringLiteral("messageImageThumbnail"));
    setProperty("kind", "imageThumbnail");
    setCursor(Qt::PointingHandCursor);
    setToolTip(QDir::toNativeSeparators(path_));
    setAccessibleDescription(QDir::toNativeSeparators(path_));
    setAlignment(Qt::AlignCenter);
    setMinimumSize(72, 48);
    setMaximumSize(ThumbnailMaximumWidth, ThumbnailMaximumHeight);

    QPixmap pixmap;
    const bool cacheHit = identity_.file &&
                          QPixmapCache::find(thumbnailCacheKey(identity_),
                                             &pixmap);
    setProperty("imageCacheHit", cacheHit);
    if (!cacheHit) {
      QElapsedTimer decodeTimer;
      decodeTimer.start();
      QImageReader reader(identity_.absolutePath);
      reader.setAutoTransform(true);
      const QSize source = reader.size();
      if (source.isValid())
        reader.setScaledSize(source.scaled(ThumbnailMaximumWidth - 8,
                                           ThumbnailMaximumHeight - 8,
                                           Qt::KeepAspectRatio));
      const QImage image = reader.read();
      setProperty("imageDecodeMicros", decodeTimer.nsecsElapsed() / 1000);
      setProperty("imageDecodePerformed", true);
      if (!image.isNull()) {
        pixmap = QPixmap::fromImage(image);
        QPixmapCache::insert(thumbnailCacheKey(identity_), pixmap);
      }
    }
    if (pixmap.isNull()) {
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
    setPixmap(pixmap);
    setFixedSize(pixmap.size() + QSize(8, 8));
  }

  [[nodiscard]] bool represents(const QString &path) const {
    return path_ == path && identity_ == imageFileIdentity(path);
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
    openLocalFile(path_);
    return true;
  }

  QString path_;
  ImageFileIdentity identity_;
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

  void setPaths(const QStringList &paths) {
    bool changed = false;
    for (qsizetype index = 0; index < paths.size(); ++index) {
      QLayoutItem *item = layout_->itemAt(static_cast<int>(index));
      const auto *thumbnail =
          item ? dynamic_cast<ImageThumbnail *>(item->widget()) : nullptr;
      if (thumbnail && thumbnail->represents(paths.at(index)))
        continue;
      if (item) {
        item = layout_->takeAt(static_cast<int>(index));
        delete item->widget();
        delete item;
      }
      layout_->insertWidget(static_cast<int>(index),
                            new ImageThumbnail(paths.at(index), strip_), 0,
                            Qt::AlignVCenter);
      changed = true;
    }
    while (layout_->count() > paths.size()) {
      QLayoutItem *item = layout_->takeAt(paths.size());
      delete item->widget();
      delete item;
      changed = true;
    }

    if (!changed) {
      setVisible(!paths.isEmpty());
      return;
    }

    const int retainedScroll = horizontalScrollBar()->value();
    layout_->activate();
    naturalSize_ = layout_->sizeHint().expandedTo(QSize(0, 0));
    strip_->setFixedSize(naturalSize_);
    refreshHeight();
    horizontalScrollBar()->setValue(retainedScroll);
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

MarkdownTextView *makeMarkdownView(
    const QString &value, std::shared_ptr<QTextDocument> preparedDocument,
    int initialWidth, QWidget *parent = nullptr,
    bool preserveSoftLineBreaks = false) {
  return new MarkdownTextView(value, std::move(preparedDocument), initialWidth,
                              parent, preserveSoftLineBreaks);
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

bool setVisibleMarkdown(MarkdownTextView *view, const QString &markdown) {
  const bool visible = !markdown.isEmpty();
  const bool contentChanged = view->markdownSource() != markdown;
  const bool explicitlyVisible = !view->isHidden();
  const bool changed = contentChanged || explicitlyVisible != visible;
  if (contentChanged)
    view->setContent(markdown);
  if (explicitlyVisible != visible)
    view->setVisible(visible);
  return changed;
}

QString displayStatus(const QString &status) {
  const QByteArray encoded = status.toUtf8();
  return presentation::statusLabel(std::string_view(
      encoded.constData(), static_cast<std::size_t>(encoded.size())));
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

bool commandHasMetadata(const CommandExecutionData &command) noexcept {
  return command.exitCode || !command.cwd.empty() ||
         command.durationMilliseconds;
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

struct FileChangesRendering {
  struct Link {
    int start = 0;
    int length = 0;
  };

  QString text;
  QStringList openPaths;
  std::vector<Link> links;
  std::optional<DiffCounts> counts;
};

struct CardCopyContent {
  QString text;
  bool markdown = false;
};

QString joinedCopyText(QStringList parts) {
  parts.removeAll(QString{});
  return parts.join(QStringLiteral("\n\n"));
}

FileChangesRendering fileChangesRendering(const FileChangesData &data) {
  FileChangesRendering result;
  DiffCounts total;
  bool countsAvailable = false;
  for (const FileChangeData &change : data.changes) {
    if (change.path.empty())
      continue;
    const QString displayPath = text(change.path);
    QFileInfo resolved(displayPath);
    if (resolved.isRelative() && !data.cwd.empty())
      resolved = QFileInfo(QDir(text(data.cwd)), displayPath);
    const int targetIndex = result.openPaths.size();
    result.openPaths.push_back(QDir::cleanPath(resolved.absoluteFilePath()));

    if (!result.text.isEmpty())
      result.text += QLatin1Char('\n');
    result.links.push_back(
        {static_cast<int>(result.text.size()),
         static_cast<int>(displayPath.size())});
    result.text += displayPath;
    result.text += QStringLiteral("  ·  ");
    QString detail = displayChangeKind(change.kind);
    if (change.additions && change.deletions) {
      detail += QStringLiteral("  +%1 −%2")
                    .arg(*change.additions)
                    .arg(*change.deletions);
      countsAvailable = true;
      total.additions += *change.additions;
      total.deletions += *change.deletions;
    }
    result.text += detail;
    Q_ASSERT(targetIndex == static_cast<int>(result.links.size()) - 1);
  }
  if (countsAvailable)
    result.counts = total;
  return result;
}

class FileChangesView final : public QPlainTextEdit {
public:
  explicit FileChangesView(QWidget *parent = nullptr) : QPlainTextEdit(parent) {
    setObjectName(QStringLiteral("fileChangesList"));
    setProperty("kind", "body");
    setStyleSheet(QStringLiteral(
        "QPlainTextEdit#fileChangesList{background:transparent;border:0;"
        "padding:0;margin:0;}"));
    setFrameShape(QFrame::NoFrame);
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMinimumSize(0, 0);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Changed files"));
    document()->setDocumentMargin(0);
  }

  void setContent(FileChangesRendering rendering) {
    const QTextCursor retained = textCursor();
    const int retainedPosition = retained.position();
    const int retainedAnchor = retained.anchor();
    QElapsedTimer phaseTimer;
    phaseTimer.start();
    setPlainText(rendering.text);
    setProperty("fileChangesSetTextMicros",
                phaseTimer.nsecsElapsed() / 1000);
    phaseTimer.restart();
    openPaths_ = std::move(rendering.openPaths);
    QTextCharFormat linkFormat;
    linkFormat.setForeground(QColor(QString::fromLatin1(UiStyle::blue)));
    linkFormat.setFontUnderline(false);
    linkFormat.setAnchor(true);
    QTextCursor cursor(document());
    cursor.beginEditBlock();
    for (std::size_t index = 0; index < rendering.links.size(); ++index) {
      const FileChangesRendering::Link &link = rendering.links[index];
      linkFormat.setAnchorHref(
          QStringLiteral("codexui-file:%1").arg(index));
      cursor.setPosition(link.start);
      cursor.setPosition(link.start + link.length, QTextCursor::KeepAnchor);
      cursor.mergeCharFormat(linkFormat);
    }
    cursor.endEditBlock();
    setProperty("fileChangesFormatLinksMicros",
                phaseTimer.nsecsElapsed() / 1000);
    phaseTimer.restart();
    const int maximum = std::max(0, document()->characterCount() - 1);
    QTextCursor restored(document());
    restored.setPosition(std::clamp(retainedAnchor, 0, maximum));
    restored.setPosition(std::clamp(retainedPosition, 0, maximum),
                         QTextCursor::KeepAnchor);
    setTextCursor(restored);
    preferredWidth_ = 0;
    preferredHeight_ = 0;
    refreshPreferredHeight(std::max(1, viewport()->width()));
    setProperty("fileChangesMeasureMicros",
                phaseTimer.nsecsElapsed() / 1000);
    updateGeometry();
  }

  [[nodiscard]] QSize sizeHint() const override {
    QSize result = QPlainTextEdit::sizeHint();
    result.setHeight(preferredHeight(std::max(1, viewport()->width())));
    return result;
  }

  [[nodiscard]] QSize minimumSizeHint() const override { return {0, 0}; }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QPlainTextEdit::resizeEvent(event);
    refreshPreferredHeight(std::max(1, viewport()->width()));
  }

  void mousePressEvent(QMouseEvent *event) override {
    pressedLink_ = event->button() == Qt::LeftButton
                       ? anchorAt(event->position().toPoint())
                       : QString{};
    QPlainTextEdit::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    const QString link = anchorAt(event->position().toPoint());
    viewport()->setCursor(link.isEmpty() ? Qt::IBeamCursor
                                         : Qt::PointingHandCursor);
    if (!link.isEmpty())
      setToolTip(linkPath(link));
    else
      setToolTip({});
    QPlainTextEdit::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    const QString releasedLink =
        event->button() == Qt::LeftButton
            ? anchorAt(event->position().toPoint())
            : QString{};
    QPlainTextEdit::mouseReleaseEvent(event);
    if (!pressedLink_.isEmpty() && releasedLink == pressedLink_ &&
        !textCursor().hasSelection())
      static_cast<void>(activateLink(releasedLink));
    pressedLink_.clear();
  }

  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
      QTextCursor cursor = textCursor();
      QString link = cursor.charFormat().anchorHref();
      if (link.isEmpty() && cursor.position() > 0) {
        cursor.setPosition(cursor.position() - 1);
        link = cursor.charFormat().anchorHref();
      }
      if (activateLink(link)) {
        event->accept();
        return;
      }
    }
    QPlainTextEdit::keyPressEvent(event);
  }

  void wheelEvent(QWheelEvent *event) override { event->ignore(); }

private:
  [[nodiscard]] QString linkPath(const QString &link) const {
    constexpr QLatin1StringView prefix("codexui-file:");
    if (!link.startsWith(prefix))
      return {};
    bool valid = false;
    const int index = link.sliced(prefix.size()).toInt(&valid);
    return valid && index >= 0 && index < openPaths_.size()
               ? QDir::toNativeSeparators(openPaths_.at(index))
               : QString{};
  }

  bool activateLink(const QString &link) {
    const QString path = linkPath(link);
    return !path.isEmpty() && openLocalFile(path);
  }

  int preferredHeight(int width) const {
    refreshPreferredHeight(width);
    return preferredHeight_;
  }

  void refreshPreferredHeight(int width) const {
    if (preferredWidth_ == width && preferredHeight_ > 0)
      return;
    document()->setTextWidth(width);
    preferredWidth_ = width;
    preferredHeight_ =
        std::max(1, static_cast<int>(std::ceil(document()->size().height())));
  }

  QStringList openPaths_;
  QString pressedLink_;
  mutable int preferredWidth_ = 0;
  mutable int preferredHeight_ = 0;
};

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
          return {presentation::fileChangesText(payload), false};
        } else if constexpr (std::is_same_v<Payload, PlanData>) {
          return {presentation::planMarkdown(payload), true};
        } else if constexpr (std::is_same_v<Payload, ImageGenerationData>) {
          return {
              joinedCopyText({text(payload.revisedPrompt), text(payload.path)}),
              false};
        } else if constexpr (std::is_same_v<Payload, GenericActivityData>) {
          return {presentation::boundedGenericActivityDetail(payload), false};
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

bool cardHasCopyContent(const VisibleCardData &card) {
  return std::visit(
      [](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, UserMessageData>)
          return !payload.text.empty() ||
                 std::ranges::any_of(payload.imagePaths,
                                     [](const auto &path) {
                                       return !path.empty();
                                     });
        else if constexpr (std::is_same_v<Payload, AgentMessageData>)
          return !payload.text.empty();
        else if constexpr (std::is_same_v<Payload, CommandExecutionData>)
          return hasTextAfterTrimmingTrailingEmptyLines(payload.command) ||
                 hasTextAfterTrimmingTrailingEmptyLines(payload.output);
        else if constexpr (std::is_same_v<Payload, AgentActivityData>)
          return !payload.prompt.empty() || !payload.resultText.empty();
        else if constexpr (std::is_same_v<Payload, ReasoningData>)
          return !payload.summary.empty();
        else if constexpr (std::is_same_v<Payload, FileChangesData>)
          return std::ranges::any_of(payload.changes, [](const auto &change) {
            return !change.path.empty();
          });
        else if constexpr (std::is_same_v<Payload, PlanData>)
          return !payload.explanation.empty() || !payload.steps.empty() ||
                 !payload.legacyText.empty();
        else if constexpr (std::is_same_v<Payload, ImageGenerationData>)
          return !payload.revisedPrompt.empty() || !payload.path.empty();
        else if constexpr (std::is_same_v<Payload, GenericActivityData>)
          return !payload.displayDetail.empty();
        else
          return !payload.prompt.empty() ||
                 std::ranges::any_of(payload.imagePaths,
                                     [](const auto &path) {
                                       return !path.empty();
                                     });
      },
      card.payload);
}

bool presentationEquals(const VisibleCardData &left,
                        const VisibleCardData &right) {
  if (left.kind != right.kind || left.activeWork != right.activeWork)
    return false;
  const auto *first = std::get_if<LocalPromptData>(&left.payload);
  const auto *second = std::get_if<LocalPromptData>(&right.payload);
  if (first && second) {
    return first->prompt == second->prompt &&
           first->imagePaths == second->imagePaths &&
           first->state == second->state &&
           first->error == second->error &&
           first->admittedAtMs == second->admittedAtMs &&
           first->requiresExplicitRecovery == second->requiresExplicitRecovery;
  }
  return left.payload == right.payload;
}

} // namespace

MarkdownTextView::MarkdownTextView(
    const QString &markdown,
    std::shared_ptr<QTextDocument> preparedDocument, int initialWidth,
    QWidget *parent, bool preserveSoftLineBreaks)
    : QTextBrowser(parent),
      document_(preparedDocument ? preparedDocument
                                 : std::make_shared<QTextDocument>()),
      preserveSoftLineBreaks_(preserveSoftLineBreaks) {
  setObjectName(QStringLiteral("markdownTextView"));
  setProperty("kind", "body");
  setStyleSheet(QStringLiteral(
      "QTextBrowser#markdownTextView{background:transparent;border:0;"
      "padding:0;margin:0;}"));
  setFrameShape(QFrame::NoFrame);
  setContentsMargins(0, 0, 0, 0);
  setReadOnly(true);
  setTextInteractionFlags(Qt::TextSelectableByMouse |
                          Qt::TextSelectableByKeyboard |
                          Qt::LinksAccessibleByMouse |
                          Qt::LinksAccessibleByKeyboard);
  setFocusPolicy(Qt::StrongFocus);
  setOpenExternalLinks(true);
  setOpenLinks(true);
  setLineWrapMode(QTextEdit::WidgetWidth);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // The editor already owns an exact document-height cache. A fixed vertical
  // policy prevents QLayout from caching a speculative height-for-width query
  // made against an intermediate narrow parent during card construction.
  QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  setSizePolicy(policy);
  setMinimumSize(0, 0);
  if (initialWidth > 0)
    resize(initialWidth, 1);
  if (preparedDocument) {
    preferredDocumentWidth_ =
        std::max(1, static_cast<int>(std::lround(document_->textWidth())));
    preferredHeight_ =
        std::max(1, static_cast<int>(std::ceil(document_->size().height())) +
                        MarkdownBottomPaintGuard);
  }
  setDocument(document_.get());
  configureDocument();
  if (preparedDocument) {
    markdown_ = markdown;
    renderedMarkdown_ = preserveSoftLineBreaks_
                            ? presentation::userMessageMarkdown(markdown_)
                            : markdown_;
    markdownTail_ =
        presentation::markdownTailState(*document_,
                                        QStringView(renderedMarkdown_));
    setProperty("markdownSource", markdown_);
  } else {
    setContent(markdown);
  }
}

MarkdownTextView::~MarkdownTextView() {
  // QTextEdit's base destructor still refers to its current document after
  // derived members have been destroyed. Detach the shared cache document
  // first so its lifetime remains explicit.
  setDocument(new QTextDocument(this));
  document_.reset();
}

void MarkdownTextView::configureDocument() {
  document_->setDocumentMargin(0);
  document_->setDefaultFont(font());
  document_->setDefaultStyleSheet(
      QStringLiteral("a{color:#5471a6;text-decoration:none;}"));
  QTextOption option = document_->defaultTextOption();
  option.setWrapMode(QTextOption::WordWrap);
  document_->setDefaultTextOption(option);
}

bool MarkdownTextView::setContent(const QString &markdown) {
  if (markdown_ == markdown)
    return false;
  const QString rendered = preserveSoftLineBreaks_
                               ? presentation::userMessageMarkdown(markdown)
                               : markdown;
  const QTextCursor retainedCursor = textCursor();
  const bool retainedSelection = retainedCursor.hasSelection();
  const int retainedPosition = retainedCursor.position();
  const int retainedAnchor = retainedCursor.anchor();
  if (!presentation::appendMarkdownDocument(
          *document_, QStringView(renderedMarkdown_), QStringView(rendered),
          markdownTail_)) {
    presentation::replaceMarkdownDocument(*document_, rendered, markdownTail_);
  }
  markdown_ = markdown;
  renderedMarkdown_ = rendered;
  setProperty("markdownSource", markdown_);
  if (retainedSelection) {
    const int maximum = std::max(0, document_->characterCount() - 1);
    QTextCursor restored(document_.get());
    restored.setPosition(std::clamp(retainedAnchor, 0, maximum));
    restored.setPosition(std::clamp(retainedPosition, 0, maximum),
                         QTextCursor::KeepAnchor);
    setTextCursor(restored);
  }
  refreshPreferredHeight(std::max(1, viewport()->width() - 2));
  updateGeometry();
  viewport()->update();
  return true;
}

const QString &MarkdownTextView::markdownSource() const noexcept {
  return markdown_;
}

std::shared_ptr<QTextDocument> MarkdownTextView::sharedDocument() const {
  return document_;
}

bool MarkdownTextView::hasSelectedText() const {
  return textCursor().hasSelection();
}

int MarkdownTextView::selectionStart() const {
  const QTextCursor cursor = textCursor();
  return cursor.hasSelection() ? cursor.selectionStart() : -1;
}

QString MarkdownTextView::selectedText() const {
  return textCursor().selectedText();
}

void MarkdownTextView::setSelection(int start, int length) {
  const int maximum = std::max(0, document_->characterCount() - 1);
  QTextCursor cursor(document_.get());
  cursor.setPosition(std::clamp(start, 0, maximum));
  cursor.setPosition(std::clamp(start + length, 0, maximum),
                     QTextCursor::KeepAnchor);
  setTextCursor(cursor);
}

int MarkdownTextView::heightForWidth(int width) const {
  if (markdown_.isEmpty())
    return 0;
  // QVBoxLayout can ask with both the frame-inclusive and assigned child
  // width. The viewport is the single authoritative rich-text paint width.
  const int viewportWidth = viewport()->width();
  refreshPreferredHeight(
      std::max(1, viewportWidth > 0 ? viewportWidth - 2 : width - 4));
  return preferredHeight_;
}

QSize MarkdownTextView::sizeHint() const {
  QSize result = QTextBrowser::sizeHint();
  result.setHeight(heightForWidth(std::max(1, width())));
  return result;
}

QSize MarkdownTextView::minimumSizeHint() const { return {0, 0}; }

void MarkdownTextView::keyPressEvent(QKeyEvent *event) {
  if (event && event->matches(QKeySequence::Copy) && hasSelectedText()) {
    // QTextBrowser owns the platform copy semantics. Remove only the
    // presentation-only glyph used to keep authored blank prompt lines
    // visible; it is not part of the user's canonical text.
    copy();
    if (QClipboard *clipboard = QApplication::clipboard()) {
      QString text = clipboard->text();
      if (text.contains(QChar(0x200B))) {
        text.remove(QChar(0x200B));
        clipboard->setText(text);
      }
    }
    event->accept();
    return;
  }
  QTextBrowser::keyPressEvent(event);
}

void MarkdownTextView::refreshPreferredHeight(int documentWidth) const {
  if (preferredDocumentWidth_ == documentWidth && preferredHeight_ > 0)
    return;
  document_->setTextWidth(documentWidth);
  preferredDocumentWidth_ = documentWidth;
  preferredHeight_ =
      std::max(1, static_cast<int>(std::ceil(document_->size().height())) +
                      MarkdownBottomPaintGuard);
}

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
  connect(verticalScrollBar(), &QScrollBar::sliderPressed, this,
          [this] { pinScrollToStart_ = false; });
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
          [this] { pinScrollToStart_ = false; });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            QScrollBar *bar = verticalScrollBar();
            if (!pinScrollToStart_ || value == bar->minimum())
              return;
            const QSignalBlocker blocker(bar);
            bar->setValue(bar->minimum());
          });
}

bool ContentSizedTextView::setContent(const QString &content) {
  if (toPlainText() == content)
    return false;
  QScrollBar *bar = verticalScrollBar();
  const bool hadUserScrollRange = bar->maximum() > bar->minimum();
  const int previousScrollValue = bar->value();
  pinScrollToStart_ = !hadUserScrollRange;
  setPlainText(content);
  if (hadUserScrollRange)
    bar->setValue(previousScrollValue);
  else {
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::Start);
    setTextCursor(cursor);
  }
  static_cast<void>(measureAtCurrentWidth(true));
  if (!hadUserScrollRange)
    bar->setValue(bar->minimum());
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
  pinScrollToStart_ = false;
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
  static_cast<void>(measureAtCurrentWidth(true));
}

bool ContentSizedTextView::measureAtCurrentWidth(bool notifyParent) {
  const QString content = toPlainText();
  int wantedHeight = 0;
  if (!content.isEmpty()) {
    const int frame = 2 * frameWidth();
    document()->setTextWidth(std::max(1, viewport()->width()));
    wantedHeight =
        frame + static_cast<int>(std::ceil(document()->size().height()));
  }
  wantedHeight = std::clamp(wantedHeight, 0, maximumHeight());
  return setPreferredContentHeight(wantedHeight, notifyParent);
}

bool ContentSizedTextView::setPreferredContentHeight(int height,
                                                     bool notifyParent) {
  const int wantedHeight = std::clamp(height, 0, maximumHeight());
  if (wantedHeight == preferredHeight_)
    return false;
  preferredHeight_ = wantedHeight;
  if (notifyParent)
    updateGeometry();
  return true;
}

bool ContentSizedTextView::contentHeightCapped() const noexcept {
  return preferredHeight_ >= maximumHeight();
}

CommandOutputView::CommandOutputView(const QString &output, QWidget *parent)
    : QTextEdit(parent) {
  setReadOnly(true);
  setAcceptRichText(false);
  setMinimumHeight(0);
  setLineWrapMode(QTextEdit::WidgetWidth);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  document()->setDocumentMargin(0);
  setProperty("kind", "code");
  setObjectName(QStringLiteral("commandOutputView"));
  setStyleSheet(
      QStringLiteral("QTextEdit#commandOutputView{background:#111827;"
                     "color:#e5e7eb;border-radius:6px;padding:%1px %2px;}")
          .arg(CommandOutputVerticalPadding)
          .arg(CommandOutputHorizontalPadding));
  ensurePolished();
  const int lineHeight = std::max(1, fontMetrics().lineSpacing());
  const int contentBudget =
      MaximumCommandOutputHeight - 2 * CommandOutputVerticalPadding;
  const int maximumRows = std::max(1, contentBudget / lineHeight);
  setMaximumHeight(2 * CommandOutputVerticalPadding + maximumRows * lineHeight);

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
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this, [this](int) {
    preservedScrollValue_ = verticalScrollBar()->sliderPosition();
    // QPlainTextEdit scroll values are block based: one unit is a complete
    // output line, not a one-pixel rounding tolerance.
    followsLatest_ = preservedScrollValue_ >= verticalScrollBar()->maximum();
  });
  connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int) {
            if (!programmaticScroll_)
              settleScroll();
          });

  setOutput(output);
  static_cast<void>(measureAtCurrentWidth(false));
  settleScroll();
}

bool CommandOutputView::retainsWheelGesture(QWheelEvent *event) {
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
    return canScroll;
  } else if (!wheelGestureActive_) {
    wheelGestureActive_ = true;
    wheelGestureDecided_ = hasDirection;
    wheelGestureOwned_ = hasDirection && canScroll;
  } else if (!wheelGestureDecided_ && hasDirection) {
    wheelGestureDecided_ = true;
    wheelGestureOwned_ = canScroll;
  }
  return wheelGestureDecided_ && wheelGestureOwned_;
}

QSize CommandOutputView::sizeHint() const {
  QSize result = QTextEdit::sizeHint();
  result.setHeight(preferredHeight_);
  return result;
}

QSize CommandOutputView::minimumSizeHint() const {
  QSize result = QTextEdit::minimumSizeHint();
  result.setHeight(0);
  return result;
}

CommandOutputView::ScrollState CommandOutputView::scrollState() const {
  return {followsLatest_, followsLatest_ ? preservedScrollValue_
                                         : verticalScrollBar()->value()};
}

bool CommandOutputView::followsLatest() const noexcept {
  return followsLatest_;
}

bool CommandOutputView::isHeightCapped() const noexcept {
  return preferredHeight_ >= maximumHeight();
}

bool CommandOutputView::setOutput(const QString &output) {
  QElapsedTimer commitTimer;
  commitTimer.start();
  const QString displayOutput = trimmedTrailingLines(output);
  if (currentOutput_ == displayOutput) {
    settleScroll();
    scheduleScrollSettlement();
    return false;
  }

  const bool retainedHeightIsCapped = isHeightCapped();
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
  // Once the output has reached its bounded height, subsequent text cannot
  // change the enclosing card's geometry. Avoid whole-document geometry and
  // an ancestor LayoutRequest for the common streaming case.
  if (outputRequiresMaximumHeight(displayOutput)) {
    static_cast<void>(setPreferredContentHeight(maximumHeight(), true));
    setProperty("boundedOutputMeasurements",
                property("boundedOutputMeasurements").toULongLong() + 1);
  } else if (!retainedHeightIsCapped || !appendOnly || displayOutput.isEmpty()) {
    static_cast<void>(measureAtCurrentWidth(true));
    setProperty("fullOutputMeasurements",
                property("fullOutputMeasurements").toULongLong() + 1);
  } else {
    viewport()->update();
  }
  settleScroll();
  scheduleScrollSettlement();
  setProperty("lastOutputCommitMicros", commitTimer.nsecsElapsed() / 1000);
  return true;
}

bool CommandOutputView::outputRequiresMaximumHeight(
    const QString &output) const {
  if (output.isEmpty())
    return false;
  const int lineHeight = std::max(1, fontMetrics().lineSpacing());
  const int availableHeight =
      std::max(1, maximumHeight() - 2 * CommandOutputVerticalPadding);
  const int requiredLines = availableHeight / lineHeight + 1;
  const int availableWidth = std::max(1, viewport()->width());
  int visualLines = 0;
  qsizetype begin = 0;
  while (begin <= output.size()) {
    const qsizetype end = output.indexOf(QLatin1Char('\n'), begin);
    const qsizetype length =
        end < 0 ? output.size() - begin : end - begin;
    const int advance =
        fontMetrics().horizontalAdvance(output.sliced(begin, length));
    visualLines += std::max(1, (advance + availableWidth - 1) / availableWidth);
    if (visualLines >= requiredLines)
      return true;
    if (end < 0)
      break;
    begin = end + 1;
  }
  return false;
}

bool CommandOutputView::measureAtCurrentWidth(bool notifyParent) {
  if (currentOutput_.isEmpty())
    return setPreferredContentHeight(0, notifyParent);
  if (outputRequiresMaximumHeight(currentOutput_))
    return setPreferredContentHeight(maximumHeight(), notifyParent);

  // QTextDocument::size() is the authoritative laid-out extent. Newer Qt
  // versions can round a wrapped document slightly taller than the union of
  // its QTextBlock layout rectangles; using the latter would then create a
  // needless one-step inner scrollbar.
  const qreal contentHeight =
      2 * CommandOutputVerticalPadding + document()->size().height();
  if (contentHeight >= maximumHeight())
    return setPreferredContentHeight(maximumHeight(), notifyParent);
  return setPreferredContentHeight(
      static_cast<int>(std::ceil(contentHeight)),
      notifyParent);
}

bool CommandOutputView::setPreferredContentHeight(int height,
                                                  bool notifyParent) {
  const int wantedHeight = std::clamp(height, 0, maximumHeight());
  if (wantedHeight == preferredHeight_)
    return false;
  preferredHeight_ = wantedHeight;
  if (notifyParent)
    updateGeometry();
  return true;
}

void CommandOutputView::resizeEvent(QResizeEvent *event) {
  QTextEdit::resizeEvent(event);
  if (outputRequiresMaximumHeight(currentOutput_)) {
    static_cast<void>(setPreferredContentHeight(maximumHeight(), true));
    setProperty("boundedOutputMeasurements",
                property("boundedOutputMeasurements").toULongLong() + 1);
    settleScroll();
    scheduleScrollSettlement();
    return;
  }
  static_cast<void>(measureAtCurrentWidth(true));
  setProperty("fullOutputMeasurements",
              property("fullOutputMeasurements").toULongLong() + 1);
  settleScroll();
  scheduleScrollSettlement();
}

void CommandOutputView::restoreScrollState(const ScrollState &state) {
  followsLatest_ = state.followsLatest;
  preservedScrollValue_ = std::max(0, state.value);
  settleScroll();
  scheduleScrollSettlement();
}

void CommandOutputView::wheelEvent(QWheelEvent *event) {
  QScrollBar *bar = verticalScrollBar();
  const int delta = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                  : event->angleDelta().y();
  if (delta > 0)
    followsLatest_ = false;
  const bool atBoundary = bar->maximum() <= bar->minimum() ||
                          (delta > 0 && bar->value() <= bar->minimum()) ||
                          (delta < 0 && bar->value() >= bar->maximum());
  if (atBoundary)
    event->accept();
  else
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

void CommandOutputView::scheduleScrollSettlement() {
  if (scrollSettlementPending_)
    return;
  scrollSettlementPending_ = true;
  QTimer::singleShot(0, this, [this] {
    scrollSettlementPending_ = false;
    settleScroll();
  });
}

bool CommandOutputView::isAtBottom() const {
  return verticalScrollBar()->value() >= verticalScrollBar()->maximum();
}

class ConversationCard::Impl final {
public:
  Impl(ConversationCard *owner, const VisibleCardData &initial,
       bool commandInitiallyCollapsed, bool imageInitiallyCollapsed,
       bool fileChangesInitiallyCollapsed,
       std::shared_ptr<QTextDocument> preparedMarkdownDocument,
       std::optional<bool> collapsedOverride)
      : owner(owner), current(initial),
        collapsed(collapsedOverride.value_or(initiallyCollapsed(
            initial.kind, commandInitiallyCollapsed, imageInitiallyCollapsed,
            fileChangesInitiallyCollapsed))),
        deferCollapsedBodyProjection(collapsedOverride.has_value()),
        preparedMarkdownDocument(std::move(preparedMarkdownDocument)) {
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
    headerLayout->addSpacing(4);
    headerLayout->addWidget(disclosure, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(header);

    content = new QWidget(owner);
    content->setObjectName(QStringLiteral("conversationCardContent"));
    contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(6);
    const QMargins cardMargins = layout->contentsMargins();
    content->resize(std::max(1, owner->contentsRect().width() -
                                   cardMargins.left() - cardMargins.right()),
                    1);
    layout->addWidget(content);

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
    if (initial.activeWork)
      setActiveWork(*initial.activeWork);
    refreshCopyPresentation();
    refreshFoldPresentation();
  }

  [[nodiscard]] bool canApply(const VisibleCardData &next) const noexcept {
    return current.key == next.key && (current.kind == next.kind ||
                                       (current.kind == CardKind::LocalPrompt &&
                                        next.kind == CardKind::UserMessage));
  }

  PresentationImpact applyPresentation(const VisibleCardData &next) {
    if (!canApply(next)) {
      Q_ASSERT_X(false, "ConversationCard::apply",
                 "a persistent conversation card received an incompatible "
                 "key or kind");
      return PresentationImpact::None;
    }
    const bool becomingAuthoritative = current.kind == CardKind::LocalPrompt &&
                                       next.kind == CardKind::UserMessage;
    const bool payloadChanged = current.payload != next.payload;
    const bool presentationChanged =
        becomingAuthoritative || !presentationEquals(current, next);
    const bool activeWorkOnly = !becomingAuthoritative && !payloadChanged &&
                                current.activeWork != next.activeWork;
    bool cappedCommandOutputOnly = false;
    bool commandLifecycleOnly = false;
    if (!becomingAuthoritative && output && output->isHeightCapped() &&
        current.kind == CardKind::CommandExecution &&
        next.kind == CardKind::CommandExecution) {
      const auto *before = std::get_if<CommandExecutionData>(&current.payload);
      const auto *after = std::get_if<CommandExecutionData>(&next.payload);
      cappedCommandOutputOnly =
          before && after && before->output != after->output &&
          after->output.starts_with(before->output) &&
          before->command == after->command &&
          before->status == after->status && before->cwd == after->cwd &&
          before->exitCode == after->exitCode &&
          before->durationMilliseconds == after->durationMilliseconds &&
          terminalOutputHasVisibleText(before->output) &&
          terminalOutputHasVisibleText(after->output) &&
          current.activeWork == next.activeWork;
    }
    if (!becomingAuthoritative &&
        current.kind == CardKind::CommandExecution &&
        next.kind == CardKind::CommandExecution) {
      const auto *before = std::get_if<CommandExecutionData>(&current.payload);
      const auto *after = std::get_if<CommandExecutionData>(&next.payload);
      commandLifecycleOnly =
          before && after && before->command == after->command &&
          before->output == after->output && before->cwd == after->cwd &&
          !before->status.empty() && !after->status.empty() &&
          commandHasMetadata(*before) && commandHasMetadata(*after);
    }
    if (becomingAuthoritative)
      promoteToAuthoritativeUserMessage();
    bool fileChangesBodyChanged = true;
    if (!becomingAuthoritative && current.kind == CardKind::FileChanges &&
        next.kind == CardKind::FileChanges) {
      const auto *before = std::get_if<FileChangesData>(&current.payload);
      const auto *after = std::get_if<FileChangesData>(&next.payload);
      fileChangesBodyChanged =
          !before || !after || before->changes != after->changes ||
          before->cwd != after->cwd;
    }
    current = next;
    if (!presentationChanged)
      return PresentationImpact::None;
    if (activeWorkOnly) {
      setActiveWork(next.activeWork.value_or(false));
      return PresentationImpact::PaintOnly;
    }
    const int previousNaturalHeight =
        commandLifecycleOnly && !collapsed ? naturalHeightForCurrentWidth()
                                           : -1;
    std::visit(
        [this, fileChangesBodyChanged](const auto &payload) {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, FileChangesData>)
            updateComposition(payload, fileChangesBodyChanged, !collapsed);
          else
            updateComposition(payload);
        },
        next.payload);
    if (next.activeWork)
      setActiveWork(*next.activeWork);
    if (fileChangesBodyChanged)
      refreshCopyPresentation();
    refreshFoldPresentation();
    const int nextNaturalHeight = commandLifecycleOnly
                                      ? naturalHeightForCurrentWidth()
                                      : -1;
    const bool measuredLifecyclePaintOnly =
        commandLifecycleOnly && previousNaturalHeight >= 0 &&
        nextNaturalHeight == previousNaturalHeight;
    const bool fileChangesLifecycleOnly = next.kind == CardKind::FileChanges &&
                                          (!fileChangesBodyChanged || collapsed);
    const bool geometryChanged = !cappedCommandOutputOnly &&
                                 !measuredLifecyclePaintOnly &&
                                 !fileChangesLifecycleOnly && !collapsed;
    if (geometryChanged)
      owner->updateGeometry();
    owner->update();
    return geometryChanged ? PresentationImpact::GeometryChanged
                           : PresentationImpact::PaintOnly;
  }

  [[nodiscard]] int naturalHeightForCurrentWidth() {
    if (!layout || owner->width() <= 0)
      return -1;
    layout->invalidate();
    const int width = owner->contentsRect().width();
    return layout->hasHeightForWidth()
               ? layout->heightForWidth(width) + 2 * owner->frameWidth()
               : layout->sizeHint().height() + 2 * owner->frameWidth();
  }

  void promoteToAuthoritativeUserMessage() {
    if (animationTimer)
      animationTimer->stop();
    if (pendingDelayTimer)
      pendingDelayTimer->stop();
    pendingFeedbackVisible = false;
    pendingFeedbackDeadlineMs.reset();
    owner->setObjectName(QStringLiteral("conversationCard"));
    owner->setProperty("conversationCardKind",
                       static_cast<int>(CardKind::UserMessage));
    owner->setProperty("messageRole", "user");
    owner->setStyleSheet(QString{});
    for (QLabel *label : {title, body, metadata})
      if (label)
        label->setStyleSheet(QString{});
    if (markdownBody)
      markdownBody->setStyleSheet(QString{});
    if (metadata) {
      metadata->clear();
      metadata->hide();
    }
    if (phase) {
      if (owner->property("nestedConversationCard").toBool()) {
        showPhase(QStringLiteral("steering"),
                  QStringLiteral("steeringMessagePhase"));
        setPhaseTone(QStringLiteral("steering"));
      } else {
        phase->hide();
      }
    }
    if (recovery)
      recovery->hide();
  }

  void setCollapsed(bool next) {
    if (collapsed == next)
      return;
    collapsed = next;
    if (!collapsed) {
      std::visit(
          [this](const auto &payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, FileChangesData>)
              updateComposition(payload, false, true);
            else
              updateComposition(payload);
          },
          current.payload);
    }
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
    if (owner->property("nestedConversationCard").toBool() == nested)
      return;
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

  void setVirtualTurnRootPresentation(bool fragmented) {
    if (owner->property("virtualTurnRoot").toBool() == fragmented)
      return;
    owner->setProperty("virtualTurnRoot", fragmented);
    const QMargins margins = layout->contentsMargins();
    if (fragmented)
      turnRootBottomMargin = margins.bottom();
    layout->setContentsMargins(margins.left(), margins.top(), margins.right(),
                               fragmented ? 0 : turnRootBottomMargin);
    owner->style()->unpolish(owner);
    owner->style()->polish(owner);
    owner->updateGeometry();
    owner->update();
  }

  void setViewportVisible(bool visible) {
    if (viewportVisible == visible)
      return;
    viewportVisible = visible;
    owner->setProperty("conversationViewportVisible", visible);
    if (refreshPendingPresentation())
      owner->updateGeometry();
    owner->update();
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
    const bool expandable = hasVisibleContent();
    disclosure->setExpanded(!collapsed);
    disclosure->setVisible(expandable);
    content->setVisible(expandable && !collapsed);
  }

  void refreshCopyPresentation() {
    copy->setVisible(cardHasCopyContent(current));
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

  std::shared_ptr<QTextDocument> takePreparedMarkdownDocument() {
    return std::exchange(preparedMarkdownDocument, {});
  }

  std::shared_ptr<QTextDocument> takePreparedVisibleMarkdownDocument() {
    if (collapsed && deferCollapsedBodyProjection) {
      preparedMarkdownDocument.reset();
      return {};
    }
    return takePreparedMarkdownDocument();
  }

  void markBodyProjectionDeferred() {
    owner->setProperty("conversationBodyProjectionDeferred", true);
  }

  void markBodyProjectionReady() {
    if (owner->property("conversationBodyProjectionDeferred").toBool())
      owner->setProperty(
          "conversationDeferredBodyBuilds",
          owner->property("conversationDeferredBodyBuilds").toULongLong() + 1);
    owner->setProperty("conversationBodyProjectionDeferred", false);
  }

  int markdownContentWidth() const {
    const QMargins margins = layout->contentsMargins();
    return std::max(1, owner->contentsRect().width() - margins.left() -
                           margins.right());
  }

  void setImages(const QStringList &paths) {
    images->setPaths(paths);
  }

  void createComposition(const UserMessageData &message) {
    owner->setProperty("messageRole", "user");
    title->setText(QStringLiteral("You"));
    markdownBody = makeMarkdownView(
        collapsed && deferCollapsedBodyProjection ? QString{}
                                                   : text(message.text),
        takePreparedVisibleMarkdownDocument(), markdownContentWidth(),
        content, true);
    contentLayout->addWidget(markdownBody);
    createImageContainer();
    updateComposition(message);
  }

  void updateComposition(const UserMessageData &message) {
    if (collapsed && deferCollapsedBodyProjection) {
      markdownBody->setVisible(!message.text.empty());
      images->setVisible(!message.imagePaths.empty());
      markBodyProjectionDeferred();
      return;
    }
    setVisibleMarkdown(markdownBody, text(message.text));
    setImages(textList(message.imagePaths));
    markBodyProjectionReady();
  }

  void createComposition(const AgentMessageData &message) {
    owner->setProperty("messageRole", "agent");
    title->setText(QStringLiteral("Codex"));
    showPhase({}, QStringLiteral("agentMessagePhase"));
    markdownBody = makeMarkdownView(
        collapsed && deferCollapsedBodyProjection ? QString{}
                                                   : text(message.text),
        takePreparedVisibleMarkdownDocument(), markdownContentWidth(),
        content);
    contentLayout->addWidget(markdownBody);
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
    if (collapsed && deferCollapsedBodyProjection) {
      markdownBody->setVisible(!message.text.empty());
      markBodyProjectionDeferred();
      return;
    }
    setVisibleMarkdown(markdownBody, text(message.text));
    markBodyProjectionReady();
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
    const QMargins outerMargins = layout->contentsMargins();
    const int editorWidth = std::max(
        1, owner->contentsRect().width() - outerMargins.left() -
               outerMargins.right());
    command->resize(editorWidth, command->maximumHeight());
    output->resize(editorWidth, output->maximumHeight());
    updateComposition(execution);
  }

  void updateComposition(const CommandExecutionData &execution) {
    setActiveWork(isActiveStatus(execution.status));
    showStatus(text(execution.status), QStringLiteral("commandStatus"));
    if (collapsed && deferCollapsedBodyProjection) {
      command->setVisible(
          hasTextAfterTrimmingTrailingEmptyLines(execution.command));
      output->setVisible(terminalOutputHasVisibleText(execution.output));
      metadata->setVisible(commandHasMetadata(execution));
      markBodyProjectionDeferred();
      return;
    }
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
    markBodyProjectionReady();
  }

  void createComposition(const AgentActivityData &activity) {
    title->setText(QStringLiteral("Agent activity"));
    metadata = makeLabel({}, "meta", content);
    body = makeLabel({}, "body", content);
    detail = makeMarkdownView(collapsed && deferCollapsedBodyProjection
                                  ? QString{}
                                  : text(activity.resultText),
                              takePreparedVisibleMarkdownDocument(),
                              markdownContentWidth(), content);
    contentLayout->addWidget(metadata);
    contentLayout->addWidget(body);
    contentLayout->addWidget(detail);
    updateComposition(activity);
  }

  void updateComposition(const AgentActivityData &activity) {
    showStatus(text(activity.status), QStringLiteral("agentActivityStatus"));
    if (collapsed && deferCollapsedBodyProjection) {
      metadata->setVisible(!activity.tool.empty() || !activity.kind.empty() ||
                           !activity.receivers.empty() ||
                           !activity.model.empty() ||
                           !activity.reasoningEffort.empty() ||
                           !activity.childThreadId.empty() ||
                           !activity.agentPath.empty() ||
                           !activity.senderThreadId.empty());
      body->setVisible(!activity.prompt.empty());
      detail->setVisible(!activity.resultText.empty());
      markBodyProjectionDeferred();
      return;
    }
    setVisibleText(metadata, presentation::agentMetadata(activity));
    setVisibleText(body, text(activity.prompt));
    setVisibleMarkdown(detail, text(activity.resultText));
    markBodyProjectionReady();
  }

  void createComposition(const ReasoningData &reasoning) {
    title->setText(QStringLiteral("Reasoning"));
    markdownBody = makeMarkdownView(collapsed && deferCollapsedBodyProjection
                                        ? QString{}
                                        : text(reasoning.summary),
                                    takePreparedVisibleMarkdownDocument(),
                                    markdownContentWidth(), content);
    contentLayout->addWidget(markdownBody);
    updateComposition(reasoning);
  }

  void updateComposition(const ReasoningData &reasoning) {
    if (collapsed && deferCollapsedBodyProjection) {
      markdownBody->setVisible(!reasoning.summary.empty());
      markBodyProjectionDeferred();
      return;
    }
    setVisibleMarkdown(markdownBody, text(reasoning.summary));
    markBodyProjectionReady();
  }

  void createComposition(const FileChangesData &changes) {
    title->setText(QStringLiteral("File changes"));
    metadata = makeLabel({}, "meta", content);
    fileChanges = new FileChangesView(content);
    contentLayout->addWidget(fileChanges);
    contentLayout->addWidget(metadata);
    updateComposition(changes, true, !collapsed);
  }

  void updateComposition(const FileChangesData &changes, bool contentChanged,
                         bool presentBody) {
    if (contentChanged)
      fileChangesBodyReady = false;
    if (!presentBody) {
      markBodyProjectionDeferred();
      showStatus(text(changes.status), QStringLiteral("fileChangesStatus"));
      return;
    }
    if (presentBody && !fileChangesBodyReady) {
      QElapsedTimer buildTimer;
      buildTimer.start();
      FileChangesRendering rendering = fileChangesRendering(changes);
      fileChanges->setVisible(!rendering.text.isEmpty());
      QStringList values{QStringLiteral("%1 paths").arg(changes.changes.size())};
      if (rendering.counts)
        values << QStringLiteral("+%1 −%2")
                      .arg(rendering.counts->additions)
                      .arg(rendering.counts->deletions);
      metadata->setText(values.join(QStringLiteral("  |  ")));
      metadata->show();
      fileChanges->setContent(std::move(rendering));
      owner->setProperty("fileChangesBodyBuildMicros",
                         buildTimer.nsecsElapsed() / 1000);
      owner->setProperty(
          "fileChangesBodyRebuilds",
          owner->property("fileChangesBodyRebuilds").toULongLong() + 1);
      fileChangesBodyReady = true;
    }
    markBodyProjectionReady();
    showStatus(text(changes.status), QStringLiteral("fileChangesStatus"));
  }

  void createComposition(const PlanData &plan) {
    title->setText(QStringLiteral("Plan"));
    markdownBody = makeMarkdownView(
        collapsed && deferCollapsedBodyProjection
            ? QString{}
            : presentation::planMarkdown(plan),
        takePreparedVisibleMarkdownDocument(), markdownContentWidth(),
        content);
    contentLayout->addWidget(markdownBody);
    updateComposition(plan);
  }

  void updateComposition(const PlanData &plan) {
    if (collapsed && deferCollapsedBodyProjection) {
      markdownBody->setVisible(!plan.explanation.empty() ||
                               !plan.steps.empty() || !plan.legacyText.empty());
      markBodyProjectionDeferred();
      return;
    }
    setVisibleMarkdown(markdownBody, presentation::planMarkdown(plan));
    markBodyProjectionReady();
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
    if (collapsed && deferCollapsedBodyProjection) {
      body->setVisible(!image.revisedPrompt.empty());
      images->setVisible(!image.path.empty());
      markBodyProjectionDeferred();
      return;
    }
    setVisibleText(body, text(image.revisedPrompt));
    setImages(image.path.empty() ? QStringList{}
                                 : QStringList{text(image.path)});
    markBodyProjectionReady();
  }

  void createComposition(const GenericActivityData &activity) {
    metadata = makeLabel({}, "meta", content);
    metadata->setObjectName(QStringLiteral("genericActivityMetadata"));
    contentLayout->addWidget(metadata);
    updateComposition(activity);
  }

  void updateComposition(const GenericActivityData &activity) {
    title->setText(presentation::genericActivityTitle(activity));
    showStatus(text(activity.status), QStringLiteral("genericActivityStatus"));
    if (collapsed && deferCollapsedBodyProjection) {
      metadata->setVisible(!activity.displayDetail.empty());
      markBodyProjectionDeferred();
      return;
    }
    metadata->setText(
        presentation::boundedGenericActivityDetail(activity));
    metadata->show();
    markBodyProjectionReady();
  }

  void createComposition(const LocalPromptData &prompt) {
    owner->setObjectName(QStringLiteral("pendingPromptCard"));
    owner->setStyleSheet(
        QStringLiteral("QFrame#pendingPromptCard{background:transparent;"
                       "border:1px solid transparent;border-radius:8px;}"));
    title->setText(QStringLiteral("You"));
    markdownBody = makeMarkdownView(collapsed && deferCollapsedBodyProjection
                                        ? QString{}
                                        : text(prompt.prompt),
                                    takePreparedVisibleMarkdownDocument(),
                                    markdownContentWidth(), content, true);
    metadata = makeLabel({}, "meta", content);
    contentLayout->addWidget(markdownBody);
    contentLayout->addWidget(metadata);
    recovery = new QPushButton(QStringLiteral("Restore to composer"), content);
    recovery->setObjectName(QStringLiteral("promptRecoveryButton"));
    recovery->setProperty("kind", "secondary");
    recovery->setAccessibleName(QStringLiteral("Restore prompt to composer"));
    recovery->hide();
    contentLayout->addWidget(recovery, 0, Qt::AlignLeft);
    QObject::connect(recovery, &QPushButton::clicked, owner,
                     [this] { emit owner->recoveryRequested(); });
    createImageContainer();
    animationTimer = new QTimer(owner);
    animationTimer->setObjectName(QStringLiteral("pendingAnimationTimer"));
    animationTimer->setInterval(PendingAnimationIntervalMilliseconds);
    QObject::connect(animationTimer, &QTimer::timeout, owner, [this] {
      if (refreshPendingPresentation())
        owner->updateGeometry();
      owner->update();
    });
    pendingDelayTimer = new QTimer(owner);
    pendingDelayTimer->setObjectName(QStringLiteral("pendingDelayTimer"));
    pendingDelayTimer->setSingleShot(true);
    QObject::connect(pendingDelayTimer, &QTimer::timeout, owner, [this] {
      pendingFeedbackVisible = true;
      if (refreshPendingPresentation())
        owner->updateGeometry();
      owner->update();
    });
    updateComposition(prompt);
  }

  void updateComposition(const LocalPromptData &prompt) {
    if (collapsed && deferCollapsedBodyProjection) {
      markdownBody->setVisible(!prompt.prompt.empty());
      images->setVisible(!prompt.imagePaths.empty());
      markBodyProjectionDeferred();
    } else {
      setVisibleMarkdown(markdownBody, text(prompt.prompt));
      setImages(textList(prompt.imagePaths));
      markBodyProjectionReady();
    }
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
    const QString foreground = waiting  ? steering ? QString::fromLatin1(UiStyle::tealText)
                                                   : QString::fromLatin1(UiStyle::blueText)
                                : failed ? QString::fromLatin1(UiStyle::redText)
                                        : QStringLiteral("#1d2633");
    const QString style =
        QStringLiteral("background:transparent;color:%1;").arg(foreground);
    bool changed = false;
    const QString lifecycle =
        waiting ? steering ? QStringLiteral("steering · pending")
                           : QStringLiteral("pending")
                : steering ? QStringLiteral("steering") : QString{};
    const bool phaseWasVisible = phase && phase->isVisible();
    const QString previousPhase = phase ? phase->text() : QString{};
    if (!lifecycle.isEmpty()) {
      showPhase(lifecycle, steering ? QStringLiteral("steeringMessagePhase")
                                    : QStringLiteral("pendingPromptStatus"));
      setPhaseTone(steering ? QStringLiteral("steering")
                            : QStringLiteral("active"));
    } else if (phase) {
      phase->hide();
    }
    changed = changed || previousPhase != lifecycle ||
              phaseWasVisible != !lifecycle.isEmpty();
    for (QLabel *label : {title, body, metadata}) {
      if (!label)
        continue;
      if (label->styleSheet() != style) {
        label->setStyleSheet(style);
        changed = true;
      }
    }
    if (markdownBody && markdownBody->styleSheet() != style) {
      markdownBody->setStyleSheet(style);
      changed = true;
    }

    QString status;
    if (failed)
      status = prompt->error.empty()
                   ? QStringLiteral("Not sent")
                   : QStringLiteral("Not sent: %1").arg(text(prompt->error));

    changed = setVisibleText(metadata, status) || changed;
    const bool recoveryVisible = failed && prompt->requiresExplicitRecovery;
    if (recovery && recovery->isVisible() != recoveryVisible) {
      recovery->setVisible(recoveryVisible);
      changed = true;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (waiting) {
      if (prompt->admittedAtMs) {
        const std::int64_t admitted = *prompt->admittedAtMs;
        constexpr std::int64_t maximum =
            std::numeric_limits<std::int64_t>::max();
        pendingFeedbackDeadlineMs =
            admitted > maximum - PendingAnimationDelayMilliseconds
                ? maximum
                : admitted + PendingAnimationDelayMilliseconds;
      } else if (!pendingFeedbackDeadlineMs) {
        pendingFeedbackDeadlineMs = now + PendingAnimationDelayMilliseconds;
      }
      pendingFeedbackVisible =
          pendingFeedbackDeadlineMs && now >= *pendingFeedbackDeadlineMs;
    } else {
      pendingFeedbackVisible = false;
      pendingFeedbackDeadlineMs.reset();
    }
    owner->setProperty("pendingFeedbackVisible",
                       waiting && pendingFeedbackVisible);

    if (!waiting || !viewportVisible) {
      pendingDelayTimer->stop();
      animationTimer->stop();
    } else if (pendingFeedbackVisible) {
      pendingDelayTimer->stop();
      if (!animationTimer->isActive())
        animationTimer->start();
    } else {
      animationTimer->stop();
      const qint64 remaining =
          std::max<qint64>(1, *pendingFeedbackDeadlineMs - now);
      const int interval = static_cast<int>(
          std::min<qint64>(remaining, std::numeric_limits<int>::max()));
      if (!pendingDelayTimer->isActive() ||
          pendingDelayTimer->remainingTime() > interval + 1)
        pendingDelayTimer->start(interval);
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
  MarkdownTextView *markdownBody = nullptr;
  QLabel *metadata = nullptr;
  MarkdownTextView *detail = nullptr;
  ContentSizedTextView *command = nullptr;
  CommandOutputView *output = nullptr;
  FileChangesView *fileChanges = nullptr;
  QTimer *animationTimer = nullptr;
  QTimer *pendingDelayTimer = nullptr;
  QPushButton *recovery = nullptr;
  bool pendingFeedbackVisible = false;
  bool viewportVisible = true;
  std::optional<qint64> pendingFeedbackDeadlineMs;
  ImageRibbon *images = nullptr;
  bool fileChangesBodyReady = false;
  bool deferCollapsedBodyProjection = false;
  std::shared_ptr<QTextDocument> preparedMarkdownDocument;
  bool authoritativeTurnActive = false;
  int turnRootBottomMargin = 10;
};

ConversationCard::ConversationCard(const VisibleCardData &data, QWidget *parent,
                                   bool commandInitiallyCollapsed,
                                   bool imageInitiallyCollapsed,
                                   bool fileChangesInitiallyCollapsed,
                                   int initialWidth,
                                   std::shared_ptr<QTextDocument>
                                       markdownDocument,
                                   std::optional<bool> collapsedOverride)
    : QFrame(parent) {
  if (initialWidth > 0)
    resize(initialWidth, 1);
  impl_ = std::make_unique<Impl>(this, data, commandInitiallyCollapsed,
                                 imageInitiallyCollapsed,
                                 fileChangesInitiallyCollapsed,
                                 std::move(markdownDocument),
                                 collapsedOverride);
}

ConversationCard::~ConversationCard() = default;

CardKind ConversationCard::cardKind() const noexcept {
  return impl_->current.kind;
}

const VisibleCardData &ConversationCard::data() const noexcept {
  return impl_->current;
}

std::shared_ptr<QTextDocument> ConversationCard::markdownDocument() const {
  if (impl_->markdownBody)
    return impl_->markdownBody->sharedDocument();
  return impl_->detail ? impl_->detail->sharedDocument()
                       : std::shared_ptr<QTextDocument>{};
}

bool ConversationCard::isCollapsed() const noexcept { return impl_->collapsed; }

void ConversationCard::setCollapsed(bool collapsed) {
  impl_->setCollapsed(collapsed);
}

bool ConversationCard::setAuthoritativeTurnActive(bool active) {
  return impl_->setAuthoritativeTurnActive(active);
}

void ConversationCard::setNestedPresentation(bool nested) {
  impl_->setNestedConversationCard(nested);
}

void ConversationCard::setVirtualTurnRootPresentation(bool fragmented) {
  impl_->setVirtualTurnRootPresentation(fragmented);
}

void ConversationCard::setViewportVisible(bool visible) {
  impl_->setViewportVisible(visible);
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
  return applyPresentation(data) != PresentationImpact::None;
}

PresentationImpact
ConversationCard::applyPresentation(const VisibleCardData &data) {
  return impl_->applyPresentation(data);
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
    painter.setPen(QPen(QColor(QStringLiteral("#98a2b3")), 2.0));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 9.0,
                            9.0);
    return;
  }
  const auto paintActiveTurnBorder = [this] {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(QStringLiteral("#6f98e8")), 2.0));
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
  const bool animated = waiting && property("pendingFeedbackVisible").toBool();
  const QColor background =
      failed ? QColor(QString::fromLatin1(UiStyle::redSurface))
             : QColor(steering ? QString::fromLatin1(UiStyle::tealSurface)
                               : QString::fromLatin1(UiStyle::blueSurface));
  const QColor border = failed ? QColor(QString::fromLatin1(UiStyle::redBorder))
                        : waiting
                            ? QColor(steering ? QString::fromLatin1(UiStyle::tealBorderStrong)
                                              : QString::fromLatin1(UiStyle::blueBorderStrong))
                            : QColor(steering ? QString::fromLatin1(UiStyle::tealBorder)
                                              : QString::fromLatin1(UiStyle::blueBorder));
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
                                         bool imageInitiallyCollapsed,
                                         bool fileChangesInitiallyCollapsed,
                                         int initialWidth,
                                         std::shared_ptr<QTextDocument>
                                             markdownDocument,
                                         std::optional<bool> collapsedOverride) {
  return new ConversationCard(data, parent, commandInitiallyCollapsed,
                              imageInitiallyCollapsed,
                              fileChangesInitiallyCollapsed, initialWidth,
                              std::move(markdownDocument), collapsedOverride);
}

} // namespace codexui::codex::middle
