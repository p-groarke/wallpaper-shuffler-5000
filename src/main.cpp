#include <fea/getopt/getopt.hpp>
#include <fea/numerics/random.hpp>
#include <fea/serialize/serialize.hpp>
#include <fea/serialize/serializer.hpp>
#include <fea/terminal/pipe.hpp>
#include <fea/terminal/utf8_io.hpp>
#include <fea/utility/error.hpp>
#include <fea/utility/file.hpp>
#include <fea/utility/scope.hpp>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>

// IDesktopWallpaper
// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-idesktopwallpaper
#define _ATL_APARTMENT_THREADED
#include <atlbase.h>
extern CComModule _Module;
#include <atlcom.h>
#include <functiondiscoverykeys.h>
#include <initguid.h>
#include <windows.h>

#include <ShObjIdl.h>
// #include <shellapi.h>
#include <cfgmgr32.h>
#include <ntddvdeo.h>

struct coinit {
	coinit() {
		THROW_IF_FAILED_MSG(::CoInitializeEx(nullptr, COINIT_MULTITHREADED),
				"Couldn't initialize COM.\n");
	}
	~coinit() {
		::CoUninitialize();
	}
};

// We need a clock with specified time_since_epock.
// TODO : Deal with daylight savings etc.
using serialize_clock = std::chrono::system_clock;
using serialize_time_point = std::chrono::system_clock::time_point;
using serialize_duration = std::chrono::system_clock::duration;

inline const coinit _coinit;
#if 1
const serialize_duration shuffle_interval = std::chrono::days{ 1 };
#else
// const serialize_duration shuffle_interval = std::chrono::hours{ 1 };
const serialize_duration shuffle_interval = std::chrono::seconds{ 60 };
#endif
const std::array<std::wstring, 4> img_extensions{
	L".bmp",
	L".jpg",
	L".jpeg",
	L".png",
};

struct img {
	bool shown = false;
	serialize_time_point shown_timestamp{};
	uint64_t file_size = 0;
	std::wstring filename;

	friend void serialize(const img& v, fea::serializer& ofs) {
		using fea::serialize;
		serialize(v.shown, ofs);
		serialize(v.shown_timestamp, ofs);
		serialize(v.file_size, ofs);
		serialize(v.filename, ofs);
	}

	friend bool deserialize(fea::deserializer& ifs, img& v) {
		using fea::deserialize;
		if (!deserialize(ifs, v.shown)) {
			return false;
		}
		if (!deserialize(ifs, v.shown_timestamp)) {
			return false;
		}
		if (!deserialize(ifs, v.file_size)) {
			return false;
		}
		if (!deserialize(ifs, v.filename)) {
			return false;
		}
		return true;
	}

	// auto operator<=>(const img&) const noexcept = default;
};

struct save_data {
	serialize_time_point last_shuffle_timestamp{};
	std::vector<img> imgs;

	friend void serialize(const save_data& v, fea::serializer& ofs) {
		using fea::serialize;
		serialize(v.last_shuffle_timestamp, ofs);
		serialize(v.imgs, ofs);
	}

	friend bool deserialize(fea::deserializer& ifs, save_data& v) {
		using fea::deserialize;
		if (!deserialize(ifs, v.last_shuffle_timestamp)) {
			return false;
		}
		if (!deserialize(ifs, v.imgs)) {
			return false;
		}
		return true;
	}
};

// Log status / info to output, ignored if not in verbose mode.
template <class... Args>
void log_status(
		bool verbose, const std::wformat_string<Args...> fmt, Args&&... args) {
	if (!verbose) {
		return;
	}
	std::wcout << "[STATUS] ";
	std::wcout << std::vformat(fmt.get(), std::make_wformat_args(args...));
	std::wcout.flush();
}

// Log warning to output.
template <class... Args>
void log_warning(const std::wformat_string<Args...> fmt, Args&&... args) {
	std::wcout << "[WARNING] ";
	std::wcout << std::vformat(fmt.get(), std::make_wformat_args(args...));
	std::wcout.flush();
}

// Log error to output.
template <class... Args>
void log_error(const std::wformat_string<Args...> fmt, Args&&... args) {
	std::wcerr << "[ERROR] ";
	std::wcerr << std::vformat(fmt.get(), std::make_wformat_args(args...));
	std::wcerr.flush();
}

// Get a value from registry.
// Returns true on success.
template <class T>
bool get_registry_value(HKEY hkey, const std::wstring& path,
		const std::wstring& value, T* out_ptr) {

	static_assert(std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>
						  || std::is_same_v<T, std::wstring>,
			"Unsupported output type.");

	unsigned dw_flags = 0;
	if constexpr (std::is_same_v<T, uint32_t>) {
		dw_flags = RRF_RT_DWORD;
	} else if constexpr (std::is_same_v<T, uint64_t>) {
		dw_flags = RRF_RT_QWORD;
	} else {
		dw_flags = RRF_RT_REG_SZ;
	}

	std::vector<uint8_t> buf(MAX_PATH + 1, 0);
	DWORD reg_value_size = DWORD(buf.size());
	if (LSTATUS err = RegGetValueW(hkey, path.c_str(), value.c_str(), dw_flags,
				nullptr, buf.data(), &reg_value_size);
			err != ERROR_SUCCESS) {
		// Failed.
		std::wcerr << std::format(
				L"Failed to get regex value : '{}\\{}'\n", path, value);

		std::error_code ec{ int(err), std::system_category() };
		fea::print_error_message_w(__FUNCTION__, __LINE__, ec);
		return false;
	}

	if constexpr (std::is_same_v<T, std::wstring>) {
		std::wstring& str = *out_ptr;
		size_t str_size = size_t(reg_value_size / 2) + 1;
		if (str.size() < str_size) {
			str = std::wstring(str_size, L'\0');
		}
		char* out_char_ptr = reinterpret_cast<char*>(str.data());
		std::memcpy(out_char_ptr, buf.data(), reg_value_size);
	} else {
		char* out_char_ptr = reinterpret_cast<char*>(out_ptr);
		std::memcpy(out_char_ptr, buf.data(), reg_value_size);
	}
	return true;
}

template <class T>
bool set_registry_value(HKEY hkey, const std::wstring& path,
		const std::wstring& value, const T& val) {

	static_assert(std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>
						  || std::is_same_v<T, std::wstring>,
			"Unsupported input type.");

	// unsigned dw_flags = 0;
	// if constexpr (std::is_same_v<T, uint32_t>) {
	//	dw_flags = RRF_RT_DWORD;
	// } else if constexpr (std::is_same_v<T, uint64_t>) {
	//	dw_flags = RRF_RT_QWORD;
	// } else {
	//	dw_flags = RRF_RT_REG_SZ;
	// }

	// std::vector<uint8_t> buf(MAX_PATH + 1, 0);
	// DWORD reg_value_size = DWORD(buf.size());
	// if (LSTATUS err = RegGetValueW(hkey, path.c_str(), value.c_str(),
	// dw_flags, 			nullptr, buf.data(), &reg_value_size); 		err !=
	// ERROR_SUCCESS) {
	//	// Failed.
	//	std::wcerr << std::format(
	//			L"Failed to get regex value : '{}\\{}'\n", path, value);

	//	std::error_code ec{ int(err), std::system_category() };
	//	fea::print_error_message_w(__FUNCTION__, __LINE__, ec);
	//	return false;
	//}

	// if constexpr (std::is_same_v<T, std::wstring>) {
	//	std::wstring& str = *out_ptr;
	//	size_t str_size = size_t(reg_value_size / 2) + 1;
	//	if (str.size() < str_size) {
	//		str = std::wstring(str_size, L'\0');
	//	}
	//	char* out_char_ptr = reinterpret_cast<char*>(str.data());
	//	std::memcpy(out_char_ptr, buf.data(), reg_value_size);
	// } else {
	//	char* out_char_ptr = reinterpret_cast<char*>(out_ptr);
	//	std::memcpy(out_char_ptr, buf.data(), reg_value_size);
	// }
	return true;
}

template <class RndIt>
void randomizeit(RndIt first, RndIt last) {
	// auto printit = [&]() {
	//	for (auto it = first; it != last; ++it) {
	//		std::wcout << std::format(L"{}, ", it->filename);
	//	}
	//	std::wcout << "\n";
	// };

	unsigned t1 = unsigned(
			std::chrono::steady_clock::now().time_since_epoch().count());
	unsigned t2 = 0;
	unsigned t3 = 0;
	unsigned t4 = 0;
	do {
		t2 = unsigned(
				std::chrono::steady_clock::now().time_since_epoch().count());
	} while (t2 == t1);
	do {
		t3 = unsigned(
				std::chrono::steady_clock::now().time_since_epoch().count());
	} while (t3 == t2);
	do {
		t4 = unsigned(
				std::chrono::steady_clock::now().time_since_epoch().count());
	} while (t4 == t3);

	assert(t1 != t2 && t2 != t3 && t3 != t4);

	std::random_device rd;
	std::seed_seq seed{
		t1 ^ rd(),
		t2 ^ rd(),
		t3 ^ rd(),
		t4 ^ rd(),
	};
	std::mt19937_64 gen(seed);

	// printit();
	std::reverse(first, last);
	std::shuffle(first, last, gen);
	// printit();
	std::reverse(first, last);
	std::shuffle(first, last, gen);
	// printit();
}

// Fills vector with monitor ids.
// Returns true on success.
[[nodiscard]]
bool get_monitors(const CComPtr<IDesktopWallpaper>& desktop_wallpaper,
		bool verbose, std::vector<std::wstring>* monitor_ids_ptr) {
	unsigned num_monitors = 0;
	if (!SUCCEEDED(
				desktop_wallpaper->GetMonitorDevicePathCount(&num_monitors))) {
		log_error(L"Couldn't get monitor count.\n");
		return false;
	}

	if (num_monitors == 0) {
		log_warning(L"Detected zero monitors, behaving as if there was one.\n");
		monitor_ids_ptr->push_back(L""); // This does work.
		return true;
	}

	log_status(verbose, L"Detected {} monitors :\n", num_monitors);

	// Filter bad / virtual monitors.
	monitor_ids_ptr->reserve(num_monitors);
	for (size_t i = 0; i < num_monitors; ++i) {
		wchar_t* str_ptr = nullptr;
		if (!SUCCEEDED(desktop_wallpaper->GetMonitorDevicePathAt(
					unsigned(i), &str_ptr))) {
			log_error(L"\tCouldn't get monitor {} path.\n", i);
			return false;
		}

		if (str_ptr == nullptr) {
			log_status(verbose, L"\tNull monitor id string, skipping.\n");
			continue;
		}

		std::wstring id_str{ str_ptr };
		if (id_str.empty()) {
			log_status(verbose, L"\tEmpty monitor id string, skipping.\n");
			continue;
		}

		log_status(verbose, L"\t{}\n", str_ptr);
		monitor_ids_ptr->push_back(std::move(id_str));
	}

	return true;
}

// Sets images as wallpapers.
// Returns true on success.
[[nodiscard]]
bool set_wallpapers(const CComPtr<IDesktopWallpaper>& desktop_wallpaper,
		const std::vector<std::wstring>& monitor_ids,
		const std::vector<std::filesystem::path>& wallpapers, bool verbose) {
	if (monitor_ids.size() < 1) {
		log_error(L"There should be at least 1 monitor id.");
		return false;
	}

	if (wallpapers.size() != monitor_ids.size()) {
		log_error(L"Invalid amount of wallpapers : '{}' vs '{}'\n",
				wallpapers.size(), monitor_ids.size());
		return false;
	}

	// Sleep in between calls, give some time to windows its very slow.
	const std::chrono::milliseconds sleep_time
			= std::chrono::milliseconds{ 500 };
	// const std::chrono::milliseconds long_sleep_time = sleep_time * 10;

	// Set the style to 'Fill'.
	{
		if (!SUCCEEDED(desktop_wallpaper->SetPosition(DWPOS_FILL))) {
			log_error(L"Couldn't set wallpaper fill.\n");
			return false;
		}
		std::this_thread::sleep_for(sleep_time);

		// Make sure it was processed.
		if (DESKTOP_WALLPAPER_POSITION wallpaper_pos = DWPOS_CENTER;
				SUCCEEDED(desktop_wallpaper->GetPosition(&wallpaper_pos))
				&& wallpaper_pos != DWPOS_FILL) {
			log_error(L"Windows reset wallpaper fill.\n");
			return false;
		}
	}

	// Set the first image everywhere ("") once. This sticks after a monitor
	// disconnect or sleep. If we don't do this, the wallpapers are completely
	// reset to an unrelated image when monitors are disconnected.
	{
		const std::filesystem::path& path = wallpapers.front();
		if (!SUCCEEDED(desktop_wallpaper->SetWallpaper(L"", path.c_str()))) {
			log_error(L"Couldn't set wallpaper.\n");
			return false;
		}
		std::this_thread::sleep_for(sleep_time);

		if (wchar_t* wallpaper_path = nullptr;
				SUCCEEDED(desktop_wallpaper->GetWallpaper(L"", &wallpaper_path))
				&& std::filesystem::path{ wallpaper_path } != path) {
			log_error(L"Windows reset wallpaper image.\n");
			return false;
		}
	}

	// If we only have 1 wallpaper, we can exit now.
	if (monitor_ids.size() == 1) {
		return true;
	}

	// Set the wallpapers for each monitor.
	for (size_t i = 0; i < wallpapers.size(); ++i) {
		const std::filesystem::path& path = wallpapers[i];
		log_status(verbose, L"Setting wallpaper on monitor {} : '{}'\n", i,
				path.wstring());

		const wchar_t* monitor_id_str = monitor_ids[i].c_str();
		if (!SUCCEEDED(desktop_wallpaper->SetWallpaper(
					monitor_id_str, path.c_str()))) {
			log_error(L"Couldn't set wallpaper.\n");
			return false;
		}
		std::this_thread::sleep_for(sleep_time);

		if (wchar_t* wallpaper_path = nullptr;
				SUCCEEDED(desktop_wallpaper->GetWallpaper(
						monitor_id_str, &wallpaper_path))
				&& std::filesystem::path{ wallpaper_path } != path) {
			log_error(L"Windows reset wallpaper image.\n");
			return false;
		}
	}

	return true;
}

// Makes sure the expected wallpapers are currently displayed.
// Returns true on success.
[[nodiscard]]
bool fix_win11(const CComPtr<IDesktopWallpaper>& desktop_wallpaper,
		const save_data& data, const std::filesystem::path& img_folder,
		bool verbose) {
	assert(!data.imgs.empty());
	log_status(verbose, L"Checking if we need to fix Windows 11 bug.\n");

	if (!data.imgs.back().shown) {
		// Nothing was ever shown.
		log_status(
				verbose, L"No wallpaper were ever selected. Weird but OK.\n");
		return true;
	}

	std::vector<std::filesystem::path> expected_wallpapers;
	serialize_time_point last_timestamp = data.imgs.back().shown_timestamp;
	for (const img& image : data.imgs) {
		if (image.shown && image.shown_timestamp == last_timestamp) {
			expected_wallpapers.push_back(img_folder / image.filename);
		}
	}

	std::vector<std::wstring> monitor_ids;
	if (!get_monitors(desktop_wallpaper, verbose, &monitor_ids)) {
		return false;
	}

	for (size_t i = 0; i < monitor_ids.size(); ++i) {
		if (i >= expected_wallpapers.size()) {
			expected_wallpapers.push_back(expected_wallpapers.back());
		}
	}
	assert(expected_wallpapers.size() == monitor_ids.size());

	// Collect all current wallpapers.
	std::vector<std::filesystem::path> current_wallpapers;
	for (size_t i = 0; i < monitor_ids.size(); ++i) {
		const wchar_t* monitor_id_str = monitor_ids[i].c_str();
		wchar_t* wallpaper_path = nullptr;
		if (!SUCCEEDED(desktop_wallpaper->GetWallpaper(
					monitor_id_str, &wallpaper_path))) {
			log_error(L"Couldn't get current wallpaper for monitor '{}'.\n",
					monitor_id_str);
			return false;
		}
		current_wallpapers.push_back(std::filesystem::path{ wallpaper_path });
	}

	{
		std::sort(current_wallpapers.begin(), current_wallpapers.end());
		auto new_end = std::unique(
				current_wallpapers.begin(), current_wallpapers.end());
		current_wallpapers.erase(new_end, current_wallpapers.end());
	}

	if (current_wallpapers.size() != expected_wallpapers.size()) {
		log_status(verbose, L"Detected Windows 11 bug! Fixing.\n");
		return set_wallpapers(
				desktop_wallpaper, monitor_ids, expected_wallpapers, verbose);
	}
	return true;
}

// Checks our data, builds a playlist of wallpapers, displays them if time is
// right.
// Outputs the next update time.
// Returns true on success.
[[nodiscard]]
bool update(const CComPtr<IDesktopWallpaper>& desktop_wallpaper,
		const std::filesystem::path& data_filepath,
		const std::filesystem::path& img_folder, bool verbose,
		serialize_time_point* out_next_update) {

	// Load our save data.
	save_data data;
	if (std::filesystem::exists(data_filepath)) {
		log_status(verbose, L"Loading saved data.\n");

		using fea::deserialize;
		fea::deserializer ifs{ data_filepath };
		deserialize(ifs, data);
	}

	serialize_time_point now = serialize_clock::now();
	serialize_time_point next_update
			= data.last_shuffle_timestamp + shuffle_interval;

	// If we have nothing to do, exit cleanly now.
	if (now < next_update) {
		// We may have been woken up by displays connecting.
		// If so, double check we are still displaying the right wallpapers
		// (fix win 11 being dumb).
		if (!data.imgs.empty()) {
			if (!fix_win11(desktop_wallpaper, data, img_folder, verbose)) {
				return false;
			}
		}

		log_status(verbose, L"Shuffle not required, going back to sleep.\n");
		(*out_next_update) = next_update;
		return true;
	}

	(*out_next_update) = now + shuffle_interval;
	log_status(verbose, L"Time interval reached, changing wallpaper.\n");

	// Remove obsolete or changed images from our saved data.
	{
		auto new_end = std::remove_if(
				data.imgs.begin(), data.imgs.end(), [&](const img& v) {
					std::filesystem::path img_path = img_folder / v.filename;
					if (!std::filesystem::exists(img_path)) {
						return true;
					}
					if (uint64_t(std::filesystem::file_size(img_path))
							!= v.file_size) {
						return true;
					}
					return false;
				});
		data.imgs.erase(new_end, data.imgs.end());

		assert(std::is_partitioned(data.imgs.begin(), data.imgs.end(),
				[](const img& v) { return !v.shown; }));
	}

#if 0
	// TEMP TESTING
	{
		auto notshown_end = std::find_if(data.imgs.begin(), data.imgs.end(),
				[](const img& v) { return v.shown; });
		size_t s = size_t(std::distance(data.imgs.begin(), notshown_end));

		// Erase half unshown for testing purposes.
		data.imgs.erase(data.imgs.begin(), data.imgs.begin() + (s / 2));
	}
#endif

	// Go through the folder and gather new images.
	std::vector<std::filesystem::path> new_images;
	{
		log_status(verbose, L"Scanning folder.\n");

		for (const std::filesystem::path& p :
				std::filesystem::directory_iterator(img_folder)) {
			if (std::filesystem::is_directory(p)) {
				log_status(verbose, L"Skipping sub-folder '{}/'\n",
						p.filename().wstring());
				continue;
			}

			std::wstring ext = p.extension();
			if (!std::ranges::contains(img_extensions, ext)) {
				log_status(verbose, L"Skipping non-image '{}'\n",
						p.filename().wstring());
				continue;
			}

			new_images.push_back(p);
		}

		if (new_images.empty()) {
			log_error(L"No images found in folder.\n");
			return false;
		}

		// Remove old images from the new images in the folder.
		std::unordered_set<std::filesystem::path> old_images_set;
		for (const img& v : data.imgs) {
			old_images_set.insert(v.filename);
		}

		auto new_end = std::remove_if(new_images.begin(), new_images.end(),
				[&](const std::filesystem::path& p) {
					return old_images_set.contains(p.filename());
				});
		new_images.erase(new_end, new_images.end());
	}

	// Shuffle in the new images into the pre-existing images.
	if (!new_images.empty()) {
		log_status(verbose, L"Adding {} new images to the playlist.\n",
				new_images.size());

		std::vector<img> temp;
		temp.reserve(new_images.size());
		for (const std::filesystem::path& p : new_images) {
			assert(std::filesystem::exists(p)
					&& !std::filesystem::is_directory(p));

			temp.push_back(img{
					.shown = false,
					.file_size = std::filesystem::file_size(p),
					.filename = p.filename(),
			});
		}
		data.imgs.insert(data.imgs.begin(), temp.begin(), temp.end());

		log_status(verbose, L"Randomizing.\n");
		auto unshown_end = std::find_if(data.imgs.begin(), data.imgs.end(),
				[](const img& v) { return v.shown; });
		randomizeit(data.imgs.begin(), unshown_end);
	}
	assert(std::is_partitioned(data.imgs.begin(), data.imgs.end(),
			[](const img& v) { return !v.shown; }));

	// Check if we've reached the end and need to re-randomize.
	if (std::all_of(data.imgs.begin(), data.imgs.end(),
				[](const img& v) { return v.shown; })) {
		log_status(verbose,
				L"Reached end of playlist, reshuffling and restarting.\n");

		randomizeit(data.imgs.begin(), data.imgs.end());

		for (img& v : data.imgs) {
			v.shown = false;
		}
	}

	if (data.imgs.empty()) {
		log_error(L"No images in playlist, exiting.\n");
		return false;
	}

	// Change the wallpaper.
	{
		std::vector<std::wstring> monitor_ids;
		if (!get_monitors(desktop_wallpaper, verbose, &monitor_ids)) {
			return false;
		}
		assert(monitor_ids.size() >= 1);

		std::vector<std::filesystem::path> selected_imgs;
		for (size_t i = 0; i < monitor_ids.size(); ++i) {
			if (i >= data.imgs.size()) {
				assert(i > 0); // Should at least have 1 wallpaper.
				selected_imgs.push_back(selected_imgs.back());
				continue;
			}
			selected_imgs.push_back(img_folder / data.imgs[i].filename);
		}

		if (!set_wallpapers(
					desktop_wallpaper, monitor_ids, selected_imgs, verbose)) {
			return false;
		}

		// Update shown value and move the images to the end of playlist.
		for (size_t i = 0; i < monitor_ids.size(); ++i) {
			if (i >= data.imgs.size()) {
				break;
			}

			img& image = data.imgs[i];
			image.shown = true;
			image.shown_timestamp = now;
		}

		size_t num = (std::min)(monitor_ids.size(), data.imgs.size());
		std::rotate(
				data.imgs.begin(), data.imgs.begin() + num, data.imgs.end());

		data.last_shuffle_timestamp = now;
	}

	// Serialize the update.
	{
		using fea::serialize;
		fea::serializer ofs{ data_filepath };
		serialize(data, ofs);
	}

	log_status(verbose, L"Success!\n\n");
	return true;
}

// Used to sleep until next time we need to change background,
// but also wake up when displays are connected because windows doesn't
// remember the display 2 wallpaper🤦
std::condition_variable sleep_cv;
std::mutex cv_mutex;
std::atomic<bool> monitor_connected = false;

// PCM_NOTIFY_CALLBACK on_pcm_device_connect;
DWORD on_monitor_connect(HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action,
		PCM_NOTIFY_EVENT_DATA, DWORD) {
	if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL
			&& !monitor_connected.load()) {
		log_status(true, L"Monitor connected.\n");
		monitor_connected.store(true);
		sleep_cv.notify_all();
	}
	return ERROR_SUCCESS;
}

#if 0
int wmain(int argc, wchar_t** argv, wchar_t**) {
	auto on_exit_reset_term = fea::utf8_io(true);
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR /*lpCmdLine*/, int) {
	// Better than commandlinetoargvw, since these include the expected argv[0].
	int argc = __argc;
	wchar_t** argv = __wargv;
#endif

	// Create a log file to replace console output.
	std::filesystem::path log_filepath;
	{
		std::filesystem::path exe_dir = fea::executable_dir(argv[0]);
		std::filesystem::path log_filename = L"wallpaper-shuffler-5000.log";
		log_filepath = exe_dir / log_filename;

		if (std::filesystem::exists(log_filepath)) {
			size_t size = std::filesystem::file_size(log_filepath);
			if (size >= 1'000'000) {
				std::filesystem::remove(log_filename);
			}
		}
	}

	// Redirect output to log file.
	std::wofstream log_ofs{ log_filepath, std::ios::app };
	{
		std::wcout.rdbuf(log_ofs.rdbuf());
		std::wcerr.rdbuf(log_ofs.rdbuf());
		std::wcout << L"\n";
	}

	fea::fast_iostreams();

	std::filesystem::path img_folder{};
	bool verbose = false;
	fea::get_opt<wchar_t> opt;
	opt.add_raw_option(
			L"image_folder_path",
			[&](std::wstring&& str) {
				img_folder = std::filesystem::path{ std::move(str) };
				if (img_folder.empty()) {
					log_error(L"No image folder provided.\n");
					return false;
				}

				img_folder = std::filesystem::absolute(img_folder);
				if (!std::filesystem::exists(img_folder)) {
					log_error(L"Image folder doesn't exist : '{}'\n",
							img_folder.wstring());
					return false;
				}
				if (!std::filesystem::is_directory(img_folder)) {
					log_error(L"Path isn't a folder : '{}'",
							img_folder.wstring());
					return false;
				}
				return true;
			},
			L"The folder path of images to shuffle.");

	opt.add_flag_option(
			L"verbose",
			[&]() {
				verbose = true;
				return true;
			},
			L"Print debugging information.", L'v');

	std::wstring help_outro = L"Wallpaper Shuffler 5000\nversion ";
	help_outro += VERSION;
	help_outro += L"\nhttps://github.com/p-groarke/wallpaper-shuffler-5000/"
				  "releases\n";
	help_outro += L"Philippe Groarke <hello@philippegroarke.com>";
	opt.add_help_outro(help_outro);
	// opt.no_options_is_ok();
	if (!opt.parse_options(argc, argv)) {
		return EXIT_FAILURE;
	}

	// Figure out our save file path.
	std::filesystem::path data_filepath;
	{
		std::filesystem::path exe_dir = fea::executable_dir(argv[0]);
		std::filesystem::path save_data_filename
				= L"wallpaper-shuffler-5000.bin";
		data_filepath = exe_dir / save_data_filename;
	}

	// Register to monitor connected events to wake us up and
	// set the wallpapers again (fix Windows bug).
	HCMNOTIFICATION notif_handle{};
	{
		CM_NOTIFY_FILTER filt{};
		filt.cbSize = sizeof(CM_NOTIFY_FILTER);
		filt.Flags = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
		filt.Reserved = 0u;
		filt.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_MONITOR;
		if (CM_Register_Notification(
					&filt, nullptr, on_monitor_connect, &notif_handle)
				!= CR_SUCCESS) {
			log_error(L"Couldn't register CM notification.\n");
			return EXIT_FAILURE;
		}
	}
	fea::on_exit e
			= [notif_handle]() { CM_Unregister_Notification(notif_handle); };

	// Our com interface.
	CComPtr<IDesktopWallpaper> desktop_wallpaper;
	if (!SUCCEEDED(desktop_wallpaper.CoCreateInstance(
				__uuidof(DesktopWallpaper)))) {
		log_error(L"Couldn't CoCreateInstance.\n");
		return false;
	}

	while (true) {
		serialize_time_point next_update{};
		if (!update(desktop_wallpaper, data_filepath, img_folder, verbose,
					&next_update)) {
			return EXIT_FAILURE;
		}
		log_status(verbose, L"Sleeping until {0:%Y-%m-%d %H:%M:%S}\n",
				next_update);

		// TEMP DEBUG
		// return EXIT_SUCCESS;

		std::unique_lock lk{ cv_mutex };
		sleep_cv.wait_until(
				lk, next_update, [&]() { return monitor_connected.load(); });

		// Give time for other connection events to come in,
		// in case we have multiple displays.
		std::this_thread::sleep_for(std::chrono::milliseconds{ 250 });
		monitor_connected.store(false);
	}

	return EXIT_SUCCESS;
}

// TODO :

/*
 TEST1

 If any of the other solutions worked, try this:
	Make sure you save the theme with the two different images.
	Go to your themes folder (usually
	C:\Users\%USERNAME%\AppData\Local\Microsoft\Windows\Themes Choose the file
	with the name of your theme with the .theme extension. Right click, open
	with NotePad. Under [Control Panel\Desktop] make sure that these two
	options are present:
 MultimonBackgrounds=1
 PicturePosition=2
	Save changes.
	After you restart, your monitor wallpapers should stay the same.
*/

/*
(Windows 11)

I just fixed this on mine

	Navigate to C:\Users\"Username"\AppData\Roaming\Microsoft\Windows\Themes
	Delete the files int hat folder (Usually named transcoded)
	set individual backgrounds again
	Check folder there should now be 1 new file for each abckground

This happens because windows transcodes the image you use typically compressing
it to 85% you can get around this permanently by doing this:

	Use the Windows key + R keyboard shortcut to open the Run command.
	Type regedit, and click OK to open the registry.
	Navigate to HKEY_CURRENT_USER\Control Panel\Desktop
	On the right side, right-click, select New, and click on DWORD (32-bit)
Value. Name the DWORD JPEGImportQuality and press Enter. Double-click the newly
created DWORD, and user Base, select the Decimal option. Change the DWORD value
from 0 to 100. Default compression Windows is 85 percent usually, and if you set
the DWORD to 100 will completely disable automatic JPEG image file compression.
	Click OK.
	Close the Registry.
	Restart your computer to complete the task.
*/


/*
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>

std::mutex mtx;
std::condition_variable cv;
bool stop_flag = false;

void worker() {
	std::unique_lock<std::mutex> lock(mtx);
	// Wait until stop_flag is true OR notified by another thread
	cv.wait(lock, [] { return stop_flag; });
	std::cout << "Thread woke up and exiting.\n";
}

int main() {
	std::thread t(worker);

	// Simulate work or delay
	std::this_thread::sleep_for(std::chrono::seconds(2));

	{
		std::lock_guard<std::mutex> lock(mtx);
		stop_flag = true;
	}
	cv.notify_one(); // Wake the worker thread

	t.join();
	return 0;
}
*/
