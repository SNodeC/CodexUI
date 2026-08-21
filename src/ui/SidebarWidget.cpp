// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/SidebarWidget.h"

#include <ai/openai/codex/frontend/client/State.h>

#include <QEnterEvent>
#include <QContextMenuEvent>
#include <QDebug>
#include <QDir>
#include <QFrame>
#include <QFocusEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLockFile>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSet>
#include <QTextDocument>
#include <QStyle>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace codexui {
namespace {

constexpr int itemKindRole = Qt::UserRole;
constexpr int stableIdRole = Qt::UserRole + 1;
constexpr std::size_t maximumOrganizationFolders = 2'048;
constexpr qsizetype maximumOrganizationAssignments = 8'192;
constexpr std::size_t maximumOrganizationDepth = 32;
constexpr qsizetype maximumOrganizationStorageBytes = 4 * 1024 * 1024;
const QString folderItemKind = QStringLiteral("folder");
const QString sectionItemKind = QStringLiteral("section");

QByteArray serializeOrganization(const std::vector<detail::ThreadFolder>& folders,
                                 const QHash<QString, QString>& threadFolders)
{
    QJsonArray serializedFolders;
    for (const auto& folder : folders) {
        serializedFolders.append(QJsonObject{{QStringLiteral("id"), folder.id},
                                              {QStringLiteral("name"), folder.name},
                                              {QStringLiteral("parentId"), folder.parentId},
                                              {QStringLiteral("expanded"), folder.expanded}});
    }
    QJsonObject assignments;
    for (auto iterator = threadFolders.constBegin(); iterator != threadFolders.constEnd(); ++iterator)
        assignments.insert(iterator.key(), iterator.value());
    return QJsonDocument(QJsonObject{{QStringLiteral("folders"), serializedFolders},
                                     {QStringLiteral("threadFolders"), assignments}})
        .toJson(QJsonDocument::Compact);
}

qsizetype serializedAssignmentMemberBytes(const QString& threadId, const QString& folderId)
{
    // Remove the surrounding object braces. The remaining bytes are exactly
    // one JSON object member, including key/value escaping.
    return QJsonDocument(QJsonObject{{threadId, folderId}})
               .toJson(QJsonDocument::Compact)
               .size()
           - 2;
}

bool validThreadOrganizationId(const QString& threadId)
{
    return !threadId.isEmpty() && threadId.size() <= 1'024
           && std::ranges::all_of(threadId, [](QChar character) {
                  const auto category = character.category();
                  return character.isPrint() && category != QChar::Separator_Line
                      && category != QChar::Separator_Paragraph
                      && category != QChar::Other_Format;
              });
}

QLabel* textLabel(const QString& text, const char* kind = nullptr)
{
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    if (kind)
        result->setProperty("kind", kind);
    return result;
}

QString plainTooltip(const QString& text)
{
    return Qt::convertFromPlainText(text, Qt::WhiteSpaceNormal);
}

QString menuLabel(QString text)
{
    return text.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

QLabel* section(const QString& text, bool attention = false)
{
    auto* result = textLabel(text, attention ? "attentionSection" : "section");
    result->setMinimumHeight(attention ? 26 : 18);
    return result;
}

QString boundedRowText(QString value)
{
    constexpr qsizetype maximumCharacters = 512;
    if (value.size() <= maximumCharacters)
        return value;
    value.truncate(maximumCharacters);
    value.append(QChar(0x2026));
    return value;
}

class ThreadRow final : public QFrame
{
public:
    ThreadRow(QString stableId,
              QString title,
              QString details,
              QString color,
              ThreadActionAvailability actions,
              bool running,
              bool attention,
              bool archived,
              QWidget* parent = nullptr)
        : QFrame(parent)
        , stableId(std::move(stableId))
        , actions(actions)
        , running(running)
        , attention(attention)
        , archived(archived)
    {
        setObjectName(QStringLiteral("threadRow"));
        setProperty("threadId", this->stableId);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(64);
        setProperty("selected", false);

        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(12, 7, 10, 7);
        row->setSpacing(10);
        dot = new QFrame;
        dot->setFixedSize(8, 8);
        row->addWidget(dot, 0, Qt::AlignTop);

        auto* content = new QVBoxLayout;
        content->setContentsMargins(0, 0, 0, 0);
        content->setSpacing(4);
        titleLabel = textLabel({}, "title");
        titleLabel->setStyleSheet(QStringLiteral("font-size:13px;font-weight:500;"));
        titleLabel->setTextFormat(Qt::PlainText);
        titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        content->addWidget(titleLabel);
        detailsLabel = textLabel({}, "meta");
        detailsLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        content->addWidget(detailsLabel);
        row->addLayout(content, 1);
        updatePresentation(std::move(title),
                           std::move(details),
                           std::move(color),
                           this->actions,
                           this->running,
                           this->attention,
                           this->archived);
        updateStyle();
    }

    void updatePresentation(QString title,
                            QString details,
                            QString color,
                            ThreadActionAvailability nextActions,
                            bool nextRunning,
                            bool nextAttention,
                            bool nextArchived)
    {
        actions = nextActions;
        running = nextRunning;
        attention = nextAttention;
        archived = nextArchived;
        if (dotColor != color) {
            dotColor = std::move(color);
        }
        if (fullTitle != title) {
            fullTitle = std::move(title);
            titleLabel->setToolTip(plainTooltip(fullTitle));
            lastAvailableWidth = -1;
        }
        if (fullDetails != details) {
            fullDetails = std::move(details);
            detailsLabel->setToolTip(plainTooltip(fullDetails));
            detailsLabel->setVisible(!fullDetails.isEmpty());
            lastAvailableWidth = -1;
        }
        updateElision(width());
        updateStyle();
    }

    void setSelected(bool value)
    {
        if (selected == value)
            return;
        selected = value;
        updateStyle();
    }

    void setInteractionEnabled(bool enabled)
    {
        if (isEnabled() == enabled)
            return;
        setEnabled(enabled);
        updateStyle();
    }

    [[nodiscard]] const QString& id() const noexcept
    {
        return stableId;
    }

    std::function<void(ThreadRow*)> clicked;
    std::function<void(ThreadRow*, const QPoint&)> contextRequested;

protected:
    void enterEvent(QEnterEvent* event) override
    {
        hovered = true;
        updateStyle();
        QFrame::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        hovered = false;
        updateStyle();
        QFrame::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && clicked)
            clicked(this);
        QFrame::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        if (contextRequested)
            contextRequested(this, event->globalPos());
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
             || event->key() == Qt::Key_Space)
            && clicked)
        {
            clicked(this);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Menu && contextRequested)
        {
            contextRequested(this, mapToGlobal(rect().center()));
            event->accept();
            return;
        }
        QFrame::keyPressEvent(event);
    }

    void focusInEvent(QFocusEvent* event) override
    {
        focused = true;
        updateStyle();
        QFrame::focusInEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        focused = false;
        updateStyle();
        QFrame::focusOutEvent(event);
    }

public:
    [[nodiscard]] const ThreadActionAvailability& availability() const noexcept { return actions; }
    [[nodiscard]] bool isRunning() const noexcept { return running; }
    [[nodiscard]] bool isArchived() const noexcept { return archived; }
    void setContextOpen(bool value)
    {
        if (contextOpen == value)
            return;
        contextOpen = value;
        updateStyle();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        updateElision(event->size().width());
        QFrame::resizeEvent(event);
    }

private:
    void updateElision(int width)
    {
        const int availableWidth = qMax(0, width - 42);
        if (availableWidth != lastAvailableWidth) {
            lastAvailableWidth = availableWidth;
            const QString title = titleLabel->fontMetrics().elidedText(fullTitle, Qt::ElideRight, availableWidth);
            if (titleLabel->text() != title)
                titleLabel->setText(title);
            if (detailsLabel) {
                const QString details = detailsLabel->fontMetrics().elidedText(fullDetails, Qt::ElideRight, availableWidth);
                if (detailsLabel->text() != details)
                    detailsLabel->setText(details);
            }
        }
    }

    void updateStyle()
    {
        if (!isEnabled()) {
            dot->setStyleSheet(QStringLiteral("background:#98a2b3;border-radius:4px;"));
            setStyleSheet(QStringLiteral(
                "QFrame#threadRow{background:#f6f8fb;border:1px solid #d7dee8;border-radius:9px;}"
                "QFrame#threadRow QLabel[kind=\"title\"],"
                "QFrame#threadRow QLabel[kind=\"meta\"]{color:#98a2b3;}"));
            return;
        }
        QString background = archived ? QStringLiteral("#f6f8fb") : QStringLiteral("#ffffff");
        QString border = QStringLiteral("#d7dee8");
        if (attention && !selected) {
            background = QStringLiteral("#fff6df");
            border = QStringLiteral("#e5c77d");
        }
        if (hovered || contextOpen) {
            background = selected ? QStringLiteral("#d9e7ff") : QStringLiteral("#f1f5fb");
            border = selected ? QStringLiteral("#bfd3f9") : QStringLiteral("#b9c4d2");
        }
        if (selected) {
            background = hovered || contextOpen ? QStringLiteral("#d9e7ff") : QStringLiteral("#e5eeff");
            border = QStringLiteral("#bfd3f9");
        }
        if (focused)
            border = QStringLiteral("#2f6feb");
        const QString effectiveDot = attention ? QStringLiteral("#a76812")
                                               : archived ? QStringLiteral("#98a2b3") : dotColor;
        dot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(effectiveDot));
        setStyleSheet(QStringLiteral(
                          "QFrame#threadRow{background:%1;border:1px solid %2;border-radius:9px;}"
                          "QFrame#threadRow QLabel[kind=\"title\"]{color:%3;}"
                          "QFrame#threadRow QLabel[kind=\"meta\"]{color:#667085;}")
                          .arg(background,
                               border,
                               archived ? QStringLiteral("#667085") : QStringLiteral("#1d2633")));
    }

    QFrame* dot = nullptr;
    QLabel* titleLabel = nullptr;
    QLabel* detailsLabel = nullptr;
    QString stableId;
    QString fullTitle;
    QString fullDetails;
    QString dotColor;
    int lastAvailableWidth = -1;
    bool selected = false;
    bool hovered = false;
    bool focused = false;
    bool contextOpen = false;
    ThreadActionAvailability actions;
    bool running = false;
    bool attention = false;
    bool archived = false;
};

QString threadStatusColor(const std::optional<std::string>& status)
{
    if (!status)
        return QStringLiteral("#98a2b3");
    const QString value = QString::fromStdString(*status).toLower();
    if (value.contains(QStringLiteral("fail")) || value.contains(QStringLiteral("error"))
        || value.contains(QStringLiteral("approval")) || value.contains(QStringLiteral("attention")))
        return QStringLiteral("#a76812");
    if (value.contains(QStringLiteral("running")) || value.contains(QStringLiteral("active"))
        || value.contains(QStringLiteral("complete")))
        return QStringLiteral("#23845a");
    return QStringLiteral("#2f6feb");
}

} // namespace

namespace detail {

ThreadUiStatus threadUiStatus(const ai::openai::codex::frontend::client::State& state,
                              const ai::openai::codex::frontend::client::ThreadState& thread,
                              bool awaitingResponse)
{
    bool hasInterruptibleTurn = false;
    for (const auto& turnId : thread.orderedTurns) {
        const auto* turn = state.turn(thread.id, turnId);
        hasInterruptibleTurn = hasInterruptibleTurn || (turn && turn->active && !turn->terminal);
    }

    ThreadUiStatus result;
    const bool archived = thread.archived.value_or(false);
    const bool fullyActionable = thread.fullyLoaded;
    result.running = hasInterruptibleTurn;
    result.awaitingResponse = awaitingResponse;
    result.archived = archived;
    result.actions.fork = fullyActionable;
    result.actions.interrupt = hasInterruptibleTurn;
    result.actions.resumeWithOptions = !result.running && fullyActionable;
    result.actions.remove = !result.running && fullyActionable;
    result.actions.archive = !result.running && fullyActionable
                             && thread.archived.has_value() && !archived;
    result.actions.unarchive = !result.running && archived;
    return result;
}

ThreadActionAvailability
threadActionAvailability(const ai::openai::codex::frontend::client::State& state,
                         const ai::openai::codex::frontend::client::ThreadState& thread)
{
    return threadUiStatus(state, thread).actions;
}

void ThreadOrganization::load(QSettings& settings)
{
    storedFolders.clear();
    threadFolders.clear();

    const QByteArray serialized =
        settings.value(QStringLiteral("sidebar/threadOrganizationV1")).toByteArray();
    const QJsonDocument document = serialized.size() <= maximumOrganizationStorageBytes
                                       ? QJsonDocument::fromJson(serialized)
                                       : QJsonDocument{};
    if (document.isObject()) {
        const QJsonObject root = document.object();
        const QJsonArray folders = root.value(QStringLiteral("folders")).toArray();
        storedFolders.reserve(std::min(maximumOrganizationFolders,
                                       static_cast<std::size_t>(folders.size())));
        for (const auto& value : folders) {
            if (storedFolders.size() >= maximumOrganizationFolders)
                break;
            const QJsonObject object = value.toObject();
            storedFolders.push_back({object.value(QStringLiteral("id")).toString(),
                                     object.value(QStringLiteral("name")).toString(),
                                     object.value(QStringLiteral("parentId")).toString(),
                                     object.value(QStringLiteral("expanded")).toBool(true)});
        }
        const QJsonObject assignments = root.value(QStringLiteral("threadFolders")).toObject();
        for (auto iterator = assignments.constBegin();
             iterator != assignments.constEnd()
             && threadFolders.size() < maximumOrganizationAssignments;
             ++iterator)
            threadFolders.insert(iterator.key(), iterator.value().toString());
    }
    normalize();
    ++currentRevision;
}

bool ThreadOrganization::save(QSettings& settings) const
{
    const QByteArray serialized = serializeOrganization(storedFolders, threadFolders);
    if (serialized.size() > maximumOrganizationStorageBytes)
        return false;
    settings.setValue(QStringLiteral("sidebar/threadOrganizationV1"), serialized);
    return true;
}

const std::vector<ThreadFolder>& ThreadOrganization::folders() const noexcept
{
    return storedFolders;
}

const ThreadFolder* ThreadOrganization::folder(const QString& folderId) const noexcept
{
    const auto iterator = std::ranges::find(storedFolders, folderId, &ThreadFolder::id);
    return iterator == storedFolders.end() ? nullptr : &*iterator;
}

QString ThreadOrganization::folderForThread(const QString& threadId) const
{
    const QString folderId = threadFolders.value(threadId);
    return folder(folderId) ? folderId : QString{};
}

QString ThreadOrganization::folderPath(const QString& folderId) const
{
    QStringList parts;
    QString currentId = folderId;
    for (std::size_t depth = 0; depth < storedFolders.size() && !currentId.isEmpty(); ++depth) {
        const auto* current = folder(currentId);
        if (!current)
            break;
        parts.prepend(current->name);
        currentId = current->parentId;
    }
    return parts.join(QStringLiteral(" › "));
}

quint64 ThreadOrganization::revision() const noexcept
{
    return currentRevision;
}

QString ThreadOrganization::createFolder(const QString& name, const QString& parentId)
{
    const QString normalizedName = name.trimmed();
    std::size_t newDepth = 1;
    for (QString ancestorId = parentId; !ancestorId.isEmpty(); ++newDepth) {
        const auto* ancestor = folder(ancestorId);
        if (!ancestor || newDepth >= maximumOrganizationDepth)
            return {};
        ancestorId = ancestor->parentId;
    }
    if (storedFolders.size() >= maximumOrganizationFolders
        || !validName(normalizedName, parentId))
        return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    storedFolders.push_back({id, normalizedName, parentId, true});
    const qsizetype candidateBytes = serializeOrganization(storedFolders, threadFolders).size();
    if (candidateBytes > maximumOrganizationStorageBytes) {
        storedFolders.pop_back();
        return {};
    }
    currentStorageBytes = candidateBytes;
    ++currentRevision;
    return id;
}

bool ThreadOrganization::renameFolder(const QString& folderId, const QString& name)
{
    auto iterator = std::ranges::find(storedFolders, folderId, &ThreadFolder::id);
    if (iterator == storedFolders.end())
        return false;
    const QString normalizedName = name.trimmed();
    if (!validName(normalizedName, iterator->parentId, folderId))
        return false;
    if (iterator->name == normalizedName)
        return false;
    const QString previousName = iterator->name;
    iterator->name = normalizedName;
    const qsizetype candidateBytes = serializeOrganization(storedFolders, threadFolders).size();
    if (candidateBytes > maximumOrganizationStorageBytes) {
        iterator->name = previousName;
        return false;
    }
    currentStorageBytes = candidateBytes;
    ++currentRevision;
    return true;
}

bool ThreadOrganization::moveFolder(const QString& folderId, const QString& parentId)
{
    auto iterator = std::ranges::find(storedFolders, folderId, &ThreadFolder::id);
    if (iterator == storedFolders.end() || iterator->parentId == parentId
        || !canMoveFolder(folderId, parentId))
        return false;
    const QString previousParentId = iterator->parentId;
    iterator->parentId = parentId;
    const qsizetype candidateBytes = serializeOrganization(storedFolders, threadFolders).size();
    if (candidateBytes > maximumOrganizationStorageBytes) {
        iterator->parentId = previousParentId;
        return false;
    }
    currentStorageBytes = candidateBytes;
    ++currentRevision;
    return true;
}

bool ThreadOrganization::removeFolderAndPromoteContents(const QString& folderId)
{
    const auto iterator = std::ranges::find(storedFolders, folderId, &ThreadFolder::id);
    if (iterator == storedFolders.end())
        return false;
    const std::vector<ThreadFolder> previousFolders = storedFolders;
    const QHash<QString, QString> previousThreadFolders = threadFolders;
    const QString parentId = iterator->parentId;
    for (auto& child : storedFolders) {
        if (child.parentId == folderId) {
            const QString baseName = child.name;
            QString promotedName = baseName;
            int suffix = 2;
            while (!validName(promotedName, parentId, child.id)) {
                const QString suffixText = QStringLiteral(" (%1)").arg(suffix++);
                promotedName = baseName.left(128 - suffixText.size()) + suffixText;
            }
            child.name = promotedName;
            child.parentId = parentId;
        }
    }
    for (auto assignment = threadFolders.begin(); assignment != threadFolders.end();) {
        if (assignment.value() != folderId) {
            ++assignment;
            continue;
        }
        if (parentId.isEmpty())
            assignment = threadFolders.erase(assignment);
        else {
            assignment.value() = parentId;
            ++assignment;
        }
    }
    storedFolders.erase(iterator);
    const qsizetype candidateBytes = serializeOrganization(storedFolders, threadFolders).size();
    if (candidateBytes > maximumOrganizationStorageBytes) {
        storedFolders = previousFolders;
        threadFolders = previousThreadFolders;
        return false;
    }
    currentStorageBytes = candidateBytes;
    ++currentRevision;
    return true;
}

bool ThreadOrganization::moveThread(const QString& threadId, const QString& folderId)
{
    if (!validThreadOrganizationId(threadId)
        || (!folderId.isEmpty() && !folder(folderId)))
        return false;
    const auto previous = threadFolders.constFind(threadId);
    const bool hadPreviousAssignment = previous != threadFolders.constEnd();
    const QString previousFolderId = hadPreviousAssignment ? previous.value() : QString{};
    if (previousFolderId == folderId)
        return false;
    if (!hadPreviousAssignment && !folderId.isEmpty()
        && threadFolders.size() >= maximumOrganizationAssignments)
        return false;

    qsizetype candidateBytes = currentStorageBytes;
    if (hadPreviousAssignment) {
        candidateBytes -= serializedAssignmentMemberBytes(threadId, previousFolderId);
        if (threadFolders.size() > 1)
            --candidateBytes;
    }
    if (!folderId.isEmpty()) {
        candidateBytes += serializedAssignmentMemberBytes(threadId, folderId);
        if ((!hadPreviousAssignment && !threadFolders.isEmpty())
            || (hadPreviousAssignment && threadFolders.size() > 1))
            ++candidateBytes;
    }
    if (candidateBytes > maximumOrganizationStorageBytes)
        return false;

    if (folderId.isEmpty())
        threadFolders.remove(threadId);
    else
        threadFolders.insert(threadId, folderId);
    currentStorageBytes = candidateBytes;
    ++currentRevision;
    return true;
}

bool ThreadOrganization::retainThreadAssignments(const QSet<QString>& threadIds)
{
    const qsizetype removed = threadFolders.removeIf([&threadIds](auto iterator) {
        return !threadIds.contains(iterator.key());
    });
    if (removed == 0)
        return false;
    currentStorageBytes = serializeOrganization(storedFolders, threadFolders).size();
    ++currentRevision;
    return true;
}

bool ThreadOrganization::setFolderExpanded(const QString& folderId, bool expanded)
{
    auto iterator = std::ranges::find(storedFolders, folderId, &ThreadFolder::id);
    if (iterator == storedFolders.end() || iterator->expanded == expanded)
        return false;
    const bool previousExpanded = iterator->expanded;
    iterator->expanded = expanded;
    const qsizetype candidateBytes = serializeOrganization(storedFolders, threadFolders).size();
    if (candidateBytes > maximumOrganizationStorageBytes) {
        iterator->expanded = previousExpanded;
        return false;
    }
    currentStorageBytes = candidateBytes;
    ++currentRevision;
    return true;
}

QSet<QString> ThreadOrganization::movableFolderParents(const QString& folderId) const
{
    QSet<QString> result;
    QHash<QString, const ThreadFolder*> foldersById;
    foldersById.reserve(static_cast<qsizetype>(storedFolders.size()));
    for (const ThreadFolder& value : storedFolders)
        foldersById.insert(value.id, &value);
    const ThreadFolder* moved = foldersById.value(folderId, nullptr);
    if (!moved)
        return result;

    QSet<QString> descendants;
    std::size_t subtreeDepth = 0;
    for (const ThreadFolder& candidate : storedFolders) {
        QString currentId = candidate.id;
        QSet<QString> visited;
        std::size_t distance = 0;
        while (!currentId.isEmpty()) {
            if (currentId == folderId) {
                descendants.insert(candidate.id);
                subtreeDepth = std::max(subtreeDepth, distance);
                break;
            }
            if (visited.contains(currentId) || distance > maximumOrganizationDepth)
                return {};
            visited.insert(currentId);
            const ThreadFolder* current = foldersById.value(currentId, nullptr);
            if (!current)
                return {};
            currentId = current->parentId;
            ++distance;
        }
    }

    QSet<QString> conflictingParents;
    for (const ThreadFolder& sibling : storedFolders) {
        if (sibling.id != folderId
            && sibling.name.compare(moved->name, Qt::CaseInsensitive) == 0)
            conflictingParents.insert(sibling.parentId);
    }
    const auto addDestination = [&](const QString& parentId) {
        if (descendants.contains(parentId) || conflictingParents.contains(parentId))
            return;
        QString currentId = parentId;
        QSet<QString> visited;
        std::size_t parentDepth = 0;
        while (!currentId.isEmpty()) {
            if (visited.contains(currentId) || ++parentDepth > maximumOrganizationDepth)
                return;
            visited.insert(currentId);
            const ThreadFolder* current = foldersById.value(currentId, nullptr);
            if (!current)
                return;
            currentId = current->parentId;
        }
        if (parentDepth + 1U + subtreeDepth <= maximumOrganizationDepth)
            result.insert(parentId);
    };
    addDestination({});
    for (const ThreadFolder& candidate : storedFolders)
        addDestination(candidate.id);
    return result;
}

bool ThreadOrganization::canMoveFolder(const QString& folderId, const QString& parentId) const
{
    return movableFolderParents(folderId).contains(parentId);
}

bool ThreadOrganization::validName(const QString& name,
                                   const QString& parentId,
                                   const QString& excludedFolderId) const
{
    if (name.isEmpty() || name.size() > 128
        || !std::ranges::all_of(name, [](QChar character) {
               const auto category = character.category();
               return character.isPrint() && category != QChar::Separator_Line
                   && category != QChar::Separator_Paragraph
                   && category != QChar::Other_Format;
           })
        || (!parentId.isEmpty() && !folder(parentId)))
        return false;
    return std::ranges::none_of(storedFolders, [&](const ThreadFolder& sibling) {
        return sibling.id != excludedFolderId && sibling.parentId == parentId
               && sibling.name.compare(name, Qt::CaseInsensitive) == 0;
    });
}

bool ThreadOrganization::isDescendantOf(const QString& folderId,
                                        const QString& possibleAncestorId) const
{
    QString currentId = folderId;
    for (std::size_t depth = 0; depth <= storedFolders.size() && !currentId.isEmpty(); ++depth) {
        if (currentId == possibleAncestorId)
            return true;
        const auto* current = folder(currentId);
        if (!current)
            return false;
        currentId = current->parentId;
    }
    return false;
}

void ThreadOrganization::normalize()
{
    QSet<QString> ids;
    std::erase_if(storedFolders, [&ids](ThreadFolder& folder) {
        folder.id = folder.id.trimmed();
        folder.name = folder.name.trimmed();
        folder.parentId = folder.parentId.trimmed();
        const bool printableName = std::ranges::all_of(folder.name, [](QChar character) {
            const auto category = character.category();
            return character.isPrint() && category != QChar::Separator_Line
                && category != QChar::Separator_Paragraph
                && category != QChar::Other_Format;
        });
        if (folder.id.isEmpty() || folder.id.size() > 128 || folder.name.isEmpty()
            || folder.name.size() > 128 || !printableName || folder.parentId.size() > 128
            || ids.contains(folder.id))
            return true;
        ids.insert(folder.id);
        return false;
    });
    for (auto& current : storedFolders) {
        if (!current.parentId.isEmpty()
            && (!folder(current.parentId) || current.parentId == current.id
                || isDescendantOf(current.parentId, current.id)))
            current.parentId.clear();
        QString ancestorId = current.parentId;
        std::size_t depth = 1;
        while (!ancestorId.isEmpty() && depth < maximumOrganizationDepth) {
            const auto* ancestor = folder(ancestorId);
            ancestorId = ancestor ? ancestor->parentId : QString{};
            ++depth;
        }
        if (!ancestorId.isEmpty())
            current.parentId.clear();
    }
    QSet<QString> siblingNames;
    for (auto& current : storedFolders) {
        const QString baseName = current.name;
        QString uniqueName = baseName;
        int suffix = 2;
        QString identity = current.parentId + QChar(u'\0') + uniqueName.toCaseFolded();
        while (siblingNames.contains(identity)) {
            const QString suffixText = QStringLiteral(" (%1)").arg(suffix++);
            uniqueName = baseName.left(128 - suffixText.size()) + suffixText;
            identity = current.parentId + QChar(u'\0') + uniqueName.toCaseFolded();
        }
        current.name = uniqueName;
        siblingNames.insert(identity);
    }
    for (auto iterator = threadFolders.begin(); iterator != threadFolders.end();) {
        const bool printableThreadId = validThreadOrganizationId(iterator.key());
        if (iterator.key().isEmpty() || iterator.key().size() > 1024
            || !printableThreadId || !folder(iterator.value()))
            iterator = threadFolders.erase(iterator);
        else
            ++iterator;
    }

    currentStorageBytes = serializeOrganization(storedFolders, threadFolders).size();
    if (currentStorageBytes <= maximumOrganizationStorageBytes)
        return;

    // Normalization can make duplicate sibling names slightly longer. Keep a
    // malformed-but-bounded settings value from becoming a value that this
    // version cannot save again by pruning only presentation assignments.
    QStringList assignmentIds = threadFolders.keys();
    std::ranges::sort(assignmentIds);
    for (auto iterator = assignmentIds.crbegin();
         iterator != assignmentIds.crend()
         && currentStorageBytes > maximumOrganizationStorageBytes;
         ++iterator) {
        const auto assignment = threadFolders.constFind(*iterator);
        if (assignment == threadFolders.constEnd())
            continue;
        currentStorageBytes -= serializedAssignmentMemberBytes(assignment.key(), assignment.value());
        if (threadFolders.size() > 1)
            --currentStorageBytes;
        threadFolders.erase(assignment);
    }

    // Folder metadata alone is bounded well below the storage budget by the
    // folder/name/depth limits. Recalculate defensively rather than relying on
    // the arithmetic above when loading an externally edited value.
    currentStorageBytes = serializeOrganization(storedFolders, threadFolders).size();
    if (currentStorageBytes > maximumOrganizationStorageBytes) {
        storedFolders.clear();
        threadFolders.clear();
        currentStorageBytes = serializeOrganization(storedFolders, threadFolders).size();
    }
}

} // namespace detail

SidebarWidget::SidebarWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sidebar"));
    setStyleSheet(QStringLiteral("QWidget#sidebar{background:#f8fafc;}"));
    setMinimumWidth(220);
    setMaximumWidth(440);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 14, 10, 17);
    root->setSpacing(0);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(8, 0, 6, 8);
    header->addWidget(section(QStringLiteral("WORK")));
    header->addStretch();
    auto* hide = new QPushButton(QStringLiteral("Hide"));
    hide->setProperty("kind", "subtle");
    hide->setFixedSize(52, 24);
    header->addWidget(hide);
    root->addLayout(header);

    newThread = new QPushButton(QStringLiteral("+  New thread"));
    newThread->setFixedHeight(36);
    newThread->setStyleSheet(QStringLiteral(
        "QPushButton{background:#ffffff;color:#2f6feb;border:1px solid #bfd3f9;border-radius:8px;"
        "text-align:left;padding-left:14px;font-weight:600;}"
        "QPushButton:hover{background:#e5eeff;border-color:#2f6feb;}"
        "QPushButton:disabled{background:#f6f8fb;color:#98a2b3;border-color:#d7dee8;}"));
    root->addWidget(newThread);
    root->addSpacing(8);

    newFolder = new QPushButton(QStringLiteral("+  New folder"));
    newFolder->setObjectName(QStringLiteral("newThreadFolderButton"));
    newFolder->setProperty("kind", "subtle");
    newFolder->setFixedHeight(28);
    newFolder->setStyleSheet(QStringLiteral(
        "QPushButton{text-align:left;padding-left:14px;color:#475467;}"
        "QPushButton:hover{background:#eef3fa;color:#1d2633;}"));
    root->addWidget(newFolder);
    root->addSpacing(10);

    threadTree = new QTreeWidget;
    threadTree->setObjectName(QStringLiteral("threadTree"));
    threadTree->setHeaderHidden(true);
    threadTree->setRootIsDecorated(true);
    threadTree->setIndentation(16);
    threadTree->setSelectionMode(QAbstractItemView::NoSelection);
    threadTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    threadTree->setContextMenuPolicy(Qt::CustomContextMenu);
    threadTree->setStyleSheet(QStringLiteral(
        "QTreeWidget#threadTree{background:transparent;border:0;outline:0;}"
        "QTreeWidget#threadTree::item{min-height:24px;border:0;padding:1px 2px;color:#344054;}"
        "QTreeWidget#threadTree::item:hover{background:#f1f5fb;border-radius:5px;}"
        "QTreeWidget#threadTree::branch{background:transparent;}"));
    auto* waiting = new QTreeWidgetItem(threadTree, QStringList{QStringLiteral("Waiting for synchronized state…")});
    waiting->setData(0, itemKindRole, sectionItemKind);
    waiting->setForeground(0, QColor(QStringLiteral("#667085")));
    root->addWidget(threadTree, 1);

    auto* divider = new QFrame;
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background:#d7dee8;"));
    root->addWidget(divider);
    root->addSpacing(26);

    auto* serverRow = new QHBoxLayout;
    serverRow->setContentsMargins(8, 0, 0, 0);
    serverRow->setSpacing(10);
    serverDot = new QFrame;
    serverDot->setFixedSize(8, 8);
    serverDot->setStyleSheet(QStringLiteral("background:#23845a;border-radius:4px;"));
    serverRow->addWidget(serverDot, 0, Qt::AlignTop);
    auto* serverCopy = new QVBoxLayout;
    serverCopy->setSpacing(3);
    serverTitle = textLabel(QStringLiteral("Not connected"), "meta");
    serverTitle->setStyleSheet(QStringLiteral("color:#1d2633;font-size:11px;font-weight:500;"));
    serverCopy->addWidget(serverTitle);
    serverDetail = textLabel(QStringLiteral("Unix frontend"), "meta");
    serverCopy->addWidget(serverDetail);
    serverRow->addLayout(serverCopy, 1);
    root->addLayout(serverRow);

    connect(hide, &QPushButton::clicked, this, &SidebarWidget::hideRequested);
    connect(newThread, &QPushButton::clicked, this, &SidebarWidget::newThreadRequested);
    connect(newFolder, &QPushButton::clicked, this, [this] { createFolder(); });
    connect(threadTree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& position) {
                auto* item = threadTree->itemAt(position);
                if (item && item->data(0, itemKindRole).toString() == folderItemKind)
                    showFolderContextMenu(item, threadTree->viewport()->mapToGlobal(position));
            });
    const auto storeFolderExpansion = [this](QTreeWidgetItem* item, bool expanded) {
        if (!organizationWritable || rebuildingTree || !item
            || item->data(0, itemKindRole).toString() != folderItemKind)
            return;
        const QString folderId = item->data(0, stableIdRole).toString();
        if (!organization.setFolderExpanded(folderId, expanded))
            return;
        rebuildingTree = true;
        QTreeWidgetItemIterator iterator(threadTree);
        while (*iterator) {
            auto* candidate = *iterator;
            ++iterator;
            if (candidate != item && candidate->data(0, stableIdRole).toString() == folderId)
                candidate->setExpanded(expanded);
        }
        rebuildingTree = false;
        persistOrganization();
        renderedOrganizationRevision = organization.revision();
    };
    connect(threadTree, &QTreeWidget::itemExpanded, this,
            [storeFolderExpansion](QTreeWidgetItem* item) { storeFolderExpansion(item, true); });
    connect(threadTree, &QTreeWidget::itemCollapsed, this,
            [storeFolderExpansion](QTreeWidgetItem* item) { storeFolderExpansion(item, false); });

    QSettings settings;
    QDir().mkpath(QFileInfo(settings.fileName()).absolutePath());
    organizationLock = new QLockFile(settings.fileName()
                                     + QStringLiteral(".thread-organization.lock"));
    organizationWritable = organizationLock->tryLock(0);
    organization.load(settings);
    newFolder->setEnabled(organizationWritable);
    if (!organizationWritable)
        newFolder->setToolTip(QStringLiteral(
            "Thread folders are read-only while another CodexUI window owns the organization lock."));
}

SidebarWidget::~SidebarWidget()
{
    delete organizationLock;
}

void SidebarWidget::tryAcquireOrganizationLock()
{
    if (organizationWritable || !organizationLock || !organizationLock->tryLock(0))
        return;

    // Another window may have changed the file while this instance was
    // read-only. Reload before enabling writes so taking over the lock can
    // never overwrite that newer organization with stale in-memory data.
    QSettings settings;
    organization.load(settings);
    organizationWritable = true;
    organizationPersistenceFailureReported = false;
    newFolder->setEnabled(true);
    newFolder->setToolTip({});
    threadsRendered = false;
}

void SidebarWidget::setThreads(const ai::openai::codex::frontend::client::State& state,
                               const QString& selectedThreadId,
                               bool allThreadDiscoveryComplete)
{
    tryAcquireOrganizationLock();
    std::vector<ThreadPresentation> presentations;
    const auto threads = state.threads();
    QSet<QString> threadsAwaitingResponse;
    if (state.hasPendingRequestProjection()) {
        for (const auto& request : state.pendingRequests()) {
            if (request.threadId)
                threadsAwaitingResponse.insert(QString::fromStdString(request.threadId->value));
        }
    }
    if (organizationWritable && allThreadDiscoveryComplete
        && state.freshness()
               == ai::openai::codex::frontend::client::StateFreshness::Current
        && state.hasThreadProjection()) {
        QSet<QString> retainedThreadIds;
        retainedThreadIds.reserve(static_cast<qsizetype>(threads.size()));
        for (const auto& thread : threads)
            retainedThreadIds.insert(QString::fromStdString(thread.id.value));
        if (organization.retainThreadAssignments(retainedThreadIds))
            persistOrganization();
    }
    presentations.reserve(threads.size());
    for (const auto& thread : threads) {
        const QString id = QString::fromStdString(thread.id.value);
        const QString title = boundedRowText(
            thread.title && !thread.title->empty() ? QString::fromStdString(*thread.title) : id);
        const detail::ThreadUiStatus uiStatus = detail::threadUiStatus(
            state, thread, threadsAwaitingResponse.contains(id));
        QStringList secondaryParts;
        if (uiStatus.archived)
            secondaryParts.append(QStringLiteral("Archived"));
        else if (!thread.fullyLoaded)
            secondaryParts.append(QStringLiteral("Loading"));
        else if (uiStatus.running)
            secondaryParts.append(QStringLiteral("Running"));
        else if (!ai::openai::codex::frontend::client::threadIsIdle(thread))
            secondaryParts.append(QStringLiteral("Ready to resume"));
        else
            secondaryParts.append(QStringLiteral("Idle"));
        secondaryParts.append(thread.orderedTurns.empty()
                                  ? QStringLiteral("Ready for first turn")
                                  : QStringLiteral("%1 turn%2")
                                        .arg(thread.orderedTurns.size())
                                        .arg(thread.orderedTurns.size() == 1
                                                 ? QString{}
                                                 : QStringLiteral("s")));
        if (thread.ephemeral.value_or(false))
            secondaryParts.append(QStringLiteral("Temporary"));
        const QString secondary = secondaryParts.join(QStringLiteral(" · "));
        presentations.push_back({id,
                                 title,
                                 boundedRowText(secondary),
                                 threadStatusColor(thread.status),
                                 uiStatus.actions,
                                 uiStatus.running,
                                 uiStatus.awaitingResponse,
                                 uiStatus.archived});
    }
    std::stable_partition(presentations.begin(), presentations.end(),
                          [](const ThreadPresentation& presentation) {
                              return !presentation.archived;
                          });

    if (threadsRendered && presentations == renderedThreads && selectedThreadId == renderedSelection
        && renderedOrganizationRevision == organization.revision())
        return;

    bool sameOrder = threadsRendered && presentations.size() == renderedThreads.size();
    for (std::size_t index = 0; sameOrder && index < presentations.size(); ++index) {
        sameOrder = presentations[index].id == renderedThreads[index].id
                    && presentations[index].archived == renderedThreads[index].archived;
    }
    sameOrder = sameOrder && renderedOrganizationRevision == organization.revision();
    if (sameOrder) {
        renderedThreads = presentations;
        renderedSelection = selectedThreadId;
        QHash<QString, const ThreadPresentation*> presentationsById;
        presentationsById.reserve(static_cast<qsizetype>(renderedThreads.size()));
        for (const ThreadPresentation& presentation : renderedThreads)
            presentationsById.insert(presentation.id, &presentation);
        std::size_t updatedRows = 0;
        QTreeWidgetItemIterator iterator(threadTree);
        while (*iterator) {
            auto* item = *iterator;
            auto* row = dynamic_cast<ThreadRow*>(threadTree->itemWidget(item, 0));
            ++iterator;
            if (!row)
                continue;
            const auto presentation = presentationsById.constFind(row->id());
            if (presentation == presentationsById.cend()) {
                sameOrder = false;
                break;
            }
            const ThreadPresentation& value = **presentation;
            row->updatePresentation(value.title,
                                    value.details,
                                    value.color,
                                    value.actions,
                                    value.running,
                                    value.attention,
                                    value.archived);
            row->setSelected(value.id == selectedThreadId);
            row->setInteractionEnabled(threadInteractionEnabled);
            ++updatedRows;
        }
        sameOrder = sameOrder && updatedRows == renderedThreads.size();
        if (sameOrder)
            return;
    }
    threadsRendered = true;
    renderedThreads = std::move(presentations);
    renderedSelection = selectedThreadId;
    renderThreadTree();
}

void SidebarWidget::renderThreadTree()
{
    rebuildingTree = true;
    threadTree->clear();
    renderedOrganizationRevision = organization.revision();

    const auto makeSection = [this](const QString& title) {
        auto* item = new QTreeWidgetItem(threadTree, QStringList{title});
        item->setData(0, itemKindRole, sectionItemKind);
        item->setFirstColumnSpanned(true);
        item->setFlags(Qt::ItemIsEnabled);
        QFont font = item->font(0);
        font.setBold(true);
        font.setPointSizeF(qMax(8.0, font.pointSizeF() - 1.0));
        item->setFont(0, font);
        item->setForeground(0, QColor(QStringLiteral("#475467")));
        item->setExpanded(true);
        return item;
    };

    const auto addGroup = [this, &makeSection](const QString& heading, bool archived) {
        const bool hasRows = std::ranges::any_of(renderedThreads, [archived](const auto& presentation) {
            return presentation.archived == archived;
        });

        QSet<QString> neededFolders;
        QSet<QString> foldersOccupiedByCurrentThreads;
        const auto addFolderAndAncestors = [this](QSet<QString>& target, QString folderId) {
            for (std::size_t depth = 0;
                 depth < organization.folders().size() && !folderId.isEmpty();
                 ++depth) {
                if (target.contains(folderId))
                    break;
                target.insert(folderId);
                const auto* folder = organization.folder(folderId);
                folderId = folder ? folder->parentId : QString{};
            }
        };
        for (const auto& presentation : renderedThreads) {
            const QString folderId = organization.folderForThread(presentation.id);
            addFolderAndAncestors(foldersOccupiedByCurrentThreads, folderId);
            if (presentation.archived == archived)
                addFolderAndAncestors(neededFolders, folderId);
        }
        if (!archived) {
            for (const auto& folder : organization.folders()) {
                if (!foldersOccupiedByCurrentThreads.contains(folder.id))
                    addFolderAndAncestors(neededFolders, folder.id);
            }
        }
        if (!hasRows && neededFolders.isEmpty())
            return;

        auto* sectionItem = makeSection(heading);
        QHash<QString, QTreeWidgetItem*> folderItems;
        std::function<QTreeWidgetItem*(const QString&)> ensureFolder;
        ensureFolder = [this, sectionItem, &folderItems, &neededFolders, &ensureFolder](const QString& folderId) {
            if (folderId.isEmpty())
                return sectionItem;
            if (auto* existing = folderItems.value(folderId, nullptr))
                return existing;
            const auto* folder = organization.folder(folderId);
            if (!folder)
                return sectionItem;
            QTreeWidgetItem* parent = sectionItem;
            if (neededFolders.contains(folder->parentId))
                parent = ensureFolder(folder->parentId);
            auto* item = new QTreeWidgetItem(parent, QStringList{folder->name});
            item->setData(0, itemKindRole, folderItemKind);
            item->setData(0, stableIdRole, folder->id);
            item->setFlags(Qt::ItemIsEnabled);
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            item->setToolTip(0, plainTooltip(organization.folderPath(folder->id)));
            item->setExpanded(folder->expanded);
            folderItems.insert(folderId, item);
            return item;
        };
        for (const auto& folder : organization.folders()) {
            if (neededFolders.contains(folder.id))
                ensureFolder(folder.id);
        }

        for (const auto& presentation : renderedThreads) {
            if (presentation.archived != archived)
                continue;
            QTreeWidgetItem* parent = sectionItem;
            const QString folderId = organization.folderForThread(presentation.id);
            if (!folderId.isEmpty() && neededFolders.contains(folderId))
                parent = ensureFolder(folderId);
            auto* item = new QTreeWidgetItem(parent);
            item->setFlags(Qt::ItemIsEnabled);
            item->setSizeHint(0, QSize(0, 64));
            auto* row = new ThreadRow(presentation.id,
                                      presentation.title,
                                      presentation.details,
                                      presentation.color,
                                      presentation.actions,
                                      presentation.running,
                                      presentation.attention,
                                      presentation.archived,
                                      threadTree);
            row->setSelected(presentation.id == renderedSelection);
            row->setInteractionEnabled(threadInteractionEnabled);
            row->clicked = [this](ThreadRow* selected) { emit threadSelected(selected->id()); };
            row->contextRequested = [this](ThreadRow* source, const QPoint& globalPosition) {
                source->setContextOpen(true);
                auto* menu = new QMenu(this);
                const QString stableId = source->id();
                const QPointer<ThreadRow> guardedSource(source);
                const ThreadActionAvailability available = source->availability();
                const bool running = source->isRunning();
                const bool archived = source->isArchived();
                const auto add = [menu, this, stableId](const QString& text,
                                                        ThreadAction action,
                                                        bool enabled = true) {
                    QAction* actionItem = menu->addAction(text);
                    actionItem->setEnabled(enabled);
                    connect(actionItem, &QAction::triggered, this, [this, stableId, action] {
                        emit threadActionRequested(stableId, action);
                    });
                    return actionItem;
                };
                add(QStringLiteral("Open"), ThreadAction::Open, available.open);
                add(QStringLiteral("Rename…"), ThreadAction::Rename, available.rename);
                add(QStringLiteral("Fork…"), ThreadAction::Fork, available.fork);

                QMenu* moveMenu = menu->addMenu(QStringLiteral("Move to folder"));
                moveMenu->setEnabled(organizationWritable);
                const QString currentFolder = organization.folderForThread(stableId);
                QAction* rootAction = moveMenu->addAction(QStringLiteral("Threads root"));
                rootAction->setCheckable(true);
                rootAction->setChecked(currentFolder.isEmpty());
                connect(rootAction, &QAction::triggered, this,
                        [this, stableId] { moveThread(stableId, {}); });
                if (!organization.folders().empty())
                    moveMenu->addSeparator();
                for (const auto& folder : organization.folders()) {
                    QAction* folderAction = moveMenu->addAction(menuLabel(organization.folderPath(folder.id)));
                    folderAction->setCheckable(true);
                    folderAction->setChecked(currentFolder == folder.id);
                    connect(folderAction, &QAction::triggered, this,
                            [this, stableId, folderId = folder.id] {
                                moveThread(stableId, folderId);
                            });
                }

                menu->addSeparator();
                if (running)
                    add(QStringLiteral("Interrupt"), ThreadAction::Interrupt, available.interrupt);
                else
                    add(QStringLiteral("Resume with options…\tadvanced"),
                        ThreadAction::ResumeWithOptions,
                        available.resumeWithOptions);
                menu->addSeparator();
                if (archived)
                    add(QStringLiteral("Unarchive"), ThreadAction::Unarchive, available.unarchive);
                else
                    add(running ? QStringLiteral("Archive\trunning") : QStringLiteral("Archive"),
                        ThreadAction::Archive,
                        available.archive);
                QAction* remove = add(running ? QStringLiteral("Delete…\trunning")
                                              : QStringLiteral("Delete…"),
                                      ThreadAction::Delete,
                                      available.remove);
                QPixmap destructiveIcon = style()->standardIcon(QStyle::SP_TrashIcon).pixmap(16, 16);
                if (!destructiveIcon.isNull()) {
                    QPainter painter(&destructiveIcon);
                    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                    painter.fillRect(destructiveIcon.rect(), QColor(QStringLiteral("#b83a3a")));
                }
                remove->setIcon(QIcon(destructiveIcon));
                menu->addSeparator();
                QMenu* moreMenu = menu->addMenu(QStringLiteral("More"));
                QAction* copyId = moreMenu->addAction(QStringLiteral("Copy thread ID"));
                connect(copyId, &QAction::triggered, this, [this, stableId] {
                    emit threadActionRequested(stableId, ThreadAction::CopyId);
                });
                connect(menu, &QMenu::aboutToHide, this, [guardedSource, menu] {
                    if (guardedSource)
                        guardedSource->setContextOpen(false);
                    menu->deleteLater();
                });
                menu->popup(globalPosition);
            };
            threadTree->setItemWidget(item, 0, row);
        }
        sectionItem->setExpanded(true);
    };
    addGroup(QStringLiteral("ACTIVE"), false);
    addGroup(QStringLiteral("ARCHIVED"), true);
    if (threadTree->topLevelItemCount() == 0) {
        auto* active = makeSection(QStringLiteral("ACTIVE"));
        auto* empty = new QTreeWidgetItem(active, QStringList{QStringLiteral("No synchronized threads")});
        empty->setFlags(Qt::ItemIsEnabled);
        empty->setForeground(0, QColor(QStringLiteral("#667085")));
    }
    rebuildingTree = false;
}

void SidebarWidget::persistOrganization()
{
    if (!organizationWritable)
        return;
    QSettings settings;
    const bool encoded = organization.save(settings);
    if (encoded)
        settings.sync();
    if (encoded && settings.status() == QSettings::NoError)
        return;
    qWarning("CodexUI could not persist thread-folder organization");
    organizationWritable = false;
    newFolder->setEnabled(false);
    newFolder->setToolTip(QStringLiteral(
        "Thread-folder changes are read-only because local settings could not be saved."));
    if (!organizationPersistenceFailureReported) {
        organizationPersistenceFailureReported = true;
        auto* message = new QMessageBox(
            QMessageBox::Warning,
            QStringLiteral("Thread folders not saved"),
            QStringLiteral(
                "CodexUI could not save the local thread-folder organization. The current in-memory arrangement may be lost when this window closes."),
            QMessageBox::Ok,
            this);
        message->setTextFormat(Qt::PlainText);
        message->setAttribute(Qt::WA_DeleteOnClose);
        message->open();
    }
}

void SidebarWidget::createFolder(const QString& parentFolderId)
{
    if (!organizationWritable)
        return;
    auto* dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("threadFolderNameDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(parentFolderId.isEmpty() ? QStringLiteral("New folder")
                                                    : QStringLiteral("New subfolder"));
    dialog->setLabelText(QStringLiteral("Folder name"));
    dialog->setInputMode(QInputDialog::TextInput);
    connect(dialog, &QInputDialog::textValueSelected, this,
            [this, parentFolderId](const QString& name) {
                if (organization.createFolder(name, parentFolderId).isEmpty()) {
                    auto* message = new QMessageBox(QMessageBox::Warning,
                                                    QStringLiteral("Folder not created"),
                                                    QStringLiteral("Use a non-empty name that is unique in this folder."),
                                                    QMessageBox::Ok,
                                                    this);
                    message->setAttribute(Qt::WA_DeleteOnClose);
                    message->open();
                    return;
                }
                persistOrganization();
                renderThreadTree();
            });
    dialog->open();
}

void SidebarWidget::renameFolder(const QString& folderId)
{
    if (!organizationWritable)
        return;
    const auto* folder = organization.folder(folderId);
    if (!folder)
        return;
    auto* dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("threadFolderNameDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Rename folder"));
    dialog->setLabelText(QStringLiteral("Folder name"));
    dialog->setInputMode(QInputDialog::TextInput);
    dialog->setTextValue(folder->name);
    connect(dialog, &QInputDialog::textValueSelected, this,
            [this, folderId](const QString& name) {
                const auto* current = organization.folder(folderId);
                if (!current || current->name == name.trimmed())
                    return;
                if (!organization.renameFolder(folderId, name)) {
                    auto* message = new QMessageBox(QMessageBox::Warning,
                                                    QStringLiteral("Folder not renamed"),
                                                    QStringLiteral("Use a non-empty name that is unique in this folder."),
                                                    QMessageBox::Ok,
                                                    this);
                    message->setAttribute(Qt::WA_DeleteOnClose);
                    message->open();
                    return;
                }
                persistOrganization();
                renderThreadTree();
            });
    dialog->open();
}

void SidebarWidget::moveFolder(const QString& folderId, const QString& parentFolderId)
{
    if (!organizationWritable)
        return;
    if (!organization.moveFolder(folderId, parentFolderId))
        return;
    persistOrganization();
    renderThreadTree();
}

void SidebarWidget::deleteFolder(const QString& folderId)
{
    if (!organizationWritable)
        return;
    const auto* folder = organization.folder(folderId);
    if (!folder)
        return;
    const QString destination = folder->parentId.isEmpty()
                                    ? QStringLiteral("the threads root")
                                    : organization.folderPath(folder->parentId);
    auto* message = new QMessageBox(QMessageBox::Warning,
                                    QStringLiteral("Delete folder?"),
                                    QStringLiteral("Delete \"%1\"? Its threads and subfolders will move to %2. No threads will be deleted.")
                                        .arg(folder->name, destination),
                                    QMessageBox::Yes | QMessageBox::Cancel,
                                    this);
    message->setObjectName(QStringLiteral("deleteThreadFolderDialog"));
    message->setTextFormat(Qt::PlainText);
    message->setAttribute(Qt::WA_DeleteOnClose);
    message->setDefaultButton(QMessageBox::Cancel);
    connect(message, &QMessageBox::finished, this, [this, folderId](int result) {
        if (result != QMessageBox::Yes || !organization.removeFolderAndPromoteContents(folderId))
            return;
        persistOrganization();
        renderThreadTree();
    });
    message->open();
}

void SidebarWidget::moveThread(const QString& threadId, const QString& folderId)
{
    if (!organizationWritable)
        return;
    if (!organization.moveThread(threadId, folderId))
        return;
    persistOrganization();
    renderThreadTree();
}

void SidebarWidget::showFolderContextMenu(QTreeWidgetItem* item, const QPoint& globalPosition)
{
    if (!organizationWritable)
        return;
    const QString folderId = item ? item->data(0, stableIdRole).toString() : QString{};
    const auto* folder = organization.folder(folderId);
    if (!folder)
        return;
    auto* menu = new QMenu(this);
    QAction* create = menu->addAction(QStringLiteral("New subfolder…"));
    connect(create, &QAction::triggered, this, [this, folderId] { createFolder(folderId); });
    QAction* rename = menu->addAction(QStringLiteral("Rename…"));
    connect(rename, &QAction::triggered, this, [this, folderId] { renameFolder(folderId); });

    QMenu* moveMenu = menu->addMenu(QStringLiteral("Move folder to"));
    const QSet<QString> movableParents = organization.movableFolderParents(folderId);
    QAction* root = moveMenu->addAction(QStringLiteral("Threads root"));
    root->setCheckable(true);
    root->setChecked(folder->parentId.isEmpty());
    root->setEnabled(folder->parentId.isEmpty() || movableParents.contains(QString{}));
    connect(root, &QAction::triggered, this, [this, folderId] { moveFolder(folderId, {}); });
    if (!organization.folders().empty())
        moveMenu->addSeparator();
    for (const auto& candidate : organization.folders()) {
        if (candidate.id == folderId)
            continue;
        QAction* target = moveMenu->addAction(menuLabel(organization.folderPath(candidate.id)));
        target->setCheckable(true);
        target->setChecked(folder->parentId == candidate.id);
        target->setEnabled(folder->parentId == candidate.id
                           || movableParents.contains(candidate.id));
        connect(target, &QAction::triggered, this,
                [this, folderId, parentId = candidate.id] { moveFolder(folderId, parentId); });
    }

    menu->addSeparator();
    QAction* remove = menu->addAction(style()->standardIcon(QStyle::SP_TrashIcon),
                                      QStringLiteral("Delete folder…"));
    connect(remove, &QAction::triggered, this, [this, folderId] { deleteFolder(folderId); });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(globalPosition);
}

void SidebarWidget::setConnectionStatus(const QString& title, const QString& connectionDetail, const QString& color)
{
    serverTitle->setText(title);
    serverDetail->setText(connectionDetail);
    serverDot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(color));
}

void SidebarWidget::setNewThreadEnabled(bool enabled)
{
    newThread->setEnabled(enabled);
}

void SidebarWidget::setThreadInteractionEnabled(bool enabled)
{
    if (threadInteractionEnabled == enabled)
        return;
    threadInteractionEnabled = enabled;
    QTreeWidgetItemIterator iterator(threadTree);
    while (*iterator) {
        auto* item = *iterator;
        ++iterator;
        if (auto* row = dynamic_cast<ThreadRow*>(threadTree->itemWidget(item, 0)))
            row->setInteractionEnabled(enabled);
    }
}

} // namespace codexui
