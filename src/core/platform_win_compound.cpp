// Windows OLE compound-document preview adapter.  The service extracts only
// SummaryInformation/PIDSI_THUMBNAIL and rasterises its WMF/EMF payload; no
// application or legacy imaging type crosses this boundary.

#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

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

	pf::bitmap_load_result failure(const pf::bitmap_load_error error, std::string diagnostic)
	{
		pf::bitmap_load_result result;
		result.error = error;
		result.diagnostic = std::move(diagnostic);
		return result;
	}

	std::string hresult_diagnostic(const char* operation, const HRESULT result)
	{
		return std::format("{} failed (0x{:08x}).", operation,
		                   static_cast<std::uint32_t>(result));
	}

	struct global_memory
	{
		HGLOBAL handle{};

		explicit global_memory(const HGLOBAL value) : handle(value) {}

		~global_memory()
		{
			if (handle) GlobalFree(handle);
		}

		global_memory(const global_memory&) = delete;
		global_memory& operator=(const global_memory&) = delete;
	};

	struct propvariant
	{
		PROPVARIANT value{};

		propvariant() { PropVariantInit(&value); }
		~propvariant() { PropVariantClear(&value); }

		propvariant(const propvariant&) = delete;
		propvariant& operator=(const propvariant&) = delete;
	};

	struct screen_dc
	{
		HDC handle{};

		screen_dc() : handle(GetDC(nullptr)) {}

		~screen_dc()
		{
			if (handle) ReleaseDC(nullptr, handle);
		}

		screen_dc(const screen_dc&) = delete;
		screen_dc& operator=(const screen_dc&) = delete;
	};

	struct metafile
	{
		HMETAFILE handle{};

		explicit metafile(const HMETAFILE value) : handle(value) {}

		~metafile()
		{
			if (handle) DeleteMetaFile(handle);
		}

		metafile(const metafile&) = delete;
		metafile& operator=(const metafile&) = delete;
	};

	struct enhanced_metafile
	{
		HENHMETAFILE handle{};

		explicit enhanced_metafile(const HENHMETAFILE value) : handle(value) {}

		~enhanced_metafile()
		{
			if (handle) DeleteEnhMetaFile(handle);
		}

		enhanced_metafile(const enhanced_metafile&) = delete;
		enhanced_metafile& operator=(const enhanced_metafile&) = delete;
	};

	class dib_surface
	{
	public:
		dib_surface() = default;
		~dib_surface()
		{
			if (old_) SelectObject(dc_, old_);
			if (bitmap_) DeleteObject(bitmap_);
			if (dc_) DeleteDC(dc_);
		}

		dib_surface(const dib_surface&) = delete;
		dib_surface& operator=(const dib_surface&) = delete;

		bool create(const int width, const int height)
		{
			if (dc_ || width <= 0 || height <= 0 ||
				static_cast<std::uint64_t>(width) * height > 2000ull * 2000ull)
				return false;
			width_ = width;
			height_ = height;
			dc_ = CreateCompatibleDC(nullptr);
			if (!dc_) return false;

			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = width;
			info.bmiHeader.biHeight = -height;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			void* pixels = nullptr;
			bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
			if (!bitmap_ || !pixels) return false;
			pixels_ = static_cast<std::uint32_t*>(pixels);
			old_ = SelectObject(dc_, bitmap_);
			if (!old_ || old_ == HGDI_ERROR)
			{
				old_ = nullptr;
				return false;
			}
			return true;
		}

		[[nodiscard]] HDC dc() const { return dc_; }

		void fill_white()
		{
			std::fill_n(pixels_, static_cast<size_t>(width_) * height_, 0xffffffffu);
		}

		bool play(const HMETAFILE source, const int logical_width, const int logical_height)
		{
			if (!source || logical_width <= 0 || logical_height <= 0) return false;
			fill_white();
			if (!SetMapMode(dc_, MM_ANISOTROPIC) ||
				!SetWindowExtEx(dc_, logical_width, logical_height, nullptr) ||
				!SetViewportExtEx(dc_, width_, height_, nullptr) ||
				!PlayMetaFile(dc_, source))
				return false;
			return GdiFlush() != FALSE;
		}

		bool play(const HENHMETAFILE source)
		{
			if (!source) return false;
			fill_white();
			const RECT rectangle{0, 0, width_, height_};
			if (!PlayEnhMetaFile(dc_, source, &rectangle)) return false;
			return GdiFlush() != FALSE;
		}

		std::vector<std::uint32_t> copy_pixels() const
		{
			std::vector<std::uint32_t> result(
				pixels_, pixels_ + static_cast<size_t>(width_) * height_);
			for (auto& pixel : result) pixel |= 0xff000000u;
			return result;
		}

	private:
		int width_{};
		int height_{};
		HDC dc_{};
		HBITMAP bitmap_{};
		HGDIOBJ old_{};
		std::uint32_t* pixels_{};
	};

#pragma pack(push, 2)
	struct packed_meta_header
	{
		std::uint16_t mm;
		std::int16_t x_ext;
		std::int16_t y_ext;
		std::uint16_t reserved;
	};

	struct aldus_header
	{
		std::uint32_t key;
		std::uint16_t hmf;
		std::int16_t left;
		std::int16_t top;
		std::int16_t right;
		std::int16_t bottom;
		std::uint16_t inch;
		std::uint32_t reserved;
		std::uint16_t checksum;
	};
#pragma pack(pop)

	static_assert(sizeof(packed_meta_header) == 8);
	static_assert(sizeof(aldus_header) == 22);

	constexpr std::uint32_t aldus_signature = 0x9ac6cdd7u;
	constexpr int max_metafile_extent = 2000;

	enum class metafile_kind
	{
		wmf,
		aldus_wmf,
		emf
	};

	struct metafile_source
	{
		metafile_kind kind{};
		const std::uint8_t* bytes{};
		size_t size{};
		int logical_width{};
		int logical_height{};
		std::uint32_t original_width{};
		std::uint32_t original_height{};
		int render_width{};
		int render_height{};
	};

	bool read_aldus_header(const std::span<const std::uint8_t> bytes, aldus_header& result)
	{
		if (bytes.size() < sizeof(result)) return false;
		std::memcpy(&result, bytes.data(), sizeof(result));
		return result.key == aldus_signature;
	}

	std::uint32_t clamp_source_dimension(const std::int64_t value)
	{
		if (value <= 0) return 0;
		return value > std::numeric_limits<std::uint32_t>::max()
			? std::numeric_limits<std::uint32_t>::max()
			: static_cast<std::uint32_t>(value);
	}

	int extent_for_render(const std::uint32_t value)
	{
		return static_cast<int>((std::min)(value, static_cast<std::uint32_t>(max_metafile_extent)));
	}

	bool mul_div_positive(const std::int64_t value, const std::int64_t multiplier,
	                      const std::int64_t divisor, std::int64_t& result)
	{
		if (value <= 0 || multiplier <= 0 || divisor <= 0) return false;
		if (value > (std::numeric_limits<std::int64_t>::max)() / multiplier) return false;
		const auto product = value * multiplier;
		if (product <= 0) return false;
		result = product / divisor;
		if (product % divisor >= divisor / 2 + divisor % 2) ++result;
		return result > 0;
	}

	bool describe_aldus(const std::span<const std::uint8_t> bytes, metafile_source& result)
	{
		aldus_header header{};
		if (!read_aldus_header(bytes, header) || header.inch == 0 ||
			bytes.size() <= sizeof(header))
			return false;

		screen_dc dc;
		if (!dc.handle) return false;
		const int horz_resolution = GetDeviceCaps(dc.handle, HORZRES);
		const int horz_size = GetDeviceCaps(dc.handle, HORZSIZE);
		const int vert_resolution = GetDeviceCaps(dc.handle, VERTRES);
		const int vert_size = GetDeviceCaps(dc.handle, VERTSIZE);
		if (horz_resolution <= 0 || horz_size <= 0 || vert_resolution <= 0 || vert_size <= 0)
			return false;

		const auto width_units = static_cast<std::int64_t>(header.right) - header.left;
		const auto height_units = static_cast<std::int64_t>(header.bottom) - header.top;
		std::int64_t x_ext = 0, y_ext = 0, width = 0, height = 0;
		if (!mul_div_positive(width_units, 2540, header.inch, x_ext) ||
			!mul_div_positive(height_units, 2540, header.inch, y_ext) ||
			!mul_div_positive(x_ext, horz_resolution,
			                 static_cast<std::int64_t>(horz_size) * 100, width) ||
			!mul_div_positive(y_ext, vert_resolution,
			                 static_cast<std::int64_t>(vert_size) * 100, height))
			return false;
		if (x_ext > std::numeric_limits<int>::max() || y_ext > std::numeric_limits<int>::max())
			return false;

		result.kind = metafile_kind::aldus_wmf;
		result.bytes = bytes.data() + sizeof(header);
		result.size = bytes.size() - sizeof(header);
		result.logical_width = static_cast<int>(x_ext);
		result.logical_height = static_cast<int>(y_ext);
		result.original_width = clamp_source_dimension(width);
		result.original_height = clamp_source_dimension(height);
		result.render_width = extent_for_render(result.original_width);
		result.render_height = extent_for_render(result.original_height);
		return result.original_width != 0 && result.original_height != 0 &&
			result.render_width != 0 && result.render_height != 0;
	}

	bool describe_emf(const std::span<const std::uint8_t> bytes, metafile_source& result)
	{
		if (bytes.size() < sizeof(ENHMETAHEADER) || bytes.size() > UINT_MAX)
			return false;
		ENHMETAHEADER header{};
		std::memcpy(&header, bytes.data(), sizeof(header));
		if (header.dSignature != ENHMETA_SIGNATURE) return false;

		std::int64_t width = 640, height = 480;
		const auto frame_width = static_cast<std::int64_t>(header.rclFrame.right) -
			header.rclFrame.left;
		const auto frame_height = static_cast<std::int64_t>(header.rclFrame.bottom) -
			header.rclFrame.top;
		if (frame_width > 0 && frame_height > 0 && header.szlDevice.cx > 0 &&
			header.szlDevice.cy > 0 && header.szlMillimeters.cx > 0 &&
			header.szlMillimeters.cy > 0)
		{
			std::int64_t computed_width = 0, computed_height = 0;
			if (mul_div_positive(frame_width, header.szlDevice.cx,
			                    static_cast<std::int64_t>(header.szlMillimeters.cx) * 100,
			                    computed_width))
				width = computed_width;
			if (mul_div_positive(frame_height, header.szlDevice.cy,
			                     static_cast<std::int64_t>(header.szlMillimeters.cy) * 100,
			                     computed_height))
				height = computed_height;
		}

		result.kind = metafile_kind::emf;
		result.bytes = bytes.data();
		result.size = bytes.size();
		result.logical_width = extent_for_render(clamp_source_dimension(width));
		result.logical_height = extent_for_render(clamp_source_dimension(height));
		result.original_width = clamp_source_dimension(width);
		result.original_height = clamp_source_dimension(height);
		result.render_width = extent_for_render(result.original_width);
		result.render_height = extent_for_render(result.original_height);
		return result.original_width != 0 && result.original_height != 0 &&
			result.render_width != 0 && result.render_height != 0;
	}

	bool describe_metafile(const std::span<const std::uint8_t> bytes, metafile_source& result)
	{
		if (describe_emf(bytes, result)) return true;
		if (describe_aldus(bytes, result)) return true;
		if (bytes.empty() || bytes.size() > UINT_MAX) return false;
		result.kind = metafile_kind::wmf;
		result.bytes = bytes.data();
		result.size = bytes.size();
		result.logical_width = 640;
		result.logical_height = 480;
		result.original_width = 640;
		result.original_height = 480;
		result.render_width = 640;
		result.render_height = 480;
		return true;
	}

	bool target_size(const metafile_source& source, const pf::bitmap_load_options& options,
	                 int& width, int& height)
	{
		width = source.render_width;
		height = source.render_height;
		if (options.target_width && options.target_height)
		{
			if (options.thumbnail)
			{
				const auto scale = (std::min)(1.0,
					(std::min)(static_cast<double>(options.target_width) / source.original_width,
					           static_cast<double>(options.target_height) / source.original_height));
				width = (std::max)(1, static_cast<int>((std::min)(
					static_cast<double>(max_metafile_extent), source.original_width * scale)));
				height = (std::max)(1, static_cast<int>((std::min)(
					static_cast<double>(max_metafile_extent), source.original_height * scale)));
			}
			else
			{
				const auto fit = (std::min)(
					static_cast<double>(options.target_width) / source.original_width,
					static_cast<double>(options.target_height) / source.original_height);
				const auto divisor = fit <= .25 ? 4u : fit <= .5 ? 2u : 1u;
				width = static_cast<int>((source.render_width + divisor - 1) / divisor);
				height = static_cast<int>((source.render_height + divisor - 1) / divisor);
			}
		}
		return width > 0 && height > 0 &&
			static_cast<std::uint64_t>(width) * height <=
				static_cast<std::uint64_t>(max_metafile_extent) * max_metafile_extent;
	}

	bool extract_metafile(const std::span<const std::uint8_t> data,
	                      std::vector<std::uint8_t>& metafile_bytes, std::string& diagnostic)
	{
		if (data.empty())
		{
			diagnostic = "The compound document is empty.";
			return false;
		}
		if (data.size() > INT_MAX)
		{
			diagnostic = "The compound document exceeds the input limit.";
			return false;
		}

		global_memory memory{GlobalAlloc(GMEM_MOVEABLE, data.size())};
		if (!memory.handle)
		{
			diagnostic = "The compound document input could not be allocated.";
			return false;
		}
		auto* destination = static_cast<std::uint8_t*>(GlobalLock(memory.handle));
		if (!destination)
		{
			diagnostic = "The compound document input could not be locked.";
			return false;
		}
		std::memcpy(destination, data.data(), data.size());
		GlobalUnlock(memory.handle);

		ComPtr<ILockBytes> lock_bytes;
		auto hr = CreateILockBytesOnHGlobal(memory.handle, TRUE, &lock_bytes);
		if (FAILED(hr))
		{
			diagnostic = hresult_diagnostic("CreateILockBytesOnHGlobal", hr);
			return false;
		}
		memory.handle = nullptr;

		ComPtr<IStorage> storage;
		hr = StgOpenStorageOnILockBytes(lock_bytes.Get(), nullptr,
		                                STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE,
		                                nullptr, 0, &storage);
		if (FAILED(hr))
		{
			diagnostic = hresult_diagnostic("StgOpenStorageOnILockBytes", hr);
			return false;
		}

		ComPtr<IPropertySetStorage> property_sets;
		hr = storage.As(&property_sets);
		if (FAILED(hr))
		{
			diagnostic = hresult_diagnostic("IStorage::QueryInterface", hr);
			return false;
		}
		ComPtr<IPropertyStorage> properties;
		hr = property_sets->Open(FMTID_SummaryInformation,
		                         STGM_READ | STGM_SHARE_EXCLUSIVE, &properties);
		if (FAILED(hr))
		{
			diagnostic = hresult_diagnostic("IPropertySetStorage::Open", hr);
			return false;
		}

		PROPSPEC spec{};
		spec.ulKind = PRSPEC_PROPID;
		spec.propid = PIDSI_THUMBNAIL;
		propvariant value;
		hr = properties->ReadMultiple(1, &spec, &value.value);
		if (FAILED(hr))
		{
			diagnostic = hresult_diagnostic("IPropertyStorage::ReadMultiple", hr);
			return false;
		}
		if (value.value.vt != VT_CF || !value.value.pclipdata ||
			value.value.pclipdata->cbSize < sizeof(value.value.pclipdata->ulClipFmt))
		{
			diagnostic = "The compound document has no metafile thumbnail.";
			return false;
		}
		const auto clip_size = static_cast<size_t>(
			value.value.pclipdata->cbSize - sizeof(value.value.pclipdata->ulClipFmt));
		constexpr size_t metafile_offset = sizeof(packed_meta_header) + sizeof(std::uint32_t);
		if (!value.value.pclipdata->pClipData || clip_size <= metafile_offset)
		{
			diagnostic = "The compound document thumbnail is incomplete.";
			return false;
		}
		const auto* bytes = value.value.pclipdata->pClipData + metafile_offset;
		metafile_bytes.assign(bytes, bytes + clip_size - metafile_offset);
		return true;
	}

	bool render_metafile(const metafile_source& source, const int width, const int height,
	                     std::vector<std::uint32_t>& pixels)
	{
		if (source.size > UINT_MAX) return false;
		dib_surface surface;
		if (!surface.create(width, height)) return false;

		if (source.kind == metafile_kind::emf)
		{
			enhanced_metafile emf{SetEnhMetaFileBits(static_cast<UINT>(source.size), source.bytes)};
			if (!emf.handle || !surface.play(emf.handle)) return false;
		}
		else
		{
			metafile wmf{SetMetaFileBitsEx(static_cast<UINT>(source.size), source.bytes)};
			if (!wmf.handle || !surface.play(wmf.handle, source.logical_width, source.logical_height))
				return false;
		}
		pixels = surface.copy_pixels();
		return true;
	}
}

pf::bitmap_load_result pf::load_compound_preview_result(
	std::span<const std::uint8_t> data, const bitmap_load_options options)
{
	com_apartment apartment;
	if (!apartment.ready())
		return failure(bitmap_load_error::unavailable,
			std::format("COM initialization failed (0x{:08x}).",
			            static_cast<std::uint32_t>(apartment.result())));
	if (data.empty())
		return failure(bitmap_load_error::invalid_data,
			"The compound document is empty.");
	if (options.cancelled && options.cancelled())
		return failure(bitmap_load_error::cancelled,
			"Compound preview decoding was cancelled.");

	std::vector<std::uint8_t> metafile_bytes;
	std::string diagnostic;
	try
	{
		if (!extract_metafile(data, metafile_bytes, diagnostic))
			return failure(data.size() > INT_MAX ? bitmap_load_error::too_large :
				bitmap_load_error::invalid_data, std::move(diagnostic));

		metafile_source source;
		if (!describe_metafile(metafile_bytes, source))
			return failure(bitmap_load_error::invalid_data,
				"The compound document thumbnail is not a valid metafile.");
		if (options.max_pixels &&
			static_cast<std::uint64_t>(source.original_width) * source.original_height >
				options.max_pixels)
			return failure(bitmap_load_error::too_large,
				"The compound preview exceeds the source pixel limit.");

		int width = 0, height = 0;
		if (!target_size(source, options, width, height))
			return failure(bitmap_load_error::too_large,
				"The compound preview exceeds the configured extent.");
		const auto decoded_bytes = static_cast<std::uint64_t>(width) * height * sizeof(std::uint32_t);
		if (options.max_decoded_bytes && decoded_bytes > options.max_decoded_bytes)
			return failure(bitmap_load_error::too_large,
				"The compound preview exceeds the decoded-memory limit.");
		if (options.progress) options.progress(0, decoded_bytes);
		if (options.cancelled && options.cancelled())
			return failure(bitmap_load_error::cancelled,
				"Compound preview decoding was cancelled.");

		std::vector<std::uint32_t> pixels;
		if (!render_metafile(source, width, height, pixels))
			return failure(bitmap_load_error::invalid_data,
				"The compound document thumbnail could not be rasterised.");
		if (options.progress) options.progress(decoded_bytes, decoded_bytes);
		if (options.cancelled && options.cancelled())
			return failure(bitmap_load_error::cancelled,
				"Compound preview decoding was cancelled.");

		pf::bitmap_load_result result;
		result.source_width = source.original_width;
		result.source_height = source.original_height;
		result.stored_orientation = 1;
		result.image = std::make_shared<bitmap>(width, height, std::move(pixels));
		return result;
	}
	catch (const std::bad_alloc&)
	{
		return failure(bitmap_load_error::too_large,
			"The compound preview exceeds the decoded-memory limit.");
	}
}
