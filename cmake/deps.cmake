# Dependencies: pinned release tags; shallow clones include only the shaping backends.
include(FetchContent)
if(MSVC)
	string(REPLACE "/O2" "/O1" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
	string(REPLACE "/O2" "/O1" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
	add_compile_options(/Gy /Gw)
else()
	add_compile_options("$<$<CONFIG:Release>:-Os>" -ffunction-sections -fdata-sections)
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static dependencies" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
# DIALOG stays on: the workspace folder picker (launch without a folder, new tab) uses SDL_ShowOpenFolderDialog.
foreach(subsystem AUDIO JOYSTICK HAPTIC SENSOR CAMERA HIDAPI GPU POWER)
	set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL_DIALOG ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_RENDER_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_D3D12 OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG release-3.2.28 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ ON CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
if(UNIX)
	# Linux/macOS emoji fonts use PNG-backed bitmap glyphs.
	find_package(PNG REQUIRED)
	set(FT_DISABLE_PNG OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(SDL3_ttf
	GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
	GIT_TAG release-3.2.2 GIT_SHALLOW TRUE
	GIT_SUBMODULES external/freetype external/harfbuzz
	GIT_SUBMODULES_RECURSE FALSE)
FetchContent_MakeAvailable(SDL3_ttf)
if(UNIX)
	# SDL_ttf embeds FreeType objects, so its PNG/zlib link requirements must follow.
	target_link_libraries(SDL3_ttf-static PRIVATE PNG::PNG ZLIB::ZLIB)
endif()

FetchContent_Declare(yoga
	GIT_REPOSITORY https://github.com/facebook/yoga.git
	GIT_TAG v3.2.1 GIT_SHALLOW TRUE
	SOURCE_SUBDIR yoga)
FetchContent_MakeAvailable(yoga)

# SVG pictures (```svg fences, image/svg+xml blobs). Text uses the bundled faces registered at
# runtime, so the system font scan stays off.
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LUNASVG_DISABLE_LOAD_SYSTEM_FONTS ON CACHE BOOL "" FORCE)
set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lunasvg
	GIT_REPOSITORY https://github.com/sammycage/lunasvg.git
	GIT_TAG v3.5.0 GIT_SHALLOW TRUE
	EXCLUDE_FROM_ALL) # Keeps lunasvg's own install rules (headers, static libs) out of the package.
FetchContent_MakeAvailable(lunasvg)
