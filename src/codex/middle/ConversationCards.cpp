// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"

#include "codex/middle/ConversationPresentation.h"

#include "codex/UiStatus.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPixmapCache>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <string_view>
#include <type_traits>
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
constexpr int CardHeaderActionSpacing = 0;

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

void setExplicitVisibility(QWidget *widget, bool visible) {
  if (widget->isHidden() == visible)
    widget->setVisible(visible);
}

ConversationCard::ContentFingerprint contentFingerprint(QStringView value) {
  const QByteArrayView bytes(reinterpret_cast<const char *>(value.data()),
                             value.size() *
                                 static_cast<qsizetype>(sizeof(QChar)));
  return {value.size(),
          QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
}

bool startsWithFingerprint(
    QStringView value,
    const ConversationCard::ContentFingerprint &fingerprint) {
  return value.size() >= fingerprint.length &&
         contentFingerprint(value.first(fingerprint.length)).digest ==
             fingerprint.digest;
}

QString trimmedTrailingLines(const QString &value) {
  qsizetype end = value.size();
  while (end > 0) {
    while (end > 0 && (value.at(end - 1) == QLatin1Char('\n') ||
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

QString thumbnailCacheKey(const ImageFileIdentity &identity,
                          qreal devicePixelRatio) {
  return QStringLiteral("codexui-thumbnail:%1:%2:%3:%4:%5")
      .arg(identity.absolutePath)
      .arg(identity.size)
      .arg(identity.modifiedMilliseconds)
      .arg(identity.metadataChangedMilliseconds)
      .arg(qRound(devicePixelRatio * 1000.0));
}

class ImageThumbnail final : public QLabel {
public:
  ImageThumbnail(QString path, std::function<void()> geometryChanged,
                 QWidget *parent)
      : QLabel(parent), path_(std::move(path)),
        identity_(imageFileIdentity(path_)),
        geometryChanged_(std::move(geometryChanged)) {
    setObjectName(QStringLiteral("messageImageThumbnail"));
    setProperty("kind", "imageThumbnail");
    setCursor(Qt::PointingHandCursor);
    setToolTip(QDir::toNativeSeparators(path_));
    setAccessibleDescription(QDir::toNativeSeparators(path_));
    setAlignment(Qt::AlignCenter);
    setMinimumSize(72, 48);
    setMaximumSize(ThumbnailMaximumWidth, ThumbnailMaximumHeight);
    loadPixmap();
  }

  [[nodiscard]] bool represents(const QString &path) const {
    return path_ == path && identity_ == imageFileIdentity(path);
  }

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::DevicePixelRatioChange)
      loadPixmap();
    return QLabel::event(event);
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && !pixmap().isNull()) {
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
  void loadPixmap() {
    const qreal devicePixelRatio = std::max<qreal>(1.0, devicePixelRatioF());
    const QString cacheKey = thumbnailCacheKey(identity_, devicePixelRatio);
    QPixmap loaded;
    const bool cacheHit =
        identity_.file && QPixmapCache::find(cacheKey, &loaded);
    if (cacheHit) {
      pendingCacheKey_.clear();
      applyPixmap(std::move(loaded), false);
      return;
    }
    if (!identity_.file) {
      pendingCacheKey_.clear();
      applyUnavailable();
      return;
    }
    if (pendingCacheKey_ == cacheKey)
      return;

    pendingCacheKey_ = cacheKey;
    const QString path = identity_.absolutePath;
    const QSize physicalTarget(
        qCeil((ThumbnailMaximumWidth - 8) * devicePixelRatio),
        qCeil((ThumbnailMaximumHeight - 8) * devicePixelRatio));
    const QPointer<ImageThumbnail> receiver(this);
    QThreadPool::globalInstance()->start(
        [receiver, path, cacheKey, physicalTarget, devicePixelRatio] {
          QImageReader reader(path);
          reader.setAutoTransform(true);
          const QSize source = reader.size();
          if (source.isValid())
            reader.setScaledSize(
                source.scaled(physicalTarget, Qt::KeepAspectRatio));
          QImage image = reader.read();
          QMetaObject::invokeMethod(
              QCoreApplication::instance(),
              [receiver, cacheKey, devicePixelRatio,
               image = std::move(image)]() mutable {
                if (!receiver || receiver->pendingCacheKey_ != cacheKey)
                  return;
                receiver->pendingCacheKey_.clear();
                QPixmap loaded;
                if (!image.isNull()) {
                  loaded = QPixmap::fromImage(std::move(image));
                  loaded.setDevicePixelRatio(devicePixelRatio);
                  QPixmapCache::insert(cacheKey, loaded);
                }
                if (loaded.isNull())
                  receiver->applyUnavailable();
                else
                  receiver->applyPixmap(std::move(loaded), true);
              },
              Qt::QueuedConnection);
        });
  }

  void applyUnavailable() {
    const QString name = QStringLiteral("Image unavailable: %1")
                             .arg(QFileInfo(path_).fileName());
    if (accessibleName() != name)
      setAccessibleName(name);
    setPixmap({});
    setText(QStringLiteral("Image unavailable\n%1")
                .arg(QFileInfo(path_).fileName()));
    setFocusPolicy(Qt::NoFocus);
    unsetCursor();
  }

  void applyPixmap(QPixmap loaded, bool notifyGeometry) {
    const QString name =
        QStringLiteral("Open image: %1").arg(QFileInfo(path_).fileName());
    if (accessibleName() != name)
      setAccessibleName(name);
    setText({});
    setFocusPolicy(Qt::StrongFocus);
    setPixmap(loaded);
    setFixedSize(loaded.deviceIndependentSize().toSize() + QSize(8, 8));
    if (notifyGeometry)
      geometryChanged_();
  }

  bool activate() {
    if (pixmap().isNull())
      return false;
    openLocalFile(path_);
    return true;
  }

  QString path_;
  ImageFileIdentity identity_;
  std::function<void()> geometryChanged_;
  QString pendingCacheKey_;
  bool leftPressArmed_ = false;
};

class ImageRibbon final : public QScrollArea {
public:
  explicit ImageRibbon(std::function<void()> geometryChanged,
                       QWidget *parent = nullptr)
      : QScrollArea(parent), geometryChanged_(std::move(geometryChanged)) {
    setObjectName(QStringLiteral("messageImages"));
    setFrameShape(QFrame::StyledPanel);
    setWidgetResizable(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

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
                            new ImageThumbnail(paths.at(index),
                                               [this] { imageSizeChanged(); },
                                               strip_),
                            0,
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

  void invalidateGeometryEnvironment() {
    const int retainedScroll = horizontalScrollBar()->value();
    layout_->invalidate();
    layout_->activate();
    naturalSize_ = layout_->sizeHint().expandedTo(QSize(0, 0));
    strip_->setFixedSize(naturalSize_);
    refreshHeight();
    horizontalScrollBar()->setValue(retainedScroll);
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QScrollArea::resizeEvent(event);
    refreshHeight();
  }

private:
  void imageSizeChanged() {
    const int previousHeight = height();
    layout_->invalidate();
    layout_->activate();
    naturalSize_ = layout_->sizeHint().expandedTo(QSize(0, 0));
    strip_->setFixedSize(naturalSize_);
    refreshHeight();
    if (height() != previousHeight)
      geometryChanged_();
  }

  void refreshHeight() {
    const int availableWidth = std::max(0, viewport()->width());
    const bool overflows = naturalSize_.width() > availableWidth;
    const int scrollBarHeight =
        overflows ? horizontalScrollBar()->sizeHint().height() : 0;
    const int target =
        std::max(0, naturalSize_.height() + scrollBarHeight + 2 * frameWidth());
    if (height() != target)
      setFixedHeight(target);
  }

  QWidget *strip_ = nullptr;
  QHBoxLayout *layout_ = nullptr;
  std::function<void()> geometryChanged_;
  QSize naturalSize_;
};

using UiStyle::makeLabel;

bool setVisibleText(QLabel *label, const QString &text) {
  const bool visible = !text.isEmpty();
  const bool explicitlyVisible = !label->isHidden();
  const bool changed = label->text() != text || explicitlyVisible != visible;
  if (label->text() != text)
    label->setText(text);
  setExplicitVisibility(label, visible);
  return changed;
}

bool setVisibleMarkdown(MarkdownTextView *view, const QString &markdown) {
  const bool visible = !markdown.isEmpty();
  const bool contentChanged = view->markdownSource() != markdown;
  const bool explicitlyVisible = !view->isHidden();
  const bool changed = contentChanged || explicitlyVisible != visible;
  if (contentChanged)
    view->setContent(markdown);
  setExplicitVisibility(view, visible);
  return changed;
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
    result.links.push_back({static_cast<int>(result.text.size()),
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

QString fileChangesMetadata(const FileChangesData &data,
                            const FileChangesRendering &rendering) {
  QStringList values{QStringLiteral("%1 paths").arg(data.changes.size())};
  if (rendering.counts)
    values << QStringLiteral("+%1 −%2")
                  .arg(rendering.counts->additions)
                  .arg(rendering.counts->deletions);
  return values.join(QStringLiteral("  |  "));
}

QString localPromptPhase(const LocalPromptData &prompt, bool nested) {
  const bool waiting = prompt.state == PromptState::Queued ||
                       prompt.state == PromptState::InFlight;
  if (waiting)
    return nested ? QStringLiteral("steering · pending")
                  : QStringLiteral("pending");
  return nested ? QStringLiteral("steering") : QString{};
}

QString localPromptStatus(const LocalPromptData &prompt) {
  if (prompt.state != PromptState::Failed)
    return {};
  return prompt.error.empty()
             ? QStringLiteral("Not sent")
             : QStringLiteral("Not sent: %1").arg(text(prompt.error));
}

using SelectionRole = ConversationCard::TextSelection::Role;

std::optional<QString> selectionSource(const VisibleCardData &card, bool nested,
                                       SelectionRole role) {
  return std::visit(
      [nested, role,
       status = card.status](const auto &payload) -> std::optional<QString> {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, CommandExecutionData> ||
                      std::is_same_v<Payload, AgentActivityData> ||
                      std::is_same_v<Payload, FileChangesData> ||
                      std::is_same_v<Payload, ImageGenerationData> ||
                      std::is_same_v<Payload, GenericActivityData>)
          if (role == SelectionRole::Phase)
            return presentation::statusLabel(status);
        if constexpr (std::is_same_v<Payload, UserMessageData>) {
          if (role == SelectionRole::Title)
            return QStringLiteral("You");
          if (role == SelectionRole::Phase && nested)
            return QStringLiteral("steering");
          if (role == SelectionRole::MarkdownBody)
            return presentation::userMessageMarkdown(text(payload.text));
        } else if constexpr (std::is_same_v<Payload, AgentMessageData>) {
          if (role == SelectionRole::Title)
            return QStringLiteral("Codex");
          if (role == SelectionRole::Phase)
            return payload.finalAnswer ? QStringLiteral("final answer")
                                       : QStringLiteral("update");
          if (role == SelectionRole::MarkdownBody)
            return text(payload.text);
        } else if constexpr (std::is_same_v<Payload, CommandExecutionData>) {
          if (role == SelectionRole::Title)
            return QStringLiteral("Command execution");
          if (role == SelectionRole::Command)
            return text(trimTrailingEmptyLines(payload.command));
          if (role == SelectionRole::Metadata)
            return commandMetadata(payload);
        } else if constexpr (std::is_same_v<Payload, AgentActivityData>) {
          if (role == SelectionRole::Title)
            return QStringLiteral("Agent activity");
          if (role == SelectionRole::Metadata)
            return presentation::agentMetadata(payload, status);
          if (role == SelectionRole::Body)
            return text(payload.prompt);
          if (role == SelectionRole::Detail)
            return text(payload.resultText);
        } else if constexpr (std::is_same_v<Payload, ReasoningData>) {
          if (role == SelectionRole::Title)
            return QStringLiteral("Reasoning");
          if (role == SelectionRole::MarkdownBody)
            return text(payload.summary);
        } else if constexpr (std::is_same_v<Payload, FileChangesData>) {
          if (role == SelectionRole::Title)
            return QStringLiteral("File changes");
          const FileChangesRendering rendering = fileChangesRendering(payload);
          if (role == SelectionRole::FileChanges)
            return rendering.text;
          if (role == SelectionRole::Metadata)
            return fileChangesMetadata(payload, rendering);
        } else if constexpr (std::is_same_v<Payload, PlanData>) {
          if (role == SelectionRole::Title)
            return QStringLiteral("Plan");
          if (role == SelectionRole::MarkdownBody)
            return presentation::planMarkdown(payload);
        } else if constexpr (std::is_same_v<Payload, ImageGenerationData>) {
          if (role == SelectionRole::Title)
            return status.empty() && payload.revisedPrompt.empty()
                       ? QStringLiteral("Image")
                       : QStringLiteral("Generated image");
          if (role == SelectionRole::Body)
            return text(payload.revisedPrompt);
        } else if constexpr (std::is_same_v<Payload, GenericActivityData>) {
          if (role == SelectionRole::Title)
            return presentation::genericActivityTitle(payload);
          if (role == SelectionRole::Metadata)
            return presentation::boundedGenericActivityDetail(payload);
        } else if constexpr (std::is_same_v<Payload, LocalPromptData>) {
          if (role == SelectionRole::Title)
            return QStringLiteral("You");
          if (role == SelectionRole::Phase)
            return localPromptPhase(payload, nested);
          if (role == SelectionRole::MarkdownBody)
            return presentation::userMessageMarkdown(text(payload.prompt));
          if (role == SelectionRole::Metadata)
            return localPromptStatus(payload);
        }
        return std::nullopt;
      },
      card.payload);
}

bool sameSelectionOwner(CardKind before, CardKind after,
                        SelectionRole role) noexcept {
  if (before == after)
    return true;
  return before == CardKind::LocalPrompt && after == CardKind::UserMessage &&
         (role == SelectionRole::Title || role == SelectionRole::MarkdownBody);
}

std::optional<std::string> visibleCommandOutput(const VisibleCardData &card) {
  const auto *command = std::get_if<CommandExecutionData>(&card.payload);
  if (!command)
    return std::nullopt;
  std::string output = trimTrailingEmptyLines(command->output);
  return terminalOutputHasVisibleText(output)
             ? std::optional<std::string>(std::move(output))
             : std::nullopt;
}

class FileChangesView final : public QTextBrowser {
public:
  explicit FileChangesView(std::function<void(QString)> openFailed,
                           QWidget *parent = nullptr)
      : QTextBrowser(parent), openFailed_(std::move(openFailed)) {
    setObjectName(QStringLiteral("fileChangesList"));
    setProperty("kind", "body");
    setFrameShape(QFrame::NoFrame);
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setLineWrapMode(QTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMinimumSize(0, 0);
    setFocusPolicy(Qt::StrongFocus);
    setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard |
        Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setAccessibleName(QStringLiteral("Changed files"));
    document()->setDocumentMargin(0);
    connect(this, &QTextBrowser::anchorClicked, this,
            [this](const QUrl &link) { activateLink(link.toString()); });
  }

  void setContent(FileChangesRendering rendering) {
    if (toPlainText() != rendering.text)
      setPlainText(rendering.text);
    openPaths_ = std::move(rendering.openPaths);
    QTextCharFormat linkFormat;
    linkFormat.setForeground(QColor(QString::fromLatin1(UiStyle::blue)));
    linkFormat.setFontUnderline(false);
    linkFormat.setAnchor(true);
    QTextCursor cursor(document());
    cursor.beginEditBlock();
    for (std::size_t index = 0; index < rendering.links.size(); ++index) {
      const FileChangesRendering::Link &link = rendering.links[index];
      linkFormat.setAnchorHref(QStringLiteral("codexui-file:%1").arg(index));
      linkFormat.setToolTip(QDir::toNativeSeparators(
          rendering.openPaths.value(static_cast<qsizetype>(index))));
      cursor.setPosition(link.start);
      cursor.setPosition(link.start + link.length, QTextCursor::KeepAnchor);
      cursor.mergeCharFormat(linkFormat);
    }
    cursor.endEditBlock();
    preferredWidth_ = 0;
    preferredHeight_ = 0;
    refreshPreferredHeight(std::max(1, maximumViewportSize().width()));
    updateGeometry();
  }

  void settleWidth(int width) {
    resize(std::max(1, width), std::max(1, height()));
    refreshPreferredHeight(std::max(1, maximumViewportSize().width()));
    updateGeometry();
  }

  void invalidateGeometryEnvironment() {
    preferredWidth_ = 0;
    preferredHeight_ = 0;
    widestUnwrappedLine_ = 0;
    allBlocksUnwrapped_ = false;
  }

  [[nodiscard]] QSize sizeHint() const override {
    QSize result = QTextBrowser::sizeHint();
    result.setHeight(
        preferredHeight(std::max(1, maximumViewportSize().width())));
    return result;
  }

  [[nodiscard]] QSize minimumSizeHint() const override { return {0, 0}; }

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

  void activateLink(const QString &link) {
    const QString path = linkPath(link);
    if (path.isEmpty() || openLocalFile(path))
      return;
    if (openFailed_)
      openFailed_(QStringLiteral("Could not open %1")
                      .arg(QDir::toNativeSeparators(path)));
  }

  int preferredHeight(int width) const {
    refreshPreferredHeight(width);
    return preferredHeight_;
  }

  void refreshPreferredHeight(int width) const {
    if (preferredWidth_ == width && preferredHeight_ > 0)
      return;
    if (preferredHeight_ > 0 && allBlocksUnwrapped_ &&
        widestUnwrappedLine_ <= width) {
      document()->setTextWidth(width);
      preferredWidth_ = width;
      return;
    }
    document()->setTextWidth(width);
    preferredWidth_ = width;
    qreal laidOutHeight = 0;
    allBlocksUnwrapped_ = true;
    widestUnwrappedLine_ = 0;
    QAbstractTextDocumentLayout *documentLayout = document()->documentLayout();
    for (QTextBlock block = document()->begin(); block.isValid();
         block = block.next()) {
      laidOutHeight += documentLayout->blockBoundingRect(block).height();
      QTextLayout *blockLayout = block.layout();
      if (!blockLayout || blockLayout->lineCount() != 1) {
        allBlocksUnwrapped_ = false;
      } else {
        widestUnwrappedLine_ = std::max(
            widestUnwrappedLine_, blockLayout->lineAt(0).naturalTextWidth());
      }
    }
    preferredHeight_ = std::max(1, static_cast<int>(std::ceil(laidOutHeight)));
  }

  QStringList openPaths_;
  std::function<void(QString)> openFailed_;
  mutable int preferredWidth_ = 0;
  mutable int preferredHeight_ = 0;
  mutable qreal widestUnwrappedLine_ = 0;
  mutable bool allBlocksUnwrapped_ = false;
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
                 std::ranges::any_of(payload.imagePaths, [](const auto &path) {
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
                 std::ranges::any_of(payload.imagePaths, [](const auto &path) {
                   return !path.empty();
                 });
      },
      card.payload);
}

bool presentationEquals(const VisibleCardData &left,
                        const VisibleCardData &right) {
  if (left.kind != right.kind || left.status != right.status)
    return false;
  const auto *first = std::get_if<LocalPromptData>(&left.payload);
  const auto *second = std::get_if<LocalPromptData>(&right.payload);
  if (first && second) {
    return first->prompt == second->prompt &&
           first->imagePaths == second->imagePaths &&
           first->state == second->state && first->error == second->error &&
           first->admittedAtMs == second->admittedAtMs &&
           first->requiresExplicitRecovery == second->requiresExplicitRecovery;
  }
  return left.payload == right.payload;
}

bool cardHasActiveWork(const VisibleCardData &card) {
  return isWorkingStatus(card.status);
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

void ContentSizedTextView::invalidateGeometryEnvironment() {
  preferredHeight_ = 0;
  static_cast<void>(measureAtCurrentWidth(false));
}

void ContentSizedTextView::settleWidth(int width) {
  resize(std::max(1, width), std::max(1, height()));
  static_cast<void>(measureAtCurrentWidth(true));
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

bool ContentSizedTextView::measureAtCurrentWidth(bool notifyParent) {
  const QString content = toPlainText();
  int wantedHeight = 0;
  if (!content.isEmpty()) {
    const int frame = 2 * frameWidth();
    document()->setTextWidth(std::max(1, maximumViewportSize().width()));
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

CommandOutputView::CommandOutputView(QWidget *parent) : QTextEdit(parent) {
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
  ensurePolished();
  setMaximumHeight(MaximumCommandOutputHeight);

  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            if (suppressScrollState_)
              return;
            if (verticalScrollBar()->isSliderDown()) {
              preservedScrollValue_ = value;
              setUserFollowLatest(isAtBottom());
            }
          });
  connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [this] {
    preservedScrollValue_ = verticalScrollBar()->value();
    setUserFollowLatest(isAtBottom());
  });
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this, [this](int) {
    if (suppressScrollState_)
      return;
    preservedScrollValue_ = verticalScrollBar()->sliderPosition();
    // A direct scrollbar action owns the exact QTextEdit pixel position.
    setUserFollowLatest(preservedScrollValue_ >=
                        verticalScrollBar()->maximum());
  });
  connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int) {
            if (!suppressScrollState_)
              settleScroll();
          });
}

void CommandOutputView::invalidateGeometryEnvironment() {
  preferredHeight_ = 0;
}

void CommandOutputView::settleWidth(int width) {
  resize(std::max(1, width), std::max(1, height()));
  measureAtCurrentWidth();
  if (!currentOutput_.isEmpty()) {
    const char *counter = isHeightCapped() ? "boundedOutputMeasurements"
                                           : "fullOutputMeasurements";
    setProperty(counter, property(counter).toULongLong() + 1);
  }
  settleScroll();
  scheduleScrollSettlement();
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

CommandOutputView::State CommandOutputView::state() const {
  const QTextCursor cursor = textCursor();
  return {followsLatest_,
          followsLatest_ ? preservedScrollValue_ : verticalScrollBar()->value(),
          cursor.hasSelection() ? cursor.position() : -1,
          cursor.hasSelection() ? cursor.anchor() : -1};
}

bool CommandOutputView::followsLatest() const noexcept {
  return followsLatest_;
}

bool CommandOutputView::isHeightCapped() const noexcept {
  return preferredHeight_ >= maximumHeight();
}

bool CommandOutputView::setOutput(const QString &output) {
  const QString displayOutput = trimmedTrailingLines(output);
  if (currentOutput_ == displayOutput)
    return false;

  const bool appendOnly =
      !currentOutput_.isEmpty() && displayOutput.startsWith(currentOutput_);
  suppressScrollState_ = true;
  if (appendOnly) {
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(displayOutput.sliced(currentOutput_.size()));
  } else {
    setPlainText(displayOutput);
  }
  currentOutput_ = displayOutput;
  suppressScrollState_ = false;
  // A capped append cannot change enclosing geometry. Its authoritative
  // document still updates in place, while the card's existing scalar height
  // and the user's selection/follow state remain valid.
  if (isHeightCapped() && appendOnly && !displayOutput.isEmpty())
    viewport()->update();
  settleScroll();
  scheduleScrollSettlement();
  return true;
}

void CommandOutputView::measureAtCurrentWidth() {
  if (currentOutput_.isEmpty()) {
    setPreferredContentHeight(0);
    return;
  }
  document()->setTextWidth(std::max(1, maximumViewportSize().width()));
  QAbstractTextDocumentLayout *layout = document()->documentLayout();
  qreal contentHeight = UiStyle::commandOutputTopPadding;
  for (QTextBlock block = document()->begin(); block.isValid();
       block = block.next()) {
    contentHeight += layout->blockBoundingRect(block).height();
    if (contentHeight >= maximumHeight()) {
      setPreferredContentHeight(maximumHeight());
      return;
    }
  }
  setPreferredContentHeight(static_cast<int>(std::ceil(contentHeight)));
}

void CommandOutputView::setPreferredContentHeight(int height) {
  const int preferredHeight = std::clamp(height, 0, maximumHeight());
  if (preferredHeight_ == preferredHeight)
    return;
  preferredHeight_ = preferredHeight;
  updateGeometry();
}

void CommandOutputView::restoreState(const State &state) {
  const bool releasedDetachedOwner = !followsLatest_ && state.followsLatest;
  followsLatest_ = state.followsLatest;
  preservedScrollValue_ = std::max(0, state.value);
  if (state.selectionPosition >= 0 && state.selectionAnchor >= 0) {
    const int maximum = std::max(0, document()->characterCount() - 1);
    QTextCursor cursor(document());
    cursor.setPosition(std::clamp(state.selectionAnchor, 0, maximum));
    cursor.setPosition(std::clamp(state.selectionPosition, 0, maximum),
                       QTextCursor::KeepAnchor);
    setTextCursor(cursor);
  } else if (textCursor().hasSelection()) {
    QTextCursor cursor = textCursor();
    cursor.clearSelection();
    setTextCursor(cursor);
  }
  settleScroll();
  scheduleScrollSettlement();
  if (releasedDetachedOwner)
    emit followLatestChanged(true);
}

void CommandOutputView::wheelEvent(QWheelEvent *event) {
  QScrollBar *bar = verticalScrollBar();
  const int delta = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                  : event->angleDelta().y();
  if (delta > 0)
    setUserFollowLatest(false);
  {
    const QScopedValueRollback suppress(suppressScrollState_, true);
    QTextEdit::wheelEvent(event);
  }
  preservedScrollValue_ = bar->value();
  if (event->phase() == Qt::NoScrollPhase || event->phase() == Qt::ScrollEnd)
    setUserFollowLatest(isAtBottom());
}

void CommandOutputView::setUserFollowLatest(bool followsLatest) {
  if (followsLatest_ == followsLatest)
    return;
  followsLatest_ = followsLatest;
  emit followLatestChanged(followsLatest_);
}

void CommandOutputView::settleScroll() {
  QScrollBar *bar = verticalScrollBar();
  const bool wasSuppressed = suppressScrollState_;
  suppressScrollState_ = true;
  const int target =
      followsLatest_
          ? bar->maximum()
          : std::clamp(preservedScrollValue_, bar->minimum(), bar->maximum());
  bar->setValue(target);
  if (followsLatest_)
    preservedScrollValue_ = target;
  suppressScrollState_ = wasSuppressed;
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
       bool initiallyCollapsed)
      : owner(owner), current(initial), collapsed(initiallyCollapsed) {
    owner->setObjectName(QStringLiteral("conversationCard"));
    owner->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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
    copy = new presentation::CopyButton(QStringLiteral("Copy card content"),
                                        header);
    copy->setObjectName(QStringLiteral("cardCopyButton"));
    disclosure = new presentation::DisclosureButton(
        QStringLiteral("Expand card"), QStringLiteral("Collapse card"), header);
    disclosure->setObjectName(QStringLiteral("cardDisclosureButton"));
    headerLayout->addWidget(title, 1);
    headerLayout->addWidget(copy, 0, Qt::AlignRight | Qt::AlignVCenter);
    headerLayout->addSpacing(
        presentation::CardHeaderMetrics::CopyDisclosureSpacing);
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
      copy->copyText(content.text, content.markdown);
    });
    owner->setProperty("kind", "raised");
    std::visit([this](const auto &payload) { createComposition(payload); },
               initial.payload);
    setActiveWork(cardHasActiveWork(initial));
    refreshCopyPresentation();
    refreshFoldPresentation();
    owner->setAccessibleName(title->text());
  }

  [[nodiscard]] bool canApply(const VisibleCardData &next) const noexcept {
    return current.key == next.key && (current.kind == next.kind ||
                                       (current.kind == CardKind::LocalPrompt &&
                                        next.kind == CardKind::UserMessage));
  }

  [[nodiscard]] ConversationCard::State state() const {
    using Role = ConversationCard::TextSelection::Role;
    ConversationCard::State result = bodyProjectionDeferred && restoredState
                                         ? *restoredState
                                         : ConversationCard::State{};
    result.sourceKind = current.kind;
    const auto capture = [&](Role role, QWidget *target) {
      std::erase_if(result.selections, [role](const auto &selection) {
        return selection.role == role;
      });
      std::optional<ConversationCard::TextSelection> selection =
          captureSelection(role, target);
      if (!selection)
        return;
      if (const std::optional<QString> source =
              selectionSource(current, nestedConversationCard, role)) {
        selection->source = contentFingerprint(*source);
        if (role == Role::MarkdownBody) {
          const auto *markdown = qobject_cast<const MarkdownTextView *>(target);
          if (!markdown)
            return;
          selection->markdownTail = markdown->markdownTailState();
        }
        result.selections.push_back(std::move(*selection));
      }
    };
    for (const auto &[role, target] : selectionTargets()) {
      if (bodyProjectionDeferred && role != Role::Title && role != Role::Phase)
        continue;
      capture(role, target);
    }
    if (!bodyProjectionDeferred) {
      result.commandOutput.reset();
      if (output) {
        CommandOutputView::State outputState = output->state();
        if (outputState.followsLatest)
          outputState.value = 0;
        if (!outputState.followsLatest || (outputState.selectionPosition >= 0 &&
                                           outputState.selectionAnchor >= 0))
          if (const std::optional<std::string> source =
                  visibleCommandOutput(current))
            result.commandOutput = ConversationCard::CommandOutputState{
                outputState, contentFingerprint(text(*source))};
      }
    }
    return result;
  }

  void restoreState(const ConversationCard::State &state) {
    restoredState = state;
    if (output)
      output->restoreState(state.commandOutput ? state.commandOutput->view
                                               : CommandOutputView::State{});
    for (const auto &[role, target] : selectionTargets()) {
      const auto selection = std::ranges::find(
          state.selections, role, &ConversationCard::TextSelection::role);
      restoreSelection(
          selection == state.selections.end() ? nullptr : &*selection, target);
    }
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
    const bool paintOnlyStatus =
        !becomingAuthoritative && !payloadChanged &&
        current.status != next.status &&
        (next.kind == CardKind::Reasoning || next.kind == CardKind::Plan);
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
          before->command == after->command && current.status == next.status &&
          before->cwd == after->cwd && before->exitCode == after->exitCode &&
          before->durationMilliseconds == after->durationMilliseconds &&
          terminalOutputHasVisibleText(before->output) &&
          terminalOutputHasVisibleText(after->output);
    }
    if (!becomingAuthoritative && current.kind == CardKind::CommandExecution &&
        next.kind == CardKind::CommandExecution) {
      const auto *before = std::get_if<CommandExecutionData>(&current.payload);
      const auto *after = std::get_if<CommandExecutionData>(&next.payload);
      commandLifecycleOnly =
          before && after && before->command == after->command &&
          before->output == after->output && before->cwd == after->cwd &&
          !current.status.empty() && !next.status.empty() &&
          commandHasMetadata(*before) && commandHasMetadata(*after);
    }
    if (becomingAuthoritative)
      promoteToAuthoritativeUserMessage();
    bool fileChangesBodyChanged = true;
    if (!becomingAuthoritative && current.kind == CardKind::FileChanges &&
        next.kind == CardKind::FileChanges) {
      const auto *before = std::get_if<FileChangesData>(&current.payload);
      const auto *after = std::get_if<FileChangesData>(&next.payload);
      fileChangesBodyChanged = !before || !after ||
                               before->changes != after->changes ||
                               before->cwd != after->cwd;
    }
    current = next;
    if (!presentationChanged)
      return PresentationImpact::None;
    if (paintOnlyStatus) {
      setActiveWork(cardHasActiveWork(next));
      return PresentationImpact::PaintOnly;
    }
    const int previousNaturalHeight =
        commandLifecycleOnly || collapsed ? naturalHeightForCurrentWidth() : -1;
    std::visit(
        [this, fileChangesBodyChanged](const auto &payload) {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, FileChangesData>)
            updateComposition(payload, fileChangesBodyChanged, !collapsed);
          else
            updateComposition(payload);
        },
        next.payload);
    setActiveWork(cardHasActiveWork(next));
    if (fileChangesBodyChanged)
      refreshCopyPresentation();
    refreshFoldPresentation();
    presentation::setAccessibleNameIfChanged(*owner, title->text());
    const int nextNaturalHeight =
        previousNaturalHeight >= 0 ? naturalHeightForCurrentWidth() : -1;
    const bool measuredHeightUnchanged =
        previousNaturalHeight >= 0 &&
        nextNaturalHeight == previousNaturalHeight;
    const bool fileChangesLifecycleOnly =
        next.kind == CardKind::FileChanges &&
        (!fileChangesBodyChanged || collapsed);
    const bool geometryChanged =
        collapsed ? previousNaturalHeight < 0 ||
                        nextNaturalHeight != previousNaturalHeight
                  : !cappedCommandOutputOnly && !measuredHeightUnchanged &&
                        !fileChangesLifecycleOnly;
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

  template <typename TextEdit>
  static std::optional<ConversationCard::TextSelection>
  captureTextSelection(ConversationCard::TextSelection::Role role,
                       const TextEdit *edit) {
    if (!edit)
      return std::nullopt;
    const QTextCursor cursor = edit->textCursor();
    if (!cursor.hasSelection())
      return std::nullopt;
    return ConversationCard::TextSelection{role, cursor.position(),
                                           cursor.anchor(),
                                           edit->verticalScrollBar()->value(),
                                           {}, {}};
  }

  static std::optional<ConversationCard::TextSelection>
  captureSelection(ConversationCard::TextSelection::Role role,
                   const QWidget *widget) {
    if (const auto *label = qobject_cast<const QLabel *>(widget)) {
      if (!label->hasSelectedText())
        return std::nullopt;
      const int anchor = label->selectionStart();
      return ConversationCard::TextSelection{
          role, anchor + static_cast<int>(label->selectedText().size()),
          anchor, 0, {}, {}};
    }
    if (const auto *edit = qobject_cast<const QTextEdit *>(widget))
      return captureTextSelection(role, edit);
    return captureTextSelection(role,
                                qobject_cast<const QPlainTextEdit *>(widget));
  }

  template <typename TextEdit>
  static void
  restoreTextSelection(const ConversationCard::TextSelection *selection,
                       TextEdit *edit) {
    if (!edit)
      return;
    if (!selection) {
      if (edit->textCursor().hasSelection()) {
        QTextCursor cursor = edit->textCursor();
        cursor.clearSelection();
        edit->setTextCursor(cursor);
      }
      return;
    }
    const int maximum = std::max(0, edit->document()->characterCount() - 1);
    QTextCursor cursor(edit->document());
    cursor.setPosition(std::clamp(selection->anchor, 0, maximum));
    cursor.setPosition(std::clamp(selection->position, 0, maximum),
                       QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);
    edit->verticalScrollBar()->setValue(selection->scrollValue);
  }

  static void restoreSelection(const ConversationCard::TextSelection *selection,
                               QWidget *widget) {
    if (auto *label = qobject_cast<QLabel *>(widget)) {
      if (!selection) {
        if (label->hasSelectedText())
          label->setSelection(0, 0);
        return;
      }
      const int maximum = static_cast<int>(label->text().size());
      const int start = std::clamp(
          std::min(selection->anchor, selection->position), 0, maximum);
      const int end = std::clamp(
          std::max(selection->anchor, selection->position), start, maximum);
      label->setSelection(start, end - start);
      return;
    }
    if (auto *edit = qobject_cast<QTextEdit *>(widget))
      restoreTextSelection(selection, edit);
    else
      restoreTextSelection(selection, qobject_cast<QPlainTextEdit *>(widget));
  }

  using SelectionTarget =
      std::pair<ConversationCard::TextSelection::Role, QWidget *>;

  [[nodiscard]] std::array<SelectionTarget, 8> selectionTargets() const {
    using Role = ConversationCard::TextSelection::Role;
    return {{{Role::Title, title},
             {Role::Phase, phase},
             {Role::Body, body},
             {Role::MarkdownBody, markdownBody},
             {Role::Metadata, metadata},
             {Role::Detail, detail},
             {Role::Command, command},
             {Role::FileChanges, fileChanges}}};
  }

  [[nodiscard]] int settleHeightForWidth(int width) {
    width = std::max(1, width);
    owner->setMinimumHeight(0);
    owner->setMaximumHeight(QWIDGETSIZE_MAX);
    owner->resize(width, std::max(1, owner->height()));

    if (!content->isHidden()) {
      const QMargins margins = layout->contentsMargins();
      const int contentWidth = std::max(
          1, owner->contentsRect().width() - margins.left() - margins.right());
      if (command && !command->isHidden())
        command->settleWidth(contentWidth);
      if (output && !output->isHidden())
        output->settleWidth(contentWidth);
      if (fileChanges && !fileChanges->isHidden())
        fileChanges->settleWidth(contentWidth);
      contentLayout->invalidate();
      const int contentHeight =
          contentLayout->hasHeightForWidth()
              ? contentLayout->heightForWidth(contentWidth)
              : contentLayout->sizeHint().height();
      content->resize(contentWidth, std::max(0, contentHeight));
      contentLayout->setGeometry(content->contentsRect());
      contentLayout->activate();
    }

    layout->invalidate();
    const int layoutWidth = owner->contentsRect().width();
    const int height =
        layout->hasHeightForWidth()
            ? layout->heightForWidth(layoutWidth) + 2 * owner->frameWidth()
            : layout->sizeHint().height() + 2 * owner->frameWidth();
    owner->setFixedHeight(std::max(1, height));
    layout->setGeometry(owner->contentsRect());
    layout->activate();
    return std::max(1, height);
  }

  void invalidateGeometryEnvironment() {
    if (markdownBody)
      markdownBody->invalidateGeometryEnvironment();
    if (detail)
      detail->invalidateGeometryEnvironment();
    if (command)
      command->invalidateGeometryEnvironment();
    if (output)
      output->invalidateGeometryEnvironment();
    if (fileChanges)
      fileChanges->invalidateGeometryEnvironment();
    if (images)
      images->invalidateGeometryEnvironment();
    refreshPendingPresentation();
    headerLayout->invalidate();
    contentLayout->invalidate();
    layout->invalidate();
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
    repolishIfPolished();
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
      if (nestedConversationCard) {
        showPhase(QStringLiteral("steering"),
                  QStringLiteral("steeringMessagePhase"));
        presentation::setLabelTone(*phase, "steering");
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

  void repolishIfPolished() {
    if (!owner->testAttribute(Qt::WA_WState_Polished))
      return;
    owner->style()->unpolish(owner);
    owner->style()->polish(owner);
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

  bool setNestedConversationCard(bool nested) {
    if (nestedConversationCard == nested)
      return false;
    nestedConversationCard = nested;
    owner->setProperty("nestedConversationCard", nested);
    if (current.kind == CardKind::UserMessage ||
        current.kind == CardKind::LocalPrompt) {
      title->setText(QStringLiteral("You"));
      if (nested) {
        showPhase(QStringLiteral("steering"),
                  QStringLiteral("steeringMessagePhase"));
        presentation::setLabelTone(*phase, "steering");
      } else if (phase) {
        phase->hide();
      }
    }
    if (current.kind == CardKind::LocalPrompt)
      refreshPendingPresentation();
    repolishIfPolished();
    owner->update();
    return true;
  }

  bool setVirtualTurnRootPresentation(bool fragmented) {
    if (virtualTurnRoot == fragmented)
      return false;
    virtualTurnRoot = fragmented;
    owner->setProperty("virtualTurnRoot", fragmented);
    const QMargins margins = layout->contentsMargins();
    if (fragmented)
      turnRootBottomMargin = margins.bottom();
    layout->setContentsMargins(margins.left(), margins.top(), margins.right(),
                               fragmented ? 0 : turnRootBottomMargin);
    repolishIfPolished();
    owner->updateGeometry();
    owner->update();
    return true;
  }

  void setViewportVisible(bool visible) {
    if (viewportVisible == visible)
      return;
    viewportVisible = visible;
    refreshViewportTabFocus();
    refreshPendingPresentation();
    owner->update();
  }

  void refreshViewportTabFocus() {
    const auto updateControl = [this](QWidget *control) {
      if (!control || control->focusPolicy() == Qt::NoFocus)
        return;
      int policy = static_cast<int>(control->focusPolicy());
      if (viewportVisible)
        policy |= static_cast<int>(Qt::TabFocus);
      else
        policy &= ~static_cast<int>(Qt::TabFocus);
      control->setFocusPolicy(static_cast<Qt::FocusPolicy>(policy));
    };
    QWidget *controls[] = {copy,    disclosure, markdownBody, detail,
                           command, output,     fileChanges,  recovery};
    for (QWidget *control : controls)
      updateControl(control);
    if (images) {
      for (QWidget *candidate : images->findChildren<QWidget *>())
        if (dynamic_cast<ImageThumbnail *>(candidate))
          updateControl(candidate);
    }
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
    setExplicitVisibility(disclosure, expandable);
    setExplicitVisibility(content, expandable && !collapsed);
    refreshPhaseSpacing();
  }

  void refreshCopyPresentation() {
    setExplicitVisibility(copy, cardHasCopyContent(current));
    refreshPhaseSpacing();
  }

  void refreshPhaseSpacing() {
    if (!phase)
      return;
    int trailing = presentation::CardHeaderMetrics::RichStatusTrailingMargin;
    if (copy->isHidden())
      trailing += presentation::CardHeaderMetrics::CopyControlWidth;
    if (disclosure->isHidden())
      trailing += presentation::CardHeaderMetrics::DisclosureControlWidth;
    const QMargins margins = phase->contentsMargins();
    if (margins.right() != trailing)
      phase->setContentsMargins(margins.left(), margins.top(), trailing,
                                margins.bottom());
  }

  void showPhase(const QString &value, const QString &objectName) {
    if (!phase) {
      phase = makeLabel({}, "messagePhase", header);
      phase->setWordWrap(false);
      phase->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
      headerLayout->insertWidget(headerLayout->indexOf(copy), phase, 0,
                                 Qt::AlignVCenter);
    }
    if (phase->objectName() != objectName)
      phase->setObjectName(objectName);
    if (phase->text() != value)
      phase->setText(value);
    if (phase->isHidden())
      phase->show();
    refreshPhaseSpacing();
  }

  void showStatus(const UiStatus &status, const QString &objectName) {
    if (!phase)
      showPhase({}, objectName);
    else if (phase->objectName() != objectName)
      phase->setObjectName(objectName);
    presentation::applyStatusLabel(*phase, status);
    refreshPhaseSpacing();
  }

  void setActiveWork(bool active) {
    if (activeWork == active)
      return;
    activeWork = active;
    owner->update();
  }

  void createImageContainer() {
    images = new ImageRibbon(
        [this] { emit owner->intrinsicGeometryChanged(); }, content);
    contentLayout->addWidget(images);
  }

  void markBodyProjectionDeferred() {
    bodyProjectionDeferred = true;
    owner->setProperty("conversationBodyProjectionDeferred", true);
  }

  void markBodyProjectionReady() {
    if (bodyProjectionDeferred)
      owner->setProperty(
          "conversationDeferredBodyBuilds",
          owner->property("conversationDeferredBodyBuilds").toULongLong() + 1);
    bodyProjectionDeferred = false;
    owner->setProperty("conversationBodyProjectionDeferred", false);
  }

  int markdownContentWidth() const {
    const QMargins margins = layout->contentsMargins();
    return std::max(1, owner->contentsRect().width() - margins.left() -
                           margins.right());
  }

  void setImages(const QStringList &paths) {
    images->setPaths(paths);
    refreshViewportTabFocus();
  }

  void createComposition(const UserMessageData &message) {
    owner->setProperty("messageRole", "user");
    title->setText(QStringLiteral("You"));
    markdownBody =
        new MarkdownTextView(collapsed ? QString{} : text(message.text),
                             markdownContentWidth(), content, true);
    contentLayout->addWidget(markdownBody);
    createImageContainer();
    updateComposition(message);
  }

  void updateComposition(const UserMessageData &message) {
    if (collapsed) {
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
    markdownBody =
        new MarkdownTextView(collapsed ? QString{} : text(message.text),
                             markdownContentWidth(), content);
    contentLayout->addWidget(markdownBody);
    updateComposition(message);
  }

  void updateComposition(const AgentMessageData &message) {
    const QString messagePhase = message.finalAnswer ? QStringLiteral("final")
                                                     : QStringLiteral("update");
    if (this->messagePhase != messagePhase) {
      this->messagePhase = messagePhase;
      owner->setProperty("messagePhase", messagePhase);
      repolishIfPolished();
    }
    showPhase(message.finalAnswer ? QStringLiteral("final answer")
                                  : QStringLiteral("update"),
              QStringLiteral("agentMessagePhase"));
    presentation::setLabelTone(*phase,
                               message.finalAnswer ? "success" : "active");
    layout->setContentsMargins(12, message.finalAnswer ? 10 : 8, 12,
                               message.finalAnswer ? 10 : 8);
    if (collapsed) {
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
    output = new CommandOutputView(content);
    output->hide();
    metadata = makeLabel({}, "meta", content);
    metadata->setObjectName(QStringLiteral("commandMetadata"));
    contentLayout->addWidget(command);
    contentLayout->addWidget(output);
    contentLayout->addWidget(metadata);
    updateComposition(execution);
  }

  void updateComposition(const CommandExecutionData &execution) {
    showStatus(current.status, QStringLiteral("commandStatus"));
    if (collapsed) {
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
    }
    setVisibleText(metadata, commandMetadata(execution));
    markBodyProjectionReady();
  }

  void createComposition(const AgentActivityData &activity) {
    title->setText(QStringLiteral("Agent activity"));
    metadata = makeLabel({}, "meta", content);
    body = makeLabel({}, "body", content);
    detail =
        new MarkdownTextView(collapsed ? QString{} : text(activity.resultText),
                             markdownContentWidth(), content);
    contentLayout->addWidget(metadata);
    contentLayout->addWidget(body);
    contentLayout->addWidget(detail);
    updateComposition(activity);
  }

  void updateComposition(const AgentActivityData &activity) {
    showStatus(current.status, QStringLiteral("agentActivityStatus"));
    if (collapsed) {
      metadata->setVisible(
          !activity.tool.empty() || !activity.kind.empty() ||
          !activity.receivers.empty() || !activity.model.empty() ||
          !activity.reasoningEffort.empty() ||
          !activity.childThreadId.empty() || !activity.agentPath.empty() ||
          !activity.senderThreadId.empty());
      body->setVisible(!activity.prompt.empty());
      detail->setVisible(!activity.resultText.empty());
      markBodyProjectionDeferred();
      return;
    }
    setVisibleText(metadata,
                   presentation::agentMetadata(activity, current.status));
    setVisibleText(body, text(activity.prompt));
    setVisibleMarkdown(detail, text(activity.resultText));
    markBodyProjectionReady();
  }

  void createComposition(const ReasoningData &reasoning) {
    title->setText(QStringLiteral("Reasoning"));
    markdownBody =
        new MarkdownTextView(collapsed ? QString{} : text(reasoning.summary),
                             markdownContentWidth(), content);
    contentLayout->addWidget(markdownBody);
    updateComposition(reasoning);
  }

  void updateComposition(const ReasoningData &reasoning) {
    if (collapsed) {
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
    fileChanges = new FileChangesView(
        [this](QString message) {
          emit owner->noticeRequested(std::move(message), true);
        },
        content);
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
      showStatus(current.status, QStringLiteral("fileChangesStatus"));
      return;
    }
    if (presentBody && !fileChangesBodyReady) {
      FileChangesRendering rendering = fileChangesRendering(changes);
      fileChanges->setVisible(!rendering.text.isEmpty());
      metadata->setText(fileChangesMetadata(changes, rendering));
      metadata->show();
      fileChanges->setContent(std::move(rendering));
      owner->setProperty(
          "fileChangesBodyRebuilds",
          owner->property("fileChangesBodyRebuilds").toULongLong() + 1);
      fileChangesBodyReady = true;
    }
    markBodyProjectionReady();
    showStatus(current.status, QStringLiteral("fileChangesStatus"));
  }

  void createComposition(const PlanData &plan) {
    title->setText(QStringLiteral("Plan"));
    markdownBody = new MarkdownTextView(
        collapsed ? QString{} : presentation::planMarkdown(plan),
        markdownContentWidth(), content);
    contentLayout->addWidget(markdownBody);
    updateComposition(plan);
  }

  void updateComposition(const PlanData &plan) {
    if (collapsed) {
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
    showStatus(current.status, QStringLiteral("imageGenerationStatus"));
    const bool generated =
        !current.status.empty() || !image.revisedPrompt.empty();
    title->setText(generated ? QStringLiteral("Generated image")
                             : QStringLiteral("Image"));
    if (collapsed) {
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
    showStatus(current.status, QStringLiteral("genericActivityStatus"));
    if (collapsed) {
      metadata->setVisible(!activity.displayDetail.empty());
      markBodyProjectionDeferred();
      return;
    }
    metadata->setText(presentation::boundedGenericActivityDetail(activity));
    metadata->show();
    markBodyProjectionReady();
  }

  void createComposition(const LocalPromptData &prompt) {
    owner->setObjectName(QStringLiteral("pendingPromptCard"));
    title->setText(QStringLiteral("You"));
    markdownBody =
        new MarkdownTextView(collapsed ? QString{} : text(prompt.prompt),
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
    QObject::connect(animationTimer, &QTimer::timeout, owner,
                     [this] { owner->update(); });
    pendingDelayTimer = new QTimer(owner);
    pendingDelayTimer->setObjectName(QStringLiteral("pendingDelayTimer"));
    pendingDelayTimer->setSingleShot(true);
    QObject::connect(pendingDelayTimer, &QTimer::timeout, owner, [this] {
      pendingFeedbackVisible = true;
      refreshPendingPresentation();
      owner->update();
    });
    updateComposition(prompt);
  }

  void updateComposition(const LocalPromptData &prompt) {
    if (collapsed) {
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

  void refreshPendingPresentation() {
    const auto *prompt = std::get_if<LocalPromptData>(&current.payload);
    if (!prompt)
      return;
    const bool waiting = prompt->state == PromptState::Queued ||
                         prompt->state == PromptState::InFlight;
    const bool failed = prompt->state == PromptState::Failed;
    const bool steering = nestedConversationCard;
    const QString foreground =
        waiting  ? steering ? QString::fromLatin1(UiStyle::tealText)
                            : QString::fromLatin1(UiStyle::blueText)
         : failed ? QString::fromLatin1(UiStyle::redText)
                 : QString::fromLatin1(UiStyle::primary);
    const QString style = QStringLiteral("color:%1;").arg(foreground);
    const QString lifecycle = localPromptPhase(*prompt, steering);
    if (!lifecycle.isEmpty()) {
      showPhase(lifecycle, steering ? QStringLiteral("steeringMessagePhase")
                                    : QStringLiteral("pendingPromptStatus"));
      presentation::setLabelTone(*phase, steering ? "steering" : "active");
    } else if (phase) {
      phase->hide();
    }
    for (QLabel *label : {title, body, metadata}) {
      if (!label)
        continue;
      if (label->styleSheet() != style)
        label->setStyleSheet(style);
    }
    if (markdownBody && markdownBody->styleSheet() != style)
      markdownBody->setStyleSheet(style);

    setVisibleText(metadata, localPromptStatus(*prompt));
    const bool recoveryVisible = failed && prompt->requiresExplicitRecovery;
    if (recovery && recovery->isVisible() != recoveryVisible)
      recovery->setVisible(recoveryVisible);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool animationsEnabled = UiStyle::animationsEnabled(*owner);
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
      pendingFeedbackVisible = animationsEnabled && pendingFeedbackDeadlineMs &&
                               now >= *pendingFeedbackDeadlineMs;
    } else {
      pendingFeedbackVisible = false;
      pendingFeedbackDeadlineMs.reset();
    }
    if (!waiting || !viewportVisible || !animationsEnabled) {
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
  }

  ConversationCard *owner = nullptr;
  VisibleCardData current;
  bool collapsed = false;
  QVBoxLayout *layout = nullptr;
  QWidget *header = nullptr;
  QHBoxLayout *headerLayout = nullptr;
  QLabel *title = nullptr;
  QLabel *phase = nullptr;
  presentation::CopyButton *copy = nullptr;
  presentation::DisclosureButton *disclosure = nullptr;
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
  bool nestedConversationCard = false;
  bool virtualTurnRoot = false;
  bool activeWork = false;
  bool bodyProjectionDeferred = false;
  std::optional<qint64> pendingFeedbackDeadlineMs;
  ImageRibbon *images = nullptr;
  bool fileChangesBodyReady = false;
  bool authoritativeTurnActive = false;
  QString messagePhase;
  int turnRootBottomMargin = 10;
  std::optional<ConversationCard::State> restoredState;
};

ConversationCard::ConversationCard(const VisibleCardData &data,
                                   bool initiallyCollapsed, QWidget *parent,
                                   int initialWidth)
    : QFrame(parent) {
  if (initialWidth > 0)
    resize(initialWidth, 1);
  impl_ = std::make_unique<Impl>(this, data, initiallyCollapsed);
}

ConversationCard::~ConversationCard() = default;

const VisibleCardData &ConversationCard::data() const noexcept {
  return impl_->current;
}

bool ConversationCard::isCollapsed() const noexcept { return impl_->collapsed; }

void ConversationCard::setCollapsed(bool collapsed) {
  if (impl_->collapsed == collapsed)
    return;
  State retained = impl_->state();
  impl_->setCollapsed(collapsed);
  impl_->restoreState(retained);
}

bool ConversationCard::setAuthoritativeTurnActive(bool active) {
  return impl_->setAuthoritativeTurnActive(active);
}

bool ConversationCard::setNestedPresentation(bool nested) {
  if (impl_->nestedConversationCard == nested)
    return false;
  State retained = impl_->state();
  static_cast<void>(normalizeState(retained, impl_->current, nested));
  const bool changed = impl_->setNestedConversationCard(nested);
  impl_->restoreState(retained);
  return changed;
}

bool ConversationCard::setVirtualTurnRootPresentation(bool fragmented) {
  return impl_->setVirtualTurnRootPresentation(fragmented);
}

void ConversationCard::setViewportVisible(bool visible) {
  impl_->setViewportVisible(visible);
}

void ConversationCard::setShowsKeyboardFocus(bool visible) {
  if (showsKeyboardFocus_ == visible)
    return;
  showsKeyboardFocus_ = visible;
  update();
}

void ConversationCard::invalidateGeometryEnvironment() {
  impl_->invalidateGeometryEnvironment();
}

int ConversationCard::settleHeightForWidth(int width) {
  return impl_->settleHeightForWidth(width);
}

ConversationCard::State ConversationCard::state() const {
  return impl_->state();
}

void ConversationCard::restoreState(const State &state) {
  impl_->restoreState(state);
}

bool ConversationCard::normalizeState(State &state, const VisibleCardData &next,
                                      bool nextNested) {
  const CardKind previousKind = state.sourceKind;
  std::erase_if(state.selections, [&](TextSelection &selection) {
    const std::optional<QString> after =
        selectionSource(next, nextNested, selection.role);
    const bool hasPreviousPrefix =
        after && startsWithFingerprint(*after, selection.source);
    const bool sameSource =
        hasPreviousPrefix && after->size() == selection.source.length;
    const bool safeAppend =
        hasPreviousPrefix &&
        (selection.role == SelectionRole::MarkdownBody
             ? std::max(selection.position, selection.anchor) <=
                       selection.markdownTail.documentPosition &&
                   presentation::markdownAppendTailIsIndependent(
                       QStringView(*after), selection.markdownTail)
             : true);
    const bool continues =
        after && sameSelectionOwner(previousKind, next.kind, selection.role) &&
        (sameSource || safeAppend);
    if (continues)
      selection.source = contentFingerprint(*after);
    return !continues;
  });

  bool releasedDetachedOwner = false;
  if (state.commandOutput) {
    const bool detached = !state.commandOutput->view.followsLatest;
    const std::optional<std::string> rawOutput = visibleCommandOutput(next);
    const std::optional<QString> output =
        rawOutput ? std::optional(text(*rawOutput)) : std::nullopt;
    const bool preservesOutput =
        output && previousKind == next.kind &&
        startsWithFingerprint(*output, state.commandOutput->source);
    if (preservesOutput)
      state.commandOutput->source = contentFingerprint(*output);
    else {
      state.commandOutput.reset();
      releasedDetachedOwner = detached;
    }
  }
  state.sourceKind = next.kind;
  return releasedDetachedOwner;
}

PresentationImpact
ConversationCard::applyPresentation(const VisibleCardData &data) {
  State retained = impl_->state();
  const bool releasedDetachedOwner =
      normalizeState(retained, data, impl_->nestedConversationCard);
  const PresentationImpact impact = impl_->applyPresentation(data);
  if (impact != PresentationImpact::None &&
      (!retained.empty() || releasedDetachedOwner))
    impl_->restoreState(retained);
  return impact;
}

bool ConversationCard::canApply(const VisibleCardData &data) const noexcept {
  return impl_->canApply(data);
}

void ConversationCard::paintEvent(QPaintEvent *event) {
  QFrame::paintEvent(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const auto paintKeyboardFocus = [this, &painter] {
    if (!showsKeyboardFocus_)
      return;

    const auto focusState =
        QStyle::State_HasFocus | QStyle::State_KeyboardFocusChange;
    QStyleOptionFocusRect option;
    option.initFrom(this);
    option.state |= focusState;
    option.rect = rect().adjusted(2, 2, -2, -2);
    style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
  };
  if (impl_->activeWork) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(
        QPen(QColor(QString::fromLatin1(UiStyle::placeholder)), 2.0));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 9.0,
                            9.0);
    paintKeyboardFocus();
    return;
  }
  const auto paintActiveTurnBorder = [this, &painter] {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(
        QPen(QColor(QString::fromLatin1(UiStyle::activeTurnBorder)), 2.0));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 8.0,
                            8.0);
  };
  if (impl_->current.kind == CardKind::UserMessage) {
    if (impl_->authoritativeTurnActive)
      paintActiveTurnBorder();
    paintKeyboardFocus();
    return;
  }
  if (impl_->current.kind != CardKind::LocalPrompt) {
    paintKeyboardFocus();
    return;
  }
  const auto *prompt = std::get_if<LocalPromptData>(&impl_->current.payload);
  if (!prompt) {
    paintKeyboardFocus();
    return;
  }

  const QRectF bounds = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
  const bool waiting = prompt->state == PromptState::Queued ||
                       prompt->state == PromptState::InFlight;
  const bool failed = prompt->state == PromptState::Failed;
  const bool steering = impl_->nestedConversationCard;
  const bool animated = waiting && impl_->pendingFeedbackVisible;
  const QColor background =
      failed ? QColor(QString::fromLatin1(UiStyle::redSurface))
             : QColor(steering ? QString::fromLatin1(UiStyle::tealSurface)
                               : QString::fromLatin1(UiStyle::blueSurface));
  const QColor border =
      failed ? QColor(QString::fromLatin1(UiStyle::redBorder))
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
    paintKeyboardFocus();
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
  sweep.setColorAt(
      0.0, QColor::fromRgba(steering ? UiStyle::pendingSteeringSweepEdge
                                     : UiStyle::pendingPromptSweepEdge));
  sweep.setColorAt(
      0.5, QColor::fromRgba(steering ? UiStyle::pendingSteeringSweepCenter
                                     : UiStyle::pendingPromptSweepCenter));
  sweep.setColorAt(
      1.0, QColor::fromRgba(steering ? UiStyle::pendingSteeringSweepEdge
                                     : UiStyle::pendingPromptSweepEdge));
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
  paintKeyboardFocus();
}

} // namespace codexui::codex::middle
