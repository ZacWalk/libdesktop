// Windows SDK image adapter. This translation unit is deliberately separate from the window
// entry point so bitmap consumers do not pull app_init/app_idle/app_destroy into their link.

#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
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

	pf::bitmap_load_result com_failure_result(const HRESULT result)
	{
		pf::bitmap_load_result failure;
		failure.error = pf::bitmap_load_error::unavailable;
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
		auto native_name = pf::utf8_to_utf16(path);
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

	std::optional<pf::bitmap_probe> probe_decoder(IWICBitmapDecoder* decoder, const bool cap_source)
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
		pf::bitmap_probe result;
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

	pf::bitmap_ptr decode(IWICBitmapDecoder* decoder, const pf::bitmap_load_options& options,
	                      const bool cap_source, uint32_t& original_width, uint32_t& original_height,
	                      uint16_t& stored_orientation, pf::bitmap_load_error& error)
	{
		ComPtr<IWICBitmapFrameDecode> frame;
		UINT source_width = 0, source_height = 0;
		if (FAILED(decoder->GetFrame(0, &frame)) ||
		    FAILED(frame->GetSize(&source_width, &source_height)) ||
		    source_width == 0 || source_height == 0)
			return {};
		if (cap_source && !options.thumbnail && (source_width > 100000 || source_height > 100000))
		{
			error = pf::bitmap_load_error::too_large;
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
			error = pf::bitmap_load_error::cancelled;
			return {};
		}
		if (options.max_pixels &&
		    static_cast<uint64_t>(source_width) * source_height > options.max_pixels)
		{
			error = pf::bitmap_load_error::too_large;
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
			error = pf::bitmap_load_error::too_large;
			return {};
		}
		const auto decoded_bytes = static_cast<uint64_t>(target_width) * target_height * sizeof(uint32_t);
		if (options.max_decoded_bytes && decoded_bytes > options.max_decoded_bytes)
		{
			error = pf::bitmap_load_error::too_large;
			return {};
		}
		if (options.progress) options.progress(0, decoded_bytes);

		auto factory = make_factory();
		if (!factory) { error = pf::bitmap_load_error::unavailable; return {}; }
		ComPtr<IWICBitmapScaler> scaler;
		IWICBitmapSource* source = frame.Get();
		if (target_width != source_width || target_height != source_height)
		{
			if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(
				scaler->Initialize(frame.Get(), target_width, target_height,
				                   WICBitmapInterpolationModeFant)))
			{
				error = pf::bitmap_load_error::invalid_data;
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
				error = pf::bitmap_load_error::invalid_data;
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
			error = pf::bitmap_load_error::invalid_data;
			return {};
		}
		UINT width = 0, height = 0;
		if (FAILED(converter->GetSize(&width, &height)) || !width || !height)
		{
			error = pf::bitmap_load_error::invalid_data;
			return {};
		}
		if (width > UINT_MAX / 4 || static_cast<uint64_t>(width) * height * 4 > UINT_MAX)
		{
			error = pf::bitmap_load_error::too_large;
			return {};
		}
		const auto count = static_cast<uint64_t>(width) * height;
		if (count > SIZE_MAX / sizeof(uint32_t))
		{
			error = pf::bitmap_load_error::too_large;
			return {};
		}
		std::vector<uint32_t> pixels;
		if (count > pixels.max_size())
		{
			error = pf::bitmap_load_error::too_large;
			return {};
		}
		if (options.cancelled && options.cancelled())
		{
			error = pf::bitmap_load_error::cancelled;
			return {};
		}
		try
		{
			pixels.resize(static_cast<size_t>(count));
			const auto hr = converter->CopyPixels(nullptr, width * 4, width * height * 4,
			                                     reinterpret_cast<BYTE*>(pixels.data()));
			if (FAILED(hr))
			{
				error = hr == E_OUTOFMEMORY ? pf::bitmap_load_error::too_large
				                          : pf::bitmap_load_error::invalid_data;
				return {};
			}
		}
		catch (const std::bad_alloc&)
		{
			error = pf::bitmap_load_error::too_large;
			return {};
		}
		if (options.progress) options.progress(count * sizeof(uint32_t), decoded_bytes);
		if (options.cancelled && options.cancelled())
		{
			error = pf::bitmap_load_error::cancelled;
			return {};
		}
		try
		{
			return std::make_shared<pf::bitmap>(static_cast<int>(width), static_cast<int>(height),
			                                    std::move(pixels));
		}
		catch (const std::bad_alloc&)
		{
			error = pf::bitmap_load_error::too_large;
			return {};
		}
	}

	pf::bitmap_load_result finish_bitmap_load(pf::bitmap_load_result result)
	{
		if (!result.image && result.error == pf::bitmap_load_error::none)
			result.error = pf::bitmap_load_error::invalid_data;
		if (!result.diagnostic.empty()) return result;
		switch (result.error)
		{
		case pf::bitmap_load_error::none: break;
		case pf::bitmap_load_error::unavailable:
			result.diagnostic = "Windows Imaging Component is unavailable.";
			break;
		case pf::bitmap_load_error::invalid_data:
			result.diagnostic = "WIC could not decode the image data.";
			break;
		case pf::bitmap_load_error::too_large:
			result.diagnostic = "The image exceeds the configured source or decoded-memory limit.";
			break;
		case pf::bitmap_load_error::io_error:
			result.diagnostic = "WIC could not open the image.";
			break;
		case pf::bitmap_load_error::cancelled:
			result.diagnostic = "Image decoding was cancelled.";
			break;
		}
		return result;
	}
}

pf::bitmap_ptr pf::load_bitmap_file(const file_path& path)
{
	com_apartment apartment;
	if (!apartment.ready()) return {};
	auto factory = make_factory();
	if (!factory) return {};
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(open_decoder(factory.Get(), path.view(), {}, &decoder))) return {};
	pf::bitmap_load_options options;
	pf::bitmap_load_error error = pf::bitmap_load_error::none;
	uint32_t width = 0, height = 0;
	uint16_t orientation_value = 1;
	return decode(decoder.Get(), options, false, width, height, orientation_value, error);
}

static pf::bitmap_load_result load_file_result_impl(const std::string_view path,
                                                    const pf::bitmap_load_options options)
{
	pf::bitmap_load_result result;
	auto factory = make_factory();
	if (!factory)
	{
		result.error = pf::bitmap_load_error::unavailable;
		return finish_bitmap_load(std::move(result));
	}
	ComPtr<IWICBitmapDecoder> decoder;
	const auto opened = open_decoder(factory.Get(), path, {}, &decoder, true);
	if (FAILED(opened))
	{
		result.error = pf::bitmap_load_error::io_error;
		result.diagnostic = std::format("WIC could not open the image (0x{:08x}).",
			static_cast<uint32_t>(opened));
		return finish_bitmap_load(std::move(result));
	}
	pf::bitmap_load_error error = pf::bitmap_load_error::none;
	result.image = decode(decoder.Get(), options, !options.thumbnail, result.source_width,
	                      result.source_height, result.stored_orientation, error);
	result.error = error;
	return finish_bitmap_load(std::move(result));
}

static pf::bitmap_load_result load_memory_result_impl(const std::span<const uint8_t> data,
                                                      const pf::bitmap_load_options options,
                                                      const bool cap_source = true)
{
	pf::bitmap_load_result result;
	if (data.size() > UINT_MAX || data.empty())
	{
		result.error = data.size() > UINT_MAX ? pf::bitmap_load_error::too_large
		                                    : pf::bitmap_load_error::invalid_data;
		return finish_bitmap_load(std::move(result));
	}
	auto factory = make_factory();
	if (!factory)
	{
		result.error = pf::bitmap_load_error::unavailable;
		return finish_bitmap_load(std::move(result));
	}
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(open_decoder(factory.Get(), {}, data, &decoder)))
	{
		result.error = pf::bitmap_load_error::invalid_data;
		return finish_bitmap_load(std::move(result));
	}
	pf::bitmap_load_error error = pf::bitmap_load_error::none;
	result.image = decode(decoder.Get(), options, cap_source && !options.thumbnail, result.source_width,
	                      result.source_height, result.stored_orientation, error);
	result.error = error;
	return finish_bitmap_load(std::move(result));
}

	pf::bitmap_ptr pf::load_bitmap_memory(const uint8_t* data, const size_t size)
	{
		if (!data || size == 0 || size > UINT_MAX) return {};
		com_apartment apartment;
		if (!apartment.ready()) return {};
		auto result = load_memory_result_impl({data, size}, {}, false);
		return std::move(result.image);
	}

	pf::bitmap_load_result pf::load_bitmap_file_result(const file_path& path,
	                                                   const bitmap_load_options options)
	{
		com_apartment apartment;
		if (!apartment.ready()) return com_failure_result(apartment.result());
		return load_file_result_impl(path.view(), options);
	}

	std::optional<pf::bitmap_probe> pf::probe_bitmap_file(const file_path& path)
	{
		com_apartment apartment;
		if (!apartment.ready()) return {};
		auto factory = make_factory();
		if (!factory) return {};
		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(open_decoder(factory.Get(), path.view(), {}, &decoder, true))) return {};
		return probe_decoder(decoder.Get(), true);
	}

	pf::bitmap_load_result pf::load_bitmap_memory_result(const std::span<const uint8_t> data,
	                                                     const bitmap_load_options options)
	{
		com_apartment apartment;
		if (!apartment.ready()) return com_failure_result(apartment.result());
		return load_memory_result_impl(data, options);
	}

	std::optional<pf::bitmap_probe> pf::probe_bitmap_memory(const std::span<const uint8_t> data)
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

	std::vector<std::string> pf::installed_bitmap_extensions()
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
							if (!extension.empty()) result.push_back(pf::utf16_to_utf8(extension));
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

	pf::bitmap_ptr pf::load_bitmap_named_resource(const std::string_view name, const std::string_view type)
	{
		const auto wname = utf8_to_utf16(name);
		const auto wtype = utf8_to_utf16(type);
		const auto instance = GetModuleHandleW(nullptr);
		const auto resource = FindResourceW(instance, wname.c_str(), wtype.c_str());
		if (!resource) return {};
		const auto data = LoadResource(instance, resource);
		const auto pointer = data ? LockResource(data) : nullptr;
		const auto size = SizeofResource(instance, resource);
		return pointer && size ? load_bitmap_memory(static_cast<const uint8_t*>(pointer), size) : bitmap_ptr{};
	}
