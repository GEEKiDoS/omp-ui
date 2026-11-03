cmake_minimum_required(VERSION 3.24)
project(omp_ui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
	add_compile_options(/utf-8 /W3 /permissive- /EHsc)
endif()
if(WIN32)
	add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(TP ${CMAKE_SOURCE_DIR}/third_party)
find_package(Threads REQUIRED)

# Protocol core: platform pty host, VT splitter, TSP session and document.
add_library(tsp_core STATIC src/core/vt.cpp src/core/tsp.cpp src/core/host.cpp)
if(WIN32)
	target_sources(tsp_core PRIVATE src/core/pty_win.cpp)
else()
	target_sources(tsp_core PRIVATE src/core/pty_posix.cpp)
endif()
target_include_directories(tsp_core PUBLIC src/core ${TP})
target_link_libraries(tsp_core PUBLIC Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_libraries(tsp_core PRIVATE util)
endif()

# The redistributable ConPTY must match the target, not the build host.
if(WIN32)
	set(OMP_TARGET_ARCH "${CMAKE_GENERATOR_PLATFORM}")
	if(NOT OMP_TARGET_ARCH)
		set(OMP_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	string(TOLOWER "${OMP_TARGET_ARCH}" OMP_TARGET_ARCH)
	if(OMP_TARGET_ARCH MATCHES "^(arm64|aarch64)$")
		set(OMP_TARGET_ARCH arm64)
	elseif(OMP_TARGET_ARCH MATCHES "^(win32|x86|i[3-6]86)$")
		set(OMP_TARGET_ARCH x86)
	elseif(OMP_TARGET_ARCH MATCHES "^(x64|amd64|x86_64)$")
		set(OMP_TARGET_ARCH x64)
	else()
		message(FATAL_ERROR "Unsupported ConPTY target architecture: ${OMP_TARGET_ARCH}")
	endif()
endif()

# All executables share an output directory. Stage once, not racing POST_BUILD
# copies from independently linking executables. Fonts go in `fonts/` beside the binaries.
file(GLOB OMP_FONTS CONFIGURE_DEPENDS "${TP}/fonts/*.ttf")
list(APPEND OMP_FONTS ${TP}/nerdfont/SymbolsNerdFontMono-Regular.ttf)
set(OMP_NATIVE_FILES)
if(WIN32)
	list(APPEND OMP_NATIVE_FILES
		${TP}/conpty/runtimes/win-${OMP_TARGET_ARCH}/native/conpty.dll
		${TP}/conpty/build/native/runtimes/${OMP_TARGET_ARCH}/OpenConsole.exe)
endif()
set(OMP_STAGE
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tsp-probe>/fonts"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OMP_FONTS} "$<TARGET_FILE_DIR:tsp-probe>/fonts")
if(OMP_NATIVE_FILES)
	list(APPEND OMP_STAGE COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OMP_NATIVE_FILES} "$<TARGET_FILE_DIR:tsp-probe>")
endif()
add_custom_target(omp-runtime ${OMP_STAGE})

add_executable(tsp-probe tools/tsp-probe.cpp)
target_link_libraries(tsp-probe PRIVATE tsp_core)
add_dependencies(tsp-probe omp-runtime)

# Dependencies: static SDL3, SDL_ttf with shaping, and Yoga.
include(cmake/deps.cmake)

# One notice file for the package: every component bundled with or linked into omp-ui.
set(OMP_LICENSES "${CMAKE_BINARY_DIR}/LICENSES.txt")
set(OMP_LICENSE_ENTRIES
	"Inter (font)|${TP}/fonts/Inter-OFL.txt"
	"JetBrains Mono (font)|${TP}/fonts/JetBrainsMono-OFL.txt"
	"Symbols Nerd Font (font)|${TP}/nerdfont/LICENSE"
	"SDL3|${sdl3_SOURCE_DIR}/LICENSE.txt"
	"SDL_ttf|${sdl3_ttf_SOURCE_DIR}/LICENSE.txt"
	"FreeType (FreeType License)|${sdl3_ttf_SOURCE_DIR}/external/freetype/docs/FTL.TXT"
	"HarfBuzz|${sdl3_ttf_SOURCE_DIR}/external/harfbuzz/COPYING"
	"Yoga|${yoga_SOURCE_DIR}/LICENSE"
	"lunasvg|${lunasvg_SOURCE_DIR}/LICENSE"
	"plutovg|${lunasvg_SOURCE_DIR}/plutovg/LICENSE"
	"stb_image|${TP}/stb/LICENSE"
	"simplewebp|${TP}/simplewebp/LICENSE.md"
	"simplewebp: WebM patent grant|${TP}/simplewebp/PATENTS.md"
	"JSON for Modern C++|${TP}/nlohmann/LICENSE.MIT")
if(WIN32)
	list(APPEND OMP_LICENSE_ENTRIES "Windows ConPTY (conpty.dll, OpenConsole.exe)|${TP}/conpty/LICENSE")
endif()
file(WRITE "${OMP_LICENSES}" "omp-ui includes the following third-party components. Each section holds that component's licence.\n")
foreach(entry IN LISTS OMP_LICENSE_ENTRIES)
	string(REPLACE "|" ";" parts "${entry}")
	list(GET parts 0 name)
	list(GET parts 1 file)
	file(READ "${file}" text)
	file(APPEND "${OMP_LICENSES}" "\n\n==== ${name} ====\n\n${text}")
endforeach()

# The live app and transcript replay use exactly the same retained renderer.
add_library(omp_view STATIC
	src/text/fonts.cpp src/text/spans.cpp src/text/inline_layout.cpp
	src/view/view_tree.cpp src/view/pointer.cpp src/view/search.cpp src/gfx/draw.cpp
	src/kinds/leaf.cpp src/kinds/preedit.cpp src/kinds/md.cpp src/kinds/code.cpp src/kinds/ansi.cpp
	src/kinds/data.cpp src/kinds/inline.cpp src/kinds/list_tabs.cpp src/kinds/tool_agent.cpp src/kinds/chrome.cpp
	src/kinds/prefs.cpp src/kinds/picker.cpp src/gfx/image.cpp src/app/config.cpp src/app/drive.cpp src/app/input_posix.cpp)
target_include_directories(omp_view PUBLIC src)
target_link_libraries(omp_view PUBLIC tsp_core SDL3::SDL3-static SDL3_ttf::SDL3_ttf-static yogacore PRIVATE lunasvg::lunasvg)
add_executable(omp-ui WIN32 src/app/main.cpp src/app/tabs.cpp src/app/folder_dialog.cpp src/app/window_style.cpp
	src/app/title_bar.cpp src/app/composition.cpp)
if(WIN32)
	target_sources(omp_view PRIVATE src/app/input_win.cpp)
	target_sources(omp-ui PRIVATE assets/omp-ui.rc)
	# The modern folder picker (IFileOpenDialog); title bar and backdrop styling (DWM).
	target_link_libraries(omp-ui PRIVATE ole32 shell32 uuid dwmapi dcomp)
endif()
target_link_libraries(omp-ui PRIVATE omp_view)
add_executable(tsp-replay tools/tsp-replay.cpp)
target_link_libraries(tsp-replay PRIVATE omp_view)
# Regression: a long single paragraph once took >10 s to lay out (per-word re-shaping of the whole line).
enable_testing()
add_test(NAME long-paragraph COMMAND tsp-replay ${CMAKE_SOURCE_DIR}/tests/recordings/long-paragraph.jsonl
	--screenshot ${CMAKE_BINARY_DIR}/long-paragraph.png)
set_tests_properties(long-paragraph PROPERTIES TIMEOUT 5)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(FONTCONFIG IMPORTED_TARGET fontconfig)
		if(FONTCONFIG_FOUND)
			target_link_libraries(omp_view PRIVATE PkgConfig::FONTCONFIG)
			target_compile_definitions(omp_view PRIVATE OMP_FONTCONFIG)
		endif()
	endif()
endif()
if(MSVC)
	target_compile_options(omp_view PRIVATE /O1 /Gy /Gw)
	target_link_options(omp-ui PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
	target_link_options(tsp-replay PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
	target_compile_options(omp_view PRIVATE -Os -ffunction-sections -fdata-sections)
	foreach(target omp-ui tsp-replay)
		if(APPLE)
			target_link_options(${target} PRIVATE -Wl,-dead_strip)
		else()
			target_link_options(${target} PRIVATE -Wl,--gc-sections -s)
		endif()
	endforeach()
endif()
add_dependencies(omp-ui omp-runtime)
add_dependencies(tsp-replay omp-runtime)

# Packaging (`cpack` after a Release build): omp-ui beside its fonts, licences and, on
# Windows, the redistributable ConPTY. macOS gets an .app with the files in Resources,
# which is where SDL_GetBasePath points inside a bundle.
if(APPLE)
	set_target_properties(omp-ui PROPERTIES MACOSX_BUNDLE TRUE MACOSX_BUNDLE_BUNDLE_NAME omp-ui
		MACOSX_BUNDLE_GUI_IDENTIFIER dev.omp.omp-ui MACOSX_BUNDLE_BUNDLE_VERSION 0.1)
	target_sources(omp-ui PRIVATE ${OMP_FONTS} ${OMP_LICENSES})
	set_source_files_properties(${OMP_FONTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
	set_source_files_properties(${OMP_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
	install(TARGETS omp-ui BUNDLE DESTINATION .)
	set(CPACK_GENERATOR TGZ)
else()
	install(TARGETS omp-ui RUNTIME DESTINATION .)
	install(FILES ${OMP_FONTS} DESTINATION fonts)
	install(FILES ${OMP_NATIVE_FILES} ${OMP_LICENSES} DESTINATION .)
	if(WIN32)
		install(FILES scripts/explorer-menu.ps1 DESTINATION .)
		set(CPACK_GENERATOR ZIP)
	else()
		set(CPACK_GENERATOR TGZ)
	endif()
endif()
install(FILES README.md DESTINATION .)
set(CPACK_PACKAGE_NAME omp-ui)
set(CPACK_PACKAGE_VERSION 0.1.0)
set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
