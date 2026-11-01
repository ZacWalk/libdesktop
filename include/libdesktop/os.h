// os.h - libdesktop-os: the operating-system services libdesktop-core (pf::) and libdesktop-system
// (platform::) share, so each service has one implementation to maintain and to port.
//
// A service here names no application, writes no log and asks nothing of its caller's startup - no
// s_app_name, no locale, and no COM but what an audio thread joins (audio_thread). It takes what it
// needs, such as an owner window, a user agent or a path, and reports failure as a value; each stack
// wraps it in its own API and adds its own policy. Text and paths are UTF-8. Include no OS header here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace os
{
	// ── Clipboard text ───────────────────────────────────────────────────────────────────────
	// owner is the native window that holds the clipboard while it is open (an HWND on Windows),
	// or 0 for none.

	bool clipboard_has_text();

	struct clipboard_text
	{
		// False while another process holds the clipboard open.
		bool opened = false;
		// Unset when the clipboard holds no text.
		std::optional<std::string> text;
	};

	clipboard_text read_clipboard_text(uintptr_t owner);

	enum class clipboard_write
	{
		ok,
		too_large,
		alloc_failed,
		lock_failed,
		open_failed,
		empty_failed,
		set_failed
	};

	// Places the text exactly as given, and empties the clipboard only once the text is ready to
	// replace what was on it. A caller that cleans the text up does so first.
	clipboard_write write_clipboard_text(uintptr_t owner, std::string_view text);

	// ── Known folders ────────────────────────────────────────────────────────────────────────

	enum class known_folder
	{
		local_app_data,
		roaming_app_data,
		profile,
		desktop,
		documents,
		pictures,
		videos,
		music,
		public_documents,
		public_pictures,
		public_videos,
		public_music
	};

	// Empty when the system has no such folder; error, when given, receives the system's code.
	// create makes the folder when it does not exist yet.
	std::string known_folder_path(known_folder folder, bool create = false, uint32_t* error = nullptr);

	// ── Files and file dialogs ──────────────────────────────────────────────────────────────
	//
	// Paths crossing this boundary are UTF-8. The Windows implementation adds an extended-length
	// prefix only for rooted paths when a native call needs it; relative paths remain relative.
	// Nothing in this interface exposes a native path or a Win32 handle.

	enum class file_presence
	{
		unknown,
		not_found,
		found
	};

	struct file_identity
	{
		uint64_t volume = 0;
		uint64_t index = 0;
		bool valid = false;

		bool operator==(const file_identity&) const = default;
	};

	struct file_attributes
	{
		file_presence presence = file_presence::unknown;
		bool directory = false;
		bool readonly = false;
		bool offline = false;
		bool hidden = false;
		bool reparse = false;
		uint64_t modified = 0;
		uint64_t created = 0;
		uint64_t size = 0;
		file_identity identity;
		uint32_t error = 0;

		bool exists() const { return presence == file_presence::found; }
		bool confirmed_missing() const { return presence == file_presence::not_found; }
	};

	struct file_entry
	{
		std::string name;
		file_attributes attributes;
		bool can_recurse = true;
	};

	struct file_enumeration
	{
		std::vector<file_entry> entries;
		bool success = false;
		uint32_t error = 0;
	};

	file_attributes query_file(std::string_view path);
	file_enumeration enumerate_files(std::string_view folder, bool show_hidden);

	enum class file_open_mode
	{
		read,
		write,
		create,
		create_new,
		read_write,
		sequential_scan
	};

	enum class file_share_mode
	{
		all,
		read,
		write,
		none
	};

	enum class file_whence
	{
		begin,
		current,
		end
	};

	class file
	{
	public:
		virtual ~file() = default;

		virtual uint64_t size() const = 0;
		virtual uint64_t read(uint8_t* data, uint64_t size) const = 0;
		virtual uint64_t write(const uint8_t* data, uint64_t size) = 0;
		virtual uint64_t read_at(uint64_t offset, uint8_t* data, uint64_t size) const = 0;
		virtual uint64_t write_at(uint64_t offset, const uint8_t* data, uint64_t size) = 0;
		virtual bool flush() const = 0;
		virtual uint64_t seek(uint64_t offset, file_whence whence) const = 0;
		virtual uint64_t position() const = 0;
		virtual bool truncate(uint64_t size) const = 0;
		virtual std::string path() const = 0;
	};

	using file_ptr = std::shared_ptr<file>;

	file_ptr open_file(std::string_view path, file_open_mode mode,
	                   file_share_mode share = file_share_mode::all);
	// Backend-only interop for libdesktop-system's specialized handle operations. No application
	// includes os.h or receives this token.
	uintptr_t native_file_handle(const file_ptr& file);

	enum class file_operation_code
	{
		ok,
		cancelled,
		failed,
		already_exists
	};

	struct file_operation
	{
		file_operation_code code = file_operation_code::failed;
		uint32_t error = 0;
		std::string diagnostic;

		bool success() const { return code == file_operation_code::ok; }
		bool failed() const { return code != file_operation_code::ok; }
	};

	file_operation copy_file(std::string_view source, std::string_view destination, bool fail_if_exists);
	file_operation move_file(std::string_view source, std::string_view destination, bool fail_if_exists,
	                         bool allow_copy = true);
	file_operation create_folder(std::string_view path);
	file_operation delete_file(std::string_view path);
	file_operation recycle_file(std::string_view path);
	bool can_recycle(std::string_view path);

	struct file_dialog_filter
	{
		std::string text;
		std::string extensions;
	};

	struct file_dialog_result
	{
		std::vector<std::string> paths;
		uint32_t error = 0;

		bool accepted() const { return !paths.empty(); }
		bool cancelled() const { return paths.empty() && error == 0; }
	};

	file_dialog_result open_file_dialog(uintptr_t owner, std::string_view title,
	                                    const std::vector<file_dialog_filter>& filters, bool allow_multiple);
	file_dialog_result save_file_dialog(uintptr_t owner, std::string_view title, std::string_view default_path,
	                                    const std::vector<file_dialog_filter>& filters);
	file_dialog_result folder_dialog(uintptr_t owner, std::string_view title, std::string_view default_path = {});

	// A profile-style INI store shared by both platform stacks. Locations and the policy that
	// selects one stay in the wrappers. Values are raw UTF-8; callers that need the historical
	// escaped-line format use escape_ini_value/unescape_ini_value below.
	class settings_store
	{
	public:
		virtual ~settings_store() = default;
		virtual bool root_created() const = 0;
		virtual std::optional<std::string> read(std::string_view section, std::string_view key) const = 0;
		virtual bool write(std::string_view section, std::string_view key, std::string_view value) = 0;
		virtual bool flush() = 0;
	};

	using settings_store_ptr = std::shared_ptr<settings_store>;
	using ini_store = settings_store;
	using ini_store_ptr = settings_store_ptr;

	ini_store_ptr open_ini_store(std::string_view path, bool write_unicode_bom = false);
	settings_store_ptr open_registry_store(std::string_view root_key);
	std::string escape_ini_value(std::string_view value);
	std::string unescape_ini_value(std::string_view value);

	// ── Web requests ─────────────────────────────────────────────────────────────────────────

	bool is_online();

	// Percent-encodes everything but RFC 3986's unreserved characters.
	std::string url_encode(std::string_view input);

	using web_params = std::vector<std::pair<std::string, std::string>>;

	enum class web_request_verb
	{
		POST,
		GET
	};

	struct web_request
	{
		std::string path;
		std::string body;

		web_params query;
		web_params headers;
		web_params form_data;

		std::string file_form_data_name;
		std::string file_name;
		std::string upload_file_path;
		std::string download_file_path;

		web_request_verb verb = web_request_verb::GET;

		bool follow_redirects = true;
		bool use_cookies = true;
		// Zero leaves body/initial-header storage unbounded; limits count bytes, not characters.
		size_t max_response_bytes = 0;
		size_t max_header_bytes = 0;
		// Per network operation, including connection establishment; zero retains the host's timeouts.
		uint32_t timeout_ms = 0;
	};

	enum class web_response_error
	{
		none,
		invalid_request,
		transport,
		response_limit
	};

	struct web_response
	{
		std::string headers;
		std::string body;
		std::string content_type;
		int status_code = 0;
		// HTTP error status codes are still valid responses. On acquisition failure body is empty.
		web_response_error error = web_response_error::none;
		// Set once the download file has been created, so what is at download_file_path came from
		// this request, whole or not.
		bool download_created = false;
	};

	struct web_host;
	using web_host_ptr = std::shared_ptr<web_host>;

	// timeouts: true gives the connection, sending and receiving 30 seconds each; false keeps the
	// system's own. A request with a timeout, or a limit, is bounded by it either way.
	web_host_ptr connect_to_host(std::string_view host, bool secure, int port, std::string_view user_agent,
	                             bool timeouts = true);
	web_response send_request(const web_host_ptr& host, const web_request& req);

	// ── Audio output ─────────────────────────────────────────────────────────────────────────
	// One engine under both stacks: platform::audio_output is this endpoint, and pf::'s sounds and
	// streams are libdesktop-core's mixer playing through it. An endpoint is driven from a thread that
	// has joined COM - an audio_thread, or a thread the application has joined itself.

	enum class audio_sample_format
	{
		none,
		float32,
		int16,
		int32
	};

	// An endpoint's mix format. Each frame holds channels samples; channel_mask names the speakers they
	// feed in WAVEFORMATEXTENSIBLE's bits, and is zero where the endpoint names none.
	struct audio_format
	{
		uint32_t sample_rate = 0;
		audio_sample_format sample_format = audio_sample_format::none;
		uint32_t channels = 0;
		uint64_t channel_mask = 0;
	};

	// Describes a WAVEFORMATEX, or a WAVEFORMATEXTENSIBLE, as an endpoint's format. Samples other than
	// 32-bit float or 16- or 32-bit integers, or a null format, have sample_format none.
	audio_format describe_wave_format(const void* wave_format);

	// An endpoint opened in its own mix format, shared with the rest of the system. It takes
	// interleaved frames in that format and keeps the clock of what it has played.
	class audio_output
	{
	public:
		virtual ~audio_output() = default;

		virtual std::string id() = 0;
		virtual audio_format format() = 0;
		// Seconds played since the endpoint was last reset.
		virtual double time() const = 0;
		virtual bool is_stopped() const = 0;
		// Set once the device has gone - unplugged, disabled, or its audio service stopped. Open
		// another to carry on.
		virtual bool is_device_lost() const = 0;

		virtual void reset() = 0;
		virtual void start() = 0;
		virtual void stop() = 0;
		// Returns when the endpoint wants frames, when wake - a native event (a HANDLE on Windows) or
		// null - is signalled, or after timeout_ms. An endpoint opened with a ring of its own length has
		// no buffer event, so only wake or the timeout ends the wait.
		virtual void wait_for_buffer(void* wake, uint32_t timeout_ms) = 0;
		// Frames the ring has room for now.
		virtual uint32_t writable_frames() = 0;
		// Queues as many whole frames from the front of frames as the ring has room for, and answers how
		// many bytes it took.
		virtual size_t write(std::span<const uint8_t> frames) = 0;
		// Fills the ring's free space with silence, so an underrun plays silence instead of replaying
		// what the ring last held - the tail of a clip, end of stream.
		virtual void write_silence() = 0;
		virtual void volume(double gain) = 0;
	};

	using audio_output_ptr = std::shared_ptr<audio_output>;

	// Opens the endpoint device_id names, or the default one when it is empty or cannot be opened; null
	// when there is none. A ring of zero seconds is the engine's minimum, a device period or two, which
	// only a thread waiting on wait_for_buffer can keep full; a caller that refills on its own schedule
	// asks for a ring long enough to cover the gap between refills.
	audio_output_ptr create_audio_output(std::string_view device_id, double ring_seconds = 0.0);

	struct audio_device
	{
		std::string id;
		std::string name;
	};

	// The active playback endpoints, at most a thousand.
	std::vector<audio_device> list_audio_playback_devices();

	// The scheduling class an audio thread joins (MMCSS's Pro Audio on Windows), so the scheduler keeps it
	// running promptly under load - starved, it lets the endpoint's ring underrun. Joining gives back the
	// handle that leaves, null when the class could not be joined; leaving null does nothing.
	void* join_pro_audio_class();
	void leave_pro_audio_class(void* task);

	// Readies the thread it lives on to drive an endpoint: joins COM's multithreaded apartment, unless
	// the thread is in COM already, and the Pro Audio class; leaves both when it goes.
	class audio_thread
	{
		void* _task = nullptr;
		bool _joined_com = false;

	public:
		audio_thread();
		~audio_thread();

		audio_thread(const audio_thread&) = delete;
		audio_thread& operator=(const audio_thread&) = delete;
	};
}
