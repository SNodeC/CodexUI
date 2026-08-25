// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/DiffViewer.h"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVBoxLayout>

namespace codexui::codex {
namespace {

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
               text.startsWith(QStringLiteral("+++"))) {
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
  return result;
}

QString pathFromHeader(QString line) {
  if (line.startsWith(QStringLiteral("+++ ")))
    line.remove(0, 4);
  line = line.section(QLatin1Char('\t'), 0, 0).trimmed();
  if (line.startsWith(QStringLiteral("b/")))
    line.remove(0, 2);
  return line == QStringLiteral("/dev/null") ? QString{} : line;
}

void countLines(const QString &content, int &additions, int &deletions) {
  additions = 0;
  deletions = 0;
  const QStringList lines = content.split(QLatin1Char('\n'));
  for (const QString &line : lines) {
    if (line.startsWith(QLatin1Char('+')) &&
        !line.startsWith(QStringLiteral("+++")))
      ++additions;
    else if (line.startsWith(QLatin1Char('-')) &&
             !line.startsWith(QStringLiteral("---")))
      ++deletions;
  }
}

} // namespace

DiffViewer::DiffViewer(QWidget *parent) : QWidget(parent) {
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
  copyButton = new QPushButton(QStringLiteral("Copy"));
  copyButton->setProperty("kind", "subtle");
  copyButton->setFixedHeight(28);
  expandButton = new QPushButton(QStringLiteral("Expand"));
  expandButton->setFixedHeight(28);
  header->addWidget(copyButton);
  header->addWidget(expandButton);
  root->addLayout(header);

  files = new QListWidget;
  files->setObjectName(QStringLiteral("codexDiffFiles"));
  files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  files->setMaximumHeight(150);
  root->addWidget(files);

  diff = new QPlainTextEdit;
  diff->setObjectName(QStringLiteral("codexDiffText"));
  diff->setReadOnly(true);
  diff->setLineWrapMode(QPlainTextEdit::NoWrap);
  diff->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  diff->setPlaceholderText(QStringLiteral("Select a changed file."));
  new DiffHighlighter(diff->document());
  root->addWidget(diff, 1);

  connect(files, &QListWidget::currentRowChanged, this,
          [this] { showSelectedFile(); });
  connect(copyButton, &QPushButton::clicked, this, [this] {
    if (!diff->toPlainText().isEmpty())
      QApplication::clipboard()->setText(diff->toPlainText());
  });
  connect(expandButton, &QPushButton::clicked, this,
          [this] { showExpanded(); });
  copyButton->setEnabled(false);
  expandButton->setEnabled(false);
}

void DiffViewer::setChanges(QString liveDiff,
                            std::vector<DiffFilePresentation> retainedChanges) {
  QByteArray fingerprintInput = liveDiff.toUtf8();
  for (const DiffFilePresentation &change : retainedChanges) {
    fingerprintInput += '\0';
    fingerprintInput += change.path.toUtf8();
    fingerprintInput += '\0';
    fingerprintInput += change.kind.toUtf8();
    fingerprintInput += '\0';
    fingerprintInput += change.diff.toUtf8();
  }
  const QByteArray fingerprint =
      QCryptographicHash::hash(fingerprintInput, QCryptographicHash::Sha256);
  if (fingerprint == contentFingerprint)
    return;
  contentFingerprint = fingerprint;

  const bool live = !liveDiff.isEmpty();
  fileDiffs = live ? parseUnifiedDiff(liveDiff) : std::vector<FileDiff>{};
  if (!live) {
    fileDiffs.reserve(retainedChanges.size());
    for (DiffFilePresentation &change : retainedChanges) {
      FileDiff file{std::move(change.path), std::move(change.kind),
                    std::move(change.diff)};
      countLines(file.content, file.additions, file.deletions);
      fileDiffs.push_back(std::move(file));
    }
  }

  files->clear();
  int additions = 0;
  int deletions = 0;
  for (const FileDiff &file : fileDiffs) {
    additions += file.additions;
    deletions += file.deletions;
    const QString path =
        file.path.isEmpty() ? QStringLiteral("Turn diff") : file.path;
    auto *item = new QListWidgetItem(QStringLiteral("%1   +%2  -%3")
                                         .arg(path)
                                         .arg(file.additions)
                                         .arg(file.deletions));
    item->setToolTip(path);
    files->addItem(item);
  }
  summary->setText(fileDiffs.empty() ? QStringLiteral("No file changes")
                                     : QStringLiteral("%1 files   +%2  -%3")
                                           .arg(fileDiffs.size())
                                           .arg(additions)
                                           .arg(deletions));
  authority->setText(
      fileDiffs.empty() ? QString{}
      : live            ? QStringLiteral("Authoritative live turn diff")
             : QStringLiteral("Reconstructed from retained file-change items"));
  if (!fileDiffs.empty())
    files->setCurrentRow(0);
  else
    diff->clear();
  copyButton->setEnabled(!fileDiffs.empty());
  expandButton->setEnabled(!fileDiffs.empty());
}

std::vector<DiffViewer::FileDiff>
DiffViewer::parseUnifiedDiff(const QString &diff) {
  std::vector<FileDiff> result;
  FileDiff current;
  const auto flush = [&] {
    if (current.content.isEmpty())
      return;
    countLines(current.content, current.additions, current.deletions);
    result.push_back(std::move(current));
    current = FileDiff{};
  };
  const QStringList lines = diff.split(QLatin1Char('\n'));
  for (const QString &line : lines) {
    if (line.startsWith(QStringLiteral("diff --git ")) &&
        !current.content.isEmpty())
      flush();
    if (line.startsWith(QStringLiteral("+++ "))) {
      const QString path = pathFromHeader(line);
      if (!path.isEmpty())
        current.path = path;
    }
    current.content += line;
    current.content += QLatin1Char('\n');
  }
  flush();
  if (result.empty() && !diff.isEmpty()) {
    FileDiff file{QStringLiteral("Turn diff"), {}, diff};
    countLines(file.content, file.additions, file.deletions);
    result.push_back(std::move(file));
  }
  return result;
}

void DiffViewer::showSelectedFile() {
  const int index = files->currentRow();
  if (index < 0 || static_cast<std::size_t>(index) >= fileDiffs.size()) {
    diff->clear();
    return;
  }
  diff->setPlainText(fileDiffs[static_cast<std::size_t>(index)].content);
  diff->moveCursor(QTextCursor::Start);
}

void DiffViewer::showExpanded() {
  const int index = files->currentRow();
  if (index < 0 || static_cast<std::size_t>(index) >= fileDiffs.size())
    return;
  const FileDiff &file = fileDiffs[static_cast<std::size_t>(index)];
  QDialog dialog(this);
  dialog.setWindowTitle(file.path.isEmpty() ? QStringLiteral("Turn diff")
                                            : file.path);
  dialog.resize(1100, 760);
  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(16, 16, 16, 16);
  auto *view = new QPlainTextEdit(file.content);
  view->setReadOnly(true);
  view->setLineWrapMode(QPlainTextEdit::NoWrap);
  view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  new DiffHighlighter(view->document());
  layout->addWidget(view, 1);
  auto *close = new QPushButton(QStringLiteral("Close"));
  close->setFixedHeight(34);
  auto *footer = new QHBoxLayout;
  footer->addStretch();
  footer->addWidget(close);
  layout->addLayout(footer);
  connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
  dialog.exec();
}

} // namespace codexui::codex
