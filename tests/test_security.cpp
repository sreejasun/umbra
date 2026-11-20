#include <gtest/gtest.h>

#include <httplib.h>

#include "server/config.hpp"
#include "server/security.hpp"

namespace umbra::server {
namespace {

TEST(RateLimiter, AllowsABurstThenRefills) {
  RateLimiter::Clock::time_point now{};
  RateLimiter limiter(2.0, 3.0, 100, [&] { return now; });
  EXPECT_TRUE(limiter.allow("1.2.3.4"));
  EXPECT_TRUE(limiter.allow("1.2.3.4"));
  EXPECT_TRUE(limiter.allow("1.2.3.4"));
  EXPECT_FALSE(limiter.allow("1.2.3.4")) << "burst of 3 spent";
  EXPECT_TRUE(limiter.allow("5.6.7.8")) << "clients are independent";
  now += std::chrono::milliseconds(500);  // 2 tokens/s -> 1 token
  EXPECT_TRUE(limiter.allow("1.2.3.4"));
  EXPECT_FALSE(limiter.allow("1.2.3.4"));
}

TEST(RateLimiter, ForgetsIdleClientsWhenFull) {
  RateLimiter::Clock::time_point now{};
  RateLimiter limiter(1.0, 1.0, 2, [&] { return now; });
  EXPECT_TRUE(limiter.allow("a"));
  EXPECT_TRUE(limiter.allow("b"));
  now += std::chrono::seconds(5);  // both refilled: idle
  EXPECT_TRUE(limiter.allow("c"));
  EXPECT_LE(limiter.tracked(), 2U);
}

TEST(ClientIp, TrustsOnlyTheProxysOwnEntry) {
  httplib::Request req;
  req.remote_addr = "10.0.0.1";
  req.set_header("X-Forwarded-For", "6.6.6.6, 203.0.113.7");
  EXPECT_EQ(client_ip(req, false), "10.0.0.1") << "not behind a proxy: the header is ignored";
  EXPECT_EQ(client_ip(req, true), "203.0.113.7") << "the client can forge earlier entries";
}

TEST(ContentSecurityPolicy, AllowsNoInlineScriptOrFraming) {
  const std::string& csp = content_security_policy();
  EXPECT_EQ(csp.find("unsafe-inline"), std::string::npos);
  EXPECT_EQ(csp.find("unsafe-eval"), std::string::npos);
  EXPECT_NE(csp.find("frame-ancestors 'none'"), std::string::npos);
}

TEST(Config, FlagsOverrideDefaults) {
  std::vector<std::string> args{"umbra_server", "--port", "9000", "--rate-limit", "0", "--db", "off"};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  const Config cfg = load_config(static_cast<int>(argv.size()), argv.data());
  EXPECT_EQ(cfg.port, 9000);
  EXPECT_DOUBLE_EQ(cfg.rate_limit, 0.0);
  EXPECT_EQ(cfg.database, "off");

  std::vector<std::string> bad{"umbra_server", "--port", "70000"};
  std::vector<char*> bad_argv;
  for (auto& a : bad) bad_argv.push_back(a.data());
  EXPECT_THROW((void)load_config(static_cast<int>(bad_argv.size()), bad_argv.data()), std::invalid_argument);
}

}  // namespace
}  // namespace umbra::server
