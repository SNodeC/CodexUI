// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/TurnSettingsWidget.h"

#include "codex/FileSelectionDialog.h"
#include "codex/ui/UiStyle.h"

#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <array>
#include <initializer_list>
#include <utility>

namespace codexui::codex {
namespace {

constexpr int SettingControlHeight = 32;
constexpr int SettingLabelSpacing = 5;
constexpr int TransientChoiceRole = Qt::UserRole + 1;

QString text(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string utf8(const QString &value) {
  const QByteArray encoded = value.toUtf8();
  return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

void addChoice(QComboBox *combo, const QString &label, const QString &value,
               const QString &description = {}) {
  combo->addItem(label, value);
  if (!description.isEmpty())
    combo->setItemData(combo->count() - 1, description, Qt::ToolTipRole);
}

void addChoices(
    QComboBox *combo,
    std::initializer_list<std::pair<const char *, const char *>> choices) {
  for (const auto &[label, value] : choices)
    addChoice(combo, QString::fromLatin1(label), QString::fromLatin1(value));
}

void selectValue(QComboBox *combo, const QString &value,
                 const QString &fallback = {}) {
  const QSignalBlocker blocker(combo);
  for (int candidate = combo->count() - 1; candidate >= 0; --candidate) {
    if (combo->itemData(candidate, TransientChoiceRole).toBool() &&
        combo->itemData(candidate) != value)
      combo->removeItem(candidate);
  }
  int index = combo->findData(value);
  if (index < 0) {
    combo->addItem(
        fallback.isEmpty() ? UiStyle::humanizeLabel(value) : fallback, value);
    index = combo->count() - 1;
    combo->setItemData(index, true, TransientChoiceRole);
  }
  combo->setCurrentIndex(index);
}

QComboBox *compactCombo(const char *name) {
  auto *combo = new UiStyle::ChevronComboBox;
  combo->setObjectName(QString::fromLatin1(name));
  combo->setProperty("codexChevron", true);
  combo->setFixedHeight(SettingControlHeight);
  combo->setMinimumContentsLength(4);
  combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  return combo;
}

QWidget *labelled(const QString &caption, QWidget *control,
                  QWidget *buddy = nullptr) {
  auto *surface = new QFrame;
  auto *layout = new QVBoxLayout(surface);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(SettingLabelSpacing);
  auto *label = new QLabel(caption);
  label->setProperty("kind", "settingLabel");
  const int labelHeight = label->fontMetrics().height();
  label->setFixedHeight(labelHeight);
  label->setBuddy(buddy ? buddy : control);
  (buddy ? buddy : control)->setAccessibleName(caption);
  layout->addWidget(label);
  layout->addWidget(control);
  surface->setFixedHeight(labelHeight + SettingLabelSpacing +
                          SettingControlHeight);
  surface->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  return surface;
}

void addSetting(QGridLayout *layout, const QString &caption, QWidget *control,
                int row, int column, QWidget *buddy = nullptr,
                int columnSpan = 1) {
  layout->addWidget(labelled(caption, control, buddy), row, column, 1,
                    columnSpan);
}

QString permissionProfileLabel(const QString &id) {
  if (id == QStringLiteral(":workspace"))
    return QStringLiteral("Workspace");
  if (id == QStringLiteral(":read-only"))
    return QStringLiteral("Read only");
  if (id == QStringLiteral(":danger-full-access") ||
      id == QStringLiteral(":full-access"))
    return QStringLiteral("Full access");
  return id;
}

} // namespace

TurnSettingsWidget::TurnSettingsWidget(TurnSettingsPolicy &policy,
                                       QWidget *parent)
    : QWidget(parent), settings(policy) {
  setObjectName(QStringLiteral("codexTurnSettings"));
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  auto *root = new QGridLayout(this);
  root->setContentsMargins(10, 8, 10, 8);
  root->setHorizontalSpacing(8);
  root->setVerticalSpacing(8);

  combos = {compactCombo("codexModel"),
            compactCombo("codexEffort"),
            compactCombo("codexPersonality"),
            compactCombo("codexSandbox"),
            compactCombo("codexNetwork"),
            compactCombo("codexApproval"),
            compactCombo("codexReviewer"),
            nullptr,
            compactCombo("codexPermissionProfile"),
            compactCombo("codexServiceTier"),
            compactCombo("codexSummary"),
            compactCombo("codexCollaboration")};
  combo(TurnSettingField::Model)->setEditable(true);
  cwd = new QLineEdit;
  cwd->setObjectName(QStringLiteral("codexWorkspace"));
  cwd->setFixedHeight(SettingControlHeight);
  cwd->setPlaceholderText(QStringLiteral("Thread default workspace"));
  auto *workspacePicker = new QWidget;
  workspacePicker->setFixedHeight(SettingControlHeight);
  auto *workspaceLayout = new QHBoxLayout(workspacePicker);
  workspaceLayout->setContentsMargins(0, 0, 0, 0);
  workspaceLayout->setSpacing(6);
  auto *browseWorkspace = new QToolButton;
  browseWorkspace->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
  browseWorkspace->setToolTip(QStringLiteral("Select workspace"));
  browseWorkspace->setAccessibleName(QStringLiteral("Select workspace"));
  browseWorkspace->setFixedSize(SettingControlHeight, SettingControlHeight);
  workspaceLayout->addWidget(cwd, 1);
  workspaceLayout->addWidget(browseWorkspace);
  more = new UiStyle::ChevronToolButton;
  more->setObjectName(QStringLiteral("codexMoreSettings"));
  more->setText(QStringLiteral("More"));
  more->setProperty("codexChevron", true);
  more->setPopupMode(QToolButton::InstantPopup);
  more->setToolButtonStyle(Qt::ToolButtonTextOnly);
  more->setFixedHeight(SettingControlHeight);

  addSetting(root, QStringLiteral("Model"), combo(TurnSettingField::Model), 0,
             0);
  addSetting(root, QStringLiteral("Reasoning"), combo(TurnSettingField::Effort),
             0, 1);
  addSetting(root, QStringLiteral("Access"), combo(TurnSettingField::Sandbox), 0,
             2);
  addSetting(root, QStringLiteral("Network"), combo(TurnSettingField::Network),
             0, 3);
  addSetting(root, QStringLiteral("Workspace"), workspacePicker, 1, 0, cwd);
  addSetting(root, QStringLiteral("Approval"), combo(TurnSettingField::Approval),
             1, 1);
  addSetting(root, QStringLiteral("Style"), combo(TurnSettingField::Personality),
             1, 2);
  addSetting(root, QStringLiteral("Additional"), more, 1, 3);
  for (int column = 0; column < 4; ++column)
    root->setColumnStretch(column, 1);

  auto *moreMenu = new QMenu(this);
  auto *moreContents = new QWidget;
  moreContents->setMinimumWidth(470);
  auto *moreLayout = new QGridLayout(moreContents);
  moreLayout->setContentsMargins(14, 12, 14, 12);
  moreLayout->setHorizontalSpacing(8);
  moreLayout->setVerticalSpacing(8);
  addSetting(moreLayout, QStringLiteral("Permission profile"),
             combo(TurnSettingField::PermissionProfile), 0, 0);
  addSetting(moreLayout, QStringLiteral("Approval reviewer"),
             combo(TurnSettingField::Reviewer), 0, 1);
  addSetting(moreLayout, QStringLiteral("Service tier"),
             combo(TurnSettingField::ServiceTier), 1, 0);
  addSetting(moreLayout, QStringLiteral("Reasoning summary"),
             combo(TurnSettingField::Summary), 1, 1);
  addSetting(moreLayout, QStringLiteral("Collaboration mode"),
             combo(TurnSettingField::Collaboration), 2, 0, nullptr, 2);
  auto *moreAction = new QWidgetAction(moreMenu);
  moreAction->setDefaultWidget(moreContents);
  moreMenu->addAction(moreAction);
  more->setMenu(moreMenu);

  addChoices(combo(TurnSettingField::Sandbox),
             {{"Thread default", DefaultTurnSetting},
              {"Workspace", "workspace-write"},
              {"Read only", "read-only"},
              {"Full access", "danger-full-access"},
              {"External", "external"}});
  addChoices(combo(TurnSettingField::Network),
             {{"Thread default", DefaultTurnSetting},
              {"Restricted", "restricted"},
              {"Enabled", "enabled"}});
  addChoices(combo(TurnSettingField::Approval),
             {{"Thread default", DefaultTurnSetting},
              {"On request", "on-request"},
              {"Untrusted", "untrusted"},
              {"Never", "never"}});
  addChoices(combo(TurnSettingField::Personality),
             {{"Thread default", DefaultTurnSetting},
              {"None", "none"},
              {"Friendly", "friendly"},
              {"Pragmatic", "pragmatic"}});
  addChoices(combo(TurnSettingField::Reviewer),
             {{"Thread default", DefaultTurnSetting},
              {"User", "user"},
              {"Auto review", "auto_review"},
              {"Guardian", "guardian_subagent"}});
  addChoices(combo(TurnSettingField::Summary),
             {{"Thread default", DefaultTurnSetting},
              {"Auto", "auto"},
              {"Concise", "concise"},
              {"Detailed", "detailed"},
              {"None", "none"}});
  addChoices(combo(TurnSettingField::Collaboration),
             {{"Code", DefaultTurnSetting}, {"Plan", "plan"}});

  for (std::size_t index = 0; index < combos.size(); ++index) {
    if (!combos[index])
      continue;
    connect(combos[index], &QComboBox::currentIndexChanged, this,
            [this, field = static_cast<TurnSettingField>(index)] {
              markTouched(field);
            });
  }
  connect(combo(TurnSettingField::Model)->lineEdit(), &QLineEdit::textEdited,
          this, [this] { markTouched(TurnSettingField::Model); });
  connect(cwd, &QLineEdit::textEdited, this,
          [this] { markTouched(TurnSettingField::Workspace); });
  connect(browseWorkspace, &QToolButton::clicked, this, [this] {
    const QString initialDirectory =
        cwd->text().trimmed().isEmpty()
            ? QDir::homePath()
            : QDir::fromNativeSeparators(cwd->text().trimmed());
    FileSelectionDialog dialog(FileSelectionDialog::Mode::Workspace,
                               initialDirectory, {}, this);
    if (dialog.exec() == QDialog::Accepted) {
      cwd->setText(QDir::toNativeSeparators(dialog.selectedDirectory()));
      settings.change(TurnSettingField::Workspace, utf8(cwd->text()));
      refreshMoreIndicator();
    }
  });

  refreshModels();
  refreshPermissionProfiles();
  render();
}

void TurnSettingsWidget::setContext(TurnSettingsContext context) {
  const bool modelsChanged = settings.context().models != context.models;
  const bool profilesChanged =
      settings.context().permissionProfiles != context.permissionProfiles;
  const bool valuesChanged = settings.setContext(std::move(context));
  if (!valuesChanged && !modelsChanged && !profilesChanged)
    return;
  if (modelsChanged)
    refreshModels();
  if (profilesChanged)
    refreshPermissionProfiles();
  render();
}

void TurnSettingsWidget::markTouched(TurnSettingField field) {
  QComboBox *control = combo(field);
  QString selected;
  if (field == TurnSettingField::Workspace)
    selected = cwd->text();
  else if (control->isEditable() &&
           (control->currentIndex() < 0 ||
            control->currentText() !=
                control->itemText(control->currentIndex())))
    selected = control->currentText().trimmed();
  else
    selected = control->currentData().toString();
  settings.change(field, utf8(selected));
  if (field == TurnSettingField::Workspace)
    refreshMoreIndicator();
  else
    render();
}

void TurnSettingsWidget::render() {
  const TurnSettingValues &values = settings.values();
  for (const TurnSettingField field :
       {TurnSettingField::Model, TurnSettingField::Sandbox,
        TurnSettingField::Network, TurnSettingField::Approval,
        TurnSettingField::Reviewer, TurnSettingField::Summary,
        TurnSettingField::Collaboration}) {
    const QString value = text(values[field]);
    selectValue(combo(field), value,
                value == DefaultTurnSetting ? QStringLiteral("Thread default")
                                            : QString{});
  }
  const QString profile = text(values[TurnSettingField::PermissionProfile]);
  selectValue(combo(TurnSettingField::PermissionProfile), profile,
              profile == DefaultTurnSetting ? QStringLiteral("Thread default")
                                            : permissionProfileLabel(profile));
  cwd->setText(
      QDir::toNativeSeparators(text(values[TurnSettingField::Workspace])));
  refreshModelOptions();
  refreshAccessCompatibility();
  refreshMoreIndicator();
}

void TurnSettingsWidget::refreshModels() {
  QComboBox *model = combo(TurnSettingField::Model);
  const QSignalBlocker blocker(model);
  model->clear();
  addChoice(model, QStringLiteral("Thread default"), DefaultTurnSetting);
  for (const TurnSettingModel &entry : settings.context().models)
    addChoice(model, text(entry.choice.label), text(entry.choice.value),
              text(entry.choice.description));
}

void TurnSettingsWidget::refreshModelOptions() {
  const TurnSettingValues &values = settings.values();
  const TurnSettingModel *definition = settings.selectedModel();

  QComboBox *effort = combo(TurnSettingField::Effort);
  const QSignalBlocker effortBlocker(effort);
  effort->clear();
  QString defaultEffort = QStringLiteral("Thread default");
  if (definition && !definition->defaultReasoningEffort.empty())
    defaultEffort =
        UiStyle::humanizeLabel(text(definition->defaultReasoningEffort)) +
        QStringLiteral(" - default");
  addChoice(effort, defaultEffort, DefaultTurnSetting);
  if (definition && !definition->reasoningEfforts.empty()) {
    for (const std::string &entry : definition->reasoningEfforts)
      addChoice(effort, UiStyle::humanizeLabel(text(entry)), text(entry));
  }
  selectValue(effort, text(values[TurnSettingField::Effort]));

  QComboBox *serviceTier = combo(TurnSettingField::ServiceTier);
  const QSignalBlocker tierBlocker(serviceTier);
  serviceTier->clear();
  QString defaultTier = QStringLiteral("Thread default");
  if (definition && !definition->defaultServiceTier.empty())
    defaultTier +=
        QStringLiteral(" (%1)").arg(text(definition->defaultServiceTier));
  addChoice(serviceTier, defaultTier, DefaultTurnSetting);
  if (definition) {
    for (const TurnSettingChoice &entry : definition->serviceTiers)
      addChoice(serviceTier,
                entry.label.empty() ? UiStyle::humanizeLabel(text(entry.value))
                                    : text(entry.label),
                text(entry.value), text(entry.description));
  }
  const QString selectedTier = text(values[TurnSettingField::ServiceTier]);
  selectValue(serviceTier, selectedTier);

  QComboBox *personality = combo(TurnSettingField::Personality);
  const bool personalitySupported = settings.personalityEnabled();
  personality->setEnabled(personalitySupported);
  personality->setToolTip(
      personalitySupported
          ? QString{}
          : QStringLiteral(
                "The selected model does not support style choices"));
  selectValue(personality,
              text(settings.values()[TurnSettingField::Personality]));
}

void TurnSettingsWidget::refreshPermissionProfiles() {
  QComboBox *permissionProfile = combo(TurnSettingField::PermissionProfile);
  const QSignalBlocker blocker(permissionProfile);
  permissionProfile->clear();
  addChoice(permissionProfile, QStringLiteral("Thread default"),
            DefaultTurnSetting);
  for (const TurnSettingChoice &entry : settings.context().permissionProfiles) {
    const QString id = text(entry.value);
    addChoice(permissionProfile,
              entry.label.empty() ? permissionProfileLabel(id)
                                  : text(entry.label),
              id, text(entry.description));
  }
}

void TurnSettingsWidget::refreshAccessCompatibility() {
  QComboBox *network = combo(TurnSettingField::Network);
  const std::string &access = settings.values()[TurnSettingField::Sandbox];
  network->setEnabled(settings.networkEnabled());
  network->setToolTip(
      access == "danger-full-access"
          ? QStringLiteral("Full access already includes network access")
      : access == DefaultTurnSetting
          ? QStringLiteral("Select an access mode before network access")
          : QString{});
}

void TurnSettingsWidget::refreshMoreIndicator() {
  const bool changed = settings.touched(TurnSettingField::PermissionProfile) ||
                       settings.touched(TurnSettingField::Reviewer) ||
                       settings.touched(TurnSettingField::ServiceTier) ||
                       settings.touched(TurnSettingField::Summary) ||
                       settings.touched(TurnSettingField::Collaboration);
  const QString caption =
      changed ? QStringLiteral("More •") : QStringLiteral("More");
  if (more->text() != caption)
    more->setText(caption);
}

} // namespace codexui::codex
