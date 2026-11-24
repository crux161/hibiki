cmake_minimum_required(VERSION 3.16)
project(hibiki VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# ---- libusb-1.0: prefer pkg-config, fall back to Homebrew paths ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBUSB QUIET libusb-1.0)
endif()

if(LIBUSB_FOUND)
  set(HIBIKI_USB_INCLUDE_DIRS ${LIBUSB_INCLUDE_DIRS})
  set(HIBIKI_USB_LIBRARIES ${LIBUSB_LIBRARIES})
  set(HIBIKI_USB_LIBRARY_DIRS ${LIBUSB_LIBRARY_DIRS})
else()
  find_path(LIBUSB_INCLUDE_DIR libusb.h
            PATH_SUFFIXES libusb-1.0
            HINTS /opt/homebrew/opt/libusb/include
                  /usr/local/opt/libusb/include)
  find_library(LIBUSB_LIBRARY usb-1.0
            HINTS /opt/homebrew/opt/libusb/lib
                  /usr/local/opt/libusb/lib)
  if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
    message(FATAL_ERROR "libusb-1.0 not found. Install it (brew install libusb).")
  endif()
  set(HIBIKI_USB_INCLUDE_DIRS ${LIBUSB_INCLUDE_DIR})
  set(HIBIKI_USB_LIBRARIES ${LIBUSB_LIBRARY})
  set(HIBIKI_USB_LIBRARY_DIRS "")
endif()

# ---- core tuner library (Android-parity surface) ----
add_library(hibiki_core STATIC src/tuner.cpp)
target_include_directories(hibiki_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${HIBIKI_USB_INCLUDE_DIRS})
target_link_directories(hibiki_core PUBLIC ${HIBIKI_USB_LIBRARY_DIRS})
target_link_libraries(hibiki_core PUBLIC ${HIBIKI_USB_LIBRARIES} Threads::Threads)
target_compile_options(hibiki_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---- ATSC PSIP decoder (guide data) + its tests ----
add_library(hibiki_psip STATIC src/psip.cpp)
target_compile_options(hibiki_psip PRIVATE -Wall -Wextra -Wno-unused-parameter)
enable_testing()
add_executable(psip_test tests/psip_test.cpp)
target_link_libraries(psip_test PRIVATE hibiki_psip)
add_test(NAME psip COMMAND psip_test)

# ---- CLI ----
add_executable(hibiki src/main.cpp)
target_link_libraries(hibiki PRIVATE hibiki_core)
target_compile_options(hibiki PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---- SDL front-end (video + audio + HUD) ----
# Needs SDL3 + SDL3_ttf + FFmpeg. Homebrew's sdl3/sdl3_ttf .pc files are not on
# the default pkg-config path, so point pkg-config at them (plus harfbuzz, which
# sdl3-ttf's .pc requires). The target is skipped if any dependency is missing.
set(_hibiki_sdl_pc "")
foreach(_p /opt/homebrew/opt/sdl3/lib/pkgconfig
           /opt/homebrew/opt/sdl3_ttf/lib/pkgconfig
           /opt/homebrew/opt/harfbuzz/lib/pkgconfig
           /opt/homebrew/opt/freetype/lib/pkgconfig
           /usr/local/opt/sdl3/lib/pkgconfig
           /usr/local/opt/sdl3_ttf/lib/pkgconfig
           /usr/local/opt/harfbuzz/lib/pkgconfig
           /usr/local/opt/freetype/lib/pkgconfig)
  if(EXISTS "${_p}")
    list(APPEND _hibiki_sdl_pc "${_p}")
  endif()
endforeach()
if(_hibiki_sdl_pc)
  string(REPLACE ";" ":" _hibiki_sdl_pc "${_hibiki_sdl_pc}")
  set(ENV{PKG_CONFIG_PATH} "${_hibiki_sdl_pc}:$ENV{PKG_CONFIG_PATH}")
endif()

pkg_check_modules(SDL3 QUIET sdl3)
pkg_check_modules(SDL3_TTF QUIET sdl3-ttf)
pkg_check_modules(FFMPEG QUIET libavformat libavcodec libavutil libswscale libswresample)

if(SDL3_FOUND AND SDL3_TTF_FOUND AND FFMPEG_FOUND)
  # macOS .app bundle (Hibiki.app) with the mangosteen icon.
  add_executable(hibiki_sdl MACOSX_BUNDLE src/sdl_frontend.cpp src/osd.cpp)
  set_target_properties(hibiki_sdl PROPERTIES
    OUTPUT_NAME "Hibiki"
    MACOSX_BUNDLE_BUNDLE_NAME "Hibiki"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.hibiki.tv"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "mangosteen.icns"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in")
  set(HIBIKI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/icon/mangosteen.icns")
  if(EXISTS "${HIBIKI_ICON}")
    target_sources(hibiki_sdl PRIVATE "${HIBIKI_ICON}")
    set_source_files_properties("${HIBIKI_ICON}" PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources")
  else()
    message(WARNING "res/icon/mangosteen.icns missing; bundle will have no icon")
  endif()
  # VCR OSD font for the on-screen display / menu.
  set(HIBIKI_FONT "${CMAKE_CURRENT_SOURCE_DIR}/res/font/VCR_OSD_MONO_1.001.ttf")
  if(EXISTS "${HIBIKI_FONT}")
    target_sources(hibiki_sdl PRIVATE "${HIBIKI_FONT}")
    set_source_files_properties("${HIBIKI_FONT}" PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources/font")
  endif()
  # Let a dev build find the (gitignored, non-redistributable) firmware in the
  # source tree, or in the main checkout when building from a git worktree.
  target_compile_definitions(hibiki_sdl PRIVATE
    HIBIKI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --path-format=absolute --git-common-dir
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE _hibiki_git_common OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _hibiki_git_rc)
    if(_hibiki_git_rc EQUAL 0 AND _hibiki_git_common)
      get_filename_component(_hibiki_main_checkout "${_hibiki_git_common}" DIRECTORY)
      target_compile_definitions(hibiki_sdl PRIVATE
        HIBIKI_MAIN_CHECKOUT_DIR="${_hibiki_main_checkout}")
    endif()
  endif()
  target_include_directories(hibiki_sdl PRIVATE
    ${SDL3_INCLUDE_DIRS} ${SDL3_TTF_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(hibiki_sdl PRIVATE
    ${SDL3_LIBRARY_DIRS} ${SDL3_TTF_LIBRARY_DIRS} ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(hibiki_sdl PRIVATE
    hibiki_core hibiki_psip ${SDL3_LIBRARIES} ${SDL3_TTF_LIBRARIES} ${FFMPEG_LIBRARIES})
  target_compile_options(hibiki_sdl PRIVATE -Wall -Wextra -Wno-unused-parameter)
else()
  message(STATUS "hibiki_sdl skipped (need SDL3 + SDL3_ttf + FFmpeg)")
endif()
