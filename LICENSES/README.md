# Third-party license files

This directory contains license/notice material for third-party components that
are vendored or distributed with UvcFieldMonitor.

## libusb

The project vendors libusb source code under
`app/src/main/cpp/third_party/libusb/`. The bundled source reports version
1.0.30 (`LIBUSB_MAJOR=1`, `LIBUSB_MINOR=0`, `LIBUSB_MICRO=30`) and carries the
GNU Lesser General Public License v2.1 or later.

The authoritative license copy shipped with the vendored source remains at:

`app/src/main/cpp/third_party/libusb/COPYING`

A convenience copy is included here as `libusb-LGPL-2.1-or-later.txt`.

## libjpeg-turbo / TurboJPEG

The project currently distributes an Arm64 prebuilt static archive:

`app/src/main/cpp/third_party/libjpeg-turbo/arm64-v8a/lib/libturbojpeg.a`

The bundled `turbojpeg.h` identifies version 3.2.0. The TurboJPEG API is subject
to the libjpeg-turbo licensing terms, including the IJG license conditions and
the Modified 3-Clause BSD license conditions.

This directory contains:

- `libjpeg-turbo/LICENSE.md` — exact upstream `LICENSE.md` from libjpeg-turbo
  3.2.0.
- `libjpeg-turbo/README.ijg` — exact upstream `README.ijg` from libjpeg-turbo
  3.2.0.
- `libjpeg-turbo-Modified-BSD.txt` — convenience copy of the Modified BSD
  notice preserved from the bundled `turbojpeg.h`.
- `libjpeg-turbo-IJG-NOTICE.txt` — convenience copy of the required IJG
  attribution message and source reference.

The upstream `LICENSE.md` and `README.ijg` files are retained without
modification.
