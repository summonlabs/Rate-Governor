#ifndef RATE_GOVERNOR_ENGINE_LOCK_HPP
#define RATE_GOVERNOR_ENGINE_LOCK_HPP

// Structural enforcement of the "no callback under the state lock" rule.
//
// The engine guards its authoritative state with one mutex and must never run
// embedder code (an event sink) while holding it. That is easy to state and
// easy to violate. ScopedLock makes the rule structural: it records the
// per-thread nesting depth, and an event emitted while any engine lock is held
// on this thread is deferred until the outermost lock is released. A sink may
// therefore re-enter the engine for read-only queries without deadlocking, and
// no future edit can silently reintroduce a callback-under-lock.

#include <mutex>
#include <vector>

#include "rate_governor/engine.hpp"

namespace rate_governor {
namespace detail {

struct EmissionContext {
  int depth{0};
  IEventSink* sink{nullptr};
  std::vector<AuditEvent> pending;
};

inline EmissionContext& emission_context() noexcept {
  static thread_local EmissionContext context;
  return context;
}

inline void flush_pending() {
  EmissionContext& context = emission_context();
  if (context.pending.empty()) {
    context.sink = nullptr;
    return;
  }
  std::vector<AuditEvent> pending;
  pending.swap(context.pending);
  IEventSink* sink = context.sink;
  context.sink = nullptr;
  if (sink == nullptr) {
    return;
  }
  for (const AuditEvent& event : pending) {
    sink->on_event(event);
  }
}

}  // namespace detail

// Emits immediately when no engine lock is held on this thread, otherwise
// defers until the outermost lock is released.
inline void deliver_events(IEventSink* sink, const std::vector<AuditEvent>& events) {
  if (sink == nullptr || events.empty()) {
    return;
  }
  detail::EmissionContext& context = detail::emission_context();
  if (context.depth > 0) {
    context.sink = sink;
    context.pending.insert(context.pending.end(), events.begin(), events.end());
    return;
  }
  for (const AuditEvent& event : events) {
    sink->on_event(event);
  }
}

class ScopedLock {
 public:
  explicit ScopedLock(std::mutex& mutex) : mutex_(mutex) {
    mutex_.lock();
    ++detail::emission_context().depth;
  }

  ~ScopedLock() {
    const bool outermost = (--detail::emission_context().depth == 0);
    mutex_.unlock();
    if (outermost) {
      detail::flush_pending();
    }
  }

  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;
  ScopedLock(ScopedLock&&) = delete;
  ScopedLock& operator=(ScopedLock&&) = delete;

 private:
  std::mutex& mutex_;
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_ENGINE_LOCK_HPP
