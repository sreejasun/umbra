#include "server/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace umbra::server {
namespace {

bool parse_on_off(const std::string& text, const std::string& what) {
  if (text == "on" || text == "1" || text == "true") return true;
  if (text == "off" || text == "0" || text == "false") return false;
  throw std::invalid_argument(what + " must be 'on' or 'off': " + text);
}

int parse_port(const std::string& text) {
  try {
    const int port = std::stoi(text);
    if (port > 0 && port < 65536) return port;
  } catch (const std::exception&) {
  }
  throw std::invalid_argument("invalid port: " + text);
}

double parse_rate(const std::string& text) {
  try {
    const double rate = std::stod(text);
    if (rate >= 0 && rate <= 10'000) return rate;
  } catch (const std::exception&) {
  }
  throw std::invalid_argument("invalid rate limit: " + text);
}

/// One setting: its flag (empty = environment only), environment variable, help text and parser.
struct Option {
  const char* flag;
  const char* env;
  const char* help;
  std::function<void(Config&, const std::string&)> set;
};

const std::vector<Option>& options() {
  static const std::vector<Option> kOptions = {
      {"--data", "UMBRA_DATA", "Overpass JSON road network", [](Config& c, const std::string& v) { c.data = v; }},
      {"--places", "UMBRA_PLACES", "Places for search (optional)", [](Config& c, const std::string& v) { c.places = v; }},
      {"--campus", "UMBRA_CAMPUS", "Campus buildings (optional)", [](Config& c, const std::string& v) { c.campus = v; }},
      {"--water", "UMBRA_WATER", "Drinking-water stops (optional)", [](Config& c, const std::string& v) { c.water = v; }},
      {"--walk", "UMBRA_WALK", "Walking network (optional)", [](Config& c, const std::string& v) { c.walk_data = v; }},
      {"--shade", "UMBRA_SHADE", "Buildings and trees (optional)", [](Config& c, const std::string& v) { c.shade_data = v; }},
      {"--web", "UMBRA_WEB_DIR", "Directory of the web UI", [](Config& c, const std::string& v) { c.web_dir = v; }},
      {"--region", "UMBRA_REGION", "Display name of the area", [](Config& c, const std::string& v) { c.region = v; }},
      {"--timezone", "UMBRA_TIMEZONE", "IANA timezone [America/Phoenix]", [](Config& c, const std::string& v) { c.timezone = v; }},
      {"--host", "UMBRA_HOST", "Bind address [127.0.0.1]", [](Config& c, const std::string& v) { c.host = v; }},
      {"--port", "UMBRA_PORT", "Port [8080] (or $PORT)", [](Config& c, const std::string& v) { c.port = parse_port(v); }},
      {"--https", "UMBRA_HTTPS", "Served over HTTPS: HSTS, Secure cookies [off]",
       [](Config& c, const std::string& v) { c.https = parse_on_off(v, "--https"); }},
      {"--trust-proxy", "UMBRA_TRUST_PROXY", "Client IP from X-Forwarded-For [off]",
       [](Config& c, const std::string& v) { c.trust_proxy = parse_on_off(v, "--trust-proxy"); }},
      {"--rate-limit", "UMBRA_RATE_LIMIT", "API requests/s per IP, 0 = off [20]",
       [](Config& c, const std::string& v) { c.rate_limit = parse_rate(v); }},
      {"--weather", "UMBRA_WEATHER", "Live weather on|off [on]",
       [](Config& c, const std::string& v) { c.weather = parse_on_off(v, "--weather"); }},
      {"--weather-url", "UMBRA_WEATHER_URL", "Open-Meteo forecast endpoint",
       [](Config& c, const std::string& v) { c.weather_url = v; }},
      {"--traffic-url", "UMBRA_TRAFFIC_URL", "HERE Traffic v7 flow endpoint",
       [](Config& c, const std::string& v) { c.traffic_url = v; }},
      {"--model", "UMBRA_XAI_MODEL", "Grok model for the assistant [grok-4.7]",
       [](Config& c, const std::string& v) { c.xai_model = v; }},
      {"--xai-url", "UMBRA_XAI_URL", "xAI Responses API endpoint",
       [](Config& c, const std::string& v) { c.xai_url = v; }},
      {"--db", "UMBRA_DB", "SQLite file for accounts, or 'off' [var/umbra.db]",
       [](Config& c, const std::string& v) { c.database = v; }},
      // Secrets: environment / .env only.
      {"", "HERE_API_KEY", "Enables live traffic", [](Config& c, const std::string& v) { c.here_api_key = v; }},
      {"", "XAI_API_KEY", "Enables the trip assistant", [](Config& c, const std::string& v) { c.xai_api_key = v; }},
  };
  return kOptions;
}

/// Load KEY=VALUE pairs from `path` without overriding real environment variables.
void load_dotenv(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
      value = value.substr(1, value.size() - 2);
    }
    ::setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
  }
}

}  // namespace

Config default_config() {
  Config cfg;
  cfg.data = UMBRA_DEFAULT_DATA_DIR "/tempe.osm.json";
  cfg.places = UMBRA_DEFAULT_DATA_DIR "/tempe.places.json";
  cfg.campus = UMBRA_DEFAULT_DATA_DIR "/tempe.campus.json";
  cfg.water = UMBRA_DEFAULT_DATA_DIR "/tempe.water.json";
  cfg.walk_data = UMBRA_DEFAULT_DATA_DIR "/tempe.walk.osm.json";
  cfg.shade_data = UMBRA_DEFAULT_DATA_DIR "/tempe.shade.json";
  cfg.web_dir = UMBRA_DEFAULT_WEB_DIR;
  return cfg;
}

void print_usage(const char* argv0) {
  std::cout << "Usage: " << argv0 << " [options]\n\nOptions (flag / environment variable):\n";
  for (const Option& o : options()) {
    std::cout << "  " << std::left << std::setw(15) << (o.flag[0] ? o.flag : "") << std::setw(19) << o.env << o.help
              << "\n";
  }
  std::cout << "  -h, --help\n\nSettings are also read from ./.env. API keys are accepted from the environment only.\n";
}

Config load_config(int argc, char** argv) {
  load_dotenv(".env");
  Config cfg = default_config();
  if (const char* port = std::getenv("PORT")) cfg.port = parse_port(port);  // set by hosting platforms
  for (const Option& o : options()) {
    if (const char* v = std::getenv(o.env)) o.set(cfg, v);
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    }
    const auto it = std::find_if(options().begin(), options().end(),
                                 [&](const Option& o) { return o.flag[0] != '\0' && arg == o.flag; });
    if (it == options().end()) throw std::invalid_argument("unknown option: " + arg);
    if (i + 1 >= argc) throw std::invalid_argument("missing value for " + arg);
    it->set(cfg, argv[++i]);
  }
  return cfg;
}

}  // namespace umbra::server
