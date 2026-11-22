// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Phones and cameras a cable connects as Windows Portable Devices - MTP or PTP - rather than as drives:
// lists them and the files under their camera folders, and copies those out. Each call opens what it needs and
// lets it go, so nothing is held across the threads callers use.

#include "libdesktop/platform.h"

#include "libdesktop/platform/win32.h"

#include <PortableDeviceApi.h>
#include <PortableDevice.h>
#include <oleauto.h>
#include <propidl.h>

#include <array>

#pragma comment(lib, "PortableDeviceGuids")

namespace
{
	class com_apartment
	{
		HRESULT _result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	public:
		com_apartment() = default;
		com_apartment(const com_apartment&) = delete;
		com_apartment& operator=(const com_apartment&) = delete;

		~com_apartment()
		{
			if (_result == S_OK || _result == S_FALSE) CoUninitialize();
		}

		// A thread already in a single-threaded apartment keeps it: the device objects made here are free-threaded.
		bool ready() const { return _result == S_OK || _result == S_FALSE || _result == RPC_E_CHANGED_MODE; }
	};

	std::string describe(const HRESULT hr)
	{
		return std::format("{:#010x}", static_cast<uint32_t>(hr));
	}

	ComPtr<IPortableDeviceManager> device_manager()
	{
		ComPtr<IPortableDeviceManager> result;
		if (FAILED(CoCreateInstance(CLSID_PortableDeviceManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&result))))
			return nullptr;
		return result;
	}

	std::vector<std::wstring> device_ids(IPortableDeviceManager* manager)
	{
		DWORD count = 0;
		if (FAILED(manager->GetDevices(nullptr, &count)) || count == 0) return {};

		std::vector<PWSTR> ids(count, nullptr);
		if (FAILED(manager->GetDevices(ids.data(), &count))) return {};

		std::vector<std::wstring> result;
		for (DWORD i = 0; i < count; ++i)
		{
			if (!ids[i]) continue;
			result.emplace_back(ids[i]);
			CoTaskMemFree(ids[i]);
		}
		return result;
	}

	std::wstring friendly_name(IPortableDeviceManager* manager, const std::wstring& id)
	{
		DWORD size = 0;
		if (FAILED(manager->GetDeviceFriendlyName(id.c_str(), nullptr, &size)) || size == 0) return {};

		std::wstring result(size, L'\0');
		if (FAILED(manager->GetDeviceFriendlyName(id.c_str(), result.data(), &size))) return {};
		result.resize(wcsnlen(result.c_str(), result.size()));
		return result;
	}

	ComPtr<IPortableDevice> open_device(const std::wstring& id)
	{
		ComPtr<IPortableDeviceValues> client;
		if (FAILED(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client))))
			return nullptr;

		const auto name = core::utf8_to_utf16(core::app_name());
		client->SetStringValue(WPD_CLIENT_NAME, name.c_str());
		client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
		client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
		client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
		client->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION);
		client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS, GENERIC_READ);

		ComPtr<IPortableDevice> device;
		if (FAILED(CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&device))))
			return nullptr;

		auto hr = device->Open(id.c_str(), client.Get());

		// A driver that will not open for reading alone is asked again for its default access.
		if (hr == E_ACCESSDENIED)
		{
			client->RemoveValue(WPD_CLIENT_DESIRED_ACCESS);
			hr = device->Open(id.c_str(), client.Get());
		}

		if (FAILED(hr))
		{
			core::log(__FUNCTION__, std::format("Could not open a portable device ({})", describe(hr)));
			return nullptr;
		}

		return device;
	}

	// A device kept in its own wall-clock time, as a camera is.
	uint64_t date_value(IPortableDeviceValues* values, REFPROPERTYKEY key)
	{
		PROPVARIANT value;
		PropVariantInit(&value);
		uint64_t result = 0;

		if (SUCCEEDED(values->GetValue(key, &value)) && value.vt == VT_DATE)
		{
			SYSTEMTIME st{};
			FILETIME ft{};
			if (VariantTimeToSystemTime(value.date, &st) && SystemTimeToFileTime(&st, &ft))
			{
				result = platform::local_to_utc(ft_to_ts(ft));
			}
		}

		PropVariantClear(&value);
		return result;
	}

	std::wstring string_value(IPortableDeviceValues* values, REFPROPERTYKEY key)
	{
		PWSTR text = nullptr;
		if (FAILED(values->GetStringValue(key, &text)) || !text) return {};
		std::wstring result(text);
		CoTaskMemFree(text);
		return result;
	}

	struct object_values
	{
		std::wstring name;
		GUID content_type = GUID_NULL;
		uint64_t size = 0;
		uint64_t modified = 0;
		uint64_t created = 0;

		bool is_folder() const
		{
			return content_type == WPD_CONTENT_TYPE_FOLDER || content_type == WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT;
		}
	};

	class device_reader
	{
		ComPtr<IPortableDeviceContent> _content;
		ComPtr<IPortableDeviceProperties> _properties;
		ComPtr<IPortableDeviceKeyCollection> _keys;

	public:
		explicit device_reader(IPortableDevice* device)
		{
			if (FAILED(device->Content(&_content)) || FAILED(_content->Properties(&_properties)) ||
			    FAILED(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER,
			                            IID_PPV_ARGS(&_keys))))
			{
				_content.Reset();
				return;
			}

			for (const auto& key : {WPD_OBJECT_NAME, WPD_OBJECT_ORIGINAL_FILE_NAME, WPD_OBJECT_CONTENT_TYPE,
			                        WPD_OBJECT_SIZE, WPD_OBJECT_DATE_MODIFIED, WPD_OBJECT_DATE_CREATED})
			{
				_keys->Add(key);
			}
		}

		bool ready() const { return _content != nullptr; }

		std::vector<std::wstring> children(const wchar_t* parent) const
		{
			std::vector<std::wstring> result;
			ComPtr<IEnumPortableDeviceObjectIDs> objects;
			if (FAILED(_content->EnumObjects(0, parent, nullptr, &objects))) return result;

			std::array<PWSTR, 64> ids{};

			for (;;)
			{
				DWORD fetched = 0;
				const auto hr = objects->Next(static_cast<ULONG>(ids.size()), ids.data(), &fetched);

				for (DWORD i = 0; i < fetched; ++i)
				{
					if (!ids[i]) continue;
					result.emplace_back(ids[i]);
					CoTaskMemFree(ids[i]);
				}

				if (hr != S_OK || fetched == 0) break;
			}

			return result;
		}

		object_values values(const std::wstring& id) const
		{
			object_values result;
			ComPtr<IPortableDeviceValues> values;
			if (FAILED(_properties->GetValues(id.c_str(), _keys.Get(), &values))) return result;

			// The original name keeps the extension the display name may leave off.
			result.name = string_value(values.Get(), WPD_OBJECT_ORIGINAL_FILE_NAME);
			if (result.name.empty()) result.name = string_value(values.Get(), WPD_OBJECT_NAME);

			values->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &result.content_type);

			ULONGLONG size = 0;
			if (SUCCEEDED(values->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, &size))) result.size = size;

			result.created = date_value(values.Get(), WPD_OBJECT_DATE_CREATED);
			result.modified = date_value(values.Get(), WPD_OBJECT_DATE_MODIFIED);
			if (result.modified == 0) result.modified = result.created;
			return result;
		}
	};

	// A mass-storage device is a drive as well, which its path reaches.
	bool is_mass_storage(IPortableDevice* device)
	{
		ComPtr<IPortableDeviceContent> content;
		ComPtr<IPortableDeviceProperties> properties;
		ComPtr<IPortableDeviceKeyCollection> keys;
		ComPtr<IPortableDeviceValues> values;

		if (FAILED(device->Content(&content)) || FAILED(content->Properties(&properties)) ||
		    FAILED(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER,
		                            IID_PPV_ARGS(&keys))))
			return false;

		keys->Add(WPD_DEVICE_PROTOCOL);
		if (FAILED(properties->GetValues(WPD_DEVICE_OBJECT_ID, keys.Get(), &values))) return false;

		const auto protocol = string_value(values.Get(), WPD_DEVICE_PROTOCOL);
		return protocol.size() >= 3 && _wcsnicmp(protocol.c_str(), L"MSC", 3) == 0;
	}

	platform::file_op_result failure(std::string message)
	{
		return {platform::file_op_result_code::FAILED, std::move(message)};
	}

	// The device's times on the copy - created as well as modified, so what files a picture by when it was taken
	// finds the device's time and not the copy's. Either stands in for the other where the device gave one alone.
	void stamp_times(const core::file_path path, const uint64_t created, const uint64_t modified)
	{
		if (created == 0 && modified == 0) return;

		const auto native = platform::to_file_system_path(path);
		const auto h = CreateFileW(native.c_str(), FILE_WRITE_ATTRIBUTES,
		                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		                           FILE_ATTRIBUTE_NORMAL, nullptr);

		if (h == INVALID_HANDLE_VALUE)
		{
			core::log(__FUNCTION__, std::format("Could not stamp the device's times on {}", path.str()));
			return;
		}

		const auto created_ft = ts_to_ft(created != 0 ? created : modified);
		const auto modified_ft = ts_to_ft(modified != 0 ? modified : created);
		if (!SetFileTime(h, &created_ft, nullptr, &modified_ft))
			core::log(__FUNCTION__, std::format("Could not stamp the device's times on {}", path.str()));
		CloseHandle(h);
	}

	// What copying one object came to: copied; unread, which leaves the rest still to copy; or a failure that ends
	// the copy - cancelled, or a copy that could not be written here.
	struct object_copy
	{
		bool unread = false;
		platform::file_op_result result{platform::file_op_result_code::OK};
	};

	object_copy copy_object(IPortableDeviceResources* resources, const platform::portable_copy_t& file,
	                        const core::cancel_token& token)
	{
		if (const auto made = platform::create_folder(file.destination.folder()); made.failed()) return {false, made};

		DWORD optimal = 0;
		ComPtr<IStream> stream;
		const auto id = core::utf8_to_utf16(file.id);
		auto hr = resources->GetStream(id.c_str(), WPD_RESOURCE_DEFAULT, STGM_READ, &optimal, &stream);
		if (FAILED(hr))
			return {true, failure(std::format("Could not read {} from the device ({})", file.destination.name(),
			                                  describe(hr)))};

		auto out = platform::open_file(file.destination, platform::file_open_mode::create_new);
		if (!out)
		{
			return {false,
			        {platform::exists(file.destination) ? platform::file_op_result_code::ALREADY_EXISTS
			                                            : platform::file_op_result_code::FAILED,
			         std::format("Could not create {}", file.destination.str())}};
		}

		std::vector<uint8_t> buffer(std::clamp<DWORD>(optimal, 64 * 1024, 4 * 1024 * 1024));
		object_copy result;

		for (;;)
		{
			if (token.is_cancelled())
			{
				result.result = {platform::file_op_result_code::CANCELLED};
				break;
			}

			ULONG read = 0;
			hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read);

			if (FAILED(hr))
			{
				result = {true, failure(std::format("Could not read {} from the device ({})", file.destination.name(),
				                                    describe(hr)))};
				break;
			}

			if (read == 0) break;

			if (out->write(buffer.data(), read) != read)
			{
				result.result = failure(std::format("Could not write {}", file.destination.str()));
				break;
			}
		}

		if (result.result.success() && !out->flush())
			result.result = failure(std::format("Could not write {}", file.destination.str()));
		out.reset();

		if (result.result.failed()) (void)platform::delete_file(file.destination);
		else stamp_times(file.destination, file.created, file.modified);
		return result;
	}
}

platform::portable_devices_t platform::portable_devices()
{
	const com_apartment com;
	if (!com.ready()) return {};

	const auto manager = device_manager();
	if (!manager) return {};

	portable_devices_t result;

	for (const auto& id : device_ids(manager.Get()))
	{
		// One that will not open - a phone still locked, or asking which way to connect - is still offered, since
		// that is the moment to say it needs unlocking; a drive always opens.
		if (const auto device = open_device(id))
		{
			const auto mass_storage = is_mass_storage(device.Get());
			device->Close();
			if (mass_storage) continue;
		}

		result.emplace_back(core::utf16_to_utf8(id), core::utf16_to_utf8(friendly_name(manager.Get(), id)));
	}

	return result;
}

std::vector<platform::portable_file_t> platform::portable_device_files(const std::string_view device_id,
                                                                       const std::vector<std::string_view>& top_folders,
                                                                       const core::cancel_token& token)
{
	const com_apartment com;
	if (!com.ready()) return {};

	const auto device = open_device(core::utf8_to_utf16(device_id));
	if (!device) return {};

	std::vector<portable_file_t> result;
	const device_reader reader(device.Get());

	if (reader.ready())
	{
		struct folder_to_walk
		{
			std::wstring id;
			std::string path;
		};

		std::vector<folder_to_walk> folders;
		const auto named = [&top_folders](const std::string_view name)
		{
			return std::ranges::any_of(top_folders,
			                           [name](const std::string_view f) { return core::icmp(name, f) == 0; });
		};

		for (const auto& storage : reader.children(WPD_DEVICE_OBJECT_ID))
		{
			const auto storage_name = core::utf16_to_utf8(reader.values(storage).name);

			for (const auto& top : reader.children(storage.c_str()))
			{
				if (token.is_cancelled()) break;

				const auto values = reader.values(top);
				const auto name = core::utf16_to_utf8(values.name);
				if (values.is_folder() && named(name))
					folders.emplace_back(top, std::format("{}{}{}", storage_name, core::preferred_path_sep, name));
			}
		}

		while (!folders.empty() && !token.is_cancelled())
		{
			const auto folder = std::move(folders.back());
			folders.pop_back();

			for (const auto& child : reader.children(folder.id.c_str()))
			{
				if (token.is_cancelled()) break;

				const auto values = reader.values(child);
				auto name = core::utf16_to_utf8(values.name);

				if (values.is_folder())
				{
					folders.emplace_back(child, std::format("{}{}{}", folder.path, core::preferred_path_sep, name));
				}
				else if (!name.empty())
				{
					result.emplace_back(core::utf16_to_utf8(child), folder.path, std::move(name),
					                    core::file_size(values.size), values.modified, values.created);
				}
			}
		}
	}

	device->Close();
	return result;
}

platform::portable_copy_result platform::copy_from_portable_device(const std::string_view device_id,
                                                                   const std::vector<portable_copy_t>& files,
                                                                   const core::cancel_token& token,
                                                                   const std::function<void(size_t tried)>& progress)
{
	portable_copy_result result;
	const com_apartment com;

	if (!com.ready())
	{
		result.stopped = failure("The device could not be reached from this thread.");
		return result;
	}

	const auto device = open_device(core::utf8_to_utf16(device_id));

	if (!device)
	{
		result.stopped = failure("The device could not be opened. It may be locked or disconnected.");
		return result;
	}

	ComPtr<IPortableDeviceContent> content;
	ComPtr<IPortableDeviceResources> resources;

	if (FAILED(device->Content(&content)) || FAILED(content->Transfer(&resources)))
	{
		device->Close();
		result.stopped = failure("The device's files could not be read.");
		return result;
	}

	for (size_t i = 0; i < files.size(); ++i)
	{
		if (token.is_cancelled())
		{
			result.stopped = {file_op_result_code::CANCELLED};
			break;
		}

		auto copied = copy_object(resources.Get(), files[i], token);

		if (copied.unread)
		{
			result.unreadable.emplace_back(i, std::move(copied.result.error_message));
		}
		else if (copied.result.failed())
		{
			result.stopped = std::move(copied.result);
			break;
		}
		else
		{
			result.copied.emplace_back(files[i].destination);
		}

		if (progress) progress(i + 1);
	}

	device->Close();
	return result;
}
