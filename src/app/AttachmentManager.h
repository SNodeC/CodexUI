// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_APP_ATTACHMENTMANAGER_H
#define CODEXUI_APP_ATTACHMENTMANAGER_H

#include <QList>
#include <QString>
#include <QStringList>

namespace codexui {

struct AttachmentInfo
{
    enum class Kind { File, Image };

    QString sourcePath;
    QString displayName;
    QString mimeType;
    qint64 sizeBytes = 0;
    Kind kind = Kind::File;

    bool operator==(const AttachmentInfo&) const = default;
};

struct PreparedAttachment
{
    AttachmentInfo source;
    QString effectivePath;
    QString workspaceRelativePath;
    bool staged = false;
};

struct AttachmentPreparation
{
    QList<PreparedAttachment> items;
    QStringList imagePaths;
    QString genericFilePrompt;
    QString stagingDirectory;
};

class AttachmentManager final
{
public:
    // Staging is deliberately synchronous and therefore bounded well below
    // filesystem limits. Image contents travel by local path, not on the
    // frontend protocol wire.
    static constexpr qint64 MaximumSingleFileBytes = 64LL * 1024LL * 1024LL;
    static constexpr qint64 MaximumTotalBytes = 256LL * 1024LL * 1024LL;

    [[nodiscard]] static bool inspectFile(const QString& path,
                                          AttachmentInfo* result,
                                          QString* errorMessage = nullptr);
    [[nodiscard]] static bool validateForWorkspace(const QList<AttachmentInfo>& attachments,
                                                   const QString& workspace,
                                                   QString* errorMessage = nullptr);
    [[nodiscard]] static bool prepare(const QList<AttachmentInfo>& attachments,
                                      const QString& workspace,
                                      const QString& threadId,
                                      AttachmentPreparation* result,
                                      QString* errorMessage = nullptr);
    [[nodiscard]] static QString composePrompt(const QString& userPrompt,
                                               const AttachmentPreparation& preparation);
    [[nodiscard]] static QString formatSize(qint64 sizeBytes);
    [[nodiscard]] static qint64 totalSize(const QList<AttachmentInfo>& attachments);

private:
    [[nodiscard]] static bool isSupportedLocalImage(const QString& path,
                                                    const QString& mimeType);
};

} // namespace codexui

#endif // CODEXUI_APP_ATTACHMENTMANAGER_H
