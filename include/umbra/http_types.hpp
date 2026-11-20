#pragma once

#include <functional>
#include <string>
#include <vector>

namespace umbra {

/// Performs an HTTP GET and returns the body of a 2xx response; throws on any failure.
/// Services take it as a parameter, so the core library has no network code.
using HttpGet = std::function<std::string(const std::string& url)>;

/// Performs an HTTP POST of a JSON body with extra headers ("Name: value") and
/// returns the body of a 2xx response; throws on any failure.
using HttpPost = std::function<std::string(const std::string& url, const std::string& json_body,
                                           const std::vector<std::string>& headers)>;

}  // namespace umbra
