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
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace codexui {
namespace {

constexpr QFileDevice::Permissions PrivateDirectoryPermissions =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
constexpr QFileDevice::Permissions PrivateFilePermissions =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr auto StagingRegistryGroup = "attachmentStaging/v1";

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
                        QString* errorMessage,
                        const AttachmentManager::CancellationCheck& cancelled)
{
    const auto reportCancellation = [errorMessage]() {
        if (errorMessage)
            *errorMessage = QStringLiteral("Attachment preparation was cancelled.");
    };
    if (cancelled && cancelled()) {
        reportCancellation();
        return false;
    }

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

    const auto cancelDestination = [&]() {
        destination.cancelWriting();
        // QSaveFile's direct-write fallback cannot roll back by itself. This
        // path is always a fresh file inside a fresh staging directory.
        (void)QFile::remove(destinationPath);
        reportCancellation();
        return false;
    };

    constexpr qint64 chunkSize = 1024 * 1024;
    QByteArray buffer(static_cast<qsizetype>(chunkSize), Qt::Uninitialized);
    while (!source.atEnd()) {
        if (cancelled && cancelled())
            return cancelDestination();
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
        if (cancelled && cancelled())
            return cancelDestination();
    }
    if (cancelled && cancelled())
        return cancelDestination();
    if (!destination.commit()) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to finish the staged attachment: %1")
                    .arg(destination.errorString()),
                destinationPath);
        return false;
    }
    if (!QFile::setPermissions(destinationPath, PrivateFilePermissions)) {
        (void)QFile::remove(destinationPath);
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to make the staged attachment private."),
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

bool ensurePrivateDirectory(const QString& path,
                            const QString& failureMessage,
                            QString* errorMessage)
{
    QFileInfo info(path);
    if (info.exists() && (!info.isDir() || info.isSymLink())) {
        if (errorMessage)
            *errorMessage = errorWithPath(failureMessage, path);
        return false;
    }
    if (!info.exists() && !QDir().mkpath(path)) {
        if (errorMessage)
            *errorMessage = errorWithPath(failureMessage, path);
        return false;
    }
    info.refresh();
    if (!info.isDir() || info.isSymLink()
        || !QFile::setPermissions(path, PrivateDirectoryPermissions)) {
        if (errorMessage)
            *errorMessage = errorWithPath(failureMessage, path);
        return false;
    }
    return true;
}

bool ensurePrivateStagingRoot(const QString& workspace,
                              QString* rootPath,
                              QString* errorMessage)
{
    QDir workspaceDirectory(workspace);
    const QString metadataPath = workspaceDirectory.filePath(QStringLiteral(".codex-ui"));
    const QString attachmentsPath = QDir(metadataPath).filePath(QStringLiteral("attachments"));
    const QString directoryError = QStringLiteral(
        "Unable to create a private CodexUI attachment directory.");
    if (!ensurePrivateDirectory(metadataPath, directoryError, errorMessage)
        || !ensurePrivateDirectory(attachmentsPath, directoryError, errorMessage)) {
        return false;
    }

    const QString ignorePath = QDir(attachmentsPath).filePath(QStringLiteral(".gitignore"));
    QFileInfo ignoreInfo(ignorePath);
    if (ignoreInfo.exists() && (!ignoreInfo.isFile() || ignoreInfo.isSymLink())) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to secure the CodexUI attachment ignore file."),
                ignorePath);
        return false;
    }
    QByteArray ignoreContents;
    bool appendIgnoreRule = true;
    if (ignoreInfo.exists()) {
        QFile existingIgnoreFile(ignorePath);
        if (!existingIgnoreFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (errorMessage)
                *errorMessage = errorWithPath(
                    QStringLiteral("Unable to read the CodexUI attachment ignore file."),
                    ignorePath);
            return false;
        }
        ignoreContents = existingIgnoreFile.readAll();
        const QList<QByteArray> lines = ignoreContents.split('\n');
        for (auto iterator = lines.crbegin(); iterator != lines.crend(); ++iterator) {
            const QByteArray line = iterator->trimmed();
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            appendIgnoreRule = line != QByteArrayLiteral("*");
            break;
        }
    }
    if (appendIgnoreRule) {
        if (!ignoreContents.isEmpty() && !ignoreContents.endsWith('\n'))
            ignoreContents.append('\n');
        ignoreContents.append("# Transient files staged by CodexUI\n*\n");
        QSaveFile ignoreFile(ignorePath);
        ignoreFile.setDirectWriteFallback(true);
        if (!ignoreFile.open(QIODevice::WriteOnly | QIODevice::Text)
            || ignoreFile.write(ignoreContents) != ignoreContents.size() || !ignoreFile.commit()) {
            if (errorMessage)
                *errorMessage = errorWithPath(
                    QStringLiteral("Unable to secure the CodexUI attachment ignore file."),
                    ignorePath);
            return false;
        }
    }
    if (!QFile::setPermissions(ignorePath, PrivateFilePermissions)) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to secure the CodexUI attachment ignore file."),
                ignorePath);
        return false;
    }
    if (rootPath)
        *rootPath = attachmentsPath;
    return true;
}

bool validRegistryId(const QString& registryId)
{
    const QUuid id(QStringLiteral("{%1}").arg(registryId));
    return !id.isNull() && id.toString(QUuid::WithoutBraces) == registryId;
}

QString cleanAbsolutePath(const QString& path)
{
    if (!QDir::isAbsolutePath(path))
        return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isSafeStagingPath(const QString& workspace,
                       const QString& stagingDirectory,
                       const QStringList& stagedFiles)
{
    const QString cleanWorkspace = cleanAbsolutePath(workspace);
    const QString cleanStagingDirectory = cleanAbsolutePath(stagingDirectory);
    if (cleanWorkspace.isEmpty() || cleanStagingDirectory.isEmpty())
        return false;

    const QFileInfo workspaceInfo(cleanWorkspace);
    if (!workspaceInfo.exists() || !workspaceInfo.isDir() || workspaceInfo.isSymLink()
        || workspaceInfo.canonicalFilePath() != cleanWorkspace) {
        return false;
    }

    const QString metadataDirectory = QDir(cleanWorkspace).filePath(QStringLiteral(".codex-ui"));
    const QString stagingRoot = QDir(metadataDirectory).filePath(QStringLiteral("attachments"));
    const QFileInfo stagingInfo(cleanStagingDirectory);
    if (QDir::cleanPath(stagingInfo.absolutePath()) != QDir::cleanPath(stagingRoot)
        || stagingInfo.fileName().isEmpty() || stagingInfo.fileName() == QStringLiteral(".")
        || stagingInfo.fileName() == QStringLiteral("..")) {
        return false;
    }

    for (const QString& component : {metadataDirectory, stagingRoot, cleanStagingDirectory}) {
        const QFileInfo info(component);
        if (info.exists() && (info.isSymLink() || !info.isDir()))
            return false;
    }

    QSet<QString> uniqueFiles;
    for (const QString& path : stagedFiles) {
        const QString cleanPath = cleanAbsolutePath(path);
        const QFileInfo fileInfo(cleanPath);
        if (cleanPath.isEmpty()
            || QDir::cleanPath(fileInfo.absolutePath()) != cleanStagingDirectory
            || fileInfo.fileName().isEmpty() || fileInfo.fileName() == QStringLiteral(".")
            || fileInfo.fileName() == QStringLiteral("..")
            || uniqueFiles.contains(cleanPath)) {
            return false;
        }
        if (fileInfo.exists() && (fileInfo.isSymLink() || fileInfo.isDir()))
            return false;
        uniqueFiles.insert(cleanPath);
    }
    return !stagedFiles.isEmpty();
}

bool stagingArtifactsAreAbsent(const QString& stagingDirectory,
                               const QStringList& stagedFiles)
{
    const QString cleanStagingDirectory = cleanAbsolutePath(stagingDirectory);
    if (cleanStagingDirectory.isEmpty())
        return false;

    const QFileInfo directoryInfo(cleanStagingDirectory);
    if (directoryInfo.exists() || directoryInfo.isSymLink())
        return false;
    return std::ranges::all_of(stagedFiles, [](const QString& path) {
        const QString cleanPath = cleanAbsolutePath(path);
        if (cleanPath.isEmpty())
            return false;
        const QFileInfo info(cleanPath);
        return !info.exists() && !info.isSymLink();
    });
}

bool synchronizePrivateSettings(QSettings& settings, QString* errorMessage)
{
    const QString fileName = settings.fileName();
    QFileInfo settingsInfo(fileName);
    if (settingsInfo.isSymLink()
        || (settingsInfo.exists()
            && (!settingsInfo.isFile()
                || !QFile::setPermissions(fileName, PrivateFilePermissions)))) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to make the attachment staging registry private."),
                fileName);
        return false;
    }

    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to update the attachment staging registry.");
        return false;
    }

    settingsInfo.refresh();
    if (settingsInfo.exists()
        && (!settingsInfo.isFile() || settingsInfo.isSymLink()
            || !QFile::setPermissions(fileName, PrivateFilePermissions))) {
        if (errorMessage)
            *errorMessage = errorWithPath(
                QStringLiteral("Unable to make the attachment staging registry private."),
                fileName);
        return false;
    }
    return true;
}

} // namespace

AttachmentStagingLease::AttachmentStagingLease(QString workspace, QString directory)
    : workspaceDirectory(std::move(workspace))
    , stagingDirectory(std::move(directory))
{
}

AttachmentStagingLease::~AttachmentStagingLease()
{
    if (!dispatched)
        (void)cleanup();
}

void AttachmentStagingLease::trackFile(QString path)
{
    stagedFiles.append(std::move(path));
}

const QString& AttachmentStagingLease::directory() const noexcept
{
    return stagingDirectory;
}

bool AttachmentStagingLease::cleanup() noexcept
{
    if (stagingDirectory.isEmpty())
        return true;

    QFileInfo directoryInfo(stagingDirectory);
    if (directoryInfo.exists() && (!directoryInfo.isDir() || directoryInfo.isSymLink()))
        return false;

    bool removed = true;
    QStringList remainingFiles;
    const QString expectedDirectory = QDir(stagingDirectory).absolutePath();
    for (const QString& path : std::as_const(stagedFiles)) {
        const QFileInfo fileInfo(path);
        if (!fileInfo.exists() && !fileInfo.isSymLink())
            continue;
        if (fileInfo.absolutePath() != expectedDirectory
            || fileInfo.isDir() || !QFile::remove(path)) {
            removed = false;
            remainingFiles.append(path);
        }
    }
    stagedFiles = std::move(remainingFiles);

    directoryInfo.refresh();
    if (directoryInfo.exists()) {
        if (!directoryInfo.isDir() || directoryInfo.isSymLink()
            || !QDir().rmdir(stagingDirectory)) {
            removed = false;
        }
    }
    return removed;
}

void AttachmentStagingLease::markDispatched() noexcept
{
    dispatched = true;
}

void AttachmentStagingLease::cancelDispatch() noexcept
{
    dispatched = false;
}

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
                                QString* errorMessage,
                                CancellationCheck cancelled)
{
    if (!result) {
        if (errorMessage)
            *errorMessage = QStringLiteral("No attachment preparation result object was provided.");
        return false;
    }
    *result = {};
    if (cancelled && cancelled()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Attachment preparation was cancelled.");
        return false;
    }
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
        if (!ensurePrivateDirectory(
                stagingDirectory,
                QStringLiteral("Unable to create a private directory for staged attachments."),
                errorMessage)) {
            (void)QDir().rmdir(stagingDirectory);
            return false;
        }
        result->stagingDirectory = stagingDirectory;
        result->stagingLease = std::shared_ptr<AttachmentStagingLease>(
            new AttachmentStagingLease(canonicalWorkspace, stagingDirectory));
    }

    QSet<QString> occupiedNames;
    QStringList promptLines;
    for (const AttachmentInfo& attachment : attachments) {
        if (cancelled && cancelled()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Attachment preparation was cancelled.");
            if (result->stagingLease)
                (void)result->stagingLease->cleanup();
            *result = {};
            return false;
        }
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
            if (!copyFileAtomically(attachment.sourcePath,
                                    prepared.effectivePath,
                                    errorMessage,
                                    cancelled)) {
                (void)result->stagingLease->cleanup();
                *result = {};
                return false;
            }
            result->stagingLease->trackFile(prepared.effectivePath);
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

QString AttachmentManager::createStagingRegistryId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool AttachmentManager::persistDispatchedStaging(
    QSettings& settings,
    const QString& registryId,
    const QString& threadId,
    const QString& turnId,
    const AttachmentStagingLeasePtr& stagingLease,
    QString* errorMessage)
{
    if (!stagingLease || !validRegistryId(registryId) || threadId.isEmpty()
        || !isSafeStagingPath(stagingLease->workspaceDirectory,
                              stagingLease->stagingDirectory,
                              stagingLease->stagedFiles)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Invalid attachment staging ownership metadata.");
        return false;
    }

    settings.beginGroup(QString::fromLatin1(StagingRegistryGroup));
    settings.beginGroup(registryId);
    settings.setValue(QStringLiteral("workspace"), stagingLease->workspaceDirectory);
    settings.setValue(QStringLiteral("directory"), stagingLease->stagingDirectory);
    settings.setValue(QStringLiteral("files"), stagingLease->stagedFiles);
    settings.setValue(QStringLiteral("threadId"), threadId);
    settings.setValue(QStringLiteral("turnId"), turnId);
    settings.endGroup();
    settings.endGroup();
    if (!synchronizePrivateSettings(settings, errorMessage))
        return false;
    stagingLease->markDispatched();
    return true;
}

bool AttachmentManager::recoverDispatchedStaging(
    QSettings& settings,
    QList<PersistedAttachmentStaging>* result,
    QString* errorMessage)
{
    if (!result) {
        if (errorMessage)
            *errorMessage = QStringLiteral("No attachment staging recovery result was provided.");
        return false;
    }
    result->clear();
    if (!synchronizePrivateSettings(settings, errorMessage))
        return false;

    settings.beginGroup(QString::fromLatin1(StagingRegistryGroup));
    const QStringList registryIds = settings.childGroups();
    bool removedStaleRecord = false;
    for (const QString& registryId : registryIds) {
        if (!validRegistryId(registryId))
            continue;
        settings.beginGroup(registryId);
        const QString workspace = settings.value(QStringLiteral("workspace")).toString();
        const QString directory = settings.value(QStringLiteral("directory")).toString();
        const QStringList files = settings.value(QStringLiteral("files")).toStringList();
        const QString threadId = settings.value(QStringLiteral("threadId")).toString();
        const QString turnId = settings.value(QStringLiteral("turnId")).toString();
        settings.endGroup();
        if (stagingArtifactsAreAbsent(directory, files)) {
            settings.remove(registryId);
            removedStaleRecord = true;
            continue;
        }
        if (threadId.isEmpty() || !isSafeStagingPath(workspace, directory, files))
            continue;

        auto stagingLease = std::shared_ptr<AttachmentStagingLease>(
            new AttachmentStagingLease(workspace, directory));
        for (const QString& file : files)
            stagingLease->trackFile(cleanAbsolutePath(file));
        stagingLease->markDispatched();
        result->append(PersistedAttachmentStaging{
            registryId, threadId, turnId, std::move(stagingLease)});
    }
    settings.endGroup();
    return !removedStaleRecord || synchronizePrivateSettings(settings, errorMessage);
}

bool AttachmentManager::forgetDispatchedStaging(QSettings& settings,
                                                 const QString& registryId,
                                                 QString* errorMessage)
{
    if (!validRegistryId(registryId)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Invalid attachment staging registry identity.");
        return false;
    }
    settings.beginGroup(QString::fromLatin1(StagingRegistryGroup));
    settings.remove(registryId);
    settings.endGroup();
    return synchronizePrivateSettings(settings, errorMessage);
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
