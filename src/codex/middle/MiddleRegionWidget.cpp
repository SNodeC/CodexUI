// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/MiddleRegionWidget.h"

#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/ThreadPane.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <array>
#include <utility>

namespace codexui::codex::middle {
namespace {

QLabel *makeLabel(QString value, const char *kind = "body") {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

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
  splitter->setChildrenCollapsible(false);
  splitter->setHandleWidth(8);

  threadPane = new ThreadPane;
  splitter->addWidget(threadPane);

  conversationRegion = new QFrame;
  conversationRegion->setObjectName(QStringLiteral("conversation"));
  conversationRegion->setStyleSheet(
      QStringLiteral("QFrame#conversation{background:#f6f8fb;}"));
  conversationRegion->setMinimumWidth(480);
  auto *center = new QVBoxLayout(conversationRegion);
  center->setContentsMargins(10, 14, 10, 12);
  center->setSpacing(0);
  auto *context = new QHBoxLayout;
  context->setContentsMargins(14, 0, 14, 0);
  context->addStrut(24);
  auto *sectionTitle =
      makeLabel(QStringLiteral("CONVERSATION"), "panelHeader");
  sectionTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  sectionTitle->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  context->addWidget(sectionTitle);
  context->addStretch();
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
  conversationMetadata->setObjectName(
      QStringLiteral("conversationMetadata"));
  conversationMetadata->setWordWrap(false);
  threadHeading->addWidget(conversationTitle, 0, Qt::AlignBaseline);
  threadHeading->addWidget(conversationMetadata, 1, Qt::AlignBaseline);
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
  contentLayout->addWidget(noticeBar);

  conversationView = new ConversationView;
  contentLayout->addWidget(conversationView, 1);
  composerPane = new ComposerPane(conversationRegion);
  composerPane->setExtraOverlayHeightAction(
      [this](int height) { conversationView->setTrailingSpaceHeight(height); });
  contentLayout->addWidget(composerPane->canonicalReserve());
  center->addWidget(content, 1);
  splitter->addWidget(conversationRegion);

  inspectorPane = new InspectorPane;
  splitter->addWidget(inspectorPane);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setStretchFactor(2, 0);
  splitter->setSizes({282, 834, 404});
  root->addWidget(splitter);

  inspectorPane->setHideAction([this] { showInspector(false); });
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

void MiddleRegionWidget::setThreadHeading(QString title, QString metadata) {
  if (conversationTitle->text() != title)
    conversationTitle->setText(std::move(title));
  if (conversationMetadata->text() != metadata)
    conversationMetadata->setText(std::move(metadata));
}

void MiddleRegionWidget::showNotice(QString message, bool error) {
  if (message.trimmed().isEmpty())
    return;
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
  // Native delivery from ConversationView to its scrollbar must finish at
  // that scrollbar. Re-routing it would recursively re-enter applyWheel().
  if (conversationView->dispatchingNativeWheel())
    return false;
  const bool inCenter =
      target == conversationRegion || conversationRegion->isAncestorOf(target);
  const bool onHandle =
      target == splitter->handle(1) || target == splitter->handle(2);
  if (!inCenter && !onHandle)
    return false;

  auto *wheel = static_cast<QWheelEvent *>(event);
  if (inCenter) {
    if (target == conversationView || target == conversationView->viewport() ||
        conversationView->isAncestorOf(target)) {
      // Cards and the outer viewport naturally route to ConversationView;
      // nested editors below are handled by the edge test.
      for (QWidget *ancestor = target; ancestor && ancestor != conversationView;
           ancestor = ancestor->parentWidget()) {
        if (auto *nested = qobject_cast<QAbstractScrollArea *>(ancestor);
            nested && nested != conversationView) {
          if (dynamic_cast<CommandOutputView *>(nested))
            return false;
          if (canConsume(nested, verticalIntent(wheel)))
            return false;
          break;
        }
      }
      if (target == conversationView || target == conversationView->viewport())
        return false;
    } else {
      for (QWidget *ancestor = target;
           ancestor && ancestor != conversationRegion;
           ancestor = ancestor->parentWidget()) {
        if (auto *nested = qobject_cast<QAbstractScrollArea *>(ancestor)) {
          if (dynamic_cast<CommandOutputView *>(nested))
            return false;
          if (canConsume(nested, verticalIntent(wheel)))
            return false;
          break;
        }
      }
    }
  }
  const bool consumed = conversationView->forwardWheelEvent(wheel);
  if (consumed)
    event->accept();
  return consumed;
}

} // namespace codexui::codex::middle
