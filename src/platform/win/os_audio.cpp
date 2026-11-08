// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Windows audio of the os:: services - a WASAPI shared-mode stream opened in its endpoint's own mix
// format, the playback endpoints, and the MMCSS class an audio thread joins. The stream is the one
// platform::audio_output plays through, which Diffractor's media player and Movie preview drive;
// libdesktop's mixer plays its sounds and streams through it as well.

#include "os.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <avrt.h>
#include <wrl/client.h>

#pragma comment(lib, "avrt.lib")

#include "os_text.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <shared_mutex>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr REFERENCE_TIME reftimes_per_sec = 10000000;

	constexpr GUID ks_subtype_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
	constexpr GUID ks_subtype_ieee_float = {
		0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
	};

	// PKEY_Device_FriendlyName, defined here so no library has to.
	constexpr PROPERTYKEY device_friendly_name = {
		{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
	};

	os::audio_sample_format calc_sample_fmt(const WAVEFORMATEX* waveformat)
	{
		if (!waveformat)
		{
			return os::audio_sample_format::none;
		}

		if (waveformat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && waveformat->wBitsPerSample == 32)
		{
			return os::audio_sample_format::float32;
		}
		if (waveformat->wFormatTag == WAVE_FORMAT_PCM && waveformat->wBitsPerSample == 16)
		{
			return os::audio_sample_format::int16;
		}
		if (waveformat->wFormatTag == WAVE_FORMAT_PCM && waveformat->wBitsPerSample == 32)
		{
			return os::audio_sample_format::int32;
		}
		if (waveformat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
		{
			if (waveformat->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				// Invalid extensible format
				return os::audio_sample_format::none;
			}

			const auto* ext = std::bit_cast<const WAVEFORMATEXTENSIBLE*>(waveformat);

			if (memcmp(&ext->SubFormat, &ks_subtype_ieee_float, sizeof(GUID)) == 0 && waveformat->wBitsPerSample == 32)
			{
				return os::audio_sample_format::float32;
			}
			if (memcmp(&ext->SubFormat, &ks_subtype_pcm, sizeof(GUID)) == 0 && waveformat->wBitsPerSample == 16)
			{
				return os::audio_sample_format::int16;
			}
			if (memcmp(&ext->SubFormat, &ks_subtype_pcm, sizeof(GUID)) == 0 && waveformat->wBitsPerSample == 32)
			{
				return os::audio_sample_format::int32;
			}
		}

		return os::audio_sample_format::none;
	}

	os::audio_format describe(const WAVEFORMATEX* format)
	{
		os::audio_format result;

		if (format)
		{
			result.sample_rate = format->nSamplesPerSec;
			result.sample_format = calc_sample_fmt(format);
			result.channels = format->nChannels;

			if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
				format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				result.channel_mask = std::bit_cast<const WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask;
			}
		}

		return result;
	}

	class wasapi_sound final : public os::audio_output
	{
		mutable std::shared_mutex _rw;

		ComPtr<IMMDevice> _device;
		ComPtr<IAudioClient> _audio;
		ComPtr<IAudioRenderClient> _render;
		ComPtr<ISimpleAudioVolume> _sav;
		ComPtr<IAudioClock> _clock;
		WAVEFORMATEX* _pwfx = nullptr;
		uint32_t _buffer_frame_count = 0;
		// Auto-reset and unsignalled: the engine signals it each time it wants frames.
		HANDLE _data_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		// Zero asks for the engine's minimum ring, refilled by a thread waiting on _data_event. Fixed at
		// construction: it decides how the stream is initialised, not how it is used.
		const double _ring_seconds = 0.0;
		// Read without a lock by is_device_lost()/is_stopped() and written from the device
		// operations below, so these are atomic rather than plain bools.
		std::atomic<bool> _device_lost = false;
		std::atomic<bool> _playing = false;
		double _vol = -1;

	public:
		explicit wasapi_sound(const double ring_seconds) : _ring_seconds(ring_seconds)
		{
		}

		~wasapi_sound() override
		{
			clear();
			if (_data_event) CloseHandle(_data_event);
		}

		wasapi_sound(const wasapi_sound&) = delete;
		wasapi_sound& operator=(const wasapi_sound&) = delete;

		void clear()
		{
			std::unique_lock lock(_rw);

			if (_audio)
			{
				_audio->Stop();
			}

			_audio.Reset();
			_device.Reset();
			_render.Reset();
			_sav.Reset();
			_clock.Reset();
			_buffer_frame_count = 0;
			if (_data_event) ResetEvent(_data_event);

			if (_pwfx)
			{
				CoTaskMemFree(_pwfx);
				_pwfx = nullptr;
			}

			_device_lost = false;
			_playing = false;
			_vol = -1;
		}

		bool is_device_lost() const override
		{
			return _device_lost;
		}

		bool activate_device(const ComPtr<IMMDevice>& device)
		{
			if (!device)
			{
				return false;
			}

			bool success = false;

			ComPtr<IAudioClient> audio;
			ComPtr<IAudioRenderClient> render;
			ComPtr<ISimpleAudioVolume> sav;
			ComPtr<IAudioClock> clock;
			WAVEFORMATEX* pwfx_temp = nullptr;
			uint32_t buffer_frame_count = 0;

			auto hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio);

			if (SUCCEEDED(hr))
			{
				hr = audio->GetMixFormat(&pwfx_temp);

				if (SUCCEEDED(hr) && pwfx_temp)
				{
					// The engine's minimum ring is a device period or two, which only a thread waiting on
					// the buffer event can keep full. A caller refilling on its own schedule asks for a
					// ring long enough to cover the gap between its refills, polls GetCurrentPadding
					// through write(), and so has no event: an event-driven shared stream takes its
					// buffer from the engine whatever duration is asked for.
					const auto event_driven = _ring_seconds <= 0.0;
					const DWORD flags = AUDCLNT_STREAMFLAGS_NOPERSIST |
						(event_driven ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0);
					const auto duration = event_driven
						                      ? REFERENCE_TIME{0}
						                      : static_cast<REFERENCE_TIME>(_ring_seconds * reftimes_per_sec);

					hr = audio->Initialize(
						AUDCLNT_SHAREMODE_SHARED,
						flags,
						duration,
						0,
						pwfx_temp,
						nullptr);

					if (SUCCEEDED(hr) && event_driven)
					{
						hr = audio->SetEventHandle(_data_event);
					}

					if (SUCCEEDED(hr))
					{
						success = SUCCEEDED(audio->GetBufferSize(&buffer_frame_count)) &&
							SUCCEEDED(audio->GetService(__uuidof(IAudioRenderClient), &render)) &&
							SUCCEEDED(audio->GetService(__uuidof(ISimpleAudioVolume), &sav)) &&
							SUCCEEDED(audio->GetService(__uuidof(IAudioClock), &clock));
					}
				}

				if (success)
				{
					// Clean up existing format before assigning new one
					if (_pwfx)
					{
						CoTaskMemFree(_pwfx);
					}

					_device = device;
					_audio = audio;
					_render = render;
					_sav = sav;
					_clock = clock;
					_pwfx = pwfx_temp;
					_buffer_frame_count = buffer_frame_count;
					pwfx_temp = nullptr; // Ownership transferred
				}
				else if (pwfx_temp)
				{
					// Clean up on failure
					CoTaskMemFree(pwfx_temp);
				}
			}

			return success;
		}

		bool init(const std::string_view device_id)
		{
			clear();

			std::unique_lock lock(_rw);
			bool success = false;
			ComPtr<IMMDeviceEnumerator> enumerator;

			auto hr = CoCreateInstance(
				__uuidof(MMDeviceEnumerator), nullptr,
				CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				&enumerator);

			if (SUCCEEDED(hr))
			{
				ComPtr<IMMDevice> device;

				if (device_id.empty())
				{
					hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
				}
				else
				{
					const auto w = os::detail::widen(device_id);
					hr = enumerator->GetDevice(w.c_str(), &device);

					if (FAILED(hr))
					{
						hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
					}
				}

				if (SUCCEEDED(hr))
				{
					success = activate_device(device);

					if (!success)
					{
						// Try with a different device
						hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);

						if (SUCCEEDED(hr))
						{
							success = activate_device(device);
						}
					}
				}
			}

			return success;
		}

		os::audio_format format() override
		{
			std::unique_lock lock(_rw);
			return describe(_pwfx);
		}

		double time() const override
		{
			std::shared_lock lock(_rw);

			if (_clock)
			{
				uint64_t device_frequency = 0;
				uint64_t position = 0;

				if (SUCCEEDED(_clock->GetFrequency(&device_frequency)) &&
					SUCCEEDED(_clock->GetPosition(&position, nullptr)) &&
					device_frequency != 0)
				{
					return static_cast<double>(position) / static_cast<double>(device_frequency);
				}
			}

			return 0.0;
		}

		bool is_stopped() const override
		{
			return !_playing;
		}

		void stop() override
		{
			std::unique_lock lock(_rw);

			if (_audio)
			{
				const auto hr = _audio->Stop();
				_playing = false;

				if (FAILED(hr))
				{
					update_status(hr);
				}
			}
		}

		void reset() override
		{
			std::unique_lock lock(_rw);

			if (_audio)
			{
				auto hr = _audio->Stop();
				if (FAILED(hr)) update_status(hr);
				hr = _audio->Reset();
				if (FAILED(hr)) update_status(hr);
				_playing = false;
				if (_data_event) ResetEvent(_data_event);
			}
		}

		void start() override
		{
			std::unique_lock lock(_rw);

			if (_audio)
			{
				const auto hr = _audio->Start();
				_playing = SUCCEEDED(hr);

				if (FAILED(hr))
				{
					update_status(hr);
				}
			}
		}

		void update_status(const HRESULT hr)
		{
			if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
				hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
				hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
				hr == AUDCLNT_E_ENDPOINT_CREATE_FAILED)
			{
				_device_lost = true;
			}
		}

		void wait_for_buffer(void* const wake, const uint32_t timeout_ms) override
		{
			const HANDLE events[] = {_data_event, wake};
			WaitForMultipleObjects(wake ? 2 : 1, events, false, timeout_ms);
		}

		uint32_t writable_frames() override
		{
			std::unique_lock lock(_rw);

			if (!_audio)
			{
				return 0;
			}

			uint32_t padding = 0;
			const auto hr = _audio->GetCurrentPadding(&padding);

			if (FAILED(hr))
			{
				update_status(hr);
				return 0;
			}

			return padding < _buffer_frame_count ? _buffer_frame_count - padding : 0u;
		}

		size_t write(const std::span<const uint8_t> samples) override
		{
			std::unique_lock lock(_rw);

			if (!_render || !_pwfx || !samples.data())
			{
				return 0; // Early exit for invalid state
			}

			// A frame of the endpoint's own format: the buffers the media code writes from are laid out by
			// the format the endpoint describes, so their frames are these.
			const uint32_t frame_bytes = _pwfx->nBlockAlign;

			if (frame_bytes == 0)
			{
				return 0;
			}

			const auto available_in_buffer = static_cast<uint32_t>(
				std::min<size_t>(samples.size() / frame_bytes, std::numeric_limits<uint32_t>::max()));
			size_t taken = 0;

			if (available_in_buffer > 0)
			{
				uint32_t numFramesPadding = 0;
				uint8_t* pData = nullptr;

				auto hr = _audio->GetCurrentPadding(&numFramesPadding);

				if (SUCCEEDED(hr))
				{
					const auto available_frames = numFramesPadding < _buffer_frame_count
						                              ? _buffer_frame_count - numFramesPadding
						                              : 0u;
					const auto copy_samples = std::min(available_frames, available_in_buffer);

					if (copy_samples > 0)
					{
						hr = _render->GetBuffer(copy_samples, &pData);

						if (SUCCEEDED(hr))
						{
							const auto copy_bytes = static_cast<size_t>(copy_samples) * frame_bytes;
							memcpy(pData, samples.data(), copy_bytes);
							taken = copy_bytes;

							constexpr uint32_t flags = 0;
							hr = _render->ReleaseBuffer(copy_samples, flags);
						}
					}
				}

				if (FAILED(hr))
				{
					update_status(hr);
				}
			}

			return taken;
		}

		void write_silence() override
		{
			std::unique_lock lock(_rw);

			if (!_render || !_audio)
			{
				return;
			}

			uint32_t numFramesPadding = 0;

			auto hr = _audio->GetCurrentPadding(&numFramesPadding);

			if (SUCCEEDED(hr))
			{
				// Fill the endpoint ring with silence so an underrun cannot replay stale
				// audio left in a driver-managed slot.
				const uint32_t target = _buffer_frame_count;

				if (numFramesPadding < target)
				{
					const auto frames = std::min<uint32_t>(target - numFramesPadding,
					                                       _buffer_frame_count - numFramesPadding);

					if (frames > 0)
					{
						uint8_t* pData = nullptr;
						hr = _render->GetBuffer(frames, &pData);

						if (SUCCEEDED(hr) && pData && _pwfx)
						{
							// Write actual zeros rather than relying on
							// AUDCLNT_BUFFERFLAGS_SILENT: GetBuffer hands back a slot in the
							// ring that still holds the last audio, and some drivers ignore
							// the SILENT flag and play it - which replays ("repeats") the last
							// sound buffer at the end of a clip, especially when the audio
							// stream ends before the video.
							memset(pData, 0, static_cast<size_t>(frames) * _pwfx->nBlockAlign);
							hr = _render->ReleaseBuffer(frames, 0);
						}
						else if (SUCCEEDED(hr))
						{
							hr = _render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
						}
					}
				}
			}

			if (FAILED(hr))
			{
				update_status(hr);
			}
		}

		void volume(const double vol) override
		{
			std::unique_lock lock(_rw);

			if (_sav && std::fabs(_vol - vol) > std::numeric_limits<double>::epsilon())
			{
				const auto hr = _sav->SetMasterVolume(static_cast<float>(vol), nullptr);

				if (FAILED(hr))
				{
					update_status(hr);
				}
				else
				{
					_vol = vol;
				}
			}
		}

		std::string id() override
		{
			std::shared_lock lock(_rw);
			std::string result;

			if (_device)
			{
				LPWSTR pwszID = nullptr;
				const auto hr = _device->GetId(&pwszID);

				if (SUCCEEDED(hr) && pwszID)
				{
					result = os::detail::narrow(pwszID);
				}

				// Freed on failure too: GetId can hand back a string it did not finish.
				CoTaskMemFree(pwszID);
			}

			return result;
		}
	};
}

os::audio_format os::describe_wave_format(const void* const wave_format)
{
	return describe(static_cast<const WAVEFORMATEX*>(wave_format));
}

os::audio_output_ptr os::create_audio_output(const std::string_view device_id, const double ring_seconds)
{
	auto result = std::make_shared<wasapi_sound>(ring_seconds);

	if (result->init(device_id))
	{
		return result;
	}

	return nullptr;
}

std::vector<os::audio_device> os::list_audio_playback_devices()
{
	std::vector<audio_device> result;

	ComPtr<IMMDeviceEnumerator> pEnumerator;
	ComPtr<IMMDeviceCollection> pCollection;

	auto hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator), nullptr,
		CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
		&pEnumerator);

	if (SUCCEEDED(hr))
	{
		hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);

		if (SUCCEEDED(hr))
		{
			UINT count;
			hr = pCollection->GetCount(&count);
			if (SUCCEEDED(hr))
			{
				// A sanity bound, so a broken driver cannot ask for an enormous list.
				count = std::min<UINT>(count, 1000);

				for (ULONG i = 0; i < count; i++)
				{
					ComPtr<IMMDevice> pEndpoint;
					hr = pCollection->Item(i, &pEndpoint);

					if (SUCCEEDED(hr))
					{
						LPWSTR pwszID = nullptr;
						hr = pEndpoint->GetId(&pwszID);

						if (SUCCEEDED(hr) && pwszID)
						{
							audio_device d;
							d.id = detail::narrow(pwszID);
							d.name = std::format("Audio device {}", static_cast<int>(i));

							ComPtr<IPropertyStore> pProps;
							hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);

							if (SUCCEEDED(hr))
							{
								PROPVARIANT name;
								PropVariantInit(&name);

								if (SUCCEEDED(pProps->GetValue(device_friendly_name, &name)) &&
									name.vt == VT_LPWSTR && name.pwszVal && *name.pwszVal)
								{
									d.name = detail::narrow(name.pwszVal);
								}

								PropVariantClear(&name);
							}

							result.emplace_back(std::move(d));
						}

						// Freed on failure too: GetId can hand back a string it did not finish.
						CoTaskMemFree(pwszID);
					}
				}
			}
		}
	}

	return result;
}

void* os::join_pro_audio_class()
{
	DWORD task_index = 0;
	return AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
}

void os::leave_pro_audio_class(void* const task)
{
	if (task)
	{
		AvRevertMmThreadCharacteristics(task);
	}
}

os::audio_thread::audio_thread()
{
	// S_OK and S_FALSE both join, and are balanced when this goes; RPC_E_CHANGED_MODE means the thread
	// is in another apartment already, which serves as well and is not this object's to leave.
	_joined_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	_task = join_pro_audio_class();
}

os::audio_thread::~audio_thread()
{
	leave_pro_audio_class(_task);

	if (_joined_com)
	{
		CoUninitialize();
	}
}
