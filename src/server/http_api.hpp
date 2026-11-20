#pragma once

#include "server/trip_queries.hpp"

namespace httplib {
class Server;
}

namespace umbra::server {

/// Register all /api/* endpoints on `server`. `context` (and everything it
/// points to) must outlive the server. Account and assistant endpoints are
/// registered only when those features are enabled.
void register_api(httplib::Server& server, const ApiContext& context);

}  // namespace umbra::server
