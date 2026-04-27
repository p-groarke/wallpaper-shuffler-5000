#include <fea/getopt/getopt.hpp>
#include <fea/numerics/random.hpp>
#include <fea/serialize/serialize.hpp>
#include <fea/serialize/serializer.hpp>
#include <fea/terminal/pipe.hpp>
#include <fea/terminal/utf8_io.hpp>
#include <fea/utility/error.hpp>
#include <fea/utility/file.hpp>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
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

#include <ShObjIdl.h>
#include <shellapi.h>
#include <windows.h>

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

namespace {
inline const coinit _coinit;
#if 1
const serialize_duration shuffle_interval = std::chrono::days{ 1 };
#else
const serialize_duration shuffle_interval = std::chrono::seconds{ 5 };
#endif
const std::array<std::wstring, 4> img_extensions{
	L".bmp",
	L".jpg",
	L".jpeg",
	L".png",
};
} // namespace

struct img {
	bool shown = false;
	// uint32_t shown_times = 0;
	uint64_t file_size = 0;
	std::wstring filename;
	// std::wstring path;

	friend void serialize(const img& v, fea::serializer& ofs) {
		using fea::serialize;
		serialize(v.shown, ofs);
		serialize(v.file_size, ofs);
		serialize(v.filename, ofs);
		// serialize(v.path, ofs);
	}

	friend bool deserialize(fea::deserializer& ifs, img& v) {
		using fea::deserialize;
		if (!deserialize(ifs, v.shown)) {
			return false;
		}
		if (!deserialize(ifs, v.file_size)) {
			return false;
		}
		if (!deserialize(ifs, v.filename)) {
			return false;
		}
		// if (!deserialize(ifs, v.path)) {
		//	return false;
		// }
		return true;
	}
};

struct save_data {
	serialize_time_point last_shuffle_time{};
	// std::wstring last_img_folder;
	std::vector<img> imgs;

	friend void serialize(const save_data& v, fea::serializer& ofs) {
		using fea::serialize;
		serialize(v.last_shuffle_time, ofs);
		// serialize(v.last_img_folder, ofs);
		serialize(v.imgs, ofs);
	}

	friend bool deserialize(fea::deserializer& ifs, save_data& v) {
		using fea::deserialize;
		if (!deserialize(ifs, v.last_shuffle_time)) {
			return false;
		}
		// if (!deserialize(ifs, v.last_img_folder)) {
		//	return false;
		// }
		if (!deserialize(ifs, v.imgs)) {
			return false;
		}
		return true;
	}
};

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

// Returns true on success.
// Outputs the next update time.
bool update(const std::filesystem::path& data_filepath,
		const std::filesystem::path& img_folder, bool verbose,
		serialize_time_point* out_next_update) {

	// Load our save data.
	save_data data;
	if (std::filesystem::exists(data_filepath)) {
		if (verbose) {
			std::wcout << L"Loading saved data.\n";
		}

		using fea::deserialize;
		fea::deserializer ifs{ data_filepath };
		deserialize(ifs, data);
	}

	// If we have nothing to do, exit cleanly now.
	serialize_time_point now = serialize_clock::now();
	if (now < data.last_shuffle_time + shuffle_interval) {
		if (verbose) {
			std::wcout << L"Update interval unreached, going back to "
						  L"sleep.\n";
		}

		(*out_next_update) = data.last_shuffle_time + shuffle_interval;
		return true;
	}
	(*out_next_update) = now + shuffle_interval;

	if (verbose) {
		std::wcout << L"Time interval reached, changing wallpaper.\n";
	}

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
		if (verbose) {
			std::wcout << L"Scanning folder.\n";
		}

		for (const std::filesystem::path& p :
				std::filesystem::directory_iterator(img_folder)) {
			if (std::filesystem::is_directory(p)) {
				if (verbose) {
					std::wcout << std::format(L"\tSkipping sub-folder '{}'\n",
							p.filename().wstring());
				}
				continue;
			}

			std::wstring ext = p.extension();
			if (!std::ranges::contains(img_extensions, ext)) {
				if (verbose) {
					std::wcout << std::format(L"\tSkipping non-image '{}'\n",
							p.filename().wstring());
				}
				continue;
			}

			new_images.push_back(p);
		}

		if (new_images.empty()) {
			std::wcout << L"No images found in folder.\n";
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
		if (verbose) {
			std::wcout << std::format(
					L"Adding {} new images to the playlist.\n",
					new_images.size());
		}

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

		auto notshown_end = std::find_if(data.imgs.begin(), data.imgs.end(),
				[](const img& v) { return v.shown; });

		if (verbose) {
			std::wcout << L"Randomizing.\n";
		}

		randomizeit(data.imgs.begin(), notshown_end);

		// fea::random_shuffle(data.imgs.begin(), notshown_end);
		// fea::random_shuffle(data.imgs.begin(), notshown_end);
		// fea::random_shuffle(data.imgs.begin(), notshown_end);
		// fea::random_shuffle(data.imgs.begin(), notshown_end);
		// fea::random_shuffle(data.imgs.begin(), notshown_end);
	}
	assert(std::is_partitioned(data.imgs.begin(), data.imgs.end(),
			[](const img& v) { return !v.shown; }));

	if (std::all_of(data.imgs.begin(), data.imgs.end(),
				[](const img& v) { return v.shown; })) {
		if (verbose) {
			std::wcout
					<< L"Reached end of playlist, resetting and reshuffling.\n";
		}

		randomizeit(data.imgs.begin(), data.imgs.end());

		for (img& v : data.imgs) {
			v.shown = false;
		}
	}

	// Change the wallpaper.
	CComPtr<IDesktopWallpaper> desktop_wallpaper;
	if (!SUCCEEDED(desktop_wallpaper.CoCreateInstance(
				__uuidof(DesktopWallpaper)))) {
		std::wcerr << L"Couldn't CoCreateInstance.\n";
		return false;
	}

	unsigned num_monitors = 0;
	if (!SUCCEEDED(
				desktop_wallpaper->GetMonitorDevicePathCount(&num_monitors))) {
		std::wcerr << L"Couldn't get monitor count.\n";
		return false;
	}
	assert(num_monitors != 0);

	if (verbose) {
		std::wcout << std::format(L"Detect {} monitors :\n", num_monitors);
	}

	if (num_monitors > data.imgs.size()) {
		// Q : Soft fail?
		std::wcerr << L"More monitors than images in folder, exiting.\n";
		return false;
	}

	std::vector<std::wstring> monitor_ids;
	monitor_ids.reserve(num_monitors);
	for (unsigned i = 0; i < num_monitors; ++i) {
		wchar_t* str_ptr = nullptr;
		if (!SUCCEEDED(
					desktop_wallpaper->GetMonitorDevicePathAt(i, &str_ptr))) {
			std::wcerr << std::format(L"Couldn't get monitor {} path.\n", i);
			return false;
		}

		monitor_ids.push_back(std::wstring{ str_ptr });
		if (verbose) {
			std::wcout << std::format(L"\t{}\n", str_ptr);
		}
	}

	for (size_t i = 0; i < monitor_ids.size(); ++i) {
		img image = data.imgs.front();
		std::filesystem::path path = img_folder / image.filename;
		if (verbose) {
			std::wcout << std::format(
					L"Setting new wallpaper on monitor {} : '{}'\n", i + 1,
					image.filename);
		}

		if (!SUCCEEDED(desktop_wallpaper->SetWallpaper(
					monitor_ids[i].c_str(), path.c_str()))) {
			std::wcerr << L"Couldn't set wallpaper.\n";
			return false;
		}

		if (!SUCCEEDED(desktop_wallpaper->SetPosition(DWPOS_FILL))) {
			std::wcerr << L"Couldn't set wallpaper fill.\n";
			return false;
		}

		image.shown = true;
		data.imgs.erase(data.imgs.begin());
		data.imgs.push_back(std::move(image));
	}

	data.last_shuffle_time = now;

	// Serialize the update.
	{
		using fea::serialize;
		fea::serializer ofs{ data_filepath };
		serialize(data, ofs);
	}

	if (verbose) {
		std::wcout << L"Success!\n\n";
	}
	return true;
}

#if 1
int wmain(int argc, wchar_t** argv, wchar_t**) {
	fea::fast_iostreams();
	auto on_exit_reset_term = fea::utf8_io(true);
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR lpCmdLine, int) {
	fea::fast_iostreams();

	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(lpCmdLine, &argc);
#endif

	std::filesystem::path img_folder{};
	bool verbose = false;
	fea::get_opt<wchar_t> opt;
	opt.add_raw_option(
			L"image_folder_path",
			[&](std::wstring&& str) {
				img_folder = std::filesystem::path{ std::move(str) };
				if (img_folder.empty()) {
					std::wcerr << "No image folder provided.\n";
					return false;
				}

				img_folder = std::filesystem::absolute(img_folder);
				if (!std::filesystem::exists(img_folder)) {
					std::wcerr << std::format(
							L"Image folder doesn't exist : '{}'\n",
							img_folder.wstring());
					return false;
				}
				if (!std::filesystem::is_directory(img_folder)) {
					std::wcerr << std::format(L"Path isn't a folder : '{}'",
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

	// Maybe temp?
	opt.no_options_is_ok();

	if (!opt.parse_options(argc, argv)) {
		return EXIT_FAILURE;
	}

	std::filesystem::path exe_dir = fea::executable_dir(argv[0]);
	std::filesystem::path save_data_filename = L"wallpaper-shuffler-5000.bin";
	std::filesystem::path data_filepath = exe_dir / save_data_filename;

	// ShowWindow(GetConsoleWindow(), SW_HIDE);
	// FreeConsole();
	//  Use int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
	//  PWSTR pCmdLine, int nCmdShow); and set the last parameter to false (if
	//  you're using windows)

	while (true) {
		serialize_time_point next_update{};
		if (!update(data_filepath, img_folder, verbose, &next_update)) {
			return EXIT_FAILURE;
		}

		std::this_thread::sleep_until(next_update);
	}

	return EXIT_SUCCESS;
}
