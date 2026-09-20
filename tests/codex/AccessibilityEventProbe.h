// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_TESTS_ACCESSIBILITYEVENTPROBE_H
#define CODEXUI_TESTS_ACCESSIBILITYEVENTPROBE_H

#include <QAccessible>
#include <QAccessibleInterface>
#include <QList>
#include <QPointer>

#include <functional>
#include <utility>

namespace codexui::tests {

#if QT_CONFIG(accessibility)

struct AccessibilityEventRecord final {
  QAccessible::Event type = QAccessible::InvalidEvent;
  QAccessible::Id target = 0;
  QPointer<QObject> object;
  QAccessible::Role role = QAccessible::NoRole;
  QAccessible::State changedStates;
  QAccessible::State state;
  QString name;
  QString description;
};

class AccessibilityEventProbe final {
public:
  explicit AccessibilityEventProbe(
      std::function<void(const AccessibilityEventRecord &)> observer = {})
      : previous_(QAccessible::installUpdateHandler(dispatch)),
        observer_(std::move(observer)) {
    // Do not force QAccessible active here. Before Qt 6.10 setActive() only
    // notified activation observers and did not portably activate the platform
    // backend. This probe is the deterministic seam for explicit app events;
    // backend-native event qualification belongs to the platform test matrix.
    active_ = this;
  }

  ~AccessibilityEventProbe() {
    active_ = nullptr;
    QAccessible::installUpdateHandler(previous_);
  }

  AccessibilityEventProbe(const AccessibilityEventProbe &) = delete;
  AccessibilityEventProbe &operator=(const AccessibilityEventProbe &) = delete;

  void clear() { events_.clear(); }

  [[nodiscard]] const QList<AccessibilityEventRecord> &all() const noexcept {
    return events_;
  }

  [[nodiscard]] QList<AccessibilityEventRecord>
  events(QAccessible::Id target, QAccessible::Event type) const {
    QList<AccessibilityEventRecord> result;
    for (const AccessibilityEventRecord &event : events_)
      if (event.target == target && event.type == type)
        result.push_back(event);
    return result;
  }

  [[nodiscard]] QList<AccessibilityEventRecord>
  events(QAccessible::Id target) const {
    QList<AccessibilityEventRecord> result;
    for (const AccessibilityEventRecord &event : events_)
      if (event.target == target)
        result.push_back(event);
    return result;
  }

private:
  static void dispatch(QAccessibleEvent *event) {
    if (active_)
      active_->record(event);
    if (active_ && active_->previous_)
      active_->previous_(event);
  }

  void record(QAccessibleEvent *event) {
    QAccessibleInterface *interface = event->accessibleInterface();
    AccessibilityEventRecord record;
    record.type = event->type();
    record.target = interface ? QAccessible::uniqueId(interface) : 0;
    record.object = interface ? interface->object() : event->object();
    if (interface) {
      record.role = interface->role();
      record.name = interface->text(QAccessible::Name);
      record.description = interface->text(QAccessible::Description);
      if (event->type() == QAccessible::StateChanged ||
          event->type() == QAccessible::Selection ||
          event->type() == QAccessible::SelectionAdd ||
          event->type() == QAccessible::SelectionRemove ||
          event->type() == QAccessible::Focus)
        record.state = interface->state();
    }
    if (event->type() == QAccessible::StateChanged)
      record.changedStates =
          static_cast<QAccessibleStateChangeEvent *>(event)->changedStates();
    events_.push_back(std::move(record));
    if (observer_)
      observer_(events_.back());
  }

  inline static AccessibilityEventProbe *active_ = nullptr;
  QAccessible::UpdateHandler previous_ = nullptr;
  QList<AccessibilityEventRecord> events_;
  std::function<void(const AccessibilityEventRecord &)> observer_;
};

#endif

} // namespace codexui::tests

#endif
