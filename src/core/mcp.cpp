// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Model Context Protocol server (core/mcp.h): reading each JSON-RPC message and answering it in the era
// the client opened with, checking a tool's arguments against its schema, running its handler where the transport
// posts it, and sending its result, progress, and nothing once it is cancelled.

#include "libdesktop/core.h"
#include "libdesktop/core/mcp.h"
#include "libdesktop/core/base64.h"

#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace
{
	using writer_t = rapidjson::Writer<rapidjson::StringBuffer>;

	constexpr int parse_error = -32700;
	constexpr int invalid_request = -32600;
	constexpr int method_not_found = -32601;
	constexpr int invalid_params = -32602;
	constexpr int unsupported_protocol_version = -32022;

	constexpr std::string_view meta_key = "_meta";
	constexpr std::string_view protocol_version_key = "io.modelcontextprotocol/protocolVersion";
	constexpr std::string_view client_capabilities_key = "io.modelcontextprotocol/clientCapabilities";
	constexpr std::string_view server_info_key = "io.modelcontextprotocol/serverInfo";

	// The tool list is fixed for the life of the process, which a client restarting the server re-asks.
	constexpr int list_ttl_ms = 10 * 60 * 1000;

	// Deep enough for any tool's arguments, and a bound on what a hostile schema or message can cost.
	constexpr int max_schema_depth = 32;

	const core::mcp::value* member(const core::mcp::value& object, const std::string_view name)
	{
		if (!object.IsObject()) return nullptr;
		const auto found = object.FindMember(core::mcp::value(rapidjson::StringRef(name.data(), name.size())));
		return found == object.MemberEnd() ? nullptr : &found->value;
	}

	std::string_view string_of(const core::mcp::value* v)
	{
		return v && v->IsString() ? std::string_view(v->GetString(), v->GetStringLength()) : std::string_view{};
	}

	bool is_supported(const std::string_view version)
	{
		return version == core::mcp::modern_version ||
		       std::ranges::find(core::mcp::legacy_versions, version) != core::mcp::legacy_versions.end();
	}

	std::string supported_versions_json()
	{
		std::string result = "[" + core::mcp::quote(core::mcp::modern_version);
		for (const auto version : core::mcp::legacy_versions) result += "," + core::mcp::quote(version);
		return result + "]";
	}

	bool valid_tool_name(const std::string_view name)
	{
		return !name.empty() && name.size() <= 128 &&
		       std::ranges::all_of(name, [](const char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
			       c == '.';
		});
	}

	size_t count_code_points(const std::string_view text)
	{
		return static_cast<size_t>(
			std::ranges::count_if(text, [](const char c) { return (static_cast<uint8_t>(c) & 0xC0u) != 0x80u; }));
	}

	bool has_type(const core::mcp::value& v, const std::string_view type)
	{
		if (type == "object") return v.IsObject();
		if (type == "array") return v.IsArray();
		if (type == "string") return v.IsString();
		if (type == "boolean") return v.IsBool();
		if (type == "null") return v.IsNull();
		if (type == "number") return v.IsNumber();
		if (type == "integer")
		{
			if (v.IsInt64() || v.IsUint64()) return true;
			if (!v.IsDouble()) return false;
			const auto d = v.GetDouble();
			return std::isfinite(d) && std::floor(d) == d;
		}
		return false;
	}

	std::string describe(const std::string& path)
	{
		return path.empty() ? std::string("the arguments") : path;
	}

	std::string article(const std::string_view type)
	{
		return type == "object" || type == "array" || type == "integer" ? std::format("an {}", type)
		                                                                : std::format("a {}", type);
	}

	std::string validate_at(const core::mcp::value& schema, const core::mcp::value& v, const std::string& path,
	                        const int depth)
	{
		if (schema.IsBool()) return schema.GetBool() ? std::string{} : std::format("{} is not allowed", describe(path));
		if (!schema.IsObject()) return {};
		if (depth > max_schema_depth) return std::format("{} is nested too deeply", describe(path));

		if (const auto* const type = member(schema, "type"))
		{
			auto matches = false;
			std::vector<std::string_view> names;

			if (type->IsString())
			{
				names.emplace_back(type->GetString(), type->GetStringLength());
			}
			else if (type->IsArray())
			{
				for (const auto& t : type->GetArray())
					if (t.IsString()) names.emplace_back(t.GetString(), t.GetStringLength());
			}

			for (const auto name : names) matches = matches || has_type(v, name);

			if (!names.empty() && !matches)
			{
				std::string expected;
				for (const auto name : names) expected += (expected.empty() ? "" : " or ") + article(name);
				return std::format("{} must be {}", describe(path), expected);
			}
		}

		if (const auto* const constant = member(schema, "const"); constant && !(*constant == v))
		{
			return std::format("{} must be {}", describe(path), core::mcp::to_json(*constant));
		}

		if (const auto* const options = member(schema, "enum"); options && options->IsArray())
		{
			auto found = false;
			std::string listed;

			for (const auto& option : options->GetArray())
			{
				found = found || option == v;
				listed += (listed.empty() ? "" : ", ") + core::mcp::to_json(option);
			}

			if (!found) return std::format("{} must be one of {}", describe(path), listed);
		}

		if (v.IsString())
		{
			const auto length = count_code_points(std::string_view(v.GetString(), v.GetStringLength()));
			if (const auto* const min = member(schema, "minLength");
			    min && min->IsNumber() && length < min->GetDouble())
				return std::format("{} must be at least {} characters", describe(path), min->GetDouble());
			if (const auto* const max = member(schema, "maxLength");
			    max && max->IsNumber() && length > max->GetDouble())
				return std::format("{} must be at most {} characters", describe(path), max->GetDouble());
		}

		if (v.IsNumber())
		{
			const auto d = v.GetDouble();
			if (const auto* const min = member(schema, "minimum"); min && min->IsNumber() && d < min->GetDouble())
				return std::format("{} must be at least {}", describe(path), min->GetDouble());
			if (const auto* const max = member(schema, "maximum"); max && max->IsNumber() && d > max->GetDouble())
				return std::format("{} must be at most {}", describe(path), max->GetDouble());
			if (const auto* const min = member(schema, "exclusiveMinimum");
			    min && min->IsNumber() && d <= min->GetDouble())
				return std::format("{} must be more than {}", describe(path), min->GetDouble());
			if (const auto* const max = member(schema, "exclusiveMaximum");
			    max && max->IsNumber() && d >= max->GetDouble())
				return std::format("{} must be less than {}", describe(path), max->GetDouble());
		}

		if (v.IsArray())
		{
			const auto size = static_cast<double>(v.Size());
			if (const auto* const min = member(schema, "minItems"); min && min->IsNumber() && size < min->GetDouble())
				return std::format("{} must hold at least {} items", describe(path), min->GetDouble());
			if (const auto* const max = member(schema, "maxItems"); max && max->IsNumber() && size > max->GetDouble())
				return std::format("{} must hold at most {} items", describe(path), max->GetDouble());

			if (const auto* const items = member(schema, "items"))
			{
				for (rapidjson::SizeType i = 0; i < v.Size(); ++i)
				{
					auto error = validate_at(*items, v[i], std::format("{}[{}]", path, i), depth + 1);
					if (!error.empty()) return error;
				}
			}
		}

		if (v.IsObject())
		{
			if (const auto* const required = member(schema, "required"); required && required->IsArray())
			{
				for (const auto& name : required->GetArray())
				{
					if (name.IsString() && !v.HasMember(name))
					{
						return path.empty() ? std::format("'{}' is required", name.GetString())
						                    : std::format("'{}' is required in {}", name.GetString(), path);
					}
				}
			}

			const auto* const properties = member(schema, "properties");
			const auto* const additional = member(schema, "additionalProperties");

			for (const auto& property : v.GetObject())
			{
				const std::string_view name(property.name.GetString(), property.name.GetStringLength());
				const auto child_path = path.empty() ? std::string(name) : std::format("{}.{}", path, name);

				if (const auto* const declared = properties ? member(*properties, name) : nullptr)
				{
					auto error = validate_at(*declared, property.value, child_path, depth + 1);
					if (!error.empty()) return error;
				}
				else if (additional)
				{
					if (additional->IsBool() && !additional->GetBool())
					{
						return path.empty() ? std::format("there is no argument '{}'", name)
						                    : std::format("{} has no '{}'", path, name);
					}

					auto error = validate_at(*additional, property.value, child_path, depth + 1);
					if (!error.empty()) return error;
				}
			}
		}

		if (const auto* const any = member(schema, "anyOf"); any && any->IsArray() && !any->Empty())
		{
			std::string first;

			for (const auto& option : any->GetArray())
			{
				auto error = validate_at(option, v, path, depth + 1);
				if (error.empty()) return {};
				if (first.empty()) first = std::move(error);
			}

			return first;
		}

		if (const auto* const one = member(schema, "oneOf"); one && one->IsArray() && !one->Empty())
		{
			auto matched = 0;
			std::string first;

			for (const auto& option : one->GetArray())
			{
				auto error = validate_at(option, v, path, depth + 1);
				if (error.empty()) ++matched;
				else if (first.empty()) first = std::move(error);
			}

			if (matched == 0) return first;
			if (matched > 1) return std::format("{} matches more than one of its forms", describe(path));
		}

		return {};
	}

	// A tool's answer as the members of a tools/call result. Text the tool gave stands as it is beside its
	// structured content; without any, the structured content is the text too, for a client that reads only text.
	std::string result_members(const core::mcp::tool_result& result)
	{
		rapidjson::StringBuffer buffer;
		writer_t writer(buffer);
		writer.StartObject();
		writer.Key("content");
		writer.StartArray();

		auto has_text = false;

		for (const auto& c : result.content)
		{
			writer.StartObject();

			if (c.kind == core::mcp::content::kind_t::image)
			{
				const auto encoded = base64_encode(c.data);
				writer.Key("type");
				writer.String("image");
				writer.Key("data");
				writer.String(encoded.c_str(), static_cast<rapidjson::SizeType>(encoded.size()));
				writer.Key("mimeType");
				writer.String(c.mime_type.c_str(), static_cast<rapidjson::SizeType>(c.mime_type.size()));
			}
			else
			{
				has_text = true;
				writer.Key("type");
				writer.String("text");
				writer.Key("text");
				writer.String(c.text.c_str(), static_cast<rapidjson::SizeType>(c.text.size()));
			}

			writer.EndObject();
		}

		rapidjson::Document structured;
		const auto has_structured = !result.structured.empty() &&
		                            !structured.Parse(result.structured.c_str()).HasParseError() &&
		                            structured.IsObject();

		if (has_structured && !has_text)
		{
			writer.StartObject();
			writer.Key("type");
			writer.String("text");
			writer.Key("text");
			writer.String(result.structured.c_str(), static_cast<rapidjson::SizeType>(result.structured.size()));
			writer.EndObject();
		}

		writer.EndArray();

		if (has_structured)
		{
			writer.Key("structuredContent");
			structured.Accept(writer);
		}

		writer.Key("isError");
		writer.Bool(result.is_error);
		writer.EndObject();

		// The members alone: the caller writes the braces round them, with the members a result of its era carries.
		const std::string_view object(buffer.GetString(), buffer.GetSize());
		return std::string(object.substr(1, object.size() - 2));
	}
}

namespace core::mcp
{
	std::string quote(const std::string_view text)
	{
		rapidjson::StringBuffer buffer;
		writer_t writer(buffer);
		writer.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string to_json(const value& v)
	{
		rapidjson::StringBuffer buffer;
		writer_t writer(buffer);
		v.Accept(writer);
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string validate(const value& schema, const value& instance)
	{
		return validate_at(schema, instance, {}, 0);
	}

	tool_result tool_result::text(std::string message)
	{
		tool_result result;
		result.content.push_back({content::kind_t::text, std::move(message), {}, {}});
		return result;
	}

	tool_result tool_result::error(std::string message)
	{
		auto result = text(std::move(message));
		result.is_error = true;
		return result;
	}

	call::call(std::string tool, const value& arguments, std::function<void(double, double, std::string_view)> progress,
	           std::function<void(tool_result)> finish) :
		_tool(std::move(tool)), _progress(std::move(progress)), _finish(std::move(finish))
	{
		if (arguments.IsObject()) _arguments.CopyFrom(arguments, _arguments.GetAllocator());
		else _arguments.SetObject();
	}

	bool call::has(const std::string_view name) const
	{
		const auto* const v = member(_arguments, name);
		return v && !v->IsNull();
	}

	std::string call::text(const std::string_view name, const std::string_view fallback) const
	{
		const auto* const v = member(_arguments, name);
		return v && v->IsString() ? std::string(v->GetString(), v->GetStringLength()) : std::string(fallback);
	}

	std::optional<double> call::number(const std::string_view name) const
	{
		const auto* const v = member(_arguments, name);
		if (!v || !v->IsNumber()) return std::nullopt;
		return v->GetDouble();
	}

	bool call::flag(const std::string_view name, const bool fallback) const
	{
		const auto* const v = member(_arguments, name);
		return v && v->IsBool() ? v->GetBool() : fallback;
	}

	std::vector<std::string> call::texts(const std::string_view name) const
	{
		std::vector<std::string> result;
		const auto* const v = member(_arguments, name);

		if (v && v->IsString())
		{
			result.emplace_back(v->GetString(), v->GetStringLength());
		}
		else if (v && v->IsArray())
		{
			for (const auto& item : v->GetArray())
				if (item.IsString()) result.emplace_back(item.GetString(), item.GetStringLength());
		}

		return result;
	}

	void call::progress(const double done, const double total, const std::string_view message)
	{
		if (is_cancelled() || is_finished() || !_progress) return;

		// Progress only ever goes forward: the protocol requires it to increase with each notification.
		std::lock_guard lock(_progress_mutex);
		if (done <= _last_progress) return;
		_last_progress = done;
		_progress(done, total, message);
	}

	void call::finish(tool_result result)
	{
		if (_finished.exchange(true)) return;
		if (_finish) _finish(std::move(result));
	}

	server::server(server_info info) : _info(std::move(info))
	{
	}

	bool server::add_tool(tool t)
	{
		registered_tool registered;

		if (!valid_tool_name(t.name))
		{
			core::log(__FUNCTION__, std::format("Refused a tool with the name '{}'", t.name));
			return false;
		}

		if (registered.input_schema.Parse(t.input_schema.c_str()).HasParseError() ||
		    !registered.input_schema.IsObject())
		{
			core::log(__FUNCTION__, std::format("Refused {}: its input schema is not a JSON object", t.name));
			return false;
		}

		registered.input_json = to_json(registered.input_schema);

		if (!t.output_schema.empty())
		{
			rapidjson::Document output;

			if (output.Parse(t.output_schema.c_str()).HasParseError() || !output.IsObject())
			{
				core::log(__FUNCTION__, std::format("Refused {}: its output schema is not a JSON object", t.name));
				return false;
			}

			registered.output_json = to_json(output);
		}

		const auto name = t.name;
		registered.definition = std::move(t);

		std::lock_guard lock(_mutex);
		return _tools.try_emplace(name, std::move(registered)).second;
	}

	std::vector<std::string> server::tool_names() const
	{
		std::lock_guard lock(_mutex);
		std::vector<std::string> result;
		for (const auto& [name, t] : _tools) result.emplace_back(name);
		return result;
	}

	void server::connect(std::function<void(std::string_view)> send, std::function<void(std::function<void()>)> post)
	{
		std::lock_guard lock(_mutex);
		_send = std::move(send);
		_post = std::move(post);
	}

	size_t server::running_calls() const
	{
		std::lock_guard lock(_mutex);
		return _calls.size();
	}

	void server::close()
	{
		std::map<std::string, call_ptr, std::less<>> calls;
		{
			std::lock_guard lock(_mutex);
			_closed = true;
			calls.swap(_calls);
		}

		for (const auto& [id, c] : calls) c->cancel();
	}

	void server::send(const std::string_view message) const
	{
		std::function<void(std::string_view)> send;
		{
			std::lock_guard lock(_mutex);
			if (_closed) return;
			send = _send;
		}

		// One message at a time, whole: two threads answering at once must not interleave their lines.
		std::lock_guard lock(_send_mutex);
		if (send) send(message);
	}

	void server::respond(const std::string_view id_json, const std::string_view result_members, const bool modern) const
	{
		std::string result = "{";
		result += result_members;

		if (modern)
		{
			if (!result_members.empty()) result += ",";
			result += std::format(R"("resultType":"complete","_meta":{{{}:{{"name":{},"version":{}}}}})",
			                      quote(server_info_key), quote(_info.name), quote(_info.version));
		}

		result += "}";
		send(std::format(R"({{"jsonrpc":"2.0","id":{},"result":{}}})", id_json, result));
	}

	void server::respond_error(const std::string_view id_json, const int code, const std::string_view message,
	                           const std::string_view data_json) const
	{
		const auto data = data_json.empty() ? std::string{} : std::format(R"(,"data":{})", data_json);
		send(std::format(R"({{"jsonrpc":"2.0","id":{},"error":{{"code":{},"message":{}{}}}}})",
		                 id_json.empty() ? "null"sv : id_json, code, quote(message), data));
	}

	std::string server::capabilities_json() const
	{
		return R"({"tools":{"listChanged":false}})";
	}

	void server::receive(std::string_view line)
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.remove_suffix(1);
		while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
		if (line.empty()) return;

		if (line.size() > max_message_bytes)
		{
			respond_error({}, invalid_request,
			              std::format("A message is at most {} bytes; this one is {}", max_message_bytes, line.size()));
			return;
		}

		rapidjson::Document message;
		message.Parse(line.data(), line.size());

		if (message.HasParseError())
		{
			respond_error({}, parse_error,
			              std::format("Not JSON: {} at offset {}", rapidjson::GetParseError_En(message.GetParseError()),
			                          message.GetErrorOffset()));
			return;
		}

		if (message.IsArray())
		{
			respond_error({}, invalid_request, "Batches are not supported: send one message a line");
			return;
		}

		if (!message.IsObject())
		{
			respond_error({}, invalid_request, "A message is a JSON object");
			return;
		}

		std::string id_json;
		const auto* const id = member(message, "id");

		if (id)
		{
			if (!id->IsString() && !id->IsInt64() && !id->IsUint64())
			{
				respond_error({}, invalid_request, "A request's id is a string or an integer");
				return;
			}

			id_json = to_json(*id);
		}

		if (string_of(member(message, "jsonrpc")) != "2.0")
		{
			respond_error(id_json, invalid_request, "jsonrpc must be \"2.0\"");
			return;
		}

		const auto* const method = member(message, "method");

		// A response: this server sends no requests, so nothing awaits one.
		if (!method) return;

		if (!method->IsString())
		{
			respond_error(id_json, invalid_request, "method must be a string");
			return;
		}

		static const value empty_params(rapidjson::kObjectType);
		const auto* params = member(message, "params");

		if (params && !params->IsObject())
		{
			respond_error(id_json, invalid_params, "params must be an object");
			return;
		}

		if (!params) params = &empty_params;
		const std::string_view method_name(method->GetString(), method->GetStringLength());

		if (!id)
		{
			handle_notification(method_name, *params);
			return;
		}

		// A modern request names its revision; an initialize, or a request after one, is legacy.
		const auto* const meta = member(*params, meta_key);
		const auto* const requested = meta ? member(*meta, protocol_version_key) : nullptr;
		const auto modern = requested != nullptr;

		if (modern)
		{
			const auto version = string_of(requested);

			if (!is_supported(version))
			{
				respond_error(id_json, unsupported_protocol_version, "Unsupported protocol version",
				              std::format(R"({{"supported":{},"requested":{}}})", supported_versions_json(),
				                          quote(version)));
				return;
			}

			const auto* const capabilities = member(*meta, client_capabilities_key);

			if (!capabilities || !capabilities->IsObject())
			{
				respond_error(id_json, invalid_params, std::format("_meta needs {}", client_capabilities_key));
				return;
			}
		}

		handle_request(id_json, method_name, *params, modern);
	}

	void server::handle_notification(const std::string_view method, const value& params)
	{
		if (method != "notifications/cancelled") return;

		const auto* const request = member(params, "requestId");
		if (!request || (!request->IsString() && !request->IsInt64() && !request->IsUint64())) return;

		call_ptr cancelled;
		{
			std::lock_guard lock(_mutex);
			const auto found = _calls.find(to_json(*request));
			if (found == _calls.end()) return;
			cancelled = found->second;
			_calls.erase(found);
		}

		cancelled->cancel();
	}

	void server::handle_request(const std::string_view id_json, const std::string_view method, const value& params,
	                            const bool modern)
	{
		if (method == "initialize")
		{
			const auto asked = string_of(member(params, "protocolVersion"));
			const auto legacy = std::ranges::find(legacy_versions, asked) != legacy_versions.end()
			                        ? asked
			                        : legacy_versions.front();
			auto members =
				std::format(R"("protocolVersion":{},"capabilities":{},"serverInfo":{{"name":{},"version":{}}})",
			                quote(legacy), capabilities_json(), quote(_info.name), quote(_info.version));
			if (!_info.instructions.empty()) members += std::format(R"(,"instructions":{})", quote(_info.instructions));
			respond(id_json, members, false);
			return;
		}

		if (method == "ping")
		{
			respond(id_json, {}, modern);
			return;
		}

		if (method == "server/discover")
		{
			auto members = std::format(R"("supportedVersions":{},"capabilities":{},"ttlMs":{},"cacheScope":"private")",
			                           supported_versions_json(), capabilities_json(), list_ttl_ms);
			if (!_info.instructions.empty()) members += std::format(R"(,"instructions":{})", quote(_info.instructions));
			respond(id_json, members, true);
			return;
		}

		if (method == "tools/list")
		{
			std::string tools;
			{
				std::lock_guard lock(_mutex);

				for (const auto& [name, t] : _tools)
				{
					const auto& d = t.definition;
					const auto& a = d.annotations;
					if (!tools.empty()) tools += ",";
					tools += std::format(R"({{"name":{},"description":{},"inputSchema":{})", quote(d.name),
					                     quote(d.description), t.input_json);
					if (!d.title.empty()) tools += std::format(R"(,"title":{})", quote(d.title));
					if (!t.output_json.empty()) tools += std::format(R"(,"outputSchema":{})", t.output_json);
					tools += std::format(
						R"(,"annotations":{{"readOnlyHint":{},"destructiveHint":{},"idempotentHint":{},"openWorldHint":{}}}}})",
						a.read_only, a.destructive, a.idempotent, a.open_world);
				}
			}

			auto members = std::format(R"("tools":[{}])", tools);
			if (modern) members += std::format(R"(,"ttlMs":{},"cacheScope":"private")", list_ttl_ms);
			respond(id_json, members, modern);
			return;
		}

		if (method == "tools/call")
		{
			call_tool(id_json, params, modern);
			return;
		}

		respond_error(id_json, method_not_found, std::format("No method {}", method));
	}

	void server::call_tool(const std::string_view id_json, const value& params, const bool modern)
	{
		const auto name = string_of(member(params, "name"));
		static const value no_arguments(rapidjson::kObjectType);
		const auto* arguments = member(params, "arguments");
		if (!arguments || arguments->IsNull()) arguments = &no_arguments;

		std::function<void(const call_ptr&)> handler;
		std::function<void(std::function<void()>)> post;
		std::string schema_error;
		{
			std::lock_guard lock(_mutex);

			if (const auto found = _tools.find(name); found != _tools.end())
			{
				handler = found->second.definition.handler;
				post = _post;
				schema_error = validate(found->second.input_schema, *arguments);
			}
		}

		if (!handler)
		{
			respond_error(id_json, invalid_params, std::format("No tool {}", name));
			return;
		}

		// An argument the schema refuses is the model's to correct, so it is answered as the tool's error.
		if (!schema_error.empty())
		{
			respond(id_json, result_members(tool_result::error(schema_error)), modern);
			return;
		}

		const auto* const meta = member(params, meta_key);
		const auto* const token = meta ? member(*meta, "progressToken") : nullptr;
		const auto token_json = token && (token->IsString() || token->IsInt64() || token->IsUint64()) ? to_json(*token)
		                                                                                              : std::string{};
		const auto weak = weak_from_this();
		const std::string id(id_json);

		auto progress = token_json.empty()
		                    ? std::function<void(double, double, std::string_view)>{}
		                    : [weak, token_json](const double done, const double total, const std::string_view message)
		{
			const auto self = weak.lock();
			if (!self || !std::isfinite(done)) return;
			auto params_json = std::format(R"("progressToken":{},"progress":{})", token_json, done);
			if (std::isfinite(total) && total > 0) params_json += std::format(R"(,"total":{})", total);
			if (!message.empty()) params_json += std::format(R"(,"message":{})", quote(message));
			self->send(
				std::format(R"({{"jsonrpc":"2.0","method":"notifications/progress","params":{{{}}}}})", params_json));
		};

		auto finish = [weak, id, modern](tool_result result)
		{
			const auto self = weak.lock();
			if (!self) return;

			// A call no longer running was cancelled, or the input closed: it is answered with nothing.
			{
				std::lock_guard lock(self->_mutex);
				const auto found = self->_calls.find(id);
				if (found == self->_calls.end()) return;
				self->_calls.erase(found);
			}

			self->respond(id, result_members(result), modern);
		};

		const auto c = std::make_shared<call>(std::string(name), *arguments, std::move(progress), std::move(finish));
		auto accepted = false;
		{
			std::lock_guard lock(_mutex);
			if (_closed) return;

			if (_calls.size() < max_running_calls && !_calls.contains(id))
			{
				_calls.emplace(id, c);
				accepted = true;
			}
		}

		if (!accepted)
		{
			respond_error(id_json, invalid_request,
			              std::format("At most {} calls run at once, each with an id of its own", max_running_calls));
			return;
		}

		const auto run = [handler, c]
		{
			if (c->is_cancelled()) return;

			try
			{
				handler(c);
			}
			catch (const std::exception& e)
			{
				c->finish(tool_result::error(std::format("{} failed: {}", c->tool(), e.what())));
			}
			catch (...)
			{
				c->finish(tool_result::error(std::format("{} failed", c->tool())));
			}
		};

		if (post) post(run);
		else run();
	}
}
