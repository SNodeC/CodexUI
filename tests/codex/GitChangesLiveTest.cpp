// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/DiffViewer.h"
#include "codex/GitDiffProvider.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QAccessible>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <git2.h>

#include <functional>
#include <iostream>

namespace {

using codexui::codex::DiffViewer;
using codexui::codex::GitDiffFile;
using codexui::codex::GitDiffProvider;
using codexui::codex::GitDiffSnapshot;

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  return file.write(contents) == contents.size();
}

bool replaceFile(const QString &path, const QByteArray &contents) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(contents) != contents.size())
    return false;
  return file.commit();
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return condition();
}

bool createInitialCommit(git_repository *repository, const QString &root) {
  if (!writeFile(QDir(root).filePath(QStringLiteral("tracked.txt")),
                 QByteArray("original\n")))
    return false;
  git_index *index = nullptr;
  if (git_repository_index(&index, repository) < 0)
    return false;
  const bool indexed = git_index_add_bypath(index, "tracked.txt") == 0 &&
                       git_index_write(index) == 0;
  git_oid treeId{};
  const bool wroteTree = indexed && git_index_write_tree(&treeId, index) == 0;
  git_index_free(index);
  if (!wroteTree)
    return false;
  git_tree *tree = nullptr;
  git_signature *signature = nullptr;
  if (git_tree_lookup(&tree, repository, &treeId) < 0 ||
      git_signature_now(&signature, "CodexUI Test", "codexui@example.invalid") <
          0) {
    git_tree_free(tree);
    git_signature_free(signature);
    return false;
  }
  git_oid commitId{};
  const bool committed =
      git_commit_create(&commitId, repository, "HEAD", signature, signature,
                        nullptr, "initial", tree, 0, nullptr) == 0;
  git_signature_free(signature);
  git_tree_free(tree);
  return committed;
}

bool hasFile(const GitDiffSnapshot &snapshot, const QString &path,
             const QString &status = {}) {
  for (const GitDiffFile &file : snapshot.files) {
    if (file.path == path && (status.isEmpty() || file.status == status))
      return true;
  }
  return false;
}

bool testEmptySnapshotLoadingState() {
  DiffViewer viewer;
  auto *provider = viewer.findChild<GitDiffProvider *>();
  auto *summary =
      viewer.findChild<QLabel *>(QStringLiteral("codexDiffSummary"));
  if (!provider || !summary)
    return expect(false, "diff loading-state controls are discoverable");

  provider->loadingChanged(true);
  const bool initialLoading =
      summary->text() == QStringLiteral("Loading changes…");

  GitDiffSnapshot empty;
  empty.workspace = QStringLiteral("/workspace");
  empty.repositoryRoot = QStringLiteral("/workspace");
  empty.repositoryRoots = {empty.repositoryRoot};
  empty.repository = true;
  provider->loadingChanged(false);
  provider->snapshotReady(empty);
  const bool emptyRendered = summary->text() == QStringLiteral("No changes");

  provider->loadingChanged(true);
  const bool backgroundRetained =
      summary->text() == QStringLiteral("No changes");
  provider->loadingChanged(false);
  provider->snapshotReady(empty);
  const bool identicalRetained =
      summary->text() == QStringLiteral("No changes");

  return expect(initialLoading && emptyRendered && backgroundRetained &&
                    identicalRetained,
                "background refreshes retain a valid empty diff state");
}

bool testSnapshotMetadataRefresh() {
  DiffViewer viewer;
  auto *provider = viewer.findChild<GitDiffProvider *>();
  auto *files =
      viewer.findChild<QListWidget *>(QStringLiteral("codexDiffFiles"));
  const bool updated = [provider, files] {
    if (!provider || !files)
      return false;
    GitDiffFile file;
    file.repositoryRoot = QStringLiteral("/repository");
    file.path = QStringLiteral("changed.txt");
    file.absolutePath = QStringLiteral("/repository/changed.txt");
    file.status = QStringLiteral("Modified");
    file.patch = QStringLiteral("@@ -1 +1 @@\n-before\n+after");
    file.additions = 1;
    file.deletions = 2;
    GitDiffSnapshot snapshot;
    snapshot.workspace = QStringLiteral("/workspace");
    snapshot.repositoryRoot = file.repositoryRoot;
    snapshot.repositoryRoots = {file.repositoryRoot};
    snapshot.files = {file};
    snapshot.repository = true;
    provider->snapshotReady(snapshot);
    const bool initialRendered =
        files->count() == 1 &&
        files->item(0)->text().contains(QStringLiteral("+1")) &&
        files->item(0)->text().contains(QStringLiteral("−2"));

    snapshot.files.front().additions = 7;
    snapshot.files.front().deletions = 5;
    provider->snapshotReady(snapshot);
    const bool metadataUpdated =
        files->count() == 1 &&
        files->item(0)->text().contains(QStringLiteral("+7")) &&
        files->item(0)->text().contains(QStringLiteral("−5"));
    return initialRendered && metadataUpdated;
  }();
  return expect(updated,
                "diff presentation updates when only line totals change");
}

bool testDiffHeadersRemainReadable() {
  DiffViewer viewer;
  viewer.setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  auto *provider = viewer.findChild<GitDiffProvider *>();
  auto *diff = viewer.findChild<QPlainTextEdit *>(QStringLiteral("codexDiffText"));
  auto *title = viewer.findChild<QLineEdit *>(QStringLiteral("codexDiffSelectedFile"));
  QPushButton *copy = nullptr;
  QPushButton *review = nullptr;
  for (auto *button : viewer.findChildren<QPushButton *>()) {
    if (button->text() == QStringLiteral("Copy"))
      copy = button;
    if (button->text() == QStringLiteral("Open review"))
      review = button;
  }
  if (!expect(provider && diff && title && copy && review, "preview controls exist"))
    return false;
  const auto key = [title](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                           QString text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers, text);
    QApplication::sendEvent(title, &press);
    QApplication::sendEvent(title, &release);
  };
  GitDiffFile file;
  file.repositoryRoot = QStringLiteral("/qualification");
  file.status = QStringLiteral("Modified");
  file.patch = QStringLiteral("@@ -1 +1 @@\n-before\n+after");
  GitDiffSnapshot snapshot;
  snapshot.repositoryRoots = {file.repositoryRoot};
  bool result = true;
  viewer.show();
  for (int pointSize : {9, 16}) {
    QFont font = viewer.font();
    font.setPointSize(pointSize);
    viewer.setFont(font);
    for (int width : {300, 404}) {
      viewer.resize(width, 720);
      for (const QString &path : {
               QStringLiteral("changed.txt"),
               QStringLiteral("directory with spaces/α🙂/") +
                   QString(120, QLatin1Char('w')) + QStringLiteral(".cpp")}) {
        file.path = path;
        file.absolutePath = file.repositoryRoot + QLatin1Char('/') + path;
        snapshot.files = {file};
        provider->snapshotReady(snapshot);
        QCoreApplication::processEvents();
        result &= expect(title->text() == path && viewer.width() == width &&
                             title->width() >= width - 30 &&
                             title->height() >= title->sizeHint().height() &&
                             title->cursorPosition() == 0 && title->isReadOnly(),
                         "complete filename starts at its beginning without widening the pane");
        for (auto *button : {copy, review})
          result &= expect(title && button->isEnabled() &&
                               viewer.rect().contains(button->geometry()) &&
                               !title->geometry().intersects(button->geometry()),
                           "preview actions remain visible and do not overlap the title");
        copy->click();
        result &= expect(QApplication::clipboard()->text() == file.patch &&
                             diff->toPlainText() == file.patch,
                         "header reflow preserves the displayed and copied diff");
        title->setFocus();
        key(Qt::Key_End);
        result &= expect(title->cursorPosition() == path.size(),
                         "keyboard navigation reaches the end of the complete path");
        for (int i = 0; i < 4; ++i)
          key(Qt::Key_Left, Qt::ShiftModifier);
        key(Qt::Key_C, Qt::ControlModifier);
        result &= expect(QApplication::clipboard()->text() == path.right(4),
                         "partial filename Copy exposes the suffix beyond the initial viewport");
        key(Qt::Key_Home);
        result &= expect(title->cursorPosition() == 0,
                         "keyboard navigation returns to the path beginning");
        key(Qt::Key_A, Qt::ControlModifier);
        key(Qt::Key_C, Qt::ControlModifier);
        result &= expect(QApplication::clipboard()->text() == path,
                         "filename Copy preserves every Unicode character and trailing suffix");
        provider->snapshotReady(snapshot);
        result &= expect(title->selectedText() == path && title->hasFocus(),
                         "unchanged snapshot preserves filename selection and focus");
        key(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        key(Qt::Key_Backspace);
        QApplication::clipboard()->setText(QStringLiteral("replacement"));
        key(Qt::Key_V, Qt::ControlModifier);
        result &= expect(title->text() == path,
                         "read-only filename rejects typing, deletion and paste");
#if QT_CONFIG(accessibility)
        auto *accessible = QAccessible::queryAccessibleInterface(title);
        result &= expect(accessible && accessible->state().readOnly &&
                             accessible->text(QAccessible::Name) == QStringLiteral("Selected file") &&
                             accessible->text(QAccessible::Value) == path,
                         "accessibility exposes the full read-only filename");
#endif
      }
    }
  }
  review->click();
  QCoreApplication::processEvents();
  auto *reviewTitle = viewer.findChild<QLineEdit *>(QStringLiteral("codexReviewSelectedFile"));
  if (!expect(reviewTitle, "review uses a selectable filename field"))
    return false;
  for (int width : {900, 1200}) {
    reviewTitle->window()->resize(width, 780);
    QCoreApplication::processEvents();
    result &= expect(reviewTitle->window()->width() == width &&
                         reviewTitle->text() == file.path && reviewTitle->isReadOnly() &&
                         reviewTitle->height() >= reviewTitle->sizeHint().height(),
                     "long review filename remains readable without widening its window");
    reviewTitle->setFocus();
    reviewTitle->selectAll();
    reviewTitle->copy();
    result &= expect(QApplication::clipboard()->text() == file.path,
                     "review filename Copy includes the entire Unicode path");
    snapshot.files.front().patch += QStringLiteral("\n+updated");
    provider->snapshotReady(snapshot);
    result &= expect(reviewTitle->selectedText() == file.path && reviewTitle->hasFocus(),
                     "changed diff content preserves unchanged filename selection and focus");
  }
#if QT_CONFIG(accessibility)
  for (auto *list : viewer.findChildren<QListWidget *>()) {
    auto *accessible = QAccessible::queryAccessibleInterface(list);
    const QString expected = list->objectName() == QStringLiteral("codexDiffFiles")
                                 ? QStringLiteral("Changed files") : QStringLiteral("Review files");
    result &= expect(accessible && accessible->text(QAccessible::Name) == expected,
                     "both file lists have semantic accessible names");
  }
  auto *reviewAccessible = QAccessible::queryAccessibleInterface(reviewTitle);
  result &= expect(reviewAccessible && reviewAccessible->state().readOnly &&
                       reviewAccessible->text(QAccessible::Name) == QStringLiteral("Selected file") &&
                       reviewAccessible->text(QAccessible::Value) == file.path,
                   "review filename is fully represented in accessibility");
  for (const auto &[objectName, name] : {
           std::pair{"codexDiffText", "Changes diff"},
           std::pair{"codexReviewUnified", "Unified diff"},
           std::pair{"codexReviewBefore", "Before changes"},
           std::pair{"codexReviewAfter", "After changes"}}) {
    auto *control = viewer.findChild<QPlainTextEdit *>(QString::fromLatin1(objectName));
    auto *accessible = QAccessible::queryAccessibleInterface(control);
    result &= expect(accessible && accessible->state().readOnly &&
                         accessible->text(QAccessible::Name) == QString::fromLatin1(name),
                     "preview and review panes expose distinct read-only accessible identities");
  }
#endif
  snapshot.files.clear();
  provider->snapshotReady(snapshot);
  result &= expect(title->text().isEmpty() && reviewTitle->text().isEmpty() &&
                       !copy->isEnabled() && !review->isEnabled(),
                   "empty changes clear the filename and disable preview actions");
  return result;
}

bool testContextSwitchCancelsOldSnapshot() {
  QTemporaryDir firstDirectory;
  QTemporaryDir secondDirectory;
  if (!expect(firstDirectory.isValid() && secondDirectory.isValid(),
              "creates repositories for context cancellation"))
    return false;
  git_repository *firstRepository = nullptr;
  git_repository *secondRepository = nullptr;
  const bool initialized =
      git_repository_init(&firstRepository,
                          firstDirectory.path().toUtf8().constData(), 0) == 0 &&
      git_repository_init(&secondRepository,
                          secondDirectory.path().toUtf8().constData(), 0) == 0 &&
      createInitialCommit(firstRepository, firstDirectory.path()) &&
      createInitialCommit(secondRepository, secondDirectory.path());
  if (!expect(initialized, "initializes context cancellation repositories")) {
    git_repository_free(firstRepository);
    git_repository_free(secondRepository);
    return false;
  }

  DiffViewer viewer;
  viewer.resize(700, 500);
  viewer.show();
  auto *provider = viewer.findChild<GitDiffProvider *>();
  QStringList deliveredWorkspaces;
  QObject::connect(provider, &GitDiffProvider::snapshotReady, &viewer,
                   [&deliveredWorkspaces](const GitDiffSnapshot &snapshot) {
                     deliveredWorkspaces.push_back(snapshot.workspace);
                   });
  provider->request(firstDirectory.path(), {firstDirectory.path()}, {}, {},
                    false, codexui::codex::GitDiffScope::Unstaged,
                    codexui::codex::GitDiffContext::Compact);
  viewer.setRepositoryContext(QStringLiteral("second-context"),
                              secondDirectory.path(),
                              {secondDirectory.path()}, {});
  const bool secondApplied = waitFor(
      [&] {
        return viewer.currentSnapshot().workspace == secondDirectory.path() &&
               viewer.currentSnapshot().repository;
      },
      1500);
  for (QTimer *timer : viewer.findChildren<QTimer *>())
    timer->stop();
  const bool oldSuppressed =
      !deliveredWorkspaces.contains(firstDirectory.path());
  git_repository_free(firstRepository);
  git_repository_free(secondRepository);
  return expect(secondApplied && oldSuppressed,
                "a context switch synchronously suppresses the old Git result");
}

bool testLiveWorkingTreeChanges() {
  QTemporaryDir directory;
  if (!expect(directory.isValid(), "creates a temporary repository"))
    return false;
  git_repository *repository = nullptr;
  if (!expect(git_repository_init(&repository,
                                  directory.path().toUtf8().constData(), 0) ==
                  0,
              "initializes a repository with libgit2"))
    return false;
  if (!expect(createInitialCommit(repository, directory.path()),
              "creates an initial commit with libgit2")) {
    git_repository_free(repository);
    return false;
  }

  DiffViewer viewer;
  viewer.resize(700, 500);
  viewer.show();
  const QString threadId =
      QStringLiteral("live-%1").arg(directory.path());
  viewer.setRepositoryContext(
      threadId, directory.path(), {directory.path()}, {});
  bool result = expect(
      waitFor([&] { return viewer.currentSnapshot().repository; }, 1500) &&
          viewer.currentSnapshot().files.empty(),
      "starts from the clean real working tree");

  const QString manual =
      directory.filePath(QStringLiteral("nested/manual.txt"));
  QDir().mkpath(QFileInfo(manual).absolutePath());
  result &= expect(writeFile(manual, QByteArray("created by hand\n")) &&
                       waitFor(
                           [&] {
                             return hasFile(viewer.currentSnapshot(),
                                            QStringLiteral("nested/manual.txt"),
                                            QStringLiteral("Untracked"));
                           },
                           3500),
                   "discovers a manually created untracked file");
  for (QTimer *timer : viewer.findChildren<QTimer *>()) {
    if (!timer->isSingleShot())
      timer->stop();
  }
  result &= expect(QFile::remove(manual) &&
                       waitFor(
                           [&] {
                             return !hasFile(viewer.currentSnapshot(),
                                             QStringLiteral("nested/manual.txt"));
                           },
                           1500),
                   "removes a reverted untracked file after a filesystem event");

  const QString tracked =
      directory.filePath(QStringLiteral("tracked.txt"));
  const bool modified = writeFile(tracked, QByteArray("modified\n"));
  viewer.refreshRepository();
  result &= expect(modified &&
                       waitFor(
                           [&] {
                             return hasFile(viewer.currentSnapshot(),
                                            QStringLiteral("tracked.txt"),
                                            QStringLiteral("Modified"));
                           },
                           3500),
                   "discovers a modified tracked file");
  const QString renamed =
      directory.filePath(QStringLiteral("renamed-tracked.txt"));
  result &= expect(writeFile(tracked, QByteArray("original\n")) &&
                       QFile::rename(tracked, renamed) &&
                       waitFor(
                           [&] {
                             return hasFile(viewer.currentSnapshot(),
                                            QStringLiteral(
                                                "renamed-tracked.txt"),
                                            QStringLiteral("Renamed"));
                           },
                           1500),
                   "moves watches with a renamed tracked file");
  result &= expect(QFile::rename(renamed, tracked) &&
                       waitFor(
                           [&] {
                             return viewer.currentSnapshot().files.empty();
                           },
                           1500),
                   "restores watches when a renamed file moves back");
  result &= expect(writeFile(tracked, QByteArray("modified\n")),
                   "modifies the restored tracked file");
  viewer.refreshRepository();
  result &= expect(waitFor(
                       [&] {
                         return hasFile(viewer.currentSnapshot(),
                                        QStringLiteral("tracked.txt"),
                                        QStringLiteral("Modified"));
                       },
                       1500),
                   "rediscovers a modified file after a rename cycle");
  result &= expect(writeFile(tracked, QByteArray("original\n")) &&
                       waitFor(
                           [&] {
                             return !hasFile(viewer.currentSnapshot(),
                                             QStringLiteral("tracked.txt"));
                           },
                           1500),
                   "removes a content reversion after a filesystem event");

  const bool atomicallyModified =
      replaceFile(tracked, QByteArray("atomic modification\n"));
  viewer.refreshRepository();
  result &= expect(atomicallyModified &&
                       waitFor(
                           [&] {
                             return hasFile(viewer.currentSnapshot(),
                                            QStringLiteral("tracked.txt"),
                                            QStringLiteral("Modified"));
                           },
                           1500),
                   "refreshes after an atomic file replacement");
  result &= expect(replaceFile(tracked, QByteArray("original\n")) &&
                       waitFor(
                           [&] {
                             return !hasFile(viewer.currentSnapshot(),
                                             QStringLiteral("tracked.txt"));
                           },
                           1500),
                   "re-registers watches and removes an atomic reversion");

  const bool deleted = QFile::remove(tracked);
  viewer.refreshRepository();
  result &= expect(deleted &&
                       waitFor(
                           [&] {
                             return hasFile(viewer.currentSnapshot(),
                                            QStringLiteral("tracked.txt"),
                                            QStringLiteral("Deleted"));
                           },
                           1500),
                   "represents a deleted tracked file consistently");
  result &= expect(writeFile(tracked, QByteArray("original\n")) &&
                       waitFor(
                           [&] {
                             return !hasFile(viewer.currentSnapshot(),
                                             QStringLiteral("tracked.txt"));
                           },
                           1500),
                   "removes a restored deletion after a directory event");

  DiffViewer restartedViewer;
  restartedViewer.resize(700, 500);
  restartedViewer.show();
  restartedViewer.setRepositoryContext(
      threadId, QFileInfo(directory.path()).absolutePath(), {}, {});
  result &= expect(
      waitFor(
          [&] {
            return restartedViewer.currentSnapshot().repositoryRoots ==
                   QStringList{QDir::cleanPath(directory.path())};
          },
          1500),
      "restores a one-repository thread from persisted resolution after viewer recreation");

  git_repository_free(repository);
  return result;
}

} // namespace

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  git_libgit2_init();
  bool result = testEmptySnapshotLoadingState();
  result &= testSnapshotMetadataRefresh();
  result &= testDiffHeadersRemainReadable();
  result &= testContextSwitchCancelsOldSnapshot();
  result &= testLiveWorkingTreeChanges();
  git_libgit2_shutdown();
  return result ? 0 : 1;
}
