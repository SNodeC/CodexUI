// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/ExpandingPromptEditor.h"

#include "codex/AttachmentInput.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractTextDocumentLayout>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QWheelEvent>
#include <Sonnet/Speller>
#include <algorithm>
#include <cmath>
#include <utility>

namespace codexui {

    class ExpandingPromptEditor::SpellingFormat final : public QSyntaxHighlighter {
    public:
        SpellingFormat(QTextDocument* document, const QList<SpellingRange>& findings)
            : QSyntaxHighlighter(document)
            , findings(findings) {
            format.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
            format.setUnderlineColor(QColor(UiStyle::spellingError));
        }

    protected:
        void highlightBlock(const QString& text) override {
            const int start = currentBlock().position();
            const int end = start + text.size();
            auto it = std::lower_bound(findings.cbegin(), findings.cend(), start, [](SpellingRange range, int pos) {
                return range.end <= pos;
            });
            while (it != findings.cend() && it->start < end) {
                setFormat(std::max(it->start, start) - start, std::min(it->end, end) - std::max(it->start, start), format);
                ++it;
            }
        }

    private:
        const QList<SpellingRange>& findings;
        QTextCharFormat format;
    };

    ExpandingPromptEditor::ExpandingPromptEditor(QWidget* parent)
        : QPlainTextEdit(parent)
        , speller(std::make_unique<Sonnet::Speller>()) {
        // Findings must be updated before Qt projects the changed block.
        connect(document(), &QTextDocument::contentsChange, this, [this](int pos, int removed, int added) {
            if (spellingEnabled && speller->isValid() && !processingInputMethod && (removed > 0 || added > 0)) {
                shiftSpellings(pos, removed, added);
                finishPendingWord();
                for (SpellingRange word : words(pos, pos + added)) {
                    updateWord(word);
                }
            }
        });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
            if (spellingEnabled && !processingInputMethod) {
                finishPendingWord();
            }
        });
        spellingFormat = std::make_unique<SpellingFormat>(document(), findings);
        setObjectName(QStringLiteral("upcomingPromptEditor"));
        setPlaceholderText(QStringLiteral("Message Codex"));
        setAccessibleName(QStringLiteral("Message Codex"));
        setAccessibleDescription(QStringLiteral("Enter to send. Shift+Enter inserts a new line. "
                                                "Paste images or drop files to attach them."));
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setFixedHeight(compactHeight());

        connect(this, &QPlainTextEdit::textChanged, this, &ExpandingPromptEditor::scheduleRemeasure);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
            if (!contentScrollable && value != verticalScrollBar()->minimum()) {
                verticalScrollBar()->setValue(verticalScrollBar()->minimum());
            }
        });
    }

    QList<ExpandingPromptEditor::SpellingRange> ExpandingPromptEditor::words(int from, int to) const {
        QList<SpellingRange> result;
        const auto hasLetter = [](QStringView segment) {
            return std::ranges::any_of(segment.toUcs4(), [](char32_t point) {
                return QChar::isLetter(point);
            });
        };
        QTextBlock block = document()->findBlock(std::clamp(from, 0, document()->characterCount() - 1));
        while (block.isValid() && block.position() <= to) {
            const QString text = block.text();
            QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
            finder.setPosition(std::clamp(from - block.position(), 0, static_cast<int>(text.size())));
            int start = finder.position() > 0 ? finder.toPreviousBoundary() : 0;
            int end = finder.toNextBoundary();
            while (end >= 0 && block.position() + start <= to) {
                const SpellingRange word{block.position() + start, block.position() + end};
                if (hasLetter(QStringView(text).mid(start, end - start)) && word.end >= from && word.start <= to) {
                    result.append(word);
                }
                start = end;
                end = finder.toNextBoundary();
            }
            // Mid-word punctuation can move a boundary beyond the actual edit.
            // Include a neighbouring word only where the edit endpoint has no word.
            for (bool previous : {true, false}) {
                const int point = previous ? from : to;
                const bool covered = std::any_of(result.cbegin(), result.cend(), [point](SpellingRange range) {
                    return range.start <= point && point <= range.end;
                });
                if (!covered && block.position() <= point && point <= block.position() + text.size()) {
                    finder.setPosition(point - block.position());
                    int edge = finder.position();
                    int next = previous ? finder.toPreviousBoundary() : finder.toNextBoundary();
                    bool searching = true;
                    while (next >= 0 && searching) {
                        const int left = std::min(edge, next);
                        const int right = std::max(edge, next);
                        const QStringView segment = QStringView(text).mid(left, right - left);
                        searching = !std::any_of(segment.begin(), segment.end(), [](QChar ch) {
                            return ch.isSpace();
                        });
                        if (searching && hasLetter(segment)) {
                            result.insert(previous ? 0 : result.size(), {block.position() + left, block.position() + right});
                            searching = false;
                        }
                        edge = next;
                        next = previous ? finder.toPreviousBoundary() : finder.toNextBoundary();
                    }
                }
            }
            block = block.next();
        }
        return result;
    }

    ExpandingPromptEditor::~ExpandingPromptEditor() {
        // Clearing Qt formats can emit textChanged while the parent destroys siblings.
        spellingEnabled = false;
        disconnect();
    }

    void ExpandingPromptEditor::checkWord(SpellingRange word) {
        QTextCursor cursor(document());
        cursor.setPosition(word.start);
        cursor.setPosition(word.end, QTextCursor::KeepAnchor);
        if (speller->isMisspelled(cursor.selectedText())) {
            const auto at = std::lower_bound(findings.begin(), findings.end(), word.start, [](SpellingRange range, int start) {
                return range.start < start;
            });
            findings.insert(at, word);
        }
    }

    void ExpandingPromptEditor::renderSpellings(int from, int to) {
        QTextBlock block = document()->findBlock(std::clamp(from, 0, document()->characterCount() - 1));
        while (block.isValid() && block.position() <= to) {
            spellingFormat->rehighlightBlock(block);
            block = block.next();
        }
    }

    void ExpandingPromptEditor::finishPendingWord() {
        if (pending && !(pending->start < textCursor().position() && textCursor().position() <= pending->end)) {
            const SpellingRange word = *pending;
            pending.reset();
            checkWord(word);
            renderSpellings(word.start, word.end);
        }
    }

    void ExpandingPromptEditor::shiftSpellings(int pos, int removed, int added) {
        findings.removeIf([=](SpellingRange range) {
            return range.end >= pos && range.start <= pos + removed;
        });
        for (SpellingRange& range : findings) {
            if (range.start > pos + removed) {
                range.start += added - removed;
                range.end += added - removed;
            }
        }
        if (pending) {
            if (pending->end >= pos && pending->start <= pos + removed) {
                pending.reset();
            } else if (pending->start > pos + removed) {
                pending->start += added - removed;
                pending->end += added - removed;
            }
        }
    }

    void ExpandingPromptEditor::updateWord(SpellingRange word) {
        findings.removeIf([word](SpellingRange range) {
            return range.start < word.end && word.start < range.end;
        });
        if (word.start < textCursor().position() && textCursor().position() <= word.end) {
            pending = word;
        } else {
            checkWord(word);
        }
    }

    void ExpandingPromptEditor::resetSpellings() {
        findings.clear();
        pending.reset();
        if (spellingEnabled && speller->isValid()) {
            for (SpellingRange word : words(0, document()->characterCount() - 1)) {
                checkWord(word);
            }
        }
        spellingFormat->rehighlight();
    }

    bool ExpandingPromptEditor::canInsertFromMimeData(const QMimeData* source) const {
        return codex::isAttachmentInput(*source) || QPlainTextEdit::canInsertFromMimeData(source);
    }

    void ExpandingPromptEditor::insertFromMimeData(const QMimeData* source) {
        if (codex::isAttachmentInput(*source))
            emit attachmentInput(source);
        else
            QPlainTextEdit::insertFromMimeData(source);
    }

    void ExpandingPromptEditor::contextMenuEvent(QContextMenuEvent* event) {
        QMenu* menu = createStandardContextMenu();
        menu->setAttribute(Qt::WA_DeleteOnClose);
        connect(document(), &QTextDocument::contentsChange, menu, [menu](int, int removed, int added) {
            if (removed > 0 || added > 0) {
                menu->close();
            }
        });
        const bool keyboard = event->reason() == QContextMenuEvent::Keyboard;
        const QTextCursor hit = keyboard ? textCursor() : cursorForPosition(event->pos());
        QTextCursor target;
        if (!preeditActive && !isReadOnly() && spellingEnabled && speller->isValid()) {
            if (pending && pending->start <= hit.position() && hit.position() <= pending->end) {
                const SpellingRange word = *pending;
                pending.reset();
                checkWord(word);
                renderSpellings(word.start, word.end);
            }
            // Committed findings are also the menu authority; preedit offsets are visual only.
            // Sorted ranges prefer an interior hit, then the word ending here, then one starting here.
            const auto found = std::lower_bound(findings.cbegin(), findings.cend(), hit.position(), [](SpellingRange range, int position) {
                return range.end < position;
            });
            if (found != findings.cend() && found->start <= hit.position()) {
                target = QTextCursor(document());
                target.setPosition(found->start);
                target.setPosition(found->end, QTextCursor::KeepAnchor);
            }
        }
        menu->addSeparator();
        if (!target.isNull()) {
            const QString word = target.selectedText();
            const QString language = speller->language();
            const auto valid = [this, target, word, language] {
                return !target.isNull() && target.hasSelection() && target.selectedText() == word && !isReadOnly() && !preeditActive &&
                       spellingEnabled && speller->isValid() && speller->language() == language;
            };
            for (const QString& suggestion : speller->suggest(word)) {
                QString label = suggestion;
                QAction* action = menu->addAction(label.replace(QLatin1Char('&'), QStringLiteral("&&")));
                connect(action, &QAction::triggered, this, [this, target, suggestion, valid]() mutable {
                    if (valid()) {
                        const bool selected = textCursor().hasSelection();
                        target.beginEditBlock();
                        target.insertText(suggestion);
                        target.endEditBlock();
                        if (!selected) {
                            setTextCursor(target);
                        }
                    }
                });
            }
            QAction* ignore = menu->addAction(tr("Ignore for this app session"));
            connect(ignore, &QAction::triggered, this, [this, word, valid] {
                if (valid()) {
                    speller->addToSession(word);
                    const auto previous = std::exchange(findings, {});
                    for (SpellingRange range : previous) {
                        checkWord(range);
                    }
                    spellingFormat->rehighlight();
                }
            });
            menu->addSeparator();
        }
        QAction* enabled = menu->addAction(tr("Check spelling"));
        enabled->setCheckable(true);
        enabled->setChecked(spellingEnabled);
        connect(enabled, &QAction::toggled, this, [this](bool active) {
            spellingEnabled = active;
            spellingFormat->setDocument(active ? document() : nullptr);
            resetSpellings();
        });
        const bool available = speller->isValid();
        QMenu* languages = menu->addMenu(available ? tr("Spelling language (%1)").arg(speller->language())
                                                   : tr("Spelling unavailable — choose a language"));
        auto* group = new QActionGroup(languages);
        const auto dictionaries = speller->availableDictionaries();
        if (dictionaries.isEmpty()) {
            languages->addAction(tr("Install a Sonnet backend and dictionary"))->setEnabled(false);
        }
        for (auto entry = dictionaries.cbegin(); entry != dictionaries.cend(); ++entry) {
            QAction* language = languages->addAction(entry.key());
            group->addAction(language);
            language->setCheckable(true);
            language->setChecked(available && entry.value() == speller->language());
            connect(language, &QAction::triggered, this, [this, code = entry.value()] {
                speller->setLanguage(code);
                resetSpellings();
            });
        }
        menu->popup(keyboard ? viewport()->mapToGlobal(cursorRect().bottomLeft()) : event->globalPos());
        event->accept();
    }

    void ExpandingPromptEditor::changeEvent(QEvent* event) {
        QPlainTextEdit::changeEvent(event);
        if (event &&
            (event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange))
            scheduleRemeasure();
    }

    void ExpandingPromptEditor::dropEvent(QDropEvent* event) {
        if (!codex::isAttachmentInput(*event->mimeData()))
            return QPlainTextEdit::dropEvent(event);
        // End Qt's drag marker and autoscroll before consuming or rejecting the drop.
        QDragLeaveEvent leave;
        QPlainTextEdit::dragLeaveEvent(&leave);
        // Attachments reference the source file; never let the drag owner move it.
        if (isReadOnly() || !event->possibleActions().testFlag(Qt::CopyAction))
            return event->ignore();
        insertFromMimeData(event->mimeData());
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    bool ExpandingPromptEditor::requiresExpandedLayout(int widgetWidth) const {
        if (document()->isEmpty())
            return false;
        if (document()->blockCount() > 1)
            return true;

        const int viewportReduction = std::max(0, width() - viewport()->width());
        const qreal contentWidth = std::max<qreal>(1, widgetWidth - viewportReduction - 2 * document()->documentMargin());
        const QTextLayout* layout = document()->firstBlock().layout();
        return layout && layout->maximumWidth() > contentWidth;
    }

    void ExpandingPromptEditor::focusInEvent(QFocusEvent* event) {
        QPlainTextEdit::focusInEvent(event);
        emit focusStateChanged(true);
    }

    void ExpandingPromptEditor::focusOutEvent(QFocusEvent* event) {
        QPlainTextEdit::focusOutEvent(event);
        preeditActive = false;
        emit focusStateChanged(false);
    }

    void ExpandingPromptEditor::inputMethodEvent(QInputMethodEvent* event) {
        const int oldLength = document()->characterCount();
        const QTextCursor cursor = textCursor();
        const int selectionStart = cursor.selectionStart();
        const int selectionLength = cursor.selectionEnd() - selectionStart;
        const bool getsInput = !event->commitString().isEmpty() || event->replacementLength() > 0 ||
                               event->preeditString() != cursor.block().layout()->preeditAreaText();
        const int replaceStart = selectionStart + event->replacementStart();
        const int replaceLength = event->replacementLength();
        const bool hasCommit = !event->commitString().isEmpty() || replaceLength > 0;
        {
            // Qt reports preedit layout changes as whole-block contents changes.
            // Apply only the semantic selection/removal/commit after its handler.
            QScopedValueRollback<bool> guard(processingInputMethod, true);
            preeditActive = !event->preeditString().isEmpty();
            QPlainTextEdit::inputMethodEvent(event);
        }
        if (spellingEnabled && speller->isValid()) {
            const int added =
                document()->characterCount() - oldLength + (getsInput ? selectionLength : 0) + (hasCommit ? replaceLength : 0);
            int dirtyStart = selectionStart;
            int dirtyEnd = selectionStart;
            if (getsInput && selectionLength > 0) {
                shiftSpellings(selectionStart, selectionLength, 0);
            }
            if (hasCommit) {
                shiftSpellings(replaceStart, replaceLength, added);
                dirtyStart = std::min(dirtyStart, replaceStart);
                dirtyEnd = std::max(dirtyEnd, replaceStart + added);
            }
            const bool textEdit = (getsInput && selectionLength > 0) || hasCommit;
            bool findingsChanged = textEdit;
            if (textEdit) {
                finishPendingWord();
                for (SpellingRange word : words(dirtyStart, dirtyEnd)) {
                    updateWord(word);
                }
            }
            if (preeditActive) {
                for (SpellingRange word : words(textCursor().position(), textCursor().position())) {
                    if (word.start < textCursor().position() && textCursor().position() <= word.end) {
                        findingsChanged |= findings.removeAll(word) > 0;
                        pending = word;
                    }
                }
            }
            finishPendingWord();
            if (findingsChanged) {
                renderSpellings(dirtyStart, std::max(dirtyEnd, textCursor().position()));
            }
        }
    }

    void ExpandingPromptEditor::keyPressEvent(QKeyEvent* event) {
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (!enter || preeditActive) {
            QPlainTextEdit::keyPressEvent(event);
            return;
        }

        const Qt::KeyboardModifiers modifiers = event->modifiers();
        if (modifiers.testFlag(Qt::ShiftModifier)) {
            insertPlainText(QStringLiteral("\n"));
            event->accept();
            return;
        }

        if (event->isAutoRepeat()) {
            event->accept();
            return;
        }

        if (!modifiers.testFlag(Qt::AltModifier)) {
            emit submitRequested();
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

    void ExpandingPromptEditor::wheelEvent(QWheelEvent* event) {
        if (contentScrollable)
            QPlainTextEdit::wheelEvent(event);
        // Composer-originated scrolling belongs to the prompt editor even at its
        // boundary. It must never chain into the conversation behind the overlay.
        event->accept();
    }

    void ExpandingPromptEditor::resizeEvent(QResizeEvent* event) {
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

        const int maximumContentHeight = fontMetrics().lineSpacing() * maximumVisibleLineCount() + 10;
        QAbstractTextDocumentLayout* layout = document()->documentLayout();
        qreal laidOutHeight = 0;
        for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
            const QRectF bounds = layout->blockBoundingRect(block);
            laidOutHeight += bounds.height();
            if (laidOutHeight >= maximumContentHeight)
                break;
        }
        const int documentHeight = static_cast<int>(std::ceil(laidOutHeight)) + 10;
        const int wanted = std::clamp(documentHeight, compactHeight(), maximumContentHeight);
        contentScrollable = documentHeight > maximumContentHeight;
        setVerticalScrollBarPolicy(contentScrollable ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        if (!contentScrollable)
            verticalScrollBar()->setValue(verticalScrollBar()->minimum());
        if (wanted == currentContentHeight)
            return;
        currentContentHeight = wanted;
        setFixedHeight(wanted);
        emit editorHeightChanged(wanted);
    }

} // namespace codexui
