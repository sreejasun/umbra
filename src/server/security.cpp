#include "server/security.hpp"

#include <algorithm>
#include <stdexcept>

#include <httplib.h>

namespace umbra::server {

// ------------------------------------------------------------- RateLimiter

RateLimiter::RateLimiter(double rate_per_second, double burst, std::size_t max_keys, NowFn now)
    : rate_(rate_per_second), burst_(burst), max_keys_(max_keys), now_(std::move(now)) {
  if (rate_ <= 0 || burst_ < 1) throw std::invalid_argument("RateLimiter: rate must be > 0 and burst >= 1");
  if (!now_) throw std::invalid_argument("RateLimiter: a clock is required");
}

bool RateLimiter::allow(const std::string& key) {
  const std::lock_guard lock(mutex_);
  const Clock::time_point now = now_();
  auto refill = [&](Bucket& b) {
    const double elapsed = std::chrono::duration<double>(now - b.updated).count();
    b.tokens = std::min(burst_, b.tokens + elapsed * rate_);
    b.updated = now;
  };

  if (buckets_.size() >= max_keys_ && !buckets_.contains(key)) {
    std::erase_if(buckets_, [&](auto& entry) {
      refill(entry.second);
      return entry.second.tokens >= burst_;  // idle long enough to be full again
    });
  }

  auto [it, inserted] = buckets_.try_emplace(key, Bucket{burst_, now});
  if (!inserted) refill(it->second);
  if (it->second.tokens < 1.0) return false;
  it->second.tokens -= 1.0;
  return true;
}

std::size_t RateLimiter::tracked() const {
  const std::lock_guard lock(mutex_);
  return buckets_.size();
}

// --------------------------------------------------------------- helpers

std::string client_ip(const httplib::Request& req, bool trust_proxy) {
  if (trust_proxy && req.has_header("X-Forwarded-For")) {
    // Clients can forge earlier entries; the last one was added by our own proxy.
    const std::string forwarded = req.get_header_value("X-Forwarded-For");
    const auto comma = forwarded.rfind(',');
    std::string ip = comma == std::string::npos ? forwarded : forwarded.substr(comma + 1);
    ip.erase(0, ip.find_first_not_of(' '));
    ip.erase(ip.find_last_not_of(' ') + 1);
    if (!ip.empty()) return ip;
  }
  return req.remote_addr;
}

const std::string& content_security_policy() {
  static const std::string kPolicy =
      "default-src 'self'; "
      "script-src 'self' https://unpkg.com; "
      "style-src 'self' https://unpkg.com https://fonts.googleapis.com; "
      "font-src https://fonts.gstatic.com; "
      "img-src 'self' data: https://tile.openstreetmap.org https://unpkg.com; "
      // The service worker fetches the CDN files it caches.
      "connect-src 'self' https://unpkg.com https://fonts.googleapis.com https://fonts.gstatic.com; "
      "worker-src 'self'; manifest-src 'self'; "
      "frame-ancestors 'none'; base-uri 'self'; form-action 'self'; object-src 'none'";
  return kPolicy;
}

void install_security(httplib::Server& server, const SecurityOptions& options, RateLimiter* limiter) {
  // Nothing in the API needs a large body; slow clients must not hold workers forever.
  server.set_payload_max_length(64 * 1024);
  server.set_read_timeout(10, 0);
  server.set_write_timeout(10, 0);
  server.set_keep_alive_timeout(5);

  server.set_pre_routing_handler([options, limiter](const httplib::Request& req, httplib::Response& res) {
    if (limiter != nullptr && req.path.rfind("/api/", 0) == 0 &&
        !limiter->allow(client_ip(req, options.trust_proxy))) {
      res.status = 429;
      res.set_header("Retry-After", "1");
      res.set_content(R"({"error":"Too many requests. Please slow down."})", "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  server.set_post_routing_handler([options](const httplib::Request&, httplib::Response& res) {
    res.set_header("Content-Security-Policy", content_security_policy());
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    // Map tile servers ask for a Referer (OpenStreetMap tile usage policy), so keep the origin.
    res.set_header("Referrer-Policy", "strict-origin-when-cross-origin");
    // Microphone: only for the opt-in danger-phrase listener (speech recognition).
    res.set_header("Permissions-Policy", "geolocation=(self), microphone=(self), camera=(), payment=()");
    res.set_header("Cross-Origin-Opener-Policy", "same-origin");
    if (options.https) res.set_header("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
  });
}

}  // namespace umbra::server
