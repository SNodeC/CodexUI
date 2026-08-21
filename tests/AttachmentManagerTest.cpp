// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/AttachmentManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

QString writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        return {};
    file.close();
    return QFileInfo(path).canonicalFilePath();
}

codexui::AttachmentInfo inspect(const QString& path)
{
    codexui::AttachmentInfo result;
    QString error;
    if (!codexui::AttachmentManager::inspectFile(path, &result, &error))
        std::cerr << error.toStdString() << '\n';
    return result;
}

bool hasOnlyOwnerPermissions(const QString& path, bool directory)
{
    const QFileDevice::Permissions permissions = QFileInfo(path).permissions();
    const QFileDevice::Permissions required = QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | (directory ? QFileDevice::ExeOwner : QFileDevice::Permissions{});
    const QFileDevice::Permissions forbidden = QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    return (permissions & required) == required
        && (permissions & forbidden) == QFileDevice::Permissions{};
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir source;
    QTemporaryDir workspace;
    if (!expect(source.isValid() && workspace.isValid(),
                "temporary attachment directories must be available"))
        return 1;

    QByteArray archiveBytes("archive bytes stay untouched");
    archiveBytes.append('\0');
    archiveBytes.append("more");
    const QString archivePath = writeFile(
        QDir(source.path()).filePath(QStringLiteral("one/code.tar.gz")), archiveBytes);
    const QString duplicatePath = writeFile(
        QDir(source.path()).filePath(QStringLiteral("two/code.tar.gz")), "second");
    const QString imagePath = writeFile(
        QDir(source.path()).filePath(QStringLiteral("screen.png")), "fake png");
    const QString preexistingMetadata =
        QDir(workspace.path()).filePath(QStringLiteral(".codex-ui"));
    const QString preexistingAttachments =
        QDir(preexistingMetadata).filePath(QStringLiteral("attachments"));
    QDir().mkpath(preexistingAttachments);
    const QString ignorePath = QDir(preexistingAttachments).filePath(QStringLiteral(".gitignore"));
    writeFile(ignorePath, "# Preserve an existing rule\n!keep-me\n");
    const QFileDevice::Permissions permissive = QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    (void)QFile::setPermissions(preexistingMetadata, permissive);
    (void)QFile::setPermissions(preexistingAttachments, permissive);

    const codexui::AttachmentInfo archive = inspect(archivePath);
    const codexui::AttachmentInfo duplicate = inspect(duplicatePath);
    const codexui::AttachmentInfo image = inspect(imagePath);
    bool passed = true;
    passed &= expect(archive.kind == codexui::AttachmentInfo::Kind::File
                         && duplicate.kind == codexui::AttachmentInfo::Kind::File
                         && image.kind == codexui::AttachmentInfo::Kind::Image,
                     "inspection must distinguish generic files from supported local images");

    codexui::AttachmentPreparation preparation;
    QString error;
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive, duplicate, image}, workspace.path(),
                         QStringLiteral("thread-123"), &preparation, &error),
                     qPrintable(error));
    passed &= expect(preparation.items.size() == 3
                         && preparation.imagePaths == QStringList{imagePath}
                         && preparation.genericFilePrompt.contains(
                             QStringLiteral("not unpacked automatically")),
                     "preparation must keep images typed and describe staged generic files");
    passed &= expect(preparation.items.at(0).staged
                         && preparation.items.at(1).staged
                         && preparation.items.at(0).effectivePath
                                != preparation.items.at(1).effectivePath,
                     "same-name generic files must receive distinct staged paths");
    const QString metadataPath = QDir(workspace.path()).filePath(QStringLiteral(".codex-ui"));
    const QString attachmentsPath = QDir(metadataPath).filePath(QStringLiteral("attachments"));
    passed &= expect(preparation.stagingLease
                         && preparation.stagingLease->directory() == preparation.stagingDirectory
                         && hasOnlyOwnerPermissions(metadataPath, true)
                         && hasOnlyOwnerPermissions(attachmentsPath, true)
                         && hasOnlyOwnerPermissions(preparation.stagingDirectory, true),
                     "attachment metadata, storage, and per-submission directories must be owner-only");
    QFile stagedArchive(preparation.items.at(0).effectivePath);
    passed &= expect(stagedArchive.open(QIODevice::ReadOnly)
                         && stagedArchive.readAll() == archiveBytes,
                     "generic attachment staging must preserve exact bytes");
    stagedArchive.close();
    passed &= expect(hasOnlyOwnerPermissions(preparation.items.at(0).effectivePath, false)
                         && hasOnlyOwnerPermissions(preparation.items.at(1).effectivePath, false),
                     "staged attachment copies must be readable and writable only by their owner");
    QFile ignoreFile(ignorePath);
    passed &= expect(ignoreFile.open(QIODevice::ReadOnly | QIODevice::Text)
                         && ignoreFile.readAll().endsWith(
                             "# Transient files staged by CodexUI\n*\n")
                         && hasOnlyOwnerPermissions(ignorePath, false),
                     "the dedicated private attachment ignore file must preserve existing rules and ignore all contents");
    const QString composed = codexui::AttachmentManager::composePrompt(
        QStringLiteral("Inspect these."), preparation);
    passed &= expect(composed.startsWith(QStringLiteral("Inspect these."))
                         && composed.contains(preparation.items.at(0).workspaceRelativePath),
                     "the submitted prompt must reference each staged workspace path");
    const QString markdownPrompt = QStringLiteral("  # Keep Markdown spacing\n\n");
    passed &= expect(codexui::AttachmentManager::composePrompt(markdownPrompt, {})
                         == markdownPrompt,
                     "prompt composition must preserve the exact user-authored Markdown source");

    const QString leasedDirectory = preparation.stagingDirectory;
    codexui::AttachmentStagingLeasePtr inFlightLease = preparation.stagingLease;
    preparation = {};
    passed &= expect(QFileInfo::exists(leasedDirectory),
                     "a staged directory must survive while an in-flight turn retains its lease");
    passed &= expect(inFlightLease->cleanup() && !QFileInfo::exists(leasedDirectory),
                     "explicit terminal cleanup must remove exact staged files and their empty directory");
    inFlightLease.reset();

    const QString cancellablePath = writeFile(
        QDir(source.path()).filePath(QStringLiteral("cancellable.bin")),
        QByteArray(2 * 1024 * 1024, 'x'));
    const codexui::AttachmentInfo cancellable = inspect(cancellablePath);
    codexui::AttachmentPreparation cancelledPreparation;
    error.clear();
    int cancellationChecks = 0;
    passed &= expect(!codexui::AttachmentManager::prepare(
                         {cancellable}, workspace.path(), QStringLiteral("thread-cancelled"),
                         &cancelledPreparation, &error,
                         [&cancellationChecks] { return ++cancellationChecks >= 5; })
                         && error.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive)
                         && cancelledPreparation.stagingDirectory.isEmpty()
                         && QDir(attachmentsPath).entryList(
                                QDir::NoDotAndDotDot | QDir::Dirs).isEmpty(),
                     "cooperative cancellation during a chunked copy must remove partial staging");

    QTemporaryDir registryDirectory;
    passed &= expect(registryDirectory.isValid(),
                     "an isolated staging registry must be available");
    const QString registryPath =
        QDir(registryDirectory.path()).filePath(QStringLiteral("attachments.ini"));
    QSettings registry(registryPath, QSettings::IniFormat);
    codexui::AttachmentPreparation restartPreparation;
    error.clear();
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive}, workspace.path(), QStringLiteral("thread-restart"),
                         &restartPreparation, &error),
                     qPrintable(error));
    const QString restartDirectory = restartPreparation.stagingDirectory;
    const QString restartFile = restartPreparation.items.constFirst().effectivePath;
    const QString restartRegistryId =
        codexui::AttachmentManager::createStagingRegistryId();
    passed &= expect(codexui::AttachmentManager::persistDispatchedStaging(
                         registry, restartRegistryId, QStringLiteral("thread-restart"), {},
                         restartPreparation.stagingLease, &error)
                         && hasOnlyOwnerPermissions(registryPath, false),
                     "dispatched attachment ownership must persist privately before correlation");
    restartPreparation = {};
    passed &= expect(QFileInfo::exists(restartDirectory) && QFileInfo::exists(restartFile),
                     "closing the originating frontend must retain dispatched attachment files");

    QList<codexui::PersistedAttachmentStaging> recovered;
    QSettings restartedRegistry(registryPath, QSettings::IniFormat);
    passed &= expect(codexui::AttachmentManager::recoverDispatchedStaging(
                         restartedRegistry, &recovered, &error)
                         && recovered.size() == 1
                         && recovered.constFirst().registryId == restartRegistryId
                         && recovered.constFirst().threadId == QStringLiteral("thread-restart")
                         && recovered.constFirst().turnId.isEmpty(),
                     "restart recovery must preserve ambiguous dispatched ownership without guessing a turn");
    passed &= expect(codexui::AttachmentManager::persistDispatchedStaging(
                         restartedRegistry, restartRegistryId,
                         QStringLiteral("thread-restart"), QStringLiteral("turn-authoritative"),
                         recovered.constFirst().stagingLease, &error),
                     "the correlated authoritative turn identity must update the existing lease record");
    recovered.clear();
    passed &= expect(QFileInfo::exists(restartDirectory),
                     "dropping a recovered lease must not clean an active backend turn");
    passed &= expect(codexui::AttachmentManager::recoverDispatchedStaging(
                         restartedRegistry, &recovered, &error)
                         && recovered.size() == 1
                         && recovered.constFirst().turnId == QStringLiteral("turn-authoritative"),
                     "a later frontend must recover the authoritative thread and turn ownership");
    passed &= expect(recovered.constFirst().stagingLease->cleanup()
                         && codexui::AttachmentManager::forgetDispatchedStaging(
                             restartedRegistry, restartRegistryId, &error)
                         && !QFileInfo::exists(restartDirectory),
                     "simulated canonical terminal state must safely clean and forget recovered staging");
    recovered.clear();
    passed &= expect(codexui::AttachmentManager::recoverDispatchedStaging(
                         restartedRegistry, &recovered, &error)
                         && recovered.isEmpty(),
                     "terminal cleanup must not recover on another restart");

    codexui::AttachmentPreparation stalePreparation;
    error.clear();
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive}, workspace.path(), QStringLiteral("thread-stale"),
                         &stalePreparation, &error),
                     qPrintable(error));
    const QString staleRegistryId =
        codexui::AttachmentManager::createStagingRegistryId();
    passed &= expect(codexui::AttachmentManager::persistDispatchedStaging(
                         restartedRegistry, staleRegistryId,
                         QStringLiteral("thread-stale"), QStringLiteral("turn-stale"),
                         stalePreparation.stagingLease, &error),
                     qPrintable(error));
    stalePreparation.stagingLease->cancelDispatch();
    passed &= expect(stalePreparation.stagingLease->cleanup(),
                     "the stale-record fixture must remove its exact staged data");
    stalePreparation = {};
    recovered.clear();
    passed &= expect(codexui::AttachmentManager::recoverDispatchedStaging(
                         restartedRegistry, &recovered, &error)
                         && recovered.isEmpty(),
                     "restart recovery must retire a registry record whose staged data is already absent");
    restartedRegistry.beginGroup(QStringLiteral("attachmentStaging/v1"));
    passed &= expect(!restartedRegistry.childGroups().contains(staleRegistryId),
                     "retiring absent staging must persistently remove its registry record");
    restartedRegistry.endGroup();

    codexui::AttachmentPreparation rejectedPreparation;
    error.clear();
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive}, workspace.path(), QStringLiteral("thread-rejected"),
                         &rejectedPreparation, &error),
                     qPrintable(error));
    const QString rejectedDirectory = rejectedPreparation.stagingDirectory;
    const QString rejectedRegistryId =
        codexui::AttachmentManager::createStagingRegistryId();
    passed &= expect(codexui::AttachmentManager::persistDispatchedStaging(
                         restartedRegistry, rejectedRegistryId,
                         QStringLiteral("thread-rejected"), {},
                         rejectedPreparation.stagingLease, &error),
                     qPrintable(error));
    rejectedPreparation.stagingLease->cancelDispatch();
    passed &= expect(rejectedPreparation.stagingLease->cleanup()
                         && codexui::AttachmentManager::forgetDispatchedStaging(
                             restartedRegistry, rejectedRegistryId, &error)
                         && !QFileInfo::exists(rejectedDirectory),
                     "an immediate rejection must clean exact files before forgetting its registry entry");
    rejectedPreparation = {};

    codexui::AttachmentPreparation tamperedPreparation;
    error.clear();
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive}, workspace.path(), QStringLiteral("thread-tampered"),
                         &tamperedPreparation, &error),
                     qPrintable(error));
    const QString tamperedDirectory = tamperedPreparation.stagingDirectory;
    const QString tamperedFile = tamperedPreparation.items.constFirst().effectivePath;
    const QString outsideFile = writeFile(
        QDir(workspace.path()).filePath(QStringLiteral("must-not-delete.txt")), "outside");
    const QString tamperedRegistryId =
        codexui::AttachmentManager::createStagingRegistryId();
    passed &= expect(codexui::AttachmentManager::persistDispatchedStaging(
                         restartedRegistry, tamperedRegistryId,
                         QStringLiteral("thread-tampered"), QStringLiteral("turn-tampered"),
                         tamperedPreparation.stagingLease, &error),
                     qPrintable(error));
    restartedRegistry.beginGroup(QStringLiteral("attachmentStaging/v1"));
    restartedRegistry.beginGroup(tamperedRegistryId);
    restartedRegistry.setValue(QStringLiteral("files"), QStringList{outsideFile});
    restartedRegistry.endGroup();
    restartedRegistry.endGroup();
    restartedRegistry.sync();
    tamperedPreparation = {};
    recovered.clear();
    passed &= expect(codexui::AttachmentManager::recoverDispatchedStaging(
                         restartedRegistry, &recovered, &error)
                         && recovered.isEmpty() && QFileInfo::exists(outsideFile)
                         && QFileInfo::exists(tamperedFile),
                     "recovery must reject paths outside the exact staging directory without deleting them");
    passed &= expect(codexui::AttachmentManager::forgetDispatchedStaging(
                         restartedRegistry, tamperedRegistryId, &error)
                         && QFile::remove(tamperedFile) && QDir().rmdir(tamperedDirectory)
                         && QFile::remove(outsideFile),
                     "the tampered recovery fixture must remain removable without recursive deletion");

    codexui::AttachmentPreparation localFailure;
    error.clear();
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive}, workspace.path(), QStringLiteral("thread-local-failure"),
                         &localFailure, &error),
                     qPrintable(error));
    const QString localFailureDirectory = localFailure.stagingDirectory;
    localFailure = {};
    passed &= expect(!QFileInfo::exists(localFailureDirectory),
                     "destroying a prepared-but-unsubmitted lease must clean local failure staging");

    codexui::AttachmentPreparation unresolved;
    error.clear();
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive}, workspace.path(), QStringLiteral("thread-unresolved"),
                         &unresolved, &error),
                     qPrintable(error));
    const QString unresolvedDirectory = unresolved.stagingDirectory;
    const QString unresolvedFile = unresolved.items.constFirst().effectivePath;
    unresolved.stagingLease->markDispatched();
    unresolved = {};
    passed &= expect(QFileInfo::exists(unresolvedDirectory) && QFileInfo::exists(unresolvedFile),
                     "destroying an unresolved lease must not remove files an accepted backend turn may still use");
    passed &= expect(QFile::remove(unresolvedFile) && QDir().rmdir(unresolvedDirectory),
                     "the unresolved-lifetime fixture must be removable without recursive deletion");

    codexui::AttachmentPreparation guardedCleanup;
    error.clear();
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive}, workspace.path(), QStringLiteral("thread-guarded"),
                         &guardedCleanup, &error),
                     qPrintable(error));
    const QString unexpectedPath = writeFile(
        QDir(guardedCleanup.stagingDirectory).filePath(QStringLiteral("backend-output.txt")),
        "must survive");
    const QString guardedStagedFile = guardedCleanup.items.constFirst().effectivePath;
    passed &= expect(!guardedCleanup.stagingLease->cleanup()
                         && !QFileInfo::exists(guardedStagedFile)
                         && QFileInfo::exists(unexpectedPath)
                         && QFileInfo::exists(guardedCleanup.stagingDirectory),
                     "cleanup must remove only tracked staged files and leave unexpected directory contents untouched");
    passed &= expect(QFile::remove(unexpectedPath)
                         && QDir().rmdir(guardedCleanup.stagingDirectory),
                     "the guarded-cleanup fixture must remain manually removable");
    guardedCleanup = {};

    codexui::AttachmentPreparation retryCleanup;
    error.clear();
    passed &= expect(codexui::AttachmentManager::prepare(
                         {archive}, workspace.path(), QStringLiteral("thread-retry"),
                         &retryCleanup, &error),
                     qPrintable(error));
    const QString replacedStagedPath = retryCleanup.items.constFirst().effectivePath;
    passed &= expect(QFile::remove(replacedStagedPath)
                         && QDir().mkpath(replacedStagedPath)
                         && !retryCleanup.stagingLease->cleanup()
                         && QFileInfo(replacedStagedPath).isDir(),
                     "cleanup must not recursively remove a directory that replaced a tracked file");
    passed &= expect(QDir().rmdir(replacedStagedPath)
                         && retryCleanup.stagingLease->cleanup()
                         && !QFileInfo::exists(retryCleanup.stagingDirectory),
                     "failed tracked paths must remain available for a safe cleanup retry");
    retryCleanup = {};

    codexui::AttachmentInfo missing = archive;
    missing.sourcePath = QDir(source.path()).filePath(QStringLiteral("removed.tar.gz"));
    codexui::AttachmentPreparation failedCopy;
    error.clear();
    passed &= expect(!codexui::AttachmentManager::prepare(
                         {archive, missing}, workspace.path(), QStringLiteral("thread-123"),
                         &failedCopy, &error)
                         && failedCopy.stagingDirectory.isEmpty()
                         && QDir(attachmentsPath).entryList(
                                QDir::NoDotAndDotDot | QDir::Dirs).isEmpty(),
                     "failed preparation must release its partially created staging directory");

    codexui::AttachmentPreparation invalid;
    error.clear();
    passed &= expect(!codexui::AttachmentManager::prepare(
                         {archive}, QStringLiteral("/definitely/not/a/workspace"),
                         QStringLiteral("thread"), &invalid, &error)
                         && error.contains(QStringLiteral("workspace"), Qt::CaseInsensitive),
                     "generic files must fail clearly when no writable workspace exists");
    return passed ? 0 : 1;
}
