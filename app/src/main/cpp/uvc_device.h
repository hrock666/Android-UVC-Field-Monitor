#pragma once

namespace uvc_device {

// Wrap an Android UsbDeviceConnection file descriptor with libusb,
// discover the required 1280x720 MJPEG 60 fps UVC mode, negotiate/commit it,
// and select the streaming backend/alternate setting.
//
// The Android-owned fd remains owned by UsbDeviceConnection.
// It must stay open until close() has returned.
bool openFromAndroidFd(int fd);

// Return the VideoStreaming interface to alt 0, release it,
// close the libusb wrapper/context, and forget the Android fd.
void close();

}  // namespace uvc_device
