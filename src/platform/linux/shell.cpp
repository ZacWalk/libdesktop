// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Linux shell locks, SDL message boxes, and honest unavailable shell services.

#include "libdesktop/platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <sys/file.h>
#include <unistd.h>

namespace
{
	uint64_t name_hash(const std::string_view name)
	{
		uint64_t result = 1469598103934665603ull;
		for (const auto byte : name)
		{
			result ^= static_cast<unsigned char>(byte);
			result *= 1099511628211ull;
		}
		return result;
	}

	std::string lock_path(const std::string_view name, const bool named)
	{
		const auto* runtime = std::getenv("XDG_RUNTIME_DIR");
		const std::string root = runtime && *runtime ? runtime : "/tmp";
		return std::format("{}/libdesktop-{}-{:016x}.lock", root, named ? "named" : "instance", name_hash(name));
	}

	class linux_instance_lock final : public platform::instance_lock
	{
		int _fd = -1;

	public:
		explicit linux_instance_lock(const int fd) : _fd(fd) {}
		~linux_instance_lock() override
		{
			if (_fd >= 0)
			{
				flock(_fd, LOCK_UN);
				close(_fd);
			}
		}
	};

	platform::instance_lock_result take_lock(const std::string_view name, const bool named)
	{
		const auto path = lock_path(name, named);
		const auto fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (fd < 0)
			return {nullptr, false, std::format("Cannot open instance lock '{}': {}", path, std::strerror(errno))};
		if (flock(fd, LOCK_EX | LOCK_NB) != 0)
		{
			const auto error = errno;
			close(fd);
			if (error == EWOULDBLOCK || error == EAGAIN) return {nullptr, true, {}};
			return {nullptr, false, std::format("Cannot lock instance '{}': {}", path, std::strerror(error))};
		}
		return {std::make_unique<linux_instance_lock>(fd), false, {}};
	}

	SDL_Window* native_owner(platform::window* owner)
	{
		if (!owner) return nullptr;
		const auto handle = owner->native_handle();
		if (const auto value = std::any_cast<SDL_Window*>(&handle)) return *value;
		return nullptr;
	}

	SDL_MessageBoxFlags message_box_flags(const platform::message_box_icon icon)
	{
		switch (icon)
		{
		case platform::message_box_icon::info: return SDL_MESSAGEBOX_INFORMATION;
		case platform::message_box_icon::warning: return SDL_MESSAGEBOX_WARNING;
		case platform::message_box_icon::error: return SDL_MESSAGEBOX_ERROR;
		case platform::message_box_icon::none: return 0;
		case platform::message_box_icon::question: return SDL_MESSAGEBOX_INFORMATION;
		}
		return 0;
	}

	void add_button(std::vector<SDL_MessageBoxButtonData>& buttons, std::vector<std::string>& labels,
	                const int id, const std::string_view text, const SDL_MessageBoxButtonFlags flags = 0)
	{
		labels.emplace_back(text);
		buttons.push_back({flags, id, labels.back().c_str()});
	}

	platform::message_box_result result_for_id(const int id)
	{
		switch (id)
		{
		case 1: return platform::message_box_result::ok;
		case 2: return platform::message_box_result::cancel;
		case 3: return platform::message_box_result::yes;
		case 4: return platform::message_box_result::no;
		case 5: return platform::message_box_result::retry;
		default: return platform::message_box_result::none;
		}
	}
}

platform::tray_service_ptr platform::create_tray_service(platform::tray_icon_image, std::string_view,
                                                          platform::tray_callbacks)
{
	return nullptr;
}

void platform::test_set_tray_publication_failure(bool)
{
}

platform::instance_lock_result platform::try_lock_instance(const std::string_view name)
{
	if (name.empty() || name.size() > 128 ||
		!std::all_of(name.begin(), name.end(), [](const char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		}))
		return {nullptr, false, "Instance lock name validation failed"};
	return take_lock(name, false);
}

platform::instance_lock_result platform::try_lock_named_instance(const std::string_view name)
{
	if (name.empty() || name.size() > 256 || name.find('\0') != std::string_view::npos)
		return {nullptr, false, "Named instance lock validation failed"};
	return take_lock(name, true);
}

bool platform::register_instance_signal(std::string_view, std::function<void()>)
{
	return false;
}

bool platform::signal_existing_instance(std::string_view)
{
	return false;
}

std::shared_ptr<platform::spell_checker> platform::create_spell_checker(std::string_view)
{
	return nullptr;
}

bool platform::begin_file_drag(platform::window*, std::span<const core::file_path>)
{
	return false;
}

platform::message_box_result platform::message_box(platform::window* owner, const std::string_view text,
                                                    const std::string_view title, const message_box_style style)
{
	std::vector<SDL_MessageBoxButtonData> buttons;
	std::vector<std::string> labels;
	buttons.reserve(3);
	labels.reserve(3);
	switch (style.buttons)
	{
	case message_box_buttons::ok:
		add_button(buttons, labels, 1, "OK", SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT);
		break;
	case message_box_buttons::ok_cancel:
		add_button(buttons, labels, 1, "OK", SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT);
		add_button(buttons, labels, 2, "Cancel", SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT);
		break;
	case message_box_buttons::yes_no:
		add_button(buttons, labels, 3, "Yes", SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT);
		add_button(buttons, labels, 4, "No", 0);
		break;
	case message_box_buttons::yes_no_cancel:
		add_button(buttons, labels, 3, "Yes", SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT);
		add_button(buttons, labels, 4, "No", 0);
		add_button(buttons, labels, 2, "Cancel", SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT);
		break;
	case message_box_buttons::retry_cancel:
		add_button(buttons, labels, 5, "Retry", SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT);
		add_button(buttons, labels, 2, "Cancel", SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT);
		break;
	}
	const std::string title_text(title);
	const std::string message_text(text);
	const SDL_MessageBoxData data{
		message_box_flags(style.icon), native_owner(owner), title_text.c_str(), message_text.c_str(),
		static_cast<int>(buttons.size()), buttons.data(), nullptr};
	int selected = 0;
	if (!SDL_ShowMessageBox(&data, &selected)) return message_box_result::none;
	return result_for_id(selected);
}

int platform::show_choice(platform::window* owner, const choice_definition& definition)
{
	if (definition.buttons.empty()) return 0;
	std::vector<SDL_MessageBoxButtonData> buttons;
	std::vector<std::string> labels;
	buttons.reserve(definition.buttons.size());
	labels.reserve(definition.buttons.size());
	for (const auto& [id, label] : definition.buttons)
	{
		const auto flags = id == definition.default_id ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
		add_button(buttons, labels, id, label, flags);
	}
	std::string message = definition.heading;
	if (!definition.message.empty())
	{
		if (!message.empty()) message += "\n\n";
		message += definition.message;
	}
	const SDL_MessageBoxData data{
		definition.warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION,
		native_owner(owner), definition.title.c_str(), message.c_str(), static_cast<int>(buttons.size()), buttons.data(), nullptr};
	int selected = 0;
	return SDL_ShowMessageBox(&data, &selected) ? selected : 0;
}

std::optional<std::map<uint32_t, platform::modal_value>> platform::show_modal_dialog(
	platform::window*, platform::modal_definition)
{
	return std::nullopt;
}

bool platform::browse_for_folder(platform::window*, core::folder_path&, std::string_view)
{
	return false;
}

bool platform::prompt_for_save_path(platform::window*, core::file_path&)
{
	return false;
}

bool platform::prompt_for_open_paths(platform::window*, std::vector<core::file_path>&,
                                     const std::vector<file_dialog_filter>&, bool, std::string_view)
{
	return false;
}

bool platform::prompt_for_save_path(platform::window*, core::file_path&,
                                    const std::vector<file_dialog_filter>&, std::string_view)
{
	return false;
}

