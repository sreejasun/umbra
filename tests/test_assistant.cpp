// The trip assistant's tool-calling loop, against a scripted fake model.

#include <gtest/gtest.h>

#include <deque>

#include "server/assistant.hpp"

namespace umbra::server {
namespace {

/// Replays canned Responses API replies and records every request.
struct FakeModel {
  std::deque<json> replies;
  std::vector<json> requests;
  std::vector<std::string> headers;

  HttpPost client() {
    return [this](const std::string&, const std::string& body, const std::vector<std::string>& h) {
      requests.push_back(json::parse(body));
      headers = h;
      if (replies.empty()) throw std::runtime_error("no more replies");
      json reply = replies.front();
      replies.pop_front();
      return reply.dump();
    };
  }
};

json function_call(const std::string& name, const json& args, const std::string& id) {
  return {{"type", "function_call"}, {"name", name}, {"arguments", args.dump()}, {"call_id", id}};
}

json message(const std::string& text) {
  return {{"type", "message"}, {"content", json::array({{{"type", "output_text"}, {"text", text}}})}};
}

std::vector<AssistantTool> echo_tool(int& calls) {
  return {{"double_it",
           "Doubles x.",
           {{"type", "object"}, {"properties", {{"x", {{"type", "number"}}}}}},
           [&calls](const json& args) {
             ++calls;
             return ToolResult{{{"result", args.value("x", 0.0) * 2}}, {{"geometry", "route"}}};
           }}};
}

TEST(TripAssistant, RunsToolsThenAnswers) {
  FakeModel model;
  model.replies.push_back({{"id", "r1"}, {"output", json::array({function_call("double_it", {{"x", 21}}, "c1")})}});
  model.replies.push_back({{"id", "r2"}, {"output", json::array({message("It is 42.")})}});
  int calls = 0;
  TripAssistant assistant(model.client(), "KEY", echo_tool(calls), {});

  const AssistantAnswer answer = assistant.ask("What is 21 doubled?", "Current time: 0");
  EXPECT_EQ(answer.text, "It is 42.");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(answer.tools_used, std::vector<std::string>{"double_it"});
  EXPECT_EQ(answer.route["geometry"], "route") << "the map gets the tool's route";

  ASSERT_EQ(model.requests.size(), 2U);
  EXPECT_EQ(model.requests[0]["tools"][0]["name"], "double_it");
  EXPECT_EQ(model.requests[1]["previous_response_id"], "r1");
  const json& output = model.requests[1]["input"][0];
  EXPECT_EQ(output["type"], "function_call_output");
  EXPECT_EQ(output["call_id"], "c1");
  EXPECT_EQ(json::parse(output["output"].get<std::string>())["result"], 42.0);
  EXPECT_EQ(model.headers.front(), "Authorization: Bearer KEY");
}

TEST(TripAssistant, ReportsToolErrorsToTheModel) {
  FakeModel model;
  model.replies.push_back({{"id", "r1"}, {"output", json::array({function_call("teleport", json::object(), "c1")})}});
  model.replies.push_back({{"id", "r2"}, {"output", json::array({message("I can't do that.")})}});
  int calls = 0;
  TripAssistant assistant(model.client(), "KEY", echo_tool(calls), {});
  EXPECT_EQ(assistant.ask("Teleport me", "").text, "I can't do that.");
  const std::string sent = model.requests[1]["input"][0]["output"];
  EXPECT_NE(sent.find("Unknown tool"), std::string::npos);
}

TEST(TripAssistant, StopsAfterMaxRounds) {
  FakeModel model;
  for (int i = 0; i < 10; ++i) {
    model.replies.push_back({{"id", "r"}, {"output", json::array({function_call("double_it", {{"x", 1}}, "c")})}});
  }
  int calls = 0;
  AssistantOptions options;
  options.max_rounds = 3;
  TripAssistant assistant(model.client(), "KEY", echo_tool(calls), options);
  const AssistantAnswer answer = assistant.ask("loop forever", "");
  EXPECT_EQ(model.requests.size(), 3U);
  EXPECT_NE(answer.text.find("couldn't finish"), std::string::npos);
}

TEST(TripAssistant, ProviderFailuresBecomeAssistantErrors) {
  FakeModel model;  // no replies: the fake throws like a network error
  int calls = 0;
  TripAssistant assistant(model.client(), "KEY", echo_tool(calls), {});
  EXPECT_THROW((void)assistant.ask("hi", ""), AssistantError);

  FakeModel garbage;
  garbage.replies.push_back({{"unexpected", true}});
  TripAssistant confused(garbage.client(), "KEY", echo_tool(calls), {});
  EXPECT_THROW((void)confused.ask("hi", ""), AssistantError);
}

TEST(TripAssistant, RequiresAKey) {
  int calls = 0;
  FakeModel model;
  EXPECT_THROW(TripAssistant(model.client(), "", echo_tool(calls), {}), std::invalid_argument);
}

}  // namespace
}  // namespace umbra::server
