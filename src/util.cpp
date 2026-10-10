#include "util.h"

#include <algorithm>
#include <cwctype>

namespace cp {

namespace {

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

std::wstring ClaudeConfigDirectory() {
    const std::wstring custom = GetEnv(L"CLAUDE_CONFIG_DIR");
    if (!custom.empty()) {
        return custom;
    }
    return JoinPath(HomeDirectory(), L".claude");
}

std::wstring JoinPath(std::wstring_view base, std::wstring_view leaf) {
    if (base.empty()) {
        return std::wstring(leaf);
    }
    std::wstring result(base);
    if (result.back() != L'\\' && result.back() != L'/') {
        result.push_back(kPathSeparator);
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

bool ReadWholeFile(const std::wstring& path, std::string& out, size_t limit) {
    uint64_t size = 0;
    if (!ReadFileTail(path, limit, out, size)) {
        return false;
    }
    return size <= limit;
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
