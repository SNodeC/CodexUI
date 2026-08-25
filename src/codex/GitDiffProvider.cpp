// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/GitDiffProvider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
  return error && error->message
             ? QString::fromUtf8(error->message)
             : fallback;
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

GitDiffSnapshot collect(QString workspace, GitDiffScope scope,
                        GitDiffContext context,
                        const std::shared_ptr<std::atomic<std::uint64_t>> &clock,
                        std::uint64_t generation) {
  GitDiffSnapshot snapshot;
  snapshot.workspace = QDir::cleanPath(std::move(workspace));
  snapshot.scope = scope;
  snapshot.context = context;
  if (snapshot.workspace.isEmpty()) {
    snapshot.error = QStringLiteral("Select a thread to inspect changes.");
    return snapshot;
  }

  git_buf discovered = GIT_BUF_INIT;
  const QByteArray start = QFile::encodeName(snapshot.workspace);
  if (git_repository_discover(&discovered, start.constData(), 0, nullptr) < 0) {
    snapshot.error = QStringLiteral("Change review requires a Git repository.");
    git_buf_dispose(&discovered);
    return snapshot;
  }
  const QString repositoryPath =
      QDir::cleanPath(QString::fromUtf8(discovered.ptr,
                                        static_cast<qsizetype>(discovered.size)));
  git_repository *rawRepository = nullptr;
  const QByteArray encodedRepository = QFile::encodeName(repositoryPath);
  const int openResult =
      git_repository_open(&rawRepository, encodedRepository.constData());
  git_buf_dispose(&discovered);
  if (openResult < 0) {
    snapshot.error = gitError(QStringLiteral("Unable to open Git repository."));
    return snapshot;
  }
  GitPointer<git_repository, git_repository_free> repository(rawRepository,
                                                              git_repository_free);
  snapshot.repository = true;
  snapshot.repositoryRoot =
      QDir::cleanPath(text(git_repository_workdir(repository.get())));
  if (git_repository_is_bare(repository.get())) {
    snapshot.error = QStringLiteral("Change review requires a working tree.");
    return snapshot;
  }

  if (clock->load() != generation)
    return snapshot;

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
    return snapshot;
  }

  git_diff *rawDiff = nullptr;
  int diffResult = 0;
  if (scope == GitDiffScope::Unstaged) {
    diffResult = git_diff_index_to_workdir(&rawDiff, repository.get(), nullptr,
                                            &options);
  } else if (scope == GitDiffScope::Staged) {
    diffResult = git_diff_tree_to_index(&rawDiff, repository.get(), tree.get(),
                                         nullptr, &options);
  } else {
    diffResult = git_diff_tree_to_workdir_with_index(
        &rawDiff, repository.get(), tree.get(), &options);
  }
  if (diffResult < 0) {
    snapshot.error = gitError(QStringLiteral("Unable to calculate Git changes."));
    return snapshot;
  }
  GitPointer<git_diff, git_diff_free> diff(rawDiff, git_diff_free);

  git_diff_find_options findOptions = GIT_DIFF_FIND_OPTIONS_INIT;
  findOptions.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES |
                      GIT_DIFF_FIND_FOR_UNTRACKED;
  git_diff_find_similar(diff.get(), &findOptions);

  std::size_t retainedBytes = 0;
  const std::size_t count = git_diff_num_deltas(diff.get());
  snapshot.files.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (clock->load() != generation)
      return snapshot;
    const git_diff_delta *delta = git_diff_get_delta(diff.get(), index);
    if (!delta || delta->status == GIT_DELTA_UNMODIFIED ||
        delta->status == GIT_DELTA_IGNORED)
      continue;
    GitDiffFile file;
    file.path = text(delta->new_file.path);
    if (file.path.isEmpty())
      file.path = text(delta->old_file.path);
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
  std::sort(snapshot.files.begin(), snapshot.files.end(),
            [](const GitDiffFile &left, const GitDiffFile &right) {
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

void GitDiffProvider::request(QString workspace, GitDiffScope scope,
                              GitDiffContext context) {
  const std::uint64_t requested = generation->fetch_add(1) + 1;
  const auto clock = generation;
  const QPointer<GitDiffProvider> receiver(this);
  emit loadingChanged(true);
  QThreadPool::globalInstance()->start(
      [receiver, clock, requested, workspace = std::move(workspace), scope,
       context]() mutable {
        GitDiffSnapshot snapshot =
            collect(std::move(workspace), scope, context, clock, requested);
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
