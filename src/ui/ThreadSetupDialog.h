// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_THREADSETUPDIALOG_H
#define CODEXUI_UI_THREADSETUPDIALOG_H

#include <QDialog>
#include <QString>
#include <QWidget>

#include <variant>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace codexui {

struct FoundationalInstructionsDraft {
    QString baseInstructions;
    QString developerInstructions;

    bool operator==(const FoundationalInstructionsDraft&) const = default;
};

struct NewThreadSetup {
    QString name;
    FoundationalInstructionsDraft instructions;
    bool temporary = false;

    bool operator==(const NewThreadSetup&) const = default;
};

struct ForkThreadSetup {
    QString name;
    FoundationalInstructionsDraft instructions;
    bool temporary = false;

    bool operator==(const ForkThreadSetup&) const = default;
};

struct ResumeWithOptionsSetup {
    FoundationalInstructionsDraft instructions;

    bool operator==(const ResumeWithOptionsSetup&) const = default;
};

using ThreadSetupResult = std::variant<NewThreadSetup, ForkThreadSetup, ResumeWithOptionsSetup>;

class FoundationalInstructionsEditor final : public QWidget
{
public:
    enum class Context { NewThread, ForkThread, ResumeWithOptions };

    explicit FoundationalInstructionsEditor(Context context, QWidget* parent = nullptr);

    [[nodiscard]] FoundationalInstructionsDraft value() const;
    void clear();

private:
    QPlainTextEdit* baseInstructionsEdit = nullptr;
    QPlainTextEdit* developerInstructionsEdit = nullptr;
};

class ThreadSetupDialog final : public QDialog
{
public:
    enum class Mode { NewThread, ForkThread, ResumeWithOptions };

    explicit ThreadSetupDialog(Mode mode, QWidget* parent = nullptr);

    [[nodiscard]] Mode mode() const noexcept;
    [[nodiscard]] ThreadSetupResult result() const;

    void setSuggestedThreadName(const QString& name);
    void setTemporary(bool temporary);

private:
    Mode currentMode;
    QLineEdit* nameEdit = nullptr;
    FoundationalInstructionsEditor* instructionsEditor = nullptr;
    QCheckBox* temporaryCheckBox = nullptr;
    QPushButton* submitButton = nullptr;
};

} // namespace codexui

#endif // CODEXUI_UI_THREADSETUPDIALOG_H
