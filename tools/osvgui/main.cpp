// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// main.cpp - OpenOSV Studio's entry point: the window, the OpenGL context,
// Dear ImGui, and the event loop that drives App (Ui.h).
//
//     osvgui [files or folders...] [--start] [--screenshot out.png]
//            [--screenshot-delay seconds]
//
// Paths on the command line are queued as if dropped on the window.
// --start starts the batch as soon as they are queued.  --screenshot writes
// one frame of the settled window to a PNG and quits (documentation and
// visual checks); --screenshot-delay waits that much longer first (e.g. to
// catch a render in progress).
//
// The loop sleeps in glfwWaitEventsTimeout whenever nothing moves: an idle
// window costs no CPU and no GPU, which matters on a machine that is busy
// rendering.  The render worker wakes it (glfwPostEmptyEvent) when there is
// news.

#include "Platform.h"
#include "Queue.h"
#include "Settings.h"
#include "Style.h"
#include "Ui.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#if defined(__APPLE__)
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#endif
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

#include "Icon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <shellapi.h>
#endif

namespace {

using namespace osvgui;
namespace fs = std::filesystem;

// ===========================================================================
//  Command line
// ===========================================================================

struct Arguments {
    std::vector<fs::path> paths;
    bool start = false;
    fs::path screenshot;
    double screenshotDelay = 0.0;
};

Arguments parseArguments(const std::vector<std::string>& args) {
    Arguments out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--start") {
            out.start = true;
        } else if (a == "--screenshot" && i + 1 < args.size()) {
            out.screenshot = pathFromUtf8(args[++i]);
        } else if (a == "--screenshot-delay" && i + 1 < args.size()) {
            out.screenshotDelay = std::clamp(std::atof(args[++i].c_str()), 0.0, 3600.0);
        } else if (a.rfind("-psn_", 0) == 0) {
            // macOS Finder's process serial number: not a path.
        } else if (!a.empty()) {
            out.paths.push_back(pathFromUtf8(a));
        }
    }
    return out;
}

// ===========================================================================
//  A small PNG writer (stored deflate: no compression library needed)
// ===========================================================================

/// CRC-32 (IEEE), as PNG chunks need.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

/// Append a big-endian 32-bit value.
void putBE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/// Append one PNG chunk (length, type, data, CRC over type + data).
void putChunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    putBE(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t typeAt = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    putBE(out, crc32(out.data() + typeAt, 4 + data.size()));
}

/// Write top-down RGB8 pixels as a PNG.  False on any failure.
bool writePng(const fs::path& path, int width, int height, const std::vector<std::uint8_t>& rgb) {
    if (width <= 0 || height <= 0 || rgb.size() < static_cast<std::size_t>(width) * height * 3u) {
        return false;
    }
    // ---- the raw scanlines: filter byte 0 + the row ------------------------------------
    const std::size_t stride = static_cast<std::size_t>(width) * 3u;
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto* row = rgb.data() + static_cast<std::size_t>(y) * stride;
        raw.insert(raw.end(), row, row + stride);
    }
    // ---- zlib: header, stored blocks of up to 65535 bytes, Adler-32 --------------------
    std::vector<std::uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78);
    z.push_back(0x01);
    std::size_t at = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFF));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                 raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    } while (at < raw.size());
    std::uint32_t s1 = 1, s2 = 0;
    for (std::uint8_t b : raw) {
        s1 = (s1 + b) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    putBE(z, (s2 << 16) | s1);

    // ---- the file ------------------------------------------------------------------------
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    putBE(ihdr, static_cast<std::uint32_t>(width));
    putBE(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, RGB, deflate, no filter, no interlace
    putChunk(png, "IHDR", ihdr);
    putChunk(png, "IDAT", z);
    putChunk(png, "IEND", {});

#if defined(_WIN32)
    std::FILE* f = _wfopen(path.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) {
        return false;
    }
    const bool wrote = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && wrote;
}

/// Read the back buffer (after rendering, before the swap) into a PNG.
bool captureFramebuffer(const fs::path& path, int width, int height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    // OpenGL's rows are bottom-up; PNG's top-down.
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = rgba.data() + static_cast<std::size_t>(height - 1 - y) * width * 4u;
        std::uint8_t* dst = rgb.data() + static_cast<std::size_t>(y) * width * 3u;
        for (int x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    return writePng(path, width, height, rgb);
}

// ===========================================================================
//  Errors before there is a window
// ===========================================================================

std::string g_glfwError;

void onGlfwError(int code, const char* description) {
    g_glfwError = "GLFW error " + std::to_string(code) + ": " + (description ? description : "unknown");
}

/// Tell the user why the app cannot start (a dialog on Windows, stderr elsewhere).
void fatal(const std::string& message) {
#if defined(_WIN32)
    const std::wstring wide = pathFromUtf8(message).wstring();
    ::MessageBoxW(nullptr, wide.c_str(), L"OpenOSV Studio", MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "OpenOSV Studio: %s\n", message.c_str());
#endif
}

// ===========================================================================
//  Window placement
// ===========================================================================

/// The UI scale for a window on this monitor.  macOS keeps 1: its window
/// coordinates are points and the framebuffer scale does the rest.
float uiScaleFor(GLFWwindow* window) {
#if defined(__APPLE__)
    (void)window;
    return 1.0f;
#else
    float sx = 1.0f, sy = 1.0f;
    if (window) {
        glfwGetWindowContentScale(window, &sx, &sy);
    } else if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        glfwGetMonitorContentScale(monitor, &sx, &sy);
    }
    return std::clamp(std::max(sx, sy), 0.75f, 4.0f);
#endif
}

/// True when the rectangle overlaps some monitor's work area enough to grab.
bool onAnyMonitor(int x, int y, int w, int h) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; monitors && i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        const int ix = std::max(0, std::min(x + w, mx + mw) - std::max(x, mx));
        const int iy = std::max(0, std::min(y + h, my + mh) - std::max(y, my));
        if (ix >= 120 && iy >= 80) {
            return true;
        }
    }
    return false;
}

// ===========================================================================
//  The app
// ===========================================================================

App* appOf(GLFWwindow* window) {
    return window ? static_cast<App*>(glfwGetWindowUserPointer(window)) : nullptr;
}

int run(const std::vector<std::string>& rawArgs) {
    const Arguments args = parseArguments(rawArgs);
    platform::initialise();

    // ---- settings --------------------------------------------------------------------------
    AppOptions options;
    options.settingsPath = defaultSettingsPath();
    std::string loadError;
    if (!loadSettings(options.settingsPath, options.settings, &loadError)) {
        options.loadError = loadError;
    }
    options.initialPaths = args.paths;
    options.autoStart = args.start;

    // ---- GLFW and the window ------------------------------------------------------------------
    glfwSetErrorCallback(onGlfwError);
    if (!glfwInit()) {
        fatal("The window system could not start.\n" + g_glfwError);
        platform::shutdown();
        return 1;
    }
#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // shown once placed: no flash at the wrong size

    // A copy: `options` moves into the app further down.
    const WindowState ws = options.settings.window;
    const bool firstRun = ws.x == INT32_MIN || ws.y == INT32_MIN;
    int width = ws.width;
    int height = ws.height;
    if (firstRun) {
        // The default size is in design points; scale it for this monitor
        // and keep it inside the work area.
        const float scale = uiScaleFor(nullptr);
        width = static_cast<int>(1240.0f * scale);
        height = static_cast<int>(820.0f * scale);
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
            int mx = 0, my = 0, mw = 0, mh = 0;
            glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
            if (mw > 0 && mh > 0) {
                width = std::min(width, static_cast<int>(mw * 0.92));
                height = std::min(height, static_cast<int>(mh * 0.92));
            }
        }
    }
    GLFWwindow* window = glfwCreateWindow(width, height, "OpenOSV Studio", nullptr, nullptr);
    if (!window) {
        // An old driver without OpenGL 3: try whatever it offers, with the
        // GLSL the ImGui backend accepts there.
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glsl = "#version 120";
        window = glfwCreateWindow(width, height, "OpenOSV Studio", nullptr, nullptr);
    }
    if (!window) {
        fatal("No OpenGL window could be created. Update the graphics driver.\n" + g_glfwError);
        glfwTerminate();
        platform::shutdown();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Placement: where it was, when that is still on a screen; else centred.
    int winW = 0, winH = 0;
    glfwGetWindowSize(window, &winW, &winH);
    if (!firstRun && onAnyMonitor(ws.x, ws.y, winW, winH)) {
        glfwSetWindowPos(window, ws.x, ws.y);
    } else if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
        glfwSetWindowPos(window, mx + std::max(0, (mw - winW) / 2), my + std::max(0, (mh - winH) / 2));
    }
    float scale = uiScaleFor(window);
    glfwSetWindowSizeLimits(window, static_cast<int>(720 * scale), static_cast<int>(520 * scale), GLFW_DONT_CARE,
                            GLFW_DONT_CARE);

#if !defined(_WIN32) && !defined(__APPLE__)
    // Windows takes its icon from the executable's GLFW_ICON resource and
    // macOS has no window icons; elsewhere it is set here.
    {
        std::vector<std::vector<std::uint8_t>> pixels;
        std::vector<GLFWimage> images;
        for (int size : {16, 32, 48, 128}) {
            pixels.push_back(renderAppIcon(size));
        }
        const int sizes[] = {16, 32, 48, 128};
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            images.push_back(GLFWimage{sizes[i], sizes[i], pixels[i].data()});
        }
        glfwSetWindowIcon(window, static_cast<int>(images.size()), images.data());
    }
#endif

    // ---- Dear ImGui -------------------------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the app saves its own state; no imgui.ini beside the exe
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigErrorRecoveryEnableAssert = false;  // recover, never abort, on a UI mistake
    io.ConfigErrorRecoveryEnableTooltip = false;
    style::loadFonts();
    style::apply(options.settings.theme != "light", scale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);

    // ---- the app and its callbacks ------------------------------------------------------------------
    auto app = std::make_unique<App>(std::move(options));
    glfwSetWindowUserPointer(window, app.get());
    void* native = nullptr;
#if defined(_WIN32)
    native = glfwGetWin32Window(window);
#elif defined(__APPLE__)
    native = glfwGetCocoaWindow(window);
#endif
    app->attach(window, native);

    // Files and folders dropped on the window (UTF-8 paths from GLFW).
    glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
        App* a = appOf(w);
        if (!a || count <= 0 || !paths) {
            return;
        }
        std::vector<fs::path> dropped;
        for (int i = 0; i < count; ++i) {
            if (paths[i] && *paths[i]) {
                dropped.push_back(pathFromUtf8(paths[i]));
            }
        }
        a->addPaths(std::move(dropped));
        glfwFocusWindow(w);
    });
    // The close box asks the app, which may want to confirm first.
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        if (App* a = appOf(w)) {
            a->requestClose();
        }
    });
    // Moved to a monitor with another DPI: rescale the whole UI.
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow* w, float, float) {
        if (App* a = appOf(w)) {
            a->setScale(uiScaleFor(w));
        }
    });

    glfwShowWindow(window);
    if (ws.maximized) {
        glfwMaximizeWindow(window);
    }

    // ---- the loop -------------------------------------------------------------------------------------
    const auto started = std::chrono::steady_clock::now();
    double settledAt = -1.0;
    int exitCode = 0;
    while (!app->shouldQuit()) {
        const double wait = app->waitSeconds();
        if (wait <= 0.0) {
            glfwPollEvents();
        } else {
            glfwWaitEventsTimeout(wait);
        }
        // Minimised: no drawing, but the batch carries on.
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            app->tick();
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app->frame();
        ImGui::Render();

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        const ImVec4 clear = app->clearColour();
        glClearColor(clear.x, clear.y, clear.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // ---- --screenshot: once settled (and the extra delay), then quit ----------------------------
        if (!args.screenshot.empty() && fbw > 0 && fbh > 0) {
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (settledAt < 0.0 && app->settled()) {
                settledAt = now;
            }
            const bool due = (settledAt >= 0.0 && now - settledAt >= 1.2 + args.screenshotDelay) ||
                             now > 60.0 + args.screenshotDelay;
            if (due) {
                if (!captureFramebuffer(args.screenshot, fbw, fbh)) {
                    exitCode = 2;
                }
                app->requestClose();
                if (!app->shouldQuit()) {
                    // A render is running: the screenshot run ends it.
                    break;
                }
            }
        }
        glfwSwapBuffers(window);
    }

    // ---- shut down in reverse order ------------------------------------------------------------------------
    app->saveNow();
    app.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    platform::shutdown();
    return exitCode;
}

}  // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // The wide command line, as UTF-8: every path survives, whatever the
    // ANSI code page.
    std::vector<std::string> args;
    int argc = 0;
    if (wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            args.push_back(pathToUtf8(fs::path(argv[i])));
        }
        ::LocalFree(argv);
    }
    try {
        return run(args);
    } catch (const std::exception& e) {
        fatal(std::string("Unexpected error: ") + e.what());
    } catch (...) {
        fatal("Unexpected error.");
    }
    return 1;
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (argv[i]) {
            args.emplace_back(argv[i]);
        }
    }
    try {
        return run(args);
    } catch (const std::exception& e) {
        fatal(std::string("Unexpected error: ") + e.what());
    } catch (...) {
        fatal("Unexpected error.");
    }
    return 1;
}
#endif
