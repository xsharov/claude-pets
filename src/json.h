#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cp {

class Json {
public:
    enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };

    using Member = std::pair<std::string, Json>;

    Json() = default;

    static Json MakeBool(bool value);
    static Json MakeNumber(double value);
    static Json MakeInteger(int64_t value);
    static Json MakeRawNumber(std::string raw);
    static Json MakeString(std::string value);
    static Json MakeArray();
    static Json MakeObject();

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isBool() const { return kind_ == Kind::Bool; }
    bool isNumber() const { return kind_ == Kind::Number; }
    bool isString() const { return kind_ == Kind::String; }
    bool isArray() const { return kind_ == Kind::Array; }
    bool isObject() const { return kind_ == Kind::Object; }

    const std::string& str() const;
    double num(double fallback = 0.0) const;
    int64_t integer(int64_t fallback = 0) const;
    bool boolean(bool fallback = false) const;

    const Json& operator[](std::string_view key) const;
    const Json& operator[](size_t index) const;
    size_t size() const;

    const std::vector<Json>& items() const { return items_; }
    const std::vector<Member>& members() const { return members_; }
    std::vector<Json>& mutableItems() { return items_; }

    Json* find(std::string_view key);
    const Json* find(std::string_view key) const;
    Json& set(std::string_view key, Json value);
    bool erase(std::string_view key);
    Json& push(Json value);

    const std::string& rawNumber() const { return text_; }

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string text_;
    std::vector<Json> items_;
    std::vector<Member> members_;
};

bool ParseJson(std::string_view text, Json& out);
std::string SerializeJson(const Json& value, bool pretty);

}
