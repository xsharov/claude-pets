#include "desktop_sessions.h"

#include "json.h"
#include "transcript.h"

#include <shlobj.h>

#include <mutex>
#include <set>
#include <unordered_map>

namespace cp {

namespace {

constexpr int kMaxDepth = 3;

bool ParseSession(const std::string& text, DesktopSession& session) {
    Json root;
    if (!ParseJson(text, root) || !root.isObject()) {
        return false;
    }
    session.sessionId = root["sessionId"].str();
    if (session.sessionId.empty()) {
        return false;
    }
    session.cliSessionId = root["cliSessionId"].str();
    session.cwd = Widen(root["cwd"].str());
    session.title = root["title"].str();
    session.permissionMode = root["permissionMode"].str();
    session.archived = root["isArchived"].boolean();
    session.createdAt = root["createdAt"].integer();
    session.lastActivityAt = root["lastActivityAt"].integer();
    session.lastFocusedAt = root["lastFocusedAt"].integer();
    session.latestUserFrameAt = root["latestUserFrameAt"].integer();
    session.lastAssistantUuid = root["lastAssistantUuid"].str();
    const Json& summary = root["postTurnSummary"];
    if (summary.isObject()) {
        session.summary.category = summary["status_category"].str();
        session.summary.detail = summary["status_detail"].str();
        session.summary.needsAction = summary["needs_action"].str();
        session.summary.forUuid = summary["summarizes_uuid"].str();
        if (session.summary.forUuid.empty()) {
            session.summary.forUuid = root["postTurnSummaryFor"].str();
        }
    }
    return true;
}

}

std::vector<std::wstring> DesktopSessionRoots() {
    static std::mutex cacheLock;
    static std::vector<std::wstring> cached;
    static int64_t cachedAtMs = 0;
    const int64_t now = UnixNowMs();
    {
        std::lock_guard<std::mutex> lock(cacheLock);
        if (cachedAtMs != 0 && now - cachedAtMs < 60000) {
            return cached;
        }
    }
    std::vector<std::wstring> roots;
    const std::wstring direct = JoinPath(JoinPath(KnownFolderPath(FOLDERID_RoamingAppData), L"Claude"), L"claude-code-sessions");
    if (DirectoryExists(direct)) {
        roots.push_back(direct);
    }
    const std::wstring packages = JoinPath(KnownFolderPath(FOLDERID_LocalAppData), L"Packages");
    for (const std::wstring& package : ListDirectories(packages)) {
        if (!StartsWith(ToLower(package), L"claude_")) {
            continue;
        }
        const std::wstring candidate = JoinPath(JoinPath(JoinPath(packages, package), L"LocalCache\\Roaming\\Claude"), L"claude-code-sessions");
        if (DirectoryExists(candidate)) {
            roots.push_back(candidate);
        }
    }
    std::lock_guard<std::mutex> lock(cacheLock);
    cached = roots;
    cachedAtMs = now;
    return roots;
}

std::wstring LocateTranscript(const std::string& cliSessionId, const std::wstring& cwd) {
    if (cliSessionId.empty()) {
        return {};
    }
    const std::wstring projects = JoinPath(ClaudeConfigDirectory(), L"projects");
    const std::wstring fileName = Widen(cliSessionId) + L".jsonl";
    const std::wstring direct = JoinPath(JoinPath(projects, ProjectSlug(cwd)), fileName);
    if (FileExists(direct)) {
        return direct;
    }
    for (const std::wstring& project : ListDirectories(projects)) {
        const std::wstring candidate = JoinPath(JoinPath(projects, project), fileName);
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

void DesktopSessionStore::CollectFiles(const std::wstring& directory, int depth, std::vector<std::wstring>& files) const {
    for (const std::wstring& name : ListFiles(directory, L"local_*.json")) {
        files.push_back(JoinPath(directory, name));
    }
    if (depth >= kMaxDepth) {
        return;
    }
    for (const std::wstring& child : ListDirectories(directory)) {
        CollectFiles(JoinPath(directory, child), depth + 1, files);
    }
}

bool DesktopSessionStore::Refresh() {
    std::vector<std::wstring> files;
    for (const std::wstring& root : DesktopSessionRoots()) {
        CollectFiles(root, 0, files);
    }
    bool changed = false;
    std::set<std::wstring> seen;
    for (const std::wstring& path : files) {
        seen.insert(path);
        FileStamp stamp;
        if (!StatFile(path, stamp)) {
            continue;
        }
        CachedFile& cached = cache_[path];
        if (cached.valid && cached.stamp == stamp) {
            continue;
        }
        std::string text;
        DesktopSession session;
        if (!ReadWholeFile(path, text, size_t{8} << 20) || !ParseSession(text, session)) {
            continue;
        }
        session.filePath = path;
        session.stamp = stamp;
        cached.stamp = stamp;
        cached.valid = true;
        cached.session = std::move(session);
        changed = true;
    }
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (seen.count(it->first) == 0 || !it->second.valid) {
            changed = changed || it->second.valid;
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    if (!changed && initialized_) {
        return false;
    }
    initialized_ = true;
    std::unordered_map<std::string, const DesktopSession*> newest;
    for (const auto& [path, cached] : cache_) {
        const DesktopSession& session = cached.session;
        auto found = newest.find(session.sessionId);
        if (found == newest.end() || found->second->stamp.modifiedMs < session.stamp.modifiedMs) {
            newest[session.sessionId] = &session;
        }
    }
    sessions_.clear();
    sessions_.reserve(newest.size());
    for (const auto& [id, session] : newest) {
        sessions_.push_back(*session);
    }
    return true;
}

const DesktopSession* DesktopSessionStore::FindByCliId(std::string_view cliSessionId) const {
    for (const DesktopSession& session : sessions_) {
        if (session.cliSessionId == cliSessionId) {
            return &session;
        }
    }
    return nullptr;
}

const DesktopSession* DesktopSessionStore::FindById(std::string_view sessionId) const {
    for (const DesktopSession& session : sessions_) {
        if (session.sessionId == sessionId) {
            return &session;
        }
    }
    return nullptr;
}

}
