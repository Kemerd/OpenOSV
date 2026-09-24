// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxFileDialogMac.mm - the macOS panel behind "Choose .OSV File..."
// (OfxFileDialog.h).  The Windows dialog and the shared path clean-up are in
// OfxFileDialog.cpp.
//
// NSOpenPanel is AppKit, and AppKit's UI may only be driven from the main
// thread.  kOfxActionInstanceChanged for a button press arrives from the
// host's UI, which on macOS is the main thread; when it is not, the panel is
// NOT shown - hopping to the main queue synchronously would deadlock a host
// whose main thread is waiting on this very call - and the user pastes the
// path into the field instead (logged, so the reason is on record).

#include "OfxFileDialog.h"

#include "PluginLog.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <filesystem>
#include <string>

namespace osv::ofx {

using osv::premiere::PluginLog;

std::optional<std::string> chooseOsvFile(const std::string& startPath) noexcept {
    @autoreleasepool {
        try {
            if (![NSThread isMainThread]) {
                PluginLog::warn("ofx source: Choose .OSV File was pressed off the main thread; paste the path "
                                "into the OSV File field instead");
                return std::nullopt;
            }

            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.title = @"Choose a DJI Osmo 360 clip";
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            panel.resolvesAliases = YES;

            // .OSV and .LRF only.  Types for extensions the system does not
            // know are created on the fly; a nil one is simply left out.
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            for (NSString* extension in @[ @"osv", @"OSV", @"lrf", @"LRF" ]) {
                UTType* type = [UTType typeWithFilenameExtension:extension];
                if (type && ![types containsObject:type]) {
                    [types addObject:type];
                }
            }
            if (types.count > 0) {
                panel.allowedContentTypes = types;
            }

            // Start in the current clip's folder, when there is one.
            const std::string current = cleanPath(startPath);
            if (!current.empty()) {
                std::error_code ec;
                const std::filesystem::path dir = std::filesystem::path(current).parent_path();
                if (!dir.empty() && std::filesystem::is_directory(dir, ec)) {
                    NSString* folder = [NSString stringWithUTF8String:dir.string().c_str()];
                    if (folder) {
                        panel.directoryURL = [NSURL fileURLWithPath:folder isDirectory:YES];
                    }
                }
            }

            if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) {
                return std::nullopt;  // cancelled
            }
            NSURL* url = panel.URLs.firstObject;
            const char* path = url.fileSystemRepresentation;
            if (!path || !*path) {
                return std::nullopt;
            }
            return std::string(path);
        } catch (...) {
            PluginLog::warn("ofx source: exception while showing the Open panel");
            return std::nullopt;
        }
    }
}

}  // namespace osv::ofx
