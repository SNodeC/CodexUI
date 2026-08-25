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
  const bool result = testLiveWorkingTreeChanges();
  git_libgit2_shutdown();
  return result ? 0 : 1;
}
