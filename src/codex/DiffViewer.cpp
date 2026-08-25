// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/DiffViewer.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyleOptionComboBox>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace codexui::codex {
namespace {

constexpr int RepositoryRefreshDelayMs = 120;
constexpr int RepositoryPollingIntervalMs = 2000;

class ChevronComboBox final : public QComboBox {
protected:
  void paintEvent(QPaintEvent *event) override {
    QComboBox::paintEvent(event);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const QRect indicator = style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
    UiStyle::drawChevron(
        this, indicator, option.state & QStyle::State_Enabled,
        option.state & (QStyle::State_MouseOver | QStyle::State_HasFocus));
  }
};

class DiffHighlighter final : public QSyntaxHighlighter {
public:
  explicit DiffHighlighter(QTextDocument *document)
      : QSyntaxHighlighter(document) {}

protected:
  void highlightBlock(const QString &text) override {
    QTextCharFormat format;
    if (text.startsWith(QStringLiteral("@@"))) {
      format.setForeground(QColor(QStringLiteral("#2f6feb")));
      format.setBackground(QColor(QStringLiteral("#edf3ff")));
      format.setFontWeight(QFont::DemiBold);
    } else if (text.startsWith(QLatin1Char('+')) &&
               !text.startsWith(QStringLiteral("+++"))) {
      format.setForeground(QColor(QStringLiteral("#176b45")));
      format.setBackground(QColor(QStringLiteral("#e9f7f0")));
    } else if (text.startsWith(QLatin1Char('-')) &&
               !text.startsWith(QStringLiteral("---"))) {
      format.setForeground(QColor(QStringLiteral("#982f3d")));
      format.setBackground(QColor(QStringLiteral("#fff0f2")));
    } else if (text.startsWith(QStringLiteral("diff --git")) ||
               text.startsWith(QStringLiteral("---")) ||
               text.startsWith(QStringLiteral("+++")) ||
               text.startsWith(QStringLiteral("Binary files")) ||
               text.startsWith(QStringLiteral("GIT binary patch"))) {
      format.setForeground(QColor(QStringLiteral("#344054")));
      format.setFontWeight(QFont::DemiBold);
    } else {
      return;
    }
    setFormat(0, text.size(), format);
  }
};

QLabel *label(QString value, const char *kind) {
  auto *result = new QLabel(std::move(value));
  result->setProperty("kind", kind);
  result->setWordWrap(true);
  result->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return result;
}

QPlainTextEdit *diffView(const QString &objectName) {
  auto *view = new QPlainTextEdit;
  view->setObjectName(objectName);
  view->setProperty("kind", "infoViewer");
  view->setReadOnly(true);
  view->setLineWrapMode(QPlainTextEdit::NoWrap);
  view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  view->verticalScrollBar()->setProperty("kind", "infoViewer");
  new DiffHighlighter(view->document());
  return view;
}

GitDiffScope scopeValue(const QComboBox *scope) {
  return static_cast<GitDiffScope>(scope->currentData().toInt());
}

QString scopeName(GitDiffScope scope) {
  switch (scope) {
  case GitDiffScope::Staged:
    return QStringLiteral("Staged changes");
  case GitDiffScope::Uncommitted:
    return QStringLiteral("All changes since HEAD");
  default:
    return QStringLiteral("Unstaged changes");
  }
}

QString fileTitle(const GitDiffFile &file) {
  return !file.previousPath.isEmpty() && file.previousPath != file.path
             ? QStringLiteral("%1  →  %2").arg(file.previousPath, file.path)
             : file.path;
}

QByteArray fingerprint(const GitDiffSnapshot &snapshot) {
  QByteArray value = snapshot.repositoryRoot.toUtf8();
  value += '\0';
  value += snapshot.error.toUtf8();
  value += static_cast<char>(snapshot.scope);
  value += static_cast<char>(snapshot.context);
  value += snapshot.repository ? '\1' : '\0';
  value += snapshot.truncated ? '\1' : '\0';
  for (const GitDiffFile &file : snapshot.files) {
    value += '\0';
    value += file.path.toUtf8();
    value += '\0';
    value += file.previousPath.toUtf8();
    value += '\0';
    value += file.status.toUtf8();
    value += '\0';
    value += file.patch.toUtf8();
  }
  return QCryptographicHash::hash(value, QCryptographicHash::Sha256);
}

struct SideBySideText {
  QString left;
  QString right;
};

QString sideLine(QChar marker, int number, const QString &content) {
  return QStringLiteral("%1%2 │ %3")
      .arg(marker)
      .arg(number > 0 ? QString::number(number).rightJustified(6)
                      : QString(6, QLatin1Char(' ')))
      .arg(content);
}

SideBySideText sideBySide(const QString &patch) {
  QStringList left;
  QStringList right;
  const QStringList lines = patch.split(QLatin1Char('\n'));
  static const QRegularExpression hunk(
      QStringLiteral(R"(^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@)"));
  int oldLine = 0;
  int newLine = 0;
  for (qsizetype index = 0; index < lines.size();) {
    const QString &line = lines[index];
    const QRegularExpressionMatch match = hunk.match(line);
    if (match.hasMatch()) {
      oldLine = match.captured(1).toInt();
      newLine = match.captured(2).toInt();
      left << line;
      right << line;
      ++index;
      continue;
    }
    if (line.startsWith(QLatin1Char('-')) &&
        !line.startsWith(QStringLiteral("---"))) {
      QStringList removed;
      QStringList added;
      while (index < lines.size() &&
             lines[index].startsWith(QLatin1Char('-')) &&
             !lines[index].startsWith(QStringLiteral("---")))
        removed << lines[index++].mid(1);
      while (index < lines.size() &&
             lines[index].startsWith(QLatin1Char('+')) &&
             !lines[index].startsWith(QStringLiteral("+++")))
        added << lines[index++].mid(1);
      const qsizetype count = std::max(removed.size(), added.size());
      for (qsizetype row = 0; row < count; ++row) {
        const bool hasOld = row < removed.size();
        const bool hasNew = row < added.size();
        left << sideLine(hasOld ? QLatin1Char('-') : QLatin1Char(' '),
                         hasOld ? oldLine++ : 0,
                         hasOld ? removed[row] : QString{});
        right << sideLine(hasNew ? QLatin1Char('+') : QLatin1Char(' '),
                          hasNew ? newLine++ : 0,
                          hasNew ? added[row] : QString{});
      }
      continue;
    }
    if (line.startsWith(QLatin1Char('+')) &&
        !line.startsWith(QStringLiteral("+++"))) {
      left << sideLine(QLatin1Char(' '), 0, {});
      right << sideLine(QLatin1Char('+'), newLine++, line.mid(1));
    } else if (line.startsWith(QLatin1Char(' ')) && oldLine > 0 &&
               newLine > 0) {
      left << sideLine(QLatin1Char(' '), oldLine++, line.mid(1));
      right << sideLine(QLatin1Char(' '), newLine++, line.mid(1));
    } else {
      left << line;
      right << line;
    }
    ++index;
  }
  return {left.join(QLatin1Char('\n')), right.join(QLatin1Char('\n'))};
}

QPushButton *modeButton(const QString &text) {
  auto *button = new QPushButton(text);
  button->setCheckable(true);
  button->setProperty("kind", "segment");
  button->setFixedHeight(30);
  return button;
}

} // namespace

class GitDiffReviewWindow final : public QDialog {
public:
  explicit GitDiffReviewWindow(QWidget *parent = nullptr) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Change Review"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    resize(1200, 780);
    provider = new GitDiffProvider(this);
    repositoryTimer = new QTimer(this);
    repositoryTimer->setInterval(RepositoryPollingIntervalMs);
    repositoryTimer->start();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);
    auto *header = new QHBoxLayout;
    title = label(QStringLiteral("Change Review"), "heading");
    subtitle = label({}, "meta");
    auto *titles = new QVBoxLayout;
    titles->setSpacing(1);
    titles->addWidget(title);
    titles->addWidget(subtitle);
    header->addLayout(titles, 1);

    unified = modeButton(QStringLiteral("Unified"));
    split = modeButton(QStringLiteral("Side by side"));
    auto *layoutModes = new QButtonGroup(this);
    layoutModes->setExclusive(true);
    layoutModes->addButton(unified);
    layoutModes->addButton(split);
    header->addWidget(unified);
    header->addWidget(split);
    header->addSpacing(8);
    compact = modeButton(QStringLiteral("Compact"));
    expanded = modeButton(QStringLiteral("Expanded"));
    auto *contextModes = new QButtonGroup(this);
    contextModes->setExclusive(true);
    contextModes->addButton(compact);
    contextModes->addButton(expanded);
    header->addWidget(compact);
    header->addWidget(expanded);
    root->addLayout(header);

    auto *body = new QSplitter;
    reviewFiles = new QListWidget;
    reviewFiles->setMinimumWidth(230);
    reviewFiles->setMaximumWidth(420);
    body->addWidget(reviewFiles);
    views = new QStackedWidget;
    unifiedView = diffView(QStringLiteral("codexReviewUnified"));
    views->addWidget(unifiedView);
    auto *sides = new QSplitter;
    leftView = diffView(QStringLiteral("codexReviewBefore"));
    rightView = diffView(QStringLiteral("codexReviewAfter"));
    sides->addWidget(leftView);
    sides->addWidget(rightView);
    sides->setSizes({600, 600});
    views->addWidget(sides);
    body->addWidget(views);
    body->setStretchFactor(1, 1);
    root->addWidget(body, 1);

    connect(reviewFiles, &QListWidget::currentRowChanged, this,
            [this] { renderSelected(); });
    connect(unified, &QPushButton::clicked, this, [this] {
      views->setCurrentIndex(0);
      QSettings().setValue(QStringLiteral("diff/layout"),
                           QStringLiteral("unified"));
      renderSelected();
    });
    connect(split, &QPushButton::clicked, this, [this] {
      views->setCurrentIndex(1);
      QSettings().setValue(QStringLiteral("diff/layout"),
                           QStringLiteral("side-by-side"));
      renderSelected();
    });
    connect(compact, &QPushButton::clicked, this, [this] {
      context = GitDiffContext::Compact;
      QSettings().setValue(QStringLiteral("diff/context"),
                           QStringLiteral("compact"));
      reload();
    });
    connect(expanded, &QPushButton::clicked, this, [this] {
      context = GitDiffContext::Expanded;
      QSettings().setValue(QStringLiteral("diff/context"),
                           QStringLiteral("expanded"));
      reload();
    });
    connect(provider, &GitDiffProvider::loadingChanged, this, [this](bool value) {
      if (value && snapshot.files.empty())
        subtitle->setText(QStringLiteral("Loading repository changes…"));
    });
    connect(provider, &GitDiffProvider::snapshotReady, this,
            [this](const GitDiffSnapshot &value) { apply(value); });
    connect(repositoryTimer, &QTimer::timeout, this, [this] {
      if (isVisible())
        reload();
    });
    connect(leftView->verticalScrollBar(), &QScrollBar::valueChanged,
            rightView->verticalScrollBar(), &QScrollBar::setValue);
    connect(rightView->verticalScrollBar(), &QScrollBar::valueChanged,
            leftView->verticalScrollBar(), &QScrollBar::setValue);
    connect(leftView->horizontalScrollBar(), &QScrollBar::valueChanged,
            rightView->horizontalScrollBar(), &QScrollBar::setValue);
    connect(rightView->horizontalScrollBar(), &QScrollBar::valueChanged,
            leftView->horizontalScrollBar(), &QScrollBar::setValue);

    const QSettings settings;
    const bool side = settings.value(QStringLiteral("diff/layout"),
                                     QStringLiteral("unified")) ==
                      QStringLiteral("side-by-side");
    unified->setChecked(!side);
    split->setChecked(side);
    views->setCurrentIndex(side ? 1 : 0);
    const bool full = settings.value(QStringLiteral("diff/context"),
                                     QStringLiteral("compact")) ==
                      QStringLiteral("expanded");
    compact->setChecked(!full);
    expanded->setChecked(full);
    context = full ? GitDiffContext::Expanded : GitDiffContext::Compact;
  }

  void setSource(QString nextWorkspace, GitDiffScope nextScope,
                 QString preferredPath) {
    workspace = std::move(nextWorkspace);
    scope = nextScope;
    requestedPath = std::move(preferredPath);
    reload();
  }

private:
  void reload() { provider->request(workspace, scope, context); }

  void apply(const GitDiffSnapshot &value) {
    const QByteArray nextFingerprint = fingerprint(value);
    if (nextFingerprint == snapshotFingerprint)
      return;
    snapshotFingerprint = nextFingerprint;
    snapshot = value;
    subtitle->setText(value.error.isEmpty()
                          ? QStringLiteral("%1  |  %2")
                                .arg(scopeName(value.scope),
                                     value.repositoryRoot)
                          : value.error);
    reviewFiles->clear();
    int selected = -1;
    for (std::size_t index = 0; index < value.files.size(); ++index) {
      const GitDiffFile &file = value.files[index];
      auto *item = new QListWidgetItem(
          QStringLiteral("%1\n%2   +%3  −%4")
              .arg(fileTitle(file), file.status)
              .arg(file.additions)
              .arg(file.deletions));
      item->setToolTip(file.path);
      reviewFiles->addItem(item);
      if (file.path == requestedPath)
        selected = static_cast<int>(index);
    }
    if (!value.files.empty())
      reviewFiles->setCurrentRow(selected >= 0 ? selected : 0);
    else {
      unifiedView->setPlainText(value.error.isEmpty()
                                    ? QStringLiteral("No file changes")
                                    : value.error);
      leftView->clear();
      rightView->clear();
    }
  }

  void renderSelected() {
    const int index = reviewFiles->currentRow();
    if (index < 0 || static_cast<std::size_t>(index) >= snapshot.files.size())
      return;
    const GitDiffFile &file = snapshot.files[static_cast<std::size_t>(index)];
    requestedPath = file.path;
    title->setText(fileTitle(file));
    const QString content = file.patch.isEmpty()
                                ? QStringLiteral("No textual patch is available for this file.")
                                : file.patch;
    unifiedView->setPlainText(content);
    unifiedView->moveCursor(QTextCursor::Start);
    const SideBySideText sides = sideBySide(content);
    leftView->setPlainText(sides.left);
    rightView->setPlainText(sides.right);
    leftView->moveCursor(QTextCursor::Start);
    rightView->moveCursor(QTextCursor::Start);
  }

  GitDiffProvider *provider = nullptr;
  GitDiffSnapshot snapshot;
  QString workspace;
  QString requestedPath;
  GitDiffScope scope = GitDiffScope::Unstaged;
  GitDiffContext context = GitDiffContext::Compact;
  QByteArray snapshotFingerprint;
  QLabel *title = nullptr;
  QLabel *subtitle = nullptr;
  QListWidget *reviewFiles = nullptr;
  QStackedWidget *views = nullptr;
  QPlainTextEdit *unifiedView = nullptr;
  QPlainTextEdit *leftView = nullptr;
  QPlainTextEdit *rightView = nullptr;
  QPushButton *unified = nullptr;
  QPushButton *split = nullptr;
  QPushButton *compact = nullptr;
  QPushButton *expanded = nullptr;
  QTimer *repositoryTimer = nullptr;
};

DiffViewer::DiffViewer(QWidget *parent) : QWidget(parent) {
  provider = new GitDiffProvider(this);
  refreshTimer = new QTimer(this);
  refreshTimer->setSingleShot(true);
  refreshTimer->setInterval(RepositoryRefreshDelayMs);
  repositoryTimer = new QTimer(this);
  repositoryTimer->setInterval(RepositoryPollingIntervalMs);
  repositoryTimer->start();

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(10, 10, 10, 10);
  root->setSpacing(8);
  auto *header = new QHBoxLayout;
  summary = label(QStringLiteral("No file changes"), "title");
  authority = label({}, "meta");
  auto *headerText = new QVBoxLayout;
  headerText->setSpacing(1);
  headerText->addWidget(summary);
  headerText->addWidget(authority);
  header->addLayout(headerText, 1);
  scope = new ChevronComboBox;
  scope->setObjectName(QStringLiteral("codexDiffScope"));
  scope->setProperty("codexChevron", true);
  scope->addItem(QStringLiteral("Unstaged"),
                 static_cast<int>(GitDiffScope::Unstaged));
  scope->addItem(QStringLiteral("Staged"),
                 static_cast<int>(GitDiffScope::Staged));
  scope->addItem(QStringLiteral("Since HEAD"),
                 static_cast<int>(GitDiffScope::Uncommitted));
  scope->setFixedHeight(30);
  const int savedScope =
      QSettings().value(QStringLiteral("diff/scope"), 0).toInt();
  scope->setCurrentIndex(std::clamp(savedScope, 0, scope->count() - 1));
  header->addWidget(scope);
  root->addLayout(header);

  files = new QListWidget;
  files->setObjectName(QStringLiteral("codexDiffFiles"));
  files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  files->setMaximumHeight(170);
  root->addWidget(files);

  auto *previewHeader = new QHBoxLayout;
  selectedFile = label(QStringLiteral("Select a changed file"), "title");
  previewHeader->addWidget(selectedFile, 1);
  copyButton = new QPushButton(QStringLiteral("Copy"));
  copyButton->setProperty("kind", "subtle");
  copyButton->setFixedHeight(28);
  reviewButton = new QPushButton(QStringLiteral("Open review"));
  reviewButton->setFixedHeight(28);
  previewHeader->addWidget(copyButton);
  previewHeader->addWidget(reviewButton);
  root->addLayout(previewHeader);

  diff = diffView(QStringLiteral("codexDiffText"));
  diff->setPlaceholderText(QStringLiteral("Select a changed file."));
  root->addWidget(diff, 1);

  connect(refreshTimer, &QTimer::timeout, this, [this] {
    provider->request(workspace, scopeValue(scope), GitDiffContext::Compact);
  });
  connect(provider, &GitDiffProvider::loadingChanged, this, [this](bool loading) {
    if (loading && snapshot.files.empty() && snapshot.error.isEmpty()) {
      summary->setText(QStringLiteral("Loading changes…"));
    }
  });
  connect(provider, &GitDiffProvider::snapshotReady, this,
          [this](const GitDiffSnapshot &value) { applySnapshot(value); });
  connect(repositoryTimer, &QTimer::timeout, this, [this] {
    if (isVisible())
      provider->request(workspace, scopeValue(scope), GitDiffContext::Compact);
  });
  connect(scope, &QComboBox::currentIndexChanged, this, [this](int index) {
    QSettings().setValue(QStringLiteral("diff/scope"), index);
    refreshRepository();
  });
  connect(files, &QListWidget::currentRowChanged, this,
          [this] { showSelectedFile(); });
  connect(files, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *) { openReview(); });
  connect(copyButton, &QPushButton::clicked, this, [this] {
    if (!diff->toPlainText().isEmpty())
      QApplication::clipboard()->setText(diff->toPlainText());
  });
  connect(reviewButton, &QPushButton::clicked, this,
          [this] { openReview(); });
  copyButton->setEnabled(false);
  reviewButton->setEnabled(false);
}

void DiffViewer::setWorkspace(QString nextWorkspace) {
  nextWorkspace = QDir::cleanPath(std::move(nextWorkspace));
  if (workspace == nextWorkspace)
    return;
  workspace = std::move(nextWorkspace);
  snapshot = {};
  snapshotFingerprint.clear();
  files->clear();
  diff->clear();
  refreshRepository();
}

void DiffViewer::refreshRepository() {
  refreshTimer->start();
  if (reviewWindow)
    reviewWindow->setSource(workspace, scopeValue(scope), selectedPath());
}

QString DiffViewer::selectedPath() const {
  const int index = files->currentRow();
  return index >= 0 && static_cast<std::size_t>(index) < snapshot.files.size()
             ? snapshot.files[static_cast<std::size_t>(index)].path
             : QString{};
}

void DiffViewer::applySnapshot(const GitDiffSnapshot &value) {
  const QByteArray nextFingerprint = fingerprint(value);
  if (nextFingerprint == snapshotFingerprint)
    return;
  snapshotFingerprint = nextFingerprint;
  const QString previous = selectedPath();
  const int previousScroll = diff->verticalScrollBar()->value();
  snapshot = value;
  files->clear();
  int additions = 0;
  int deletions = 0;
  int selected = -1;
  for (std::size_t index = 0; index < value.files.size(); ++index) {
    const GitDiffFile &file = value.files[index];
    additions += file.additions;
    deletions += file.deletions;
    auto *item = new QListWidgetItem(
        QStringLiteral("%1   %2   +%3  −%4")
            .arg(fileTitle(file), file.status)
            .arg(file.additions)
            .arg(file.deletions));
    item->setToolTip(file.path);
    files->addItem(item);
    if (file.path == previous)
      selected = static_cast<int>(index);
  }
  if (!value.error.isEmpty()) {
    summary->setText(QStringLiteral("Changes unavailable"));
    authority->setText(value.error);
  } else {
    summary->setText(value.files.empty()
                         ? QStringLiteral("No file changes")
                         : QStringLiteral("%1 files   +%2  −%3")
                               .arg(value.files.size())
                               .arg(additions)
                               .arg(deletions));
    authority->setText(
        value.truncated
            ? QStringLiteral("%1  |  display truncated  |  %2")
                  .arg(scopeName(value.scope), value.repositoryRoot)
            : QStringLiteral("%1  |  %2")
                  .arg(scopeName(value.scope), value.repositoryRoot));
  }
  if (!value.files.empty()) {
    files->setCurrentRow(selected >= 0 ? selected : 0);
    if (selected >= 0)
      diff->verticalScrollBar()->setValue(previousScroll);
  } else {
    selectedFile->setText(QStringLiteral("Select a changed file"));
    diff->setPlainText(value.error);
  }
  copyButton->setEnabled(!value.files.empty());
  reviewButton->setEnabled(!value.files.empty());
}

void DiffViewer::showSelectedFile() {
  const int index = files->currentRow();
  if (index < 0 || static_cast<std::size_t>(index) >= snapshot.files.size()) {
    selectedFile->setText(QStringLiteral("Select a changed file"));
    diff->clear();
    return;
  }
  const GitDiffFile &file = snapshot.files[static_cast<std::size_t>(index)];
  selectedFile->setText(fileTitle(file));
  diff->setPlainText(file.patch.isEmpty()
                         ? QStringLiteral("No textual patch is available for this file.")
                         : file.patch);
  diff->moveCursor(QTextCursor::Start);
}

void DiffViewer::openReview() {
  if (selectedPath().isEmpty())
    return;
  if (!reviewWindow)
    reviewWindow = new GitDiffReviewWindow(window());
  reviewWindow->setSource(workspace, scopeValue(scope), selectedPath());
  reviewWindow->show();
  reviewWindow->raise();
  reviewWindow->activateWindow();
}

} // namespace codexui::codex
