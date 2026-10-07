#pragma once

#include "desktop_sessions.h"
#include "session_model.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cp {

class SessionMonitor {
public:
    SessionMonitor();
    ~SessionMonitor();
    SessionMonitor(const SessionMonitor&) = delete;
    SessionMonitor& operator=(const SessionMonitor&) = delete;

    void Start(HWND notifyWindow, UINT notifyMessage);
    void Stop();
    void SubmitHookEvent(std::string payload);
    void MarkTurnRead(const std::string& sessionId, const std::string& turnKey);
    std::vector<SessionView> Snapshot() const;
    std::vector<SessionView> EvaluateOnce();
    void SetUnreadBaseline(int64_t baselineMs) { unreadBaselineMs_ = baselineMs; }
    int64_t LastHookEventMs() const { return lastHookMs_.load(); }

private:
    struct HookState {
        TurnPhase phase = TurnPhase::Unknown;
        Activity activity;
        std::string detail;
        int64_t atMs = 0;
        std::wstring transcriptPath;
    };

    struct Tracked {
        std::string cliSessionId;
        std::wstring transcriptPath;
        int64_t nextResolveMs = 0;
        int64_t nextStatMs = 0;
        int64_t lastAnalyzeMs = 0;
        int64_t nextSubagentCheckMs = 0;
        int64_t nextAgentCheckMs = 0;
        FileStamp stamp;
        bool analyzed = false;
        TranscriptSnapshot transcript;
        std::vector<BackgroundAgent> runningAgents;
        int64_t activityMs = 0;
        std::string readTurnKey;
        SessionStatus lastStatus = SessionStatus::Idle;
        bool seen = false;
        bool alive = false;
    };

    void Run();
    void DrainQueues(int64_t now);
    void ApplyHook(const std::string& payload, int64_t now);
    void Cycle(int64_t now);
    void UpdateTranscript(Tracked& tracked, const DesktopSession& session, const HookState* hook, int64_t now, bool hot);
    bool ResolveTranscript(Tracked& tracked, const DesktopSession& session, const HookState* hook);
    void RefreshSubagentActivity(Tracked& tracked, int64_t now);
    void RefreshBackgroundAgents(Tracked& tracked, int64_t now);
    SessionView BuildView(const DesktopSession& session, Tracked& tracked, const HookState* hook, int64_t now, int64_t newestFocus);
    void TrackForegroundReading(int64_t now, int64_t newestFocus);
    void Publish(std::vector<SessionView> views);

    HWND notifyWindow_ = nullptr;
    UINT notifyMessage_ = 0;
    std::thread thread_;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> lastHookMs_{0};
    std::atomic<int64_t> unreadBaselineMs_{0};

    mutable std::mutex mutex_;
    std::vector<SessionView> snapshot_;
    std::vector<std::string> hookQueue_;
    std::vector<std::pair<std::string, std::string>> readQueue_;

    DesktopSessionStore store_;
    std::unordered_map<std::string, Tracked> tracked_;
    std::unordered_map<std::string, HookState> hooks_;
    int64_t nextRegistryMs_ = 0;
    int64_t nextProcessCheckMs_ = 0;
    int64_t claudeStartedMs_ = 0;
    bool claudeRunning_ = false;
    bool claudeForeground_ = false;
    int64_t foregroundSinceMs_ = 0;
};

}
