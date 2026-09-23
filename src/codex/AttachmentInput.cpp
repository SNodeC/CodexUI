// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/AttachmentInput.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QMimeData>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>
#include <algorithm>

namespace codexui::codex {

bool isAttachmentInput(const QMimeData &source) {
  const auto urls = source.urls();
  return source.hasImage() || std::ranges::any_of(urls, [](const QUrl &url) {
           return url.isLocalFile();
         });
}

QString appendAttachmentFiles(std::vector<AttachmentDraft> &draft,
                              const QStringList &paths) {
  auto next = draft;
  QMimeDatabase database;
  for (const QString &path : paths) {
    const QFileInfo file(path);
    const std::string absolute = file.absoluteFilePath().toUtf8().toStdString();
    if (std::ranges::find(next, absolute, &AttachmentDraft::path) != next.end())
      continue;
    if (next.size() >= MaximumAttachments)
      return QStringLiteral("A message can contain at most %1 attachments.")
          .arg(MaximumAttachments);
    if (!file.isFile() || !file.isReadable())
      return QStringLiteral("Cannot attach unreadable file: %1").arg(path);
    next.push_back(
        {absolute, file.fileName().toUtf8().toStdString(),
         database.mimeTypeForFile(file, QMimeDatabase::MatchExtension)
             .name()
             .toUtf8()
             .toStdString(),
         file.size()});
  }
  draft = std::move(next);
  return {};
}

QString appendAttachmentInput(std::vector<AttachmentDraft> &draft,
                              const QMimeData &source) {
  if (source.hasUrls()) {
    QStringList paths;
    for (const QUrl &url : source.urls()) {
      if (!url.isLocalFile())
        return QStringLiteral("Only local files can be attached; remote URLs "
                              "can be pasted as text.");
      paths.push_back(url.toLocalFile());
    }
    return appendAttachmentFiles(draft, paths);
  }
  const QImage image = qvariant_cast<QImage>(source.imageData());
  if (image.isNull())
    return QStringLiteral("The clipboard does not contain a readable image.");
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  const QString directory = base + QStringLiteral("/attachments");
  if (base.isEmpty() || !QDir().mkpath(directory))
    return QStringLiteral("Cannot create the pasted-image directory.");
  QTemporaryFile file(directory + QStringLiteral("/pasted-XXXXXX.png"));
  if (!file.open())
    return QStringLiteral("Cannot save the pasted image: %1")
        .arg(file.errorString());
  QImageWriter writer(&file, "png");
  if (!writer.write(image) || !file.flush())
    return QStringLiteral("Cannot save the pasted image: %1")
        .arg(writer.errorString());
  const QString error = appendAttachmentFiles(draft, {file.fileName()});
  // Paths escape the draft through queued prompts and server history. Keep the
  // private file as application data; widget/acknowledgement lifetime is
  // shorter.
  if (error.isEmpty())
    file.setAutoRemove(false);
  return error;
}

} // namespace codexui::codex
