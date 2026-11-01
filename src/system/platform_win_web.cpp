// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The HTTP client as platform:: offers it: libdesktop-os's client, with the application's name
// as its user agent and the system's own timeouts, where a transfer that fails or is cut short is no
// response at all.

#include "libdesktop/system.h"
#include "libdesktop/platform_win.h"
#include "libdesktop/os.h"

#include <utility>

df_assert_movable(platform::web_request);
df_assert_movable(platform::web_response);

bool platform::is_online()
{
	return os::is_online();
}

struct platform::web_host
{
	os::web_host_ptr native;
};

platform::web_host_ptr platform::connect_to_host(const std::string_view host, const bool secure_in, const int port_in)
{
	auto native = os::connect_to_host(host, secure_in, port_in, s_app_name, false);
	if (!native) return nullptr;

	auto result = std::make_shared<web_host>();
	result->native = std::move(native);
	return result;
}

platform::web_response platform::send_request(const web_host_ptr& host, const web_request& req)
{
	web_response result;

	if (!host)
		return result;

	os::web_request native;
	// WinINet sends an object name that lacks its leading '/' as though it had one - download_and_verify
	// passes a bare file name - and the shared client refuses a path without one.
	native.path = req.path.empty() || req.path.front() == '/' ? req.path : "/" + req.path;
	native.query = req.query;
	native.headers = req.headers;
	native.form_data = req.form_data;
	native.verb = req.verb == web_request_verb::GET ? os::web_request_verb::GET : os::web_request_verb::POST;

	// An attachment that cannot be read is left off rather than failing the request, so what goes with
	// it - a crash report's text - still goes. Files are named as the OS takes them, with the \\?\
	// prefix a long path needs.
	if (!req.file_path.is_empty() && !req.file_form_data_name.empty())
	{
		if (open_file(req.file_path, file_open_mode::read))
		{
			native.file_form_data_name = req.file_form_data_name;
			native.file_name = req.file_name;
			native.upload_file_path = to_utf8_file_system_path(req.file_path);
		}
		else
		{
			df::log(__FUNCTION__, std::format("could not read {}", req.file_path.str()));
		}
	}

	if (!req.download_file_path.is_empty())
	{
		native.download_file_path = to_utf8_file_system_path(req.download_file_path);
	}

	auto response = os::send_request(host->native, native);

	if (response.error != os::web_response_error::none)
	{
		// A transfer that failed or was cut short is no response: status 0 and no body. What it left
		// at the download path is removed, so the caller cannot validate and run whatever is there -
		// an installer from an earlier session, which is exactly the file a download replaces.
		if (!req.download_file_path.is_empty())
		{
			if (response.download_created) delete_file(req.download_file_path);
			df::log(__FUNCTION__, std::format("could not download {}", req.download_file_path.str()));
		}

		return result;
	}

	result.headers = std::move(response.headers);
	result.body = std::move(response.body);
	result.content_type = std::move(response.content_type);
	result.status_code = response.status_code;
	return result;
}