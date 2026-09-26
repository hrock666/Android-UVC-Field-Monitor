# Vendored libusb subset

This directory contains the Android/Linux backend subset of libusb used by
UvcFieldMonitor.

- Upstream: https://github.com/libusb/libusb
- Upstream tag: `v1.0.30`
- License: LGPL-2.1-or-later
- Build integration: UvcFieldMonitor builds these sources as the separate
  shared library `usb-1.0`.

Only files required by the current Android CMake build are retained here.
Upstream CI files, tests, examples, IDE projects, non-Android/non-Linux
backends, and upstream build-system files are intentionally omitted.

The original libusb source-file notices are preserved. The complete LGPL
v2.1 license text is retained in `COPYING`.

If the UvcFieldMonitor CMake source list changes, review this subset again
before removing or adding libusb files.
