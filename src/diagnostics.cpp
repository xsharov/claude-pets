#include "diagnostics.h"

#include "claude_app.h"
#include "desktop_sessions.h"
#include "hooks.h"
#include "i18n.h"
#include "monitor.h"
#include "notices.h"
#include "pets.h"
#include "settings.h"
#include "transcript.h"
#include "util.h"

#include <algorithm>

namespace cp {

namespace {

const char* PhaseName(TurnPhase phase) {
    switch (phase) {
    case TurnPhase::Running:
        return "running";
    case TurnPhase::Waiting:
        return "waiting";
    case TurnPhase::Completed:
        return "completed";
    case TurnPhase::Interrupted:
        return "interrupted";
    case TurnPhase::Failed:
        return "failed";
    default:
        return "unknown";
    }
}

const char* StatusName(SessionStatus status) {
    switch (status) {
    case SessionStatus::Running:
        return "running";
    case SessionStatus::Waiting:
        return "waiting";
    case SessionStatus::Failed:
        return "failed";
    case SessionStatus::Review:
        return "ready";
    default:
        return "idle";
    }
}

std::string Age(int64_t now, int64_t timestamp) {
    if (timestamp <= 0) {
        return "-";
    }
    const int64_t seconds = (now - timestamp) / 1000;
    if (seconds < 120) {
        return std::to_string(seconds) + "s";
    }
    if (seconds < 7200) {
        return std::to_string(seconds / 60) + "m";
    }
    return std::to_string(seconds / 3600) + "h";
}

}

void WriteConsoleText(const std::string& text) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            output = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

int RunDiagnostics() {
    const int64_t now = UnixNowMs();
    std::string out;
    out += "claude-pets diagnostics\n";
    out += "claude desktop running: " + std::string(IsClaudeDesktopRunning() ? "yes" : "no") + "\n";
    const HookState hooks = QueryHookState(ExecutablePath());
    out += "hooks: " + std::string(hooks == HookState::Installed ? "installed" : hooks == HookState::Outdated ? "outdated" : hooks == HookState::Unreadable ? "unreadable" : "not installed") + "\n";
    out += "pets:\n";
    for (const PetInfo& pet : DiscoverPets()) {
        out += "  " + Narrow(pet.key) + "  \"" + Narrow(pet.displayName) + "\"  v" + std::to_string(pet.spriteVersion) + "  " + Narrow(pet.spritesheet) + "\n";
    }
    const CodexPetPreference preference = ReadCodexPetPreference();
    out += "codex preference: pet=" + Narrow(preference.petKey) + " width=" + std::to_string(preference.mascotWidth) + "\n";
    out += "session roots:\n";
    for (const std::wstring& root : DesktopSessionRoots()) {
        out += "  " + Narrow(root) + "\n";
    }
    DesktopSessionStore store;
    store.Refresh();
    std::vector<DesktopSession> sessions = store.Sessions();
    std::sort(sessions.begin(), sessions.end(), [](const DesktopSession& a, const DesktopSession& b) { return a.lastActivityAt > b.lastActivityAt; });
    out += "sessions (" + std::to_string(sessions.size()) + "):\n";
    for (const DesktopSession& session : sessions) {
        if (session.archived) {
            continue;
        }
        out += "- " + session.sessionId + "  \"" + session.title + "\"  activity " + Age(now, session.lastActivityAt) + " ago, focus " + Age(now, session.lastFocusedAt) +
               " ago, mode " + session.permissionMode + "\n";
        const std::wstring path = LocateTranscript(session.cliSessionId, session.cwd);
        if (path.empty()) {
            out += "    transcript: not found\n";
            continue;
        }
        TranscriptSnapshot snapshot;
        const bool analyzed = AnalyzeTranscript(path, snapshot);
        out += "    transcript: " + std::string(analyzed ? PhaseName(snapshot.phase) : "unreadable") + ", last entry " + Age(now, snapshot.lastEntryMs) + " ago";
        if (snapshot.completedMs > 0) {
            out += ", completed " + Age(now, snapshot.completedMs) + " ago";
        }
        out += "\n    activity: " + Narrow(DescribeActivity(snapshot.activity)) + "\n";
        if (!snapshot.finalText.empty()) {
            out += "    final: " + snapshot.finalText + "\n";
        }
        if (!session.summary.category.empty()) {
            out += "    summary: [" + session.summary.category + "] " + session.summary.detail;
            out += session.summary.forUuid == snapshot.lastAssistantUuid ? " (current)\n" : " (stale)\n";
        }
    }
    const Settings settings = LoadSettings();
    SessionMonitor monitor;
    monitor.SetUnreadBaseline(settings.lastAliveMs > 0 ? settings.lastAliveMs : now - 15LL * 60 * 1000);
    const std::vector<SessionView> views = monitor.EvaluateOnce();
    const std::vector<Notice> notices = BuildNotices(views, settings, std::nullopt, now);
    out += "bubbles (" + std::to_string(notices.size()) + "):\n";
    for (const Notice& notice : notices) {
        out += "  [" + std::string(StatusName(notice.status)) + "] " + Narrow(notice.title) + " - " + Narrow(notice.subtitle) + "  (" + notice.id + ")\n";
    }
    WriteConsoleText(out);
    return 0;
}

}
