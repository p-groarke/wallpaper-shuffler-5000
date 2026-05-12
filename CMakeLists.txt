cmake_policy(SET CMP0091 NEW)
cmake_minimum_required (VERSION 3.15)
project(wallpaper-shuffler-5000 VERSION 1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(GoogleTest)
include(FetchContent)

# Build options.
# option(M3U_TESTS "Build and run tests." On)

# Get our fea_cmake helpers.
if (${FEA_CMAKE_LOCAL})
	set(FETCHCONTENT_SOURCE_DIR_FEA_CMAKE ${CMAKE_CURRENT_SOURCE_DIR}/../fea_cmake CACHE INTERNAL "")
endif()
FetchContent_Declare(fea_cmake
	GIT_REPOSITORY https://github.com/p-groarke/fea_cmake.git
	GIT_TAG main
)
FetchContent_MakeAvailable(fea_cmake)
set(FEA_CMAKE_PATH ${fea_cmake_SOURCE_DIR}/CMakeLists.txt)
include(${FEA_CMAKE_PATH})

# Use standard fea_cmake settings.
fea_standard_settings()

# Use fea clang-format file.
fea_import_clangformat()

# Use fea gitignore file.
fea_import_gitignore()

# Output to predictable location.
fea_set_output_dir(${CMAKE_BINARY_DIR}/bin)

# General options
set(CMAKE_CXX_STANDARD 23)


# FetchContent dependencies.
if (${FEA_LIBS_LOCAL})
	set(FEA_TESTS On CACHE INTERNAL "")  # Forces the value
	set(FEA_BENCHMARKS Off CACHE INTERNAL "")
else()
	set(FEA_TESTS Off CACHE INTERNAL "")
	set(FEA_BENCHMARKS Off CACHE INTERNAL "")
endif()
set(FEA_CPP20 On CACHE INTERNAL "")

# Pull fea_libs.
fea_fetch_content(fea_libs_fetch
	https://github.com/p-groarke/fea_libs.git
	main
)

# Set wil options.
set(WIL_BUILD_PACKAGING OFF CACHE INTERNAL "")
set(WIL_BUILD_TESTS OFF CACHE INTERNAL "")

# Fetch wil
fea_fetch_content(wil_fetch
	https://github.com/microsoft/wil.git
	master
)

# # To see files in IDE
# target_sources(${PROJECT_NAME} INTERFACE
# 	# $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
# 	"$<BUILD_INTERFACE:${HEADER_FILES_CPP14}>"
# 	"$<BUILD_INTERFACE:$<$<COMPILE_FEATURES:cxx_std_17>:${HEADER_FILES_CPP17}>>"
# )

# Conan dependencies.
fea_pull_conan(CONAN_MSVC_TOOLSET "${CMAKE_VS_PLATFORM_TOOLSET}"
	CONAN_ARGS "
		--build missing
		--build cascade
		--build outdated
	"
		# --profile default
		# -o fea_libs/*:with_tbb=${_FEA_WITH_TBB_PYBOOL}
		# -o fea_libs/*:with_onetbb=${_FEA_WITH_ONETBB_PYBOOL}
		# -o fea_libs/*:with_date=${_FEA_WITH_DATE_PYBOOL}
)

# find_package(TBB CONFIG REQUIRED)
# find_package(httplib CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
# find_package(openssl CONFIG REQUIRED)
# find_package(tinyxml2 CONFIG REQUIRED)


file(GLOB_RECURSE HEADERS "include/*.hpp" "include/*.h" "include/*.tpp")
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.c" "src/*.hpp" "src/*.h" "src/*.tpp" "resources/*.rc")
# add_executable(${PROJECT_NAME} ${HEADERS} ${SOURCES})
add_executable(${PROJECT_NAME} WIN32 ${HEADERS} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE include)
target_include_directories(${PROJECT_NAME} PRIVATE src) # For based paths.
target_link_libraries(${PROJECT_NAME} PRIVATE
	fea_libs
	WIL
	# simdjson::simdjson
	Cfgmgr32
	OneCoreUAP
)

fea_set_compile_options(${PROJECT_NAME} PUBLIC)
fea_static_runtime(${PROJECT_NAME})
fea_whole_program_optimization(${PROJECT_NAME} PUBLIC)

# target_compile_definitions(${PROJECT_NAME} PUBLIC _WIN32_WINNT=0x0601)
target_compile_definitions(${PROJECT_NAME} PRIVATE -DVERSION=L"${PROJECT_VERSION}")
# target_compile_definitions(${PROJECT_NAME} PRIVATE -DWIN32_LEAN_AND_MEAN -DWIN32_EXTRA_LEAN -DVC_EXTRALEAN)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/include PREFIX "Header Files" FILES ${LIB_HEADERS})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Source Files" FILES ${LIB_SOURCES})

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
# set_target_properties(${PROJECT_NAME} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "--import https://iptv-org.github.io/iptv/subdivisions/ca-qc.m3u")

# # Copy data on build.
# set(DATA_IN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
# set(DATA_OUT_DIR ${BINARY_OUT_DIR})
# add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
# 	COMMAND ${CMAKE_COMMAND} -E copy_directory ${DATA_IN_DIR} ${DATA_OUT_DIR}
# )