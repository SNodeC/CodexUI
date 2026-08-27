// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/GitDiffProvider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

#include <git2.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>

namespace codexui::codex {
namespace {

constexpr std::size_t MaximumDiffBytes = 16U * 1024U * 1024U;

void ensureLibGit() {
  static std::once_flag initialized;
  std::call_once(initialized, [] { git_libgit2_init(); });
}

template <typename Type, void (*Free)(Type *)>
using GitPointer = std::unique_ptr<Type, decltype(Free)>;

QString gitError(const QString &fallback) {
  const git_error *error = git_error_last();
  return error && error->message ? QString::fromUtf8(error->message) : fallback;
}

QString text(const char *value) {
  return value ? QString::fromUtf8(value) : QString{};
}

QString statusName(git_delta_t status) {
  switch (status) {
  case GIT_DELTA_ADDED:
    return QStringLiteral("Added");
  case GIT_DELTA_DELETED:
    return QStringLiteral("Deleted");
  case GIT_DELTA_RENAMED:
    return QStringLiteral("Renamed");
  case GIT_DELTA_COPIED:
    return QStringLiteral("Copied");
  case GIT_DELTA_UNTRACKED:
    return QStringLiteral("Untracked");
  case GIT_DELTA_TYPECHANGE:
    return QStringLiteral("Type changed");
  case GIT_DELTA_UNREADABLE:
    return QStringLiteral("Unreadable");
  case GIT_DELTA_CONFLICTED:
    return QStringLiteral("Conflict");
  default:
    return QStringLiteral("Modified");
  }
}

QString discoverRoot(const QString &directory) {
  if (directory.isEmpty())
    return {};
  QString start = directory;
  if (QFileInfo(start).isFile())
    start = QFileInfo(start).absolutePath();
  git_buf discovered = GIT_BUF_INIT;
  const QByteArray encoded = QFile::encodeName(start);
  if (git_repository_discover(&discovered, encoded.constData(), 0, nullptr) <
      0) {
    git_buf_dispose(&discovered);
    return {};
  }
  git_repository *raw = nullptr;
  const int opened = git_repository_open(&raw, discovered.ptr);
  git_buf_dispose(&discovered);
  if (opened < 0)
    return {};
  GitPointer<git_repository, git_repository_free> repository(raw,
                                                              git_repository_free);
  if (git_repository_is_bare(repository.get()))
    return {};
  return QDir::cleanPath(text(git_repository_workdir(repository.get())));
}

GitPointer<git_tree, git_tree_free> headTree(git_repository *repository,
                                             QString &error) {
  git_reference *rawReference = nullptr;
  const int headResult = git_repository_head(&rawReference, repository);
  GitPointer<git_reference, git_reference_free> reference(rawReference,
                                                          git_reference_free);
  if (headResult == GIT_EUNBORNBRANCH || headResult == GIT_ENOTFOUND)
    return {nullptr, git_tree_free};
  if (headResult < 0) {
    error = gitError(QStringLiteral("Unable to read repository HEAD."));
    return {nullptr, git_tree_free};
  }
  git_object *rawObject = nullptr;
  if (git_reference_peel(&rawObject, reference.get(), GIT_OBJECT_TREE) < 0) {
    error = gitError(QStringLiteral("Unable to read the HEAD tree."));
    return {nullptr, git_tree_free};
  }
  return {reinterpret_cast<git_tree *>(rawObject), git_tree_free};
}

QString normalizedHint(QString path) {
  path = QDir::fromNativeSeparators(std::move(path));
  if (path.startsWith(QStringLiteral("a/")) ||
      path.startsWith(QStringLiteral("b/")))
    path.remove(0, 2);
  return QDir::cleanPath(path);
}

bool containsHiddenDirectory(const QString &path) {
  const QStringList parts = QDir::fromNativeSeparators(path).split(
      QLatin1Char('/'), Qt::SkipEmptyParts);
  return std::any_of(parts.begin(), parts.end(), [](const QString &part) {
    return part.size() > 1 && part.startsWith(QLatin1Char('.'));
  });
}

int repositoryPathScore(git_repository *repository, const QString &root,
                        QString path) {
  path = normalizedHint(std::move(path));
  if (QDir::isAbsolutePath(path)) {
    path = QDir(root).relativeFilePath(path);
    if (path == QStringLiteral("..") || path.startsWith(QStringLiteral("../")))
      return 0;
  }
  const QByteArray encoded = QFile::encodeName(path);
  unsigned int status = 0;
  if (git_status_file(&status, repository, encoded.constData()) == 0)
    return status == GIT_STATUS_CURRENT ? 1 : 2;
  git_index *rawIndex = nullptr;
  if (git_repository_index(&rawIndex, repository) == 0) {
    GitPointer<git_index, git_index_free> index(rawIndex, git_index_free);
    if (git_index_get_bypath(index.get(), encoded.constData(), 0))
      return 1;
  }
  QString error;
  GitPointer<git_tree, git_tree_free> tree = headTree(repository, error);
  if (!tree)
    return 0;
  git_tree_entry *entry = nullptr;
  const bool found =
      git_tree_entry_bypath(&entry, tree.get(), encoded.constData()) == 0;
  git_tree_entry_free(entry);
  return found ? 1 : 0;
}

std::vector<int> rootHintScores(const QString &root,
                                const QStringList &directories,
                                const QStringList &paths) {
  std::vector<int> scores(static_cast<std::size_t>(paths.size()), 0);
  git_repository *raw = nullptr;
  const QByteArray encodedRoot = QFile::encodeName(root);
  if (git_repository_open(&raw, encodedRoot.constData()) < 0)
    return scores;
  GitPointer<git_repository, git_repository_free> repository(raw,
                                                              git_repository_free);
  for (qsizetype index = 0; index < paths.size(); ++index) {
    const QString &path = paths[index];
    int score = repositoryPathScore(repository.get(), root, path);
    for (const QString &directory : directories) {
      if (score == 2)
        break;
      const QString absolute = QDir(directory).absoluteFilePath(path);
      score = std::max(
          score, repositoryPathScore(repository.get(), root, absolute));
    }
    scores[static_cast<std::size_t>(index)] = score;
  }
  return scores;
}

bool appendRepository(GitDiffSnapshot &snapshot, const QString &root,
                      GitDiffScope scope, GitDiffContext context,
                      const std::shared_ptr<std::atomic<std::uint64_t>> &clock,
                      std::uint64_t generation, std::size_t &retainedBytes) {
  git_repository *rawRepository = nullptr;
  const QByteArray encodedRoot = QFile::encodeName(root);
  if (git_repository_open(&rawRepository, encodedRoot.constData()) < 0)
    return false;
  GitPointer<git_repository, git_repository_free> repository(rawRepository,
                                                              git_repository_free);
  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.flags = GIT_DIFF_INCLUDE_UNTRACKED |
                  GIT_DIFF_RECURSE_UNTRACKED_DIRS |
                  GIT_DIFF_SHOW_UNTRACKED_CONTENT |
                  GIT_DIFF_INCLUDE_TYPECHANGE |
                  GIT_DIFF_INCLUDE_TYPECHANGE_TREES |
                  GIT_DIFF_INCLUDE_UNREADABLE;
  options.context_lines =
      context == GitDiffContext::Compact
          ? 3
          : std::numeric_limits<std::uint16_t>::max();
  options.interhunk_lines = context == GitDiffContext::Compact ? 0 : 3;

  QString treeError;
  GitPointer<git_tree, git_tree_free> tree =
      headTree(repository.get(), treeError);
  if (!treeError.isEmpty()) {
    snapshot.error = treeError;
    return false;
  }
  git_diff *rawDiff = nullptr;
  int result = 0;
  if (scope == GitDiffScope::Unstaged)
    result = git_diff_index_to_workdir(&rawDiff, repository.get(), nullptr,
                                        &options);
  else if (scope == GitDiffScope::Staged)
    result = git_diff_tree_to_index(&rawDiff, repository.get(), tree.get(),
                                     nullptr, &options);
  else
    result = git_diff_tree_to_workdir_with_index(
        &rawDiff, repository.get(), tree.get(), &options);
  if (result < 0) {
    snapshot.error = gitError(QStringLiteral("Unable to calculate Git changes."));
    return false;
  }
  GitPointer<git_diff, git_diff_free> diff(rawDiff, git_diff_free);
  git_diff_find_options findOptions = GIT_DIFF_FIND_OPTIONS_INIT;
  findOptions.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES |
                      GIT_DIFF_FIND_FOR_UNTRACKED;
  git_diff_find_similar(diff.get(), &findOptions);

  const std::size_t count = git_diff_num_deltas(diff.get());
  for (std::size_t index = 0; index < count; ++index) {
    if (clock->load() != generation)
      return false;
    const git_diff_delta *delta = git_diff_get_delta(diff.get(), index);
    if (!delta || delta->status == GIT_DELTA_UNMODIFIED ||
        delta->status == GIT_DELTA_IGNORED)
      continue;
    GitDiffFile file;
    file.repositoryRoot = root;
    file.path = text(delta->new_file.path);
    if (file.path.isEmpty())
      file.path = text(delta->old_file.path);
    file.absolutePath = QDir(root).absoluteFilePath(file.path);
    file.previousPath = text(delta->old_file.path);
    if (file.previousPath == file.path)
      file.previousPath.clear();
    file.status = statusName(delta->status);
    file.binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
    git_patch *rawPatch = nullptr;
    const int patchResult = git_patch_from_diff(&rawPatch, diff.get(), index);
    GitPointer<git_patch, git_patch_free> patch(rawPatch, git_patch_free);
    if (patchResult == 0 && patch) {
      std::size_t additions = 0;
      std::size_t deletions = 0;
      git_patch_line_stats(nullptr, &additions, &deletions, patch.get());
      file.additions = static_cast<int>(std::min<std::size_t>(
          additions, static_cast<std::size_t>(std::numeric_limits<int>::max())));
      file.deletions = static_cast<int>(std::min<std::size_t>(
          deletions, static_cast<std::size_t>(std::numeric_limits<int>::max())));
      git_buf rendered = GIT_BUF_INIT;
      if (git_patch_to_buf(&rendered, patch.get()) == 0) {
        if (retainedBytes + rendered.size <= MaximumDiffBytes) {
          file.patch = QString::fromUtf8(
              rendered.ptr, static_cast<qsizetype>(rendered.size));
          retainedBytes += rendered.size;
        } else {
          snapshot.truncated = true;
          file.patch = QStringLiteral(
              "Diff omitted because the review exceeds the 16 MiB display limit.");
        }
      }
      git_buf_dispose(&rendered);
    }
    snapshot.files.push_back(std::move(file));
  }
  return true;
}

GitDiffSnapshot collect(QString workspace, QStringList directories,
                        QStringList paths, QString selectedRepository,
                        bool includeHiddenRepositories, GitDiffScope scope,
                        GitDiffContext context,
                        const std::shared_ptr<std::atomic<std::uint64_t>> &clock,
                        std::uint64_t generation) {
  GitDiffSnapshot snapshot;
  snapshot.workspace = workspace.isEmpty()
                           ? QString{}
                           : QDir::cleanPath(std::move(workspace));
  snapshot.scope = scope;
  snapshot.context = context;
  if (!snapshot.workspace.isEmpty())
    directories.prepend(snapshot.workspace);
  directories.removeDuplicates();

  QStringList roots;
  QHash<QString, QStringList> rootDirectories;
  for (const QString &directory : directories) {
    if (clock->load() != generation)
      return snapshot;
    if (!includeHiddenRepositories && containsHiddenDirectory(directory))
      continue;
    const QString root = discoverRoot(directory);
    if (root.isEmpty() ||
        (!includeHiddenRepositories && containsHiddenDirectory(root)))
      continue;
    rootDirectories[root].push_back(directory);
    if (!roots.contains(root))
      roots.push_back(root);
  }
  if (roots.isEmpty()) {
    snapshot.error = snapshot.workspace.isEmpty()
                         ? QStringLiteral("Select a thread to inspect changes.")
                         : QStringLiteral("Change review requires a Git repository.");
    return snapshot;
  }

  QStringList matched;
  QHash<QString, std::vector<int>> hintScores;
  for (const QString &root : roots)
    hintScores.insert(root,
                      rootHintScores(root, rootDirectories[root], paths));
  for (qsizetype pathIndex = 0; pathIndex < paths.size(); ++pathIndex) {
    QStringList pathMatches;
    int bestScore = 0;
    for (const QString &root : roots) {
      const int score =
          hintScores[root][static_cast<std::size_t>(pathIndex)];
      if (score > bestScore) {
        bestScore = score;
        pathMatches.clear();
      }
      if (score != 0 && score == bestScore)
        pathMatches.push_back(root);
    }
    for (const QString &root : pathMatches) {
      if (!matched.contains(root))
        matched.push_back(root);
    }
  }
  if (!matched.isEmpty())
    roots = std::move(matched);
  std::sort(roots.begin(), roots.end(), [](const QString &left,
                                           const QString &right) {
    return QString::localeAwareCompare(left, right) < 0;
  });
  snapshot.repositoryRoots = roots;
  snapshot.repository = true;

  if (!selectedRepository.isEmpty() && roots.contains(selectedRepository))
    roots = {selectedRepository};
  snapshot.repositoryRoot = roots.size() == 1 ? roots.front() : QString{};
  std::size_t retainedBytes = 0;
  for (const QString &root : roots) {
    if (clock->load() != generation)
      return snapshot;
    appendRepository(snapshot, root, scope, context, clock, generation,
                     retainedBytes);
  }
  std::sort(snapshot.files.begin(), snapshot.files.end(),
            [](const GitDiffFile &left, const GitDiffFile &right) {
              if (left.repositoryRoot != right.repositoryRoot)
                return QString::localeAwareCompare(left.repositoryRoot,
                                                    right.repositoryRoot) < 0;
              return QString::localeAwareCompare(left.path, right.path) < 0;
            });
  return snapshot;
}

} // namespace

GitDiffProvider::GitDiffProvider(QObject *parent)
    : QObject(parent),
      generation(std::make_shared<std::atomic<std::uint64_t>>(0)) {
  ensureLibGit();
}

GitDiffProvider::~GitDiffProvider() { cancel(); }

void GitDiffProvider::cancel() {
  generation->fetch_add(1);
  emit loadingChanged(false);
}

void GitDiffProvider::request(QString workspace,
                              QStringList candidateDirectories,
                              QStringList changedPaths,
                              QString selectedRepository,
                              bool includeHiddenRepositories,
                              GitDiffScope scope,
                              GitDiffContext context) {
  const std::uint64_t requested = generation->fetch_add(1) + 1;
  const auto clock = generation;
  const QPointer<GitDiffProvider> receiver(this);
  emit loadingChanged(true);
  QThreadPool::globalInstance()->start(
      [receiver, clock, requested, workspace = std::move(workspace),
       candidateDirectories = std::move(candidateDirectories),
       changedPaths = std::move(changedPaths),
       selectedRepository = std::move(selectedRepository),
       includeHiddenRepositories, scope,
       context]() mutable {
        GitDiffSnapshot snapshot = collect(
            std::move(workspace), std::move(candidateDirectories),
            std::move(changedPaths), std::move(selectedRepository),
            includeHiddenRepositories, scope, context, clock, requested);
        if (clock->load() != requested)
          return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [receiver, clock, requested, snapshot = std::move(snapshot)]() {
              if (!receiver || clock->load() != requested)
                return;
              emit receiver->loadingChanged(false);
              emit receiver->snapshotReady(snapshot);
            },
            Qt::QueuedConnection);
      });
}

} // namespace codexui::codex
