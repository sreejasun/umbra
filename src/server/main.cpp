// Umbra HTTP server: loads the OpenStreetMap extracts, starts the optional live
// services (weather, traffic, accounts, assistant) and serves the JSON API and
// the web UI. See `umbra_server --help` and docs/DEPLOYMENT.md.

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <httplib.h>

#include "server/assistant.hpp"
#include "server/config.hpp"
#include "server/http_api.hpp"
#include "server/http_client.hpp"
#include "server/security.hpp"
#include "umbra/osm_loader.hpp"
#include "umbra/route_service.hpp"
#include "umbra/traffic.hpp"
#include "umbra/user_store.hpp"
#include "umbra/walk_planner.hpp"
#include "umbra/weather.hpp"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

long long ms_since(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

/// Load an optional place file; a broken file is a warning, not a failure.
std::vector<umbra::Place> load_optional(const fs::path& path, const char* what, const char* script,
                                        std::vector<umbra::Place> (*load)(const fs::path&)) {
  if (path.empty() || !fs::exists(path)) {
    std::cout << "  (no " << what << " file; run " << script << ")\n";
    return {};
  }
  try {
    return load(path);
  } catch (const std::exception& e) {
    std::cerr << "warning: ignoring " << what << " file: " << e.what() << "\n";
    return {};
  }
}

std::unique_ptr<umbra::WalkPlanner> load_walking(const umbra::server::Config& cfg,
                                                 std::unique_ptr<umbra::RouteService>& walk_service) {
  if (!fs::exists(cfg.walk_data) || !fs::exists(cfg.shade_data)) {
    std::cout << "  (no walking/shade data; run scripts/fetch_walk.sh to enable shade-aware walking)\n";
    return nullptr;
  }
  try {
    const auto t0 = Clock::now();
    umbra::OsmLoadOptions options;
    options.mode = umbra::TravelMode::kWalk;
    umbra::RouteServiceOptions service_options;
    service_options.max_snap_distance_m = 400.0;
    service_options.unnamed_road = "a walkway";
    service_options.unnamed_place = "Walkway";
    walk_service = std::make_unique<umbra::RouteService>(umbra::load_overpass_file(cfg.walk_data, options),
                                                         std::vector<umbra::Place>{}, service_options);
    auto shade = std::make_shared<const umbra::ShadeMap>(umbra::load_shade_file(cfg.shade_data));

    std::vector<umbra::WalkStop> stops;
    for (umbra::Place& p : load_optional(cfg.water, "drinking-water", "scripts/fetch_water.sh", umbra::load_water_file)) {
      stops.push_back({std::move(p.name), p.position});
    }
    auto planner = std::make_unique<umbra::WalkPlanner>(*walk_service, shade, std::move(stops));
    std::cout << "  walking:     " << walk_service->graph().node_count() << " nodes, " << shade->building_count()
              << " buildings, " << shade->tree_count() << " trees, " << planner->stop_count() << " water stops ("
              << ms_since(t0) << " ms)\n";
    return planner;
  } catch (const std::exception& e) {
    std::cerr << "warning: walking mode disabled: " << e.what() << "\n";
    walk_service.reset();
    return nullptr;
  }
}

}  // namespace

int main(int argc, char** argv) {
  namespace srv = umbra::server;
  srv::Config cfg;
  try {
    cfg = srv::load_config(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n\n";
    srv::print_usage(argv[0]);
    return 2;
  }

  // ---------------------------------------------------------------- map data
  std::cout << "Loading road network from " << cfg.data << " ...\n";
  const auto t0 = Clock::now();
  umbra::OsmLoadStats stats;
  std::unique_ptr<umbra::RouteService> service;
  std::size_t campus_buildings = 0;
  try {
    umbra::RoadGraph road_graph = umbra::load_overpass_file(cfg.data, {}, &stats);
    std::vector<umbra::Place> buildings =
        load_optional(cfg.campus, "campus", "scripts/fetch_campus.sh", umbra::load_campus_file);
    campus_buildings = buildings.size();
    // Campus buildings replace same-named entries from the general places file.
    std::vector<umbra::Place> places = umbra::merge_places(
        std::move(buildings), load_optional(cfg.places, "places", "scripts/fetch_places.sh", umbra::load_places_file));
    service = std::make_unique<umbra::RouteService>(std::move(road_graph), std::move(places));
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\nhint: download map data first with  scripts/fetch_osm.sh\n";
    return 1;
  }
  const umbra::RoadGraph& graph = service->graph();
  std::cout << "  ways used:   " << stats.ways_used << " / " << stats.ways_seen << "\n"
            << "  nodes:       " << graph.node_count() << " (largest strongly connected component of "
            << stats.nodes_before_pruning << ")\n"
            << "  edges:       " << graph.edge_count() << "\n"
            << "  searchable:  " << service->searchable_count() << " places, streets and intersections ("
            << campus_buildings << " campus buildings)\n"
            << "  loaded in    " << ms_since(t0) << " ms\n";

  std::unique_ptr<umbra::RouteService> walk_service;
  const std::unique_ptr<umbra::WalkPlanner> walk_planner = load_walking(cfg, walk_service);

  // ----------------------------------------------------------- live services
  // Only the map's centre / bounding box is sent upstream, never a user's position.
  srv::http_client_global_init();
  const bool online = srv::http_client_available();
  const umbra::HttpGet get = [](const std::string& url) { return srv::http_get(url, std::chrono::seconds(8)); };

  std::unique_ptr<umbra::WeatherService> weather;
  if (cfg.weather && online) {
    weather = std::make_unique<umbra::WeatherService>(graph.bounds().center(), get,
                                                      umbra::WeatherServiceOptions{cfg.weather_url, {}});
    std::cout << "  weather:     live from Open-Meteo (refreshed every 10 min)\n";
  }

  std::unique_ptr<umbra::TrafficService> traffic;
  if (!cfg.here_api_key.empty() && online) {
    umbra::TrafficServiceOptions options;
    options.base_url = cfg.traffic_url;
    traffic = std::make_unique<umbra::TrafficService>(graph, cfg.here_api_key, get, options);
    std::cout << "  traffic:     live from HERE (refreshed every 15 min)\n";
  } else {
    std::cout << "  (no live traffic; set HERE_API_KEY to enable it)\n";
  }

  std::unique_ptr<umbra::UserStore> users;
  if (cfg.database != "off") {
    try {
      const fs::path db(cfg.database);
      if (db.has_parent_path()) fs::create_directories(db.parent_path());
      users = std::make_unique<umbra::UserStore>(cfg.database);
      std::cout << "  accounts:    " << cfg.database << "\n";
    } catch (const std::exception& e) {
      std::cerr << "warning: accounts disabled: " << e.what() << "\n";
    }
  }
  if (!online) std::cout << "  (built without libcurl: no live weather, traffic or assistant)\n";

  srv::ApiContext ctx{*service, walk_planner.get(), weather.get(), traffic.get(), users.get(), nullptr,
                      {cfg.region, cfg.timezone}, cfg.https, cfg.trust_proxy};

  std::unique_ptr<srv::TripAssistant> assistant;
  if (!cfg.xai_api_key.empty() && online) {
    const umbra::HttpPost post = [](const std::string& url, const std::string& body,
                                    const std::vector<std::string>& headers) {
      return srv::http_post(url, body, headers, std::chrono::seconds(60));
    };
    srv::AssistantOptions options;
    options.model = cfg.xai_model;
    options.url = cfg.xai_url;
    assistant = std::make_unique<srv::TripAssistant>(post, cfg.xai_api_key, srv::trip_tools(ctx), options);
    ctx.assistant = assistant.get();
    std::cout << "  assistant:   " << cfg.xai_model << " via the xAI API\n";
  } else {
    std::cout << "  (no trip assistant; set XAI_API_KEY to enable it)\n";
  }

  // -------------------------------------------------------------------- HTTP
  httplib::Server server;
  std::unique_ptr<srv::RateLimiter> limiter;
  if (cfg.rate_limit > 0) limiter = std::make_unique<srv::RateLimiter>(cfg.rate_limit, cfg.rate_limit * 4);
  // Bursts of 4 s worth of requests: search-as-you-type sends several at once.
  srv::install_security(server, {.trust_proxy = cfg.trust_proxy, .https = cfg.https}, limiter.get());
  srv::register_api(server, ctx);

  // Types the installable web app needs that cpp-httplib does not know.
  server.set_file_extension_and_mimetype_mapping("webmanifest", "application/manifest+json");
  if (!server.set_mount_point("/", cfg.web_dir.string())) {
    std::cerr << "error: web directory not found: " << cfg.web_dir << "\n";
    return 1;
  }

  server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
    try {
      if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      std::cerr << "unhandled exception: " << e.what() << "\n";
    } catch (...) {
    }
    res.status = 500;
    res.set_content(R"({"error":"Internal server error"})", "application/json");
  });

  // Access log without query strings: they can contain positions.
  server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
    if (req.path.rfind("/api/", 0) == 0) std::cout << req.method << " " << req.path << " -> " << res.status << "\n";
  });

  std::cout << "\nUmbra running at http://" << cfg.host << ":" << cfg.port << "\nPress Ctrl+C to stop.\n"
            << std::flush;
  const bool listened = server.listen(cfg.host, cfg.port);
  srv::http_client_global_cleanup();
  if (!listened) {
    std::cerr << "error: could not bind to " << cfg.host << ":" << cfg.port << "\n";
    return 1;
  }
  return 0;
}
