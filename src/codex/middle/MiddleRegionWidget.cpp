// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/MiddleRegionWidget.h"

#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr auto ShowReasoningKey = "conversation/showReasoning";
constexpr auto ShowCodexUpdatesKey = "conversation/showCodexUpdates";
constexpr auto CommandsInitiallyExpandedKey =
    "conversation/commandsInitiallyExpanded";
constexpr auto ImagesInitiallyExpandedKey =
    "conversation/imagesInitiallyExpanded";
constexpr auto FileChangesInitiallyExpandedKey =
    "conversation/fileChangesInitiallyExpanded";

enum class PresentationIcon { Reasoning, Updates, Command, FileChanges, Image };

QPixmap presentationIconPixmap(PresentationIcon kind, QColor color) {
  QPixmap pixmap(16, 16);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(
      QPen(std::move(color), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

  if (kind == PresentationIcon::Reasoning) {
    painter.drawEllipse(QRectF(4.0, 2.0, 8.0, 8.0));
    painter.drawLine(QPointF(5.5, 9.0), QPointF(6.5, 11.0));
    painter.drawLine(QPointF(10.5, 9.0), QPointF(9.5, 11.0));
    painter.drawLine(QPointF(6.5, 11.0), QPointF(9.5, 11.0));
    painter.drawLine(QPointF(7.0, 13.0), QPointF(9.0, 13.0));
  } else if (kind == PresentationIcon::Updates) {
    QPainterPath bubble;
    bubble.addRoundedRect(QRectF(2.0, 2.5, 12.0, 9.0), 2.5, 2.5);
    bubble.moveTo(5.0, 11.0);
    bubble.lineTo(4.0, 14.0);
    bubble.lineTo(8.0, 11.5);
    painter.drawPath(bubble);
    painter.drawPoint(QPointF(5.5, 7.0));
    painter.drawPoint(QPointF(8.0, 7.0));
    painter.drawPoint(QPointF(10.5, 7.0));
  } else if (kind == PresentationIcon::Command) {
    painter.drawRoundedRect(QRectF(1.5, 2.5, 13.0, 11.0), 2.0, 2.0);
    painter.drawLine(QPointF(4.5, 6.0), QPointF(6.5, 8.0));
    painter.drawLine(QPointF(6.5, 8.0), QPointF(4.5, 10.0));
    painter.drawLine(QPointF(8.5, 10.0), QPointF(11.5, 10.0));
  } else if (kind == PresentationIcon::FileChanges) {
    QPainterPath document;
    document.moveTo(3.0, 1.5);
    document.lineTo(9.5, 1.5);
    document.lineTo(13.0, 5.0);
    document.lineTo(13.0, 14.0);
    document.lineTo(3.0, 14.0);
    document.closeSubpath();
    document.moveTo(9.5, 1.5);
    document.lineTo(9.5, 5.0);
    document.lineTo(13.0, 5.0);
    painter.drawPath(document);
    painter.drawLine(QPointF(5.0, 8.0), QPointF(8.0, 8.0));
    painter.drawLine(QPointF(6.5, 6.5), QPointF(6.5, 9.5));
    painter.drawLine(QPointF(9.5, 11.0), QPointF(11.5, 11.0));
  } else {
    painter.drawRoundedRect(QRectF(1.5, 2.5, 13.0, 11.0), 2.0, 2.0);
    painter.drawEllipse(QRectF(9.5, 4.5, 2.0, 2.0));
    QPainterPath landscape;
    landscape.moveTo(3.5, 11.0);
    landscape.lineTo(6.5, 7.5);
    landscape.lineTo(8.5, 9.5);
    landscape.lineTo(10.0, 8.0);
    landscape.lineTo(12.5, 11.0);
    painter.drawPath(landscape);
  }
  return pixmap;
}

QIcon presentationIcon(PresentationIcon kind) {
  QIcon icon;
  icon.addPixmap(presentationIconPixmap(
                     kind, QColor(QString::fromLatin1(UiStyle::secondary))),
                 QIcon::Normal, QIcon::Off);
  icon.addPixmap(presentationIconPixmap(
                     kind, QColor(QString::fromLatin1(UiStyle::primary))),
                 QIcon::Normal, QIcon::On);
  return icon;
}

using UiStyle::makeLabel;

QFrame *divider(const char *name = nullptr) {
  auto *line = new QFrame;
  if (name)
    line->setObjectName(QString::fromLatin1(name));
  line->setProperty("kind", "standardDivider");
  line->setFixedHeight(1);
  return line;
}

int verticalIntent(const QWheelEvent *event) {
  if (!event->pixelDelta().isNull())
    return event->pixelDelta().y();
  return event->angleDelta().y();
}

bool canConsume(const QAbstractScrollArea *area, int delta) {
  if (!area)
    return false;
  const QScrollBar *bar = area->verticalScrollBar();
  if (!bar || bar->maximum() <= bar->minimum())
    return false;
  if (delta > 0)
    return bar->value() > bar->minimum();
  if (delta < 0)
    return bar->value() < bar->maximum();
  return false;
}

} // namespace

MiddleRegionWidget::MiddleRegionWidget(QWidget *parent) : QWidget(parent) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  splitter = new QSplitter(Qt::Horizontal);
  splitter->setObjectName(QStringLiteral("workspaceSplitter"));
  splitter->setChildrenCollapsible(false);
  splitter->setHandleWidth(8);

  threadPane = new ThreadPane;
  splitter->addWidget(threadPane);

  conversationRegion = new QFrame;
  conversationRegion->setObjectName(QStringLiteral("conversation"));
  auto *center = new QVBoxLayout(conversationRegion);
  center->setContentsMargins(10, 14, 10, 12);
  center->setSpacing(0);
  auto *context = new QHBoxLayout;
  context->setContentsMargins(14, 0, 14, 0);
  context->addStrut(24);
  auto *sectionTitle = makeLabel(QStringLiteral("CONVERSATION"), "panelHeader");
  sectionTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  context->addWidget(sectionTitle);
  context->addStretch();
  const QSettings settings;
  const auto presentationToggle = [&context](QString objectName,
                                             PresentationIcon icon) {
    auto *button = new QToolButton;
    button->setObjectName(std::move(objectName));
    button->setProperty("kind", "presentationToggle");
    button->setCheckable(true);
    button->setIcon(presentationIcon(icon));
    button->setIconSize(QSize(16, 16));
    button->setFixedSize(28, 24);
    context->addWidget(button);
    return button;
  };
  reasoningVisibility =
      presentationToggle(QStringLiteral("conversationReasoningToggle"),
                         PresentationIcon::Reasoning);
  reasoningVisibility->setChecked(
      settings.value(ShowReasoningKey, false).toBool());
  updateVisibility = presentationToggle(
      QStringLiteral("conversationUpdatesToggle"), PresentationIcon::Updates);
  updateVisibility->setChecked(
      settings.value(ShowCodexUpdatesKey, true).toBool());
  commandInitialFolding =
      presentationToggle(QStringLiteral("conversationCommandFoldingToggle"),
                         PresentationIcon::Command);
  commandInitialFolding->setChecked(
      settings.value(CommandsInitiallyExpandedKey, true).toBool());
  imageInitialFolding =
      presentationToggle(QStringLiteral("conversationImageFoldingToggle"),
                         PresentationIcon::Image);
  imageInitialFolding->setChecked(
      settings.value(ImagesInitiallyExpandedKey, true).toBool());
  fileChangesInitialFolding =
      presentationToggle(QStringLiteral("conversationFileChangesFoldingToggle"),
                         PresentationIcon::FileChanges);
  fileChangesInitialFolding->setChecked(
      settings.value(FileChangesInitiallyExpandedKey, false).toBool());
  const auto persistPresentation = [this](const char *key, bool checked) {
    QSettings().setValue(QString::fromLatin1(key), checked);
    applyConversationPresentationOptions();
  };
  connect(reasoningVisibility, &QToolButton::toggled, this,
          [persistPresentation](bool checked) {
            persistPresentation(ShowReasoningKey, checked);
          });
  connect(updateVisibility, &QToolButton::toggled, this,
          [persistPresentation](bool checked) {
            persistPresentation(ShowCodexUpdatesKey, checked);
          });
  connect(commandInitialFolding, &QToolButton::toggled, this,
          [persistPresentation](bool checked) {
            persistPresentation(CommandsInitiallyExpandedKey, checked);
          });
  connect(imageInitialFolding, &QToolButton::toggled, this,
          [persistPresentation](bool checked) {
            persistPresentation(ImagesInitiallyExpandedKey, checked);
          });
  connect(fileChangesInitialFolding, &QToolButton::toggled, this,
          [persistPresentation](bool checked) {
            persistPresentation(FileChangesInitiallyExpandedKey, checked);
          });
  center->addLayout(context);
  center->addWidget(divider("conversationHeaderDivider"));
  center->addSpacing(8);

  auto *content = new QWidget;
  auto *contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(14, 0, 14, 0);
  contentLayout->setSpacing(0);
  auto *threadHeading = new QHBoxLayout;
  threadHeading->setSpacing(10);
  conversationTitle =
      makeLabel(QStringLiteral("No synchronized thread"), "heading");
  conversationTitle->setObjectName(QStringLiteral("conversationTitle"));
  conversationTitle->setWordWrap(false);
  conversationTitle->setSizePolicy(QSizePolicy::Minimum,
                                   QSizePolicy::Preferred);
  conversationMetadata = makeLabel({}, "meta");
  conversationMetadata->setObjectName(QStringLiteral("conversationMetadata"));
  conversationMetadata->setWordWrap(false);
  conversationTrailingMetadata = makeLabel({}, "meta");
  conversationTrailingMetadata->setObjectName(
      QStringLiteral("conversationTrailingMetadata"));
  conversationTrailingMetadata->setProperty("tone", "strong");
  conversationTrailingMetadata->setWordWrap(false);
  conversationTrailingMetadata->setSizePolicy(QSizePolicy::Minimum,
                                              QSizePolicy::Preferred);
  conversationStateSeparator = makeLabel(QStringLiteral("|"), "meta");
  conversationStateSeparator->setProperty("tone", "strong");
  conversationStateSeparator->setSizePolicy(QSizePolicy::Minimum,
                                            QSizePolicy::Preferred);
  conversationState = makeLabel({}, "meta");
  conversationState->setObjectName(QStringLiteral("conversationState"));
  conversationState->setSizePolicy(QSizePolicy::Minimum,
                                   QSizePolicy::Preferred);
  conversationTitle->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  conversationMetadata->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  conversationTrailingMetadata->setAlignment(Qt::AlignRight | Qt::AlignTop);
  conversationState->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  alignThreadHeadingBaselines();
  auto *trailingHeading = new QHBoxLayout;
  trailingHeading->setSpacing(4);
  trailingHeading->addWidget(conversationTrailingMetadata, 0, Qt::AlignTop);
  trailingHeading->addWidget(conversationStateSeparator, 0, Qt::AlignTop);
  trailingHeading->addWidget(conversationState, 0, Qt::AlignTop);
  threadHeading->addWidget(conversationTitle, 0, Qt::AlignTop);
  threadHeading->addWidget(conversationMetadata, 1, Qt::AlignTop);
  threadHeading->addLayout(trailingHeading);
  contentLayout->addLayout(threadHeading);
  contentLayout->addSpacing(7);
  contentLayout->addWidget(divider());
  contentLayout->addSpacing(7);

  noticeBar = new QFrame;
  noticeBar->setObjectName(QStringLiteral("conversationNoticeBar"));
  noticeBar->setProperty("tone", "danger");
  auto *noticeLayout = new QHBoxLayout(noticeBar);
  noticeLayout->setContentsMargins(10, 6, 8, 6);
  noticeLabel = makeLabel({}, "meta");
  noticeLabel->setProperty("tone", "danger");
  auto *dismiss = new QPushButton(QStringLiteral("Dismiss"));
  dismiss->setProperty("kind", "subtle");
  dismiss->setFixedHeight(28);
  noticeLayout->addWidget(noticeLabel, 1);
  noticeLayout->addWidget(dismiss);
  noticeBar->hide();
  noticeTimer = new QTimer(noticeBar);
  noticeTimer->setObjectName(QStringLiteral("conversationNoticeTimer"));
  noticeTimer->setSingleShot(true);
  connect(noticeTimer, &QTimer::timeout, noticeBar, &QWidget::hide);
  connect(dismiss, &QPushButton::clicked, noticeBar, [this] {
    noticeTimer->stop();
    noticeBar->hide();
  });
  auto *conversationLayer = new QWidget(content);
  conversationLayer->setObjectName(QStringLiteral("conversationLayer"));
  auto *conversationLayerLayout = new QGridLayout(conversationLayer);
  conversationLayerLayout->setContentsMargins(0, 0, 0, 0);
  conversationLayerLayout->setSpacing(0);
  conversationView = new ConversationView(conversationLayer);
  conversationView->setNoticeAction(
      [this](QString message, bool error) {
        showNotice(std::move(message), error);
      });
  applyConversationPresentationOptions();
  conversationLayerLayout->addWidget(conversationView, 0, 0);
  conversationLayerLayout->addWidget(noticeBar, 0, 0, Qt::AlignTop);
  contentLayout->addWidget(conversationLayer, 1);
  composerPane = new ComposerPane(content);
  contentLayout->addWidget(composerPane);
  contentLayout->addSpacing(12);
  center->addWidget(content, 1);
  splitter->addWidget(conversationRegion);

  inspectorPane = new InspectorPane;
  splitter->addWidget(inspectorPane);
  splitter->handle(1)->installEventFilter(this);
  splitter->handle(2)->installEventFilter(this);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setStretchFactor(2, 0);
  splitter->setSizes({282, 834, 404});
  root->addWidget(splitter);

  inspectorPane->setHideAction([this] { showInspector(false); });
}

void MiddleRegionWidget::applyConversationPresentationOptions() {
  const bool showReasoning = reasoningVisibility->isChecked();
  const bool showUpdates = updateVisibility->isChecked();
  const bool expandCommands = commandInitialFolding->isChecked();
  const bool expandImages = imageInitialFolding->isChecked();
  const bool expandFileChanges = fileChangesInitialFolding->isChecked();
  const auto describe = [](QToolButton &button, QString value) {
    if (button.toolTip() != value)
      button.setToolTip(value);
    presentation::setAccessibleNameIfChanged(button, std::move(value));
  };
  describe(*reasoningVisibility, showReasoning
                                            ? QStringLiteral("Hide reasoning cards")
                                            : QStringLiteral("Show reasoning cards"));
  describe(*updateVisibility, showUpdates
                                         ? QStringLiteral("Hide Codex update cards")
                                         : QStringLiteral("Show Codex update cards"));
  describe(*commandInitialFolding,
           expandCommands ? QStringLiteral("New command cards start expanded")
                          : QStringLiteral("New command cards start collapsed"));
  describe(*imageInitialFolding,
           expandImages ? QStringLiteral("New image cards start expanded")
                        : QStringLiteral("New image cards start collapsed"));
  describe(*fileChangesInitialFolding,
           expandFileChanges
               ? QStringLiteral("New file changes cards start expanded")
               : QStringLiteral("New file changes cards start collapsed"));
  if (conversationView)
    conversationView->setPresentationOptions({showReasoning, showUpdates,
                                              expandCommands, expandImages,
                                              expandFileChanges});
}

ThreadPane &MiddleRegionWidget::threads() const noexcept { return *threadPane; }

ConversationView &MiddleRegionWidget::conversation() const noexcept {
  return *conversationView;
}

ComposerPane &MiddleRegionWidget::composer() const noexcept {
  return *composerPane;
}

InspectorPane &MiddleRegionWidget::inspector() const noexcept {
  return *inspectorPane;
}

QSplitter *MiddleRegionWidget::splitterWidget() const noexcept {
  return splitter;
}

bool MiddleRegionWidget::eventFilter(QObject *watched, QEvent *event) {
  const bool splitterHandle =
      watched == splitter->handle(1) || watched == splitter->handle(2);
  if (splitterHandle && event) {
    if (event->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
      conversationView->beginInteractiveResize();
    } else if (event->type() == QEvent::MouseButtonRelease ||
               event->type() == QEvent::UngrabMouse ||
               event->type() == QEvent::Hide) {
      conversationView->endInteractiveResize();
    }
  }
  return QWidget::eventFilter(watched, event);
}

void MiddleRegionWidget::setThreadHeading(QString title, QString metadata,
                                          QString trailingMetadata,
                                          QString state, QString stateTone) {
  const bool separatorVisible = !state.isEmpty();
  if (conversationTitle->text() == title &&
      conversationMetadata->text() == metadata &&
      conversationTrailingMetadata->text() == trailingMetadata &&
      conversationState->text() == state &&
      conversationState->property("tone").toString() == stateTone &&
      conversationStateSeparator->isVisible() == separatorVisible)
    return;
  if (conversationTitle->text() != title)
    conversationTitle->setText(std::move(title));
  if (conversationMetadata->text() != metadata)
    conversationMetadata->setText(std::move(metadata));
  if (conversationTrailingMetadata->text() != trailingMetadata)
    conversationTrailingMetadata->setText(std::move(trailingMetadata));
  if (conversationState->text() != state)
    conversationState->setText(std::move(state));
  conversationStateSeparator->setVisible(separatorVisible);
  if (conversationState->property("tone").toString() != stateTone) {
    conversationState->setProperty("tone", std::move(stateTone));
    conversationState->style()->unpolish(conversationState);
    conversationState->style()->polish(conversationState);
  }
  alignThreadHeadingBaselines();
}

void MiddleRegionWidget::alignThreadHeadingBaselines() {
  conversationTitle->ensurePolished();
  conversationMetadata->ensurePolished();
  conversationTrailingMetadata->ensurePolished();
  conversationStateSeparator->ensurePolished();
  conversationState->ensurePolished();
  const int offset =
      std::max(0, conversationTitle->fontMetrics().ascent() -
                      conversationMetadata->fontMetrics().ascent());
  conversationMetadata->setContentsMargins(0, offset, 0, 0);
  const int trailingOffset =
      std::max(0, conversationTitle->fontMetrics().ascent() -
                      conversationTrailingMetadata->fontMetrics().ascent());
  conversationTrailingMetadata->setContentsMargins(0, trailingOffset, 0, 0);
  conversationStateSeparator->setContentsMargins(0, trailingOffset, 0, 0);
  conversationState->setContentsMargins(0, trailingOffset, 0, 0);
}

void MiddleRegionWidget::showNotice(QString message, bool error) {
  if (message.trimmed().isEmpty())
    return;
  const QString announcement = message;
  noticeLabel->setText(std::move(message));
  const QString tone =
      error ? QStringLiteral("danger") : QStringLiteral("warning");
  const std::array<QWidget *, 2> tonedWidgets{noticeBar, noticeLabel};
  for (QWidget *widget : tonedWidgets) {
    widget->setProperty("tone", tone);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
  }
  noticeBar->show();
  noticeBar->raise();
  presentation::announce(*noticeBar, announcement);
  noticeTimer->start(error ? 10000 : 6000);
}

void MiddleRegionWidget::showSidebar(bool visible) {
  if (threadPane->isVisible() == visible)
    return;
  threadPane->setVisible(visible);
  if (paneVisibilityAction)
    paneVisibilityAction(sidebarVisible(), inspectorVisible());
}

void MiddleRegionWidget::showInspector(bool visible) {
  if (inspectorPane->isVisible() == visible)
    return;
  inspectorPane->setVisible(visible);
  if (paneVisibilityAction)
    paneVisibilityAction(sidebarVisible(), inspectorVisible());
}

bool MiddleRegionWidget::sidebarVisible() const noexcept {
  return threadPane->isVisible();
}

bool MiddleRegionWidget::inspectorVisible() const noexcept {
  return inspectorPane->isVisible();
}

void MiddleRegionWidget::setPaneVisibilityAction(
    std::function<void(bool, bool)> action) {
  paneVisibilityAction = std::move(action);
}

bool MiddleRegionWidget::routeScrollEvent(QObject *watched, QEvent *event) {
  if (!event || event->type() != QEvent::Wheel)
    return false;
  auto *target = qobject_cast<QWidget *>(watched);
  if (!target)
    return false;
  if (scrollGestureRoute_ == ScrollGestureRoute::Dispatching) {
    QAbstractScrollArea *const owner = scrollGestureArea_.data();
    const bool withinOwner =
        owner && (target == owner || owner->isAncestorOf(target));
    if (!withinOwner)
      event->accept();
    return !withinOwner;
  }
  auto *wheel = static_cast<QWheelEvent *>(event);
  if (wheel->phase() == Qt::ScrollBegin) {
    scrollGestureRoute_ = ScrollGestureRoute::Inactive;
    scrollGestureArea_.clear();
  }
  const bool inCenter =
      target == conversationRegion || conversationRegion->isAncestorOf(target);
  const bool onHandle =
      target == splitter->handle(1) || target == splitter->handle(2);
  const bool continuing = wheel->phase() != Qt::NoScrollPhase &&
                          scrollGestureRoute_ != ScrollGestureRoute::Inactive;
  if (!inCenter && !onHandle && !continuing)
    return false;

  const bool inComposer = inCenter && (target == composerPane ||
                                       composerPane->isAncestorOf(target));
  const bool inConversation =
      inCenter &&
      (target == conversationView || target == conversationView->viewport() ||
       conversationView->isAncestorOf(target));
  const auto scrollAreaBefore = [](QWidget *from, QWidget *stop) {
    for (QWidget *ancestor = from; ancestor && ancestor != stop;
         ancestor = ancestor->parentWidget())
      if (auto *area = qobject_cast<QAbstractScrollArea *>(ancestor))
        return area;
    return static_cast<QAbstractScrollArea *>(nullptr);
  };
  QAbstractScrollArea *const currentArea =
      inComposer       ? scrollAreaBefore(target, composerPane)
      : inConversation ? scrollAreaBefore(target, conversationView)
                       : nullptr;
  const auto dispatch = [this, wheel](QAbstractScrollArea *owner) {
    wheel->accept();
    if (!owner || !owner->viewport())
      return true;
    const QScopedValueRollback route(scrollGestureRoute_,
                                     ScrollGestureRoute::Dispatching);
    const QScopedValueRollback area(scrollGestureArea_,
                                    QPointer<QAbstractScrollArea>(owner));
    QWidget *const receiver = owner->viewport();
    const QPointF local = receiver->mapFromGlobal(wheel->globalPosition());
    QWheelEvent forwarded(local, wheel->globalPosition(), wheel->pixelDelta(),
                          wheel->angleDelta(), wheel->buttons(),
                          wheel->modifiers(), wheel->phase(), wheel->inverted(),
                          wheel->source(), wheel->pointingDevice());
    forwarded.setTimestamp(wheel->timestamp());
    static_cast<void>(QApplication::sendEvent(receiver, &forwarded));
    return true;
  };

  const int intent = verticalIntent(wheel);
  if (wheel->phase() == Qt::NoScrollPhase) {
    if (inComposer) {
      if (currentArea)
        return dispatch(currentArea);
      event->accept();
      return true;
    }
    if (currentArea && canConsume(currentArea, intent))
      return dispatch(currentArea);
    return dispatch(conversationView);
  }

  if (wheel->phase() == Qt::ScrollBegin ||
      scrollGestureRoute_ == ScrollGestureRoute::Inactive) {
    scrollGestureArea_ = currentArea;
    if (inComposer) {
      scrollGestureRoute_ = ScrollGestureRoute::Native;
    } else if (currentArea) {
      scrollGestureRoute_ = ScrollGestureRoute::AwaitingNested;
    } else {
      scrollGestureArea_ = conversationView;
      scrollGestureRoute_ = ScrollGestureRoute::Conversation;
    }
  }

  if (intent != 0 &&
      scrollGestureRoute_ == ScrollGestureRoute::AwaitingNested) {
    if (!scrollGestureArea_) {
      scrollGestureRoute_ = ScrollGestureRoute::Native;
    } else if (canConsume(scrollGestureArea_, intent)) {
      scrollGestureRoute_ = ScrollGestureRoute::Native;
    } else {
      scrollGestureArea_ = conversationView;
      scrollGestureRoute_ = ScrollGestureRoute::Conversation;
    }
  }

  switch (scrollGestureRoute_) {
  case ScrollGestureRoute::AwaitingNested:
    event->accept();
    break;
  case ScrollGestureRoute::Native:
  case ScrollGestureRoute::Conversation:
    static_cast<void>(dispatch(scrollGestureArea_));
    break;
  case ScrollGestureRoute::Dispatching:
  case ScrollGestureRoute::Inactive:
    event->accept();
    break;
  }

  if (wheel->phase() == Qt::ScrollEnd) {
    scrollGestureRoute_ = ScrollGestureRoute::Inactive;
    scrollGestureArea_.clear();
  }
  return true;
}

} // namespace codexui::codex::middle
