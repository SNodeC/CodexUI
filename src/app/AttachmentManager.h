// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_APP_ATTACHMENTMANAGER_H
#define CODEXUI_APP_ATTACHMENTMANAGER_H

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

class QSettings;

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

class AttachmentStagingLease final
{
public:
    ~AttachmentStagingLease();

    AttachmentStagingLease(const AttachmentStagingLease&) = delete;
    AttachmentStagingLease& operator=(const AttachmentStagingLease&) = delete;

    [[nodiscard]] const QString& directory() const noexcept;
    // Prepared-but-unsubmitted leases clean up on destruction. Dispatch makes
    // cleanup explicit so destroying the frontend cannot break an active turn.
    [[nodiscard]] bool cleanup() noexcept;
    void markDispatched() noexcept;
    void cancelDispatch() noexcept;

private:
    friend class AttachmentManager;
    AttachmentStagingLease(QString workspace, QString directory);
    void trackFile(QString path);

    QString workspaceDirectory;
    QString stagingDirectory;
    QStringList stagedFiles;
    bool dispatched = false;
};

using AttachmentStagingLeasePtr = std::shared_ptr<AttachmentStagingLease>;

struct AttachmentPreparation
{
    QList<PreparedAttachment> items;
    QStringList imagePaths;
    QString genericFilePrompt;
    QString stagingDirectory;
    AttachmentStagingLeasePtr stagingLease;
};

struct PersistedAttachmentStaging
{
    QString registryId;
    QString threadId;
    QString turnId;
    AttachmentStagingLeasePtr stagingLease;
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
    [[nodiscard]] static QString createStagingRegistryId();
    [[nodiscard]] static bool persistDispatchedStaging(
        QSettings& settings,
        const QString& registryId,
        const QString& threadId,
        const QString& turnId,
        const AttachmentStagingLeasePtr& stagingLease,
        QString* errorMessage = nullptr);
    [[nodiscard]] static bool recoverDispatchedStaging(
        QSettings& settings,
        QList<PersistedAttachmentStaging>* result,
        QString* errorMessage = nullptr);
    [[nodiscard]] static bool forgetDispatchedStaging(
        QSettings& settings,
        const QString& registryId,
        QString* errorMessage = nullptr);

private:
    [[nodiscard]] static bool isSupportedLocalImage(const QString& path,
                                                    const QString& mimeType);
};

} // namespace codexui

#endif // CODEXUI_APP_ATTACHMENTMANAGER_H
