#include "log.h"

#include "util.h"

#include <mutex>

namespace cp {

namespace {

std::wstring LogPath() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"CLAUDE_PETS_LOG", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::wstring(buffer, length);
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
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    char stamp[32] = {};
    const int length = wsprintfA(stamp, "%02u:%02u:%02u.%03u ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    std::string text(stamp, static_cast<size_t>(length));
    text.append(line);
    text.push_back('\n');
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

}
