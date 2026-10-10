#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cp {

#if defined(_WIN32)
inline constexpr wchar_t kPathSeparator = L'\\';
#else
inline constexpr wchar_t kPathSeparator = L'/';
#endif

std::wstring Widen(std::string_view text);
std::string Narrow(std::wstring_view text);

std::wstring GetEnv(const wchar_t* name);
std::wstring HomeDirectory();
std::wstring ClaudeConfigDirectory();
std::wstring AppDataDirectory();
std::wstring ExecutablePath();

std::wstring JoinPath(std::wstring_view base, std::wstring_view leaf);
std::wstring FileNameOf(std::wstring_view path);
std::string FileNameOf(std::string_view path);
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool EnsureDirectory(const std::wstring& path);
std::vector<std::wstring> ListDirectories(const std::wstring& directory);
std::vector<std::wstring> ListFiles(const std::wstring& directory, std::wstring_view pattern);

struct FileStamp {
    uint64_t size = 0;
    int64_t modifiedMs = 0;
    bool operator==(const FileStamp&) const = default;
};

bool StatFile(const std::wstring& path, FileStamp& stamp);
bool ReadWholeFile(const std::wstring& path, std::string& out, size_t limit = size_t{64} << 20);
bool ReadFileTail(const std::wstring& path, size_t maxBytes, std::string& out, uint64_t& fileSize);
bool WriteFileAtomically(const std::wstring& path, std::string_view data);
bool AppendToFile(const std::wstring& path, std::string_view data);
bool CopyFileIfMissing(const std::wstring& source, const std::wstring& target);

int64_t UnixNowMs();
double MonotonicMs();
std::string LocalTimeStamp();
int64_t ParseIsoTimestampMs(std::string_view text);

std::string CollapseWhitespace(std::string_view text);
std::string FirstMeaningfulLine(std::string_view text);
std::string StripMarkdown(std::string_view text);
std::string TruncateUtf8(std::string_view text, size_t maxCodePoints);
bool StartsWith(std::string_view text, std::string_view prefix);
bool StartsWith(std::wstring_view text, std::wstring_view prefix);
std::wstring ReplaceAll(std::wstring text, std::wstring_view from, std::wstring_view to);
std::wstring ToLower(std::wstring_view text);

}
