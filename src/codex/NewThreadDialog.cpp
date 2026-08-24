// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/NewThreadDialog.h"

#include "codex/FileSelectionDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace codexui::codex {
namespace {

QLabel *label(QString text, const char *kind = "body") {
  auto *value = new QLabel(std::move(text));
  value->setProperty("kind", kind);
  value->setWordWrap(true);
  return value;
}

QWidget *field(QString caption, QWidget *control) {
  auto *widget = new QWidget;
  auto *layout = new QVBoxLayout(widget);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(6);
  auto *captionLabel = label(std::move(caption), "title");
  captionLabel->setBuddy(control);
  layout->addWidget(captionLabel);
  layout->addWidget(control);
  return widget;
}

} // namespace

NewThreadDialog::NewThreadDialog(QString initialWorkspace, QWidget *parent)
    : QDialog(parent) {
  setModal(true);
  setWindowTitle(QStringLiteral("New thread"));
  resize(680, 620);
  setMinimumSize(540, 480);

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(24, 22, 24, 20);
  root->setSpacing(14);
  root->addWidget(label(QStringLiteral("New thread"), "heading"));
  root->addWidget(
      label(QStringLiteral("Set the thread context. Model, access, reasoning, "
                           "and style remain in the upcoming-turn controls."),
            "muted"));

  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *content = new QWidget;
  auto *form = new QVBoxLayout(content);
  form->setContentsMargins(0, 2, 8, 2);
  form->setSpacing(16);

  workspace = new QLineEdit(std::move(initialWorkspace));
  workspace->setPlaceholderText(QDir::homePath());
  auto *workspaceRow = new QWidget;
  auto *workspaceLayout = new QHBoxLayout(workspaceRow);
  workspaceLayout->setContentsMargins(0, 0, 0, 0);
  workspaceLayout->setSpacing(8);
  auto *browse = new QPushButton(QStringLiteral("Browse"));
  browse->setFixedHeight(34);
  workspaceLayout->addWidget(workspace, 1);
  workspaceLayout->addWidget(browse);
  form->addWidget(field(QStringLiteral("Workspace"), workspaceRow));

  name = new QLineEdit;
  name->setPlaceholderText(QStringLiteral("Optional thread name"));
  form->addWidget(field(QStringLiteral("Name"), name));

  baseInstructions = new QPlainTextEdit;
  baseInstructions->setPlaceholderText(
      QStringLiteral("Optional base instructions"));
  baseInstructions->setMaximumHeight(110);
  baseInstructions->setProperty("kind", "dialogEditor");
  form->addWidget(field(QStringLiteral("Base instructions"), baseInstructions));

  developerInstructions = new QPlainTextEdit;
  developerInstructions->setPlaceholderText(
      QStringLiteral("Optional developer instructions"));
  developerInstructions->setMaximumHeight(110);
  developerInstructions->setProperty("kind", "dialogEditor");
  form->addWidget(
      field(QStringLiteral("Developer instructions"), developerInstructions));

  auto *ephemeralSurface = new QFrame;
  ephemeralSurface->setProperty("kind", "summary");
  auto *ephemeralLayout = new QVBoxLayout(ephemeralSurface);
  ephemeralLayout->setContentsMargins(12, 10, 12, 10);
  ephemeral = new QCheckBox(QStringLiteral("Temporary thread"));
  ephemeralLayout->addWidget(ephemeral);
  ephemeralLayout->addWidget(
      label(QStringLiteral(
                "Temporary threads are not retained in normal Codex history."),
            "meta"));
  form->addWidget(ephemeralSurface);
  form->addStretch();
  scroll->setWidget(content);
  root->addWidget(scroll, 1);

  errorLabel = label({}, "meta");
  errorLabel->setStyleSheet(QStringLiteral("color:#b83a3a;"));
  errorLabel->hide();
  root->addWidget(errorLabel);

  auto *footer = new QHBoxLayout;
  footer->addStretch();
  auto *cancel = new QPushButton(QStringLiteral("Cancel"));
  cancel->setProperty("kind", "subtle");
  cancel->setFixedHeight(34);
  auto *create = new QPushButton(QStringLiteral("Continue"));
  create->setProperty("kind", "primary");
  create->setFixedHeight(34);
  footer->addWidget(cancel);
  footer->addWidget(create);
  root->addLayout(footer);

  connect(browse, &QPushButton::clicked, this, [this] { chooseWorkspace(); });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(create, &QPushButton::clicked, this, [this] { acceptDraft(); });
}

NewThreadDraft NewThreadDialog::draft() const {
  return {QDir::fromNativeSeparators(workspace->text().trimmed()),
          name->text().trimmed(), baseInstructions->toPlainText().trimmed(),
          developerInstructions->toPlainText().trimmed(),
          ephemeral->isChecked()};
}

void NewThreadDialog::chooseWorkspace() {
  FileSelectionDialog dialog(FileSelectionDialog::Mode::Workspace,
                             draft().workspace, {}, this);
  if (dialog.exec() == QDialog::Accepted)
    workspace->setText(QDir::toNativeSeparators(dialog.selectedDirectory()));
}

void NewThreadDialog::acceptDraft() {
  const QFileInfo selectedWorkspace(draft().workspace);
  if (!selectedWorkspace.exists() || !selectedWorkspace.isDir()) {
    errorLabel->setText(
        QStringLiteral("Select an existing workspace directory."));
    errorLabel->show();
    return;
  }
  accept();
}

} // namespace codexui::codex
