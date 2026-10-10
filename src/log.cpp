#include "log.h"

#include "util.h"

#include <mutex>

namespace cp {

namespace {

std::wstring LogPath() {
    const std::wstring custom = GetEnv(L"CLAUDE_PETS_LOG");
    if (!custom.empty()) {
        return custom;
    }
    const std::wstring marker = JoinPath(AppDataDirectory(), L"debug.log");
    return FileExists(marker) ? marker : std::wstring();
}

}

bool LogEnabled() {
    static const bool enabled = !LogPath().empty();
    return enabled;
}

void LogLine(const std::string& line) {
    if (!LogEnabled()) {
        return;
    }
    static std::mutex lock;
    static const std::wstring path = LogPath();
    std::lock_guard<std::mutex> guard(lock);
    std::string text = LocalTimeStamp();
    text.append(line);
    text.push_back('\n');
    AppendToFile(path, text);
}

}
