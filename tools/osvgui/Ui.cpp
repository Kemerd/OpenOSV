// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Ui.cpp - OpenOSV Studio's window and the batch behind it (see Ui.h).
//
// Reading guide: frame() first (the layout of the whole window), then the
// background polling that feeds it (render worker, scans, tool probe), then
// the batch, then one drawing function per region, top to bottom.

#include "Ui.h"

#include "Icon.h"
#include "Platform.h"
#include "Progress.h"
#include "Style.h"
#include "Widgets.h"

#include <imgui_internal.h>  // ErrorRecoveryStoreState / TryToRecoverState
#include <imgui_stdlib.h>    // InputText with std::string

#if defined(__APPLE__)
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <system_error>
#include <utility>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F  // OpenGL 1.2; Windows' gl.h stops at 1.1
#endif

namespace osvgui {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using ui::px;

namespace {

// ---------------------------------------------------------------------------
//  Layout constants (design points; px() scales them)
// ---------------------------------------------------------------------------
constexpr float kRowHeight = 58.0f;     ///< One clip in the queue.
constexpr float kFooterHeight = 66.0f;  ///< The bar with Start / Stop.
constexpr float kHeaderHeight = 50.0f;  ///< Logo and title.
constexpr float kWideLayout = 900.0f;   ///< From here the settings sit beside the queue.
constexpr std::size_t kMaxLogLines = 20000;

/// Log line kinds (App::LogLine::kind).
constexpr int kLogOutput = 0;
constexpr int kLogProgress = 1;
constexpr int kLogError = 2;
constexpr int kLogHeading = 3;
constexpr int kLogNote = 4;

/// Where to get FFmpeg.
constexpr const char* kFfmpegUrl = "https://ffmpeg.org/download.html";
/// The guide's section on this app.
constexpr const char* kDocsUrl = "https://github.com/Kemerd/OpenOSV#openosv-studio-the-batch-app";

/// printf into a std::string.
template <typename... Args> std::string format(const char* fmt, Args... args) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), fmt, args...);
    return buf;
}

/// Seconds since `t`.
double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

/// "3 clips" / "1 clip".
std::string clipCount(std::size_t n) {
    return std::to_string(n) + (n == 1 ? " clip" : " clips");
}

/// A slider label in whole degrees ("-30°").
std::string degrees(float value) {
    // + 0.0 turns a rounded -0 into 0, so a centred slider never reads "-0°".
    return format("%.0f\xC2\xB0", static_cast<double>(std::round(value)) + 0.0);
}

/// The quality slider's label: the number and what it means.
std::string qualityLabel(float value) {
    const int crf = static_cast<int>(std::lround(value));
    const char* grade = crf <= 14 ? "archival" : crf <= 20 ? "high" : crf <= 26 ? "balanced" : "small";
    return format("%d  \xC2\xB7  %s", crf, grade);
}

/// Draw text with the bold face at `size`.
void boldText(const char* text, float size, const ImVec4& colour) {
    ImGui::PushFont(style::fonts().bold, size);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/// Draw small secondary text (no wrapping).
void smallText(const std::string& text, const ImVec4& colour) {
    ImGui::PushFont(nullptr, style::kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/// The suffixed variant of `path` for the n-th collision: "a_360 (2).mp4".
fs::path withSuffix(const fs::path& path, int n) {
    fs::path out = path.parent_path() / path.stem();
    out += " (" + std::to_string(n) + ")";
    out += path.extension();
    return out;
}

/// A key for output collisions (case-insensitive on Windows).
std::string outputKey(const fs::path& path) {
    std::string key = pathToUtf8(path.lexically_normal());
#if defined(_WIN32)
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
#endif
    return key;
}

}  // namespace

// ===========================================================================
//  Life cycle
// ===========================================================================

App::App(AppOptions options)
    : m_settings(std::move(options.settings))
    , m_settingsPath(std::move(options.settingsPath))
    , m_runner([] { glfwPostEmptyEvent(); })
    , m_autoStart(options.autoStart) {
    m_savedText = settingsToJsonText(m_settings);
    if (!options.loadError.empty()) {
        appendLog("Settings file unreadable, defaults used: " + options.loadError, kLogError);
    }
    if (!options.initialPaths.empty()) {
        addPaths(std::move(options.initialPaths));
    }
}

App::~App() {
    // A render still running (quit from the modal): end it and tidy its
    // unfinished output before the process goes.
    try {
        if (m_runner.busy()) {
            m_runner.stop();
            m_runner.reset();
            if (m_batch.currentId != 0 && !m_batch.outputExisted && !m_batch.currentOutput.empty()) {
                std::error_code ec;
                fs::remove(m_batch.currentOutput, ec);
            }
        }
        for (auto& scan : m_scans) {
            if (scan.valid()) {
                scan.wait();
            }
        }
        platform::keepAwake(false);
        platform::setTaskbarProgress(m_native, -1.0, false, false);
        if (m_logoTexture != 0) {
            const GLuint texture = m_logoTexture;
            glDeleteTextures(1, &texture);
        }
    } catch (...) {
        // Nothing more can be done on the way out.
    }
}

void App::attach(GLFWwindow* window, void* nativeHandle) {
    m_window = window;
    m_native = nativeHandle;
    createLogo();
    platform::styleTitleBar(m_native, darkTheme(), darkTheme() ? 0x121214u : 0xF2F2F7u);
    appendLog("OpenOSV Studio. Settings: " +
                  (m_settingsPath.empty() ? std::string("not saved (no location)") : pathToUtf8(m_settingsPath)),
              kLogNote);
    m_toolchain.start(m_settings);
}

void App::createLogo() {
    // The app icon, drawn at twice the size it is shown for a crisp header
    // on any display.
    const int size = 96;
    const std::vector<std::uint8_t> pixels = renderAppIcon(size);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    m_logoTexture = texture;
}

void App::setScale(float scale) noexcept {
    style::apply(darkTheme(), scale);
}

ImVec4 App::clearColour() const noexcept {
    return style::palette().window;
}

double App::waitSeconds() const noexcept {
    const bool toastVisible = ImGui::GetTime() - m_toast.shownAt < 4.0;
    if (ui::animating() || !m_scans.empty() || toastVisible || m_confirmClose) {
        return 0.0;
    }
    if (m_batch.active || m_runner.busy()) {
        return 0.25;
    }
    if (m_toolchain.probing()) {
        return 0.1;
    }
    return 1.0;
}

bool App::settled() const noexcept {
    return m_scans.empty() && m_haveTools && !m_toolchain.probing() && m_frames > 20;
}

// ===========================================================================
//  The frame
// ===========================================================================

void App::frame() {
    ++m_frames;
    ui::beginFrame();

    // An exception anywhere below (an allocation, a filesystem call) must
    // not leave ImGui's window stack half open: remember it, and unwind to
    // it if something throws.
    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);
    try {
        pollBackground();

        // ---- the root window: the whole client area -----------------------------------
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##OpenOSVStudio", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);
        ImGui::PopStyleVar(3);

        const float W = viewport->WorkSize.x;
        const float H = viewport->WorkSize.y;
        const float margin = W < px(760.0f) ? px(14.0f) : px(22.0f);
        const float gap = px(14.0f);
        const float innerW = std::max(1.0f, W - 2.0f * margin);

        // ---- top: header and banners --------------------------------------------------------
        float y = px(14.0f);
        drawHeader(margin, y, innerW);
        y += px(kHeaderHeight) + px(10.0f);
        y += drawBanners(margin, y, innerW);

        // ---- bottom up: footer, log (slides), command --------------------------------------
        const float footerH = px(kFooterHeight);
        const float logTarget = m_settings.showLog ? std::clamp(H * 0.28f, px(150.0f), px(320.0f)) : 0.0f;
        const float logH = std::max(0.0f, ui::spring(ImGui::GetID("##logHeight"), logTarget, 260.0f, 1.0f));
        const float commandH = H < px(640.0f) ? px(96.0f) : px(118.0f);
        const float bottom = H - footerH - gap * 0.6f;
        const float logY = bottom - logH;
        const float commandY = logY - (logH > 1.0f ? gap * std::min(1.0f, logH / px(40.0f)) : 0.0f) - commandH;
        const float mainH = std::max(px(120.0f), commandY - gap - y);

        // ---- the main area ----------------------------------------------------------------------
        const bool wide = innerW >= px(kWideLayout);
        ImGui::SetCursorPos(ImVec2(margin, y));
        if (wide) {
            const float rightW = std::clamp(innerW * 0.38f, px(360.0f), px(470.0f));
            const float leftW = innerW - rightW - gap;
            drawQueue(ImVec2(leftW, mainH));
            ImGui::SetCursorPos(ImVec2(margin + leftW + gap, y));
            ImGui::BeginChild("##settingsScroll", ImVec2(rightW, mainH), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);
            drawSettings(ImGui::GetContentRegionAvail().x);
            ImGui::EndChild();
        } else {
            // Narrow: one scrolling column, the queue first.
            ImGui::BeginChild("##stack", ImVec2(innerW, mainH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            const float queueH = std::max(px(250.0f), mainH * 0.55f);
            drawQueue(ImVec2(ImGui::GetContentRegionAvail().x, queueH));
            ImGui::Dummy(ImVec2(0.0f, px(4.0f)));
            drawSettings(ImGui::GetContentRegionAvail().x);
            ImGui::EndChild();
        }

        // ---- command, log, footer -------------------------------------------------------------
        ImGui::SetCursorPos(ImVec2(margin, commandY));
        drawCommand(innerW, commandH);
        if (logH > 1.0f) {
            ImGui::SetCursorPos(ImVec2(margin, logY));
            drawLog(innerW, logH);
        }
        drawFooter(0.0f, H - footerH, W, footerH);
        drawToast();
        drawCloseModal();

        // ---- keyboard ----------------------------------------------------------------------------
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O)) {
                addFolderDialog();
            } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
                addFilesDialog();
            }
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter) && !m_batch.active && canStart(nullptr)) {
                startBatch();
            }
            if (m_selectedId != 0 &&
                (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
                const ClipItem* item = m_queue.find(m_selectedId);
                if (item && item->status != ClipStatus::Rendering) {
                    m_queue.remove(m_selectedId);
                    m_selectedId = 0;
                }
            }
        }
        ImGui::End();
    } catch (const std::exception& e) {
        ImGui::ErrorRecoveryTryToRecoverState(&recovery);
        appendLog(std::string("Interface error: ") + e.what(), kLogError);
    } catch (...) {
        ImGui::ErrorRecoveryTryToRecoverState(&recovery);
        appendLog("Interface error.", kLogError);
    }
}

// ===========================================================================
//  Background: render worker, scans, tool probe, saving
// ===========================================================================

void App::tick() {
    try {
        pollBackground();
    } catch (const std::exception& e) {
        appendLog(std::string("Background error: ") + e.what(), kLogError);
    } catch (...) {
        appendLog("Background error.", kLogError);
    }
}

void App::pollBackground() {
    pollScans();
    pollToolchain();
    pollRunner();
    // The batch moves on as soon as the worker is free.  Clips that end
    // without a render (skipped, or osvtool would not start) are settled
    // here in one go, not one per frame; every pass either starts a render,
    // ends the batch, or finishes one clip, so the loop is bounded by the
    // queue.
    while (m_batch.active && !m_runner.busy() && m_batch.currentId == 0) {
        const std::size_t before = m_batch.finished;
        startNextClip();
        if (m_batch.currentId != 0 || !m_batch.active || m_batch.finished == before) {
            break;
        }
    }
    // --start: once the clips are queued and the tools are known.
    if (m_autoStart && m_scans.empty() && m_haveTools) {
        m_autoStart = false;
        if (canStart(nullptr)) {
            startBatch();
        }
    }
    updateSessionState();
    autosave(false);
}

void App::pollRunner() {
    // ---- the tool's output into the log ----------------------------------------------------
    std::vector<std::string> lines;
    m_runner.takeLines(lines);
    for (std::string& line : lines) {
        int kind = kLogOutput;
        if (parseProgressLine(line)) {
            kind = kLogProgress;
        } else if (errorMessage(line)) {
            kind = kLogError;
        }
        appendLog(std::move(line), kind);
    }
    if (m_batch.currentId == 0) {
        return;
    }

    // ---- the clip in flight ------------------------------------------------------------------
    const RunnerState state = m_runner.state();
    if (ClipItem* item = m_queue.find(m_batch.currentId)) {
        item->elapsedSec = secondsSince(m_batch.clipStart);
        if (state.haveProgress) {
            item->framesDone = state.progress.done;
            item->framesTotal = state.progress.total;
            item->fps = state.progress.fps;
            item->device = state.progress.device;
        }
    }
    if (state.finished) {
        finishClip();
    }
}

void App::pollScans() {
    for (auto it = m_scans.begin(); it != m_scans.end();) {
        if (!it->valid() || it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        ScanResult result;
        try {
            result = it->get();
        } catch (...) {
            result.problems.push_back("The scan failed.");
        }
        it = m_scans.erase(it);

        const std::size_t added = m_queue.add(result.clips);
        for (const std::string& problem : result.problems) {
            appendLog(problem, kLogError);
        }
        // One short sentence about what happened.
        std::string message;
        if (added > 0) {
            message = "Added " + clipCount(added);
        } else if (!result.clips.empty()) {
            message = "Already in the queue";
        } else {
            message = "No .OSV clips there";
        }
        if (result.skippedProxies > 0) {
            message += "  \xC2\xB7  " + std::to_string(result.skippedProxies) + " .LRF skipped";
        }
        if (result.skippedEmpty > 0) {
            message += "  \xC2\xB7  " + std::to_string(result.skippedEmpty) + " empty skipped";
        }
        if (result.truncated) {
            message += "  \xC2\xB7  stopped at 20000";
        }
        toast(message, added == 0 && result.clips.empty());
        appendLog(message, kLogNote);
    }
}

void App::pollToolchain() {
    std::optional<ToolchainReport> report = m_toolchain.take();
    if (!report) {
        return;
    }
    m_tools = std::move(*report);
    m_haveTools = true;
    for (const std::string& note : m_tools.notes) {
        appendLog(note, kLogNote);
    }
    // Remember the encoder test's answer for this ffmpeg.
    if (!m_tools.encoderCache.ffmpegPath.empty()) {
        m_settings.encoderCache = m_tools.encoderCache;
    }
    if (m_reprobe) {
        m_reprobe = false;
        m_toolchain.start(m_settings);
    }
}

void App::autosave(bool force) {
    const double now = ImGui::GetTime();
    if (!force && now - m_lastSaveCheck < 0.75) {
        return;
    }
    m_lastSaveCheck = now;
    try {
        // ---- the window, unless minimised (its size then means nothing) -------------------
        if (m_window && !glfwGetWindowAttrib(m_window, GLFW_ICONIFIED)) {
            const bool maximized = glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) != 0;
            m_settings.window.maximized = maximized;
            if (!maximized) {
                int x = 0, y = 0, w = 0, h = 0;
                glfwGetWindowPos(m_window, &x, &y);
                glfwGetWindowSize(m_window, &w, &h);
                if (w > 0 && h > 0) {
                    m_settings.window.x = x;
                    m_settings.window.y = y;
                    m_settings.window.width = w;
                    m_settings.window.height = h;
                }
            }
        }
        // ---- the command it would run, for the record ----------------------------------------
        m_settings.lastCommandLine = previewCommand();

        const std::string text = settingsToJsonText(m_settings);
        if (text.empty() || text == m_savedText || m_settingsPath.empty()) {
            return;
        }
        std::string error;
        if (saveSettings(m_settingsPath, m_settings, &error)) {
            m_savedText = text;
        } else {
            // Once per distinct failure, not every second.
            static std::string lastError;
            if (error != lastError) {
                lastError = error;
                appendLog("Can't save settings: " + error, kLogError);
            }
        }
    } catch (...) {
        // Saving is best effort; the next check tries again.
    }
}

void App::saveNow() noexcept {
    autosave(true);
}

void App::updateSessionState() {
    // Keep the machine awake while a batch runs.
    if (m_batch.active != m_sessionAwake) {
        m_sessionAwake = m_batch.active;
        platform::keepAwake(m_sessionAwake);
    }
    // Taskbar progress: the batch's overall fraction.
    double fraction = -1.0;
    if (m_batch.active) {
        const std::size_t queued = m_queue.count(ClipStatus::Queued);
        const std::size_t current = m_batch.currentId != 0 ? 1 : 0;
        double currentFraction = 0.0;
        if (const ClipItem* item = m_queue.find(m_batch.currentId); item && item->framesTotal > 0) {
            currentFraction = static_cast<double>(item->framesDone) / static_cast<double>(item->framesTotal);
        }
        const double total = static_cast<double>(m_batch.finished + queued + current);
        fraction = total > 0.0 ? (static_cast<double>(m_batch.finished) + currentFraction) / total : 0.0;
    }
    if (std::fabs(fraction - m_taskbarFraction) > 0.004) {
        m_taskbarFraction = fraction;
        platform::setTaskbarProgress(m_native, fraction, m_batch.pauseAfterCurrent, m_batch.failed > 0);
    }
}

// ===========================================================================
//  Adding clips
// ===========================================================================

void App::addPaths(std::vector<fs::path> paths) {
    if (paths.empty()) {
        return;
    }
    try {
        // The scan can take a while on a big card or a network share: off
        // the UI thread, with the result collected in pollScans().  A copy
        // goes to the worker, so `paths` is intact for the fallback below.
        m_scans.push_back(std::async(std::launch::async, [paths] { return scanPaths(paths); }));
    } catch (...) {
        // No thread to be had: scan here rather than drop the paths.
        std::promise<ScanResult> done;
        done.set_value(scanPaths(paths));
        m_scans.push_back(done.get_future());
    }
}

void App::addFilesDialog() {
    std::vector<fs::path> picked = platform::chooseClips(m_native);
    if (!picked.empty()) {
        addPaths(std::move(picked));
    }
}

void App::addFolderDialog() {
    std::vector<fs::path> picked = platform::chooseFolders(m_native, "Add folders", true);
    if (!picked.empty()) {
        addPaths(std::move(picked));
    }
}

// ===========================================================================
//  The batch
// ===========================================================================

bool App::ffmpegMissing() const noexcept {
    return m_haveTools && m_tools.ffmpeg.empty();
}

bool App::canStart(std::string* why) const {
    const auto no = [why](const char* reason) {
        if (why) {
            *why = reason;
        }
        return false;
    };
    if (m_batch.active) {
        return no("Already rendering.");
    }
    if (!m_haveTools) {
        return no("Still checking osvtool and FFmpeg.");
    }
    if (m_tools.osvtool.empty()) {
        return no("osvtool isn't here.");
    }
    if (outputIsVideo(m_settings) && m_tools.ffmpeg.empty()) {
        return no("FFmpeg is missing; video output needs it.");
    }
    if (!m_queue.firstQueued()) {
        return no(m_queue.empty() ? "Add clips first." : "Nothing left to render.");
    }
    return true;
}

void App::startBatch() {
    std::string why;
    if (!canStart(&why)) {
        toast(why, true);
        return;
    }
    // A snapshot: the settings are locked while it runs, and a clip added
    // mid-batch renders exactly like the others.
    m_batch = Batch{};
    m_batch.active = true;
    m_batch.settings = m_settings;
    m_batch.caps = m_tools.caps;
    m_batch.osvtool = m_tools.osvtool;
    const std::size_t queued = m_queue.count(ClipStatus::Queued);
    appendLog("Batch: " + clipCount(queued), kLogHeading);
    startNextClip();
}

void App::startNextClip() {
    ClipItem* next = m_queue.firstQueued();
    if (!next) {
        finishBatch("done");
        return;
    }

    // ---- where it goes; two clips never share an output within a batch ----------------
    fs::path output = outputPathFor(m_batch.settings, next->path);
    for (int n = 2; m_batch.outputs.count(outputKey(output)) > 0 && n < 1000; ++n) {
        output = withSuffix(outputPathFor(m_batch.settings, next->path), n);
    }
    m_batch.outputs.insert(outputKey(output));
    next->output = output;
    next->framesDone = 0;
    next->framesTotal = 0;
    next->fps = 0.0;
    next->device.clear();
    next->message.clear();
    next->elapsedSec = 0.0;

    std::error_code ec;
    // ---- "Skip clips already rendered" ------------------------------------------------------
    if (m_batch.settings.skipExisting && fs::exists(output, ec) && fs::file_size(output, ec) > 0 && !ec) {
        next->status = ClipStatus::Skipped;
        next->message = "Already rendered";
        ++m_batch.finished;
        appendLog("Skipped " + next->name + ": " + pathToUtf8(output.filename()) + " is already there.", kLogNote);
        return;  // the next frame starts the one after
    }

    // ---- the output folder must exist ---------------------------------------------------------
    if (output.has_parent_path() && !fs::is_directory(output.parent_path(), ec)) {
        fs::create_directories(output.parent_path(), ec);
        if (ec) {
            next->status = ClipStatus::Failed;
            next->message = "Can't create " + pathToUtf8(output.parent_path()) + ": " + ec.message();
            ++m_batch.finished;
            ++m_batch.failed;
            appendLog(next->message, kLogError);
            return;
        }
    }

    // ---- start osvtool ---------------------------------------------------------------------------
    const std::vector<std::string> args =
        buildRenderArgs(m_batch.settings, m_batch.caps, pathToUtf8(next->path), pathToUtf8(output));
    std::vector<std::string> argv{pathToUtf8(m_batch.osvtool)};
    argv.insert(argv.end(), args.begin(), args.end());
    appendLog(next->name, kLogHeading);
    appendLog(joinCommandLine(argv, nativeShellStyle()), kLogNote);

    m_batch.outputExisted = fs::exists(output, ec);
    std::string error;
    if (!m_runner.start(m_batch.osvtool, args, &error)) {
        next->status = ClipStatus::Failed;
        next->message = error;
        ++m_batch.finished;
        ++m_batch.failed;
        appendLog(error, kLogError);
        return;
    }
    next->status = ClipStatus::Rendering;
    m_batch.currentId = next->id;
    m_batch.currentOutput = output;
    m_batch.clipStart = Clock::now();
}

void App::finishClip() {
    const RunnerState state = m_runner.state();
    m_runner.reset();
    ClipItem* item = m_queue.find(m_batch.currentId);
    const std::uint64_t id = m_batch.currentId;
    m_batch.currentId = 0;
    ++m_batch.finished;
    if (!item) {
        return;  // removed meanwhile (the UI does not allow it, but be safe)
    }
    (void)id;
    item->elapsedSec = secondsSince(m_batch.clipStart);
    if (state.haveProgress) {
        item->framesDone = state.progress.done;
        item->framesTotal = state.progress.total;
        item->fps = state.progress.fps;
        item->device = state.progress.device;
    }

    std::error_code ec;
    if (state.stopRequested) {
        // ---- stopped: an unfinished file is no file at all ------------------------------------
        item->status = ClipStatus::Stopped;
        item->message = "Stopped";
        if (!m_batch.outputExisted && !m_batch.currentOutput.empty()) {
            fs::remove(m_batch.currentOutput, ec);
        }
        appendLog("Stopped " + item->name + ".", kLogNote);
        m_batch.active = false;
        m_batch.paused = m_queue.firstQueued() != nullptr;
    } else if (state.exitCode == 0) {
        // ---- done -----------------------------------------------------------------------------------
        item->status = ClipStatus::Done;
        m_batch.doneSeconds += item->elapsedSec;
        m_batch.doneBytes += item->sizeBytes;
        appendLog("Done: " + pathToUtf8(item->output) + " in " + formatDuration(item->elapsedSec) + ".", kLogNote);
    } else {
        // ---- failed: the tool's own words -------------------------------------------------------------
        item->status = ClipStatus::Failed;
        item->message = !state.lastError.empty()  ? state.lastError
                        : !state.lastLine.empty() ? state.lastLine
                                                  : "osvtool exited with code " + std::to_string(state.exitCode);
        ++m_batch.failed;
        appendLog("Failed: " + item->name + ": " + item->message, kLogError);
    }

    // ---- pause after this clip ---------------------------------------------------------------------
    if (m_batch.active && m_batch.pauseAfterCurrent) {
        m_batch.active = false;
        m_batch.paused = m_queue.firstQueued() != nullptr;
        m_batch.pauseAfterCurrent = false;
        appendLog("Paused.", kLogNote);
        if (!m_batch.paused) {
            finishBatch("done");
        }
    }
}

void App::finishBatch(const char* why) {
    m_batch.active = false;
    m_batch.pauseAfterCurrent = false;
    m_batch.paused = false;
    const std::size_t done = m_batch.finished - m_batch.failed;
    std::string summary = "Batch " + std::string(why ? why : "done") + ": " + std::to_string(done) + " finished";
    if (m_batch.failed > 0) {
        summary += ", " + std::to_string(m_batch.failed) + " failed";
    }
    appendLog(summary + ".", kLogHeading);
    toast(summary, m_batch.failed > 0);
    // Tell the user, when they are elsewhere.
    if (m_window && !glfwGetWindowAttrib(m_window, GLFW_FOCUSED)) {
        glfwRequestWindowAttention(m_window);
    }
}

void App::stopBatch() {
    if (m_runner.busy()) {
        m_runner.stop();  // finishClip() marks the clip and ends the batch
    } else {
        m_batch.active = false;
    }
}

// ===========================================================================
//  Log and toast
// ===========================================================================

void App::appendLog(std::string text, int kind) {
    try {
        if (m_log.size() >= kMaxLogLines) {
            m_log.pop_front();
        }
        m_log.push_back(LogLine{std::move(text), kind});
    } catch (...) {
        // A log line is never worth an exception.
    }
}

void App::toast(std::string text, bool error) {
    m_toast.text = std::move(text);
    m_toast.error = error;
    m_toast.shownAt = ImGui::GetTime();
}

// ===========================================================================
//  Command preview
// ===========================================================================

fs::path App::previewClip() const {
    if (const ClipItem* q = m_queue.firstQueued()) {
        return q->path;
    }
    if (!m_queue.empty()) {
        return m_queue.items().front().path;
    }
    return {};
}

std::string App::previewCommand() const {
    try {
        const fs::path clip = previewClip();
        const std::string exe = m_tools.osvtool.empty() ? std::string("osvtool") : pathToUtf8(m_tools.osvtool);
        std::vector<std::string> argv{exe};
        // No clip yet: a stand-in name, so the options can still be read.
        const std::vector<std::string> args = clip.empty()
                                                  ? buildRenderArgs(m_settings, m_tools.caps, std::string("CLIP.OSV"),
                                                                    outputFileName(m_settings, fs::path("CLIP.OSV")))
                                                  : buildRenderArgs(m_settings, m_tools.caps, clip);
        argv.insert(argv.end(), args.begin(), args.end());
        return joinCommandLine(argv, nativeShellStyle());
    } catch (...) {
        return {};
    }
}

std::string App::previewBatchCommand() const {
    try {
        const fs::path clip = previewClip();
        const std::string exe = m_tools.osvtool.empty() ? std::string("osvtool") : pathToUtf8(m_tools.osvtool);
        const fs::path folder = clip.empty() ? fs::path(".") : clip.parent_path();
        return batchCommandLine(m_settings, m_tools.caps, exe, folder, nativeShellStyle());
    } catch (...) {
        return {};
    }
}

// ===========================================================================
//  Header and banners
// ===========================================================================

void App::drawHeader(float x, float y, float width) {
    const style::Palette& p = style::palette();
    const float logo = px(36.0f);
    ImGui::SetCursorPos(ImVec2(x, y + (px(kHeaderHeight) - logo) * 0.5f));
    if (m_logoTexture != 0) {
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(static_cast<std::uintptr_t>(m_logoTexture))),
                     ImVec2(logo, logo));
    } else {
        ImGui::Dummy(ImVec2(logo, logo));
    }

    // ---- title and subtitle --------------------------------------------------------------------
    ImGui::SetCursorPos(ImVec2(x + logo + px(12.0f), y + px(3.0f)));
    ImGui::BeginGroup();
    boldText("OpenOSV Studio", style::kTitleSize, p.text);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(8.0f));
    smallText("Stitch, reframe and batch-render Osmo 360 clips.", p.text2);
    ImGui::EndGroup();

    // ---- right: guide and theme ----------------------------------------------------------------
    const float button = ImGui::GetFrameHeight();
    const float right = x + width;
    ImGui::SetCursorPos(ImVec2(right - button, y + (px(kHeaderHeight) - button) * 0.5f));
    const bool dark = darkTheme();
    if (ui::iconButton("##theme", dark ? ui::Icon::Sun : ui::Icon::Moon, dark ? "Light mode" : "Dark mode")) {
        m_settings.theme = dark ? "light" : "dark";
        style::apply(!dark, style::scale());
        platform::styleTitleBar(m_native, !dark, !dark ? 0x121214u : 0xF2F2F7u);
    }
    ImGui::SetCursorPos(ImVec2(right - 2.0f * button - px(4.0f), y + (px(kHeaderHeight) - button) * 0.5f));
    if (ui::iconButton("##docs", ui::Icon::Reveal, "Open the guide")) {
        platform::openUrl(kDocsUrl);
    }
}

float App::drawBanners(float x, float y, float width) {
    const style::Palette& p = style::palette();
    float used = 0.0f;

    // One banner: a tinted card with an icon, a sentence and its buttons.
    const auto banner = [&](const char* id, const ImVec4& tint, ui::Icon icon, const char* title, const char* body,
                            const auto& buttons) {
        ImGui::SetCursorPos(ImVec2(x, y + used));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, style::mix(p.surface, tint, p.dark ? 0.14f : 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(tint.x, tint.y, tint.z, 0.35f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14.0f), px(10.0f)));
        ImGui::BeginChild(id, ImVec2(width, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float iconSize = px(22.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(start.x + iconSize * 0.5f, start.y + iconSize * 0.5f + px(2.0f)), iconSize * 0.5f,
                            style::u32(tint), 24);
        ui::drawIcon(dl, icon, ImVec2(start.x + iconSize * 0.5f, start.y + iconSize * 0.5f + px(2.0f)), iconSize * 0.8f,
                     style::u32(p.onAccent));
        ImGui::SetCursorScreenPos(ImVec2(start.x + iconSize + px(12.0f), start.y));
        ImGui::BeginGroup();
        boldText(title, style::kBodySize, p.text);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(px(160.0f), width - px(420.0f)));
        ImGui::PushFont(nullptr, style::kSmallSize);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
        ImGui::TextUnformatted(body);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        // Buttons: on the right when there is room, else underneath.
        if (width > px(700.0f)) {
            ImGui::SameLine();
        }
        buttons();
        ImGui::EndChild();
        used += ImGui::GetItemRectSize().y + px(10.0f);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    };

    const auto reprobe = [this] {
        if (m_toolchain.probing()) {
            m_reprobe = true;
        } else {
            m_toolchain.start(m_settings);
        }
    };

    if (m_haveTools && m_tools.osvtool.empty()) {
        banner("##noOsvtool", p.danger, ui::Icon::Alert, "osvtool isn't here",
               "OpenOSV Studio renders with osvtool. Keep osvgui next to osvtool (the zip's cli folder), or put "
               "osvtool on PATH.",
               [&] {
                   const float bw = ui::buttonWidth("Check again", ui::Icon::Retry);
                   ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - bw - px(14.0f)));
                   if (ui::button("Check again", ui::Icon::Retry)) {
                       reprobe();
                   }
               });
    }
    if (ffmpegMissing() && outputIsVideo(m_settings)) {
        banner("##noFfmpeg", p.warning, ui::Icon::Alert, "FFmpeg is missing",
               "Video goes out through FFmpeg. Install it, or show OpenOSV Studio where ffmpeg is.", [&] {
                   const float w1 = ui::buttonWidth("Get FFmpeg", ui::Icon::Reveal);
                   const float w2 = ui::buttonWidth("Locate...", ui::Icon::Folder);
                   const float w3 = ui::buttonWidth("Check again", ui::Icon::Retry);
                   const float spacing = ImGui::GetStyle().ItemSpacing.x;
                   ImGui::SetCursorPosX(std::max(
                       ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - (w1 + w2 + w3 + 2.0f * spacing) - px(14.0f)));
                   if (ui::button("Get FFmpeg", ui::Icon::Reveal, ui::ButtonKind::Primary)) {
                       platform::openUrl(kFfmpegUrl);
                   }
                   ImGui::SameLine();
                   if (ui::button("Locate...", ui::Icon::Folder)) {
                       if (auto picked = platform::chooseProgram(m_native, "Where is ffmpeg?")) {
                           m_settings.ffmpegPath = pathToUtf8(*picked);
                           reprobe();
                       }
                   }
                   ImGui::SameLine();
                   if (ui::button("Check again", ui::Icon::Retry)) {
                       reprobe();
                   }
               });
    }
    return used;
}

// ===========================================================================
//  The queue
// ===========================================================================

void App::drawQueue(ImVec2 size) {
    const style::Palette& p = style::palette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16.0f), px(14.0f)));
    ImGui::BeginChild("##queueCard", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // ---- header: title, counts, actions ---------------------------------------------------------
    const float frameH = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x, start.y + (frameH - px(20.0f)) * 0.5f));
    boldText("Queue", style::kHeadingSize, p.text);
    ImGui::SameLine(0.0f, px(10.0f));
    ImGui::SetCursorPosY(start.y + (frameH - px(16.0f)) * 0.5f);
    std::string counts;
    if (m_queue.empty()) {
        counts = m_scans.empty() ? std::string() : std::string("Scanning...");
    } else {
        counts = clipCount(m_queue.size()) + "  \xC2\xB7  " + formatBytes(m_queue.totalBytes());
        if (!m_scans.empty()) {
            counts += "  \xC2\xB7  scanning...";
        }
    }
    smallText(counts, p.text2);

    // Buttons, right-aligned; icon-only when the card is narrow.
    const bool compact = avail < px(430.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x * 0.6f;
    const float w1 = compact ? frameH : ui::buttonWidth("Add files", ui::Icon::Plus);
    const float w2 = compact ? frameH : ui::buttonWidth("Add folder", ui::Icon::Folder);
    const float w3 = frameH;
    ImGui::SetCursorPos(ImVec2(start.x + avail - (w1 + w2 + w3 + 2.0f * spacing), start.y));
    const bool dialogs = platform::haveDialogs();
    if (compact ? ui::iconButton("##addFiles", ui::Icon::Plus, "Add files", dialogs)
                : ui::button("Add files", ui::Icon::Plus, ui::ButtonKind::Secondary, 0.0f, 0.0f, dialogs)) {
        addFilesDialog();
    }
    ImGui::SameLine(0.0f, spacing);
    if (compact ? ui::iconButton("##addFolder", ui::Icon::Folder, "Add folder", dialogs)
                : ui::button("Add folder", ui::Icon::Folder, ui::ButtonKind::Secondary, 0.0f, 0.0f, dialogs)) {
        addFolderDialog();
    }
    ImGui::SameLine(0.0f, spacing);
    if (ui::iconButton("##queueMenu", ui::Icon::ChevronDown, "More")) {
        ImGui::OpenPopup("##queueMenuPopup");
    }
    if (ImGui::BeginPopup("##queueMenuPopup")) {
        const std::size_t finished =
            m_queue.size() - m_queue.count(ClipStatus::Queued) - m_queue.count(ClipStatus::Rendering);
        if (ImGui::MenuItem("Clear finished", nullptr, false, finished > 0)) {
            m_queue.removeFinished();
        }
        if (ImGui::MenuItem("Render finished again", nullptr, false, finished > 0 && !m_batch.active)) {
            for (ClipItem& item : m_queue.items()) {
                if (isFinished(item.status)) {
                    item.status = ClipStatus::Queued;
                    item.message.clear();
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear queue", nullptr, false, !m_queue.empty() && !m_batch.active)) {
            m_queue.clear();
            m_selectedId = 0;
        }
        ImGui::EndPopup();
    }

    ImGui::SetCursorPosY(start.y + frameH + px(10.0f));
    ui::hairline();
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));

    // ---- the list (or the drop zone) ------------------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, px(2.0f)));
    ImGui::BeginChild("##queueList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (m_queue.empty()) {
        drawEmptyState();
    } else {
        drawQueueRows();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void App::drawEmptyState() {
    const style::Palette& p = style::palette();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- the drop zone ------------------------------------------------------------------------------
    const float inset = px(6.0f);
    const ImVec2 a(origin.x + inset, origin.y + inset);
    const ImVec2 b(origin.x + region.x - inset, origin.y + region.y - inset);
    dl->AddRectFilled(a, b, style::u32(p.field, p.dark ? 0.35f : 0.55f), px(14.0f));
    ui::dashedRect(dl, a, b, style::u32(p.text3, 0.55f), px(14.0f), px(1.5f), px(7.0f), px(5.0f));

    // ---- icon, words, buttons, centred ------------------------------------------------------------------
    const float iconR = px(30.0f);
    const float titleSize = style::kHeadingSize + 1.0f;
    const char* title = "Drop .OSV files or folders here";
    const char* body = "Folders are searched all the way down. .LRF proxies are skipped.";
    const float buttonsW = ui::buttonWidth("Add files...", ui::Icon::Plus, ui::ButtonKind::Primary) +
                           ui::buttonWidth("Add folder...", ui::Icon::Folder) + px(10.0f);
    const float blockH = iconR * 2.0f + px(16.0f) + px(24.0f) + px(22.0f) + px(18.0f) + ImGui::GetFrameHeight();
    float y = origin.y + std::max(px(12.0f), (region.y - blockH) * 0.5f);
    const float cx = origin.x + region.x * 0.5f;

    // A soft accent disc with the tray icon; it breathes a little on a
    // spring when the window gains focus, to invite the drop.
    dl->AddCircleFilled(ImVec2(cx, y + iconR), iconR, style::u32(p.accent, p.dark ? 0.16f : 0.12f), 48);
    ui::drawIcon(dl, ui::Icon::Tray, ImVec2(cx, y + iconR), iconR * 1.05f, style::u32(p.accent));
    y += iconR * 2.0f + px(16.0f);

    ImGui::PushFont(style::fonts().bold, titleSize);
    const ImVec2 titleSize2 = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(std::round(cx - titleSize2.x * 0.5f), y), style::u32(p.text), title);
    ImGui::PopFont();
    y += titleSize2.y + px(6.0f);

    ImGui::PushFont(nullptr, style::kSmallSize);
    const std::string bodyShown = ui::ellipsize(body, region.x - px(40.0f), false);
    const ImVec2 bodySize = ImGui::CalcTextSize(bodyShown.c_str());
    dl->AddText(ImVec2(std::round(cx - bodySize.x * 0.5f), y), style::u32(p.text2), bodyShown.c_str());
    ImGui::PopFont();
    y += bodySize.y + px(18.0f);

    if (platform::haveDialogs()) {
        ImGui::SetCursorScreenPos(ImVec2(std::round(cx - buttonsW * 0.5f), y));
        if (ui::button("Add files...", ui::Icon::Plus, ui::ButtonKind::Primary)) {
            addFilesDialog();
        }
        ImGui::SameLine(0.0f, px(10.0f));
        if (ui::button("Add folder...", ui::Icon::Folder)) {
            addFolderDialog();
        }
    }
    // Claim the region so the child's layout is complete.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - px(2.0f))));
}

void App::drawQueueRows() {
    const style::Palette& p = style::palette();
    std::vector<ClipItem>& items = m_queue.items();
    const float rowH = px(kRowHeight);
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    std::optional<std::pair<std::size_t, std::size_t>> move;
    std::uint64_t removeId = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(items.size()), rowH + spacing);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            if (row < 0 || static_cast<std::size_t>(row) >= items.size()) {
                continue;
            }
            ClipItem& item = items[static_cast<std::size_t>(row)];
            ImGui::PushID(static_cast<int>(item.id & 0x7FFFFFFF));
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;

            // ---- the row is one item: select, open, context menu, drag ----------------------
            ImGui::SetNextItemAllowOverlap();
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(width, rowH));
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
            const ImGuiID rowId = ImGui::GetItemID();
            const ImVec2 afterRow = ImGui::GetCursorScreenPos();
            if (clicked) {
                m_selectedId = item.id;
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                std::error_code ec;
                if (item.status == ClipStatus::Done && fs::exists(item.output, ec)) {
                    platform::openWithDefaultApp(item.output);
                } else {
                    platform::revealInFileManager(item.path);
                }
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                const std::size_t from = static_cast<std::size_t>(row);
                ImGui::SetDragDropPayload("OSV_QUEUE_ROW", &from, sizeof(from));
                ImGui::TextUnformatted(item.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("OSV_QUEUE_ROW")) {
                    if (payload->DataSize == sizeof(std::size_t)) {
                        std::size_t from = 0;
                        std::memcpy(&from, payload->Data, sizeof(from));
                        move = std::make_pair(from, static_cast<std::size_t>(row));
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##rowMenu")) {
                std::error_code ec;
                const bool haveOutput = !item.output.empty() && fs::exists(item.output, ec);
                if (ImGui::MenuItem("Show clip in folder")) {
                    platform::revealInFileManager(item.path);
                }
                if (ImGui::MenuItem("Open render", nullptr, false, haveOutput)) {
                    platform::openWithDefaultApp(item.output);
                }
                if (ImGui::MenuItem("Show render in folder", nullptr, false, haveOutput)) {
                    platform::revealInFileManager(item.output);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Render again", nullptr, false, isFinished(item.status))) {
                    item.status = ClipStatus::Queued;
                    item.message.clear();
                }
                if (ImGui::MenuItem("Move to top", nullptr, false, row > 0)) {
                    move = std::make_pair(static_cast<std::size_t>(row), std::size_t{0});
                }
                if (ImGui::MenuItem("Remove", nullptr, false, item.status != ClipStatus::Rendering)) {
                    removeId = item.id;
                }
                ImGui::EndPopup();
            }
            if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(px(520.0f));
                ImGui::TextUnformatted(pathToUtf8(item.path).c_str());
                if (!item.output.empty()) {
                    ImGui::TextColored(p.text2, "Out: %s", pathToUtf8(item.output).c_str());
                }
                if (!item.message.empty()) {
                    ImGui::TextColored(item.status == ClipStatus::Failed ? p.danger : p.text2, "%s",
                                       item.message.c_str());
                }
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            // ---- background: selection and a gliding hover -----------------------------------
            const float hover = ui::spring(rowId, hovered ? 1.0f : 0.0f, 500.0f, 1.0f);
            const bool selected = m_selectedId == item.id;
            if (selected) {
                dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + rowH), style::u32(p.selection), px(10.0f));
            } else if (hover > 0.01f) {
                dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + rowH), style::u32(p.surfaceAlt, hover), px(10.0f));
            }

            // ---- status glyph -----------------------------------------------------------------------
            const ImVec2 glyph(pos.x + px(22.0f), pos.y + rowH * 0.5f);
            const float r = px(11.0f);
            const float fraction = item.framesTotal > 0
                                       ? static_cast<float>(item.framesDone) / static_cast<float>(item.framesTotal)
                                       : 0.0f;
            switch (item.status) {
            case ClipStatus::Queued:
                dl->AddCircle(glyph, r * 0.72f, style::u32(p.text3), 32, px(1.6f));
                break;
            case ClipStatus::Rendering: {
                dl->AddCircle(glyph, r * 0.80f, style::u32(p.trackOff), 32, px(2.6f));
                if (item.framesTotal == 0) {
                    ui::spinner(dl, glyph, r * 0.80f, style::u32(p.accent), px(2.6f));
                } else {
                    // A ring that fills with the clip: progress at a glance,
                    // and nothing to animate between progress lines.
                    const float shown = ui::spring(rowId ^ 0x2545F491u, fraction, 90.0f, 1.0f);
                    const float a0 = -3.14159265f * 0.5f;
                    dl->PathClear();
                    dl->PathArcTo(glyph, r * 0.80f, a0, a0 + 2.0f * 3.14159265f * std::clamp(shown, 0.02f, 1.0f), 40);
                    dl->PathStroke(style::u32(p.accent), ImDrawFlags_None, px(2.6f));
                }
                break;
            }
            case ClipStatus::Done:
                dl->AddCircleFilled(glyph, r, style::u32(p.success), 32);
                ui::drawIcon(dl, ui::Icon::Check, glyph, r * 1.15f, style::u32(p.onAccent));
                break;
            case ClipStatus::Failed:
                dl->AddCircleFilled(glyph, r, style::u32(p.danger), 32);
                ui::drawIcon(dl, ui::Icon::Alert, glyph, r * 1.2f, style::u32(p.onAccent));
                break;
            case ClipStatus::Stopped:
                dl->AddCircleFilled(glyph, r, style::u32(p.warning), 32);
                ui::drawIcon(dl, ui::Icon::Stop, glyph, r * 0.9f, style::u32(p.onAccent));
                break;
            case ClipStatus::Skipped:
                dl->AddCircle(glyph, r * 0.9f, style::u32(p.text3), 32, px(1.6f));
                ui::drawIcon(dl, ui::Icon::Skip, glyph, r * 0.95f, style::u32(p.text3));
                break;
            }

            // ---- right side: what is happening ------------------------------------------------------
            std::string status;
            std::string detail;
            ImVec4 statusColour = p.text2;
            switch (item.status) {
            case ClipStatus::Queued:
                status = "Queued";
                break;
            case ClipStatus::Rendering:
                statusColour = p.accent;
                if (item.framesTotal > 0) {
                    const ProgressLine line{item.framesDone, item.framesTotal, item.fps, item.device};
                    const double eta = etaSeconds(line);
                    status = format("%d%%", static_cast<int>(std::lround(fraction * 100.0f)));
                    if (eta >= 0.0) {
                        status += "  \xC2\xB7  " + formatDuration(eta) + " left";
                    }
                    detail = format("%u / %u frames  \xC2\xB7  %.1f fps", item.framesDone, item.framesTotal, item.fps);
                    if (!item.device.empty()) {
                        detail += "  \xC2\xB7  " + item.device;
                    }
                } else {
                    status = "Starting...";
                    detail = formatDuration(item.elapsedSec);
                }
                break;
            case ClipStatus::Done:
                status = "Done";
                statusColour = p.success;
                detail = "in " + formatDuration(item.elapsedSec);
                break;
            case ClipStatus::Failed:
                status = "Failed";
                statusColour = p.danger;
                break;
            case ClipStatus::Stopped:
                status = "Stopped";
                statusColour = p.warning;
                break;
            case ClipStatus::Skipped:
                status = "Skipped";
                detail = item.message;
                break;
            }
            const float rightW = std::min(px(230.0f), width * 0.38f);
            const float textX = pos.x + px(46.0f);
            const float nameW = std::max(px(40.0f), width - (textX - pos.x) - rightW - px(12.0f));
            const float line1 = pos.y + px(10.0f);
            const float line2 = pos.y + px(31.0f);

            // Name (bold) and its folder / error underneath.
            ImGui::PushFont(style::fonts().bold, style::kBodySize);
            const std::string name = ui::ellipsize(item.name, nameW, false);
            dl->AddText(ImVec2(textX, line1), style::u32(p.text), name.c_str());
            ImGui::PopFont();
            ImGui::PushFont(nullptr, style::kSmallSize);
            if (item.status == ClipStatus::Failed && !item.message.empty()) {
                const std::string message = ui::ellipsize(item.message, nameW + rightW * 0.5f, false);
                dl->AddText(ImVec2(textX, line2), style::u32(p.danger), message.c_str());
            } else {
                const std::string sizeText = "  \xC2\xB7  " + formatBytes(item.sizeBytes);
                const float sizeW = ImGui::CalcTextSize(sizeText.c_str()).x;
                const std::string folder = ui::ellipsize(item.folder, std::max(px(20.0f), nameW - sizeW), true);
                dl->AddText(ImVec2(textX, line2), style::u32(p.text2), (folder + sizeText).c_str());
            }

            ImGui::PopFont();

            // Status on the right (the remove button makes room on hover).
            const bool showRemove = hovered && item.status != ClipStatus::Rendering;
            const float rightEdge =
                pos.x + width - px(12.0f) - (showRemove ? ImGui::GetFrameHeight() + px(4.0f) : 0.0f);
            ImGui::PushFont(style::fonts().bold, style::kSmallSize);
            const std::string statusShown = ui::ellipsize(status, rightW, false);
            const float statusW = ImGui::CalcTextSize(statusShown.c_str()).x;
            dl->AddText(ImVec2(rightEdge - statusW, line1 + px(1.0f)), style::u32(statusColour), statusShown.c_str());
            ImGui::PopFont();
            ImGui::PushFont(nullptr, style::kSmallSize);
            if (!detail.empty()) {
                const std::string detailShown = ui::ellipsize(detail, rightW, false);
                const float detailW = ImGui::CalcTextSize(detailShown.c_str()).x;
                dl->AddText(ImVec2(rightEdge - detailW, line2), style::u32(p.text3), detailShown.c_str());
            }
            ImGui::PopFont();

            // A thin bar along the bottom of a rendering row.
            if (item.status == ClipStatus::Rendering) {
                ui::drawProgress(dl, ImVec2(textX, pos.y + rowH - px(8.0f)),
                                 ImVec2(pos.x + width - px(12.0f) - textX, px(3.0f)), rowId ^ 0x68E31DA4u, fraction,
                                 style::u32(p.accent), item.framesTotal == 0);
            }

            // ---- remove, on hover --------------------------------------------------------------------------
            if (showRemove) {
                const float b = ImGui::GetFrameHeight();
                ImGui::SetCursorScreenPos(ImVec2(pos.x + width - px(8.0f) - b, pos.y + (rowH - b) * 0.5f));
                if (ui::iconButton("##remove", ui::Icon::Close, "Remove from the queue")) {
                    removeId = item.id;
                }
                ImGui::SetCursorScreenPos(afterRow);
            }
            ImGui::PopID();
        }
    }
    clipper.End();

    // ---- changes after the loop, so the list never shifts under it -----------------------------------
    if (move && move->first != move->second) {
        m_queue.move(move->first, move->second);
    }
    if (removeId != 0) {
        m_queue.remove(removeId);
        if (m_selectedId == removeId) {
            m_selectedId = 0;
        }
    }
}

// ===========================================================================
//  Settings
// ===========================================================================

void App::drawSettings(float width) {
    const style::Palette& p = style::palette();
    ImGui::PushItemWidth(-1.0f);
    ImGui::BeginGroup();

    if (m_batch.active) {
        ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
        ImGui::PushFont(nullptr, style::kSmallSize);
        ImGui::TextWrapped("Settings are locked while the batch renders. Pause to change them for the next clips.");
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }

    drawOutputCard(width);
    drawColourCard(width);
    drawMotionCard(width);
    drawEncodingCard(width);
    drawDestinationCard(width);
    drawAdvancedCard(width);
    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
    ImGui::EndGroup();
    ImGui::PopItemWidth();
}

void App::drawOutputCard(float) {
    GuiSettings& s = m_settings;
    const bool editable = !m_batch.active;
    ui::sectionHeader("Output");
    ui::beginCard("##outputCard");
    ui::segmentedChoice("##mode", kModes, s.mode, ImGui::GetContentRegionAvail().x, editable);

    if (s.mode == "equirect") {
        const float w = ui::rowLabel("Size", editable);
        ui::comboChoice("##equirectSize", kEquirectSizes, s.equirectSize, w, editable);
        ui::caption(s.equirectSize == "native" ? "Native keeps the camera's own resolution."
                                               : "The whole sphere, 2:1. Every 360 player and editor takes it.");
    } else {
        float w = ui::rowLabel("Size", editable);
        ui::comboChoice("##reframeSize", kReframeSizes, s.reframeSize, w, editable);
        w = ui::rowLabel("View", editable);
        ui::comboChoice("##view", kViews, s.view, w, editable);

        // Angles, whole degrees (what the eye can tell apart in a view).
        const auto angle = [&](const char* label, const char* id, double& target, float lo, float hi, float reset) {
            const float width = ui::rowLabel(label, editable);
            float value = static_cast<float>(target);
            if (ui::slider(id, &value, lo, hi, reset, width, &degrees, editable)) {
                target = std::round(value);
            }
        };
        if (s.view == "custom") {
            angle("Field of view", "##fov", s.fov, 20.0f, 170.0f, 90.0f);
        }
        angle("Pan", "##yaw", s.yaw, -180.0f, 180.0f, 0.0f);
        angle("Tilt", "##pitch", s.pitch, -90.0f, 90.0f, 0.0f);
        angle("Roll", "##roll", s.roll, -180.0f, 180.0f, 0.0f);
        if (s.yaw != 0.0 || s.pitch != 0.0 || s.roll != 0.0) {
            if (ui::button("Straighten up", ui::Icon::Retry, ui::ButtonKind::Plain, 0.0f, 0.0f, editable)) {
                s.yaw = 0.0;
                s.pitch = 0.0;
                s.roll = 0.0;
            }
        }
        ui::caption("One fixed view for the whole clip. Double-click a slider to reset it.");
    }
    ui::endCard();
}

void App::drawColourCard(float) {
    GuiSettings& s = m_settings;
    const bool editable = !m_batch.active && !s.useUserDefaults;
    ui::sectionHeader("Colour");
    ui::beginCard("##colourCard");
    if (s.useUserDefaults) {
        ui::caption("From your Premiere defaults.");
    }
    ui::segmentedChoice("##color", kColors, s.color, ImGui::GetContentRegionAvail().x, editable);
    const char* note = "D-Log M clips come out HDR10. SDR clips stay SDR.";
    if (s.color == "pq") {
        note = "Rec.2100 PQ: what YouTube HDR and HDR TVs expect.";
    } else if (s.color == "hlg") {
        note = "Rec.2100 HLG: HDR that still looks right on an SDR screen.";
    } else if (s.color == "709") {
        note = "Rec.709: plays right everywhere.";
    }
    ui::caption(note);
    if (s.color != "709") {
        const float w = ui::rowLabel("HDR style", editable);
        ui::comboChoice("##tone", kTones, s.tone, w, editable);
    }
    if (s.color == "709" || s.color == "auto") {
        const float w = ui::rowLabel("SDR look", editable);
        ui::comboChoice("##look", kLooks, s.look, w, editable);
    }
    ui::endCard();
}

void App::drawMotionCard(float) {
    GuiSettings& s = m_settings;
    const bool editable = !m_batch.active;
    const bool own = editable && !s.useUserDefaults;
    ui::sectionHeader("Motion and stitch");
    ui::beginCard("##motionCard");
    const float w = ui::rowLabel("Stabilisation", own);
    ui::comboChoice("##stab", kStabs, s.stab, w, own);
    ui::toggleRow("Remove sun ghosts", &s.flare, own);
    ui::hairline();
    ui::toggleRow("Use my Premiere defaults", &s.useUserDefaults, editable);
    ui::caption(s.useUserDefaults
                    ? "Colour, stabilisation and sun ghosts follow the Source Settings you saved as Premiere's "
                      "default. So does Native size."
                    : "Off: every render uses exactly what is set here.");
    ui::endCard();
}

void App::drawEncodingCard(float) {
    const style::Palette& p = style::palette();
    GuiSettings& s = m_settings;
    const bool editable = !m_batch.active;
    const bool video = outputIsVideo(s);
    ui::sectionHeader("Encoding");
    ui::beginCard("##encodingCard");
    if (!video) {
        ui::caption("The file name has no video extension, so frames are written as stills.");
    }
    const bool on = editable && video;

    // ---- encoder: auto (what the probe found) or a fixed one -------------------------------------
    float w = ui::rowLabel("Encoder", on);
    const auto labelFor = [&](const Choice& c) -> std::string {
        if (std::string_view(c.token) == "auto") {
            if (!m_haveTools || m_toolchain.probing()) {
                return "Auto  \xC2\xB7  testing...";
            }
            if (m_tools.caps.autoCodec.empty()) {
                return "Auto  \xC2\xB7  osvtool's choice";
            }
            return std::string("Auto  \xC2\xB7  ") + choiceLabel(kCodecs, m_tools.caps.autoCodec);
        }
        return c.label;
    };
    const Choice* current = &kCodecs[0];
    for (const Choice& c : kCodecs) {
        if (s.codec == c.token) {
            current = &c;
        }
    }
    ImGui::BeginDisabled(!on);
    if (ui::beginCombo("##codec", labelFor(*current).c_str(), w, on)) {
        for (const Choice& c : kCodecs) {
            const bool selected = s.codec == c.token;
            const bool listed = std::string_view(c.token) == "auto" || !m_haveTools ||
                                std::find(m_tools.hevcEncoders.begin(), m_tools.hevcEncoders.end(), c.token) !=
                                    m_tools.hevcEncoders.end();
            std::string label = labelFor(c);
            if (!listed) {
                label += "  (not in this FFmpeg)";
            }
            ImGui::PushStyleColor(ImGuiCol_Text, listed ? p.text : p.text3);
            if (ImGui::Selectable(label.c_str(), selected)) {
                s.codec = c.token;
            }
            ImGui::PopStyleColor();
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    // ---- quality ----------------------------------------------------------------------------------
    w = ui::rowLabel("Quality", on);
    float crf = static_cast<float>(s.crf);
    if (ui::slider("##crf", &crf, 10.0f, 35.0f, 18.0f, w, &qualityLabel, on)) {
        s.crf = static_cast<int>(std::lround(crf));
    }
    ui::caption("Lower is better and bigger. 18 looks like the source.");

    ui::toggleRow("Keep audio", &s.audio, on);
    // The 360 tag: only for an equirect video, only when osvtool has it.
    if (s.mode == "equirect" && m_tools.caps.sphericalMetadata) {
        ui::toggleRow("Tag as 360 video (YouTube, VR players)", &s.sphericalMetadata, on);
    }
    ui::endCard();
}

void App::drawDestinationCard(float) {
    const style::Palette& p = style::palette();
    GuiSettings& s = m_settings;
    const bool editable = !m_batch.active;
    ui::sectionHeader("Destination");
    ui::beginCard("##destinationCard");

    // ---- where: next to each clip, or one folder ----------------------------------------------------
    static const char* const kWhere[] = {"Next to each clip", "One folder"};
    int where = s.outputFolder.empty() ? 0 : 1;
    if (ui::segmented("##where", kWhere, 2, &where, ImGui::GetContentRegionAvail().x, editable)) {
        if (where == 1) {
            // Choosing "One folder" asks which, at once; cancelling keeps
            // the renders next to their clips.
            const std::vector<fs::path> picked = platform::chooseFolders(m_native, "Save renders to", false);
            if (!picked.empty()) {
                s.outputFolder = pathToUtf8(picked.front());
            }
        } else {
            s.outputFolder.clear();
        }
    }
    if (!s.outputFolder.empty()) {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float changeW = ui::buttonWidth("Change...", ui::Icon::Folder);
        const float pathW = std::max(px(40.0f), avail - changeW - ImGui::GetStyle().ItemSpacing.x);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, p.text);
        ImGui::TextUnformatted(ui::ellipsize(s.outputFolder, pathW, true).c_str());
        ImGui::PopStyleColor();
        ui::tooltip(s.outputFolder.c_str());
        ImGui::SameLine(avail - changeW + ImGui::GetStyle().WindowPadding.x);
        if (ui::button("Change...", ui::Icon::Folder, ui::ButtonKind::Secondary, 0.0f, 0.0f, editable)) {
            const std::vector<fs::path> picked = platform::chooseFolders(m_native, "Save renders to", false);
            if (!picked.empty()) {
                s.outputFolder = pathToUtf8(picked.front());
            }
        }
    }

    // ---- the file name ------------------------------------------------------------------------------
    std::string& pattern = s.mode == "reframe" ? s.reframePattern : s.equirectPattern;
    const char* hint = s.mode == "reframe" ? "{name}_reframe.mp4" : "{name}_360.mp4";
    const float w = ui::rowLabel("File name", editable);
    ImGui::SetNextItemWidth(w);
    ImGui::BeginDisabled(!editable);
    ImGui::InputTextWithHint("##pattern", hint, &pattern);
    ImGui::EndDisabled();
    const fs::path example = previewClip();
    const std::string exampleName = outputFileName(s, example.empty() ? fs::path("CAM_0001.OSV") : example);
    ui::caption(("{name} {mode} {preset} {size}  \xE2\x86\x92  " + exampleName).c_str());
    ui::toggleRow("Skip clips already rendered", &s.skipExisting, editable);
    ui::endCard();
}

void App::drawAdvancedCard(float) {
    const style::Palette& p = style::palette();
    GuiSettings& s = m_settings;
    const bool editable = !m_batch.active;

    // ---- a disclosure header ---------------------------------------------------------------------
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##advancedHeader", ImVec2(width, ImGui::GetFrameHeight()))) {
        s.advancedOpen = !s.advancedOpen;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight();
    ui::drawIcon(dl, s.advancedOpen ? ui::Icon::ChevronDown : ui::Icon::ChevronRight,
                 ImVec2(pos.x + px(10.0f), pos.y + h * 0.5f), px(12.0f), style::u32(hovered ? p.text : p.text2));
    ImGui::PushFont(style::fonts().bold, style::kSmallSize);
    const ImVec2 textSize = ImGui::CalcTextSize("Advanced");
    dl->AddText(ImVec2(pos.x + px(22.0f), pos.y + (h - textSize.y) * 0.5f), style::u32(hovered ? p.text : p.text2),
                "Advanced");
    ImGui::PopFont();
    if (!s.advancedOpen) {
        return;
    }

    ui::beginCard("##advancedCard");
    // ---- FFmpeg ----------------------------------------------------------------------------------------
    std::string where;
    if (!m_haveTools) {
        where = "Looking...";
    } else if (m_tools.ffmpeg.empty()) {
        where = "Not found";
    } else {
        where = pathToUtf8(m_tools.ffmpeg);
    }
    const float w = ui::rowLabel("FFmpeg", editable);
    ImGui::PushStyleColor(ImGuiCol_Text, m_tools.ffmpeg.empty() && m_haveTools ? p.warning : p.text2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ui::ellipsize(where, w, true).c_str());
    ImGui::PopStyleColor();
    ui::tooltip(where.c_str());
    if (ui::button("Locate...", ui::Icon::Folder, ui::ButtonKind::Secondary, 0.0f, 0.0f, editable)) {
        if (auto picked = platform::chooseProgram(m_native, "Where is ffmpeg?")) {
            s.ffmpegPath = pathToUtf8(*picked);
            if (m_toolchain.probing()) {
                m_reprobe = true;
            } else {
                m_toolchain.start(s);
            }
        }
    }
    if (!s.ffmpegPath.empty()) {
        ImGui::SameLine();
        if (ui::button("Use PATH", ui::Icon::Retry, ui::ButtonKind::Plain, 0.0f, 0.0f, editable)) {
            s.ffmpegPath.clear();
            if (m_toolchain.probing()) {
                m_reprobe = true;
            } else {
                m_toolchain.start(s);
            }
        }
    }
    ui::hairline();

    // ---- extra arguments -----------------------------------------------------------------------------------
    const float ew = ui::rowLabel("Extra arguments", editable);
    ImGui::SetNextItemWidth(ew);
    ImGui::BeginDisabled(!editable);
    ImGui::PushFont(style::fonts().mono, style::kMonoSize);
    ImGui::InputTextWithHint("##extra", "--range 0-299", &s.extraArgs);
    ImGui::PopFont();
    ImGui::EndDisabled();
    ui::caption("Added to every command. An option here replaces the same one above.");
    ui::hairline();

    // ---- where the settings live ------------------------------------------------------------------------------
    ui::caption(("Settings are saved to " + pathToUtf8(m_settingsPath)).c_str());
    if (!m_settingsPath.empty()) {
        if (ui::button("Show settings file", ui::Icon::Reveal, ui::ButtonKind::Plain)) {
            platform::revealInFileManager(m_settingsPath);
        }
    }
    ui::endCard();
}

// ===========================================================================
//  Command preview, log, footer
// ===========================================================================

void App::drawCommand(float width, float height) {
    const style::Palette& p = style::palette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16.0f), px(10.0f)));
    ImGui::BeginChild("##commandCard", ImVec2(width, height),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // ---- header: title, form, copy -------------------------------------------------------------------
    const float frameH = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x, start.y + (frameH - px(18.0f)) * 0.5f));
    boldText("Command", style::kBodySize, p.text);
    ImGui::SameLine(0.0f, px(10.0f));
    ImGui::SetCursorPosY(start.y + (frameH - px(16.0f)) * 0.5f);
    const bool batch = m_settings.batchPreview;
    std::string note;
    if (batch) {
        note = nativeShellStyle() == ShellStyle::Windows ? "every .OSV in the next clip's folder (cmd; %%f in a .cmd)"
                                                         : "every .OSV in the next clip's folder (sh)";
    } else {
        note = previewClip().empty() ? "add a clip to see its exact command" : "exactly what runs for the next clip";
    }
    const float segW = std::min(px(220.0f), avail * 0.4f);
    const float copyW = ui::buttonWidth("Copy", ui::Icon::Copy);
    const float noteW = std::max(0.0f, avail - segW - copyW - px(130.0f));
    smallText(ui::ellipsize(note, noteW, false), p.text3);

    ImGui::SetCursorPos(ImVec2(start.x + avail - segW - copyW - ImGui::GetStyle().ItemSpacing.x, start.y));
    static const char* const kForms[] = {"This clip", "Whole folder"};
    int form = batch ? 1 : 0;
    if (ui::segmented("##form", kForms, 2, &form, segW)) {
        m_settings.batchPreview = form == 1;
    }
    ImGui::SameLine();
    m_previewText = m_settings.batchPreview ? previewBatchCommand() : previewCommand();
    if (ui::button("Copy", ui::Icon::Copy)) {
        ImGui::SetClipboardText(m_previewText.c_str());
        toast("Command copied");
    }

    // ---- the command itself: selectable, read-only, wrapped -----------------------------------------------
    ImGui::SetCursorPosY(start.y + frameH + px(8.0f));
    ImGui::PushFont(style::fonts().mono, style::kMonoSize);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.field);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
    ImGui::InputTextMultiline("##commandText", &m_previewText,
                              ImVec2(-1.0f, std::max(px(24.0f), ImGui::GetContentRegionAvail().y)),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
    ImGui::PopStyleColor(2);
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void App::drawLog(float width, float height) {
    const style::Palette& p = style::palette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16.0f), px(10.0f)));
    ImGui::BeginChild("##logCard", ImVec2(width, height),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float frameH = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x, start.y + (frameH - px(18.0f)) * 0.5f));
    boldText("Log", style::kBodySize, p.text);
    const float copyW = ui::buttonWidth("Copy", ui::Icon::Copy, ui::ButtonKind::Plain);
    const float clearW = ui::buttonWidth("Clear", ui::Icon::Trash, ui::ButtonKind::Plain);
    ImGui::SetCursorPos(ImVec2(start.x + avail - copyW - clearW - ImGui::GetStyle().ItemSpacing.x, start.y));
    if (ui::button("Copy", ui::Icon::Copy, ui::ButtonKind::Plain)) {
        std::string all;
        for (const LogLine& line : m_log) {
            all += line.text;
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
        toast("Log copied");
    }
    ImGui::SameLine();
    if (ui::button("Clear", ui::Icon::Trash, ui::ButtonKind::Plain)) {
        m_log.clear();
    }

    // ---- the lines: virtualised, following the tail unless scrolled up ------------------------------------
    ImGui::SetCursorPosY(start.y + frameH + px(6.0f));
    ImGui::PushFont(style::fonts().mono, style::kMonoSize);
    ImGui::BeginChild("##logLines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_log.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogLine& line = m_log[static_cast<std::size_t>(i)];
            ImVec4 colour = p.text2;
            switch (line.kind) {
            case kLogProgress:
                colour = p.text3;
                break;
            case kLogError:
                colour = p.danger;
                break;
            case kLogHeading:
                colour = p.text;
                break;
            case kLogNote:
                colour = p.accent;
                break;
            default:
                break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    clipper.End();
    if (atBottom && m_logFollow) {
        ImGui::SetScrollHereY(1.0f);
    }
    m_logFollow = atBottom || ImGui::GetScrollMaxY() <= 0.0f;
    ImGui::EndChild();
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void App::drawFooter(float x, float y, float width, float height) {
    const style::Palette& p = style::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(origin.x + x, origin.y + y), ImVec2(origin.x + x + width, origin.y + y), style::u32(p.border),
                1.0f);

    const float margin = width < px(760.0f) ? px(14.0f) : px(22.0f);
    const float frameH = ImGui::GetFrameHeight();
    const float buttonH = frameH + px(4.0f);
    const float midY = y + height * 0.5f;

    // ---- what is going on -----------------------------------------------------------------------------------
    std::string title = "Ready";
    std::string detail;
    const std::size_t queued = m_queue.count(ClipStatus::Queued);
    double overall = -1.0;
    if (m_batch.active) {
        const std::size_t current = m_batch.currentId != 0 ? 1 : 0;
        const std::size_t total = m_batch.finished + queued + current;
        title = "Rendering " + std::to_string(std::min(total, m_batch.finished + 1)) + " of " + std::to_string(total);
        if (m_batch.pauseAfterCurrent) {
            title += "  \xC2\xB7  pausing after this clip";
        }
        // ETA: the clip's own, plus the rest at the rate seen so far
        // (render seconds per source byte: bigger clips take longer).
        double currentFraction = 0.0;
        double clipLeft = -1.0;
        double rate = -1.0;
        std::uintmax_t queuedBytes = 0;
        for (const ClipItem& item : m_queue.items()) {
            if (item.status == ClipStatus::Queued) {
                queuedBytes += item.sizeBytes;
            }
        }
        if (const ClipItem* item = m_queue.find(m_batch.currentId)) {
            if (item->framesTotal > 0) {
                currentFraction = static_cast<double>(item->framesDone) / static_cast<double>(item->framesTotal);
                clipLeft = etaSeconds(ProgressLine{item->framesDone, item->framesTotal, item->fps, item->device});
                if (currentFraction > 0.05 && item->sizeBytes > 0) {
                    rate = (item->elapsedSec / currentFraction) / static_cast<double>(item->sizeBytes);
                }
            }
            if (!item->device.empty() && item->fps > 0.0) {
                detail = format("%.1f fps  \xC2\xB7  ", item->fps) + item->device;
            }
        }
        if (m_batch.doneBytes > 0) {
            rate = m_batch.doneSeconds / static_cast<double>(m_batch.doneBytes);
        }
        if (clipLeft >= 0.0 && (queuedBytes == 0 || rate > 0.0)) {
            const double left = clipLeft + (rate > 0.0 ? rate * static_cast<double>(queuedBytes) : 0.0);
            detail = "about " + formatDuration(left) + " left" + (detail.empty() ? "" : "  \xC2\xB7  " + detail);
        } else if (detail.empty()) {
            detail = "working out the time left...";
        }
        overall =
            total > 0 ? (static_cast<double>(m_batch.finished) + currentFraction) / static_cast<double>(total) : 0.0;
    } else if (m_batch.paused && queued > 0) {
        title = "Paused";
        detail = clipCount(queued) + " left";
    } else if (!m_haveTools || m_toolchain.probing()) {
        detail = "Checking osvtool and FFmpeg...";
    } else if (queued > 0) {
        detail = clipCount(queued) + " to render";
    } else if (!m_queue.empty()) {
        title = m_batch.finished > 0 ? "Done" : "Ready";
        detail = "Nothing left to render";
    } else {
        detail = "Add clips to begin.";
    }

    ImGui::SetCursorPos(ImVec2(x + margin, midY - px(19.0f)));
    boldText(title.c_str(), style::kBodySize, p.text);
    ImGui::SetCursorPos(ImVec2(x + margin, midY + px(1.0f)));
    smallText(detail, p.text2);

    // ---- buttons, right to left --------------------------------------------------------------------------------
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float right = x + width - margin;
    const auto place = [&](float w) {
        right -= w;
        ImGui::SetCursorPos(ImVec2(right, midY - buttonH * 0.5f));
        right -= spacing;
    };

    if (!m_batch.active) {
        std::string why;
        const bool ready = canStart(&why);
        const char* label = m_batch.paused && queued > 0 ? "Resume" : "Start";
        const float w = std::max(px(110.0f), ui::buttonWidth(label, ui::Icon::Play, ui::ButtonKind::Primary));
        place(w);
        if (ui::button(label, ui::Icon::Play, ui::ButtonKind::Primary, w, buttonH, ready)) {
            startBatch();
        }
        if (!ready) {
            ui::tooltip(why.c_str());
        }
    } else {
        const char* pauseLabel = m_batch.pauseAfterCurrent ? "Keep going" : "Pause after this clip";
        const ui::Icon pauseIcon = m_batch.pauseAfterCurrent ? ui::Icon::Play : ui::Icon::Pause;
        const float pw = ui::buttonWidth(pauseLabel, pauseIcon);
        place(pw);
        if (ui::button(pauseLabel, pauseIcon, ui::ButtonKind::Secondary, pw, buttonH)) {
            m_batch.pauseAfterCurrent = !m_batch.pauseAfterCurrent;
        }
        const float sw = ui::buttonWidth("Stop", ui::Icon::Stop, ui::ButtonKind::Destructive);
        place(sw);
        if (ui::button("Stop", ui::Icon::Stop, ui::ButtonKind::Destructive, sw, buttonH)) {
            stopBatch();
        }
        ui::tooltip("Stop now. The clip in progress is left unfinished.");
    }
    const float lw = ui::buttonWidth("Log", ui::Icon::Terminal, ui::ButtonKind::Plain);
    place(lw);
    if (ui::button("Log", ui::Icon::Terminal, ui::ButtonKind::Plain, lw, buttonH)) {
        m_settings.showLog = !m_settings.showLog;
    }

    // ---- the overall bar, between the words and the buttons ----------------------------------------------------
    if (overall >= 0.0) {
        const float barX = x + margin + px(260.0f);
        const float barW = right - barX - px(8.0f);
        if (barW > px(60.0f)) {
            ImGui::SetCursorPos(ImVec2(barX, midY - px(3.0f)));
            ui::progressBar("##overall", static_cast<float>(overall), barW, px(6.0f), style::u32(p.accent),
                            m_batch.currentId != 0 && overall <= 0.0);
        }
    }
}

void App::drawToast() {
    const style::Palette& p = style::palette();
    const double age = ImGui::GetTime() - m_toast.shownAt;
    const float alpha = ui::spring(ImGui::GetID("##toastAlpha"), age < 3.2 ? 1.0f : 0.0f, 220.0f, 1.0f);
    if (alpha < 0.01f || m_toast.text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGui::PushFont(nullptr, style::kSmallSize + 0.5f);
    const ImVec2 size = ImGui::CalcTextSize(m_toast.text.c_str());
    const float padX = px(16.0f);
    const float padY = px(9.0f);
    const float lift = (1.0f - alpha) * px(10.0f);
    const ImVec2 centre(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y - px(kFooterHeight) - px(28.0f) + lift);
    const ImVec2 a(centre.x - size.x * 0.5f - padX, centre.y - size.y * 0.5f - padY);
    const ImVec2 b(centre.x + size.x * 0.5f + padX, centre.y + size.y * 0.5f + padY);
    const ImVec4 fill = p.dark ? style::mix(p.popup, p.text, 0.08f) : ImVec4(0.11f, 0.11f, 0.12f, 1.0f);
    dl->AddRectFilled(ImVec2(a.x, a.y + px(2.0f)), ImVec2(b.x, b.y + px(2.0f)), style::u32(p.shadow, 0.5f * alpha),
                      (b.y - a.y) * 0.5f);
    dl->AddRectFilled(a, b, style::u32(fill, 0.97f * alpha), (b.y - a.y) * 0.5f);
    const ImVec4 ink =
        m_toast.error ? style::mix(p.danger, ImVec4(1, 1, 1, 1), 0.25f) : ImVec4(0.96f, 0.96f, 0.97f, 1.0f);
    dl->AddText(ImVec2(a.x + padX, a.y + padY), style::u32(ink, alpha), m_toast.text.c_str());
    ImGui::PopFont();
}

void App::drawCloseModal() {
    const style::Palette& p = style::palette();
    if (m_confirmClose) {
        ImGui::OpenPopup("##confirmClose");
        m_confirmClose = false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(22.0f), px(18.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, px(14.0f));
    if (ImGui::BeginPopupModal("##confirmClose", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoMove)) {
        boldText("Stop rendering and quit?", style::kHeadingSize, p.text);
        smallText("The clip in progress is left unfinished; the rest stay queued for next time.", p.text2);
        ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
        const float w1 = ui::buttonWidth("Keep rendering");
        const float w2 = ui::buttonWidth("Stop and quit", ui::Icon::None, ui::ButtonKind::Destructive);
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, avail - w1 - w2 - ImGui::GetStyle().ItemSpacing.x));
        if (ui::button("Keep rendering") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::button("Stop and quit", ui::Icon::None, ui::ButtonKind::Destructive)) {
            // End the render now (the destructor tidies its output) and go.
            m_batch.active = false;
            m_quit = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void App::requestClose() {
    if (m_batch.active && m_runner.busy()) {
        m_confirmClose = true;
        glfwPostEmptyEvent();
        return;
    }
    m_quit = true;
}

}  // namespace osvgui
