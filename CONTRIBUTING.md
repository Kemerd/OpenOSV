# Contributing to OpenOSV

Thanks for helping Osmo 360 owners get a real HDR pipeline on Windows. A few
ground rules keep the project buildable, testable and legally clean.

## DJI and Adobe material

* Never commit DJI material: no masks, remap tables, neural network models,
  LUT images, `.proto` text copied from DJI software, or SDK headers from Adobe.
* Facts about a file format (field numbers, box layouts, constants measured
  from recorded files) are fine. Code copied or transcribed from DJI software
  is not.
* Every numeric colour constant needs a provenance note in `NOTICE`.
* Every fact or parameter taken from studying DJI software goes in the list
  in `docs/LEGAL.md` section 5, with where it lives and where it came from.

## Code style

* C++20, MSVC first. `clang-format` config is in the repo; run it before you
  push. Doxygen comments on every public symbol, and comments inside functions
  every few lines explaining *why*.
* No exceptions across the public API. Return `osv::Result<T>` / `osv::Status`
  and propagate with `OSV_TRY` / `OSV_TRY_ASSIGN`.
* Defensive by default: assume every input is truncated, hostile or `nullptr`.
* No `TODO`, no stubs, no placeholder data. If a feature is not finished it
  does not merge.
* Console output stays 7-bit ASCII (Windows code pages are not your friend).

## Verified conventions

`docs/GEOMETRY.md` and `docs/COLOR.md` list the conventions that were verified
against real footage (quaternion order, crop scale, lens model, colour
anchors). Each has a test that fails if the default changes. If you believe a
convention is wrong, add a test with the evidence, do not just flip a sign.

## Tests

* `ctest --preset all` must pass locally before a pull request.
* Tests tagged `[sample]` need the sample clip (`OSV_SAMPLE_FILE`); they SKIP
  when it is absent, so please state in the PR whether you ran them.
* Golden data under `tests/golden` is produced by the independent Python
  scripts in `scripts/`; regenerate with the script, never by hand and never
  from `osvtool` output.

## Pull requests

* One topic per PR, with a short description of what changed and how you
  verified it (commands, footage used).
* Update `CHANGELOG.md` under "Unreleased".
