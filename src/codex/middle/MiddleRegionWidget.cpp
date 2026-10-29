// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/MiddleRegionWidget.h"

#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationPresentation.h"
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
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
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
  const auto headerButton = [&context](QString objectName) {
      auto* button = new QToolButton;
      button->setObjectName(std::move(objectName));
      button->setProperty("kind", "presentationToggle");
      button->setIconSize(QSize(presentation::CardHeaderMetrics::IconSize, presentation::CardHeaderMetrics::IconSize));
      button->setFixedSize(presentation::CardHeaderMetrics::ControlWidth, presentation::CardHeaderMetrics::LineHeight);
      context->addWidget(button);
      return button;
  };
  auto* firstTurn = headerButton(QStringLiteral("conversationFirstTurn"));
  firstTurn->setText(QStringLiteral("↥"));
  firstTurn->setToolTip(QStringLiteral("First turn; load earlier history if needed"));
  auto* previousTurn = headerButton(QStringLiteral("conversationPreviousTurn"));
  previousTurn->setText(QStringLiteral("↑"));
  previousTurn->setToolTip(QStringLiteral("Turn opening; previous turn when already there"));
  auto* turns = headerButton(QStringLiteral("conversationTurnOutline"));
  turns->setText(QStringLiteral("Turns"));
  turns->setFixedWidth(62);
  turns->setToolTip(QStringLiteral("Loaded turn prompts"));
  auto* nextTurn = headerButton(QStringLiteral("conversationNextTurn"));
  nextTurn->setText(QStringLiteral("↓"));
  nextTurn->setToolTip(QStringLiteral("Next turn opening"));
  auto* latest = headerButton(QStringLiteral("conversationLatest"));
  latest->setText(QStringLiteral("↧"));
  latest->setToolTip(QStringLiteral("Latest output; resume following"));
  for (auto* button : {firstTurn, previousTurn, turns, nextTurn, latest}) {
      button->setAccessibleName(button->toolTip());
  }
  auto* outline = new QFrame(turns, Qt::Popup | Qt::FramelessWindowHint);
  outline->setObjectName(QStringLiteral("conversationTurnMenu"));
  auto* list = new QListWidget(outline);
  list->setObjectName(QStringLiteral("conversationTurnList"));
  list->setAccessibleName(QStringLiteral("Loaded turn prompts"));
  list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list->setUniformItemSizes(true);
  auto* popupLayout = new QVBoxLayout(outline);
  popupLayout->setContentsMargins(6, 6, 6, 6);
  popupLayout->setSpacing(4);
  auto* count = new QLabel(outline);
  count->setObjectName(QStringLiteral("conversationTurnCount"));
  auto* load = new QPushButton(QStringLiteral("Load earlier turns"), outline);
  load->setObjectName(QStringLiteral("conversationTurnLoadEarlier"));
  popupLayout->addWidget(count);
  popupLayout->addWidget(load);
  popupLayout->addWidget(list);
  popupLayout->setSizeConstraint(QLayout::SetFixedSize);
  connect(load, &QPushButton::clicked, this, [this, outline] {
      outline->close();
      conversationView->loadEarlier();
  });
  connect(firstTurn, &QToolButton::clicked, this, [this] {
      conversationView->navigateToFirstTurn();
  });
  connect(previousTurn, &QToolButton::clicked, this, [this] {
      conversationView->navigateToTurn(conversationView->navigationTarget(-1));
  });
  connect(nextTurn, &QToolButton::clicked, this, [this] {
      conversationView->navigateToTurn(conversationView->navigationTarget(1));
  });
  connect(latest, &QToolButton::clicked, this, [this] {
      conversationView->followLatest();
  });
  const auto activate = [this, list, outline] {
      const auto* item = list->currentItem();
      if (item && item->flags().testFlag(Qt::ItemIsEnabled)) {
          const auto key = item->data(Qt::UserRole).toString().toStdString();
          const auto thread = item->data(Qt::UserRole + 1).toString().toStdString();
          outline->close();
          if (conversationView->presentedThreadId() == thread) {
              conversationView->navigateToTurn(key);
          }
      }
  };
  connect(list, &QListWidget::itemClicked, this, activate);
  auto* accept = new QShortcut(list);
  accept->setKeys({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
  accept->setContext(Qt::WidgetShortcut);
  connect(accept, &QShortcut::activated, this, activate);
  connect(turns, &QToolButton::clicked, this, [this, outline, list, turns, count, load, popupLayout] {
      list->clear();
      const auto destinations = conversationView->turnDestinations();
      count->setText(QStringLiteral("%1 loaded").arg(destinations.size()));
      const auto threadId = QString::fromStdString(conversationView->presentedThreadId());
      const bool hasMore = conversationView->conversationModel()->hasMore();
      load->setVisible(hasMore);
      load->setEnabled(!conversationView->historyRequestPending());
      const auto current = conversationView->navigationTarget();
      for (const auto& destination : destinations) {
          auto* item = new QListWidgetItem(destination.prompt, list);
          item->setToolTip(destination.prompt);
          item->setData(Qt::UserRole, QString::fromStdString(destination.key));
          item->setData(Qt::UserRole + 1, threadId);
          if (destination.key == current) {
              list->setCurrentItem(item);
          }
      }
      const auto available = turns->screen()->availableGeometry();
      const int maximum = available.height() * 3 / 5;
      const int rowHeight = std::max(30, list->fontMetrics().height() + 12);
      for (int row = 0; row < list->count(); ++row) {
          list->item(row)->setSizeHint(QSize(0, rowHeight));
      }
      outline->ensurePolished();
      const auto margins = outline->contentsMargins() + popupLayout->contentsMargins();
      const int chrome = margins.top() + margins.bottom() + count->sizeHint().height() + popupLayout->spacing() +
                         (hasMore ? load->sizeHint().height() + popupLayout->spacing() : 0);
      list->setFixedSize(std::min(420, available.width() - margins.left() - margins.right() - 12),
                         std::max(1, std::min(maximum - chrome, list->count() * rowHeight)));
      popupLayout->activate();
      outline->adjustSize();
      const auto below = turns->mapToGlobal(QPoint(0, turns->height()));
      const int top =
          below.y() + outline->height() <= available.bottom() + 1 ? below.y() : turns->mapToGlobal(QPoint()).y() - outline->height();
      outline->move(std::clamp(below.x(), available.left(), available.right() - outline->width() + 1),
                    std::clamp(top, available.top(), available.bottom() - outline->height() + 1));
      outline->show();
      list->doItemsLayout();
      list->setFocus(Qt::PopupFocusReason);
      list->scrollToItem(list->currentItem(), QAbstractItemView::PositionAtCenter);
  });
  const auto presentationToggle = [&headerButton](QString objectName, PresentationIcon icon) {
      auto* button = headerButton(std::move(objectName));
      button->setCheckable(true);
      button->setIcon(presentationIcon(icon));
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
  conversationTitle->setToolTip(conversationTitle->text());
  conversationTitle->installEventFilter(this);
  conversationTokens = new UiStyle::TokenUsageLabel;
  conversationTokens->setObjectName(QStringLiteral("threadTokenUsage"));
  conversationState = makeLabel({}, "meta");
  conversationState->setObjectName(QStringLiteral("conversationState"));
  conversationState->setSizePolicy(QSizePolicy::Minimum,
                                   QSizePolicy::Preferred);
  conversationTokens->setAlignment(Qt::AlignRight | Qt::AlignTop);
  threadHeading->addWidget(conversationTitle, 2);
  conversationWorkspace = new QToolButton;
  conversationWorkspace->setObjectName(QStringLiteral("conversationWorkspace"));
  conversationWorkspace->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  conversationWorkspace->setMinimumWidth(0);
  conversationWorkspace->setMaximumWidth(300);
  conversationWorkspace->setAutoRaise(true);
  conversationWorkspace->installEventFilter(this);
  threadHeading->addWidget(conversationWorkspace, 1);
  workspaceCopy = new presentation::CopyButton(QStringLiteral("Copy thread workspace"));
  workspaceCopy->setObjectName(QStringLiteral("copyThreadWorkspace"));
  threadHeading->addWidget(workspaceCopy);
  connect(conversationWorkspace, &QToolButton::clicked, this, [this] {
      if (workspaceAction) {
          workspaceAction();
      }
  });
  connect(workspaceCopy, &QToolButton::clicked, this, [this] {
      workspaceCopy->copyText(workspacePath, false);
  });
  threadHeading->addWidget(conversationState);
  contentLayout->addLayout(threadHeading);
  contentLayout->addWidget(conversationTokens);
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
  connect(conversationView, &ConversationView::navigationChanged, this, [this, firstTurn, previousTurn, turns, nextTurn, latest, outline] {
      const bool available = !conversationView->navigationTarget().empty();
      const bool historyPending = conversationView->historyRequestPending();
      firstTurn->setEnabled(available && !historyPending &&
                            (conversationView->conversationModel()->hasMore() || conversationView->verticalScrollBar()->value() > 1));
      previousTurn->setEnabled(available && !conversationView->navigationTarget(-1).empty());
      nextTurn->setEnabled(available && !conversationView->navigationTarget(1).empty());
      turns->setEnabled(available);
      latest->setEnabled(available && conversationView->mode() != ConversationView::Mode::Following);
      if (!available) {
          outline->close();
      }
  });
  connect(conversationView->conversationModel(), &QAbstractItemModel::modelReset, outline, &QFrame::close);
  connect(conversationView->conversationModel(), &QAbstractItemModel::rowsRemoved, outline, &QFrame::close);
  firstTurn->setEnabled(false);
  previousTurn->setEnabled(false);
  nextTurn->setEnabled(false);
  turns->setEnabled(false);
  latest->setEnabled(false);
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
  if (watched == conversationTitle && event->type() == QEvent::Resize)
    conversationTitle->setText(conversationTitle->fontMetrics().elidedText(
        conversationTitle->toolTip(), Qt::ElideRight, conversationTitle->width()));
  if (watched == conversationWorkspace && event->type() == QEvent::Resize) {
      conversationWorkspace->setText(conversationWorkspace->fontMetrics().elidedText(
          workspacePath.isEmpty() ? QStringLiteral("Workspace not reported") : workspacePath,
          Qt::ElideMiddle,
          std::max(0, conversationWorkspace->width() - 12)));
  }
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

void MiddleRegionWidget::setThreadHeading(QString title, const ui::TokenUsageText &usage,
                                          QString trailingMetadata,
                                          QString state, QString stateTone) {
  QString summary = QString::fromStdString(usage.summary);
  if (!trailingMetadata.isEmpty())
    summary += QStringLiteral(" | ") + trailingMetadata;
  conversationTokens->setUsage(summary,
                               QString::fromStdString(usage.details));
  conversationTitle->setText(conversationTitle->fontMetrics().elidedText(title, Qt::ElideRight, conversationTitle->width()));
  conversationTitle->setToolTip(title);
  conversationState->setText(state);
  conversationState->setProperty("tone", stateTone);
  presentation::setLabelTone(*conversationState, stateTone.toStdString());
}

void MiddleRegionWidget::setThreadWorkspace(QString path, QString unavailable, std::function<void()> action) {
    workspacePath = std::move(path);
    workspaceAction = std::move(action);
    const QString display = workspacePath.isEmpty() ? QStringLiteral("Workspace not reported") : workspacePath;
    conversationWorkspace->setText(
        conversationWorkspace->fontMetrics().elidedText(display, Qt::ElideMiddle, std::max(0, conversationWorkspace->width() - 12)));
    conversationWorkspace->setToolTip(display + QStringLiteral("\n") +
                                      (unavailable.isEmpty() ? QStringLiteral("Change workspace…") : unavailable));
    conversationWorkspace->setAccessibleName(QStringLiteral("Thread workspace: ") + display);
    conversationWorkspace->setAccessibleDescription(unavailable.isEmpty() ? QStringLiteral("Change workspace…") : unavailable);
    conversationWorkspace->setEnabled(unavailable.isEmpty() && static_cast<bool>(workspaceAction));
    workspaceCopy->setEnabled(!workspacePath.isEmpty());
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

bool MiddleRegionWidget::routeScrollEvent(QObject* watched, QEvent* event) {
    bool routed = false;
    auto* target = event && event->type() == QEvent::Wheel ? qobject_cast<QWidget*>(watched) : nullptr;
    // Popups are independent input surfaces even when parented below this region.
    if (target && target->window() == window()) {
        if (scrollGestureRoute_ == ScrollGestureRoute::Dispatching) {
            QAbstractScrollArea* const owner = scrollGestureArea_.data();
            routed = !owner || !(target == owner || owner->isAncestorOf(target));
            if (routed) {
                event->accept();
            }
        } else {
            auto* wheel = static_cast<QWheelEvent*>(event);
            if (wheel->phase() == Qt::ScrollBegin) {
                scrollGestureRoute_ = ScrollGestureRoute::Inactive;
                scrollGestureArea_.clear();
            }
            const bool inCenter = target == conversationRegion || conversationRegion->isAncestorOf(target);
            const bool inThreads =
                (target == threadPane && QRect(0, threadPane->scrollArea()->y(), threadPane->width(), threadPane->scrollArea()->height())
                                             .contains(threadPane->mapFromGlobal(wheel->globalPosition().toPoint()))) ||
                target == threadPane->scrollArea() || threadPane->scrollArea()->isAncestorOf(target);
            const bool onHandle = target == splitter->handle(1) || target == splitter->handle(2);
            const bool continuing = wheel->phase() != Qt::NoScrollPhase && scrollGestureRoute_ != ScrollGestureRoute::Inactive;
            if (inCenter || inThreads || onHandle || continuing) {
                routed = true;
                QAbstractScrollArea* const outerArea =
                    (continuing ? scrollGestureArea_ && threadPane->isAncestorOf(scrollGestureArea_) : inThreads) ? threadPane->scrollArea()
                                                                                                                  : conversationView;
                const bool inComposer = inCenter && (target == composerPane || composerPane->isAncestorOf(target));
                const bool inConversation = inCenter && (target == conversationView || target == conversationView->viewport() ||
                                                         conversationView->isAncestorOf(target));
                const auto scrollAreaBefore = [](QWidget* from, QWidget* stop) {
                    QAbstractScrollArea* result = nullptr;
                    for (QWidget* ancestor = from; ancestor && ancestor != stop && !result; ancestor = ancestor->parentWidget()) {
                        result = qobject_cast<QAbstractScrollArea*>(ancestor);
                    }
                    return result;
                };
                QAbstractScrollArea* const currentArea = inComposer       ? scrollAreaBefore(target, composerPane)
                                                         : inConversation ? scrollAreaBefore(target, conversationView)
                                                         : inThreads      ? scrollAreaBefore(target, threadPane)
                                                                          : nullptr;
                const auto dispatch = [this, wheel](QAbstractScrollArea* owner) {
                    wheel->accept();
                    if (owner && owner->viewport()) {
                        const QScopedValueRollback route(scrollGestureRoute_, ScrollGestureRoute::Dispatching);
                        const QScopedValueRollback area(scrollGestureArea_, QPointer<QAbstractScrollArea>(owner));
                        QWidget* const receiver = owner->viewport();
                        const QPointF local = receiver->mapFromGlobal(wheel->globalPosition());
                        QWheelEvent forwarded(local,
                                              wheel->globalPosition(),
                                              wheel->pixelDelta(),
                                              wheel->angleDelta(),
                                              wheel->buttons(),
                                              wheel->modifiers(),
                                              wheel->phase(),
                                              wheel->inverted(),
                                              wheel->source(),
                                              wheel->pointingDevice());
                        forwarded.setTimestamp(wheel->timestamp());
                        static_cast<void>(QApplication::sendEvent(receiver, &forwarded));
                    }
                };
                const int intent = verticalIntent(wheel);
                if (wheel->phase() == Qt::NoScrollPhase) {
                    dispatch(inComposer ? currentArea : currentArea && canConsume(currentArea, intent) ? currentArea : outerArea);
                } else {
                    if (wheel->phase() == Qt::ScrollBegin || scrollGestureRoute_ == ScrollGestureRoute::Inactive) {
                        scrollGestureArea_ = currentArea;
                        if (inComposer) {
                            scrollGestureRoute_ = ScrollGestureRoute::Native;
                        } else if (currentArea) {
                            scrollGestureRoute_ = ScrollGestureRoute::AwaitingNested;
                        } else {
                            scrollGestureArea_ = outerArea;
                            scrollGestureRoute_ = ScrollGestureRoute::Outer;
                        }
                    }
                    if (intent != 0 && scrollGestureRoute_ == ScrollGestureRoute::AwaitingNested) {
                        if (!scrollGestureArea_ || canConsume(scrollGestureArea_, intent)) {
                            scrollGestureRoute_ = ScrollGestureRoute::Native;
                        } else {
                            scrollGestureArea_ = outerArea;
                            scrollGestureRoute_ = ScrollGestureRoute::Outer;
                        }
                    }
                    if (scrollGestureRoute_ == ScrollGestureRoute::Native || scrollGestureRoute_ == ScrollGestureRoute::Outer) {
                        dispatch(scrollGestureArea_);
                    } else {
                        event->accept();
                    }
                    if (wheel->phase() == Qt::ScrollEnd) {
                        scrollGestureRoute_ = ScrollGestureRoute::Inactive;
                        scrollGestureArea_.clear();
                    }
                }
            }
        }
    }
    return routed;
}

} // namespace codexui::codex::middle
