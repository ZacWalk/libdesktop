// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for platform media services and core pixel blending.

#include "libdesktop/platform.h"
#include "test.h"

#include <array>
#include <cstring>
#include <future>
#include <random>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <propidl.h>
#include <wrl/client.h>
#endif

using namespace std::literals;

namespace
{
	std::vector<uint8_t> bmp_fixture()
	{
		// A two-pixel, one-row 24-bit BMP: blue then green, with the required four-byte row padding.
		std::vector<uint8_t> bytes(14 + 40 + 8, 0);
		auto word = [&](const size_t offset, const uint16_t value)
		{
			bytes[offset] = static_cast<uint8_t>(value);
			bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
		};
		auto dword = [&](const size_t offset, const uint32_t value)
		{
			word(offset, static_cast<uint16_t>(value));
			word(offset + 2, static_cast<uint16_t>(value >> 16));
		};
		word(0, 0x4d42);
		dword(2, static_cast<uint32_t>(bytes.size()));
		dword(10, 54);
		dword(14, 40);
		dword(18, 2);
		dword(22, 1);
		word(26, 1);
		word(28, 24);
		dword(34, 8);
		bytes[54] = 0xff;
		bytes[55] = 0;
		bytes[56] = 0;
		bytes[57] = 0;
		bytes[58] = 0xff;
		bytes[59] = 0;
		return bytes;
	}

#ifdef _WIN32
	void fixture_word(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value)
	{
		bytes[offset] = static_cast<uint8_t>(value);
		bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
	}

	void fixture_dword(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value)
	{
		fixture_word(bytes, offset, static_cast<uint16_t>(value));
		fixture_word(bytes, offset + 2, static_cast<uint16_t>(value >> 16));
	}

	constexpr std::array<uint32_t, 6> wic_fixture_colors{
		0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xff00ffff, 0xffff00ff};

	std::vector<uint8_t> wic_tiff_fixture(const uint16_t orientation)
	{
		// A single-strip, uncompressed 2x3 RGB TIFF with an EXIF orientation tag.
		constexpr uint32_t samples_offset = 8 + 2 + 11 * 12 + 4;
		constexpr uint32_t pixels_offset = samples_offset + 6;
		const std::array<std::array<uint32_t, 4>, 11> fields{{
			{256, 3, 1, 2}, {257, 3, 1, 3}, {258, 3, 3, samples_offset},
			{259, 3, 1, 1}, {262, 3, 1, 2}, {273, 4, 1, pixels_offset},
			{274, 3, 1, orientation}, {277, 3, 1, 3}, {278, 4, 1, 3},
			{279, 4, 1, 18}, {284, 3, 1, 1}}};
		std::vector<uint8_t> bytes(pixels_offset + wic_fixture_colors.size() * 3);
		bytes[0] = bytes[1] = 'I';
		fixture_word(bytes, 2, 42);
		fixture_dword(bytes, 4, 8);
		fixture_word(bytes, 8, static_cast<uint16_t>(fields.size()));
		for (size_t i = 0; i < fields.size(); ++i)
		{
			const auto offset = 10 + i * 12;
			fixture_word(bytes, offset, static_cast<uint16_t>(fields[i][0]));
			fixture_word(bytes, offset + 2, static_cast<uint16_t>(fields[i][1]));
			fixture_dword(bytes, offset + 4, fields[i][2]);
			fixture_dword(bytes, offset + 8, fields[i][3]);
		}
		for (size_t i = 0; i < 3; ++i) fixture_word(bytes, samples_offset + i * 2, 8);
		for (size_t i = 0; i < wic_fixture_colors.size(); ++i)
		{
			const auto color = wic_fixture_colors[i];
			bytes[pixels_offset + i * 3] = static_cast<uint8_t>(color >> 16);
			bytes[pixels_offset + i * 3 + 1] = static_cast<uint8_t>(color >> 8);
			bytes[pixels_offset + i * 3 + 2] = static_cast<uint8_t>(color);
		}
		return bytes;
	}
#endif

#ifdef _WIN32
	std::vector<uint8_t> compound_preview_fixture()
	{
		using Microsoft::WRL::ComPtr;
		ComPtr<ILockBytes> bytes;
		if (FAILED(CreateILockBytesOnHGlobal(nullptr, TRUE, &bytes))) return {};
		ComPtr<IStorage> storage;
		if (FAILED(StgCreateDocfileOnILockBytes(bytes.Get(),
		                                      STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0,
		                                      &storage)))
			return {};
		ComPtr<IPropertySetStorage> sets;
		if (FAILED(storage.As(&sets))) return {};
		ComPtr<IPropertyStorage> properties;
		if (FAILED(sets->Create(FMTID_SummaryInformation, nullptr, PROPSETFLAG_DEFAULT,
		                       STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, &properties)))
			return {};

		const HDC dc = CreateMetaFileW(nullptr);
		if (!dc) return {};
		const RECT rectangle{0, 0, 640, 480};
		FillRect(dc, &rectangle, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
		const HMETAFILE metafile = CloseMetaFile(dc);
		if (!metafile) return {};
		const auto metafile_size = GetMetaFileBitsEx(metafile, 0, nullptr);
		std::vector<uint8_t> thumbnail(12 + metafile_size);
		const auto copied = GetMetaFileBitsEx(metafile, metafile_size, thumbnail.data() + 12);
		DeleteMetaFile(metafile);
		if (copied != metafile_size) return {};
		const DWORD format = CF_METAFILEPICT;
		std::memcpy(thumbnail.data(), &format, sizeof(format));
		CLIPDATA clip{static_cast<ULONG>(thumbnail.size() + sizeof(LONG)), -1, thumbnail.data()};
		PROPVARIANT value{};
		PropVariantInit(&value);
		value.vt = VT_CF;
		value.pclipdata = &clip;
		PROPSPEC spec{};
		spec.ulKind = PRSPEC_PROPID;
		spec.propid = PIDSI_THUMBNAIL;
		const auto written = properties->WriteMultiple(1, &spec, &value, PID_FIRST_USABLE);
		value.vt = VT_EMPTY;
		value.pclipdata = nullptr;
		PropVariantClear(&value);
		if (FAILED(written) || FAILED(properties->Commit(STGC_DEFAULT)) || FAILED(storage->Commit(STGC_DEFAULT)))
			return {};
		properties.Reset();
		sets.Reset();
		storage.Reset();

		STATSTG stat{};
		if (FAILED(bytes->Stat(&stat, STATFLAG_NONAME)) ||
			stat.cbSize.QuadPart > static_cast<ULONGLONG>(SIZE_MAX))
			return {};
		HGLOBAL memory{};
		if (FAILED(GetHGlobalFromILockBytes(bytes.Get(), &memory))) return {};
		const auto* input = static_cast<const uint8_t*>(GlobalLock(memory));
		if (!input) return {};
		std::vector<uint8_t> result(input, input + static_cast<size_t>(stat.cbSize.QuadPart));
		GlobalUnlock(memory);
		return result;
	}
#endif

	std::vector<uint8_t> wav_fixture()
	{
		// RIFF/WAVE, PCM 8-bit mono at 8 kHz, two frames.
		std::vector<uint8_t> bytes(46, 0);
		auto word = [&](const size_t offset, const uint16_t value)
		{
			bytes[offset] = static_cast<uint8_t>(value);
			bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
		};
		auto dword = [&](const size_t offset, const uint32_t value)
		{
			word(offset, static_cast<uint16_t>(value));
			word(offset + 2, static_cast<uint16_t>(value >> 16));
		};
		std::memcpy(bytes.data(), "RIFF", 4);
		dword(4, 38);
		std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
		dword(16, 16);
		word(20, 1);
		word(22, 1);
		dword(24, 8000);
		dword(28, 8000);
		word(32, 1);
		word(34, 8);
		std::memcpy(bytes.data() + 36, "data", 4);
		dword(40, 2);
		bytes[44] = 128;
		bytes[45] = 255;
		return bytes;
	}

	void should_match_bgra_blend_backends()
	{
		std::mt19937 generator(0x5eed1234u);
		std::array<uint32_t, 17> input{};
		for (auto& pixel : input) pixel = generator();
		const auto foreground = core::rgba(37, 149, 231, 173);

		auto scalar = input;
		core::blend_constant_bgra(scalar, foreground, core::pixel_backend::scalar);
		auto automatic = input;
		core::blend_constant_bgra(automatic, foreground);
		for (size_t i = 0; i < scalar.size(); ++i)
			assert_equal(scalar[i], automatic[i], "automatic BGRA blend", "backend result");

		if (core::has_sse2())
		{
			auto sse2 = input;
			core::blend_constant_bgra(sse2, foreground, core::pixel_backend::sse2);
			for (size_t i = 0; i < scalar.size(); ++i)
				assert_equal(scalar[i], sse2[i], "SSE2 BGRA blend", "backend result");
		}
		if (core::has_avx2())
		{
			auto avx2 = input;
			core::blend_constant_bgra(avx2, foreground, core::pixel_backend::avx2);
			for (size_t i = 0; i < scalar.size(); ++i)
				assert_equal(scalar[i], avx2[i], "AVX2 BGRA blend", "backend result");
		}

		std::array<uint32_t, 17> first{}, second{}, scalar_interpolation{}, automatic_interpolation{};
		for (auto& pixel : first) pixel = generator();
		for (auto& pixel : second) pixel = generator();
		core::interpolate_bgra(first, second, scalar_interpolation, 93, core::pixel_backend::scalar);
		core::interpolate_bgra(first, second, automatic_interpolation, 93);
		for (size_t i = 0; i < scalar_interpolation.size(); ++i)
			assert_equal(scalar_interpolation[i], automatic_interpolation[i], "automatic interpolation", "backend result");
		if (core::has_sse2())
		{
			std::array<uint32_t, 17> sse2{};
			core::interpolate_bgra(first, second, sse2, 93, core::pixel_backend::sse2);
			for (size_t i = 0; i < scalar_interpolation.size(); ++i)
				assert_equal(scalar_interpolation[i], sse2[i], "SSE2 interpolation", "backend result");
		}
		if (core::has_avx2())
		{
			std::array<uint32_t, 17> avx2{};
			core::interpolate_bgra(first, second, avx2, 93, core::pixel_backend::avx2);
			for (size_t i = 0; i < scalar_interpolation.size(); ++i)
				assert_equal(scalar_interpolation[i], avx2[i], "AVX2 interpolation", "backend result");
		}
		std::array<uint32_t, 19> bounded;
		bounded.fill(0xdeadbeefu);
		core::interpolate_bgra(first, second, std::span<uint32_t>(bounded).first(17), 93,
		                       core::pixel_backend::scalar);
		assert_equal(0xdeadbeefu, bounded[17], "interpolation tail", "destination bounds");
		assert_equal(0xdeadbeefu, bounded[18], "interpolation tail", "destination bounds");

		bool refused = false;
		try
		{
			core::interpolate_bgra({}, {}, {}, 257);
		}
		catch (const std::invalid_argument&)
		{
			refused = true;
		}
		assert_equal(true, refused, "invalid interpolation fraction", "range check");
	}

	void should_keep_sound_creation_honest()
	{
		platform::sound_shutdown();
		assert_equal(true, platform::create_sound_buffer(std::span<const uint8_t>{}) == nullptr, "sound before init", "lifecycle");
		assert_equal(true, platform::create_audio_stream(0, 1) == nullptr, "invalid stream rate", "validation");

		const auto wav = wav_fixture();
		const auto truncated = std::span<const uint8_t>(wav.data(), 20);
		assert_equal(true, platform::create_sound_buffer(truncated) == nullptr, "truncated WAV", "input validation");
		const bool initialized = platform::sound_init();
		if (!initialized)
		{
			assert_equal(true, platform::create_sound_buffer(wav) == nullptr, "missing endpoint", "honest failure");
			assert_equal(true, platform::create_audio_stream(8000, 1) == nullptr, "missing endpoint", "honest failure");
			return;
		}

		auto sound = platform::create_sound_buffer(wav);
		assert_equal(true, sound != nullptr, "valid WAV sound", "mixer creation");
		if (sound)
		{
			sound->set_frequency(8000);
			sound->set_volume(0.5f);
			sound->set_pan(0.0f);
			sound->play();
			sound->stop();
			assert_equal(true, sound->play_position().has_value(), "sound cursor", "mixer state");
		}

		assert_equal(true, platform::create_audio_stream(8000, 0) == nullptr, "zero-channel stream", "validation");
		assert_equal(true, platform::create_audio_stream(8000, 1, 0) == nullptr, "zero-queue stream", "validation");
		auto stream = platform::create_audio_stream(8000, 1, 2);
		assert_equal(true, stream != nullptr, "valid audio stream", "mixer creation");
		if (stream)
		{
			const std::array<int16_t, 2> samples{0, 32767};
			assert_equal(true, stream->write(samples), "stream write", "queue state");
			assert_equal(1, stream->queued_blocks(), "queued stream block", "queue state");
			stream->stop();
			assert_equal(0, stream->queued_blocks(), "stopped stream", "queue state");
		}
		platform::sound_shutdown();
	}

	void should_preserve_public_audio_lifecycle()
	{
		platform::sound_shutdown();
		const auto initialized = platform::sound_init();
		if (!initialized)
		{
			assert_equal(true, platform::create_sound_buffer(wav_fixture()) == nullptr, "audio unavailable honestly");
			assert_equal(true, platform::create_audio_stream(8000, 1) == nullptr, "stream unavailable honestly");
			return;
		}

		auto sound = platform::create_sound_buffer(wav_fixture());
		assert_equal(true, sound != nullptr, "public sound created");
		if (sound)
		{
			sound->set_volume(0.0f);
			sound->set_frequency(8000);
			sound->set_pan(0.0f);
			sound->play(true);
			const auto before = sound->play_position();
			std::this_thread::sleep_for(40ms);
			assert_equal(true, sound->play_position().has_value(), "playing sound keeps a cursor");
			sound->set_play_position(1);
			assert_equal(true, sound->play_position().value_or(0) >= 1, "sound seek");
			sound->stop();
			assert_equal(true, sound->play_position().has_value(), "stopped sound keeps its cursor");
			(void)before;
		}

		auto stream = platform::create_audio_stream(8000, 1, 3);
		assert_equal(true, stream != nullptr, "public stream created");
		if (stream)
		{
			assert_equal(false, stream->write({}), "empty stream write refused");
			const std::array<int16_t, 2> samples{0, 32767};
			assert_equal(true, stream->write(samples), "stream block accepted");
			assert_equal(1, stream->queued_blocks(), "stream block queued");
			stream->stop();
			assert_equal(0, stream->queued_blocks(), "stream stop drains queue");
		}

		// Existing voices remain valid across shutdown; new creation does not.
		sound = platform::create_sound_buffer(wav_fixture());
		assert_equal(true, sound != nullptr, "held sound created");
		if (sound)
		{
			sound->set_volume(0.0f);
			sound->play(true);
			platform::sound_shutdown();
			assert_equal(true, platform::create_sound_buffer(wav_fixture()) == nullptr, "shutdown refuses new sound");
			assert_equal(true, sound->play_position().has_value(), "held sound survives shutdown");
			sound->stop();
		}
	}

	void should_reject_empty_compound_preview()
	{
		const auto result = platform::load_compound_preview(std::span<const uint8_t>{});
#ifdef _WIN32
		assert_equal(true, result.error == platform::image_load_error::invalid_data,
		             "empty compound preview", "input validation");
#else
		assert_equal(true, result.error == platform::image_load_error::unavailable,
		             "Linux compound preview", "honest unavailable service");
#endif
	}

	void should_balance_image_com_apartments()
	{
#ifdef _WIN32
		const auto bytes = wic_tiff_fixture(1);
		auto run_existing_apartment = [&](const DWORD model)
		{
			std::promise<bool> promise;
			auto result = promise.get_future();
			std::thread worker([&bytes, model, promise = std::move(promise)]() mutable
			{
				try
				{
					const auto initialized = CoInitializeEx(nullptr, model);
					if (FAILED(initialized))
					{
						promise.set_value(false);
						return;
					}
					const auto loaded = platform::load_image_memory(
						std::span<const uint8_t>(bytes.data(), bytes.size()), {});
					APTTYPE type{};
					APTTYPEQUALIFIER qualifier{};
					const bool survives = SUCCEEDED(CoGetApartmentType(&type, &qualifier));
					promise.set_value(loaded.success() && survives);
					if (initialized == S_OK || initialized == S_FALSE) CoUninitialize();
				}
				catch (...)
				{
					promise.set_value(false);
				}
			});
			worker.join();
			return result.get();
		};

		std::promise<bool> fresh_promise;
		auto fresh_result = fresh_promise.get_future();
		std::thread fresh([&bytes, promise = std::move(fresh_promise)]() mutable
		{
			try
			{
				const auto loaded = platform::load_image_memory(
					std::span<const uint8_t>(bytes.data(), bytes.size()), {});
				APTTYPE type{};
				APTTYPEQUALIFIER qualifier{};
				const bool still_initialized = SUCCEEDED(CoGetApartmentType(&type, &qualifier));
				promise.set_value(loaded.success() && !still_initialized);
			}
			catch (...)
			{
				promise.set_value(false);
			}
		});
		fresh.join();
		assert_equal(true, fresh_result.get(), "fresh COM apartment", "WIC lifecycle");
		assert_equal(true, run_existing_apartment(COINIT_APARTMENTTHREADED),
		             "existing STA apartment", "WIC lifecycle");
		assert_equal(true, run_existing_apartment(COINIT_MULTITHREADED),
		             "existing MTA apartment", "WIC lifecycle");
#else
		assert_equal(true, true, "Linux COM lifecycle", "not applicable");
#endif
	}

	void should_render_compound_preview()
	{
#ifdef _WIN32
		const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		assert_equal(true, SUCCEEDED(initialized), "compound COM apartment", "preview setup");
		if (FAILED(initialized)) return;
		struct apartment_cleanup
		{
			HRESULT initialized;
			~apartment_cleanup()
			{
				if (initialized == S_OK || initialized == S_FALSE) CoUninitialize();
			}
		} cleanup{initialized};

		const auto bytes = compound_preview_fixture();
		assert_equal(true, !bytes.empty(), "compound fixture", "preview setup");
		if (bytes.empty()) return;
		const std::span<const uint8_t> encoded(bytes.data(), bytes.size());
		const auto decoded = platform::load_compound_preview(encoded);
		assert_equal(true, decoded.success(), "compound preview decode", "WIC metafile service");
		if (decoded.image)
		{
			const auto dimensions = decoded.image->dimensions();
			assert_equal(640, dimensions.cx, "compound width", "WIC metafile service");
			assert_equal(480, dimensions.cy, "compound height", "WIC metafile service");
			assert_equal(640u, decoded.source_width, "compound source width", "metadata");
			assert_equal(480u, decoded.source_height, "compound source height", "metadata");
		}

		platform::image_load_options limited;
		limited.max_decoded_bytes = 640ull * 480 * sizeof(uint32_t) - 1;
		const auto refused = platform::load_compound_preview(encoded, limited);
		assert_equal(true, refused.error == platform::image_load_error::too_large,
		             "compound decoded-byte limit", "bounded preview service");

		limited = {};
		limited.target_width = limited.target_height = 64;
		limited.thumbnail = true;
		const auto thumbnail = platform::load_compound_preview(encoded, limited);
		assert_equal(true, thumbnail.success(), "compound thumbnail", "bounded preview service");
		if (thumbnail.image)
		{
			const auto dimensions = thumbnail.image->dimensions();
			assert_equal(64, dimensions.cx, "thumbnail width", "bounded preview service");
			assert_equal(48, dimensions.cy, "thumbnail height", "bounded preview service");
		}

		limited.cancelled = [] { return true; };
		const auto stopped = platform::load_compound_preview(encoded, limited);
		assert_equal(true, stopped.error == platform::image_load_error::cancelled,
		             "compound cancellation", "bounded preview service");
#else
		const std::array<uint8_t, 1> bytes{0};
		const auto unavailable = platform::load_compound_preview(bytes);
		assert_equal(true, unavailable.error == platform::image_load_error::unavailable,
		             "Linux compound preview", "honest unavailable service");
#endif
	}

	void should_preserve_image_limits_and_probe()
	{
		const auto bytes = bmp_fixture();
		const std::span<const uint8_t> encoded(bytes.data(), bytes.size());
#ifndef _WIN32
		const auto unavailable = platform::load_image_memory(encoded, {});
		assert_equal(true, unavailable.error == platform::image_load_error::unavailable,
		             "Linux image decoder", "honest unavailable service");
		assert_equal(true, platform::installed_image_extensions().empty(), "Linux image extensions",
		             "honest unavailable service");
#else
		const auto loaded = platform::load_image_memory(encoded, {});
		assert_equal(true, loaded.success(), "BMP decode", "WIC image service");
		if (loaded.image)
		{
			const auto dimensions = loaded.image->dimensions();
			assert_equal(2, dimensions.cx, "decoded width", "surface dimensions");
			assert_equal(1, dimensions.cy, "decoded height", "surface dimensions");
		}

#ifdef _WIN32
		const std::array<std::array<size_t, 6>, 8> source_indexes{{
			{0, 1, 2, 3, 4, 5}, {1, 0, 3, 2, 5, 4}, {5, 4, 3, 2, 1, 0},
			{4, 5, 2, 3, 0, 1}, {0, 2, 4, 1, 3, 5}, {4, 2, 0, 5, 3, 1},
			{5, 3, 1, 4, 2, 0}, {1, 3, 5, 0, 2, 4}}};
		for (uint16_t orientation = 1; orientation <= 8; ++orientation)
		{
			const auto oriented_bytes = wic_tiff_fixture(orientation);
			const std::span<const uint8_t> oriented(oriented_bytes.data(), oriented_bytes.size());
			const auto stored = platform::load_image_memory(oriented, {});
			assert_equal(true, stored.success(), "stored TIFF orientation", "WIC metadata");
			if (stored.image)
			{
				const auto dimensions = stored.image->dimensions();
				assert_equal(2, dimensions.cx, "stored TIFF width", "WIC metadata");
				assert_equal(3, dimensions.cy, "stored TIFF height", "WIC metadata");
				assert_equal(orientation, stored.stored_orientation, "stored orientation", "WIC metadata");
			}

			platform::image_load_options upright_options;
			upright_options.apply_orientation = true;
			const auto upright = platform::load_image_memory(oriented, upright_options);
			assert_equal(true, upright.success(), "upright TIFF orientation", "WIC transform");
			if (upright.image)
			{
				const auto dimensions = upright.image->dimensions();
				const auto expected_width = orientation >= 5 ? 3 : 2;
				const auto expected_height = orientation >= 5 ? 2 : 3;
				assert_equal(expected_width, dimensions.cx, "upright TIFF width", "WIC transform");
				assert_equal(expected_height, dimensions.cy, "upright TIFF height", "WIC transform");
				assert_equal(static_cast<uint16_t>(1), upright.stored_orientation, "upright orientation", "WIC transform");
				for (size_t i = 0; i < 6; ++i)
				{
					const auto y = static_cast<int>(i / expected_width);
					const auto x = static_cast<int>(i % expected_width);
					const auto* row = reinterpret_cast<const uint32_t*>(upright.image->pixels_line(y));
					assert_equal(wic_fixture_colors[source_indexes[orientation - 1][i]], row[x],
					             "upright TIFF pixels", "WIC transform");
				}
			}
		}

#endif
		const auto probe = platform::probe_image_memory(encoded);
		assert_equal(true, probe.has_value(), "BMP probe", "WIC image service");
		if (probe)
		{
			assert_equal(2u, probe->width, "probed width", "metadata");
			assert_equal(1u, probe->height, "probed height", "metadata");
			assert_equal(true, probe->container == "bmp", "probed container", "metadata");
		}

		platform::image_load_options limited;
		limited.max_decoded_bytes = 7;
		const auto refused = platform::load_image_memory(encoded, limited);
		assert_equal(true, refused.error == platform::image_load_error::too_large,
		             "decoded-byte limit", "bounded image service");

		platform::image_load_options thumbnail_options;
		thumbnail_options.thumbnail = true;
		thumbnail_options.target_width = thumbnail_options.target_height = 1;
		thumbnail_options.max_decoded_bytes = 4;
		std::vector<std::pair<uint64_t, uint64_t>> progress;
		thumbnail_options.progress = [&](const uint64_t done, const uint64_t total)
		{
			progress.emplace_back(done, total);
		};
		const auto thumbnail = platform::load_image_memory(encoded, thumbnail_options);
		assert_equal(true, thumbnail.success(), "image thumbnail", "bounded image service");
		assert_equal(2, static_cast<int>(progress.size()), "image progress callbacks", "bounded image service");

		platform::image_load_options cancelled;
		cancelled.cancelled = [] { return true; };
		const auto stopped = platform::load_image_memory(encoded, cancelled);
		assert_equal(true, stopped.error == platform::image_load_error::cancelled,
		             "image cancellation", "bounded image service");
#endif
	}
}

void register_platform_media_tests(test_registry& tests)
{
	tests.add("Should match BGRA blend backends"s, should_match_bgra_blend_backends);
	tests.add("Should keep sound creation honest"s, should_keep_sound_creation_honest);
	tests.add("Should preserve public audio lifecycle"s, should_preserve_public_audio_lifecycle);
	tests.add("Should reject empty compound previews"s, should_reject_empty_compound_preview);
	tests.add("Should render compound previews"s, should_render_compound_preview);
	tests.add("Should balance image COM apartments"s, should_balance_image_com_apartments);
	tests.add("Should preserve image limits and probe"s, should_preserve_image_limits_and_probe);
}
