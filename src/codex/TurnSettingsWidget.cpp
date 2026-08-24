// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/TurnSettingsWidget.h"

#include "codex/FileSelectionDialog.h"

#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace codexui::codex {
namespace {

constexpr auto DefaultValue = "default";
constexpr int SettingControlHeight = 32;
constexpr int SettingLabelSpacing = 5;

void drawChevron(QWidget *widget, const QRect &indicator, bool enabled,
                 bool highlighted) {
  if (!indicator.isValid() || indicator.isEmpty())
    return;
  const QPointF center = indicator.center();
  QPainterPath chevron;
  chevron.moveTo(center.x() - 3.5, center.y() - 1.5);
  chevron.lineTo(center.x(), center.y() + 2.0);
  chevron.lineTo(center.x() + 3.5, center.y() - 1.5);

  QColor color(QStringLiteral("#667085"));
  if (!enabled)
    color = QColor(QStringLiteral("#98a2b3"));
  else if (highlighted)
    color = QColor(QStringLiteral("#1d2633"));

  QPainter painter(widget);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(chevron);
}

class CompactComboBox final : public QComboBox {
protected:
  void paintEvent(QPaintEvent *event) override {
    QComboBox::paintEvent(event);

    QStyleOptionComboBox option;
    initStyleOption(&option);
    const QRect indicator = style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
    drawChevron(this, indicator, option.state & QStyle::State_Enabled,
                option.state &
                    (QStyle::State_MouseOver | QStyle::State_HasFocus));
  }
};

class ChevronMenuButton final : public QPushButton {
public:
  using QPushButton::QPushButton;

protected:
  void paintEvent(QPaintEvent *event) override {
    QPushButton::paintEvent(event);
    QStyleOptionButton option;
    initStyleOption(&option);
    const QRect contents =
        style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
    const int indicatorWidth =
        style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, this);
    QRect indicator(contents.right() - std::max(12, indicatorWidth),
                    contents.top(), std::max(12, indicatorWidth),
                    contents.height());
    drawChevron(this, indicator, option.state & QStyle::State_Enabled,
                option.state &
                    (QStyle::State_MouseOver | QStyle::State_HasFocus));
  }
};

QString text(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto iterator = object.find(key);
  return iterator != object.end() && iterator->is_string()
             ? iterator->get<std::string>()
             : std::string{};
}

QString friendly(QString value) {
  value.replace(QLatin1Char('-'), QLatin1Char(' '));
  value.replace(QLatin1Char('_'), QLatin1Char(' '));
  if (!value.isEmpty())
    value[0] = value[0].toUpper();
  return value;
}

void addChoice(QComboBox *combo, const QString &label, const QString &value) {
  if (combo->findData(value) < 0)
    combo->addItem(label, value);
}

void selectValue(QComboBox *combo, const QString &value,
                 const QString &fallback = {}) {
  int index = combo->findData(value);
  if (index < 0) {
    combo->addItem(fallback.isEmpty() ? friendly(value) : fallback, value);
    index = combo->count() - 1;
  }
  combo->setCurrentIndex(index);
}

QComboBox *compactCombo(const char *name) {
  auto *combo = new CompactComboBox;
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
  label->setStyleSheet(QStringLiteral("color:#667085;font-weight:600;"));
  const int labelHeight = label->fontMetrics().height();
  label->setFixedHeight(labelHeight);
  label->setBuddy(buddy ? buddy : control);
  control->setAccessibleName(caption);
  layout->addWidget(label);
  layout->addWidget(control);
  surface->setFixedHeight(labelHeight + SettingLabelSpacing +
                          SettingControlHeight);
  surface->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  return surface;
}

nlohmann::json canonicalSandbox(const nlohmann::json &canonical) {
  if (!canonical.is_object())
    return nullptr;
  if (canonical.contains("sandboxPolicy"))
    return canonical["sandboxPolicy"];
  if (canonical.contains("sandbox"))
    return canonical["sandbox"];
  return nullptr;
}

QString sandboxKey(const nlohmann::json &value) {
  std::string key;
  if (value.is_string())
    key = value.get<std::string>();
  else
    key = stringValue(value, "type");
  if (key == "readOnly")
    return QStringLiteral("read-only");
  if (key == "workspaceWrite")
    return QStringLiteral("workspace-write");
  if (key == "dangerFullAccess")
    return QStringLiteral("danger-full-access");
  if (key == "externalSandbox")
    return QStringLiteral("external");
  return key.empty() ? QString::fromLatin1(DefaultValue) : text(key);
}

bool sandboxNetworkEnabled(const nlohmann::json &value) {
  const QString access = sandboxKey(value);
  if (access == QStringLiteral("danger-full-access"))
    return true;
  if (value.is_object()) {
    const auto member = value.find("networkAccess");
    if (member != value.end()) {
      if (member->is_boolean())
        return member->get<bool>();
      if (member->is_string())
        return member->get<std::string>() == "enabled";
    }
  }
  return false;
}

QString optionalString(const nlohmann::json &object, const char *key) {
  const std::string value = stringValue(object, key);
  return value.empty() ? QString::fromLatin1(DefaultValue) : text(value);
}

} // namespace

TurnSettingsWidget::TurnSettingsWidget(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("codexTurnSettings"));
  setStyleSheet(QStringLiteral(
      "QWidget#codexTurnSettings{background:#ffffff;border-top:1px solid "
      "#d7dee8;}"));
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  auto *root = new QGridLayout(this);
  root->setContentsMargins(10, 8, 10, 8);
  root->setHorizontalSpacing(8);
  root->setVerticalSpacing(8);

  model = compactCombo("codexModel");
  model->setEditable(true);
  effort = compactCombo("codexEffort");
  sandbox = compactCombo("codexSandbox");
  network = compactCombo("codexNetwork");
  cwd = new QLineEdit;
  cwd->setObjectName(QStringLiteral("codexWorkspace"));
  cwd->setFixedHeight(SettingControlHeight);
  cwd->setStyleSheet(
      QStringLiteral("QLineEdit#codexWorkspace{min-height:30px;}"));
  cwd->setPlaceholderText(QStringLiteral("Codex default workspace"));
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
  approval = compactCombo("codexApproval");
  personality = compactCombo("codexPersonality");
  more = new ChevronMenuButton(QStringLiteral("More"));
  more->setProperty("codexChevron", true);
  more->setFixedHeight(SettingControlHeight);

  root->addWidget(labelled(QStringLiteral("Model"), model), 0, 0);
  root->addWidget(labelled(QStringLiteral("Reasoning"), effort), 0, 1);
  root->addWidget(labelled(QStringLiteral("Access"), sandbox), 0, 2);
  root->addWidget(labelled(QStringLiteral("Network"), network), 0, 3);
  root->addWidget(labelled(QStringLiteral("Workspace"), workspacePicker, cwd),
                  1, 0);
  root->addWidget(labelled(QStringLiteral("Approval"), approval), 1, 1);
  root->addWidget(labelled(QStringLiteral("Style"), personality), 1, 2);
  root->addWidget(labelled(QStringLiteral("Additional"), more), 1, 3);
  for (int column = 0; column < 4; ++column)
    root->setColumnStretch(column, 1);

  moreMenu = new QMenu(this);
  auto *moreContents = new QWidget;
  moreContents->setMinimumWidth(470);
  auto *moreLayout = new QGridLayout(moreContents);
  moreLayout->setContentsMargins(14, 12, 14, 12);
  moreLayout->setHorizontalSpacing(8);
  moreLayout->setVerticalSpacing(8);
  permissionProfile = compactCombo("codexPermissionProfile");
  reviewer = compactCombo("codexReviewer");
  serviceTier = compactCombo("codexServiceTier");
  serviceTier->setEditable(true);
  summary = compactCombo("codexSummary");
  collaboration = compactCombo("codexCollaboration");
  moreLayout->addWidget(
      labelled(QStringLiteral("Permission profile"), permissionProfile), 0, 0);
  moreLayout->addWidget(labelled(QStringLiteral("Approval reviewer"), reviewer),
                        0, 1);
  moreLayout->addWidget(labelled(QStringLiteral("Service tier"), serviceTier),
                        1, 0);
  moreLayout->addWidget(labelled(QStringLiteral("Reasoning summary"), summary),
                        1, 1);
  moreLayout->addWidget(
      labelled(QStringLiteral("Collaboration mode"), collaboration), 2, 0, 1,
      2);
  auto *moreAction = new QWidgetAction(moreMenu);
  moreAction->setDefaultWidget(moreContents);
  moreMenu->addAction(moreAction);
  more->setMenu(moreMenu);

  addChoice(effort, QStringLiteral("Codex default"), DefaultValue);
  for (const char *value :
       {"minimal", "low", "medium", "high", "xhigh", "ultra"})
    addChoice(effort, friendly(QString::fromLatin1(value)),
              QString::fromLatin1(value));
  addChoice(sandbox, QStringLiteral("Codex default"), DefaultValue);
  addChoice(sandbox, QStringLiteral("Workspace"),
            QStringLiteral("workspace-write"));
  addChoice(sandbox, QStringLiteral("Read only"), QStringLiteral("read-only"));
  addChoice(sandbox, QStringLiteral("Full access"),
            QStringLiteral("danger-full-access"));
  addChoice(sandbox, QStringLiteral("External"), QStringLiteral("external"));
  addChoice(network, QStringLiteral("Codex default"), DefaultValue);
  addChoice(network, QStringLiteral("Restricted"),
            QStringLiteral("restricted"));
  addChoice(network, QStringLiteral("Enabled"), QStringLiteral("enabled"));
  addChoice(approval, QStringLiteral("Codex default"), DefaultValue);
  addChoice(approval, QStringLiteral("On request"),
            QStringLiteral("on-request"));
  addChoice(approval, QStringLiteral("Untrusted"), QStringLiteral("untrusted"));
  addChoice(approval, QStringLiteral("Never"), QStringLiteral("never"));
  addChoice(personality, QStringLiteral("Codex default"), DefaultValue);
  addChoice(personality, QStringLiteral("None"), QStringLiteral("none"));
  addChoice(personality, QStringLiteral("Friendly"),
            QStringLiteral("friendly"));
  addChoice(personality, QStringLiteral("Pragmatic"),
            QStringLiteral("pragmatic"));
  addChoice(reviewer, QStringLiteral("Codex default"), DefaultValue);
  addChoice(reviewer, QStringLiteral("User"), QStringLiteral("user"));
  addChoice(reviewer, QStringLiteral("Auto review"),
            QStringLiteral("auto_review"));
  addChoice(reviewer, QStringLiteral("Guardian"),
            QStringLiteral("guardian_subagent"));
  addChoice(serviceTier, QStringLiteral("Codex default"), DefaultValue);
  addChoice(summary, QStringLiteral("Codex default"), DefaultValue);
  addChoice(summary, QStringLiteral("Auto"), QStringLiteral("auto"));
  addChoice(summary, QStringLiteral("Concise"), QStringLiteral("concise"));
  addChoice(summary, QStringLiteral("Detailed"), QStringLiteral("detailed"));
  addChoice(summary, QStringLiteral("None"), QStringLiteral("none"));
  addChoice(collaboration, QStringLiteral("Code"), QStringLiteral("default"));
  addChoice(collaboration, QStringLiteral("Plan"), QStringLiteral("plan"));
  addChoice(permissionProfile, QStringLiteral("Codex default"), DefaultValue);

  const auto connectCombo = [this](QComboBox *combo, Field field) {
    connect(combo, &QComboBox::currentIndexChanged, this,
            [this, field] { markTouched(field); });
  };
  connectCombo(model, Field::Model);
  connect(model->lineEdit(), &QLineEdit::textEdited, this,
          [this] { markTouched(Field::Model); });
  connectCombo(effort, Field::Effort);
  connectCombo(personality, Field::Personality);
  connectCombo(sandbox, Field::Sandbox);
  connectCombo(network, Field::Network);
  connectCombo(approval, Field::Approval);
  connectCombo(reviewer, Field::Reviewer);
  connectCombo(permissionProfile, Field::PermissionProfile);
  connectCombo(serviceTier, Field::ServiceTier);
  connect(serviceTier->lineEdit(), &QLineEdit::textEdited, this,
          [this] { markTouched(Field::ServiceTier); });
  connectCombo(summary, Field::Summary);
  connectCombo(collaboration, Field::Collaboration);
  connect(cwd, &QLineEdit::textEdited, this,
          [this] { markTouched(Field::Workspace); });
  connect(browseWorkspace, &QToolButton::clicked, this, [this] {
    const QString initialDirectory =
        cwd->text().trimmed().isEmpty()
            ? QDir::homePath()
            : QDir::fromNativeSeparators(cwd->text().trimmed());
    FileSelectionDialog dialog(FileSelectionDialog::Mode::Workspace,
                               initialDirectory, {}, this);
    if (dialog.exec() == QDialog::Accepted)
      setWorkspace(dialog.selectedDirectory());
  });
  connect(model, &QComboBox::currentIndexChanged, this,
          [this] { refreshModelOptions(); });
  connect(sandbox, &QComboBox::currentIndexChanged, this,
          [this] { refreshAccessCompatibility(); });
  connect(permissionProfile, &QComboBox::currentIndexChanged, this,
          [this] { refreshAccessCompatibility(); });
}

void TurnSettingsWidget::setContext(std::string identity,
                                    const nlohmann::json &canonical,
                                    const nlohmann::json &models,
                                    const nlohmann::json &permissionProfiles) {
  const bool changed = contextIdentity != identity;
  contextIdentity = std::move(identity);
  modelCatalog = models.is_array() ? models : nlohmann::json::array();
  if (changed)
    resetFromCanonical(canonical);
  refreshModels(modelCatalog);
  refreshPermissionProfiles(permissionProfiles);
  refreshModelOptions();
  refreshAccessCompatibility();
  refreshMoreIndicator();
}

void TurnSettingsWidget::setControlsEnabled(bool enabled) {
  setEnabled(enabled);
  setToolTip(enabled ? QString{}
                     : QStringLiteral("Settings apply when starting a turn"));
}

void TurnSettingsWidget::setWorkspace(QString path) {
  cwd->setText(QDir::toNativeSeparators(std::move(path)));
  markTouched(Field::Workspace);
}

std::string TurnSettingsWidget::workspace(const std::string &fallback) const {
  const QString selected = cwd->text().trimmed();
  return selected.isEmpty() ? fallback : selected.toStdString();
}

nlohmann::json TurnSettingsWidget::threadStartOptions() const {
  nlohmann::json result = nlohmann::json::object();
  const auto copyChoice = [this, &result](Field field, const char *name,
                                          const QComboBox *combo) {
    if (!touched(field))
      return;
    const QString selected = value(combo);
    result[name] = selected == DefaultValue
                       ? nlohmann::json(nullptr)
                       : nlohmann::json(selected.toStdString());
  };
  copyChoice(Field::Model, "model", model);
  copyChoice(Field::Approval, "approvalPolicy", approval);
  copyChoice(Field::Reviewer, "approvalsReviewer", reviewer);
  copyChoice(Field::Personality, "personality", personality);
  copyChoice(Field::ServiceTier, "serviceTier", serviceTier);
  if (touched(Field::Workspace))
    result["cwd"] = cwd->text().trimmed().isEmpty()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(cwd->text().trimmed().toStdString());
  const QString selectedProfile = value(permissionProfile);
  if (touched(Field::PermissionProfile))
    result["permissions"] = selectedProfile == DefaultValue
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(selectedProfile.toStdString());
  if (selectedProfile == DefaultValue && touched(Field::Sandbox)) {
    const QString selected = value(sandbox);
    result["sandbox"] = selected == DefaultValue || selected == "external"
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(selected.toStdString());
  }
  return result;
}

nlohmann::json TurnSettingsWidget::turnStartOptions() const {
  nlohmann::json result = nlohmann::json::object();
  const auto copyChoice = [this, &result](Field field, const char *name,
                                          const QComboBox *combo) {
    if (!touched(field))
      return;
    const QString selected = value(combo);
    result[name] = selected == DefaultValue
                       ? nlohmann::json(nullptr)
                       : nlohmann::json(selected.toStdString());
  };
  copyChoice(Field::Model, "model", model);
  copyChoice(Field::Effort, "effort", effort);
  copyChoice(Field::Personality, "personality", personality);
  copyChoice(Field::Approval, "approvalPolicy", approval);
  copyChoice(Field::Reviewer, "approvalsReviewer", reviewer);
  copyChoice(Field::ServiceTier, "serviceTier", serviceTier);
  copyChoice(Field::Summary, "summary", summary);
  if (touched(Field::Workspace))
    result["cwd"] = cwd->text().trimmed().isEmpty()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(cwd->text().trimmed().toStdString());
  const QString selectedProfile = value(permissionProfile);
  if (touched(Field::PermissionProfile))
    result["permissions"] = selectedProfile == DefaultValue
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(selectedProfile.toStdString());
  if (selectedProfile == DefaultValue &&
      (touched(Field::Sandbox) || touched(Field::Network))) {
    result["sandboxPolicy"] = sandboxPolicy();
  }
  const nlohmann::json mode = collaborationMode();
  if (!mode.is_null())
    result["collaborationMode"] = mode;
  return result;
}

void TurnSettingsWidget::markTouched(Field field) {
  touchedFields[static_cast<std::size_t>(field)] = true;
  refreshMoreIndicator();
}

void TurnSettingsWidget::resetFromCanonical(const nlohmann::json &canonical) {
  touchedFields.fill(false);
  const QSignalBlocker modelBlocker(model);
  const QSignalBlocker effortBlocker(effort);
  const QSignalBlocker personalityBlocker(personality);
  const QSignalBlocker sandboxBlocker(sandbox);
  const QSignalBlocker networkBlocker(network);
  const QSignalBlocker approvalBlocker(approval);
  const QSignalBlocker reviewerBlocker(reviewer);
  const QSignalBlocker cwdBlocker(cwd);
  const QSignalBlocker permissionBlocker(permissionProfile);
  const QSignalBlocker tierBlocker(serviceTier);
  const QSignalBlocker summaryBlocker(summary);
  const QSignalBlocker collaborationBlocker(collaboration);

  selectValue(model, optionalString(canonical, "model"),
              QStringLiteral("Codex default"));
  QString canonicalEffort = optionalString(canonical, "reasoningEffort");
  if (canonicalEffort == DefaultValue)
    canonicalEffort = optionalString(canonical, "effort");
  selectValue(effort, canonicalEffort, QStringLiteral("Codex default"));
  selectValue(personality, optionalString(canonical, "personality"),
              QStringLiteral("Codex default"));
  const nlohmann::json nativeSandbox = canonicalSandbox(canonical);
  selectValue(sandbox, sandboxKey(nativeSandbox),
              QStringLiteral("Codex default"));
  selectValue(network, sandboxKey(nativeSandbox) == DefaultValue
                           ? QString::fromLatin1(DefaultValue)
                       : sandboxNetworkEnabled(nativeSandbox)
                           ? QStringLiteral("enabled")
                           : QStringLiteral("restricted"));
  std::string nativeApproval = stringValue(canonical, "approvalPolicy");
  selectValue(approval,
              nativeApproval.empty() ? QString::fromLatin1(DefaultValue)
                                     : text(nativeApproval),
              nativeApproval.empty() ? QStringLiteral("Codex default")
                                     : QStringLiteral("Current policy"));
  selectValue(reviewer, optionalString(canonical, "approvalsReviewer"),
              QStringLiteral("Codex default"));
  cwd->setText(text(stringValue(canonical, "cwd")));
  QString activeProfile = QString::fromLatin1(DefaultValue);
  const nlohmann::json profile =
      canonical.value("activePermissionProfile", nlohmann::json::object());
  if (profile.is_object() && profile.contains("id") &&
      profile["id"].is_string())
    activeProfile = text(profile["id"].get<std::string>());
  selectValue(permissionProfile, activeProfile,
              QStringLiteral("Current permission profile"));
  selectValue(serviceTier, optionalString(canonical, "serviceTier"),
              QStringLiteral("Codex default"));
  selectValue(summary, optionalString(canonical, "summary"),
              QStringLiteral("Codex default"));
  const nlohmann::json mode =
      canonical.value("collaborationMode", nlohmann::json::object());
  selectValue(collaboration, mode.is_object() && mode.contains("mode") &&
                                     mode["mode"].is_string()
                                 ? text(mode["mode"].get<std::string>())
                                 : QStringLiteral("default"));
}

void TurnSettingsWidget::refreshModels(const nlohmann::json &models) {
  const QString selected = model->currentData().toString();
  const QString edited = model->currentText().trimmed();
  const bool custom =
      model->isEditable() &&
      (model->currentIndex() < 0 ||
       model->currentText() != model->itemText(model->currentIndex()));
  const QSignalBlocker blocker(model);
  model->clear();
  addChoice(model, QStringLiteral("Codex default"), DefaultValue);
  if (models.is_array()) {
    for (const auto &entry : models) {
      if (!entry.is_object() || entry.value("hidden", false))
        continue;
      std::string id = stringValue(entry, "model");
      if (id.empty())
        id = stringValue(entry, "id");
      if (id.empty())
        continue;
      const std::string display = stringValue(entry, "displayName");
      addChoice(model, display.empty() ? text(id) : text(display), text(id));
    }
  }
  selectValue(model, selected.isEmpty() ? QString::fromLatin1(DefaultValue)
                                        : selected);
  if (custom && !edited.isEmpty())
    model->setEditText(edited);
}

void TurnSettingsWidget::refreshModelOptions() {
  const QString selected = value(effort);
  const QString modelId = model->currentIndex() >= 0
                              ? model->currentData().toString()
                              : model->currentText().trimmed();
  const nlohmann::json *definition = nullptr;
  if (modelCatalog.is_array()) {
    const auto match =
        std::find_if(modelCatalog.begin(), modelCatalog.end(),
                     [&modelId](const nlohmann::json &entry) {
                       return text(stringValue(entry, "model")) == modelId ||
                              text(stringValue(entry, "id")) == modelId;
                     });
    if (match != modelCatalog.end())
      definition = &*match;
  }
  const QSignalBlocker blocker(effort);
  effort->clear();
  QString defaultLabel = QStringLiteral("Codex default");
  if (definition) {
    const std::string defaultEffort =
        stringValue(*definition, "defaultReasoningEffort");
    if (!defaultEffort.empty())
      defaultLabel =
          friendly(text(defaultEffort)) + QStringLiteral(" - default");
  }
  addChoice(effort, defaultLabel, DefaultValue);
  const nlohmann::json supported =
      definition ? definition->value("supportedReasoningEfforts",
                                     nlohmann::json::array())
                 : nlohmann::json::array();
  if (supported.is_array() && !supported.empty()) {
    for (const auto &option : supported) {
      const std::string key = stringValue(option, "reasoningEffort");
      if (!key.empty())
        addChoice(effort, friendly(text(key)), text(key));
    }
  } else {
    for (const char *key :
         {"minimal", "low", "medium", "high", "xhigh", "ultra"})
      addChoice(effort, friendly(QString::fromLatin1(key)),
                QString::fromLatin1(key));
  }
  const QString requestedEffort =
      selected.isEmpty() ? QString::fromLatin1(DefaultValue) : selected;
  const bool constrained = supported.is_array() && !supported.empty();
  selectValue(effort, constrained && effort->findData(requestedEffort) < 0
                          ? QString::fromLatin1(DefaultValue)
                          : requestedEffort);

  const QString selectedTier = value(serviceTier);
  const QSignalBlocker tierBlocker(serviceTier);
  serviceTier->clear();
  QString defaultTierLabel = QStringLiteral("Codex default");
  if (definition) {
    const std::string defaultTier =
        stringValue(*definition, "defaultServiceTier");
    if (!defaultTier.empty())
      defaultTierLabel += QStringLiteral(" (%1)").arg(text(defaultTier));
  }
  addChoice(serviceTier, defaultTierLabel, DefaultValue);
  if (definition) {
    const nlohmann::json tiers =
        definition->value("serviceTiers", nlohmann::json::array());
    if (tiers.is_array()) {
      for (const auto &tier : tiers) {
        const std::string id = stringValue(tier, "id");
        if (id.empty())
          continue;
        const std::string name = stringValue(tier, "name");
        addChoice(serviceTier, name.empty() ? text(id) : text(name), text(id));
        const int index = serviceTier->findData(text(id));
        if (index >= 0)
          serviceTier->setItemData(
              index, text(stringValue(tier, "description")), Qt::ToolTipRole);
      }
    }
    const nlohmann::json legacyTiers =
        definition->value("additionalSpeedTiers", nlohmann::json::array());
    if (legacyTiers.is_array()) {
      for (const auto &tier : legacyTiers) {
        if (tier.is_string())
          addChoice(serviceTier, friendly(text(tier.get<std::string>())),
                    text(tier.get<std::string>()));
      }
    }
  }
  const QString requestedTier =
      selectedTier.isEmpty() ? QString::fromLatin1(DefaultValue) : selectedTier;
  if (serviceTier->findData(requestedTier) >= 0)
    selectValue(serviceTier, requestedTier);
  else
    serviceTier->setEditText(requestedTier);

  const bool supportsPersonality =
      !definition || definition->value("supportsPersonality", true);
  personality->setEnabled(supportsPersonality);
  personality->setToolTip(
      supportsPersonality
          ? QString{}
          : QStringLiteral(
                "The selected model does not support style choices"));
  if (!supportsPersonality && value(personality) != DefaultValue) {
    const QSignalBlocker personalityBlocker(personality);
    selectValue(personality, QString::fromLatin1(DefaultValue));
  }
}

void TurnSettingsWidget::refreshPermissionProfiles(
    const nlohmann::json &profiles) {
  const QString selected = value(permissionProfile);
  const QSignalBlocker blocker(permissionProfile);
  permissionProfile->clear();
  addChoice(permissionProfile, QStringLiteral("Codex default"), DefaultValue);
  nlohmann::json entries = profiles;
  if (profiles.is_object())
    entries = profiles.value("data", nlohmann::json::array());
  if (entries.is_array()) {
    for (const auto &entry : entries) {
      const std::string id = stringValue(entry, "id");
      if (id.empty() || !entry.value("allowed", true))
        continue;
      addChoice(permissionProfile, text(id), text(id));
      const int index = permissionProfile->findData(text(id));
      if (index >= 0)
        permissionProfile->setItemData(
            index, text(stringValue(entry, "description")), Qt::ToolTipRole);
    }
  }
  selectValue(permissionProfile,
              selected.isEmpty() ? QString::fromLatin1(DefaultValue) : selected,
              QStringLiteral("Current permission profile"));
}

void TurnSettingsWidget::refreshAccessCompatibility() {
  const bool namedProfile = value(permissionProfile) != DefaultValue;
  sandbox->setEnabled(!namedProfile);
  network->setEnabled(!namedProfile && value(sandbox) != "danger-full-access" &&
                      value(sandbox) != DefaultValue);
  if (value(sandbox) == "danger-full-access") {
    const QSignalBlocker blocker(network);
    selectValue(network, QStringLiteral("enabled"));
  } else if (value(sandbox) == DefaultValue) {
    const QSignalBlocker blocker(network);
    selectValue(network, QString::fromLatin1(DefaultValue));
  }
  const QString reason =
      namedProfile
          ? QStringLiteral("The selected permission profile owns access policy")
          : QString{};
  sandbox->setToolTip(reason);
  network->setToolTip(reason);
}

void TurnSettingsWidget::refreshMoreIndicator() {
  const bool changed = touched(Field::PermissionProfile) ||
                       touched(Field::Reviewer) ||
                       touched(Field::ServiceTier) || touched(Field::Summary) ||
                       touched(Field::Collaboration);
  more->setProperty("changed", changed);
  more->style()->unpolish(more);
  more->style()->polish(more);
  more->update();
}

bool TurnSettingsWidget::touched(Field field) const noexcept {
  return touchedFields[static_cast<std::size_t>(field)];
}

QString TurnSettingsWidget::value(const QComboBox *combo) const {
  if (combo->isEditable()) {
    const int index = combo->currentIndex();
    if (index < 0 || combo->currentText() != combo->itemText(index))
      return combo->currentText().trimmed();
  }
  return combo->currentData().toString();
}

nlohmann::json TurnSettingsWidget::sandboxPolicy() const {
  const QString access = value(sandbox);
  if (access == DefaultValue)
    return nullptr;
  if (access == "danger-full-access")
    return {{"type", "dangerFullAccess"}};
  if (access == "external")
    return {{"type", "externalSandbox"},
            {"networkAccess",
             value(network) == "enabled" ? "enabled" : "restricted"}};
  if (access == "read-only")
    return {{"type", "readOnly"},
            {"networkAccess", value(network) == "enabled"}};
  return {{"type", "workspaceWrite"},
          {"writableRoots", nlohmann::json::array()},
          {"networkAccess", value(network) == "enabled"},
          {"excludeTmpdirEnvVar", false},
          {"excludeSlashTmp", false}};
}

nlohmann::json TurnSettingsWidget::collaborationMode() const {
  QString selectedModel = value(model);
  if (selectedModel == DefaultValue && modelCatalog.is_array()) {
    const auto defaultModel = std::find_if(
        modelCatalog.begin(), modelCatalog.end(),
        [](const nlohmann::json &entry) {
          return entry.is_object() && entry.value("isDefault", false);
        });
    if (defaultModel != modelCatalog.end()) {
      std::string modelId = stringValue(*defaultModel, "model");
      if (modelId.empty())
        modelId = stringValue(*defaultModel, "id");
      selectedModel = text(modelId);
    }
  }
  if (selectedModel.isEmpty() || selectedModel == DefaultValue)
    return nullptr;

  const QString selectedEffort = value(effort);
  nlohmann::json settings{{"model", selectedModel.toStdString()},
                          {"developer_instructions", nullptr}};
  if (selectedEffort == DefaultValue)
    settings["reasoning_effort"] = nullptr;
  else if (!selectedEffort.isEmpty())
    settings["reasoning_effort"] = selectedEffort.toStdString();
  return {{"mode", value(collaboration).toStdString()},
          {"settings", std::move(settings)}};
}

} // namespace codexui::codex
