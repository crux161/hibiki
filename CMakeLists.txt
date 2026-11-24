cmake_minimum_required(VERSION 3.16)
project(hibiki VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
include(CTest)

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

# ---- ATSC PSIP decoder (channel names and program listings) ----
add_library(hibiki_psip STATIC src/psip.cpp)
target_compile_options(hibiki_psip PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(BUILD_TESTING)
  add_executable(psip_test tests/psip_test.cpp)
  target_link_libraries(psip_test PRIVATE hibiki_psip)
  add_test(NAME psip COMMAND psip_test)
endif()

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
  add_executable(hibiki_sdl MACOSX_BUNDLE src/sdl_frontend.cpp src/osd.cpp src/player.cpp src/firmware.cpp)
  target_compile_definitions(hibiki_sdl PRIVATE HIBIKI_VERSION="${PROJECT_VERSION}")
  if(APPLE)
    enable_language(OBJCXX)
    target_sources(hibiki_sdl PRIVATE src/macos_menu.mm)
    set_source_files_properties(src/macos_menu.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(hibiki_sdl PRIVATE "-framework Cocoa")
  endif()
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
  target_include_directories(hibiki_sdl PRIVATE
    ${SDL3_INCLUDE_DIRS} ${SDL3_TTF_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(hibiki_sdl PRIVATE
    ${SDL3_LIBRARY_DIRS} ${SDL3_TTF_LIBRARY_DIRS} ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(hibiki_sdl PRIVATE
    hibiki_core hibiki_psip ${SDL3_LIBRARIES} ${SDL3_TTF_LIBRARIES} ${FFMPEG_LIBRARIES})
  target_compile_definitions(hibiki_sdl PRIVATE HIBIKI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_custom_command(TARGET hibiki_sdl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
      "$<TARGET_FILE:hibiki_sdl>" "${CMAKE_CURRENT_BINARY_DIR}/hibiki-tv" VERBATIM)
  target_compile_options(hibiki_sdl PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(BUILD_TESTING)
    add_executable(hibiki_firmware_tests tests/firmware_tests.cpp src/firmware.cpp)
    target_include_directories(hibiki_firmware_tests PRIVATE include ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(hibiki_firmware_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(hibiki_firmware_tests PRIVATE ${FFMPEG_LIBRARIES})
    add_test(NAME firmware_validation COMMAND hibiki_firmware_tests)

    add_executable(hibiki_frontend_tests tests/frontend_tests.cpp src/osd.cpp src/player.cpp)
    target_compile_definitions(hibiki_frontend_tests PRIVATE HIBIKI_VERSION="${PROJECT_VERSION}")
    target_include_directories(hibiki_frontend_tests PRIVATE
      ${SDL3_INCLUDE_DIRS} ${SDL3_TTF_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(hibiki_frontend_tests PRIVATE
      ${SDL3_LIBRARY_DIRS} ${SDL3_TTF_LIBRARY_DIRS} ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(hibiki_frontend_tests PRIVATE
      hibiki_core hibiki_psip ${SDL3_LIBRARIES} ${SDL3_TTF_LIBRARIES} ${FFMPEG_LIBRARIES})
    if(APPLE)
      target_sources(hibiki_frontend_tests PRIVATE src/macos_menu.mm)
      target_link_libraries(hibiki_frontend_tests PRIVATE "-framework Cocoa")
    endif()
    target_sources(hibiki_frontend_tests PRIVATE src/firmware.cpp)
    add_test(NAME frontend_controls_and_guide COMMAND hibiki_frontend_tests)
    set_tests_properties(frontend_controls_and_guide PROPERTIES TIMEOUT 10
      ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_test(NAME fx2_firmware_validation
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/wake_tuner_tests.py)
    endif()

    # ---- M0 video fixtures: generation, verification, replay diagnostics ----
    find_program(HIBIKI_FFMPEG_EXECUTABLE ffmpeg)
    find_program(HIBIKI_FFPROBE_EXECUTABLE ffprobe)
    if(Python3_Interpreter_FOUND AND HIBIKI_FFMPEG_EXECUTABLE AND HIBIKI_FFPROBE_EXECUTABLE)
      set(HIBIKI_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")
      set(_fixture_env
        "HIBIKI_FFMPEG=${HIBIKI_FFMPEG_EXECUTABLE};HIBIKI_FFPROBE=${HIBIKI_FFPROBE_EXECUTABLE}")
      add_test(NAME video_fixture_generation
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generate_fixtures.py
                --out ${HIBIKI_FIXTURE_DIR})
      set_tests_properties(video_fixture_generation PROPERTIES
        FIXTURES_SETUP hibiki_video_fixtures
        TIMEOUT 600 SKIP_RETURN_CODE 77 ENVIRONMENT "${_fixture_env}")
      add_test(NAME video_fixture_verification
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/verify_fixtures.py
                --dir ${HIBIKI_FIXTURE_DIR})
      set_tests_properties(video_fixture_verification PROPERTIES
        FIXTURES_REQUIRED hibiki_video_fixtures
        TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "${_fixture_env}")
      add_test(NAME video_fixture_replay
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/replay_check.py
                --app $<TARGET_FILE:hibiki_sdl> --dir ${HIBIKI_FIXTURE_DIR} --quick)
      set_tests_properties(video_fixture_replay PROPERTIES
        FIXTURES_REQUIRED hibiki_video_fixtures
        TIMEOUT 300 SKIP_RETURN_CODE 77
        ENVIRONMENT "${_fixture_env};SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()
  endif()
else()
  message(STATUS "hibiki_sdl skipped (need SDL3 + SDL3_ttf + FFmpeg)")
endif()
