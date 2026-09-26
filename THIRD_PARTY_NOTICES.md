# Third-Party Notices

UvcFieldMonitor includes and/or links against third-party open-source software.
The licenses for those components are independent of the license selected for
UvcFieldMonitor's original source code.

## libusb

**Component:** libusb  
**Vendored form:** source code  
**Version:** 1.0.30  
**Upstream tag:** `v1.0.30`  
**License:** GNU Lesser General Public License v2.1 or later
(`LGPL-2.1-or-later`)  
**Upstream:** https://github.com/libusb/libusb

UvcFieldMonitor builds the vendored libusb sources as a separate shared library
(`usb-1.0`) and links the application's native library against it.

The vendored copy is pinned to the official upstream `v1.0.30` tag. Keeping
the upstream tag recorded here makes the third-party source provenance
explicit and reproducible.

The complete LGPL text distributed with the vendored source is retained at:

- `app/src/main/cpp/third_party/libusb/COPYING`
- `LICENSES/libusb-LGPL-2.1-or-later.txt`

If the vendored libusb source is modified, keep the original notices and make
those libusb modifications available under the applicable LGPL terms.

## libjpeg-turbo / TurboJPEG

**Component:** libjpeg-turbo / TurboJPEG  
**Vendored form:** Arm64 static binary (`libturbojpeg.a`) plus `turbojpeg.h`  
**Version:** 3.2.0, as identified by the bundled header  
**License:** IJG License terms + Modified 3-Clause BSD terms applicable to the
TurboJPEG API  
**Upstream:** https://github.com/libjpeg-turbo/libjpeg-turbo

UvcFieldMonitor statically links the TurboJPEG library into its native shared
library.

Required attribution:

> This software is based in part on the work of the Independent JPEG Group.

The exact upstream license files from libjpeg-turbo 3.2.0 are retained without
modification at:

- `LICENSES/libjpeg-turbo/LICENSE.md`
- `LICENSES/libjpeg-turbo/README.ijg`

Additional convenience notices are retained at:

- `LICENSES/libjpeg-turbo-Modified-BSD.txt`
- `LICENSES/libjpeg-turbo-IJG-NOTICE.txt`

Keep these license and attribution files with the source distribution.

## Gradle/Maven dependencies

AndroidX, Material Components, JUnit, Espresso, and other Gradle-resolved
components are not vendored into this repository. Before distributing APK/AAB
binaries, generate or otherwise maintain an open-source notice covering the
actual resolved runtime dependency set and preserve each dependency's required
license notices.
