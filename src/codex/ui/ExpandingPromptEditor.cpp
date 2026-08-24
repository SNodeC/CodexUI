// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/ExpandingPromptEditor.h"

#include <QAbstractTextDocumentLayout>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace codexui {

ExpandingPromptEditor::ExpandingPromptEditor(QWidget *parent)
    : QPlainTextEdit(parent) {
  setObjectName(QStringLiteral("upcomingPromptEditor"));
  setPlaceholderText(QStringLiteral("Message Codex"));
  setLineWrapMode(QPlainTextEdit::WidgetWidth);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  maximumEditorHeight =
      fontMetrics().lineSpacing() * maximumVisibleLineCount() + 10;
  setMinimumHeight(compactHeight());
  setMaximumHeight(maximumEditorHeight);
  setFixedHeight(compactHeight());
  setStyleSheet(QStringLiteral(
      "QPlainTextEdit{background:transparent;color:#1d2633;border:0;padding:"
      "3px 2px;}"));

  connect(this, &QPlainTextEdit::textChanged, this,
          &ExpandingPromptEditor::remeasure);
}

bool ExpandingPromptEditor::requiresExpandedLayout(int widgetWidth) const {
  const QString content = toPlainText();
  if (content.isEmpty())
    return false;
  if (content.contains(QLatin1Char('\n')))
    return true;

  const int viewportReduction = std::max(0, width() - viewport()->width());
  const qreal lineWidth = std::max(1, widgetWidth - viewportReduction);
  QTextLayout layout(content, font());
  layout.setTextOption(document()->defaultTextOption());
  layout.beginLayout();
  QTextLine firstLine = layout.createLine();
  if (firstLine.isValid())
    firstLine.setLineWidth(lineWidth);
  const bool wraps = layout.createLine().isValid();
  layout.endLayout();
  return wraps;
}

void ExpandingPromptEditor::focusInEvent(QFocusEvent *event) {
  QPlainTextEdit::focusInEvent(event);
  emit focusStateChanged(true);
}

void ExpandingPromptEditor::focusOutEvent(QFocusEvent *event) {
  QPlainTextEdit::focusOutEvent(event);
  emit focusStateChanged(false);
}

void ExpandingPromptEditor::keyPressEvent(QKeyEvent *event) {
  if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
      event->modifiers().testFlag(Qt::ControlModifier) &&
      !event->isAutoRepeat()) {
    emit submitRequested();
    event->accept();
    return;
  }
  QPlainTextEdit::keyPressEvent(event);
}

void ExpandingPromptEditor::resizeEvent(QResizeEvent *event) {
  QPlainTextEdit::resizeEvent(event);
  scheduleRemeasure();
}

void ExpandingPromptEditor::scheduleRemeasure() {
  if (remeasureScheduled)
    return;
  remeasureScheduled = true;
  QTimer::singleShot(0, this, [this] {
    remeasureScheduled = false;
    remeasure();
  });
}

void ExpandingPromptEditor::remeasure() {
  if (viewport()->width() <= 0)
    return;

  document()->setTextWidth(viewport()->width());
  qreal laidOutHeight = 0;
  QAbstractTextDocumentLayout *documentLayout = document()->documentLayout();
  for (QTextBlock block = document()->begin(); block.isValid();
       block = block.next())
    laidOutHeight += documentLayout->blockBoundingRect(block).height();
  const int documentHeight = static_cast<int>(std::ceil(laidOutHeight)) + 10;
  const int wanted =
      std::clamp(documentHeight, compactHeight(), maximumEditorHeight);
  setVerticalScrollBarPolicy(wanted >= maximumEditorHeight
                                 ? Qt::ScrollBarAsNeeded
                                 : Qt::ScrollBarAlwaysOff);
  if (wanted == currentContentHeight)
    return;
  currentContentHeight = wanted;
  setFixedHeight(wanted);
  emit editorHeightChanged(wanted);
}

} // namespace codexui
