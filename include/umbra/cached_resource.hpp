#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace umbra {

/// When a CachedResource refreshes, backs off and gives up.
struct CachePolicy {
  std::chrono::seconds refresh_after{10 * 60};  ///< refetch at most this often
  std::chrono::seconds retry_after{60};         ///< wait this long after a failed fetch
  std::chrono::seconds max_age{3 * 60 * 60};    ///< stop serving data older than this
};

/**
 * A value fetched from an upstream service on demand and shared by all
 * callers (live weather, live traffic).
 *
 * Thread-safe. At most one fetch runs at a time; a failing upstream is retried
 * no more than once per `retry_after`; after a failed refresh the previous
 * value is served until it is `max_age` old. Callers get a shared_ptr to an
 * immutable value, so a refresh never changes data a request is using. The
 * fetch function and the clock are injected, so tests need no network or sleep.
 */
template <typename T>
class CachedResource {
 public:
  using Clock = std::chrono::steady_clock;
  using NowFn = std::function<Clock::time_point()>;
  using FetchFn = std::function<T()>;  ///< download and parse; throws on failure

  explicit CachedResource(FetchFn fetch, CachePolicy policy = {}, NowFn now = Clock::now)
      : fetch_(std::move(fetch)), policy_(policy), now_(std::move(now)) {
    if (!fetch_) throw std::invalid_argument("CachedResource: a fetch function is required");
    if (!now_) throw std::invalid_argument("CachedResource: a clock is required");
  }

  /// Current value, or null if none could be fetched recently.
  [[nodiscard]] std::shared_ptr<const T> get() const {
    const std::lock_guard lock(mutex_);
    const Clock::time_point now = now_();
    const bool fresh = fetched_at_ && now - *fetched_at_ < policy_.refresh_after;
    const bool backing_off = failed_at_ && now - *failed_at_ < policy_.retry_after;
    if (!fresh && !backing_off) {
      try {
        value_ = std::make_shared<const T>(fetch_());
        fetched_at_ = now;
        failed_at_.reset();
      } catch (const std::exception&) {
        failed_at_ = now;  // keep serving the previous value while it is not too old
      }
    }
    if (fetched_at_ && now - *fetched_at_ > policy_.max_age) return nullptr;
    return value_;
  }

 private:
  FetchFn fetch_;
  CachePolicy policy_;
  NowFn now_;

  mutable std::mutex mutex_;
  mutable std::shared_ptr<const T> value_;
  mutable std::optional<Clock::time_point> fetched_at_;
  mutable std::optional<Clock::time_point> failed_at_;
};

}  // namespace umbra
