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
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyleOptionComboBox>
#include <QStyleOptionSlider>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace codexui::codex {
namespace {

constexpr int RepositoryRefreshDelayMs = 120;
constexpr int RepositoryPollingIntervalMs = 2000;

struct DiffMark {
  qreal position = 0;
  QColor color;
};

class DiffScrollBar final : public QScrollBar {
public:
  explicit DiffScrollBar(QWidget *parent = nullptr)
      : QScrollBar(Qt::Vertical, parent) {}

  void setMarks(std::vector<DiffMark> nextMarks) {
    marks = std::move(nextMarks);
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    QScrollBar::paintEvent(event);
    if (marks.empty())
      return;
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove = style()->subControlRect(
        QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarGroove, this);
    if (!groove.isValid())
      return;
    QPainter painter(this);
    painter.setPen(Qt::NoPen);
    const int width = std::min(4, groove.width());
    for (const DiffMark &mark : marks) {
      painter.setBrush(mark.color);
      const int y = groove.top() + qRound(
                                      mark.position *
                                      std::max(0, groove.height() - 2));
      painter.drawRoundedRect(groove.right() - width + 1, y, width, 2, 1, 1);
    }
  }

private:
  std::vector<DiffMark> marks;
};

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
  auto *scrollBar = new DiffScrollBar(view);
  view->setVerticalScrollBar(scrollBar);
  new DiffHighlighter(view->document());
  QObject::connect(view, &QPlainTextEdit::textChanged, view,
                   [view, scrollBar] {
                     std::vector<DiffMark> marks;
                     const int blockCount = view->document()->blockCount();
                     for (QTextBlock block = view->document()->begin();
                          block.isValid(); block = block.next()) {
                       const QString text = block.text();
                       QColor color;
                       if (text.startsWith(QStringLiteral("@@")))
                         color = QColor(QStringLiteral("#2f6feb"));
                       else if (text.startsWith(QLatin1Char('+')) &&
                                !text.startsWith(QStringLiteral("+++")))
                         color = QColor(QStringLiteral("#18865e"));
                       else if (text.startsWith(QLatin1Char('-')) &&
                                !text.startsWith(QStringLiteral("---")))
                         color = QColor(QStringLiteral("#c43d4d"));
                       if (!color.isValid())
                         continue;
                       const qreal position =
                           blockCount > 1
                               ? static_cast<qreal>(block.blockNumber()) /
                                     static_cast<qreal>(blockCount - 1)
                               : 0;
                       marks.push_back({position, color});
                     }
                     scrollBar->setMarks(std::move(marks));
                   });
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

QString repositoryName(const QString &root) {
  const QString name = QFileInfo(QDir::cleanPath(root)).fileName();
  return name.isEmpty() ? root : name;
}

QString fileTitle(const GitDiffFile &file, bool includeRepository = false) {
  QString result =
      !file.previousPath.isEmpty() && file.previousPath != file.path
          ? QStringLiteral("%1  →  %2").arg(file.previousPath, file.path)
          : file.path;
  if (includeRepository)
    result = QStringLiteral("%1 / %2")
                 .arg(repositoryName(file.repositoryRoot), result);
  return result;
}

QString repositorySummary(const GitDiffSnapshot &snapshot) {
  return snapshot.repositoryRoots.size() > 1
             ? QStringLiteral("%1 repositories")
                   .arg(snapshot.repositoryRoots.size())
             : snapshot.repositoryRoots.isEmpty()
                   ? QString{}
                   : snapshot.repositoryRoots.front();
}

QString settingsBase(const QString &threadId) {
  return QStringLiteral("diff/threads/%1")
      .arg(QString::fromLatin1(QCryptographicHash::hash(
                                   threadId.toUtf8(), QCryptographicHash::Sha256)
                                   .toHex()));
}

QStringList stringListSetting(const QString &key) {
  const QVariant stored = QSettings().value(key);
  QStringList result = stored.toStringList();
  if (result.isEmpty()) {
    const QString scalar = stored.toString();
    if (!scalar.isEmpty())
      result.push_back(scalar);
  }
  result.removeDuplicates();
  return result;
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
      if (value && (!snapshot || snapshot->files.empty()))
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

  void setSource(QString nextWorkspace, QStringList nextDirectories,
                 QStringList nextPaths, QString nextRepository,
                 bool nextIncludeHiddenRepositories,
                 GitDiffScope nextScope, QString preferredPath) {
    workspace = std::move(nextWorkspace);
    commandDirectories = std::move(nextDirectories);
    changedPaths = std::move(nextPaths);
    selectedRepository = std::move(nextRepository);
    includeHiddenRepositories = nextIncludeHiddenRepositories;
    scope = nextScope;
    requestedPath = std::move(preferredPath);
    reload();
  }

private:
  void reload() {
    provider->request(workspace, commandDirectories, changedPaths,
                      selectedRepository, includeHiddenRepositories, scope,
                      context);
  }

  void apply(const GitDiffSnapshot &value) {
    if (snapshot && *snapshot == value)
      return;
    snapshot = value;
    subtitle->setText(value.error.isEmpty()
                          ? QStringLiteral("%1  |  %2")
                                .arg(scopeName(value.scope),
                                     repositorySummary(value))
                          : value.error);
    reviewFiles->clear();
    int selected = -1;
    for (std::size_t index = 0; index < value.files.size(); ++index) {
      const GitDiffFile &file = value.files[index];
      auto *item = new QListWidgetItem(
          QStringLiteral("%1\n%2   +%3  −%4")
              .arg(fileTitle(file, value.repositoryRoots.size() > 1),
                   file.status)
              .arg(file.additions)
              .arg(file.deletions));
      item->setToolTip(file.absolutePath);
      reviewFiles->addItem(item);
      if (file.absolutePath == requestedPath)
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
    if (!snapshot || index < 0 ||
        static_cast<std::size_t>(index) >= snapshot->files.size())
      return;
    const GitDiffFile &file = snapshot->files[static_cast<std::size_t>(index)];
    requestedPath = file.absolutePath;
    title->setText(fileTitle(file, snapshot->repositoryRoots.size() > 1));
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
  std::optional<GitDiffSnapshot> snapshot;
  QString workspace;
  QStringList commandDirectories;
  QStringList changedPaths;
  QString selectedRepository;
  bool includeHiddenRepositories = false;
  QString requestedPath;
  GitDiffScope scope = GitDiffScope::Unstaged;
  GitDiffContext context = GitDiffContext::Compact;
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
  fileWatcher = new QFileSystemWatcher(this);
  refreshTimer = new QTimer(this);
  refreshTimer->setSingleShot(true);
  refreshTimer->setInterval(RepositoryRefreshDelayMs);
  repositoryTimer = new QTimer(this);
  repositoryTimer->setInterval(RepositoryPollingIntervalMs);
  repositoryTimer->start();

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(8);
  auto *filters = new QHBoxLayout;
  filters->setContentsMargins(10, 10, 10, 0);
  filters->setSpacing(8);
  repositories = new ChevronComboBox;
  repositories->setObjectName(QStringLiteral("codexDiffRepository"));
  repositories->setProperty("codexChevron", true);
  repositories->addItem(QStringLiteral("Repository"), QString{});
  repositories->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  filters->addWidget(repositories, 1);
  hiddenRepositories = new QPushButton(QStringLiteral("Hidden"));
  hiddenRepositories->setObjectName(
      QStringLiteral("codexDiffHiddenRepositories"));
  hiddenRepositories->setProperty("kind", "segment");
  hiddenRepositories->setProperty("comboPeer", true);
  hiddenRepositories->setCheckable(true);
  hiddenRepositories->setChecked(
      QSettings().value(QStringLiteral("diff/includeHiddenRepositories"), false)
          .toBool());
  hiddenRepositories->setToolTip(
      QStringLiteral("Also include hidden repositories"));
  filters->addWidget(hiddenRepositories);
  scope = new ChevronComboBox;
  scope->setObjectName(QStringLiteral("codexDiffScope"));
  scope->setProperty("codexChevron", true);
  scope->addItem(QStringLiteral("Unstaged"),
                 static_cast<int>(GitDiffScope::Unstaged));
  scope->addItem(QStringLiteral("Staged"),
                 static_cast<int>(GitDiffScope::Staged));
  scope->addItem(QStringLiteral("Since HEAD"),
                 static_cast<int>(GitDiffScope::Uncommitted));
  const int savedScope =
      QSettings().value(QStringLiteral("diff/scope"), 0).toInt();
  scope->setCurrentIndex(std::clamp(savedScope, 0, scope->count() - 1));
  scope->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  filters->addWidget(scope, 1);
  root->addLayout(filters);

  files = new QListWidget;
  files->setObjectName(QStringLiteral("codexDiffFiles"));
  files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  files->setMaximumHeight(170);
  auto *fileList = new QVBoxLayout;
  fileList->setContentsMargins(10, 0, 10, 0);
  fileList->addWidget(files);
  root->addLayout(fileList);

  auto *fileSummary = new QHBoxLayout;
  fileSummary->setContentsMargins(10, 0, 10, 0);
  fileSummary->setSpacing(8);
  summary = label(QStringLiteral("No changes"), "meta");
  summary->setObjectName(QStringLiteral("codexDiffSummary"));
  authority = label({}, "meta");
  authority->setWordWrap(false);
  authority->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  truncationSummary = label({}, "attentionSection");
  additionSummary = label({}, "diffAdditionMeta");
  deletionSummary = label({}, "diffDeletionMeta");
  fileSummary->addWidget(summary);
  fileSummary->addWidget(authority, 1);
  fileSummary->addWidget(truncationSummary);
  fileSummary->addWidget(additionSummary);
  fileSummary->addWidget(deletionSummary);
  root->addLayout(fileSummary);

  auto *previewDivider = new QFrame;
  previewDivider->setProperty("kind", "standardDivider");
  previewDivider->setFixedHeight(1);
  root->addWidget(previewDivider);

  auto *previewHeader = new QHBoxLayout;
  previewHeader->setContentsMargins(10, 0, 10, 0);
  selectedFile = label(QStringLiteral("Select a changed file"), "title");
  previewHeader->addWidget(selectedFile, 1);
  copyButton = new QPushButton(QStringLiteral("Copy"));
  copyButton->setProperty("kind", "subtle");
  copyButton->setFixedHeight(28);
  reviewButton = new QPushButton(QStringLiteral("Open review"));
  reviewButton->setProperty("comboPeer", true);
  previewHeader->addWidget(copyButton);
  previewHeader->addWidget(reviewButton);
  root->addLayout(previewHeader);

  diff = diffView(QStringLiteral("codexDiffText"));
  diff->setPlaceholderText(QStringLiteral("Select a changed file."));
  auto *diffArea = new QVBoxLayout;
  diffArea->setContentsMargins(10, 0, 10, 10);
  diffArea->addWidget(diff);
  root->addLayout(diffArea, 1);

  connect(refreshTimer, &QTimer::timeout, this, [this] {
    provider->request(workspace, repositoryCandidates(), changedPaths,
                      selectedRepository, hiddenRepositories->isChecked(),
                      scopeValue(scope), GitDiffContext::Compact);
  });
  connect(provider, &GitDiffProvider::loadingChanged, this, [this](bool loading) {
    if (loading && !snapshot) {
      summary->setText(QStringLiteral("Loading changes…"));
    }
  });
  connect(provider, &GitDiffProvider::snapshotReady, this,
          [this](const GitDiffSnapshot &value) { applySnapshot(value); });
  connect(fileWatcher, &QFileSystemWatcher::fileChanged, this,
          [this](const QString &) { refreshRepository(); });
  connect(fileWatcher, &QFileSystemWatcher::directoryChanged, this,
          [this](const QString &) { refreshRepository(); });
  connect(repositoryTimer, &QTimer::timeout, this, [this] {
    if (isVisible())
      provider->request(workspace, repositoryCandidates(), changedPaths,
                        selectedRepository, hiddenRepositories->isChecked(),
                        scopeValue(scope),
                        GitDiffContext::Compact);
  });
  connect(repositories, &QComboBox::currentIndexChanged, this,
          [this](int) {
            selectedRepository = repositories->currentData().toString();
            if (!threadId.isEmpty())
              QSettings().setValue(settingsBase(threadId) +
                                       QStringLiteral("/selected"),
                                   selectedRepository);
            refreshRepository();
          });
  connect(hiddenRepositories, &QPushButton::toggled, this, [this](bool value) {
    QSettings().setValue(QStringLiteral("diff/includeHiddenRepositories"),
                         value);
    refreshRepository();
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

void DiffViewer::setRepositoryContext(QString nextThreadId,
                                      QString nextWorkspace,
                                      QStringList nextCommandDirectories,
                                      QStringList nextChangedPaths) {
  if (!nextWorkspace.isEmpty())
    nextWorkspace = QDir::cleanPath(std::move(nextWorkspace));
  nextCommandDirectories.removeDuplicates();
  nextChangedPaths.removeDuplicates();
  if (threadId == nextThreadId && workspace == nextWorkspace &&
      commandDirectories == nextCommandDirectories &&
      changedPaths == nextChangedPaths)
    return;
  const bool changedThread = threadId != nextThreadId;
  threadId = std::move(nextThreadId);
  workspace = std::move(nextWorkspace);
  commandDirectories = std::move(nextCommandDirectories);
  changedPaths = std::move(nextChangedPaths);
  if (changedThread) {
    const QString base = settingsBase(threadId);
    persistedRepositoryRoots =
        stringListSetting(base + QStringLiteral("/roots"));
    selectedRepository =
        QSettings().value(base + QStringLiteral("/selected")).toString();
  }
  snapshot.reset();
  updateFileWatches();
  files->clear();
  diff->clear();
  refreshRepository();
}

const GitDiffSnapshot &DiffViewer::currentSnapshot() const noexcept {
  static const GitDiffSnapshot empty;
  return snapshot ? *snapshot : empty;
}

void DiffViewer::refreshRepository() {
  refreshTimer->start();
  if (reviewWindow)
    reviewWindow->setSource(workspace, repositoryCandidates(), changedPaths,
                            selectedRepository,
                            hiddenRepositories->isChecked(), scopeValue(scope),
                            selectedPath());
}

QStringList DiffViewer::repositoryCandidates() const {
  QStringList result = commandDirectories;
  result.append(persistedRepositoryRoots);
  result.removeDuplicates();
  return result;
}

QString DiffViewer::selectedPath() const {
  const int index = files->currentRow();
  if (!snapshot || index < 0 ||
      static_cast<std::size_t>(index) >= snapshot->files.size())
    return {};
  return snapshot->files[static_cast<std::size_t>(index)].absolutePath;
}

void DiffViewer::applySnapshot(const GitDiffSnapshot &value) {
  if (snapshot && *snapshot == value) {
    updateFileWatches();
    return;
  }
  const QString previous = selectedPath();
  const int previousScroll = diff->verticalScrollBar()->value();
  snapshot = value;
  updateFileWatches();
  if (!threadId.isEmpty() && !value.repositoryRoots.isEmpty()) {
    persistedRepositoryRoots = value.repositoryRoots;
    QSettings settings;
    settings.setValue(settingsBase(threadId) + QStringLiteral("/roots"),
                      persistedRepositoryRoots);
    settings.sync();
  }
  {
    const QSignalBlocker blocked(repositories);
    repositories->clear();
    if (value.repositoryRoots.size() > 1)
      repositories->addItem(QStringLiteral("All repositories"), QString{});
    for (const QString &root : value.repositoryRoots) {
      repositories->addItem(repositoryName(root), root);
      repositories->setItemData(repositories->count() - 1, root,
                                Qt::ToolTipRole);
    }
    int selectedIndex = repositories->findData(selectedRepository);
    if (selectedIndex < 0)
      selectedIndex = 0;
    repositories->setCurrentIndex(selectedIndex);
    selectedRepository = repositories->currentData().toString();
  }
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
            .arg(fileTitle(file, value.repositoryRoots.size() > 1), file.status)
            .arg(file.additions)
            .arg(file.deletions));
    item->setToolTip(file.absolutePath);
    files->addItem(item);
    if (file.absolutePath == previous)
      selected = static_cast<int>(index);
  }
  if (!value.error.isEmpty()) {
    summary->setText(QStringLiteral("Changes unavailable"));
    authority->setText(value.error);
    authority->setToolTip(value.error);
    truncationSummary->clear();
    additionSummary->clear();
    deletionSummary->clear();
  } else {
    summary->setText(
        value.files.empty()
            ? QStringLiteral("No changes")
            : value.files.size() == 1
                  ? QStringLiteral("1 changed file")
                  : QStringLiteral("%1 changed files").arg(value.files.size()));
    authority->clear();
    authority->setToolTip(QString{});
    truncationSummary->setText(value.truncated
                                   ? QStringLiteral("Display truncated")
                                   : QString{});
    additionSummary->setText(value.files.empty()
                                 ? QString{}
                                 : QStringLiteral("+%1").arg(additions));
    deletionSummary->setText(value.files.empty()
                                 ? QString{}
                                 : QStringLiteral("−%1").arg(deletions));
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

void DiffViewer::updateFileWatches() {
  QStringList desired;
  QSet<QString> desiredSet;
  const auto retain = [&desired, &desiredSet](const QString &path) {
    if (!path.isEmpty() && !desiredSet.contains(path)) {
      desired.push_back(path);
      desiredSet.insert(path);
    }
  };
  if (snapshot) {
    for (const GitDiffFile &file : snapshot->files) {
      const QFileInfo info(file.absolutePath);
      if (info.exists())
        retain(info.absoluteFilePath());
      const QString parent = info.absolutePath();
      if (QFileInfo(parent).isDir())
        retain(parent);
    }
  }
  const QStringList existing = fileWatcher->files() + fileWatcher->directories();
  const QSet<QString> existingSet(existing.begin(), existing.end());
  QStringList removed;
  for (const QString &path : existing) {
    if (!desiredSet.contains(path))
      removed.push_back(path);
  }
  if (!removed.isEmpty())
    fileWatcher->removePaths(removed);
  QStringList added;
  for (const QString &path : desired) {
    if (!existingSet.contains(path))
      added.push_back(path);
  }
  if (!added.isEmpty())
    fileWatcher->addPaths(added);
}

void DiffViewer::showSelectedFile() {
  const int index = files->currentRow();
  if (!snapshot || index < 0 ||
      static_cast<std::size_t>(index) >= snapshot->files.size()) {
    selectedFile->setText(QStringLiteral("Select a changed file"));
    diff->clear();
    return;
  }
  const GitDiffFile &file = snapshot->files[static_cast<std::size_t>(index)];
  selectedFile->setText(
      fileTitle(file, snapshot->repositoryRoots.size() > 1));
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
  reviewWindow->setSource(workspace, repositoryCandidates(), changedPaths,
                          selectedRepository, hiddenRepositories->isChecked(),
                          scopeValue(scope), selectedPath());
  reviewWindow->show();
  reviewWindow->raise();
  reviewWindow->activateWindow();
}

} // namespace codexui::codex
