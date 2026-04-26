#include <fea/getopt/getopt.hpp>
#include <fea/numerics/random.hpp>
#include <fea/serialize/serialize.hpp>
#include <fea/serialize/serializer.hpp>
#include <fea/terminal/pipe.hpp>
#include <fea/terminal/utf8_io.hpp>
#include <fea/utility/error.hpp>
#include <fea/utility/file.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>

// Could be useful.
// HKEY_CURRENT_USER\Control Panel\Desktop\WallpaperStyle
enum class wallpaper_style : unsigned {
	tile = 0,
	center = 1,
	stretch = 2,
	fill = 3,
	fit = 4,
	span = 5,
};

// We need a clock with specified time_since_epock.
// TODO : Deal with daylight savings etc.
using serialize_clock = std::chrono::system_clock;
using serialize_time_point = std::chrono::system_clock::time_point;
using serialize_duration = std::chrono::system_clock::duration;

namespace {
const serialize_duration shuffle_interval = std::chrono::days{ 1 };
const std::array<std::wstring, 4> img_extensions{
	L".bmp",
	L".jpg",
	L".jpeg",
	L".png",
};
const std::wstring save_data_filename = L"wallpaper-shuffler-5000.bin";
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

void serialize(const std::filesystem::path& v, fea::serializer& ofs) {
	using fea::serialize;
	serialize(v.wstring(), ofs);
}

bool deserialize(fea::deserializer& ifs, std::filesystem::path& v) {
	using fea::deserialize;
	std::wstring wstr;
	if (!deserialize(ifs, wstr)) {
		return false;
	}
	v = wstr;
	return true;
}

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

int wmain(int argc, wchar_t** argv, wchar_t**) {
	fea::fast_iostreams();
	auto on_exit_reset_term = fea::utf8_io(true);

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

	// Go through the folder and gather all images.
	std::vector<std::filesystem::path> all_images;
	{
		if (verbose) {
			std::wcout << L"Scanning folder...\n";
		}

		for (const std::filesystem::path& p :
				std::filesystem::directory_iterator(img_folder)) {
			if (std::filesystem::is_directory(p)) {
				if (verbose) {
					std::wcout << std::format(L"\tSkipping sub-folder : '{}'\n",
							p.filename().wstring());
				}
				continue;
			}

			std::wstring ext = p.extension();
			if (!std::ranges::contains(img_extensions, ext)) {
				if (verbose) {
					std::wcout << std::format(L"\tSkipping non-image : '{}'\n",
							p.filename().wstring());
				}
				continue;
			}

			all_images.push_back(p);
		}

		if (all_images.empty()) {
			std::wcout << L"No images found, exiting.\n";
			return EXIT_FAILURE;
		}
		std::wcout << L"Success!\n";
	}

	// Load our save data.
	serialize_time_point last_shuffle_time{};
	last_shuffle_time;
	std::vector<img> sorted_images;
	{
		save_data data;
		std::filesystem::path exe_dir = fea::executable_dir(argv[0]);
		std::filesystem::path data_filepath = exe_dir / save_data_filename;

		if (std::filesystem::exists(data_filepath)) {
			using fea::deserialize;
			fea::deserializer ifs{ data_filepath };
			deserialize(ifs, data);

			last_shuffle_time = data.last_shuffle_time;

			auto new_end = std::remove_if(
					data.imgs.begin(), data.imgs.end(), [&](const img& v) {
						std::filesystem::path img_path
								= img_folder / v.filename;
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
			sorted_images = std::move(data.imgs);
		}
	}
	assert(std::is_partitioned(sorted_images.begin(), sorted_images.end(),
			[](const img& v) { return !v.shown; }));

	// Find new images in the folder.
	{
		std::unordered_set<std::filesystem::path> saved_images_set;
		for (const img& v : sorted_images) {
			saved_images_set.insert(v.filename);
		}

		auto new_end = std::remove_if(all_images.begin(), all_images.end(),
				[&](const std::filesystem::path& p) {
					return saved_images_set.contains(p.filename());
				});
		all_images.erase(new_end, all_images.end());
	}

	// Shuffle in the new images to the pre-existing images.
	if (!all_images.empty()) {

		std::vector<img> temp;
		temp.reserve(all_images.size());
		for (const std::filesystem::path& p : all_images) {
			assert(std::filesystem::exists(p)
					&& !std::filesystem::is_directory(p));

			temp.push_back(img{
					.shown = false,
					.file_size = std::filesystem::file_size(p),
					.filename = p.filename(),
			});
		}
		sorted_images.insert(sorted_images.begin(), temp.begin(), temp.end());

		auto notshown_end = std::find_if(sorted_images.begin(),
				sorted_images.end(), [](const img& v) { return v.shown; });
		fea::random_shuffle(sorted_images.begin(), notshown_end);
	}

	assert(std::is_partitioned(sorted_images.begin(), sorted_images.end(),
			[](const img& v) { return !v.shown; }));

	// Now check if we need to shuffle. If we do, change the background
	// and update save data.
	if (serialize_clock::now() >= last_shuffle_time + shuffle_interval) {
		last_shuffle_time = serialize_clock::now();
	}


	// std::vector<img> sorted_images;


	//{

	//	if (!std::filesystem::exists(data_filepath)) {
	//		save_data default_data{};

	//		using fea::serialize;
	//		fea::serializer ofs{ data_filepath };
	//		serialize(default_data, ofs);
	//	}

	//	if (!std::filesystem::exists(data_filepath)) {
	//		std::wcerr << std::format(L"Couldn't create save file : '{}'\n",
	//				data_filepath.wstring());
	//		return EXIT_FAILURE;
	//	}

	//	using fea::deserialize;
	//	fea::deserializer ifs{ data_filepath };
	//	deserialize(ifs, data);
	//}

	// if (data.last_img_folder.empty() || data.last_img_folder !=
	// img_folder) {
	//	// Changed image folder, recompute fully.
	//	data.shuffle_time = {};
	//	data.imgs.clear();


	//	if (verbose) {
	//		std::wcout << L"\nShuffling images...\n";
	//		fea::random_shuffle(data.imgs);
	//		std::wcout << L"Success!\n";

	//		std::wcout << L"\nSorted Images :\n";
	//		for (const img& v : data.imgs) {
	//			std::filesystem::path p = v.path;
	//			std::wcout << std::format(L"'{}', ",
	// p.filename().wstring());
	//		}
	//	}

	//	data.last_img_folder = img_folder;
	//	data.shuffle_time = std::chrono::system_clock::now();

	//	using fea::serialize;
	//	fea::serializer ofs{ data_filepath };
	//	serialize(data, ofs);
	//}

	// Does the current wallpaper match expectations.
	std::wstring path(MAX_PATH + 1, L'\0');
	if (!SystemParametersInfoW(
				SPI_GETDESKWALLPAPER, MAX_PATH, path.data(), 0u)) {
		fea::print_error_message_w(
				__FUNCTION__, __LINE__, fea::last_os_error());
		return -1;
	}
	// BOOL SystemParametersInfoW(
	//   [in]      UINT  uiAction,
	//   [in]      UINT  uiParam,
	//   [in, out] PVOID pvParam,
	//   [in]      UINT  fWinIni
	//);

	// while (true) {
	//	std::this_thread::sleep_for(std::chrono::seconds{ 2 });

	//	// Reload settings if updated.
	//	std::chrono::time_point<std::chrono::file_clock> new_timestamp
	//			= std::filesystem::last_write_time(settings_filepath);
	//	if (new_timestamp > settings_timestamp) {
	//		ui_proxy.log(L"Detected ini settings change, reloading.");
	//		ws_settings = load_ini(settings_filepath, ui_proxy);
	//		sim.update_settings(ws_settings);
	//		settings_timestamp = new_timestamp;
	//	}
	//}

	return EXIT_SUCCESS;
}

// int main() {
//     const wchar_t *path = L"C:\\image.png";
//     int result;
//     result = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void *)path,
//     SPIF_UPDATEINIFILE); std::cout << result; return 0;
// }

// class Program
//{
//     [DllImport("user32.dll")]
//     public static extern bool SystemParametersInfo(UInt32 uiAction, UInt32
//     uiParam, string pvParam, UInt32 fWinIni); static FileInfo[] images;
//     static int currentImage;
//
//     static void Main(string[] args)
//     {
//         DirectoryInfo dirInfo = new
//         DirectoryInfo(@"C:/users/Smart-PC/Desktop"); images =
//         dirInfo.GetFiles("*.bmp", SearchOption.TopDirectoryOnly);
//
//         currentImage = 0;
//
//         System.Timers.Timer imageChangeTimer = new Timer(5000);
//         imageChangeTimer.Elapsed += new
//         ElapsedEventHandler(imageChangeTimer_Elapsed);
//         imageChangeTimer.Start();
//
//         Console.ReadLine();
//     }
//
//     static void imageChangeTimer_Elapsed(object sender, ElapsedEventArgs e)
//     {
//         const uint SPI_SETDESKWALLPAPER = 30;
//         const int SPIF_UPDATEINIFILE = 0x01;
//         const int SPIF_SENDWININICHANGE = 0x02;
//         bool gk;
//         gk = SystemParametersInfo(SPI_SETDESKWALLPAPER, 0,
//         images[currentImage++].FullName, SPIF_SENDWININICHANGE |
//         SPIF_UPDATEINIFILE); Console.Write(gk);
//         Console.WriteLine(images[currentImage].FullName);
//         currentImage = (currentImage >= images.Length) ? 0 : currentImage;
//     }
// }

// SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, (void*)s.c_str(),
// SPIF_SENDCHANGE);