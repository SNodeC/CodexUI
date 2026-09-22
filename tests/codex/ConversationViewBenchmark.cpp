// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

class BenchmarkApplication final : public QApplication {
public:
  struct FrameProbe {
    qint64 microseconds = -1;
    qulonglong layoutRequests = 0;
    qulonglong moves = 0;
    qulonglong paints = 0;
    qulonglong resizes = 0;
    qulonglong updateRequests = 0;
  };

  BenchmarkApplication(int &argc, char **argv) : QApplication(argc, argv) {}

  void resetEventTimings() {
    layoutMicros.clear();
    paintMicros.clear();
    measuring_ = true;
  }

  void setEventTimingsEnabled(bool enabled) noexcept { measuring_ = enabled; }

  void armFrameProbe(QWidget *topLevel, QElapsedTimer *timer,
                     QEventLoop *loop) noexcept {
    frameTopLevel_ = topLevel;
    frameTimer_ = timer;
    frameLoop_ = loop;
    frameProbe_ = {};
  }

  FrameProbe disarmFrameProbe() noexcept {
    frameTopLevel_ = nullptr;
    frameTimer_ = nullptr;
    frameLoop_ = nullptr;
    return frameProbe_;
  }

  std::vector<qint64> layoutMicros;
  std::vector<qint64> paintMicros;

  bool notify(QObject *receiver, QEvent *event) override {
    auto *widget = qobject_cast<QWidget *>(receiver);
    const bool inFrameTree =
        frameTopLevel_ && widget &&
        (widget == frameTopLevel_ || frameTopLevel_->isAncestorOf(widget));
    if (inFrameTree && event->type() == QEvent::LayoutRequest)
      ++frameProbe_.layoutRequests;
    else if (inFrameTree && event->type() == QEvent::Move)
      ++frameProbe_.moves;
    else if (inFrameTree && event->type() == QEvent::Paint)
      ++frameProbe_.paints;
    else if (inFrameTree && event->type() == QEvent::Resize)
      ++frameProbe_.resizes;
    const bool frameUpdate = frameTopLevel_ && receiver == frameTopLevel_ &&
                             event->type() == QEvent::UpdateRequest;
    const bool timed = measuring_ && (event->type() == QEvent::LayoutRequest ||
                                      event->type() == QEvent::Paint);
    QElapsedTimer dispatchTimer;
    if (timed)
      dispatchTimer.start();
    const bool handled = QApplication::notify(receiver, event);
    if (timed)
      (event->type() == QEvent::LayoutRequest ? layoutMicros : paintMicros)
          .push_back(dispatchTimer.nsecsElapsed() / 1000);
    if (frameUpdate) {
      ++frameProbe_.updateRequests;
      frameProbe_.microseconds = frameTimer_->nsecsElapsed() / 1000;
      frameLoop_->quit();
    }
    return handled;
  }

private:
  bool measuring_ = false;
  QWidget *frameTopLevel_ = nullptr;
  QElapsedTimer *frameTimer_ = nullptr;
  QEventLoop *frameLoop_ = nullptr;
  FrameProbe frameProbe_;
};

namespace codexui::codex::middle {
namespace {

bool changed(ConversationView::ReconciliationResult result) {
  return result == ConversationView::ReconciliationResult::Changed;
}

constexpr int NormalScrollSamples = 120;
constexpr int WarmedScrollSamples = 120;
constexpr int PaintedScrollSamples = 60;
constexpr int SeekSamples = 48;
constexpr int StreamSamples = 24;
constexpr int FollowingAppendSamples = 20;
constexpr int ResizeFrameSamples = 20;
constexpr int SparseVisibilitySamples = 32;
constexpr std::size_t CardsPerTurn = 10;

#if defined(__SANITIZE_ADDRESS__)
constexpr bool InstrumentedBuild = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool InstrumentedBuild = true;
#else
constexpr bool InstrumentedBuild = false;
#endif
#else
constexpr bool InstrumentedBuild = false;
#endif

VisibleCardData cardData(std::size_t index) {
  const std::string suffix = std::to_string(index);
  const std::string turn = "turn-" + std::to_string(index / CardsPerTurn);
  VisibleCardData card;
  card.key = AuthoritativeItemKey{"benchmark-thread", turn, "item-" + suffix};
  card.threadId = "benchmark-thread";
  card.turnId = turn;
  card.itemId = "item-" + suffix;
  switch (index % CardsPerTurn) {
  case 0:
    card.kind = CardKind::UserMessage;
    card.payload = UserMessageData{
        "Prompt " + suffix +
            " with **Markdown** and a [link](https://example.com).",
        {}};
    break;
  case 1:
    card.kind = CardKind::AgentMessage;
    {
      std::string markdown =
          "A compact final answer for benchmark row " + suffix +
          ".\n\nA second paragraph exercises wrapped Markdown layout.";
      if (index % 317 == 1)
        for (int paragraph = 0; paragraph < 48; ++paragraph)
          markdown += "\n\nLong streaming paragraph " +
                      std::to_string(paragraph) +
                      " contains **Markdown**, wrapping, and a "
                      "[link](https://example.com).";
      card.payload = AgentMessageData{std::move(markdown), true};
    }
    break;
  case 2:
    card.kind = CardKind::CommandExecution;
    {
      std::string output = "line one\nline two";
      if (index % 317 == 2)
        for (int line = 0; line < 96; ++line)
          output += "\nlong command output " + std::to_string(line);
      card.payload =
          CommandExecutionData{"printf benchmark", output, "/tmp", 0, 4};
      card.status = nodegraph::NodeStatus::Completed;
    }
    break;
  case 3:
    card.kind = CardKind::Reasoning;
    card.payload = ReasoningData{"Reasoning summary " + suffix};
    card.status = nodegraph::NodeStatus::Completed;
    break;
  case 4:
    card.kind = CardKind::AgentActivity;
    card.payload =
        AgentActivityData{"worker", "worker", "Inspect benchmark state",
                          "Agent result " + suffix};
    card.status = nodegraph::NodeStatus::Completed;
    break;
  case 5:
    card.kind = CardKind::FileChanges;
    card.payload = FileChangesData{
        {{"src/example-" + suffix + ".cpp", "update", 2, 1}}, "/tmp"};
    card.status = nodegraph::NodeStatus::Completed;
    break;
  case 6:
    card.kind = CardKind::Plan;
    card.payload = PlanData{"Plan " + suffix,
                            {{"Inspect", nodegraph::NodeStatus::Completed},
                             {"Change", nodegraph::NodeStatus::Pending}},
                            {}};
    card.status = nodegraph::NodeStatus::Completed;
    break;
  case 7:
    card.kind = CardKind::GenericActivity;
    card.payload =
        GenericActivityData{"toolCall", "detail: benchmark " + suffix};
    card.status = nodegraph::NodeStatus::Completed;
    break;
  case 8:
    card.kind = CardKind::ImageGeneration;
    card.payload =
        ImageGenerationData{{}, "Generated benchmark image " + suffix};
    card.status = nodegraph::NodeStatus::Completed;
    break;
  default:
    card.kind = CardKind::LocalPrompt;
    card.payload = LocalPromptData{index, "Queued prompt " + suffix,
                                   PromptState::Accepted, false};
    break;
  }
  return card;
}

std::optional<PresentationImpact> applyPresentation(ConversationView &view,
                                                    VisibleCardData card) {
  ConversationDelta delta;
  delta.threadId = card.threadId;
  delta.presentations.push_back(std::move(card));
  return view.applyConversationDelta(std::move(delta));
}

bool appendProjected(ConversationView &view, ConversationRowPlacement tail) {
  ConversationRowChange change;
  change.placement = std::move(tail);
  if (const ConversationItemModel::Row *last = view.conversationModel()->row(
          view.conversationModel()->rowCount() - 1))
    change.previousCardKey = last->card.key;
  ConversationDelta delta;
  delta.threadId = change.placement.card.threadId;
  delta.rows.push_back(std::move(change));
  return view.applyConversationDelta(std::move(delta)).has_value();
}

ConversationSnapshot snapshot(std::size_t count) {
  ConversationSnapshot result;
  result.threadId = "benchmark-thread";
  result.sections.reserve((count + CardsPerTurn - 1) / CardsPerTurn);
  for (std::size_t index = 0; index < count; ++index) {
    VisibleCardData card = cardData(index);
    if (index % CardsPerTurn == 0) {
      TurnSection section;
      section.key = "turn-section-" + std::to_string(index / CardsPerTurn);
      section.turnId = card.turnId;
      section.rootCardKey = card.key;
      result.sections.push_back(std::move(section));
    }
    result.sections.back().cards.push_back(std::move(card));
  }
  if (!result.sections.empty()) {
    result.activeTurnId = result.sections.back().turnId;
    for (VisibleCardData &card : result.sections.back().cards)
      if (card.kind == CardKind::AgentActivity)
        card.status = nodegraph::NodeStatus::Running;
  }
  return result;
}

ConversationSnapshot sparseVisibilitySnapshot(std::size_t hiddenRows) {
  ConversationSnapshot result;
  result.threadId = "sparse-benchmark";
  TurnSection section;
  section.key = "sparse-section";
  section.turnId = "sparse-turn";
  VisibleCardData root{
      AuthoritativeItemKey{result.threadId, section.turnId, "root"},
      CardKind::UserMessage,
      result.threadId,
      section.turnId,
      "root",
      UserMessageData{"Visible root", {}}};
  section.rootCardKey = root.key;
  section.cards.push_back(std::move(root));
  section.cards.reserve(hiddenRows + 2);
  for (std::size_t row = 0; row < hiddenRows; ++row) {
    const std::string item = "hidden-" + std::to_string(row);
    section.cards.push_back(
        {AuthoritativeItemKey{result.threadId, section.turnId, item},
         CardKind::Reasoning, result.threadId, section.turnId, item,
         ReasoningData{"Filtered reasoning"}});
  }
  section.cards.push_back(
      {AuthoritativeItemKey{result.threadId, section.turnId, "tail"},
       CardKind::AgentMessage, result.threadId, section.turnId, "tail",
       AgentMessageData{"Visible tail", true}});
  result.sections.push_back(std::move(section));
  return result;
}

class EventWorkProbe final : public QObject {
public:
  void reset() noexcept {
    layoutRequests = 0;
    paints = 0;
  }

  qulonglong layoutRequests = 0;
  qulonglong paints = 0;

  void beginAdmissionProbe(const ConversationView &view) {
    admissionView_ = &view;
    admissionConstructionBaseline_ =
        view.property("conversationCardConstructions").toULongLong();
    maximumAdmissionConstructions_ = 0;
  }

  qulonglong endAdmissionProbe() noexcept {
    admissionView_ = nullptr;
    return maximumAdmissionConstructions_;
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::LayoutRequest)
      ++layoutRequests;
    else if (event->type() == QEvent::Paint)
      ++paints;
    else if (watched == admissionView_ &&
             event->type() == QEvent::DynamicPropertyChange &&
             static_cast<QDynamicPropertyChangeEvent *>(event)
                     ->propertyName() == "conversationCardAdmissionPasses") {
      const qulonglong constructions =
          watched->property("conversationCardConstructions").toULongLong();
      maximumAdmissionConstructions_ =
          std::max(maximumAdmissionConstructions_,
                   constructions - admissionConstructionBaseline_);
      admissionConstructionBaseline_ = constructions;
    }
    return false;
  }

private:
  const ConversationView *admissionView_ = nullptr;
  qulonglong admissionConstructionBaseline_ = 0;
  qulonglong maximumAdmissionConstructions_ = 0;
};

class DocumentTracker final : public QObject {
public:
  void observe(const ConversationView &view) {
    const auto track = [this](QTextDocument *document) {
      if (!document || !documents_.insert(document).second)
        return;
      connect(document, &QTextDocument::contentsChanged, this,
              [this] { ++changes; });
      connect(document, &QObject::destroyed, this,
              [this, document] { documents_.erase(document); });
    };
    for (QTextEdit *edit : view.findChildren<QTextEdit *>())
      track(edit->document());
    for (QPlainTextEdit *edit : view.findChildren<QPlainTextEdit *>())
      track(edit->document());
    peakDocuments = std::max(peakDocuments, documents_.size());
  }

  void reset() noexcept { changes = 0; }

  qulonglong changes = 0;
  std::size_t peakDocuments = 0;

private:
  std::unordered_set<QTextDocument *> documents_;
};

class ConstructionTracker final : public QObject {
public:
  void observe(const ConversationView &view) {
    for (ConversationCard *card : view.findChildren<ConversationCard *>()) {
      const QVariant metric = card->property("conversationConstructionMicros");
      if (!metric.isValid() || !cards_.insert(card).second)
        continue;
      samples.push_back(metric.toLongLong());
      connect(card, &QObject::destroyed, this,
              [this, card] { cards_.erase(card); });
    }
  }

  std::vector<qint64> samples;

private:
  std::unordered_set<ConversationCard *> cards_;
};

void processEventsAndDeferredDeletes(int maximumTimeMilliseconds) {
  // These benchmark loops do not enter a persistent event loop, so Qt leaves
  // DeferredDelete queued unless it is sent after each processed event batch.
  QApplication::processEvents(QEventLoop::AllEvents, maximumTimeMilliseconds);
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void processFrame() {
  for (int pass = 0; pass < 3; ++pass) {
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    processEventsAndDeferredDeletes(5);
  }
}

struct SparseVisibilityWork {
  qint64 microseconds = 0;
  bool correct = false;
};

SparseVisibilityWork sampleSparseVisibility(std::size_t hiddenRows) {
  ConversationView view;
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.showReasoning = false;
  view.setPresentationOptions(options);
  view.resize(900, 700);
  view.show();
  const bool reconciled =
      changed(view.reconcile(sparseVisibilitySnapshot(hiddenRows)));
  processFrame();
  const int lastRow = static_cast<int>(hiddenRows + 1);
  const QModelIndex first = view.conversationModel()->index(0);
  const QModelIndex last = view.conversationModel()->index(lastRow);
  const bool correct =
      reconciled && view.conversationModel()->rowCount() == lastRow + 1 &&
      view.materializedCardCount() == 2 &&
      view.visualRect(first).intersects(view.viewport()->rect()) &&
      view.visualRect(last).intersects(view.viewport()->rect());
  QElapsedTimer timer;
  timer.start();
  for (int sample = 0; sample < SparseVisibilitySamples; ++sample) {
    view.scrollTo(sample % 2 == 0 ? first : last,
                  QAbstractItemView::PositionAtCenter);
    view.viewport()->repaint();
  }
  return {timer.nsecsElapsed() / 1000, correct};
}

void waitForFrameTimer() {
  QEventLoop loop;
  QTimer::singleShot(18, &loop, &QEventLoop::quit);
  loop.exec();
  processFrame();
}

qulonglong counter(const QObject &object, const char *name) {
  return object.property(name).toULongLong();
}

bool hasCounter(const QObject &object, const char *name) {
  return object.property(name).isValid();
}

qint64 peakResidentKiB() {
#if defined(__linux__)
  rusage usage{};
  return getrusage(RUSAGE_SELF, &usage) == 0
             ? static_cast<qint64>(usage.ru_maxrss)
             : -1;
#else
  return -1;
#endif
}

qint64 currentResidentKiB() {
#if defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  qint64 totalPages = 0;
  qint64 residentPages = 0;
  if (statm >> totalPages >> residentPages) {
    static_cast<void>(totalPages);
    const long pageBytes = sysconf(_SC_PAGESIZE);
    if (pageBytes > 0)
      return residentPages * pageBytes / 1024;
  }
#endif
  return -1;
}

struct SampleStats {
  qint64 median = 0;
  qint64 p95 = 0;
  qint64 maximum = 0;
};

SampleStats statistics(std::vector<qint64> samples) {
  if (samples.empty())
    return {};
  std::ranges::sort(samples);
  const auto percentile = [&samples](double proportion) {
    const std::size_t index = static_cast<std::size_t>(std::clamp<qint64>(
        static_cast<qint64>(std::ceil(proportion * samples.size())) - 1, 0,
        static_cast<qint64>(samples.size() - 1)));
    return samples[index];
  };
  return {percentile(0.5), percentile(0.95), samples.back()};
}

struct Census {
  int cards = 0;
  int documents = 0;
  int widgets = 0;
  int visibleCards = 0;
  int viewportRows = 0;
  qint64 residentKiB = -1;
  qint64 maximumConstructionMicros = 0;
  qulonglong maximumHeightUpdateSteps = 0;
  quint64 checksum = 1469598103934665603ULL;
  quint16 kindMask = 0;
  bool identitiesMatch = true;
  bool residentKeysUnique = true;
  bool shownCardsIntersect = true;
  bool viewportRowsRendered = true;
  bool residentCardsInViewportBuffer = true;
  bool boundsHold = true;
  bool visibleTextPresent = true;
  bool constructionMetricObserved = false;
};

void mixChecksum(quint64 &checksum, std::string_view value) {
  for (const unsigned char byte : value) {
    checksum ^= byte;
    checksum *= 1099511628211ULL;
  }
}

void mixChecksum(quint64 &checksum, qint64 value) {
  mixChecksum(checksum, std::to_string(value));
}

void mixText(quint64 &checksum, const QString &value) {
  const QByteArray encoded = value.toUtf8();
  mixChecksum(checksum,
              std::string_view(encoded.constData(),
                               std::min<qsizetype>(encoded.size(), 512)));
  mixChecksum(checksum, encoded.size());
}

quint64 pixelChecksum(const QImage &image) {
  quint64 checksum = 1469598103934665603ULL;
  mixChecksum(checksum, image.width());
  mixChecksum(checksum, image.height());
  const uchar *bits = image.constBits();
  const qsizetype bytes = image.sizeInBytes();
  for (qsizetype offset = 0; offset < bytes; offset += 97) {
    checksum ^= bits[offset];
    checksum *= 1099511628211ULL;
  }
  return checksum;
}

Census inspect(const ConversationView &view) {
  Census result;
  const auto cards = view.findChildren<ConversationCard *>();
  const auto widgets = view.findChildren<QWidget *>();
  std::unordered_map<std::string, const ConversationCard *> cardsByKey;
  std::unordered_set<const QTextDocument *> documents;
  for (const QTextEdit *edit : view.findChildren<QTextEdit *>())
    documents.insert(edit->document());
  for (const QPlainTextEdit *edit : view.findChildren<QPlainTextEdit *>())
    documents.insert(edit->document());
  result.cards = cards.size();
  result.documents = static_cast<int>(documents.size());
  result.widgets = widgets.size();
  result.residentKiB = currentResidentKiB();
  const QRect viewportBuffer = view.viewport()->rect().adjusted(
      0, -2 * view.viewport()->height(), 0, 2 * view.viewport()->height());
  QWidget *const focused = QApplication::focusWidget();
  for (const ConversationCard *card : cards) {
    const std::string key = stableKey(card->data().key);
    result.residentKeysUnique =
        result.residentKeysUnique && cardsByKey.emplace(key, card).second;
    const QModelIndex index = view.conversationModel()->indexForStableKey(key);
    const VisibleCardData *modelCard =
        index.isValid() ? view.conversationModel()->card(index.row()) : nullptr;
    result.identitiesMatch =
        result.identitiesMatch && modelCard && stableKey(modelCard->key) == key;
    mixChecksum(result.checksum, key);
    mixChecksum(result.checksum, card->x());
    mixChecksum(result.checksum, card->y());
    mixChecksum(result.checksum, card->width());
    mixChecksum(result.checksum, card->height());
    result.kindMask |=
        static_cast<quint16>(1U << static_cast<unsigned>(card->data().kind));
    const QVariant construction =
        card->property("conversationConstructionMicros");
    result.constructionMetricObserved =
        result.constructionMetricObserved || construction.isValid();
    result.maximumConstructionMicros =
        std::max(result.maximumConstructionMicros, construction.toLongLong());
    const QRect geometry(card->mapTo(view.viewport(), QPoint{}), card->size());
    const bool ownsFocus =
        focused && (focused == card || card->isAncestorOf(focused));
    result.residentCardsInViewportBuffer =
        result.residentCardsInViewportBuffer &&
        (geometry.intersects(viewportBuffer) || ownsFocus);
    if (card->isHidden())
      continue;
    ++result.visibleCards;
    result.shownCardsIntersect = result.shownCardsIntersect &&
                                 geometry.intersects(view.viewport()->rect());
    bool cardHasText = false;
    for (const QLabel *label : card->findChildren<QLabel *>()) {
      if (!label->isVisibleTo(card) || label->text().isEmpty())
        continue;
      cardHasText = true;
      mixText(result.checksum, label->text());
    }
    for (const QTextEdit *edit : card->findChildren<QTextEdit *>()) {
      if (!edit->isVisibleTo(card) || edit->toPlainText().isEmpty())
        continue;
      cardHasText = true;
      mixText(result.checksum, edit->toPlainText());
    }
    for (const QPlainTextEdit *edit : card->findChildren<QPlainTextEdit *>()) {
      if (!edit->isVisibleTo(card) || edit->toPlainText().isEmpty())
        continue;
      cardHasText = true;
      mixText(result.checksum, edit->toPlainText());
    }
    result.visibleTextPresent = result.visibleTextPresent && cardHasText;
  }
  std::set<int> viewportRows;
  const int x = std::max(0, view.viewport()->width() / 2);
  for (int y = 0; y < view.viewport()->height(); ++y) {
    const QModelIndex index = view.indexAt(QPoint(x, y));
    if (index.isValid())
      viewportRows.insert(index.row());
  }
  result.viewportRows = static_cast<int>(viewportRows.size());
  for (const int rowIndex : viewportRows) {
    const auto *row = view.conversationModel()->row(rowIndex);
    const auto found = row ? cardsByKey.find(row->stableKey) : cardsByKey.end();
    if (!row || found == cardsByKey.end()) {
      result.viewportRowsRendered = false;
      continue;
    }
    const ConversationCard *card = found->second;
    const QRect actual(card->mapTo(view.viewport(), QPoint{}), card->size());
    result.viewportRowsRendered =
        result.viewportRowsRendered && !card->isHidden() &&
        actual == view.visualRect(view.conversationModel()->index(rowIndex));
  }
  result.boundsHold = result.documents <= std::max(2, result.cards * 2) &&
                      result.widgets <= result.cards * 24 + 16;
  result.maximumHeightUpdateSteps =
      counter(view, "conversationHeightIndexUpdateSteps");
  return result;
}

void accumulate(Census &peak, const Census &sample) {
  peak.cards = std::max(peak.cards, sample.cards);
  peak.documents = std::max(peak.documents, sample.documents);
  peak.widgets = std::max(peak.widgets, sample.widgets);
  peak.visibleCards = std::max(peak.visibleCards, sample.visibleCards);
  peak.viewportRows = std::max(peak.viewportRows, sample.viewportRows);
  peak.residentKiB = std::max(peak.residentKiB, sample.residentKiB);
  peak.maximumConstructionMicros = std::max(peak.maximumConstructionMicros,
                                            sample.maximumConstructionMicros);
  peak.maximumHeightUpdateSteps =
      std::max(peak.maximumHeightUpdateSteps, sample.maximumHeightUpdateSteps);
  peak.checksum = std::rotl(peak.checksum, 1) ^ sample.checksum;
  peak.kindMask |= sample.kindMask;
  peak.identitiesMatch = peak.identitiesMatch && sample.identitiesMatch;
  peak.residentKeysUnique =
      peak.residentKeysUnique && sample.residentKeysUnique;
  peak.shownCardsIntersect =
      peak.shownCardsIntersect && sample.shownCardsIntersect;
  peak.viewportRowsRendered =
      peak.viewportRowsRendered && sample.viewportRowsRendered;
  peak.residentCardsInViewportBuffer = peak.residentCardsInViewportBuffer &&
                                       sample.residentCardsInViewportBuffer;
  peak.boundsHold = peak.boundsHold && sample.boundsHold;
  peak.visibleTextPresent =
      peak.visibleTextPresent && sample.visibleTextPresent;
  peak.constructionMetricObserved =
      peak.constructionMetricObserved || sample.constructionMetricObserved;
}

struct ScrollWork {
  std::vector<qint64> timings;
  qulonglong maximumConstructions = 0;
  qulonglong maximumReleases = 0;
  qulonglong maximumResidencyScans = 0;
  qulonglong maximumAdmissionPasses = 0;
  qulonglong maximumCombinedPasses = 0;
  qulonglong maximumLayoutRequests = 0;
  qulonglong maximumPaints = 0;
  bool targetsReached = true;
};

struct WarmedScrollWork {
  std::vector<qint64> timings;
  qulonglong maximumConstructions = 0;
  qulonglong maximumResidencyScans = 0;
  qulonglong maximumAdmissionConstructions = 0;
  qulonglong maximumAdmissionPasses = 0;
  qulonglong maximumConstructionsPerAdmissionPass = 0;
  bool targetsReached = true;
  bool viewportRowsRendered = true;
  bool admissionCadenceExact = true;
};

struct PaintedScrollWork {
  std::vector<qint64> timings;
  std::vector<qint64> zeroResidencyTimings;
  qulonglong maximumConstructions = 0;
  qulonglong maximumAdmissionPasses = 0;
  qulonglong maximumResidencyScans = 0;
  qulonglong maximumLayoutRequests = 0;
  qulonglong maximumMoves = 0;
  qulonglong maximumPaints = 0;
  qulonglong maximumResizes = 0;
  qulonglong maximumUpdateRequests = 0;
  bool targetsReached = true;
  bool viewportRowsRendered = true;
  bool firstFramePixelsMoved = false;
};

bool paintedFrameWorkBounded(const PaintedScrollWork &work) {
  return work.maximumLayoutRequests <= 64 && work.maximumMoves <= 64 &&
         work.maximumPaints <= 256 && work.maximumResizes <= 64;
}

void sampleScroll(ConversationView &view, int value, ScrollWork &work,
                  Census &peak, EventWorkProbe &events,
                  ConstructionTracker &constructionTracker,
                  const QModelIndex &targetIndex = {}) {
  const qulonglong constructions =
      counter(view, "conversationCardConstructions");
  const qulonglong releases = counter(view, "conversationRowsReleased");
  const qulonglong passes = counter(view, "conversationMaterializationPasses");
  const qulonglong admissions =
      counter(view, "conversationCardAdmissionPasses");
  events.reset();
  QElapsedTimer timer;
  timer.start();
  view.verticalScrollBar()->setValue(value);
  processFrame();
  work.timings.push_back(timer.nsecsElapsed() / 1000);
  work.maximumConstructions =
      std::max(work.maximumConstructions,
               counter(view, "conversationCardConstructions") - constructions);
  work.maximumReleases =
      std::max(work.maximumReleases,
               counter(view, "conversationRowsReleased") - releases);
  const qulonglong residencyScans =
      counter(view, "conversationMaterializationPasses") - passes;
  const qulonglong admissionPasses =
      counter(view, "conversationCardAdmissionPasses") - admissions;
  work.maximumResidencyScans =
      std::max(work.maximumResidencyScans, residencyScans);
  work.maximumAdmissionPasses =
      std::max(work.maximumAdmissionPasses, admissionPasses);
  work.maximumCombinedPasses =
      std::max(work.maximumCombinedPasses, residencyScans + admissionPasses);
  work.maximumLayoutRequests =
      std::max(work.maximumLayoutRequests, events.layoutRequests);
  work.maximumPaints = std::max(work.maximumPaints, events.paints);
  work.targetsReached =
      work.targetsReached &&
      (targetIndex.isValid()
           ? view.visualRect(targetIndex).intersects(view.viewport()->rect())
           : view.verticalScrollBar()->value() == value);
  constructionTracker.observe(view);
  accumulate(peak, inspect(view));
}

void sampleWarmedWheel(ConversationView &view, int angleDelta,
                       WarmedScrollWork &work, Census &peak,
                       EventWorkProbe &events,
                       ConstructionTracker &constructionTracker) {
  QScrollBar *const scrollBar = view.verticalScrollBar();
  const int value = scrollBar->value();
  const qulonglong constructions =
      counter(view, "conversationCardConstructions");
  const qulonglong passes = counter(view, "conversationMaterializationPasses");
  const QPointF local = view.viewport()->rect().center();
  QWheelEvent wheel(local, view.viewport()->mapToGlobal(local.toPoint()), {},
                    QPoint(0, angleDelta), Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false,
                    Qt::MouseEventSynthesizedByApplication,
                    QPointingDevice::primaryPointingDevice());
  wheel.ignore();
  QElapsedTimer timer;
  timer.start();
  static_cast<void>(QApplication::sendEvent(view.viewport(), &wheel));
  work.timings.push_back(timer.nsecsElapsed() / 1000);
  work.maximumConstructions =
      std::max(work.maximumConstructions,
               counter(view, "conversationCardConstructions") - constructions);
  work.maximumResidencyScans =
      std::max(work.maximumResidencyScans,
               counter(view, "conversationMaterializationPasses") - passes);
  work.targetsReached =
      work.targetsReached && wheel.isAccepted() && scrollBar->value() != value;
  work.viewportRowsRendered =
      work.viewportRowsRendered && inspect(view).viewportRowsRendered;
  const qulonglong constructionsBeforeAdmission =
      counter(view, "conversationCardConstructions");
  const qulonglong passesBeforeAdmission =
      counter(view, "conversationCardAdmissionPasses");
  events.beginAdmissionProbe(view);
  processFrame();
  work.maximumConstructionsPerAdmissionPass = std::max(
      work.maximumConstructionsPerAdmissionPass, events.endAdmissionProbe());
  const qulonglong admissionConstructions =
      counter(view, "conversationCardConstructions") -
      constructionsBeforeAdmission;
  const qulonglong admissionPasses =
      counter(view, "conversationCardAdmissionPasses") - passesBeforeAdmission;
  work.maximumAdmissionConstructions =
      std::max(work.maximumAdmissionConstructions, admissionConstructions);
  work.maximumAdmissionPasses =
      std::max(work.maximumAdmissionPasses, admissionPasses);
  work.admissionCadenceExact =
      work.admissionCadenceExact && admissionConstructions == admissionPasses;
  constructionTracker.observe(view);
  const Census afterAdmission = inspect(view);
  work.viewportRowsRendered =
      work.viewportRowsRendered && afterAdmission.viewportRowsRendered;
  accumulate(peak, afterAdmission);
}

template <typename Action>
void samplePaintedAction(BenchmarkApplication &application,
                         ConversationView &view, PaintedScrollWork &work,
                         Census &peak, ConstructionTracker &constructionTracker,
                         Action &&action) {
  QScrollBar *const scrollBar = view.verticalScrollBar();
  const int value = scrollBar->value();
  const bool verifyFirstFramePixels = work.timings.empty();
  const QImage pixelsBefore =
      verifyFirstFramePixels ? view.viewport()->grab().toImage() : QImage{};
  const qulonglong constructions =
      counter(view, "conversationCardConstructions");
  const qulonglong admissions =
      counter(view, "conversationCardAdmissionPasses");
  const qulonglong scans = counter(view, "conversationMaterializationPasses");
  QElapsedTimer timer;
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(250);
  bool accepted = false;
  QTimer::singleShot(0, &loop, [&] {
    timer.start();
    application.armFrameProbe(view.window(), &timer, &loop);
    accepted = action();
  });
  loop.exec();
  const BenchmarkApplication::FrameProbe frame = application.disarmFrameProbe();
  timeout.stop();

  const qulonglong constructionDelta =
      counter(view, "conversationCardConstructions") - constructions;
  const qulonglong admissionDelta =
      counter(view, "conversationCardAdmissionPasses") - admissions;
  if (frame.microseconds >= 0) {
    work.timings.push_back(frame.microseconds);
    if (constructionDelta == 0 && admissionDelta == 0)
      work.zeroResidencyTimings.push_back(frame.microseconds);
  }
  work.maximumConstructions =
      std::max(work.maximumConstructions, constructionDelta);
  work.maximumAdmissionPasses =
      std::max(work.maximumAdmissionPasses, admissionDelta);
  work.maximumResidencyScans =
      std::max(work.maximumResidencyScans,
               counter(view, "conversationMaterializationPasses") - scans);
  work.maximumLayoutRequests =
      std::max(work.maximumLayoutRequests, frame.layoutRequests);
  work.maximumMoves = std::max(work.maximumMoves, frame.moves);
  work.maximumPaints = std::max(work.maximumPaints, frame.paints);
  work.maximumResizes = std::max(work.maximumResizes, frame.resizes);
  work.maximumUpdateRequests =
      std::max(work.maximumUpdateRequests, frame.updateRequests);
  work.targetsReached = work.targetsReached && accepted &&
                        scrollBar->value() != value &&
                        frame.microseconds >= 0 && frame.paints > 0;
  if (verifyFirstFramePixels)
    work.firstFramePixelsMoved =
        view.viewport()->grab().toImage() != pixelsBefore;
  constructionTracker.observe(view);
  const Census frameCensus = inspect(view);
  work.viewportRowsRendered =
      work.viewportRowsRendered && frameCensus.viewportRowsRendered;
  accumulate(peak, frameCensus);
}

void samplePaintedWheel(
    BenchmarkApplication &application, ConversationView &view,
    QPoint pixelDelta, QPoint angleDelta, Qt::ScrollPhase phase,
    PaintedScrollWork &work, Census &peak,
    ConstructionTracker &constructionTracker,
    const QPointingDevice *device = QPointingDevice::primaryPointingDevice()) {
  const QPointF local = view.viewport()->rect().center();
  QWheelEvent wheel(local, view.viewport()->mapToGlobal(local.toPoint()),
                    pixelDelta, angleDelta, Qt::NoButton, Qt::NoModifier, phase,
                    false, Qt::MouseEventSynthesizedByApplication, device);
  samplePaintedAction(application, view, work, peak, constructionTracker, [&] {
    return QApplication::sendEvent(view.viewport(), &wheel) &&
           wheel.isAccepted();
  });
}

void samplePaintedScrollBar(BenchmarkApplication &application,
                            ConversationView &view, int target,
                            PaintedScrollWork &work, Census &peak,
                            ConstructionTracker &constructionTracker) {
  samplePaintedAction(application, view, work, peak, constructionTracker, [&] {
    QScrollBar *const scrollBar = view.verticalScrollBar();
    scrollBar->setValue(target);
    return scrollBar->value() == target;
  });
}

using CardSnapshot =
    std::unordered_map<std::string, std::pair<const ConversationCard *, QRect>>;

CardSnapshot captureCards(const ConversationView &view) {
  CardSnapshot result;
  for (const ConversationCard *card : view.findChildren<ConversationCard *>())
    result.emplace(stableKey(card->data().key),
                   std::pair{card, QRect(card->mapTo(view.viewport(), QPoint{}),
                                         card->size())});
  return result;
}

bool sameCards(const ConversationView &view, const CardSnapshot &before) {
  const CardSnapshot after = captureCards(view);
  if (after.size() != before.size())
    return false;
  return std::ranges::all_of(before, [&after](const auto &entry) {
    const auto found = after.find(entry.first);
    return found != after.end() && found->second == entry.second;
  });
}

bool retainsCards(const ConversationView &view, const CardSnapshot &before) {
  const CardSnapshot after = captureCards(view);
  return std::ranges::all_of(before, [&after](const auto &entry) {
    const auto found = after.find(entry.first);
    return found != after.end() && found->second == entry.second;
  });
}

bool residencyMatchesViewportWindow(const ConversationView &view) {
  const int height = view.viewport()->height();
  const QRect window = view.viewport()->rect().adjusted(0, -height, 0, height);
  std::unordered_set<std::string> expected;
  for (int rowIndex = 0; rowIndex < view.conversationModel()->rowCount();
       ++rowIndex) {
    const QModelIndex index = view.conversationModel()->index(rowIndex);
    if (view.visualRect(index).intersects(window)) {
      const auto *row = view.conversationModel()->row(rowIndex);
      if (row)
        expected.insert(row->stableKey);
    }
  }

  int boundaryExtras = 0;
  int focusPins = 0;
  QWidget *const focused = QApplication::focusWidget();
  for (const ConversationCard *card : view.findChildren<ConversationCard *>()) {
    if (expected.erase(stableKey(card->data().key)) != 0)
      continue;
    const bool ownsFocus =
        focused && (focused == card || card->isAncestorOf(focused));
    ownsFocus ? ++focusPins : ++boundaryExtras;
  }
  // The height index includes inter-row spacing while visualRect() does not,
  // so one extra boundary row on either side may legitimately be resident.
  // Every card rect that intersects the window must still be present.
  return expected.empty() && boundaryExtras <= 2 && focusPins <= 1;
}

bool waitForResidency(ConversationView &view,
                      ConstructionTracker *constructions = nullptr,
                      int timeoutMilliseconds = 5000) {
  QElapsedTimer deadline;
  deadline.start();
  while ((view.structuralStagingActive() ||
          !residencyMatchesViewportWindow(view)) &&
         deadline.elapsed() < timeoutMilliseconds) {
    processEventsAndDeferredDeletes(20);
    if (constructions)
      constructions->observe(view);
    QThread::msleep(1);
  }
  if (constructions)
    constructions->observe(view);
  return !view.structuralStagingActive() &&
         residencyMatchesViewportWindow(view);
}

ConversationCard *cardForKey(ConversationView &view, const std::string &key) {
  for (ConversationCard *card : view.findChildren<ConversationCard *>())
    if (stableKey(card->data().key) == key)
      return card;
  return nullptr;
}

QJsonObject statsJson(const SampleStats &stats) {
  return {{"medianMicroseconds", stats.median},
          {"p95Microseconds", stats.p95},
          {"maximumMicroseconds", stats.maximum}};
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  BenchmarkApplication application(argc, argv);
  using namespace codexui::codex::middle;
  const QString activeApplicationStyle = qApp->style()->name();
  const QString activeApplicationStyleClass =
      QString::fromLatin1(qApp->style()->metaObject()->className());
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());

  const std::size_t count =
      argc > 1 ? std::max<std::size_t>(1, std::strtoull(argv[1], nullptr, 10))
               : 320;
  const qreal expectedDpr =
      argc > 2 ? std::max(0.1, std::strtod(argv[2], nullptr)) : 1.0;
  QStringList failures;
  const auto require = [&failures](bool condition, QString message) {
    if (!condition) {
      failures.push_back(std::move(message));
      std::cerr << "PERFORMANCE GATE FAILED: " << failures.back().toStdString()
                << '\n';
    }
  };

  EventWorkProbe events;
  qApp->installEventFilter(&events);
  codexui::nodegraph::NodeGraph structuralCommitGraph;
  codexui::nodegraph::NodeRef structuralRootA;
  codexui::nodegraph::NodeRef structuralRootB;
  if (count >= 10'000) {
    auto write = structuralCommitGraph.write();
    structuralRootA =
        write.upsert({codexui::nodegraph::NodeKind::Item, "benchmark-root-a"});
    structuralRootB =
        write.upsert({codexui::nodegraph::NodeKind::Item, "benchmark-root-b"});
    static_cast<void>(write.finish());
  }
  QWidget benchmarkWindow;
  benchmarkWindow.resize(1200, 700);
  ConversationView view(&benchmarkWindow);
  DocumentTracker documents;
  ConstructionTracker constructions;
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.commandsInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(900, 700);
  view.show();
  benchmarkWindow.show();
  processFrame();
  const qreal actualDpr = view.devicePixelRatioF();
  const QScreen *screen = view.screen();
  const QString platformName = QGuiApplication::platformName();
  const QString screenName = screen ? screen->name() : QString{};
  const qreal screenDpr = screen ? screen->devicePixelRatio() : 0.0;
  const qreal screenRefreshRate = screen ? screen->refreshRate() : 0.0;
  const bool benchmarkWindowExposed =
      benchmarkWindow.windowHandle() &&
      benchmarkWindow.windowHandle()->isExposed();
  const qint64 startupResidentKiB = currentResidentKiB();
  application.resetEventTimings();

  ConversationSnapshot canonical = snapshot(count);
  if (structuralRootA)
    canonical.sections.front().cards.front().target = structuralRootA;
  ConversationSnapshot initialInput = canonical;
  const qulonglong stagePassesBefore =
      counter(view, "structuralStageCardPasses");
  const qulonglong constructionsBefore =
      counter(view, "conversationCardConstructions");
  const qulonglong admissionsBefore =
      counter(view, "conversationCardAdmissionPasses");
  bool publicationObserved = false;
  bool publicationValid = false;
  bool publicationOverscanComplete = false;
  qint64 initialMilliseconds = 0;
  qulonglong initialLayoutRequests = 0;
  qulonglong initialPaints = 0;
  qulonglong firstFrameStagePasses = 0;
  qulonglong firstFrameSynchronousConstructions = 0;
  qulonglong firstFrameAdmissions = 0;
  Census firstFrame;
  CardSnapshot firstFrameCards;
  QWidget *firstFrameFocus = nullptr;
  quint64 firstFramePixels = 0;
  events.reset();
  QElapsedTimer initial;
  initial.start();
  view.setReconciliationFinishedAction(
      [&](const std::string &threadId,
          ConversationView::ReconciliationResult reconciliation,
          bool selectionCommitted) {
        if (threadId != "benchmark-thread")
          return;
        publicationObserved = true;
        publicationValid = changed(reconciliation) && selectionCommitted;
        initialMilliseconds = initial.elapsed();
        initialLayoutRequests = events.layoutRequests;
        initialPaints = events.paints;
        firstFrameStagePasses =
            counter(view, "structuralStageCardPasses") - stagePassesBefore;
        firstFrameSynchronousConstructions =
            counter(view, "conversationCardConstructions") -
            constructionsBefore;
        firstFrameAdmissions =
            counter(view, "conversationCardAdmissionPasses") - admissionsBefore;
        firstFrame = inspect(view);
        publicationOverscanComplete = residencyMatchesViewportWindow(view);
        firstFrameCards = captureCards(view);
        firstFrameFocus = QApplication::focusWidget();
        firstFramePixels = pixelChecksum(view.viewport()->grab().toImage());
        events.beginAdmissionProbe(view);
      });
  static_cast<void>(view.reconcileStaged(std::move(initialInput)));
  const bool stagingDeferred = view.structuralStagingActive() &&
                               view.conversationModel()->rowCount() == 0 &&
                               view.materializedCardCount() == 0;
  QElapsedTimer publicationDeadline;
  publicationDeadline.start();
  while (!publicationObserved && publicationDeadline.elapsed() < 120000) {
    processEventsAndDeferredDeletes(20);
    constructions.observe(view);
  }
  const bool staged = publicationObserved && !view.structuralStagingActive();
  const bool initialResidencySettled = waitForResidency(view, &constructions);
  const qulonglong initialMaximumConstructionsPerAdmissionPass =
      events.endAdmissionProbe();
  const Census settledInitial = inspect(view);
  const bool firstFrameStable =
      retainsCards(view, firstFrameCards) &&
      QApplication::focusWidget() == firstFrameFocus &&
      pixelChecksum(view.viewport()->grab().toImage()) == firstFramePixels;
  const qint64 initialResidentKiB = currentResidentKiB();
  Census peak = firstFrame;
  accumulate(peak, settledInitial);
  bool exactResidency = initialResidencySettled;
  const qulonglong deferredInitialConstructions =
      counter(view, "conversationCardConstructions") - constructionsBefore -
      firstFrameSynchronousConstructions;
  const qulonglong deferredInitialAdmissionPasses =
      counter(view, "conversationCardAdmissionPasses") - admissionsBefore -
      firstFrameAdmissions;

  SampleStats structuralPlanStats;
  bool structuralPlanCorrect = count < 10'000;
  std::size_t structuralPlanSampleCount = 0;
  if (count >= 10'000) {
    constexpr std::size_t StructuralRows = 64;
    constexpr int StructuralSamples = 32;
    const std::size_t sectionCount = count / CardsPerTurn;
    codexui::nodegraph::NodeGraph graph;
    std::vector<codexui::nodegraph::NodeRef> targets;
    targets.reserve(StructuralRows);
    {
      auto write = graph.write();
      for (std::size_t sample = 0; sample < StructuralRows; ++sample)
        targets.push_back(
            write.upsert({codexui::nodegraph::NodeKind::Item,
                          "benchmark-planned-" + std::to_string(sample)}));
      static_cast<void>(write.finish());
    }
    std::vector<ConversationRowChange> rows;
    rows.reserve(StructuralRows);
    for (std::size_t sample = 0; sample < StructuralRows; ++sample) {
      const std::size_t section = sample * sectionCount / StructuralRows;
      const int nextRow = static_cast<int>(section * CardsPerTurn + 5);
      const VisibleCardData *previous =
          view.conversationModel()->card(nextRow - 1);
      const VisibleCardData *next = view.conversationModel()->card(nextRow);
      const std::string item = "planned-" + std::to_string(sample);
      VisibleCardData inserted{
          AuthoritativeItemKey{"benchmark-thread", previous->turnId, item},
          CardKind::AgentMessage,
          "benchmark-thread",
          previous->turnId,
          item,
          AgentMessageData{"Planned row", true}};
      inserted.target = targets[sample];
      rows.push_back(
          {{std::move(inserted), "turn-section-" + std::to_string(section),
            false, true, false},
           previous->key,
           next->key});
    }

    const int modelRowsBefore = view.conversationModel()->rowCount();
    const std::string firstKey = view.conversationModel()->row(0)->stableKey;
    const std::string lastKey =
        view.conversationModel()->row(modelRowsBefore - 1)->stableKey;
    const qulonglong indexRebuilds =
        counter(*view.conversationModel(), "modelIndexRebuildCount");
    const qulonglong modelResets =
        counter(*view.conversationModel(), "modelResetCount");
    const qulonglong exactInserts =
        counter(*view.conversationModel(), "modelExactInsertCount");
    const qulonglong exactMoves =
        counter(*view.conversationModel(), "modelExactMoveCount");
    const qulonglong exactRemovals =
        counter(*view.conversationModel(), "modelExactRemoveCount");
    const qulonglong graphRefreshes = counter(view, "graphRefreshPasses");
    const qulonglong geometryPasses =
        counter(view, "conversationLocalGeometryPasses");
    const qulonglong materializationPasses =
        counter(view, "conversationMaterializationPasses");
    const qulonglong cardConstructions =
        counter(view, "conversationCardConstructions");

    std::vector<qint64> timings;
    timings.reserve(StructuralSamples);
    for (int sample = -1; sample < StructuralSamples; ++sample) {
      QElapsedTimer timer;
      timer.start();
      const auto plan = view.conversationModel()->planStructuralDelta(rows, {});
      const qint64 elapsed = timer.nsecsElapsed() / 1000;
      structuralPlanCorrect = plan && plan->operations.size() == StructuralRows;
      std::vector<bool> seen(StructuralRows, false);
      std::vector<std::size_t> insertedSections;
      insertedSections.reserve(StructuralRows);
      if (plan) {
        for (const auto &operation : plan->operations) {
          if (operation.deltaIndex >= StructuralRows ||
              operation.kind != ConversationItemModel::StructuralDeltaPlan::
                                    Operation::Kind::Place ||
              seen[operation.deltaIndex] || operation.tailAppend) {
            structuralPlanCorrect = false;
            break;
          }
          seen[operation.deltaIndex] = true;
          const std::size_t section =
              operation.deltaIndex * sectionCount / StructuralRows;
          const int priorInsertions = static_cast<int>(std::ranges::count_if(
              insertedSections,
              [section](std::size_t prior) { return prior < section; }));
          const int expectedDestination =
              static_cast<int>(section * CardsPerTurn + 5) + priorInsertions;
          if (operation.destination != expectedDestination) {
            structuralPlanCorrect = false;
            break;
          }
          insertedSections.push_back(section);
        }
      }
      if (sample >= 0)
        timings.push_back(elapsed);
      if (!structuralPlanCorrect)
        break;
    }
    structuralPlanSampleCount = timings.size();
    structuralPlanStats = statistics(std::move(timings));
    structuralPlanCorrect =
        structuralPlanCorrect &&
        structuralPlanSampleCount ==
            static_cast<std::size_t>(StructuralSamples) &&
        view.conversationModel()->rowCount() == modelRowsBefore &&
        view.conversationModel()->indexForStableKey(firstKey).row() == 0 &&
        view.conversationModel()->indexForStableKey(lastKey).row() ==
            modelRowsBefore - 1 &&
        counter(*view.conversationModel(), "modelIndexRebuildCount") ==
            indexRebuilds &&
        counter(*view.conversationModel(), "modelResetCount") == modelResets &&
        counter(*view.conversationModel(), "modelExactInsertCount") ==
            exactInserts &&
        counter(*view.conversationModel(), "modelExactMoveCount") ==
            exactMoves &&
        counter(*view.conversationModel(), "modelExactRemoveCount") ==
            exactRemovals &&
        counter(view, "graphRefreshPasses") == graphRefreshes &&
        counter(view, "conversationLocalGeometryPasses") == geometryPasses &&
        counter(view, "conversationMaterializationPasses") ==
            materializationPasses &&
        counter(view, "conversationCardConstructions") == cardConstructions;
  }

  SampleStats structuralCommitStats;
  bool structuralCommitCorrect = count < 10'000;
  std::size_t structuralCommitSampleCount = 0;
  qulonglong structuralCommitMaximumLayoutRequests = 0;
  qulonglong structuralCommitMaximumPaints = 0;
  if (count >= 10'000) {
    constexpr int StructuralCommitSamples = 32;
    const VisibleCardData rootA = canonical.sections.front().cards.front();
    VisibleCardData rootB = rootA;
    rootB.key =
        AuthoritativeItemKey{"benchmark-thread", "turn-0", "replacement-root"};
    rootB.itemId = "replacement-root";
    rootB.target = structuralRootB;
    const CardKey nextKey = canonical.sections.front().cards[1].key;
    const auto visibleAnchor = [&view] {
      for (int y = 0; y < view.viewport()->height(); ++y) {
        const QModelIndex index =
            view.indexAt(QPoint(view.viewport()->width() / 2, y));
        if (!index.isValid())
          continue;
        return std::pair{index.data(ConversationItemModel::StableKeyRole)
                             .toString()
                             .toStdString(),
                         view.visualRect(index).top()};
      }
      return std::pair<std::string, int>{};
    };
    const QModelIndex focusedIndex =
        view.indexAt(view.viewport()->rect().center());
    if (focusedIndex.isValid()) {
      view.setCurrentIndex(focusedIndex);
      view.selectionModel()->select(focusedIndex,
                                    QItemSelectionModel::ClearAndSelect |
                                        QItemSelectionModel::Rows);
    }
    view.setFocus(Qt::TabFocusReason);
    processFrame();
    const auto anchorBefore = visibleAnchor();
    const QPersistentModelIndex persistentFocus(focusedIndex);
    const QPersistentModelIndex persistentLast(view.conversationModel()->index(
        view.conversationModel()->rowCount() - 1));
    QWidget *const focusBefore = QApplication::focusWidget();
    const CardSnapshot cardsBefore = captureCards(view);
    const Census censusBefore = inspect(view);
    const quint64 pixelsBefore =
        pixelChecksum(view.viewport()->grab().toImage());
    bool rootAActive = true;
    std::vector<qint64> timings;
    timings.reserve(StructuralCommitSamples);
    structuralCommitCorrect = !anchorBefore.first.empty() &&
                              persistentFocus.isValid() &&
                              persistentLast.isValid();

    for (int sample = -2;
         structuralCommitCorrect && sample < StructuralCommitSamples;
         ++sample) {
      const bool nextIsA = !rootAActive;
      ConversationDelta delta;
      delta.threadId = "benchmark-thread";
      delta.removals.push_back(rootAActive ? structuralRootA : structuralRootB);
      delta.rows.push_back(
          {{nextIsA ? rootA : rootB, "turn-section-0", true, false, false},
           {},
           nextKey});

      const qulonglong graphRefreshes = counter(view, "graphRefreshPasses");
      const qulonglong materializationPasses =
          counter(view, "conversationMaterializationPasses");
      const qulonglong geometryPasses =
          counter(view, "conversationLocalGeometryPasses");
      const qulonglong constructionsBeforeCommit =
          counter(view, "conversationCardConstructions");
      const qulonglong releasesBeforeCommit =
          counter(view, "conversationRowsReleased");
      const qulonglong heightRebuilds =
          counter(view, "conversationHeightIndexRebuilds");
      const qulonglong sectionRebuilds =
          counter(view, "conversationSectionRangeRebuilds");
      const qulonglong indexRebuilds =
          counter(*view.conversationModel(), "modelIndexRebuildCount");
      const qulonglong modelResets =
          counter(*view.conversationModel(), "modelResetCount");
      const qulonglong exactInserts =
          counter(*view.conversationModel(), "modelExactInsertCount");
      const qulonglong exactRemovals =
          counter(*view.conversationModel(), "modelExactRemoveCount");
      const qulonglong sectionRows = counter(
          *view.conversationModel(), "modelSectionStructureRowsTouched");
      events.reset();
      QElapsedTimer timer;
      timer.start();
      const auto applied = view.applyConversationDelta(std::move(delta));
      processFrame();
      const qint64 elapsed = timer.nsecsElapsed() / 1000;
      if (sample >= 0)
        timings.push_back(elapsed);
      structuralCommitMaximumLayoutRequests = std::max(
          structuralCommitMaximumLayoutRequests, events.layoutRequests);
      structuralCommitMaximumPaints =
          std::max(structuralCommitMaximumPaints, events.paints);
      rootAActive = nextIsA;
      const codexui::nodegraph::NodeRef expectedRoot =
          rootAActive ? structuralRootA : structuralRootB;
      const Census censusAfter = inspect(view);
      structuralCommitCorrect =
          applied &&
          view.conversationModel()->rowCount() == static_cast<int>(count) &&
          view.conversationModel()->indexForTarget(expectedRoot).row() == 0 &&
          !view.conversationModel()
               ->indexForTarget(rootAActive ? structuralRootB : structuralRootA)
               .isValid() &&
          persistentFocus.isValid() && persistentLast.isValid() &&
          persistentLast.row() == static_cast<int>(count) - 1 &&
          view.currentIndex() == persistentFocus &&
          view.selectionModel()->isSelected(persistentFocus) &&
          QApplication::focusWidget() == focusBefore &&
          visibleAnchor() == anchorBefore && sameCards(view, cardsBefore) &&
          censusAfter.cards == censusBefore.cards &&
          censusAfter.documents == censusBefore.documents &&
          censusAfter.widgets == censusBefore.widgets &&
          pixelChecksum(view.viewport()->grab().toImage()) == pixelsBefore &&
          counter(view, "graphRefreshPasses") == graphRefreshes + 1 &&
          counter(view, "conversationMaterializationPasses") ==
              materializationPasses + 1 &&
          counter(view, "conversationLocalGeometryPasses") == geometryPasses &&
          counter(view, "conversationCardConstructions") ==
              constructionsBeforeCommit &&
          counter(view, "conversationRowsReleased") == releasesBeforeCommit &&
          counter(view, "conversationHeightIndexRebuilds") == heightRebuilds &&
          counter(view, "conversationSectionRangeRebuilds") ==
              sectionRebuilds &&
          counter(*view.conversationModel(), "modelIndexRebuildCount") ==
              indexRebuilds &&
          counter(*view.conversationModel(), "modelResetCount") ==
              modelResets &&
          counter(*view.conversationModel(), "modelExactInsertCount") ==
              exactInserts + 1 &&
          counter(*view.conversationModel(), "modelExactRemoveCount") ==
              exactRemovals + 1 &&
          counter(*view.conversationModel(),
                  "modelSectionStructureRowsTouched") -
                  sectionRows <=
              7;
    }
    structuralCommitSampleCount = timings.size();
    structuralCommitStats = statistics(std::move(timings));
    structuralCommitCorrect =
        structuralCommitCorrect && rootAActive &&
        structuralCommitSampleCount ==
            static_cast<std::size_t>(StructuralCommitSamples);
  }

  QScrollBar *const scrollBar = view.verticalScrollBar();
  const int paintedCenter = scrollBar->maximum() / 2;
  for (const int value :
       {paintedCenter - 80, paintedCenter + 80, paintedCenter}) {
    scrollBar->setValue(
        std::clamp(value, scrollBar->minimum(), scrollBar->maximum()));
    processFrame();
    exactResidency &= waitForResidency(view, &constructions);
  }
  processFrame();

  application.setEventTimingsEnabled(false);
  qApp->removeEventFilter(&events);
  PaintedScrollWork paintedWarm;
  for (int sample = 0; sample < PaintedScrollSamples; ++sample)
    samplePaintedWheel(application, view, {},
                       QPoint(0, sample % 2 == 0 ? -15 : 15), Qt::NoScrollPhase,
                       paintedWarm, peak, constructions);
  exactResidency &= waitForResidency(view, &constructions);

  scrollBar->setValue(paintedCenter);
  processFrame();
  exactResidency &= waitForResidency(view, &constructions);
  PaintedScrollWork paintedScrollBarWarm;
  for (int sample = 0; sample < PaintedScrollSamples; ++sample)
    samplePaintedScrollBar(application, view, scrollBar->value() + 1,
                           paintedScrollBarWarm, peak, constructions);
  exactResidency &= waitForResidency(view, &constructions);

  scrollBar->setValue(paintedCenter);
  processFrame();
  exactResidency &= waitForResidency(view, &constructions);
  PaintedScrollWork paintedScrollBarStepWork;
  int paintedScrollBarDirection = 1;
  const int paintedScrollBarStep = std::max(1, view.viewport()->height() / 3);
  for (int sample = 0; sample < PaintedScrollSamples; ++sample) {
    if (paintedScrollBarDirection > 0 &&
        scrollBar->value() >= scrollBar->maximum() - scrollBar->pageStep())
      paintedScrollBarDirection = -1;
    else if (paintedScrollBarDirection < 0 &&
             scrollBar->value() <= scrollBar->minimum() + scrollBar->pageStep())
      paintedScrollBarDirection = 1;
    samplePaintedScrollBar(
        application, view,
        std::clamp(scrollBar->value() +
                       paintedScrollBarDirection * paintedScrollBarStep,
                   scrollBar->minimum(), scrollBar->maximum()),
        paintedScrollBarStepWork, peak, constructions);
  }
  exactResidency &= waitForResidency(view, &constructions);

  scrollBar->setValue(paintedCenter);
  processFrame();
  exactResidency &= waitForResidency(view, &constructions);
  PaintedScrollWork paintedAngle;
  int paintedAngleDelta = -120;
  for (int sample = 0; sample < PaintedScrollSamples; ++sample) {
    if (paintedAngleDelta < 0 &&
        scrollBar->value() >= scrollBar->maximum() - scrollBar->pageStep())
      paintedAngleDelta = 120;
    else if (paintedAngleDelta > 0 &&
             scrollBar->value() <= scrollBar->minimum() + scrollBar->pageStep())
      paintedAngleDelta = -120;
    samplePaintedWheel(application, view, {}, QPoint(0, paintedAngleDelta),
                       Qt::NoScrollPhase, paintedAngle, peak, constructions);
  }
  exactResidency &= waitForResidency(view, &constructions);
  qApp->installEventFilter(&events);
  application.setEventTimingsEnabled(true);

  scrollBar->setValue(scrollBar->maximum() / 2);
  processFrame();
  bool warmedWheelSettled = waitForResidency(view, &constructions);
  WarmedScrollWork warmedWheel;
  std::set<int> warmedWheelValues;
  int wheelAngleDelta = -120;
  for (int sample = 0; sample < WarmedScrollSamples; ++sample) {
    if (wheelAngleDelta < 0 &&
        scrollBar->value() >= scrollBar->maximum() - scrollBar->pageStep())
      wheelAngleDelta = 120;
    else if (wheelAngleDelta > 0 &&
             scrollBar->value() <= scrollBar->minimum() + scrollBar->pageStep())
      wheelAngleDelta = -120;
    sampleWarmedWheel(view, wheelAngleDelta, warmedWheel, peak, events,
                      constructions);
    warmedWheelValues.insert(scrollBar->value());
  }
  warmedWheelSettled =
      waitForResidency(view, &constructions) && warmedWheelSettled;

  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  processFrame();
  ScrollWork normal;
  int direction = 1;
  const int normalStep = std::max(1, view.viewport()->height() / 3);
  for (int sample = 0; sample < NormalScrollSamples; ++sample) {
    QScrollBar *bar = view.verticalScrollBar();
    int next = bar->value() + direction * normalStep;
    if (next >= bar->maximum()) {
      next = bar->maximum();
      direction = -1;
    } else if (next <= bar->minimum()) {
      next = bar->minimum();
      direction = 1;
    }
    sampleScroll(view, next, normal, peak, events, constructions);
  }
  const qint64 normalResidentKiB = currentResidentKiB();
  exactResidency &= waitForResidency(view, &constructions);

  ScrollWork seeks;
  std::set<int> visitedSeekValues;
  std::set<quint64> seekPixelChecksums;
  for (int sample = 0; sample < SeekSamples; ++sample) {
    const int permutation = (sample * 17) % SeekSamples;
    const int targetRow = static_cast<int>(
        (count - 1) * static_cast<std::size_t>(permutation) /
        static_cast<std::size_t>(std::max(1, SeekSamples - 1)));
    const QModelIndex targetIndex = view.conversationModel()->index(targetRow);
    const QRect targetGeometry = view.visualRect(targetIndex);
    const int target = view.verticalScrollBar()->value() +
                       targetGeometry.center().y() -
                       view.viewport()->height() / 2;
    sampleScroll(view, target, seeks, peak, events, constructions, targetIndex);
    visitedSeekValues.insert(view.verticalScrollBar()->value());
    if (sample % 6 == 0)
      seekPixelChecksums.insert(
          pixelChecksum(view.viewport()->grab().toImage()));
  }
  const qint64 seekResidentKiB = currentResidentKiB();
  exactResidency &= waitForResidency(view, &constructions);

  const quint64 noOpWarmChecksum =
      pixelChecksum(view.viewport()->grab().toImage());
  processFrame();
  const QImage noOpPixelsBefore = view.viewport()->grab().toImage();
  processFrame();
  const bool noOpPreconditionStable =
      noOpWarmChecksum == pixelChecksum(noOpPixelsBefore);
  documents.observe(view);
  const CardSnapshot beforeNoOp = captureCards(view);
  QWidget *const noOpFocus = QApplication::focusWidget();
  const int noOpScroll = view.verticalScrollBar()->value();
  const qulonglong noOpConstructions =
      counter(view, "conversationCardConstructions");
  const qulonglong noOpGeometry =
      counter(view, "conversationLocalGeometryPasses");
  const qulonglong noOpMaterializations =
      counter(view, "conversationMaterializationPasses");
  ConversationSnapshot noOpInput = canonical;
  documents.reset();
  events.reset();
  QElapsedTimer noOp;
  noOp.start();
  const bool noOpChanged = changed(view.reconcile(std::move(noOpInput)));
  processFrame();
  const qint64 noOpMicroseconds = noOp.nsecsElapsed() / 1000;
  const qulonglong noOpLayoutRequests = events.layoutRequests;
  const qulonglong noOpPaints = events.paints;
  const QImage noOpPixelsAfter = view.viewport()->grab().toImage();
  const bool noOpStable =
      noOpPreconditionStable && !noOpChanged && sameCards(view, beforeNoOp) &&
      noOpPixelsAfter == noOpPixelsBefore &&
      QApplication::focusWidget() == noOpFocus && documents.changes == 0 &&
      noOpLayoutRequests == 0 && noOpPaints == 0 &&
      view.verticalScrollBar()->value() == noOpScroll &&
      counter(view, "conversationCardConstructions") == noOpConstructions &&
      counter(view, "conversationLocalGeometryPasses") == noOpGeometry &&
      counter(view, "conversationMaterializationPasses") ==
          noOpMaterializations;

  // Row 1 deliberately contains a large immutable Markdown prefix; every
  // profile therefore measures tail-only streaming against the same document.
  const std::size_t streamRow = count > 1 ? 1 : 0;
  const QModelIndex streamIndex =
      view.conversationModel()->index(static_cast<int>(streamRow));
  view.scrollTo(streamIndex, QAbstractItemView::PositionAtCenter);
  processFrame();
  exactResidency &= waitForResidency(view, &constructions);
  const VisibleCardData *streamModel =
      view.conversationModel()->card(static_cast<int>(streamRow));
  VisibleCardData streamData = streamModel ? *streamModel : cardData(streamRow);
  const std::string streamKey = stableKey(streamData.key);
  ConversationCard *streamCard = cardForKey(view, streamKey);
  MarkdownTextView *streamBody =
      streamCard ? streamCard->findChild<MarkdownTextView *>() : nullptr;
  QTextDocument *streamDocument = streamBody ? streamBody->document() : nullptr;
  qulonglong streamConstructions = 0;
  std::vector<qint64> streamTimings;
  std::vector<qint64> streamApplyTimings;
  qulonglong streamDocumentChanges = 0;
  qulonglong streamTailLocalityChecks = 0;
  qulonglong streamMaximumLayoutRequests = 0;
  qulonglong streamMaximumPaints = 0;
  int streamEarliestChange = std::numeric_limits<int>::max();
  bool streamTailLocal = true;
  bool streamAccepted =
      streamCard && streamBody && streamData.kind == CardKind::AgentMessage;
  QMetaObject::Connection streamChangeConnection;
  documents.observe(view);
  PaintedScrollWork paintedStreaming;
  int streamingAngleDelta = -120;
  for (int sample = 0; sample < StreamSamples && streamAccepted; ++sample) {
    QScrollBar *const bar = view.verticalScrollBar();
    if (streamingAngleDelta < 0 &&
        bar->value() >= bar->maximum() - bar->pageStep())
      streamingAngleDelta = 120;
    else if (streamingAngleDelta > 0 &&
             bar->value() <= bar->minimum() + bar->pageStep())
      streamingAngleDelta = -120;
    std::get<AgentMessageData>(streamData.payload).text +=
        " concurrent-scroll-delta-" + std::to_string(sample);
    const QPointF local = view.viewport()->rect().center();
    QWheelEvent wheel(local, view.viewport()->mapToGlobal(local.toPoint()), {},
                      QPoint(0, streamingAngleDelta), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false,
                      Qt::MouseEventSynthesizedByApplication,
                      QPointingDevice::primaryPointingDevice());
    samplePaintedAction(application, view, paintedStreaming, peak, constructions,
                        [&] {
                          const bool applied =
                              applyPresentation(view, streamData).has_value();
                          return applied &&
                                 QApplication::sendEvent(view.viewport(),
                                                         &wheel) &&
                                 wheel.isAccepted();
                        });
  }
  exactResidency &= waitForResidency(view, &constructions);
  view.scrollTo(streamIndex, QAbstractItemView::PositionAtCenter);
  processFrame();
  exactResidency &= waitForResidency(view, &constructions);
  streamCard = cardForKey(view, streamKey);
  streamBody =
      streamCard ? streamCard->findChild<MarkdownTextView *>() : nullptr;
  streamDocument = streamBody ? streamBody->document() : nullptr;
  streamAccepted = streamCard && streamBody && streamDocument;
  streamConstructions = counter(view, "conversationCardConstructions");
  streamChangeConnection =
      streamDocument
          ? QObject::connect(streamDocument, &QTextDocument::contentsChange,
                             &view,
                             [&streamEarliestChange](int position, int, int) {
                               streamEarliestChange =
                                   std::min(streamEarliestChange, position);
                             })
          : QMetaObject::Connection{};
  for (int sample = 0; sample < StreamSamples && streamAccepted; ++sample) {
    const presentation::MarkdownTailState tailBefore =
        streamBody->markdownTailState();
    const int firstBlockRevision = streamDocument->begin().revision();
    std::get<AgentMessageData>(streamData.payload).text +=
        " streaming-delta-" + std::to_string(sample);
    documents.reset();
    events.reset();
    streamEarliestChange = std::numeric_limits<int>::max();
    QElapsedTimer timer;
    timer.start();
    streamAccepted = applyPresentation(view, streamData).has_value();
    streamApplyTimings.push_back(timer.nsecsElapsed() / 1000);
    processFrame();
    streamTimings.push_back(timer.nsecsElapsed() / 1000);
    streamDocumentChanges += documents.changes;
    streamMaximumLayoutRequests =
        std::max(streamMaximumLayoutRequests, events.layoutRequests);
    streamMaximumPaints = std::max(streamMaximumPaints, events.paints);
    ++streamTailLocalityChecks;
    streamTailLocal = streamTailLocal && tailBefore.valid() &&
                      streamEarliestChange != std::numeric_limits<int>::max() &&
                      streamEarliestChange >= tailBefore.documentPosition &&
                      streamDocument->begin().revision() == firstBlockRevision;
    streamAccepted = streamAccepted &&
                     cardForKey(view, streamKey) == streamCard &&
                     streamBody->document() == streamDocument;
    constructions.observe(view);
    accumulate(peak, inspect(view));
  }
  QObject::disconnect(streamChangeConnection);
  const qulonglong streamingConstructions =
      counter(view, "conversationCardConstructions") - streamConstructions;

  const std::size_t commandRow = std::min(count - 1, streamRow + 1);
  view.scrollTo(view.conversationModel()->index(static_cast<int>(commandRow)),
                QAbstractItemView::PositionAtCenter);
  processFrame();
  exactResidency &= waitForResidency(view, &constructions);
  const VisibleCardData *commandModel =
      view.conversationModel()->card(static_cast<int>(commandRow));
  VisibleCardData commandData =
      commandModel ? *commandModel : cardData(commandRow);
  const std::string commandKey = stableKey(commandData.key);
  ConversationCard *commandCard = cardForKey(view, commandKey);
  CommandOutputView *commandOutput =
      commandCard ? commandCard->findChild<CommandOutputView *>() : nullptr;
  QTextDocument *commandDocument =
      commandOutput ? commandOutput->document() : nullptr;
  const qulonglong commandConstructions =
      counter(view, "conversationCardConstructions");
  std::vector<qint64> commandTimings;
  qulonglong commandDocumentChanges = 0;
  qulonglong commandMaximumLayoutRequests = 0;
  qulonglong commandMaximumPaints = 0;
  bool commandAccepted = commandCard && commandOutput &&
                         commandData.kind == CardKind::CommandExecution;
  documents.observe(view);
  for (int sample = 0; sample < StreamSamples && commandAccepted; ++sample) {
    std::get<CommandExecutionData>(commandData.payload).output +=
        "\nfollow-tail output delta " + std::to_string(sample);
    documents.reset();
    events.reset();
    QElapsedTimer timer;
    timer.start();
    commandAccepted = applyPresentation(view, commandData).has_value();
    processFrame();
    commandTimings.push_back(timer.nsecsElapsed() / 1000);
    commandDocumentChanges += documents.changes;
    commandMaximumLayoutRequests =
        std::max(commandMaximumLayoutRequests, events.layoutRequests);
    commandMaximumPaints = std::max(commandMaximumPaints, events.paints);
    commandAccepted = commandAccepted &&
                      cardForKey(view, commandKey) == commandCard &&
                      commandOutput->document() == commandDocument &&
                      commandOutput->followsLatest() &&
                      commandOutput->verticalScrollBar()->value() ==
                          commandOutput->verticalScrollBar()->maximum();
    constructions.observe(view);
    accumulate(peak, inspect(view));
  }
  const qulonglong commandStreamingConstructions =
      counter(view, "conversationCardConstructions") - commandConstructions;

  const qulonglong modelRebuildsBefore =
      counter(*view.conversationModel(), "modelIndexRebuildCount");
  const qulonglong sectionRebuildsBefore =
      counter(view, "conversationSectionRangeRebuilds");
  const qulonglong heightRebuildsBefore =
      counter(view, "conversationHeightIndexRebuilds");
  const qulonglong appendConstructionsBefore =
      counter(view, "conversationCardConstructions");
  ConversationRowPlacement tail;
  tail.card = cardData(count);
  tail.sectionKey = "turn-section-" + std::to_string(count / CardsPerTurn);
  tail.turnRoot = count % CardsPerTurn == 0;
  tail.nested = !tail.turnRoot;
  const std::string tailKey = stableKey(tail.card.key);
  events.reset();
  QElapsedTimer append;
  append.start();
  const bool appendAccepted = appendProjected(view, std::move(tail));
  processFrame();
  const qint64 appendMicroseconds = append.nsecsElapsed() / 1000;
  const qulonglong appendLayoutRequests = events.layoutRequests;
  const qulonglong appendPaints = events.paints;
  const bool appendCorrect =
      appendAccepted &&
      view.conversationModel()->rowCount() == static_cast<int>(count + 1) &&
      view.conversationModel()->indexForStableKey(tailKey).row() ==
          static_cast<int>(count) &&
      counter(*view.conversationModel(), "modelIndexRebuildCount") ==
          modelRebuildsBefore &&
      counter(view, "conversationSectionRangeRebuilds") ==
          sectionRebuildsBefore &&
      counter(view, "conversationHeightIndexRebuilds") == heightRebuildsBefore;
  const qulonglong appendConstructions =
      counter(view, "conversationCardConstructions") -
      appendConstructionsBefore;
  constructions.observe(view);
  accumulate(peak, inspect(view));
  exactResidency &= waitForResidency(view, &constructions);

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  processFrame();
  bool followingAppendsCorrect =
      view.mode() == ConversationView::Mode::Following && view.isAtBottom();
  std::vector<qint64> followingAppendTimings;
  qulonglong followingAppendMaximumConstructions = 0;
  qulonglong followingAppendMaximumLayoutRequests = 0;
  qulonglong followingAppendMaximumPaints = 0;
  std::string followingTailKey = tailKey;
  for (int offset = 1; offset <= FollowingAppendSamples; ++offset) {
    const std::size_t row = count + offset;
    ConversationRowPlacement followingTail;
    followingTail.card = cardData(row);
    followingTail.sectionKey =
        "turn-section-" + std::to_string(row / CardsPerTurn);
    followingTail.turnRoot = row % CardsPerTurn == 0;
    followingTail.nested = !followingTail.turnRoot;
    followingTailKey = stableKey(followingTail.card.key);
    const qulonglong constructionsBefore =
        counter(view, "conversationCardConstructions");
    events.reset();
    QElapsedTimer timer;
    timer.start();
    const bool accepted = appendProjected(view, std::move(followingTail));
    processFrame();
    followingAppendTimings.push_back(timer.nsecsElapsed() / 1000);
    followingAppendMaximumConstructions = std::max(
        followingAppendMaximumConstructions,
        counter(view, "conversationCardConstructions") - constructionsBefore);
    followingAppendMaximumLayoutRequests =
        std::max(followingAppendMaximumLayoutRequests, events.layoutRequests);
    followingAppendMaximumPaints =
        std::max(followingAppendMaximumPaints, events.paints);
    const QModelIndex appended =
        view.conversationModel()->indexForStableKey(followingTailKey);
    followingAppendsCorrect =
        followingAppendsCorrect && accepted && appended.isValid() &&
        view.mode() == ConversationView::Mode::Following && view.isAtBottom();
    constructions.observe(view);
    accumulate(peak, inspect(view));
  }
  followingAppendsCorrect =
      followingAppendsCorrect && cardForKey(view, followingTailKey) != nullptr;
  exactResidency &= waitForResidency(view, &constructions);

  const qulonglong resizeRebuildsBefore =
      counter(view, "conversationHeightIndexRebuilds");
  const qulonglong resizeFrameReflowsBefore =
      counter(view, "conversationInteractiveResizeFrameReflows");
  const qulonglong resizeExactReflowsBefore =
      counter(view, "conversationInteractiveResizeExactReflows");
  std::vector<qint64> resizeFrameTimings;
  qulonglong resizeMaximumLayoutRequests = 0;
  qulonglong resizeMaximumPaints = 0;
  QElapsedTimer resize;
  resize.start();
  view.beginInteractiveResize();
  events.reset();
  QElapsedTimer resizeEntryTimer;
  resizeEntryTimer.start();
  for (int event = 0; event < 3; ++event)
    view.resize(760 + event * 3, 700);
  waitForFrameTimer();
  const qint64 resizeEntryMicroseconds = resizeEntryTimer.nsecsElapsed() / 1000;
  resizeMaximumLayoutRequests = events.layoutRequests;
  resizeMaximumPaints = events.paints;
  for (int frame = 0; frame < ResizeFrameSamples; ++frame) {
    events.reset();
    QElapsedTimer frameTimer;
    frameTimer.start();
    for (int event = 0; event < 3; ++event)
      view.resize(780 + frame * 20 + event * 3, 700);
    waitForFrameTimer();
    resizeFrameTimings.push_back(frameTimer.nsecsElapsed() / 1000);
    resizeMaximumLayoutRequests =
        std::max(resizeMaximumLayoutRequests, events.layoutRequests);
    resizeMaximumPaints = std::max(resizeMaximumPaints, events.paints);
  }
  QElapsedTimer exactResize;
  exactResize.start();
  view.endInteractiveResize();
  processFrame();
  const qint64 exactResizeMicroseconds = exactResize.nsecsElapsed() / 1000;
  const qint64 resizeMicroseconds = resize.nsecsElapsed() / 1000;
  const bool resizeCorrect =
      counter(view, "conversationHeightIndexRebuilds") ==
          resizeRebuildsBefore &&
      counter(view, "conversationInteractiveResizeFrameReflows") ==
          resizeFrameReflowsBefore + ResizeFrameSamples + 1 &&
      counter(view, "conversationInteractiveResizeExactReflows") ==
          resizeExactReflowsBefore + 1 &&
      !view.property("conversationInteractiveResizeActive").toBool();
  constructions.observe(view);
  accumulate(peak, inspect(view));
  exactResidency &= waitForResidency(view, &constructions);

  const SampleStats paintedWarmStats = statistics(paintedWarm.timings);
  const SampleStats paintedWarmZeroResidencyStats =
      statistics(paintedWarm.zeroResidencyTimings);
  const SampleStats paintedScrollBarWarmStats =
      statistics(paintedScrollBarWarm.timings);
  const SampleStats paintedScrollBarWarmZeroResidencyStats =
      statistics(paintedScrollBarWarm.zeroResidencyTimings);
  const SampleStats paintedScrollBarStepStats =
      statistics(paintedScrollBarStepWork.timings);
  const SampleStats paintedAngleStats = statistics(paintedAngle.timings);
  const SampleStats warmedWheelStats = statistics(warmedWheel.timings);
  const SampleStats normalStats = statistics(normal.timings);
  const SampleStats seekStats = statistics(seeks.timings);
  const SampleStats paintedStreamingStats =
      statistics(paintedStreaming.timings);
  const SampleStats streamStats = statistics(streamTimings);
  const SampleStats commandStats = statistics(commandTimings);
  const SampleStats followingAppendStats = statistics(followingAppendTimings);
  const SampleStats resizeFrameStats = statistics(resizeFrameTimings);
  const qint64 finalResidentKiB = currentResidentKiB();
  const qint64 inheritedPeakResidentKiB = peakResidentKiB();

  SparseVisibilityWork sparseSmall;
  SparseVisibilityWork sparseLarge;
  if (count >= 10'000) {
    sparseSmall = sampleSparseVisibility(20);
    sparseLarge = sampleSparseVisibility(20'000);
  }
  const SampleStats constructionStats = statistics(constructions.samples);
  const SampleStats layoutEventStats = statistics(application.layoutMicros);
  const SampleStats paintEventStats = statistics(application.paintMicros);

  const qint64 timingScale = InstrumentedBuild ? 4 : 1;
  const qint64 initialLimit = timingScale * (count <= 320    ? 250
                                             : count <= 1280 ? 400
                                                             : 750);
  const qint64 noOpLimit = timingScale * (count <= 1280 ? 50000 : 150000);
  const qint64 appendLimit = timingScale * 30000;
  const qint64 sparseVisibilityLimit = timingScale * 250000;
  const auto timingWithin = [timingScale](const SampleStats &stats,
                                          qint64 median, qint64 p95,
                                          qint64 maximum) {
    return stats.median <= timingScale * median &&
           stats.p95 <= timingScale * p95 &&
           stats.maximum <= timingScale * maximum;
  };
  constexpr quint16 EveryCardKind = (1U << 10U) - 1U;
  const bool requiredMetricsPresent =
      hasCounter(view, "structuralStageCardPasses") &&
      hasCounter(view, "conversationCardConstructions") &&
      hasCounter(view, "conversationRowsReleased") &&
      hasCounter(view, "conversationMaterializationPasses") &&
      hasCounter(view, "conversationCardAdmissionPasses") &&
      hasCounter(view, "conversationLocalGeometryPasses") &&
      hasCounter(view, "conversationSectionRangeRebuilds") &&
      hasCounter(view, "conversationHeightIndexRebuilds") &&
      hasCounter(view, "conversationHeightIndexUpdateSteps") &&
      hasCounter(view, "conversationInteractiveResizeFrameReflows") &&
      hasCounter(view, "conversationInteractiveResizeExactReflows") &&
      hasCounter(*view.conversationModel(), "modelIndexRebuildCount");
  const bool rssSupported = startupResidentKiB >= 0 &&
                            initialResidentKiB >= 0 && normalResidentKiB >= 0 &&
                            seekResidentKiB >= 0 && finalResidentKiB >= 0 &&
                            peak.residentKiB >= 0;
  const qint64 residentLimit = InstrumentedBuild ? 1048576 : 196608;
  // Pixel verification concurrently reaches a grab pixmap, retained baseline,
  // and comparison image; gate their DPR-scaled bytes separately from UI state.
  const qint64 dprBufferAllowanceKiB = static_cast<qint64>(
      std::ceil(3.0 * view.width() * view.height() * 4.0 *
                std::max<qreal>(0.0, actualDpr * actualDpr - 1.0) / 1024.0));
  const qint64 residentGrowthLimit =
      (InstrumentedBuild ? 786432 : 65536) + dprBufferAllowanceKiB;
  constexpr int MinimumStageExtent = 44 + 8;
  const qulonglong firstFrameStageBudget = static_cast<qulonglong>(
      (std::max(1, view.viewport()->height()) + MinimumStageExtent - 1) /
          MinimumStageExtent +
      2);
  require(staged, QStringLiteral("staged initial presentation completed"));
  require(stagingDeferred,
          QStringLiteral("staging defers model publication and visible cards"));
  require(publicationValid,
          QStringLiteral("the staged snapshot publishes once as the selected "
                         "authoritative frame"));
  require(requiredMetricsPresent,
          QStringLiteral("every required quantitative metric is present"));
  require(std::abs(actualDpr - expectedDpr) <= 0.02,
          QStringLiteral("actual DPR %1 matches requested DPR %2")
              .arg(actualDpr)
              .arg(expectedDpr));
  require(initialMilliseconds <= initialLimit,
          QStringLiteral("initial presentation <= %1 ms").arg(initialLimit));
  require(firstFrame.viewportRowsRendered &&
              firstFrame.cards == firstFrame.visibleCards &&
              firstFrameStagePasses >=
                  static_cast<qulonglong>(firstFrame.cards) &&
              firstFrameStagePasses <= firstFrameStageBudget &&
              firstFrameSynchronousConstructions == 0 &&
              firstFrameAdmissions == 0 && !publicationOverscanComplete,
          QStringLiteral("initial staging publishes one bounded complete "
                         "frame without synchronous fallback or overscan"));
  require(initialLayoutRequests <= firstFrameStagePasses * 64 + 64 &&
              initialPaints <= firstFrameStagePasses * 32 + 32,
          QStringLiteral("first-frame layout/paint work is card-bounded"));
  require(initialResidencySettled && deferredInitialConstructions > 0 &&
              deferredInitialConstructions == deferredInitialAdmissionPasses &&
              initialMaximumConstructionsPerAdmissionPass <= 1 &&
              firstFrameStable,
          QStringLiteral("deferred one-card admission fills overscan without "
                         "changing the published frame"));
  require(peak.cards >= 1 && peak.cards <= 64 &&
              peak.residentCardsInViewportBuffer && exactResidency,
          QStringLiteral("resident cards stay inside the viewport-derived "
                         "buffer except for one focus-owned card"));
  require(peak.boundsHold,
          QStringLiteral("document and widget bounds hold in every sample"));
  require(peak.constructionMetricObserved &&
              constructions.samples.size() >=
                  static_cast<std::size_t>(peak.cards) &&
              timingWithin(constructionStats, 5000, 10000, 25000),
          QStringLiteral("observed resident-card construction p50/p95/max "
                         "stay within 5/10/25 ms"));
  require(peak.visibleCards >= 1 && peak.identitiesMatch &&
              peak.residentKeysUnique && peak.shownCardsIntersect &&
              peak.viewportRowsRendered && peak.viewportRows >= 1 &&
              peak.visibleTextPresent && peak.kindMask == EveryCardKind &&
              peak.checksum != 0,
          QStringLiteral(
              "all card kinds match model, text, and viewport geometry"));
  require(paintedWarm.timings.size() == PaintedScrollSamples &&
              paintedWarm.zeroResidencyTimings.size() >=
                  PaintedScrollSamples - 5 &&
              paintedWarm.firstFramePixelsMoved && paintedWarm.targetsReached &&
              paintedWarm.viewportRowsRendered &&
              paintedWarm.maximumConstructions <= 1 &&
              paintedWarm.maximumAdmissionPasses <= 1 &&
              paintedWarm.maximumResidencyScans <= 1 &&
              paintedWarm.maximumLayoutRequests == 0 &&
              paintedWarm.maximumUpdateRequests <= 2 &&
              paintedFrameWorkBounded(paintedWarm),
          QStringLiteral("warm wheel input reaches one complete backing-store "
                         "frame with changed pixels and bounded work"));
  require(timingWithin(paintedWarmZeroResidencyStats, 8000, 16667, 33334),
          QStringLiteral("warm wheel input reaches completed QWidget paint "
                         "within one 60-Hz frame at p95"));
  require(paintedScrollBarWarm.timings.size() == PaintedScrollSamples &&
              paintedScrollBarWarm.zeroResidencyTimings.size() >=
                  PaintedScrollSamples - 5 &&
              paintedScrollBarWarm.firstFramePixelsMoved &&
              paintedScrollBarWarm.targetsReached &&
              paintedScrollBarWarm.viewportRowsRendered &&
              paintedScrollBarWarm.maximumConstructions <= 1 &&
              paintedScrollBarWarm.maximumAdmissionPasses <= 1 &&
              paintedScrollBarWarm.maximumResidencyScans <= 1 &&
              paintedScrollBarWarm.maximumUpdateRequests <= 2 &&
              paintedFrameWorkBounded(paintedScrollBarWarm),
          QStringLiteral("warm direct scrollbar movement reaches one complete "
                         "backing-store frame with changed pixels and bounded "
                         "work"));
  require(
      timingWithin(paintedScrollBarWarmZeroResidencyStats, 8000, 16667, 33334),
      QStringLiteral("warm direct scrollbar movement without card "
                     "construction or admission reaches completed QWidget "
                     "paint within one 60-Hz frame at p95"));
  require(paintedScrollBarStepWork.timings.size() == PaintedScrollSamples &&
              paintedScrollBarStepWork.firstFramePixelsMoved &&
              paintedScrollBarStepWork.targetsReached &&
              paintedScrollBarStepWork.viewportRowsRendered &&
              paintedScrollBarStepWork.maximumConstructions <= 8 &&
              paintedScrollBarStepWork.maximumAdmissionPasses <= 3 &&
              paintedScrollBarStepWork.maximumResidencyScans <= 4 &&
              paintedScrollBarStepWork.maximumUpdateRequests <= 2 &&
              paintedFrameWorkBounded(paintedScrollBarStepWork),
          QStringLiteral("viewport-step scrollbar movement reaches one "
                         "complete frame with changed pixels and bounded "
                         "work"));
  require(timingWithin(paintedScrollBarStepStats, 15000, 16667, 33334),
          QStringLiteral("viewport-step scrollbar movement reaches completed "
                         "QWidget paint within one 60-Hz frame at p95"));
  require(paintedAngle.timings.size() == PaintedScrollSamples &&
              paintedAngle.firstFramePixelsMoved &&
              paintedAngle.targetsReached &&
              paintedAngle.viewportRowsRendered &&
              paintedAngle.maximumConstructions <= 3 &&
              paintedAngle.maximumAdmissionPasses <= 3 &&
              paintedAngle.maximumResidencyScans <= 2 &&
              paintedAngle.maximumUpdateRequests <= 2 &&
              paintedFrameWorkBounded(paintedAngle),
          QStringLiteral("progressive angle input reaches one complete "
                         "backing-store frame with changed pixels and bounded "
                         "work"));
  require(timingWithin(paintedAngleStats, 8000, 16667, 33334),
          QStringLiteral("progressive wheel input reaches completed QWidget "
                         "paint within one 60-Hz frame at p95"));
  require(paintedStreaming.timings.size() == StreamSamples &&
              paintedStreaming.firstFramePixelsMoved &&
              paintedStreaming.targetsReached &&
              paintedStreaming.viewportRowsRendered &&
              paintedStreaming.maximumConstructions <= 1 &&
              paintedStreaming.maximumAdmissionPasses <= 1 &&
              paintedStreaming.maximumResidencyScans <= 4 &&
              paintedStreaming.maximumPaints <= 128,
          QStringLiteral("streaming during wheel input reaches one complete "
                         "frame with bounded renderer work"));
  require(timingWithin(paintedStreamingStats, 15000, 16667, 33334),
          QStringLiteral("streaming during wheel input reaches completed "
                         "QWidget paint within one 60-Hz frame at p95"));
  require(warmedWheelSettled &&
              warmedWheel.timings.size() == WarmedScrollSamples &&
              warmedWheelValues.size() >= 80 && warmedWheel.targetsReached &&
              warmedWheel.viewportRowsRendered &&
              warmedWheel.maximumConstructions == 0 &&
              warmedWheel.maximumResidencyScans <= 1 &&
              warmedWheel.maximumAdmissionConstructions <= 3 &&
              warmedWheel.maximumAdmissionPasses <= 3 &&
              warmedWheel.maximumConstructionsPerAdmissionPass <= 1 &&
              warmedWheel.admissionCadenceExact,
          QStringLiteral("progressive native wheel input renders the viewport "
                         "immediately with zero synchronous hidden-card "
                         "construction and bounded asynchronous admission"));
  require(warmedWheelStats.p95 <= timingScale * 2000 &&
              warmedWheelStats.maximum <= timingScale * 5000,
          QStringLiteral("progressive viewport-wheel synchronous handler "
                         "p95/max stay within 2/5 ms"));
  require(
      normal.targetsReached && normal.maximumConstructions <= 8 &&
          normal.maximumReleases <= 8 && normal.maximumResidencyScans <= 4 &&
          normal.maximumAdmissionPasses <= 4 &&
          normal.maximumCombinedPasses <= normal.maximumConstructions + 1 &&
          normal.maximumLayoutRequests <= 512 && normal.maximumPaints <= 256,
      QStringLiteral("overlapping scroll and admission work stays "
                     "viewport- and card-bounded"));
  require(seeks.targetsReached && visitedSeekValues.size() >= 40 &&
              seekPixelChecksums.size() >= 6 &&
              seeks.maximumConstructions <= 32 && seeks.maximumReleases <= 32 &&
              seeks.maximumResidencyScans <= 8 &&
              seeks.maximumAdmissionPasses <= 4 &&
              seeks.maximumCombinedPasses <= 8 &&
              seeks.maximumCombinedPasses <= seeks.maximumConstructions + 1 &&
              seeks.maximumLayoutRequests <= 2048 && seeks.maximumPaints <= 512,
          QStringLiteral("large seek and admission work stays within one "
                         "bounded residency window"));
  require(timingWithin(normalStats, 15000, 30000, 50000),
          QStringLiteral("overlapping-scroll median/p95/max stay within "
                         "15/30/50 ms"));
  require(timingWithin(seekStats, 70000, 85000, 120000),
          QStringLiteral("large-seek median/p95/max stay within "
                         "70/85/120 ms"));
  require(noOpStable,
          QStringLiteral("semantic no-op preserves pixels, objects, and work"));
  require(noOpMicroseconds <= noOpLimit,
          QStringLiteral("unchanged reconciliation <= %1 us").arg(noOpLimit));
  require(streamAccepted && streamingConstructions == 0 && streamTailLocal &&
              streamTailLocalityChecks == StreamSamples &&
              streamDocumentChanges >= StreamSamples &&
              streamDocumentChanges <= StreamSamples * 4 &&
              streamMaximumLayoutRequests <= 64 && streamMaximumPaints <= 64 &&
              timingWithin(streamStats, 10000, 30000, 50000),
          QStringLiteral("Markdown streaming mutates only its mutable tail "
                         "and retains one card/document"));
  require(commandAccepted && commandStreamingConstructions == 0 &&
              commandDocumentChanges >= StreamSamples &&
              commandDocumentChanges <= StreamSamples * 6 &&
              commandMaximumLayoutRequests <= 64 &&
              commandMaximumPaints <= 64 &&
              timingWithin(commandStats, 10000, 30000, 50000),
          QStringLiteral("command streaming retains its document and follows "
                         "within the work budget"));
  require(appendCorrect && appendConstructions == 0 &&
              appendLayoutRequests <= 64 && appendPaints <= 64 &&
              appendMicroseconds <= appendLimit,
          QStringLiteral("paused tail append is rebuild-free and bounded"));
  require(
      followingAppendsCorrect && followingAppendMaximumConstructions <= 4 &&
          followingAppendMaximumLayoutRequests <= 128 &&
          followingAppendMaximumPaints <= 128 &&
          timingWithin(followingAppendStats, 15000, 30000, 50000),
      QStringLiteral("following tail appends stay smooth and fully visible"));
  require(resizeCorrect && resizeEntryMicroseconds <= timingScale * 100000 &&
              timingWithin(resizeFrameStats, 35000, 50000, 75000) &&
              exactResizeMicroseconds <= timingScale * 100000 &&
              resizeMaximumLayoutRequests <= 512 && resizeMaximumPaints <= 256,
          QStringLiteral("interactive resize entry, steady frames, and exact "
                         "settlement stay within budget"));
  require(count < 10'000 ||
              (sparseSmall.correct && sparseLarge.correct &&
               sparseLarge.microseconds <= sparseVisibilityLimit &&
               sparseLarge.microseconds <=
                   sparseSmall.microseconds * 12 + timingScale * 20000),
          QStringLiteral("zero-height rows do not scale viewport traversal"));
  require(count < 10'000 ||
              (structuralPlanCorrect &&
               timingWithin(structuralPlanStats, 5000, 10000, 20000)),
          QStringLiteral("a 64-row structural delta over 10,000 rows is "
                         "side-effect-free and plans within 5/10/20 ms"));
  require(count < 10'000 ||
              (structuralCommitCorrect &&
               structuralCommitMaximumLayoutRequests <= 64 &&
               structuralCommitMaximumPaints <= 128 &&
               timingWithin(structuralCommitStats, 5000, 10000, 20000)),
          QStringLiteral("accepted 10,000-row root transactions preserve the "
                         "viewport, renderer resources, focus, and selection "
                         "within 5/10/20 ms and 64/128 layout/paint events"));
  require(peak.maximumHeightUpdateSteps <= 4 * std::bit_width(count + 1),
          QStringLiteral("height update path remains logarithmically bounded"));
  require(!application.layoutMicros.empty() &&
              timingWithin(layoutEventStats, 1000, 5000, 30000),
          QStringLiteral("layout-event dispatch p50/p95/max stay within "
                         "1/5/30 ms"));
  require(!application.paintMicros.empty() &&
              timingWithin(paintEventStats, 5000, 15000, 50000),
          QStringLiteral("paint-event dispatch p50/p95/max stay within "
                         "5/15/50 ms"));
#if defined(__linux__)
  require(rssSupported,
          QStringLiteral("Linux RSS metrics are available for gating"));
#endif
  require(!rssSupported ||
              (peak.residentKiB <= residentLimit &&
               peak.residentKiB - startupResidentKiB <= residentGrowthLimit),
          QStringLiteral("sampled RSS stays within the configured profile"));

  QJsonArray failureJson;
  for (const QString &failure : failures)
    failureJson.push_back(failure);
  const QJsonObject result{
      {"passed", failures.empty()},
      {"failures", failureJson},
      {"rows", static_cast<qint64>(count)},
      {"instrumentedBuild", InstrumentedBuild},
      {"requiredMetricsPresent", requiredMetricsPresent},
      {"rssSupported", rssSupported},
      {"qpaPlatform", platformName},
      {"style", activeApplicationStyle},
      {"styleClass", activeApplicationStyleClass},
      {"screen", screenName},
      {"screenDpr", screenDpr},
      {"screenRefreshRate", screenRefreshRate},
      {"benchmarkWindowExposed", benchmarkWindowExposed},
      {"expectedDpr", expectedDpr},
      {"actualDpr", actualDpr},
      {"initialMilliseconds", initialMilliseconds},
      {"initialLayoutRequests", static_cast<qint64>(initialLayoutRequests)},
      {"initialPaints", static_cast<qint64>(initialPaints)},
      {"firstFrameResidentCards", firstFrame.cards},
      {"firstFrameViewportRows", firstFrame.viewportRows},
      {"firstFrameStageBudget", static_cast<qint64>(firstFrameStageBudget)},
      {"firstFrameStageConstructions",
       static_cast<qint64>(firstFrameStagePasses)},
      {"firstFrameSynchronousConstructions",
       static_cast<qint64>(firstFrameSynchronousConstructions)},
      {"deferredInitialConstructions",
       static_cast<qint64>(deferredInitialConstructions)},
      {"deferredInitialAdmissionPasses",
       static_cast<qint64>(deferredInitialAdmissionPasses)},
      {"initialMaxConstructionsPerAdmissionPass",
       static_cast<qint64>(initialMaximumConstructionsPerAdmissionPass)},
      {"warmInputToBackingStorePaint", statsJson(paintedWarmStats)},
      {"warmNoResidencyInputToBackingStorePaint",
       statsJson(paintedWarmZeroResidencyStats)},
      {"warmDirectScrollBarValueToBackingStorePaint",
       statsJson(paintedScrollBarWarmStats)},
      {"warmNoResidencyDirectScrollBarValueToBackingStorePaint",
       statsJson(paintedScrollBarWarmZeroResidencyStats)},
      {"viewportStepDirectScrollBarValueToBackingStorePaint",
       statsJson(paintedScrollBarStepStats)},
      {"warmPaintedFrameSamples",
       static_cast<qint64>(paintedWarm.timings.size())},
      {"warmPaintedFramePixelsMoved", paintedWarm.firstFramePixelsMoved},
      {"warmNoResidencyPaintedFrameSamples",
       static_cast<qint64>(paintedWarm.zeroResidencyTimings.size())},
      {"warmPaintedFrameMaxConstructions",
       static_cast<qint64>(paintedWarm.maximumConstructions)},
      {"warmPaintedFrameMaxAdmissionPasses",
       static_cast<qint64>(paintedWarm.maximumAdmissionPasses)},
      {"warmPaintedFrameMaxResidencyScans",
       static_cast<qint64>(paintedWarm.maximumResidencyScans)},
      {"warmPaintedFrameMaxLayoutRequests",
       static_cast<qint64>(paintedWarm.maximumLayoutRequests)},
      {"warmPaintedFrameMaxMoves",
       static_cast<qint64>(paintedWarm.maximumMoves)},
      {"warmPaintedFrameMaxPaints",
       static_cast<qint64>(paintedWarm.maximumPaints)},
      {"warmPaintedFrameMaxResizes",
       static_cast<qint64>(paintedWarm.maximumResizes)},
      {"warmPaintedFrameMaxUpdateRequests",
       static_cast<qint64>(paintedWarm.maximumUpdateRequests)},
      {"warmScrollBarPaintedFrameSamples",
       static_cast<qint64>(paintedScrollBarWarm.timings.size())},
      {"warmScrollBarPaintedFramePixelsMoved",
       paintedScrollBarWarm.firstFramePixelsMoved},
      {"warmScrollBarNoResidencyPaintedFrameSamples",
       static_cast<qint64>(paintedScrollBarWarm.zeroResidencyTimings.size())},
      {"warmScrollBarPaintedFrameMaxConstructions",
       static_cast<qint64>(paintedScrollBarWarm.maximumConstructions)},
      {"warmScrollBarPaintedFrameMaxAdmissionPasses",
       static_cast<qint64>(paintedScrollBarWarm.maximumAdmissionPasses)},
      {"warmScrollBarPaintedFrameMaxResidencyScans",
       static_cast<qint64>(paintedScrollBarWarm.maximumResidencyScans)},
      {"warmScrollBarPaintedFrameMaxLayoutRequests",
       static_cast<qint64>(paintedScrollBarWarm.maximumLayoutRequests)},
      {"warmScrollBarPaintedFrameMaxMoves",
       static_cast<qint64>(paintedScrollBarWarm.maximumMoves)},
      {"warmScrollBarPaintedFrameMaxPaints",
       static_cast<qint64>(paintedScrollBarWarm.maximumPaints)},
      {"warmScrollBarPaintedFrameMaxResizes",
       static_cast<qint64>(paintedScrollBarWarm.maximumResizes)},
      {"warmScrollBarPaintedFrameMaxUpdateRequests",
       static_cast<qint64>(paintedScrollBarWarm.maximumUpdateRequests)},
      {"viewportStepScrollBarPaintedFrameSamples",
       static_cast<qint64>(paintedScrollBarStepWork.timings.size())},
      {"viewportStepScrollBarPaintedFramePixelsMoved",
       paintedScrollBarStepWork.firstFramePixelsMoved},
      {"viewportStepScrollBarPaintedFrameMaxConstructions",
       static_cast<qint64>(paintedScrollBarStepWork.maximumConstructions)},
      {"viewportStepScrollBarPaintedFrameMaxAdmissionPasses",
       static_cast<qint64>(paintedScrollBarStepWork.maximumAdmissionPasses)},
      {"viewportStepScrollBarPaintedFrameMaxResidencyScans",
       static_cast<qint64>(paintedScrollBarStepWork.maximumResidencyScans)},
      {"viewportStepScrollBarPaintedFrameMaxLayoutRequests",
       static_cast<qint64>(paintedScrollBarStepWork.maximumLayoutRequests)},
      {"viewportStepScrollBarPaintedFrameMaxMoves",
       static_cast<qint64>(paintedScrollBarStepWork.maximumMoves)},
      {"viewportStepScrollBarPaintedFrameMaxPaints",
       static_cast<qint64>(paintedScrollBarStepWork.maximumPaints)},
      {"viewportStepScrollBarPaintedFrameMaxResizes",
       static_cast<qint64>(paintedScrollBarStepWork.maximumResizes)},
      {"viewportStepScrollBarPaintedFrameMaxUpdateRequests",
       static_cast<qint64>(paintedScrollBarStepWork.maximumUpdateRequests)},
      {"angleInputToBackingStorePaint", statsJson(paintedAngleStats)},
      {"streamingInputToBackingStorePaint",
       statsJson(paintedStreamingStats)},
      {"streamingPaintedFrameMaxConstructions",
       static_cast<qint64>(paintedStreaming.maximumConstructions)},
      {"streamingPaintedFrameMaxAdmissionPasses",
       static_cast<qint64>(paintedStreaming.maximumAdmissionPasses)},
      {"streamingPaintedFrameMaxResidencyScans",
       static_cast<qint64>(paintedStreaming.maximumResidencyScans)},
      {"streamingPaintedFrameMaxPaints",
       static_cast<qint64>(paintedStreaming.maximumPaints)},
      {"anglePaintedFrameSamples",
       static_cast<qint64>(paintedAngle.timings.size())},
      {"anglePaintedFramePixelsMoved", paintedAngle.firstFramePixelsMoved},
      {"anglePaintedFrameMaxConstructions",
       static_cast<qint64>(paintedAngle.maximumConstructions)},
      {"anglePaintedFrameMaxAdmissionPasses",
       static_cast<qint64>(paintedAngle.maximumAdmissionPasses)},
      {"anglePaintedFrameMaxResidencyScans",
       static_cast<qint64>(paintedAngle.maximumResidencyScans)},
      {"anglePaintedFrameMaxLayoutRequests",
       static_cast<qint64>(paintedAngle.maximumLayoutRequests)},
      {"anglePaintedFrameMaxMoves",
       static_cast<qint64>(paintedAngle.maximumMoves)},
      {"anglePaintedFrameMaxPaints",
       static_cast<qint64>(paintedAngle.maximumPaints)},
      {"anglePaintedFrameMaxResizes",
       static_cast<qint64>(paintedAngle.maximumResizes)},
      {"anglePaintedFrameMaxUpdateRequests",
       static_cast<qint64>(paintedAngle.maximumUpdateRequests)},
      {"progressiveWheelHandler", statsJson(warmedWheelStats)},
      {"progressiveWheelHandlerSamples",
       static_cast<qint64>(warmedWheel.timings.size())},
      {"progressiveWheelUniqueValues",
       static_cast<qint64>(warmedWheelValues.size())},
      {"progressiveWheelMaxImmediateConstructions",
       static_cast<qint64>(warmedWheel.maximumConstructions)},
      {"progressiveWheelMaxImmediateScans",
       static_cast<qint64>(warmedWheel.maximumResidencyScans)},
      {"progressiveWheelMaxAdmissionConstructions",
       static_cast<qint64>(warmedWheel.maximumAdmissionConstructions)},
      {"progressiveWheelMaxAdmissionPasses",
       static_cast<qint64>(warmedWheel.maximumAdmissionPasses)},
      {"progressiveWheelMaxConstructionsPerAdmissionPass",
       static_cast<qint64>(warmedWheel.maximumConstructionsPerAdmissionPass)},
      {"progressiveWheelAdmissionCadenceExact",
       warmedWheel.admissionCadenceExact},
      {"progressiveWheelImmediateCoverage", warmedWheel.viewportRowsRendered},
      {"progressiveWheelEventuallySettled", warmedWheelSettled},
      {"normalScroll", statsJson(normalStats)},
      {"largeSeek", statsJson(seekStats)},
      {"stream", statsJson(streamStats)},
      {"streamApply", statsJson(statistics(streamApplyTimings))},
      {"commandStream", statsJson(commandStats)},
      {"followingTailAppend", statsJson(followingAppendStats)},
      {"interactiveResizeEntryMicroseconds", resizeEntryMicroseconds},
      {"interactiveResizeFrame", statsJson(resizeFrameStats)},
      {"residentCardConstructorAndWiring", statsJson(constructionStats)},
      {"layoutEventDispatch", statsJson(layoutEventStats)},
      {"paintEventDispatch", statsJson(paintEventStats)},
      {"structuralDeltaPlan", statsJson(structuralPlanStats)},
      {"structuralDeltaPlanCorrect", structuralPlanCorrect},
      {"structuralDeltaPlanSamples",
       static_cast<qint64>(structuralPlanSampleCount)},
      {"structuralDeltaCommit", statsJson(structuralCommitStats)},
      {"structuralDeltaCommitCorrect", structuralCommitCorrect},
      {"structuralDeltaCommitSamples",
       static_cast<qint64>(structuralCommitSampleCount)},
      {"structuralDeltaCommitMaxLayoutRequests",
       static_cast<qint64>(structuralCommitMaximumLayoutRequests)},
      {"structuralDeltaCommitMaxPaints",
       static_cast<qint64>(structuralCommitMaximumPaints)},
      {"residentCardConstructorAndWiringSamples",
       static_cast<qint64>(constructions.samples.size())},
      {"layoutEventSamples",
       static_cast<qint64>(application.layoutMicros.size())},
      {"paintEventSamples",
       static_cast<qint64>(application.paintMicros.size())},
      {"noOpMicroseconds", noOpMicroseconds},
      {"noOpStable", noOpStable},
      {"noOpLayoutRequests", static_cast<qint64>(noOpLayoutRequests)},
      {"noOpPaints", static_cast<qint64>(noOpPaints)},
      {"tailAppendMicroseconds", appendMicroseconds},
      {"tailAppendLayoutRequests", static_cast<qint64>(appendLayoutRequests)},
      {"tailAppendPaints", static_cast<qint64>(appendPaints)},
      {"resizeBurstMicroseconds", resizeMicroseconds},
      {"resizeExactMicroseconds", exactResizeMicroseconds},
      {"sparseVisibilitySmallMicroseconds", sparseSmall.microseconds},
      {"sparseVisibilityLargeMicroseconds", sparseLarge.microseconds},
      {"residentCardPeak", peak.cards},
      {"documentPeak", peak.documents},
      {"widgetPeak", peak.widgets},
      {"visibleCardPeak", peak.visibleCards},
      {"viewportRowPeak", peak.viewportRows},
      {"residentKeysUnique", peak.residentKeysUnique},
      {"exactViewportResidency", exactResidency},
      {"viewportRowsRendered", peak.viewportRowsRendered},
      {"visibleTextPresent", peak.visibleTextPresent},
      {"kindMask", static_cast<qint64>(peak.kindMask)},
      {"uniqueSeekValues", static_cast<qint64>(visitedSeekValues.size())},
      {"uniqueSeekPixelChecksums",
       static_cast<qint64>(seekPixelChecksums.size())},
      {"normalMaxConstructions",
       static_cast<qint64>(normal.maximumConstructions)},
      {"normalMaxReleases", static_cast<qint64>(normal.maximumReleases)},
      {"normalMaxResidencyScans",
       static_cast<qint64>(normal.maximumResidencyScans)},
      {"normalMaxAdmissionPasses",
       static_cast<qint64>(normal.maximumAdmissionPasses)},
      {"normalMaxCombinedPasses",
       static_cast<qint64>(normal.maximumCombinedPasses)},
      {"seekMaxConstructions", static_cast<qint64>(seeks.maximumConstructions)},
      {"seekMaxReleases", static_cast<qint64>(seeks.maximumReleases)},
      {"seekMaxResidencyScans",
       static_cast<qint64>(seeks.maximumResidencyScans)},
      {"seekMaxAdmissionPasses",
       static_cast<qint64>(seeks.maximumAdmissionPasses)},
      {"seekMaxCombinedPasses",
       static_cast<qint64>(seeks.maximumCombinedPasses)},
      {"normalMaxLayoutRequests",
       static_cast<qint64>(normal.maximumLayoutRequests)},
      {"normalMaxPaints", static_cast<qint64>(normal.maximumPaints)},
      {"seekMaxLayoutRequests",
       static_cast<qint64>(seeks.maximumLayoutRequests)},
      {"seekMaxPaints", static_cast<qint64>(seeks.maximumPaints)},
      {"streamingConstructions", static_cast<qint64>(streamingConstructions)},
      {"streamDocumentChanges", static_cast<qint64>(streamDocumentChanges)},
      {"streamTailLocalityChecks",
       static_cast<qint64>(streamTailLocalityChecks)},
      {"streamTailLocal", streamTailLocal},
      {"streamMaxLayoutRequests",
       static_cast<qint64>(streamMaximumLayoutRequests)},
      {"streamMaxPaints", static_cast<qint64>(streamMaximumPaints)},
      {"commandStreamingConstructions",
       static_cast<qint64>(commandStreamingConstructions)},
      {"commandDocumentChanges", static_cast<qint64>(commandDocumentChanges)},
      {"commandMaxLayoutRequests",
       static_cast<qint64>(commandMaximumLayoutRequests)},
      {"commandMaxPaints", static_cast<qint64>(commandMaximumPaints)},
      {"tailAppendConstructions", static_cast<qint64>(appendConstructions)},
      {"followingAppendMaxConstructions",
       static_cast<qint64>(followingAppendMaximumConstructions)},
      {"followingAppendMaxLayoutRequests",
       static_cast<qint64>(followingAppendMaximumLayoutRequests)},
      {"followingAppendMaxPaints",
       static_cast<qint64>(followingAppendMaximumPaints)},
      {"resizeMaxLayoutRequests",
       static_cast<qint64>(resizeMaximumLayoutRequests)},
      {"resizeMaxPaints", static_cast<qint64>(resizeMaximumPaints)},
      {"maximumConstructionMicroseconds", peak.maximumConstructionMicros},
      {"heightIndexUpdateSteps",
       static_cast<qint64>(peak.maximumHeightUpdateSteps)},
      {"startupResidentKiB", startupResidentKiB},
      {"initialResidentKiB", initialResidentKiB},
      {"normalResidentKiB", normalResidentKiB},
      {"seekResidentKiB", seekResidentKiB},
      {"finalResidentKiB", finalResidentKiB},
      {"sampledPeakResidentKiB", peak.residentKiB},
      {"dprBufferAllowanceKiB", dprBufferAllowanceKiB},
      {"residentGrowthLimitKiB", residentGrowthLimit},
      {"kernelInheritedPeakResidentKiB", inheritedPeakResidentKiB},
      {"checksum", QString::number(peak.checksum)}};
  qApp->removeEventFilter(&events);
  std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData()
            << '\n';
  return failures.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
}
