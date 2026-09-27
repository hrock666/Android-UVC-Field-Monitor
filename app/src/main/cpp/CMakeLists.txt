# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("uvcfieldmonitor")

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
# ============================================================
# libusb 1.0.30
#
# Expected source location:
#   app/src/main/cpp/third_party/libusb
# ============================================================

set(
        LIBUSB_ROOT
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb
)

find_library(log_lib log)
find_library(android_lib android)
find_library(egl_lib EGL)
find_library(glesv3_lib GLESv3)
find_library(m_lib m)

add_library(
        usb-1.0
        SHARED

        ${LIBUSB_ROOT}/libusb/core.c
        ${LIBUSB_ROOT}/libusb/descriptor.c
        ${LIBUSB_ROOT}/libusb/hotplug.c
        ${LIBUSB_ROOT}/libusb/io.c
        ${LIBUSB_ROOT}/libusb/sync.c
        ${LIBUSB_ROOT}/libusb/strerror.c

        ${LIBUSB_ROOT}/libusb/os/linux_usbfs.c
        ${LIBUSB_ROOT}/libusb/os/events_posix.c
        ${LIBUSB_ROOT}/libusb/os/threads_posix.c
        ${LIBUSB_ROOT}/libusb/os/linux_netlink.c
)

target_include_directories(
        usb-1.0
        PRIVATE
        ${LIBUSB_ROOT}/android
        ${LIBUSB_ROOT}/libusb
        ${LIBUSB_ROOT}/libusb/os
)

target_compile_options(
        usb-1.0
        PRIVATE
        -fvisibility=hidden
        -pthread
)

target_link_libraries(
        usb-1.0
        ${log_lib}
)

# ============================================================
# libjpeg-turbo 3.2.0 / TurboJPEG static library
#
# Pinned/manual dependency. Nothing is downloaded or built by the Android
# Studio configure step. Build libjpeg-turbo 3.2.0 once with the same NDK,
# then install it to:
#
#   third_party/libjpeg-turbo/<ANDROID_ABI>/include/turbojpeg.h
#   third_party/libjpeg-turbo/<ANDROID_ABI>/lib/libturbojpeg.a
# ============================================================

set(
        LIBJPEG_TURBO_ROOT
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjpeg-turbo/${ANDROID_ABI}
)

set(
        LIBJPEG_TURBO_HEADER
        ${LIBJPEG_TURBO_ROOT}/include/turbojpeg.h
)

set(
        LIBJPEG_TURBO_LIBRARY
        ${LIBJPEG_TURBO_ROOT}/lib/libturbojpeg.a
)

if(NOT EXISTS "${LIBJPEG_TURBO_HEADER}")
    message(FATAL_ERROR
            "Pinned libjpeg-turbo header not found: ${LIBJPEG_TURBO_HEADER}\n"
            "Install libjpeg-turbo 3.2.0 manually before configuring the app.")
endif()

if(NOT EXISTS "${LIBJPEG_TURBO_LIBRARY}")
    message(FATAL_ERROR
            "Pinned libturbojpeg.a not found: ${LIBJPEG_TURBO_LIBRARY}\n"
            "Build/install libjpeg-turbo 3.2.0 for ${ANDROID_ABI} first.")
endif()

add_library(
        turbojpeg
        STATIC
        IMPORTED
        GLOBAL
)

set_target_properties(
        turbojpeg
        PROPERTIES
        IMPORTED_LOCATION
        ${LIBJPEG_TURBO_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES
        ${LIBJPEG_TURBO_ROOT}/include
)

# ============================================================
# UVC Field Monitor
# ============================================================

add_library(
        ${CMAKE_PROJECT_NAME}
        SHARED
        native-lib.cpp
        ms2109_pu_controls.cpp
        uvc_mjpeg_decoder.cpp
        scope_gpu.cpp
        scope_ui.cpp
        uvc_device.cpp
        uvc_stream.cpp
)

target_include_directories(
        ${CMAKE_PROJECT_NAME}
        PRIVATE
        ${LIBUSB_ROOT}/libusb
)

# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
target_link_libraries(
        ${CMAKE_PROJECT_NAME}
        usb-1.0
        turbojpeg
        ${android_lib}
        ${log_lib}
        ${egl_lib}
        ${glesv3_lib}
        ${m_lib}
)
