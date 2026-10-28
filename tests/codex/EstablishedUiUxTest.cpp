// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "TimingPolicy.h"
#include "codex/AttachmentInput.h"
#include "codex/TurnSettingsWidget.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/ExpandingPromptEditor.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QHelpEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {


bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

bool changed(ConversationView::ReconciliationResult result) {
  return result == ConversationView::ReconciliationResult::Changed;
}

void sendKey(codexui::ExpandingPromptEditor &editor, int key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QKeyEvent event(QEvent::KeyPress, key, modifiers);
  QCoreApplication::sendEvent(&editor, &event);
}

bool promptKeyboardAndFocusContract() {
  codexui::ExpandingPromptEditor editor;
  editor.resize(480, 80);
  editor.show();
  QCoreApplication::processEvents();
  editor.activateWindow();
  editor.setFocus(Qt::TabFocusReason);
  QCoreApplication::processEvents();
  std::cerr << "prompt focus diagnostics: visible=" << editor.isVisible()
            << " active=" << editor.isActiveWindow()
            << " focus=" << editor.hasFocus()
            << " activeWindow=" << QApplication::activeWindow()
            << " focusWidget=" << QApplication::focusWidget() << '\n';
  bool result =
      expect(editor.hasFocus(), "the prompt acquires native keyboard focus");
  int submissions = 0;
  QObject::connect(&editor, &codexui::ExpandingPromptEditor::submitRequested,
                   [&] { ++submissions; });
  editor.setPlainText(QStringLiteral("prompt"));
  editor.moveCursor(QTextCursor::End);
  sendKey(editor, Qt::Key_Return);
  result &= expect(submissions == 1 && editor.hasFocus(),
                   "Return submits without losing prompt focus");
  editor.setPlainText(QStringLiteral("prompt"));
  editor.moveCursor(QTextCursor::End);
  sendKey(editor, Qt::Key_Return, Qt::ShiftModifier);
  result &= expect(submissions == 1 &&
                       editor.toPlainText() == QStringLiteral("prompt\n"),
                   "Shift+Return inserts a newline without submission");
  result &= expect(editor.accessibleName() == QStringLiteral("Message Codex"),
                   "the prompt retains its accessible identity");
  return result;
}

bool promptDropCursorContract() {
  codexui::ExpandingPromptEditor editor;
  editor.resize(480, 80);
  editor.show();
  editor.activateWindow();
  editor.setFocus();
  const int originalFlashTime = QApplication::cursorFlashTime();
  QApplication::setCursorFlashTime(0);
  editor.setPlainText(QStringLiteral("cursor probe"));
  QCoreApplication::processEvents();
  const auto capture = [&](int key) {
    sendKey(editor, key);
    QCoreApplication::processEvents();
    return editor.viewport()->grab().toImage();
  };
  const QImage start = capture(Qt::Key_Home);
  const QPoint startPosition = editor.cursorRect().center();
  const QImage end = capture(Qt::Key_End);
  bool result = expect(start != end, "cursor movement changes visible pixels");
  int attachments = 0;
  QObject::connect(&editor, &codexui::ExpandingPromptEditor::attachmentInput,
                   [&](const QMimeData *) { ++attachments; });
  QMimeData files;
  files.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/cursor-probe.txt"))});
  for (int scenario = 0; scenario < 4; ++scenario) {
    const auto actions = scenario == 1 ? Qt::MoveAction
                                     : Qt::CopyAction | Qt::MoveAction;
    const QPoint position = editor.cursorRect().center();
    QDragEnterEvent enter(position, actions, &files, Qt::LeftButton,
                          Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &enter);
    QDragMoveEvent move(position, actions, &files, Qt::LeftButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &move);
    result &= expect(enter.isAccepted() && move.isAccepted(),
                     "attachment drag reaches the editor before completion");
    const int before = attachments;
    if (scenario == 3) {
      QDragLeaveEvent leave;
      QCoreApplication::sendEvent(editor.viewport(), &leave);
    } else {
      editor.setReadOnly(scenario == 2);
      QDropEvent drop(position, actions, &files, Qt::LeftButton,
                      Qt::NoModifier);
      drop.setDropAction(Qt::MoveAction);
      QCoreApplication::sendEvent(editor.viewport(), &drop);
      result &= expect(scenario == 0
                           ? drop.isAccepted() && drop.dropAction() == Qt::CopyAction
                           : !drop.isAccepted(),
                       "attachments are copied or rejected, never moved");
      editor.setReadOnly(false);
    }
    result &= expect(attachments == before + (scenario == 0 ? 1 : 0) &&
                         editor.toPlainText() == QStringLiteral("cursor probe"),
                     "only accepted drops emit attachments and retain draft text");
    const QImage movedStart = capture(Qt::Key_Home);
    const QImage movedEnd = capture(Qt::Key_End);
    result &= expect(movedStart == start && movedEnd == end,
                     "completed, rejected and cancelled drops restore the visible cursor");
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
      QMouseEvent mouse(type, startPosition,
                        editor.viewport()->mapToGlobal(startPosition),
                        Qt::LeftButton,
                        type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                        Qt::NoModifier);
      QCoreApplication::sendEvent(editor.viewport(), &mouse);
    }
    QCoreApplication::processEvents();
    result &= expect(editor.textCursor().position() == 0 &&
                         editor.viewport()->grab().toImage() == start,
                     "mouse placement moves the visible cursor after a drop");
    QApplication::setCursorFlashTime(100);
    editor.clearFocus();
    editor.setFocus();
    const QImage initial = editor.viewport()->grab().toImage();
    bool blinked = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 500 && !blinked) {
      QCoreApplication::processEvents();
      blinked = editor.viewport()->grab().toImage() != initial;
      QThread::msleep(5);
    }
    result &= expect(blinked, "cursor visibly blinks after attachment drag completion");
    QApplication::setCursorFlashTime(0);
    capture(Qt::Key_End);
  }
  QApplication::setCursorFlashTime(originalFlashTime);
  return result;
}

bool promptAttachmentAdmission() {
  QTemporaryDir files;
  QStringList paths;
  bool result = expect(files.isValid(), "attachment fixture directory exists");
  for (int index = 0; index < 17; ++index) {
    const QString path =
        files.filePath(QStringLiteral("file %1.dat").arg(index));
    QFile file(path);
    result &= expect(file.open(QIODevice::WriteOnly) && file.write("data") == 4,
                     "attachment fixture file is readable");
    paths.push_back(path);
  }
  std::vector<AttachmentDraft> draft;
  result &=
      expect(!appendAttachmentFiles(draft, paths).isEmpty() && draft.empty(),
             "over-limit batches are rejected without partial admission");
  paths.removeLast();
  result &= expect(appendAttachmentFiles(draft, paths).isEmpty() &&
                       draft.size() == 16,
                   "the shared picker/drop admission accepts sixteen files");
  const auto original = draft;
  result &=
      expect(appendAttachmentFiles(draft, paths).isEmpty() && draft == original,
             "duplicate files do not consume attachment slots");
  draft.clear();
  result &= expect(
      !appendAttachmentFiles(draft, {paths.front(), files.path()}).isEmpty() &&
          draft.empty(),
      "directories reject the whole batch rather than becoming file links");
  result &= expect(
      !appendAttachmentFiles(draft, {files.filePath("missing")}).isEmpty(),
      "missing files are rejected");
  QMimeData mixed;
  mixed.setUrls(
      {QUrl::fromLocalFile(paths.front()), QUrl("https://example.com/file")});
  result &=
      expect(!appendAttachmentInput(draft, mixed).isEmpty() && draft.empty(),
             "mixed local and remote drops cannot silently lose files");
  return result;
}

bool promptClipboardAndDropContract() {
  QTemporaryDir files;
  const QByteArray previousDataHome = qgetenv("XDG_DATA_HOME");
  qputenv("XDG_DATA_HOME", files.path().toUtf8());
  ComposerPane composer;
  composer.resize(900, 300);
  composer.show();
  composer.setCanSubmit(true);
  auto *editor = composer.promptEditor();
  QCoreApplication::processEvents();
  QString error;
  bool admit = false;
  int submissions = 0;
  std::vector<AttachmentDraft> submitted;
  ComposerPane::Actions actions;
  actions.attachmentError = [&](QString message) {
    error = std::move(message);
  };
  actions.submit = [&](QString prompt,
                       std::vector<AttachmentDraft> attachments) {
    ++submissions;
    submitted = std::move(attachments);
    return prompt.isEmpty() && admit;
  };
  composer.setActions(std::move(actions));
  QToolButton *attachButton = nullptr;
  for (auto *button : composer.findChildren<QToolButton *>())
    if (button->accessibleName() == QStringLiteral("Attach files"))
      attachButton = button;
  const auto settle = [&] {
    QElapsedTimer wait;
    wait.start();
    while (attachButton && !attachButton->isEnabled() &&
           wait.elapsed() < 5000) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
  };
  QApplication::clipboard()->setText(QStringLiteral("first\n\nlast"));
  editor->paste();
  bool result =
      expect(editor->toPlainText() == QStringLiteral("first\n\nlast") &&
                 composer.attachments().empty(),
             "ordinary multiline paste is unchanged");
  editor->clear();
  QImage image(3840, 2160, QImage::Format_RGB32);
  for (int y = 0; y < image.height(); ++y) {
    auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < image.width(); ++x)
      row[x] = qRgb(x % 256, y % 256, (x + y) % 256);
  }
  QApplication::clipboard()->setImage(image);
  result &= expect(editor->canPaste(), "image clipboard enables native Paste");
  QElapsedTimer timer;
  timer.start();
  editor->paste();
  std::cerr << "4K paste dispatch: " << timer.elapsed() << " ms\n";
  sendKey(*editor, Qt::Key_Return);
  result &= expect(submissions == 0, "Send waits for image-file preparation");
  QApplication::clipboard()->setText(
      QStringLiteral("typing during preparation"));
  editor->paste();
  result &= expect(editor->toPlainText() ==
                       QStringLiteral("typing during preparation"),
                   "text editing continues while image preparation runs");
  editor->clear();
  settle();
  std::cerr << "4K clipboard image ready: " << timer.elapsed() << " ms\n";
  result &= expect(error.isEmpty() && composer.attachments().size() == 1 &&
                       editor->toPlainText().isEmpty(),
                   "clipboard pixels become one attachment, not prompt text");
  if (!composer.attachments().empty()) {
    const QString path =
        QString::fromStdString(composer.attachments().front().path);
    result &= expect(QImage(path) == image &&
                         composer.attachments().front().mimeType == "image/png",
                     "the saved image preserves pixels and is typed as PNG");
    const auto permissions = QFile::permissions(path);
    result &= expect(!(permissions & (QFile::ReadGroup | QFile::ReadOther)),
                     "pasted image data is private to the user");
    sendKey(*editor, Qt::Key_Return);
    result &= expect(submissions == 1 && composer.attachments().size() == 1,
                     "rejected attachment-only submission preserves the draft");
    admit = true;
    composer.setActiveTurn(true);
    sendKey(*editor, Qt::Key_Return);
    result &= expect(submissions == 2 && composer.attachments().empty() &&
                         QFileInfo::exists(path),
                     "image-only steering clears the draft, not its file");
    composer.setAttachments(submitted);
    result &= expect(
        QImage(QString::fromStdString(composer.attachments().front().path)) ==
            image,
        "recovery through attachment descriptors retains a readable image");
    composer.clearDraft();
  }
  QList<QUrl> urls;
  for (const QString &name :
       {QStringLiteral("image.png"), QStringLiteral("notes ü.txt"),
        QStringLiteral("document.pdf"), QStringLiteral("archive.zip")}) {
    QFile file(files.filePath(name));
    result &= expect(file.open(QIODevice::WriteOnly) && file.write("data") == 4,
                     "mixed drop fixture file exists");
    urls.push_back(QUrl::fromLocalFile(file.fileName()));
  }
  QMimeData dropData;
  dropData.setUrls(urls);
  const auto drop = [&](const QMimeData &mime) {
    QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor->viewport(), &enter);
    QDropEvent event(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                     Qt::LeftButton, Qt::NoModifier);
    event.setDropAction(Qt::MoveAction);
    QCoreApplication::sendEvent(editor->viewport(), &event);
    if (!editor->isReadOnly())
      settle();
    return enter.isAccepted() && event.isAccepted() &&
           event.dropAction() == Qt::CopyAction;
  };
  result &= expect(drop(dropData) && composer.attachments().size() == 4 &&
                       editor->toPlainText().isEmpty(),
                   "drop copies all file types without inserting URLs");
  result &= expect(drop(dropData) && composer.attachments().size() == 4,
                   "repeated drops do not duplicate attachments");
  composer.resize(900, composer.sizeHint().height());
  QCoreApplication::processEvents();
  if (qEnvironmentVariableIsSet("CODEXUI_ATTACHMENT_CAPTURE"))
    composer.grab().save(qEnvironmentVariable("CODEXUI_ATTACHMENT_CAPTURE"));
  for (const QUrl &url : urls)
    result &= expect(QFileInfo::exists(url.toLocalFile()),
                     "drop never moves the source file");
  auto *pasteData = new QMimeData;
  pasteData->setUrls(urls);
  composer.clearDraft();
  QApplication::clipboard()->setMimeData(pasteData);
  editor->paste();
  settle();
  result &= expect(composer.attachments().size() == 4,
                   "file-manager copy/paste uses the same admission");
  composer.clearDraft();
  composer.setCanSubmit(false);
  editor->paste();
  result &= expect(!error.isEmpty() && composer.attachments().empty(),
                   "unavailable attachment admission reports an error without "
                   "changing the draft");
  composer.setCanSubmit(true);
  editor->setReadOnly(true);
  result &= expect(!drop(dropData) && composer.attachments().empty(),
                   "read-only editor rejects file drops");
  editor->setReadOnly(false);
  error.clear();
  QApplication::clipboard()->setImage(image);
  editor->paste();
  composer.clearDraft();
  settle();
  result &= expect(composer.attachments().empty(),
                   "cleared drafts do not regain an in-flight image");
  {
    ComposerPane closing;
    closing.setCanSubmit(true);
    closing.promptEditor()->paste();
  }
  QCoreApplication::processEvents();
  QApplication::clipboard()->setText(
      QStringLiteral("https://example.com/link"));
  editor->paste();
  result &= expect(editor->toPlainText() ==
                       QStringLiteral("https://example.com/link"),
                   "ordinary web links remain prompt text");
  QApplication::clipboard()->clear();
  if (previousDataHome.isNull())
    qunsetenv("XDG_DATA_HOME");
  else
    qputenv("XDG_DATA_HOME", previousDataHome);
  return result;
}

bool turnSettingsAreStableAccessibleProjections() {
  TurnSettingsPolicy policy;
  TurnSettingsWidget widget(policy);
  widget.resize(900, widget.sizeHint().height());
  TurnSettingsContext settings;
  settings.identity = "thread-a";
  settings.canonical[TurnSettingField::Sandbox] = "danger-full-access";
  settings.canonical[TurnSettingField::Network] = "enabled";
  settings.canonical[TurnSettingField::Approval] = "future-policy";
  TurnSettingModel model;
  model.choice = {"gpt-a", "GPT A", "Model description"};
  model.isDefault = true;
  model.supportsPersonality = false;
  settings.models.push_back(std::move(model));
  settings.permissionProfiles.push_back(
      {":workspace", "Workspace", "Workspace description"});
  widget.setContext(settings);
  widget.show();
  QCoreApplication::processEvents();

  constexpr std::array<std::pair<const char*, const char*>, 11> settingControls{{{"Model", "codexModel"},
                                                                                 {"Reasoning", "codexEffort"},
                                                                                 {"Style", "codexPersonality"},
                                                                                 {"Access", "codexSandbox"},
                                                                                 {"Network", "codexNetwork"},
                                                                                 {"Approval", "codexApproval"},
                                                                                 {"Approval reviewer", "codexReviewer"},
                                                                                 {"Permission profile", "codexPermissionProfile"},
                                                                                 {"Service tier", "codexServiceTier"},
                                                                                 {"Reasoning summary", "codexSummary"},
                                                                                 {"Collaboration mode", "codexCollaboration"}}};
  std::array<QWidget *, settingControls.size()> controls{};
  std::array<QRect, settingControls.size()> controlGeometry{};
  for (std::size_t index = 0; index < settingControls.size(); ++index) {
    controls[index] = widget.findChild<QWidget *>(
        QString::fromLatin1(settingControls[index].second));
    if (controls[index])
      controlGeometry[index] = controls[index]->geometry();
  }
  auto *modelControl = qobject_cast<QComboBox *>(controls[0]);
  auto *network = widget.findChild<QComboBox *>(QStringLiteral("codexNetwork"));
  auto *personality =
      widget.findChild<QComboBox *>(QStringLiteral("codexPersonality"));
  auto *approval = qobject_cast<QComboBox *>(controls[5]);
  auto *reviewer = qobject_cast<QComboBox *>(controls[6]);
  auto* permissionProfile = qobject_cast<QComboBox*>(controls[7]);
  auto *more =
      widget.findChild<QToolButton *>(QStringLiteral("codexMoreSettings"));
  const auto labelBuddy = [&widget](const QString &caption) -> QWidget * {
    for (QLabel *label : widget.findChildren<QLabel *>())
      if (label->text() == caption)
        return label->buddy();
    return nullptr;
  };
  bool completeAccessibleProjection = true;
  for (std::size_t index = 0; index < settingControls.size(); ++index) {
    const QString caption = QString::fromLatin1(settingControls[index].first);
    completeAccessibleProjection =
        completeAccessibleProjection && controls[index] &&
        controls[index]->accessibleName() == caption &&
        labelBuddy(caption) == controls[index];
  }
  bool result = expect(completeAccessibleProjection && modelControl && network && personality && approval && reviewer &&
                           permissionProfile && more && !widget.findChild<QLineEdit*>(QStringLiteral("codexWorkspace")),
                       "turn settings expose one stable native control per setting");
  result &=
      expect(network && !network->isEnabled() && network->toolTip() == QStringLiteral("Full access already includes network access") &&
                 personality && !personality->isEnabled() &&
                 personality->toolTip() == QStringLiteral("The selected model does not support style choices"),
             "typed compatibility drives enabled state and guidance");
  result &= expect(policy.startOptions(TurnSettingsScope::Turn).empty(),
                   "reported settings and catalog defaults do not become outgoing overrides");
  auto* effort = widget.findChild<QComboBox*>(QStringLiteral("codexEffort"));
  result &= expect(effort && effort->currentText() == QStringLiteral("Default (not reported)") &&
                       effort->toolTip().contains(QStringLiteral("not reported")),
                   "unreported effective reasoning is explicit and accessible through its tooltip");
  auto* collaboration = widget.findChild<QComboBox*>(QStringLiteral("codexCollaboration"));
  result &=
      expect(collaboration && collaboration->currentIndex() == -1 && collaboration->placeholderText() == QStringLiteral("Not reported"),
             "missing collaboration is distinct from reported Code mode");
  result &= expect(collaboration && more && !more->menu()->isAncestorOf(collaboration) &&
                       collaboration->mapTo(&widget, QPoint()).y() == approval->mapTo(&widget, QPoint()).y() &&
                       collaboration->mapTo(&widget, QPoint()).x() == modelControl->mapTo(&widget, QPoint()).x(),
                   "Collaboration occupies row two column one and has no second popup implementation");
  TurnSettingsContext tabContext = settings;
  tabContext.canonical[TurnSettingField::Sandbox] = "read-only";
  widget.setContext(tabContext);
  widget.activateWindow();
  network->setFocus();
  QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
  QApplication::sendEvent(network, &tab);
  result &= expect(collaboration->hasFocus(), "Tab moves from Network to Collaboration mode");
  QApplication::sendEvent(collaboration, &tab);
  result &= expect(approval->hasFocus(), "Tab moves from Collaboration mode to Approval");
  widget.setContext(settings);
  TurnSettingsContext codeMode = settings;
  codeMode.canonical[TurnSettingField::Collaboration] = DefaultTurnSetting;
  widget.setContext(codeMode);
  result &= expect(collaboration && collaboration->currentText() == QStringLiteral("Code") &&
                       policy.startOptions(TurnSettingsScope::Turn).empty(),
                   "reported Code mode is displayed without becoming an authored override");
  widget.setContext(settings);
  int projectionSignals = 0;
  for (QWidget *control : controls) {
    if (auto *combo = qobject_cast<QComboBox *>(control))
      QObject::connect(combo, &QComboBox::currentIndexChanged, &widget,
                       [&projectionSignals] { ++projectionSignals; });
  }
  TurnSettingsContext signalUpdate = settings;
  signalUpdate.canonical[TurnSettingField::Approval] = "on-request";
  widget.setContext(signalUpdate);
  widget.setContext(settings);
  bool untouched = true;
  for (std::size_t field = 0; field < TurnSettingFieldCount; ++field)
    untouched =
        untouched && !policy.touched(static_cast<TurnSettingField>(field));
  result &= expect(projectionSignals == 0 && untouched,
                   "authoritative projection emits no authored edits");
  result &= expect(
      modelControl->itemData(modelControl->findData(QStringLiteral("gpt-a")),
                             Qt::ToolTipRole) ==
              QStringLiteral("Model description") &&
          permissionProfile->itemData(
              permissionProfile->findData(QStringLiteral(":workspace")),
              Qt::ToolTipRole) == QStringLiteral("Workspace description") &&
          approval->currentData() == QStringLiteral("future-policy") &&
          approval->currentText() == QStringLiteral("Future policy"),
      "catalog descriptions and unknown provider values remain visible");
  const int approvalChoices = approval ? approval->count() : 0;
  TurnSettingsContext secondUnknown = settings;
  secondUnknown.canonical[TurnSettingField::Approval] = "later-policy";
  widget.setContext(secondUnknown);
  result &=
      expect(approval && approval->count() == approvalChoices &&
                 approval->currentData() == QStringLiteral("later-policy") &&
                 approval->findData(QStringLiteral("future-policy")) < 0,
             "fixed settings retain only the current unknown value");
  widget.setContext(settings);
  if (modelControl && modelControl->lineEdit()) {
    modelControl->lineEdit()->selectAll();
    QKeyEvent blankModel(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier,
                         QStringLiteral(" "));
    QCoreApplication::sendEvent(modelControl->lineEdit(), &blankModel);
  }
  result &=
      expect(modelControl &&
                 modelControl->currentData() ==
                     QString::fromLatin1(DefaultTurnSetting) &&
                 policy.values()[TurnSettingField::Model] == DefaultTurnSetting,
             "blank editable model input reprojects its normalized value");

  widget.setContext(settings);
  QCoreApplication::processEvents();
  bool controlsStable = true;
  for (std::size_t index = 0; index < settingControls.size(); ++index)
    controlsStable = controlsStable &&
                     widget.findChild<QWidget *>(QString::fromLatin1(
                         settingControls[index].second)) == controls[index] &&
                     (!controls[index] ||
                      controls[index]->geometry() == controlGeometry[index]);
  result &= expect(controlsStable,
                   "a semantic no-op neither replaces nor moves controls");
  TurnSettingsContext canonicalUpdate = settings;
  canonicalUpdate.canonical[TurnSettingField::Approval] = "on-request";
  widget.setContext(canonicalUpdate);
  result &=
      expect(!policy.touched(TurnSettingField::Approval) &&
                 approval->currentData() == QStringLiteral("on-request"),
             "programmatic projection changes do not become authored edits");
  widget.setContext(settings);
  reviewer->setCurrentIndex(reviewer->findData(QStringLiteral("user")));
  result &= expect(policy.touched(TurnSettingField::Reviewer) &&
                       more->text() == QStringLiteral("More •"),
                   "secondary authored settings update the one changed marker");
  if (approval)
    approval->setCurrentIndex(approval->findData(QStringLiteral("never")));
  TurnSettingsContext other = settings;
  other.identity = "thread-b";
  widget.setContext(std::move(other));
  widget.setContext(settings);
  result &= expect(policy.startOptions(TurnSettingsScope::Turn)
                           .value("approvalPolicy", "") == "never",
                   "thread switching restores the draft by semantic identity");
  const QString capturePath = qEnvironmentVariable("CODEXUI_SETTINGS_CAPTURE");
  if (!capturePath.isEmpty()) {
      result &= expect(widget.grab().save(capturePath), "turn-settings visual evidence is captured");
  }
  return result;
}

bool turnSettingsCatalogProjectionScalesLinearly() {
  struct Measurement final {
    qint64 nanoseconds = 0;
    int modelChoices = 0;
    int profileChoices = 0;
  };
  const auto measure = [](int count) {
    TurnSettingsPolicy policy;
    TurnSettingsWidget widget(policy);
    TurnSettingsContext context;
    context.identity = "catalog-scale";
    context.models.reserve(static_cast<std::size_t>(count));
    context.permissionProfiles.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
      const std::string suffix = std::to_string(index);
      TurnSettingModel model;
      model.choice = {"model-" + suffix, "Model " + suffix, {}};
      context.models.push_back(std::move(model));
      context.permissionProfiles.push_back(
          {"profile-" + suffix, "Profile " + suffix, {}});
    }
    QElapsedTimer elapsed;
    elapsed.start();
    widget.setContext(std::move(context));
    const qint64 nanoseconds = elapsed.nsecsElapsed();
    const auto *models =
        widget.findChild<QComboBox *>(QStringLiteral("codexModel"));
    const auto *profiles =
        widget.findChild<QComboBox *>(QStringLiteral("codexPermissionProfile"));
    return Measurement{nanoseconds, models ? models->count() : 0,
                       profiles ? profiles->count() : 0};
  };
  const auto minimum = [&measure](int count) {
    Measurement result{std::numeric_limits<qint64>::max(), 0, 0};
    for (int run = 0; run < 3; ++run) {
      const Measurement current = measure(count);
      if (current.nanoseconds < result.nanoseconds)
        result = current;
    }
    return result;
  };

  static_cast<void>(measure(64));
  const Measurement small = minimum(2'048);
  const Measurement large = minimum(8'192);
  const qint64 linearAllowance = small.nanoseconds * 7 + 25'000'000;
  std::cout << "Settings catalog projection: "
            << small.nanoseconds / 1'000'000.0 << " ms/2048, "
            << large.nanoseconds / 1'000'000.0 << " ms/8192\n";
  return expect(
      small.modelChoices == 2'049 && small.profileChoices == 2'049 &&
          large.modelChoices == 8'193 && large.profileChoices == 8'193 &&
          codexui::testing::timingLimit(large.nanoseconds <= linearAllowance),
      "settings catalog projection remains quantitatively linear");
}

bool attachmentLayoutFollowsFontAndWidth() {
  const QFont original = qApp->font();
  ComposerPane composer;
  std::vector<AttachmentDraft> attachments;
  for (int row = 0; row < 6; ++row)
    attachments.push_back({"/workspace/file-" + std::to_string(row),
                           "Long attachment filename with spaces α " +
                               std::to_string(row), "text/plain", 1});
  composer.setAttachments(attachments);
  composer.show();
  auto *scroll = composer.findChild<QScrollArea *>();
  bool result = true;
  for (int points : {10, 16, 10}) {
    QFont font = original;
    font.setPointSize(points);
    qApp->setFont(font);
    for (int width : {900, 440, 900}) {
      composer.resize(width, composer.sizeHint().height());
      for (int pass = 0; pass < 4; ++pass)
        QCoreApplication::processEvents();
      result &= expect(scroll->verticalScrollBar()->maximum() > 0,
                       "attachments beyond four rows scroll without external sizing");
      for (auto *box : composer.findChildren<QFrame *>("attachmentFileBox")) {
        auto *label = box->findChild<QLabel *>();
        result &= expect(label && label->height() >= label->heightForWidth(label->width()),
                         "attachment text fits after font and width changes");
      }
      for (auto *button : scroll->findChildren<QPushButton *>())
        result &= expect(button->height() >= button->sizeHint().height(),
                         "attachment removal caption fits enlarged fonts");
      auto *layout = scroll->widget()->layout();
      const int fourthBottom = layout->itemAt(3)->geometry().bottom() + 1;
      result &= expect(scroll->viewport()->height() == fourthBottom,
                       "attachment viewport ends at the fourth natural row");
    }
  }
  composer.setAttachments({attachments.front()});
  for (int pass = 0; pass < 4; ++pass)
    QCoreApplication::processEvents();
  result &= expect(scroll->verticalScrollBar()->maximum() == 0,
                   "one attachment shrinks the viewport without stale scroll range");
  composer.clearDraft();
  result &= expect(!scroll->isVisible(), "empty attachment panel disappears");
  qApp->setFont(original);
  return result;
}

bool controlsRetainUsableGeometryAndAuthority() {
  const QFont originalFont = qApp->font();
  const QString originalStyle = qApp->styleSheet();
  bool result = true;
  for (int points : {10, 16}) {
    QFont font = originalFont;
    font.setPointSize(points);
    qApp->setFont(font);
    qApp->setStyleSheet(UiStyle::applicationStyleSheet());
    TurnSettingsPolicy policy;
    TurnSettingsWidget settings(policy);
    settings.show();
    auto *model = settings.findChild<QComboBox *>("codexModel");
    auto *access = settings.findChild<QComboBox *>("codexSandbox");
    const auto y = [&settings](QWidget *widget) {
      return widget->mapTo(&settings, QPoint{}).y();
    };
    for (int width : {1800, 360, 1800}) {
      settings.resize(width, settings.layout()->heightForWidth(width));
      QCoreApplication::processEvents();
      result &= expect((width == 1800) == (y(model) == y(access)),
                       "settings reflow in place between four and two columns");
      for (const char *name : {"codexModel", "codexEffort", "codexApproval",
                               "codexSandbox", "codexNetwork", "codexPersonality"}) {
        auto *control = settings.findChild<QComboBox *>(name);
        result &= expect(control->width() >= control->minimumSizeHint().width() &&
                             control->height() >= control->minimumSizeHint().height(),
                         "settings respect font-dependent control geometry");
      }
    }
    ComposerPane composer;
    composer.resize(700, composer.sizeHint().height());
    composer.setActiveTurn(true);
    composer.setCanSubmit(false);
    composer.show();
    QCoreApplication::processEvents();
    for (auto *button : composer.findChildren<QPushButton *>()) {
      if (button->text() != "Stop" && button->text() != "Steer")
        continue;
      result &= expect(!button->isEnabled(), "observer cannot steer or stop");
      result &= expect(button->width() >= button->sizeHint().width(),
                       "composer action captions fit enlarged fonts");
    }
    composer.setCanSubmit(true);
    for (auto *button : composer.findChildren<QPushButton *>())
      if (button->text() == "Stop")
        result &= expect(button->isEnabled(), "controller can stop without a draft");
  }
  qApp->setFont(originalFont);
  qApp->setStyleSheet(originalStyle);
  return result;
}

ui::ThreadListRow
row(std::string id, std::string title,
    nodegraph::NodeStatus status = nodegraph::NodeStatus::Unknown) {
  ui::ThreadListRow result;
  result.id = std::move(id);
  result.presentationKey = result.id;
  result.title = std::move(title);
  result.status = std::move(status);
  result.cwd = "/workspace";
  return result;
}

bool threadPaneSnapshotAndActionContract() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef rootTarget;
  nodegraph::NodeRef childTarget;
  {
    auto write = graph.write();
    rootTarget = write.upsert({nodegraph::NodeKind::Thread, "root"});
    childTarget = write.upsert({nodegraph::NodeKind::Thread, "child"});
    static_cast<void>(write.finish());
  }
  ThreadPane pane;
  pane.resize(320, 520);
  ui::ThreadListSnapshot snapshot;
  snapshot.selectedThreadId = "child";
  snapshot.providerReady = true;
  snapshot.canControl = true;
  ui::ThreadListRow root = row("root", "Root", nodegraph::NodeStatus::Running);
  root.target = rootTarget;
  root.children.push_back(
      row("child", "Child", nodegraph::NodeStatus::Completed));
  root.children.front().target = childTarget;
  snapshot.roots.push_back(std::move(root));

  nodegraph::NodeRef selected;
  ThreadPane::Actions actions;
  actions.select = [&](const nodegraph::NodeRef &target) { selected = target; };
  pane.setActions(std::move(actions));
  pane.refresh(snapshot);
  pane.show();
  QCoreApplication::processEvents();
  auto *tree = pane.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  QTreeWidgetItem *rootItem = tree ? tree->topLevelItem(0) : nullptr;
  QTreeWidgetItem *childItem =
      rootItem && rootItem->childCount() == 1 ? rootItem->child(0) : nullptr;
  bool result = expect(tree && tree->topLevelItemCount() == 1 && rootItem &&
                           childItem && tree->currentItem() == childItem,
                       "a selected child retains its native tree hierarchy");
  if (tree) {
    selected.reset();
    tree->setCurrentItem(rootItem);
    tree->setCurrentItem(childItem);
    QCoreApplication::processEvents();
    result &= expect(selected == childTarget,
                     "the established action API emits the exact target");
  }

  ui::ThreadListRow draft;
  draft.id = "draft:new-thread";
  draft.presentationKey = "creation:test";
  draft.title = "New thread";
  draft.cwd = "/workspace";
  pane.beginOptimisticThread(draft);
  QCoreApplication::processEvents();
  result &= expect(tree && tree->topLevelItemCount() == 2,
                   "the optimistic row appears through normal refresh");
  pane.discardOptimisticThread("draft:new-thread");
  QCoreApplication::processEvents();
  result &= expect(tree && tree->topLevelItemCount() == 1,
                   "confirming the draft removes only its optimistic row");
  return result;
}

VisibleCardData card(CardKind kind, std::string item, CardPayload payload) {
  VisibleCardData result;
  result.key = AuthoritativeItemKey{"thread", "turn", item};
  result.kind = kind;
  result.threadId = "thread";
  result.turnId = "turn";
  result.itemId = std::move(item);
  result.payload = std::move(payload);
  return result;
}

bool conversationOwnershipAndAtomicReconcileContract() {
  ConversationView view;
  view.resize(820, 620);
  view.show();
  ConversationSnapshot snapshot;
  snapshot.threadId = "thread";
  TurnSection section;
  section.key = "turn";
  section.turnId = "turn";
  section.cards.push_back(
      card(CardKind::UserMessage, "user", UserMessageData{"Question", {}}));
  section.rootCardKey = section.cards.front().key;
  section.cards.push_back(
      card(CardKind::AgentMessage, "agent", AgentMessageData{"Answer", "final_answer"}));
  snapshot.sections.push_back(std::move(section));
  const bool reconciled = changed(view.reconcile(snapshot));
  QCoreApplication::processEvents();

  const QModelIndex owner = view.conversationModel()->index(0);
  const QModelIndex answer = view.conversationModel()->index(1);
  bool result =
      expect(reconciled && view.conversationModel()->rowCount() == 2 &&
                 owner.data(ConversationItemModel::TurnRootRole).toBool() &&
                 answer.data(ConversationItemModel::NestedCardRole).toBool() &&
                 view.visualRect(answer).left() > view.visualRect(owner).left(),
             "one reconcile exposes a complete virtualized turn");
  const qulonglong presentationPasses =
      view.property("graphRefreshPasses").toULongLong();
  result &= expect(view.reconcile(snapshot) ==
                           ConversationView::ReconciliationResult::Unchanged &&
                       view.property("graphRefreshPasses").toULongLong() ==
                           presentationPasses,
                   "repeating identical visible state performs no Qt "
                   "presentation pass");
  return result;
}

bool nestedWheelGestureHasOneRoutingOwner(bool measurePerformance) {
  MiddleRegionWidget region;
  region.resize(1100, 700);
  region.show();
  ConversationView &conversation = region.conversation();
  ConversationSnapshot snapshot;
  snapshot.threadId = "thread";
  TurnSection section;
  section.key = "turn";
  section.turnId = "turn";
  std::string longMessage;
  std::string commandText;
  std::string commandOutput;
  for (int line = 0; line < 70; ++line)
    longMessage += "outer conversation line " + std::to_string(line) + '\n';
  for (int line = 0; line < 40; ++line)
    commandText += "printf command-line-" + std::to_string(line) + '\n';
  for (int line = 0; line < 100; ++line)
    commandOutput += "nested output " + std::to_string(line) + '\n';
  section.cards.push_back(
      card(CardKind::UserMessage, "user", UserMessageData{longMessage, {}}));
  section.rootCardKey = section.cards.front().key;
  section.cards.push_back(card(
      CardKind::CommandExecution, "command",
      CommandExecutionData{commandText, commandOutput, "/workspace", 0, 1}));
  snapshot.sections.push_back(std::move(section));
  bool result = expect(changed(conversation.reconcile(snapshot)),
                       "real nested-wheel fixture reconciles");
  for (int pass = 0; pass < 8; ++pass)
    QCoreApplication::processEvents();

  CommandOutputView *const output = conversation.findChild<CommandOutputView *>(
      QStringLiteral("commandOutputView"));
  ContentSizedTextView *const command = dynamic_cast<ContentSizedTextView *>(
      conversation.findChild<QTextEdit *>(QStringLiteral("commandTextView")));
  ConversationCard *commandCard = nullptr;
  for (QWidget *candidate = output; candidate && candidate != &conversation;
       candidate = candidate->parentWidget()) {
    if ((commandCard = qobject_cast<ConversationCard *>(candidate)))
      break;
  }
  result &= expect(output && command && commandCard && output->isVisible() &&
                       command->isVisible(),
                   "the wheel fixture uses one real expanded materialized "
                   "command card");
  if (!output || !command || !commandCard)
    return false;

  output->setFocus(Qt::MouseFocusReason);
  QCoreApplication::processEvents();
  QScrollBar *const inner = output->verticalScrollBar();
  QScrollBar *const commandInner = command->verticalScrollBar();
  QScrollBar *const outer = conversation.verticalScrollBar();
  class ApplicationScrollRouter final : public QObject {
  public:
    explicit ApplicationScrollRouter(MiddleRegionWidget &region)
        : region_(region) {}

    int maximumDepth = 0;
    bool overflow = false;
    bool topLevelConsumed = false;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
      if (!event || event->type() != QEvent::Wheel)
        return false;
      const bool topLevel = depth_ == 0;
      ++depth_;
      maximumDepth = std::max(maximumDepth, depth_);
      if (depth_ > 3) {
        overflow = true;
        --depth_;
        return true;
      }
      const bool consumed = region_.routeScrollEvent(watched, event);
      if (topLevel)
        topLevelConsumed = consumed;
      --depth_;
      return consumed;
    }

  private:
    MiddleRegionWidget &region_;
    int depth_ = 0;
  } router(region);

  struct WheelSample {
    QPointF position;
    QPoint pixelDelta;
    QPoint angleDelta;
    QPointF globalPosition;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    Qt::ScrollPhase phase = Qt::NoScrollPhase;
    bool inverted = false;
    Qt::MouseEventSource source = Qt::MouseEventNotSynthesized;
    const QPointingDevice *device = nullptr;
    ulong timestamp = 0;
  };
  class WheelProbe final : public QObject {
  public:
    std::vector<WheelSample> samples;

    void clear() { samples.clear(); }

  protected:
    bool eventFilter(QObject *, QEvent *event) override {
      if (event && event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        samples.push_back({wheel->position(), wheel->pixelDelta(),
                           wheel->angleDelta(), wheel->globalPosition(),
                           wheel->buttons(), wheel->modifiers(), wheel->phase(),
                           wheel->inverted(), wheel->source(),
                           wheel->pointingDevice(), wheel->timestamp()});
      }
      return false;
    }
  } outerProbe, outputProbe, commandProbe;

  qApp->installEventFilter(&router);
  conversation.viewport()->installEventFilter(&outerProbe);
  output->viewport()->installEventFilter(&outputProbe);
  command->viewport()->installEventFilter(&commandProbe);
  ulong nextTimestamp = 42000;
  const QPointingDevice *const device =
      QPointingDevice::primaryPointingDevice();
  const auto wheelPosition = [](const QWidget *target) {
    return QPointF(target->rect().center()) + QPointF(0.25, 0.75);
  };
  const auto sendAt = [&](QWidget *target, Qt::ScrollPhase phase,
                          QPoint pixelDelta, QPoint angleDelta = {},
                          Qt::MouseButtons buttons = Qt::NoButton,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                          bool inverted = false, std::optional<QPointF> position = {}) -> ulong {
    const QPointF local = position.value_or(wheelPosition(target));
    QWheelEvent event(local, target->mapToGlobal(local), pixelDelta, angleDelta,
                      buttons, modifiers, phase, inverted,
                      Qt::MouseEventSynthesizedByApplication, device);
    const ulong timestamp = ++nextTimestamp;
    event.setTimestamp(timestamp);
    event.ignore();
    router.topLevelConsumed = false;
    static_cast<void>(QApplication::sendEvent(target, &event));
    result &= expect(router.topLevelConsumed && event.isAccepted(),
                     "the sole middle-region router consumes the source "
                     "wheel event");
    return timestamp;
  };
  const auto sendOutput = [&](Qt::ScrollPhase phase, QPoint pixelDelta,
                              QPoint angleDelta = {}) {
    return sendAt(output->viewport(), phase, pixelDelta, angleDelta);
  };

  result &= expect(inner->maximum() > inner->minimum() &&
                       commandInner->maximum() > commandInner->minimum() &&
                       outer->maximum() > outer->minimum(),
                   "the real command and conversation expose independent "
                   "scroll ranges");

  // Platforms may omit ScrollBegin. Test the visible real command editor
  // before moving its card out of the viewport.
  commandInner->setValue(commandInner->minimum());
  const int commandBefore = commandInner->value();
  const int outerBeforeCommand = outer->value();
  commandProbe.clear();
  outerProbe.clear();
  static_cast<void>(
      sendAt(command->viewport(), Qt::ScrollUpdate, {}, QPoint(0, -120)));
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
  result &= expect(commandInner->value() > commandBefore &&
                       outer->value() == outerBeforeCommand &&
                       commandProbe.samples.size() == 2 &&
                       commandProbe.samples.back().phase == Qt::ScrollEnd &&
                       outerProbe.samples.empty(),
                   "a missing-Begin command-text gesture retains one native "
                   "owner through crossed End");

  // Lifecycle-only events must not release a command-output pause, and an
  // established owner remains authoritative after the pointer leaves the
  // complete center region. End over a sibling pane must also retire it.
  CommandOutputView::State detached = output->state();
  detached.followsLatest = true;
  detached.value = inner->maximum();
  output->restoreState(detached);
  outer->setValue(outer->maximum());
  static_cast<void>(sendOutput(Qt::ScrollBegin, {}));
  static_cast<void>(sendOutput(Qt::ScrollUpdate, {}, QPoint(0, 120)));
  static_cast<void>(sendOutput(Qt::ScrollEnd, {}));
  result &= expect(!output->followsLatest() &&
                       conversation.mode() == ConversationView::Mode::Paused &&
                       outer->value() == outer->maximum(),
                   "an upward output gesture establishes the semantic pause "
                   "used by the zero-intent regression");
  outerProbe.clear();
  const QPointF paneGlobal =
      region.threads().mapToGlobal(wheelPosition(&region.threads()));
  const QPointF paneInOuter =
      conversation.viewport()->mapFromGlobal(paneGlobal);
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollBegin, {}));
  static_cast<void>(sendAt(&region.threads(), Qt::ScrollUpdate, QPoint(24, 0)));
  static_cast<void>(sendAt(&region.threads(), Qt::ScrollEnd, {}));
  result &= expect(
      !output->followsLatest() &&
          conversation.mode() == ConversationView::Mode::Paused &&
          outerProbe.samples.size() == 3 &&
          outerProbe.samples[1].position == paneInOuter &&
          outerProbe.samples[1].globalPosition == paneGlobal &&
          outerProbe.samples.back().phase == Qt::ScrollEnd,
      "zero-intent lifecycle preserves pause state, fractional coordinates, "
      "and the outer owner beyond the center region");

  inner->setValue(inner->maximum() / 2);
  outer->setValue(outer->maximum() / 2);
  const int innerAfterOutsideEnd = inner->value();
  const int outerAfterOutsideEnd = outer->value();
  outputProbe.clear();
  outerProbe.clear();
  static_cast<void>(sendOutput(Qt::ScrollUpdate, {}, QPoint(0, -120)));
  static_cast<void>(sendOutput(Qt::ScrollEnd, {}));
  result &=
      expect(inner->value() > innerAfterOutsideEnd &&
                 outer->value() == outerAfterOutsideEnd &&
                 outputProbe.samples.size() == 2 && outerProbe.samples.empty(),
             "End outside the center retires the owner before a new "
             "missing-Begin nested gesture");

  // Rule 1: starting outside a nested output fixes the outer owner even when
  // the pointer later crosses the output, reverses, sends zero, and ends there.
  outer->setValue(outer->maximum() / 2);
  inner->setValue(inner->maximum() / 2);
  const int outerBeforeCrossing = outer->value();
  const int innerBeforeCrossing = inner->value();
  outerProbe.clear();
  outputProbe.clear();
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollBegin, {}));
  const ulong crossedTimestamp =
      sendOutput(Qt::ScrollUpdate, {}, QPoint(0, 120));
  const int outerAfterUp = outer->value();
  const QPointF crossedGlobal =
      output->viewport()->mapToGlobal(wheelPosition(output->viewport()));
  const ulong zeroTimestamp =
      sendAt(output->viewport(), Qt::ScrollUpdate, {}, {}, Qt::MiddleButton,
             Qt::AltModifier, true);
  static_cast<void>(sendOutput(Qt::ScrollUpdate, {}, QPoint(0, -120)));
  static_cast<void>(sendOutput(Qt::ScrollEnd, {}));
  result &= expect(
      outerAfterUp < outerBeforeCrossing && outer->value() > outerAfterUp &&
          inner->value() == innerBeforeCrossing &&
          outputProbe.samples.empty() && outerProbe.samples.size() == 5 &&
          outerProbe.samples[1].timestamp == crossedTimestamp &&
          outerProbe.samples[1].angleDelta == QPoint(0, 120) &&
          outerProbe.samples[1].source ==
              Qt::MouseEventSynthesizedByApplication &&
          outerProbe.samples[1].device == device &&
          outerProbe.samples[2].timestamp == zeroTimestamp &&
          outerProbe.samples[2].globalPosition == crossedGlobal &&
          outerProbe.samples[2].buttons == Qt::MiddleButton &&
          outerProbe.samples[2].modifiers == Qt::AltModifier &&
          outerProbe.samples[2].inverted &&
          outerProbe.samples.back().phase == Qt::ScrollEnd,
      "a gesture that starts outside command output keeps the conversation "
      "owner and exact lifecycle after crossing into output");

  // Rule 2: a nested start already at the chosen directional edge gives the
  // complete gesture to the conversation, in both directions.
  outer->setValue(outer->maximum() / 2);
  inner->setValue(inner->minimum());
  const int outerBeforeTopBoundary = outer->value();
  outerProbe.clear();
  outputProbe.clear();
  static_cast<void>(sendOutput(Qt::ScrollBegin, {}));
  static_cast<void>(sendOutput(Qt::ScrollUpdate, {}, QPoint(0, 120)));
  static_cast<void>(
      sendAt(conversation.viewport(), Qt::ScrollUpdate, {}, QPoint(0, 120)));
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
  result &= expect(
      outer->value() < outerBeforeTopBoundary &&
          inner->value() == inner->minimum() && outputProbe.samples.empty() &&
          outerProbe.samples.size() == 3 &&
          outerProbe.samples.front().phase == Qt::ScrollUpdate &&
          outerProbe.samples.back().phase == Qt::ScrollEnd,
      "an upward gesture starting at the output top chooses and retains the "
      "conversation owner");

  outer->setValue(outer->maximum() / 2);
  inner->setValue(inner->maximum());
  const int outerBeforeBottomBoundary = outer->value();
  outerProbe.clear();
  outputProbe.clear();
  static_cast<void>(sendOutput(Qt::ScrollBegin, {}));
  static_cast<void>(sendOutput(Qt::ScrollUpdate, {}, QPoint(0, -120)));
  static_cast<void>(sendOutput(Qt::ScrollEnd, {}));
  result &= expect(
      outer->value() > outerBeforeBottomBoundary &&
          inner->value() == inner->maximum() && outputProbe.samples.empty() &&
          outerProbe.samples.size() == 2 &&
          outerProbe.samples.back().phase == Qt::ScrollEnd,
      "a downward gesture starting at the output bottom chooses and retains "
      "the conversation owner");

  // Rule 3: a scrollable nested origin keeps the whole gesture. Reaching its
  // edge, reversing, and crossing the pointer never transfers to the outer
  // view; follow-tail is committed only when the complete gesture ends.
  CommandOutputView::State following = output->state();
  following.followsLatest = true;
  following.value = inner->maximum();
  output->restoreState(following);
  QCoreApplication::processEvents();
  outer->setValue(outer->maximum() / 2);
  const int outerBeforeInnerGesture = outer->value();
  outerProbe.clear();
  outputProbe.clear();
  static_cast<void>(sendOutput(Qt::ScrollBegin, {}));
  static_cast<void>(sendOutput(Qt::ScrollUpdate, {}, QPoint(0, 120)));
  const int innerAfterUp = inner->value();
  const bool detachedAfterUp = !output->followsLatest();
  static_cast<void>(sendOutput(Qt::ScrollUpdate, QPoint(0, 48)));
  for (int step = 0; step < 100 && inner->value() < inner->maximum(); ++step)
    static_cast<void>(sendOutput(Qt::ScrollUpdate, {}, QPoint(0, -120)));
  const bool remainsDetachedAtBottom =
      inner->value() == inner->maximum() && !output->followsLatest();
  static_cast<void>(
      sendAt(conversation.viewport(), Qt::ScrollUpdate, {}, QPoint(0, -120)));
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollUpdate, {}));
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
  result &= expect(
      innerAfterUp < inner->maximum() && detachedAfterUp &&
          remainsDetachedAtBottom && output->followsLatest() &&
          outer->value() == outerBeforeInnerGesture &&
          outerProbe.samples.empty() && outputProbe.samples.size() >= 2 &&
          outputProbe.samples[1].pixelDelta == QPoint(0, 48) &&
          outputProbe.samples.back().phase == Qt::ScrollEnd &&
          commandCard->findChild<CommandOutputView *>(
              QStringLiteral("commandOutputView")) == output,
      "a scrollable output owns pixel/angle continuation through its edge, "
      "crossing, reversal, and follow-tail commit");

  // Every NoScrollPhase notch is independent: it hands outward at an existing
  // edge, and remains nested when that editor can move.
  inner->setValue(inner->minimum());
  outer->setValue(outer->maximum() / 2);
  const int outerBeforeEdgeNotch = outer->value();
  outerProbe.clear();
  outputProbe.clear();
  static_cast<void>(sendOutput(Qt::NoScrollPhase, {}, QPoint(0, 120)));
  const bool edgeNotchMovedOuter = outer->value() < outerBeforeEdgeNotch;
  inner->setValue(inner->maximum() / 2);
  const int innerBeforeNestedNotch = inner->value();
  const int outerBeforeNestedNotch = outer->value();
  static_cast<void>(sendOutput(Qt::NoScrollPhase, {}, QPoint(0, 120)));
  result &= expect(
      edgeNotchMovedOuter && inner->value() < innerBeforeNestedNotch &&
          outer->value() == outerBeforeNestedNotch &&
          outerProbe.samples.size() == 1 && outputProbe.samples.size() == 1,
      "standalone wheel notches choose outer only at the nested "
      "directional edge");

  codexui::ExpandingPromptEditor *editor = region.composer().promptEditor();
  QString draft;
  for (int line = 0; line < 80; ++line)
    draft += QStringLiteral("composer line %1\n").arg(line);
  editor->setPlainText(draft);
  QCoreApplication::processEvents();
  QScrollBar *const editorScroll = editor->verticalScrollBar();
  editorScroll->setValue(editorScroll->maximum() / 2);
  outer->setValue(outer->maximum() / 2);
  WheelProbe editorProbe;
  editor->viewport()->installEventFilter(&editorProbe);
  const int editorBefore = editorScroll->value();
  const int outerBeforeComposer = outer->value();
  result &= expect(editorScroll->maximum() > editorScroll->minimum(),
                   "composer wheel fixture has an editor scroll range");
  static_cast<void>(sendAt(editor->viewport(), Qt::ScrollBegin, {}));
  static_cast<void>(
      sendAt(editor->viewport(), Qt::ScrollUpdate, {}, QPoint(0, 120)));
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
  result &= expect(editorScroll->value() < editorBefore &&
                       outer->value() == outerBeforeComposer &&
                       editorProbe.samples.size() == 3 &&
                       editorProbe.samples.back().phase == Qt::ScrollEnd,
                   "a composer gesture stays with its native editor after "
                   "pointer crossing");

  std::vector<AttachmentDraft> attachments;
  for (int index = 0; index < 12; ++index)
    attachments.push_back({"/workspace/file-" + std::to_string(index),
                           "file-" + std::to_string(index), "text/plain", 1});
  region.composer().setAttachments(std::move(attachments));
  QCoreApplication::processEvents();
  QScrollArea *const attachmentScroll =
      region.composer().findChild<QScrollArea *>();
  QCoreApplication::processEvents();
  QScrollBar *const attachmentBar =
      attachmentScroll ? attachmentScroll->verticalScrollBar() : nullptr;
  result &= expect(attachmentBar &&
                       attachmentBar->maximum() > attachmentBar->minimum(),
                   "composer attachment fixture has a scroll range");
  if (attachmentBar) {
    WheelProbe attachmentProbe;
    attachmentScroll->viewport()->installEventFilter(&attachmentProbe);
    attachmentBar->setValue(attachmentBar->maximum() / 2);
    outer->setValue(outer->maximum() / 2);
    const int attachmentBefore = attachmentBar->value();
    const int outerBeforeAttachment = outer->value();
    static_cast<void>(
        sendAt(attachmentScroll->viewport(), Qt::ScrollBegin, {}));
    static_cast<void>(sendAt(attachmentScroll->viewport(), Qt::ScrollUpdate, {},
                             QPoint(0, 120)));
    static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
    result &= expect(attachmentBar->value() < attachmentBefore &&
                         outer->value() == outerBeforeAttachment,
                     "an attachment gesture moves only its native scroll "
                     "surface after pointer crossing");
    result &= expect(attachmentProbe.samples.size() == 3 &&
                         attachmentProbe.samples.back().phase == Qt::ScrollEnd,
                     "the attachment owner receives the complete gesture "
                     "lifecycle");
  }

  struct NativeWheelBar : QScrollBar {
    using QScrollBar::wheelEvent;
  } native;
  native.setRange(outer->minimum(), outer->maximum());
  native.setSingleStep(outer->singleStep());
  native.setPageStep(outer->pageStep());
  const int savedLines = QApplication::wheelScrollLines();
  for (int lines : {1, 3, 7}) {
    QApplication::setWheelScrollLines(lines);
    for (Qt::KeyboardModifiers modifiers :
         {Qt::KeyboardModifiers{}, Qt::KeyboardModifiers{Qt::ControlModifier},
          Qt::KeyboardModifiers{Qt::ShiftModifier}}) {
      outer->setValue(outer->maximum() / 2);
      native.setValue(outer->value());
      static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollBegin, {}));
      for (int delta : {-120, -1, -1, -1, -1, 4, 120}) {
        QWheelEvent reference({}, {}, QPoint(0, -17), QPoint(0, delta),
                              Qt::NoButton, modifiers, Qt::ScrollUpdate, false);
        native.wheelEvent(&reference);
        static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollUpdate,
                                 QPoint(0, -17), QPoint(0, delta),
                                 Qt::NoButton, modifiers));
        result &= expect(outer->value() == native.value(),
                         "conversation uses Qt native distance, fractional "
                         "accumulation, direction and configured scroll lines");
      }
      static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
    }
  }
  QApplication::setWheelScrollLines(savedLines);
  outer->setValue(outer->maximum() / 2);
  const int beforePixels = outer->value();
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollUpdate, QPoint(0, 9)));
  static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
  result &= expect(outer->value() == beforePixels - 9,
                   "pixel-only scrolling works without a Begin event");

  ui::ThreadListSnapshot projects;
  for (int group = 0; group < 12; ++group) {
      ui::ThreadListRow project;
      project.kind = ui::ThreadRowKind::Project;
      project.id = project.presentationKey = "wheel-project-" + std::to_string(group);
      project.title = project.id;
      for (int row = 0; row < 24; ++row) {
          ui::ThreadListRow thread;
          thread.id = thread.presentationKey = project.id + "-thread-" + std::to_string(row);
          thread.title = thread.id;
          project.children.push_back(std::move(thread));
      }
      projects.roots.push_back(std::move(project));
  }
  region.threads().refresh(projects);
  auto* threadList = region.threads().findChild<QTreeWidget*>("threadList");
  threadList->expandAll();
  for (int pass = 0; pass < 4; ++pass) {
      QCoreApplication::processEvents();
  }
  const auto bodyAtTop = [&]() {
      QTreeView* body = nullptr;
      for (auto* candidate : region.threads().findChildren<QTreeView*>("projectThreadBody")) {
          if (candidate->rootIndex() == threadList->model()->index(0, 0)) {
              body = candidate;
          }
      }
      return body;
  };
  QPointer<QTreeView> projectBody = bodyAtTop();
  result &= expect(projectBody && threadList->verticalScrollBar()->maximum() > 100,
                   "real project and threads panel expose nested scrolling ranges");
  if (projectBody) {
      auto* threadScroll = threadList->verticalScrollBar();
      const int conversationBeforeProjects = outer->value();
      for (const bool right : {false, true}) {
          for (const int direction : {-1, 1}) {
              threadScroll->setValue(40);
              projectBody = bodyAtTop();
              auto* projectScroll = projectBody->verticalScrollBar();
              projectScroll->setValue(projectScroll->maximum() / 2);
              const int projectBefore = projectScroll->value();
              const int panelBefore = threadScroll->value();
              const QPointF margin(right ? region.threads().width() - 2 : 1, threadList->y() + 60);
              result &=
                  expect(!region.threads().childAt(margin.toPoint()), "wheel fixture targets the actual bare left/right panel margin");
              static_cast<void>(sendAt(&region.threads(), Qt::ScrollBegin, {}, {}, Qt::NoButton, Qt::NoModifier, false, margin));
              static_cast<void>(
                  sendAt(&region.threads(), Qt::ScrollUpdate, {}, QPoint(0, direction * 15), Qt::NoButton, Qt::NoModifier, false, margin));
              static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollMomentum, {}, QPoint(0, direction * 15)));
              static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
              result &= expect((threadScroll->value() - panelBefore) * direction < 0 &&
                                   (!projectBody || projectBody->verticalScrollBar()->value() == projectBefore) &&
                                   outer->value() == conversationBeforeProjects,
                               "both margins scroll only the threads panel and retain ownership when the pointer enters a project");
              threadScroll->setValue(40);
              projectBody = bodyAtTop();
              projectScroll = projectBody->verticalScrollBar();
              const int beforeNotch = threadScroll->value();
              static_cast<void>(
                  sendAt(&region.threads(), Qt::NoScrollPhase, {}, QPoint(0, direction * 15), Qt::NoButton, Qt::NoModifier, false, margin));
              result &= expect(threadScroll->value() != beforeNotch &&
                                   (!projectBody || projectBody->verticalScrollBar()->value() == projectBefore) &&
                                   outer->value() == conversationBeforeProjects,
                               "a discrete wheel notch over either margin scrolls the threads panel");
          }
      }
      const int beforeControls = threadScroll->value();
      const QPointF controlsMargin(1, threadList->y() - 2);
      QWheelEvent controlsWheel(controlsMargin,
                                region.threads().mapToGlobal(controlsMargin),
                                {},
                                QPoint(0, 120),
                                Qt::NoButton,
                                Qt::NoModifier,
                                Qt::NoScrollPhase,
                                false);
      router.topLevelConsumed = false;
      QApplication::sendEvent(&region.threads(), &controlsWheel);
      result &= expect(!router.topLevelConsumed && threadScroll->value() == beforeControls && outer->value() == conversationBeforeProjects,
                       "the margin route stops above the list, leaving its controls outside the scroll band");
      for (const int direction : {-1, 1}) {
          threadScroll->setValue(40);
          projectBody = bodyAtTop();
          auto* projectScroll = projectBody->verticalScrollBar();
          const int panelBefore = threadScroll->value();
          const int boundary = direction < 0 ? projectScroll->maximum() : projectScroll->minimum();
          projectScroll->setValue(boundary + direction);
          static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollBegin, {}));
          static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollUpdate, {}, QPoint(0, direction * 120)));
          static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollUpdate, {}, QPoint(0, direction * 120)));
          static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollMomentum, QPoint(0, direction * 9)));
          result &= expect(projectScroll->value() == boundary && threadScroll->value() == panelBefore,
                           "a project retains its gesture and momentum after reaching either boundary");
          static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollEnd, {}));
          static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollBegin, {}));
          // Ownership comes from the gesture origin, even if the pointer moves
          // before the first directional update.
          static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollUpdate, {}, QPoint(0, direction * 120)));
          const int panelAfterHandoff = threadScroll->value();
          static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollMomentum, QPoint(0, direction * 9)));
          static_cast<void>(sendAt(conversation.viewport(), Qt::ScrollEnd, {}));
          result &= expect(panelAfterHandoff != panelBefore && (!projectBody || projectBody->verticalScrollBar()->value() == boundary) &&
                               outer->value() == conversationBeforeProjects,
                           "a new gesture at a project boundary belongs to the threads panel, including momentum");
      }
      threadScroll->setValue(40);
      projectBody = bodyAtTop();
      auto* projectScroll = projectBody->verticalScrollBar();
      const int panelBefore = threadScroll->value();
      projectScroll->setValue(projectScroll->maximum() - 1);
      static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollUpdate, {}, QPoint(0, -120)));
      static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollMomentum, QPoint(0, -9)));
      static_cast<void>(sendAt(projectBody->viewport(), Qt::ScrollEnd, {}));
      result &= expect(projectScroll->value() == projectScroll->maximum() && threadScroll->value() == panelBefore,
                       "a gesture without ScrollBegin still keeps its project owner");
      projectScroll->setValue(projectScroll->maximum() - 1);
      static_cast<void>(sendAt(projectBody->viewport(), Qt::NoScrollPhase, {}, QPoint(0, -120)));
      result &= expect(threadScroll->value() == panelBefore, "a discrete wheel notch is consumed by the project when it can scroll");
      static_cast<void>(sendAt(projectBody->viewport(), Qt::NoScrollPhase, {}, QPoint(0, -120)));
      result &= expect(threadScroll->value() != panelBefore && outer->value() == conversationBeforeProjects,
                       "the next discrete wheel notch at the boundary scrolls only the threads panel");
  }

  if (measurePerformance) {
    // Quantify the central route on the same native viewport against direct
    // Qt delivery. Event construction and filter installation are outside the
    // sample; both paths execute the same ConversationView wheel handler.
    conversation.viewport()->removeEventFilter(&outerProbe);
    constexpr int RoutingSamples = 128;
    std::vector<qint64> directMicros;
    std::vector<qint64> routedMicros;
    directMicros.reserve(RoutingSamples);
    routedMicros.reserve(RoutingSamples);
    bool performanceMovement = true;
    outer->setValue(outer->maximum() / 2);
    for (int sample = 0; sample < RoutingSamples; ++sample) {
      for (int order = 0; order < 2; ++order) {
        const bool routed = (sample + order) % 2 != 0;
        const int delta =
            outer->value() >= outer->maximum() - outer->pageStep() ? 120
            : outer->value() <= outer->minimum() + outer->pageStep()
                ? -120
                : (sample % 2 == 0 ? -120 : 120);
        const QPointF local = wheelPosition(conversation.viewport());
        QWheelEvent wheel(local, conversation.viewport()->mapToGlobal(local),
                          {}, QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false,
                          Qt::MouseEventSynthesizedByApplication, device);
        wheel.ignore();
        const int before = outer->value();
        router.topLevelConsumed = false;
        if (!routed)
          qApp->removeEventFilter(&router);
        QElapsedTimer timer;
        timer.start();
        static_cast<void>(
            QApplication::sendEvent(conversation.viewport(), &wheel));
        (routed ? routedMicros : directMicros)
            .push_back(timer.nsecsElapsed() / 1000);
        if (!routed)
          qApp->installEventFilter(&router);
        performanceMovement = performanceMovement && wheel.isAccepted() &&
                              outer->value() != before &&
                              (!routed || router.topLevelConsumed);
      }
    }
    std::ranges::sort(directMicros);
    std::ranges::sort(routedMicros);
    const auto percentile = [](const std::vector<qint64> &samples, int value) {
      return samples[(samples.size() * value - 1) / 100];
    };
    const qint64 directP95 = percentile(directMicros, 95);
    const qint64 routedP95 = percentile(routedMicros, 95);
    const qint64 routedMaximum = routedMicros.back();
    std::cerr << "Middle-region wheel route: direct p95=" << directP95
              << " us, routed p95/max=" << routedP95 << '/' << routedMaximum
              << " us\n";
    result &= expect(performanceMovement &&
                         codexui::testing::timingLimit(
                             routedP95 <= 2000 && routedMaximum <= 5000 &&
                             routedP95 <= directP95 * 4 + 500),
                     "the registered central wheel route stays within its "
                     "2/5 ms budget and bounded against native dispatch");
  }
  result &= expect(router.maximumDepth == 2 && !router.overflow,
                   "native redispatch re-enters the application filter once "
                   "without recursive rerouting");
  qApp->removeEventFilter(&router);
  return result;
}

bool tokenDetailsRemainVisible() {
  UiStyle::TokenUsageLabel label;
  QPalette dark = label.palette();
  dark.setColor(QPalette::Window, Qt::black);
  dark.setColor(QPalette::WindowText, Qt::white);
  label.setPalette(dark);
  label.setStyleSheet(UiStyle::applicationStyleSheet());
  label.setUsage("Tokens 100 | In 80 | Out 20", "Total: 100\nInput: 80\nOutput: 20");
  label.resize(300, 30);
  label.move(100, 100);
  label.show();
  QCoreApplication::processEvents();
  QHelpEvent help(QEvent::ToolTip, label.rect().center(), label.mapToGlobal(label.rect().center()));
  QApplication::sendEvent(&label, &help);
  auto *details = label.findChild<QLabel *>(QStringLiteral("tokenUsageDetails"));
  bool result = expect(details && details->isVisible(), "token details open on hover");
  if (!details) return false;
  if (qEnvironmentVariableIsSet("CODEXUI_CAPTURE_HEADER"))
    details->grab().save(QStringLiteral("/tmp/codexui-token-tooltip.png"));
  result &= expect(details->palette().color(QPalette::Window) == QColor(UiStyle::panel) &&
                       details->palette().color(QPalette::WindowText) == QColor(UiStyle::primary),
                   "persistent token details use the shared light tooltip colors");
  QCursor::setPos(details->geometry().center());
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&label, &leave);
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 1600) {
    QCoreApplication::processEvents();
    QThread::msleep(10);
  }
  result &= expect(details->isVisible(), "details stay visible beneath the pointer without a timeout");
  QCursor::setPos(label.mapToGlobal(label.rect().center()));
  QApplication::sendEvent(details, &leave);
  result &= expect(details->isVisible(), "moving back to the trigger keeps details open");
  QCursor::setPos(10, 10);
  QApplication::sendEvent(&label, &leave);
  result &= expect(!details->isVisible(), "leaving both surfaces dismisses details");
  QFocusEvent focus(QEvent::FocusIn, Qt::TabFocusReason);
  QApplication::sendEvent(&label, &focus);
  result &= expect(details->isVisible() && label.accessibleName() == "Tokens 100",
                   "keyboard focus opens details with the unchanged token accessibility name");
  label.setUsage("Tokens 200 | In 160 | Out 40", "Total: 200\nInput: 160\nOutput: 40");
  result &= expect(details->isVisible() && details->text().contains("Total: 200") &&
                       label.accessibleName() == "Tokens 200",
                   "visible details and accessibility update with the latest usage");
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&label, &escape);
  result &= expect(!details->isVisible(), "Escape dismisses keyboard-opened details");
  return result;
}

bool turnNavigationUsesTheExistingHeader() {
    MiddleRegionWidget region;
    region.setStyleSheet(UiStyle::applicationStyleSheet());
    region.showSidebar(false);
    region.showInspector(false);
    region.resize(820, 700);
    region.show();
    auto* first = region.findChild<QToolButton*>(QStringLiteral("conversationFirstTurn"));
    auto* previous = region.findChild<QToolButton*>(QStringLiteral("conversationPreviousTurn"));
    auto* outline = region.findChild<QToolButton*>(QStringLiteral("conversationTurnOutline"));
    auto* next = region.findChild<QToolButton*>(QStringLiteral("conversationNextTurn"));
    auto* latest = region.findChild<QToolButton*>(QStringLiteral("conversationLatest"));
    auto* presentation = region.findChild<QToolButton*>(QStringLiteral("conversationReasoningToggle"));
    bool result = expect(first && previous && outline && next && latest && presentation,
                         "the established conversation header owns the navigation controls");
    ConversationSnapshot snapshot;
    snapshot.threadId = "header-navigation";
    snapshot.hasMore = true;
    for (int index = 0; index < 3; ++index) {
        const std::string id = "turn-" + std::to_string(index);
        TurnSection section;
        section.key = section.turnId = id;
        auto prompt = card(CardKind::UserMessage, "prompt-" + id, UserMessageData{"Prompt & " + id});
        prompt.threadId = snapshot.threadId;
        prompt.turnId = id;
        prompt.key = AuthoritativeItemKey{snapshot.threadId, id, prompt.itemId};
        section.rootCardKey = prompt.key;
        section.cards.push_back(prompt);
        auto answer = card(CardKind::AgentMessage,
                           "answer-" + id,
                           AgentMessageData{QString("A long answer\n\n").repeated(25).toStdString(), "final_answer"});
        answer.threadId = snapshot.threadId;
        answer.turnId = id;
        answer.key = AuthoritativeItemKey{snapshot.threadId, id, answer.itemId};
        section.cards.push_back(answer);
        snapshot.sections.push_back(section);
    }
    CommandExecutionData output;
    output.command = "echo navigation";
    output.output = QString("Command output\n").repeated(100).toStdString();
    auto command = card(CardKind::CommandExecution, "command", output);
    command.threadId = snapshot.threadId;
    command.turnId = snapshot.sections.back().turnId;
    command.key = AuthoritativeItemKey{snapshot.threadId, command.turnId, command.itemId};
    snapshot.sections.back().cards.push_back(command);
    auto* commandFolding = region.findChild<QToolButton*>(QStringLiteral("conversationCommandFoldingToggle"));
    if (commandFolding) {
        commandFolding->setChecked(true);
    }
    region.setThreadHeading(QStringLiteral("History navigation fixture"), {}, {}, QStringLiteral("completed"));
    region.setThreadWorkspace(QStringLiteral("/workspace"), QStringLiteral("Test fixture"), {});
    region.conversation().beginThreadSelection(snapshot.threadId);
    static_cast<void>(region.conversation().reconcileStaged(snapshot));
    QElapsedTimer loading;
    loading.start();
    while (region.conversation().structuralStagingActive() && loading.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    result &= expect(outline && outline->isEnabled(), "navigation enables after staged history commits");
    if (first && previous && outline && next && latest && presentation) {
        const QRect header = presentation->geometry();
        result &= expect(!latest->isVisible(), "Latest is absent while following");
        previous->click();
        QCoreApplication::processEvents();
        result &= expect(region.conversation().mode() == ConversationView::Mode::Paused && latest->isVisible(),
                         "the actual Previous control pauses following and reveals Latest");
        result &= expect(presentation->geometry() == header, "Latest retains its slot and never shifts presentation controls");
        for (int width : {820, 460, 340}) {
            region.resize(width, 700);
            QCoreApplication::processEvents();
            int right = 0;
            for (auto* button : {first, previous, outline, next, latest, presentation}) {
                result &= expect(button->geometry().top() == presentation->geometry().top() && button->height() == presentation->height() &&
                                     !button->accessibleName().isEmpty() && button->geometry().left() >= right,
                                 "navigation and presentation share one accessible header row at every width");
                right = button->geometry().right();
            }
        }
        auto* menu = region.findChild<QFrame*>(QStringLiteral("conversationTurnMenu"));
        outline->click();
        QCoreApplication::processEvents();
        auto* list = menu->findChild<QListWidget*>(QStringLiteral("conversationTurnList"));
        result &= expect(menu->isVisible() && list && list->count() == 3 &&
                             menu->findChild<QLabel*>(QStringLiteral("conversationTurnCount"))->text() == "3 loaded" &&
                             menu->findChild<QPushButton*>(QStringLiteral("conversationTurnLoadEarlier"))->isVisible(),
                         "the native outline exposes loaded-only counts and earlier loading above prompts");
        if (list) {
            result &= expect(list->item(2)->text() == "Prompt & turn-2" && list->item(2)->toolTip() == "Prompt & turn-2",
                             "list prompts preserve literal ampersands");
            list->setCurrentRow(2);
            const auto point = list->visualItemRect(list->currentItem()).center();
            for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent click(type,
                                  point,
                                  list->viewport()->mapToGlobal(point),
                                  Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &click);
            }
        }
        QCoreApplication::processEvents();
        result &=
            expect(!menu->isVisible() && region.conversation().navigationTarget() == stableKey(snapshot.sections.back().cards.front().key),
                   "a single outline click reaches its stable prompt destination and dismisses the popup");
        latest->click();
        QCoreApplication::processEvents();
        result &= expect(region.conversation().mode() == ConversationView::Mode::Following && !latest->isVisible(),
                         "the actual Latest control restores following without a toolbar row");
        auto* commandOutput = region.conversation().findChild<CommandOutputView*>(QStringLiteral("commandOutputView"));
        result &= expect(commandOutput && commandOutput->verticalScrollBar()->maximum() > 0,
                         "the actual header fixture includes scrollable command output");
        if (commandOutput) {
            commandOutput->verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
            QCoreApplication::processEvents();
            result &= expect(region.conversation().mode() == ConversationView::Mode::Paused && latest->isVisible(),
                             "command-owned follow pauses immediately update the same Latest control");
            latest->click();
        }
        for (int index = 3; index < 100; ++index) {
            auto section = snapshot.sections.front();
            section.key = section.turnId = "turn-" + std::to_string(index);
            section.cards.resize(1);
            auto& prompt = section.cards.front();
            prompt.turnId = section.turnId;
            prompt.key = AuthoritativeItemKey{snapshot.threadId, section.turnId, "opening"};
            prompt.payload = UserMessageData{"Prompt " + section.turnId};
            section.rootCardKey = prompt.key;
            snapshot.sections.push_back(section);
        }
        snapshot.hasMore = false;
        static_cast<void>(region.conversation().reconcile(snapshot));
        region.conversation().navigateToTurn(stableKey(snapshot.sections[50].cards.front().key));
        QCoreApplication::processEvents();
        const auto available = outline->screen()->availableGeometry();
        class PopupScrollRouter final : public QObject {
        public:
            explicit PopupScrollRouter(MiddleRegionWidget& region)
                : region_(region) {
            }

        protected:
            bool eventFilter(QObject* target, QEvent* event) override {
                return region_.routeScrollEvent(target, event);
            }

        private:
            MiddleRegionWidget& region_;
        } router(region);
        qApp->installEventFilter(&router);
        const auto originalPosition = region.pos();
        for (const auto point : {available.topLeft(), available.bottomRight()}) {
            region.move(point - outline->mapTo(&region, QPoint()));
            outline->click();
            QCoreApplication::processEvents();
            result &= expect(available.contains(menu->geometry()) && menu->height() <= available.height() * 3 / 5,
                             "a hundred-turn popup stays within the work area and its sixty-percent height cap at screen edges");
            if (list) {
                result &= expect(list->count() == 100 && list->verticalScrollBar()->maximum() > 0 && list->hasFocus(),
                                 "the bounded native list owns a scrollbar and keyboard focus");
                const auto reading = region.conversation().navigationTarget();
                const auto readingIndex = region.conversation().conversationModel()->indexForStableKey(reading);
                const int readingPixel = region.conversation().visualRect(readingIndex).top();
                list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());
                const auto local = list->viewport()->rect().center();
                for (int step = 0; step < 100; ++step) {
                    QWheelEvent wheel(local,
                                      list->viewport()->mapToGlobal(local),
                                      {},
                                      QPoint(0, 120),
                                      Qt::NoButton,
                                      Qt::NoModifier,
                                      Qt::NoScrollPhase,
                                      false);
                    QApplication::sendEvent(list->viewport(), &wheel);
                }
                QCoreApplication::processEvents();
                result &= expect(list->verticalScrollBar()->value() == 0 &&
                                     list->viewport()->rect().contains(list->visualItemRect(list->item(0))) &&
                                     region.conversation().navigationTarget() == reading &&
                                     std::abs(region.conversation().visualRect(readingIndex).top() - readingPixel) <= 1,
                                 "wheel scrolling reaches the very first prompt without moving the conversation");
                QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
                QApplication::sendEvent(list, &end);
                QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
                QApplication::sendEvent(list, &home);
                result &= expect(list->currentRow() == 0 && list->visualItemRect(list->item(0)).top() >= 0,
                                 "Home returns to the first selectable prompt after End");
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(list, &enter);
                QCoreApplication::processEvents();
                result &= expect(!menu->isVisible() &&
                                     region.conversation().navigationTarget() == stableKey(snapshot.sections.front().cards.front().key),
                                 "Enter activates the first prompt through the focused bounded list");
            }
            menu->close();
            outline->click();
            QCoreApplication::processEvents();
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(list, &escape);
            QCoreApplication::processEvents();
            result &= expect(!menu->isVisible(), "Escape dismisses the focused outline without activating a prompt");
        }
        qApp->removeEventFilter(&router);
        region.move(originalPosition);
        latest->click();
        first->click();
        QCoreApplication::processEvents();
        result &= expect(region.conversation().navigationTarget() == stableKey(snapshot.sections.front().cards.front().key) &&
                             latest->isVisible(),
                         "the actual First header control lands at the beginning and pauses following");
        if (!qEnvironmentVariable("CODEXUI_TEST_EVIDENCE_DIR").isEmpty()) {
            region.resize(1400, 850);
            region.conversation().navigateToTurn(stableKey(snapshot.sections.front().cards.front().key));
            QCoreApplication::processEvents();
            region.grab().save(qEnvironmentVariable("CODEXUI_TEST_EVIDENCE_DIR") + QStringLiteral("/native-thread-navigation.png"));
            outline->click();
            QCoreApplication::processEvents();
            menu->grab().save(qEnvironmentVariable("CODEXUI_TEST_EVIDENCE_DIR") + QStringLiteral("/native-turn-outline.png"));
            menu->close();
        }
    }
    return result;
}

bool completeMiddleSurfaceRetainsPaneAndHeadingBehavior() {
  MiddleRegionWidget region;
  region.setStyleSheet(UiStyle::applicationStyleSheet());
  region.resize(1500, 850);
  region.show();
  region.setThreadHeading(QString(300, QLatin1Char('T')),
                          {"Tokens 48.2k | In 42.0k | Out 6.2k | Left ≈180k",
                           "Total: 48,200"},
                          QStringLiteral("Last activity: 12:00"),
                          QStringLiteral("running"), QStringLiteral("active"));
  QCoreApplication::processEvents();
  bool result = expect(region.sidebarVisible() && region.inspectorVisible(),
                       "the complete three-pane workspace starts visible");
  result &= expect(region.findChild<QLabel *>(
                       QStringLiteral("conversationTitle")) != nullptr,
                   "the established conversation heading remains present");
  bool wrappedUsage = false;
  bool fullUsage = false;
  for (int width : {1100, 1500, 2000}) {
    region.resize(width, 850);
    QCoreApplication::processEvents();
    int headings = 0;
    for (QLabel *label : region.findChildren<QLabel *>()) {
      if (label->property("kind") != "panelHeader")
        continue;
      ++headings;
      result &= expect(label->isVisible() &&
                           label->width() >= label->fontMetrics().horizontalAdvance(label->text()) &&
                           label->height() >= label->fontMetrics().height(),
                       "all panel headings retain readable geometry");
    }
    result &= expect(headings == 3, "all three panel headings remain present");
    auto *tokens = region.findChild<QLabel *>(QStringLiteral("threadTokenUsage"));
    auto *title = region.findChild<QLabel *>(QStringLiteral("conversationTitle"));
    if (qEnvironmentVariableIsSet("CODEXUI_CAPTURE_HEADER"))
      region.grab().save(QStringLiteral("/tmp/codexui-header-%1.png").arg(width));
    wrappedUsage |= tokens && tokens->text().contains('\n');
    fullUsage |= tokens && !tokens->text().contains('\n');
    result &= expect(tokens && title && title->width() > 0 &&
                         title->x() == title->parentWidget()->layout()->contentsMargins().left() &&
                         title->y() + title->height() <= tokens->y() &&
                         title->text().endsWith(QChar(0x2026)) &&
                         title->toolTip().size() == 300 &&
                         tokens->alignment().testFlag(Qt::AlignRight) &&
                         tokens->property("tone") == "strong" && !tokens->wordWrap() &&
                         tokens->hasHeightForWidth() &&
                         tokens->text().contains("Left ≈180k") &&
                         tokens->text().contains("Last activity: 12:00") &&
                         tokens->height() >= tokens->heightForWidth(tokens->width()) &&
                         tokens->toolTip() == "Total: 48,200" &&
                         tokens->accessibleDescription() == tokens->toolTip() &&
                         tokens->focusPolicy() == Qt::StrongFocus &&
                         !region.findChild<QLabel *>(QStringLiteral("conversationMetadata")),
                     "title has its own row; metadata wraps without losing fields");
    for (const auto &line : tokens->text().split('\n'))
      result &= expect(!line.startsWith('|') && !line.endsWith('|') &&
                           tokens->fontMetrics().horizontalAdvance(line) <= tokens->width(),
                       "metadata lines fit with no stray separators");
  }
  UiStyle::TokenUsageLabel narrow;
  narrow.setUsage("Tokens 48.2k | In 42.0k | Out 6.2k | Left ≈180k | Last activity: 12:00",
                  "Total: 48,200");
  narrow.resize(160, narrow.heightForWidth(160));
  narrow.show();
  QCoreApplication::processEvents();
  wrappedUsage |= narrow.text().contains('\n');
  result &= expect(narrow.text().contains("Last activity: 12:00") &&
                       narrow.text().contains("Left ≈180k"),
                   "narrow metadata preserves every field");
  result &= expect(wrappedUsage && fullUsage,
                   "metadata wraps at narrow widths and stays continuous when space permits");
  region.setThreadHeading(QStringLiteral("Empty thread"),
                          {"Tokens 0 | In 0 | Out 0", "No retained usage"},
                          QStringLiteral("Last activity: 12:00"));
  QCoreApplication::processEvents();
  auto *tokens = region.findChild<QLabel *>(QStringLiteral("threadTokenUsage"));
  result &= expect(tokens->text().contains(" | Last activity: 12:00") &&
                       !tokens->text().contains("Left"),
                   "the separator remains when context information is unavailable");
  auto *fileChangesFolding = region.findChild<QToolButton *>(
      QStringLiteral("conversationFileChangesFoldingToggle"));
  result &= expect(
      fileChangesFolding && fileChangesFolding->isCheckable() &&
          fileChangesFolding->toolTip().startsWith(
              QStringLiteral("New file changes cards start ")) &&
          fileChangesFolding->accessibleName() == fileChangesFolding->toolTip(),
      "the Files Changed folding preference matches established header "
      "control semantics");
  region.showSidebar(false);
  region.showInspector(false);
  QCoreApplication::processEvents();
  result &= expect(!region.sidebarVisible() && !region.inspectorVisible(),
                   "pane visibility remains user-controlled");
  region.showSidebar(true);
  region.showInspector(true);
  QCoreApplication::processEvents();
  result &= expect(region.sidebarVisible() && region.inspectorVisible(),
                   "hidden panes restore without reconstructing the shell");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;
  if (argc == 2 && std::string(argv[1]) == "--wheel-routing-performance")
    return nestedWheelGestureHasOneRoutingOwner(true) ? 0 : 1;
  bool passed = true;
  const auto run = [&passed](const char *name, bool (*test)()) {
    const bool casePassed = test();
    if (!casePassed)
      std::cerr << "CASE FAILED: " << name << '\n';
    passed &= casePassed;
  };
  run("promptKeyboardAndFocusContract", promptKeyboardAndFocusContract);
  run("promptDropCursorContract", promptDropCursorContract);
  run("promptAttachmentAdmission", promptAttachmentAdmission);
  run("promptClipboardAndDropContract", promptClipboardAndDropContract);
  if (argc == 2 && std::string(argv[1]) == "--attachment-input")
    return passed ? 0 : 1;
  run("turnSettingsAreStableAccessibleProjections",
      turnSettingsAreStableAccessibleProjections);
  run("turnSettingsCatalogProjectionScalesLinearly",
      turnSettingsCatalogProjectionScalesLinearly);
  run("controlsRetainUsableGeometryAndAuthority",
      controlsRetainUsableGeometryAndAuthority);
  run("attachmentLayoutFollowsFontAndWidth", attachmentLayoutFollowsFontAndWidth);
  run("threadPaneSnapshotAndActionContract",
      threadPaneSnapshotAndActionContract);
  run("conversationOwnershipAndAtomicReconcileContract",
      conversationOwnershipAndAtomicReconcileContract);
  run("nestedWheelGestureHasOneRoutingOwner",
      [] { return nestedWheelGestureHasOneRoutingOwner(false); });
  run("tokenDetailsRemainVisible", tokenDetailsRemainVisible);
  run("turnNavigationUsesTheExistingHeader", turnNavigationUsesTheExistingHeader);
  run("completeMiddleSurfaceRetainsPaneAndHeadingBehavior",
      completeMiddleSurfaceRetainsPaneAndHeadingBehavior);
  if (passed)
    std::cout << "Established UI/UX compatibility tests passed\n";
  return passed ? 0 : 1;
}
