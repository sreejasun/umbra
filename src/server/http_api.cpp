// JSON HTTP API. See docs/API.md for every endpoint, parameter and response.
//
// Trips:    GET  /api/health, /api/info, /api/nearest, /api/search, /api/route, /api/walk,
//                /api/walk/departures, /api/weather, /api/sun, /api/shadows
// Accounts: POST /api/auth/register, /api/auth/login, /api/auth/logout
//           GET/DELETE /api/me, GET /api/me/export, GET/POST /api/me/places,
//           DELETE /api/me/places/{id}, GET/POST/DELETE /api/me/trips
// Assistant: POST /api/assistant

#include "server/http_api.hpp"

#include <memory>

#include <httplib.h>

#include "server/assistant.hpp"
#include "server/security.hpp"
#include "umbra/traffic.hpp"
#include "umbra/user_store.hpp"
#include "umbra/weather.hpp"

namespace umbra::server {
namespace {

constexpr const char* kSessionCookie = "umbra_session";

TripEnd trip_end(const httplib::Request& req, const char* points_key, const char* name_key) {
  if (!req.has_param(points_key)) throw BadRequest(std::string("Missing parameter '") + points_key + "'");
  return {parse_points(req.get_param_value(points_key), points_key), checked_name(param(req, name_key), name_key)};
}

std::int64_t time_or_now(const httplib::Request& req) {
  return req.has_param("time") ? parse_unix_time(req.get_param_value("time")) : now_unix();
}

bool live_param(const httplib::Request& req, const char* key) {
  const std::string v = param(req, key, "live");
  if (v != "live" && v != "off") throw BadRequest(std::string("'") + key + "' must be 'live' or 'off'");
  return v == "live";
}

WalkQuery walk_params(const httplib::Request& req) {
  WalkQuery q;
  q.from = trip_end(req, "from", "fromName");
  q.to = trip_end(req, "to", "toName");
  q.options.unix_time = time_or_now(req);
  q.options.preference = parse_shade(param(req, "shade"));
  q.options.algorithm = parse_algorithm(param(req, "algorithm"));
  q.options.record_settled = flag_param(req, "explored", true);
  q.options.avoid_stairs = flag_param(req, "avoidStairs", false);
  q.options.prefer_lit = flag_param(req, "preferLit", true);
  q.live_weather = live_param(req, "weather");
  q.via_water = flag_param(req, "viaWater", false);
  q.alternatives = flag_param(req, "alternatives", false);
  return q;
}

// ------------------------------------------------------------ accounts

std::string session_token(const httplib::Request& req) {
  const std::string cookies = req.get_header_value("Cookie");
  const std::string prefix = std::string(kSessionCookie) + "=";
  for (std::size_t start = 0; start < cookies.size();) {
    std::size_t end = cookies.find(';', start);
    if (end == std::string::npos) end = cookies.size();
    std::string item = cookies.substr(start, end - start);
    item.erase(0, item.find_first_not_of(' '));
    if (item.rfind(prefix, 0) == 0) return item.substr(prefix.size());
    start = end + 1;
  }
  return {};
}

std::string session_cookie(const std::string& token, std::int64_t max_age, bool https) {
  std::string cookie = std::string(kSessionCookie) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                       std::to_string(max_age);
  if (https) cookie += "; Secure";
  return cookie;
}

/// State-changing requests must carry a custom header. Browsers refuse to send
/// one cross-site without a CORS preflight (which this server never grants),
/// so together with SameSite=Strict cookies this stops cross-site request forgery.
void require_same_origin(const httplib::Request& req) {
  if (req.get_header_value("X-Umbra-Request") != "1") throw BadRequest("Missing X-Umbra-Request header");
}

json body_json(const httplib::Request& req) {
  json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
  if (!body.is_object()) throw BadRequest("Request body must be a JSON object");
  return body;
}

std::string body_string(const json& body, const char* key) {
  const auto it = body.find(key);
  return it != body.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

LatLng body_point(const json& body, const char* key) {
  const auto it = body.find(key);
  if (it == body.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) {
    throw BadRequest(std::string("'") + key + "' must be [lat, lng]");
  }
  return {(*it)[0].get<double>(), (*it)[1].get<double>()};
}

json user_json(const User& u) {
  return {{"id", u.id}, {"email", u.email}, {"name", u.display_name}, {"createdAt", u.created_at}};
}

json place_json(const SavedPlace& p) {
  return {{"id", p.id}, {"name", p.name}, {"position", to_json(p.position)}, {"createdAt", p.created_at}};
}

json trip_json(const TripRecord& t) {
  return {{"id", t.id},           {"mode", t.mode},           {"fromName", t.from_name}, {"from", to_json(t.from)},
          {"toName", t.to_name}, {"to", to_json(t.to)}, {"createdAt", t.created_at}};
}

void register_account_routes(httplib::Server& server, const ApiContext& ctx) {
  UserStore& store = *ctx.users;
  // Password guessing: 5 attempts, then one every 12 s, per IP (Argon2id is also deliberately slow).
  auto auth_limiter = std::make_shared<RateLimiter>(1.0 / 12.0, 5.0);
  const bool https = ctx.https;
  const bool trust_proxy = ctx.trust_proxy;

  auto signed_in = [&store](const httplib::Request& req) {
    const auto user = store.user_for_token(session_token(req));
    if (!user) throw Unauthorized("Please sign in");
    return *user;
  };
  auto limit_auth = [auth_limiter, trust_proxy](const httplib::Request& req) {
    if (!auth_limiter->allow(client_ip(req, trust_proxy))) {
      throw TooManyRequests("Too many attempts. Wait a minute and try again.");
    }
  };
  auto start_session = [https](httplib::Response& res, const Session& s) {
    res.set_header("Set-Cookie", session_cookie(s.token, UserStore::kSessionSeconds, https));
    send_json(res, {{"user", user_json(s.user)}});
  };

  server.Post("/api/auth/register", guarded([&store, limit_auth, start_session](const httplib::Request& req,
                                                                               httplib::Response& res) {
    require_same_origin(req);
    limit_auth(req);
    const json body = body_json(req);
    const std::string email = body_string(body, "email");
    const std::string password = body_string(body, "password");
    (void)store.create_user(email, password, body_string(body, "name"));
    start_session(res, store.login(email, password));
  }));

  server.Post("/api/auth/login", guarded([&store, limit_auth, start_session](const httplib::Request& req,
                                                                            httplib::Response& res) {
    require_same_origin(req);
    limit_auth(req);
    const json body = body_json(req);
    start_session(res, store.login(body_string(body, "email"), body_string(body, "password")));
  }));

  server.Post("/api/auth/logout", guarded([&store, https](const httplib::Request& req, httplib::Response& res) {
    require_same_origin(req);
    store.logout(session_token(req));
    res.set_header("Set-Cookie", session_cookie("", 0, https));
    send_json(res, {{"ok", true}});
  }));

  // Signed out is a normal state for the UI, not an error.
  server.Get("/api/me", guarded([&store](const httplib::Request& req, httplib::Response& res) {
    const auto user = store.user_for_token(session_token(req));
    send_json(res, {{"user", user ? user_json(*user) : json(nullptr)}});
  }));

  server.Delete("/api/me", guarded([&store, signed_in, https](const httplib::Request& req, httplib::Response& res) {
    require_same_origin(req);
    store.delete_user(signed_in(req).id);
    res.set_header("Set-Cookie", session_cookie("", 0, https));
    send_json(res, {{"deleted", true}});
  }));

  server.Get("/api/me/export", guarded([&store, signed_in](const httplib::Request& req, httplib::Response& res) {
    const User user = signed_in(req);
    json places = json::array();
    for (const SavedPlace& p : store.places(user.id)) places.push_back(place_json(p));
    json trips = json::array();
    for (const TripRecord& t : store.trips(user.id)) trips.push_back(trip_json(t));
    res.set_header("Content-Disposition", "attachment; filename=\"umbra-export.json\"");
    send_json(res, {{"user", user_json(user)}, {"places", places}, {"trips", trips}});
  }));

  server.Get("/api/me/places", guarded([&store, signed_in](const httplib::Request& req, httplib::Response& res) {
    json places = json::array();
    for (const SavedPlace& p : store.places(signed_in(req).id)) places.push_back(place_json(p));
    send_json(res, {{"places", places}});
  }));

  server.Post("/api/me/places", guarded([&store, signed_in](const httplib::Request& req, httplib::Response& res) {
    require_same_origin(req);
    const User user = signed_in(req);
    const json body = body_json(req);
    send_json(res, place_json(store.add_place(user.id, body_string(body, "name"), body_point(body, "position"))), 201);
  }));

  server.Delete(R"(/api/me/places/(\d+))", guarded([&store, signed_in](const httplib::Request& req,
                                                                     httplib::Response& res) {
    require_same_origin(req);
    store.delete_place(signed_in(req).id, std::stoll(req.matches[1].str()));
    send_json(res, {{"deleted", true}});
  }));

  server.Get("/api/me/trips", guarded([&store, signed_in](const httplib::Request& req, httplib::Response& res) {
    json trips = json::array();
    for (const TripRecord& t : store.trips(signed_in(req).id)) trips.push_back(trip_json(t));
    send_json(res, {{"trips", trips}});
  }));

  server.Post("/api/me/trips", guarded([&store, signed_in](const httplib::Request& req, httplib::Response& res) {
    require_same_origin(req);
    const User user = signed_in(req);
    const json body = body_json(req);
    TripRecord trip;
    trip.mode = body_string(body, "mode");
    trip.from_name = body_string(body, "fromName");
    trip.from = body_point(body, "from");
    trip.to_name = body_string(body, "toName");
    trip.to = body_point(body, "to");
    store.add_trip(user.id, trip);
    send_json(res, {{"saved", true}}, 201);
  }));

  server.Delete("/api/me/trips", guarded([&store, signed_in](const httplib::Request& req, httplib::Response& res) {
    require_same_origin(req);
    store.clear_trips(signed_in(req).id);
    send_json(res, {{"deleted", true}});
  }));
}

// ----------------------------------------------------------- assistant

void register_assistant_routes(httplib::Server& server, const ApiContext& ctx) {
  // Each question costs model tokens: 20 an hour per account (burst of 5).
  auto limiter = std::make_shared<RateLimiter>(20.0 / 3600.0, 5.0);
  server.Post("/api/assistant", guarded([&ctx, limiter](const httplib::Request& req, httplib::Response& res) {
    require_same_origin(req);
    std::string who = client_ip(req, ctx.trust_proxy);
    if (ctx.users != nullptr) {
      const auto user = ctx.users->user_for_token(session_token(req));
      if (!user) throw Unauthorized("Sign in to use the trip assistant");
      who = "user:" + std::to_string(user->id);
    }
    if (!limiter->allow(who)) throw TooManyRequests("You have asked a lot of questions. Try again later.");

    const json body = body_json(req);
    const std::string question = body_string(body, "question");
    if (question.empty() || question.size() > 500) throw BadRequest("'question' must be 1-500 characters");
    std::string context = "Current time: " + std::to_string(now_unix()) + " (Unix seconds); timezone " +
                          ctx.region.timezone + "; map area: " + ctx.region.name + ".";
    if (body.contains("position")) {
      const LatLng me = body_point(body, "position");
      context += " The user is at " + std::to_string(me.lat) + "," + std::to_string(me.lng) + ".";
    }
    try {
      const AssistantAnswer answer = ctx.assistant->ask(question, context);
      send_json(res, {{"answer", answer.text}, {"route", answer.route}, {"toolsUsed", answer.tools_used}});
    } catch (const AssistantError& e) {
      send_error(res, 502, e.what());
    }
  }));
}

}  // namespace

void register_api(httplib::Server& server, const ApiContext& ctx) {
  const RouteService& service = ctx.drive;
  const RoadGraph& graph = service.graph();

  server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    send_json(res, {{"status", "ok"}});
  });

  server.Get("/api/info", [&ctx, &graph, &service](const httplib::Request&, httplib::Response& res) {
    const BoundingBox& b = graph.bounds();
    json walking = nullptr;
    if (ctx.walk != nullptr) {
      walking = {{"nodes", ctx.walk->network().graph().node_count()},
                 {"edges", ctx.walk->network().graph().edge_count()},
                 {"buildings", ctx.walk->shade_map().building_count()},
                 {"trees", ctx.walk->shade_map().tree_count()},
                 {"waterStops", ctx.walk->stop_count()}};
    }
    send_json(res, {{"region", {{"name", ctx.region.name},
                                {"timezone", ctx.region.timezone},
                                {"center", to_json(b.center())},
                                {"bounds", json::array({to_json(LatLng{b.min_lat, b.min_lng}),
                                                        to_json(LatLng{b.max_lat, b.max_lng})})}}},
                    {"graph", {{"nodes", graph.node_count()},
                               {"edges", graph.edge_count()},
                               {"maxSpeedKmh", round_to(graph.max_speed_mps() * 3.6, 1)},
                               {"searchable", service.searchable_count()}}},
                    {"walking", walking},
                    {"features", {{"weather", ctx.weather != nullptr},
                                  {"traffic", ctx.traffic != nullptr},
                                  {"accounts", ctx.users != nullptr},
                                  {"assistant", ctx.assistant != nullptr}}},
                    {"attribution", "Map data © OpenStreetMap contributors (ODbL); weather by Open-Meteo.com "
                                    "(CC BY 4.0); traffic © HERE"}});
  });

  server.Get("/api/nearest", guarded([&service](const httplib::Request& req, httplib::Response& res) {
    const auto lat = parse_double(param(req, "lat"));
    const auto lng = parse_double(param(req, "lng"));
    if (!lat || !lng) throw BadRequest("Parameters 'lat' and 'lng' are required numbers");
    send_json(res, to_json(service.snap({*lat, *lng})));
  }));

  server.Get("/api/search", guarded([&ctx](const httplib::Request& req, httplib::Response& res) {
    const auto limit = parse_double(param(req, "limit", "8"));
    if (!limit) throw BadRequest("'limit' must be 1-20");
    send_json(res, search_query(ctx, param(req, "q"), static_cast<std::size_t>(*limit)));
  }));

  server.Get("/api/route", guarded([&ctx](const httplib::Request& req, httplib::Response& res) {
    DriveQuery q;
    q.from = trip_end(req, "from", "fromName");
    q.to = trip_end(req, "to", "toName");
    q.metric = parse_metric(param(req, "metric"));
    q.algorithm = parse_algorithm(param(req, "algorithm"));
    q.explored = flag_param(req, "explored", true);
    q.live_traffic = live_param(req, "traffic");
    q.live_weather = live_param(req, "weather");
    q.unix_time = time_or_now(req);
    q.avoid_glare = flag_param(req, "avoidGlare", false);
    q.alternatives = flag_param(req, "alternatives", false);
    if (req.has_param("glareMaxElevation")) {
      const auto deg = parse_double(req.get_param_value("glareMaxElevation"));
      if (!deg || *deg < 5 || *deg > 60) throw BadRequest("'glareMaxElevation' must be 5-60 degrees");
      q.glare_max_elevation_deg = *deg;
    }
    send_json(res, drive_query(ctx, q));
  }));

  server.Get("/api/walk", guarded([&ctx](const httplib::Request& req, httplib::Response& res) {
    send_json(res, walk_query(ctx, walk_params(req)));
  }));

  server.Get("/api/walk/departures", guarded([&ctx](const httplib::Request& req, httplib::Response& res) {
    WalkQuery q = walk_params(req);
    const auto step = parse_double(param(req, "step", "900"));
    const auto count = parse_double(param(req, "count", "12"));
    if (!step || !count) throw BadRequest("'step' and 'count' must be numbers");
    send_json(res, departures_query(ctx, q, q.options.unix_time, static_cast<std::int64_t>(*step),
                                    static_cast<int>(*count)));
  }));

  server.Get("/api/weather", guarded([&ctx](const httplib::Request& req, httplib::Response& res) {
    send_json(res, weather_query(ctx, time_or_now(req)));
  }));

  server.Get("/api/sun", guarded([&graph](const httplib::Request& req, httplib::Response& res) {
    const std::int64_t time = time_or_now(req);
    json body = to_json(sun_position(graph.bounds().center(), time));
    body["time"] = time;
    send_json(res, body);
  }));

  server.Get("/api/shadows", guarded([&ctx](const httplib::Request& req, httplib::Response& res) {
    if (ctx.walk == nullptr) throw RoutingError("Shade data is not loaded on this server");
    const std::string raw = param(req, "bbox");
    double v[4];
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
      const std::size_t comma = i < 3 ? raw.find(',', start) : raw.size();
      if (comma == std::string::npos) throw BadRequest("'bbox' must be 'south,west,north,east'");
      const auto value = parse_double(raw.substr(start, comma - start));
      if (!value) throw BadRequest("'bbox' must be 'south,west,north,east'");
      v[i] = *value;
      start = comma + 1;
    }
    if (v[0] > v[2] || v[1] > v[3]) throw BadRequest("'bbox' must be 'south,west,north,east'");

    constexpr std::size_t kLimit = 2500;
    const SunPosition sun = ctx.walk->sun_at(time_or_now(req));
    const auto shapes = ctx.walk->shade_map().shadows({v[0], v[1], v[2], v[3]}, sun, kLimit);
    json buildings = json::array();
    for (const auto& shape : shapes) {
      buildings.push_back({{"footprint", path_json(shape.footprint)}, {"shadow", path_json(shape.shadow)}});
    }
    send_json(res, {{"sun", to_json(sun)}, {"buildings", buildings}, {"truncated", shapes.size() >= kLimit}});
  }));

  if (ctx.users != nullptr) register_account_routes(server, ctx);
  if (ctx.assistant != nullptr) register_assistant_routes(server, ctx);
}

}  // namespace umbra::server
