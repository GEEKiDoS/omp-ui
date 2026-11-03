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
# SDL_ttf patches, applied to the fetched sources before they build. Each replaces text of the
# pinned release; a patch whose text is gone stops the configure (so an upgrade can't skip one).
set(SDL_TTF_C "${sdl3_ttf_SOURCE_DIR}/src/SDL_ttf.c")
file(READ "${SDL_TTF_C}" SDL_TTF_SOURCE)
set(SDL_TTF_PATCHED "${SDL_TTF_SOURCE}")
function(patch_sdl_ttf from to)
	string(REPLACE "${from}" "${to}" patched "${SDL_TTF_PATCHED}")
	string(FIND "${patched}" "${to}" found)
	if(found EQUAL -1)
		message(FATAL_ERROR "SDL_ttf patch no longer applies:\n${from}")
	endif()
	set(SDL_TTF_PATCHED "${patched}" PARENT_SCOPE)
endfunction()
# Upstream fix after 3.2.2 (libsdl-org/SDL_ttf fc8d545, "Make the fallback fonts use the same
# ascent value as the main font"): 3.2.2 puts each fallback glyph on its own face's baseline, so
# CJK and emoji from a taller face sit lower than the Latin text beside them. Drop it once the
# pinned release includes the commit.
patch_sdl_ttf("pos->y = y + F26Dot6(pos->font->ascent) - pos->y_offset;"
              "pos->y = y + F26Dot6(font->ascent) - pos->y_offset;")
# Synthetic bold and italic (not upstream). SDL_ttf shears the outline for italic but keeps the
# upright outline's box, widened by a fixed 0.2125 x font height, and TTF_Text draws the bitmap
# into that box: every sheared glyph is resampled sideways and misplaced (fat, crowded letters;
# better texture filtering only blurs it). Bold is a smear of the finished bitmap, a whole
# y_ppem/10 pixels to the right, and widens every advance by as much. Instead, synthesize on the
# outline once, right after loading: dilate it by em/24 about its own centre with the advances
# unchanged (the weight and spacing of DirectWrite's synthetic bold, measured against Chromium),
# then shear; and take the box from that outline rasterized the way it's drawn. Colour faces get
# neither: their layers render from freshly loaded glyphs.
patch_sdl_ttf([[
#define TTF_HANDLE_STYLE_BOLD(font)          ((font)->style & TTF_STYLE_BOLD)
#define TTF_HANDLE_STYLE_ITALIC(font)        ((font)->style & TTF_STYLE_ITALIC)
]] [[
#define TTF_HANDLE_STYLE_BOLD(font)          (((font)->style & TTF_STYLE_BOLD) && !FT_HAS_COLOR((font)->face))
#define TTF_HANDLE_STYLE_ITALIC(font)        (((font)->style & TTF_STYLE_ITALIC) && !FT_HAS_COLOR((font)->face))
#define TTF_BOLD_STRENGTH(font)              (FT_MulFix((font)->face->units_per_EM, (font)->face->size->metrics.y_scale) / 24)
]])
patch_sdl_ttf([[
    slot = font->face->glyph;

    if (want & CACHED_LCD) {
]] [[
    slot = font->face->glyph;

    // Synthesized styles, on the outline so that its box and its bitmap agree
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (TTF_HANDLE_STYLE_BOLD(font)) {
            FT_Outline_EmboldenXY(&slot->outline, TTF_BOLD_STRENGTH(font), TTF_BOLD_STRENGTH(font));
            FT_Outline_Translate(&slot->outline, -TTF_BOLD_STRENGTH(font) / 2, -TTF_BOLD_STRENGTH(font) / 2);
        }
        if (TTF_HANDLE_STYLE_ITALIC(font)) {
            FT_Matrix shear = { 1 << 16, GLYPH_ITALICS, 0, 1 << 16 };
            FT_Outline_Transform(&slot->outline, &shear);
        }
    }

    if (want & CACHED_LCD) {
]])
patch_sdl_ttf([[
        // Adjust for bold text
        if (TTF_HANDLE_STYLE_BOLD(font)) {
            cached->sz_width += font->glyph_overhang;
            cached->advance  += F26Dot6(font->glyph_overhang);
        }

        // Adjust for italic text
        if (TTF_HANDLE_STYLE_ITALIC(font) && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
            cached->sz_width += (GLYPH_ITALICS * font->height) >> 16;
        }
]] [[
        // Synthesized styles: the box of the synthesized outline as it rasterizes
        if ((TTF_HANDLE_STYLE_BOLD(font) || TTF_HANDLE_STYLE_ITALIC(font)) && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Glyph synthesized;
            if (FT_Get_Glyph(slot, &synthesized) == 0) {
                if (FT_Glyph_To_Bitmap(&synthesized, FT_RENDER_MODE_NORMAL, NULL, 1) == 0) {
                    const FT_BitmapGlyph box = (FT_BitmapGlyph)synthesized;
                    cached->sz_left  = box->left;
                    cached->sz_top   = box->top;
                    cached->sz_rows  = (int)box->bitmap.rows;
                    cached->sz_width = (int)box->bitmap.width;
                }
                FT_Done_Glyph(synthesized);
            }
        }
]])
patch_sdl_ttf([[
        // Handle the italic style, only for scalable fonts
        if (TTF_HANDLE_STYLE_ITALIC(font) && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Matrix shear;
            shear.xx = 1 << 16;
            shear.xy = GLYPH_ITALICS;
            shear.yx = 0;
            shear.yy = 1 << 16;
            FT_Outline_Transform(&slot->outline, &shear);
        }
]] [[
        // Italic: sheared after loading
]])
patch_sdl_ttf([[
        // Adjust for bold text
        if (TTF_HANDLE_STYLE_BOLD(font)) {
            dst->width += font->glyph_overhang;
        }
]] [[
        // Bold: emboldened after loading
]])
patch_sdl_ttf([[
        // Handle the bold style
        if (TTF_HANDLE_STYLE_BOLD(font)) {
            int row;
            // The pixmap is a little hard, we have to add and clamp
            for (row = dst->rows - 1; row >= 0; --row) {
                Uint8 *pixmap = dst->buffer + row * dst->pitch;
                int col, offset;
                // Minimal memset
                // SDL_memset(pixmap + dst->width - font->glyph_overhang, 0, font->glyph_overhang);
                for (offset = 1; offset <= font->glyph_overhang; ++offset) {
                    for (col = dst->width - 1; col > 0; --col) {
                        if (mono) {
                            pixmap[col] |= pixmap[col-1];
                        } else {
                            int pixel = (pixmap[col] + pixmap[col-1]);
                            if (pixel > NUM_GRAYS - 1) {
                                pixel = NUM_GRAYS - 1;
                            }
                            pixmap[col] = (Uint8) pixel;
                        }
                    }
                }
            }
        }
]] [[
        // Bold: no bitmap smear, the outline was emboldened
]])
patch_sdl_ttf("advance_if_bold = F26Dot6(font->glyph_overhang);" "advance_if_bold = 0; // Synthetic bold keeps the advances")
if(NOT SDL_TTF_PATCHED STREQUAL SDL_TTF_SOURCE)
	file(WRITE "${SDL_TTF_C}" "${SDL_TTF_PATCHED}")
endif()
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
