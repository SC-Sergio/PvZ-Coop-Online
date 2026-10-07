# Third-party notices

This file records optional third-party software added for cooperative networking. It does not replace the license text or notices distributed with each dependency.

## libdatachannel

- Used only when the `coop-webrtc` vcpkg feature is enabled.
- Upstream: [paullouisageneau/libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- Version resolved by the vcpkg baseline: 0.24.6.
- License: Mozilla Public License 2.0 (MPL-2.0).
- Upstream license text: [LICENSE](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE); vcpkg installs a copy at `share/libdatachannel/copyright`.
- The MPL notice and source-availability terms apply to libdatachannel and any modifications to its covered files. This project does not modify libdatachannel source files. A distributed application must preserve the dependency's license and applicable notices; consult the full license and the actual package's transitive notices when preparing a release.

The exact transitive dependency set can vary by platform, vcpkg features, and triplet. Use the installed vcpkg SPDX document (`share/libdatachannel/vcpkg.spdx.json`) together with each installed package's `copyright` file when assembling a binary release. Do not assume this short list exhausts all runtime dependency notices.
