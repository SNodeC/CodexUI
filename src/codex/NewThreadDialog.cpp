// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/NewThreadDialog.h"

#include "codex/ui/UiStyle.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

namespace codexui::codex {
namespace {

QLabel *label(QString text, const char *kind = "body") {
  auto *value = new QLabel(std::move(text));
  value->setProperty("kind", kind);
  value->setWordWrap(true);
  return value;
}

QWidget *field(QString caption, QWidget *control, QWidget *buddy = nullptr) {
  auto *widget = new QWidget;
  auto *layout = new QVBoxLayout(widget);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(6);
  auto *captionLabel = label(std::move(caption), "title");
  captionLabel->setBuddy(buddy ? buddy : control);
  captionLabel->buddy()->setAccessibleName(captionLabel->text());
  layout->addWidget(captionLabel);
  layout->addWidget(control);
  return widget;
}

} // namespace

NewThreadDialog::NewThreadDialog(QString initialWorkspace, QWidget *parent)
    : NewThreadDialog(
          NewThreadDraft{std::move(initialWorkspace), {}, {}, {}, false},
          Purpose::Create, parent) {}

NewThreadDialog::NewThreadDialog(
    NewThreadDraft initialDraft, Purpose purpose, QWidget* parent, QStringList workspaceChoices, FileSelectionDialog::Access filesystem)
    : UiStyle::ScrollFormDialog(parent) {
    setModal(true);
    const bool forFork = purpose == Purpose::Fork;
    const bool forResume = purpose == Purpose::Resume || purpose == Purpose::ChangeInstructions;
    const QString heading = purpose == Purpose::ChangeInstructions ? QStringLiteral("Change developer instructions")
                            : forResume                            ? QStringLiteral("Resume with options")
                            : forFork                              ? QStringLiteral("Fork with options")
                                                                   : QStringLiteral("New thread");
    setWindowTitle(heading);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 20);
    root->setSpacing(14);
    root->addWidget(label(heading, "heading"));
    root->addWidget(label(forResume ? QStringLiteral("Current instruction text is unavailable. Select Replace to supply new text; an empty "
                                                     "replacement clears it. Unchecked instructions are unchanged.")
                          : forFork ? QStringLiteral("Adjust the copied thread context. Model, access, "
                                                     "reasoning, and style remain in the upcoming-turn "
                                                     "controls.")
                                    : QStringLiteral("Set the thread context. Model, access, reasoning, and "
                                                     "style remain in the upcoming-turn controls."),
                          "muted"));

    auto* scroll = formScrollArea();
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* content = new QWidget;
    auto* form = new QVBoxLayout(content);
    form->setContentsMargins(0, 2, 8, 2);
    form->setSpacing(16);

    workspace = new QLineEdit(std::move(initialDraft.workspace));
    workspace->setPlaceholderText(QStringLiteral("App-server directory path"));
    auto* workspaceRow = new QWidget;
    auto* workspaceLayout = new QHBoxLayout(workspaceRow);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    auto* browse = new QPushButton("Browse app-server…");
    browse->setAccessibleName("Choose app-server workspace");
    workspaceLayout->addWidget(workspace, 1);
    workspaceLayout->addWidget(browse);
    form->addWidget(field("Workspace", workspaceRow, workspace));
    connect(browse, &QPushButton::clicked, this, [this, filesystem, workspaceChoices] {
        FileSelectionDialog selection(
            workspace->text(), this, {}, workspaceChoices, filesystem ? filesystem() : FileSelectionDialog::Filesystem{});
        if (selection.exec() == QDialog::Accepted) {
            workspace->setText(selection.selectedDirectory());
        }
    });

    name = new QLineEdit;
    name->setPlaceholderText(QStringLiteral("Optional thread name"));
    if (!initialDraft.ephemeral) {
        name->setText(std::move(initialDraft.name));
    }
    name->setEnabled(!initialDraft.ephemeral);
    auto* nameField = field(QStringLiteral("Name"), name);
    form->addWidget(nameField);
    nameField->setVisible(!forResume);

    baseInstructions = new UiStyle::DialogTextEdit;
    baseInstructions->setPlaceholderText(QStringLiteral("Optional base instructions"));
    baseInstructions->setMaximumHeight(110);
    baseInstructions->setProperty("kind", "dialogEditor");
    baseInstructions->setPlainText(std::move(initialDraft.baseInstructions));
    form->addWidget(field(QStringLiteral("Base instructions"), baseInstructions));

    developerInstructions = new UiStyle::DialogTextEdit;
    developerInstructions->setPlaceholderText(QStringLiteral("Optional developer instructions"));
    developerInstructions->setMaximumHeight(110);
    developerInstructions->setProperty("kind", "dialogEditor");
    developerInstructions->setPlainText(std::move(initialDraft.developerInstructions));
    form->addWidget(field(QStringLiteral("Developer instructions"), developerInstructions));
    if (forResume) {
        replaceBase = new QCheckBox(QStringLiteral("Replace base instructions"));
        replaceDeveloper = new QCheckBox(QStringLiteral("Replace developer instructions"));
        for (const auto& [check, editor] : {std::pair{replaceBase, baseInstructions}, {replaceDeveloper, developerInstructions}}) {
            editor->clear();
            editor->setEnabled(false);
            form->insertWidget(form->indexOf(editor->parentWidget()), check);
            connect(check, &QCheckBox::toggled, editor, &QWidget::setEnabled);
        }
        baseInstructions->setPlainText(initialDraft.resumeBaseInstructions.value_or(QString{}));
        developerInstructions->setPlainText(initialDraft.resumeDeveloperInstructions.value_or(QString{}));
        replaceBase->setChecked(initialDraft.resumeBaseInstructions.has_value());
        replaceDeveloper->setChecked(initialDraft.resumeDeveloperInstructions.has_value() || purpose == Purpose::ChangeInstructions);
    }

    auto* ephemeralSurface = new QFrame;
    ephemeralSurface->setProperty("kind", "summary");
    auto* ephemeralLayout = new QVBoxLayout(ephemeralSurface);
    ephemeralLayout->setContentsMargins(12, 10, 12, 10);
    ephemeral = new QCheckBox(QStringLiteral("Temporary thread"));
    ephemeral->setChecked(initialDraft.ephemeral);
    ephemeralLayout->addWidget(ephemeral);
    ephemeralLayout->addWidget(label(QStringLiteral("Temporary threads are not retained in normal Codex history."), "meta"));
    form->addWidget(ephemeralSurface);
    ephemeralSurface->setVisible(!forResume);
    form->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
    for (auto* editor : {baseInstructions, developerInstructions})
        connect(editor, &QPlainTextEdit::cursorPositionChanged, this, [this, editor] {
            revealFormFocus(editor);
        });

    errorLabel = label({}, "meta");
    errorLabel->setProperty("tone", "danger");
    errorLabel->hide();
    root->addWidget(errorLabel);

    auto* footer = new QHBoxLayout;
    footer->addStretch();
    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    cancel->setProperty("kind", "cancel");
    cancel->setMinimumHeight(34);
    auto* create = new QPushButton(QStringLiteral("Continue"));
    create->setProperty("kind", "primary");
    create->setMinimumHeight(34);
    footer->addWidget(cancel);
    footer->addWidget(create);
    root->addLayout(footer);

    connect(ephemeral, &QCheckBox::toggled, this, [this](bool checked) {
        name->setEnabled(!checked);
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(create, &QPushButton::clicked, this, [this] {
        acceptDraft();
    });

    QScreen* targetScreen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QRect available = targetScreen ? targetScreen->availableGeometry() : QRect(0, 0, 1280, 800);
    constexpr int ScreenMargin = 64;
    const int maximumWidth = std::max(minimumWidth(), available.width() - ScreenMargin);
    const int targetWidth = std::clamp(680, minimumWidth(), maximumWidth);
    resize(targetWidth, minimumHeight());
    root->activate();
    form->activate();
    const int desiredScrollHeight = content->sizeHint().height() + 2 * scroll->frameWidth();
    const int desiredHeight = root->sizeHint().height() - scroll->sizeHint().height() + desiredScrollHeight;
    const int maximumHeight = std::max(minimumHeight(), available.height() - ScreenMargin);
    resize(targetWidth, std::clamp(desiredHeight, minimumHeight(), maximumHeight));
}

NewThreadDraft NewThreadDialog::draft() const {
    NewThreadDraft result{workspace->text().trimmed(),
                          ephemeral->isChecked() ? QString{} : name->text().trimmed(),
                          baseInstructions->toPlainText().trimmed(),
                          developerInstructions->toPlainText().trimmed(),
                          ephemeral->isChecked()};
    if (replaceBase && replaceBase->isChecked()) {
        result.resumeBaseInstructions = baseInstructions->toPlainText();
    }
    if (replaceDeveloper && replaceDeveloper->isChecked()) {
        result.resumeDeveloperInstructions = developerInstructions->toPlainText();
    }
    return result;
}

void NewThreadDialog::acceptDraft() {
    if (draft().workspace.isEmpty()) {
        errorLabel->setText(QStringLiteral("Enter the app-server workspace directory."));
        errorLabel->show();
    } else {
        accept();
    }
}

} // namespace codexui::codex
