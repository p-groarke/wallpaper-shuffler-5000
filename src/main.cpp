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
	friend void serialize(const img& v, fea::serializer& ofs) {
		using fea::serialize;
		serialize(v.file_size, ofs);
		serialize(v.filename, ofs);
	}

	friend bool deserialize(fea::deserializer& ifs, img& v) {
		using fea::deserialize;
		if (!deserialize(ifs, v.file_size)) {
			return false;
		}
		if (!deserialize(ifs, v.filename)) {
			return false;
		}
		return true;
	}

	uint64_t file_size = 0;
	std::wstring filename;
};

struct wallpaper {
	friend void serialize(const wallpaper& v, fea::serializer& ofs) {
		using fea::serialize;
		serialize(v.display_id, ofs);
		serialize(v.img_filename, ofs);
	}

	friend bool deserialize(fea::deserializer& ifs, wallpaper& v) {
		using fea::deserialize;
		if (!deserialize(ifs, v.display_id)) {
			return false;
		}
		if (!deserialize(ifs, v.img_filename)) {
			return false;
		}
		return true;
	}

	friend auto operator<=>(const wallpaper&, const wallpaper&) = default;

	std::wstring display_id;
	std::wstring img_filename;

	// The path is not serialized, forcing runtime reconstruction thus allowing
	// image folder to change.
	std::wstring img_filepath;
};

struct save_data {
	friend void serialize(const save_data& v, fea::serializer& ofs) {
		using fea::serialize;
		serialize(v.last_shuffle_timestamp, ofs);
		serialize(v.playlist, ofs);
		serialize(v.recycle_bin, ofs);
		serialize(v.wallpapers, ofs);
	}

	friend bool deserialize(fea::deserializer& ifs, save_data& v) {
		using fea::deserialize;
		if (!deserialize(ifs, v.last_shuffle_timestamp)) {
			return false;
		}
		if (!deserialize(ifs, v.playlist)) {
			return false;
		}
		if (!deserialize(ifs, v.recycle_bin)) {
			return false;
		}
		if (!deserialize(ifs, v.wallpapers)) {
			return false;
		}
		return true;
	}

	// Last timestamp we updated and shuffled the images.
	serialize_time_point last_shuffle_timestamp{};

	// Our image queue / playlist.
	std::vector<img> playlist;

	// TODO : Used / shown images of current playlist.
	std::vector<img> recycle_bin;

	// The expected currently displayed images (to fix windows 11 bugs).
	std::vector<wallpaper> wallpapers;
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
	[[maybe_unused]]
	auto printit
			= [&]() {
				  for (auto it = first; it != last; ++it) {
					  std::wcout << std::format(L"{}, ", it->filename);
				  }
				  std::wcout << "\n";
			  };

#if 0
	auto now = std::chrono::steady_clock::now().time_since_epoch();
	unsigned t1 = unsigned(now.count());
	unsigned t2 = unsigned((++now).count());
	unsigned t3 = unsigned((++now).count());
	unsigned t4 = unsigned((++now).count());
	assert(t1 != t2 && t2 != t3 && t3 != t4);
#else
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
#endif

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

// Clean image playlist (remove deleted files, add new files, etc).
// Returns true on success.
bool refresh_data(const std::filesystem::path& img_folder, bool verbose,
		save_data* data_ptr) {
	save_data& data = *data_ptr;

	// We've reached the end of our playlist, reset it.
	if (data.playlist.empty()) {
		data.playlist = std::move(data.recycle_bin);
		data.recycle_bin = {};
		randomizeit(data.playlist.begin(), data.playlist.end());
	}

	// Remove obsolete or changed images from our playlist.
	{
		auto ret = std::ranges::remove_if(data.playlist, [&](const img& v) {
			std::filesystem::path img_path = img_folder / v.filename;
			if (!std::filesystem::exists(img_path)) {
				return true;
			}
			if (uint64_t(std::filesystem::file_size(img_path)) != v.file_size) {
				return true;
			}
			return false;
		});
		data.playlist.erase(ret.begin(), ret.end());
	}

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

		// Remove recycled images from the new images.
		std::unordered_set<std::filesystem::path> recycle_set;
		for (const img& v : data.recycle_bin) {
			recycle_set.insert(v.filename);
		}

		// Remove old images from the new images in the folder.
		std::unordered_set<std::filesystem::path> playlist_set;
		for (const img& v : data.playlist) {
			playlist_set.insert(v.filename);
		}

		auto ret = std::ranges::remove_if(
				new_images, [&](const std::filesystem::path& p) {
					return playlist_set.contains(p.filename())
						|| recycle_set.contains(p.filename());
				});
		new_images.erase(ret.begin(), ret.end());
	}

	// Shuffle in the new images into the pre-existing images.
	if (!new_images.empty()) {
		log_status(verbose, L"Adding {} new images to the playlist.\n",
				new_images.size());

		data.playlist.reserve(data.playlist.size() + new_images.size());
		for (const std::filesystem::path& p : new_images) {
			data.playlist.push_back(img{
					.file_size = std::filesystem::file_size(p),
					.filename = p.filename(),
			});
		}

		randomizeit(data.playlist.begin(), data.playlist.end());
	}

	return true;
}

// Fills vector with monitor ids.
// Returns true on success.
[[nodiscard]]
bool get_displays(const CComPtr<IDesktopWallpaper>& desktop_wallpaper,
		bool verbose, std::vector<std::wstring>* display_ids_ptr) {
	unsigned num_monitors = 0;
	if (!SUCCEEDED(
				desktop_wallpaper->GetMonitorDevicePathCount(&num_monitors))) {
		log_error(L"Couldn't get monitor count.\n");
		return false;
	}

	if (num_monitors == 0) {
		log_warning(L"Detected zero monitors, behaving as if there was one.\n");
		display_ids_ptr->push_back(L""); // This does work.
		return true;
	}

	log_status(verbose, L"Detected {} monitors :\n", num_monitors);

	// Filter bad / virtual monitors.
	display_ids_ptr->reserve(num_monitors);
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

		log_status(verbose, L"\t'{}'\n", str_ptr);
		display_ids_ptr->push_back(std::wstring{ str_ptr });
	}

	// We always want "" first, sort just to make everything predictable.
	std::sort(display_ids_ptr->begin(), display_ids_ptr->end());
	return true;
}

// Gets the currently displayed OS wallpapers.
// Returns true on success.
[[nodiscard]]
bool get_wallpapers(const CComPtr<IDesktopWallpaper>& desktop_wallpaper,
		bool verbose, std::vector<wallpaper>* wallpapers_ptr) {
	std::vector<std::wstring> displays;
	if (!get_displays(desktop_wallpaper, verbose, &displays)) {
		return false;
	}

	std::vector<wallpaper>& wallpapers = *wallpapers_ptr;
	for (size_t i = 0; i < displays.size(); ++i) {
		const wchar_t* monitor_id_str = displays[i].c_str();
		wchar_t* wallpaper_path = nullptr;
		if (!SUCCEEDED(desktop_wallpaper->GetWallpaper(
					monitor_id_str, &wallpaper_path))) {
			log_error(L"Couldn't get current wallpaper for monitor '{}'.\n",
					monitor_id_str);
			return false;
		}

		std::filesystem::path wp{ wallpaper_path };
		wallpapers.push_back(wallpaper{
				.display_id = displays[i],
				.img_filename = wp.filename().wstring(),
				.img_filepath = wp.wstring(),
		});
	}

	return true;
}

// Sets images as wallpapers.
// Returns true on success.
[[nodiscard]]
bool set_wallpapers(const CComPtr<IDesktopWallpaper>& desktop_wallpaper,
		const std::vector<wallpaper>& wallpapers, bool verbose) {
	if (wallpapers.empty()) {
		log_error(L"There should be at least 1 wallpaper to display.");
		return false;
	}

	if constexpr (fea::debug_build) {
		// Either we don't contain the virtual "" display, or it is at the
		// beginning.
		[[maybe_unused]]
		auto it = std::ranges::find(wallpapers, L"", &wallpaper::display_id);
		assert(it == wallpapers.end() || it == wallpapers.begin());
	}

	// Sleep in between calls, give some time to windows its very slow.
	const std::chrono::milliseconds sleep_time
			= std::chrono::milliseconds{ 250 };

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
			log_error(L"Wallpaper fill was reset.\n");
			return false;
		}
	}

	// Set the wallpapers for each monitor.
	log_status(verbose, L"Setting {} wallpapers :\n", wallpapers.size());
	for (size_t i = 0; i < wallpapers.size(); ++i) {
		const wallpaper& w = wallpapers[i];
		assert(w.img_filepath != L"");

		log_status(verbose, L"\t'{}' (monitor : '{}')\n", w.img_filename,
				w.display_id);

		const wchar_t* monitor_id_cstr = w.display_id.c_str();
		const wchar_t* img_path_cstr = w.img_filepath.c_str();
		if (!SUCCEEDED(desktop_wallpaper->SetWallpaper(
					monitor_id_cstr, img_path_cstr))) {
			log_error(L"Couldn't set wallpaper.\n");
			return false;
		}
		std::this_thread::sleep_for(sleep_time);

		if (wchar_t* wallpaper_path = nullptr;
				SUCCEEDED(desktop_wallpaper->GetWallpaper(
						monitor_id_cstr, &wallpaper_path))
				&& std::filesystem::path{ wallpaper_path }
						   != std::filesystem::path{ w.img_filepath }) {
			log_error(L"Wallpaper image was reset.\n");
			return false;
		}
	}

	return true;
}

// Makes sure the expected wallpapers are currently displayed.
// Returns true on success.
[[nodiscard]]
bool fix_win11(const CComPtr<IDesktopWallpaper>& desktop_wallpaper,
		const std::filesystem::path& img_folder,
		std::vector<wallpaper> expected_wallpapers, bool verbose) {
	if (expected_wallpapers.empty()) {
		// Nothing to do, not failure.
		return true;
	}
	log_status(verbose,
			L"Checking if we need to reset wallpapers (Windows 11 bug).\n");

	// Cleanup saved wallpapers.
	{
		// Reconstruct path, we only serialize filename to allow changing
		// folder.
		for (wallpaper& w : expected_wallpapers) {
			assert(w.img_filename != L"");
			w.img_filepath = img_folder / w.img_filename;
		}

		// Remove deleted files.
		auto new_end = std::remove_if(expected_wallpapers.begin(),
				expected_wallpapers.end(), [](const wallpaper& ew) {
					return !std::filesystem::exists(ew.img_filepath);
				});
		expected_wallpapers.erase(new_end, expected_wallpapers.end());
		if (expected_wallpapers.empty()) {
			return true;
		}
	}

	std::vector<wallpaper> current_wallpapers;
	if (!get_wallpapers(desktop_wallpaper, verbose, &current_wallpapers)) {
		return false;
	}

	// Remove currently shown wallpapers.
	for (const wallpaper& cw : current_wallpapers) {
		auto it = std::find(
				expected_wallpapers.begin(), expected_wallpapers.end(), cw);
		if (it != expected_wallpapers.end()) {
			expected_wallpapers.erase(it);
		}
	}

	// Remove wallpapers for displays that aren't connected anymore.
	{
		auto new_end = std::remove_if(expected_wallpapers.begin(),
				expected_wallpapers.end(), [&](const wallpaper& ew) {
					auto it = std::find_if(current_wallpapers.begin(),
							current_wallpapers.end(), [&](const wallpaper& cw) {
								return cw.display_id == ew.display_id;
							});
					return it == current_wallpapers.end();
				});
		expected_wallpapers.erase(new_end, expected_wallpapers.end());
	}

	if (expected_wallpapers.empty()) {
		return true;
	}

	log_status(verbose, L"Detected incorrect wallpapers, fixing.\n");
	return set_wallpapers(desktop_wallpaper, expected_wallpapers, verbose);
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

	bool needs_update = now >= next_update;
	// TODO : Handle new display connected.

	// If we have nothing to do, exit cleanly now.
	if (!needs_update) {
		// We may have been woken up by displays connecting.
		// If so, double check we are still displaying the right wallpapers
		// (fix win 11 being dumb).
		if (!fix_win11(
					desktop_wallpaper, img_folder, data.wallpapers, verbose)) {
			return false;
		}

		log_status(verbose, L"Shuffle not required, going back to sleep.\n");
		(*out_next_update) = next_update;
		return true;
	}

	log_status(verbose, L"Time interval reached, changing wallpaper.\n");
	(*out_next_update) = now + shuffle_interval;

	if (!refresh_data(img_folder, verbose, &data)) {
		return false;
	}

	if (data.playlist.empty()) {
		log_error(L"No images in playlist, exiting.\n");
		return false;
	}

	// Change the wallpapers.
	{
		std::vector<std::wstring> displays;
		if (!get_displays(desktop_wallpaper, verbose, &displays)) {
			return false;
		}
		assert(displays.size() >= 1);

		if (displays.size() > data.playlist.size()) {
			// We have more displays than images left in playlist.
			// The playlist will get recomputed next update, so just grab
			// a few images to fix.
			for (size_t i = data.playlist.size(); i < displays.size(); ++i) {
				if (data.recycle_bin.empty()) {
					log_error(L"More displays than images, "
							  L"please add more images.\n");
					return false;
				}
				data.playlist.push_back(data.recycle_bin.front());
				data.recycle_bin.erase(data.recycle_bin.begin());
			}
		}
		assert(data.playlist.size() >= displays.size());

		std::vector<wallpaper>& wallpapers = data.wallpapers;
		wallpapers.clear();

		// We always want to have the virtual display "" at the beginning,
		// with the same image as the first.
		// This sets the image on all displays as a backup which will be shown
		// if you disconnect and reconnect your displays in Windows 11.
		{
			// Remove the virtual display from the list if it is there.
			auto ret = std::ranges::remove(displays, std::wstring{ L"" });
			displays.erase(ret.begin(), ret.end());

			// Guarantee the first wallpaper is the virtual one.
			wallpapers.push_back(wallpaper{
					.display_id = L"",
					.img_filename = data.playlist.back().filename,
					.img_filepath = img_folder / data.playlist.back().filename,
			});
		}

		for (size_t i = 0; i < displays.size(); ++i) {
			data.recycle_bin.push_back(std::move(data.playlist.back()));
			data.playlist.pop_back();

			wallpapers.push_back(wallpaper{
					.display_id = displays[i],
					.img_filename = data.recycle_bin.back().filename,
					.img_filepath
					= img_folder / data.recycle_bin.back().filename,
			});
		}

		assert(wallpapers.front().display_id == L"");
		if (!set_wallpapers(desktop_wallpaper, wallpapers, verbose)) {
			return false;
		}

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
