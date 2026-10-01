#include "monitor.h"

#include "claude_app.h"
#include "json.h"
#include "util.h"

#include <algorithm>

namespace cp {

namespace {

constexpr int64_t kCycleMs = 500;
constexpr int64_t kRegistryIntervalMs = 2000;
constexpr int64_t kProcessIntervalMs = 3000;
constexpr int64_t kColdStatIntervalMs = 5000;
constexpr int64_t kResolveRetryMs = 15000;
constexpr int64_t kAnalyzeThrottleMs = 250;
constexpr int64_t kHotWindowMs = 2 * 60 * 1000;
constexpr int64_t kHookRelevanceMs = 10 * 60 * 1000;
constexpr int64_t kRunningStaleMs = 45 * 60 * 1000;
constexpr int64_t kRecentSessionMs = 7LL * 24 * 60 * 60 * 1000;
constexpr int64_t kForegroundDwellMs = 1500;
constexpr int64_t kSubagentCheckMs = 10000;

std::string SessionTitle(const DesktopSession& session) {
    if (!session.title.empty()) {
        return session.title;
    }
    const std::wstring leaf = FileNameOf(std::wstring_view(session.cwd));
    return leaf.empty() ? std::string("Claude") : Narrow(leaf);
}

std::string TurnKeyFor(const DesktopSession& session, const TranscriptSnapshot& transcript) {
    if (session.latestUserFrameAt > 0) {
        return "t" + std::to_string(session.latestUserFrameAt);
    }
    return transcript.turnKey;
}

int64_t NewestModifiedUnder(const std::wstring& directory, int depth) {
    int64_t newest = 0;
    for (const std::wstring& file : ListFiles(directory, L"*")) {
        FileStamp stamp;
        if (StatFile(JoinPath(directory, file), stamp)) {
            newest = std::max(newest, stamp.modifiedMs);
        }
    }
    if (depth > 0) {
        for (const std::wstring& child : ListDirectories(directory)) {
            newest = std::max(newest, NewestModifiedUnder(JoinPath(directory, child), depth - 1));
        }
    }
    return newest;
}

}

SessionMonitor::SessionMonitor() {
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

SessionMonitor::~SessionMonitor() {
    Stop();
    if (wakeEvent_ != nullptr) {
        CloseHandle(wakeEvent_);
    }
}

void SessionMonitor::Start(HWND notifyWindow, UINT notifyMessage) {
    if (thread_.joinable()) {
        return;
    }
    notifyWindow_ = notifyWindow;
    notifyMessage_ = notifyMessage;
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void SessionMonitor::Stop() {
    if (!thread_.joinable()) {
        return;
    }
    stop_ = true;
    SetEvent(wakeEvent_);
    thread_.join();
}

void SessionMonitor::SubmitHookEvent(std::string payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hookQueue_.size() < 256) {
            hookQueue_.push_back(std::move(payload));
        }
    }
    lastHookMs_ = UnixNowMs();
    SetEvent(wakeEvent_);
}

void SessionMonitor::MarkTurnRead(const std::string& sessionId, const std::string& turnKey) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        readQueue_.emplace_back(sessionId, turnKey);
    }
    SetEvent(wakeEvent_);
}

std::vector<SessionView> SessionMonitor::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

std::vector<SessionView> SessionMonitor::EvaluateOnce() {
    const int64_t now = UnixNowMs();
    DrainQueues(now);
    Cycle(now);
    return Snapshot();
}

void SessionMonitor::Run() {
    while (!stop_) {
        const int64_t now = UnixNowMs();
        DrainQueues(now);
        Cycle(now);
        WaitForSingleObject(wakeEvent_, static_cast<DWORD>(kCycleMs));
    }
}

void SessionMonitor::DrainQueues(int64_t now) {
    std::vector<std::string> hooks;
    std::vector<std::pair<std::string, std::string>> reads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hooks.swap(hookQueue_);
        reads.swap(readQueue_);
    }
    for (const std::string& payload : hooks) {
        ApplyHook(payload, now);
    }
    for (const auto& [sessionId, turnKey] : reads) {
        tracked_[sessionId].readTurnKey = turnKey;
    }
}

void SessionMonitor::ApplyHook(const std::string& payload, int64_t now) {
    Json event;
    if (!ParseJson(payload, event) || !event.isObject()) {
        return;
    }
    const std::string& cliSessionId = event["session_id"].str();
    const std::string& name = event["hook_event_name"].str();
    if (cliSessionId.empty() || name.empty()) {
        return;
    }
    HookState& hook = hooks_[cliSessionId];
    const std::string& transcriptPath = event["transcript_path"].str();
    if (!transcriptPath.empty()) {
        hook.transcriptPath = Widen(transcriptPath);
    }
    const std::string& toolName = event["tool_name"].str();
    const Json& toolInput = event["tool_input"];
    auto set = [&](TurnPhase phase, Activity activity) {
        hook.phase = phase;
        hook.activity = std::move(activity);
        hook.detail.clear();
        hook.atMs = now;
    };
    if (name == "UserPromptSubmit" || name == "PostToolUse" || name == "PostToolUseFailure" || name == "PermissionDenied" || name == "ElicitationResult") {
        set(TurnPhase::Running, {ActivityKind::Thinking, {}});
    } else if (name == "PreToolUse") {
        Activity activity = ActivityForTool(toolName, toolInput);
        const bool interactive = activity.kind == ActivityKind::Question || activity.kind == ActivityKind::PlanReview;
        set(interactive ? TurnPhase::Waiting : TurnPhase::Running, std::move(activity));
    } else if (name == "SubagentStart") {
        set(TurnPhase::Running, {ActivityKind::Agent, event["agent_type"].str()});
    } else if (name == "PermissionRequest") {
        Activity tool = ActivityForTool(toolName, toolInput);
        std::string detail = tool.detail.empty() ? toolName : tool.detail;
        set(TurnPhase::Waiting, {ActivityKind::Permission, std::move(detail)});
    } else if (name == "Notification") {
        const std::string& type = event["notification_type"].str();
        if (type == "permission_prompt" || type == "elicitation_dialog") {
            set(TurnPhase::Waiting, {ActivityKind::Permission, TruncateUtf8(event["message"].str(), 120)});
        }
    } else if (name == "Elicitation") {
        std::string detail = event["message"].str();
        if (detail.empty()) {
            detail = event["mcp_server_name"].str();
        }
        set(TurnPhase::Waiting, {ActivityKind::Elicitation, TruncateUtf8(detail, 120)});
    } else if (name == "Stop") {
        set(TurnPhase::Completed, {});
    } else if (name == "StopFailure") {
        set(TurnPhase::Failed, {});
        std::string detail = event["error"].str();
        if (detail.empty()) {
            detail = event["error_message"].str();
        }
        hook.detail = TruncateUtf8(CollapseWhitespace(detail), 160);
    } else if (name == "SessionEnd") {
        set(TurnPhase::Interrupted, {});
    } else if (name == "SessionStart") {
        set(TurnPhase::Unknown, {});
    }
    if (store_.FindByCliId(cliSessionId) == nullptr) {
        nextRegistryMs_ = 0;
    }
}

bool SessionMonitor::ResolveTranscript(Tracked& tracked, const DesktopSession& session, const HookState* hook) {
    if (hook != nullptr && !hook->transcriptPath.empty() && FileExists(hook->transcriptPath)) {
        tracked.transcriptPath = hook->transcriptPath;
        return true;
    }
    tracked.transcriptPath = LocateTranscript(session.cliSessionId, session.cwd);
    return !tracked.transcriptPath.empty();
}

void SessionMonitor::RefreshSubagentActivity(Tracked& tracked, int64_t now) {
    if (now < tracked.nextSubagentCheckMs || tracked.transcriptPath.size() <= 6) {
        return;
    }
    tracked.nextSubagentCheckMs = now + kSubagentCheckMs;
    const std::wstring sessionDirectory = tracked.transcriptPath.substr(0, tracked.transcriptPath.size() - 6);
    if (DirectoryExists(sessionDirectory)) {
        tracked.activityMs = std::max(tracked.activityMs, NewestModifiedUnder(sessionDirectory, 2));
    }
}

void SessionMonitor::UpdateTranscript(Tracked& tracked, const DesktopSession& session, const HookState* hook, int64_t now, bool hot) {
    if (tracked.transcriptPath.empty()) {
        if (now < tracked.nextResolveMs) {
            return;
        }
        if (!ResolveTranscript(tracked, session, hook)) {
            tracked.nextResolveMs = now + kResolveRetryMs;
            return;
        }
    }
    if (!hot && now < tracked.nextStatMs) {
        return;
    }
    tracked.nextStatMs = now + kColdStatIntervalMs;
    FileStamp stamp;
    if (!StatFile(tracked.transcriptPath, stamp)) {
        tracked.transcriptPath.clear();
        tracked.nextResolveMs = now + kResolveRetryMs;
        return;
    }
    tracked.activityMs = std::max(tracked.activityMs, stamp.modifiedMs);
    if (tracked.analyzed && stamp == tracked.stamp) {
        if (tracked.transcript.phase == TurnPhase::Running && now - stamp.modifiedMs > 60000) {
            RefreshSubagentActivity(tracked, now);
        }
        return;
    }
    if (now - tracked.lastAnalyzeMs < kAnalyzeThrottleMs) {
        return;
    }
    TranscriptSnapshot snapshot;
    if (AnalyzeTranscript(tracked.transcriptPath, snapshot)) {
        tracked.transcript = std::move(snapshot);
    }
    tracked.stamp = stamp;
    tracked.analyzed = true;
    tracked.lastAnalyzeMs = now;
}

SessionView SessionMonitor::BuildView(const DesktopSession& session, Tracked& tracked, const HookState* hook, int64_t now, int64_t newestFocus) {
    SessionView view;
    view.sessionId = session.sessionId;
    view.title = SessionTitle(session);
    view.cwd = session.cwd;
    const TranscriptSnapshot& transcript = tracked.transcript;
    TurnPhase phase = transcript.phase;
    Activity activity = transcript.activity;
    std::string failureDetail;
    int64_t phaseMs = transcript.lastEntryMs;
    if (hook != nullptr && hook->phase != TurnPhase::Unknown && now - hook->atMs < kHookRelevanceMs * 6) {
        const int64_t transcriptMs = transcript.lastEntryMs;
        switch (hook->phase) {
        case TurnPhase::Waiting:
            if (hook->atMs + 250 >= transcriptMs && phase != TurnPhase::Completed && phase != TurnPhase::Interrupted) {
                phase = TurnPhase::Waiting;
                activity = hook->activity;
                phaseMs = hook->atMs;
            }
            break;
        case TurnPhase::Running:
            if (hook->atMs > transcriptMs + 1500 && phase != TurnPhase::Running && phase != TurnPhase::Waiting) {
                phase = TurnPhase::Running;
                activity = hook->activity;
                phaseMs = hook->atMs;
            }
            break;
        case TurnPhase::Failed:
            if (hook->atMs + 250 >= transcriptMs) {
                phase = TurnPhase::Failed;
                failureDetail = hook->detail;
                phaseMs = hook->atMs;
            }
            break;
        case TurnPhase::Completed:
            if (hook->atMs > transcriptMs + 1500 && (phase == TurnPhase::Running || phase == TurnPhase::Waiting)) {
                phase = TurnPhase::Completed;
                phaseMs = hook->atMs;
            }
            break;
        case TurnPhase::Interrupted:
            if (hook->atMs >= transcriptMs) {
                phase = TurnPhase::Interrupted;
            }
            break;
        default:
            break;
        }
    }
    if (phase == TurnPhase::Running || phase == TurnPhase::Waiting) {
        const int64_t lastSignal = std::max({tracked.activityMs, transcript.lastEntryMs, hook != nullptr ? hook->atMs : 0});
        if (!claudeRunning_ || (phase == TurnPhase::Running && now - lastSignal > kRunningStaleMs)) {
            phase = TurnPhase::Interrupted;
        }
    }
    view.turnKey = TurnKeyFor(session, transcript);
    switch (phase) {
    case TurnPhase::Running:
        view.status = SessionStatus::Running;
        view.activity = activity;
        view.updatedAtMs = transcript.turnStartedMs > 0 ? transcript.turnStartedMs : std::max(session.latestUserFrameAt, phaseMs);
        break;
    case TurnPhase::Waiting:
        view.status = SessionStatus::Waiting;
        view.activity = activity;
        view.detail = activity.detail;
        view.updatedAtMs = phaseMs;
        break;
    case TurnPhase::Completed:
    case TurnPhase::Failed: {
        const int64_t endedMs = phase == TurnPhase::Completed && transcript.completedMs > 0 ? transcript.completedMs : phaseMs;
        const bool wasActive = tracked.seen && (tracked.lastStatus == SessionStatus::Running || tracked.lastStatus == SessionStatus::Waiting);
        if (wasActive && claudeForeground_ && session.lastFocusedAt >= newestFocus && newestFocus > 0) {
            tracked.readTurnKey = view.turnKey;
        }
        const bool read = tracked.readTurnKey == view.turnKey || session.lastFocusedAt >= endedMs - 1000 || endedMs <= unreadBaselineMs_.load();
        if (read) {
            view.status = SessionStatus::Idle;
            break;
        }
        view.updatedAtMs = endedMs;
        view.completedTurn = true;
        if (phase == TurnPhase::Failed) {
            view.status = SessionStatus::Failed;
            view.detail = !failureDetail.empty() ? failureDetail : transcript.finalText;
            break;
        }
        const TurnSummary& summary = session.summary;
        const bool summaryCurrent = !summary.forUuid.empty() && (summary.forUuid == transcript.lastAssistantUuid || (transcript.lastAssistantUuid.empty() && summary.forUuid == session.lastAssistantUuid));
        if (summaryCurrent && summary.category == "blocked") {
            view.status = SessionStatus::Waiting;
            view.detail = !summary.needsAction.empty() ? summary.needsAction : summary.detail;
        } else {
            view.status = SessionStatus::Review;
            view.detail = summaryCurrent && !summary.detail.empty() ? summary.detail : transcript.finalText;
        }
        break;
    }
    default:
        view.status = SessionStatus::Idle;
        break;
    }
    tracked.lastStatus = view.status;
    tracked.seen = true;
    return view;
}

void SessionMonitor::TrackForegroundReading(int64_t now, int64_t newestFocus) {
    const bool foreground = IsClaudeForeground();
    if (foreground && !claudeForeground_) {
        foregroundSinceMs_ = now;
    }
    claudeForeground_ = foreground;
    if (!foreground || now - foregroundSinceMs_ < kForegroundDwellMs || newestFocus == 0) {
        return;
    }
    for (const DesktopSession& session : store_.Sessions()) {
        if (session.lastFocusedAt != newestFocus) {
            continue;
        }
        auto found = tracked_.find(session.sessionId);
        if (found == tracked_.end()) {
            continue;
        }
        Tracked& tracked = found->second;
        const SessionStatus status = tracked.lastStatus;
        if (status == SessionStatus::Review || status == SessionStatus::Failed ||
            (status == SessionStatus::Waiting && tracked.transcript.phase == TurnPhase::Completed)) {
            tracked.readTurnKey = TurnKeyFor(session, tracked.transcript);
        }
    }
}

void SessionMonitor::Cycle(int64_t now) {
    if (now >= nextRegistryMs_) {
        store_.Refresh();
        nextRegistryMs_ = now + kRegistryIntervalMs;
    }
    if (now >= nextProcessCheckMs_) {
        claudeRunning_ = IsClaudeDesktopRunning();
        nextProcessCheckMs_ = now + kProcessIntervalMs;
    }
    int64_t newestFocus = 0;
    for (const DesktopSession& session : store_.Sessions()) {
        if (!session.archived) {
            newestFocus = std::max(newestFocus, session.lastFocusedAt);
        }
    }
    TrackForegroundReading(now, newestFocus);
    for (auto& [id, tracked] : tracked_) {
        tracked.alive = false;
    }
    std::vector<SessionView> views;
    for (const DesktopSession& session : store_.Sessions()) {
        if (session.archived) {
            continue;
        }
        auto hookIt = hooks_.find(session.cliSessionId);
        const HookState* hook = hookIt == hooks_.end() ? nullptr : &hookIt->second;
        const bool recentHook = hook != nullptr && now - hook->atMs < kHookRelevanceMs;
        if (now - std::max(session.lastActivityAt, session.lastFocusedAt) > kRecentSessionMs && !recentHook) {
            continue;
        }
        Tracked& tracked = tracked_[session.sessionId];
        tracked.alive = true;
        const bool hot = tracked.lastStatus == SessionStatus::Running || tracked.lastStatus == SessionStatus::Waiting ||
                         now - session.lastActivityAt < kHotWindowMs || (hook != nullptr && now - hook->atMs < kHotWindowMs);
        UpdateTranscript(tracked, session, hook, now, hot);
        if (!tracked.analyzed && hook == nullptr) {
            continue;
        }
        SessionView view = BuildView(session, tracked, hook, now, newestFocus);
        if (view.status != SessionStatus::Idle) {
            views.push_back(std::move(view));
        }
    }
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        it = it->second.alive ? std::next(it) : tracked_.erase(it);
    }
    for (auto it = hooks_.begin(); it != hooks_.end();) {
        it = now - it->second.atMs > kRecentSessionMs ? hooks_.erase(it) : std::next(it);
    }
    std::sort(views.begin(), views.end(), [](const SessionView& a, const SessionView& b) { return a.sessionId < b.sessionId; });
    Publish(std::move(views));
}

void SessionMonitor::Publish(std::vector<SessionView> views) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (views == snapshot_) {
            return;
        }
        snapshot_ = std::move(views);
    }
    if (notifyWindow_ != nullptr) {
        PostMessageW(notifyWindow_, notifyMessage_, 0, 0);
    }
}

}
