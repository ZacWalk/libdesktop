// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tests of the Model Context Protocol server (core/mcp.h), over an in-memory transport: both eras of client,
// the errors each protocol failure answers with, argument checks, results, progress and cancellation.

#include "test.h"

#include "libdesktop/core/mcp.h"

using namespace std::literals;

namespace
{
	constexpr std::string_view modern_meta =
		R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}})";

	// A server whose messages are kept, and whose handlers run as they are posted.
	struct fixture
	{
		std::shared_ptr<core::mcp::server> server;
		std::vector<std::string> sent;
		core::mcp::call_ptr held;

		fixture()
		{
			server = std::make_shared<core::mcp::server>(
				core::mcp::server_info{.name = "Test", .version = "1.2", .instructions = "Use echo."});
			server->connect([this](const std::string_view message) { sent.emplace_back(message); },
			                [](std::function<void()> task) { task(); });

			core::mcp::tool echo;
			echo.name = "echo";
			echo.description = "Says the text back.";
			echo.input_schema =
				R"({"type":"object","properties":{"text":{"type":"string"},"times":{"type":"integer","minimum":1}},"required":["text"],"additionalProperties":false})";
			echo.annotations.read_only = true;
			echo.handler = [](const core::mcp::call_ptr& c)
			{
				auto result = core::mcp::tool_result::text(c->text("text"));
				result.structured = std::format(R"({{"times":{}}})", c->number("times").value_or(1));
				c->finish(std::move(result));
			};
			server->add_tool(echo);

			core::mcp::tool hold;
			hold.name = "hold";
			hold.description = "Answers later.";
			hold.handler = [this](const core::mcp::call_ptr& c)
			{
				held = c;
			};
			server->add_tool(hold);

			core::mcp::tool picture;
			picture.name = "picture";
			picture.description = "A picture and what it shows.";
			picture.handler = [](const core::mcp::call_ptr& c)
			{
				core::mcp::tool_result result;
				result.content.push_back({core::mcp::content::kind_t::image, {}, "PNG", "image/png"});
				result.structured = R"({"width":1})";
				c->finish(std::move(result));
			};
			server->add_tool(picture);
		}

		rapidjson::Document last() const
		{
			rapidjson::Document d;
			if (!sent.empty()) d.Parse(sent.back().c_str());
			return d;
		}

		rapidjson::Document send(const std::string_view line)
		{
			sent.clear();
			server->receive(line);
			return last();
		}
	};

	int error_code(const rapidjson::Document& d)
	{
		return d.IsObject() && d.HasMember("error") && d["error"]["code"].IsInt() ? d["error"]["code"].GetInt() : 0;
	}

	std::string text_of(const rapidjson::Value& v)
	{
		return v.IsString() ? std::string(v.GetString(), v.GetStringLength()) : std::string{};
	}
}

static void should_answer_a_modern_request_without_a_handshake()
{
	fixture f;
	const auto discover = f.send(
		std::format(R"({{"jsonrpc":"2.0","id":"d","method":"server/discover","params":{{{}}}}})", modern_meta));
	assert_equal("complete", text_of(discover["result"]["resultType"]), "a modern result says it is complete");
	assert_equal("2026-07-28", text_of(discover["result"]["supportedVersions"][0]), "the modern revision first");
	assert_equal("Use echo.", text_of(discover["result"]["instructions"]), "and the server's instructions");
	assert_equal(true, discover["result"]["ttlMs"].IsInt(), "a discovery result carries a time to live");
	assert_equal("Test", text_of(discover["result"]["_meta"]["io.modelcontextprotocol/serverInfo"]["name"]),
	             "and names its server");

	const auto list = f.send(
		std::format(R"({{"jsonrpc":"2.0","id":7,"method":"tools/list","params":{{{}}}}})", modern_meta));
	assert_equal(7, list["id"].GetInt(), "the answer carries the request's id");
	assert_equal(3, static_cast<int>(list["result"]["tools"].Size()), "every tool is listed");
	assert_equal("echo", text_of(list["result"]["tools"][0]["name"]), "in name order");
	assert_equal(true, list["result"]["tools"][0]["annotations"]["readOnlyHint"].GetBool(), "with its annotations");
	assert_equal("private", text_of(list["result"]["cacheScope"]), "and a modern list says who may cache it");

	const auto called = f.send(std::format(
		R"({{"jsonrpc":"2.0","id":"c","method":"tools/call","params":{{"name":"echo","arguments":{{"text":"hi","times":2}},{}}}}})",
		modern_meta));
	assert_equal("hi", text_of(called["result"]["content"][0]["text"]), "the tool's text");
	assert_equal(2, called["result"]["structuredContent"]["times"].GetInt(), "and its structured content");
	assert_equal(false, called["result"]["isError"].GetBool(), "is not an error");
}

static void should_answer_a_legacy_client_after_initialize()
{
	fixture f;
	const auto init = f.send(
		R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"old","version":"1"}}})");
	assert_equal("2025-06-18", text_of(init["result"]["protocolVersion"]), "a legacy revision the server speaks");
	assert_equal("Test", text_of(init["result"]["serverInfo"]["name"]), "and its name");
	assert_equal(false, init["result"].HasMember("resultType"), "with no modern members");

	const auto newer = f.send(
		R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2099-01-01"}})");
	assert_equal("2025-11-25", text_of(newer["result"]["protocolVersion"]), "an unknown one gets the newest legacy");

	f.sent.clear();
	f.server->receive(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
	assert_equal(true, f.sent.empty(), "a notification is not answered");

	const auto list = f.send(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
	assert_equal(3, static_cast<int>(list["result"]["tools"].Size()), "its tools");
	assert_equal(false, list["result"].HasMember("ttlMs"), "with no caching hints for a legacy client");

	const auto pong = f.send(R"({"jsonrpc":"2.0","id":4,"method":"ping"})");
	assert_equal(true, pong["result"].IsObject(), "ping answers");
}

static void should_refuse_what_the_protocol_does_not_allow()
{
	fixture f;
	auto d = f.send(
		R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1900-01-01","io.modelcontextprotocol/clientCapabilities":{}}}})");
	assert_equal(-32022, error_code(d), "an unknown revision");
	assert_equal("2026-07-28", text_of(d["error"]["data"]["supported"][0]), "names the ones the server speaks");
	assert_equal("1900-01-01", text_of(d["error"]["data"]["requested"]), "and the one asked for");

	d = f.send(
		R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})");
	assert_equal(-32602, error_code(d), "a modern request without client capabilities");

	d = f.send("{not json");
	assert_equal(-32700, error_code(d), "text that is not JSON");
	assert_equal(true, d["id"].IsNull(), "answers with no id");

	assert_equal(-32600, error_code(f.send(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])")), "a batch");
	assert_equal(-32600, error_code(f.send(R"({"jsonrpc":"1.0","id":1,"method":"ping"})")), "the wrong JSON-RPC");
	assert_equal(-32600, error_code(f.send(R"({"jsonrpc":"2.0","id":null,"method":"ping"})")), "a null id");
	assert_equal(-32601, error_code(f.send(R"({"jsonrpc":"2.0","id":1,"method":"nothing/here"})")), "a method");
	assert_equal(-32602,
	             error_code(f.send(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"nope"}})")),
	             "a tool there is none of");

	f.sent.clear();
	f.server->receive(R"({"jsonrpc":"2.0","id":9,"result":{}})");
	f.server->receive("   ");
	assert_equal(true, f.sent.empty(), "a response, and an empty line, are not answered");
}

static void should_check_a_tools_arguments()
{
	fixture f;
	auto d = f.send(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{}}})");
	assert_equal(true, d["result"]["isError"].GetBool(), "a missing argument is the tool's error");
	assert_equal("'text' is required", text_of(d["result"]["content"][0]["text"]), "naming the argument");

	d = f.send(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":5}}})");
	assert_equal("text must be a string", text_of(d["result"]["content"][0]["text"]), "a value of the wrong type");

	d = f.send(
		R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"a","more":1}}})");
	assert_equal("there is no argument 'more'", text_of(d["result"]["content"][0]["text"]),
	             "an argument there is none of");

	d = f.send(
		R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{"text":"a","times":0}}})");
	assert_equal("times must be at least 1", text_of(d["result"]["content"][0]["text"]), "a number out of range");

	d = f.send(
		R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo","arguments":{"text":"a","times":2.5}}})");
	assert_equal("times must be an integer", text_of(d["result"]["content"][0]["text"]), "a fraction for an integer");
}

static void should_validate_schema_keywords()
{
	const auto check = [](const std::string_view schema, const std::string_view instance)
	{
		rapidjson::Document s;
		rapidjson::Document i;
		s.Parse(schema.data(), schema.size());
		i.Parse(instance.data(), instance.size());
		return core::mcp::validate(s, i);
	};

	assert_equal("", check(R"({"enum":["a","b"]})", R"("b")"), "a value enum names");
	assert_equal(R"(the arguments must be one of "a", "b")", check(R"({"enum":["a","b"]})", R"("c")"),
	             "one it does not");
	assert_equal("", check(R"({"type":"array","items":{"type":"number"},"maxItems":2})", "[1,2.5]"), "items");
	assert_equal("[1] must be a number", check(R"({"type":"array","items":{"type":"number"}})", R"([1,"x"])"),
	             "an item of the wrong type");
	assert_equal("the arguments must hold at most 1 items", check(R"({"maxItems":1})", "[1,2]"), "too many items");
	assert_equal("", check(R"({"anyOf":[{"type":"string"},{"type":"integer"}]})", "3"), "anyOf one of them");
	assert_equal("the arguments must be a string", check(R"({"anyOf":[{"type":"string"},{"type":"integer"}]})", "true"),
	             "anyOf none of them");
	assert_equal("the arguments matches more than one of its forms",
	             check(R"({"oneOf":[{"type":"number"},{"type":"integer"}]})", "3"), "oneOf more than one");
	assert_equal("", check(R"({"type":["string","null"]})", "null"), "a list of types");
	assert_equal(R"(the arguments must be "x")", check(R"({"const":"x"})", R"("y")"), "const");
	assert_equal("the arguments must be at most 2 characters", check(R"({"maxLength":2})", R"("née")"),
	             "length counts characters, not bytes");
	assert_equal("", check(R"({"maxLength":3})", R"("née")"), "so a word of three fits three");
	assert_equal("a.b must be a boolean",
	             check(R"({"properties":{"a":{"properties":{"b":{"type":"boolean"}}}}})", R"({"a":{"b":1}})"),
	             "a nested property names its path");
}

static void should_send_progress_and_nothing_once_cancelled()
{
	fixture f;
	f.send(
		R"({"jsonrpc":"2.0","id":"h","method":"tools/call","params":{"name":"hold","_meta":{"progressToken":"p"}}})");
	assert_equal(true, f.held != nullptr, "the handler ran");
	assert_equal(1, static_cast<int>(f.server->running_calls()), "and its call is running");

	f.sent.clear();
	f.held->progress(1, 4, "a quarter");
	f.held->progress(1, 4, "no further");
	f.held->progress(2, 4);
	assert_equal(2, static_cast<int>(f.sent.size()), "progress that does not advance is not sent");
	const auto progress = f.last();
	assert_equal("notifications/progress", text_of(progress["method"]), "a progress notification");
	assert_equal("p", text_of(progress["params"]["progressToken"]), "for the token the call gave");
	assert_equal(2.0, progress["params"]["progress"].GetDouble(), "how far it has got");

	f.sent.clear();
	f.server->receive(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"h"}})");
	assert_equal(true, f.held->is_cancelled(), "the client cancelled it");
	f.held->progress(3, 4);
	f.held->finish(core::mcp::tool_result::text("late"));
	assert_equal(true, f.sent.empty(), "so nothing more is sent for it");
	assert_equal(0, static_cast<int>(f.server->running_calls()), "and it no longer runs");

	f.send(R"({"jsonrpc":"2.0","id":"h2","method":"tools/call","params":{"name":"hold"}})");
	f.held->progress(1, 2);
	assert_equal(true, f.sent.empty(), "a call that asked for no progress hears none");
	f.server->close();
	assert_equal(true, f.held->is_cancelled(), "closing the input cancels what runs");
	f.held->finish(core::mcp::tool_result::text("late"));
	assert_equal(true, f.sent.empty(), "and answers none of it");
}

static void should_send_structured_and_image_content()
{
	fixture f;
	const auto d = f.send(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"picture"}})");
	const auto& content = d["result"]["content"];
	assert_equal("image", text_of(content[0]["type"]), "a picture");
	assert_equal("UE5H", text_of(content[0]["data"]), "in base64");
	assert_equal("image/png", text_of(content[0]["mimeType"]), "of its type");
	assert_equal(R"({"width":1})", text_of(content[1]["text"]),
	             "with no text of its own, the structured content is the text too");
	assert_equal(1, d["result"]["structuredContent"]["width"].GetInt(), "as well as structured");
}

static void should_refuse_a_tool_it_cannot_list()
{
	core::mcp::server s({.name = "Test", .version = "1"});
	core::mcp::tool t;
	t.name = "has space";
	assert_equal(false, s.add_tool(t), "a name with a space");
	t.name = "ok";
	t.input_schema = "{broken";
	assert_equal(false, s.add_tool(t), "a schema that is not JSON");
	t.input_schema = R"({"type":"object"})";
	assert_equal(true, s.add_tool(t), "a good one");
	assert_equal(false, s.add_tool(t), "but not twice");
}

void register_mcp_tests(test_registry& tests)
{
	tests.add("Should answer a modern MCP request without a handshake"s,
	          should_answer_a_modern_request_without_a_handshake);
	tests.add("Should answer a legacy MCP client after initialize"s, should_answer_a_legacy_client_after_initialize);
	tests.add("Should refuse what the MCP protocol does not allow"s, should_refuse_what_the_protocol_does_not_allow);
	tests.add("Should check an MCP tool's arguments"s, should_check_a_tools_arguments);
	tests.add("Should validate JSON schema keywords"s, should_validate_schema_keywords);
	tests.add("Should send MCP progress and nothing once cancelled"s, should_send_progress_and_nothing_once_cancelled);
	tests.add("Should send structured and image MCP content"s, should_send_structured_and_image_content);
	tests.add("Should refuse an MCP tool it cannot list"s, should_refuse_a_tool_it_cannot_list);
}
