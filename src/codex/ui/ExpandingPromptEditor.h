// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_EXPANDINGPROMPTEDITOR_H
#define CODEXUI_UI_EXPANDINGPROMPTEDITOR_H

#include <QPlainTextEdit>

class QFocusEvent;
class QKeyEvent;
class QResizeEvent;

namespace codexui {

// Owns the prompt-specific keyboard and content-height behavior. The parent
// dock remains responsible for submission policy and for anchoring itself.
class ExpandingPromptEditor final : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit ExpandingPromptEditor(QWidget* parent = nullptr);

    [[nodiscard]] static constexpr int compactHeight() noexcept { return 32; }
    [[nodiscard]] static constexpr int maximumVisibleLineCount() noexcept { return 20; }

signals:
    void submitRequested();
    void focusStateChanged(bool focused);
    void editorHeightChanged(int height);

protected:
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void scheduleRemeasure();
    void remeasure();

    int maximumEditorHeight = compactHeight();
    int currentContentHeight = compactHeight();
    bool remeasureScheduled = false;
};

} // namespace codexui

#endif // CODEXUI_UI_EXPANDINGPROMPTEDITOR_H
