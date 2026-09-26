#pragma once

namespace uvc_device {

// Wrap an Android UsbDeviceConnection file descriptor with libusb,
// discover the MS2109 UVC mode, negotiate/commit 720x480 YUYV 30 fps,
// and select the streaming alternate setting.
//
// The Android-owned fd remains owned by UsbDeviceConnection.
// It must stay open until close() has returned.
bool openFromAndroidFd(int fd);

// Return the VideoStreaming interface to alt 0, release it,
// close the libusb wrapper/context, and forget the Android fd.
void close();

}  // namespace uvc_device
