// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_UPCOMINGTURNDOCK_H
#define CODEXUI_UI_UPCOMINGTURNDOCK_H

#include <ai/openai/codex/frontend/client/StateTypes.h>
#include <ai/openai/codex/typed/Models.h>
#include <ai/openai/codex/typed/Turns.h>

#include <QWidget>

#include <array>
#include <optional>
#include <utility>
#include <vector>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;

namespace codexui {

// A non-authoritative, write-only description of the settings that the user
// changed for the upcoming turn. Untouched fields stay omitted so callers can
// pass the draft to the typed AISuite turn/start surface without replacing
// canonical thread settings with UI defaults.
struct UpcomingTurnDraft
{
    QString threadIdentity;
    std::array<QString, 10> presentationKeys{};
    ai::openai::codex::typed::OptionalNullable<ai::openai::codex::typed::ModelId> model;
    ai::openai::codex::typed::OptionalNullable<ai::openai::codex::typed::ReasoningEffort> effort;
    ai::openai::codex::typed::OptionalNullable<ai::openai::codex::typed::Personality> personality;
    ai::openai::codex::typed::OptionalNullable<ai::openai::codex::typed::SandboxPolicy> sandboxPolicy;
    ai::openai::codex::typed::OptionalNullable<ai::openai::codex::typed::AskForApproval> approvalPolicy;
    ai::openai::codex::typed::OptionalNullable<ai::openai::codex::typed::ApprovalsReviewer> approvalsReviewer;
    ai::openai::codex::typed::OptionalNullable<std::string> cwd;
    ai::openai::codex::typed::OptionalNullable<std::string> serviceTier;
    ai::openai::codex::typed::OptionalNullable<ai::openai::codex::typed::ReasoningSummary> summary;
    ai::openai::codex::typed::OptionalNullable<ai::openai::codex::typed::CollaborationMode> collaborationMode;

    [[nodiscard]] bool empty() const noexcept;
};

class UpcomingTurnDock final : public QWidget
{
    Q_OBJECT

public:
    explicit UpcomingTurnDock(QWidget* parent = nullptr);

    // Rebase untouched controls from immutable AISuite State. User changes are
    // kept while the same thread receives unrelated streaming state updates.
    void setCanonicalConfiguration(
        const std::optional<ai::openai::codex::frontend::client::ExecutionConfiguration>& configuration,
        const QString& stableThreadIdentity,
        bool useCodexDefaults = false);
    void setModelCatalog(const std::vector<ai::openai::codex::typed::Model>& catalog);
    [[nodiscard]] UpcomingTurnDraft draft() const;
    [[nodiscard]] bool hasSettingsChanges() const noexcept;
    void clearTouchedSettings();
    void acknowledgeSubmittedSettings(const UpcomingTurnDraft& submitted);

    [[nodiscard]] QString prompt() const;
    void clearPrompt();
    void focusPrompt();
    void setActionState(bool sendAllowed,
                        bool stopAllowed,
                        bool editorAllowed,
                        bool stopVisible);
    void setStatus(const QString& text, bool error = false);
    [[nodiscard]] int baseHeight() const noexcept;

signals:
    void sendRequested(const QString& prompt);
    void stopRequested();
    void settingsChanged();
    void dockHeightChanged(int height);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Field : std::size_t {
        Model,
        Effort,
        Personality,
        Sandbox,
        Approval,
        Reviewer,
        Cwd,
        ServiceTier,
        Summary,
        Collaboration,
        Count
    };

    void refreshControls(bool resetAll);
    void refreshModelControl();
    void refreshModelDependentControls(bool modelChangedByUser);
    [[nodiscard]] const ai::openai::codex::typed::Model* defaultModelDefinition() const;
    [[nodiscard]] const ai::openai::codex::typed::Model* selectedModelDefinition() const;
    void resolveSubmittedSettings(const UpcomingTurnDraft& submitted);
    void updatePromptHeight();
    void updateSendEnabled();
    void updateChangedPresentation();
    void markComboChange(Field field, QComboBox* combo);
    void markTextChange(Field field, QLineEdit* edit);
    [[nodiscard]] QString currentFieldKey(Field field) const;
    [[nodiscard]] bool touched(Field field) const noexcept;
    void setTouched(Field field, bool value);

    std::optional<ai::openai::codex::frontend::client::ExecutionConfiguration> canonicalConfiguration;
    QString threadIdentity;
    std::array<bool, static_cast<std::size_t>(Field::Count)> touchedFields{};
    std::array<QString, static_cast<std::size_t>(Field::Count)> canonicalKeys{};
    std::array<QWidget*, static_cast<std::size_t>(Field::Count)> fieldSurfaces{};
    std::vector<ai::openai::codex::typed::Model> modelCatalog;
    bool codexDefaultsContext = false;

    QFrame* settingsSurface = nullptr;
    QComboBox* model = nullptr;
    QComboBox* effort = nullptr;
    QComboBox* personality = nullptr;
    QComboBox* sandbox = nullptr;
    QComboBox* approval = nullptr;
    QLineEdit* cwd = nullptr;
    QPushButton* more = nullptr;
    QMenu* moreMenu = nullptr;
    QComboBox* reviewer = nullptr;
    QComboBox* serviceTier = nullptr;
    QComboBox* summary = nullptr;
    QComboBox* collaboration = nullptr;
    QFrame* composerSurface = nullptr;
    QPlainTextEdit* editor = nullptr;
    QLabel* settingsHint = nullptr;
    QLabel* status = nullptr;
    QPushButton* send = nullptr;
    QPushButton* stop = nullptr;
    bool sendContextAllowed = false;
    bool controlsContextAllowed = false;
    bool showingStopAction = false;
    int currentEditorHeight = 0;
    int compactBaseHeight = 0;
};

// Keeps the conversation viewport at a fixed geometry and anchors the dock to
// the bottom edge. When the composer grows, only the dock's top edge moves and
// the additional area overlays the conversation.
class AnchoredTurnSurface final : public QWidget
{
    Q_OBJECT

public:
    explicit AnchoredTurnSurface(QWidget* parent = nullptr);
    void setConversationWidget(QWidget* widget);
    void setUpcomingTurnDock(UpcomingTurnDock* widget);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void relayout();

    QWidget* conversation = nullptr;
    UpcomingTurnDock* dock = nullptr;
};

} // namespace codexui

#endif // CODEXUI_UI_UPCOMINGTURNDOCK_H
