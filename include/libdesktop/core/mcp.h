// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: A Model Context Protocol server over newline-delimited JSON-RPC, for an application an agent drives: the
// protocol both eras of client speak - modern requests that carry their version in _meta, and the legacy initialize
// handshake - the tools the application offers, the checks their arguments pass, and their results, progress and
// cancellation. The transport is the caller's: a line in, a message out. Not in core.h; an application that serves
// MCP includes it.

#pragma once

#include "libdesktop/core.h"

namespace core::mcp
{
	using value = rapidjson::GenericValue<rapidjson::UTF8<char>>;

	// The revisions this server speaks: the modern one, then the legacy ones an initialize can ask for.
	inline constexpr std::string_view modern_version = "2026-07-28";
	inline constexpr std::array<std::string_view, 3> legacy_versions = {"2025-11-25", "2025-06-18", "2025-03-26"};

	// What a client may assume of a tool. Hints, not guarantees: a client asks before running what is not read-only.
	struct tool_annotations
	{
		bool read_only = false;
		bool destructive = false;
		bool idempotent = false;
		// Reaches beyond the machine: the network.
		bool open_world = false;
	};

	struct content
	{
		enum class kind_t
		{
			text,
			image
		};

		kind_t kind = kind_t::text;
		std::string text;
		// An image's encoded bytes, which the server sends as base64, and their type.
		std::string data;
		std::string mime_type;
	};

	struct tool_result
	{
		std::vector<mcp::content> content;
		// A JSON object, as text, matching the tool's output schema; empty for none.
		std::string structured;
		bool is_error = false;

		static tool_result text(std::string message);
		// A failure the model can act on: what failed, why, and what to do.
		static tool_result error(std::string message);
	};

	// One tools/call: its arguments, whether the client has cancelled it, and its answer. Shared, so a handler can
	// carry it to a worker and answer from there.
	class call
	{
	public:
		// progress and finish are how the call reaches its client; either may be empty, as a test's call is.
		call(std::string tool, const value& arguments, std::function<void(double, double, std::string_view)> progress,
		     std::function<void(tool_result)> finish);
		call(const call&) = delete;
		call& operator=(const call&) = delete;

		const std::string& tool() const { return _tool; }
		// The arguments, which have passed the tool's input schema: always an object.
		const value& arguments() const { return _arguments; }

		bool has(std::string_view name) const;
		std::string text(std::string_view name, std::string_view fallback = {}) const;
		std::optional<double> number(std::string_view name) const;
		bool flag(std::string_view name, bool fallback) const;
		// A string or an array of strings, as a list.
		std::vector<std::string> texts(std::string_view name) const;

		bool is_cancelled() const { return _cancelled.load(std::memory_order_relaxed); }
		// Valid while the call is held.
		core::cancel_token cancel_token() { return core::cancel_token(_cancelled); }
		void cancel() { _cancelled = true; }

		// How far the call has got, where the client asked to hear: a total of zero is work of unknown size.
		void progress(double done, double total, std::string_view message = {});
		// Answers the call. Only the first answer is sent, and none once the call is cancelled.
		void finish(tool_result result);
		bool is_finished() const { return _finished.load(); }

	private:
		std::string _tool;
		rapidjson::Document _arguments;
		std::function<void(double, double, std::string_view)> _progress;
		std::function<void(tool_result)> _finish;
		std::atomic_bool _cancelled = false;
		std::atomic_bool _finished = false;
		double _last_progress = -1.0;
		std::mutex _progress_mutex;
	};

	using call_ptr = std::shared_ptr<call>;

	struct tool
	{
		// Letters, digits, '_', '-' and '.', at most 128.
		std::string name;
		std::string title;
		std::string description;
		// JSON Schema objects, as text.
		std::string input_schema = R"({"type":"object","additionalProperties":false})";
		std::string output_schema;
		tool_annotations annotations;
		// Runs on the thread the server posts to, and answers through the call, now or later, from any thread. It
		// must not block: long work goes to a worker.
		std::function<void(const call_ptr&)> handler;
	};

	struct server_info
	{
		std::string name;
		std::string version;
		// Guidance for the model on using the server as a whole.
		std::string instructions;
	};

	// Serves tools to one client. Every method is safe from any thread.
	class server : public std::enable_shared_from_this<server>
	{
	public:
		// The longest message accepted, and the most calls running at once.
		static constexpr size_t max_message_bytes = 4 * 1024 * 1024;
		static constexpr size_t max_running_calls = 32;

		explicit server(server_info info);
		server(const server&) = delete;
		server& operator=(const server&) = delete;

		// A tool whose schema is not valid JSON, or whose name is taken, is refused, and logged.
		bool add_tool(tool t);
		std::vector<std::string> tool_names() const;
		const server_info& info() const { return _info; }

		// Where messages go - one JSON-RPC message a call, with no line end - and how a tool's handler is run.
		// Before the first receive.
		void connect(std::function<void(std::string_view message)> send,
		             std::function<void(std::function<void()>)> post);
		// One line of input.
		void receive(std::string_view line);
		// The input has closed: every running call is cancelled, and none is answered.
		void close();
		size_t running_calls() const;

	private:
		struct registered_tool
		{
			tool definition;
			rapidjson::Document input_schema;
			// The schemas as compact JSON, for tools/list.
			std::string input_json;
			std::string output_json;
		};

		void send(std::string_view message) const;
		// result_members: the result object's members as JSON, without its braces.
		void respond(std::string_view id_json, std::string_view result_members, bool modern) const;
		void respond_error(std::string_view id_json, int code, std::string_view message,
		                   std::string_view data_json = {}) const;
		void handle_request(std::string_view id_json, std::string_view method, const value& params, bool modern);
		void handle_notification(std::string_view method, const value& params);
		void call_tool(std::string_view id_json, const value& params, bool modern);
		std::string capabilities_json() const;

		server_info _info;
		std::map<std::string, registered_tool, std::less<>> _tools;
		std::function<void(std::string_view)> _send;
		std::function<void(std::function<void()>)> _post;
		// Running calls, by their request id as JSON.
		std::map<std::string, call_ptr, std::less<>> _calls;
		bool _closed = false;
		mutable std::mutex _mutex;
		mutable std::mutex _send_mutex;
	};

	using server_ptr = std::shared_ptr<server>;

	// Why instance does not conform to schema, or empty when it does: the JSON Schema keywords a tool's
	// arguments use - type, properties, required, additionalProperties, items, enum, const, the numeric and
	// length bounds, anyOf and oneOf. Other keywords are ignored.
	std::string validate(const value& schema, const value& instance);

	// value as JSON text.
	std::string to_json(const value& v);
	// A string as a JSON string literal, quotes and escapes included.
	std::string quote(std::string_view text);
}
