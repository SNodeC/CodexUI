// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PendingRequestDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <ranges>
#include <string_view>
#include <vector>

namespace codexui::codex {
namespace {

QString text(std::string_view value) {
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

QLabel *wrapped(QString value, const char *kind = "body") {
  auto *label = new QLabel(std::move(value));
  label->setTextFormat(Qt::PlainText);
  label->setProperty("kind", kind);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QString displayKey(std::string_view key) {
  if (key == "fileSystem")
    return QStringLiteral("File system");
  if (key == "network")
    return QStringLiteral("Network");
  if (key == "globScanMaxDepth")
    return QStringLiteral("glob scan maximum depth");
  return text(std::string(key));
}

QString displayValue(const nlohmann::json &value) {
  if (value.is_boolean())
    return value.get<bool>() ? QStringLiteral("Yes") : QStringLiteral("No");
  if (value.is_string())
    return text(value.get<std::string>());
  return value.is_null() ? QStringLiteral("None") : text(value.dump());
}

void addJsonValue(QVBoxLayout *layout, const nlohmann::json &value,
                  const QString &path) {
  if (value.is_object()) {
    if (value.empty()) {
      layout->addWidget(wrapped(path.isEmpty()
                                    ? QStringLiteral("None specified")
                                    : QStringLiteral("%1: None").arg(path),
                                "meta"));
      return;
    }
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
      const QString key = displayKey(iterator.key());
      addJsonValue(layout, iterator.value(),
                   path.isEmpty() ? key
                                  : QStringLiteral("%1 / %2").arg(path, key));
    }
    return;
  }
  if (value.is_array()) {
    if (value.empty()) {
      layout->addWidget(wrapped(path.isEmpty()
                                    ? QStringLiteral("None specified")
                                    : QStringLiteral("%1: None").arg(path),
                                "meta"));
      return;
    }
    for (qsizetype index = 0; index < static_cast<qsizetype>(value.size());
         ++index) {
      addJsonValue(layout, value[static_cast<std::size_t>(index)],
                   path.isEmpty()
                       ? QStringLiteral("Item %1").arg(index + 1)
                       : QStringLiteral("%1 / %2").arg(path).arg(index + 1));
    }
    return;
  }
  layout->addWidget(wrapped(
      QStringLiteral("%1: %2").arg(
          path.isEmpty() ? QStringLiteral("Value") : path, displayValue(value)),
      "meta"));
}

void addRequestDisclosure(QVBoxLayout *layout, const nlohmann::json &request) {
  addJsonValue(layout, request, {});
}

QComboBox *
addDecisionEditor(QVBoxLayout *layout,
                  const std::vector<PendingRequestAction> &requestActions) {
  auto *decision = new QComboBox;
  for (const PendingRequestAction &action : requestActions)
    decision->addItem(text(action.label), text(action.value));
  auto *label = wrapped(QStringLiteral("Decision"), "title");
  label->setBuddy(decision);
  layout->addWidget(label);
  layout->addWidget(decision);
  return decision;
}

void showValidationWarning(QWidget *parent, QString title, QString message) {
  QMessageBox warning(QMessageBox::Warning, std::move(title),
                      std::move(message), QMessageBox::Ok, parent);
  // Validation keeps the parent request dialog and its authored controls
  // alive. An explicit Qt-owned dialog also avoids platform-native teardown
  // reentrancy when the warning is dismissed from the nested modal loop.
  warning.setOption(QMessageBox::Option::DontUseNativeDialog, true);
  warning.exec();
}

struct QuestionEditor {
  std::string id;
  std::vector<std::pair<std::string, QCheckBox *>> choices;
  QLineEdit *other = nullptr;
};

} // namespace

std::optional<PendingRequestSubmission> PendingRequestDialog::present(
    const PendingRequestDescriptor &request, QWidget *parent,
    const PendingRequestSubmission *initialSubmission) {
  QDialog dialog(parent);
  const QString dialogTitle =
      text(PendingRequestPolicy::dialogTitle(request.kind));
  dialog.setWindowTitle(dialogTitle);
  dialog.setModal(true);
  dialog.resize(620, 560);
  auto *root = new QVBoxLayout(&dialog);
  root->setContentsMargins(18, 16, 18, 16);
  root->setSpacing(10);
  root->addWidget(wrapped(dialogTitle, "heading"));

  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *content = new QWidget;
  auto *contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(0, 0, 8, 0);
  contentLayout->setSpacing(8);
  scroll->setWidget(content);
  root->addWidget(scroll, 1);

  QComboBox *decision = nullptr;
  QPlainTextEdit *structuredContent = nullptr;
  std::vector<QuestionEditor> questions;
  const nlohmann::json &raw = request.raw;
  const std::vector<PendingRequestAction> requestActions =
      PendingRequestPolicy::actions(request);
  const nlohmann::json &initialInput =
      initialSubmission ? initialSubmission->input : nlohmann::json::object();
  contentLayout->addWidget(wrapped(QStringLiteral("Request details"), "title"));
  addRequestDisclosure(contentLayout,
                       PendingRequestPolicy::disclosure(request));

  if (request.kind == PendingRequestKind::CommandApproval) {
    decision = addDecisionEditor(contentLayout, requestActions);
  } else if (request.kind == PendingRequestKind::FileChangeApproval) {
    decision = addDecisionEditor(contentLayout, requestActions);
  } else if (request.kind == PendingRequestKind::UserInput) {
    decision = addDecisionEditor(contentLayout, requestActions);
    const nlohmann::json requestedQuestions =
        raw.value("questions", nlohmann::json::array());
    if (decision->findData(QStringLiteral("submit")) >= 0 &&
        requestedQuestions.is_array()) {
      for (const auto &question : requestedQuestions) {
        QuestionEditor editor;
        editor.id = stringValue(question, "id");
        auto *section = new QFrame;
        section->setProperty("kind", "summary");
        auto *sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(12, 10, 12, 10);
        sectionLayout->setSpacing(6);
        const std::string header = stringValue(question, "header");
        const QString questionText = text(stringValue(question, "question"));
        section->setAccessibleName(header.empty() ? questionText
                                                  : text(header));
        section->setAccessibleDescription(questionText);
        if (!header.empty())
          sectionLayout->addWidget(wrapped(text(header), "title"));
        sectionLayout->addWidget(wrapped(questionText));
        const nlohmann::json options =
            question.value("options", nlohmann::json::array());
        if (options.is_array()) {
          for (const auto &option : options) {
            const std::string label = stringValue(option, "label");
            if (label.empty())
              continue;
            auto *choice = new QCheckBox(text(label));
            const std::string description = stringValue(option, "description");
            if (!description.empty()) {
              choice->setToolTip(text(description));
              choice->setAccessibleDescription(text(description));
            }
            sectionLayout->addWidget(choice);
            if (!description.empty())
              sectionLayout->addWidget(wrapped(text(description), "meta"));
            editor.choices.emplace_back(label, choice);
          }
        }
        if (options.empty() || question.value("isOther", false)) {
          editor.other = new QLineEdit;
          editor.other->setPlaceholderText(
              options.empty() ? QStringLiteral("Type your answer")
                              : QStringLiteral("Other answer"));
          editor.other->setAccessibleName(questionText);
          if (question.value("isSecret", false))
            editor.other->setEchoMode(QLineEdit::Password);
          sectionLayout->addWidget(editor.other);
        }
        if (initialInput.is_object()) {
          const auto savedQuestion = initialInput.find(editor.id);
          if (savedQuestion != initialInput.end() &&
              savedQuestion->is_object()) {
            const auto savedValues = savedQuestion->find("answers");
            if (savedValues != savedQuestion->end() &&
                savedValues->is_array()) {
              for (const auto &savedValue : *savedValues) {
                if (!savedValue.is_string())
                  continue;
                const std::string saved = savedValue.get<std::string>();
                const auto known = std::ranges::find_if(
                    editor.choices, [&saved](const auto &choice) {
                      return choice.first == saved;
                    });
                if (known != editor.choices.end())
                  known->second->setChecked(true);
                else if (editor.other)
                  editor.other->setText(text(saved));
              }
            }
          }
        }
        questions.push_back(std::move(editor));
        contentLayout->addWidget(section);
      }
    } else {
      contentLayout->addWidget(wrapped(
          QStringLiteral("This request cannot be answered safely because its "
                         "question structure is incomplete or exceeds the "
                         "interactive display bound. It may still be "
                         "declined."),
          "meta"));
    }
  } else if (request.kind == PendingRequestKind::McpElicitation) {
    decision = addDecisionEditor(contentLayout, requestActions);
    const bool acceptsContent = std::ranges::any_of(
        requestActions, [](const PendingRequestAction &action) {
          return action.value == "accept" && action.requiresInput;
        });
    if (acceptsContent && raw.contains("requestedSchema")) {
      auto *structuredLabel =
          wrapped(QStringLiteral("Structured response (JSON)"), "title");
      structuredContent = new QPlainTextEdit(QStringLiteral("{}"));
      structuredLabel->setBuddy(structuredContent);
      contentLayout->addWidget(structuredLabel);
      structuredContent->setMinimumHeight(150);
      structuredContent->setLineWrapMode(QPlainTextEdit::WidgetWidth);
      structuredContent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      structuredContent->setProperty("kind", "dialogEditor");
      if (initialSubmission)
        structuredContent->setPlainText(text(initialInput.dump(2)));
      contentLayout->addWidget(structuredContent);
    }
  } else if (request.kind == PendingRequestKind::PermissionsApproval) {
    decision = addDecisionEditor(contentLayout, requestActions);
  } else if (request.kind == PendingRequestKind::LegacyPatchApproval ||
             request.kind == PendingRequestKind::LegacyCommandApproval) {
    contentLayout->addWidget(wrapped(
        QStringLiteral("This is a legacy approval request. Prefer the current "
                       "typed approval path when available.")));
    decision = addDecisionEditor(contentLayout, requestActions);
  } else {
    contentLayout->addWidget(wrapped(
        request.kind == PendingRequestKind::DynamicToolCall
            ? QStringLiteral("CodexUI does not implement the requested dynamic "
                             "tool. Submitting will return a typed failed tool "
                             "result.")
            : QStringLiteral("CodexUI cannot safely produce this capability. "
                             "Submitting will return an explicit JSON-RPC "
                             "unsupported error.")));
  }
  if (decision && initialSubmission) {
    const std::string &savedDecision = initialSubmission->choice;
    const int savedIndex =
        decision->findData(text(savedDecision), Qt::UserRole, Qt::MatchExactly);
    if (savedIndex >= 0)
      decision->setCurrentIndex(savedIndex);
  }
  contentLayout->addStretch();

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Submit"));
  nlohmann::json acceptedAnswers = nlohmann::json::object();
  nlohmann::json acceptedStructuredContent = nullptr;
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    if (request.kind == PendingRequestKind::UserInput &&
        decision->currentData().toString() == QStringLiteral("submit")) {
      nlohmann::json answers = nlohmann::json::object();
      for (const QuestionEditor &question : questions) {
        nlohmann::json values = nlohmann::json::array();
        for (const auto &[label, choice] : question.choices) {
          if (choice->isChecked())
            values.push_back(label);
        }
        if (question.other && !question.other->text().trimmed().isEmpty())
          values.push_back(question.other->text().toStdString());
        if (values.empty()) {
          QWidget *invalid = question.other;
          if (!invalid && !question.choices.empty())
            invalid = question.choices.front().second;
          if (invalid) {
            scroll->ensureWidgetVisible(invalid);
            invalid->setFocus();
          }
          showValidationWarning(
              &dialog, QStringLiteral("Incomplete response"),
              QStringLiteral("Answer every question before submitting."));
          if (invalid)
            invalid->setFocus();
          return;
        }
        answers[question.id] = {{"answers", std::move(values)}};
      }
      acceptedAnswers = std::move(answers);
    } else if (request.kind == PendingRequestKind::McpElicitation &&
               structuredContent &&
               decision->currentData().toString() == QStringLiteral("accept")) {
      nlohmann::json content = nlohmann::json::parse(
          structuredContent->toPlainText().toStdString(), nullptr, false);
      if (content.is_discarded()) {
        scroll->ensureWidgetVisible(structuredContent);
        structuredContent->setFocus();
        showValidationWarning(
            &dialog, QStringLiteral("Invalid response"),
            QStringLiteral("The MCP response must be valid JSON."));
        structuredContent->setFocus();
        return;
      }
      acceptedStructuredContent = std::move(content);
    }
    dialog.accept();
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  root->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted)
    return std::nullopt;

  if (!decision)
    return PendingRequestSubmission{
        requestActions.empty() ? std::string{} : requestActions.front().value,
        nullptr, nullptr};
  std::string selectedDecision =
      decision->currentData().toString().toStdString();
  if (request.kind == PendingRequestKind::UserInput) {
    const bool submits = selectedDecision == "submit";
    return PendingRequestSubmission{std::move(selectedDecision),
                                    submits ? std::move(acceptedAnswers)
                                            : nlohmann::json(nullptr),
                                    nullptr};
  }
  if (request.kind == PendingRequestKind::McpElicitation)
    return PendingRequestSubmission{
        std::move(selectedDecision), std::move(acceptedStructuredContent),
        initialSubmission ? initialSubmission->metadata
                          : nlohmann::json(nullptr)};
  return PendingRequestSubmission{std::move(selectedDecision), nullptr,
                                  nullptr};
}

} // namespace codexui::codex
