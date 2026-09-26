#pragma once

struct libusb_device_handle;

namespace ms2109_pu_controls {

// Apply the fixed MS2109 UVC Processing Unit preset used by the field monitor:
//   brightness = 0
//   contrast   = 128
//   saturation = 132
//   hue        = 0
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
