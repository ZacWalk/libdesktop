// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows Imaging Component image loading with bounded decode policies.

#include "libdesktop/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace
{
	class com_apartment
	{
		HRESULT result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	public:
		~com_apartment()
		{
			if (result_ == S_OK || result_ == S_FALSE) CoUninitialize();
		}

		[[nodiscard]] bool ready() const
		{
			return result_ == S_OK || result_ == S_FALSE || result_ == RPC_E_CHANGED_MODE;
		}

		[[nodiscard]] HRESULT result() const { return result_; }
	};

	std::string com_failure_diagnostic(const HRESULT result)
	{
		return std::format("COM initialization failed (0x{:08x}).",
		                   static_cast<uint32_t>(result));
	}

	platform::image_load_result com_failure_result(const HRESULT result)
	{
		platform::image_load_result failure;
		failure.error = platform::image_load_error::unavailable;
		failure.diagnostic = com_failure_diagnostic(result);
		return failure;
	}

	ComPtr<IWICImagingFactory> make_factory()
	{
		ComPtr<IWICImagingFactory> result;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		                            IID_PPV_ARGS(&result))))
			result.Reset();
		return result;
	}

	HRESULT open_decoder(IWICImagingFactory* factory, const std::string_view path,
	                     const std::span<const uint8_t> bytes, IWICBitmapDecoder** decoder,
	                     const bool literal_name = false)
	{
		if (!bytes.empty())
		{
			if (bytes.size() > UINT_MAX) return E_INVALIDARG;
			ComPtr<IWICStream> stream;
			auto hr = factory->CreateStream(&stream);
			if (SUCCEEDED(hr))
				hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
				                                  static_cast<DWORD>(bytes.size()));
			if (SUCCEEDED(hr))
				hr = factory->CreateDecoderFromStream(stream.Get(), nullptr,
				                                      WICDecodeMetadataCacheOnLoad, decoder);
			return hr;
		}
		auto native_name = core::utf8_to_utf16(path);
		if (literal_name && !native_name.starts_with(LR"(\\?\)") &&
		    !native_name.starts_with(LR"(\\.\)"))
		{
			std::error_code error;
			// Expanding the whole filename with Win32 would strip literal trailing dots/spaces.
			const std::filesystem::path requested(native_name);
			std::filesystem::path full;
			if (requested.is_absolute())
				full = requested;
			else if (requested.has_root_name())
			{
				const auto drive_folder = std::filesystem::absolute(requested.root_name() / L".", error);
				full = drive_folder / requested.relative_path();
			}
			else
				full = std::filesystem::current_path(error) / requested;
			if (error || full.empty()) return E_INVALIDARG;
			native_name = full.lexically_normal().native();
			std::ranges::replace(native_name, L'/', L'\\');
			const auto root_length = full.root_path().native().size();
			while (native_name.size() > root_length && native_name.back() == L'\\')
				native_name.pop_back();
			native_name = native_name.starts_with(LR"(\\)")
				? LR"(\\?\UNC\)" + native_name.substr(2) : LR"(\\?\)" + native_name;
		}
		return factory->CreateDecoderFromFilename(native_name.c_str(), nullptr,
		                                          GENERIC_READ, literal_name ? WICDecodeMetadataCacheOnDemand
		                                                                     : WICDecodeMetadataCacheOnLoad,
		                                          decoder);
	}

	uint16_t orientation(IWICBitmapFrameDecode* frame)
	{
		ComPtr<IWICMetadataQueryReader> reader;
		if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
		// The canonical property accounts for intrinsic container transforms, including HEIF.
		for (const auto* query : {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}",
		                         L"/ifd/{ushort=274}"})
		{
			PROPVARIANT value;
			PropVariantInit(&value);
			const auto hr = reader->GetMetadataByName(query, &value);
			uint32_t result = 0;
			if (SUCCEEDED(hr))
			{
				if (value.vt == VT_UI2) result = value.uiVal;
				else if (value.vt == VT_UI4) result = value.ulVal;
			}
			PropVariantClear(&value);
			if (result >= 1 && result <= 8) return static_cast<uint16_t>(result);
		}
		return 1;
	}

	WICBitmapTransformOptions transform(const uint16_t value)
	{
		switch (value)
		{
		case 2: return WICBitmapTransformFlipHorizontal;
		case 3: return WICBitmapTransformRotate180;
		case 4: return WICBitmapTransformFlipVertical;
		case 5: return static_cast<WICBitmapTransformOptions>(
			WICBitmapTransformRotate90 | WICBitmapTransformFlipVertical);
		case 6: return WICBitmapTransformRotate90;
		case 7: return static_cast<WICBitmapTransformOptions>(
			WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
		case 8: return WICBitmapTransformRotate270;
		default: return WICBitmapTransformRotate0;
		}
	}

	bool sideways(const uint16_t value) { return value >= 5 && value <= 8; }

	std::optional<platform::image_probe> probe_decoder(IWICBitmapDecoder* decoder, const bool cap_source)
	{
		UINT frames = 0, width = 0, height = 0;
		if (FAILED(decoder->GetFrameCount(&frames)) || frames == 0) return {};
		ComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
		    width == 0 || height == 0)
			return {};
		if (cap_source && (width > 100000 || height > 100000)) return {};

		GUID container = {};
		if (FAILED(decoder->GetContainerFormat(&container))) return {};
		platform::image_probe result;
		result.width = width;
		result.height = height;
		result.frame_count = frames;
		result.orientation = orientation(frame.Get());
		if (IsEqualGUID(container, GUID_ContainerFormatPng))
		{
			result.container = "png";
			result.has_alpha = true;
		}
		else if (IsEqualGUID(container, GUID_ContainerFormatJpeg))
			result.container = "jpeg";
		else if (IsEqualGUID(container, GUID_ContainerFormatGif))
		{
			result.container = "gif";
			result.has_alpha = true;
		}
		else if (IsEqualGUID(container, GUID_ContainerFormatBmp))
			result.container = "bmp";
		else if (IsEqualGUID(container, GUID_ContainerFormatTiff))
			result.container = "tiff";
		else if (IsEqualGUID(container, GUID_ContainerFormatIco))
		{
			result.container = "ico";
			result.has_alpha = true;
		}
		else
			result.container = "unknown";
		return result;
	}

	core::surface_ptr decode(IWICBitmapDecoder* decoder, const platform::image_load_options& options,
	                      const bool cap_source, uint32_t& original_width, uint32_t& original_height,
	                      uint16_t& stored_orientation, platform::image_load_error& error)
	{
		ComPtr<IWICBitmapFrameDecode> frame;
		UINT source_width = 0, source_height = 0;
		if (FAILED(decoder->GetFrame(0, &frame)) ||
		    FAILED(frame->GetSize(&source_width, &source_height)) ||
		    source_width == 0 || source_height == 0)
			return {};
		if (cap_source && !options.thumbnail && (source_width > 100000 || source_height > 100000))
		{
			error = platform::image_load_error::too_large;
			return {};
		}
		original_width = source_width;
		original_height = source_height;
		const auto raw_orientation = orientation(frame.Get());
		stored_orientation = options.apply_orientation ? 1 : raw_orientation;
		if (options.apply_orientation && sideways(raw_orientation))
			std::swap(original_width, original_height);
		if (options.cancelled && options.cancelled())
		{
			error = platform::image_load_error::cancelled;
			return {};
		}
		if (options.max_pixels &&
		    static_cast<uint64_t>(source_width) * source_height > options.max_pixels)
		{
			error = platform::image_load_error::too_large;
			return {};
		}

		const bool turn = options.apply_orientation && sideways(raw_orientation);
		UINT target_width = source_width, target_height = source_height;
		if (options.target_width && options.target_height)
		{
			const double upright_width = turn ? source_height : source_width;
			const double upright_height = turn ? source_width : source_height;
			if (options.thumbnail)
			{
				const auto scale = std::min(1.0, std::min(
					static_cast<double>(options.target_width) / upright_width,
					static_cast<double>(options.target_height) / upright_height));
				const auto width = std::max(1u, static_cast<UINT>(upright_width * scale));
				const auto height = std::max(1u, static_cast<UINT>(upright_height * scale));
				target_width = turn ? height : width;
				target_height = turn ? width : height;
			}
			else
			{
				const auto fit = std::min(
					static_cast<double>(options.target_width) / upright_width,
					static_cast<double>(options.target_height) / upright_height);
				const auto divisor = fit <= .25 ? 4u : fit <= .5 ? 2u : 1u;
				target_width = (source_width + divisor - 1) / divisor;
				target_height = (source_height + divisor - 1) / divisor;
			}
		}
		if (target_height && static_cast<uint64_t>(target_width) >
			std::numeric_limits<uint64_t>::max() /
				(static_cast<uint64_t>(target_height) * sizeof(uint32_t)))
		{
			error = platform::image_load_error::too_large;
			return {};
		}
		const auto decoded_bytes = static_cast<uint64_t>(target_width) * target_height * sizeof(uint32_t);
		if (options.max_decoded_bytes && decoded_bytes > options.max_decoded_bytes)
		{
			error = platform::image_load_error::too_large;
			return {};
		}
		if (options.progress) options.progress(0, decoded_bytes);

		auto factory = make_factory();
		if (!factory) { error = platform::image_load_error::unavailable; return {}; }
		ComPtr<IWICBitmapScaler> scaler;
		IWICBitmapSource* source = frame.Get();
		if (target_width != source_width || target_height != source_height)
		{
			if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(
				scaler->Initialize(frame.Get(), target_width, target_height,
				                   WICBitmapInterpolationModeFant)))
			{
				error = platform::image_load_error::invalid_data;
				return {};
			}
			source = scaler.Get();
		}
		ComPtr<IWICBitmapFlipRotator> rotator;
		if (options.apply_orientation && transform(raw_orientation) != WICBitmapTransformRotate0)
		{
			if (FAILED(factory->CreateBitmapFlipRotator(&rotator)) ||
			    FAILED(rotator->Initialize(source, transform(raw_orientation))))
			{
				error = platform::image_load_error::invalid_data;
				return {};
			}
			source = rotator.Get();
		}
		ComPtr<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(&converter)) ||
		    FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
		                                 WICBitmapDitherTypeNone, nullptr, 0,
		                                 WICBitmapPaletteTypeCustom)))
		{
			error = platform::image_load_error::invalid_data;
			return {};
		}
		UINT width = 0, height = 0;
		if (FAILED(converter->GetSize(&width, &height)) || !width || !height)
		{
			error = platform::image_load_error::invalid_data;
			return {};
		}
		if (width > UINT_MAX / 4 || static_cast<uint64_t>(width) * height * 4 > UINT_MAX)
		{
			error = platform::image_load_error::too_large;
			return {};
		}
		const auto count = static_cast<uint64_t>(width) * height;
		if (count > SIZE_MAX / sizeof(uint32_t))
		{
			error = platform::image_load_error::too_large;
			return {};
		}
		if (options.cancelled && options.cancelled())
		{
			error = platform::image_load_error::cancelled;
			return {};
		}
		// Decoded straight into the surface it is returned in, so the decode limits measure the one copy
		// there is.
		auto result = std::make_shared<core::surface>();
		try
		{
			if (!result->alloc(static_cast<int>(width), static_cast<int>(height), core::texture_format::ARGB,
			                   core::orientation::top_left))
			{
				error = platform::image_load_error::too_large;
				return {};
			}
			const auto stride = result->stride();
			if (stride > UINT_MAX || static_cast<uint64_t>(stride) * height > UINT_MAX)
			{
				error = platform::image_load_error::too_large;
				return {};
			}
			const auto hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(stride * height),
			                                     result->pixels());
			if (FAILED(hr))
			{
				error = hr == E_OUTOFMEMORY ? platform::image_load_error::too_large
				                          : platform::image_load_error::invalid_data;
				return {};
			}
		}
		catch (const std::bad_alloc&)
		{
			error = platform::image_load_error::too_large;
			return {};
		}
		if (options.progress) options.progress(count * sizeof(uint32_t), decoded_bytes);
		if (options.cancelled && options.cancelled())
		{
			error = platform::image_load_error::cancelled;
			return {};
		}
		return result;
	}

	platform::image_load_result finish_image_load(platform::image_load_result result)
	{
		if (!result.image && result.error == platform::image_load_error::none)
			result.error = platform::image_load_error::invalid_data;
		if (!result.diagnostic.empty()) return result;
		switch (result.error)
		{
		case platform::image_load_error::none: break;
		case platform::image_load_error::unavailable:
			result.diagnostic = "Windows Imaging Component is unavailable.";
			break;
		case platform::image_load_error::invalid_data:
			result.diagnostic = "WIC could not decode the image data.";
			break;
		case platform::image_load_error::too_large:
			result.diagnostic = "The image exceeds the configured source or decoded-memory limit.";
			break;
		case platform::image_load_error::io_error:
			result.diagnostic = "WIC could not open the image.";
			break;
		case platform::image_load_error::cancelled:
			result.diagnostic = "Image decoding was cancelled.";
			break;
		}
		return result;
	}
}

core::surface_ptr platform::load_image_file(const core::file_path& path)
{
	com_apartment apartment;
	if (!apartment.ready()) return {};
	auto factory = make_factory();
	if (!factory) return {};
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(open_decoder(factory.Get(), path.pack(), {}, &decoder))) return {};
	platform::image_load_options options;
	platform::image_load_error error = platform::image_load_error::none;
	uint32_t width = 0, height = 0;
	uint16_t orientation_value = 1;
	return decode(decoder.Get(), options, false, width, height, orientation_value, error);
}

static platform::image_load_result load_file_result_impl(const std::string_view path,
                                                    const platform::image_load_options options)
{
	platform::image_load_result result;
	auto factory = make_factory();
	if (!factory)
	{
		result.error = platform::image_load_error::unavailable;
		return finish_image_load(std::move(result));
	}
	ComPtr<IWICBitmapDecoder> decoder;
	const auto opened = open_decoder(factory.Get(), path, {}, &decoder, true);
	if (FAILED(opened))
	{
		result.error = platform::image_load_error::io_error;
		result.diagnostic = std::format("WIC could not open the image (0x{:08x}).",
			static_cast<uint32_t>(opened));
		return finish_image_load(std::move(result));
	}
	platform::image_load_error error = platform::image_load_error::none;
	result.image = decode(decoder.Get(), options, !options.thumbnail, result.source_width,
	                      result.source_height, result.stored_orientation, error);
	result.error = error;
	return finish_image_load(std::move(result));
}

static platform::image_load_result load_memory_result_impl(const std::span<const uint8_t> data,
                                                      const platform::image_load_options options,
                                                      const bool cap_source = true)
{
	platform::image_load_result result;
	if (data.size() > UINT_MAX || data.empty())
	{
		result.error = data.size() > UINT_MAX ? platform::image_load_error::too_large
		                                    : platform::image_load_error::invalid_data;
		return finish_image_load(std::move(result));
	}
	auto factory = make_factory();
	if (!factory)
	{
		result.error = platform::image_load_error::unavailable;
		return finish_image_load(std::move(result));
	}
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(open_decoder(factory.Get(), {}, data, &decoder)))
	{
		result.error = platform::image_load_error::invalid_data;
		return finish_image_load(std::move(result));
	}
	platform::image_load_error error = platform::image_load_error::none;
	result.image = decode(decoder.Get(), options, cap_source && !options.thumbnail, result.source_width,
	                      result.source_height, result.stored_orientation, error);
	result.error = error;
	return finish_image_load(std::move(result));
}

	core::surface_ptr platform::load_image_memory(const std::span<const uint8_t> data)
	{
		if (data.empty() || data.size() > UINT_MAX) return {};
		com_apartment apartment;
		if (!apartment.ready()) return {};
		auto result = load_memory_result_impl(data, {}, false);
		return std::move(result.image);
	}

	platform::image_load_result platform::load_image_file(const core::file_path& path,
	                                                      const platform::image_load_options options)
	{
		com_apartment apartment;
		if (!apartment.ready()) return com_failure_result(apartment.result());
		return load_file_result_impl(path.pack(), options);
	}

	std::optional<platform::image_probe> platform::probe_image_file(const core::file_path& path)
	{
		com_apartment apartment;
		if (!apartment.ready()) return {};
		auto factory = make_factory();
		if (!factory) return {};
		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(open_decoder(factory.Get(), path.pack(), {}, &decoder, true))) return {};
		return probe_decoder(decoder.Get(), true);
	}

	platform::image_load_result platform::load_image_memory(const std::span<const uint8_t> data,
	                                                        const platform::image_load_options options)
	{
		com_apartment apartment;
		if (!apartment.ready()) return com_failure_result(apartment.result());
		return load_memory_result_impl(data, options);
	}

	std::optional<platform::image_probe> platform::probe_image_memory(const std::span<const uint8_t> data)
	{
		com_apartment apartment;
		if (!apartment.ready()) return {};
		if (data.empty() || data.size() > UINT_MAX) return {};
		auto factory = make_factory();
		if (!factory) return {};
		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(open_decoder(factory.Get(), {}, data, &decoder))) return {};
		return probe_decoder(decoder.Get(), true);
	}

	std::vector<std::string> platform::installed_image_extensions()
	{
		com_apartment apartment;
		if (!apartment.ready()) return {};
		std::vector<std::string> result;
		auto factory = make_factory();
		if (!factory) return result;
		ComPtr<IEnumUnknown> enumerator;
		if (FAILED(factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault,
		                                              &enumerator)))
			return result;
		IUnknown* component = nullptr;
		while (enumerator->Next(1, &component, nullptr) == S_OK)
		{
			ComPtr<IWICBitmapCodecInfo> codec;
			if (SUCCEEDED(component->QueryInterface(IID_PPV_ARGS(&codec)))
				&& codec)
			{
				UINT length = 0;
				if (SUCCEEDED(codec->GetFileExtensions(0, nullptr, &length)) && length > 1)
				{
					std::wstring extensions(length, L'\0');
					if (SUCCEEDED(codec->GetFileExtensions(length, extensions.data(), &length)))
					{
						size_t start = 0;
						while (start < extensions.size())
						{
							const auto end = extensions.find_first_of(L",;\0", start);
							auto extension = extensions.substr(start, end - start);
							extension.erase(std::remove_if(extension.begin(), extension.end(),
								[](const wchar_t c) { return c == L'*' || std::iswspace(c); }),
								extension.end());
							std::ranges::transform(extension, extension.begin(),
								[](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
							if (!extension.empty()) result.push_back(core::utf16_to_utf8(extension));
							if (end == std::wstring::npos) break;
							start = end + 1;
						}
					}
				}
			}
			component->Release();
			component = nullptr;
		}
		std::sort(result.begin(), result.end());
		result.erase(std::unique(result.begin(), result.end()), result.end());
		return result;
	}
