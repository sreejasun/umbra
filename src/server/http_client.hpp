#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace umbra::server {

/// True if this build can make outgoing HTTPS requests (compiled with libcurl).
[[nodiscard]] bool http_client_available() noexcept;

/// Initialise / release the HTTP client library. Call once from main().
void http_client_global_init();
void http_client_global_cleanup();

/**
 * Blocking HTTPS GET with a timeout. Returns the body of a 2xx response and
 * throws std::runtime_error otherwise (network failure, timeout, HTTP error,
 * oversized body, or a build without libcurl).
 */
[[nodiscard]] std::string http_get(const std::string& url, std::chrono::seconds timeout);

/// Blocking HTTPS POST of a JSON body with extra headers ("Name: value"); same
/// error handling as http_get.
[[nodiscard]] std::string http_post(const std::string& url, const std::string& body,
                                    const std::vector<std::string>& headers, std::chrono::seconds timeout);

}  // namespace umbra::server
