// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/InspectorPane.h"

#include "codex/DiffViewer.h"
#include "codex/PendingRequestPolicy.h"
#include "codex/UiStatus.h"
#include "codex/middle/ConversationHeightIndex.h"
#include "codex/middle/ConversationPresentation.h"
#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <ranges>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {

using nodegraph::scalarTextFromValue;
using nodegraph::unsignedIntegerFromValue;
using nodegraph::valueMember;

constexpr int MaximumProtocolLines = 2000;
constexpr int InfoChoicePage = 0;
constexpr int StatePage = 1;
constexpr int ProtocolPage = 2;
constexpr int InspectorRowMargin = 12;
constexpr int InspectorRowSpacing = 8;
constexpr int InspectorRowOverscan = 2;

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QStringList texts(const std::vector<std::string> &values) {
  QStringList result;
  result.reserve(static_cast<qsizetype>(values.size()));
  for (const std::string &value : values)
    result.push_back(text(value));
  return result;
}

bool sensitiveDiagnosticText(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const unsigned char character : value)
    lowered.push_back(static_cast<char>(std::tolower(character)));
  constexpr std::array markers{
      std::string_view("authorization"), std::string_view("bearer "),
      std::string_view("password"),      std::string_view("secret"),
      std::string_view("token="),        std::string_view("token:"),
      std::string_view("credential"),    std::string_view("cookie"),
      std::string_view("-----begin"),    std::string_view("github_pat_"),
      std::string_view("ghp_"),          std::string_view("xoxb-"),
      std::string_view("xoxp-")};
  return std::ranges::any_of(markers,
                             [&lowered](std::string_view marker) {
                               return lowered.find(marker) != std::string::npos;
                             }) ||
         lowered.find("sk-") != std::string::npos;
}

QString protocolMetadata(const nodegraph::Value *value,
                         qsizetype maximumCharacters = 240) {
  const std::string raw = scalarTextFromValue(value);
  if (raw.empty())
    return {};
  if (sensitiveDiagnosticText(raw))
    return QStringLiteral("<redacted sensitive text>");
  QString result = text(raw);
  for (qsizetype index = 0; index < result.size(); ++index) {
    const QChar character = result.at(index);
    if (character.unicode() < 0x20U || character.unicode() == 0x7fU)
      result[index] = QLatin1Char(' ');
  }
  if (result.size() > maximumCharacters) {
    result.truncate(maximumCharacters);
    result += QStringLiteral("...");
  }
  return result;
}

using UiStyle::makeLabel;

QString agentCopyText(const ui::InspectorAgentRow &agent) {
  QStringList lines{QStringLiteral("Agent")};
  if (!agent.agentPath.empty())
    lines << QStringLiteral("Path: %1").arg(text(agent.agentPath));
  if (!agent.status.empty())
    lines
        << QStringLiteral("Status: %1").arg(text(displayStatus(agent.status)));
  if (!agent.tool.empty())
    lines << QStringLiteral("Tool: %1").arg(text(agent.tool));
  if (!agent.model.empty())
    lines << QStringLiteral("Model: %1").arg(text(agent.model));
  if (!agent.reasoningEffort.empty())
    lines << QStringLiteral("Reasoning: %1").arg(text(agent.reasoningEffort));
  if (!agent.prompt.empty())
    lines << QString{} << text(agent.prompt);
  if (!agent.resultText.empty())
    lines << QString{} << text(agent.resultText);
  if (!agent.childThreadId.empty())
    lines << QString{}
          << QStringLiteral("Thread: %1").arg(text(agent.childThreadId));
  if (!agent.senderThreadId.empty())
    lines << QStringLiteral("Sender: %1").arg(text(agent.senderThreadId));
  if (!agent.receiverThreadIds.empty())
    lines << QStringLiteral("Receivers: %1")
                 .arg(
                     texts(agent.receiverThreadIds).join(QStringLiteral(", ")));
  return lines.join(QLatin1Char('\n'));
}

class InfoChoiceButton final : public QPushButton {
protected:
  void paintEvent(QPaintEvent *event) override {
    QPushButton::paintEvent(event);
    QStyleOptionButton option;
    initStyleOption(&option);
    const QRect contents =
        style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
    const QRect indicator(contents.right() - 18, contents.top(), 18,
                          contents.height());
    UiStyle::drawChevron(this, indicator, option.state & QStyle::State_Enabled,
                         option.state &
                             (QStyle::State_MouseOver | QStyle::State_HasFocus),
                         UiStyle::ChevronDirection::Right);
  }
};

QPushButton *infoChoice(const QString &title, const QString &description) {
  auto *button = new InfoChoiceButton;
  button->setProperty("kind", "infoChoice");
  button->setMinimumHeight(64);
  button->setCursor(Qt::PointingHandCursor);

  auto *layout = new QHBoxLayout(button);
  layout->setContentsMargins(12, 9, 30, 9);
  layout->setSpacing(8);
  auto *copy = new QVBoxLayout;
  copy->setSpacing(2);
  auto *titleLabel = makeLabel(title, "title");
  auto *descriptionLabel = makeLabel(description, "meta");
  titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  descriptionLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
  descriptionLabel->setTextInteractionFlags(Qt::NoTextInteraction);
  copy->addWidget(titleLabel);
  copy->addWidget(descriptionLabel);
  layout->addLayout(copy, 1);
  return button;
}

QWidget *infoDetail(const QString &title, QWidget *content,
                    QPushButton **backButton) {
  auto *page = new QWidget;
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(8);
  auto *heading = new QHBoxLayout;
  *backButton = new QPushButton(QStringLiteral("‹  Info"));
  (*backButton)->setProperty("kind", "subtle");
  (*backButton)->setFixedHeight(28);
  heading->addWidget(*backButton);
  heading->addStretch();
  heading->addWidget(makeLabel(title, "title"));
  layout->addLayout(heading);
  layout->addWidget(content, 1);
  return page;
}

struct ScrollPosition {
  bool followsTail = true;
  int value = 0;
};

void restoreScrollPosition(QPlainTextEdit *view,
                           const ScrollPosition &position) {
  QScrollBar *scrollBar = view->verticalScrollBar();
  if (position.followsTail) {
    scrollBar->setValue(scrollBar->maximum());
    return;
  }
  scrollBar->setValue(
      std::clamp(position.value, scrollBar->minimum(), scrollBar->maximum()));
}

} // namespace

class InspectorPane::PlanStepFrame final : public QFrame {
public:
  PlanStepFrame() {
    setObjectName(QStringLiteral("inspectorPlanStepFrame"));
    setProperty("kind", "raised");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    description_ = makeLabel({});
    description_->setObjectName(QStringLiteral("planStepDescription"));
    status_ = makeLabel({}, "meta");
    status_->setObjectName(QStringLiteral("planStepStatus"));
    layout->addWidget(description_);
    layout->addWidget(status_);
  }

  void apply(const PlanStepData &step) {
    const QString description = text(step.text);
    if (description_->text() != description)
      description_->setText(description);
    presentation::setAccessibleNameIfChanged(*this, description_->text());
    presentation::applyStatusLabel(*status_, step.status);
    presentation::setAccessibleDescriptionIfChanged(*this, status_->text());
  }

private:
  QLabel *description_ = nullptr;
  QLabel *status_ = nullptr;
};

class InspectorPane::AgentFrame final : public QFrame {
public:
  AgentFrame() {
    setObjectName(QStringLiteral("inspectorAgentFrame"));
    setProperty("kind", "raised");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);

    auto *heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(4);
    auto *title = makeLabel(QStringLiteral("Agent"), "title");
    title->setObjectName(QStringLiteral("agentTitle"));
    title->setWordWrap(false);
    title->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    heading->addWidget(title, 0, Qt::AlignBaseline);
    name_ = makeLabel({}, "code");
    name_->setObjectName(QStringLiteral("agentName"));
    name_->setWordWrap(false);
    name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    heading->addWidget(name_, 1, Qt::AlignBaseline);
    status_ = makeLabel({}, "meta");
    status_->setObjectName(QStringLiteral("agentStatus"));
    status_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    heading->addWidget(status_, 0, Qt::AlignBaseline);
    copy_ = new presentation::CopyButton(QStringLiteral("Copy agent content"),
                                         this);
    copy_->setObjectName(QStringLiteral("agentCopyButton"));
    disclosure_ = new presentation::DisclosureButton(
        QStringLiteral("Expand agent"), QStringLiteral("Collapse agent"), this);
    disclosure_->setObjectName(QStringLiteral("agentDisclosureButton"));
    heading->addWidget(copy_, 0, Qt::AlignRight | Qt::AlignVCenter);
    heading->addWidget(disclosure_, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addLayout(heading);

    content_ = new QWidget(this);
    content_->setObjectName(QStringLiteral("agentCardContent"));
    contentLayout_ = new QVBoxLayout(content_);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(6);
    metadata_ = makeLabel({}, "meta");
    metadata_->setObjectName(QStringLiteral("agentMetadata"));
    prompt_ = makeLabel({});
    prompt_->setObjectName(QStringLiteral("agentPrompt"));
    identities_ = makeLabel({}, "meta");
    identities_->setObjectName(QStringLiteral("agentIdentities"));
    contentLayout_->addWidget(metadata_);
    contentLayout_->addWidget(prompt_);
    contentLayout_->addWidget(identities_);
    layout->addWidget(content_);

    QObject::connect(copy_, &QToolButton::clicked, this, [this] {
      if (!rendered_)
        return;
      const QString value = agentCopyText(*rendered_);
      copy_->copyText(value, true);
    });
    setExpanded(false);
  }

  void apply(const ui::InspectorAgentRow &agent) {
    if (rendered_ && *rendered_ == agent)
      return;
    rendered_ = agent;
    const QString agentPath = text(agent.agentPath);
    const QStringList pathParts = agentPath.split('/', Qt::SkipEmptyParts);
    const QString agentName =
        !pathParts.isEmpty() ? pathParts.back() : text(agent.tool);
    if (name_->text() != agentName)
      name_->setText(agentName);
    if (name_->toolTip() != agentPath)
      name_->setToolTip(agentPath);
    presentation::setAccessibleNameIfChanged(
        *this, agentName.isEmpty()
                   ? QStringLiteral("Agent")
                   : QStringLiteral("Agent %1").arg(agentName));
    presentation::applyStatusLabel(*status_, agent.status);
    presentation::setAccessibleDescriptionIfChanged(*this, status_->text());

    QStringList metadata;
    if (!agent.tool.empty() && !agentPath.isEmpty())
      metadata << text(agent.tool);
    for (const std::string *value : {&agent.model, &agent.reasoningEffort})
      if (!value->empty())
        metadata << text(*value);
    setOptionalText(metadata_, metadata.join(QStringLiteral("  ·  ")));
    setOptionalText(prompt_, text(agent.prompt));

    const QString resultText = text(agent.resultText);
    if (!resultText.isEmpty() && !result_) {
      result_ = new MarkdownTextView({}, 0, content_);
      result_->setAccessibleName(QStringLiteral("Agent result"));
      contentLayout_->insertWidget(contentLayout_->indexOf(identities_),
                                   result_);
    }
    if (result_) {
      result_->setContent(resultText);
      result_->setVisible(!resultText.isEmpty());
    }

    QStringList identities;
    if (!agent.childThreadId.empty())
      identities << QStringLiteral("thread %1").arg(text(agent.childThreadId));
    if (!agent.senderThreadId.empty())
      identities << QStringLiteral("sender %1").arg(text(agent.senderThreadId));
    const QString receivers =
        texts(agent.receiverThreadIds).join(QStringLiteral(", "));
    if (!receivers.isEmpty())
      identities << QStringLiteral("receivers %1").arg(receivers);
    setOptionalText(identities_, identities.join(QStringLiteral("  |  ")));
  }

  void setExpanded(bool expanded) {
    disclosure_->setExpanded(expanded);
    content_->setVisible(expanded);
  }

  [[nodiscard]] presentation::DisclosureButton *disclosure() const noexcept {
    return disclosure_;
  }

private:
  static void setOptionalText(QLabel *label, QString value) {
    const bool visible = !value.isEmpty();
    if (label->text() != value)
      label->setText(std::move(value));
    if (label->isHidden() == visible)
      label->setVisible(visible);
  }

  QLabel *name_ = nullptr;
  QLabel *status_ = nullptr;
  presentation::CopyButton *copy_ = nullptr;
  presentation::DisclosureButton *disclosure_ = nullptr;
  QWidget *content_ = nullptr;
  QVBoxLayout *contentLayout_ = nullptr;
  QLabel *metadata_ = nullptr;
  QLabel *prompt_ = nullptr;
  MarkdownTextView *result_ = nullptr;
  QLabel *identities_ = nullptr;
  std::optional<ui::InspectorAgentRow> rendered_;
};

class InspectorPane::RequestFrame final : public QFrame {
public:
  explicit RequestFrame(InspectorPane *owner) : owner_(owner) {
    setObjectName(QStringLiteral("inspectorRequestFrame"));
    setProperty("kind", "raised");
    setProperty("tone", "warning");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    title_ = makeLabel({}, "title");
    title_->setObjectName(QStringLiteral("pendingRequestTitle"));
    detail_ = makeLabel({}, "meta");
    detail_->setObjectName(QStringLiteral("pendingRequestDetail"));
    status_ = makeLabel({}, "meta");
    status_->setObjectName(QStringLiteral("pendingRequestStatus"));
    layout->addWidget(title_);
    layout->addWidget(detail_);
    layout->addWidget(status_);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 2, 0, 0);
    actions->addStretch();
    reject_ = new QPushButton(QStringLiteral("Reject"));
    reject_->setObjectName(QStringLiteral("pendingRequestReject"));
    reject_->setProperty("kind", "destructive");
    reject_->setFixedHeight(28);
    accept_ = new QPushButton(QStringLiteral("Accept"));
    accept_->setObjectName(QStringLiteral("pendingRequestAccept"));
    accept_->setProperty("kind", "request");
    accept_->setFixedHeight(28);
    review_ = new QPushButton(QStringLiteral("Review"));
    review_->setObjectName(QStringLiteral("pendingRequestReview"));
    review_->setProperty("kind", "request");
    review_->setFixedHeight(28);
    actions->addWidget(reject_);
    actions->addWidget(accept_);
    actions->addWidget(review_);
    layout->addLayout(actions);

    connect(reject_, &QPushButton::clicked, this, [this] {
      invoke(owner_ ? owner_->rejectRequest : RequestAction{});
    });
    connect(accept_, &QPushButton::clicked, this, [this] {
      invoke(owner_ ? owner_->acceptRequest : RequestAction{});
    });
    connect(review_, &QPushButton::clicked, this, [this] {
      invoke(owner_ ? owner_->reviewRequest : RequestAction{});
    });
  }

  [[nodiscard]] bool apply(const PendingRequestDescriptor &request) {
    target_ = request.target;
    const QString title = text(PendingRequestPolicy::title(request.kind));
    const QString detail = text(PendingRequestPolicy::detail(request));
    const QString statusText = text(PendingRequestPolicy::status(request));
    const PendingRequestControls controls =
        PendingRequestPolicy::controls(request);
    const QString rejectText =
        controls.negative ? text(controls.negative->label) : QString{};
    const QString acceptText =
        controls.positive ? text(controls.positive->label) : QString{};
    const bool geometryChanged =
        title_->text() != title || detail_->text() != detail ||
        status_->text() != statusText ||
        status_->isVisible() != !statusText.isEmpty() ||
        reject_->isVisible() != controls.negative.has_value() ||
        (controls.negative && reject_->text() != rejectText) ||
        accept_->isVisible() != controls.positive.has_value() ||
        (controls.positive && accept_->text() != acceptText);
    presentation::setAccessibleNameIfChanged(*this, title);
    presentation::setAccessibleDescriptionIfChanged(
        *this, statusText.isEmpty()
                   ? detail
                   : detail + QStringLiteral("  |  ") + statusText);
    if (title_->text() != title)
      title_->setText(title);
    if (detail_->text() != detail)
      detail_->setText(detail);
    if (status_->text() != statusText)
      status_->setText(statusText);
    status_->setVisible(!statusText.isEmpty());

    const bool rejectEnabled = controls.directEnabled && controls.negative;
    const bool acceptEnabled = controls.directEnabled && controls.positive;
    const bool transferFocus = (reject_->hasFocus() && !rejectEnabled) ||
                               (accept_->hasFocus() && !acceptEnabled) ||
                               (review_->hasFocus() && !controls.reviewEnabled);
    if (controls.negative)
      reject_->setText(rejectText);
    reject_->setVisible(controls.negative.has_value());
    reject_->setEnabled(rejectEnabled);
    if (controls.positive)
      accept_->setText(acceptText);
    accept_->setVisible(controls.positive.has_value());
    accept_->setEnabled(acceptEnabled);
    review_->setVisible(true);
    review_->setEnabled(controls.reviewEnabled);
    if (transferFocus) {
      if (controls.reviewEnabled)
        review_->setFocus();
      else
        owner_->inspectorTabs->currentWidget()->setFocus();
    }
    return geometryChanged;
  }

private:
  void invoke(const RequestAction &action) {
    if (action && target_) {
      QPointer<RequestFrame> guard(this);
      invokingAction_ = true;
      action(target_);
      if (guard)
        invokingAction_ = false;
    }
  }

  [[nodiscard]] bool invokingAction() const noexcept { return invokingAction_; }

  friend class RowViewport;

  InspectorPane *owner_ = nullptr;
  QLabel *title_ = nullptr;
  QLabel *detail_ = nullptr;
  QLabel *status_ = nullptr;
  QPushButton *reject_ = nullptr;
  QPushButton *accept_ = nullptr;
  QPushButton *review_ = nullptr;
  nodegraph::NodeRef target_;
  bool invokingAction_ = false;
};

class InspectorPane::RowViewport final : public QAbstractScrollArea {
public:
  enum class Page { Plan, Agents, Requests };

  RowViewport(InspectorPane *owner, Page page, QString objectName,
              QString accessibleName)
      : QAbstractScrollArea(owner), owner_(owner), pageType_(page) {
    setObjectName(std::move(objectName));
    setAccessibleName(std::move(accessibleName));
    setProperty("kind", "inspectorScroll");
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    verticalScrollBar()->setSingleStep(20);
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *old, QWidget *current) {
              Resident *source = residentFor(old);
              Resident *destination = residentFor(current);
              if (!source && !destination)
                return;
              const QScopedValueRollback dispatching(
                  dispatchingRow_, source ? source->widget : nullptr);
              synchronize();
            });
  }

  [[nodiscard]] ui::InspectorRowRequest rowRequest() const {
    ui::InspectorRowRequest result;
    if (rowCount() != 0)
      result.first = materializationRows().first;
    result.anchorKey = captureAnchor().key;
    QWidget *focused = QApplication::focusWidget();
    for (const Resident &resident : residents_)
      if (ownsFocus(resident.widget, focused)) {
        result.focusedKey = resident.key;
        break;
      }
    if (pageType_ == Page::Agents)
      result.retainedKeys.assign(owner_->expandedAgentIds.begin(),
                                 owner_->expandedAgentIds.end());
    return result;
  }

  [[nodiscard]] bool
  retainsTarget(const nodegraph::NodeRef &target) const noexcept {
    if (!target)
      return false;
    const auto retains = [&target](const ui::InspectorRow &row) {
      const auto *request = std::get_if<PendingRequestDescriptor>(&row.value);
      return request && request->target == target;
    };
    return std::ranges::any_of(pageData_.rows, retains) ||
           (pageData_.focused && retains(*pageData_.focused));
  }

  void setActive(bool active) {
    if (active_ == active)
      return;
    active_ = active;
    if (!active_) {
      pendingFocus_.reset();
      releaseAll(true);
      return;
    }
    synchronize();
  }

  void apply(ui::InspectorPageSnapshot next, bool replace = false) {
    if (!replace && pageData_ == next)
      return;
    bool needsPage = false;
    {
      const QScopedValueRollback synchronizing(synchronizing_, true);
      const Anchor anchor = replace ? Anchor{} : captureAnchor();
      const bool orderChanged = replace || rowCount() != rowCount(next) ||
                                pageData_.orderRevision != next.orderRevision;
      ui::InspectorPageSnapshot previous = std::move(pageData_);
      if (orderChanged)
        pendingFocus_.reset();
      if (replace)
        releaseAll(true);
      pageData_ = std::move(next);
      if (orderChanged)
        rebuildHeightIndex();
      if (replace)
        verticalScrollBar()->setValue(0);

      for (auto iterator = residents_.begin(); iterator != residents_.end();) {
        const std::optional<std::size_t> row =
            rowForKey(pageData_, iterator->key);
        if (!row) {
          release(iterator->widget, true);
          iterator = residents_.erase(iterator);
          continue;
        }
        iterator->row = *row;
        const ui::InspectorRow current = *modelRow(pageData_, *row);
        const ui::InspectorRow old =
            *modelRow(previous, *rowForKey(previous, iterator->key));
        if (current != old && patch(iterator->widget, current) &&
            !orderChanged) {
          static_cast<void>(measure(*iterator));
          iterator->measured = true;
        }
        ++iterator;
      }
      if (active_ && indexedWidth_ != -2)
        needsPage = reconcile(anchor, orderChanged);
      else {
        updateScrollRange();
        restoreAnchor(anchor);
        layoutResidents();
      }
    }
    if (needsPage && owner_->refreshRequested)
      owner_->refreshRequested();
    if (!needsPage)
      completePendingFocus();
  }

  void setAgentExpanded(const std::string &key, bool expanded) {
    Resident *resident = residentForKey(key);
    if (!resident)
      return;
    static_cast<AgentFrame *>(resident->widget)->setExpanded(expanded);
    synchronize(resident);
  }

protected:
  bool focusNextPrevChild(bool next) override {
    if (!active_)
      return QAbstractScrollArea::focusNextPrevChild(next);
    QWidget *focused = QApplication::focusWidget();
    Resident *source = residentFor(focused);
    if (source) {
      const std::vector<QWidget *> stops = tabStops(source->widget);
      const auto current = std::ranges::find(stops, focused);
      if (current != stops.end()) {
        const auto target = next                       ? std::next(current)
                            : current == stops.begin() ? stops.end()
                                                       : std::prev(current);
        if (target != stops.end()) {
          (*target)->setFocus(next ? Qt::TabFocusReason
                                   : Qt::BacktabFocusReason);
          reveal(*target, next);
          return true;
        }
      }
    } else if (focused != this || !next) {
      return QAbstractScrollArea::focusNextPrevChild(next);
    }

    const qint64 edge = next ? verticalScrollBar()->value() - InspectorRowMargin
                             : verticalScrollBar()->value() +
                                   viewport()->height() - InspectorRowMargin;
    std::ptrdiff_t row = static_cast<std::ptrdiff_t>(
        source ? source->row : heights_.rowAt(std::max<qint64>(0, edge)));
    if (source)
      row += next ? 1 : -1;
    if (focusFrom(row, next, source != nullptr))
      return true;
    if (source && !next) {
      setFocus(Qt::BacktabFocusReason);
      return true;
    }
    return QAbstractScrollArea::focusNextPrevChild(next);
  }

  void resizeEvent(QResizeEvent *event) override {
    QAbstractScrollArea::resizeEvent(event);
    synchronize();
  }

  void scrollContentsBy(int dx, int dy) override {
    QAbstractScrollArea::scrollContentsBy(dx, dy);
    synchronize();
  }

  bool eventFilter(QObject *watched, QEvent *event) override {
    if (!active_ || event->type() != QEvent::Wheel)
      return QAbstractScrollArea::eventFilter(watched, event);
    auto *wheel = static_cast<QWheelEvent *>(event);
    int delta = wheel->pixelDelta().y();
    if (delta == 0)
      delta =
          wheel->angleDelta().y() / 120 * verticalScrollBar()->singleStep() * 3;
    if (delta == 0)
      return false;
    QScrollBar *bar = verticalScrollBar();
    const int next =
        std::clamp(bar->value() - delta, bar->minimum(), bar->maximum());
    if (next == bar->value())
      return false;
    Resident *source = residentFor(qobject_cast<QWidget *>(watched));
    const QScopedValueRollback dispatching(dispatchingRow_,
                                           source ? source->widget : nullptr);
    bar->setValue(next);
    wheel->accept();
    return true;
  }

  bool event(QEvent *event) override {
    const bool result = QAbstractScrollArea::event(event);
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::StyleChange:
    case QEvent::ApplicationFontChange:
    case QEvent::DevicePixelRatioChange:
      if (indexedWidth_ == -2)
        break;
      indexedWidth_ = -2;
      for (const Resident &resident : residents_) {
        for (MarkdownTextView *view :
             resident.widget->findChildren<MarkdownTextView *>())
          view->invalidateGeometryEnvironment();
        if (auto *view = qobject_cast<MarkdownTextView *>(resident.widget))
          view->invalidateGeometryEnvironment();
      }
      // Descendant layouts observe a new font, style, or DPR in the next Qt
      // event-loop phase; measure once they share that geometry environment.
      QTimer::singleShot(0, this, [this] {
        if (indexedWidth_ != -2)
          return;
        indexedWidth_ = -1;
        synchronize();
      });
      break;
    default:
      break;
    }
    return result;
  }

private:
  struct PageStyle {
    std::string_view messageKey;
    std::string_view accessibleName;
    const char *objectName;
    int estimatedHeight;
  };

  const PageStyle &pageStyle() const {
    static constexpr std::array styles{
        PageStyle{"plan:message", "Plan status", "inspectorPlanMessage", 68},
        PageStyle{"agents:message", "Agent status", "inspectorAgentMessage",
                  52},
        PageStyle{"requests:message", "Request status",
                  "inspectorRequestMessage", 120}};
    return styles[static_cast<std::size_t>(pageType_)];
  }

  struct Resident {
    std::string key;
    std::size_t row = 0;
    QWidget *widget = nullptr;
    bool measured = false;
  };

  struct Anchor {
    std::string key;
    int pixelOffset = 0;
  };

  struct PendingFocus {
    std::size_t row = 0;
    bool forward = true;
  };

  static std::size_t rowCount(const ui::InspectorPageSnapshot &page) {
    return page.total != 0
               ? page.total
               : static_cast<std::size_t>(!page.emptyMessage.empty());
  }

  std::size_t rowCount() const { return rowCount(pageData_); }

  bool rowAcceptsTab(const ui::InspectorRow &row) const {
    if (row.key == messageKey() ||
        std::holds_alternative<PlanStepData>(row.value))
      return false;
    const auto *request = std::get_if<PendingRequestDescriptor>(&row.value);
    if (!request)
      return true;
    const PendingRequestControls controls =
        PendingRequestPolicy::controls(*request);
    return controls.reviewEnabled ||
           (controls.directEnabled && (controls.positive || controls.negative));
  }

  static bool ownsFocus(QWidget *row, QWidget *focused) {
    return focused && (focused == row || row->isAncestorOf(focused));
  }

  Resident *residentForKey(std::string_view key) {
    const auto found = std::ranges::find(residents_, key, &Resident::key);
    return found == residents_.end() ? nullptr : &*found;
  }

  Resident *residentFor(QWidget *widget) {
    if (!widget)
      return nullptr;
    const auto found = std::ranges::find_if(residents_, [widget](auto &entry) {
      QWidget *row = entry.widget;
      return row == widget || row->isAncestorOf(widget);
    });
    return found == residents_.end() ? nullptr : &*found;
  }

  static std::vector<QWidget *> tabStops(QWidget *row) {
    std::vector<QWidget *> result;
    QWidget *candidate = row;
    do {
      if ((candidate == row || row->isAncestorOf(candidate)) &&
          candidate->isEnabled() && candidate->isVisibleTo(row) &&
          (candidate->focusPolicy() & Qt::TabFocus))
        result.push_back(candidate);
      candidate = candidate->nextInFocusChain();
    } while (candidate && candidate != row);
    return result;
  }

  void scrollToRow(std::size_t row, bool fromTop) {
    const qint64 top = InspectorRowMargin + heights_.top(row);
    const qint64 target = fromTop
                              ? top
                              : top + heights_.height(row) -
                                    InspectorRowSpacing - viewport()->height();
    verticalScrollBar()->setValue(static_cast<int>(
        std::clamp<qint64>(target, verticalScrollBar()->minimum(),
                           verticalScrollBar()->maximum())));
    synchronize();
  }

  bool focusFrom(std::ptrdiff_t row, bool forward, bool requestPage = true) {
    while (row >= 0 && static_cast<std::size_t>(row) < rowCount()) {
      const std::size_t candidate = static_cast<std::size_t>(row);
      const std::optional<ui::InspectorRow> model =
          modelRow(pageData_, candidate);
      if (!model) {
        if (pageType_ == Page::Plan || !requestPage)
          break;
        pendingFocus_ = PendingFocus{candidate, forward};
        scrollToRow(candidate, forward);
        return true;
      }
      if (!rowAcceptsTab(*model)) {
        row += forward ? 1 : -1;
        continue;
      }
      scrollToRow(candidate, forward);
      Resident *target = residentForKey(model->key);
      const std::vector<QWidget *> stops =
          target ? tabStops(target->widget) : std::vector<QWidget *>{};
      if (stops.empty()) {
        row += forward ? 1 : -1;
        continue;
      }
      QWidget *control = forward ? stops.front() : stops.back();
      control->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
      reveal(control, forward);
      pendingFocus_.reset();
      return true;
    }
    pendingFocus_.reset();
    return false;
  }

  void completePendingFocus() {
    if (!pendingFocus_ || synchronizing_)
      return;
    const PendingFocus pending = *pendingFocus_;
    pendingFocus_.reset();
    if (!focusFrom(static_cast<std::ptrdiff_t>(pending.row), pending.forward,
                   false))
      QAbstractScrollArea::focusNextPrevChild(pending.forward);
  }

  void reveal(QWidget *control, bool fromTop) {
    const QRect geometry(control->mapTo(viewport(), QPoint{}), control->size());
    int delta = 0;
    if (geometry.height() >= viewport()->height()) {
      if (fromTop &&
          (geometry.top() < 0 || geometry.top() >= viewport()->height()))
        delta = geometry.top();
      else if (!fromTop && (geometry.bottom() < 0 ||
                            geometry.bottom() >= viewport()->height()))
        delta = geometry.bottom() - viewport()->height() + 1;
    } else if (geometry.top() < 0) {
      delta = geometry.top();
    } else if (geometry.bottom() >= viewport()->height()) {
      delta = geometry.bottom() - viewport()->height() + 1;
    }
    if (delta != 0)
      verticalScrollBar()->setValue(verticalScrollBar()->value() + delta);
  }

  std::string_view messageKey() const { return pageStyle().messageKey; }

  std::optional<ui::InspectorRow>
  modelRow(const ui::InspectorPageSnapshot &page, std::size_t row) const {
    if (row >= rowCount(page))
      return std::nullopt;
    if (page.total == 0) {
      if (row != 0 || page.emptyMessage.empty())
        return std::nullopt;
      return ui::InspectorRow{
          std::string(messageKey()),
          ui::InspectorMarkdownRow{page.emptyMessage,
                                   std::string(pageStyle().accessibleName), {}}};
    }
    if (row >= page.first && row - page.first < page.rows.size())
      return page.rows[row - page.first];
    if (page.focusedIndex == row && page.focused)
      return *page.focused;
    return std::nullopt;
  }

  QWidget *create(const ui::InspectorRow &row) {
    QWidget *widget = nullptr;
    if (const auto *markdown =
            std::get_if<ui::InspectorMarkdownRow>(&row.value)) {
      if (row.key == messageKey()) {
        auto *label = makeLabel({}, "muted");
        label->setObjectName(QString::fromLatin1(pageStyle().objectName));
        widget = label;
      } else {
        auto *view = new MarkdownTextView({});
        view->setPreparedContent(text(markdown->text),
                                 text(markdown->preparedHtml));
        widget = view;
      }
    } else if (std::holds_alternative<PlanStepData>(row.value)) {
      widget = new PlanStepFrame;
    } else if (std::holds_alternative<ui::InspectorAgentRow>(row.value)) {
      widget = owner_->agentFrame(row.key);
    } else {
      widget = owner_->requestFrame();
    }
    widget->setParent(viewport());
    static_cast<void>(patch(widget, row));
    if (auto *view = qobject_cast<MarkdownTextView *>(widget))
      view->viewport()->installEventFilter(this);
    return widget;
  }

  [[nodiscard]] bool patch(QWidget *widget, const ui::InspectorRow &row) {
    if (const auto *markdown =
            std::get_if<ui::InspectorMarkdownRow>(&row.value)) {
      presentation::setAccessibleNameIfChanged(
          *widget, text(markdown->accessibleName));
      if (row.key == messageKey())
        static_cast<QLabel *>(widget)->setText(text(markdown->text));
      else
        static_cast<MarkdownTextView *>(widget)->setPreparedContent(
            text(markdown->text), text(markdown->preparedHtml));
      return true;
    }
    if (const auto *step = std::get_if<PlanStepData>(&row.value)) {
      static_cast<PlanStepFrame *>(widget)->apply(*step);
      return true;
    }
    if (const auto *agent = std::get_if<ui::InspectorAgentRow>(&row.value)) {
      auto *frame = static_cast<AgentFrame *>(widget);
      frame->apply(*agent);
      if (MarkdownTextView *view = frame->findChild<MarkdownTextView *>())
        view->viewport()->installEventFilter(this);
      return true;
    }
    return static_cast<RequestFrame *>(widget)->apply(
        std::get<PendingRequestDescriptor>(row.value));
  }

  void synchronize(Resident *remeasure = nullptr) {
    if (synchronizing_ || !active_ || indexedWidth_ == -2)
      return;
    bool needsPage = false;
    {
      const QScopedValueRollback synchronizing(synchronizing_, true);
      const Anchor anchor = captureAnchor();
      if (remeasure) {
        static_cast<void>(measure(*remeasure));
        remeasure->measured = true;
      }
      needsPage = reconcile(anchor, false);
    }
    if (needsPage && owner_->refreshRequested)
      owner_->refreshRequested();
  }

  bool reconcile(const Anchor &anchor, bool remeasureResidents) {
    constexpr int MaximumSettlementPasses = 3;
    constexpr qint64 AdmissionBudgetMilliseconds = 3;
    QElapsedTimer admissionTiming;
    admissionTiming.start();
    for (int pass = 0; pass < MaximumSettlementPasses; ++pass) {
      bool heightChanged = false;
      if (indexedWidth_ != rowWidth()) {
        rebuildHeightIndex();
        remeasureResidents = true;
      }
      if (remeasureResidents) {
        for (Resident &resident : residents_) {
          heightChanged |= measure(resident);
          resident.measured = true;
        }
        remeasureResidents = false;
      }
      updateScrollRange();
      restoreAnchor(anchor);
      const auto [first, last] = materializationRows();
      if (!pageCovers(first, last)) {
        layoutResidents();
        return true;
      }
      releaseOutside(first, last);
      bool created = false;
      for (std::size_t row = first; row < last; ++row) {
        const ui::InspectorRow model = *modelRow(pageData_, row);
        Resident *resident = residentForKey(model.key);
        if (!resident) {
          residents_.push_back({model.key, row, create(model), false});
          resident = &residents_.back();
          created = true;
          updateScrollRange();
          restoreAnchor(anchor);
          layoutResidents();
          scheduleAdmission();
          return false;
        } else {
          resident->row = row;
        }
        if (!resident->measured) {
          heightChanged |= measure(*resident);
          resident->measured = true;
          if (row + 1 < last &&
              admissionTiming.elapsed() >= AdmissionBudgetMilliseconds) {
            updateScrollRange();
            restoreAnchor(anchor);
            layoutResidents();
            scheduleAdmission();
            return false;
          }
        }
      }
      if (heightChanged) {
        updateScrollRange();
        restoreAnchor(anchor);
      }
      if ((!created && indexedWidth_ == rowWidth()) ||
          pass + 1 == MaximumSettlementPasses)
        break;
    }
    layoutResidents();
    return false;
  }

  void scheduleAdmission() {
    if (admissionScheduled_)
      return;
    admissionScheduled_ = true;
    QTimer::singleShot(1, Qt::PreciseTimer, this, [this] {
      admissionScheduled_ = false;
      synchronize();
    });
  }

  [[nodiscard]] bool pageCovers(std::size_t first, std::size_t last) const {
    if (first >= last)
      return true;
    if (pageData_.total == 0)
      return rowCount() == 1 && first == 0 && last == 1 &&
             !pageData_.emptyMessage.empty();
    if (first < pageData_.first)
      return false;
    const std::size_t offset = first - pageData_.first;
    return offset <= pageData_.rows.size() &&
           last - first <= pageData_.rows.size() - offset;
  }

  std::pair<std::size_t, std::size_t> materializationRows() const {
    if (rowCount() == 0)
      return {0, 0};
    const qint64 scroll = verticalScrollBar()->value();
    const qint64 visibleTop = std::max<qint64>(0, scroll - InspectorRowMargin);
    const qint64 visibleBottom =
        std::max(visibleTop, scroll + std::max(1, viewport()->height()) -
                                 InspectorRowMargin);
    std::size_t first = heights_.rowAt(visibleTop);
    std::size_t last = heights_.rowAt(visibleBottom) + 1;
    first = first > InspectorRowOverscan ? first - InspectorRowOverscan : 0;
    last = std::min(rowCount(), last + InspectorRowOverscan);
    if (last - first > ui::MaximumInspectorRows)
      last = first + ui::MaximumInspectorRows;
    return {first, last};
  }

  void releaseOutside(std::size_t first, std::size_t last) {
    QWidget *focused = QApplication::focusWidget();
    std::erase_if(residents_, [this, first, last, focused](Resident &resident) {
      if ((resident.row >= first && resident.row < last) ||
          ownsFocus(resident.widget, focused))
        return false;
      release(resident.widget, false);
      return true;
    });
  }

  void releaseAll(bool transferFocus) {
    for (Resident &resident : residents_)
      release(resident.widget, transferFocus);
    residents_.clear();
  }

  void release(QWidget *widget, bool transferFocus) {
    QWidget *focused = QApplication::focusWidget();
    const bool focusedRow = ownsFocus(widget, focused);
    if (transferFocus && focusedRow) {
      if (active_)
        setFocus(Qt::OtherFocusReason);
      else
        owner_->inspectorTabs->setFocus(Qt::OtherFocusReason);
    }
    widget->hide();
    auto *request = dynamic_cast<RequestFrame *>(widget);
    if (focusedRow || widget == dispatchingRow_ ||
        (request && request->invokingAction())) {
      widget->setParent(nullptr);
      widget->deleteLater();
    } else {
      delete widget;
    }
  }

  bool measure(const Resident &resident) {
    QWidget *widget = resident.widget;
    const int width = rowWidth();
    widget->setFixedWidth(width);
    int height = 0;
    if (auto *markdown = qobject_cast<MarkdownTextView *>(widget)) {
      height = markdown->heightForWidth(width);
    } else if (QLayout *layout = widget->layout()) {
      layout->invalidate();
      layout->activate();
      auto *frame = qobject_cast<QFrame *>(widget);
      const int frameWidth = frame ? frame->frameWidth() : 0;
      const int contentsWidth = std::max(1, width - 2 * frameWidth);
      height =
          (layout->hasHeightForWidth() ? layout->heightForWidth(contentsWidth)
                                       : layout->sizeHint().height()) +
          2 * frameWidth;
    } else {
      height = widget->hasHeightForWidth() ? widget->heightForWidth(width)
                                           : widget->sizeHint().height();
    }
    height = std::max(1, height);
    return heights_.setHeight(resident.row, height + InspectorRowSpacing);
  }

  void rebuildHeightIndex() {
    if (indexedWidth_ != -2)
      indexedWidth_ = rowWidth();
    const int estimate =
        pageData_.total == 0 ? 28 : pageStyle().estimatedHeight;
    std::vector<int> extents(rowCount(), estimate + InspectorRowSpacing);
    heights_.assign(extents);
  }

  void updateScrollRange() {
    const qint64 naturalHeight = rowCount() == 0 ? 0
                                                 : 2 * InspectorRowMargin +
                                                       heights_.totalHeight() -
                                                       InspectorRowSpacing;
    const qint64 maximum64 =
        std::max<qint64>(0, naturalHeight - viewport()->height());
    verticalScrollBar()->setPageStep(std::max(0, viewport()->height()));
    verticalScrollBar()->setRange(
        0, static_cast<int>(std::min<qint64>(INT_MAX, maximum64)));
  }

  void layoutResidents() {
    const int scroll = verticalScrollBar()->value();
    const int width = rowWidth();
    QWidget *focused = QApplication::focusWidget();
    std::ranges::sort(residents_, {}, &Resident::row);
    for (Resident &resident : residents_) {
      const int height =
          std::max(1, heights_.height(resident.row) - InspectorRowSpacing);
      const qint64 top64 = InspectorRowMargin + heights_.top(resident.row) -
                           static_cast<qint64>(scroll);
      const int top =
          static_cast<int>(std::clamp<qint64>(top64, INT_MIN, INT_MAX));
      const QRect geometry(InspectorRowMargin, top, width, height);
      resident.widget->setGeometry(geometry);
      resident.widget->setVisible(
          resident.measured &&
          (geometry.intersects(viewport()->rect()) ||
           ownsFocus(resident.widget, focused)));
    }
  }

  Anchor captureAnchor() const {
    if (rowCount() == 0)
      return {};
    const qint64 scroll = verticalScrollBar()->value();
    const qint64 contentY = std::max<qint64>(0, scroll - InspectorRowMargin);
    const std::size_t row = heights_.rowAt(contentY);
    const auto resident = std::ranges::find_if(
        residents_, [row](const Resident &entry) { return entry.row == row; });
    if (resident != residents_.end())
      return {resident->key, static_cast<int>(InspectorRowMargin +
                                              heights_.top(row) - scroll)};
    const std::optional<ui::InspectorRow> model = modelRow(pageData_, row);
    if (!model)
      return {};
    return {model->key,
            static_cast<int>(InspectorRowMargin + heights_.top(row) - scroll)};
  }

  void restoreAnchor(const Anchor &anchor) {
    if (anchor.key.empty())
      return;
    std::optional<std::size_t> row = rowForKey(pageData_, anchor.key);
    if (!row)
      row = pageData_.anchorIndex;
    if (!row)
      return;
    const qint64 value = InspectorRowMargin + heights_.top(*row) -
                         static_cast<qint64>(anchor.pixelOffset);
    verticalScrollBar()->setValue(static_cast<int>(
        std::clamp<qint64>(value, verticalScrollBar()->minimum(),
                           verticalScrollBar()->maximum())));
  }

  std::optional<std::size_t> rowForKey(const ui::InspectorPageSnapshot &page,
                                       const std::string &key) const {
    if (page.total == 0)
      return !page.emptyMessage.empty() && key == messageKey()
                 ? std::optional<std::size_t>{0}
                 : std::nullopt;
    const auto found =
        std::ranges::find(page.rows, key, &ui::InspectorRow::key);
    if (found != page.rows.end())
      return page.first +
             static_cast<std::size_t>(std::distance(page.rows.begin(), found));
    if (page.focused && page.focused->key == key)
      return page.focusedIndex;
    return std::nullopt;
  }

  int rowWidth() const {
    return std::max(1, viewport()->width() - 2 * InspectorRowMargin);
  }

  InspectorPane *owner_ = nullptr;
  Page pageType_ = Page::Plan;
  ui::InspectorPageSnapshot pageData_;
  ConversationHeightIndex heights_;
  std::vector<Resident> residents_;
  std::optional<PendingFocus> pendingFocus_;
  int indexedWidth_ = -1;
  bool active_ = false;
  bool synchronizing_ = false;
  bool admissionScheduled_ = false;
  QWidget *dispatchingRow_ = nullptr;
};

InspectorPane::AgentFrame *
InspectorPane::agentFrame(const std::string &rowKey) {
  auto *frame = new AgentFrame;
  frame->setExpanded(std::ranges::find(expandedAgentIds, rowKey) !=
                     expandedAgentIds.end());
  QObject::connect(
      frame->disclosure(), &QToolButton::clicked, frame, [this, rowKey, frame] {
        const bool expanded = !frame->disclosure()->isExpanded();
        if (expanded) {
          if (expandedAgentIds.size() >= ui::MaximumInspectorRows) {
            const std::string evicted = expandedAgentIds.front();
            expandedAgentIds.pop_front();
            agentsRows->setAgentExpanded(evicted, false);
          }
          expandedAgentIds.push_back(rowKey);
        } else {
          std::erase(expandedAgentIds, rowKey);
        }
        agentsRows->setAgentExpanded(rowKey, expanded);
      });
  return frame;
}

InspectorPane::RequestFrame *InspectorPane::requestFrame() {
  return new RequestFrame(this);
}

void InspectorPane::prepareMarkdown(ui::InspectorSnapshot &snapshot) {
  const auto prepare = [](ui::InspectorRow &row) {
    auto *markdown = std::get_if<ui::InspectorMarkdownRow>(&row.value);
    if (!markdown || !markdown->preparedHtml.empty())
      return;
    markdown->preparedHtml =
        presentation::prepareMarkdownHtml(text(markdown->text))
            .toUtf8()
            .toStdString();
  };
  for (ui::InspectorRow &row : snapshot.plan.rows)
    prepare(row);
  if (snapshot.plan.focused)
    prepare(*snapshot.plan.focused);
}

InspectorPane::InspectorPane(QWidget *parent) : QFrame(parent) {
  setObjectName(QStringLiteral("inspector"));
  setMaximumWidth(520);

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(18, 14, 20, 0);
  outer->setSpacing(0);
  auto *heading = new QHBoxLayout;
  heading->addStrut(24);
  auto *sectionTitle = makeLabel(QStringLiteral("INSPECTOR"), "panelHeader");
  sectionTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  heading->addWidget(sectionTitle);
  heading->addStretch();
  auto *hide = new QPushButton(QStringLiteral("Hide"));
  hide->setProperty("kind", "subtle");
  hide->setMinimumSize(58, 24);
  connect(hide, &QPushButton::clicked, this, [this] {
    if (hideAction)
      hideAction();
  });
  heading->addWidget(hide);
  outer->addLayout(heading);
  auto *headerDivider = new QFrame;
  headerDivider->setProperty("kind", "standardDivider");
  headerDivider->setFixedHeight(1);
  outer->addWidget(headerDivider);
  outer->addSpacing(8);

  inspectorTabs = new QTabWidget;
  inspectorTabs->setDocumentMode(true);
  planRows = new RowViewport(this, RowViewport::Page::Plan,
                             QStringLiteral("inspectorPlanRows"),
                             QStringLiteral("Plan"));
  agentsRows = new RowViewport(this, RowViewport::Page::Agents,
                               QStringLiteral("inspectorAgentRows"),
                               QStringLiteral("Agents"));
  requestRows = new RowViewport(this, RowViewport::Page::Requests,
                                QStringLiteral("inspectorRequestRows"),
                                QStringLiteral("Requests"));
  diffViewer = new DiffViewer;

  stateView = new QPlainTextEdit;
  stateView->setObjectName(QStringLiteral("stateInfoView"));
  stateView->setProperty("kind", "infoViewer");
  stateView->setReadOnly(true);
  stateView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  stateView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stateView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  auto *protocolContent = new QWidget;
  auto *protocolLayout = new QVBoxLayout(protocolContent);
  protocolLayout->setContentsMargins(0, 0, 0, 0);
  protocolLayout->setSpacing(6);
  protocolLog = new QPlainTextEdit;
  protocolLog->setObjectName(QStringLiteral("protocolInfoLog"));
  protocolLog->setProperty("kind", "infoViewer");
  protocolLog->setReadOnly(true);
  protocolLog->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  protocolLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  protocolLog->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  protocolLog->document()->setMaximumBlockCount(MaximumProtocolLines);
  connect(protocolLog->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            if (mutatingProtocolLog)
              return;
            QScrollBar *scrollBar = protocolLog->verticalScrollBar();
            protocolFollowsTail = value >= scrollBar->maximum() - 1;
            if (!protocolFollowsTail)
              protocolPausedScrollValue = value;
          });
  protocolStats = makeLabel({}, "meta");
  protocolStats->setObjectName(QStringLiteral("protocolInfoStats"));
  protocolLayout->addWidget(protocolLog, 1);
  protocolLayout->addWidget(protocolStats);

  infoStack = new QStackedWidget;
  infoStack->setObjectName(QStringLiteral("infoStack"));
  auto *choices = new QWidget;
  auto *choicesLayout = new QVBoxLayout(choices);
  choicesLayout->setContentsMargins(8, 8, 8, 8);
  choicesLayout->setSpacing(8);
  auto *stateChoice = infoChoice(QStringLiteral("State"),
                                 QStringLiteral("Current application state"));
  stateChoice->setObjectName(QStringLiteral("stateInfoChoice"));
  auto *protocolChoice =
      infoChoice(QStringLiteral("Protocol"),
                 QStringLiteral("App-server protocol messages"));
  protocolChoice->setObjectName(QStringLiteral("protocolInfoChoice"));
  choicesLayout->addWidget(stateChoice);
  choicesLayout->addWidget(protocolChoice);
  choicesLayout->addStretch();

  QPushButton *stateBack = nullptr;
  QPushButton *protocolBack = nullptr;
  infoStack->addWidget(choices);
  infoStack->addWidget(
      infoDetail(QStringLiteral("State"), stateView, &stateBack));
  infoStack->addWidget(
      infoDetail(QStringLiteral("Protocol"), protocolContent, &protocolBack));
  connect(stateChoice, &QPushButton::clicked, this,
          [this] { infoStack->setCurrentIndex(StatePage); });
  connect(protocolChoice, &QPushButton::clicked, this, [this] {
    infoStack->setCurrentIndex(ProtocolPage);
    showProtocolTail();
  });
  const auto showInfoChoices = [this] {
    infoStack->setCurrentIndex(InfoChoicePage);
  };
  connect(stateBack, &QPushButton::clicked, this, showInfoChoices);
  connect(protocolBack, &QPushButton::clicked, this, showInfoChoices);

  inspectorTabs->addTab(planRows, QStringLiteral("Plan"));
  inspectorTabs->addTab(agentsRows, QStringLiteral("Agents"));
  inspectorTabs->addTab(diffViewer, QStringLiteral("Changes"));
  inspectorTabs->addTab(requestRows, QStringLiteral("Requests"));
  inspectorTabs->addTab(infoStack, QStringLiteral("Info"));
  outer->addWidget(inspectorTabs, 1);

  const auto requestCurrentPage = [this](int) {
    planRows->setActive(false);
    agentsRows->setActive(false);
    requestRows->setActive(false);
    if (refreshRequested)
      refreshRequested();
    else
      refreshCurrentTab();
  };
  connect(inspectorTabs, &QTabWidget::currentChanged, this, requestCurrentPage);
  connect(infoStack, &QStackedWidget::currentChanged, this, requestCurrentPage);
}

void InspectorPane::setHideAction(std::function<void()> hide) {
  hideAction = std::move(hide);
}

void InspectorPane::setRefreshRequestedAction(std::function<void()> refresh) {
  refreshRequested = std::move(refresh);
}

void InspectorPane::setRequestActions(RequestAction review,
                                      RequestAction accept,
                                      RequestAction reject) {
  reviewRequest = std::move(review);
  acceptRequest = std::move(accept);
  rejectRequest = std::move(reject);
}

ui::InspectorRowRequest
InspectorPane::rowRequest(ui::InspectorProjection projection) const {
  switch (projection) {
  case ui::InspectorProjection::Plan:
    return planRows->rowRequest();
  case ui::InspectorProjection::Agents:
    return agentsRows->rowRequest();
  case ui::InspectorProjection::Requests:
    return requestRows->rowRequest();
  case ui::InspectorProjection::Changes:
  case ui::InspectorProjection::State:
  case ui::InspectorProjection::Protocol:
    return {};
  }
  return {};
}

std::optional<ui::InspectorProjection>
InspectorPane::currentProjection() const {
  static constexpr std::array projections{
      ui::InspectorProjection::Plan, ui::InspectorProjection::Agents,
      ui::InspectorProjection::Changes, ui::InspectorProjection::Requests};
  const int tab = inspectorTabs->currentIndex();
  if (tab >= 0 && tab < static_cast<int>(projections.size()))
    return projections[static_cast<std::size_t>(tab)];
  if (tab == 4 && (infoStack->currentIndex() == StatePage ||
                   infoStack->currentIndex() == ProtocolPage))
    return infoStack->currentIndex() == StatePage
               ? ui::InspectorProjection::State
               : ui::InspectorProjection::Protocol;
  return std::nullopt;
}

void InspectorPane::refresh(const ui::InspectorSnapshot &snapshot,
                            ui::InspectorProjection projection) {
  const bool initial = !currentThreadIncarnation;
  const bool threadChanged =
      currentThreadIncarnation &&
      *currentThreadIncarnation != snapshot.threadIncarnation;
  const bool changesChanged =
      projection == ui::InspectorProjection::Changes &&
      (initial || threadChanged || changes != snapshot.changes);
  const bool stateChanged =
      projection == ui::InspectorProjection::State &&
      (initial || threadChanged || state != snapshot.state);
  const bool protocolChanged =
      projection == ui::InspectorProjection::Protocol &&
      (initial || threadChanged || state != snapshot.state);
  if (threadChanged) {
    retireThreadPresentation();
    planRows->apply(projection == ui::InspectorProjection::Plan
                        ? snapshot.plan
                        : ui::InspectorPageSnapshot{},
                    true);
    agentsRows->apply(projection == ui::InspectorProjection::Agents
                          ? snapshot.agents
                          : ui::InspectorPageSnapshot{},
                      true);
    changes = {};
    state = {};
  } else if (projection == ui::InspectorProjection::Plan) {
    planRows->apply(snapshot.plan);
  } else if (projection == ui::InspectorProjection::Agents) {
    if (snapshot.agents.validRetainedKeys)
      std::erase_if(expandedAgentIds, [&](const std::string &key) {
        return std::ranges::find(*snapshot.agents.validRetainedKeys, key) ==
               snapshot.agents.validRetainedKeys->end();
      });
    agentsRows->apply(snapshot.agents);
  }
  currentThreadIncarnation = snapshot.threadIncarnation;
  if (projection == ui::InspectorProjection::Changes)
    changes = snapshot.changes;
  else if (projection == ui::InspectorProjection::Requests)
    requestRows->apply(snapshot.requests);
  else if (projection == ui::InspectorProjection::State ||
           projection == ui::InspectorProjection::Protocol)
    state = snapshot.state;

  if (!isVisible() || currentProjection() != projection)
    return;
  if (projection == ui::InspectorProjection::Plan ||
      projection == ui::InspectorProjection::Agents ||
      projection == ui::InspectorProjection::Requests)
    refreshCurrentTab();
  if (changesChanged)
    refreshChanges();
  else if (stateChanged && infoStack->currentIndex() == StatePage)
    refreshState();
  else if (projection == ui::InspectorProjection::State &&
           infoStack->currentIndex() == ProtocolPage) {
    showProtocolTail();
    refreshProtocolStats();
  } else if (protocolChanged) {
    showProtocolTail();
    refreshProtocolStats();
  }
}

void InspectorPane::retireThreadPresentation() {
  expandedAgentIds.clear();
  diffViewer->setRepositoryContext({}, {}, {}, {});
  stateSnapshot.clear();
  stateView->clear();
}

void InspectorPane::hideEvent(QHideEvent *event) {
  planRows->setActive(false);
  agentsRows->setActive(false);
  requestRows->setActive(false);
  QFrame::hideEvent(event);
}

void InspectorPane::showEvent(QShowEvent *event) {
  QFrame::showEvent(event);
  planRows->setActive(false);
  agentsRows->setActive(false);
  requestRows->setActive(false);
  if (refreshRequested)
    refreshRequested();
  else
    refreshCurrentTab();
}

void InspectorPane::refreshCurrentTab() {
  const int current = inspectorTabs->currentIndex();
  const bool visible = isVisible();
  planRows->setActive(visible && current == 0);
  agentsRows->setActive(visible && current == 1);
  requestRows->setActive(visible && current == 3);
  const std::optional projection = currentProjection();
  if (!currentThreadIncarnation || !projection)
    return;
  if (*projection == ui::InspectorProjection::Changes) {
    refreshChanges();
  } else if (*projection == ui::InspectorProjection::State) {
    if (infoStack->currentIndex() == StatePage)
      refreshState();
    else if (infoStack->currentIndex() == ProtocolPage) {
      showProtocolTail();
      refreshProtocolStats();
    }
  } else if (*projection == ui::InspectorProjection::Protocol) {
    showProtocolTail();
    refreshProtocolStats();
  }
}

void InspectorPane::refreshChanges() {
  diffViewer->setRepositoryContext(text(changes.threadId), text(changes.cwd),
                                   texts(changes.commandCwds),
                                   texts(changes.changedPaths));
  diffViewer->refreshRepository();
}

void InspectorPane::refreshState() {
  std::string rendered = state.state.dump(2);
  constexpr std::size_t MaximumBytes = 32U * 1024U;
  if (rendered.size() > MaximumBytes) {
    const std::size_t total = rendered.size();
    rendered.resize(MaximumBytes);
    rendered += "\n\n[State display truncated at 32 KiB; retained bytes: " +
                std::to_string(total) + "]";
  }
  const QByteArray next(rendered.data(),
                        static_cast<qsizetype>(rendered.size()));
  if (next == stateSnapshot)
    return;
  stateSnapshot = next;
  stateView->setPlainText(text(rendered));
}

void InspectorPane::refreshProtocolStats() {
  const ui::InspectorStateSnapshot &snapshot = state;
  const QString value =
      QStringLiteral("seq %1  |  threads %2  |  models %3  |  turns %4  |  "
                     "items %5  |  pending %6  |  telemetry %7")
          .arg(static_cast<qulonglong>(observedSequence))
          .arg(static_cast<qulonglong>(snapshot.threadCount))
          .arg(static_cast<qulonglong>(snapshot.modelCount))
          .arg(static_cast<qulonglong>(snapshot.selectedThreadTurnCount))
          .arg(static_cast<qulonglong>(snapshot.selectedThreadItemCount))
          .arg(static_cast<qulonglong>(snapshot.pendingRequestCount))
          .arg(static_cast<qulonglong>(
              std::max(snapshot.telemetryCount, protocolTelemetryCount)));
  if (value.toUtf8() == protocolStatsSnapshot)
    return;
  protocolStatsSnapshot = value.toUtf8();
  protocolStats->setText(value);
}

void InspectorPane::showProtocolTail() {
  QStringList lines;
  lines.reserve(static_cast<qsizetype>(protocolLines.size()));
  for (const QString &line : protocolLines)
    lines << line;
  const QString value = lines.join(QLatin1Char('\n'));
  if (protocolLog->toPlainText() == value) {
    pendingProtocolLines.clear();
    return;
  }
  const ScrollPosition position{protocolFollowsTail, protocolPausedScrollValue};
  mutatingProtocolLog = true;
  protocolLog->setPlainText(value);
  pendingProtocolLines.clear();
  restoreProtocolScroll(position.followsTail, position.value);
}

void InspectorPane::flushProtocolPresentation() {
  if (pendingProtocolLines.empty())
    return;
  const bool visibleProtocol = isVisible() &&
                               inspectorTabs->currentIndex() == 4 &&
                               infoStack->currentIndex() == ProtocolPage;
  if (!visibleProtocol) {
    pendingProtocolLines.clear();
    return;
  }
  QStringList lines;
  lines.reserve(static_cast<qsizetype>(pendingProtocolLines.size()));
  for (QString &line : pendingProtocolLines)
    lines.push_back(std::move(line));
  pendingProtocolLines.clear();
  const ScrollPosition position{protocolFollowsTail, protocolPausedScrollValue};
  mutatingProtocolLog = true;
  protocolLog->appendPlainText(lines.join(QLatin1Char('\n')));
  restoreProtocolScroll(position.followsTail, position.value);
  refreshProtocolStats();
}

bool InspectorPane::retainsTarget(
    const nodegraph::NodeRef &target) const noexcept {
  return requestRows && requestRows->retainsTarget(target);
}

void InspectorPane::restoreProtocolScroll(bool followsTail, int pausedValue) {
  const ScrollPosition position{followsTail, pausedValue};
  restoreScrollPosition(protocolLog, position);
  const std::uint64_t revision = ++protocolScrollRevision;
  QTimer::singleShot(0, this, [this, position, revision] {
    if (revision != protocolScrollRevision)
      return;
    restoreScrollPosition(protocolLog, position);
    protocolFollowsTail = position.followsTail;
    if (!position.followsTail)
      protocolPausedScrollValue = protocolLog->verticalScrollBar()->value();
    mutatingProtocolLog = false;
  });
}

void InspectorPane::appendProtocolFrame(const nlohmann::json &frame) {
  nodegraph::ProtocolDiagnostic diagnostic;
  const auto copyUnsigned = [&frame, &diagnostic](const char *from,
                                                  const char *to) {
    const auto found = frame.find(from);
    if (found != frame.end() && found->is_number_unsigned())
      diagnostic.details.emplace(to,
                                 nodegraph::Value(found->get<std::uint64_t>()));
  };
  const auto copyString = [&frame, &diagnostic](const char *from,
                                                const char *to) {
    const auto found = frame.find(from);
    if (found != frame.end() && found->is_string())
      diagnostic.details.emplace(to,
                                 nodegraph::Value(found->get<std::string>()));
  };
  copyUnsigned("sequence", "sequence");
  copyUnsigned("generation", "connectionGeneration");
  copyString("kind", "direction");
  copyString(frame.value("kind", std::string{}) == "result" ? "action" : "type",
             "subject");
  copyString("authority", "authority");
  copyString("correlationId", "correlation");
  const auto scope = frame.find("scope");
  if (scope != frame.end() && scope->is_object()) {
    for (const char *key :
         {"threadId", "turnId", "itemId", "requestId", "processId"}) {
      const auto found = scope->find(key);
      if (found != scope->end() && found->is_string())
        diagnostic.details.emplace(
            key, nodegraph::Value(found->get<std::string>()));
    }
  }
  if (frame.value("kind", std::string{}) == "result") {
    diagnostic.details.emplace(
        "outcome", nodegraph::Value(frame.value("ok", false) ? "ok" : "ERROR"));
    const auto error = frame.find("error");
    if (error != frame.end() && error->is_object()) {
      const auto message = error->find("message");
      if (message != error->end() && message->is_string())
        diagnostic.details.emplace(
            "error", nodegraph::Value(message->get<std::string>()));
    }
  }
  static_cast<void>(appendProtocolDiagnostic(diagnostic));
}

bool InspectorPane::appendProtocolDiagnostic(
    const nodegraph::ProtocolDiagnostic &diagnostic) {
  if (!diagnostic.diagnosticBatch.empty()) {
    bool visible = false;
    for (const nodegraph::Value::Object &details :
         diagnostic.diagnosticBatch) {
      nodegraph::ProtocolDiagnostic entry;
      entry.details = details;
      visible = appendProtocolDiagnostic(entry) || visible;
    }
    return visible;
  }
  const QString timestamp =
      QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
  std::vector<QString> recorded;
  const auto record = [this, &recorded](QString line) {
    while (protocolLines.size() >= MaximumProtocolLines)
      protocolLines.pop_front();
    protocolLines.push_back(line);
    recorded.push_back(std::move(line));
  };
  const std::optional<std::uint64_t> sequence =
      unsignedIntegerFromValue(valueMember(diagnostic.details, "sequence"));
  if (sequence && *sequence != 0) {
    if (observedSequence != 0 && *sequence != observedSequence + 1) {
      record(QStringLiteral("[%1]  %2  expected=%3  received=%4")
                 .arg(timestamp, *sequence <= observedSequence
                                     ? QStringLiteral("NON-MONOTONIC")
                                     : QStringLiteral("SEQUENCE GAP"))
                 .arg(observedSequence + 1)
                 .arg(*sequence));
    }
    observedSequence = std::max(observedSequence, *sequence);
  }
  if (const auto dropped = unsignedIntegerFromValue(
          valueMember(diagnostic.details, "droppedBefore"));
      dropped && *dropped != 0)
    record(QStringLiteral("[%1]  DROPPED %2 DIAGNOSTICS before #%3")
               .arg(timestamp)
               .arg(*dropped)
               .arg(sequence.value_or(0)));

  QStringList parts{QStringLiteral("[%1]").arg(timestamp)};
  if (sequence && *sequence != 0)
    parts << QStringLiteral("#%1").arg(*sequence);
  if (const auto connection = unsignedIntegerFromValue(
          valueMember(diagnostic.details, "connectionGeneration")))
    parts << QStringLiteral("g%1").arg(*connection);
  if (const auto provider = unsignedIntegerFromValue(
          valueMember(diagnostic.details, "providerGeneration")))
    parts << QStringLiteral("p%1").arg(*provider);
  for (std::string_view key : {"direction", "subject", "source"}) {
    const QString value =
        protocolMetadata(valueMember(diagnostic.details, key));
    if (!value.isEmpty())
      parts << value;
  }
  for (std::string_view key :
       {"authority", "outcome", "threadId", "turnId", "itemId", "requestId",
        "processId", "connectionId", "targetId", "role", "state", "event",
        "correlation", "error", "errorCategory", "errorCode"}) {
    const QString value =
        protocolMetadata(valueMember(diagnostic.details, key));
    if (!value.isEmpty())
      parts << QStringLiteral("%1=%2").arg(text(key), value);
  }
  record(parts.join(QStringLiteral("  ")));

  const std::string direction =
      scalarTextFromValue(valueMember(diagnostic.details, "direction"));
  const std::string authority =
      scalarTextFromValue(valueMember(diagnostic.details, "authority"));
  if (authority == "none" &&
      (direction.find("notification") != std::string::npos ||
       direction.find("event") != std::string::npos ||
       direction.ends_with("frame")))
    protocolTelemetryCount =
        std::min<std::size_t>(256, protocolTelemetryCount + 1);

  for (QString &line : recorded) {
    while (pendingProtocolLines.size() >= MaximumProtocolLines)
      pendingProtocolLines.pop_front();
    pendingProtocolLines.push_back(std::move(line));
  }
  return isVisible() && inspectorTabs->currentIndex() == 4 &&
         infoStack->currentIndex() == ProtocolPage;
}

} // namespace codexui::codex::middle
