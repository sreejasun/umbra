#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace httplib {
class Server;
struct Request;
}  // namespace httplib

namespace umbra::server {

/**
 * Token-bucket rate limiter keyed by client (IP address or user id).
 *
 * Each key earns `rate_per_second` tokens up to `burst`; a request spends one.
 * Thread-safe. Memory is bounded: when more than `max_keys` clients are
 * tracked, buckets that have refilled completely (idle clients) are dropped.
 */
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  using NowFn = std::function<Clock::time_point()>;

  RateLimiter(double rate_per_second, double burst, std::size_t max_keys = 100'000, NowFn now = Clock::now);

  /// Spend one token for `key`; false if the client is over its limit.
  [[nodiscard]] bool allow(const std::string& key);

  [[nodiscard]] std::size_t tracked() const;

 private:
  struct Bucket {
    double tokens;
    Clock::time_point updated;
  };

  double rate_;
  double burst_;
  std::size_t max_keys_;
  NowFn now_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Bucket> buckets_;
};

struct SecurityOptions {
  bool trust_proxy = false;  ///< take the client IP from X-Forwarded-For (behind a reverse proxy)
  bool https = false;        ///< served over HTTPS: send HSTS
};

/// The client's IP address: the socket peer, or with `trust_proxy` the
/// address the reverse proxy appended last to X-Forwarded-For.
[[nodiscard]] std::string client_ip(const httplib::Request& req, bool trust_proxy);

/// Content-Security-Policy for the web UI: only this origin plus the CDNs and
/// map tiles the page actually uses.
[[nodiscard]] const std::string& content_security_policy();

/**
 * Install request limits, per-IP rate limiting for /api/ paths (429 with
 * Retry-After) and security headers on every response (CSP, nosniff,
 * frame-ancestors, referrer and permissions policies, HSTS when on HTTPS).
 * `limiter` (null = no limit) must outlive the server.
 */
void install_security(httplib::Server& server, const SecurityOptions& options, RateLimiter* limiter);

}  // namespace umbra::server
