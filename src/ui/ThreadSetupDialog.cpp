// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/ThreadSetupDialog.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace codexui {
namespace {

QLabel* plainLabel(const QString& value, const char* kind = nullptr)
{
    auto* result = new QLabel(value);
    result->setTextFormat(Qt::PlainText);
    if (kind)
        result->setProperty("kind", kind);
    return result;
}

QLabel* wrappedPlainLabel(const QString& value, const char* kind = nullptr)
{
    auto* result = plainLabel(value, kind);
    result->setWordWrap(true);
    return result;
}

QString dialogTitle(ThreadSetupDialog::Mode mode)
{
    switch (mode) {
        case ThreadSetupDialog::Mode::NewThread:
            return QStringLiteral("New Thread");
        case ThreadSetupDialog::Mode::ForkThread:
            return QStringLiteral("Fork Thread");
        case ThreadSetupDialog::Mode::ResumeWithOptions:
            return QStringLiteral("Resume with options");
    }
    return {};
}

QString dialogDescription(ThreadSetupDialog::Mode mode)
{
    switch (mode) {
        case ThreadSetupDialog::Mode::NewThread:
            return QStringLiteral("Set the thread's foundational context. Execution settings belong to the upcoming turn.");
        case ThreadSetupDialog::Mode::ForkThread:
            return QStringLiteral("Create a new thread from the selected history. Blank instruction fields keep the inherited context.");
        case ThreadSetupDialog::Mode::ResumeWithOptions:
            return QStringLiteral("Resume the selected thread with explicit foundational-instruction overrides.");
    }
    return {};
}

QString submitText(ThreadSetupDialog::Mode mode)
{
    switch (mode) {
        case ThreadSetupDialog::Mode::NewThread:
            return QStringLiteral("Create thread");
        case ThreadSetupDialog::Mode::ForkThread:
            return QStringLiteral("Fork thread");
        case ThreadSetupDialog::Mode::ResumeWithOptions:
            return QStringLiteral("Resume thread");
    }
    return {};
}

FoundationalInstructionsEditor::Context editorContext(ThreadSetupDialog::Mode mode)
{
    switch (mode) {
        case ThreadSetupDialog::Mode::NewThread:
            return FoundationalInstructionsEditor::Context::NewThread;
        case ThreadSetupDialog::Mode::ForkThread:
            return FoundationalInstructionsEditor::Context::ForkThread;
        case ThreadSetupDialog::Mode::ResumeWithOptions:
            return FoundationalInstructionsEditor::Context::ResumeWithOptions;
    }
    return FoundationalInstructionsEditor::Context::NewThread;
}

QString localStyleSheet()
{
    return QStringLiteral(R"QSS(
        QDialog#threadSetupDialog {
            background: #ffffff;
            color: #1d2633;
        }
        QDialog#threadSetupDialog QWidget {
            color: #1d2633;
            font-size: 12px;
        }
        QDialog#threadSetupDialog QLabel {
            background: transparent;
            color: #1d2633;
            font-weight: 400;
        }
        QDialog#threadSetupDialog QLabel[kind="dialogTitle"] {
            color: #1d2633;
            font-size: 20px;
            font-weight: 600;
        }
        QDialog#threadSetupDialog QLabel[kind="description"] {
            color: #667085;
            font-size: 12px;
        }
        QDialog#threadSetupDialog QLabel[kind="fieldLabel"] {
            color: #344054;
            font-size: 11px;
            font-weight: 600;
        }
        QDialog#threadSetupDialog QLabel[kind="fieldHelp"] {
            color: #667085;
            font-size: 10px;
        }
        QDialog#threadSetupDialog QLineEdit,
        QDialog#threadSetupDialog QPlainTextEdit {
            background: #ffffff;
            color: #1d2633;
            border: 2px solid #b9c4d2;
            border-radius: 7px;
            selection-background-color: #dce8ff;
            selection-color: #1d2633;
        }
        QDialog#threadSetupDialog QLineEdit {
            min-height: 36px;
            padding: 0 10px;
        }
        QDialog#threadSetupDialog QPlainTextEdit {
            padding: 9px 10px;
        }
        QDialog#threadSetupDialog QLineEdit:focus,
        QDialog#threadSetupDialog QPlainTextEdit:focus {
            border: 2px solid #2f6feb;
        }
        QDialog#threadSetupDialog QCheckBox {
            color: #1d2633;
            spacing: 9px;
            font-size: 12px;
            font-weight: 600;
        }
        QDialog#threadSetupDialog QFrame#temporaryThreadPanel {
            background: #f8fafc;
            border: 1px solid #d7dee8;
            border-radius: 8px;
        }
        QDialog#threadSetupDialog QFrame#resumeOptionsWarning {
            background: #fff8e8;
            border: 1px solid #eccb86;
            border-radius: 8px;
        }
        QDialog#threadSetupDialog QLabel[kind="warningTitle"] {
            color: #855600;
            font-size: 11px;
            font-weight: 600;
        }
        QDialog#threadSetupDialog QLabel[kind="warningBody"] {
            color: #765a24;
            font-size: 10px;
        }
        QDialog#threadSetupDialog QPushButton,
        QDialog#threadSetupDialog QToolButton {
            min-height: 36px;
            border: 1px solid #b9c4d2;
            border-radius: 7px;
            padding: 0 14px;
            background: #ffffff;
            color: #344054;
            font-size: 11px;
            font-weight: 600;
        }
        QDialog#threadSetupDialog QPushButton:hover,
        QDialog#threadSetupDialog QToolButton:hover {
            background: #f1f5fb;
        }
        QDialog#threadSetupDialog QPushButton[kind="primary"] {
            color: #ffffff;
            background: #2f6feb;
            border-color: #2f6feb;
        }
        QDialog#threadSetupDialog QPushButton[kind="primary"]:hover {
            background: #245fce;
            border-color: #245fce;
        }
        QDialog#threadSetupDialog QToolButton#threadSetupClose {
            min-width: 32px;
            max-width: 32px;
            padding: 0;
            border-color: transparent;
            font-size: 18px;
            font-weight: 400;
        }
    )QSS");
}

} // namespace

FoundationalInstructionsEditor::FoundationalInstructionsEditor(Context context, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("foundationalInstructionsEditor"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    auto* baseLabel = plainLabel(QStringLiteral("Base Instructions"), "fieldLabel");
    root->addWidget(baseLabel);
    baseInstructionsEdit = new QPlainTextEdit;
    baseInstructionsEdit->setObjectName(QStringLiteral("baseInstructionsEdit"));
    baseInstructionsEdit->setAccessibleName(QStringLiteral("Base Instructions"));
    baseInstructionsEdit->setMinimumHeight(102);
    baseInstructionsEdit->setMaximumHeight(132);
    root->addWidget(baseInstructionsEdit);

    auto* baseHelp = wrappedPlainLabel(
        QStringLiteral("Fundamental Codex behavior for this thread. Leave blank to avoid an override."),
        "fieldHelp");
    root->addWidget(baseHelp);
    root->addSpacing(4);

    auto* developerLabel = plainLabel(QStringLiteral("Developer Instructions"), "fieldLabel");
    root->addWidget(developerLabel);
    developerInstructionsEdit = new QPlainTextEdit;
    developerInstructionsEdit->setObjectName(QStringLiteral("developerInstructionsEdit"));
    developerInstructionsEdit->setAccessibleName(QStringLiteral("Developer Instructions"));
    developerInstructionsEdit->setMinimumHeight(102);
    developerInstructionsEdit->setMaximumHeight(132);
    root->addWidget(developerInstructionsEdit);

    setFocusProxy(baseInstructionsEdit);

    auto* developerHelp = wrappedPlainLabel(
        QStringLiteral("Project, workflow, architecture, and testing constraints. Leave blank to avoid an override."),
        "fieldHelp");
    root->addWidget(developerHelp);

    switch (context) {
        case Context::NewThread:
            baseInstructionsEdit->setPlaceholderText(QStringLiteral("Use the Codex default when empty"));
            developerInstructionsEdit->setPlaceholderText(QStringLiteral("Use the Codex default when empty"));
            break;
        case Context::ForkThread:
            baseInstructionsEdit->setPlaceholderText(QStringLiteral("Keep inherited Base Instructions when empty"));
            developerInstructionsEdit->setPlaceholderText(QStringLiteral("Keep inherited Developer Instructions when empty"));
            break;
        case Context::ResumeWithOptions:
            baseInstructionsEdit->setPlaceholderText(QStringLiteral("Keep current Base Instructions when empty"));
            developerInstructionsEdit->setPlaceholderText(QStringLiteral("Keep current Developer Instructions when empty"));
            break;
    }
}

FoundationalInstructionsDraft FoundationalInstructionsEditor::value() const
{
    return {baseInstructionsEdit->toPlainText(), developerInstructionsEdit->toPlainText()};
}

void FoundationalInstructionsEditor::clear()
{
    baseInstructionsEdit->clear();
    developerInstructionsEdit->clear();
}

ThreadSetupDialog::ThreadSetupDialog(Mode mode, QWidget* parent)
    : QDialog(parent)
    , currentMode(mode)
{
    setObjectName(QStringLiteral("threadSetupDialog"));
    setProperty("mode", static_cast<int>(mode));
    setWindowTitle(dialogTitle(mode));
    setWindowModality(Qt::WindowModal);
    setModal(true);
    setSizeGripEnabled(false);
    setMinimumWidth(520);
    resize(664, mode == Mode::ResumeWithOptions ? 590 : 680);
    setStyleSheet(localStyleSheet());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(18);

    auto* header = new QHBoxLayout;
    header->setSpacing(12);
    auto* heading = new QVBoxLayout;
    heading->setSpacing(5);
    auto* title = plainLabel(dialogTitle(mode), "dialogTitle");
    title->setObjectName(QStringLiteral("threadSetupTitle"));
    heading->addWidget(title);
    auto* description = wrappedPlainLabel(dialogDescription(mode), "description");
    description->setObjectName(QStringLiteral("threadSetupSubtitle"));
    heading->addWidget(description);
    header->addLayout(heading, 1);

    auto* closeButton = new QToolButton;
    closeButton->setObjectName(QStringLiteral("threadSetupClose"));
    closeButton->setText(QString(QChar(0x00d7)));
    closeButton->setAccessibleName(QStringLiteral("Close"));
    connect(closeButton, &QToolButton::clicked, this, &QDialog::reject);
    header->addWidget(closeButton, 0, Qt::AlignTop);
    root->addLayout(header);

    auto* bodyScroll = new QScrollArea;
    bodyScroll->setObjectName(QStringLiteral("threadSetupBodyScroll"));
    bodyScroll->setWidgetResizable(true);
    bodyScroll->setFrameShape(QFrame::NoFrame);
    bodyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(18);
    bodyLayout->setSizeConstraint(QLayout::SetMinimumSize);
    bodyScroll->setWidget(body);
    root->addWidget(bodyScroll, 1);

    if (mode == Mode::ResumeWithOptions) {
        auto* warning = new QFrame;
        warning->setObjectName(QStringLiteral("resumeOptionsWarning"));
        auto* warningLayout = new QVBoxLayout(warning);
        warningLayout->setContentsMargins(12, 10, 12, 10);
        warningLayout->setSpacing(3);
        warningLayout->addWidget(plainLabel(QStringLiteral("Expert operation"), "warningTitle"));
        warningLayout->addWidget(wrappedPlainLabel(
            QStringLiteral("This resumes the same historical thread while changing its foundational context. Normal opening resumes automatically without this dialog."),
            "warningBody"));
        bodyLayout->addWidget(warning);
    }

    if (mode != Mode::ResumeWithOptions) {
        auto* nameGroup = new QVBoxLayout;
        nameGroup->setSpacing(7);
        nameGroup->addWidget(plainLabel(QStringLiteral("Thread name (optional)"), "fieldLabel"));
        nameEdit = new QLineEdit;
        nameEdit->setObjectName(QStringLiteral("threadNameEdit"));
        nameEdit->setAccessibleName(QStringLiteral("Thread name"));
        nameEdit->setPlaceholderText(QStringLiteral("Derived from the first turn when empty"));
        nameGroup->addWidget(nameEdit);
        bodyLayout->addLayout(nameGroup);
    }

    instructionsEditor = new FoundationalInstructionsEditor(editorContext(mode));
    bodyLayout->addWidget(instructionsEditor);

    if (mode != Mode::ResumeWithOptions) {
        auto* temporaryPanel = new QFrame;
        temporaryPanel->setObjectName(QStringLiteral("temporaryThreadPanel"));
        auto* temporaryLayout = new QVBoxLayout(temporaryPanel);
        temporaryLayout->setContentsMargins(12, 10, 12, 10);
        temporaryLayout->setSpacing(3);
        temporaryCheckBox = new QCheckBox(QStringLiteral("Temporary thread"));
        temporaryCheckBox->setObjectName(QStringLiteral("temporaryThreadCheckBox"));
        temporaryCheckBox->setAccessibleName(QStringLiteral("Temporary thread"));
        temporaryLayout->addWidget(temporaryCheckBox);
        temporaryLayout->addWidget(wrappedPlainLabel(
            QStringLiteral("Not persisted to normal thread history."), "fieldHelp"));
        bodyLayout->addWidget(temporaryPanel);
    }
    bodyLayout->addStretch();

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addStretch();
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"));
    cancelButton->setObjectName(QStringLiteral("threadSetupCancel"));
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancelButton);
    submitButton = new QPushButton(submitText(mode));
    submitButton->setObjectName(QStringLiteral("threadSetupSubmit"));
    submitButton->setProperty("kind", "primary");
    submitButton->setDefault(true);
    connect(submitButton, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(submitButton);
    root->addLayout(buttons);

    if (nameEdit)
        nameEdit->setFocus(Qt::OtherFocusReason);
    else
        instructionsEditor->setFocus(Qt::OtherFocusReason);
}

ThreadSetupDialog::Mode ThreadSetupDialog::mode() const noexcept
{
    return currentMode;
}

ThreadSetupResult ThreadSetupDialog::result() const
{
    const FoundationalInstructionsDraft instructions = instructionsEditor->value();
    switch (currentMode) {
        case Mode::NewThread:
            return NewThreadSetup{nameEdit->text(), instructions, temporaryCheckBox->isChecked()};
        case Mode::ForkThread:
            return ForkThreadSetup{nameEdit->text(), instructions, temporaryCheckBox->isChecked()};
        case Mode::ResumeWithOptions:
            return ResumeWithOptionsSetup{instructions};
    }
    return ResumeWithOptionsSetup{};
}

void ThreadSetupDialog::setSuggestedThreadName(const QString& name)
{
    if (nameEdit)
        nameEdit->setText(name);
}

void ThreadSetupDialog::setTemporary(bool temporary)
{
    if (temporaryCheckBox)
        temporaryCheckBox->setChecked(temporary);
}

} // namespace codexui
