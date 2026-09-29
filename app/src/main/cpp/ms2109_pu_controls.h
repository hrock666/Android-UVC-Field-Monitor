#pragma once

struct libusb_device_handle;

namespace ms2109_pu_controls {

// Apply the UVC Processing Unit values from the matched calibration profile.
// When no profile is enabled the current device values are left unchanged.
//
// The VideoControl interface and Processing Unit ID are discovered from the
// active UVC descriptors. This test version uses class-specific EP0 control
// transfers without claiming the VC interface. PU failures are logged but are
// non-fatal so VideoStreaming can continue.
//
// Returns false only for an invalid/null libusb handle. PU discovery/control
// failures are intentionally treated as success by this isolation test patch.
bool applyFixedPreset(libusb_device_handle* handle);

}  // namespace ms2109_pu_controls
