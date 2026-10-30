// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_EXPANDINGPROMPTEDITOR_H
#define CODEXUI_UI_EXPANDINGPROMPTEDITOR_H

#include <QPlainTextEdit>
#include <memory>
#include <optional>

class QContextMenuEvent;
class QFocusEvent;
class QEvent;
class QInputMethodEvent;
class QKeyEvent;
class QResizeEvent;
class QWheelEvent;

namespace Sonnet {
    class Speller;
}

namespace codexui {

    // Owns the prompt-specific keyboard and content-height behavior. The parent
    // dock remains responsible for submission policy and for anchoring itself.
    class ExpandingPromptEditor final : public QPlainTextEdit {
        Q_OBJECT

    public:
        explicit ExpandingPromptEditor(QWidget* parent = nullptr);
        ~ExpandingPromptEditor() override;

        [[nodiscard]] static constexpr int compactHeight() noexcept {
            return 32;
        }
        [[nodiscard]] static constexpr int maximumVisibleLineCount() noexcept {
            return 20;
        }
        [[nodiscard]] bool requiresExpandedLayout(int widgetWidth) const;

    signals:
        void submitRequested();
        void focusStateChanged(bool focused);
        void editorHeightChanged(int height);
        // Consumed synchronously; the clipboard or drop event owns source.
        void attachmentInput(const QMimeData* source);

    protected:
        bool canInsertFromMimeData(const QMimeData* source) const override;
        void insertFromMimeData(const QMimeData* source) override;
        void dropEvent(QDropEvent* event) override;
        void changeEvent(QEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        void focusInEvent(QFocusEvent* event) override;
        void focusOutEvent(QFocusEvent* event) override;
        void inputMethodEvent(QInputMethodEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;

    private:
        void scheduleRemeasure();
        void remeasure();

        struct SpellingRange {
            int start;
            int end;
            bool operator==(const SpellingRange&) const = default;
        };
        class SpellingFormat;
        QList<SpellingRange> words(int from, int to) const;
        void checkWord(SpellingRange word);
        void renderSpellings(int from, int to);
        void finishPendingWord();
        void shiftSpellings(int pos, int removed, int added);
        void updateWord(SpellingRange word);
        void resetSpellings();

        std::unique_ptr<Sonnet::Speller> speller;
        QList<SpellingRange> findings;
        std::optional<SpellingRange> pending;
        bool spellingEnabled = true;
        bool processingInputMethod = false;
        // Destroy the projection before the findings it references and the document.
        std::unique_ptr<SpellingFormat> spellingFormat;
        int currentContentHeight = compactHeight();
        bool contentScrollable = false;
        bool preeditActive = false;
        bool remeasureScheduled = false;
    };

} // namespace codexui

#endif // CODEXUI_UI_EXPANDINGPROMPTEDITOR_H
