// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QIODevice>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextDocumentWriter>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace codexui::codex::middle {
namespace {

class AnimationDurationStyle final : public QProxyStyle {
public:
  explicit AnimationDurationStyle(int duration) : duration_(duration) {}

  int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                const QWidget *widget = nullptr,
                QStyleHintReturn *returnData = nullptr) const override {
    return hint == SH_Widget_Animation_Duration
               ? duration_
               : QProxyStyle::styleHint(hint, option, widget, returnData);
  }

private:
  int duration_;
};

QByteArray normalizedZipMetadata(QByteArray archive) {
  const auto clearTimestamp = [&archive](QByteArrayView signature,
                                         qsizetype timestampOffset) {
    qsizetype offset = 0;
    while ((offset = archive.indexOf(signature, offset)) >= 0) {
      if (offset + timestampOffset + 4 <= archive.size())
        std::fill_n(archive.data() + offset + timestampOffset, 4, '\0');
      offset += signature.size();
    }
  };
  clearTimestamp(QByteArrayView("PK\x03\x04", 4), 10);
  clearTimestamp(QByteArrayView("PK\x01\x02", 4), 12);
  return archive;
}

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

bool changed(ConversationView::ReconciliationResult result) {
  return result == ConversationView::ReconciliationResult::Changed;
}

bool labelUsesColor(QLabel *label, const char *color) {
  if (!label)
    return false;
  label->ensurePolished();
  return label->palette().color(QPalette::WindowText) ==
         QColor(QString::fromLatin1(color));
}

std::optional<PresentationImpact> applyPresentation(ConversationView &view,
                                                    VisibleCardData card) {
  ConversationDelta delta;
  delta.threadId = card.threadId;
  delta.presentations.push_back(std::move(card));
  return view.applyConversationDelta(std::move(delta));
}

struct PerceptualColor {
  double lightness = 0.0;
  double chroma = 0.0;
};

PerceptualColor perceptualColor(const char *hex) {
  const QColor color(QString::fromLatin1(hex));
  const auto linear = [](double channel) {
    return channel <= 0.04045 ? channel / 12.92
                              : std::pow((channel + 0.055) / 1.055, 2.4);
  };
  const double red = linear(color.redF());
  const double green = linear(color.greenF());
  const double blue = linear(color.blueF());
  const double l = std::cbrt(0.4122214708 * red + 0.5363325363 * green +
                             0.0514459929 * blue);
  const double m = std::cbrt(0.2119034982 * red + 0.6806995451 * green +
                             0.1073969566 * blue);
  const double s = std::cbrt(0.0883024619 * red + 0.2817188376 * green +
                             0.6299787005 * blue);
  const double lightness =
      0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
  return {lightness, std::hypot(a, b)};
}

bool perceptuallyMatched(std::initializer_list<const char *> colors,
                         const char *message) {
  double minimumLightness = std::numeric_limits<double>::max();
  double maximumLightness = 0.0;
  double minimumChroma = std::numeric_limits<double>::max();
  double maximumChroma = 0.0;
  for (const char *hex : colors) {
    const PerceptualColor color = perceptualColor(hex);
    minimumLightness = std::min(minimumLightness, color.lightness);
    maximumLightness = std::max(maximumLightness, color.lightness);
    minimumChroma = std::min(minimumChroma, color.chroma);
    maximumChroma = std::max(maximumChroma, color.chroma);
  }
  return expect(maximumLightness - minimumLightness < 0.0035 &&
                    maximumChroma - minimumChroma < 0.0035,
                message);
}

bool testPerceptuallyUniformPalette() {
  using namespace codexui::UiStyle;
  bool result = perceptuallyMatched(
      {blue, green, yellow, orange, red, purple, teal},
      "palette base colors share perceptual lightness and chroma");
  result &= perceptuallyMatched(
      {blueHover, greenHover, yellowHover, orangeHover, redHover, purpleHover,
       tealHover},
      "palette hover colors share perceptual lightness and chroma");
  result &= perceptuallyMatched(
      {bluePressed, greenPressed, yellowPressed, orangePressed, redPressed,
       purplePressed, tealPressed},
      "palette pressed colors share perceptual lightness and chroma");
  result &= perceptuallyMatched(
      {blueSurface, greenSurface, yellowSurface, orangeSurface, redSurface,
       purpleSurface, tealSurface},
      "palette surfaces share perceptual lightness and chroma");
  result &= perceptuallyMatched(
      {blueBorder, greenBorder, yellowBorder, orangeBorder, redBorder,
       purpleBorder, tealBorder},
      "palette borders share perceptual lightness and chroma");
  result &= perceptuallyMatched(
      {blueText, greenText, yellowText, orangeText, redText, purpleText,
       tealText},
      "palette text colors share perceptual lightness and chroma");
  result &= perceptuallyMatched(
      {blueSelected, yellowSurfaceHover, orangeSurfaceHover},
      "palette hover surfaces share perceptual lightness and chroma");
  result &= perceptuallyMatched(
      {blueBorderStrong, yellowBorderStrong, orangeBorderStrong,
       tealBorderStrong},
      "palette strong borders share perceptual lightness and chroma");
  return result;
}

std::string utf8(const QString &value) { return value.toUtf8().toStdString(); }

class DesktopUrlCapture final : public QObject {
  Q_OBJECT

public:
  std::vector<QUrl> urls;

public slots:
  void open(const QUrl &url) { urls.push_back(url); }
};

class ScopedFileUrlHandler final {
public:
  explicit ScopedFileUrlHandler(DesktopUrlCapture &capture) {
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &capture, "open");
  }

  ~ScopedFileUrlHandler() {
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
  }
};

class LayoutRequestProbe final : public QObject {
public:
  explicit LayoutRequestProbe(QWidget *root) : root_(root) {
    qApp->installEventFilter(this);
  }

  ~LayoutRequestProbe() override { qApp->removeEventFilter(this); }

  void start() {
    count = 0;
    active = true;
  }

  int count = 0;
  bool active = false;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (active && event->type() == QEvent::LayoutRequest && widget &&
        (widget == root_ || root_->isAncestorOf(widget)))
      ++count;
    return false;
  }

private:
  QWidget *root_ = nullptr;
};

class NamedTimerEventProbe final : public QObject {
public:
  explicit NamedTimerEventProbe(QString objectName)
      : objectName_(std::move(objectName)) {
    qApp->installEventFilter(this);
  }

  ~NamedTimerEventProbe() override { qApp->removeEventFilter(this); }

  void reset() { events_ = 0; }
  [[nodiscard]] int events() const noexcept { return events_; }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event && event->type() == QEvent::Timer && watched &&
        watched->objectName() == objectName_)
      ++events_;
    return false;
  }

private:
  QString objectName_;
  int events_ = 0;
};

void spin(int milliseconds = 0) {
  if (milliseconds == 0) {
    // One selected/load-more page is admitted in eight-card slices. Drain a
    // bounded page worth of zero-delay continuations without turning every
    // test settle into an arbitrary wall-clock delay.
    for (int pass = 0; pass < 16; ++pass)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    return;
  }
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (milliseconds > 0)
      QThread::msleep(1);
  } while (timer.elapsed() < milliseconds);
}

template <typename Predicate>
bool spinUntil(Predicate &&predicate, int maximumPasses = 64) {
  for (int pass = 0; pass < maximumPasses; ++pass) {
    if (predicate())
      return true;
    // Zero-delay continuations normally drain in one event dispatch, while
    // contention recovery deliberately uses a small nonzero timer. Give both
    // paths a real event-loop tick without assuming synchronous completion.
    spin(1);
  }
  return predicate();
}

VisibleCardData agentCard(const std::string &threadId,
                          const std::string &turnId, int index,
                          QString text = {}) {
  const std::string itemId = "agent-" + std::to_string(index);
  if (text.isEmpty())
    text = QStringLiteral("Codex output line %1 with enough text to wrap a "
                          "little in the viewport.")
               .arg(index);
  return {AuthoritativeItemKey{threadId, turnId, itemId},
          CardKind::AgentMessage,
          threadId,
          turnId,
          itemId,
          AgentMessageData{utf8(text), index % 3 == 0}};
}

VisibleCardData cardForAppearanceAudit(const std::string &threadId,
                                       CardKind kind, int index) {
  const std::string itemId = "appearance-" + std::to_string(index);
  CardKey key =
      kind == CardKind::LocalPrompt
          ? CardKey{LocalPromptKey{9000U + static_cast<std::uint64_t>(index)}}
          : CardKey{AuthoritativeItemKey{threadId, "turn-2", itemId}};
  CardPayload payload = GenericActivityData{};
  switch (kind) {
  case CardKind::UserMessage:
    payload = UserMessageData{"User appearance audit", {}};
    break;
  case CardKind::AgentMessage:
    payload = AgentMessageData{"Agent appearance audit", true};
    break;
  case CardKind::CommandExecution:
    payload = CommandExecutionData{"printf audit", {}, "/workspace", {}, {}};
    break;
  case CardKind::AgentActivity:
    payload = AgentActivityData{"spawn_agent",
                                "tool",
                                "Inspect appearance",
                                {},
                                {},
                                {},
                                {},
                                {},
                                {},
                                {}};
    break;
  case CardKind::Reasoning:
    payload = ReasoningData{"Initial reasoning summary"};
    break;
  case CardKind::FileChanges:
    payload = FileChangesData{{{"src/a.cpp", "update", 1, 0}}};
    break;
  case CardKind::ImageGeneration:
    payload = ImageGenerationData{{}, "Initial image prompt"};
    break;
  case CardKind::Plan:
    payload = PlanData{
        "Initial plan", {{"Inspect", nodegraph::NodeStatus::Running}}, {}};
    break;
  case CardKind::GenericActivity:
    payload = GenericActivityData{"unknownActivity",
                                  "type: unknownActivity\nstatus: inProgress"};
    break;
  case CardKind::LocalPrompt:
    payload = LocalPromptData{9000U + static_cast<std::uint64_t>(index),
                              "Local prompt appearance audit",
                              PromptState::InFlight,
                              0,
                              {},
                              {}};
    break;
  }
  VisibleCardData result{std::move(key), kind,   threadId,
                         "turn-2",       itemId, std::move(payload)};
  if (kind != CardKind::UserMessage && kind != CardKind::AgentMessage &&
      kind != CardKind::LocalPrompt)
    result.status = nodegraph::NodeStatus::Running;
  return result;
}

// These concise fixture records keep presentation-oriented cases readable.
struct TurnGraphSpec {
  std::string key;
  std::string turnId;
  std::vector<VisibleCardData> cards;
  std::optional<CardKey> rootCardKey;
};

struct ConversationGraphSpec {
  std::string threadId;
  std::vector<TurnGraphSpec> sections;
  bool hasMore = false;
  std::optional<std::string> activeTurnId;
};

ConversationSnapshot
projectConversation(const ConversationGraphSpec &snapshot) {
  ConversationSnapshot projected;
  projected.threadId = snapshot.threadId;
  projected.hasMore = snapshot.hasMore;
  projected.activeTurnId = snapshot.activeTurnId;
  projected.sections.reserve(snapshot.sections.size());
  for (const TurnGraphSpec &section : snapshot.sections)
    projected.sections.push_back(
        {section.key, section.turnId, section.cards, section.rootCardKey});
  return projected;
}

bool applyConversation(ConversationView &view,
                       const ConversationGraphSpec &snapshot) {
  return changed(view.reconcile(projectConversation(snapshot)));
}

ConversationGraphSpec conversation(const std::string &threadId, int count) {
  ConversationGraphSpec result;
  result.threadId = threadId;
  TurnGraphSpec first{"turn:" + threadId + ":1", "turn-1", {}};
  TurnGraphSpec second{"turn:" + threadId + ":2", "turn-2", {}};
  for (int index = 0; index < count; ++index)
    (index < count / 2 ? first : second)
        .cards.push_back(agentCard(
            threadId, index < count / 2 ? "turn-1" : "turn-2", index));
  result.sections.push_back(std::move(first));
  result.sections.push_back(std::move(second));
  return result;
}

bool testApplicationStyleSheetContract() {
  const QString sheet = codexui::UiStyle::applicationStyleSheet();
  const QString normalized = sheet.simplified();
  const bool resolved = !sheet.contains(
      QRegularExpression(QStringLiteral("%[1-9][0-9]*|%\\{[^}]+\\}")));
  const auto token = [](const char *value) {
    return QString::fromLatin1(value);
  };
  const std::array<QString, 17> componentRules{
      QStringLiteral(
          "QFrame#topBar { background: %1; border-bottom: 1px solid %2; }")
          .arg(token(codexui::UiStyle::panel),
               token(codexui::UiStyle::divider)),
      QStringLiteral("QLabel#workspaceBreadcrumb { color: %1; font-weight: "
                     "500; }")
          .arg(token(codexui::UiStyle::secondary)),
      QStringLiteral("QFrame#customStatusBar { background: %1; border-top: "
                     "1px solid %2; }")
          .arg(token(codexui::UiStyle::raised),
               token(codexui::UiStyle::divider)),
      QStringLiteral("QFrame[kind=\"statusDot\"] { background: %1; "
                     "border-radius: 5px; }")
          .arg(token(codexui::UiStyle::placeholder)),
      QStringLiteral("QFrame[kind=\"statusDot\"][tone=\"active\"] { "
                     "background: %1; }")
          .arg(token(codexui::UiStyle::blue)),
      QStringLiteral("QFrame[kind=\"statusDot\"][tone=\"success\"] { "
                     "background: %1; }")
          .arg(token(codexui::UiStyle::green)),
      QStringLiteral("QFrame[kind=\"statusDot\"][tone=\"warning\"] { "
                     "background: %1; }")
          .arg(token(codexui::UiStyle::orange)),
      QStringLiteral("QFrame[kind=\"statusDot\"][tone=\"danger\"] { "
                     "background: %1; }")
          .arg(token(codexui::UiStyle::red)),
      QStringLiteral("QPlainTextEdit#upcomingPromptEditor { background: "
                     "transparent; color: %1; border: 0; padding: 3px 2px; }")
          .arg(token(codexui::UiStyle::primary)),
      QStringLiteral("QFrame#conversation { background: %1; }")
          .arg(token(codexui::UiStyle::appBackground)),
      QStringLiteral("QFrame#attachmentFileBox { background: %1; border: 1px "
                     "solid %2; border-radius: 6px; }")
          .arg(token(codexui::UiStyle::panel),
               token(codexui::UiStyle::divider)),
      QStringLiteral(
          "QTextBrowser#markdownTextView, "
          "QPlainTextEdit#fileChangesList { background: transparent; "
          "border: 0; padding: 0; margin: 0; }"),
      QStringLiteral("QScrollArea#messageImages { background: %1; border: 1px "
                     "solid %2; border-radius: 6px; }")
          .arg(token(codexui::UiStyle::codeSurface),
               token(codexui::UiStyle::divider)),
      QStringLiteral("QWidget#messageImageStrip { background: %1; }")
          .arg(token(codexui::UiStyle::codeSurface)),
      QStringLiteral("QTextEdit#commandOutputView { background: %1; color: %2; "
                     "border-radius: 6px; padding: %3px %4px; }")
          .arg(token(codexui::UiStyle::codeSurface),
               token(codexui::UiStyle::codeText))
          .arg(codexui::UiStyle::commandOutputVerticalPadding)
          .arg(codexui::UiStyle::commandOutputHorizontalPadding),
      QStringLiteral("QTextEdit#commandTextView { background: %1; border: 1px "
                     "solid %2; border-radius: 6px; }")
          .arg(token(codexui::UiStyle::raised),
               token(codexui::UiStyle::divider)),
      QStringLiteral("QFrame#pendingPromptCard { background: transparent; "
                     "border: 1px solid transparent; border-radius: 8px; }"),
  };
  const bool oneEffectiveTooltipRule =
      sheet.count(QRegularExpression(QStringLiteral("QToolTip\\s*\\{"))) == 1 &&
      normalized.contains(
          QStringLiteral("QToolTip { background: %1; color: %2; border: 1px "
                         "solid %3; border-radius: 6px; padding: 5px; }")
              .arg(token(codexui::UiStyle::panel),
                   token(codexui::UiStyle::primary),
                   token(codexui::UiStyle::dividerStrong)));
  bool result =
      expect(resolved, "the generated application stylesheet has no unresolved "
                       "placeholders");
  for (const QString &rule : componentRules) {
    const QByteArray failure =
        QStringLiteral("the application stylesheet owns exact rule: %1")
            .arg(rule)
            .toUtf8();
    result &= expect(normalized.contains(rule), failure.constData());
  }
  result &= expect(oneEffectiveTooltipRule,
                   "one tooltip rule preserves the effective background, "
                   "foreground, border, radius, and padding");
  return result;
}

bool testMessageIdentityPalette() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  ConversationCard user(
      VisibleCardData{AuthoritativeItemKey{"identity-palette", "turn", "user"},
                      CardKind::UserMessage, "identity-palette", "turn", "user",
                      UserMessageData{"Prompt", {}}},
      false);
  ConversationCard update(
      VisibleCardData{
          AuthoritativeItemKey{"identity-palette", "turn", "update"},
          CardKind::AgentMessage, "identity-palette", "turn", "update",
          AgentMessageData{"Working", false}},
      false);
  ConversationCard final(
      VisibleCardData{AuthoritativeItemKey{"identity-palette", "turn", "final"},
                      CardKind::AgentMessage, "identity-palette", "turn",
                      "final", AgentMessageData{"Response", true}},
      false);
  for (ConversationCard *card : {&user, &update, &final}) {
    card->resize(600, card->sizeHint().height());
    card->show();
  }
  spin();

  const auto titleColor = [](ConversationCard &card) {
    for (QLabel *label : card.findChildren<QLabel *>())
      if (label->property("kind").toString() == QStringLiteral("title"))
        return label->palette().color(QPalette::WindowText);
    return QColor{};
  };
  const auto surfaceColor = [](ConversationCard &card) {
    const QImage rendered = card.grab().toImage();
    return rendered.pixelColor(rendered.width() - 10, rendered.height() - 10);
  };
  const bool result = expect(
      titleColor(user) ==
              QColor(QString::fromLatin1(codexui::UiStyle::blueText)) &&
          surfaceColor(user) ==
              QColor(QString::fromLatin1(codexui::UiStyle::blueSurface)) &&
          titleColor(update) ==
              QColor(QString::fromLatin1(codexui::UiStyle::yellowText)) &&
          surfaceColor(update) ==
              QColor(QString::fromLatin1(codexui::UiStyle::yellowSurface)) &&
          titleColor(final) ==
              QColor(QString::fromLatin1(codexui::UiStyle::purpleText)) &&
          surfaceColor(final) ==
              QColor(QString::fromLatin1(codexui::UiStyle::purpleSurface)),
      "You is blue, interim Codex is yellow, and final Codex is violet");
  qApp->setStyleSheet(originalStyleSheet);
  return result;
}

bool testActiveWorkBordersFollowStatus() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  VisibleCardData command{
      AuthoritativeItemKey{"active-border", "turn", "command"},
      CardKind::CommandExecution,
      "active-border",
      "turn",
      "command",
      CommandExecutionData{"sleep 1", {}, {}, {}, {}},
      nodegraph::NodeStatus::Running};
  ConversationCard commandCard(command, false);
  commandCard.resize(560, commandCard.sizeHint().height());
  commandCard.show();
  spin();
  auto *commandStatus =
      commandCard.findChild<QLabel *>(QStringLiteral("commandStatus"));
  const auto emphasizedAtMidpoint = [](ConversationCard &card) {
    const QImage frame = card.grab().toImage();
    return frame.pixelColor(1, frame.height() / 2).red() < 180;
  };
  bool result = expect(
      commandCard.data().status.semantic == nodegraph::NodeStatus::Running &&
          commandStatus && emphasizedAtMidpoint(commandCard) &&
          labelUsesColor(commandStatus, codexui::UiStyle::blueText),
      "a running command uses the emphasized card border and active header "
      "status");
  command.status = nodegraph::NodeStatus::Completed;
  result &= expect(commandCard.applyPresentation(command) !=
                           PresentationImpact::None &&
                       commandCard.data().status.semantic ==
                           nodegraph::NodeStatus::Completed &&
                       !emphasizedAtMidpoint(commandCard),
                   "a completed command returns to the normal card border");

  VisibleCardData image{AuthoritativeItemKey{"active-border", "turn", "image"},
                        CardKind::ImageGeneration,
                        "active-border",
                        "turn",
                        "image",
                        ImageGenerationData{{}, {}},
                        nodegraph::NodeStatus::Running};
  ConversationCard imageCard(image, false);
  auto *imageStatus =
      imageCard.findChild<QLabel *>(QStringLiteral("imageGenerationStatus"));
  result &= expect(
      imageCard.data().status.semantic == nodegraph::NodeStatus::Running &&
          imageStatus &&
          imageStatus->font().capitalization() == QFont::MixedCase &&
          imageStatus->text() == QStringLiteral("running") &&
          labelUsesColor(imageStatus, codexui::UiStyle::blueText),
      "a loading figure uses the emphasized card border and active header "
      "status");
  image.status = nodegraph::NodeStatus::Completed;
  result &= expect(
      imageCard.applyPresentation(image) != PresentationImpact::None &&
          imageCard.data().status.semantic ==
              nodegraph::NodeStatus::Completed &&
          imageStatus->text() == QStringLiteral("completed") &&
          labelUsesColor(imageStatus, codexui::UiStyle::greenText),
      "a loaded figure returns to the normal card border and success header "
      "status");
  qApp->setStyleSheet(originalStyleSheet);
  return result;
}

ConversationCard *card(ConversationView &view, const std::string &key) {
  const auto findMaterialized = [&]() -> ConversationCard * {
    for (ConversationCard *candidate : view.findChildren<ConversationCard *>())
      if (stableKey(candidate->data().key) == key)
        return candidate;
    return nullptr;
  };
  if (ConversationCard *materialized = findMaterialized())
    return materialized;
  const QModelIndex index = view.conversationModel()->indexForStableKey(key);
  const QRect geometry = view.visualRect(index);
  if (!index.isValid() || !geometry.intersects(view.viewport()->rect()))
    return nullptr;
  const QPoint position =
      geometry.intersected(view.viewport()->rect()).center();
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(position),
                    QPointF(position), view.viewport()->mapToGlobal(position),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &press);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position),
                      QPointF(position), view.viewport()->mapToGlobal(position),
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &release);
  QApplication::processEvents();
  return findMaterialized();
}

QString cardTitle(const ConversationCard *card) {
  if (!card)
    return {};
  const auto labels = card->findChildren<QLabel *>();
  const auto title = std::ranges::find_if(labels, [](QLabel *label) {
    return label->property("kind").toString() == QStringLiteral("title");
  });
  return title == labels.end() ? QString{} : (*title)->text();
}

QColor cardTitleColor(const ConversationCard *card) {
  if (!card)
    return {};
  const auto labels = card->findChildren<QLabel *>();
  const auto title = std::ranges::find_if(labels, [](QLabel *label) {
    return label->property("kind").toString() == QStringLiteral("title");
  });
  return title == labels.end()
             ? QColor{}
             : (*title)->palette().color(QPalette::WindowText);
}

std::vector<std::string> visualCardKeys(ConversationView &view) {
  std::vector<std::string> keys;
  keys.reserve(static_cast<std::size_t>(view.conversationModel()->rowCount()));
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
    const QModelIndex index = view.conversationModel()->index(row);
    if (index.data(ConversationItemModel::PresentedRole).toBool())
      keys.push_back(index.data(ConversationItemModel::StableKeyRole)
                         .toString()
                         .toStdString());
  }
  return keys;
}

bool hasConversationItem(ConversationView &view, const std::string &key) {
  return view.conversationModel()->indexForStableKey(key).isValid();
}

QToolButton *disclosure(ConversationCard *card) {
  return card ? card->findChild<QToolButton *>(
                    QStringLiteral("cardDisclosureButton"))
              : nullptr;
}

QToolButton *copyButton(ConversationCard *card) {
  if (!card)
    return nullptr;
  QWidget *header = card->findChild<QWidget *>(
      QStringLiteral("conversationCardHeader"), Qt::FindDirectChildrenOnly);
  return header
             ? header->findChild<QToolButton *>(
                   QStringLiteral("cardCopyButton"), Qt::FindDirectChildrenOnly)
             : nullptr;
}

int phaseTextRight(ConversationCard *card, QLabel *phase) {
  if (!card || !phase || !phase->parentWidget())
    return -1;
  return phase
      ->mapTo(card, QPoint(phase->contentsRect().right(),
                           phase->contentsRect().center().y()))
      .x();
}

bool usesReferencePhaseCopySpacing(ConversationCard *card, QLabel *phase) {
  QToolButton *copy = copyButton(card);
  if (!phase || !copy || phase->parentWidget() != copy->parentWidget())
    return false;
  const int copyLeft = copy->mapTo(card, QPoint{}).x();
  return !copy->isHidden() && phase->parentWidget()->layout()->spacing() == 0 &&
         copyLeft - phaseTextRight(card, phase) - 1 == 12;
}

QRect paintedDisclosureBounds(QToolButton *button, qreal devicePixelRatio = 1) {
  if (!button)
    return {};
  QImage image(QSize(qCeil(button->width() * devicePixelRatio),
                     qCeil(button->height() * devicePixelRatio)),
               QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(devicePixelRatio);
  image.fill(Qt::transparent);
  button->render(&image, QPoint{}, QRegion{}, QWidget::DrawChildren);
  QRect bounds;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (qAlpha(image.pixel(x, y)) > 0)
        bounds |= QRect(x, y, 1, 1);
    }
  }
  return bounds;
}

bool setFolded(ConversationCard *card, bool collapsed) {
  if (!card)
    return false;
  if (card->isCollapsed() == collapsed)
    return true;
  QPointer<ConversationCard> guard(card);
  QToolButton *button = disclosure(card);
  if (!button)
    return false;
  button->click();
  spin();
  return guard && guard->isCollapsed() == collapsed;
}

bool testAgentActivityLifecycleLabelRetention() {
  VisibleCardData activity{
      AuthoritativeItemKey{"agent-lifecycle", "turn", "activity"},
      CardKind::AgentActivity,
      "agent-lifecycle",
      "turn",
      "activity",
      AgentActivityData{"spawn_agent", {}, {}, "Inspect lifecycle", {}}};
  ConversationCard card(activity, true);
  card.resize(620, card.sizeHint().height());
  card.show();
  spin();
  auto *status =
      card.findChild<QLabel *>(QStringLiteral("agentActivityStatus"));
  bool result =
      expect((!status || status->isHidden()) &&
                 std::ranges::none_of(card.findChildren<QLabel *>(),
                                      [](QLabel *label) {
                                        return label->text() ==
                                               QStringLiteral("unknown");
                                      }),
             "agent activity with no authoritative lifecycle omits unknown");

  activity.status = nodegraph::NodeStatus::Completed;
  result &=
      expect(card.applyPresentation(activity) == PresentationImpact::PaintOnly,
             "collapsed agent completion updates only its header presentation");
  spin();
  status = card.findChild<QLabel *>(QStringLiteral("agentActivityStatus"));
  result &= expect(status && !status->isHidden() &&
                       status->text() == QStringLiteral("completed") &&
                       labelUsesColor(status, UiStyle::greenText),
                   "completed agent lifecycle remains visible in a QWidget");
  const QSize retainedSize = card.size();
  result &= expect(
      card.applyPresentation(activity) == PresentationImpact::None &&
          card.size() == retainedSize,
      "an identical agent lifecycle update performs no presentation work");
  return result;
}

bool testMarkdownLongLinesWrapInsideMaterializedCards() {
  std::string naturalLine;
  for (int word = 0; word < 90; ++word)
    naturalLine += "segment ";
  const std::string longToken =
      "/workspace/" + std::string(420, 'p') + "/artifact.txt";
  const std::string indented =
      "    keep    authored    indentation " + std::string(260, 'I');
  const std::string markdown =
      "An ordinary paragraph keeps its established wrapping.\n\n```text\n" +
      naturalLine + "\n" + longToken + "\n```\n\n" + indented;
  bool result = true;
  for (const bool nested : {false, true}) {
    ConversationCard card(
        VisibleCardData{AuthoritativeItemKey{"markdown-overflow", "turn",
                                             nested ? "nested" : "root"},
                        CardKind::AgentMessage, "markdown-overflow", "turn",
                        nested ? "nested" : "root",
                        AgentMessageData{markdown, false}},
        false);
    card.setNestedPresentation(nested);
    card.resize(nested ? 676 : 700, card.sizeHint().height());
    card.show();
    spin();
    auto *body = card.findChild<MarkdownTextView *>();
    if (!body) {
      result &= expect(false, "materialized Markdown body exists");
      continue;
    }
    bool linesBounded = true;
    for (const int width :
         {nested ? 676 : 700, nested ? 336 : 360, nested ? 676 : 700}) {
      card.resize(width, std::max(1, card.height()));
      spin();
      card.resize(width, card.sizeHint().height());
      spin();
      const qreal documentWidth = body->document()->textWidth();
      for (QTextBlock block = body->document()->begin(); block.isValid();
           block = block.next()) {
        const QTextLayout *layout = block.layout();
        if (!layout)
          continue;
        for (int line = 0; line < layout->lineCount(); ++line)
          linesBounded =
              linesBounded &&
              layout->lineAt(line).naturalTextWidth() <= documentWidth + 0.5;
      }
    }
    const QRect bodyRect(body->mapTo(&card, QPoint{}), body->size());
    result &= expect(
        bodyRect.left() >= card.contentsRect().left() &&
            bodyRect.right() <= card.contentsRect().right() &&
            body->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff &&
            body->markdownSource() == QString::fromStdString(markdown) &&
            body->toPlainText().contains(
                QStringLiteral("keep    authored    indentation")) &&
            linesBounded,
        nested ? "nested materialized Markdown wraps every code line inside "
                 "its editor"
               : "root materialized Markdown wraps every code line inside "
                 "its editor");

    QTextCursor selection(body->document());
    selection.setPosition(22, QTextCursor::KeepAnchor);
    body->setTextCursor(selection);
    const QString selected = body->textCursor().selectedText();
    body->setFocus(Qt::OtherFocusReason);
    QKeyEvent copySelection(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(body, &copySelection);
    result &= expect(!selected.isEmpty() &&
                         QApplication::clipboard()->text() == selected,
                     "selection and Ctrl+C remain native after code wraps");
    QApplication::clipboard()->clear();
    if (QToolButton *copy = copyButton(&card))
      copy->click();
    result &= expect(QApplication::clipboard()->text() ==
                         QString::fromStdString(markdown),
                     "card Copy retains the canonical Markdown source after "
                     "code wrapping");
  }
  return result;
}

std::pair<std::string, int> firstVisible(ConversationView &view) {
  for (int y = 0; y < view.viewport()->height(); ++y) {
    const QModelIndex index =
        view.indexAt(QPoint(view.viewport()->width() / 2, y));
    if (index.isValid())
      return {index.data(ConversationItemModel::StableKeyRole)
                  .toString()
                  .toStdString(),
              view.visualRect(index).top()};
  }
  return {};
}

class PaintAnchorProbe final : public QObject {
public:
  explicit PaintAnchorProbe(ConversationView &view) : view_(view) {
    view_.viewport()->installEventFilter(this);
  }

  ~PaintAnchorProbe() override { view_.viewport()->removeEventFilter(this); }

  void start(QWidget *tracked = nullptr) {
    anchors.clear();
    trackedGeometries.clear();
    representationCounts.clear();
    tracked_ = tracked;
    active = true;
  }

  std::vector<std::pair<std::string, int>> anchors;
  std::vector<QRect> trackedGeometries;
  std::vector<int> representationCounts;
  bool active = false;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (active && watched == view_.viewport() &&
        event->type() == QEvent::Paint) {
      anchors.push_back(firstVisible(view_));
      representationCounts.push_back(static_cast<int>(std::ranges::count_if(
          view_.findChildren<QWidget *>(), [](QWidget *widget) {
            return dynamic_cast<ConversationCard *>(widget) ||
                   widget->objectName() ==
                       QStringLiteral("conversationCardPlaceholder");
          })));
      if (tracked_)
        trackedGeometries.emplace_back(
            tracked_->mapTo(view_.viewport(), QPoint{}), tracked_->size());
    }
    return false;
  }

private:
  ConversationView &view_;
  QPointer<QWidget> tracked_;
};

void wheel(ConversationView &view, int pixelDelta) {
  const QPointF local(view.viewport()->rect().center());
  QWheelEvent event(local, view.viewport()->mapToGlobal(local.toPoint()),
                    QPoint(), QPoint(0, pixelDelta), Qt::NoButton,
                    Qt::NoModifier, Qt::ScrollUpdate, false);
  QApplication::sendEvent(view.viewport(), &event);
  spin();
}

void mouseWheelNotch(ConversationView &view, int angleDelta) {
  const QPointF local(view.viewport()->rect().center());
  QWheelEvent event(local, view.viewport()->mapToGlobal(local.toPoint()),
                    QPoint(), QPoint(0, angleDelta), Qt::NoButton,
                    Qt::NoModifier, Qt::ScrollUpdate, false);
  QApplication::sendEvent(view.viewport(), &event);
  spin();
}

bool testStructuralOrderAndIdentity() {
  ConversationView view;
  view.resize(620, 420);
  view.show();
  ConversationGraphSpec snapshot = conversation("structural-order", 8);
  applyConversation(view, snapshot);
  spin();

  std::unordered_map<std::string, QPersistentModelIndex> identities;
  for (const TurnGraphSpec &section : snapshot.sections)
    for (const VisibleCardData &value : section.cards)
      identities.emplace(
          stableKey(value.key),
          view.conversationModel()->indexForStableKey(stableKey(value.key)));

  for (TurnGraphSpec &section : snapshot.sections)
    std::ranges::reverse(section.cards);
  std::ranges::reverse(snapshot.sections);
  std::vector<std::string> expectedKeys;
  for (const TurnGraphSpec &section : snapshot.sections)
    for (const VisibleCardData &value : section.cards)
      expectedKeys.push_back(stableKey(value.key));

  bool result = expect(applyConversation(view, snapshot),
                       "structural order changes reconcile");
  spin();
  result &= expect(visualCardKeys(view) == expectedKeys,
                   "section and card order follows the projection exactly");
  bool retainedIdentity = true;
  for (const auto &[key, identity] : identities) {
    const QModelIndex current =
        view.conversationModel()->indexForStableKey(key);
    retainedIdentity = retainedIdentity && identity.isValid() &&
                       current.isValid() && identity == current;
  }
  result &= expect(retainedIdentity,
                   "structural moves preserve stable item identity");

  const std::string pagingThread = "turn-root-paging";
  VisibleCardData laterPrompt{
      AuthoritativeItemKey{pagingThread, "turn", "later-user"},
      CardKind::UserMessage,
      pagingThread,
      "turn",
      "later-user",
      UserMessageData{"Later prompt", {}}};
  VisibleCardData activity = agentCard(pagingThread, "turn", 50);
  ConversationGraphSpec paged{
      pagingThread,
      {{"turn:paged", "turn", {laterPrompt, activity}, laterPrompt.key}},
      false};
  ConversationView pagedView;
  pagedView.resize(620, 420);
  pagedView.show();
  applyConversation(pagedView, paged);
  spin();
  const QPersistentModelIndex laterIdentity =
      pagedView.conversationModel()->indexForStableKey(
          stableKey(laterPrompt.key));
  const QPersistentModelIndex activityIdentity =
      pagedView.conversationModel()->indexForStableKey(stableKey(activity.key));
  VisibleCardData earlierPrompt{
      AuthoritativeItemKey{pagingThread, "turn", "earlier-user"},
      CardKind::UserMessage,
      pagingThread,
      "turn",
      "earlier-user",
      UserMessageData{"Earlier prompt", {}}};
  paged.sections.front().cards.insert(paged.sections.front().cards.begin(),
                                      earlierPrompt);
  paged.sections.front().rootCardKey = earlierPrompt.key;
  result &= expect(applyConversation(pagedView, paged),
                   "older history can introduce the real turn prompt");
  spin();
  const QModelIndex earlierRoot =
      pagedView.conversationModel()->indexForStableKey(
          stableKey(earlierPrompt.key));
  const QModelIndex laterRoot =
      pagedView.conversationModel()->indexForStableKey(
          stableKey(laterPrompt.key));
  const QModelIndex activityRow =
      pagedView.conversationModel()->indexForStableKey(stableKey(activity.key));
  result &= expect(
      earlierRoot.isValid() && laterRoot.isValid() && activityRow.isValid() &&
          earlierRoot.row() < laterRoot.row() &&
          laterRoot.row() < activityRow.row() &&
          earlierRoot.data(ConversationItemModel::TurnRootRole).toBool() &&
          laterRoot.data(ConversationItemModel::NestedCardRole).toBool() &&
          activityRow.data(ConversationItemModel::NestedCardRole).toBool() &&
          pagedView.visualRect(laterRoot).left() >
              pagedView.visualRect(earlierRoot).left(),
      "history paging installs the canonical root and nested row "
      "geometry");

  paged.sections.front().cards.erase(paged.sections.front().cards.begin());
  result &= expect(applyConversation(pagedView, paged),
                   "a transient projection can omit the declared root");
  spin();
  result &= expect(
      laterIdentity.isValid() && activityIdentity.isValid() &&
          !laterIdentity.data(ConversationItemModel::TurnRootRole).toBool() &&
          !laterIdentity.data(ConversationItemModel::NestedCardRole).toBool() &&
          !activityIdentity.data(ConversationItemModel::NestedCardRole)
               .toBool(),
      "a retained steering message never becomes an inferred turn root");

  paged.sections.front().cards.insert(paged.sections.front().cards.begin(),
                                      earlierPrompt);
  result &= expect(applyConversation(pagedView, paged),
                   "the declared turn root can return");
  spin();
  const QModelIndex restoredRoot =
      pagedView.conversationModel()->indexForStableKey(
          stableKey(earlierPrompt.key));
  result &= expect(
      restoredRoot.isValid() && laterIdentity.isValid() &&
          activityIdentity.isValid() &&
          restoredRoot.data(ConversationItemModel::TurnRootRole).toBool() &&
          laterIdentity.data(ConversationItemModel::NestedCardRole).toBool() &&
          activityIdentity.data(ConversationItemModel::NestedCardRole).toBool(),
      "root restoration re-nests retained rows without changing their "
      "stable identity");
  return result;
}

bool testFollowPauseAndStableAnchor() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationGraphSpec snapshot = conversation("thread-a", 34);
  bool result =
      expect(applyConversation(view, snapshot), "initial projection renders");
  spin();
  QScrollArea nativeReference;
  nativeReference.setWidgetResizable(true);
  auto *nativeContent = new QWidget;
  nativeContent->setMinimumHeight(5000);
  nativeReference.setWidget(nativeContent);
  nativeReference.resize(view.size());
  nativeReference.show();
  spin();
  result &=
      expect(view.verticalScrollBar()->singleStep() ==
                 nativeReference.verticalScrollBar()->singleStep(),
             "conversation line-step matches the previous native QScrollArea");
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom(),
                   "a new thread starts following at its real bottom");

  const int oldValue = view.verticalScrollBar()->value();
  snapshot.sections.back().cards.push_back(agentCard("thread-a", "turn-2", 34));
  result &=
      expect(applyConversation(view, snapshot), "a new card materializes");
  int previous = view.verticalScrollBar()->value();
  bool monotonic = previous >= oldValue;
  QElapsedTimer animation;
  animation.start();
  while (animation.elapsed() < 400 && !view.isAtBottom()) {
    spin(8);
    const int current = view.verticalScrollBar()->value();
    monotonic = monotonic && current >= previous;
    previous = current;
  }
  result &= expect(monotonic && view.isAtBottom(),
                   "follow animation is monotonic and reaches the new bottom");

  const int beforeWheelNotch = view.verticalScrollBar()->value();
  const auto beforeWheelAnchor = firstVisible(view);
  const QModelIndex beforeWheelIndex =
      view.conversationModel()->indexForStableKey(beforeWheelAnchor.first);
  mouseWheelNotch(view, 120);
  const int nativeWheelDistance = std::min(
      beforeWheelNotch, view.verticalScrollBar()->singleStep() *
                            std::max(1, QApplication::wheelScrollLines()));
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       !view.isAtBottom() && beforeWheelIndex.isValid() &&
                       view.visualRect(beforeWheelIndex).top() -
                               beforeWheelAnchor.second ==
                           nativeWheelDistance,
                   "native mouse-wheel handling uses the configured line "
                   "distance and "
                   "pauses following immediately");
  const auto anchor = firstVisible(view);
  result &=
      expect(!anchor.first.empty(), "paused view has a visible card anchor");

  // Reflow a card above the anchor and append another card in one projection.
  const auto anchorPosition =
      std::ranges::find_if(snapshot.sections.front().cards,
                           [&anchor](const VisibleCardData &candidate) {
                             return stableKey(candidate.key) == anchor.first;
                           });
  if (anchorPosition != snapshot.sections.front().cards.begin() &&
      anchorPosition != snapshot.sections.front().cards.end()) {
    auto &message = std::get<AgentMessageData>((anchorPosition - 1)->payload);
    message.text +=
        "\nA reflowing upstream update.\nA second line.\nA third line.";
  }
  snapshot.sections.back().cards.push_back(agentCard("thread-a", "turn-2", 35));
  LayoutRequestProbe layoutRequests(&view);
  result &= expect(applyConversation(view, snapshot),
                   "paused incoming changes still materialize");
  layoutRequests.start();
  spin();
  result &= expect(layoutRequests.count <= 24,
                   "a paused append leaves only bounded ancestor/new-card "
                   "layout settlement across sliced graph rendering");
  const auto after = firstVisible(view);
  result &= expect(after.first == anchor.first &&
                       std::abs(after.second - anchor.second) <= 1,
                   "paused reconciliation preserves key and pixel anchor");
  result &= expect(
      hasConversationItem(view,
                          stableKey(snapshot.sections.back().cards.back().key)),
      "paused mode admits the later row without disturbing the viewport");

  const int unchangedValue = view.verticalScrollBar()->value();
  const auto unchangedAnchor = firstVisible(view);
  result &= expect(!applyConversation(view, snapshot),
                   "an identical visible projection is a true no-op");
  spin();
  result &= expect(view.verticalScrollBar()->value() == unchangedValue &&
                       firstVisible(view) == unchangedAnchor,
                   "a no-op changes neither scroll nor visible geometry");

  while (!view.isAtBottom())
    wheel(view, -300);
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
  spin();
  const auto pageStepAnchor = firstVisible(view);
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       !pageStepAnchor.first.empty(),
                   "a scrollbar page action pauses at its resulting anchor");
  auto &upstream = std::get<AgentMessageData>(
      snapshot.sections.front().cards.front().payload);
  upstream.text += "\nTrack-action upstream reflow.\nSecond line.\nThird line.";
  result &= expect(applyConversation(view, snapshot),
                   "page-action coverage applies an upstream reflow");
  spin();
  const auto afterPageStepReflow = firstVisible(view);
  result &= expect(
      afterPageStepReflow.first == pageStepAnchor.first &&
          std::abs(afterPageStepReflow.second - pageStepAnchor.second) <= 1,
      "page-action scroll ownership survives later reflow");
  return result;
}

bool testPausedExpandedCommandStaysPainted() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  const std::string thread = "paused-expanded-command";
  ConversationGraphSpec snapshot = conversation(
      thread,
      qEnvironmentVariableIsSet("CODEXUI_CONVERSATION_TIMINGS") ? 80 : 24);
  QString output;
  for (int line = 0; line < 48; ++line)
    output += QStringLiteral("completed command output %1\n").arg(line);
  VisibleCardData completedCommand{
      AuthoritativeItemKey{thread, "turn-2", "completed-command"},
      CardKind::CommandExecution,
      thread,
      "turn-2",
      "completed-command",
      CommandExecutionData{"run completed command", utf8(output), "/workspace",
                           0, 1250},
      nodegraph::NodeStatus::Completed};
  snapshot.sections.back().cards.insert(
      snapshot.sections.back().cards.begin(),
      cardForAppearanceAudit(thread, CardKind::UserMessage, 99));
  snapshot.sections.back().rootCardKey =
      snapshot.sections.back().cards.front().key;
  snapshot.sections.back().cards.push_back(completedCommand);

  ConversationView view;
  view.resize(620, 420);
  view.show();
  bool result = expect(applyConversation(view, snapshot),
                       "expanded-command audit renders its conversation");
  spin();
  QPointer<ConversationCard> commandCard =
      card(view, stableKey(completedCommand.key));
  result &= expect(setFolded(commandCard, false),
                   "completed command is expanded before incoming cards");
  QPointer<CommandOutputView> outputView =
      commandCard ? commandCard->findChild<CommandOutputView *>(
                        QStringLiteral("commandOutputView"))
                  : nullptr;
  if (outputView && outputView->verticalScrollBar()->maximum() > 0) {
    outputView->verticalScrollBar()->setValue(
        outputView->verticalScrollBar()->maximum() / 2);
    spin();
  }
  result &= expect(commandCard && outputView &&
                       view.mode() == ConversationView::Mode::Paused,
                   "expanded completed command owns a paused viewport");

  PaintAnchorProbe paintProbe(view);
  const std::vector<CardKind> incomingKinds{
      CardKind::UserMessage,      CardKind::AgentMessage,
      CardKind::CommandExecution, CardKind::AgentActivity,
      CardKind::Reasoning,        CardKind::FileChanges,
      CardKind::ImageGeneration,  CardKind::Plan,
      CardKind::GenericActivity,  CardKind::LocalPrompt};
  const auto stableAgainst = [](const std::pair<std::string, int> &reference,
                                const std::pair<std::string, int> &candidate) {
    return candidate.first == reference.first &&
           std::abs(candidate.second - reference.second) <= 1;
  };
  bool allIncomingRowsAdmitted = true;
  for (std::size_t index = 0; index < incomingKinds.size(); ++index) {
    if (!commandCard) {
      result &= expect(false, "incoming activity retains the visible expanded "
                              "command QWidget");
      break;
    }
    const auto anchorBefore = firstVisible(view);
    const QRect commandBefore(commandCard->mapTo(view.viewport(), QPoint{}),
                              commandCard->size());
    const auto outputStateBefore = commandCard->state().commandOutput;
    const qulonglong fullGeometryBefore =
        view.property("conversationGeometryPasses").toULongLong();
    const qulonglong localGeometryBefore =
        view.property("conversationLocalGeometryPasses").toULongLong();
    const qulonglong cachedAppendBefore =
        view.property("conversationCachedAppendGeometryPasses").toULongLong();
    const qulonglong structuralCommitsBefore =
        view.property("incrementalStructuralCommits").toULongLong();
    const qulonglong stageCommitsBefore =
        view.property("structuralStageCommits").toULongLong();
    snapshot.sections.back().cards.push_back(cardForAppearanceAudit(
        thread, incomingKinds[index], 100 + static_cast<int>(index)));
    const std::string incomingKey =
        stableKey(snapshot.sections.back().cards.back().key);
    paintProbe.start(commandCard);
    QElapsedTimer insertionTimer;
    insertionTimer.start();
    static_cast<void>(view.reconcileStaged(projectConversation(snapshot)));
    const auto stagedAnchor = firstVisible(view);
    const bool hiddenUntilCommit = card(view, incomingKey) == nullptr;
    const bool changed = spinUntil([&] {
      return view.property("structuralStageCommits").toULongLong() ==
             stageCommitsBefore + 1;
    });
    if (qEnvironmentVariableIsSet("CODEXUI_CONVERSATION_TIMINGS"))
      std::cerr << "single insertion kind="
                << static_cast<int>(incomingKinds[index])
                << " us=" << insertionTimer.nsecsElapsed() / 1000
                << " construct="
                << view.property("lastStructuralStageCardConstructionMicros")
                       .toLongLong()
                << " validation="
                << view.property("lastIncrementalValidationMicros").toLongLong()
                << " geometry="
                << view.property("lastIncrementalGeometryMicros").toLongLong()
                << " structural="
                << view.property("lastIncrementalStructuralMicros").toLongLong()
                << '\n';
    const auto immediateAnchor = firstVisible(view);
    const QRect immediateCommand(commandCard->mapTo(view.viewport(), QPoint{}),
                                 commandCard->size());
    QPointer<ConversationCard> incomingCard = card(view, incomingKey);
    const int immediateIncomingHeight =
        incomingCard ? incomingCard->height() : -1;
    if (!hasConversationItem(view, incomingKey))
      allIncomingRowsAdmitted = false;
    spin(80);
    paintProbe.active = false;
    if (!commandCard) {
      result &= expect(false, "incoming activity retains the visible expanded "
                              "command QWidget through settlement");
      break;
    }
    const auto settledAnchor = firstVisible(view);
    const QRect settledCommand(commandCard->mapTo(view.viewport(), QPoint{}),
                               commandCard->size());
    const int settledIncomingHeight =
        incomingCard ? incomingCard->height() : -1;
    const bool paintedAnchorStable =
        std::ranges::all_of(paintProbe.anchors, [&](const auto &anchor) {
          return stableAgainst(anchorBefore, anchor);
        });
    const bool paintedStable = std::ranges::all_of(
        paintProbe.trackedGeometries, [&commandBefore](const QRect &geometry) {
          return geometry == commandBefore;
        });
    const bool incomingWidgetStable =
        immediateIncomingHeight < 0
            ? !incomingCard
            : incomingCard && immediateIncomingHeight == settledIncomingHeight;
    const bool auditPass =
        changed && hiddenUntilCommit &&
        stableAgainst(anchorBefore, stagedAnchor) &&
        card(view, stableKey(completedCommand.key)) == commandCard &&
        view.mode() == ConversationView::Mode::Paused &&
        stableAgainst(anchorBefore, immediateAnchor) &&
        stableAgainst(anchorBefore, settledAnchor) &&
        immediateCommand == commandBefore && settledCommand == commandBefore &&
        paintedAnchorStable && paintedStable && incomingWidgetStable &&
        commandCard->state().commandOutput == outputStateBefore &&
        view.property("conversationGeometryPasses").toULongLong() ==
            fullGeometryBefore &&
        view.property("conversationLocalGeometryPasses").toULongLong() ==
            localGeometryBefore &&
        hasConversationItem(view, incomingKey) &&
        view.materializedCardCount() <= 48;
    if (!auditPass)
      std::cerr
          << "incoming audit kind=" << static_cast<int>(incomingKinds[index])
          << " anchorImmediate=" << stableAgainst(anchorBefore, immediateAnchor)
          << " anchorSettled=" << stableAgainst(anchorBefore, settledAnchor)
          << " commandImmediate=" << (immediateCommand == commandBefore)
          << " commandSettled=" << (settledCommand == commandBefore)
          << " paintAnchor=" << paintedAnchorStable
          << " paintGeometry=" << paintedStable
          << " widget=" << incomingWidgetStable << " local="
          << view.property("conversationLocalGeometryPasses").toULongLong()
          << '/' << localGeometryBefore << " cached="
          << view.property("conversationCachedAppendGeometryPasses")
                 .toULongLong()
          << '/' << cachedAppendBefore << " outputState="
          << (commandCard->state().commandOutput == outputStateBefore)
          << " identity="
          << (card(view, stableKey(completedCommand.key)) == commandCard)
          << " mode=" << (view.mode() == ConversationView::Mode::Paused)
          << " full="
          << view.property("conversationGeometryPasses").toULongLong() << '/'
          << fullGeometryBefore << " structural="
          << view.property("incrementalStructuralCommits").toULongLong() << '/'
          << structuralCommitsBefore << '\n';
    result &= expect(
        auditPass,
        "incoming card preserves a visible expanded command in every paint "
        "and settles only its affected Turn");
  }
  result &= expect(allIncomingRowsAdmitted,
                   "selected-thread incoming cards enter the canonical item "
                   "order immediately");

  auto appendedCommand = std::ranges::find_if(
      snapshot.sections.back().cards, [](const VisibleCardData &candidate) {
        return candidate.kind == CardKind::CommandExecution &&
               candidate.itemId == "appearance-102";
      });
  const std::string appendedCommandKey =
      appendedCommand == snapshot.sections.back().cards.end()
          ? std::string{}
          : stableKey(appendedCommand->key);
  const auto completionAnchorBefore = firstVisible(view);
  const int completionRangeBefore = view.verticalScrollBar()->maximum();
  const qulonglong completionFullGeometryBefore =
      view.property("conversationGeometryPasses").toULongLong();
  const qulonglong completionLocalGeometryBefore =
      view.property("conversationLocalGeometryPasses").toULongLong();
  if (appendedCommand != snapshot.sections.back().cards.end()) {
    auto &data = std::get<CommandExecutionData>(appendedCommand->payload);
    data.exitCode = 0;
    data.durationMilliseconds = 20;
    appendedCommand->status = nodegraph::NodeStatus::Completed;
  }
  const bool appendedCommandCompleted = applyConversation(view, snapshot);
  spin();
  const QModelIndex appendedCommandIndex =
      view.conversationModel()->indexForStableKey(appendedCommandKey);
  const VisibleCardData *appendedCommandData =
      view.conversationModel()->card(appendedCommandIndex.row());
  const auto *appendedCommandPresentation =
      appendedCommandData
          ? std::get_if<CommandExecutionData>(&appendedCommandData->payload)
          : nullptr;
  result &= expect(
      appendedCommandCompleted && appendedCommandIndex.isValid() &&
          appendedCommandPresentation && appendedCommandData &&
          appendedCommandData->status.semantic ==
              nodegraph::NodeStatus::Completed &&
          view.verticalScrollBar()->maximum() == completionRangeBefore &&
          stableAgainst(completionAnchorBefore, firstVisible(view)) &&
          view.property("conversationGeometryPasses").toULongLong() ==
              completionFullGeometryBefore &&
          view.property("conversationLocalGeometryPasses").toULongLong() ==
              completionLocalGeometryBefore,
      "an offscreen running command completes in its exact model row without "
      "widget work or paused-viewport movement");

  const QModelIndex turnRoot = view.conversationModel()->indexForStableKey(
      stableKey(*snapshot.sections.back().rootCardKey));
  view.resize(view.width() - 24, view.height());
  spin();
  result &= expect(
      turnRoot.isValid() &&
          turnRoot.data(ConversationItemModel::TurnRootRole).toBool() &&
          std::ranges::all_of(
              snapshot.sections.back().cards,
              [&](const VisibleCardData &data) {
                const QModelIndex retained =
                    view.conversationModel()->indexForStableKey(
                        stableKey(data.key));
                return retained.isValid() &&
                       (retained == turnRoot ||
                        retained.data(ConversationItemModel::NestedCardRole)
                            .toBool());
              }) &&
          view.materializedCardCount() <= 48,
      "a later viewport resize preserves every virtual row in its Turn/You "
      "geometry with bounded editors");

  const auto sectionAnchorBefore = firstVisible(view);
  const qulonglong fullBeforeNewTurn =
      view.property("conversationGeometryPasses").toULongLong();
  const qulonglong localBeforeNewTurn =
      view.property("conversationLocalGeometryPasses").toULongLong();
  const qulonglong cachedSectionBefore =
      view.property("conversationCachedSectionAppendGeometryPasses")
          .toULongLong();
  const qulonglong sectionStageCommitsBefore =
      view.property("structuralStageCommits").toULongLong();
  VisibleCardData newTurnPrompt{
      LocalPromptKey{12'345},
      CardKind::LocalPrompt,
      thread,
      "turn-3",
      {},
      LocalPromptData{12'345,
                      "A newly admitted turn",
                      PromptState::InFlight,
                      true,
                      {},
                      {},
                      QDateTime::currentMSecsSinceEpoch(),
                      false}};
  snapshot.sections.push_back(
      {"turn:" + thread + ":3", "turn-3", {newTurnPrompt}, newTurnPrompt.key});
  snapshot.activeTurnId = "turn-3";
  QElapsedTimer newTurnTimer;
  newTurnTimer.start();
  static_cast<void>(view.reconcileStaged(projectConversation(snapshot)));
  const bool newTurnHiddenUntilCommit =
      card(view, stableKey(newTurnPrompt.key)) == nullptr &&
      stableAgainst(sectionAnchorBefore, firstVisible(view));
  const bool newTurnChanged = spinUntil([&] {
    return view.property("structuralStageCommits").toULongLong() ==
           sectionStageCommitsBefore + 1;
  });
  if (qEnvironmentVariableIsSet("CODEXUI_CONVERSATION_TIMINGS"))
    std::cerr << "new turn insertion us=" << newTurnTimer.nsecsElapsed() / 1000
              << '\n';
  const auto sectionAnchorAfter = firstVisible(view);
  result &= expect(
      newTurnChanged && newTurnHiddenUntilCommit &&
          hasConversationItem(view, stableKey(newTurnPrompt.key)) &&
          stableAgainst(sectionAnchorBefore, sectionAnchorAfter) &&
          view.property("conversationGeometryPasses").toULongLong() ==
              fullBeforeNewTurn &&
          view.property("conversationLocalGeometryPasses").toULongLong() ==
              localBeforeNewTurn &&
          view.materializedCardCount() <= 48,
      "a new Turn/You row commits atomically without traversing retained "
      "widgets or moving a paused viewport");

  qApp->setStyleSheet(originalStyleSheet);
  spin();
  return result;
}

bool testCommandCompletionWithoutGeometryWork() {
  const std::string thread = "command-completion-paint-only";
  VisibleCardData prompt{AuthoritativeItemKey{thread, "turn", "prompt"},
                         CardKind::UserMessage,
                         thread,
                         "turn",
                         "prompt",
                         UserMessageData{"Run the command", {}}};
  QString output;
  for (int line = 0; line < 80; ++line)
    output += QStringLiteral("streamed output line %1\n").arg(line);
  VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{
          "run long command", utf8(output), "/workspace", {}, {}},
      nodegraph::NodeStatus::Running};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.activeTurnId = "turn";
  snapshot.sections.push_back(
      {"turn-section", "turn", {prompt, command}, prompt.key});

  ConversationView view;
  view.resize(760, 420);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "running command completion audit renders");
  spin();
  ConversationCard *commandCard = card(view, stableKey(command.key));
  if (!commandCard)
    return expect(false, "running command completion audit owns its card");
  const int heightBefore = commandCard->height();
  const int rangeBefore = view.verticalScrollBar()->maximum();
  const qulonglong fullGeometryBefore =
      view.property("conversationGeometryPasses").toULongLong();
  const qulonglong localGeometryBefore =
      view.property("conversationLocalGeometryPasses").toULongLong();

  auto &completed = std::get<CommandExecutionData>(command.payload);
  completed.exitCode = 0;
  completed.durationMilliseconds = 12'000;
  command.status = nodegraph::NodeStatus::Completed;
  const std::optional<PresentationImpact> impact =
      applyPresentation(view, command);
  spin();

  auto *status =
      commandCard->findChild<QLabel *>(QStringLiteral("commandStatus"));
  const bool completionStayedLocal =
      impact == PresentationImpact::PaintOnly && status &&
      status->text() == QStringLiteral("completed") &&
      commandCard->data().status.semantic == nodegraph::NodeStatus::Completed &&
      commandCard->height() == heightBefore &&
      view.verticalScrollBar()->maximum() == rangeBefore &&
      view.property("conversationGeometryPasses").toULongLong() ==
          fullGeometryBefore &&
      view.property("conversationLocalGeometryPasses").toULongLong() ==
          localGeometryBefore;
  if (!completionStayedLocal)
    std::cerr << "completion impact="
              << (impact ? static_cast<int>(*impact) : -1)
              << " height=" << heightBefore << "->" << commandCard->height()
              << " range=" << rangeBefore << "->"
              << view.verticalScrollBar()->maximum()
              << " full=" << fullGeometryBefore << "->"
              << view.property("conversationGeometryPasses").toULongLong()
              << " local=" << localGeometryBefore << "->"
              << view.property("conversationLocalGeometryPasses").toULongLong()
              << '\n';
  result &= expect(
      completionStayedLocal,
      "running-to-completed patches lifecycle paint without conversation "
      "geometry or scroll-range work");
  return result;
}

bool testStreamingAgentBecomesVisibleWithoutReselection() {
  const std::string thread = "streaming-final-visibility";
  VisibleCardData prompt{AuthoritativeItemKey{thread, "turn", "prompt"},
                         CardKind::UserMessage,
                         thread,
                         "turn",
                         "prompt",
                         UserMessageData{"Prompt", {}}};
  VisibleCardData response{
      AuthoritativeItemKey{thread, "turn", "streaming-response"},
      CardKind::AgentMessage,
      thread,
      "turn",
      "streaming-response",
      AgentMessageData{"The completed response must appear immediately.",
                       false}};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.sections.push_back(
      {"turn-section", "turn", {prompt, response}, prompt.key});

  ConversationView view;
  view.setPresentationOptions({true, false, false, false});
  view.resize(620, 420);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "a filtered streaming response is retained");
  spin();
  const QModelIndex responseIndex =
      view.conversationModel()->indexForStableKey(stableKey(response.key));
  const QModelIndex rootIndex = view.conversationModel()->indexForStableKey(
      stableKey(*snapshot.sections.front().rootCardKey));
  result &= expect(
      responseIndex.isValid() && rootIndex.isValid() &&
          !responseIndex.data(ConversationItemModel::PresentedRole).toBool() &&
          card(view, stableKey(response.key)) == nullptr,
      "the streaming response performs no visible work while "
      "updates are filtered");

  VisibleCardData completed = snapshot.sections.front().cards.back();
  std::get<AgentMessageData>(completed.payload).finalAnswer = true;
  result &= expect(applyPresentation(view, std::move(completed)) ==
                       PresentationImpact::GeometryChanged,
                   "completion makes the indexed response visible");
  spin();
  result &= expect(
      responseIndex.data(ConversationItemModel::PresentedRole).toBool() &&
          responseIndex.data(ConversationItemModel::NestedCardRole).toBool() &&
          view.visualRect(responseIndex).height() > 0 &&
          view.visualRect(responseIndex).left() >
              view.visualRect(rootIndex).left(),
      "the final response and its settled owner appear without "
      "thread reselection");

  ConversationView optimisticView;
  optimisticView.setPresentationOptions({true, false, false, false});
  optimisticView.resize(620, 420);
  optimisticView.show();
  ConversationSnapshot liveSnapshot;
  liveSnapshot.threadId = "optimistic-live-final";
  static_cast<void>(optimisticView.reconcile(liveSnapshot));
  VisibleCardData localPrompt{
      LocalPromptKey{1},
      CardKind::LocalPrompt,
      liveSnapshot.threadId,
      "live-turn",
      {},
      LocalPromptData{1, "Live prompt", PromptState::InFlight, 0, {}, {}}};
  liveSnapshot.sections.push_back(
      {"live-turn-section", "live-turn", {localPrompt}, localPrompt.key});
  result &= expect(changed(optimisticView.reconcile(liveSnapshot)),
                   "an optimistic Turn/You owner inserts immediately");
  liveSnapshot.sections.front().cards.front().kind = CardKind::UserMessage;
  liveSnapshot.sections.front().cards.front().payload =
      UserMessageData{"Live prompt", {}};
  result &= expect(changed(optimisticView.reconcile(liveSnapshot)),
                   "the optimistic Turn/You owner acknowledges in place");
  VisibleCardData liveResponse{
      AuthoritativeItemKey{liveSnapshot.threadId, "live-turn", "live-answer"},
      CardKind::AgentMessage,
      liveSnapshot.threadId,
      "live-turn",
      "live-answer",
      AgentMessageData{"Live final answer", true}};
  liveSnapshot.sections.front().cards.push_back(liveResponse);
  result &= expect(changed(optimisticView.reconcile(liveSnapshot)),
                   "the final response inserts into the acknowledged Turn");
  spin();
  const QModelIndex liveRoot =
      optimisticView.conversationModel()->indexForStableKey(
          stableKey(localPrompt.key));
  const QModelIndex liveAnswer =
      optimisticView.conversationModel()->indexForStableKey(
          stableKey(liveResponse.key));
  result &= expect(
      liveRoot.isValid() && liveAnswer.isValid() &&
          liveRoot.data(ConversationItemModel::TurnRootRole).toBool() &&
          liveAnswer.data(ConversationItemModel::NestedCardRole).toBool() &&
          optimisticView.visualRect(liveAnswer).height() > 0,
      "the optimistic live sequence exposes the final answer in "
      "its settled Turn without reselection");
  LayoutRequestProbe idleLayoutRequests(&optimisticView);
  idleLayoutRequests.start();
  spin(40);
  idleLayoutRequests.active = false;
  result &= expect(
      idleLayoutRequests.count <= 4,
      "the acknowledged prompt widget reaches layout quiescence after its "
      "final answer arrives");
  return result;
}

bool testThreadLocalScrollState() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationGraphSpec first = conversation("thread-a", 30);
  ConversationGraphSpec second = conversation("thread-b", 26);
  applyConversation(view, first);
  spin();
  wheel(view, 220);
  const auto saved = firstVisible(view);
  bool result = expect(view.mode() == ConversationView::Mode::Paused,
                       "first thread is paused before switching");

  applyConversation(view, second);
  spin();
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom(),
                   "a new thread does not inherit another thread's pause");
  applyConversation(view, first);
  spin();
  const auto restored = firstVisible(view);
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       restored.first == saved.first &&
                       std::abs(restored.second - saved.second) <= 1,
                   "switching back restores that thread's own visual anchor");

  return result;
}

bool testPromptAdmissionFollowOwnership() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationGraphSpec snapshot = conversation("prompt-follow", 30);
  applyConversation(view, snapshot);
  spin();

  bool result = true;
  VisibleCardData pending{LocalPromptKey{1001},
                          CardKind::LocalPrompt,
                          "prompt-follow",
                          {},
                          {},
                          LocalPromptData{1001,
                                          "a newly admitted pending prompt",
                                          PromptState::InFlight,
                                          0,
                                          {}}};
  snapshot.sections.back().cards.push_back(pending);
  applyConversation(view, snapshot);
  ConversationCard *pendingCard = nullptr;
  const bool admittedPromptReady = spinUntil(
      [&] {
        pendingCard = card(view, stableKey(pending.key));
        return view.isAtBottom() && pendingCard &&
               pendingCard->mapTo(view.viewport(), QPoint{}).y() +
                       pendingCard->height() <=
                   view.viewport()->height();
      },
      512);
  result &= expect(
      admittedPromptReady && view.mode() == ConversationView::Mode::Following,
      "a following view reveals the complete admitted prompt");

  wheel(view, 180);
  const auto userAnchor = firstVisible(view);
  VisibleCardData later = pending;
  later.key = LocalPromptKey{1002};
  std::get<LocalPromptData>(later.payload).submissionId = 1002;
  std::get<LocalPromptData>(later.payload).prompt =
      "must not displace a user-owned reading position";
  snapshot.sections.back().cards.push_back(later);
  applyConversation(view, snapshot);
  spin(40);
  const auto retainedAnchor = firstVisible(view);
  result &=
      expect(view.mode() == ConversationView::Mode::Paused &&
                 retainedAnchor.first == userAnchor.first &&
                 std::abs(retainedAnchor.second - userAnchor.second) <= 1,
             "local admission never overrides an explicit user scroll pause");
  return result;
}

bool testCardCopyControls() {
  const std::string thread = "copy-controls";
  struct CopyCase {
    VisibleCardData card;
    QString expected;
    bool markdown = false;
  };
  const std::vector<CopyCase> cases{
      {{AuthoritativeItemKey{thread, "turn", "user"}, CardKind::UserMessage,
        thread, "turn", "user",
        UserMessageData{"# Prompt\n\n**bold**",
                        {"/tmp/first.png", "/tmp/second.png"}}},
       QStringLiteral("# Prompt\n\n**bold**"),
       true},
      {{AuthoritativeItemKey{thread, "turn", "image-only-user"},
        CardKind::UserMessage, thread, "turn", "image-only-user",
        UserMessageData{{}, {"/tmp/only-image.png"}}},
       QStringLiteral("/tmp/only-image.png"),
       false},
      {{AuthoritativeItemKey{thread, "turn", "agent"}, CardKind::AgentMessage,
        thread, "turn", "agent", AgentMessageData{"## Answer\n\n- item", true}},
       QStringLiteral("## Answer\n\n- item"),
       true},
      {{AuthoritativeItemKey{thread, "turn", "command"},
        CardKind::CommandExecution, thread, "turn", "command",
        CommandExecutionData{"printf copy\n\n", "one\n\n", {}, 0, {}}},
       QStringLiteral("printf copy\n\none"),
       false},
      {{AuthoritativeItemKey{thread, "turn", "activity"},
        CardKind::AgentActivity, thread, "turn", "activity",
        AgentActivityData{"tool", {}, "Inspect", "**result**"}},
       QStringLiteral("Inspect\n\n**result**"),
       true},
      {{AuthoritativeItemKey{thread, "turn", "reasoning"}, CardKind::Reasoning,
        thread, "turn", "reasoning", ReasoningData{"Reasoning *summary*"}},
       QStringLiteral("Reasoning *summary*"),
       true},
      {{AuthoritativeItemKey{thread, "turn", "files"}, CardKind::FileChanges,
        thread, "turn", "files",
        FileChangesData{{{"src/card.cpp", "update", 2, 1}}, {}}},
       QStringLiteral("src/card.cpp  ·  Update  +2 −1"),
       false},
      {{AuthoritativeItemKey{thread, "turn", "plan"}, CardKind::Plan, thread,
        "turn", "plan",
        PlanData{"Plan explanation",
                 {{"Inspect", nodegraph::NodeStatus::Completed},
                  {"Implement", nodegraph::NodeStatus::Running}},
                 {}}},
       QStringLiteral("Plan explanation\n\n✓ Inspect  \n◉ Implement  "),
       true},
      {{AuthoritativeItemKey{thread, "turn", "image"},
        CardKind::ImageGeneration, thread, "turn", "image",
        ImageGenerationData{"/tmp/generated.png", "A revised prompt"}},
       QStringLiteral("A revised prompt\n\n/tmp/generated.png"),
       false},
      {{AuthoritativeItemKey{thread, "turn", "generic"},
        CardKind::GenericActivity, thread, "turn", "generic",
        GenericActivityData{"custom", "detail: value"}},
       QStringLiteral("detail: value"),
       false},
      {{LocalPromptKey{99},
        CardKind::LocalPrompt,
        thread,
        {},
        {},
        LocalPromptData{99,
                        "Pending `prompt`",
                        PromptState::InFlight,
                        0,
                        {},
                        {"/tmp/pending.png"}}},
       QStringLiteral("Pending `prompt`"),
       true},
  };

  bool result = true;
  for (std::size_t index = 0; index < cases.size(); ++index) {
    ConversationCard card(cases[index].card, false);
    card.show();
    spin();
    QToolButton *button = copyButton(&card);
    if (index == 0)
      card.setCollapsed(true);
    const QImage copyIcon =
        index == 0 && button ? button->grab().toImage() : QImage{};
    QApplication::clipboard()->clear();
    if (button)
      button->click();
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    result &= expect(
        button && !button->isHidden() && mime &&
            mime->text() == cases[index].expected &&
            mime->hasFormat("text/markdown") == cases[index].markdown &&
            (!cases[index].markdown ||
             mime->data("text/markdown") == cases[index].expected.toUtf8()),
        "each content card copies its canonical source while collapsed or "
        "expanded");
    if (index == 0) {
      auto *morph = button->findChild<QVariantAnimation *>();
      spin(220);
      const QImage checkIcon = button->grab().toImage();
      const QRect checkInk = paintedDisclosureBounds(button, 2);
      result &= expect(
          morph && copyIcon != checkIcon && QToolTip::isVisible() &&
              QToolTip::text() == QStringLiteral("Copied"),
          "Copy quickly morphs into a visible success check while showing "
          "the canonical transient Copied overlay");
      const QSize cardSize = card.size();
      spin(1700);
      const QRect returnedCopyInk = paintedDisclosureBounds(button, 2);
      result &= expect(button->grab().toImage() == copyIcon &&
                           card.size() == cardSize,
                       "the held check morphs back to Copy without changing "
                       "card geometry");
      result &= expect(checkInk.center().x() == returnedCopyInk.center().x(),
                       "the success check and Copy glyph share one horizontal "
                       "center");

      button->clearFocus();
      spin();
      const QImage unfocusedIcon = button->grab().toImage();
      button->setFocus(Qt::MouseFocusReason);
      spin();
      const QImage mouseFocusedIcon = button->grab().toImage();
      result &= expect(
          button->hasFocus() && mouseFocusedIcon == unfocusedIcon &&
              card.size() == cardSize,
          "mouse focus leaves Copy at its normal intensity and geometry");
      button->clearFocus();
      button->setFocus(Qt::TabFocusReason);
      spin();
      result &= expect(
          button->hasFocus() && button->grab().toImage() != unfocusedIcon &&
              card.size() == cardSize,
          "keyboard focus keeps a visible Copy affordance without moving the "
          "card");
      button->clearFocus();
    }
    if (index == 0)
      result &= expect(
          button->parentWidget()->layout()->indexOf(button) <
              button->parentWidget()->layout()->indexOf(disclosure(&card)),
          "Copy precedes the disclosure control in the card header");
    if (index == 0) {
      QToolButton *fold = disclosure(&card);
      const QRect copyInk = paintedDisclosureBounds(button).translated(
          button->mapTo(button->parentWidget(), QPoint{}));
      const QRect foldInk = paintedDisclosureBounds(fold).translated(
          fold->mapTo(fold->parentWidget(), QPoint{}));
      result &= expect(
          button->text().isEmpty() && button->height() == fold->height() &&
              std::abs(copyInk.center().y() - foldInk.center().y()) <= 1 &&
              foldInk.left() - copyInk.right() - 1 <= 14 &&
              button->parentWidget()->layout()->spacing() == 0,
          "copy and disclosure are backgroundless, vertically aligned, and "
          "use canonical compact spacing");
    }
  }

  VisibleCardData mutableMessage = cases.front().card;
  ConversationCard mutableCard(mutableMessage, false);
  std::get<UserMessageData>(mutableMessage.payload).text =
      "Updated **Markdown**";
  result &= expect(mutableCard.applyPresentation(mutableMessage) !=
                       PresentationImpact::None,
                   "copy fixture accepts an in-place content update");
  QApplication::clipboard()->clear();
  copyButton(&mutableCard)->click();
  result &= expect(
      QApplication::clipboard()->text() ==
          QStringLiteral("Updated **Markdown**"),
      "copy reads the latest retained card data after an in-place update");

  ConversationCard emptyReasoning(
      VisibleCardData{AuthoritativeItemKey{thread, "turn", "empty"},
                      CardKind::Reasoning, thread, "turn", "empty",
                      ReasoningData{}},
      true);
  emptyReasoning.show();
  spin();
  result &= expect(copyButton(&emptyReasoning) &&
                       copyButton(&emptyReasoning)->isHidden(),
                   "contentless cards omit the Copy control");
  VisibleCardData populatedReasoning = emptyReasoning.data();
  std::get<ReasoningData>(populatedReasoning.payload).summary =
      "Late **summary**";
  result &= expect(emptyReasoning.applyPresentation(populatedReasoning) !=
                           PresentationImpact::None &&
                       !copyButton(&emptyReasoning)->isHidden(),
                   "Copy appears when retained card content arrives later");
  QApplication::clipboard()->clear();
  copyButton(&emptyReasoning)->click();
  result &= expect(
      QApplication::clipboard()->text() == QStringLiteral("Late **summary**") &&
          QApplication::clipboard()->mimeData()->hasFormat("text/markdown"),
      "late Markdown content copies from the updated source");
  return result;
}

bool testUserMessageLineBreakPresentation() {
  const std::string thread = "line-breaks";
  const QString source =
      QStringLiteral("First authored line\n\nThird authored line");
  ConversationCard card(
      VisibleCardData{AuthoritativeItemKey{thread, "turn", "user"},
                      CardKind::UserMessage, thread, "turn", "user",
                      UserMessageData{source.toStdString(), {}}},
      false);
  card.resize(520, 160);
  card.show();
  spin();

  MarkdownTextView *body = card.findChild<MarkdownTextView *>();
  bool result = expect(
      body && body->markdownSource() == source &&
          body->document()->toPlainText() ==
              QStringLiteral(
                  "First authored line\n\u200B\nThird authored line") &&
          body->document()->blockCount() == 3,
      "an authoritative turn You card displays the empty row authored by two "
      "newlines");

  card.setNestedPresentation(true);
  spin();
  result &= expect(
      body && body->markdownSource() == source &&
          body->document()->blockCount() == 3,
      "an authoritative steering You card keeps the same authored blank row");

  QApplication::clipboard()->clear();
  copyButton(&card)->click();
  result &=
      expect(QApplication::clipboard()->text() == source &&
                 QApplication::clipboard()->mimeData()->data("text/markdown") ==
                     source.toUtf8(),
             "newline presentation does not alter copied prompt Markdown");

  QApplication::clipboard()->clear();
  QTextCursor selection(body->document());
  selection.setPosition(body->document()->characterCount() - 1,
                        QTextCursor::KeepAnchor);
  body->setTextCursor(selection);
  body->setFocus(Qt::OtherFocusReason);
  QKeyEvent selectionCopy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
  QApplication::sendEvent(body, &selectionCopy);
  const QMimeData *selectionMime = QApplication::clipboard()->mimeData();
  constexpr auto OdfMime = "application/vnd.oasis.opendocument.text";
  QByteArray expectedOdf;
  QBuffer expectedOdfBuffer(&expectedOdf);
  const bool expectedOdfOpened = expectedOdfBuffer.open(QIODevice::WriteOnly);
  QTextDocument expectedOdfDocument;
  // The fixture knows the sole synthetic glyph's exact position. Build the
  // expected rich selection without reading Copy's output or origin metadata.
  QTextCursor expectedSelection(&expectedOdfDocument);
  expectedSelection.insertFragment(selection.selection());
  const int emptyRow = expectedOdfDocument.findBlockByNumber(1).position();
  expectedSelection.setPosition(emptyRow);
  expectedSelection.setPosition(emptyRow + 1, QTextCursor::KeepAnchor);
  expectedSelection.removeSelectedText();
  QTextDocumentWriter expectedOdfWriter(&expectedOdfBuffer, "ODF");
  const bool expectedOdfWritten =
      expectedOdfOpened &&
      expectedOdfWriter.write(QTextDocumentFragment(&expectedOdfDocument));
  result &= expect(
      selectionMime && selectionMime->text() == source &&
          selectionMime->hasHtml() &&
          !selectionMime->html().contains(QChar(0x200B)) &&
          selectionMime->hasFormat("text/markdown") &&
          !QString::fromUtf8(selectionMime->data("text/markdown"))
               .contains(QChar(0x200B)) &&
          selectionMime->hasFormat(OdfMime) && expectedOdfWritten &&
          normalizedZipMetadata(selectionMime->data(OdfMime)) ==
              normalizedZipMetadata(expectedOdf),
      "selection MIME preserves rich text while omitting the presentation-only "
      "blank-line marker");

  const CardKey localKey = LocalPromptKey{91};
  ConversationCard pending(
      VisibleCardData{
          localKey,
          CardKind::LocalPrompt,
          thread,
          "turn",
          {},
          LocalPromptData{
              91, source.toStdString(), PromptState::InFlight, {}, {}}},
      false);
  pending.resize(520, 160);
  pending.show();
  spin();
  const VisibleCardData acknowledged{
      localKey, CardKind::UserMessage,
      thread,   "turn",
      "user",   UserMessageData{source.toStdString(), {}}};
  result &= expect(pending.applyPresentation(acknowledged) !=
                       PresentationImpact::None,
                   "a pending prompt promotes in place on acknowledgement");
  MarkdownTextView *promoted = pending.findChild<MarkdownTextView *>();
  result &=
      expect(promoted && promoted->markdownSource() == source &&
                 promoted->document()->blockCount() == 3,
             "local-to-authoritative promotion retains the authored blank row");

  const QString fenced =
      QStringLiteral("Before\nAfter\n\n```text\ninside\ncode\n```\n\nDone");
  const QString rendered = presentation::userMessageMarkdown(fenced);
  result &= expect(
      rendered == QStringLiteral(
                      "Before  \nAfter\n\n```text\ninside\ncode\n```\n\nDone"),
      "prompt newline projection preserves fenced code and paragraph breaks");
  result &= expect(
      presentation::userMessageMarkdown(source) ==
          QStringLiteral(
              "First authored line  \n\u200B  \nThird authored line"),
      "plain prompt projection represents an empty source line explicitly");
  return result;
}

bool testMarkdownSelectionPreservesAuthoredCharacters() {
  bool result = true;
  const auto copy = [](QTextBrowser &view, int start = 0, int end = -1) {
    QTextCursor selection(view.document());
    selection.setPosition(start);
    selection.setPosition(end < 0 ? view.document()->characterCount() - 1 : end,
                          QTextCursor::KeepAnchor);
    view.setTextCursor(selection);
    view.copy();
  };
  for (const QString &source :
       {QStringLiteral(
            "**left\u200Bright** [link](https://example.org/a\u200Bb)"),
        QStringLiteral("left&#8203;right"),
        QStringLiteral("&#x200b;\n\nend")}) {
    for (bool prepared : {false, true}) {
      MarkdownTextView view(prepared ? QString{} : source, 500);
      if (prepared)
        view.setPreparedContent(source,
                                presentation::prepareMarkdownHtml(source));
      copy(view);
      const auto *actual = QApplication::clipboard()->mimeData();
      const QString plain = actual->text(), html = actual->html();
      const QByteArray markdown = actual->data("text/markdown");
      const QByteArray odf =
          actual->data("application/vnd.oasis.opendocument.text");
      QTextBrowser reference;
      reference.document()->setDefaultStyleSheet(
          view.document()->defaultStyleSheet());
      reference.document()->setDefaultFont(view.document()->defaultFont());
      if (prepared)
        reference.setHtml(presentation::prepareMarkdownHtml(source));
      else
        reference.setMarkdown(source);
      copy(reference);
      const auto *expected = QApplication::clipboard()->mimeData();
      if (plain != expected->text() ||
          markdown != expected->data("text/markdown") ||
          normalizedZipMetadata(odf) !=
              normalizedZipMetadata(
                  expected->data("application/vnd.oasis.opendocument.text")))
        std::cerr << "Native Copy comparison: " << source.toStdString()
                  << ", prepared=" << prepared
                  << ", plain=" << (plain == expected->text())
                  << ", md=" << (markdown == expected->data("text/markdown"))
                  << ", odf="
                  << (normalizedZipMetadata(odf) ==
                      normalizedZipMetadata(expected->data(
                          "application/vnd.oasis.opendocument.text")))
                  << '\n';
      result &= expect(plain == expected->text() &&
                           markdown == expected->data("text/markdown") &&
                           html.contains(QChar(0x200B)) &&
                           normalizedZipMetadata(odf) ==
                               normalizedZipMetadata(expected->data(
                                   "application/vnd.oasis.opendocument.text")),
                       "ordinary/prepared selected Copy retains authored "
                       "Unicode in every format");
    }
  }
  for (const auto &[source, expected] :
       std::vector<std::pair<QString, QString>>{
           {QStringLiteral("left\u200Bright\n\nend"),
            QStringLiteral("left\u200Bright\n\nend")},
           {QStringLiteral("left&#8203;right\n\nend"),
            QStringLiteral("left\u200Bright\n\nend")},
           {QStringLiteral("left\uE000right\n\nend"),
            QStringLiteral("left\uE000right\n\nend")},
           {QStringLiteral("left&#57344;right\n\nend"),
            QStringLiteral("left\uE000right\n\nend")},
           {QStringLiteral("left\u200B\uE000\uE001right\n\nend"),
            QStringLiteral("left\u200B\uE000\uE001right\n\nend")},
           {QStringLiteral("left&#8203;&#57344;&#57345;right\n\nend"),
            QStringLiteral("left\u200B\uE000\uE001right\n\nend")},
           {QStringLiteral("**First\n\nThird**"),
            QStringLiteral("First\n\nThird")},
           {QStringLiteral("`First\n\nThird`"),
            QStringLiteral("First    Third")},
           // Preserve the existing projection's whitespace-only-line policy.
           {QStringLiteral("First\n  \nThird"),
            QStringLiteral("First\n Third")},
           {QStringLiteral("\n\nThird"), QStringLiteral("\n\nThird")},
           {QStringLiteral("First\n\n"), QStringLiteral("First\n\n")},
           {QStringLiteral("a\n\u200B\nb"), QStringLiteral("a\n\u200B\nb")},
           {QStringLiteral("a [link](https://example.org/a\u200Bb)\n\nend"),
            QStringLiteral("a link\n\nend")}}) {
    MarkdownTextView view(source, 500, nullptr, true);
    const QString liveText = view.document()->toRawText();
    QTextDocument reference;
    presentation::MarkdownTailState unused;
    presentation::replaceMarkdownDocument(
        reference, presentation::userMessageMarkdown(source), unused);
    reference.setDefaultFont(view.document()->defaultFont());
    reference.setDocumentMargin(0);
    reference.setTextWidth(500);
    const qreal height = view.document()->size().height();
    copy(view);
    const auto *mime = QApplication::clipboard()->mimeData();
    if (mime->text() != expected)
      std::cerr << "Copy mismatch for " << source.toStdString() << ": ["
                << mime->text().toStdString() << "]\n";
    result &=
        expect(mime->text() == expected && mime->hasHtml() &&
                   mime->hasFormat("text/markdown") &&
                   mime->hasFormat("application/vnd.oasis.opendocument.text"),
               "prompt selection removes generated content and preserves "
               "authored content");
    result &=
        expect(liveText == view.document()->toRawText() &&
                   liveText == reference.toRawText() &&
                   qAbs(height - reference.size().height()) < 1.0,
               "origin preparation and Copy preserve live text and geometry");
    if (source.contains(QStringLiteral("https://")))
      result &= expect(mime->html().contains(QStringLiteral("a\u200Bb")) &&
                           mime->data("text/markdown")
                               .contains(QStringLiteral("a\u200Bb").toUtf8()),
                       "selected link destinations preserve authored Unicode");
  }
  MarkdownTextView mixed(QStringLiteral("left\u200Bright\n\nend"), 500, nullptr,
                         true);
  copy(mixed, 4, 5);
  result &=
      expect(QApplication::clipboard()->text() == QString(QChar(0x200B)),
             "selecting only an authored zero-width character preserves it");
  const int blank = mixed.document()->findBlockByNumber(1).position();
  copy(mixed, blank, blank + 1);
  result &= expect(
      QApplication::clipboard()->text().isEmpty(),
      "selecting only a generated blank placeholder exports no character");
  for (const auto &[start, end] : std::vector<std::pair<int, int>>{
           {1, blank}, {4, blank + 2}, {blank, blank + 3}, {blank + 1, blank + 3}}) {
    QString expected = mixed.document()->toPlainText().mid(start, end - start);
    if (start <= blank && blank < end)
      expected.remove(blank - start, 1);
    const auto before = mixed.document()->revision();
    copy(mixed, start, end);
    result &= expect(QApplication::clipboard()->text() == expected &&
                         mixed.document()->revision() == before &&
                         mixed.textCursor().selectionStart() == start &&
                         mixed.textCursor().selectionEnd() == end,
                     "partial selections map origins without modifying the live document or selection");
  }
  for (const QString &source :
       {QStringLiteral(
            "**left\u200Bright**\n\nend [link](https://example.org/a\u200Bb)"),
        QStringLiteral("**left&#8203;right**\n\nend "
                       "[link](https://example.org/a&#8203;b)")}) {
    MarkdownTextView view(source, 500, nullptr, true);
    copy(view);
    const auto *actual = QApplication::clipboard()->mimeData();
    const QString plain = actual->text();
    const QByteArray markdown = actual->data("text/markdown");
    const QByteArray odf =
        actual->data("application/vnd.oasis.opendocument.text");
    QTextDocument html;
    html.setHtml(actual->html());
    result &= expect(
        html.toPlainText() == plain &&
            html.firstBlock().begin().fragment().charFormat().fontWeight() ==
                QFont::Bold,
        "cleaned selection HTML preserves authored Unicode and emphasis");
    QTextBrowser expected;
    expected.document()->setDefaultStyleSheet(
        view.document()->defaultStyleSheet());
    expected.document()->setDefaultFont(view.document()->defaultFont());
    expected.setMarkdown(presentation::userMessageMarkdown(source));
    QTextCursor generated(expected.document());
    const int position = expected.document()->findBlockByNumber(1).position();
    generated.setPosition(position);
    generated.setPosition(position + 1, QTextCursor::KeepAnchor);
    generated.removeSelectedText();
    copy(expected);
    const auto *reference = QApplication::clipboard()->mimeData();
    result &= expect(plain == reference->text() &&
                         markdown == reference->data("text/markdown") &&
                         normalizedZipMetadata(odf) ==
                             normalizedZipMetadata(reference->data(
                                 "application/vnd.oasis.opendocument.text")),
                     "mixed authored/generated selection exports the "
                     "independently cleaned rich fragment in all formats");
  }
  for (const auto &[source, property] : std::vector<std::pair<QString, int>>{
           {QStringLiteral(
                "a [link](https://example.org/a \"first\u200B\n\nlast\")"),
            QTextFormat::TextToolTip},
           {QStringLiteral("a ![first\u200B\n\nlast](https://example.org/a)"),
            QTextFormat::ImageAltText}}) {
    MarkdownTextView view(source, 500, nullptr, true);
    copy(view);
    const auto *actual = QApplication::clipboard()->mimeData();
    const QString html = actual->html();
    const QByteArray markdown = actual->data("text/markdown");
    const QByteArray odf =
        actual->data("application/vnd.oasis.opendocument.text");
    QTextBrowser expected;
    expected.document()->setDefaultStyleSheet(
        view.document()->defaultStyleSheet());
    expected.setMarkdown(presentation::userMessageMarkdown(source));
    // This fixture puts an authored glyph immediately after "first"; any
    // other glyph in this attribute is the single inserted empty-row marker.
    for (auto block = expected.document()->begin(); block.isValid();
         block = block.next()) {
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        auto format = fragment.charFormat();
        QString value = format.stringProperty(property);
        const int generated = value.indexOf(
            QChar(0x200B), value.startsWith(QStringLiteral("first")) ? 6 : 0);
        if (generated < 0)
          continue;
        value.remove(generated, 1);
        format.setProperty(property, value);
        QTextCursor range(expected.document());
        range.setPosition(fragment.position());
        range.setPosition(fragment.position() + fragment.length(),
                          QTextCursor::KeepAnchor);
        range.setCharFormat(format);
      }
    }
    copy(expected);
    const auto *reference = QApplication::clipboard()->mimeData();
    result &= expect(html == reference->html() &&
                         markdown == reference->data("text/markdown") &&
                         normalizedZipMetadata(odf) ==
                             normalizedZipMetadata(reference->data(
                                 "application/vnd.oasis.opendocument.text")),
                     "Copy preserves authored image/link attributes and "
                     "removes generated attribute content only");
  }
  for (const QString &initial : {QStringLiteral("First\n\nThird"),
                                 QStringLiteral("First\n\nThird\nfourth"),
                                 QStringLiteral("prefix\n\nmutable  \nline"),
                                 QStringLiteral("prefix\n\nmutable  \n"),
                                 QStringLiteral("prefix\n\nleft\u2029right"),
                                 QStringLiteral("prefix\n\nleft&#8233;right"),
                                 QStringLiteral("prefix\n\nleft&#x2029;right"),
                                 QStringLiteral("prefix\n\nleft\r\rright")}) {
    for (bool preserve : {false, true}) {
      MarkdownTextView updated(initial, 500, nullptr, preserve);
      QTextDocument *identity = updated.document();
      QString source = initial;
      for (const QString &suffix :
           {QStringLiteral(" appended"), QStringLiteral("\n\nnew **end**")}) {
        source += suffix;
        updated.setContent(source);
        MarkdownTextView fresh(source, 500, nullptr, preserve);
        if (updated.document()->toRawText() != fresh.document()->toRawText() ||
            updated.heightForWidth(500) != fresh.heightForWidth(500))
          std::cerr << "Incremental mismatch: " << source.toStdString()
                    << ", preserve=" << preserve << ", text="
                    << (updated.document()->toRawText() == fresh.document()->toRawText())
                    << ", heights=" << updated.heightForWidth(500) << '/'
                    << fresh.heightForWidth(500) << '\n';
        result &=
            expect(updated.document() == identity &&
                       updated.document()->toRawText() ==
                           fresh.document()->toRawText() &&
                       updated.heightForWidth(500) == fresh.heightForWidth(500),
                   "incremental source/document boundaries match a fresh "
                   "prompt without duplication");
        copy(updated);
        const QString updatedCopy = QApplication::clipboard()->text();
        copy(fresh);
        result &= expect(updatedCopy == QApplication::clipboard()->text() &&
                             !updated.setContent(source),
                         "incremental Copy origins agree with fresh "
                         "preparation and semantic no-ops");
      }
    }
  }
  return result;
}

bool testMutableCardsAndCommandOutput() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  DesktopUrlCapture openedFiles;
  ScopedFileUrlHandler fileUrlHandler(openedFiles);
  const std::string thread = "card-thread";
  TurnGraphSpec section{"turn:cards", "turn", {}};
  section.cards = {
      {AuthoritativeItemKey{thread, "turn", "user"}, CardKind::UserMessage,
       thread, "turn", "user",
       UserMessageData{"hello **Markdown**\n\n| Value | Rating "
                       "|\n|---|---|\n| State | 10 |\n\n"
                       "[Docs](https://example.com)"}},
      {AuthoritativeItemKey{thread, "turn", "agent"}, CardKind::AgentMessage,
       thread, "turn", "agent", AgentMessageData{"answer", false}},
      {AuthoritativeItemKey{thread, "turn", "command"},
       CardKind::CommandExecution, thread, "turn", "command",
       CommandExecutionData{
           "printf test\n\n \t", " \n\t", {}, std::nullopt, {}},
       nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{thread, "turn", "activity"},
       CardKind::AgentActivity, thread, "turn", "activity",
       AgentActivityData{"tool", {}, "prompt", {}, {}},
       nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{thread, "turn", "reasoning"}, CardKind::Reasoning,
       thread, "turn", "reasoning", ReasoningData{"summary"}},
      {AuthoritativeItemKey{thread, "turn", "files"}, CardKind::FileChanges,
       thread, "turn", "files",
       FileChangesData{{{"src/card.cpp", "update", 2, 1}}, "/workspace"},
       nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{thread, "turn", "plan"}, CardKind::Plan, thread,
       "turn", "plan",
       PlanData{"Keep the card compact",
                {{"Inspect data", nodegraph::NodeStatus::Completed},
                 {"Render cards", nodegraph::NodeStatus::Running}},
                {}},
       nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{thread, "turn", "generic"},
       CardKind::GenericActivity, thread, "turn", "generic",
       GenericActivityData{"custom activity", "detail: initial"},
       nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{thread, "turn", "image"}, CardKind::ImageGeneration,
       thread, "turn", "image", ImageGenerationData{{}, "A generated diagram"},
       nodegraph::NodeStatus::Running},
      {LocalPromptKey{77},
       CardKind::LocalPrompt,
       thread,
       {},
       {},
       LocalPromptData{77,
                       "pending\n\nAttached files:\n"
                       "- [report.pdf](file:///tmp/report.pdf)",
                       PromptState::InFlight,
                       0,
                       {}}},
  };
  section.rootCardKey = section.cards.front().key;
  ConversationGraphSpec snapshot{thread, {section}, false};
  ConversationView view;
  // This test exercises every real card editor at once. A deliberately tall
  // viewport keeps that editor count proportional to visible content while
  // the dedicated virtualization test covers bounded normal-size viewports.
  view.resize(650, 5000);
  view.show();
  applyConversation(view, snapshot);
  const bool allCoVisibleCardsReady = spinUntil([&] {
    return std::ranges::all_of(
        snapshot.sections.front().cards, [&view](const VisibleCardData &value) {
          return card(view, stableKey(value.key)) != nullptr;
        });
  });

  bool result =
      expect(allCoVisibleCardsReady,
             "bounded render continuations materialize every co-visible card");
  if (!allCoVisibleCardsReady) {
    qApp->setStyleSheet(originalStyleSheet);
    spin();
    return false;
  }

  std::unordered_map<std::string, ConversationCard *> identities;
  for (const auto &value : snapshot.sections.front().cards)
    identities[stableKey(value.key)] = card(view, stableKey(value.key));
  auto containsLabelText = [](QWidget *parent, const QString &needle) {
    const bool labelContains = std::ranges::any_of(
        parent->findChildren<QLabel *>(),
        [&needle](QLabel *label) { return label->text().contains(needle); });
    return labelContains ||
           std::ranges::any_of(parent->findChildren<MarkdownTextView *>(),
                               [&needle](MarkdownTextView *view) {
                                 return view->markdownSource().contains(needle);
                               });
  };
  auto titleText = [](QWidget *parent) {
    const auto labels = parent->findChildren<QLabel *>();
    const auto title = std::ranges::find_if(labels, [](QLabel *label) {
      return label->property("kind").toString() == QStringLiteral("title");
    });
    return title == labels.end() ? QString{} : (*title)->text();
  };
  auto *commandCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "command"}})];
  auto *output = commandCard->findChild<CommandOutputView *>(
      QStringLiteral("commandOutputView"));
  auto *commandText = dynamic_cast<ContentSizedTextView *>(
      commandCard->findChild<QTextEdit *>(QStringLiteral("commandTextView")));
  auto *commandStatus =
      commandCard->findChild<QLabel *>(QStringLiteral("commandStatus"));
  auto *commandMeta =
      commandCard->findChild<QLabel *>(QStringLiteral("commandMetadata"));
  result &= expect(
      output && output->isHidden() && commandStatus && commandMeta &&
          commandMeta->isHidden() &&
          labelUsesColor(commandStatus, UiStyle::blueText) &&
          commandStatus->font().capitalization() == QFont::MixedCase &&
          commandStatus->text() == QStringLiteral("running") &&
          commandStatus->parentWidget()->layout()->indexOf(commandStatus) <
              commandStatus->parentWidget()->layout()->indexOf(
                  copyButton(commandCard)),
      "empty-line command output has no black surface and exposes its "
      "lowercase status before Copy");
  auto *userCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "user"}})];
  auto *agentCardWidget = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "agent"}})];
  auto *agentPhase =
      agentCardWidget->findChild<QLabel *>(QStringLiteral("agentMessagePhase"));
  auto *activityCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "activity"}})];
  auto *activityStatus =
      activityCard->findChild<QLabel *>(QStringLiteral("agentActivityStatus"));
  auto *userMarkdown = userCard->findChild<MarkdownTextView *>();
  result &=
      expect(userMarkdown &&
                 userMarkdown->markdownSource() ==
                     QStringLiteral("hello **Markdown**\n\n| Value | Rating |\n"
                                    "|---|---|\n| State | 10 |\n\n"
                                    "[Docs](https://example.com)") &&
                 userMarkdown->toHtml().contains(QStringLiteral("<table")) &&
                 userMarkdown->textInteractionFlags().testFlag(
                     Qt::LinksAccessibleByKeyboard),
             "authoritative user messages render GitHub Markdown tables");
  result &=
      expect(titleText(agentCardWidget) == QStringLiteral("Codex") &&
                 agentPhase && agentPhase->text() == QStringLiteral("update") &&
                 labelUsesColor(agentPhase, UiStyle::blueText) &&
                 agentPhase->font().weight() == QFont::Normal &&
                 usesReferencePhaseCopySpacing(agentCardWidget, agentPhase) &&
                 agentPhase->parentWidget()->layout()->indexOf(agentPhase) <
                     agentPhase->parentWidget()->layout()->indexOf(
                         copyButton(agentCardWidget)),
             "interim agent messages show a right-aligned normal-weight update "
             "phase before Copy");
  result &= expect(
      activityStatus &&
          activityStatus->font().capitalization() == QFont::MixedCase &&
          activityStatus->text() == QStringLiteral("running") &&
          labelUsesColor(activityStatus, UiStyle::blueText),
      "agent activity exposes its canonical lowercase status in the header");
  auto *filesCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "files"}})];
  filesCard->setCollapsed(false);
  spin();
  auto *filesStatus =
      filesCard->findChild<QLabel *>(QStringLiteral("fileChangesStatus"));
  auto *filesList =
      filesCard->findChild<QTextBrowser *>(QStringLiteral("fileChangesList"));
  auto *planCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "plan"}})];
  auto *genericCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "generic"}})];
  auto *imageCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "image"}})];
  const std::array<std::pair<ConversationCard *, QString>, 6> statusCards{{
      {agentCardWidget, QStringLiteral("agentMessagePhase")},
      {commandCard, QStringLiteral("commandStatus")},
      {activityCard, QStringLiteral("agentActivityStatus")},
      {filesCard, QStringLiteral("fileChangesStatus")},
      {imageCard, QStringLiteral("imageGenerationStatus")},
      {genericCard, QStringLiteral("genericActivityStatus")},
  }};
  result &= expect(
      std::ranges::all_of(
          statusCards,
          [](const std::pair<ConversationCard *, QString> &entry) {
            QLabel *status =
                entry.first ? entry.first->findChild<QLabel *>(entry.second)
                            : nullptr;
            return status && usesReferencePhaseCopySpacing(entry.first, status);
          }),
      "every rich-card lifecycle label uses the Codex Update status-to-Copy "
      "geometry");
  result &= expect(
      filesList &&
          filesList->toPlainText().contains(QStringLiteral("src/card.cpp")) &&
          filesList->toPlainText().contains(QStringLiteral("+2 −1")) &&
          [&] {
            QTextCursor cursor(filesList->document());
            cursor.setPosition(1);
            return cursor.charFormat().anchorHref() ==
                       QStringLiteral("codexui-file:0") &&
                   cursor.charFormat().foreground().color() ==
                       QColor(QString::fromLatin1(UiStyle::blue));
          }() &&
          filesStatus &&
          filesStatus->font().capitalization() == QFont::MixedCase &&
          filesStatus->text() == QStringLiteral("running") &&
          labelUsesColor(filesStatus, UiStyle::blueText),
      "file-change cards keep counts below and expose status in the "
      "header");
  const qulonglong fileBodyRebuilds =
      filesCard->property("fileChangesBodyRebuilds").toULongLong();
  if (filesList) {
    QTextCursor retainedFileSelection(filesList->document());
    retainedFileSelection.setPosition(0);
    retainedFileSelection.setPosition(QStringLiteral("src/card.cpp").size(),
                                      QTextCursor::KeepAnchor);
    filesList->setTextCursor(retainedFileSelection);
  }
  VisibleCardData fileLifecycle = filesCard->data();
  fileLifecycle.status = nodegraph::NodeStatus::Completed;
  result &= expect(
      filesCard->applyPresentation(fileLifecycle) ==
              PresentationImpact::PaintOnly &&
          filesCard->property("fileChangesBodyRebuilds").toULongLong() ==
              fileBodyRebuilds &&
          filesStatus->text() == QStringLiteral("completed") && filesList &&
          filesList->textCursor().selectedText() ==
              QStringLiteral("src/card.cpp"),
      "a file-change lifecycle update does not rebuild, reparse, or remeasure "
      "the unchanged path list");
  snapshot.sections.front().cards[5] = fileLifecycle;
  if (filesList) {
    static_cast<void>(QMetaObject::invokeMethod(
        filesList, "anchorClicked", Qt::DirectConnection,
        Q_ARG(QUrl, QUrl(QStringLiteral("codexui-file:0")))));
  }
  result &= expect(
      openedFiles.urls.size() == 1 && openedFiles.urls.back().isLocalFile() &&
          openedFiles.urls.back().toLocalFile() ==
              QStringLiteral("/workspace/src/card.cpp"),
      "a relative changed-file link opens from the canonical thread workspace");
  planCard->setCollapsed(false);
  commandCard->setCollapsed(false);
  spin();
  result &= expect(
      containsLabelText(planCard, QStringLiteral("Keep the card compact")) &&
          containsLabelText(planCard, QStringLiteral("✓ Inspect data")) &&
          containsLabelText(planCard, QStringLiteral("◉ Render cards")),
      "structured plan cards show explanation and step status");
  result &= expect(
      commandText &&
          commandText->toPlainText() == QStringLiteral("printf test") &&
          commandText->height() < commandText->maximumHeight() &&
          commandText->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
      "short command text trims empty lines and uses its content height");
  auto *pendingCard = identities[stableKey(CardKey{LocalPromptKey{77}})];
  auto *pendingMarkdown = pendingCard->findChild<MarkdownTextView *>();
  result &= expect(
      pendingMarkdown &&
          pendingMarkdown->markdownSource().contains(
              QStringLiteral("[report.pdf](file:///tmp/report.pdf)")),
      "pending prompts render file links before authoritative replacement");

  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      commandCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  auto &cards = snapshot.sections.front().cards;
  std::get<UserMessageData>(cards[0].payload).text += " updated";
  auto &agent = std::get<AgentMessageData>(cards[1].payload);
  agent.text += " updated";
  agent.finalAnswer = true;
  auto &command = std::get<CommandExecutionData>(cards[2].payload);
  QString longCommand;
  for (int line = 0; line < 30; ++line)
    longCommand += QStringLiteral("command argument line %1\n").arg(line);
  command.command = utf8(longCommand);
  command.output =
      utf8(QString(120, QLatin1Char('x')) + QStringLiteral("\nvisible\n\n \t"));
  cards[2].status = nodegraph::NodeStatus::Completed;
  command.exitCode = 0;
  command.cwd = "/workspace";
  command.durationMilliseconds = 1500;
  std::get<AgentActivityData>(cards[3].payload).resultText = "result";
  std::get<ReasoningData>(cards[4].payload).summary += " more";
  std::get<FileChangesData>(cards[5].payload)
      .changes.push_back({"tests/card.cpp", "add", 3, 0});
  std::get<PlanData>(cards[6].payload).steps[1].status =
      nodegraph::NodeStatus::Completed;
  auto &generic = std::get<GenericActivityData>(cards[7].payload);
  generic.type = "updated custom activity";
  generic.displayDetail = "detail: updated";
  std::get<LocalPromptData>(cards[9].payload).state = PromptState::Failed;
  std::get<LocalPromptData>(cards[9].payload).error = "error";
  result &= expect(applyConversation(view, snapshot),
                   "all card types accept visible updates");
  result &= spinUntil([&] {
    const auto *presented =
        std::get_if<CommandExecutionData>(&commandCard->data().payload);
    return presented && presented->output.ends_with("visible\n\n \t");
  });
  const int immediateCommandHeight = commandCard->height();
  const int immediatePreferredOutputHeight = output->sizeHint().height();
  spin();
  result &=
      expect(commandCard->height() == immediateCommandHeight &&
                 output->sizeHint().height() == immediatePreferredOutputHeight,
             "command output has no delayed card geometry settlement while "
             "other graph renders remain sliced");
  const bool longCommandStartsAtTop =
      commandText->verticalScrollBar()->maximum() > 0 &&
      commandText->verticalScrollBar()->value() ==
          commandText->verticalScrollBar()->minimum();
  if (!longCommandStartsAtTop)
    std::cerr << "long command scroll: value="
              << commandText->verticalScrollBar()->value()
              << " minimum=" << commandText->verticalScrollBar()->minimum()
              << " maximum=" << commandText->verticalScrollBar()->maximum()
              << " height=" << commandText->height()
              << " hint=" << commandText->sizeHint().height()
              << " cursor=" << commandText->textCursor().position()
              << " focus=" << commandText->hasFocus() << '\n';
  result &= expect(longCommandStartsAtTop,
                   "long executed-command text opens at its beginning");
  for (const auto &value : cards)
    result &= expect(card(view, stableKey(value.key)) ==
                         identities[stableKey(value.key)],
                     "same-key same-kind card updates in place");
  // The graph update above deliberately does no QWidget projection for this
  // offscreen card. Bringing the already-materialized card into view applies
  // its latest canonical state without reconstructing it.
  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      agentCardWidget->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  result &= expect(
      titleText(agentCardWidget) == QStringLiteral("Codex") && agentPhase &&
          agentPhase->text() == QStringLiteral("final answer") &&
          labelUsesColor(agentPhase, UiStyle::greenText) &&
          agentPhase->font().weight() == QFont::Normal,
      "final agent messages show a normal-weight success answer phase");
  result &= expect(
      commandStatus->text() == QStringLiteral("completed") &&
          labelUsesColor(commandStatus, UiStyle::greenText) &&
          commandMeta->text() ==
              QStringLiteral("exit 0  |  /workspace  |  1.5 s") &&
          !commandMeta->text().contains(QStringLiteral("completed")),
      "command completion moves only lifecycle status while retaining exit, "
      "cwd, and duration below output");
  result &= expect(
      !output->isHidden() && output->minimumHeight() == 0 &&
          output->maximumHeight() <= 220 &&
          (output->maximumHeight() - 8) % output->fontMetrics().lineSpacing() ==
              0 &&
          output->toPlainText().endsWith(QStringLiteral("visible")) &&
          output->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
      "visible output trims empty lines and grows by whole rows within "
      "the 220px cap");

  QString longOutput;
  for (int line = 0; line < 80; ++line)
    longOutput += QStringLiteral("line %1 with terminal output\n").arg(line);
  output->setOutput(longOutput);
  view.resize(650, 520);
  spin(40);
  result &= expect(output->verticalScrollBar()->maximum() > 0 &&
                       output->followsLatest(),
                   "long command output exposes its own scrollbar and follows");

  // A scrollbar move immediately after an output update is user-owned.  It
  // must not be overwritten by a deferred follow-latest settlement.
  output->setOutput(longOutput + QStringLiteral("new output before gesture\n"));
  output->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  spin();
  const int immediateGestureValue = output->verticalScrollBar()->value();
  result &=
      expect(!output->followsLatest() &&
                 output->verticalScrollBar()->value() == immediateGestureValue,
             "an immediate inner-scroll gesture supersedes following");

  output->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  spin();
  const int preserved = output->verticalScrollBar()->value();
  output->setOutput(longOutput + QStringLiteral("one more line\n"));
  spin();
  result &= expect(!output->followsLatest() &&
                       output->verticalScrollBar()->value() == preserved,
                   "paused command output preserves its inner scroll value");
  output->verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  spin();
  result &= expect(output->followsLatest(),
                   "inner output following resumes at its real bottom");

  output->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  QTextCursor removedOutputSelection(output->document());
  removedOutputSelection.setPosition(0);
  removedOutputSelection.setPosition(12, QTextCursor::KeepAnchor);
  output->setTextCursor(removedOutputSelection);
  result &=
      expect(!output->followsLatest() && output->textCursor().hasSelection(),
             "command output has detached interaction state before its "
             "semantic removal");

  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      commandCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  command.output = "\x1b]0;terminal title\x07\x1b[0m \n\t";
  result &= expect(applyConversation(view, snapshot),
                   "non-presentable replacement updates the command card");
  result &= spinUntil([&] { return output->isHidden(); });
  const int hiddenOuterRange = view.verticalScrollBar()->maximum();
  const int hiddenCommandHeight = commandCard->height();
  result &= expect(output->isHidden(),
                   "non-presentable replacement removes the black surface");
  spin();
  result &=
      expect(output->isHidden() &&
                 view.verticalScrollBar()->maximum() == hiddenOuterRange &&
                 commandCard->height() == hiddenCommandHeight,
             "hidden command output causes no delayed outer reflow");

  command.output = utf8(longOutput + QStringLiteral("fresh output\n"));
  result &= expect(applyConversation(view, snapshot),
                   "visible output can reappear on the same command card");
  result &= spinUntil([&] { return !output->isHidden(); });
  result &=
      expect(output->followsLatest() &&
                 output->verticalScrollBar()->value() ==
                     output->verticalScrollBar()->maximum() &&
                 !output->textCursor().hasSelection() &&
                 output->toPlainText().endsWith(QStringLiteral("fresh output")),
             "reappearing output starts at latest without resurrecting "
             "removed scroll or selection state");
  qApp->setStyleSheet(originalStyleSheet);
  spin();
  return result;
}

bool testCardFoldingGeometryAndRetention() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  const std::string thread = "folding-thread";
  const VisibleCardData user{
      AuthoritativeItemKey{thread, "turn", "user"},
      CardKind::UserMessage,
      thread,
      "turn",
      "user",
      UserMessageData{"Keep this message initially expanded.", {}}};
  const VisibleCardData agent{
      AuthoritativeItemKey{thread, "turn", "agent"},
      CardKind::AgentMessage,
      thread,
      "turn",
      "agent",
      AgentMessageData{"Codex also starts expanded.", true}};
  const VisibleCardData reasoning{
      AuthoritativeItemKey{thread, "turn", "reasoning"},
      CardKind::Reasoning,
      thread,
      "turn",
      "reasoning",
      ReasoningData{"A retained public summary with enough "
                    "detail to create real height.\n\n"
                    "The second paragraph proves expansion uses "
                    "the final wrapped size."}};
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{
          "produce output", "initial output", "/workspace", 0, {}},
      nodegraph::NodeStatus::Completed};
  const VisibleCardData files{
      AuthoritativeItemKey{thread, "turn", "files"},
      CardKind::FileChanges,
      thread,
      "turn",
      "files",
      FileChangesData{{{"src/card.cpp", "update", 4, 1}}, {}},
      nodegraph::NodeStatus::Completed};
  const VisibleCardData activity{
      AuthoritativeItemKey{thread, "turn", "activity"},
      CardKind::AgentActivity,
      thread,
      "turn",
      "activity",
      AgentActivityData{
          "spawn_agent", {}, "Inspect folding", "Inspection complete", {}},
      nodegraph::NodeStatus::Completed};
  const VisibleCardData image{
      AuthoritativeItemKey{thread, "turn", "image"},
      CardKind::ImageGeneration,
      thread,
      "turn",
      "image",
      ImageGenerationData{"/tmp/folding-preview.png", "A folding preview"},
      nodegraph::NodeStatus::Completed};
  const VisibleCardData plan{
      AuthoritativeItemKey{thread, "turn", "plan"},
      CardKind::Plan,
      thread,
      "turn",
      "plan",
      PlanData{"Verify folding",
               {{"Inspect geometry", nodegraph::NodeStatus::Completed}},
               {}},
      nodegraph::NodeStatus::Completed};
  const VisibleCardData generic{
      AuthoritativeItemKey{thread, "turn", "generic"},
      CardKind::GenericActivity,
      thread,
      "turn",
      "generic",
      GenericActivityData{"Unknown activity", "detail: bounded"}};
  const VisibleCardData emptyReasoning{
      AuthoritativeItemKey{thread, "turn", "empty-reasoning"},
      CardKind::Reasoning,
      thread,
      "turn",
      "empty-reasoning",
      ReasoningData{}};
  ConversationGraphSpec snapshot{
      thread,
      {{"turn:folding",
        "turn",
        {user, agent, reasoning, command, files, activity, image, plan, generic,
         emptyReasoning},
        user.key}},
      false};
  snapshot.activeTurnId = "turn";

  ConversationView view;
  // Folding behavior is tested with all rich editors genuinely visible.
  view.resize(700, 5000);
  view.show();
  ConversationGraphSpec promptOnly = snapshot;
  promptOnly.sections.front().cards = {user};
  bool result = expect(applyConversation(view, promptOnly),
                       "prompt-only folding fixture renders");
  spin();

  ConversationCard *promptOnlyCard = card(view, stableKey(user.key));
  QWidget *promptOnlyNestedCards =
      promptOnlyCard ? promptOnlyCard->findChild<QWidget *>(
                           QStringLiteral("conversationNestedCards"),
                           Qt::FindDirectChildrenOnly)
                     : nullptr;
  result &= expect(promptOnlyCard && !promptOnlyNestedCards,
                   "a virtualized turn prompt owns no nested-card container");
  result &= expect(applyConversation(view, snapshot),
                   "first nested activity extends the folding fixture");
  const bool foldingCardsReady = spinUntil([&] {
    return std::ranges::all_of(
        snapshot.sections.front().cards, [&view](const VisibleCardData &value) {
          return card(view, stableKey(value.key)) != nullptr;
        });
  });
  result &= expect(
      foldingCardsReady,
      "bounded render continuations materialize every co-visible folding "
      "card");
  if (!foldingCardsReady) {
    qApp->setStyleSheet(originalStyleSheet);
    spin();
    return false;
  }

  QPointer<ConversationCard> userCard = card(view, stableKey(user.key));
  QPointer<ConversationCard> agentCardWidget = card(view, stableKey(agent.key));
  QPointer<ConversationCard> reasoningCard =
      card(view, stableKey(reasoning.key));
  QPointer<ConversationCard> commandCard = card(view, stableKey(command.key));
  QPointer<ConversationCard> filesCard = card(view, stableKey(files.key));
  const std::vector<QPointer<ConversationCard>> additionalActionCards{
      card(view, stableKey(activity.key)), card(view, stableKey(image.key)),
      card(view, stableKey(plan.key)), card(view, stableKey(generic.key))};
  QPointer<ConversationCard> emptyReasoningCard =
      card(view, stableKey(emptyReasoning.key));
  result &= expect(
      userCard && agentCardWidget && reasoningCard && commandCard &&
          filesCard && !userCard->isCollapsed() &&
          !agentCardWidget->isCollapsed() && reasoningCard->isCollapsed() &&
          commandCard->isCollapsed() && filesCard->isCollapsed() &&
          disclosure(userCard) && disclosure(agentCardWidget) &&
          disclosure(reasoningCard) && disclosure(commandCard) &&
          disclosure(filesCard) &&
          disclosure(userCard)->accessibleName() ==
              QStringLiteral("Collapse card") &&
          disclosure(reasoningCard)->accessibleName() ==
              QStringLiteral("Expand card"),
      "all cards share disclosure controls with role-correct initial state");
  result &= expect(
      userCard && userCard == promptOnlyCard &&
          view.conversationModel()
              ->indexForStableKey(stableKey(user.key))
              .data(ConversationItemModel::ActiveTurnRole)
              .toBool() &&
          userCard->property("virtualTurnRoot").toBool() && agentCardWidget &&
          !agentCardWidget->property("authoritativeTurnActive").toBool() &&
          !userCard->findChild<QTimer *>(QStringLiteral("activeTurnAnimation")),
      "the virtual running Turn/You surface owns the static emphasized border");
  snapshot.activeTurnId.reset();
  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      userCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  result &=
      expect(applyConversation(view, snapshot) && spinUntil([&] {
               return !userCard->property("authoritativeTurnActive").toBool();
             }) &&
                 userCard == card(view, stableKey(user.key)) &&
                 !userCard->property("authoritativeTurnActive").toBool(),
             "turn completion restores the same card's canonical border");
  const QRect collapsedDisclosure =
      paintedDisclosureBounds(disclosure(reasoningCard));
  result &= expect(
      collapsedDisclosure.isValid() && collapsedDisclosure.width() <= 8 &&
          collapsedDisclosure.right() >= disclosure(reasoningCard)->width() - 3,
      "collapsed disclosure paints only a right-inset left chevron");
  result &= expect(
      std::ranges::all_of(additionalActionCards,
                          [](const QPointer<ConversationCard> &value) {
                            return value && value->isCollapsed() &&
                                   disclosure(value);
                          }),
      "agent, image, plan, and fallback activity cards also start collapsed");
  result &= expect(emptyReasoningCard && emptyReasoningCard->isCollapsed() &&
                       disclosure(emptyReasoningCard) &&
                       disclosure(emptyReasoningCard)->isHidden(),
                   "title-only reasoning omits a meaningless disclosure");
  if (!userCard || !agentCardWidget || !reasoningCard || !commandCard ||
      !filesCard || !emptyReasoningCard)
    return false;

  result &=
      expect(view.conversationModel()
                     ->indexForStableKey(stableKey(user.key))
                     .data(ConversationItemModel::TurnRootRole)
                     .toBool() &&
                 view.conversationModel()
                     ->indexForStableKey(stableKey(agent.key))
                     .data(ConversationItemModel::NestedCardRole)
                     .toBool() &&
                 agentCardWidget->property("nestedConversationCard").toBool(),
             "the first You row structurally owns its flat virtual turn "
             "activity");
  QWidget *promptContent = userCard->findChild<QWidget *>(
      QStringLiteral("conversationCardContent"), Qt::FindDirectChildrenOnly);
  const QModelIndex promptIndex =
      view.conversationModel()->indexForStableKey(stableKey(user.key));
  const QModelIndex firstNestedIndex =
      view.conversationModel()->indexForStableKey(stableKey(agent.key));
  result &= expect(
      promptContent && promptIndex.isValid() && firstNestedIndex.isValid() &&
          view.visualRect(firstNestedIndex).top() -
                  view.visualRect(promptIndex).bottom() - 1 ==
              14,
      "turn prompt content adds a visible canonical 8 px section boundary "
      "before its first nested card");

  const LocalPromptKey steeringKey{4343};
  VisibleCardData steering{
      steeringKey,
      CardKind::LocalPrompt,
      thread,
      "turn",
      {},
      LocalPromptData{4343,
                      "A steering prompt",
                      PromptState::InFlight,
                      false,
                      {},
                      {},
                      QDateTime::currentMSecsSinceEpoch() - 1500,
                      false}};
  snapshot.sections.front().cards.push_back(steering);
  result &= expect(applyConversation(view, snapshot),
                   "a steering prompt joins the active turn");
  spin(40);
  ConversationCard *steeringCard = card(view, stableKey(steeringKey));
  auto *steeringPhase = steeringCard
                            ? steeringCard->findChild<QLabel *>(
                                  QStringLiteral("steeringMessagePhase"))
                            : nullptr;
  auto *steeringAnimation = steeringCard
                                ? steeringCard->findChild<QTimer *>(
                                      QString{}, Qt::FindDirectChildrenOnly)
                                : nullptr;
  result &= expect(
      steeringCard &&
          steeringCard->property("nestedConversationCard").toBool() &&
          cardTitle(steeringCard) == QStringLiteral("You") && steeringPhase &&
          steeringPhase->text() == QStringLiteral("steering · pending") &&
          steeringPhase->font().weight() == QFont::Normal &&
          usesReferencePhaseCopySpacing(steeringCard, steeringPhase) &&
          steeringPhase->parentWidget()->layout()->indexOf(steeringPhase) <
              steeringPhase->parentWidget()->layout()->indexOf(
                  copyButton(steeringCard)) &&
          cardTitleColor(steeringCard) ==
              QColor(QString::fromLatin1(codexui::UiStyle::tealText)) &&
          steeringAnimation && steeringAnimation->isActive(),
      "a pending steering You card is nested and keeps its animation");

  snapshot.sections.front().cards.back() = {
      steeringKey,     CardKind::UserMessage,
      thread,          "turn",
      "steering-user", UserMessageData{"A steering prompt", {}}};
  result &= expect(applyConversation(view, snapshot),
                   "the steering prompt receives authoritative content");
  spin();
  ConversationCard *authoritativeSteering = card(view, stableKey(steeringKey));
  result &= expect(
      authoritativeSteering == steeringCard &&
          authoritativeSteering->property("nestedConversationCard").toBool() &&
          authoritativeSteering->data().kind == CardKind::UserMessage &&
          cardTitle(authoritativeSteering) == QStringLiteral("You") &&
          steeringPhase->text() == QStringLiteral("steering") &&
          authoritativeSteering->palette().color(QPalette::Window) ==
              QColor(QString::fromLatin1(codexui::UiStyle::tealSurface)) &&
          steeringAnimation && !steeringAnimation->isActive(),
      "steering acknowledgement morphs the same nested card");

  auto retainedEmptyReasoning = std::ranges::find_if(
      snapshot.sections.front().cards, [&emptyReasoning](const auto &value) {
        return value.key == emptyReasoning.key;
      });
  std::get<ReasoningData>(retainedEmptyReasoning->payload).summary =
      "Public reasoning summary arrived";
  view.verticalScrollBar()->setValue(
      view.verticalScrollBar()->value() +
      emptyReasoningCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  result &= expect(applyConversation(view, snapshot),
                   "empty reasoning accepts later public content");
  result &=
      spinUntil([&] { return !disclosure(emptyReasoningCard)->isHidden(); });
  result &=
      expect(!disclosure(emptyReasoningCard)->isHidden() &&
                 disclosure(emptyReasoningCard)->accessibleName() ==
                     QStringLiteral("Expand card"),
             "reasoning disclosure appears collapsed when detail arrives");

  wheel(view, 10000);

  const int userTop = userCard->mapTo(view.viewport(), QPoint{}).y();
  const int reasoningTop = reasoningCard->mapTo(view.viewport(), QPoint{}).y();
  const int filesTop = filesCard->mapTo(view.viewport(), QPoint{}).y();
  const int foldedReasoningHeight = reasoningCard->height();
  result &= expect(setFolded(reasoningCard, false),
                   "reasoning expands through its disclosure control");
  const int expandedReasoningHeight = reasoningCard->height();
  result &= expect(
      reasoningCard->mapTo(view.viewport(), QPoint{}).y() == reasoningTop &&
          userCard->mapTo(view.viewport(), QPoint{}).y() == userTop &&
          expandedReasoningHeight > foldedReasoningHeight &&
          disclosure(reasoningCard)->accessibleName() ==
              QStringLiteral("Collapse card") &&
          filesCard->mapTo(view.viewport(), QPoint{}).y() ==
              filesTop + expandedReasoningHeight - foldedReasoningHeight,
      "expansion fixes the affected title and grows only downward");
  const QRect expandedDisclosure =
      paintedDisclosureBounds(disclosure(reasoningCard));
  result &= expect(
      expandedDisclosure.isValid() && expandedDisclosure.width() <= 10 &&
          expandedDisclosure.right() >= disclosure(reasoningCard)->width() - 3,
      "expanded disclosure paints only a right-inset down chevron");

  const int commandHeight = commandCard->height();
  auto &execution = std::get<CommandExecutionData>(
      snapshot.sections.front().cards[3].payload);
  execution.output =
      "streamed line 1\nstreamed line 2\nstreamed line 3\nstreamed line 4";
  const int scrollBeforeCommandUpdate = view.verticalScrollBar()->value();
  view.verticalScrollBar()->setValue(
      scrollBeforeCommandUpdate +
      commandCard->mapTo(view.viewport(), QPoint{}).y() - 8);
  spin(40);
  result &= expect(applyConversation(view, snapshot),
                   "folded command accepts a streamed content update");
  auto *output = commandCard->findChild<CommandOutputView *>(
      QStringLiteral("commandOutputView"));
  result &= expect(
      output &&
          !output->toPlainText().contains(QStringLiteral("streamed line 4")) &&
          commandCard->property("conversationBodyProjectionDeferred").toBool(),
      "streaming into a folded command defers its hidden document work");
  view.verticalScrollBar()->setValue(scrollBeforeCommandUpdate);
  spin(20);
  result &= expect(commandCard->isCollapsed() &&
                       commandCard->height() == commandHeight && output,
                   "streaming keeps folded command geometry unchanged");
  result &= expect(setFolded(commandCard, false),
                   "the updated folded command expands on demand");
  result &= spinUntil([&] {
    return output &&
           output->toPlainText().contains(QStringLiteral("streamed line 4"));
  });
  result &= expect(
      output &&
          output->toPlainText().contains(QStringLiteral("streamed line 4")) &&
          !commandCard->property("conversationBodyProjectionDeferred").toBool(),
      "command expansion projects the latest deferred output exactly once");
  result &= expect(setFolded(commandCard, true),
                   "the command returns to its retained folded state");

  const int userHeight = userCard->height();
  result &= expect(setFolded(userCard, true),
                   "You can be folded from its expanded default");
  const bool foldedTurnPass =
      userCard->mapTo(view.viewport(), QPoint{}).y() == userTop &&
      userCard->height() < userHeight &&
      (!agentCardWidget || !agentCardWidget->isVisibleTo(userCard)) &&
      (!reasoningCard || !reasoningCard->isVisibleTo(userCard)) &&
      (!commandCard || !commandCard->isVisibleTo(userCard));
  result &= expect(
      foldedTurnPass,
      "folding You fixes its title and hides or releases nested widgets");

  applyConversation(view, conversation("folding-other-thread", 4));
  spin();
  applyConversation(view, snapshot);
  spin(40);
  userCard = card(view, stableKey(user.key));
  reasoningCard = card(view, stableKey(reasoning.key));
  commandCard = card(view, stableKey(command.key));
  result &= expect(userCard && userCard->isCollapsed(),
                   "the root fold survives thread switching and updates");
  result &= expect(setFolded(userCard, false),
                   "the restored root can rematerialize its nested cards");
  spin(80);
  reasoningCard = card(view, stableKey(reasoning.key));
  commandCard = card(view, stableKey(command.key));
  result &=
      expect(reasoningCard && commandCard && !reasoningCard->isCollapsed() &&
                 commandCard->isCollapsed(),
             "rematerialized nested cards restore user-owned folds");

  const std::string promptThread = "folding-prompt-replacement";
  const LocalPromptKey promptKey{4242};
  VisibleCardData localPrompt{
      promptKey,
      CardKind::LocalPrompt,
      promptThread,
      {},
      {},
      LocalPromptData{
          4242, "A temporary prompt", PromptState::InFlight, 0, {}, {}}};
  VisibleCardData promptActivity = agentCard(promptThread, "turn", 77);
  ConversationGraphSpec promptSnapshot{
      promptThread,
      {{"local:folding-prompt", {}, {localPrompt, promptActivity}, promptKey}},
      false};
  applyConversation(view, promptSnapshot);
  spin();
  ConversationCard *promptCard = card(view, stableKey(promptKey));
  QPointer<ConversationCard> promptActivityCard =
      card(view, stableKey(promptActivity.key));
  result &= expect(
      promptCard && !promptCard->isCollapsed() && promptActivityCard &&
          promptActivityCard->property("nestedConversationCard").toBool() &&
          setFolded(promptCard, true),
      "temporary You prompts start expanded and can be folded");
  ConversationCard *const admittedPromptCard = promptCard;
  QWidget *const admittedPromptHeader =
      admittedPromptCard ? admittedPromptCard->findChild<QWidget *>(
                               QStringLiteral("conversationCardHeader"))
                         : nullptr;
  const QSize admittedPromptSize =
      admittedPromptCard ? admittedPromptCard->size() : QSize{};
  const QRect admittedPromptHeaderGeometry =
      admittedPromptHeader ? admittedPromptHeader->geometry() : QRect{};
  const int admittedPromptFrameWidth =
      admittedPromptCard ? admittedPromptCard->frameWidth() : -1;
  promptSnapshot.sections.front().cards.front() = {
      promptKey,    CardKind::UserMessage,
      promptThread, "turn",
      "user",       UserMessageData{"A temporary prompt", {}}};
  promptSnapshot.sections.front().key = "turn:folding-prompt";
  promptSnapshot.sections.front().turnId = "turn";
  applyConversation(view, promptSnapshot);
  spin();
  promptCard = card(view, stableKey(promptKey));
  auto *promptAnimation = admittedPromptCard->findChild<QTimer *>(
      QString{}, Qt::FindDirectChildrenOnly);
  const bool promptMorphPass =
      promptCard && promptCard == admittedPromptCard &&
      promptCard->data().kind == CardKind::UserMessage &&
      promptCard->isCollapsed() && promptAnimation &&
      !promptAnimation->isActive() &&
      promptCard->property("messageRole") == QStringLiteral("user") &&
      promptCard->property("conversationCardKind").toInt() ==
          static_cast<int>(CardKind::UserMessage) &&
      promptCard->objectName() == QStringLiteral("conversationCard") &&
      promptCard->styleSheet().isEmpty() &&
      promptCard->size() == admittedPromptSize && admittedPromptHeader &&
      admittedPromptHeader->geometry() == admittedPromptHeaderGeometry &&
      promptCard->frameWidth() == admittedPromptFrameWidth;
  result &= expect(
      promptMorphPass,
      "acknowledgement morphs the retained You card without geometry drift");
  result &= expect(setFolded(promptCard, false),
                   "acknowledged prompt can reveal current turn activity");
  spin(40);
  ConversationCard *rematerializedPromptActivity =
      card(view, stableKey(promptActivity.key));
  result &= expect(
      rematerializedPromptActivity &&
          rematerializedPromptActivity->property("nestedConversationCard")
              .toBool() &&
          (!promptActivityCard ||
           rematerializedPromptActivity == promptActivityCard),
      "prompt activity remains structurally nested across lazy release");

  const std::string edgeThread = "folding-bottom-edge";
  ConversationGraphSpec edge = conversation(edgeThread, 12);
  QString longOutput;
  for (int line = 0; line < 70; ++line)
    longOutput += QStringLiteral("bottom-edge line %1\n").arg(line);
  VisibleCardData edgeCommand{
      AuthoritativeItemKey{edgeThread, "turn-2", "edge-command"},
      CardKind::CommandExecution,
      edgeThread,
      "turn-2",
      "edge-command",
      CommandExecutionData{
          "produce capped output", utf8(longOutput), {}, 0, {}},
      nodegraph::NodeStatus::Completed};
  edge.sections.back().cards.push_back(edgeCommand);
  ConversationView edgeView;
  edgeView.resize(650, 520);
  edgeView.show();
  applyConversation(edgeView, edge);
  spin();
  ConversationCard *edgeCard = card(edgeView, stableKey(edgeCommand.key));
  const int collapsedTop =
      edgeCard ? edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() : 0;
  result &= expect(setFolded(edgeCard, false),
                   "bottom-edge command expands from its compact default");
  result &= expect(
      edgeCard &&
          edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() < collapsedTop &&
          edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() +
                  edgeCard->height() <=
              edgeView.viewport()->height(),
      "bottom-edge expansion shifts upward to reveal the complete card");
  wheel(edgeView, -10000);
  spin(120);
  const int followedTitleTop =
      edgeCard ? edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() : 0;
  const int expandedScrollMaximum = edgeView.verticalScrollBar()->maximum();
  result &= expect(edgeView.isAtBottom() && followedTitleTop >= 0,
                   "expanded lower-limit fixture exposes its title at bottom");
  result &= expect(setFolded(edgeCard, true),
                   "expanded bottom-edge command collapses");
  spin(120);
  result &= expect(
      edgeCard &&
          edgeCard->mapTo(edgeView.viewport(), QPoint{}).y() >
              followedTitleTop &&
          edgeView.verticalScrollBar()->maximum() < expandedScrollMaximum &&
          edgeView.isAtBottom() &&
          edgeView.mode() == ConversationView::Mode::Paused,
      "bottom-edge collapse accepts the natural range without a blank tail");
  qApp->setStyleSheet(originalStyleSheet);
  spin();
  return result;
}

bool testPresentationOptionsRetainCardsAndInitialFolding() {
  {
    const std::string nestedThread = "nested-presentation-options";
    const AuthoritativeItemKey nestedUserKey{nestedThread, "turn", "user"};
    const AuthoritativeItemKey nestedReasoningKey{nestedThread, "turn",
                                                  "reasoning"};
    ConversationGraphSpec nestedSnapshot;
    nestedSnapshot.threadId = nestedThread;
    nestedSnapshot.sections.push_back(
        {"turn:nested-presentation-options:turn",
         "turn",
         {{nestedUserKey, CardKind::UserMessage, nestedThread, "turn", "user",
           UserMessageData{"Prompt", {}}}},
         nestedUserKey});
    ConversationView nestedView;
    nestedView.setPresentationOptions({false, true, true, true});
    nestedView.resize(700, 700);
    nestedView.show();
    bool nestedResult = applyConversation(nestedView, nestedSnapshot);
    spin();
    nestedSnapshot.sections.front().cards.push_back(
        {nestedReasoningKey, CardKind::Reasoning, nestedThread, "turn",
         "reasoning", ReasoningData{"Hidden reasoning"}});
    nestedResult &= applyConversation(nestedView, nestedSnapshot);
    spin();
    ConversationCard *nestedReasoning =
        card(nestedView, stableKey(nestedReasoningKey));
    const QModelIndex reasoningIndex =
        nestedView.conversationModel()->indexForStableKey(
            stableKey(nestedReasoningKey));
    if (!expect(nestedResult && reasoningIndex.isValid() &&
                    !reasoningIndex.data(ConversationItemModel::PresentedRole)
                         .toBool() &&
                    nestedReasoning == nullptr,
                "filtered nested reasoning is retained without painting"))
      return false;
  }

  {
    const std::string followingThread = "following-nested-insertion";
    ConversationGraphSpec followingSnapshot;
    followingSnapshot.threadId = followingThread;
    TurnGraphSpec followingSection{
        "turn:following-nested-insertion:turn", "turn", {}};
    followingSection.cards.push_back(
        {AuthoritativeItemKey{followingThread, "turn", "user"},
         CardKind::UserMessage, followingThread, "turn", "user",
         UserMessageData{"Prompt", {}}});
    followingSection.rootCardKey = followingSection.cards.front().key;
    for (int index = 0; index < 12; ++index)
      followingSection.cards.push_back(
          agentCard(followingThread, "turn", index));
    followingSection.cards.insert(
        followingSection.cards.begin() + 6,
        {AuthoritativeItemKey{followingThread, "turn", "reasoning"},
         CardKind::Reasoning, followingThread, "turn", "reasoning",
         ReasoningData{"Initially hidden reasoning detail"}});
    followingSnapshot.sections.push_back(std::move(followingSection));

    ConversationView followingView;
    followingView.setPresentationOptions({false, true, true, true});
    followingView.resize(520, 320);
    followingView.show();
    bool followingResult = applyConversation(followingView, followingSnapshot);
    spin();
    const AuthoritativeItemKey incomingKey{followingThread, "turn", "incoming"};
    followingSnapshot.sections.front().cards.push_back(
        {incomingKey, CardKind::AgentActivity, followingThread, "turn",
         "incoming",
         AgentActivityData{
             "tool", "tool", "New nested activity", {}, {}, {}, {}, {}, {}, {}},
         nodegraph::NodeStatus::Completed});
    followingResult &= applyConversation(followingView, followingSnapshot);
    followingResult &= spinUntil(
        [&] { return card(followingView, stableKey(incomingKey)) != nullptr; });
    ConversationCard *incoming = card(followingView, stableKey(incomingKey));
    const int materializedTop =
        incoming ? incoming->mapTo(followingView.viewport(), QPoint{}).y() : -1;
    const bool materializedAtBottom = followingView.isAtBottom();
    spin(320);
    const int settledTop =
        incoming ? incoming->mapTo(followingView.viewport(), QPoint{}).y() : -1;
    if (!expect(followingResult && incoming && materializedAtBottom &&
                    materializedTop == settledTop,
                "a new nested card reaches its final followed position in "
                "the bounded materialization continuation"))
      return false;

    followingView.setPresentationOptions({true, true, true, true});
    const int immediateToggleTop =
        incoming->mapTo(followingView.viewport(), QPoint{}).y();
    const bool toggleImmediatelyAtBottom = followingView.isAtBottom();
    spin(320);
    const int settledToggleTop =
        incoming->mapTo(followingView.viewport(), QPoint{}).y();
    if (!expect(toggleImmediatelyAtBottom &&
                    immediateToggleTop == settledToggleTop,
                "presentation toggles relayout atomically without a follow "
                "animation"))
      return false;
  }

  const std::string thread = "presentation-options";
  const AuthoritativeItemKey updateKey{thread, "turn", "update"};
  const AuthoritativeItemKey finalKey{thread, "turn", "final"};
  const AuthoritativeItemKey reasoningKey{thread, "turn", "reasoning"};
  const AuthoritativeItemKey firstCommandKey{thread, "turn", "command-1"};
  const AuthoritativeItemKey firstImageKey{thread, "turn", "image-1"};
  const AuthoritativeItemKey firstFileChangesKey{thread, "turn", "files-1"};
  ConversationGraphSpec snapshot;
  snapshot.threadId = thread;
  snapshot.sections.push_back(
      {"turn:presentation-options:turn",
       "turn",
       {{updateKey, CardKind::AgentMessage, thread, "turn", "update",
         AgentMessageData{"First retained update", false}},
        {finalKey, CardKind::AgentMessage, thread, "turn", "final",
         AgentMessageData{"Final answer remains visible", true}},
        {reasoningKey, CardKind::Reasoning, thread, "turn", "reasoning",
         ReasoningData{"First retained reasoning"}},
        {firstCommandKey, CardKind::CommandExecution, thread, "turn",
         "command-1", CommandExecutionData{"printf first", {}, {}, 0, {}},
         nodegraph::NodeStatus::Completed},
        {firstImageKey, CardKind::ImageGeneration, thread, "turn", "image-1",
         ImageGenerationData{"/missing/image-1.png", "First image"},
         nodegraph::NodeStatus::Completed},
        {firstFileChangesKey, CardKind::FileChanges, thread, "turn", "files-1",
         FileChangesData{{{"src/a-deliberately-long-file-name-that-wraps-at-"
                           "the-final-viewport-width.cpp",
                           "update", 2, 1}},
                         "/workspace"},
         nodegraph::NodeStatus::Completed}}});
  const auto containsText = [](QWidget *widget, const QString &needle) {
    return std::ranges::any_of(widget->findChildren<QLabel *>(),
                               [&needle](QLabel *label) {
                                 return label->text().contains(needle);
                               }) ||
           std::ranges::any_of(widget->findChildren<MarkdownTextView *>(),
                               [&needle](MarkdownTextView *view) {
                                 return view->markdownSource().contains(needle);
                               });
  };

  ConversationView view;
  view.setPresentationOptions({false, true, true, true, true});
  view.resize(700, 5000);
  view.show();
  bool result = expect(applyConversation(view, snapshot),
                       "presentation-options fixture renders");
  result &= spinUntil([&] {
    return card(view, stableKey(updateKey)) &&
           card(view, stableKey(finalKey)) &&
           card(view, stableKey(firstCommandKey)) &&
           card(view, stableKey(firstImageKey)) &&
           card(view, stableKey(firstFileChangesKey));
  });
  QPointer<ConversationCard> update = card(view, stableKey(updateKey));
  QPointer<ConversationCard> final = card(view, stableKey(finalKey));
  QPointer<ConversationCard> reasoning = card(view, stableKey(reasoningKey));
  QPointer<ConversationCard> firstCommand =
      card(view, stableKey(firstCommandKey));
  QPointer<ConversationCard> firstImage = card(view, stableKey(firstImageKey));
  QPointer<ConversationCard> firstFileChanges =
      card(view, stableKey(firstFileChangesKey));
  const QModelIndex reasoningIndex =
      view.conversationModel()->indexForStableKey(stableKey(reasoningKey));
  result &= expect(
      update && final && !reasoning && reasoningIndex.isValid() &&
          !reasoningIndex.data(ConversationItemModel::PresentedRole).toBool() &&
          firstCommand && firstImage && firstFileChanges &&
          !firstCommand->isCollapsed() && !firstImage->isCollapsed() &&
          !firstFileChanges->isCollapsed(),
      "default presentation retains hidden reasoning and opens "
      "commands, images, and file changes");
  if (!update || !final || !firstCommand || !firstImage || !firstFileChanges)
    return false;
  auto *firstFileChangesList = firstFileChanges->findChild<QTextBrowser *>(
      QStringLiteral("fileChangesList"));
  view.resize(360, 5000);
  spin();
  result &= expect(
      firstFileChangesList && firstFileChangesList->isVisible() &&
          firstFileChangesList->height() >=
              2 * firstFileChangesList->fontMetrics().lineSpacing() &&
          firstFileChangesList->toPlainText().contains(
              QStringLiteral("a-deliberately-long-file-name")),
      "an initially expanded file-change card keeps its body height after "
      "viewport reflow");
  view.resize(700, 5000);
  spin();

  view.setPresentationOptions({false, false, false, false, false});
  spin();
  result &= expect(
      !update && !reasoning && final && firstCommand &&
          !firstCommand->isCollapsed() &&
          !view.conversationModel()
               ->indexForStableKey(stableKey(updateKey))
               .data(ConversationItemModel::PresentedRole)
               .toBool() &&
          !reasoningIndex.data(ConversationItemModel::PresentedRole).toBool(),
      "filters release hidden update/reasoning editors without "
      "changing the final answer or existing folds");

  std::get<AgentMessageData>(snapshot.sections.front().cards[0].payload).text =
      "Updated while hidden";
  std::get<ReasoningData>(snapshot.sections.front().cards[2].payload).summary =
      "Reasoning updated while hidden";
  const AuthoritativeItemKey secondCommandKey{thread, "turn", "command-2"};
  const AuthoritativeItemKey secondImageKey{thread, "turn", "image-2"};
  const AuthoritativeItemKey secondFileChangesKey{thread, "turn", "files-2"};
  snapshot.sections.front().cards.push_back(
      {secondCommandKey, CardKind::CommandExecution, thread, "turn",
       "command-2", CommandExecutionData{"printf second", {}, {}, 0, {}},
       nodegraph::NodeStatus::Completed});
  snapshot.sections.front().cards.push_back(
      {secondImageKey, CardKind::ImageGeneration, thread, "turn", "image-2",
       ImageGenerationData{"/missing/image-2.png", "Second image"},
       nodegraph::NodeStatus::Completed});
  snapshot.sections.front().cards.push_back(
      {secondFileChangesKey, CardKind::FileChanges, thread, "turn", "files-2",
       FileChangesData{{{"src/second.cpp", "add", 1, 0}}, "/workspace"},
       nodegraph::NodeStatus::Completed});
  result &= expect(applyConversation(view, snapshot),
                   "hidden cards and a new command accept updates");
  result &= spinUntil([&] {
    return card(view, stableKey(secondCommandKey)) &&
           card(view, stableKey(secondImageKey)) &&
           card(view, stableKey(secondFileChangesKey));
  });
  QPointer<ConversationCard> secondCommand =
      card(view, stableKey(secondCommandKey));
  QPointer<ConversationCard> secondImage =
      card(view, stableKey(secondImageKey));
  QPointer<ConversationCard> secondFileChanges =
      card(view, stableKey(secondFileChangesKey));
  result &= expect(!update && !reasoning && secondCommand &&
                       secondCommand->isCollapsed() && secondImage &&
                       secondImage->isCollapsed() && secondFileChanges &&
                       secondFileChanges->isCollapsed(),
                   "filtered rows remain widget-free while new commands, "
                   "images, and file changes use current initial preferences");

  result &= expect(setFolded(firstCommand, true),
                   "an existing command records a user-owned collapsed state");
  view.setPresentationOptions({true, true, true, true, true});
  result &= spinUntil([&] {
    return card(view, stableKey(updateKey)) &&
           card(view, stableKey(reasoningKey));
  });
  update = card(view, stableKey(updateKey));
  reasoning = card(view, stableKey(reasoningKey));
  result &= expect(
      update && reasoning && !update->isHidden() && !reasoning->isHidden() &&
          containsText(update, QStringLiteral("Updated while hidden")) &&
          reasoning->property("conversationBodyProjectionDeferred").toBool() &&
          firstCommand && firstCommand->isCollapsed() && secondCommand &&
          secondCommand->isCollapsed() && firstImage &&
          !firstImage->isCollapsed() && secondImage &&
          secondImage->isCollapsed() && firstFileChanges &&
          !firstFileChanges->isCollapsed() && secondFileChanges &&
          secondFileChanges->isCollapsed(),
      "restoring visibility reveals latest content and preserves existing "
      "folds");
  result &= expect(setFolded(reasoning, false),
                   "the restored reasoning card expands on demand");
  result &= expect(
      containsText(reasoning,
                   QStringLiteral("Reasoning updated while hidden")) &&
          !reasoning->property("conversationBodyProjectionDeferred").toBool(),
      "expansion projects the latest reasoning retained while hidden");

  const AuthoritativeItemKey thirdCommandKey{thread, "turn", "command-3"};
  const AuthoritativeItemKey thirdFileChangesKey{thread, "turn", "files-3"};
  snapshot.sections.front().cards.push_back(
      {thirdCommandKey, CardKind::CommandExecution, thread, "turn", "command-3",
       CommandExecutionData{"printf third", {}, {}, 0, {}},
       nodegraph::NodeStatus::Completed});
  snapshot.sections.front().cards.push_back(
      {thirdFileChangesKey, CardKind::FileChanges, thread, "turn", "files-3",
       FileChangesData{{{"src/third.cpp", "update", 1, 1}}, "/workspace"},
       nodegraph::NodeStatus::Completed});
  result &= expect(applyConversation(view, snapshot),
                   "a command arrives after restoring expanded-by-default");
  result &= spinUntil([&] {
    return card(view, stableKey(thirdCommandKey)) != nullptr &&
           card(view, stableKey(thirdFileChangesKey)) != nullptr;
  });
  QPointer<ConversationCard> thirdCommand =
      card(view, stableKey(thirdCommandKey));
  QPointer<ConversationCard> thirdFileChanges =
      card(view, stableKey(thirdFileChangesKey));
  result &=
      expect(thirdCommand && !thirdCommand->isCollapsed() && firstCommand &&
                 firstCommand->isCollapsed() && secondCommand &&
                 secondCommand->isCollapsed() && thirdFileChanges &&
                 !thirdFileChanges->isCollapsed() && firstFileChanges &&
                 !firstFileChanges->isCollapsed() && secondFileChanges &&
                 secondFileChanges->isCollapsed(),
             "only newly appearing commands and file changes use the changed "
             "initial folding preference");
  return result;
}

bool testInitialCommandGeometrySettlement() {
  const std::string thread = "initial-command-thread";
  const QString output =
      QStringLiteral("Thread debugging using libthread_db enabled.\n"
                     "Using host libthread_db library.\n"
                     "0x00007f01 found.\n"
                     "0x00007f02 found.\n"
                     "1 pattern found.\n"
                     "[Inferior 1 detached]");
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{
          "/bin/bash -lc 'YDOTOOL_SOCKET=/run/user/1000/ydotool_socket "
          "ydotool mousemove -x -10000 -y -10000 && ydotool click 0xC0; "
          "sleep 1; spectacle -b -n -o /tmp/codexui-live-front.png && "
          "file /tmp/codexui-live-front.png'",
          utf8(output), "/workspace", 0, 1200},
      nodegraph::NodeStatus::Completed};
  const VisibleCardData compact{
      AuthoritativeItemKey{thread, "turn", "compact"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "compact",
      CommandExecutionData{
          "/bin/bash -lc \"gdb -q -batch -p 2285053 -ex 'set pagination "
          "off' -ex 'find /g 0x5638307a6000, 0x563834f9e000'\"",
          "/tmp/codexui-live-front.png: PNG image data, 1920 x 1200",
          "/workspace", 0, 900},
      nodegraph::NodeStatus::Completed};
  ConversationGraphSpec snapshot{
      thread, {{"turn:initial-command", "turn", {command, compact}}}, false};

  ConversationView view;
  view.setPresentationOptions({true, true, true, false, false});
  view.resize(820, 1000);
  view.show();
  spin();
  bool result = expect(applyConversation(view, snapshot),
                       "initially expanded screenshot-shaped commands render");
  ConversationCard *commandCard = card(view, stableKey(command.key));
  ConversationCard *compactCard = card(view, stableKey(compact.key));
  auto *outputView = commandCard ? commandCard->findChild<CommandOutputView *>(
                                       QStringLiteral("commandOutputView"))
                                 : nullptr;
  auto *compactOutput = compactCard
                            ? compactCard->findChild<CommandOutputView *>(
                                  QStringLiteral("commandOutputView"))
                            : nullptr;
  const auto commandText = [](ConversationCard *card) {
    return card ? dynamic_cast<ContentSizedTextView *>(
                      card->findChild<QTextEdit *>(
                          QStringLiteral("commandTextView")))
                : nullptr;
  };
  const auto exactCard = [&](ConversationCard *card,
                             const VisibleCardData &data) {
    auto *header = card ? card->findChild<QWidget *>(
                              QStringLiteral("conversationCardHeader"))
                        : nullptr;
    auto *content = card ? card->findChild<QWidget *>(
                               QStringLiteral("conversationCardContent"))
                         : nullptr;
    const QModelIndex index =
        view.conversationModel()->indexForStableKey(stableKey(data.key));
    const int contentHeight =
        content && content->layout()
            ? (content->layout()->hasHeightForWidth()
                   ? content->layout()->heightForWidth(content->width())
                   : content->layout()->sizeHint().height())
            : -1;
    return card && header && content &&
           view.visualRect(index).height() == card->height() &&
           header->height() == header->sizeHint().height() &&
           content->height() == contentHeight;
  };
  const auto exactEditor = [](auto *editor) {
    return editor && !editor->isHidden() &&
           editor->height() == editor->sizeHint().height() &&
           editor->height() < editor->maximumHeight() &&
           editor->verticalScrollBar()->maximum() == 0;
  };
  result &= expect(exactCard(commandCard, command) &&
                       exactCard(compactCard, compact) &&
                       exactEditor(commandText(commandCard)) &&
                       exactEditor(commandText(compactCard)) &&
                       exactEditor(outputView) && exactEditor(compactOutput),
                   "initial commands export one compact height after their "
                   "editors reach final widths");
  if (!commandCard || !compactCard || !outputView)
    return false;
  const int immediateRange = view.verticalScrollBar()->maximum();
  const QRect commandGeometry = commandCard->geometry();
  const QRect compactGeometry = compactCard->geometry();
  spin();
  result &= expect(view.verticalScrollBar()->maximum() == immediateRange &&
                       commandCard->geometry() == commandGeometry &&
                       compactCard->geometry() == compactGeometry,
                   "initial commands need no delayed geometry settlement");
  const QImage commandPixels = commandCard->grab().toImage();
  result &= expect(!applyConversation(view, snapshot),
                   "an identical command snapshot is a semantic no-op");
  spin();
  result &= expect(commandCard->geometry() == commandGeometry &&
                       compactCard->geometry() == compactGeometry &&
                       commandCard->grab().toImage() == commandPixels,
                   "a command no-op leaves row geometry and pixels unchanged");

  const QFontMetricsF glyphMetrics(outputView->font());
  const qreal glyphWidth =
      std::max(0.01, glyphMetrics.horizontalAdvance(QLatin1Char('W')));
  const int charactersPerLine =
      std::max(1, static_cast<int>(std::floor(outputView->viewport()->width() /
                                              glyphWidth)));
  QString wrappedOutput(charactersPerLine + 1, QLatin1Char('W'));
  while (glyphMetrics.horizontalAdvance(wrappedOutput) <=
         outputView->viewport()->width())
    wrappedOutput += QLatin1Char('W');
  auto &execution = std::get<CommandExecutionData>(
      snapshot.sections.front().cards.front().payload);
  execution.output = utf8(wrappedOutput);
  result &= expect(applyConversation(view, snapshot),
                   "single logical output line changes to two visual lines");
  spin();
  const QTextBlock wrappedBlock = outputView->document()->firstBlock();
  const bool wrappedOutputFullyVisible =
      wrappedBlock.layout() && wrappedBlock.layout()->lineCount() == 2 &&
      outputView->verticalScrollBar()->maximum() == 0 &&
      outputView->viewport()->height() >=
          static_cast<int>(std::ceil(outputView->document()->size().height()));
  if (!wrappedOutputFullyVisible)
    std::cerr << "initial command geometry: widget=" << outputView->height()
              << " hint=" << outputView->sizeHint().height()
              << " viewport=" << outputView->viewport()->height()
              << " document=" << outputView->document()->size().height()
              << " lines="
              << (wrappedBlock.layout() ? wrappedBlock.layout()->lineCount()
                                        : -1)
              << " block="
              << (wrappedBlock.layout()
                      ? wrappedBlock.layout()->boundingRect().height()
                      : -1)
              << " scroll=" << outputView->verticalScrollBar()->maximum()
              << '\n';
  result &= expect(
      wrappedOutputFullyVisible,
      "two visual output lines are fully visible without inner scrolling");
  return result;
}

bool testRootlessFinalAnswerGeometrySettlement() {
  const std::string thread = "rootless-child-thread";
  const VisibleCardData activity{
      AuthoritativeItemKey{thread, "turn", "activity"},
      CardKind::AgentActivity,
      thread,
      "turn",
      "activity",
      AgentActivityData{
          "spawn_agent", "tool", "Child work", {}, {}, {}, {}, {}, {}, {}},
      nodegraph::NodeStatus::Completed};
  VisibleCardData answer{
      AuthoritativeItemKey{thread, "turn", "answer"},
      CardKind::AgentMessage,
      thread,
      "turn",
      "answer",
      AgentMessageData{"Implemented the requested child-thread change.", true}};
  ConversationGraphSpec snapshot{
      thread, {{"turn:rootless-child", "turn", {activity, answer}}}, false};

  ConversationView view;
  view.resize(700, 700);
  view.show();
  bool result = expect(applyConversation(view, snapshot),
                       "rootless child activity and final answer appear");
  spin();
  ConversationCard *answerCard = card(view, stableKey(answer.key));
  if (!answerCard)
    return false;
  answerCard->setMinimumHeight(600);
  answerCard->resize(answerCard->width(), 600);
  std::get<AgentMessageData>(answer.payload).text += "\n\nValidation passed.";
  snapshot.sections.front().cards.back() = answer;
  result &= expect(applyConversation(view, snapshot),
                   "rootless final answer accepts an authoritative update");
  spin();
  MarkdownTextView *answerBody = answerCard->findChild<MarkdownTextView *>();
  result &= expect(answerBody &&
                       answerCard->height() == answerCard->minimumHeight() &&
                       answerCard->height() < 200 &&
                       answerBody->height() >=
                           answerBody->heightForWidth(answerBody->width()),
                   "rootless final answer settles to its natural final-width "
                   "height instead of retaining stale viewport space");
  return result;
}

bool testRetainedNestedFinalAnswerGeometrySettlement() {
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  const std::string thread = "retained-nested-final-answer";
  const VisibleCardData prompt{
      AuthoritativeItemKey{thread, "turn", "prompt"},
      CardKind::UserMessage,
      thread,
      "turn",
      "prompt",
      UserMessageData{"Please provide the complete retained report.", {}}};
  QString markdown = QStringLiteral(
      "The retained report contains enough Markdown to require its final "
      "nested width before height calculation.\n\n"
      "Its complete list must remain inside the final-answer border:\n\n");
  for (int index = 1; index <= 48; ++index)
    markdown += QStringLiteral(
                    "- Retained result %1 with explanatory text, **emphasis**, "
                    "and enough detail to wrap naturally at the nested card "
                    "width.\n")
                    .arg(index);
  markdown += QStringLiteral(
      "\nRenamed:\n\n"
      "- `src/codex/PresentationStatus.h` → `src/codex/UiStatus.h`\n\n"
      "</details>\n\n"
      "No remote operation was performed; the final line must remain fully "
      "visible.\n");
  const VisibleCardData answer{
      AuthoritativeItemKey{thread, "turn", "answer"},
      CardKind::AgentMessage,
      thread,
      "turn",
      "answer",
      AgentMessageData{"Retained final answer is materializing.", true}};
  TurnGraphSpec section{"turn:retained", "turn", {prompt}, prompt.key};
  for (int index = 0; index < 4; ++index)
    section.cards.push_back(
        agentCard(thread, "turn", index,
                  QStringLiteral("Retained update %1 preceding the final "
                                 "answer with enough text to wrap.")
                      .arg(index)));
  section.cards.push_back(answer);
  ConversationGraphSpec snapshot{thread, {std::move(section)}, false};

  ConversationView view;
  view.resize(980, 420);
  bool result =
      expect(applyConversation(view, snapshot),
             "retained prompt and partial final answer materialize initially");
  std::get<AgentMessageData>(snapshot.sections.front().cards.back().payload)
      .text = utf8(markdown);
  result &= expect(applyConversation(view, snapshot),
                   "retained hydration completes before first exposure");
  view.resize(560, 420);
  view.show();
  result &= spinUntil([&] {
    return view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  });
  spin(160);
  const QModelIndex promptIndex =
      view.conversationModel()->indexForStableKey(stableKey(prompt.key));
  const QModelIndex answerIndex =
      view.conversationModel()->indexForStableKey(stableKey(answer.key));
  view.scrollTo(answerIndex, QAbstractItemView::PositionAtTop);
  spin(40);
  ConversationCard *answerCard = card(view, stableKey(answer.key));
  MarkdownTextView *answerBody =
      answerCard ? answerCard->findChild<MarkdownTextView *>() : nullptr;
  int documentHeight = 0;
  if (answerBody)
    documentHeight =
        static_cast<int>(std::ceil(answerBody->document()->size().height()));
  if (answerBody && answerBody->height() <
                        documentHeight + answerBody->fontMetrics().descent())
    std::cerr << "nested final geometry: body=" << answerBody->height()
              << " document=" << documentHeight
              << " descent=" << answerBody->fontMetrics().descent()
              << " hfw=" << answerBody->heightForWidth(answerBody->width())
              << " hint=" << answerBody->sizeHint().height()
              << " width=" << answerBody->width()
              << " card=" << answerCard->height()
              << " row=" << view.visualRect(answerIndex).height() << '\n';
  result &= expect(
      promptIndex.isValid() && answerIndex.isValid() && answerCard &&
          answerBody && answerBody->markdownSource() == markdown &&
          promptIndex.data(ConversationItemModel::TurnRootRole).toBool() &&
          answerIndex.data(ConversationItemModel::NestedCardRole).toBool() &&
          answerBody->height() >=
              documentHeight + answerBody->fontMetrics().descent() &&
          answerBody->mapTo(answerCard, QPoint(0, answerBody->height())).y() <=
              answerCard->contentsRect().bottom() + 1 &&
          view.visualRect(answerIndex).height() == answerCard->height(),
      "an initially retained nested final answer fully fits its rendered "
      "document and virtual Turn/You row");

  QPointer<ConversationCard> retainedAnswer = answerCard;
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
  spin();
  const auto retainedAnchor = firstVisible(view);
  const VisibleCardData laterPrompt{
      AuthoritativeItemKey{thread, "later-turn", "later-prompt"},
      CardKind::UserMessage,
      thread,
      "later-turn",
      "later-prompt",
      UserMessageData{"A later prompt arrives after the long answer.", {}}};
  const VisibleCardData laterAnswer{
      AuthoritativeItemKey{thread, "later-turn", "later-answer"},
      CardKind::AgentMessage,
      thread,
      "later-turn",
      "later-answer",
      AgentMessageData{"The later result is complete.", true}};
  snapshot.sections.push_back({"turn:later",
                               "later-turn",
                               {laterPrompt, laterAnswer},
                               laterPrompt.key});
  result &= expect(applyConversation(view, snapshot),
                   "a later completed Turn is appended after the long answer");
  spin(160);
  answerCard = card(view, stableKey(answer.key));
  const QModelIndex retainedAnswerIndex =
      view.conversationModel()->indexForStableKey(stableKey(answer.key));
  const QModelIndex laterPromptIndex =
      view.conversationModel()->indexForStableKey(stableKey(laterPrompt.key));
  result &= expect(
      answerCard && retainedAnswer == answerCard &&
          retainedAnswerIndex.isValid() && laterPromptIndex.isValid() &&
          view.visualRect(laterPromptIndex).top() >
              view.visualRect(retainedAnswerIndex).bottom() &&
          firstVisible(view) == retainedAnchor &&
          answerBody->mapTo(answerCard, QPoint(0, answerBody->height())).y() <=
              answerCard->contentsRect().bottom() + 1,
      "appending a later conversation card cannot clip the retained long "
      "answer or move its paused virtual-row anchor");
  spin();
  qApp->setStyleSheet(originalStyleSheet);
  return result;
}

bool testBottomAnchoredCommandOutputGrowth() {
  const std::string thread = "bottom-anchored-output";
  ConversationGraphSpec snapshot = conversation(thread, 14);
  VisibleCardData command{
      AuthoritativeItemKey{thread, "turn-2", "live-command"},
      CardKind::CommandExecution,
      thread,
      "turn-2",
      "live-command",
      CommandExecutionData{"run live command", {}, {}, std::nullopt, {}},
      nodegraph::NodeStatus::Running};
  snapshot.sections.back().cards.push_back(command);

  ConversationView view;
  view.resize(620, 360);
  view.show();
  applyConversation(view, snapshot);
  spinUntil([&] {
    return view.viewport()->updatesEnabled() &&
           !view.property("bulkMaterializationUpdatesSuppressed").toBool();
  });
  ConversationCard *commandCard = card(view, stableKey(command.key));
  bool result = expect(setFolded(commandCard, false),
                       "live command expands from its compact default");
  wheel(view, -10000);
  auto *metadata =
      commandCard
          ? commandCard->findChild<QLabel *>(QStringLiteral("commandMetadata"))
          : nullptr;
  auto *status =
      commandCard
          ? commandCard->findChild<QLabel *>(QStringLiteral("commandStatus"))
          : nullptr;
  auto *output = commandCard ? commandCard->findChild<CommandOutputView *>(
                                   QStringLiteral("commandOutputView"))
                             : nullptr;
  result &= expect(commandCard && metadata && metadata->isHidden() && status &&
                       output && output->isHidden() && view.isAtBottom() &&
                       labelUsesColor(status, UiStyle::blueText),
                   "live command starts with a hidden zero-line output");
  if (!commandCard || !metadata || !status || !output)
    return false;
  const int cardBottomBefore =
      commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height())).y();

  auto &live = std::get<CommandExecutionData>(
      snapshot.sections.back().cards.back().payload);
  live.output =
      "first wrapped output line with enough words to use real width\n"
      "second output line\nthird output line\n\n";
  result &=
      expect(applyConversation(view, snapshot), "live output becomes visible");
  spinUntil([&] {
    return !output->isHidden() && output->height() > 2 * 20 &&
           output->height() == output->sizeHint().height();
  });
  const int cardBottomAfter =
      commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height())).y();
  QTextCursor initialEnd(output->document());
  initialEnd.movePosition(QTextCursor::End);
  const int initialBottomGap =
      output->viewport()->height() - output->cursorRect(initialEnd).bottom();
  qreal initialLineHeight = 0;
  for (QTextBlock block = output->document()->begin(); block.isValid();
       block = block.next())
    if (block.layout())
      initialLineHeight += block.layout()->boundingRect().height();
  result &= expect(
      !output->isHidden() && output->height() > 2 * 20 &&
          output->height() == output->sizeHint().height() &&
          output->height() ==
              8 + static_cast<int>(std::ceil(initialLineHeight)) &&
          (output->maximumHeight() - 8) % output->fontMetrics().lineSpacing() ==
              0 &&
          initialBottomGap <= output->document()->documentMargin() + 2 &&
          !output->document()->lastBlock().text().isEmpty() &&
          output->verticalScrollBar()->value() ==
              output->verticalScrollBar()->maximum() &&
          cardBottomAfter == cardBottomBefore && view.isAtBottom(),
      "multiline output uses complete text rows with symmetric "
      "padding, no synthetic trailing row, and grows upward");

  QString cappedOutput;
  for (int line = 0; line < 80; ++line)
    cappedOutput += QStringLiteral("scrollable line %1\n").arg(line);
  live.output = utf8(cappedOutput);
  result &=
      expect(applyConversation(view, snapshot), "live output reaches its cap");
  spinUntil([&] {
    return output->height() == output->maximumHeight() &&
           output->verticalScrollBar()->maximum() > 0;
  });
  if (!(output->height() == output->maximumHeight() &&
        output->verticalScrollBar()->maximum() > 0 &&
        commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height()))
                .y() == cardBottomBefore))
    std::cerr
        << "capped output: height=" << output->height()
        << " maximum=" << output->verticalScrollBar()->maximum()
        << " presentedBytes="
        << std::get<CommandExecutionData>(commandCard->data().payload)
               .output.size()
        << " expectedBytes=" << utf8(cappedOutput).size() << " bottom="
        << commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height()))
               .y()
        << " expectedBottom=" << cardBottomBefore << " frozen="
        << view.property("bulkMaterializationUpdatesSuppressed").toBool()
        << " blocker="
        << view.property("bulkMaterializationBlocker").toString().toStdString()
        << '\n';
  QTextCursor cappedEnd(output->document());
  cappedEnd.movePosition(QTextCursor::End);
  const int cappedBottomGap =
      output->viewport()->height() - output->cursorRect(cappedEnd).bottom();
  const QRect cappedViewport = output->viewport()->geometry();
  const int cappedTopInset = cappedViewport.top();
  const int cappedBottomInset =
      output->height() - cappedViewport.bottom() - 1;
  const QRect firstVisibleLine =
      output->cursorRect(output->cursorForPosition(QPoint(0, 0)));
  const bool cappedViewportAligned =
      cappedTopInset >= UiStyle::commandOutputVerticalPadding &&
      cappedBottomInset >= UiStyle::commandOutputVerticalPadding &&
      std::abs(cappedTopInset - cappedBottomInset) <= 1 &&
      (firstVisibleLine.top() >= 0 || firstVisibleLine.bottom() < 0);
  result &= expect(
      output->height() == output->maximumHeight() &&
          output->verticalScrollBar()->maximum() > 0 && cappedBottomGap <= 2 &&
          cappedViewportAligned &&
          !output->document()->lastBlock().text().isEmpty() &&
          output->verticalScrollBar()->value() ==
              output->verticalScrollBar()->maximum() &&
          commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height()))
                  .y() == cardBottomBefore,
      "capped output keeps symmetric outer padding, exposes only whole rows, "
      "and follows its final populated row");

  const qulonglong geometryBeforeAppend =
      view.property("conversationLocalGeometryPasses").toULongLong();
  QPointer<ConversationCard> retainedCommand = commandCard;
  QTextCursor selectedOutput(output->document());
  selectedOutput.setPosition(12);
  selectedOutput.setPosition(38, QTextCursor::KeepAnchor);
  output->setTextCursor(selectedOutput);
  const QString selectionBeforeAppend = output->textCursor().selectedText();
  live.output += "one more append-only streaming line\n";
  result &= expect(applyConversation(view, snapshot),
                   "capped output accepts another streaming append");
  spin();
  result &= expect(
      retainedCommand == commandCard &&
          output->height() == output->maximumHeight() &&
          output->verticalScrollBar()->value() ==
              output->verticalScrollBar()->maximum() &&
          view.property("conversationLocalGeometryPasses").toULongLong() ==
              geometryBeforeAppend &&
          commandCard->mapTo(view.viewport(), QPoint(0, commandCard->height()))
                  .y() == cardBottomBefore,
      "append-only capped output repaints its retained card without a "
      "conversation geometry pass");
  result &= expect(output->textCursor().selectedText() == selectionBeforeAppend,
                   "append-only command streaming preserves output text "
                   "selection");
  snapshot.sections.back().cards.back().status =
      nodegraph::NodeStatus::Completed;
  result &= expect(applyConversation(view, snapshot),
                   "the live command reaches completion");
  spin();
  QTextCursor completedEnd(output->document());
  completedEnd.movePosition(QTextCursor::End);
  result &= expect(
      output->viewport()->height() -
                  output->cursorRect(completedEnd).bottom() <=
              2 &&
          !output->document()->lastBlock().text().isEmpty() &&
          output->verticalScrollBar()->value() ==
              output->verticalScrollBar()->maximum(),
      "command completion retains follow-tail without a synthetic output row");
  return result;
}

bool testCommandOutputStateAcrossNavigation() {
  const std::string thread = "command-navigation-thread";
  QString output;
  for (int line = 0; line < 80; ++line)
    output += QStringLiteral("retained line %1\n").arg(line);
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{"produce output", utf8(output), {}, 0, {}},
      nodegraph::NodeStatus::Completed};
  const ConversationGraphSpec commandThread{
      thread, {{"turn:command-navigation", "turn", {command}}}, false};

  ConversationView view;
  view.resize(650, 520);
  view.show();
  applyConversation(view, commandThread);
  spin();
  ConversationCard *commandCard = card(view, stableKey(command.key));
  bool result = expect(setFolded(commandCard, false),
                       "navigation command expands from its compact default");
  auto *initialOutput = commandCard
                            ? commandCard->findChild<CommandOutputView *>(
                                  QStringLiteral("commandOutputView"))
                            : nullptr;
  result &=
      expect(initialOutput && initialOutput->verticalScrollBar()->maximum() > 0,
             "navigation test has independently scrollable output");
  if (!initialOutput)
    return false;
  applyConversation(view, conversation("other-thread", 8));
  spin();
  applyConversation(view, commandThread);
  spin();
  commandCard = card(view, stableKey(command.key));
  initialOutput = commandCard ? commandCard->findChild<CommandOutputView *>(
                                    QStringLiteral("commandOutputView"))
                              : nullptr;
  result &= expect(initialOutput && initialOutput->followsLatest() &&
                       initialOutput->verticalScrollBar()->value() ==
                           initialOutput->verticalScrollBar()->maximum(),
                   "framework geometry during navigation does not pause a "
                   "following command output");
  if (!initialOutput)
    return false;
  wheel(view, -300);
  result &= expect(view.mode() == ConversationView::Mode::Following,
                   "the outer conversation follows before inner output "
                   "detachment");
  initialOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  spin();
  QTextCursor retainedSelection(initialOutput->document());
  retainedSelection.setPosition(output.size() - 48);
  retainedSelection.setPosition(output.size() - 22, QTextCursor::KeepAnchor);
  initialOutput->setTextCursor(retainedSelection);
  const QString selectedText = retainedSelection.selectedText();
  const int pausedValue = initialOutput->verticalScrollBar()->value();
  result &= expect(!initialOutput->followsLatest() &&
                       view.mode() == ConversationView::Mode::Paused,
                   "command output owns the outer pause before thread "
                   "navigation");

  applyConversation(view, conversation("other-thread", 8));
  spin();
  applyConversation(view, commandThread);
  spin();
  commandCard = card(view, stableKey(command.key));
  auto *restoredOutput = commandCard
                             ? commandCard->findChild<CommandOutputView *>(
                                   QStringLiteral("commandOutputView"))
                             : nullptr;
  result &=
      expect(restoredOutput && !restoredOutput->followsLatest() &&
                 view.mode() == ConversationView::Mode::Paused &&
                 restoredOutput->verticalScrollBar()->value() == pausedValue &&
                 restoredOutput->textCursor().selectedText() == selectedText,
             "thread navigation restores paused command output and selection "
             "state under the same semantic owner");
  if (restoredOutput) {
    restoredOutput->verticalScrollBar()->triggerAction(
        QAbstractSlider::SliderToMaximum);
    spin();
    result &= expect(restoredOutput->followsLatest() &&
                         view.mode() == ConversationView::Mode::Following,
                     "the restored command owner resumes outer follow-tail");
  }
  return result;
}

bool testPendingPromptAnimation() {
  AnimationDurationStyle normalMotion(100);
  VisibleCardData pending{
      LocalPromptKey{901},
      CardKind::LocalPrompt,
      "prompt-thread",
      {},
      {},
      LocalPromptData{
          901, "pending prompt", PromptState::InFlight, false, {}, {}}};
  ConversationCard card(pending, false);
  card.setStyle(&normalMotion);
  card.invalidateGeometryEnvironment();
  card.resize(560, 92);
  card.show();
  spin(40);
  const QImage first = card.grab().toImage();
  auto *pendingStatus =
      card.findChild<QLabel *>(QStringLiteral("pendingPromptStatus"));
  spin(110);
  const QImage second = card.grab().toImage();
  bool result = expect(pendingStatus &&
                           pendingStatus->text() == QStringLiteral("pending") &&
                           first == second,
                       "a newly admitted prompt begins as a calm static card");
  result &= expect(first.pixelColor(10, first.height() - 10).blue() >
                           first.pixelColor(10, first.height() - 10).red() &&
                       first.pixelColor(10, first.height() - 10).blue() >
                           first.pixelColor(10, first.height() - 10).green(),
                   "the temporary You card stays in the blue identity family");

  spin(950);
  const QImage delayedFirst = card.grab().toImage();
  spin(110);
  result &= expect(delayedFirst != card.grab().toImage(),
                   "pending feedback starts locally after one second without "
                   "a worker or graph timer update");

  const QImage animatedFirst = card.grab().toImage();
  spin(110);
  result &= expect(animatedFirst != card.grab().toImage(),
                   "an overdue unacknowledged prompt visibly animates");

  result &= expect(card.setAuthoritativeTurnActive(true),
                   "the retained prompt immediately owns the active border");
  const auto activeBorderVisible = [&card] {
    const QImage frame = card.grab().toImage();
    return frame.pixelColor(1, frame.height() / 2).red() < 175;
  };
  for (int frame = 0; frame < 10; ++frame) {
    spin(50);
    result &= expect(activeBorderVisible(),
                     "pending feedback never weakens the active border");
  }

  auto &prompt = std::get<LocalPromptData>(pending.payload);
  prompt.state = PromptState::Accepted;
  result &= expect(card.applyPresentation(pending) != PresentationImpact::None,
                   "the correlated request acknowledgement is applied");
  const QImage accepted = card.grab().toImage();
  spin(100);
  result &= expect(accepted == card.grab().toImage(),
                   "request acknowledgement immediately stops feedback");

  VisibleCardData materialized{LocalPromptKey{901},
                               CardKind::UserMessage,
                               "prompt-thread",
                               "turn",
                               "user",
                               UserMessageData{"pending prompt", {}}};
  result &=
      expect(card.applyPresentation(materialized) != PresentationImpact::None,
             "authoritative materialization retains the settled card");
  const QImage settled = card.grab().toImage();
  spin(100);
  result &= expect(settled == card.grab().toImage(),
                   "acknowledgement leaves a stable retained card");

  VisibleCardData steering{
      LocalPromptKey{902},
      CardKind::LocalPrompt,
      "prompt-thread",
      "turn",
      {},
      LocalPromptData{
          902, "steering prompt", PromptState::InFlight, false, {}, {}}};
  ConversationCard steeringCard(steering, false);
  steeringCard.setStyle(&normalMotion);
  steeringCard.invalidateGeometryEnvironment();
  steeringCard.setNestedPresentation(true);
  steeringCard.resize(520, 92);
  steeringCard.show();
  spin(40);
  const QImage steeringStatic = steeringCard.grab().toImage();
  spin(100);
  result &= expect(steeringStatic == steeringCard.grab().toImage(),
                   "steering uses the same calm initial timing");
  spin(900);
  auto *steeringStatus =
      steeringCard.findChild<QLabel *>(QStringLiteral("steeringMessagePhase"));
  result &= expect(
      steeringStatus &&
          steeringStatus->text() == QStringLiteral("steering · pending"),
      "pending steering retains its identity and shows its lifecycle state");
  const QImage steeringAnimated = steeringCard.grab().toImage();
  spin(110);
  result &= expect(steeringAnimated != steeringCard.grab().toImage(),
                   "the delayed steering sweep is visibly animated");
  result &= expect(
      steeringAnimated.pixelColor(10, steeringAnimated.height() - 10).green() >
          steeringAnimated.pixelColor(10, steeringAnimated.height() - 10).red(),
      "the steering feedback stays in the teal identity family");
  auto &steeringPrompt = std::get<LocalPromptData>(steering.payload);
  steeringPrompt.state = PromptState::Accepted;
  result &= expect(steeringCard.applyPresentation(steering) !=
                       PresentationImpact::None,
                   "the steering request acknowledgement is applied");
  auto *steeringTimer =
      steeringCard.findChild<QTimer *>(QStringLiteral("pendingAnimationTimer"));
  result &= expect(steeringTimer && !steeringTimer->isActive(),
                   "steering acknowledgement synchronously stops its timer");
  spin(40);
  const QImage acceptedSteering = steeringCard.grab().toImage();
  spin(100);
  result &=
      expect(acceptedSteering == steeringCard.grab().toImage(),
             "steering acknowledgement immediately stops its feedback sweep");
  VisibleCardData materializedSteering{
      LocalPromptKey{902}, CardKind::UserMessage,
      "prompt-thread",     "turn",
      "steering-user",     UserMessageData{"steering prompt", {}}};
  result &=
      expect(steeringCard.applyPresentation(materializedSteering) !=
                 PresentationImpact::None,
             "authoritative steering materialization retains the settled card");
  result &= expect(steeringStatus &&
                       steeringStatus->text() == QStringLiteral("steering"),
                   "acknowledgement clears pending from the steering header");
  spin(40);
  const QImage steeringSettled = steeringCard.grab().toImage();
  spin(100);
  result &= expect(steeringSettled == steeringCard.grab().toImage(),
                   "acknowledged steering remains visually stable");

  result &= expect(activeBorderVisible(),
                   "authoritative promotion retains the same active border");
  return result;
}

bool testReducedMotionUsesTheQtStyleAuthority() {
  AnimationDurationStyle normalMotion(100);
  AnimationDurationStyle reducedMotion(0);
  VisibleCardData pending{
      LocalPromptKey{903},
      CardKind::LocalPrompt,
      "reduced-motion",
      {},
      {},
      LocalPromptData{903,
                      "static pending prompt",
                      PromptState::InFlight,
                      true,
                      {},
                      {},
                      QDateTime::currentMSecsSinceEpoch() - 1500,
                      false}};
  ConversationCard pendingCard(pending, false);
  pendingCard.setStyle(&reducedMotion);
  pendingCard.invalidateGeometryEnvironment();
  pendingCard.resize(560, 92);
  pendingCard.show();
  spin(40);
  auto *pendingTimer =
      pendingCard.findChild<QTimer *>(QStringLiteral("pendingAnimationTimer"));
  const QImage pendingFrame = pendingCard.grab().toImage();
  spin(110);
  bool result = expect(
      pendingTimer && !pendingTimer->isActive() &&
          pendingCard.grab().toImage() == pendingFrame,
      "reduced motion keeps pending feedback static without a repeating timer");

  QToolButton *copy = copyButton(&pendingCard);
  if (copy)
    copy->setStyle(&normalMotion);
  const QImage copyFrame = copy ? copy->grab().toImage() : QImage{};
  QApplication::clipboard()->clear();
  if (copy)
    copy->click();
  auto *copyMotion = copy ? copy->findChild<QVariantAnimation *>() : nullptr;
  result &=
      expect(copyMotion && copyMotion->state() == QAbstractAnimation::Running,
             "enabled motion starts the shared Copy morph");
  if (copy)
    copy->setStyle(&reducedMotion);
  spin();
  result &=
      expect(copy && copyMotion &&
                 copyMotion->state() == QAbstractAnimation::Stopped &&
                 copy->grab().toImage() != copyFrame,
             "a live reduced-motion change settles Copy success immediately");

  const std::string pendingThread = "reduced-pending";
  const VisibleCardData turnRoot{
      AuthoritativeItemKey{pendingThread, "turn", "root"},
      CardKind::UserMessage,
      pendingThread,
      "turn",
      "root",
      UserMessageData{"root prompt", {}}};
  const VisibleCardData nestedPending{
      LocalPromptKey{904},
      CardKind::LocalPrompt,
      pendingThread,
      "turn",
      {},
      LocalPromptData{904,
                      "nested steering prompt",
                      PromptState::InFlight,
                      true,
                      {},
                      {},
                      QDateTime::currentMSecsSinceEpoch() - 1500,
                      false}};
  ConversationView pendingView;
  pendingView.setStyle(&normalMotion);
  pendingView.resize(620, 340);
  pendingView.show();
  ConversationGraphSpec pendingSnapshot{pendingThread,
                                        {{"turn:reduced-pending",
                                          "turn",
                                          {turnRoot, nestedPending},
                                          turnRoot.key}},
                                        false};
  result &= expect(applyConversation(pendingView, pendingSnapshot),
                   "nested pending transition fixture materializes");
  spin();
  ConversationCard *nestedPendingCard =
      card(pendingView, stableKey(nestedPending.key));
  auto *nestedPendingTimer = nestedPendingCard
                                 ? nestedPendingCard->findChild<QTimer *>(
                                       QStringLiteral("pendingAnimationTimer"))
                                 : nullptr;
  result &= expect(nestedPendingCard && nestedPendingTimer &&
                       nestedPendingTimer->isActive(),
                   "enabled motion runs the overdue steering sweep");
  if (nestedPendingCard)
    nestedPendingCard->setStyle(&reducedMotion);
  pendingView.setStyle(&reducedMotion);
  spin();
  const QImage staticPending =
      nestedPendingCard ? nestedPendingCard->grab().toImage() : QImage{};
  spin(110);
  result &= expect(nestedPendingTimer && !nestedPendingTimer->isActive() &&
                       nestedPendingCard->grab().toImage() == staticPending,
                   "live reduced motion stops the resident steering sweep");
  if (nestedPendingCard)
    nestedPendingCard->setStyle(&normalMotion);
  pendingView.setStyle(&normalMotion);
  spin();
  result &= expect(nestedPendingTimer && nestedPendingTimer->isActive(),
                   "restoring motion resumes the resident steering sweep");

  ConversationView view;
  view.setStyle(&normalMotion);
  view.resize(620, 340);
  view.show();
  ConversationGraphSpec snapshot = conversation("reduced-follow", 34);
  QString commandOutput;
  for (int line = 0; line < 80; ++line)
    commandOutput += QStringLiteral("motion output %1\n").arg(line);
  const VisibleCardData command{
      AuthoritativeItemKey{"reduced-follow", "turn-2", "motion-command"},
      CardKind::CommandExecution,
      "reduced-follow",
      "turn-2",
      "motion-command",
      CommandExecutionData{"produce output", utf8(commandOutput), {}, 0, {}},
      nodegraph::NodeStatus::Completed};
  snapshot.sections.back().cards.push_back(command);
  result &= expect(applyConversation(view, snapshot) && view.isAtBottom(),
                   "reduced-motion follow fixture starts at the tail");
  spin();
  ConversationCard *commandCard = card(view, stableKey(command.key));
  result &= expect(setFolded(commandCard, false),
                   "motion-transition command expands in place");
  spin();
  auto *output = commandCard ? commandCard->findChild<CommandOutputView *>(
                                   QStringLiteral("commandOutputView"))
                             : nullptr;
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin();
  if (output)
    output->verticalScrollBar()->triggerAction(
        QAbstractSlider::SliderSingleStepSub);
  spin();
  snapshot.sections.back().cards.push_back(agentCard(
      "reduced-follow", "turn-2", 35, QString(2400, QLatin1Char('x'))));
  result &= expect(applyConversation(view, snapshot),
                   "detached command output retains the outer viewport");
  spin();
  if (output)
    output->verticalScrollBar()->triggerAction(
        QAbstractSlider::SliderToMaximum);
  auto *followMotion = view.findChild<QVariantAnimation *>(
      QString{}, Qt::FindDirectChildrenOnly);
  result &= expect(output && followMotion &&
                       followMotion->state() == QAbstractAnimation::Running,
                   "enabled motion starts follow-tail animation");
  view.setStyle(&reducedMotion);
  spin();
  result &= expect(
      followMotion && followMotion->state() == QAbstractAnimation::Stopped &&
          view.isAtBottom(),
      "a live reduced-motion change settles follow-tail synchronously");

  auto *overlay =
      view.findChild<QWidget *>(QStringLiteral("conversationStagingOverlay"));
  NamedTimerEventProbe spinnerEvents(
      QStringLiteral("conversationSpinnerAnimationTimer"));
  if (overlay)
    overlay->setStyle(&normalMotion);
  view.beginThreadSelection("reduced-loading");
  spin(560);
  const QImage animatedSpinner = overlay ? overlay->grab().toImage() : QImage{};
  spin(70);
  result &= expect(overlay && spinnerEvents.events() > 0 &&
                       overlay->grab().toImage() != animatedSpinner,
                   "enabled motion advances the visible loading ring");
  if (overlay)
    overlay->setStyle(&reducedMotion);
  spin(40);
  const QImage staticSpinner = overlay ? overlay->grab().toImage() : QImage{};
  spinnerEvents.reset();
  spin(110);
  result &= expect(overlay && overlay->isVisible() && !staticSpinner.isNull() &&
                       spinnerEvents.events() == 0 &&
                       overlay->grab().toImage() == staticSpinner,
                   "a live reduced-motion change freezes the visible ring");
  if (overlay)
    overlay->setStyle(&normalMotion);
  spin(40);
  const QImage resumedSpinner = overlay ? overlay->grab().toImage() : QImage{};
  spinnerEvents.reset();
  spin(70);
  result &= expect(overlay && spinnerEvents.events() > 0 &&
                       overlay->grab().toImage() != resumedSpinner,
                   "restoring motion resumes the same visible loading ring");
  ConversationSnapshot rejected =
      projectConversation(conversation("reduced-loading", 2));
  rejected.sections.push_back(rejected.sections.front());
  static_cast<void>(view.reconcileStaged(std::move(rejected)));
  result &=
      expect(spinUntil([&] {
               return overlay && overlay->accessibleName() ==
                                     QStringLiteral("Conversation unavailable");
             }),
             "a rejected staged selection reaches its failed surface");
  spinnerEvents.reset();
  spin(110);
  result &= expect(spinnerEvents.events() == 0,
                   "failed loading leaves no repeating spinner timer work");

  view.beginThreadSelection("completed-loading");
  spin(560);
  spinnerEvents.reset();
  spin(70);
  result &= expect(spinnerEvents.events() > 0,
                   "completion fixture starts the actual spinner timer");
  static_cast<void>(view.reconcileStaged(
      projectConversation(conversation("completed-loading", 2))));
  result &= expect(spinUntil([&] { return overlay && !overlay->isVisible(); }),
                   "a valid staged selection removes its loading surface");
  spinnerEvents.reset();
  spin(110);
  result &= expect(spinnerEvents.events() == 0,
                   "completed loading leaves no repeating spinner timer work");
  return result;
}

bool testMessageImagePresentation() {
  DesktopUrlCapture openedImages;
  ScopedFileUrlHandler fileUrlHandler(openedImages);
  QTemporaryDir directory;
  const QString path = directory.filePath(QStringLiteral("sample.png"));
  const QString portraitPath =
      directory.filePath(QStringLiteral("portrait.png"));
  const QString squarePath = directory.filePath(QStringLiteral("square.png"));
  const QString replacementPath =
      directory.filePath(QStringLiteral("replacement.png"));
  QImage source(640, 360, QImage::Format_ARGB32_Premultiplied);
  source.fill(QColor(QStringLiteral("#2f6feb")));
  QImage portrait(320, 640, QImage::Format_ARGB32_Premultiplied);
  portrait.fill(QColor(QStringLiteral("#6941c6")));
  QImage square(420, 420, QImage::Format_ARGB32_Premultiplied);
  square.fill(QColor(QStringLiteral("#18865e")));
  QImage replacement(500, 260, QImage::Format_ARGB32_Premultiplied);
  replacement.fill(QColor(QStringLiteral("#bc5c32")));
  bool result = expect(
      directory.isValid() && source.save(path) && portrait.save(portraitPath) &&
          square.save(squarePath) && replacement.save(replacementPath),
      "image test fixtures are real readable images");

  VisibleCardData message{
      AuthoritativeItemKey{"images", "turn", "message"},
      CardKind::UserMessage,
      "images",
      "turn",
      "message",
      UserMessageData{"attached images",
                      {utf8(path), utf8(portraitPath), utf8(squarePath)}}};
  auto *card = new ConversationCard(message, false);
  card->resize(430, 400);
  card->show();
  spin();
  auto *ribbon =
      card->findChild<QScrollArea *>(QStringLiteral("messageImages"));
  auto thumbnails =
      card->findChildren<QLabel *>(QStringLiteral("messageImageThumbnail"));
  std::ranges::sort(thumbnails, [ribbon](QLabel *left, QLabel *right) {
    return left->mapTo(ribbon, QPoint{}).x() <
           right->mapTo(ribbon, QPoint{}).x();
  });
  auto *thumbnail = thumbnails.empty() ? nullptr : thumbnails.front();
  result &= expect(
      spinUntil(
          [&] {
            return std::ranges::all_of(thumbnails, [](QLabel *image) {
              return image && !image->pixmap().isNull();
            });
          },
          256),
      "image decoding completes without blocking the GUI event loop");
  const QPixmap thumbnailPixmap = thumbnail ? thumbnail->pixmap() : QPixmap{};
  const auto hasEvenVerticalGap = [ribbon](QLabel *image) {
    if (!ribbon || !image)
      return false;
    const int top = image->mapTo(ribbon->viewport(), QPoint{}).y();
    const int bottom = ribbon->viewport()->height() - top - image->height();
    return std::abs(top - bottom) <= 1;
  };
  const QImage ribbonImage = ribbon ? ribbon->grab().toImage() : QImage{};
  result &= expect(ribbon && thumbnails.size() == 3 && thumbnail,
                   "the image ribbon owns all three thumbnail widgets");
  result &=
      expect(thumbnail && !thumbnailPixmap.isNull() &&
                 thumbnailPixmap.width() <= 280 &&
                 thumbnailPixmap.height() <= 180,
             "the first image is available and canonically bounded");
  const bool thumbnailsCentered =
      ribbon && std::ranges::all_of(thumbnails, hasEvenVerticalGap);
  if (!thumbnailsCentered && ribbon) {
    std::cerr << "image ribbon viewport=" << ribbon->viewport()->size().width()
              << 'x' << ribbon->viewport()->size().height()
              << " outer=" << ribbon->size().width() << 'x'
              << ribbon->size().height()
              << " strip=" << ribbon->widget()->size().width() << 'x'
              << ribbon->widget()->size().height() << " scrollbar="
              << ribbon->horizontalScrollBar()->sizeHint().height() << '/'
              << ribbon->horizontalScrollBar()->height() << " metric="
              << ribbon->style()->pixelMetric(QStyle::PM_ScrollBarExtent)
              << " frame=" << ribbon->frameWidth();
    for (QLabel *image : thumbnails) {
      const int top = image->mapTo(ribbon->viewport(), QPoint{}).y();
      std::cerr << " image=" << image->size().width() << 'x'
                << image->size().height() << " gaps=" << top << '/'
                << ribbon->viewport()->height() - top - image->height();
    }
    std::cerr << '\n';
  }
  result &= expect(thumbnailsCentered,
                   "all thumbnails remain vertically centered in the ribbon");
  result &=
      expect(!ribbonImage.isNull() && ribbonImage.pixelColor(2, 2) ==
                                          QColor(QStringLiteral("#111827")),
             "the ribbon paints the canonical code surface");
  result &= expect(ribbon && thumbnails.size() == 3 &&
                       thumbnails[0]->mapTo(ribbon, QPoint{}).x() <
                           thumbnails[1]->mapTo(ribbon, QPoint{}).x() &&
                       thumbnails[1]->mapTo(ribbon, QPoint{}).x() <
                           thumbnails[2]->mapTo(ribbon, QPoint{}).x(),
                   "image thumbnails preserve their attachment order");
  result &= expect(ribbon && ribbon->horizontalScrollBar()->maximum() > 0 &&
                       ribbon->verticalScrollBar()->maximum() == 0,
                   "the narrow image ribbon scrolls only horizontally");
  result &= expect(ribbon && ribbon->frameWidth() == 1 && ribbon->widget() &&
                       ribbon->widget()->layout() &&
                       ribbon->widget()->layout()->contentsMargins() ==
                           QMargins(4, 4, 4, 4),
                   "the image ribbon has one canonical frame and inset");
  const int narrowRibbonHeight = ribbon ? ribbon->height() : 0;
  card->resize(1000, card->height());
  spin();
  result &= expect(ribbon && ribbon->horizontalScrollBar()->maximum() == 0 &&
                       ribbon->height() < narrowRibbonHeight,
                   "a wide ribbon removes unnecessary horizontal overflow");
  card->resize(430, card->height());
  spin();
  result &= expect(ribbon && ribbon->horizontalScrollBar()->maximum() > 0,
                   "narrowing restores accessible horizontal overflow");
  auto &payload = std::get<UserMessageData>(message.payload);
  payload.text = "attached image with edited text";
  result &= expect(card->applyPresentation(message) != PresentationImpact::None,
                   "message text updates with an unchanged attachment");
  auto *retainedThumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &= expect(retainedThumbnail == thumbnail,
                   "an unchanged attachment retains its decoded thumbnail");
  thumbnail = retainedThumbnail;
  QPointer<QLabel> retainedFirst(thumbnails.at(0));
  QPointer<QLabel> replacedMiddle(thumbnails.at(1));
  QPointer<QLabel> retainedLast(thumbnails.at(2));
  payload.imagePaths.at(1) = utf8(replacementPath);
  result &= expect(card->applyPresentation(message) != PresentationImpact::None,
                   "one changed attachment invalidates card presentation");
  spin();
  auto changedThumbnails =
      card->findChildren<QLabel *>(QStringLiteral("messageImageThumbnail"));
  std::ranges::sort(changedThumbnails, [ribbon](QLabel *left, QLabel *right) {
    return left->mapTo(ribbon, QPoint{}).x() <
           right->mapTo(ribbon, QPoint{}).x();
  });
  result &= expect(changedThumbnails.size() == 3 &&
                       changedThumbnails.at(0) == retainedFirst &&
                       changedThumbnails.at(2) == retainedLast &&
                       replacedMiddle.isNull(),
                   "changing one attachment reconstructs only its thumbnail");
  result &= expect(
      spinUntil(
          [&] {
            return changedThumbnails.size() == 3 &&
                   std::ranges::all_of(changedThumbnails, [](QLabel *image) {
                     return image && !image->pixmap().isNull();
                   });
          },
          256),
      "a changed attachment completes its replacement decode");
  thumbnail = changedThumbnails.empty() ? nullptr : changedThumbnails.front();
  result &= expect(thumbnail && thumbnail->focusPolicy() == Qt::StrongFocus &&
                       !thumbnail->accessibleName().isEmpty(),
                   "available image thumbnails expose a named keyboard target");
  if (thumbnail) {
    QKeyEvent activate(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &activate);
    spin();
  }
  result &= expect(
      openedImages.urls.size() == 1 &&
          openedImages.urls.back() == QUrl::fromLocalFile(path),
      "keyboard activation opens the image with the desktop file handler");

  const QString missingPath = directory.filePath(QStringLiteral("missing.png"));
  QPointer<QLabel> retainedGuard(retainedThumbnail);
  payload.imagePaths = {utf8(missingPath)};
  result &= expect(card->applyPresentation(message) != PresentationImpact::None,
                   "changing the image list invalidates card presentation");
  auto *missingThumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &= expect(
      retainedGuard.isNull() && missingThumbnail &&
          missingThumbnail->pixmap().isNull() &&
          missingThumbnail->text().contains(QStringLiteral("unavailable")) &&
          missingThumbnail->focusPolicy() == Qt::NoFocus &&
          !missingThumbnail->accessibleName().isEmpty(),
      "an unreadable image has a stable restrained placeholder");

  result &= expect(source.save(missingPath),
                   "the missing attachment can be recreated");
  QPointer<QLabel> missingGuard(missingThumbnail);
  payload.text += " after recreation";
  static_cast<void>(card->applyPresentation(message));
  auto *recreatedThumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &= expect(
      spinUntil(
          [&] {
            return missingGuard.isNull() && recreatedThumbnail &&
                   !recreatedThumbnail->pixmap().isNull();
          },
          256),
      "recreating an attachment replaces its placeholder");

  result &= expect(QFile::remove(missingPath),
                   "the recreated attachment can be deleted");
  QPointer<QLabel> recreatedGuard(recreatedThumbnail);
  payload.text += " after deletion";
  static_cast<void>(card->applyPresentation(message));
  auto *deletedThumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &= expect(recreatedGuard.isNull() && deletedThumbnail &&
                       deletedThumbnail->pixmap().isNull(),
                   "deleting an attachment restores its placeholder");

  payload.imagePaths = {utf8(path)};
  static_cast<void>(card->applyPresentation(message));
  thumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  const std::size_t openedBeforeMouse = openedImages.urls.size();
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, local, local,
                      thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &press);
    spin();
  }
  result &= expect(openedImages.urls.size() == openedBeforeMouse,
                   "mouse-down does not invoke the desktop image viewer");
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent release(QEvent::MouseButtonRelease, local, local,
                        thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &release);
    spin();
  }
  delete card;
  spin();
  result &= expect(
      openedImages.urls.size() == openedBeforeMouse + 1 &&
          openedImages.urls.back() == QUrl::fromLocalFile(path),
      "mouse-up invokes the independent desktop image viewer exactly once");
  return result;
}

bool testGeneratedImagePresentationAndGenericBound() {
  DesktopUrlCapture openedImages;
  ScopedFileUrlHandler fileUrlHandler(openedImages);
  QTemporaryDir directory;
  const QString path = directory.filePath(QStringLiteral("generated.png"));
  QImage source(800, 450, QImage::Format_ARGB32_Premultiplied);
  source.fill(QColor(QStringLiteral("#e9f7f0")));
  bool result = expect(directory.isValid() && source.save(path),
                       "generated-image fixture is readable");

  VisibleCardData generated{
      AuthoritativeItemKey{"generated", "turn", "image"},
      CardKind::ImageGeneration,
      "generated",
      "turn",
      "image",
      ImageGenerationData{utf8(path), "A generated UI proposal"},
      nodegraph::NodeStatus::Completed};
  ConversationCard generatedCard(generated, false);
  generatedCard.show();
  spin();
  auto *thumbnail = generatedCard.findChild<QLabel *>(
      QStringLiteral("messageImageThumbnail"));
  result &= expect(
      spinUntil([&] { return thumbnail && !thumbnail->pixmap().isNull(); }, 256),
      "generated-image card reuses the bounded thumbnail");
  QPointer<QLabel> retainedGenerated(thumbnail);
  auto &generatedPayload = std::get<ImageGenerationData>(generated.payload);
  generated.status = nodegraph::NodeStatus::Running;
  generatedPayload.revisedPrompt += " while streaming";
  result &= expect(generatedCard.applyPresentation(generated) !=
                           PresentationImpact::None &&
                       retainedGenerated == thumbnail,
                   "a generated-image status update performs no thumbnail "
                   "rebuild or decode");
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, local, local,
                      thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, local,
                        thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &release);
    spin();
  }
  result &=
      expect(openedImages.urls.size() == 1 &&
                 openedImages.urls.back() == QUrl::fromLocalFile(path),
             "generated-image thumbnail opens the system-default image viewer");

  VisibleCardData viewed{AuthoritativeItemKey{"generated", "turn", "view"},
                         CardKind::ImageGeneration,
                         "generated",
                         "turn",
                         "view",
                         ImageGenerationData{utf8(path), {}}};
  ConversationCard viewedCard(viewed, false);
  auto *viewedThumbnail =
      viewedCard.findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  const bool cacheReadyBeforeEvents =
      viewedThumbnail && !viewedThumbnail->pixmap().isNull();
  viewedCard.show();
  spin();
  const auto viewedLabels = viewedCard.findChildren<QLabel *>();
  result &= expect(
      std::ranges::any_of(viewedLabels,
                          [](QLabel *label) {
                            return label->property("kind").toString() ==
                                       QStringLiteral("title") &&
                                   label->text() == QStringLiteral("Image");
                          }) &&
          std::ranges::any_of(
              viewedLabels,
              [](QLabel *label) {
                return label->objectName() ==
                           QStringLiteral("messageImageThumbnail") &&
                       !label->pixmap().isNull();
              }) &&
          viewedThumbnail && cacheReadyBeforeEvents,
      "plain image-view cards use a neutral title and reuse the cached "
      "thumbnail without decoding");

  VisibleCardData generic{
      AuthoritativeItemKey{"generated", "turn", "unknown"},
      CardKind::GenericActivity,
      "generated",
      "turn",
      "unknown",
      GenericActivityData{"contextCompaction",
                          "type: contextCompaction\nlarge: " +
                              std::string(100000, 'x')}};
  ConversationCard genericCard(generic, false);
  genericCard.show();
  spin();
  auto *details = genericCard.findChild<QLabel *>(
      QStringLiteral("genericActivityMetadata"));
  const auto genericLabels = genericCard.findChildren<QLabel *>();
  result &= expect(
      std::ranges::any_of(genericLabels,
                          [](QLabel *label) {
                            return label->property("kind").toString() ==
                                       QStringLiteral("title") &&
                                   label->text() ==
                                       QStringLiteral("Context compaction");
                          }) &&
          std::get<GenericActivityData>(generic.payload).type ==
              "contextCompaction" &&
          details && details->text().size() < 4200 &&
          details->text().endsWith(
              QStringLiteral("[Activity details truncated]")),
      "protocol labels are humanized while retaining bounded display details");
  auto &genericData = std::get<GenericActivityData>(generic.payload);
  genericData.displayDetail = "field: direct graph detail";
  result &= expect(
      genericCard.applyPresentation(generic) != PresentationImpact::None &&
          details &&
          details->text() == QStringLiteral("field: direct graph detail"),
      "graph generic activity detail renders without JSON "
      "construction");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  using namespace codexui::codex::middle;
  if (qEnvironmentVariableIsSet("CODEXUI_MARKDOWN_SELECTION_TESTS")) {
    bool focused = testUserMessageLineBreakPresentation();
    focused &= testMarkdownSelectionPreservesAuthoredCharacters();
    return focused ? 0 : 1;
  }
  if (qEnvironmentVariableIsSet("CODEXUI_MUTABLE_CARD_TESTS"))
    return testMutableCardsAndCommandOutput() ? 0 : 1;
  if (qEnvironmentVariableIsSet("CODEXUI_FOLLOW_TESTS"))
    return testFollowPauseAndStableAnchor() ? 0 : 1;
  if (qEnvironmentVariableIsSet("CODEXUI_BORDER_TESTS")) {
    bool focused = testActiveWorkBordersFollowStatus();
    focused &= testPendingPromptAnimation();
    return focused ? 0 : 1;
  }
  if (qEnvironmentVariableIsSet("CODEXUI_SETTLEMENT_TESTS")) {
    bool focused = testInitialCommandGeometrySettlement();
    focused &= testRetainedNestedFinalAnswerGeometrySettlement();
    focused &= testBottomAnchoredCommandOutputGrowth();
    if (focused)
      std::cout << "Conversation settlement tests passed\n";
    return focused ? 0 : 1;
  }
  if (qEnvironmentVariableIsSet("CODEXUI_FILE_CHANGES_REFLOW_TESTS"))
    return testPresentationOptionsRetainCardsAndInitialFolding() ? 0 : 1;
  if (qEnvironmentVariableIsSet("CODEXUI_REMAINING_UI_TESTS")) {
    bool focused = testAgentActivityLifecycleLabelRetention();
    focused &= testMarkdownLongLinesWrapInsideMaterializedCards();
    focused &= testMutableCardsAndCommandOutput();
    return focused ? 0 : 1;
  }
  bool result = testPerceptuallyUniformPalette();
  result &= testApplicationStyleSheetContract();
  result &= testMessageIdentityPalette();
  result &= testActiveWorkBordersFollowStatus();
  result &= testStructuralOrderAndIdentity();
  result &= testFollowPauseAndStableAnchor();
  result &= testPausedExpandedCommandStaysPainted();
  result &= testCommandCompletionWithoutGeometryWork();
  result &= testStreamingAgentBecomesVisibleWithoutReselection();
  result &= testThreadLocalScrollState();
  result &= testPromptAdmissionFollowOwnership();
  result &= testCardCopyControls();
  result &= testAgentActivityLifecycleLabelRetention();
  result &= testMarkdownLongLinesWrapInsideMaterializedCards();
  result &= testUserMessageLineBreakPresentation();
  result &= testMarkdownSelectionPreservesAuthoredCharacters();
  result &= testMutableCardsAndCommandOutput();
  result &= testCardFoldingGeometryAndRetention();
  result &= testPresentationOptionsRetainCardsAndInitialFolding();
  result &= testInitialCommandGeometrySettlement();
  result &= testRootlessFinalAnswerGeometrySettlement();
  result &= testRetainedNestedFinalAnswerGeometrySettlement();
  result &= testBottomAnchoredCommandOutputGrowth();
  result &= testCommandOutputStateAcrossNavigation();
  result &= testPendingPromptAnimation();
  result &= testReducedMotionUsesTheQtStyleAuthority();
  result &= testMessageImagePresentation();
  result &= testGeneratedImagePresentationAndGenericBound();
  if (result)
    std::cout << "Conversation card tests passed\n";
  return result ? 0 : 1;
}

#include "ConversationCardsTest.moc"
