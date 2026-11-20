#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "server/api_common.hpp"
#include "umbra/http_types.hpp"

namespace umbra::server {

struct ApiContext;

/// The language-model provider failed or returned something unusable (HTTP 502).
class AssistantError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// What a tool hands back: a compact view for the model and, optionally, the
/// full route JSON for the map.
struct ToolResult {
  json for_model;
  json for_map = nullptr;
};

/// A function the model may call. `parameters` is a JSON Schema object.
struct AssistantTool {
  std::string name;
  std::string description;
  json parameters;
  std::function<ToolResult(const json& arguments)> run;
};

struct AssistantAnswer {
  std::string text;
  json route = nullptr;  ///< the last route a tool planned, for the map
  std::vector<std::string> tools_used;
};

struct AssistantOptions {
  std::string url = "https://api.x.ai/v1/responses";
  std::string model = "grok-4.7";
  int max_rounds = 6;           ///< model calls per question (bounds cost and latency)
  int max_output_tokens = 800;
};

/**
 * Natural-language trip planning with tool calling (xAI Responses API).
 *
 * The loop: send the question with the tool list; while the model answers
 * with `function_call` items, run each tool on Umbra's own engine and send
 * the results back (`function_call_output`, chained by `previous_response_id`);
 * stop at a text answer or after `max_rounds`. The model never sees the API
 * key or the user's account, only the question, the context line and tool
 * results. The HTTP client is injected, so tests run against a fake model.
 */
class TripAssistant {
 public:
  TripAssistant(HttpPost post, std::string api_key, std::vector<AssistantTool> tools, AssistantOptions options = {});

  /// `context` describes the situation (current time, timezone, the user's
  /// position if shared). Throws AssistantError if the provider fails.
  [[nodiscard]] AssistantAnswer ask(const std::string& question, const std::string& context) const;

  [[nodiscard]] const std::string& model() const noexcept { return options_.model; }

 private:
  [[nodiscard]] json call_model(const json& body) const;

  HttpPost post_;
  std::string api_key_;
  std::vector<AssistantTool> tools_;
  json tool_specs_;
  AssistantOptions options_;
};

/// Umbra's tools: search_places, plan_walk, plan_drive, get_weather, best_departure.
/// `ctx` must outlive the tools.
[[nodiscard]] std::vector<AssistantTool> trip_tools(const ApiContext& ctx);

}  // namespace umbra::server
