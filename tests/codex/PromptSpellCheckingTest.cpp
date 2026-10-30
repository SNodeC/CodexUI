// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "codex/middle/ComposerPane.h"
#include "codex/ui/ExpandingPromptEditor.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPluginLoader>
#include <QPointer>
#include <QRandomGenerator>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <Sonnet/Speller>
#include <algorithm>
#include <dlfcn.h>
#include <iostream>

namespace {
    qint64 lookups = 0;
    bool missingBackend = false;
} // namespace
// Observe the public dictionary boundary without adding production instrumentation.
bool Sonnet::Speller::isMisspelled(const QString& word) const {
    static const auto original = reinterpret_cast<bool (*)(const Sonnet::Speller*, const QString&)>(
        dlsym(RTLD_NEXT, "_ZNK6Sonnet7Speller12isMisspelledERK7QString"));
    ++lookups;
    return original(this, word);
}

bool QPluginLoader::load() {
    static const auto original = reinterpret_cast<bool (*)(QPluginLoader*)>(dlsym(RTLD_NEXT, "_ZN13QPluginLoader4loadEv"));
    return (!missingBackend || !fileName().contains(QStringLiteral("/kf6/sonnet/"))) && original(this);
}

namespace {
    using Editor = codexui::ExpandingPromptEditor;
    bool expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }
    void settle() {
        for (int phase = 0; phase < 8; ++phase) {
            QApplication::processEvents();
        }
    }
    QMenu* menuAt(Editor& editor, int position, QContextMenuEvent::Reason reason = QContextMenuEvent::Keyboard) {
        QTextCursor cursor(editor.document());
        cursor.setPosition(position);
        QPoint point = editor.cursorRect(cursor).center();
        if (reason == QContextMenuEvent::Other) {
            point.rx() += 80;
        }
        if (reason == QContextMenuEvent::Keyboard) {
            editor.setTextCursor(cursor);
        }
        QContextMenuEvent event(reason, point, editor.mapToGlobal(point));
        QApplication::sendEvent(reason == QContextMenuEvent::Keyboard ? static_cast<QWidget*>(&editor) : editor.viewport(), &event);
        settle();
        return qobject_cast<QMenu*>(QApplication::activePopupWidget());
    }
    QAction* action(QMenu* menu, const QString& prefix, bool exact = false) {
        QAction* found = nullptr;
        if (menu) {
            for (QAction* candidate : menu->actions()) {
                QString label = candidate->text();
                if (exact ? label == prefix : label.remove(QLatin1Char('&')).startsWith(prefix)) {
                    found = candidate;
                }
            }
        }
        return found;
    }
    bool trigger(QMenu* menu, const QString& prefix, bool exact = false) {
        QAction* found = action(menu, prefix, exact);
        const bool present = expect(found != nullptr, qPrintable(QStringLiteral("expected menu action: %1").arg(prefix)));

        if (found) {
            found->trigger();
        }
        return present;
    }
    void language(Editor& editor, const QString& code) {
        QMenu* menu = menuAt(editor, editor.textCursor().position());
        QAction* languages = action(menu, QStringLiteral("Spelling language"));
        if (!languages) {
            languages = action(menu, QStringLiteral("Spelling unavailable"));
        }
        if (languages && languages->menu()) {
            trigger(languages->menu(), Sonnet::Speller().availableDictionaries().key(code), true);
        }
        menu->close();
        settle();
    }
    void enabled(Editor& editor, bool value) {
        QMenu* menu = menuAt(editor, editor.textCursor().position());
        QAction* toggle = action(menu, QStringLiteral("Check spelling"));
        if (toggle && toggle->isChecked() != value) {
            toggle->trigger();
        }
        menu->close();
        settle();
    }
    void prepare(Editor& editor, const QString& text = QStringLiteral("mispelled word ")) {
        editor.resize(640, 120);
        editor.show();
        editor.activateWindow();
        editor.setFocus();
        editor.setPlainText(text);
        editor.moveCursor(QTextCursor::End);
        enabled(editor, true);
        language(editor, QStringLiteral("en_US"));
        editor.activateWindow();
        editor.setFocus();
        settle();
    }
    QStringList dictionarySuggestions(const QString& word) {
        return Sonnet::Speller(QStringLiteral("en_US")).suggest(word);
    }
    void rehighlight(Editor& editor) {
        for (auto* projection : editor.document()->findChildren<QSyntaxHighlighter*>()) {
            projection->rehighlight();
        }
    }
    using Ranges = QList<QPair<int, int>>;
    Ranges marks(Editor& editor) {
        Ranges result;
        for (QTextBlock block = editor.document()->begin(); block.isValid(); block = block.next()) {
            for (const auto& range : block.layout()->formats()) {
                if (range.format.underlineStyle() == QTextCharFormat::SpellCheckUnderline) {
                    result.append({block.position() + range.start, block.position() + range.start + range.length});
                }
            }
        }
        return result;
    }
    bool oracle(Editor& editor) {
        editor.moveCursor(QTextCursor::Start); // Complete pending input through public navigation.
        // Full public segmentation and dictionary recomputation: no edit-coordinate arithmetic.
        Sonnet::Speller dictionary(QStringLiteral("en_US"));
        Ranges expected;
        for (QTextBlock block = editor.document()->begin(); block.isValid(); block = block.next()) {
            const QString text = block.text();
            QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
            int start = 0;
            int end = finder.toNextBoundary();
            while (end >= 0) {
                const QString word = text.mid(start, end - start);
                const auto points = word.toUcs4();
                if (std::any_of(points.begin(),
                                points.end(),
                                [](char32_t point) {
                                    return QChar::isLetter(point);
                                }) &&
                    dictionary.isMisspelled(word)) {
                    // Qt coalesces adjacent identical character formats (e.g. Chinese segments).
                    if (!expected.isEmpty() && expected.last().second == block.position() + start) {
                        expected.last().second = block.position() + end;
                    } else {
                        expected.append({block.position() + start, block.position() + end});
                    }
                }
                start = end;
                end = finder.toNextBoundary();
            }
        }
        return expect(marks(editor) == expected, "incremental marks equal independent full recomputation");
    }
    void key(Editor& editor, int code, const QString& text = {}) {
        QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier, text);
        QApplication::sendEvent(&editor, &event);
        settle();
    }
    bool incrementalEdits() {
        Editor editor;
        bool passed = true;
        for (int length : {1000, 40000}) {
            prepare(editor, QStringLiteral("mispelled ").repeated(length / 10));
            lookups = 0;
            for (QChar letter : QStringLiteral("wrnog")) {
                key(editor, 0, QString(letter));
            }
            passed &= expect(lookups == 0, "extending an unfinished word performs zero dictionary requests");
            key(editor, Qt::Key_Space, QStringLiteral(" "));
            passed &= expect(lookups == 1 && marks(editor).last().second == editor.toPlainText().size() - 1,
                             "word completion checks and marks once independently of unrelated paragraph length");
            lookups = 0;
            rehighlight(editor);
            passed &= expect(lookups == 0, "formatting performs zero dictionary requests");
            key(editor, 0, QStringLiteral("qzzx"));
            QMenu* menu = menuAt(editor, editor.textCursor().position());
            passed &= expect(action(menu, QStringLiteral("Ignore for this app session")), "pending word is checked on demand for its menu");
            menu->close();
        }
        for (const QString& text : {QString(), QStringLiteral("wrnog\nqzzx "), QStringLiteral("mispelled\r\nhello ")}) {
            editor.setPlainText(text);
            passed &= oracle(editor);
        }
        QRandomGenerator random(0x5370656c);
        prepare(editor, QString::fromUtf8("isn't isn’t e\xcc\x81zzx 𐐀qzzx 3rd 中文 wrnog_qzzx mispelled\nhello wrnog "));
        const QStringList inserts{QStringLiteral("'"),
                                  QStringLiteral("’"),
                                  QStringLiteral(" "),
                                  QStringLiteral("qzzx"),
                                  QStringLiteral("\nwrnog\n"),
                                  QString::fromUtf8("𐐀"),
                                  QStringLiteral("_")};
        for (int edit = 0; edit < 120; ++edit) {
            QTextCursor cursor(editor.document());
            cursor.setPosition(random.bounded(editor.toPlainText().size() + 1));
            editor.setTextCursor(cursor);
            cursor.beginEditBlock();
            if (edit % 3 == 0) {
                cursor.deleteChar();
            } else if (edit % 3 == 1) {
                cursor.deletePreviousChar();
            } else {
                cursor.insertText(inserts.at(random.bounded(inserts.size())));
            }
            cursor.endEditBlock();
            editor.setTextCursor(cursor);
            passed &= oracle(editor);
        }
        for (int operation = 0; operation < 18; ++operation) {
            editor.undo();
            passed &= oracle(editor);
            editor.redo();
            passed &= oracle(editor);
        }
        for (int position : {0, 2, 5}) {
            prepare(editor, QStringLiteral("wrnog hello qzzx "));
            QTextCursor cursor(editor.document());
            cursor.setPosition(position);
            editor.setTextCursor(cursor);
            const auto before = marks(editor);
            const int undoSteps = editor.document()->availableUndoSteps();
            const bool modified = editor.document()->isModified();
            lookups = 0;
            QInputMethodEvent preedit(QStringLiteral("候補"), {});
            QApplication::sendEvent(&editor, &preedit);
            Ranges expected;
            for (auto range : before) {
                if (!(range.first < position && position <= range.second)) {
                    if (range.first >= position) {
                        range.first += 2;
                        range.second += 2;
                    }
                    expected.append(range);
                }
            }
            passed &=
                expect(lookups == 0 && marks(editor) == expected, "preedit preserves retained marks at Qt visual offsets without lookups");
            passed &= expect(editor.toPlainText() == QStringLiteral("wrnog hello qzzx ") && editor.document()->isModified() == modified &&
                                 editor.document()->availableUndoSteps() == undoSteps,
                             "preedit formatting preserves text, modified state and undo");
            QInputMethodEvent commit;
            commit.setCommitString(QStringLiteral("mispelled "));
            QApplication::sendEvent(&editor, &commit);
            passed &= oracle(editor);
        }
        for (int operation = 0; operation < 8; ++operation) {
            prepare(editor);
            QPlainTextEdit baseline;
            baseline.setPlainText(editor.toPlainText());
            QTextCursor cursor(editor.document()), plain(baseline.document());
            cursor.setPosition(4);
            plain.setPosition(4);
            if (operation % 2 == 0) {
                cursor.setPosition(9, QTextCursor::KeepAnchor);
                plain.setPosition(9, QTextCursor::KeepAnchor);
            }
            editor.setTextCursor(cursor);
            baseline.setTextCursor(plain);
            QInputMethodEvent commit, control;
            const QString text = operation < 4 ? QStringLiteral("wrnog ") : QStringLiteral("x\r\ny ");
            const int offset = operation % 4 == 2 ? -2 : 0;
            commit.setCommitString(text, offset, operation % 4 == 3 ? 2 : 0);
            control.setCommitString(text, offset, operation % 4 == 3 ? 2 : 0);
            QApplication::sendEvent(&editor, &commit);
            QApplication::sendEvent(&baseline, &control);
            passed &=
                expect(editor.toPlainText() == baseline.toPlainText(), "IME selection, replacement and CRLF follow ordinary Qt editing");
            passed &= oracle(editor);
            editor.undo();
            passed &= oracle(editor);
            editor.redo();
            passed &= oracle(editor);
        }
        return passed;
    }
    bool rangesAndCorrections() {
        Editor editor;
        bool passed = true;
        for (const QString& text :
             {QStringLiteral("wrnog/wrnog "), QStringLiteral("wrnog-wrnog "), QString::fromUtf8("geht's Straße qzzxüß e\xcc\x81zzx ")}) {
            prepare(editor, text);
            int previousEnd = -1;
            for (const auto& range : editor.document()->firstBlock().layout()->formats()) {
                passed &= expect(range.format.underlineStyle() == QTextCharFormat::SpellCheckUnderline &&
                                     range.format.underlineColor() == QColor(codexui::UiStyle::spellingError) && range.length > 0,
                                 "Qt publishes the spelling underline style and UiStyle color");
                passed &= expect(range.start > previousEnd, "adjacent words retain separate correction ranges");
                const QString word = text.mid(range.start, range.length);
                passed &= expect(!word.contains(QLatin1Char('/')) && !word.contains(QLatin1Char('-')),
                                 "a correction never spans slash or hyphen tokens");
                QMenu* menu = menuAt(editor, range.start + 1);
                passed &= expect(action(menu, QStringLiteral("Ignore for this app session")), "Unicode/range target has spelling actions");
                menu->close();
                previousEnd = range.start + range.length;
            }
            passed &= expect(previousEnd > 0, "the fixture actually produces spelling ranges");
        }
        for (int position : {0, 3, 9}) {
            prepare(editor);
            const auto suggestions = dictionarySuggestions(QStringLiteral("mispelled"));
            passed &= expect(!suggestions.isEmpty(), "the real dictionary supplies corrections");
            QMenu* menu = menuAt(editor, position);
            const QString replacement = suggestions.value(0);
            passed &= trigger(menu, replacement);
            settle();
            passed &= expect(editor.toPlainText() == replacement + QStringLiteral(" word "), "only the authoritative target is replaced");
            passed &=
                expect(editor.textCursor().position() == replacement.size() && editor.hasFocus(), "correction restores caret and focus");
            editor.undo();
            passed &= expect(editor.toPlainText() == QStringLiteral("mispelled word ") && !editor.document()->isUndoAvailable(),
                             "one user undo reverses exactly one correction");
            editor.redo();
            passed &= expect(editor.toPlainText() == replacement + QStringLiteral(" word "), "one user redo restores the correction");
        }
        prepare(editor);
        QTextCursor selection(editor.document());
        selection.setPosition(2);
        selection.setPosition(12, QTextCursor::KeepAnchor);
        editor.setTextCursor(selection);
        QMenu* menu = menuAt(editor, 3, QContextMenuEvent::Mouse);
        passed &= expect(editor.textCursor().selectedText() == QStringLiteral("spelled wo"),
                         "opening the mouse menu preserves an overlapping selection");
        passed &= trigger(menu, dictionarySuggestions(QStringLiteral("mispelled")).value(0));
        passed &= expect(editor.textCursor().hasSelection(), "Qt adjusts an existing selection through correction");
        prepare(editor, QStringLiteral("hello mispelled"));
        menu = menuAt(editor, editor.toPlainText().size(), QContextMenuEvent::Other);
        passed &= expect(action(menu, QStringLiteral("Ignore for this app session")),
                         "Other invocation past the line end snaps to the final underlined word");
        menu->close();
        prepare(editor, QStringLiteral("hello world "));
        menu = menuAt(editor, 2);
        passed &= expect(!action(menu, QStringLiteral("Ignore for this app session")), "no underline means no correction target");
        menu->close();
        return passed;
    }
    bool rawSuggestion() {
        Editor editor;
        prepare(editor, QStringLiteral("qzzxamp "));
        language(editor, QStringLiteral("en_US-codexuitest"));
        QMenu* menu = menuAt(editor, 3);
        QAction* suggestion = action(menu, QStringLiteral("qzzx"));
        bool passed = expect(suggestion && suggestion->text() == QStringLiteral("qzzx&&ampp"),
                             "ampersand suggestion is escaped only for menu display");
        if (suggestion) {
            suggestion->trigger();
        }
        passed &= expect(editor.toPlainText() == QStringLiteral("qzzx&ampp "), "correction inserts the raw ampersand suggestion");
        return passed;
    }
    bool menuLifetimeAndGuards() {
        Editor editor;
        bool passed = true;
        for (int operation = 0; operation < 4; ++operation) {
            prepare(editor);
            QPointer<QMenu> menu = menuAt(editor, 3);
            rehighlight(editor);
            settle();
            passed &= expect(menu && menu->isVisible(), "highlighting leaves the standard menu open");
            if (operation == 0) {
                editor.insertPlainText(QStringLiteral("x"));
            } else if (operation == 1) {
                auto cursor = editor.textCursor();
                cursor.deleteChar();
            } else if (operation == 2) {
                editor.setPlainText(editor.toPlainText());
            } else {
                editor.clear();
            }
            passed &= expect(!menu || !menu->isVisible(), "insertion/removal/restore/clear closes the menu");
            settle();
        }
        for (int guard = 0; guard < 5; ++guard) {
            prepare(editor);
            QMenu* menu = menuAt(editor, 3);
            QAction* correction = action(menu, dictionarySuggestions(QStringLiteral("mispelled")).value(0));
            if (guard == 0) {
                trigger(menu, QStringLiteral("Check spelling"));
            } else if (guard == 1) {
                QAction* languages = action(menu, QStringLiteral("Spelling language"));
                passed &= trigger(languages->menu(), Sonnet::Speller().availableDictionaries().key(QStringLiteral("de_AT")), true);
            } else if (guard == 2) {
                QInputMethodEvent preedit(QStringLiteral("候"), {});
                QApplication::sendEvent(&editor, &preedit);
            } else if (guard == 3) {
                editor.setReadOnly(true);
            } else {
                editor.setPlainText(QStringLiteral("different"));
            }
            const QString before = editor.toPlainText();
            passed &= expect(correction != nullptr, "guard fixture has a correction action");
            if (correction) {
                correction->trigger();
            }
            passed &= expect(editor.toPlainText() == before, "stale, disabled, language-changed and preedit actions cannot edit");
            QInputMethodEvent endPreedit;
            QApplication::sendEvent(&editor, &endPreedit);
            menu->close();
            editor.setReadOnly(false);
            enabled(editor, true);
            settle();
        }
        prepare(editor);
        QInputMethodEvent preedit(QStringLiteral("候"), {});
        QApplication::sendEvent(&editor, &preedit);
        QMenu* menu = menuAt(editor, 3);
        passed &= expect(!action(menu, QStringLiteral("Ignore for this app session")),
                         "preedit preserves standard menu without spelling actions");
        menu->close();
        auto* closing = new codexui::codex::middle::ComposerPane;
        prepare(*closing->promptEditor());
        closing->promptEditor()->insertPlainText(QStringLiteral("qzzx"));
        QPointer<QMenu> owned = menuAt(*closing->promptEditor(), 3, QContextMenuEvent::Mouse);
        delete closing;
        settle();
        passed &= expect(owned.isNull(), "destroying the composer destroys its open menu safely");
        return passed;
    }
    bool policyClipboardAndSubmission() {
        Editor editor;
        prepare(editor);
        bool passed = expect(editor.document()->findChildren<QSyntaxHighlighter*>().size() == 1,
                             "one pure formatting projection owns the draft marks");
        QTextDocument* document = editor.document();
        QMenu* menu = menuAt(editor, 3);
        passed &=
            expect(action(menu, QStringLiteral("Undo")) && action(menu, QStringLiteral("Copy")) && action(menu, QStringLiteral("Paste")),
                   "the standard editing menu is retained");
        passed &= trigger(menu, QStringLiteral("Check spelling"));
        menu->close();
        passed &= expect(editor.document()->firstBlock().layout()->formats().isEmpty(), "menu toggle changes the checker authority");
        menu = menuAt(editor, 3);
        passed &= trigger(menu, QStringLiteral("Check spelling"));
        menu->close();
        language(editor, QStringLiteral("de_AT"));
        menu = menuAt(editor, 3);
        QAction* installed = action(menu, QStringLiteral("Spelling language (de_AT)"));
        passed &= expect(installed && installed->menu(), "installed German dictionary is selected and displayed");
        menu->close();
        prepare(editor, QStringLiteral("qzzxsessionword "));
        passed &= expect(!editor.document()->firstBlock().layout()->formats().isEmpty(), "ignore does not survive an app restart");
        Editor other;
        other.setPlainText(QStringLiteral("qzzxsessionword "));
        menu = menuAt(editor, 3);
        passed &= trigger(menu, QStringLiteral("Ignore for this app session"));
        menu->close();
        passed &= expect(!marks(other).isEmpty(), "ignore does not notify another editor about existing findings");
        other.setPlainText(QStringLiteral("qzzxsessionword "));
        passed &=
            expect(other.document()->firstBlock().layout()->formats().isEmpty(), "ignore is shared with another editor's future lookups");
        const QString original = QStringLiteral("mispelled **source**\nStraße ");
        QPlainTextEdit baseline;
        baseline.setPlainText(original);
        baseline.selectAll();
        baseline.copy();
        auto baselineFormats = QApplication::clipboard()->mimeData()->formats();
        baselineFormats.sort();
        const auto baselineMarkdown = QApplication::clipboard()->mimeData()->data(QStringLiteral("text/markdown"));
        const auto baselineHtml = QApplication::clipboard()->mimeData()->html();
        const auto baselineOdt = QApplication::clipboard()->mimeData()->data(QStringLiteral("application/vnd.oasis.opendocument.text"));
        prepare(editor, original);
        editor.selectAll();
        editor.copy();
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        auto formats = mime->formats();
        formats.sort();
        passed &= expect(mime->text() == original && mime->hasHtml() && mime->hasFormat(QStringLiteral("text/markdown")) &&
                             mime->hasFormat(QStringLiteral("application/vnd.oasis.opendocument.text")),
                         "all standard clipboard flavors retain exact plain text");
        // Qt formatting never becomes document content. MIME ordering is unspecified.
        passed &= expect(mime->html() == baselineHtml && formats == baselineFormats, "HTML and MIME flavors match Qt without a checker");
        passed &= expect(mime->data(QStringLiteral("text/markdown")) == baselineMarkdown, "Markdown copying matches Qt without a checker");
        const QString auditDirectory = qEnvironmentVariable("CODEXUI_SPELLING_AUDIT_DIR");
        if (!auditDirectory.isEmpty()) {
            QFile plain(auditDirectory + QStringLiteral("/baseline.odt"));
            passed &= expect(plain.open(QIODevice::WriteOnly) && plain.write(baselineOdt) == baselineOdt.size(),
                             "baseline ODT audit artifact is written");
            QFile highlighted(auditDirectory + QStringLiteral("/highlighted.odt"));
            const auto odt = mime->data(QStringLiteral("application/vnd.oasis.opendocument.text"));
            passed &= expect(highlighted.open(QIODevice::WriteOnly) && highlighted.write(odt) == odt.size(),
                             "highlighted ODT audit artifact is written");
        }
        const QString highlightedHtml = mime->html();
        enabled(editor, false);
        editor.copy();
        passed &=
            expect(QApplication::clipboard()->mimeData()->html() == highlightedHtml, "disabling spelling does not change clipboard HTML");
        enabled(editor, true);
        prepare(editor);
        int attachments = 0, submissions = 0;
        QObject::connect(&editor, &Editor::attachmentInput, &editor, [&](const QMimeData*) {
            ++attachments;
        });
        QObject::connect(&editor, &Editor::submitRequested, &editor, [&] {
            ++submissions;
        });
        auto* image = new QMimeData;
        image->setImageData(QImage(2, 2, QImage::Format_ARGB32));
        QApplication::clipboard()->setMimeData(image);
        menu = menuAt(editor, 3);
        passed &= trigger(menu, QStringLiteral("Paste"));
        menu->close();
        passed &= expect(attachments == 1, "standard Paste retains image attachment routing");
        menu = menuAt(editor, 3);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(menu, &enter);
        menu->close();
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, {}, true);
        QApplication::sendEvent(&editor, &repeat);
        passed &= expect(submissions == 0, "menu Enter and auto-repeat never submit the draft");
        QKeyEvent shift(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
        QApplication::sendEvent(&editor, &shift);
        passed &= expect(editor.toPlainText().contains(QLatin1Char('\n')), "Shift+Enter still inserts a newline");
        passed &= expect(editor.document() == document && editor.document()->findChildren<QSyntaxHighlighter*>().size() == 1,
                         "restores retain the live document and checker");
        codexui::codex::middle::ComposerPane composer;
        composer.setCanSubmit(true);
        QString submitted;
        codexui::codex::middle::ComposerPane::Actions actions;
        actions.submit = [&](QString text, auto) {
            submitted = text;
            return true;
        };
        composer.setActions(std::move(actions));
        prepare(*composer.promptEditor());
        menu = menuAt(*composer.promptEditor(), 3);
        const QString replacement = dictionarySuggestions(QStringLiteral("mispelled")).value(0);
        passed &= trigger(menu, replacement);
        settle();
        QApplication::sendEvent(composer.promptEditor(), &enter);
        passed &= expect(submitted == replacement + QStringLiteral(" word ") && composer.promptEditor()->toPlainText().isEmpty(),
                         "real composer submits exactly the corrected draft");
        return passed;
    }
} // namespace

int main(int argc, char** argv) {
    QTemporaryDir isolation;
    qputenv("XDG_CONFIG_HOME", isolation.path().toUtf8());
    qputenv("XDG_CACHE_HOME", isolation.path().toUtf8());
    qputenv("XDG_RUNTIME_DIR", isolation.path().toUtf8());
    QDir().mkpath(isolation.path() + QStringLiteral("/KDE"));
    QFile settings(isolation.path() + QStringLiteral("/KDE/Sonnet.conf"));
    bool passed = expect(settings.open(QIODevice::WriteOnly), "isolated settings fixture opens");
    const bool unavailable = argc == 2;
    const QByteArray config = QByteArray("[General]\ndefaultLanguage=") + (unavailable ? "zz_ZZ" : "en_US") +
                              "\ndefaultClient=Hunspell\nautodetectLanguage=false\n";
    settings.write(config);
    settings.close();
    const QFileInfo before(settings);
    const auto beforeModified = before.lastModified();
    const auto beforeSize = before.size();
    qputenv("XDG_DATA_HOME", isolation.path().toUtf8());
    QDir().mkpath(isolation.path() + QStringLiteral("/hunspell"));
    for (const QString& extension : {QStringLiteral("aff"), QStringLiteral("dic")}) {
        QFile dictionary(isolation.path() + QStringLiteral("/hunspell/en_US-codexuitest.") + extension);
        passed &= expect(dictionary.open(QIODevice::WriteOnly), "real Hunspell fixture opens");
        dictionary.write(extension == QStringLiteral("aff") ? "SET UTF-8\nREP 1\nREP qzzxamp qzzx&ampp\n" : "1\nqzzx&ampp\n");
    }
    QApplication application(argc, argv);
    if (unavailable) {
        missingBackend = QString::fromLocal8Bit(argv[1]) == QStringLiteral("--unavailable");
        Editor editor;
        QMenu* menu = menuAt(editor, 0);
        passed &=
            expect(action(menu, missingBackend ? QStringLiteral("Spelling unavailable") : QStringLiteral("Spelling language (en_US)")),
                   "missing backend is unavailable; missing configured language uses Sonnet's available fallback");
        passed &= expect(action(menu, QStringLiteral("Paste")), "unavailable checking retains standard Paste");
        menu->close();
        if (QString::fromLocal8Bit(argv[1]) == QStringLiteral("--missing-language")) {
            language(editor, QStringLiteral("en_US"));
            menu = menuAt(editor, 0);
            passed &= expect(action(menu, QStringLiteral("Spelling language (en_US)")), "choosing an installed language recovers checking");
            menu->close();
        }
    } else {
        passed &= incrementalEdits();
        passed &= rangesAndCorrections();
        passed &= rawSuggestion();
        passed &= menuLifetimeAndGuards();
        passed &= policyClipboardAndSubmission();
    }
    passed &= expect(settings.open(QIODevice::ReadOnly), "isolated settings audit opens");
    passed &= expect(settings.readAll() == config && QFileInfo(settings).size() == beforeSize &&
                         QFileInfo(settings).lastModified() == beforeModified,
                     "enumeration, language, ignore and enable never persist Sonnet settings");
    std::cout << "Prompt spelling: " << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
