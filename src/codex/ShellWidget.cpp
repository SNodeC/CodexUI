// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ShellWidget.h"

#include "codex/ConnectionDialog.h"
#include "codex/DiffViewer.h"
#include "codex/FileSelectionDialog.h"
#include "codex/FrontendSession.h"
#include "codex/NewThreadDialog.h"
#include "codex/PendingRequestDialog.h"
#include "codex/TurnSettingsWidget.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>

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

QString displayStatus(const std::string &status) {
  if (status == "inProgress" || status == "active")
    return QStringLiteral("Running");
  if (status == "completed" || status == "idle")
    return QStringLiteral("Completed");
  if (status == "failed" || status == "systemError")
    return QStringLiteral("Failed");
  if (status.empty())
    return QStringLiteral("Unknown");
  return text(status);
}

QLabel *makeLabel(QString value, const char *kind = "body") {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QLabel *makeMarkdownLabel(const QString &value) {
  QTextDocument document;
  document.setMarkdown(value, QTextDocument::MarkdownNoHTML);
  auto *label = new QLabel(document.toHtml());
  label->setProperty("kind", "body");
  label->setTextFormat(Qt::RichText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setOpenExternalLinks(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::LinksAccessibleByMouse);
  return label;
}

QFrame *makeDivider() {
  auto *divider = new QFrame;
  divider->setFixedHeight(1);
  divider->setStyleSheet(QStringLiteral("background:#d7dee8;"));
  return divider;
}

QFrame *makeStatusDot() {
  auto *dot = new QFrame;
  dot->setFixedSize(8, 8);
  dot->setStyleSheet(QStringLiteral("background:#98a2b3;border-radius:4px;"));
  return dot;
}

void clearLayout(QLayout *layout) {
  while (QLayoutItem *item = layout->takeAt(0)) {
    if (QWidget *widget = item->widget())
      widget->deleteLater();
    if (QLayout *child = item->layout()) {
      clearLayout(child);
      delete child;
    }
    delete item;
  }
}

QString joinedStrings(const nlohmann::json &value) {
  if (!value.is_array())
    return {};
  QStringList result;
  for (const auto &item : value) {
    if (item.is_string())
      result.push_back(text(item.get<std::string>()));
  }
  return result.join(QStringLiteral(", "));
}

QString messageText(const nlohmann::json &item) {
  const std::string type = stringValue(item, "type");
  if (type == "agentMessage" || type == "plan")
    return text(stringValue(item, "text"));
  if (type == "userMessage") {
    QStringList parts;
    const nlohmann::json content =
        item.value("content", nlohmann::json::array());
    if (content.is_array()) {
      for (const auto &entry : content) {
        const std::string value = stringValue(entry, "text");
        if (!value.empty())
          parts.push_back(text(value));
      }
    }
    return parts.join(QStringLiteral("\n"));
  }
  return {};
}

std::string safeMessage(const nlohmann::json &value) {
  std::string message = stringValue(value, "message");
  if (message.empty())
    message = stringValue(value, "detail");
  if (!message.empty())
    return message;
  const auto error = value.find("error");
  return error != value.end() && error->is_object()
             ? stringValue(*error, "message")
             : std::string{};
}

QFrame *itemFrame(const ItemPresentation &presentation) {
  const nlohmann::json &item = presentation.raw;
  const std::string typeName = stringValue(item, "type");
  auto *frame = new QFrame;
  frame->setProperty("kind", "raised");
  if (typeName == "userMessage") {
    frame->setStyleSheet(QStringLiteral(
        "background:#eaf2ff;border:1px solid #bfd3f9;border-radius:8px;"));
  } else if (typeName == "agentMessage") {
    frame->setStyleSheet(
        QStringLiteral("background:#ffffff;border:0;border-radius:8px;"));
  }
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(6);

  QString title;
  if (typeName == "userMessage")
    title = QStringLiteral("You");
  else if (typeName == "agentMessage")
    title = stringValue(item, "phase") == "final_answer"
                ? QStringLiteral("Codex")
                : QStringLiteral("Codex activity");
  else if (typeName == "commandExecution")
    title = QStringLiteral("Shell command");
  else if (typeName == "collabAgentToolCall" || typeName == "subAgentActivity")
    title = QStringLiteral("Agent activity");
  else if (typeName == "reasoning")
    title = QStringLiteral("Reasoning");
  else if (typeName == "fileChange")
    title = QStringLiteral("File changes");
  else
    title = text(typeName.empty() ? std::string("Activity") : typeName);
  layout->addWidget(makeLabel(title, "title"));

  const QString body = messageText(item);
  if (!body.isEmpty()) {
    layout->addWidget(typeName == "agentMessage" || typeName == "plan"
                          ? makeMarkdownLabel(body)
                          : makeLabel(body));
  }

  if (typeName == "commandExecution") {
    const QString command = text(stringValue(item, "command"));
    if (!command.isEmpty()) {
      auto *commandView = new QPlainTextEdit(command);
      commandView->setReadOnly(true);
      commandView->setMaximumHeight(90);
      commandView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
      commandView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      commandView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      commandView->setProperty("kind", "command");
      commandView->setStyleSheet(QStringLiteral(
          "background:#f8fafc;border:1px solid #d7dee8;border-radius:6px;"
          "padding:7px;font-family:monospace;font-size:11px;"));
      layout->addWidget(commandView);
    }
    const QString output = text(stringValue(item, "aggregatedOutput"));
    if (!output.isEmpty()) {
      auto *outputView = new QPlainTextEdit(output);
      outputView->setReadOnly(true);
      outputView->setMaximumHeight(220);
      outputView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
      outputView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      outputView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      outputView->setStyleSheet(QStringLiteral(
          "background:#111827;color:#e5e7eb;border-radius:6px;padding:7px;"
          "font-family:monospace;font-size:11px;"));
      layout->addWidget(outputView);
    }
    QStringList metadata;
    metadata << displayStatus(stringValue(item, "status"));
    if (item.contains("exitCode") && item["exitCode"].is_number_integer())
      metadata << QStringLiteral("exit %1").arg(item["exitCode"].get<int>());
    const QString cwd = text(stringValue(item, "cwd"));
    if (!cwd.isEmpty())
      metadata << cwd;
    layout->addWidget(
        makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));
  } else if (typeName == "collabAgentToolCall" ||
             typeName == "subAgentActivity") {
    QStringList metadata;
    const QString tool = text(stringValue(item, "tool"));
    if (!tool.isEmpty())
      metadata << tool;
    std::string status = stringValue(item, "status");
    if (status.empty())
      status = stringValue(item, "kind");
    metadata << displayStatus(status);
    const QString receivers =
        joinedStrings(item.value("receiverThreadIds", nlohmann::json::array()));
    if (!receivers.isEmpty())
      metadata << receivers;
    layout->addWidget(
        makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));
    const QString prompt = text(stringValue(item, "prompt"));
    if (!prompt.isEmpty())
      layout->addWidget(makeLabel(prompt));
    const QString result = text(stringValue(item, "resultText"));
    if (!result.isEmpty())
      layout->addWidget(makeMarkdownLabel(result));
  } else if (typeName == "reasoning") {
    const QString summaries =
        joinedStrings(item.value("summary", nlohmann::json::array()));
    if (!summaries.isEmpty())
      layout->addWidget(makeMarkdownLabel(summaries));
  } else if (typeName == "fileChange") {
    QStringList metadata;
    metadata << displayStatus(stringValue(item, "status"));
    const nlohmann::json changes =
        item.value("changes", nlohmann::json::array());
    if (changes.is_array())
      metadata << QStringLiteral("%1 paths")
                      .arg(static_cast<qulonglong>(changes.size()));
    layout->addWidget(
        makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));
  } else if (body.isEmpty()) {
    layout->addWidget(makeLabel(text(item.dump(2)), "meta"));
  }
  return frame;
}

QFrame *agentFrame(const AgentPresentation &agent) {
  const nlohmann::json &activity = agent.raw;
  auto *frame = new QFrame;
  frame->setProperty("kind", "raised");
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(6);

  const std::string tool = stringValue(activity, "tool");
  const bool childAgent = !agent.childThreadId.empty();
  const QString title = childAgent ? QStringLiteral("Subagent")
                        : tool.empty()
                            ? QStringLiteral("Agent activity")
                            : QStringLiteral("Agent %1").arg(text(tool));
  layout->addWidget(makeLabel(title, "title"));

  QStringList metadata;
  metadata << displayStatus(agent.status);
  const QString path = text(stringValue(activity, "agentPath"));
  if (!path.isEmpty())
    metadata << path;
  if (!tool.empty())
    metadata << text(tool);
  const QString model = text(stringValue(activity, "model"));
  if (!model.isEmpty())
    metadata << model;
  const QString effort = text(stringValue(activity, "reasoningEffort"));
  if (!effort.isEmpty())
    metadata << effort;
  layout->addWidget(makeLabel(metadata.join(QStringLiteral("  |  ")), "meta"));

  const QString prompt = text(stringValue(activity, "prompt"));
  if (!prompt.isEmpty())
    layout->addWidget(makeLabel(prompt));

  const QString result = text(stringValue(activity, "resultText"));
  if (!result.isEmpty())
    layout->addWidget(makeMarkdownLabel(result));

  QStringList identities;
  if (!agent.childThreadId.empty())
    identities << QStringLiteral("thread %1").arg(text(agent.childThreadId));
  const QString sender = text(stringValue(activity, "senderThreadId"));
  if (!sender.isEmpty())
    identities << QStringLiteral("sender %1").arg(sender);
  const QString receivers = joinedStrings(
      activity.value("receiverThreadIds", nlohmann::json::array()));
  if (!receivers.isEmpty())
    identities << QStringLiteral("receivers %1").arg(receivers);
  if (!identities.isEmpty())
    layout->addWidget(
        makeLabel(identities.join(QStringLiteral("  |  ")), "meta"));
  return frame;
}

} // namespace

ShellWidget::ShellWidget(FrontendSession &session, QWidget *parent)
    : QWidget(parent), session(session) {
  setObjectName(QStringLiteral("workbench"));
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto *top = new QFrame;
  top->setObjectName(QStringLiteral("topBar"));
  top->setStyleSheet(QStringLiteral(
      "QFrame#topBar{background:#ffffff;border-bottom:1px solid #d7dee8;}"));
  top->setFixedHeight(56);
  auto *topLayout = new QHBoxLayout(top);
  topLayout->setContentsMargins(20, 0, 18, 0);
  topLayout->setSpacing(12);
  auto *brand = makeLabel(QStringLiteral("CODEX WORKBENCH"), "title");
  brand->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;"));
  topLayout->addWidget(brand);
  restoreSidebarButton = new QPushButton(QStringLiteral("Show threads"));
  restoreSidebarButton->setProperty("kind", "subtle");
  restoreSidebarButton->setFixedHeight(32);
  restoreSidebarButton->hide();
  topLayout->addSpacing(12);
  topLayout->addWidget(restoreSidebarButton);
  topLayout->addSpacing(18);
  workspaceBreadcrumb = makeLabel(QStringLiteral("No workspace"), "muted");
  workspaceBreadcrumb->setWordWrap(false);
  workspaceBreadcrumb->setMaximumWidth(280);
  workspaceBreadcrumb->setStyleSheet(
      QStringLiteral("color:#667085;font-size:12px;font-weight:500;"));
  topLayout->addWidget(workspaceBreadcrumb);
  topLayout->addStretch();
  attentionButton = new QPushButton(QStringLiteral("0 requests"));
  attentionButton->setFixedSize(106, 32);
  connectionButton = new QToolButton;
  connectionButton->setText(QStringLiteral("Connection"));
  connectionButton->setProperty("kind", "subtle");
  connectionButton->setPopupMode(QToolButton::InstantPopup);
  connectionButton->setFixedHeight(32);
  auto *connectionMenu = new QMenu(connectionButton);
  connectionMenu->addAction(QStringLiteral("Configure..."), this, [this] {
    if (!model.connection().settings.is_object() ||
        model.connection().settings.empty()) {
      showNotice(QStringLiteral("Connection settings are not available yet."));
      return;
    }
    ConnectionDialog dialog(model.connection().settings, this);
    if (dialog.exec() != QDialog::Accepted)
      return;
    this->session.configureConnection(
        dialog.selection(), [this](const nlohmann::json &result) {
          if (result.value("ok", false))
            return;
          const std::string message =
              safeMessage(result.value("error", nlohmann::json::object()));
          showNotice(text(message.empty()
                              ? std::string("Connection configuration failed")
                              : message));
        });
  });
  connectionMenu->addSeparator();
  connectAction =
      connectionMenu->addAction(QStringLiteral("Connect"), this,
                                [this] { this->session.connectTransport(); });
  disconnectAction =
      connectionMenu->addAction(QStringLiteral("Disconnect"), this, [this] {
        this->session.disconnectTransport();
      });
  reconnectAction = connectionMenu->addAction(
      QStringLiteral("Reconnect"), this, [this] { this->session.reconnect(); });
  connectionButton->setMenu(connectionMenu);
  controllerButton = new QPushButton(QStringLiteral("Claim control"));
  controllerButton->setFixedHeight(32);
  restoreInspectorButton = new QPushButton(QStringLiteral("Show inspector"));
  restoreInspectorButton->setProperty("kind", "subtle");
  restoreInspectorButton->setFixedHeight(32);
  restoreInspectorButton->hide();
  connect(controllerButton, &QPushButton::clicked, this, [this] {
    if (model.connection().role == "controller")
      this->session.releaseController();
    else
      this->session.claimController();
  });
  topLayout->addWidget(attentionButton);
  topLayout->addWidget(connectionButton);
  topLayout->addWidget(controllerButton);
  topLayout->addWidget(restoreInspectorButton);
  root->addWidget(top);

  splitter = new QSplitter(Qt::Horizontal);
  splitter->setChildrenCollapsible(false);
  splitter->setHandleWidth(8);

  sidebar = new QFrame;
  sidebar->setObjectName(QStringLiteral("sidebar"));
  sidebar->setStyleSheet(QStringLiteral("QFrame#sidebar{background:#f8fafc;}"));
  sidebar->setMinimumWidth(220);
  sidebar->setMaximumWidth(440);
  auto *sidebarLayout = new QVBoxLayout(sidebar);
  sidebarLayout->setContentsMargins(10, 14, 10, 17);
  sidebarLayout->setSpacing(0);
  auto *sidebarHeader = new QHBoxLayout;
  sidebarHeader->setContentsMargins(8, 0, 6, 8);
  sidebarHeader->addWidget(makeLabel(QStringLiteral("WORK"), "section"));
  sidebarHeader->addStretch();
  auto *hideSidebarButton = new QPushButton(QStringLiteral("Hide"));
  hideSidebarButton->setProperty("kind", "subtle");
  hideSidebarButton->setFixedSize(52, 24);
  sidebarHeader->addWidget(hideSidebarButton);
  sidebarLayout->addLayout(sidebarHeader);

  auto *newButton = new QPushButton(QStringLiteral("+  New thread"));
  newButton->setFixedHeight(36);
  newButton->setStyleSheet(QStringLiteral(
      "QPushButton{background:#ffffff;color:#2f6feb;border:1px solid #bfd3f9;"
      "border-radius:8px;text-align:left;padding-left:14px;font-weight:600;}"
      "QPushButton:hover{background:#e5eeff;border-color:#2f6feb;}"
      "QPushButton:disabled{background:#f6f8fb;color:#98a2b3;"
      "border-color:#d7dee8;}"));
  sidebarLayout->addWidget(newButton);
  sidebarLayout->addSpacing(8);

  auto *threadToolbar = new QHBoxLayout;
  threadToolbar->setContentsMargins(4, 0, 4, 6);
  auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
  refreshButton->setProperty("kind", "subtle");
  refreshButton->setFixedHeight(28);
  threadToolbar->addWidget(refreshButton);
  threadToolbar->addStretch();
  sidebarLayout->addLayout(threadToolbar);
  threadList = new QListWidget;
  threadList->setObjectName(QStringLiteral("threadList"));
  threadList->setSelectionMode(QAbstractItemView::SingleSelection);
  threadList->setContextMenuPolicy(Qt::CustomContextMenu);
  threadList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  threadList->setTextElideMode(Qt::ElideRight);
  threadList->setStyleSheet(QStringLiteral(
      "QListWidget#threadList{background:transparent;border:0;outline:0;}"
      "QListWidget#threadList::item{min-height:30px;border:0;border-radius:5px;"
      "padding:2px 8px;color:#344054;}"
      "QListWidget#threadList::item:hover{background:#eef3fa;}"
      "QListWidget#threadList::item:selected{background:#e5eeff;"
      "color:#1d2633;font-weight:600;}"));
  sidebarLayout->addWidget(threadList);
  sidebarLayout->addWidget(makeDivider());
  sidebarLayout->addSpacing(18);
  auto *serverRow = new QHBoxLayout;
  serverRow->setContentsMargins(8, 0, 0, 0);
  serverRow->setSpacing(10);
  connectionStatusDot = makeStatusDot();
  serverRow->addWidget(connectionStatusDot);
  connectionLabel = makeLabel(QStringLiteral("Not connected"), "meta");
  connectionLabel->setStyleSheet(
      QStringLiteral("color:#1d2633;font-size:11px;font-weight:500;"));
  serverRow->addWidget(connectionLabel);
  serverRow->addStretch();
  sidebarLayout->addLayout(serverRow);
  connect(refreshButton, &QPushButton::clicked, this,
          [this] { requestThreads(); });
  connect(newButton, &QPushButton::clicked, this, [this] { beginNewThread(); });
  connect(hideSidebarButton, &QPushButton::clicked, this, [this] {
    sidebar->hide();
    restoreSidebarButton->show();
  });
  connect(restoreSidebarButton, &QPushButton::clicked, this, [this] {
    sidebar->show();
    restoreSidebarButton->hide();
  });
  connect(threadList, &QListWidget::itemClicked, this,
          [this](QListWidgetItem *item) {
            selectThread(item->data(Qt::UserRole).toString().toStdString());
          });
  connect(threadList, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint &position) {
            QListWidgetItem *item = threadList->itemAt(position);
            if (!item)
              return;
            const std::string threadId =
                item->data(Qt::UserRole).toString().toStdString();
            const ThreadPresentation *thread = model.thread(threadId);
            if (!thread)
              return;
            QMenu menu(threadList);
            menu.addAction(QStringLiteral("Reload"), this,
                           [this, threadId] { readThread(threadId); });
            const bool canControl = model.connection().connected &&
                                    model.connection().role == "controller";
            QAction *rename =
                menu.addAction(QStringLiteral("Rename"), this,
                               [this, threadId] { renameThread(threadId); });
            QAction *fork =
                menu.addAction(QStringLiteral("Fork"), this,
                               [this, threadId] { forkThread(threadId); });
            QAction *archive = menu.addAction(
                thread->archived ? QStringLiteral("Unarchive")
                                 : QStringLiteral("Archive"),
                this, [this, threadId] { toggleThreadArchive(threadId); });
            menu.addSeparator();
            QAction *remove =
                menu.addAction(QStringLiteral("Delete"), this,
                               [this, threadId] { deleteThread(threadId); });
            rename->setEnabled(canControl);
            fork->setEnabled(canControl);
            archive->setEnabled(canControl);
            remove->setEnabled(canControl);
            menu.exec(threadList->viewport()->mapToGlobal(position));
          });
  splitter->addWidget(sidebar);

  auto *center = new QFrame;
  center->setObjectName(QStringLiteral("conversation"));
  center->setStyleSheet(
      QStringLiteral("QFrame#conversation{background:#f6f8fb;}"));
  center->setMinimumWidth(480);
  auto *centerLayout = new QVBoxLayout(center);
  centerLayout->setContentsMargins(24, 14, 24, 12);
  centerLayout->setSpacing(0);
  auto *context = new QHBoxLayout;
  auto *threadBadge = makeLabel(QStringLiteral("THREAD"), "small");
  threadBadge->setAlignment(Qt::AlignCenter);
  threadBadge->setFixedSize(54, 18);
  threadBadge->setStyleSheet(
      QStringLiteral("background:#e5eeff;color:#2f6feb;border-radius:5px;"
                     "font-size:9px;font-weight:600;"));
  context->addWidget(threadBadge);
  context->addStretch();
  centerLayout->addLayout(context);
  centerLayout->addSpacing(2);
  conversationTitle =
      makeLabel(QStringLiteral("No synchronized thread"), "heading");
  conversationMeta = makeLabel({}, "meta");
  centerLayout->addWidget(conversationTitle);
  centerLayout->addSpacing(2);
  centerLayout->addWidget(conversationMeta);
  centerLayout->addSpacing(7);
  centerLayout->addWidget(makeDivider());
  centerLayout->addSpacing(7);

  noticeBar = new QFrame;
  noticeBar->setStyleSheet(QStringLiteral(
      "background:#fff4f2;border:1px solid #efc2bc;border-radius:6px;"));
  auto *noticeLayout = new QHBoxLayout(noticeBar);
  noticeLayout->setContentsMargins(10, 6, 8, 6);
  noticeLabel = makeLabel({}, "meta");
  noticeLabel->setStyleSheet(QStringLiteral("color:#9d2e2e;font-size:11px;"));
  auto *dismissNotice = new QPushButton(QStringLiteral("Dismiss"));
  dismissNotice->setProperty("kind", "subtle");
  dismissNotice->setFixedSize(62, 24);
  noticeLayout->addWidget(noticeLabel, 1);
  noticeLayout->addWidget(dismissNotice);
  noticeBar->hide();
  connect(dismissNotice, &QPushButton::clicked, noticeBar, &QWidget::hide);
  centerLayout->addWidget(noticeBar);

  conversationScroll = new QScrollArea;
  conversationScroll->setWidgetResizable(true);
  conversationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  conversationContent = new QWidget;
  conversationContent->setMinimumWidth(0);
  conversationContent->setSizePolicy(QSizePolicy::Ignored,
                                     QSizePolicy::Preferred);
  conversationLayout = new QVBoxLayout(conversationContent);
  conversationLayout->setContentsMargins(0, 0, 0, 16);
  conversationLayout->setSpacing(8);
  emptyConversation =
      makeLabel(QStringLiteral("Conversation activity appears here."), "muted");
  conversationLayout->addWidget(emptyConversation);
  conversationLayout->addStretch();
  conversationScroll->setWidget(conversationContent);
  centerLayout->addWidget(conversationScroll, 1);

  auto *attention = new QFrame;
  attention->setProperty("kind", "amberBadge");
  auto *attentionLayout = new QHBoxLayout(attention);
  attentionLayout->setContentsMargins(10, 6, 10, 6);
  attentionLayout->addWidget(makeLabel(
      QStringLiteral("A Codex request needs attention"), "attentionSection"));
  attentionLayout->addStretch();
  approveButton = new QPushButton(QStringLiteral("Review"));
  denyButton = new QPushButton(QStringLiteral("Deny"));
  attentionLayout->addWidget(denyButton);
  attentionLayout->addWidget(approveButton);
  connect(approveButton, &QPushButton::clicked, this,
          [this] { respondToFirstPending(true); });
  connect(denyButton, &QPushButton::clicked, this,
          [this] { respondToFirstPending(false); });
  centerLayout->addWidget(attention);

  turnSettings = new TurnSettingsWidget;
  centerLayout->addWidget(turnSettings);

  auto *composer = new QFrame;
  composer->setProperty("kind", "composer");
  auto *composerLayout = new QVBoxLayout(composer);
  composerLayout->setContentsMargins(10, 8, 8, 8);
  composerLayout->setSpacing(6);
  auto *attachmentRow = new QHBoxLayout;
  attachmentSummary = makeLabel({}, "meta");
  attachmentSummary->hide();
  clearAttachmentsButton = new QPushButton(QStringLiteral("Clear"));
  clearAttachmentsButton->setProperty("kind", "subtle");
  clearAttachmentsButton->setFixedHeight(24);
  clearAttachmentsButton->hide();
  attachmentRow->addWidget(attachmentSummary, 1);
  attachmentRow->addWidget(clearAttachmentsButton);
  composerLayout->addLayout(attachmentRow);
  auto *composerRow = new QHBoxLayout;
  composerRow->setSpacing(8);
  attachmentButton = new QPushButton(QStringLiteral("Attach"));
  attachmentButton->setProperty("kind", "subtle");
  attachmentButton->setFixedHeight(34);
  promptEditor = new codexui::ExpandingPromptEditor;
  sendButton = new QPushButton(QStringLiteral("Send"));
  sendButton->setProperty("kind", "primary");
  interruptButton = new QPushButton(QStringLiteral("Stop"));
  interruptButton->setProperty("kind", "stop");
  composerRow->addWidget(attachmentButton);
  composerRow->addWidget(promptEditor, 1);
  composerRow->addWidget(interruptButton);
  composerRow->addWidget(sendButton);
  composerLayout->addLayout(composerRow);
  centerLayout->addWidget(composer);
  connect(sendButton, &QPushButton::clicked, this, [this] { submitPrompt(); });
  connect(promptEditor, &codexui::ExpandingPromptEditor::submitRequested, this,
          [this] { submitPrompt(); });
  connect(interruptButton, &QPushButton::clicked, this,
          [this] { interruptActiveTurn(); });
  connect(attachmentButton, &QPushButton::clicked, this,
          [this] { chooseAttachments(); });
  connect(clearAttachmentsButton, &QPushButton::clicked, this, [this] {
    attachmentDrafts.clear();
    ++attachmentRevision;
    refreshAttachments();
  });
  splitter->addWidget(center);

  inspector = new QFrame;
  inspector->setObjectName(QStringLiteral("inspector"));
  inspector->setStyleSheet(
      QStringLiteral("QFrame#inspector{background:#fbfcfe;}"));
  inspector->setMinimumWidth(300);
  inspector->setMaximumWidth(520);
  auto *inspectorLayout = new QVBoxLayout(inspector);
  inspectorLayout->setContentsMargins(18, 14, 20, 0);
  inspectorLayout->setSpacing(0);
  auto *inspectorHeader = new QHBoxLayout;
  inspectorHeader->addWidget(makeLabel(QStringLiteral("INSPECTOR"), "section"));
  inspectorHeader->addStretch();
  auto *hideInspectorButton = new QPushButton(QStringLiteral("Hide"));
  hideInspectorButton->setProperty("kind", "subtle");
  hideInspectorButton->setFixedSize(58, 24);
  inspectorHeader->addWidget(hideInspectorButton);
  inspectorLayout->addLayout(inspectorHeader);
  inspectorLayout->addSpacing(7);
  inspectorTabs = new QTabWidget;
  inspectorTabs->setDocumentMode(true);
  planContent = new QWidget;
  planLayout = new QVBoxLayout(planContent);
  planLayout->setContentsMargins(12, 12, 12, 12);
  planLayout->setSpacing(8);
  agentsContent = new QWidget;
  agentsLayout = new QVBoxLayout(agentsContent);
  agentsLayout->setContentsMargins(12, 12, 12, 12);
  agentsLayout->setSpacing(8);
  diffViewer = new DiffViewer;
  requestsContent = new QWidget;
  requestsLayout = new QVBoxLayout(requestsContent);
  requestsLayout->setContentsMargins(12, 12, 12, 12);
  requestsLayout->setSpacing(8);
  auto *planScroll = new QScrollArea;
  planScroll->setWidgetResizable(true);
  planScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  planScroll->setWidget(planContent);
  auto *agentsScroll = new QScrollArea;
  agentsScroll->setWidgetResizable(true);
  agentsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  agentsScroll->setWidget(agentsContent);
  auto *requestsScroll = new QScrollArea;
  requestsScroll->setWidgetResizable(true);
  requestsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  requestsScroll->setWidget(requestsContent);
  auto *protocolContent = new QWidget;
  auto *protocolLayout = new QVBoxLayout(protocolContent);
  protocolLayout->setContentsMargins(8, 8, 8, 8);
  protocolLayout->setSpacing(6);
  protocolStats = makeLabel({}, "meta");
  protocolLog = new QPlainTextEdit;
  protocolLog->setReadOnly(true);
  protocolLog->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  protocolLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  protocolLog->document()->setMaximumBlockCount(200);
  protocolLayout->addWidget(protocolStats);
  protocolLayout->addWidget(protocolLog, 1);
  auto *stateContent = new QWidget;
  auto *stateLayout = new QVBoxLayout(stateContent);
  stateLayout->setContentsMargins(8, 8, 8, 8);
  stateView = new QPlainTextEdit;
  stateView->setReadOnly(true);
  stateView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  stateView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stateLayout->addWidget(stateView);
  infoTabs = new QTabWidget;
  infoTabs->setDocumentMode(true);
  infoTabs->addTab(stateContent, QStringLiteral("State"));
  infoTabs->addTab(protocolContent, QStringLiteral("Protocol"));
  connect(infoTabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == 0) {
      scheduleRefresh(RefreshState);
      return;
    }
    showProtocolTail();
    scheduleRefresh(RefreshProtocolStats);
  });
  inspectorTabs->addTab(planScroll, QStringLiteral("Plan"));
  inspectorTabs->addTab(agentsScroll, QStringLiteral("Agents"));
  inspectorTabs->addTab(diffViewer, QStringLiteral("Changes"));
  inspectorTabs->addTab(requestsScroll, QStringLiteral("Requests"));
  inspectorTabs->addTab(infoTabs, QStringLiteral("Info"));
  connect(inspectorTabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == 4) {
      if (infoTabs && infoTabs->currentIndex() == 1)
        showProtocolTail();
    }
    scheduleRefresh(RefreshInspector | RefreshState | RefreshProtocolStats);
  });
  inspectorLayout->addWidget(inspectorTabs, 1);
  connect(hideInspectorButton, &QPushButton::clicked, this, [this] {
    inspector->hide();
    restoreInspectorButton->show();
  });
  connect(restoreInspectorButton, &QPushButton::clicked, this, [this] {
    inspector->show();
    restoreInspectorButton->hide();
  });
  connect(attentionButton, &QPushButton::clicked, this, [this] {
    inspector->show();
    restoreInspectorButton->hide();
    inspectorTabs->setCurrentIndex(3);
  });
  splitter->addWidget(inspector);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setStretchFactor(2, 0);
  splitter->setSizes({282, 834, 404});
  root->addWidget(splitter, 1);

  auto *statusBar = new QFrame;
  statusBar->setObjectName(QStringLiteral("customStatusBar"));
  statusBar->setStyleSheet(
      QStringLiteral("QFrame#customStatusBar{background:#f8fafc;"
                     "border-top:1px solid #d7dee8;}"));
  statusBar->setFixedHeight(40);
  auto *statusLayout = new QHBoxLayout(statusBar);
  statusLayout->setContentsMargins(18, 0, 24, 0);
  statusLayout->setSpacing(8);
  bottomConnectionStatusDot = makeStatusDot();
  statusLayout->addWidget(bottomConnectionStatusDot);
  statusLayout->addWidget(makeLabel(QStringLiteral("Codex"), "meta"));
  statusLayout->addSpacing(42);
  threadContextStatus = makeLabel(QStringLiteral("No thread context"), "meta");
  statusLayout->addWidget(threadContextStatus);
  statusLayout->addSpacing(42);
  agentActivityStatus = makeLabel(QStringLiteral("No agent activity"), "meta");
  statusLayout->addWidget(agentActivityStatus);
  statusLayout->addStretch();
  controllerLabel = makeLabel(QStringLiteral("Observer"), "meta");
  statusLayout->addWidget(controllerLabel);
  root->addWidget(statusBar);

  refreshTimer = new QTimer(this);
  refreshTimer->setSingleShot(true);
  refreshTimer->setInterval(32);
  connect(refreshTimer, &QTimer::timeout, this, [this] { refresh(); });

  session.setEventHandler(
      [this](const nlohmann::json &event) { handleEvent(event); });
  refresh();
}

void ShellWidget::handleEvent(const nlohmann::json &event) {
  appendProtocolFrame(event);
  model.applyEvent(event);
  const std::string kind = stringValue(event, "kind");
  const std::string type = stringValue(event, "type");
  const nlohmann::json data = event.value("data", nlohmann::json::object());
  if (kind == "result" && !event.value("ok", false)) {
    const nlohmann::json error = event.value("error", nlohmann::json::object());
    const std::string message = safeMessage(error);
    showNotice(text(message.empty() ? std::string("Codex operation failed")
                                    : message));
  } else if (kind == "event" && type == "notice.added") {
    const nlohmann::json notice =
        data.value("notice", nlohmann::json::object());
    const std::string message = safeMessage(notice);
    if (!message.empty())
      showNotice(text(message), stringValue(data, "severity") == "error");
  } else if (kind == "event" && type == "system.diagnostic") {
    const std::string message = safeMessage(data);
    if (!message.empty())
      showNotice(QStringLiteral("Protocol diagnostic: %1").arg(text(message)));
  } else if (kind == "event" && type == "connection.lifecycle" &&
             (stringValue(data, "state") == "failure" ||
              stringValue(data, "state") == "disconnected")) {
    const std::string detail = stringValue(data, "detail");
    if (!detail.starts_with("local-"))
      showNotice(detail.empty() ? QStringLiteral("Codex bridge disconnected")
                                : text(detail));
  }
  if (event.value("kind", std::string{}) == "event" &&
      event.value("type", std::string{}) == "connection.bridge" &&
      event.value("data", nlohmann::json::object())
              .value("state", std::string{}) == "opened") {
    requestThreads();
    requestModels();
    session.listPermissionProfiles(
        {{"cwd", QDir::currentPath().toStdString()}});
  }

  hydrateHistoricalAgents();

  if (!selectedThreadId.empty() && !model.thread(selectedThreadId))
    selectedThreadId.clear();

  const nlohmann::json scope = event.value("scope", nlohmann::json::object());
  const std::string eventThreadId = stringValue(scope, "threadId");
  const std::string turnId = stringValue(scope, "turnId");
  const std::string itemId = stringValue(scope, "itemId");
  if (eventThreadId == selectedThreadId && !turnId.empty() && !itemId.empty() &&
      (type == "conversation.item.upsert" ||
       type == "conversation.item.append" ||
       type == "conversation.reasoning.part-added" ||
       type == "conversation.file-change.output-appended" ||
       type == "conversation.file-change.patch-replaced" ||
       type == "conversation.mcp.progress")) {
    const std::string key = turnId + '\x1f' + itemId;
    dirtyConversationItems[key] = {turnId, itemId};
  }

  const std::string action = stringValue(event, "action");
  if ((event.value("kind", std::string{}) == "result" &&
       action == "thread.read" && eventThreadId == selectedThreadId) ||
      type == "thread.removed") {
    conversationRebuildPending = true;
  }
  scheduleRefresh(refreshAreasForEvent(event));
}

std::uint32_t
ShellWidget::refreshAreasForEvent(const nlohmann::json &event) const {
  const std::string kind = stringValue(event, "kind");
  if (kind == "result") {
    const std::string action = stringValue(event, "action");
    if (action == "thread.read")
      return RefreshAll;
    if (action == "threads.list")
      return RefreshThreads | RefreshProtocolStats | RefreshStatus;
    if (action == "thread.create" || action == "thread.resume" ||
        action == "thread.fork")
      return RefreshThreads | RefreshTurnSettings | RefreshStatus;
    if (action == "turn.start")
      return RefreshThreads | RefreshInspector | RefreshProtocolStats |
             RefreshStatus;
    if (action == "models.list" || action == "permission-profiles.list")
      return RefreshState | RefreshProtocolStats | RefreshTurnSettings;
    return RefreshState | RefreshProtocolStats;
  }

  const std::string type = stringValue(event, "type");
  if (type.starts_with("connection."))
    return RefreshThreads | RefreshInspector | RefreshProtocolStats |
           RefreshStatus;
  if (type == "thread.upsert" || type == "thread.name.changed" ||
      type == "thread.status.changed" || type == "thread.lifecycle")
    return RefreshThreads | RefreshTurnSettings | RefreshProtocolStats |
           RefreshStatus;
  if (type == "thread.removed")
    return RefreshThreads | RefreshConversation | RefreshInspector |
           RefreshTurnSettings | RefreshProtocolStats | RefreshStatus;
  if (type == "turn.upsert")
    return RefreshThreads | RefreshInspector | RefreshProtocolStats |
           RefreshStatus;
  if (type == "plan.replaced")
    return RefreshInspector | RefreshProtocolStats;
  if (type.starts_with("conversation."))
    return RefreshConversation | RefreshInspector | RefreshProtocolStats;
  if (type == "agents.activity.upsert")
    return RefreshInspector | RefreshProtocolStats | RefreshStatus;
  if (type.starts_with("pending-request."))
    return RefreshThreads | RefreshInspector | RefreshProtocolStats |
           RefreshStatus;
  if (type == "thread.token-usage.changed")
    return RefreshProtocolStats | RefreshStatus;
  return RefreshState | RefreshProtocolStats;
}

void ShellWidget::hydrateHistoricalAgents() {
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread)
    return;
  for (const std::string &agentId : thread->agentOrder) {
    const auto agent = thread->agents.find(agentId);
    if (agent == thread->agents.end() || agent->second.childThreadId.empty() ||
        agent->second.status != "started")
      continue;
    if (!requestedAgentThreads.insert(agent->second.childThreadId).second)
      continue;
    session.readThread(agent->second.childThreadId);
  }
}

void ShellWidget::scheduleRefresh(std::uint32_t areas) {
  pendingRefreshAreas |= areas;
  if (!refreshTimer->isActive())
    refreshTimer->start();
}

void ShellWidget::refresh() {
  const std::uint32_t areas = pendingRefreshAreas;
  pendingRefreshAreas = RefreshNone;
  if ((areas & RefreshThreads) != 0)
    refreshThreads();
  if ((areas & RefreshConversation) != 0) {
    if (conversationRebuildPending) {
      refreshConversation();
    } else {
      bool requiresRebuild = false;
      for (const auto &[key, identity] : dirtyConversationItems) {
        if (!refreshConversationItem(key, identity.first, identity.second)) {
          requiresRebuild = true;
          break;
        }
      }
      if (requiresRebuild)
        refreshConversation();
    }
    dirtyConversationItems.clear();
    conversationRebuildPending = false;
  }
  if ((areas & RefreshInspector) != 0)
    refreshInspector();
  if ((areas & RefreshState) != 0)
    refreshStateInspector();
  if ((areas & RefreshProtocolStats) != 0)
    refreshProtocolStats();
  if ((areas & RefreshTurnSettings) != 0)
    refreshTurnSettings();
  if ((areas & RefreshStatus) != 0)
    refreshStatus();
}

void ShellWidget::showNotice(QString message, bool error) {
  if (message.trimmed().isEmpty())
    return;
  noticeLabel->setText(std::move(message));
  noticeBar->setStyleSheet(
      error ? QStringLiteral("background:#fff4f2;border:1px solid #efc2bc;"
                             "border-radius:6px;")
            : QStringLiteral("background:#fff8e8;border:1px solid #e5c77d;"
                             "border-radius:6px;"));
  noticeLabel->setStyleSheet(
      error ? QStringLiteral("color:#9d2e2e;font-size:11px;")
            : QStringLiteral("color:#8a5a00;font-size:11px;"));
  noticeBar->show();
}

void ShellWidget::refreshProtocolStats() {
  if (!infoTabs || inspectorTabs->currentIndex() != 4 ||
      infoTabs->currentIndex() != 1)
    return;
  std::size_t turns = 0;
  std::size_t items = 0;
  if (const ThreadPresentation *thread = model.thread(selectedThreadId)) {
    turns = thread->turnOrder.size();
    for (const auto &[turnId, turn] : thread->turns) {
      static_cast<void>(turnId);
      items += turn.itemOrder.size();
    }
  }
  protocolStats->setText(
      QStringLiteral("seq %1  |  threads %2  |  models %3  |  turns %4  |  "
                     "items %5  |  pending %6  |  telemetry %7")
          .arg(static_cast<qulonglong>(observedPresentationSequence))
          .arg(static_cast<qulonglong>(model.threadOrder().size()))
          .arg(static_cast<qulonglong>(model.modelCatalog().size()))
          .arg(static_cast<qulonglong>(turns))
          .arg(static_cast<qulonglong>(items))
          .arg(static_cast<qulonglong>(model.pendingRequestCount()))
          .arg(static_cast<qulonglong>(model.telemetry().size())));
}

void ShellWidget::showProtocolTail() {
  if (!protocolLog)
    return;
  QStringList lines;
  lines.reserve(static_cast<qsizetype>(protocolLines.size()));
  for (const QString &line : protocolLines)
    lines.push_back(line);
  protocolLog->setPlainText(lines.join(QLatin1Char('\n')));
  protocolLog->moveCursor(QTextCursor::End);
}

void ShellWidget::refreshTurnSettings() {
  nlohmann::json canonical = nlohmann::json::object();
  std::string identity = "new-thread";
  if (const ThreadPresentation *thread = model.thread(selectedThreadId)) {
    identity = thread->id;
    canonical = thread->raw;
    const auto settings = thread->domains.find("thread.settings.changed");
    if (settings != thread->domains.end() && settings->second.is_object()) {
      nlohmann::json update = settings->second;
      if (update.contains("threadSettings") &&
          update["threadSettings"].is_object())
        update = update["threadSettings"];
      canonical.merge_patch(update);
    }
  } else {
    canonical["cwd"] =
        (localNewThreadIntent && !newThreadDraftWorkspace.isEmpty()
             ? newThreadDraftWorkspace
             : QDir::currentPath())
            .toStdString();
  }

  nlohmann::json permissionProfiles = nlohmann::json::array();
  const auto profiles =
      model.globalDomains().find("operation.permission-profiles.list");
  if (profiles != model.globalDomains().end())
    permissionProfiles = profiles->second;
  turnSettings->setContext(identity, canonical, model.modelCatalog(),
                           permissionProfiles);
}

void ShellWidget::appendProtocolFrame(const nlohmann::json &frame) {
  if (!protocolLog)
    return;

  const auto recordLine = [this](QString line) {
    if (protocolLines.size() == 200)
      protocolLines.pop_front();
    protocolLines.push_back(line);
    if (inspectorTabs->currentIndex() == 4 && infoTabs &&
        infoTabs->currentIndex() == 1)
      protocolLog->appendPlainText(std::move(line));
  };

  const std::uint64_t sequence = frame.value("sequence", 0ULL);
  if (sequence != 0) {
    if (observedPresentationSequence != 0 &&
        sequence != observedPresentationSequence + 1) {
      const QString relation = sequence <= observedPresentationSequence
                                   ? QStringLiteral("NON-MONOTONIC")
                                   : QStringLiteral("SEQUENCE GAP");
      recordLine(
          QStringLiteral("[%1] %2 expected=%3 received=%4")
              .arg(QDateTime::currentDateTime().toString(
                       QStringLiteral("HH:mm:ss.zzz")),
                   relation)
              .arg(static_cast<qulonglong>(observedPresentationSequence + 1))
              .arg(static_cast<qulonglong>(sequence)));
    }
    observedPresentationSequence =
        std::max(observedPresentationSequence, sequence);
  }

  const std::string kind = stringValue(frame, "kind");
  const std::string subject = kind == "result" ? stringValue(frame, "action")
                                               : stringValue(frame, "type");
  const nlohmann::json scope = frame.value("scope", nlohmann::json::object());
  QStringList parts;
  parts << QStringLiteral("[%1]").arg(
      QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")));
  if (sequence != 0)
    parts << QStringLiteral("#%1").arg(static_cast<qulonglong>(sequence));
  parts << QStringLiteral("g%1").arg(
      static_cast<qulonglong>(frame.value("generation", 0ULL)));
  parts << text(kind);
  parts << text(subject);
  parts << text(stringValue(frame, "authority"));
  if (kind == "result")
    parts << (frame.value("ok", false) ? QStringLiteral("ok")
                                       : QStringLiteral("ERROR"));
  for (const char *key :
       {"threadId", "turnId", "itemId", "requestId", "processId"}) {
    const std::string value = stringValue(scope, key);
    if (!value.empty())
      parts << QStringLiteral("%1=%2").arg(QString::fromLatin1(key),
                                           text(value));
  }
  const std::string correlationId = stringValue(frame, "correlationId");
  if (!correlationId.empty())
    parts << QStringLiteral("correlation=%1").arg(text(correlationId));
  if (kind == "result" && !frame.value("ok", false)) {
    const nlohmann::json error = frame.value("error", nlohmann::json::object());
    const std::string message = stringValue(error, "message");
    if (!message.empty())
      parts << text(message);
  }
  recordLine(parts.join(QStringLiteral("  ")));
}

void ShellWidget::refreshThreads() {
  threadList->blockSignals(true);
  threadList->clear();
  for (const std::string &threadId : model.threadOrder()) {
    const ThreadPresentation *thread = model.thread(threadId);
    if (!thread)
      continue;
    QString title = text(thread->title);
    if (title.isEmpty())
      title = text(threadId.substr(0, 12));
    if (model.pendingRequestCount(threadId) != 0)
      title.prepend(QStringLiteral("! "));
    auto *item = new QListWidgetItem(threadList);
    item->setSizeHint(QSize(0, 44));
    item->setData(Qt::UserRole, text(threadId));
    item->setToolTip(text(thread->cwd));
    auto *row = new QWidget;
    row->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(5, 2, 5, 2);
    rowLayout->setSpacing(8);
    auto *dot = makeStatusDot();
    QString dotColor = QStringLiteral("#98a2b3");
    if (model.pendingRequestCount(threadId) != 0)
      dotColor = QStringLiteral("#a76812");
    else if (thread->status == "active" || thread->status == "inProgress")
      dotColor = QStringLiteral("#2f6feb");
    else if (thread->status == "failed" || thread->status == "systemError")
      dotColor = QStringLiteral("#b83a3a");
    dot->setStyleSheet(
        QStringLiteral("background:%1;border-radius:4px;").arg(dotColor));
    rowLayout->addWidget(dot);
    auto *copy = new QVBoxLayout;
    copy->setContentsMargins(0, 0, 0, 0);
    copy->setSpacing(1);
    auto *titleLabel = makeLabel(title, "title");
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:11px;font-weight:500;"));
    copy->addWidget(titleLabel);
    copy->addWidget(makeLabel(displayStatus(thread->status), "small"));
    rowLayout->addLayout(copy, 1);
    threadList->setItemWidget(item, row);
    if (threadId == selectedThreadId)
      threadList->setCurrentItem(item);
  }
  threadList->blockSignals(false);
}

void ShellWidget::refreshConversation() {
  const int previousMaximum =
      conversationScroll->verticalScrollBar()->maximum();
  const bool followLatest =
      conversationScroll->verticalScrollBar()->value() >= previousMaximum - 12;
  conversationCards.clear();
  clearLayout(conversationLayout);

  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread) {
    conversationTitle->setText(localNewThreadIntent
                                   ? QStringLiteral("New thread")
                                   : QStringLiteral("Select a thread"));
    conversationMeta->setText(localNewThreadIntent ? QDir::currentPath()
                                                   : QString{});
    emptyConversation =
        makeLabel(localNewThreadIntent
                      ? QStringLiteral("Send a message to create this thread.")
                      : QStringLiteral("Conversation activity appears here."),
                  "muted");
    conversationLayout->addWidget(emptyConversation);
    conversationLayout->addStretch();
    return;
  }

  conversationTitle->setText(text(thread->title));
  conversationMeta->setText(text(thread->cwd) + QStringLiteral("  |  ") +
                            displayStatus(thread->status));
  struct VisibleItem {
    std::string key;
    const ItemPresentation *item = nullptr;
  };
  std::vector<VisibleItem> items;
  for (const std::string &turnId : thread->turnOrder) {
    const auto turn = thread->turns.find(turnId);
    if (turn == thread->turns.end())
      continue;
    for (const std::string &itemId : turn->second.itemOrder) {
      const auto item = turn->second.items.find(itemId);
      if (item == turn->second.items.end())
        continue;
      items.push_back({turnId + '\x1f' + itemId, &item->second});
    }
  }
  const std::size_t first = items.size() > conversationItemLimit
                                ? items.size() - conversationItemLimit
                                : 0;
  if (first != 0) {
    const std::size_t page = std::min<std::size_t>(80, first);
    auto *loadEarlier = new QPushButton(
        QStringLiteral("Load %1 earlier activities (%2 retained)")
            .arg(static_cast<qulonglong>(page))
            .arg(static_cast<qulonglong>(first)));
    loadEarlier->setProperty("kind", "subtle");
    loadEarlier->setFixedHeight(30);
    connect(loadEarlier, &QPushButton::clicked, this, [this, page] {
      conversationItemLimit += page;
      conversationRebuildPending = true;
      scheduleRefresh(RefreshConversation);
    });
    conversationLayout->addWidget(loadEarlier);
  }
  for (std::size_t index = first; index < items.size(); ++index) {
    QWidget *card = itemFrame(*items[index].item);
    conversationCards[items[index].key] = card;
    conversationLayout->addWidget(card);
  }
  if (items.empty())
    conversationLayout->addWidget(
        makeLabel(QStringLiteral("No materialized activity."), "muted"));
  conversationLayout->addStretch();
  if (followLatest)
    QTimer::singleShot(0, conversationScroll, [scroll = conversationScroll] {
      scroll->verticalScrollBar()->setValue(
          scroll->verticalScrollBar()->maximum());
    });
}

bool ShellWidget::refreshConversationItem(const std::string &key,
                                          const std::string &turnId,
                                          const std::string &itemId) {
  const auto existing = conversationCards.find(key);
  if (existing == conversationCards.end())
    return false;
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (!thread)
    return false;
  const auto turn = thread->turns.find(turnId);
  if (turn == thread->turns.end())
    return false;
  const auto item = turn->second.items.find(itemId);
  if (item == turn->second.items.end())
    return false;

  QScrollBar *scrollBar = conversationScroll->verticalScrollBar();
  const bool followLatest = scrollBar->value() >= scrollBar->maximum() - 12;
  QWidget *replacement = itemFrame(item->second);
  QLayoutItem *replaced =
      conversationLayout->replaceWidget(existing->second, replacement);
  if (!replaced) {
    replacement->deleteLater();
    return false;
  }
  delete replaced;
  existing->second->deleteLater();
  existing->second = replacement;
  if (followLatest)
    QTimer::singleShot(0, conversationScroll, [scroll = conversationScroll] {
      scroll->verticalScrollBar()->setValue(
          scroll->verticalScrollBar()->maximum());
    });
  return true;
}

void ShellWidget::refreshInspector() {
  const int activeTab = inspectorTabs->currentIndex();
  QVBoxLayout *activeLayout = nullptr;
  if (activeTab == 0)
    activeLayout = planLayout;
  else if (activeTab == 1)
    activeLayout = agentsLayout;
  else if (activeTab == 3)
    activeLayout = requestsLayout;
  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (activeTab == 2) {
    QString liveDiff;
    std::vector<DiffFilePresentation> retained;
    if (thread) {
      for (auto turnId = thread->turnOrder.rbegin();
           turnId != thread->turnOrder.rend() && liveDiff.isEmpty(); ++turnId) {
        const auto turn = thread->turns.find(*turnId);
        if (turn == thread->turns.end())
          continue;
        const auto domain = turn->second.domains.find("turn.diff.changed");
        if (domain != turn->second.domains.end())
          liveDiff = text(stringValue(domain->second, "diff"));
      }
      if (liveDiff.isEmpty()) {
        for (auto turnId = thread->turnOrder.rbegin();
             turnId != thread->turnOrder.rend() && retained.empty(); ++turnId) {
          const auto turn = thread->turns.find(*turnId);
          if (turn == thread->turns.end())
            continue;
          for (auto itemId = turn->second.itemOrder.rbegin();
               itemId != turn->second.itemOrder.rend(); ++itemId) {
            const auto item = turn->second.items.find(*itemId);
            if (item == turn->second.items.end() ||
                stringValue(item->second.raw, "type") != "fileChange")
              continue;
            const nlohmann::json changes =
                item->second.raw.value("changes", nlohmann::json::array());
            if (!changes.is_array())
              continue;
            for (const auto &change : changes) {
              QString kind = text(stringValue(change, "kind"));
              if (kind.isEmpty() && change.contains("kind") &&
                  change["kind"].is_object())
                kind = text(stringValue(change["kind"], "type"));
              retained.push_back({text(stringValue(change, "path")),
                                  std::move(kind),
                                  text(stringValue(change, "diff"))});
            }
            if (!retained.empty())
              break;
          }
        }
      }
    }
    diffViewer->setChanges(std::move(liveDiff), std::move(retained));
    return;
  }
  if (!activeLayout)
    return;

  clearLayout(activeLayout);
  if (!thread) {
    activeLayout->addWidget(
        makeLabel(QStringLiteral("No selected thread."), "muted"));
    activeLayout->addStretch();
    return;
  }

  if (activeTab == 0) {
    const TurnPresentation *planTurn = nullptr;
    const ItemPresentation *planItem = nullptr;
    for (auto turnId = thread->turnOrder.rbegin();
         turnId != thread->turnOrder.rend(); ++turnId) {
      const auto turn = thread->turns.find(*turnId);
      if (turn == thread->turns.end())
        continue;
      if (turn->second.plan.is_object() &&
          turn->second.plan.contains("steps")) {
        planTurn = &turn->second;
        break;
      }
      for (auto itemId = turn->second.itemOrder.rbegin();
           itemId != turn->second.itemOrder.rend(); ++itemId) {
        const auto item = turn->second.items.find(*itemId);
        if (item != turn->second.items.end() &&
            stringValue(item->second.raw, "type") == "plan") {
          planItem = &item->second;
          break;
        }
      }
      if (planItem)
        break;
    }
    if (planTurn) {
      const QString explanation =
          text(stringValue(planTurn->plan, "explanation"));
      if (!explanation.isEmpty())
        planLayout->addWidget(makeMarkdownLabel(explanation));
      const nlohmann::json steps =
          planTurn->plan.value("steps", nlohmann::json::array());
      for (const auto &step : steps) {
        auto *row = new QFrame;
        row->setProperty("kind", "summary");
        auto *rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(9, 7, 9, 7);
        rowLayout->addWidget(makeLabel(text(stringValue(step, "step"))));
        rowLayout->addWidget(
            makeLabel(displayStatus(stringValue(step, "status")), "meta"));
        planLayout->addWidget(row);
      }
    } else if (planItem) {
      const QString planText = text(stringValue(planItem->raw, "text"));
      if (planText.isEmpty())
        planLayout->addWidget(
            makeLabel(QStringLiteral("Plan is being prepared."), "muted"));
      else
        planLayout->addWidget(makeMarkdownLabel(planText));
    } else {
      planLayout->addWidget(
          makeLabel(QStringLiteral("No plan for this thread."), "muted"));
    }
    planLayout->addStretch();
    return;
  }

  if (activeTab == 1) {
    std::size_t agentCount = 0;
    for (const std::string &agentId : thread->agentOrder) {
      const auto agent = thread->agents.find(agentId);
      if (agent == thread->agents.end())
        continue;
      agentsLayout->addWidget(agentFrame(agent->second));
      ++agentCount;
    }
    if (agentCount == 0)
      agentsLayout->addWidget(makeLabel(
          QStringLiteral("No agent activity for this thread."), "muted"));
    agentsLayout->addStretch();
    return;
  }

  std::size_t requestCount = 0;
  for (const auto &[id, request] : model.pendingRequestPresentations()) {
    if (request.threadId != selectedThreadId)
      continue;
    auto *frame = new QFrame;
    frame->setProperty("kind", "summary");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(9, 7, 9, 7);
    layout->setSpacing(5);
    layout->addWidget(makeLabel(text(request.kind), "title"));
    layout->addWidget(
        makeLabel(QStringLiteral("generation %1  |  request %2")
                      .arg(static_cast<qulonglong>(request.generation))
                      .arg(text(id)),
                  "meta"));
    const std::string command = stringValue(request.raw, "command");
    const std::string reason = stringValue(request.raw, "reason");
    const std::string message = stringValue(request.raw, "message");
    if (!command.empty())
      layout->addWidget(
          makeLabel(QStringLiteral("Command: %1").arg(text(command)), "meta"));
    if (!reason.empty())
      layout->addWidget(
          makeLabel(QStringLiteral("Reason: %1").arg(text(reason)), "meta"));
    if (!message.empty())
      layout->addWidget(makeLabel(text(message), "meta"));
    if (request.raw.contains("questions") &&
        request.raw["questions"].is_array())
      layout->addWidget(makeLabel(
          QStringLiteral("%1 questions")
              .arg(static_cast<qulonglong>(request.raw["questions"].size())),
          "meta"));
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 2, 0, 0);
    auto *deny = new QPushButton(QStringLiteral("Deny"));
    auto *review = new QPushButton(QStringLiteral("Review"));
    review->setProperty("kind", "primary");
    deny->setFixedHeight(28);
    review->setFixedHeight(28);
    connect(deny, &QPushButton::clicked, this,
            [this, id] { rejectPending(id); });
    connect(review, &QPushButton::clicked, this,
            [this, id] { reviewPending(id); });
    actions->addStretch();
    actions->addWidget(deny);
    actions->addWidget(review);
    layout->addLayout(actions);
    requestsLayout->addWidget(frame);
    ++requestCount;
  }
  if (requestCount == 0)
    requestsLayout->addWidget(makeLabel(
        QStringLiteral("No pending requests for this thread."), "muted"));
  requestsLayout->addStretch();
}

void ShellWidget::refreshStatus() {
  const ConnectionPresentation &connection = model.connection();
  connectionLabel->setText(connection.connected
                               ? QStringLiteral("Connected")
                               : QStringLiteral("Not connected"));
  const QString dotStyle =
      connection.connected
          ? QStringLiteral("background:#23845a;border-radius:4px;")
          : QStringLiteral("background:#98a2b3;border-radius:4px;");
  connectionStatusDot->setStyleSheet(dotStyle);
  bottomConnectionStatusDot->setStyleSheet(dotStyle);
  QString selectedTransport;
  const std::string selectedKey = stringValue(connection.settings, "selected");
  const nlohmann::json available =
      connection.settings.value("available", nlohmann::json::array());
  if (available.is_array()) {
    for (const auto &entry : available) {
      if (stringValue(entry, "key") == selectedKey) {
        selectedTransport = text(stringValue(entry, "label"));
        break;
      }
    }
  }
  connectionButton->setText(selectedTransport.isEmpty()
                                ? QStringLiteral("Connection")
                                : selectedTransport);
  connectionButton->setToolTip(
      connection.connected ? QStringLiteral("Connected bridge transport")
                           : QStringLiteral("Disconnected bridge transport"));
  connectAction->setEnabled(!connection.connected);
  disconnectAction->setEnabled(connection.connected);
  reconnectAction->setEnabled(connection.connected);
  controllerLabel->setText(connection.role.empty() ? QStringLiteral("No role")
                                                   : text(connection.role));
  controllerButton->setText(connection.role == "controller"
                                ? QStringLiteral("Release control")
                                : QStringLiteral("Claim control"));
  controllerButton->setEnabled(connection.connected);
  const std::size_t pending = model.pendingRequestCount(selectedThreadId);
  const std::size_t totalPending = model.pendingRequestCount();
  attentionButton->setText(
      QStringLiteral("%1 requests").arg(static_cast<qulonglong>(totalPending)));
  attentionButton->setStyleSheet(
      totalPending == 0 ? QString{}
                        : QStringLiteral("background:#fff6df;color:#8a5a00;"
                                         "border:1px solid #e5c77d;"));
  approveButton->parentWidget()->setVisible(pending != 0);

  const ThreadPresentation *thread = model.thread(selectedThreadId);
  if (thread) {
    const QString workspace = text(thread->cwd);
    workspaceBreadcrumb->setToolTip(workspace);
    workspaceBreadcrumb->setText(workspaceBreadcrumb->fontMetrics().elidedText(
        workspace, Qt::ElideMiddle, workspaceBreadcrumb->maximumWidth()));
    threadContextStatus->setText(
        QStringLiteral("%1  |  %2")
            .arg(text(thread->title), displayStatus(thread->status)));
    std::size_t runningAgents = 0;
    for (const auto &[agentId, agent] : thread->agents) {
      static_cast<void>(agentId);
      if (agent.status == "inProgress" || agent.status == "running" ||
          agent.status == "started")
        ++runningAgents;
    }
    agentActivityStatus->setText(
        thread->agents.empty()
            ? QStringLiteral("No agent activity")
            : QStringLiteral("%1 agents  |  %2 active")
                  .arg(static_cast<qulonglong>(thread->agents.size()))
                  .arg(static_cast<qulonglong>(runningAgents)));
  } else {
    const QString workspace =
        localNewThreadIntent
            ? text(turnSettings->workspace(QDir::currentPath().toStdString()))
            : QStringLiteral("No workspace");
    workspaceBreadcrumb->setToolTip(workspace);
    workspaceBreadcrumb->setText(workspaceBreadcrumb->fontMetrics().elidedText(
        workspace, Qt::ElideMiddle, workspaceBreadcrumb->maximumWidth()));
    threadContextStatus->setText(localNewThreadIntent
                                     ? QStringLiteral("New thread")
                                     : QStringLiteral("No thread context"));
    agentActivityStatus->setText(QStringLiteral("No agent activity"));
  }
  const bool active = model.activeTurnId(selectedThreadId).has_value();
  interruptButton->setVisible(active);
  sendButton->setText(active ? QStringLiteral("Steer")
                             : QStringLiteral("Send"));
  sendButton->setEnabled(connection.connected &&
                         connection.role == "controller");
  attachmentButton->setEnabled(connection.connected &&
                               connection.role == "controller");
  turnSettings->setControlsEnabled(connection.connected &&
                                   connection.role == "controller" && !active);
}

void ShellWidget::refreshStateInspector() {
  if (!stateView || !infoTabs || inspectorTabs->currentIndex() != 4 ||
      infoTabs->currentIndex() != 0)
    return;
  nlohmann::json domains = nlohmann::json::object();
  for (const auto &[name, value] : model.globalDomains())
    domains[name] = value;

  nlohmann::json pending = nlohmann::json::object();
  for (const auto &[id, request] : model.pendingRequestPresentations()) {
    pending[id] = {{"category", request.kind},
                   {"threadId", request.threadId},
                   {"generation", request.generation}};
  }

  const nlohmann::json state{{"models", model.modelCatalog()},
                             {"pendingRequests", std::move(pending)},
                             {"domains", std::move(domains)}};
  std::string rendered = state.dump(2);
  constexpr std::size_t MaximumRenderedStateBytes = 32U * 1024U;
  if (rendered.size() > MaximumRenderedStateBytes) {
    const std::size_t totalBytes = rendered.size();
    rendered.resize(MaximumRenderedStateBytes);
    rendered += "\n\n[State display truncated at 32 KiB; retained bytes: " +
                std::to_string(totalBytes) + "]";
  }
  stateView->setPlainText(text(rendered));
}

void ShellWidget::selectThread(std::string threadId) {
  if (threadId != selectedThreadId)
    conversationItemLimit = 80;
  selectedThreadId = std::move(threadId);
  localNewThreadIntent = false;
  newThreadDraftOptions = nlohmann::json::object();
  newThreadDraftName.clear();
  newThreadDraftWorkspace.clear();
  conversationRebuildPending = true;
  readThread(selectedThreadId);
  scheduleRefresh();
}

void ShellWidget::beginNewThread() {
  NewThreadDialog dialog(
      text(turnSettings->workspace(QDir::currentPath().toStdString())), this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  const NewThreadDraft draft = dialog.draft();
  selectedThreadId.clear();
  localNewThreadIntent = true;
  newThreadDraftName = draft.name;
  newThreadDraftWorkspace = draft.workspace;
  newThreadDraftOptions = nlohmann::json::object();
  if (!draft.baseInstructions.isEmpty())
    newThreadDraftOptions["baseInstructions"] =
        draft.baseInstructions.toStdString();
  if (!draft.developerInstructions.isEmpty())
    newThreadDraftOptions["developerInstructions"] =
        draft.developerInstructions.toStdString();
  if (draft.ephemeral)
    newThreadDraftOptions["ephemeral"] = true;
  turnSettings->setWorkspace(draft.workspace);
  conversationItemLimit = 80;
  conversationRebuildPending = true;
  threadList->clearSelection();
  promptEditor->setFocus();
  scheduleRefresh();
}

void ShellWidget::requestThreads() { session.listThreads(); }

void ShellWidget::requestModels() { session.listModels(); }

void ShellWidget::readThread(const std::string &threadId) {
  if (threadId.empty())
    return;
  session.readThread(threadId);
}

void ShellWidget::renameThread(const std::string &threadId) {
  const ThreadPresentation *thread = model.thread(threadId);
  if (!thread)
    return;
  bool accepted = false;
  const QString name =
      QInputDialog::getText(this, QStringLiteral("Rename thread"),
                            QStringLiteral("Name"), QLineEdit::Normal,
                            text(thread->title), &accepted)
          .trimmed();
  if (accepted && !name.isEmpty())
    session.renameThread(threadId, name.toStdString());
}

void ShellWidget::forkThread(const std::string &threadId) {
  if (threadId.empty())
    return;
  session.forkThread(
      threadId, nlohmann::json::object(), [this](const nlohmann::json &result) {
        if (!result.value("ok", false))
          return;
        const std::string threadId =
            stringValue(result.value("data", nlohmann::json::object())
                            .value("thread", nlohmann::json::object()),
                        "id");
        if (!threadId.empty())
          selectThread(threadId);
      });
}

void ShellWidget::toggleThreadArchive(const std::string &threadId) {
  const ThreadPresentation *thread = model.thread(threadId);
  if (!thread)
    return;
  if (thread->archived)
    session.unarchiveThread(threadId);
  else
    session.archiveThread(threadId);
}

void ShellWidget::deleteThread(const std::string &threadId) {
  if (threadId.empty())
    return;
  if (QMessageBox::question(this, QStringLiteral("Delete thread"),
                            QStringLiteral("Delete the selected thread?"),
                            QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel) == QMessageBox::Yes) {
    session.deleteThread(threadId);
  }
}

void ShellWidget::submitPrompt() {
  const QString promptValue = promptEditor->toPlainText().trimmed();
  if (promptValue.isEmpty())
    return;
  const std::string prompt = promptValue.toStdString();
  promptEditor->clear();

  if (!selectedThreadId.empty()) {
    submitPromptToThread(selectedThreadId, prompt,
                         turnSettings->turnStartOptions(), attachmentDrafts,
                         attachmentRevision);
    return;
  }
  localNewThreadIntent = true;
  nlohmann::json threadOptions = turnSettings->threadStartOptions();
  threadOptions.update(newThreadDraftOptions);
  threadOptions["cwd"] =
      turnSettings->workspace(QDir::currentPath().toStdString());
  nlohmann::json turnOptions = turnSettings->turnStartOptions();
  const std::vector<AttachmentDraft> submittedAttachments = attachmentDrafts;
  const std::uint64_t submittedAttachmentRevision = attachmentRevision;
  const QString requestedName = newThreadDraftName;
  session.createThread(
      std::move(threadOptions),
      [this, prompt, requestedName, submittedAttachments,
       submittedAttachmentRevision, turnOptions = std::move(turnOptions)](
          const nlohmann::json &result) mutable {
        if (!result.value("ok", false)) {
          const nlohmann::json error =
              result.value("error", nlohmann::json::object());
          const std::string message = safeMessage(error);
          showNotice(text(message.empty()
                              ? std::string("Thread creation failed")
                              : message));
          return;
        }
        const std::string threadId =
            stringValue(result.value("data", nlohmann::json::object())
                            .value("thread", nlohmann::json::object()),
                        "id");
        if (threadId.empty())
          return;
        selectedThreadId = threadId;
        localNewThreadIntent = false;
        newThreadDraftOptions = nlohmann::json::object();
        newThreadDraftName.clear();
        newThreadDraftWorkspace.clear();
        conversationItemLimit = 80;
        conversationRebuildPending = true;
        if (!requestedName.isEmpty())
          session.renameThread(threadId, requestedName.toStdString());
        submitPromptToThread(threadId, prompt, std::move(turnOptions),
                             submittedAttachments, submittedAttachmentRevision);
        scheduleRefresh();
      });
}

void ShellWidget::submitPromptToThread(std::string threadId, std::string prompt,
                                       nlohmann::json options,
                                       std::vector<AttachmentDraft> attachments,
                                       std::uint64_t submittedRevision) {
  nlohmann::json input =
      nlohmann::json::array({{{"type", "text"},
                              {"text", std::move(prompt)},
                              {"text_elements", nlohmann::json::array()}}});
  for (const AttachmentDraft &attachment : attachments) {
    if (attachment.mimeType.startsWith(QStringLiteral("image/"))) {
      input.push_back(
          {{"type", "localImage"}, {"path", attachment.path.toStdString()}});
    } else if (attachment.mimeType.startsWith(QStringLiteral("audio/"))) {
      input.push_back(
          {{"type", "localAudio"}, {"path", attachment.path.toStdString()}});
    } else {
      input.push_back({{"type", "mention"},
                       {"name", attachment.name.toStdString()},
                       {"path", attachment.path.toStdString()}});
    }
  }
  const auto completed = [this,
                          submittedRevision](const nlohmann::json &result) {
    if (!result.value("ok", false)) {
      const nlohmann::json error =
          result.value("error", nlohmann::json::object());
      const std::string message = safeMessage(error);
      showNotice(text(message.empty() ? std::string("Turn submission failed")
                                      : message));
      return;
    }
    if (attachmentRevision == submittedRevision) {
      attachmentDrafts.clear();
      ++attachmentRevision;
      refreshAttachments();
    }
  };
  const auto activeTurn = model.activeTurnId(threadId);
  if (activeTurn) {
    session.steerTurn(threadId, *activeTurn, std::move(input), completed);
  } else {
    session.startTurn(threadId, std::move(input), std::move(options),
                      completed);
  }
}

void ShellWidget::chooseAttachments() {
  const QString initialDirectory =
      text(turnSettings->workspace(QDir::currentPath().toStdString()));
  FileSelectionDialog dialog(FileSelectionDialog::Mode::Attachments,
                             initialDirectory, attachmentDrafts, this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  attachmentDrafts = dialog.selectedAttachments();
  ++attachmentRevision;
  refreshAttachments();
}

void ShellWidget::refreshAttachments() {
  const bool hasAttachments = !attachmentDrafts.empty();
  attachmentSummary->setVisible(hasAttachments);
  clearAttachmentsButton->setVisible(hasAttachments);
  if (!hasAttachments) {
    attachmentSummary->clear();
    attachmentSummary->setToolTip({});
    return;
  }
  QStringList names;
  for (const AttachmentDraft &attachment : attachmentDrafts)
    names.push_back(attachment.name);
  attachmentSummary->setText(
      attachmentDrafts.size() == 1
          ? QStringLiteral("1 attachment: %1").arg(names.front())
          : QStringLiteral("%1 attachments").arg(attachmentDrafts.size()));
  attachmentSummary->setToolTip(names.join(QStringLiteral("\n")));
}

void ShellWidget::interruptActiveTurn() {
  const auto turnId = model.activeTurnId(selectedThreadId);
  if (!turnId)
    return;
  session.interruptTurn(selectedThreadId, *turnId);
}

void ShellWidget::respondToFirstPending(bool approve) {
  const auto &pending = model.pendingRequestPresentations();
  const auto request =
      std::find_if(pending.begin(), pending.end(), [this](const auto &entry) {
        return entry.second.threadId == selectedThreadId;
      });
  if (request == pending.end())
    return;

  if (approve)
    reviewPending(request->first);
  else
    rejectPending(request->first);
}

void ShellWidget::reviewPending(const std::string &requestKey) {
  const auto request = model.pendingRequestPresentations().find(requestKey);
  if (request == model.pendingRequestPresentations().end())
    return;
  const auto response = PendingRequestDialog::present(request->second, this);
  if (!response)
    return;
  session.respondToServerRequest(nlohmann::json::parse(requestKey),
                                 response->result, response->error);
}

void ShellWidget::rejectPending(const std::string &requestKey) {
  const auto request = model.pendingRequestPresentations().find(requestKey);
  if (request == model.pendingRequestPresentations().end())
    return;
  PendingRequestResponse response =
      PendingRequestDialog::negativeResponse(request->second);
  session.respondToServerRequest(nlohmann::json::parse(requestKey),
                                 std::move(response.result),
                                 std::move(response.error));
}

} // namespace codexui::codex
