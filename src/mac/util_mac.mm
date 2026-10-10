#include "util.h"

#include "mac/mac_util.h"

#import <AppKit/AppKit.h>

#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace cp {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

std::string Native(const std::wstring& path) {
    return Narrow(path);
}

void AppendUtf8(std::string& out, char32_t code) {
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        code = kReplacement;
    }
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::wstring ResolveLink(const std::wstring& path) {
    struct stat info = {};
    if (lstat(Native(path).c_str(), &info) != 0 || !S_ISLNK(info.st_mode)) {
        return path;
    }
    char resolved[PATH_MAX] = {};
    if (realpath(Native(path).c_str(), resolved) == nullptr) {
        return path;
    }
    return Widen(resolved);
}

bool WriteAll(int file, std::string_view data) {
    size_t total = 0;
    while (total < data.size()) {
        const ssize_t written = write(file, data.data() + total, data.size() - total);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        total += static_cast<size_t>(written);
    }
    return true;
}

}

std::wstring Widen(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        char32_t code = 0;
        if (lead < 0x80) {
            result.push_back(static_cast<wchar_t>(lead));
            ++i;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code = lead & 0x07;
        } else {
            result.push_back(static_cast<wchar_t>(kReplacement));
            ++i;
            continue;
        }
        if (i + length > text.size()) {
            result.push_back(static_cast<wchar_t>(kReplacement));
            break;
        }
        bool valid = true;
        for (size_t k = 1; k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code = (code << 6) | (next & 0x3F);
        }
        const bool overlong = (length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000);
        if (!valid || overlong || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            result.push_back(static_cast<wchar_t>(kReplacement));
            ++i;
            continue;
        }
        result.push_back(static_cast<wchar_t>(code));
        i += length;
    }
    return result;
}

std::string Narrow(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t c : text) {
        AppendUtf8(result, static_cast<char32_t>(c));
    }
    return result;
}

std::wstring GetEnv(const wchar_t* name) {
    const char* value = std::getenv(Narrow(name).c_str());
    return value == nullptr ? std::wstring() : Widen(value);
}

std::wstring HomeDirectory() {
    const std::wstring home = GetEnv(L"HOME");
    if (!home.empty()) {
        return home;
    }
    const passwd* entry = getpwuid(getuid());
    return entry != nullptr && entry->pw_dir != nullptr ? Widen(entry->pw_dir) : std::wstring();
}

std::wstring AppDataDirectory() {
    return JoinPath(JoinPath(JoinPath(HomeDirectory(), L"Library"), L"Application Support"), L"ClaudePets");
}

std::wstring ExecutablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    char resolved[PATH_MAX] = {};
    if (realpath(buffer.c_str(), resolved) == nullptr) {
        return Widen(buffer.c_str());
    }
    return Widen(resolved);
}

bool FileExists(const std::wstring& path) {
    struct stat info = {};
    return stat(Native(path).c_str(), &info) == 0 && !S_ISDIR(info.st_mode);
}

bool DirectoryExists(const std::wstring& path) {
    struct stat info = {};
    return stat(Native(path).c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    if (DirectoryExists(path)) {
        return true;
    }
    const size_t slash = path.find_last_of(L'/');
    if (slash != std::wstring::npos && slash > 0 && !EnsureDirectory(path.substr(0, slash))) {
        return false;
    }
    return mkdir(Native(path).c_str(), 0755) == 0 || errno == EEXIST;
}

std::vector<std::wstring> ListDirectories(const std::wstring& directory) {
    std::vector<std::wstring> result;
    const std::string root = Native(directory);
    DIR* handle = opendir(root.c_str());
    if (handle == nullptr) {
        return result;
    }
    while (const dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        bool isDirectory = entry->d_type == DT_DIR;
        if (entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
            struct stat info = {};
            isDirectory = stat((root + "/" + name).c_str(), &info) == 0 && S_ISDIR(info.st_mode);
        }
        if (isDirectory) {
            result.push_back(Widen(name));
        }
    }
    closedir(handle);
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::wstring> ListFiles(const std::wstring& directory, std::wstring_view pattern) {
    std::vector<std::wstring> result;
    const std::string root = Native(directory);
    const std::string glob = Narrow(pattern);
    DIR* handle = opendir(root.c_str());
    if (handle == nullptr) {
        return result;
    }
    while (const dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || fnmatch(glob.c_str(), name.c_str(), FNM_CASEFOLD) != 0) {
            continue;
        }
        bool isFile = entry->d_type == DT_REG;
        if (entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
            struct stat info = {};
            isFile = stat((root + "/" + name).c_str(), &info) == 0 && !S_ISDIR(info.st_mode);
        }
        if (isFile) {
            result.push_back(Widen(name));
        }
    }
    closedir(handle);
    std::sort(result.begin(), result.end());
    return result;
}

bool StatFile(const std::wstring& path, FileStamp& stamp) {
    struct stat info = {};
    if (stat(Native(path).c_str(), &info) != 0) {
        return false;
    }
    stamp.size = static_cast<uint64_t>(info.st_size);
    stamp.modifiedMs = static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1000 + info.st_mtimespec.tv_nsec / 1000000;
    return true;
}

bool ReadFileTail(const std::wstring& path, size_t maxBytes, std::string& out, uint64_t& fileSize) {
    out.clear();
    fileSize = 0;
    const int file = open(Native(path).c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return false;
    }
    struct stat info = {};
    if (fstat(file, &info) != 0 || S_ISDIR(info.st_mode)) {
        close(file);
        return false;
    }
    fileSize = static_cast<uint64_t>(info.st_size);
    const uint64_t toRead = std::min<uint64_t>(fileSize, maxBytes);
    out.resize(static_cast<size_t>(toRead));
    const off_t start = static_cast<off_t>(fileSize - toRead);
    size_t total = 0;
    while (total < out.size()) {
        const ssize_t chunk = pread(file, out.data() + total, std::min<size_t>(out.size() - total, size_t{1} << 20), start + static_cast<off_t>(total));
        if (chunk < 0 && errno == EINTR) {
            continue;
        }
        if (chunk <= 0) {
            break;
        }
        total += static_cast<size_t>(chunk);
    }
    out.resize(total);
    close(file);
    return true;
}

bool WriteFileAtomically(const std::wstring& path, std::string_view data) {
    const std::wstring target = ResolveLink(path);
    const std::string native = Native(target);
    const std::string temporary = native + ".tmp";
    mode_t mode = 0644;
    struct stat existing = {};
    if (stat(native.c_str(), &existing) == 0) {
        mode = existing.st_mode & 07777;
    }
    const int file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (file < 0) {
        return false;
    }
    fchmod(file, mode);
    const bool ok = WriteAll(file, data) && fsync(file) == 0;
    close(file);
    if (!ok || rename(temporary.c_str(), native.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

bool AppendToFile(const std::wstring& path, std::string_view data) {
    const int file = open(Native(path).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (file < 0) {
        return false;
    }
    const bool ok = WriteAll(file, data);
    close(file);
    return ok;
}

bool CopyFileIfMissing(const std::wstring& source, const std::wstring& target) {
    return copyfile(Native(source).c_str(), Native(target).c_str(), nullptr, COPYFILE_ALL | COPYFILE_EXCL) == 0;
}

int64_t UnixNowMs() {
    timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

double MonotonicMs() {
    return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1000000.0;
}

std::string LocalTimeStamp() {
    timeval now = {};
    gettimeofday(&now, nullptr);
    tm local = {};
    const time_t seconds = now.tv_sec;
    localtime_r(&seconds, &local);
    char stamp[32] = {};
    const int length = std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ", local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(now.tv_usec / 1000));
    return std::string(stamp, length > 0 ? static_cast<size_t>(length) : 0);
}

double PrimaryScreenHeight() {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    return screens.count > 0 ? screens[0].frame.size.height : 0.0;
}

CGRect ToCocoaRect(const RectI& rect) {
    const double height = PrimaryScreenHeight();
    return CGRectMake(rect.left, height - rect.bottom, rect.right - rect.left, rect.bottom - rect.top);
}

RectI FromCocoaRect(CGRect rect) {
    const double height = PrimaryScreenHeight();
    return RectI{static_cast<int32_t>(std::lround(CGRectGetMinX(rect))), static_cast<int32_t>(std::lround(height - CGRectGetMaxY(rect))),
                 static_cast<int32_t>(std::lround(CGRectGetMaxX(rect))), static_cast<int32_t>(std::lround(height - CGRectGetMinY(rect)))};
}

NSString* ToNSString(const std::wstring& text) {
    const std::string utf8 = Narrow(text);
    NSString* string = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
    return string != nil ? string : @"";
}

std::wstring FromNSString(NSString* text) {
    const char* utf8 = text.UTF8String;
    return utf8 != nullptr ? Widen(utf8) : std::wstring();
}

}
