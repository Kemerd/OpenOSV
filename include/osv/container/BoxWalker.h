// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// BoxWalker: bounds-safe iteration over ISO BMFF boxes.
//
// The walker never trusts a size field.  Every child is clamped to its parent,
// a child that claims to extend past the parent is reported once as a warning
// and ends iteration of that parent (its siblings in the grand-parent are
// still visited), nesting is capped at kMaxDepth, and a box shorter than its
// own header stops iteration instead of looping forever.
#pragma once

#include "osv/container/Box.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace osv {

class BoxWalker {
public:
    /// Deepest nesting level visited (top level is depth 0).  Real files need
    /// about 7 (moov/trak/mdia/minf/stbl/stsd/hvc1/hvcC); the cap protects
    /// against hostile files with self-referencing sizes.
    static constexpr int kMaxDepth = 16;

    /// Visitor callback.  Return false to stop iterating the current parent.
    using Visitor = std::function<bool(const BoxHeader& box)>;

    /// Parse the header of the box that starts at `offset` inside `root`,
    /// bounded by `parentEnd` (exclusive).  `depth` is stored in the result.
    /// Fails with Truncated when fewer than 8 (or 16 for largesize) bytes
    /// remain and with Malformed when the size is smaller than the header.
    /// A box whose declared size runs past `parentEnd` is returned with
    /// `truncated == true` and its size clamped (not an error).
    [[nodiscard]] static Result<BoxHeader> readHeader(ByteSpan root, std::uint64_t offset, std::uint64_t parentEnd,
                                                      int depth);

    /// Visit every box in [start, end) of `root` at nesting level `depth`.
    /// Problems are appended to `warnings` (may be null).
    static void forEachChild(ByteSpan root, std::uint64_t start, std::uint64_t end, int depth, WarningList* warnings,
                             const Visitor& visitor);

    /// Visit every child box of `parent` (its payload range).
    static void forEachChild(ByteSpan root, const BoxHeader& parent, WarningList* warnings, const Visitor& visitor);

    /// Visit every top-level box of `root`.
    static void forEachTopLevel(ByteSpan root, WarningList* warnings, const Visitor& visitor);

    /// Collect the children of `parent` into a vector.
    [[nodiscard]] static std::vector<BoxHeader> children(ByteSpan root, const BoxHeader& parent, WarningList* warnings);

    /// First child of `parent` with the given type, if any.
    [[nodiscard]] static std::optional<BoxHeader> findChild(ByteSpan root, const BoxHeader& parent, Fourcc type,
                                                            WarningList* warnings);

    /// Every child of `parent` with the given type.
    [[nodiscard]] static std::vector<BoxHeader> findChildren(ByteSpan root, const BoxHeader& parent, Fourcc type,
                                                             WarningList* warnings);

    /// Follow a path of box types starting at `parent`, e.g. {"mdia","minf","stbl"}.
    [[nodiscard]] static std::optional<BoxHeader> findPath(ByteSpan root, const BoxHeader& parent,
                                                           const std::vector<Fourcc>& path, WarningList* warnings);

    /// True for box types defined as FullBox (version + flags after the
    /// header).  'meta' is special: QuickTime writes it as a plain box, ISO
    /// as a full box; readHeader sniffs the bytes to tell them apart.
    [[nodiscard]] static bool isFullBoxType(Fourcc type) noexcept;

    /// True for the handful of box types that legitimately appear at the top
    /// level of an ISO BMFF file (used to reject non-MP4 input early).
    [[nodiscard]] static bool isKnownTopLevelType(Fourcc type) noexcept;
};

}  // namespace osv
