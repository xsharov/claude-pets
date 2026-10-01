#pragma once

#include "util.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cp {

struct TurnSummary {
    std::string category;
    std::string detail;
    std::string needsAction;
    std::string forUuid;
};

struct DesktopSession {
    std::string sessionId;
    std::string cliSessionId;
    std::wstring cwd;
    std::string title;
    std::string permissionMode;
    bool archived = false;
    int64_t createdAt = 0;
    int64_t lastActivityAt = 0;
    int64_t lastFocusedAt = 0;
    int64_t latestUserFrameAt = 0;
    std::string lastAssistantUuid;
    TurnSummary summary;
    std::wstring filePath;
    FileStamp stamp;
};

std::vector<std::wstring> DesktopSessionRoots();
std::wstring LocateTranscript(const std::string& cliSessionId, const std::wstring& cwd);

class DesktopSessionStore {
public:
    bool Refresh();
    const std::vector<DesktopSession>& Sessions() const { return sessions_; }
    const DesktopSession* FindByCliId(std::string_view cliSessionId) const;
    const DesktopSession* FindById(std::string_view sessionId) const;

private:
    struct CachedFile {
        FileStamp stamp;
        bool valid = false;
        DesktopSession session;
    };

    void CollectFiles(const std::wstring& directory, int depth, std::vector<std::wstring>& files) const;

    std::map<std::wstring, CachedFile> cache_;
    std::vector<DesktopSession> sessions_;
    bool initialized_ = false;
};

}
