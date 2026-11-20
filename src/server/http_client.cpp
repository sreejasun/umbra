#include "server/http_client.hpp"

#include <stdexcept>

#if UMBRA_HAS_LIBCURL
#include <curl/curl.h>
#endif

namespace umbra::server {

#if UMBRA_HAS_LIBCURL

namespace {

constexpr std::size_t kMaxBodyBytes = 4 * 1024 * 1024;  // a 7-day hourly forecast is ~30 KB

std::size_t append_body(char* data, std::size_t size, std::size_t count, void* user) {
  auto* body = static_cast<std::string*>(user);
  const std::size_t bytes = size * count;
  if (body->size() + bytes > kMaxBodyBytes) return 0;  // abort the transfer
  body->append(data, bytes);
  return bytes;
}

/// Shared by GET and POST: perform the request and return a 2xx body.
std::string perform(const std::string& url, std::chrono::seconds timeout, const std::string* post_body,
                    const std::vector<std::string>& headers) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");

  curl_slist* header_list = nullptr;
  for (const std::string& h : headers) header_list = curl_slist_append(header_list, h.c_str());

  std::string body;
  char error[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout.count()));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // required for timeouts in multi-threaded programs
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "umbra/1.0 (open-source routing demo)");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // accept any encoding curl supports
  if (header_list != nullptr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  if (post_body != nullptr) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_body->size()));
  }

  const CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(header_list);

  // Never put the URL in errors: it can carry an API key (HERE uses ?apiKey=).
  if (code != CURLE_OK) {
    throw std::runtime_error(std::string("HTTP request failed: ") + (error[0] ? error : curl_easy_strerror(code)));
  }
  if (status < 200 || status >= 300) {
    throw std::runtime_error("HTTP request failed with status " + std::to_string(status));
  }
  return body;
}

}  // namespace

bool http_client_available() noexcept { return true; }

void http_client_global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void http_client_global_cleanup() { curl_global_cleanup(); }

std::string http_get(const std::string& url, std::chrono::seconds timeout) {
  return perform(url, timeout, nullptr, {});
}

std::string http_post(const std::string& url, const std::string& body, const std::vector<std::string>& headers,
                      std::chrono::seconds timeout) {
  return perform(url, timeout, &body, headers);
}

#else  // built without libcurl

bool http_client_available() noexcept { return false; }
void http_client_global_init() {}
void http_client_global_cleanup() {}

std::string http_get(const std::string&, std::chrono::seconds) {
  throw std::runtime_error("this build has no HTTP client (libcurl was not found at configure time)");
}

std::string http_post(const std::string&, const std::string&, const std::vector<std::string>&, std::chrono::seconds) {
  throw std::runtime_error("this build has no HTTP client (libcurl was not found at configure time)");
}

#endif

}  // namespace umbra::server
