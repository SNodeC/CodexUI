// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/AttachmentManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    QFile stagedArchive(preparation.items.at(0).effectivePath);
    passed &= expect(stagedArchive.open(QIODevice::ReadOnly)
                         && stagedArchive.readAll() == archiveBytes,
                     "generic attachment staging must preserve exact bytes");
    passed &= expect(QFileInfo::exists(
                         QDir(workspace.path()).filePath(QStringLiteral(".codex-ui/.gitignore"))),
                     "transient workspace attachments must be ignored by source control");
    const QString composed = codexui::AttachmentManager::composePrompt(
        QStringLiteral("Inspect these."), preparation);
    passed &= expect(composed.startsWith(QStringLiteral("Inspect these."))
                         && composed.contains(preparation.items.at(0).workspaceRelativePath),
                     "the submitted prompt must reference each staged workspace path");
    const QString markdownPrompt = QStringLiteral("  # Keep Markdown spacing\n\n");
    passed &= expect(codexui::AttachmentManager::composePrompt(markdownPrompt, {})
                         == markdownPrompt,
                     "prompt composition must preserve the exact user-authored Markdown source");

    codexui::AttachmentPreparation invalid;
    error.clear();
    passed &= expect(!codexui::AttachmentManager::prepare(
                         {archive}, QStringLiteral("/definitely/not/a/workspace"),
                         QStringLiteral("thread"), &invalid, &error)
                         && error.contains(QStringLiteral("workspace"), Qt::CaseInsensitive),
                     "generic files must fail clearly when no writable workspace exists");
    return passed ? 0 : 1;
}
