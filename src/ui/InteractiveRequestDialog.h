// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_INTERACTIVEREQUESTDIALOG_H
#define CODEXUI_UI_INTERACTIVEREQUESTDIALOG_H

#include <ai/openai/codex/frontend/Messages.h>
#include <ai/openai/codex/frontend/client/StateTypes.h>
#include <ai/openai/codex/typed/ServerRequests.h>

#include <QDialog>
#include <QString>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QVBoxLayout;
class QWidget;

namespace ai::openai::codex::frontend::client {
class State;
}

namespace codexui {

struct InteractiveRequestSource {
    ai::openai::codex::frontend::client::PendingRequestState request;
    std::optional<ai::openai::codex::frontend::client::ItemSemanticView> linkedItem;

    bool operator==(const InteractiveRequestSource&) const = default;
};

enum class InteractiveRequestResponseSafety { Disabled, NegativeOnly, Complete };

namespace detail {

[[nodiscard]] std::optional<InteractiveRequestSource>
interactiveRequestSource(const ai::openai::codex::frontend::client::State& state,
                         const ai::openai::codex::frontend::client::PendingRequestId& requestId);
[[nodiscard]] InteractiveRequestResponseSafety
interactiveRequestResponseSafety(const InteractiveRequestSource& source);

} // namespace detail

struct InteractiveRequestResponse {
    using Value = std::variant<ai::openai::codex::typed::ApprovalDecision,
                               ai::openai::codex::typed::ApplyPatchApprovalResponse,
                               ai::openai::codex::typed::ExecCommandApprovalResponse,
                               std::vector<ai::openai::codex::typed::UserInputAnswer>>;

    ai::openai::codex::frontend::client::PendingRequestId requestId;
    ai::openai::codex::frontend::PendingRequestKind kind;
    InteractiveRequestSource source;
    Value value;
};

namespace detail {

[[nodiscard]] bool interactiveResponseIsNegative(const InteractiveRequestResponse& response);

} // namespace detail

class InteractiveRequestDialog final : public QDialog
{
public:
    using ResponseHandler = std::function<void(InteractiveRequestResponse)>;
    using StateProvider = std::function<const ai::openai::codex::frontend::client::State&()>;

    InteractiveRequestDialog(StateProvider stateProvider, ResponseHandler responseHandler, QWidget* parent = nullptr);

    void synchronize(const ai::openai::codex::frontend::client::State& state);
    void present();
    void setSubmitting(const std::string& requestId, const QString& status);
    void responseAccepted(const std::string& requestId);
    void responseFailed(const std::string& requestId, const QString& error);

private:
    friend struct InteractiveRequestDialogTestAccess;

    struct QuestionDraft {
        std::set<std::string> selectedOptions;
        QString freeText;
    };
    struct RequestDraft {
        int approvalIndex = 0;
        std::map<std::string, QuestionDraft> questions;
    };
    struct QuestionEditor {
        std::string id;
        std::vector<std::pair<std::string, QCheckBox*>> options;
        QLineEdit* freeText = nullptr;
        bool secret = false;
    };

    void saveCurrentDraft();
    void clearSecretEditors();
    void rebuild(const ai::openai::codex::frontend::client::State& state);
    void showNext();
    void submitCurrent();
    void setStatus(const QString& text, bool error = false);
    void updateSubmitEnabled();

    StateProvider stateProvider;
    ResponseHandler responseHandler;
    QVBoxLayout* root = nullptr;
    QWidget* body = nullptr;
    QLabel* queueLabel = nullptr;
    QLabel* statusLabel = nullptr;
    QPushButton* nextButton = nullptr;
    QPushButton* submitButton = nullptr;
    std::vector<QRadioButton*> approvalChoices;
    std::vector<QuestionEditor> questionEditors;
    std::vector<std::string> orderedRequestIds;
    std::map<std::string, RequestDraft> drafts;
    std::set<std::string> submittedRequestIds;
    std::optional<InteractiveRequestSource> presentedSource;
    std::string currentRequestId;
    std::string submittingRequestId;
    std::size_t previousCount = 0;
    InteractiveRequestResponseSafety currentResponseSafety = InteractiveRequestResponseSafety::Disabled;
};

} // namespace codexui

#endif // CODEXUI_UI_INTERACTIVEREQUESTDIALOG_H
