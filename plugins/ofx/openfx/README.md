# OpenFX headers (vendored)

The C API headers of the OpenFX image effect standard, copied unmodified from
<https://github.com/AcademySoftwareFoundation/openfx>:

| | |
|---|---|
| Release | `OFX_Release_1.5.1` |
| Commit  | `ab779510b2655b4d11a7e01e5c521f9aa8c88976` |
| Files   | `include/*.h`, except `ofxColour.h` (the OCIO colour extension; unused) |
| Licence | BSD 3-Clause, see [LICENSE.md](LICENSE.md) |

They are vendored rather than taken from vcpkg because the `openfx` port at
the project's pinned baseline is 1.4, which predates `ofxGPURender.h`'s CUDA
properties (`kOfxImageEffectPropCudaRenderSupported` and friends). The CUDA
path of the Open 360 Reframe filter is built on those.

Only the C API is used. The C++ "Support" library is not: the plug-in speaks
to the host through the raw suites (`plugins/ofx/OfxHost.h`), so there is
nothing between the host and our code that we did not write.

To update, replace `include/` with the new release's headers, update the
table above, and rebuild. `tests/ofx` checks every action and property the
plug-in uses against a mock host that includes the same headers.
