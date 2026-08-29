// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/DiffViewer.h"
#include "codex/GitDiffProvider.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
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
  result &= testLiveWorkingTreeChanges();
  git_libgit2_shutdown();
  return result ? 0 : 1;
}
