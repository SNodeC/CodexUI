// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_WORKSPACEDIALOG_H
#define CODEXUI_CODEX_WORKSPACEDIALOG_H

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;

namespace codexui::codex {
    class WorkspaceDialog final : public QDialog {
    public:
        explicit WorkspaceDialog(QString initialDirectory,
                                 QWidget* parent = nullptr,
                                 QString targetTitle = {},
                                 QString confirmedWorkspace = {});
        [[nodiscard]] QString selectedDirectory() const;

    private:
        void updateActions();
        void acceptSelection();

        QLineEdit* location = nullptr;
        QPushButton* acceptButton = nullptr;
    };
} // namespace codexui::codex

#endif // CODEXUI_CODEX_WORKSPACEDIALOG_H
