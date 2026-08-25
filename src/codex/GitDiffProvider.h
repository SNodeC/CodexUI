// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_GITDIFFPROVIDER_H
#define CODEXUI_CODEX_GITDIFFPROVIDER_H

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace codexui::codex {

enum class GitDiffScope { Unstaged, Staged, Uncommitted };
enum class GitDiffContext { Compact, Expanded };

struct GitDiffFile {
  QString path;
  QString previousPath;
  QString status;
  QString patch;
  int additions = 0;
  int deletions = 0;
  bool binary = false;
};

struct GitDiffSnapshot {
  QString workspace;
  QString repositoryRoot;
  QString error;
  GitDiffScope scope = GitDiffScope::Unstaged;
  GitDiffContext context = GitDiffContext::Compact;
  std::vector<GitDiffFile> files;
  bool repository = false;
  bool truncated = false;
};

class GitDiffProvider final : public QObject {
  Q_OBJECT

public:
  explicit GitDiffProvider(QObject *parent = nullptr);
  ~GitDiffProvider() override;

  void request(QString workspace, GitDiffScope scope,
               GitDiffContext context);
  void cancel();

signals:
  void loadingChanged(bool loading);
  void snapshotReady(const codexui::codex::GitDiffSnapshot &snapshot);

private:
  std::shared_ptr<std::atomic<std::uint64_t>> generation;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_GITDIFFPROVIDER_H
