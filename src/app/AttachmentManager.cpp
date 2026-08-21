// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/AttachmentManager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace codexui {
namespace {

QString errorWithPath(const QString& message, const QString& path)
{
    return QStringLiteral("%1\n\n%2").arg(message, QDir::toNativeSeparators(path));
}

QString safeFileName(const QString& name)
{
    QString result = QFileInfo(name).fileName();
    if (result.isEmpty() || result == QStringLiteral(".") || result == QStringLiteral(".."))
        result = QStringLiteral("attachment");
    for (QChar& character : result) {
        if (character.unicode() < 0x20 || character == QLatin1Char('/')
            || character == QLatin1Char('\\'))
            character = QLatin1Char('_');
    }
    return result;
}

QString uniqueDestinationName(const QString& requested, QSet<QString>& occupiedNames)
{
    const QFileInfo info(requested);
    const QString suffix = info.completeSuffix();
    const QString base = info.completeBaseName().isEmpty()
        ? QStringLiteral("attachment") : info.completeBaseName();
    QString candidate = requested;
    int ordinal = 2;
    while (occupiedNames.contains(candidate.toCaseFolded())) {
        candidate = suffix.isEmpty()
            ? QStringLiteral("%1-%2").arg(base).arg(ordinal)
            : QStringLiteral("%1-%2.%3").arg(base).arg(ordinal).arg(suffix);
        ++ordinal;
    }
    occupiedNames.insert(candidate.toCaseFolded());
    return candidate;
}

bool copyFileAtomically(const QString& sourcePath,
                        const QString& destinationPath,
                        QString* errorMessage)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to open the attachment: %1").arg(source.errorString()),
                sourcePath);
        return false;
    }

    QSaveFile destination(destinationPath);
    destination.setDirectWriteFallback(true);
    if (!destination.open(QIODevice::WriteOnly)) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to create the staged attachment: %1")
                    .arg(destination.errorString()),
                destinationPath);
        return false;
    }

    constexpr qint64 chunkSize = 1024 * 1024;
    QByteArray buffer(static_cast<qsizetype>(chunkSize), Qt::Uninitialized);
    while (!source.atEnd()) {
        const qint64 count = source.read(buffer.data(), chunkSize);
        if (count < 0 || (count > 0 && destination.write(buffer.constData(), count) != count)) {
            destination.cancelWriting();
            if (errorMessage)
                *errorMessage = errorWithPath(
                    count < 0
                        ? QStringLiteral("Unable to read the attachment: %1").arg(source.errorString())
                        : QStringLiteral("Unable to write the staged attachment: %1")
                              .arg(destination.errorString()),
                    count < 0 ? sourcePath : destinationPath);
            return false;
        }
        if (count == 0)
            break;
    }
    if (!destination.commit()) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to finish the staged attachment: %1")
                    .arg(destination.errorString()),
                destinationPath);
        return false;
    }
    return true;
}

QString stagingThreadToken(const QString& threadId)
{
    const QByteArray source = threadId.isEmpty() ? QByteArrayLiteral("pending-thread")
                                                  : threadId.toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex().left(16));
}

bool ensurePrivateStagingRoot(const QString& workspace,
                              QString* rootPath,
                              QString* errorMessage)
{
    QDir workspaceDirectory(workspace);
    const QString metadataPath = workspaceDirectory.filePath(QStringLiteral(".codex-ui"));
    if (!workspaceDirectory.mkpath(QStringLiteral(".codex-ui/attachments"))) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to create the CodexUI attachment directory."),
                metadataPath);
        return false;
    }

    const QString ignorePath = QDir(metadataPath).filePath(QStringLiteral(".gitignore"));
    if (!QFileInfo::exists(ignorePath)) {
        QSaveFile ignoreFile(ignorePath);
        ignoreFile.setDirectWriteFallback(true);
        if (ignoreFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            ignoreFile.write("# Transient files staged by CodexUI\n*\n");
            (void)ignoreFile.commit();
        }
    }
    if (rootPath)
        *rootPath = QDir(metadataPath).filePath(QStringLiteral("attachments"));
    return true;
}

} // namespace

bool AttachmentManager::inspectFile(const QString& path,
                                    AttachmentInfo* result,
                                    QString* errorMessage)
{
    if (!result) {
        if (errorMessage)
            *errorMessage = QStringLiteral("No attachment result object was provided.");
        return false;
    }
    QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("The selected attachment does not exist or cannot be resolved."),
                path);
        return false;
    }
    info.setFile(canonicalPath);
    if (!info.isFile() || !info.isReadable()) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                info.isFile() ? QStringLiteral("The selected attachment is not readable.")
                              : QStringLiteral("The selected attachment is not a regular file."),
                canonicalPath);
        return false;
    }
    if (info.size() > MaximumSingleFileBytes) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("The selected attachment exceeds the %1 per-file limit.")
                    .arg(formatSize(MaximumSingleFileBytes)),
                canonicalPath);
        return false;
    }

    QMimeDatabase mimeDatabase;
    QString mimeType = mimeDatabase.mimeTypeForFile(
        canonicalPath, QMimeDatabase::MatchExtension).name();
    if (mimeType.isEmpty())
        mimeType = QStringLiteral("application/octet-stream");
    *result = AttachmentInfo{
        canonicalPath,
        info.fileName(),
        mimeType,
        info.size(),
        isSupportedLocalImage(canonicalPath, mimeType)
            ? AttachmentInfo::Kind::Image : AttachmentInfo::Kind::File};
    return true;
}

bool AttachmentManager::validateForWorkspace(const QList<AttachmentInfo>& attachments,
                                             const QString& workspace,
                                             QString* errorMessage)
{
    if (totalSize(attachments) > MaximumTotalBytes) {
        if (errorMessage)
            *errorMessage = QStringLiteral(
                "The selected attachments exceed the %1 total attachment limit.")
                                .arg(formatSize(MaximumTotalBytes));
        return false;
    }
    const bool hasGenericFile = std::ranges::any_of(attachments, [](const auto& attachment) {
        return attachment.kind == AttachmentInfo::Kind::File;
    });
    if (!hasGenericFile)
        return true;

    const QFileInfo workspaceInfo(workspace.trimmed());
    if (!workspaceInfo.exists() || !workspaceInfo.isDir()
        || !workspaceInfo.isReadable() || !workspaceInfo.isWritable()) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("A readable and writable workspace is required for non-image attachments."),
                workspace.trimmed());
        return false;
    }
    return true;
}

bool AttachmentManager::prepare(const QList<AttachmentInfo>& attachments,
                                const QString& workspace,
                                const QString& threadId,
                                AttachmentPreparation* result,
                                QString* errorMessage)
{
    if (!result) {
        if (errorMessage)
            *errorMessage = QStringLiteral("No attachment preparation result object was provided.");
        return false;
    }
    *result = {};
    if (!validateForWorkspace(attachments, workspace, errorMessage))
        return false;

    const bool hasGenericFile = std::ranges::any_of(attachments, [](const auto& attachment) {
        return attachment.kind == AttachmentInfo::Kind::File;
    });
    QString stagingDirectory;
    const QString canonicalWorkspace = QFileInfo(workspace).canonicalFilePath();
    if (hasGenericFile) {
        QString stagingRoot;
        if (!ensurePrivateStagingRoot(canonicalWorkspace, &stagingRoot, errorMessage))
            return false;
        stagingDirectory = QDir(stagingRoot).filePath(
            QStringLiteral("%1-%2-%3")
                .arg(stagingThreadToken(threadId),
                     QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
                     QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().mkpath(stagingDirectory)) {
            if (errorMessage)
                *errorMessage = errorWithPath(
                    QStringLiteral("Unable to create a directory for staged attachments."),
                    stagingDirectory);
            return false;
        }
        result->stagingDirectory = stagingDirectory;
    }

    QSet<QString> occupiedNames;
    QStringList promptLines;
    for (const AttachmentInfo& attachment : attachments) {
        PreparedAttachment prepared;
        prepared.source = attachment;
        if (attachment.kind == AttachmentInfo::Kind::Image) {
            prepared.effectivePath = attachment.sourcePath;
            result->imagePaths.append(prepared.effectivePath);
        } else {
            const QString destinationName = uniqueDestinationName(
                safeFileName(attachment.displayName), occupiedNames);
            prepared.effectivePath = QDir(stagingDirectory).filePath(destinationName);
            prepared.staged = true;
            if (!copyFileAtomically(attachment.sourcePath, prepared.effectivePath, errorMessage))
                return false;
            prepared.workspaceRelativePath = QDir::fromNativeSeparators(
                QDir(canonicalWorkspace).relativeFilePath(prepared.effectivePath));
            if (!prepared.workspaceRelativePath.startsWith(QStringLiteral("./")))
                prepared.workspaceRelativePath.prepend(QStringLiteral("./"));
            promptLines.append(QStringLiteral("- `%1` (original `%2`, %3, `%4`)")
                                   .arg(prepared.workspaceRelativePath,
                                        attachment.displayName,
                                        formatSize(attachment.sizeBytes),
                                        attachment.mimeType));
        }
        result->items.append(std::move(prepared));
    }
    if (!promptLines.isEmpty()) {
        result->genericFilePrompt = QStringLiteral(
            "The following local files were attached by the user and copied into the workspace by CodexUI. "
            "Read them as input for this turn. Archives are not unpacked automatically; inspect or extract "
            "them only when needed and permitted.\n%1")
                                        .arg(promptLines.join(QLatin1Char('\n')));
    }
    return true;
}

QString AttachmentManager::composePrompt(const QString& userPrompt,
                                         const AttachmentPreparation& preparation)
{
    QString result = userPrompt;
    if (!preparation.genericFilePrompt.isEmpty()) {
        if (!result.trimmed().isEmpty())
            result += QStringLiteral("\n\n");
        else
            result.clear();
        result += preparation.genericFilePrompt;
    }
    return result;
}

QString AttachmentManager::formatSize(qint64 sizeBytes)
{
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KiB"),
                                   QStringLiteral("MiB"), QStringLiteral("GiB")};
    double value = static_cast<double>(std::max<qint64>(0, sizeBytes));
    qsizetype unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    const int precision = unit == 0 ? 0 : (value >= 100.0 ? 0 : 1);
    return QStringLiteral("%1 %2").arg(value, 0, 'f', precision).arg(units.at(unit));
}

qint64 AttachmentManager::totalSize(const QList<AttachmentInfo>& attachments)
{
    qint64 total = 0;
    for (const auto& attachment : attachments) {
        if (attachment.sizeBytes > 0 && total > MaximumTotalBytes - attachment.sizeBytes)
            return MaximumTotalBytes + 1;
        total += std::max<qint64>(0, attachment.sizeBytes);
    }
    return total;
}

bool AttachmentManager::isSupportedLocalImage(const QString& path, const QString& mimeType)
{
    static const QSet<QString> extensions{QStringLiteral("png"), QStringLiteral("jpg"),
                                          QStringLiteral("jpeg"), QStringLiteral("webp"),
                                          QStringLiteral("gif"), QStringLiteral("bmp")};
    return mimeType.startsWith(QStringLiteral("image/"))
        && extensions.contains(QFileInfo(path).suffix().toLower());
}

} // namespace codexui
