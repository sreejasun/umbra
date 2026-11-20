#pragma once

#include <filesystem>
#include <string>

namespace umbra::server {

/**
 * Server configuration. Precedence: command-line flag > environment variable >
 * `.env` file > default. Secrets (API keys) are read from the environment or
 * `.env` only, never from flags, which other users can see in the process list.
 */
struct Config {
  // Network
  std::string host = "127.0.0.1";
  int port = 8080;
  bool https = false;        ///< behind HTTPS: HSTS header, Secure cookies
  bool trust_proxy = false;  ///< client IP from X-Forwarded-For (behind a reverse proxy)
  double rate_limit = 20.0;  ///< /api requests per second per IP; 0 disables

  // Data
  std::filesystem::path data;
  std::filesystem::path places;
  std::filesystem::path campus;
  std::filesystem::path water;
  std::filesystem::path walk_data;
  std::filesystem::path shade_data;
  std::filesystem::path web_dir;
  std::string region = "Tempe, Arizona";
  std::string timezone = "America/Phoenix";

  // Live services
  bool weather = true;
  std::string weather_url = "https://api.open-meteo.com/v1/forecast";
  std::string here_api_key;  ///< HERE_API_KEY: enables live traffic
  std::string traffic_url = "https://data.traffic.hereapi.com/v7/flow";
  std::string xai_api_key;   ///< XAI_API_KEY: enables the trip assistant
  std::string xai_model = "grok-4.7";
  std::string xai_url = "https://api.x.ai/v1/responses";

  // Accounts
  std::string database = "var/umbra.db";  ///< SQLite file; "off" disables accounts
};

/// Defaults that depend on the source tree (data and web directories).
[[nodiscard]] Config default_config();

/// Parse flags and environment (after loading `.env` from the working directory).
/// Throws std::invalid_argument for bad input. `--help` prints usage and exits.
[[nodiscard]] Config load_config(int argc, char** argv);

void print_usage(const char* argv0);

}  // namespace umbra::server
