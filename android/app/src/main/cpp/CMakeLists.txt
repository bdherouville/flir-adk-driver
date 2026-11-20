cmake_minimum_required(VERSION 3.22.1)
project(bosonthermal C)

# android/app/src/main/cpp -> repo root (5 levels up)
get_filename_component(FLIR_REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(NOT EXISTS ${TP}/libusb/libusb/core.c)
  message(FATAL_ERROR "third_party sources missing: run android/fetch_deps.sh")
endif()

# libusb 1.0.29: sources and config as in its android/jni/libusb.mk
add_library(usb1 STATIC
  ${TP}/libusb/libusb/core.c ${TP}/libusb/libusb/descriptor.c
  ${TP}/libusb/libusb/hotplug.c ${TP}/libusb/libusb/io.c
  ${TP}/libusb/libusb/sync.c ${TP}/libusb/libusb/strerror.c
  ${TP}/libusb/libusb/os/linux_usbfs.c ${TP}/libusb/libusb/os/events_posix.c
  ${TP}/libusb/libusb/os/threads_posix.c ${TP}/libusb/libusb/os/linux_netlink.c)
target_include_directories(usb1 PRIVATE ${TP}/libusb/android ${TP}/libusb/libusb/os
  PUBLIC ${TP}/libusb/libusb)
target_compile_options(usb1 PRIVATE -fvisibility=hidden -pthread -w)

# Repo C code, unchanged (V4L2 and termios transport files are excluded)
add_library(bosonthermal SHARED
  native-lib.c
  ${FLIR_REPO}/platform/accel_generic.c
  ${FLIR_REPO}/src/colorbar.c
  ${FLIR_REPO}/src/denoise.c
  ${FLIR_REPO}/src/spotmeter.c)
target_include_directories(bosonthermal PRIVATE ${FLIR_REPO}/include)
target_compile_options(bosonthermal PRIVATE -std=c11 -O2 -Wall)
set_source_files_properties(${FLIR_REPO}/platform/accel_generic.c PROPERTIES COMPILE_OPTIONS -Wno-constant-conversion)
target_link_libraries(bosonthermal usb1 android jnigraphics log m)
