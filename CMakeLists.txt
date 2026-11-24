cmake_minimum_required(VERSION 3.16)
project(hibiki LANGUAGES CXX)

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

# ---- CLI ----
add_executable(hibiki src/main.cpp)
target_link_libraries(hibiki PRIVATE hibiki_core)
target_compile_options(hibiki PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---- SDL front-end (agreed plan step 4) ----
# Will add: add_executable(hibiki_sdl ...) linking hibiki_core + SDL2 +
# libavcodec/libswscale/libswresample. Left out until that step is started.
