// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Platform_mac.mm - the AppKit half of Platform.h: open panels, Finder and
// the browser through NSWorkspace, and a power assertion that keeps the Mac
// awake during a batch.  Compiled with ARC (tools/osvgui/CMakeLists.txt).
//
// AppKit's UI may only be driven from the main thread; the GLFW loop that
// calls these runs there, and a call from anywhere else returns "nothing
// picked" instead of risking a deadlock.

#include "Platform.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <string>

namespace osvgui::platform {

namespace fs = std::filesystem;

namespace {

/// The power assertion held while a batch renders (0 = none).
IOPMAssertionID g_awakeAssertion = 0;

/// Run an open panel and return the file-system paths picked.
std::vector<fs::path> runPanel(NSOpenPanel* panel) {
    std::vector<fs::path> out;
    if (!panel) {
        return out;
    }
    if ([panel runModal] != NSModalResponseOK) {
        return out;  // cancelled
    }
    for (NSURL* url in panel.URLs) {
        const char* path = url.fileSystemRepresentation;
        if (path && *path) {
            out.emplace_back(path);
        }
    }
    return out;
}

/// An NSURL for a path, or nil.
NSURL* urlForPath(const fs::path& path, bool directory) {
    NSString* text = [NSString stringWithUTF8String:path.string().c_str()];
    return text ? [NSURL fileURLWithPath:text isDirectory:directory ? YES : NO] : nil;
}

}  // namespace

std::vector<fs::path> chooseClips(void*) noexcept {
    @autoreleasepool {
        try {
            if (![NSThread isMainThread]) {
                return {};
            }
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.title = @"Add clips";
            panel.prompt = @"Add";
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = YES;
            panel.resolvesAliases = YES;
            // .OSV only; a type the system does not know is made on the fly.
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            for (NSString* extension in @[ @"osv", @"OSV" ]) {
                UTType* type = [UTType typeWithFilenameExtension:extension];
                if (type && ![types containsObject:type]) {
                    [types addObject:type];
                }
            }
            if (types.count > 0) {
                panel.allowedContentTypes = types;
            }
            return runPanel(panel);
        } catch (...) {
            return {};
        }
    }
}

std::vector<fs::path> chooseFolders(void*, const char* title, bool multiple) noexcept {
    @autoreleasepool {
        try {
            if (![NSThread isMainThread]) {
                return {};
            }
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            if (title) {
                NSString* text = [NSString stringWithUTF8String:title];
                if (text) {
                    panel.title = text;
                }
            }
            panel.prompt = @"Choose";
            panel.canChooseFiles = NO;
            panel.canChooseDirectories = YES;
            panel.canCreateDirectories = YES;
            panel.allowsMultipleSelection = multiple ? YES : NO;
            return runPanel(panel);
        } catch (...) {
            return {};
        }
    }
}

std::optional<fs::path> chooseProgram(void*, const char* title) noexcept {
    @autoreleasepool {
        try {
            if (![NSThread isMainThread]) {
                return std::nullopt;
            }
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            if (title) {
                NSString* text = [NSString stringWithUTF8String:title];
                if (text) {
                    panel.title = text;
                }
            }
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            panel.treatsFilePackagesAsDirectories = YES;
            // /opt/homebrew/bin is hidden from a plain panel; start there
            // when it exists, since that is where ffmpeg usually is.
            NSURL* brew = [NSURL fileURLWithPath:@"/opt/homebrew/bin" isDirectory:YES];
            if ([[NSFileManager defaultManager] fileExistsAtPath:brew.path]) {
                panel.directoryURL = brew;
            }
            panel.showsHiddenFiles = YES;
            auto picked = runPanel(panel);
            if (picked.empty()) {
                return std::nullopt;
            }
            return picked.front();
        } catch (...) {
            return std::nullopt;
        }
    }
}

bool haveDialogs() noexcept {
    return true;
}

void openUrl(const std::string& url) noexcept {
    @autoreleasepool {
        NSString* text = [NSString stringWithUTF8String:url.c_str()];
        NSURL* link = text ? [NSURL URLWithString:text] : nil;
        if (link) {
            [[NSWorkspace sharedWorkspace] openURL:link];
        }
    }
}

void revealInFileManager(const fs::path& path) noexcept {
    @autoreleasepool {
        try {
            std::error_code ec;
            if (fs::exists(path, ec)) {
                NSURL* url = urlForPath(path, false);
                if (url) {
                    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
                }
                return;
            }
            NSURL* folder = urlForPath(path.parent_path(), true);
            if (folder) {
                [[NSWorkspace sharedWorkspace] openURL:folder];
            }
        } catch (...) {
        }
    }
}

void openWithDefaultApp(const fs::path& path) noexcept {
    @autoreleasepool {
        try {
            NSURL* url = urlForPath(path, false);
            if (url) {
                [[NSWorkspace sharedWorkspace] openURL:url];
            }
        } catch (...) {
        }
    }
}

void keepAwake(bool awake) noexcept {
    if (awake && g_awakeAssertion == 0) {
        // Idle system sleep is held off; the display may still sleep.
        IOPMAssertionID id = 0;
        if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn,
                                        CFSTR("OpenOSV Studio is rendering"), &id) == kIOReturnSuccess) {
            g_awakeAssertion = id;
        }
    } else if (!awake && g_awakeAssertion != 0) {
        IOPMAssertionRelease(g_awakeAssertion);
        g_awakeAssertion = 0;
    }
}

}  // namespace osvgui::platform
