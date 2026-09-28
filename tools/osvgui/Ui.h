// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Ui.h - OpenOSV Studio's window: the queue, the settings, the command
// preview, the log and the batch that renders the queue.
//
// App owns every piece of state and is driven by main.cpp once per frame
// between ImGui::NewFrame() and ImGui::Render().  All of it lives on the UI
// thread; the only other threads are the render worker (ProcessRunner), the
// tool probe (Toolchain) and folder scans (std::async), each of which hands
// its results over through a poll, never a callback into the UI.
//
// Layout, top to bottom: a header (logo, title, theme), banners when a tool
// is missing, the main area (queue beside the settings on a wide window,
// stacked on a narrow one), the command preview, the log (slides open), and
// the footer with the overall progress and Start / Pause / Stop.
#pragma once

#include "CommandBuilder.h"
#include "Queue.h"
#include "Runner.h"
#include "Settings.h"
#include "Toolchain.h"

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <set>
#include <string>
#include <vector>

struct GLFWwindow;

namespace osvgui {

/// What main() hands the app.
struct AppOptions {
    std::vector<std::filesystem::path> initialPaths;  ///< Files / folders from the command line.
    bool autoStart = false;                           ///< Start the batch once they are queued (--start).
    std::filesystem::path settingsPath;               ///< Where the settings live (empty = not saved).
    GuiSettings settings;                             ///< As loaded.
    std::string loadError;                            ///< Why the settings file could not be read, if so.
};

/// The application.
class App {
public:
    explicit App(AppOptions options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Hand over the window once it exists (GL context current).
    void attach(GLFWwindow* window, void* nativeHandle);

    /// Draw one frame of the UI.
    void frame();

    /// The background work of a frame without drawing (the window is
    /// minimised: the batch must keep going all the same).
    void tick();

    /// Queue files / folders (a drop, the command line, a dialog).  The scan
    /// runs on a worker thread; the clips appear when it is done.
    void addPaths(std::vector<std::filesystem::path> paths);

    /// The window's close box was pressed: quit, or ask first while a batch
    /// is rendering.
    void requestClose();

    /// The app is done; main() leaves the loop.
    [[nodiscard]] bool shouldQuit() const noexcept { return m_quit; }

    /// How long the event loop may sleep before the next frame: 0 while
    /// something animates, a quarter second during a render (elapsed times
    /// and the ETA tick; progress itself wakes the loop), longer when idle.
    [[nodiscard]] double waitSeconds() const noexcept;

    /// Nothing is pending (scans, probes, first layout): a screenshot now
    /// shows the settled state.
    [[nodiscard]] bool settled() const noexcept;

    /// The theme in force, and the window colour for glClear.
    [[nodiscard]] bool darkTheme() const noexcept { return m_settings.theme != "light"; }
    [[nodiscard]] ImVec4 clearColour() const noexcept;

    /// The UI scale changed (monitor DPI).
    void setScale(float scale) noexcept;

    /// Save the settings now (on exit), window state included.
    void saveNow() noexcept;

private:
    // ---- per-frame work ------------------------------------------------------------
    void pollBackground();
    void pollRunner();
    void pollScans();
    void pollToolchain();
    void autosave(bool force);
    void updateSessionState();

    // ---- the batch ------------------------------------------------------------------
    void startBatch();
    void startNextClip();
    void finishClip();
    void finishBatch(const char* why);
    void stopBatch();
    [[nodiscard]] bool canStart(std::string* why) const;

    // ---- drawing ----------------------------------------------------------------------
    void drawHeader(float x, float y, float width);
    float drawBanners(float x, float y, float width);
    void drawQueue(ImVec2 size);
    void drawQueueRows();
    void drawEmptyState();
    void drawSettings(float width);
    void drawOutputCard(float width);
    void drawColourCard(float width);
    void drawMotionCard(float width);
    void drawEncodingCard(float width);
    void drawDestinationCard(float width);
    void drawAdvancedCard(float width);
    void drawCommand(float width, float height);
    void drawLog(float width, float height);
    void drawFooter(float x, float y, float width, float height);
    void drawToast();
    void drawCloseModal();

    // ---- helpers ------------------------------------------------------------------------
    void addFilesDialog();
    void addFolderDialog();
    void appendLog(std::string text, int kind);
    void toast(std::string text, bool error = false);
    [[nodiscard]] std::string previewCommand() const;
    [[nodiscard]] std::string previewBatchCommand() const;
    [[nodiscard]] std::filesystem::path previewClip() const;
    [[nodiscard]] bool ffmpegMissing() const noexcept;
    void createLogo();

    // ---- state -----------------------------------------------------------------------------
    GuiSettings m_settings;
    std::filesystem::path m_settingsPath;
    std::string m_savedText;
    double m_lastSaveCheck = 0.0;

    ClipQueue m_queue;
    std::uint64_t m_selectedId = 0;

    ProcessRunner m_runner;
    Toolchain m_toolchain;
    ToolchainReport m_tools;
    bool m_haveTools = false;
    bool m_reprobe = false;

    /// Folder scans in flight.
    std::vector<std::future<ScanResult>> m_scans;
    bool m_autoStart = false;

    /// The batch in progress (or the last one).
    struct Batch {
        bool active = false;             ///< Rendering, or about to start the next clip.
        bool pauseAfterCurrent = false;  ///< Stop starting new clips when this one ends.
        bool paused = false;             ///< Ended by a pause (Resume continues).
        GuiSettings settings;            ///< The settings it renders with (a snapshot).
        ToolCaps caps;                   ///< The tools' capabilities then.
        std::filesystem::path osvtool;   ///< The osvtool it runs.
        std::uint64_t currentId = 0;     ///< The clip rendering now (0 = none).
        std::chrono::steady_clock::time_point clipStart{};
        std::filesystem::path currentOutput;
        bool outputExisted = false;     ///< The output was there before this render.
        std::set<std::string> outputs;  ///< Outputs written in this batch (collisions get a suffix).
        std::size_t finished = 0;       ///< Clips this batch has finished (any outcome).
        std::size_t failed = 0;         ///< ... of which failed.
        double doneSeconds = 0.0;       ///< Render time of the finished clips.
        std::uintmax_t doneBytes = 0;   ///< Their total size (for the ETA).
    };
    Batch m_batch;

    /// One log line and how to colour it.
    struct LogLine {
        std::string text;
        int kind = 0;  ///< 0 output, 1 progress, 2 error, 3 heading, 4 note
    };
    std::deque<LogLine> m_log;
    bool m_logFollow = true;

    /// A short notice at the bottom of the window.
    struct Toast {
        std::string text;
        bool error = false;
        double shownAt = -100.0;
    };
    Toast m_toast;

    // ---- window ----------------------------------------------------------------------------------
    GLFWwindow* m_window = nullptr;
    void* m_native = nullptr;
    unsigned m_logoTexture = 0;
    bool m_quit = false;
    bool m_confirmClose = false;
    bool m_sessionAwake = false;
    double m_taskbarFraction = -2.0;
    int m_frames = 0;
    std::string m_previewText;  ///< Buffer for the read-only preview field.
};

}  // namespace osvgui
