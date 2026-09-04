# LeRobot vcpkg overlays

`x265` is based on the port at the repository's pinned vcpkg baseline
`84bab45d415d22042bd0b9081aea57f362da3f35`. The upstream port builds only the
8-bit API, while LeRobot's lossless depth-video contract requires `gray12le`.
This overlay adds one `main12` feature that enables x265's `HIGH_BIT_DEPTH` and
`MAIN12` CMake options; the remaining port files track that baseline unchanged.

The root manifest requests this feature only through the opt-in `gpl-codecs`
feature. Keep the overlay synchronized with the pinned baseline whenever either
vcpkg or x265 is upgraded.
