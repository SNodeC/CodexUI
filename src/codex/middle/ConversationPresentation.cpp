// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationPresentation.h"

#include "codex/UiStatus.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QBuffer>
#include <QClipboard>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextDocumentWriter>
#include <QTextOption>
#include <QTimer>
#include <QToolTip>
#include <QUuid>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace codexui::codex::middle {
namespace {

constexpr int MarkdownBottomPaintGuard = 4;
constexpr int GeneratedBlank = QTextFormat::UserProperty;
constexpr int AuthoredAttributes = QTextFormat::UserProperty + 1;
constexpr QTextDocument::MarkdownFeatures MarkdownFeatures{
    QTextDocument::MarkdownDialectGitHub, QTextDocument::MarkdownNoHTML};

void recordBlankOrigins(QTextDocument &selection, const QTextDocument &document,
                        const QString &source, int selectionStart) {
  QString origin(QChar(0x200B));
  const auto contents = [](const QTextDocument &value) {
    QString text = value.toRawText();
    for (const QTextFormat &format : value.allFormats())
      for (const QVariant &property : format.properties())
        text += property.toString() + property.toStringList().join(QString{});
    return text;
  };
  if (!contents(selection).contains(origin))
    return;
  QTextDocument tagged;
  QTextDocument *projection = &selection;
  // Without a literal or an entity, source cannot contribute U+200B. Otherwise
  // carry an explicit, collision-checked origin through the same Qt importer.
  if (source.contains(origin) || source.contains(QLatin1Char('&'))) {
    const QString corpus = source + contents(document);
    origin = QStringLiteral("\uE000\uE001");
    while (corpus.contains(origin)) {
      origin = QChar(0xE000) + QUuid::createUuid().toString(QUuid::Id128) +
               QChar(0xE001);
    }
    tagged.setLayoutEnabled(false);
    tagged.setMarkdown(presentation::userMessageMarkdown(source, origin),
                       MarkdownFeatures);
    projection = &tagged;
  }
  QTextCursor found(projection);
  while (!(found = projection->find(origin, found,
                                    QTextDocument::FindCaseSensitively))
              .isNull()) {
    QTextCharFormat format = found.charFormat();
    format.setProperty(GeneratedBlank, true);
    if (projection == &selection)
      found.setCharFormat(format);
    else
      found.insertText(QStringLiteral("\u200B"), format);
  }
  // Transfer origins only into the selected fragment, never the live document.
  // Attribute origins include image alt text/link titles.
  for (QTextBlock block = projection->begin(); block.isValid();
       block = block.next()) {
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const QTextFragment fragment = it.fragment();
      const QTextCharFormat format = fragment.charFormat();
      QTextCharFormat metadata;
      if (projection != &selection && format.boolProperty(GeneratedBlank))
        metadata.setProperty(GeneratedBlank, true);
      QTextCharFormat attributes;
      const auto properties = format.properties();
      for (auto property = properties.cbegin(); property != properties.cend();
           ++property) {
        QString value = property.value().toString();
        if (value.contains(origin))
          attributes.setProperty(property.key(), value.remove(origin));
      }
      if (!attributes.isEmpty())
        metadata.setProperty(AuthoredAttributes, QVariant::fromValue(attributes));
      if (metadata.isEmpty())
        continue;
      const int offset = projection == &selection ? 0 : selectionStart;
      const int start = std::max(0, fragment.position() - offset);
      const int end =
          std::min(selection.characterCount() - 1,
                   fragment.position() + fragment.length() - offset);
      if (start >= end)
        continue;
      QTextCursor target(&selection);
      target.setPosition(start);
      target.setPosition(end, QTextCursor::KeepAnchor);
      target.mergeCharFormat(metadata);
    }
  }
}

#if QT_CONFIG(accessibility)
class DisclosureAccessible final : public QAccessibleWidget {
public:
  explicit DisclosureAccessible(presentation::DisclosureButton *button)
      : QAccessibleWidget(button, QAccessible::Button) {}

  QAccessible::State state() const override {
    QAccessible::State result = QAccessibleWidget::state();
    const auto *button = dynamic_cast<const presentation::DisclosureButton *>(
        widget());
    result.expandable = button != nullptr;
    result.expanded = button && button->isExpanded();
    result.collapsed = button && !button->isExpanded();
    return result;
  }

  QStringList actionNames() const override {
    return {QAccessibleActionInterface::pressAction(),
            QAccessibleActionInterface::setFocusAction()};
  }

  void doAction(const QString &action) override {
    auto *button = dynamic_cast<presentation::DisclosureButton *>(widget());
    if (!button || !button->isEnabled())
      return;
    if (action == QAccessibleActionInterface::pressAction())
      button->click();
    else if (action == QAccessibleActionInterface::setFocusAction())
      button->setFocus(Qt::OtherFocusReason);
  }

  QStringList keyBindingsForAction(const QString &action) const override {
    return action == QAccessibleActionInterface::pressAction()
               ? QStringList{QStringLiteral("Space"), QStringLiteral("Enter")}
               : QStringList{};
  }
};

QAccessibleInterface *presentationAccessibleFactory(const QString &,
                                                    QObject *object) {
  auto *button = dynamic_cast<presentation::DisclosureButton *>(object);
  return button ? new DisclosureAccessible(button) : nullptr;
}

[[maybe_unused]] const bool PresentationAccessibleFactoryInstalled =
    (QAccessible::installFactory(presentationAccessibleFactory), true);
#endif

} // namespace

MarkdownTextView::MarkdownTextView(const QString &markdown, int initialWidth,
                                   QWidget *parent, bool preserveSoftLineBreaks)
    : QTextBrowser(parent), preserveSoftLineBreaks_(preserveSoftLineBreaks) {
  setObjectName(QStringLiteral("markdownTextView"));
  setProperty("kind", "body");
  setFrameShape(QFrame::NoFrame);
  setContentsMargins(0, 0, 0, 0);
  setReadOnly(true);
  setTextInteractionFlags(
      Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard |
      Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
  setFocusPolicy(Qt::StrongFocus);
  setOpenExternalLinks(true);
  setOpenLinks(true);
  setLineWrapMode(QTextEdit::WidgetWidth);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Keep the width hint: Ignored makes parent layouts probe height at width 0.
  QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  policy.setHeightForWidth(true);
  setSizePolicy(policy);
  setMinimumSize(0, 0);
  if (initialWidth > 0)
    resize(initialWidth, 1);
  configureDocument();
  setContent(markdown);
}

void MarkdownTextView::configureDocument() {
  document()->setDocumentMargin(0);
  document()->setDefaultFont(font());
  document()->setDefaultStyleSheet(
      QStringLiteral("a{color:%1;text-decoration:none;}")
          .arg(QString::fromLatin1(UiStyle::blue)));
  QTextOption option = document()->defaultTextOption();
  option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  document()->setDefaultTextOption(option);
}

bool MarkdownTextView::setContent(const QString &markdown) {
  if (markdown_ == markdown)
    return false;
  if (preparedLayoutDisabled_) {
    document()->setLayoutEnabled(true);
    preparedLayoutDisabled_ = false;
  }
  const QString rendered = preserveSoftLineBreaks_
                               ? presentation::userMessageMarkdown(markdown)
                               : markdown;
  if (!presentation::appendMarkdownDocument(
          *document(), QStringView(renderedMarkdown_), QStringView(rendered),
          markdownTail_)) {
    presentation::replaceMarkdownDocument(*document(), rendered, markdownTail_);
  }
  markdown_ = markdown;
  renderedMarkdown_ = rendered;
  preferredHeight_ = 0;
  refreshPreferredHeight(std::max(1, width()));
  updateGeometry();
  return true;
}

bool MarkdownTextView::setPreparedContent(const QString &markdown,
                                          const QString &html) {
  if (markdown_ == markdown)
    return false;
  if (html.isEmpty() || preserveSoftLineBreaks_)
    return setContent(markdown);
  document()->setLayoutEnabled(false);
  preparedLayoutDisabled_ = true;
  document()->setHtml(html);
  markdown_ = markdown;
  renderedMarkdown_ = markdown;
  markdownTail_ = presentation::markdownTailState(*document(), markdown);
  preferredHeight_ = 0;
  updateGeometry();
  return true;
}

void MarkdownTextView::invalidateGeometryEnvironment() {
  preferredDocumentWidth_ = 0;
  preferredHeight_ = 0;
  viewport()->update();
}

const QString &MarkdownTextView::markdownSource() const noexcept {
  return markdown_;
}

presentation::MarkdownTailState MarkdownTextView::markdownTailState() const {
  return markdownTail_;
}

int MarkdownTextView::heightForWidth(int width) const {
  if (markdown_.isEmpty())
    return 0;
  refreshPreferredHeight(std::max(1, width));
  return preferredHeight_;
}

QSize MarkdownTextView::sizeHint() const {
  return {width(), heightForWidth(std::max(1, width()))};
}

QSize MarkdownTextView::minimumSizeHint() const { return {0, 0}; }

QMimeData *MarkdownTextView::createMimeDataFromSelection() const {
  if (!preserveSoftLineBreaks_ ||
      presentation::userMessageMarkdown(markdown_, {}) == renderedMarkdown_)
    return QTextBrowser::createMimeDataFromSelection();
  QTextDocument cleaned;
  cleaned.setLayoutEnabled(false);
  QTextCursor selection(&cleaned);
  selection.insertFragment(QTextDocumentFragment(textCursor()));
  recordBlankOrigins(cleaned, *document(), markdown_,
                     textCursor().selectionStart());
  struct SelectionSpan {
    int position;
    int length;
    QTextCharFormat format;
  };
  std::vector<SelectionSpan> fragments;
  for (QTextBlock block = cleaned.begin(); block.isValid();
       block = block.next())
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const auto fragment = it.fragment();
      fragments.push_back(
          {fragment.position(), fragment.length(), fragment.charFormat()});
    }
  for (auto it = fragments.crbegin(); it != fragments.crend(); ++it) {
    selection.setPosition(it->position);
    selection.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
    if (it->format.boolProperty(GeneratedBlank)) {
      selection.removeSelectedText();
    } else if (it->format.hasProperty(AuthoredAttributes)) {
      selection.mergeCharFormat(
          it->format.property(AuthoredAttributes).value<QTextCharFormat>());
    }
  }
  const QTextDocumentFragment fragment(&cleaned);
  auto *mime = new QMimeData;
  mime->setText(fragment.toPlainText());
  mime->setHtml(fragment.toHtml());
  mime->setData("text/markdown", fragment.toMarkdown().toUtf8());
  QByteArray odf;
  QBuffer output(&odf);
  if (QTextDocumentWriter(&output, "ODF").write(fragment))
    mime->setData("application/vnd.oasis.opendocument.text", odf);
  return mime;
}

void MarkdownTextView::refreshPreferredHeight(int documentWidth) const {
  if (preparedLayoutDisabled_) {
    document()->setLayoutEnabled(true);
    preparedLayoutDisabled_ = false;
  }
  if (preferredDocumentWidth_ == documentWidth && preferredHeight_ > 0)
    return;
  document()->setTextWidth(documentWidth);
  preferredDocumentWidth_ = documentWidth;
  preferredHeight_ =
      std::max(1, static_cast<int>(std::ceil(document()->size().height())) +
                      MarkdownBottomPaintGuard);
}

namespace presentation {
namespace {

constexpr int CopyMorphDurationMilliseconds = 160;
constexpr int CopyCheckHoldMilliseconds = 500;

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

QString displayChangeKind(std::string_view kind) {
  if (kind.empty())
    return QStringLiteral("Changed");
  return UiStyle::humanizeLabel(text(kind));
}

qsizetype lastSimpleMarkdownParagraphStart(QStringView source) {
  qsizetype paragraphStart = 0;
  qsizetype lineEnd = source.size();
  bool foundContent = false;
  while (lineEnd > 0) {
    const qsizetype separator =
        source.lastIndexOf(QLatin1Char('\n'), lineEnd - 1);
    const qsizetype lineStart = separator + 1;
    QStringView line = source.sliced(lineStart, lineEnd - lineStart);
    if (!line.isEmpty() && line.back() == QLatin1Char('\r'))
      line.chop(1);
    bool blank = true;
    for (QChar character : line) {
      if (!character.isSpace()) {
        blank = false;
        break;
      }
    }
    if (blank) {
      if (foundContent)
        return paragraphStart;
    } else {
      foundContent = true;
      paragraphStart = lineStart;
    }
    if (separator < 0)
      break;
    lineEnd = separator;
  }
  return foundContent ? paragraphStart : 0;
}

bool simpleMarkdownParagraphs(QStringView source) {
  qsizetype lineStart = 0;
  while (lineStart <= source.size()) {
    qsizetype lineEnd = source.indexOf(QLatin1Char('\n'), lineStart);
    if (lineEnd < 0)
      lineEnd = source.size();
    QStringView line = source.sliced(lineStart, lineEnd - lineStart);
    if (!line.isEmpty() && line.back() == QLatin1Char('\r'))
      line.chop(1);
    qsizetype indentation = 0;
    while (indentation < line.size() &&
           line.at(indentation) == QLatin1Char(' '))
      ++indentation;
    const QStringView content = line.sliced(indentation);
    const auto markerSpace = [](QChar character) {
      return character == QLatin1Char(' ') || character == QLatin1Char('\t');
    };
    qsizetype hashes = 0;
    while (hashes < content.size() && content.at(hashes) == QLatin1Char('#'))
      ++hashes;
    const bool heading =
        hashes > 0 && hashes <= 6 &&
        (hashes == content.size() || markerSpace(content.at(hashes)));
    const bool quote = content.startsWith(QLatin1Char('>'));
    const bool fence = content.startsWith(QLatin1StringView("```")) ||
                       content.startsWith(QLatin1StringView("~~~"));
    const bool unorderedList = content.size() >= 2 &&
                               (content.at(0) == QLatin1Char('-') ||
                                content.at(0) == QLatin1Char('*') ||
                                content.at(0) == QLatin1Char('+')) &&
                               markerSpace(content.at(1));
    qsizetype digits = 0;
    while (digits < content.size() && content.at(digits) >= QLatin1Char('0') &&
           content.at(digits) <= QLatin1Char('9'))
      ++digits;
    const bool orderedList = digits > 0 && digits <= 9 &&
                             digits + 1 < content.size() &&
                             (content.at(digits) == QLatin1Char('.') ||
                              content.at(digits) == QLatin1Char(')')) &&
                             markerSpace(content.at(digits + 1));
    const bool referenceDefinition = content.startsWith(QLatin1Char('['));
    const bool table = content.contains(QLatin1Char('|'));
    if (indentation >= 4 || line.startsWith(QLatin1Char('\t')) || heading ||
        quote || fence || unorderedList || orderedList || referenceDefinition ||
        table)
      return false;
    if (lineEnd == source.size())
      break;
    lineStart = lineEnd + 1;
  }
  return true;
}

} // namespace

CopyButton::CopyButton(QString accessibleName, QWidget *parent)
    : QToolButton(parent) {
  setFixedSize(CardHeaderMetrics::CopyControlWidth,
               CardHeaderMetrics::LineHeight);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(std::move(accessibleName));
  setToolTip(this->accessibleName());

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
    hold_->start(CopyCheckHoldMilliseconds);
  });
  hold_ = new QTimer(this);
  hold_->setSingleShot(true);
  QObject::connect(hold_, &QTimer::timeout, this, [this] {
    returningToCopy_ = true;
    if (UiStyle::animationsEnabled(*this)) {
      morph_->setStartValue(morphProgress_);
      morph_->setEndValue(0.0);
      morph_->start();
    } else {
      finishFeedback();
    }
  });
}

void CopyButton::copyText(const QString &text, bool markdown) {
  if (text.isEmpty())
    return;
  auto *mime = new QMimeData;
  mime->setText(text);
  if (markdown)
    mime->setData("text/markdown", text.toUtf8());
  QApplication::clipboard()->setMimeData(mime);
  showCopiedFeedback();
}

void CopyButton::showCopiedFeedback() {
  morph_->stop();
  hold_->stop();
  returningToCopy_ = false;
  feedbackActive_ = true;
  if (UiStyle::animationsEnabled(*this)) {
    morph_->setStartValue(morphProgress_);
    morph_->setEndValue(1.0);
    morph_->start();
  } else {
    morphProgress_ = 1.0;
    hold_->start(CopyCheckHoldMilliseconds);
  }
  QToolTip::showText(mapToGlobal(QPoint(width() / 2, height())),
                     QStringLiteral("Copied"), this, rect(),
                     CopyCheckHoldMilliseconds);
  announce(*this, QStringLiteral("Copied"));
  update();
}

void CopyButton::changeEvent(QEvent *event) {
  QToolButton::changeEvent(event);
  if (!event || event->type() != QEvent::StyleChange ||
      UiStyle::animationsEnabled(*this) ||
      morph_->state() == QAbstractAnimation::Stopped)
    return;
  morph_->stop();
  if (returningToCopy_) {
    finishFeedback();
    return;
  }
  morphProgress_ = 1.0;
  if (!hold_->isActive())
    hold_->start(CopyCheckHoldMilliseconds);
  update();
}

void CopyButton::focusInEvent(QFocusEvent *event) {
  keyboardFocusVisible_ = event && event->reason() != Qt::MouseFocusReason;
  QToolButton::focusInEvent(event);
  update();
}

void CopyButton::focusOutEvent(QFocusEvent *event) {
  keyboardFocusVisible_ = false;
  QToolButton::focusOutEvent(event);
  update();
}

void CopyButton::paintEvent(QPaintEvent *event) {
  static_cast<void>(event);
  QColor color(QString::fromLatin1(UiStyle::secondary));
  if (!isEnabled())
    color = QColor(QString::fromLatin1(UiStyle::placeholder));
  else if (underMouse() || keyboardFocusVisible_)
    color = QColor(QString::fromLatin1(UiStyle::primary));
  if (feedbackActive_)
    color = QColor(QString::fromLatin1(UiStyle::greenText));

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.save();
  painter.setOpacity(1.0 - morphProgress_);
  painter.drawRoundedRect(QRectF(4.5, 5.5, 8.0, 9.0), 1.2, 1.2);
  painter.drawRoundedRect(QRectF(7.5, 8.5, 8.0, 9.0), 1.2, 1.2);
  painter.restore();
  painter.setOpacity(morphProgress_);
  QPainterPath check;
  check.moveTo(4.5, 12.0);
  check.lineTo(8.0, 15.5);
  check.lineTo(15.5, 7.5);
  painter.drawPath(check);
}

void CopyButton::finishFeedback() {
  returningToCopy_ = false;
  morphProgress_ = 0.0;
  feedbackActive_ = false;
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
  setAccessibleDescriptionIfChanged(*this, {});
#endif
  update();
}

DisclosureButton::DisclosureButton(QString expandAccessibleName,
                                   QString collapseAccessibleName,
                                   QWidget *parent)
    : QToolButton(parent),
      expandAccessibleName_(std::move(expandAccessibleName)),
      collapseAccessibleName_(std::move(collapseAccessibleName)) {
  setProperty("kind", "subtle");
  setFixedSize(CardHeaderMetrics::DisclosureControlWidth,
               CardHeaderMetrics::LineHeight);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(expandAccessibleName_);
  setToolTip(accessibleName());
}

void DisclosureButton::setExpanded(bool expanded) {
  if (expanded_ == expanded)
    return;
  expanded_ = expanded;
  setAccessibleName(expanded ? collapseAccessibleName_ : expandAccessibleName_);
  setToolTip(accessibleName());
#if QT_CONFIG(accessibility)
  QAccessible::State changed;
  changed.expanded = changed.collapsed = true;
  QAccessibleStateChangeEvent event(this, changed);
  QAccessible::updateAccessibility(&event);
#endif
  update();
}

void DisclosureButton::paintEvent(QPaintEvent *event) {
  static_cast<void>(event);
  QRect indicator(0, 3, 12, std::max(0, height() - 6));
  indicator.translate(expanded_ ? 3 : 5, 0);
  UiStyle::drawChevron(this, indicator, isEnabled(), underMouse() || hasFocus(),
                       expanded_ ? UiStyle::ChevronDirection::Down
                                 : UiStyle::ChevronDirection::Left);
}

QString statusLabel(const UiStatus &status) {
  if (status.empty())
    return {};
  return text(codexui::codex::displayStatus(status));
}

void setLabelTone(QLabel &label, std::string_view tone) {
  const char *color = nullptr;
  if (tone == "active")
    color = UiStyle::blueText;
  else if (tone == "success")
    color = UiStyle::greenText;
  else if (tone == "warning")
    color = UiStyle::orangeText;
  else if (tone == "danger")
    color = UiStyle::redText;
  else if (tone == "steering")
    color = UiStyle::tealText;
  const QString style =
      color ? QStringLiteral("color:%1;").arg(QString::fromLatin1(color))
            : QString{};
  if (label.styleSheet() != style)
    label.setStyleSheet(style);
}

void applyStatusLabel(QLabel &label, const UiStatus &status) {
  const QString value = statusLabel(status);
  if (label.text() != value)
    label.setText(value);
  setLabelTone(label, statusTone(status));
  label.setVisible(!value.isEmpty());
}

void setAccessibleNameIfChanged(QWidget &widget, QString name) {
  if (widget.accessibleName() != name)
    widget.setAccessibleName(std::move(name));
}

void setAccessibleDescriptionIfChanged(QWidget &widget, QString description) {
  if (widget.accessibleDescription() != description)
    widget.setAccessibleDescription(std::move(description));
}

void announce(QWidget &widget, const QString &message) {
#if QT_CONFIG(accessibility)
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  QAccessibleAnnouncementEvent event(&widget, message);
#else
  setAccessibleDescriptionIfChanged(widget, message);
  QAccessibleEvent event(&widget, QAccessible::Alert);
#endif
  QAccessible::updateAccessibility(&event);
#else
  static_cast<void>(widget);
  static_cast<void>(message);
#endif
}

QString userMessageMarkdown(QStringView source, QStringView blankOrigin) {
  QString rendered;
  rendered.reserve(source.size() + source.count(QLatin1Char('\n')) * 3);

  // Markdown treats an empty source line as a paragraph separator and Qt's
  // Markdown layout consequently paints the two adjacent paragraphs without
  // the authored empty row. For ordinary prose, keep every editor line in one
  // paragraph with explicit hard breaks and give empty lines an invisible
  // layout glyph. The canonical source remains untouched on the view and is
  // still used for copy and protocol correlation.
  const bool simple = simpleMarkdownParagraphs(source);

  bool fenced = false;
  QChar fenceMarker;
  qsizetype fenceLength = 0;
  qsizetype lineStart = 0;
  while (lineStart <= source.size()) {
    qsizetype lineEnd = source.indexOf(QLatin1Char('\n'), lineStart);
    const bool hasNewline = lineEnd >= 0;
    if (!hasNewline)
      lineEnd = source.size();
    QStringView line = source.sliced(lineStart, lineEnd - lineStart);
    if (!line.isEmpty() && line.back() == QLatin1Char('\r'))
      line.chop(1);

    qsizetype indentation = 0;
    while (indentation < line.size() && indentation < 4 &&
           line.at(indentation) == QLatin1Char(' '))
      ++indentation;
    const bool indentedCode =
        indentation >= 4 || line.startsWith(QLatin1Char('\t'));
    const QChar marker =
        indentation < line.size() ? line.at(indentation) : QChar{};
    qsizetype markerLength = 0;
    if (indentation <= 3 &&
        (marker == QLatin1Char('`') || marker == QLatin1Char('~'))) {
      while (indentation + markerLength < line.size() &&
             line.at(indentation + markerLength) == marker)
        ++markerLength;
    }
    const bool opensFence = !fenced && markerLength >= 3;
    const bool closesFence =
        fenced && marker == fenceMarker && markerLength >= fenceLength &&
        line.sliced(indentation + markerLength).trimmed().isEmpty();
    const bool fenceLine = opensFence || closesFence;
    const bool insideFence = fenced || opensFence;

    rendered += line;
    if (simple && line.trimmed().isEmpty())
      rendered += blankOrigin;
    if (hasNewline) {
      qsizetype nextEnd = source.indexOf(QLatin1Char('\n'), lineEnd + 1);
      if (nextEnd < 0)
        nextEnd = source.size();
      QStringView next = source.sliced(lineEnd + 1, nextEnd - lineEnd - 1);
      if (!next.isEmpty() && next.back() == QLatin1Char('\r'))
        next.chop(1);
      const bool currentBlank = line.trimmed().isEmpty();
      const bool nextBlank = next.trimmed().isEmpty();
      const bool alreadyHardBreak = line.endsWith(QLatin1Char('\\')) ||
                                    line.endsWith(QLatin1StringView("  "));
      if (!alreadyHardBreak &&
          (simple || (!insideFence && !fenceLine && !indentedCode &&
                      !currentBlank && !nextBlank)))
        rendered += QLatin1StringView("  ");
      rendered += QLatin1Char('\n');
    }

    if (opensFence) {
      fenced = true;
      fenceMarker = marker;
      fenceLength = markerLength;
    } else if (closesFence) {
      fenced = false;
      fenceMarker = QChar{};
      fenceLength = 0;
    }
    if (!hasNewline)
      break;
    lineStart = lineEnd + 1;
  }
  return rendered;
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
    const QString marker =
        step.status.semantic == nodegraph::NodeStatus::Completed
            ? QStringLiteral("✓")
        : step.status.semantic == nodegraph::NodeStatus::Running
            ? QStringLiteral("◉")
            : QStringLiteral("○");
    rows << QStringLiteral("%1 %2  ").arg(marker, text(step.text));
  }
  return rows.join(QLatin1Char('\n'));
}

QString agentMetadata(const AgentActivityData &activity,
                      const UiStatus &status) {
  QStringList metadata;
  if (!activity.tool.empty())
    metadata << text(activity.tool);
  if (status.empty() && !activity.kind.empty())
    metadata << text(activity.kind);
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

QString fileChangesText(const FileChangesData &changes) {
  QStringList rows;
  for (const FileChangeData &change : changes.changes) {
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

QString genericActivityTitle(const GenericActivityData &activity) {
  const QString label = UiStyle::humanizeLabel(text(activity.type));
  return label.isEmpty() ? QStringLiteral("Activity") : label;
}

void allowPreformattedMarkdownWrapping(QTextDocument &document) {
  for (QTextBlock block = document.begin(); block.isValid();
       block = block.next()) {
    QTextBlockFormat format = block.blockFormat();
    if (!format.nonBreakableLines())
      continue;
    format.setNonBreakableLines(false);
    QTextCursor cursor(block);
    cursor.setBlockFormat(format);
  }
}

void allowPreformattedMarkdownWrappingFrom(QTextDocument &document,
                                           int position) {
  for (QTextBlock block = document.findBlock(std::max(0, position));
       block.isValid(); block = block.next()) {
    QTextBlockFormat format = block.blockFormat();
    if (!format.nonBreakableLines())
      continue;
    format.setNonBreakableLines(false);
    QTextCursor cursor(block);
    cursor.setBlockFormat(format);
  }
}

MarkdownTailState markdownTailState(const QTextDocument &document,
                                    QStringView markdown) {
  if (markdown.isEmpty())
    return {};
  const qsizetype tail = lastSimpleMarkdownParagraphStart(markdown);
  const QStringView paragraph = markdown.sliced(tail);
  if (!simpleMarkdownParagraphs(paragraph))
    return {};
  // A source paragraph can span Qt blocks, including entity-created blocks
  // whose margins depend on import context. Only independent tails are spliced.
  if (tail == 0)
    return {0, 0};
  if (paragraph.contains(QLatin1Char('\n')) ||
      paragraph.contains(QLatin1Char('\r')) ||
      paragraph.contains(QChar::ParagraphSeparator) ||
      paragraph.contains(u"&#")) {
    const auto fragment = QTextDocumentFragment::fromMarkdown(
        paragraph.toString(), MarkdownFeatures);
    if (fragment.toRawText().contains(QChar::ParagraphSeparator))
      return {};
  }
  return {tail, document.lastBlock().position()};
}

void replaceMarkdownDocument(QTextDocument &document, const QString &markdown,
                             MarkdownTailState &tailState) {
  document.setMarkdown(markdown, MarkdownFeatures);
  allowPreformattedMarkdownWrapping(document);
  tailState = markdownTailState(document, QStringView(markdown));
}

QString prepareMarkdownHtml(const QString &markdown) {
  if (markdown.isEmpty())
    return {};
  QTextDocument document;
  MarkdownTailState tail;
  replaceMarkdownDocument(document, markdown, tail);
  return document.toHtml();
}

bool markdownAppendTailIsIndependent(QStringView next,
                                     const MarkdownTailState &tailState) {
  return tailState.valid() && tailState.sourceOffset <= next.size() &&
         simpleMarkdownParagraphs(next.sliced(tailState.sourceOffset));
}

bool appendMarkdownDocument(QTextDocument &document, QStringView previous,
                            QStringView next, MarkdownTailState &tailState) {
  if (!next.startsWith(previous) ||
      !markdownAppendTailIsIndependent(next, tailState))
    return false;
  const QStringView reparsedTail = next.sliced(tailState.sourceOffset);
  QTextCursor cursor(&document);
  cursor.beginEditBlock();
  cursor.setPosition(tailState.documentPosition);
  cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  cursor.insertMarkdown(reparsedTail.toString(), MarkdownFeatures);
  allowPreformattedMarkdownWrappingFrom(document,
                                        tailState.documentPosition);
  cursor.endEditBlock();
  tailState = markdownTailState(document, next);
  return true;
}

} // namespace presentation
} // namespace codexui::codex::middle
