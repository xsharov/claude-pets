#include "util.h"

#include <shlobj.h>

#include <algorithm>
#include <cwctype>

namespace cp {

namespace {

constexpr int64_t kUnixEpochIn100ns = 116444736000000000LL;

int64_t DaysFromCivil(int year, int month, int day) {
    year -= month <= 2 ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int yearOfEra = static_cast<int>(year - era * 400);
    const int dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

bool ReadDigits(std::string_view text, size_t& pos, size_t count, int& value) {
    if (pos + count > text.size()) {
        return false;
    }
    value = 0;
    for (size_t i = 0; i < count; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    pos += count;
    return true;
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view TrimView(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && IsSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}

std::wstring Widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring KnownFolderPath(const GUID& folderId) {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &raw)) && raw != nullptr) {
        result = raw;
    }
    CoTaskMemFree(raw);
    return result;
}

std::wstring HomeDirectory() {
    std::wstring home = KnownFolderPath(FOLDERID_Profile);
    if (!home.empty()) {
        return home;
    }
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring();
}

std::wstring ClaudeConfigDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::wstring(buffer, length);
    }
    return JoinPath(HomeDirectory(), L".claude");
}

std::wstring AppDataDirectory() {
    return JoinPath(KnownFolderPath(FOLDERID_RoamingAppData), L"ClaudePets");
}

std::wstring ExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring JoinPath(std::wstring_view base, std::wstring_view leaf) {
    if (base.empty()) {
        return std::wstring(leaf);
    }
    std::wstring result(base);
    if (result.back() != L'\\' && result.back() != L'/') {
        result.push_back(L'\\');
    }
    result.append(leaf);
    return result;
}

std::wstring FileNameOf(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    return std::wstring(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

std::string FileNameOf(std::string_view path) {
    const size_t slash = path.find_last_of("\\/");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectory(const std::wstring& path) {
    if (DirectoryExists(path)) {
        return true;
    }
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

std::vector<std::wstring> ListDirectories(const std::wstring& directory) {
    std::vector<std::wstring> result;
    WIN32_FIND_DATAW data = {};
    const std::wstring pattern = JoinPath(directory, L"*");
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        return result;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        result.emplace_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::wstring> ListFiles(const std::wstring& directory, std::wstring_view pattern) {
    std::vector<std::wstring> result;
    WIN32_FIND_DATAW data = {};
    const std::wstring query = JoinPath(directory, pattern);
    HANDLE find = FindFirstFileExW(query.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        return result;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        result.emplace_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(result.begin(), result.end());
    return result;
}

bool StatFile(const std::wstring& path, FileStamp& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    stamp.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    stamp.modifiedMs = FileTimeToUnixMs(data.ftLastWriteTime);
    return true;
}

bool ReadWholeFile(const std::wstring& path, std::string& out, size_t limit) {
    uint64_t size = 0;
    if (!ReadFileTail(path, limit, out, size)) {
        return false;
    }
    return size <= limit;
}

bool ReadFileTail(const std::wstring& path, size_t maxBytes, std::string& out, uint64_t& fileSize) {
    out.clear();
    fileSize = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return false;
    }
    fileSize = static_cast<uint64_t>(size.QuadPart);
    const uint64_t toRead = std::min<uint64_t>(fileSize, maxBytes);
    LARGE_INTEGER offset = {};
    offset.QuadPart = static_cast<LONGLONG>(fileSize - toRead);
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(toRead));
    size_t total = 0;
    while (total < out.size()) {
        DWORD chunk = 0;
        const DWORD request = static_cast<DWORD>(std::min<size_t>(out.size() - total, 1u << 20));
        if (!ReadFile(file, out.data() + total, request, &chunk, nullptr) || chunk == 0) {
            break;
        }
        total += chunk;
    }
    out.resize(total);
    CloseHandle(file);
    return true;
}

bool WriteFileAtomically(const std::wstring& path, std::string_view data) {
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    size_t total = 0;
    bool ok = true;
    while (total < data.size()) {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(std::min<size_t>(data.size() - total, 1u << 20));
        if (!WriteFile(file, data.data() + total, request, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        total += written;
    }
    ok = ok && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

int64_t UnixNowMs() {
    FILETIME now = {};
    GetSystemTimePreciseAsFileTime(&now);
    return FileTimeToUnixMs(now);
}

double MonotonicMs() {
    static const double frequency = [] {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / frequency;
}

int64_t FileTimeToUnixMs(const FILETIME& fileTime) {
    const int64_t ticks = (static_cast<int64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
    return (ticks - kUnixEpochIn100ns) / 10000;
}

int64_t ParseIsoTimestampMs(std::string_view text) {
    size_t pos = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ReadDigits(text, pos, 4, year) || pos >= text.size() || text[pos++] != '-') {
        return 0;
    }
    if (!ReadDigits(text, pos, 2, month) || pos >= text.size() || text[pos++] != '-') {
        return 0;
    }
    if (!ReadDigits(text, pos, 2, day) || pos >= text.size() || (text[pos] != 'T' && text[pos] != ' ')) {
        return 0;
    }
    ++pos;
    if (!ReadDigits(text, pos, 2, hour) || pos >= text.size() || text[pos++] != ':') {
        return 0;
    }
    if (!ReadDigits(text, pos, 2, minute) || pos >= text.size() || text[pos++] != ':') {
        return 0;
    }
    if (!ReadDigits(text, pos, 2, second)) {
        return 0;
    }
    int64_t millis = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits < 3) {
                millis = millis * 10 + (text[pos] - '0');
            }
            ++digits;
            ++pos;
        }
        while (digits < 3) {
            millis *= 10;
            ++digits;
        }
    }
    int64_t offsetMinutes = 0;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        const int sign = text[pos] == '-' ? -1 : 1;
        ++pos;
        int offsetHours = 0;
        int offsetMins = 0;
        if (ReadDigits(text, pos, 2, offsetHours)) {
            if (pos < text.size() && text[pos] == ':') {
                ++pos;
            }
            ReadDigits(text, pos, 2, offsetMins);
            offsetMinutes = sign * (offsetHours * 60 + offsetMins);
        }
    }
    const int64_t days = DaysFromCivil(year, month, day);
    const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second - offsetMinutes * 60;
    return seconds * 1000 + millis;
}

std::string CollapseWhitespace(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool pendingSpace = false;
    for (const char c : text) {
        if (IsSpace(c)) {
            pendingSpace = !result.empty();
            continue;
        }
        if (pendingSpace) {
            result.push_back(' ');
            pendingSpace = false;
        }
        result.push_back(c);
    }
    return result;
}

std::string StripMarkdown(std::string_view text) {
    std::string_view line = TrimView(text);
    while (!line.empty() && (line.front() == '#' || line.front() == '>')) {
        line.remove_prefix(1);
        line = TrimView(line);
    }
    if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
        line.remove_prefix(2);
    } else {
        size_t digits = 0;
        while (digits < line.size() && digits < 3 && line[digits] >= '0' && line[digits] <= '9') {
            ++digits;
        }
        if (digits > 0 && digits + 1 < line.size() && (line[digits] == '.' || line[digits] == ')') && line[digits + 1] == ' ') {
            line.remove_prefix(digits + 2);
        }
    }
    std::string result;
    result.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '`') {
            continue;
        }
        if ((c == '*' || c == '_') && i + 1 < line.size() && line[i + 1] == c) {
            ++i;
            continue;
        }
        if (c == '[') {
            const size_t close = line.find("](", i);
            const size_t end = close == std::string_view::npos ? std::string_view::npos : line.find(')', close + 2);
            if (close != std::string_view::npos && end != std::string_view::npos) {
                result.append(line.substr(i + 1, close - i - 1));
                i = end;
                continue;
            }
        }
        result.push_back(c);
    }
    return CollapseWhitespace(result);
}

std::string FirstMeaningfulLine(std::string_view text) {
    bool inFence = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view raw = TrimView(text.substr(start, end - start));
        start = end + 1;
        if (StartsWith(raw, "```")) {
            inFence = !inFence;
            continue;
        }
        if (inFence || raw.empty()) {
            continue;
        }
        if (raw.find_first_not_of("|-: ") == std::string_view::npos) {
            continue;
        }
        std::string cleaned = StripMarkdown(raw);
        if (!cleaned.empty()) {
            return cleaned;
        }
        if (end == text.size()) {
            break;
        }
    }
    return {};
}

std::string TruncateUtf8(std::string_view text, size_t maxCodePoints) {
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (count == maxCodePoints) {
                std::string result(text.substr(0, i));
                while (!result.empty() && IsSpace(result.back())) {
                    result.pop_back();
                }
                result.append("\xE2\x80\xA6");
                return result;
            }
            ++count;
        }
    }
    return std::string(text);
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool StartsWith(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::wstring ReplaceAll(std::wstring text, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) {
        return text;
    }
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

std::wstring ToLower(std::wstring_view text) {
    std::wstring result(text);
    for (wchar_t& c : result) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return result;
}

}
