#include "json.h"

#include <charconv>
#include <cmath>

namespace cp {

namespace {

const Json& NullJson() {
    static const Json value;
    return value;
}

const std::string& EmptyString() {
    static const std::string value;
    return value;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool Parse(Json& out) {
        SkipWhitespace();
        if (!ParseValue(out, 0)) {
            return false;
        }
        SkipWhitespace();
        return pos_ == text_.size();
    }

private:
    static constexpr int kMaxDepth = 256;

    std::string_view text_;
    size_t pos_ = 0;

    void SkipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    bool Consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool ConsumeLiteral(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    bool ParseValue(Json& out, int depth) {
        if (depth > kMaxDepth || pos_ >= text_.size()) {
            return false;
        }
        const char c = text_[pos_];
        switch (c) {
        case '{':
            return ParseObject(out, depth);
        case '[':
            return ParseArray(out, depth);
        case '"': {
            std::string value;
            if (!ParseString(value)) {
                return false;
            }
            out = Json::MakeString(std::move(value));
            return true;
        }
        case 't':
            if (ConsumeLiteral("true")) {
                out = Json::MakeBool(true);
                return true;
            }
            return false;
        case 'f':
            if (ConsumeLiteral("false")) {
                out = Json::MakeBool(false);
                return true;
            }
            return false;
        case 'n':
            if (ConsumeLiteral("null")) {
                out = Json();
                return true;
            }
            return false;
        default:
            return ParseNumber(out);
        }
    }

    bool ParseObject(Json& out, int depth) {
        ++pos_;
        out = Json::MakeObject();
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"' || !ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }
            SkipWhitespace();
            Json value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.set(key, std::move(value));
            SkipWhitespace();
            if (Consume(',')) {
                continue;
            }
            return Consume('}');
        }
    }

    bool ParseArray(Json& out, int depth) {
        ++pos_;
        out = Json::MakeArray();
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            SkipWhitespace();
            Json value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.push(std::move(value));
            SkipWhitespace();
            if (Consume(',')) {
                continue;
            }
            return Consume(']');
        }
    }

    static void AppendUtf8(std::string& out, uint32_t codePoint) {
        if (codePoint < 0x80) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    bool ParseHex4(uint32_t& value) {
        if (pos_ + 4 > text_.size()) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;
        for (;;) {
            const size_t chunkStart = pos_;
            while (pos_ < text_.size() && text_[pos_] != '"' && text_[pos_] != '\\') {
                ++pos_;
            }
            out.append(text_.data() + chunkStart, pos_ - chunkStart);
            if (pos_ >= text_.size()) {
                return false;
            }
            if (text_[pos_] == '"') {
                ++pos_;
                return true;
            }
            ++pos_;
            if (pos_ >= text_.size()) {
                return false;
            }
            const char escape = text_[pos_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t codePoint = 0;
                if (!ParseHex4(codePoint)) {
                    return false;
                }
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF && pos_ + 6 <= text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                    const size_t save = pos_;
                    pos_ += 2;
                    uint32_t low = 0;
                    if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        pos_ = save;
                        codePoint = 0xFFFD;
                    }
                } else if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
                    codePoint = 0xFFFD;
                }
                AppendUtf8(out, codePoint);
                break;
            }
            default:
                return false;
            }
        }
    }

    bool ParseNumber(Json& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        const size_t digitsStart = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        if (pos_ == digitsStart) {
            return false;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            const size_t fractionStart = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
            if (pos_ == fractionStart) {
                return false;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            const size_t exponentStart = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
            if (pos_ == exponentStart) {
                return false;
            }
        }
        out = Json::MakeRawNumber(std::string(text_.substr(start, pos_ - start)));
        return true;
    }
};

void AppendEscaped(std::string& out, const std::string& value) {
    out.push_back('"');
    for (const char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        default:
            if (c < 0x20) {
                static const char kHex[] = "0123456789abcdef";
                out.append("\\u00");
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 0xF]);
            } else {
                out.push_back(raw);
            }
        }
    }
    out.push_back('"');
}

void AppendNumber(std::string& out, const Json& value) {
    if (!value.rawNumber().empty()) {
        out.append(value.rawNumber());
        return;
    }
    const double number = value.num();
    if (!std::isfinite(number)) {
        out.append("0");
        return;
    }
    char buffer[64] = {};
    const double rounded = std::round(number);
    std::to_chars_result result;
    if (rounded == number && std::fabs(number) < 9007199254740992.0) {
        result = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<int64_t>(rounded));
    } else {
        result = std::to_chars(buffer, buffer + sizeof(buffer), number);
    }
    out.append(buffer, result.ptr);
}

void AppendIndent(std::string& out, int depth) {
    out.append(static_cast<size_t>(depth) * 2, ' ');
}

void Serialize(std::string& out, const Json& value, bool pretty, int depth) {
    switch (value.kind()) {
    case Json::Kind::Null:
        out.append("null");
        break;
    case Json::Kind::Bool:
        out.append(value.boolean() ? "true" : "false");
        break;
    case Json::Kind::Number:
        AppendNumber(out, value);
        break;
    case Json::Kind::String:
        AppendEscaped(out, value.str());
        break;
    case Json::Kind::Array: {
        if (value.items().empty()) {
            out.append("[]");
            break;
        }
        out.push_back('[');
        bool first = true;
        for (const Json& item : value.items()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            if (pretty) {
                out.push_back('\n');
                AppendIndent(out, depth + 1);
            }
            Serialize(out, item, pretty, depth + 1);
        }
        if (pretty) {
            out.push_back('\n');
            AppendIndent(out, depth);
        }
        out.push_back(']');
        break;
    }
    case Json::Kind::Object: {
        if (value.members().empty()) {
            out.append("{}");
            break;
        }
        out.push_back('{');
        bool first = true;
        for (const Json::Member& member : value.members()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            if (pretty) {
                out.push_back('\n');
                AppendIndent(out, depth + 1);
            }
            AppendEscaped(out, member.first);
            out.append(pretty ? ": " : ":");
            Serialize(out, member.second, pretty, depth + 1);
        }
        if (pretty) {
            out.push_back('\n');
            AppendIndent(out, depth);
        }
        out.push_back('}');
        break;
    }
    }
}

}

Json Json::MakeBool(bool value) {
    Json result;
    result.kind_ = Kind::Bool;
    result.bool_ = value;
    return result;
}

Json Json::MakeNumber(double value) {
    Json result;
    result.kind_ = Kind::Number;
    result.number_ = value;
    return result;
}

Json Json::MakeInteger(int64_t value) {
    Json result;
    result.kind_ = Kind::Number;
    result.number_ = static_cast<double>(value);
    result.text_ = std::to_string(value);
    return result;
}

Json Json::MakeRawNumber(std::string raw) {
    Json result;
    result.kind_ = Kind::Number;
    double parsed = 0.0;
    std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    result.number_ = parsed;
    result.text_ = std::move(raw);
    return result;
}

Json Json::MakeString(std::string value) {
    Json result;
    result.kind_ = Kind::String;
    result.text_ = std::move(value);
    return result;
}

Json Json::MakeArray() {
    Json result;
    result.kind_ = Kind::Array;
    return result;
}

Json Json::MakeObject() {
    Json result;
    result.kind_ = Kind::Object;
    return result;
}

const std::string& Json::str() const {
    return kind_ == Kind::String ? text_ : EmptyString();
}

double Json::num(double fallback) const {
    return kind_ == Kind::Number ? number_ : fallback;
}

int64_t Json::integer(int64_t fallback) const {
    if (kind_ != Kind::Number) {
        return fallback;
    }
    int64_t parsed = 0;
    const auto result = std::from_chars(text_.data(), text_.data() + text_.size(), parsed);
    if (result.ec == std::errc() && result.ptr == text_.data() + text_.size()) {
        return parsed;
    }
    return static_cast<int64_t>(number_);
}

bool Json::boolean(bool fallback) const {
    return kind_ == Kind::Bool ? bool_ : fallback;
}

const Json& Json::operator[](std::string_view key) const {
    const Json* found = find(key);
    return found != nullptr ? *found : NullJson();
}

const Json& Json::operator[](size_t index) const {
    return kind_ == Kind::Array && index < items_.size() ? items_[index] : NullJson();
}

size_t Json::size() const {
    if (kind_ == Kind::Array) {
        return items_.size();
    }
    if (kind_ == Kind::Object) {
        return members_.size();
    }
    return 0;
}

Json* Json::find(std::string_view key) {
    if (kind_ != Kind::Object) {
        return nullptr;
    }
    for (Member& member : members_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

const Json* Json::find(std::string_view key) const {
    if (kind_ != Kind::Object) {
        return nullptr;
    }
    for (const Member& member : members_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

Json& Json::set(std::string_view key, Json value) {
    if (kind_ != Kind::Object) {
        *this = MakeObject();
    }
    if (Json* existing = find(key)) {
        *existing = std::move(value);
        return *existing;
    }
    members_.emplace_back(std::string(key), std::move(value));
    return members_.back().second;
}

bool Json::erase(std::string_view key) {
    for (auto it = members_.begin(); it != members_.end(); ++it) {
        if (it->first == key) {
            members_.erase(it);
            return true;
        }
    }
    return false;
}

Json& Json::push(Json value) {
    if (kind_ != Kind::Array) {
        *this = MakeArray();
    }
    items_.push_back(std::move(value));
    return items_.back();
}

bool ParseJson(std::string_view text, Json& out) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Parser parser(text);
    return parser.Parse(out);
}

std::string SerializeJson(const Json& value, bool pretty) {
    std::string out;
    Serialize(out, value, pretty, 0);
    if (pretty) {
        out.push_back('\n');
    }
    return out;
}

}
