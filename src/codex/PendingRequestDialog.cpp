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

#include <vector>

namespace codexui::codex {
namespace {

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

QLabel *wrapped(QString value, const char *kind = "body") {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::LinksAccessibleByMouse);
  label->setOpenExternalLinks(true);
  return label;
}

QString titleFor(const std::string &kind) {
  if (kind == "command-approval")
    return QStringLiteral("Command approval");
  if (kind == "file-change-approval")
    return QStringLiteral("File-change approval");
  if (kind == "user-input")
    return QStringLiteral("Codex needs input");
  if (kind == "mcp-elicitation")
    return QStringLiteral("MCP server request");
  if (kind == "permissions-approval")
    return QStringLiteral("Permission request");
  if (kind == "dynamic-tool-call")
    return QStringLiteral("Dynamic tool request");
  if (kind == "authentication-refresh")
    return QStringLiteral("Authentication refresh");
  if (kind == "attestation")
    return QStringLiteral("Attestation request");
  if (kind == "legacy-patch-approval")
    return QStringLiteral("Legacy patch approval");
  if (kind == "legacy-command-approval")
    return QStringLiteral("Legacy command approval");
  return QStringLiteral("Unsupported Codex request");
}

void addDetail(QVBoxLayout *layout, const QString &label,
               const std::string &value) {
  if (!value.empty())
    layout->addWidget(
        wrapped(QStringLiteral("%1: %2").arg(label, text(value)), "meta"));
}

void addChoice(QComboBox *combo, const QString &label, const char *value) {
  if (combo->findData(QString::fromLatin1(value)) < 0)
    combo->addItem(label, QString::fromLatin1(value));
}

nlohmann::json jsonRpcError(std::string message) {
  return {{"code", -32601}, {"message", std::move(message)}};
}

struct QuestionEditor {
  std::string id;
  std::vector<std::pair<std::string, QCheckBox *>> choices;
  QLineEdit *other = nullptr;
};

} // namespace

std::optional<PendingRequestResponse>
PendingRequestDialog::present(const PendingRequestPresentation &request,
                              QWidget *parent) {
  QDialog dialog(parent);
  dialog.setWindowTitle(titleFor(request.kind));
  dialog.setModal(true);
  dialog.resize(620, 560);
  auto *root = new QVBoxLayout(&dialog);
  root->setContentsMargins(18, 16, 18, 16);
  root->setSpacing(10);
  root->addWidget(wrapped(titleFor(request.kind), "heading"));
  root->addWidget(wrapped(QStringLiteral("Thread %1  |  request %2")
                              .arg(text(request.threadId), text(request.id)),
                          "meta"));

  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
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

  if (request.kind == "command-approval") {
    addDetail(contentLayout, QStringLiteral("Command"),
              stringValue(raw, "command"));
    addDetail(contentLayout, QStringLiteral("Working directory"),
              stringValue(raw, "cwd"));
    addDetail(contentLayout, QStringLiteral("Reason"),
              stringValue(raw, "reason"));
    decision = new QComboBox;
    const nlohmann::json available =
        raw.value("availableDecisions", nlohmann::json::array());
    if (available.is_array()) {
      for (const auto &entry : available) {
        if (!entry.is_string())
          continue;
        const std::string value = entry.get<std::string>();
        addChoice(decision, text(value), value.c_str());
      }
    }
    if (decision->count() == 0) {
      addChoice(decision, QStringLiteral("Approve"), "accept");
      addChoice(decision, QStringLiteral("Approve for this session"),
                "acceptForSession");
      addChoice(decision, QStringLiteral("Decline"), "decline");
      addChoice(decision, QStringLiteral("Cancel"), "cancel");
    }
    contentLayout->addWidget(wrapped(QStringLiteral("Decision"), "title"));
    contentLayout->addWidget(decision);
  } else if (request.kind == "file-change-approval") {
    addDetail(contentLayout, QStringLiteral("Reason"),
              stringValue(raw, "reason"));
    addDetail(contentLayout, QStringLiteral("Grant root"),
              stringValue(raw, "grantRoot"));
    decision = new QComboBox;
    addChoice(decision, QStringLiteral("Approve"), "accept");
    addChoice(decision, QStringLiteral("Approve for this session"),
              "acceptForSession");
    addChoice(decision, QStringLiteral("Decline"), "decline");
    addChoice(decision, QStringLiteral("Cancel"), "cancel");
    contentLayout->addWidget(wrapped(QStringLiteral("Decision"), "title"));
    contentLayout->addWidget(decision);
  } else if (request.kind == "user-input") {
    const nlohmann::json requestedQuestions =
        raw.value("questions", nlohmann::json::array());
    if (requestedQuestions.is_array()) {
      for (const auto &question : requestedQuestions) {
        QuestionEditor editor;
        editor.id = stringValue(question, "id");
        auto *section = new QFrame;
        section->setProperty("kind", "summary");
        auto *sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(12, 10, 12, 10);
        sectionLayout->setSpacing(6);
        const std::string header = stringValue(question, "header");
        if (!header.empty())
          sectionLayout->addWidget(wrapped(text(header), "title"));
        sectionLayout->addWidget(
            wrapped(text(stringValue(question, "question"))));
        const nlohmann::json options =
            question.value("options", nlohmann::json::array());
        if (options.is_array()) {
          for (const auto &option : options) {
            const std::string label = stringValue(option, "label");
            if (label.empty())
              continue;
            auto *choice = new QCheckBox(text(label));
            const std::string description = stringValue(option, "description");
            if (!description.empty())
              choice->setToolTip(text(description));
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
          if (question.value("isSecret", false))
            editor.other->setEchoMode(QLineEdit::Password);
          sectionLayout->addWidget(editor.other);
        }
        questions.push_back(std::move(editor));
        contentLayout->addWidget(section);
      }
    }
  } else if (request.kind == "mcp-elicitation") {
    const std::string message = stringValue(raw, "message");
    if (!message.empty())
      contentLayout->addWidget(wrapped(text(message)));
    const std::string url = stringValue(raw, "url");
    if (!url.empty()) {
      auto *link = wrapped(
          QStringLiteral("<a href=\"%1\">%1</a>").arg(text(url)), "body");
      link->setTextFormat(Qt::RichText);
      contentLayout->addWidget(link);
    }
    decision = new QComboBox;
    addChoice(decision, QStringLiteral("Accept"), "accept");
    addChoice(decision, QStringLiteral("Decline"), "decline");
    addChoice(decision, QStringLiteral("Cancel"), "cancel");
    contentLayout->addWidget(decision);
    if (raw.contains("requestedSchema")) {
      contentLayout->addWidget(wrapped(
          QStringLiteral("Structured response (JSON object)"), "title"));
      structuredContent = new QPlainTextEdit(QStringLiteral("{}"));
      structuredContent->setMinimumHeight(150);
      structuredContent->setStyleSheet(QStringLiteral(
          "background:#f8fafc;border:1px solid #d7dee8;border-radius:6px;"
          "padding:7px;font-family:monospace;"));
      contentLayout->addWidget(structuredContent);
    }
  } else if (request.kind == "permissions-approval") {
    addDetail(contentLayout, QStringLiteral("Reason"),
              stringValue(raw, "reason"));
    addDetail(contentLayout, QStringLiteral("Working directory"),
              stringValue(raw, "cwd"));
    contentLayout->addWidget(wrapped(
        QStringLiteral("Codex requests additional filesystem or network "
                       "permissions. Review the reason before approving.")));
    decision = new QComboBox;
    addChoice(decision, QStringLiteral("Approve for this turn"), "turn");
    addChoice(decision, QStringLiteral("Approve for this session"), "session");
    addChoice(decision, QStringLiteral("Decline"), "decline");
    contentLayout->addWidget(decision);
  } else if (request.kind == "legacy-patch-approval" ||
             request.kind == "legacy-command-approval") {
    contentLayout->addWidget(wrapped(
        QStringLiteral("This is a legacy approval request. Prefer the current "
                       "typed approval path when available.")));
    decision = new QComboBox;
    addChoice(decision, QStringLiteral("Approve"), "approved");
    addChoice(decision, QStringLiteral("Approve for this session"),
              "approved_for_session");
    addChoice(decision, QStringLiteral("Deny"), "denied");
    addChoice(decision, QStringLiteral("Abort"), "abort");
    contentLayout->addWidget(decision);
  } else {
    contentLayout->addWidget(wrapped(
        request.kind == "dynamic-tool-call"
            ? QStringLiteral("CodexUI does not implement the requested dynamic "
                             "tool. Submitting will return a typed failed tool "
                             "result.")
            : QStringLiteral("CodexUI cannot safely produce this capability. "
                             "Submitting will return an explicit JSON-RPC "
                             "unsupported error.")));
  }
  contentLayout->addStretch();

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Submit"));
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                   &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  root->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted)
    return std::nullopt;

  PendingRequestResponse response;
  if (request.kind == "command-approval" ||
      request.kind == "file-change-approval") {
    response.result = {
        {"decision", decision->currentData().toString().toStdString()}};
  } else if (request.kind == "user-input") {
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
        QMessageBox::warning(parent, QStringLiteral("Incomplete response"),
                             QStringLiteral("Answer every question before "
                                            "submitting."));
        return std::nullopt;
      }
      answers[question.id] = {{"answers", std::move(values)}};
    }
    response.result = {{"answers", std::move(answers)}};
  } else if (request.kind == "mcp-elicitation") {
    const std::string action = decision->currentData().toString().toStdString();
    nlohmann::json content = nullptr;
    if (action == "accept" && structuredContent) {
      content = nlohmann::json::parse(
          structuredContent->toPlainText().toStdString(), nullptr, false);
      if (content.is_discarded() || !content.is_object()) {
        QMessageBox::warning(parent, QStringLiteral("Invalid response"),
                             QStringLiteral("The MCP response must be a valid "
                                            "JSON object."));
        return std::nullopt;
      }
    }
    response.result = {{"action", action},
                       {"content", std::move(content)},
                       {"_meta", nullptr}};
  } else if (request.kind == "permissions-approval") {
    const std::string scope = decision->currentData().toString().toStdString();
    if (scope == "decline") {
      response.error = jsonRpcError("Permission request declined by user");
    } else {
      response.result = {
          {"permissions", raw.value("permissions", nlohmann::json::object())},
          {"scope", scope}};
    }
  } else if (request.kind == "legacy-patch-approval" ||
             request.kind == "legacy-command-approval") {
    const std::string selected =
        decision->currentData().toString().toStdString();
    if (selected == "approved" || selected == "approved_for_session")
      response.result = {{"decision", selected}};
    else if (selected == "denied")
      response.result = {
          {"decision", {{"denied", {{"rejection", "Denied by user"}}}}}};
    else
      response.result = {{"decision", "abort"}};
  } else if (request.kind == "dynamic-tool-call") {
    response.result = {
        {"contentItems",
         nlohmann::json::array(
             {{{"type", "inputText"},
               {"text", "CodexUI does not provide this dynamic tool"}}})},
        {"success", false}};
  } else {
    response.error =
        jsonRpcError("CodexUI does not support this server request");
  }
  return response;
}

PendingRequestResponse PendingRequestDialog::negativeResponse(
    const PendingRequestPresentation &request) {
  PendingRequestResponse response;
  if (request.kind == "command-approval" ||
      request.kind == "file-change-approval") {
    response.result = {{"decision", "decline"}};
  } else if (request.kind == "mcp-elicitation") {
    response.result = {
        {"action", "decline"}, {"content", nullptr}, {"_meta", nullptr}};
  } else if (request.kind == "legacy-patch-approval" ||
             request.kind == "legacy-command-approval") {
    response.result = {
        {"decision", {{"denied", {{"rejection", "Denied by user"}}}}}};
  } else if (request.kind == "dynamic-tool-call") {
    response.result = {
        {"contentItems",
         nlohmann::json::array(
             {{{"type", "inputText"}, {"text", "Request declined by user"}}})},
        {"success", false}};
  } else {
    response.error = jsonRpcError("Request declined by user");
  }
  return response;
}

} // namespace codexui::codex
