#include <fea/getopt/getopt.hpp>
#include <fea/numerics/random.hpp>
#include <fea/serialize/serialize.hpp>
#include <fea/serialize/serializer.hpp>
#include <fea/terminal/pipe.hpp>
#include <fea/terminal/utf8_io.hpp>
#include <fea/utility/error.hpp>
#include <fea/utility/file.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <windows.h>

// We need a clock with specified time_since_epock.
// TODO : Deal with daylight savings etc.
using serialize_time_point = std::chrono::system_clock::time_point;
using serialize_duration = std::chrono::system_clock::duration;

namespace {
const serialize_duration shuffle_time = std::chrono::days{ 1 };
const std::array<std::wstring, 4> img_extensions{
	L".bmp",
	L".jpg",
	L".jpeg",
	L".png",
};
} // namespace

struct img {
	bool shown = false;
	std::wstring path;
};

struct save_data {
	std::wstring last_img_folder;
	serialize_time_point last_shuffle{};
	std::vector<img> imgs;
};

void serialize(const std::filesystem::path& v, fea::serializer& ofs) {
	using fea::serialize;
	serialize(v.wstring(), ofs);
}

void serialize(const img& v, fea::serializer& ofs) {
	using fea::serialize;
	serialize(v.shown, ofs);
	serialize(v.path, ofs);
}

void serialize(const save_data& v, fea::serializer& ofs) {
	using fea::serialize;
	serialize(v.last_img_folder, ofs);
	serialize(v.last_shuffle, ofs);
	serialize(v.imgs, ofs);
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

bool deserialize(fea::deserializer& ifs, img& v) {
	using fea::deserialize;
	if (!deserialize(ifs, v.shown)) {
		return false;
	}
	if (!deserialize(ifs, v.path)) {
		return false;
	}
	return true;
}

bool deserialize(fea::deserializer& ifs, save_data& v) {
	using fea::deserialize;
	if (!deserialize(ifs, v.last_img_folder)) {
		return false;
	}
	if (!deserialize(ifs, v.last_shuffle)) {
		return false;
	}
	if (!deserialize(ifs, v.imgs)) {
		return false;
	}
	return true;
}

// void serialize(const save_data& in, fea::ini& out) {
// }
//
// void deserialize(fea::ini& in, save_data& out) {
//	static const ws_settings_v0 defaults{};
//
//	std::string img_folder = in["User Settings"]["image_folder_path"]
//						   | defaults.img_folder.string();
//	out.img_folder = img_folder;
//
//	std::string last_img_folder = in["User Settings"]["last_image_folder_path"]
//								| defaults.last_img_folder.string();
//	out.last_img_folder = last_img_folder;
//
//	int64_t last_shuffle = in["User Settings"]["last_shuffle_time"]
//						 | defaults.last_shuffle.time_since_epoch().count();
//	out.last_shuffle
//			= serialize_time_point{ serialize_duration{ last_shuffle } };
// }

//[[nodiscard]]
// bool doit(const ws_settings&) {
//}

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

	// Load our data.
	// Create default settings file if we don't have it.
	save_data data;
	std::filesystem::path exe_dir = fea::executable_dir(argv[0]);
	const std::wstring data_filename = L"wallpaper-shuffler-5000.bin";
	std::filesystem::path data_filepath = exe_dir / data_filename;

	{

		if (!std::filesystem::exists(data_filepath)) {
			save_data default_data{};

			using fea::serialize;
			fea::serializer ofs{ data_filepath };
			serialize(default_data, ofs);
		}

		if (!std::filesystem::exists(data_filepath)) {
			std::wcerr << std::format(L"Couldn't create save file : '{}'\n",
					data_filepath.wstring());
			return EXIT_FAILURE;
		}

		using fea::deserialize;
		fea::deserializer ifs{ data_filepath };
		deserialize(ifs, data);
	}

	if (data.last_img_folder.empty() || data.last_img_folder != img_folder) {
		// Changed image folder, recompute fully.
		data.last_shuffle = {};
		data.imgs.clear();

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

			data.imgs.push_back(img{ .shown = false, .path = p.wstring() });
		}

		if (data.imgs.empty()) {
			std::wcout << L"Found no images, exiting.\n";
			return EXIT_FAILURE;
		}
		std::wcout << L"Success!\n";

		if (verbose) {
			std::wcout << L"\nShuffling images...\n";
			fea::random_shuffle(data.imgs);
			std::wcout << L"Success!\n";

			std::wcout << L"\nSorted Images :\n";
			for (const img& v : data.imgs) {
				std::filesystem::path p = v.path;
				std::wcout << std::format(L"'{}', ", p.filename().wstring());
			}
		}

		data.last_img_folder = img_folder;
		data.last_shuffle = std::chrono::system_clock::now();

		using fea::serialize;
		fea::serializer ofs{ data_filepath };
		serialize(data, ofs);
	}

	// Does the current wallpaper match expectations.
	std::wstring path(MAX_PATH, L'\0');
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