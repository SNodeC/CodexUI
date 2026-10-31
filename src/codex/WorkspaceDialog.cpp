// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/WorkspaceDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace codexui::codex {
    namespace {
        QLabel* dialogLabel(QString text, const char* kind) {
            auto* label = new QLabel(std::move(text));
            label->setProperty("kind", kind);
            label->setWordWrap(true);
            return label;
        }
    } // namespace

    WorkspaceDialog::WorkspaceDialog(QString initialDirectory, QWidget* parent, QString targetTitle, QString confirmedWorkspace)
        : QDialog(parent) {
        setModal(true);
        const QString heading = targetTitle.isEmpty() ? QStringLiteral("Select workspace") : QStringLiteral("Change workspace…");
        setWindowTitle(heading);
        setMinimumWidth(300);
        resize(560, 240);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(24, 22, 24, 20);
        root->setSpacing(12);
        root->addWidget(dialogLabel(heading, "heading"));
        if (!targetTitle.isEmpty()) {
            auto* target = dialogLabel(targetTitle, "body");
            target->setTextFormat(Qt::PlainText);
            target->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            target->setAccessibleName(QStringLiteral("Target thread: ") + targetTitle);
            root->addWidget(target);
            auto* current = new QLineEdit(confirmedWorkspace.isEmpty() ? QStringLiteral("Workspace not reported") : confirmedWorkspace);
            current->setReadOnly(true);
            current->setAccessibleName(QStringLiteral("Current workspace"));
            current->setToolTip(confirmedWorkspace);
            root->addWidget(current);
        }
        root->addWidget(dialogLabel(QStringLiteral("Enter a directory in the app-server filesystem."), "muted"));
        location = new QLineEdit(std::move(initialDirectory));
        location->setObjectName(QStringLiteral("codexWorkspaceDirectory"));
        location->setAccessibleName(QStringLiteral("Proposed workspace"));
        location->setPlaceholderText(QStringLiteral("App-server directory path"));
        root->addWidget(location);
        auto* footer = new QHBoxLayout;
        footer->addStretch();
        auto* cancel = new QPushButton(QStringLiteral("Cancel"));
        acceptButton = new QPushButton(targetTitle.isEmpty() ? QStringLiteral("Select workspace") : QStringLiteral("Change workspace"));
        acceptButton->setProperty("kind", "primary");
        footer->addWidget(cancel);
        footer->addWidget(acceptButton);
        root->addLayout(footer);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(acceptButton, &QPushButton::clicked, this, [this] {
            acceptSelection();
        });
        connect(location, &QLineEdit::textChanged, this, [this] {
            updateActions();
        });
        connect(location, &QLineEdit::returnPressed, this, [this] {
            acceptSelection();
        });
        updateActions();
        location->setFocus();
        location->selectAll();
    }

    QString WorkspaceDialog::selectedDirectory() const {
        return location->text().trimmed();
    }

    void WorkspaceDialog::updateActions() {
        acceptButton->setEnabled(!selectedDirectory().isEmpty());
    }

    void WorkspaceDialog::acceptSelection() {
        if (!selectedDirectory().isEmpty()) {
            accept();
        }
    }

} // namespace codexui::codex
