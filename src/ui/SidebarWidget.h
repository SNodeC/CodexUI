// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_SIDEBARWIDGET_H
#define CODEXUI_UI_SIDEBARWIDGET_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QFrame;
class QLabel;
class QLockFile;
class QPushButton;
class QSettings;
class QTreeWidget;
class QTreeWidgetItem;

namespace ai::openai::codex::frontend::client {
class State;
struct ThreadState;
}

namespace codexui {

enum class ThreadAction {
    Open,
    Rename,
    Fork,
    Interrupt,
    ResumeWithOptions,
    Archive,
    Unarchive,
    Delete,
    CopyId,
};

struct ThreadActionAvailability {
    bool open = true;
    bool rename = true;
    bool fork = true;
    bool interrupt = false;
    bool resumeWithOptions = true;
    bool archive = false;
    bool unarchive = false;
    bool remove = true;

    bool operator==(const ThreadActionAvailability&) const = default;
};

namespace detail {
struct ThreadUiStatus {
    ThreadActionAvailability actions;
    bool running = false;
    bool awaitingResponse = false;
    bool archived = false;

    bool operator==(const ThreadUiStatus&) const = default;
};

[[nodiscard]] ThreadUiStatus
threadUiStatus(const ai::openai::codex::frontend::client::State& state,
               const ai::openai::codex::frontend::client::ThreadState& thread,
               bool awaitingResponse = false);

[[nodiscard]] ThreadActionAvailability
threadActionAvailability(const ai::openai::codex::frontend::client::State& state,
                         const ai::openai::codex::frontend::client::ThreadState& thread);

struct ThreadFolder {
    QString id;
    QString name;
    QString parentId;
    bool expanded = true;

    bool operator==(const ThreadFolder&) const = default;
};

class ThreadOrganization
{
public:
    void load(QSettings& settings);
    [[nodiscard]] bool save(QSettings& settings) const;

    [[nodiscard]] const std::vector<ThreadFolder>& folders() const noexcept;
    [[nodiscard]] const ThreadFolder* folder(const QString& folderId) const noexcept;
    [[nodiscard]] QString folderForThread(const QString& threadId) const;
    [[nodiscard]] QString folderPath(const QString& folderId) const;
    [[nodiscard]] quint64 revision() const noexcept;

    [[nodiscard]] QString createFolder(const QString& name, const QString& parentId = {});
    [[nodiscard]] bool renameFolder(const QString& folderId, const QString& name);
    [[nodiscard]] bool moveFolder(const QString& folderId, const QString& parentId);
    [[nodiscard]] bool removeFolderAndPromoteContents(const QString& folderId);
    [[nodiscard]] bool moveThread(const QString& threadId, const QString& folderId);
    [[nodiscard]] bool retainThreadAssignments(const QSet<QString>& threadIds);
    [[nodiscard]] bool setFolderExpanded(const QString& folderId, bool expanded);
    [[nodiscard]] QSet<QString> movableFolderParents(const QString& folderId) const;
    [[nodiscard]] bool canMoveFolder(const QString& folderId, const QString& parentId) const;

private:
    [[nodiscard]] bool validName(const QString& name,
                                 const QString& parentId,
                                 const QString& excludedFolderId = {}) const;
    [[nodiscard]] bool isDescendantOf(const QString& folderId,
                                      const QString& possibleAncestorId) const;
    void normalize();

    std::vector<ThreadFolder> storedFolders;
    QHash<QString, QString> threadFolders;
    qsizetype currentStorageBytes = 0;
    quint64 currentRevision = 0;
};
}

class SidebarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SidebarWidget(QWidget* parent = nullptr);
    ~SidebarWidget() override;
    void setThreads(const ai::openai::codex::frontend::client::State& state,
                    const QString& selectedThreadId,
                    bool allThreadDiscoveryComplete);
    void updateThreads(const ai::openai::codex::frontend::client::State& state,
                       const QString& selectedThreadId,
                       bool allThreadDiscoveryComplete,
                       const QStringList& affectedThreadIds);
    void setConnectionStatus(const QString& title, const QString& detail, const QString& color);
    void setNewThreadEnabled(bool enabled);
    void setThreadInteractionEnabled(bool enabled);

signals:
    void hideRequested();
    void newThreadRequested();
    void threadSelected(const QString& threadId);
    void threadActionRequested(const QString& threadId, codexui::ThreadAction action);

private:
    struct ThreadPresentation {
        QString id;
        QString title;
        QString details;
        QString color;
        ThreadActionAvailability actions;
        bool running = false;
        bool attention = false;
        bool archived = false;

        bool operator==(const ThreadPresentation&) const = default;
    };

    void renderThreadTree();
    [[nodiscard]] ThreadPresentation threadPresentation(
        const ai::openai::codex::frontend::client::State& state,
        const ai::openai::codex::frontend::client::ThreadState& thread,
        bool awaitingResponse) const;
    void rebuildRenderedThreadIndex();
    void updateRenderedRows(const QSet<QString>& threadIds);
    void tryAcquireOrganizationLock();
    void persistOrganization();
    void createFolder(const QString& parentFolderId = {});
    void renameFolder(const QString& folderId);
    void moveFolder(const QString& folderId, const QString& parentFolderId);
    void deleteFolder(const QString& folderId);
    void moveThread(const QString& threadId, const QString& folderId);
    void showFolderContextMenu(QTreeWidgetItem* item, const QPoint& globalPosition);

    QTreeWidget* threadTree = nullptr;
    QFrame* serverDot = nullptr;
    QLabel* serverTitle = nullptr;
    QLabel* serverDetail = nullptr;
    QPushButton* newThread = nullptr;
    QPushButton* newFolder = nullptr;
    std::vector<ThreadPresentation> renderedThreads;
    QHash<QString, qsizetype> renderedThreadIndexes;
    QHash<QString, QWidget*> renderedThreadRows;
    QString renderedSelection;
    detail::ThreadOrganization organization;
    quint64 renderedOrganizationRevision = 0;
    bool threadsRendered = false;
    bool threadInteractionEnabled = false;
    bool rebuildingTree = false;
    QLockFile* organizationLock = nullptr;
    bool organizationWritable = false;
    bool organizationPersistenceFailureReported = false;
};

} // namespace codexui

#endif // CODEXUI_UI_SIDEBARWIDGET_H
