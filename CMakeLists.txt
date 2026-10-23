cmake_minimum_required(VERSION 3.16)
project(zeroos C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- backends -------------------------------------------------------------
# Desktop builds get the SDL simulator; on the device turn SDL off and use fbdev.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 QUIET sdl2)
if(SDL2_FOUND)
    option(ZOS_WITH_SDL "Build the SDL desktop simulator backend" ON)
else()
    option(ZOS_WITH_SDL "Build the SDL desktop simulator backend" OFF)
endif()
option(ZOS_WITH_FBDEV "Build the Linux /dev/fb + evdev backend" ON)

# ---- LVGL -----------------------------------------------------------------
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/lvgl EXCLUDE_FROM_ALL)

set(ZOS_DEFS ZOS_WITH_SDL=$<BOOL:${ZOS_WITH_SDL}> ZOS_WITH_FBDEV=$<BOOL:${ZOS_WITH_FBDEV}>)
target_compile_definitions(lvgl PUBLIC ${ZOS_DEFS})
if(ZOS_WITH_SDL)
    target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
endif()

# ---- MuPDF ----------------------------------------------------------------
# Debian / Raspberry Pi OS ship MuPDF as static libs whose third-party
# dependencies are the system libraries listed here.
pkg_check_modules(MUPDF REQUIRED mupdf)
find_library(MUPDF_THIRD mupdf-third)
set(MUPDF_LIBS ${MUPDF_LIBRARIES})
if(MUPDF_THIRD)
    list(APPEND MUPDF_LIBS ${MUPDF_THIRD})
endif()
list(APPEND MUPDF_LIBS freetype harfbuzz jbig2dec openjp2 jpeg z gumbo mujs m)

# ---- app ------------------------------------------------------------------
add_executable(zeroos
    src/main.c
    src/app.c
    src/theme.c
    src/launcher.c
    src/input_evdev.c
    src/assets/icons.c
    src/apps/stubs.c
    src/apps/reader/reader.c
    src/apps/reader/book.c
    src/apps/reader/shelf.c
)
target_compile_options(zeroos PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(zeroos PRIVATE src ${MUPDF_INCLUDE_DIRS})
target_link_libraries(zeroos PRIVATE lvgl ${MUPDF_LIBS})

install(TARGETS zeroos RUNTIME DESTINATION bin)
install(DIRECTORY assets/books/ DESTINATION share/zeroos/books)
