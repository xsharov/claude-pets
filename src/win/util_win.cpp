#include "util.h"

#include "win/win_util.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>

namespace cp {

namespace {

constexpr int64_t kUnixEpochIn100ns = 116444736000000000LL;

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

std::wstring GetEnv(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length <= 1) {
        return {};
    }
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (written == 0 || written >= length) {
        return {};
    }
    value.resize(written);
    return value;
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

bool AppendToFile(const std::wstring& path, std::string_view data) {
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != FALSE;
    CloseHandle(file);
    return ok;
}

bool CopyFileIfMissing(const std::wstring& source, const std::wstring& target) {
    return CopyFileW(source.c_str(), target.c_str(), TRUE) != FALSE;
}

std::string LocalTimeStamp() {
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    char stamp[32] = {};
    const int length = std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u ", static_cast<unsigned>(time.wHour), static_cast<unsigned>(time.wMinute),
                                     static_cast<unsigned>(time.wSecond), static_cast<unsigned>(time.wMilliseconds));
    return std::string(stamp, length > 0 ? static_cast<size_t>(length) : 0);
}

}
