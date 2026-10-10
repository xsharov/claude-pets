#include "settings.h"

#include "json.h"
#include "util.h"

#include <algorithm>

namespace cp {

namespace {

constexpr size_t kDismissedLimit = 300;
constexpr size_t kPositionLimit = 16;

}

std::wstring SettingsPath() {
    return JoinPath(AppDataDirectory(), L"settings.json");
}

Settings LoadSettings() {
    Settings settings;
    std::string text;
    if (!ReadWholeFile(SettingsPath(), text, 4u << 20)) {
        return settings;
    }
    Json root;
    if (!ParseJson(text, root) || !root.isObject()) {
        return settings;
    }
    settings.existed = true;
    settings.petKey = Widen(root["pet"].str());
    if (root["petsFolder"].isString()) {
        settings.petsFolder = Widen(root["petsFolder"].str());
    }
    settings.mascotWidth = static_cast<int>(root["mascotWidth"].integer(settings.mascotWidth));
    settings.showActivity = root["showActivity"].boolean(settings.showActivity);
    settings.lookAtCursor = root["lookAtCursor"].boolean(settings.lookAtCursor);
    if (root["language"].isString()) {
        settings.language = Widen(root["language"].str());
    }
    if (root["theme"].isString()) {
        settings.theme = Widen(root["theme"].str());
    }
    if (root["hotkey"].isString()) {
        settings.hotkey = Widen(root["hotkey"].str());
    }
    settings.lastMonitorKey = Widen(root["lastMonitor"].str());
    settings.lastAliveMs = root["lastAlive"].integer();
    for (const Json& item : root["positions"].items()) {
        SavedPosition position;
        position.monitorKey = Widen(item["monitor"].str());
        position.anchor.x = static_cast<int32_t>(item["x"].integer());
        position.anchor.y = static_cast<int32_t>(item["y"].integer());
        if (!position.monitorKey.empty()) {
            settings.positions.push_back(std::move(position));
        }
    }
    for (const Json& item : root["greetedPets"].items()) {
        settings.greetedPets.push_back(Widen(item.str()));
    }
    for (const Json& item : root["dismissed"].items()) {
        settings.dismissed.emplace_back(item["id"].str(), item["turn"].str());
    }
    for (const Json& item : root["muted"].items()) {
        settings.muted.insert(item.str());
    }
    return settings;
}

bool SaveSettings(const Settings& settings) {
    Json root = Json::MakeObject();
    root.set("pet", Json::MakeString(Narrow(settings.petKey)));
    root.set("petsFolder", Json::MakeString(Narrow(settings.petsFolder)));
    root.set("mascotWidth", Json::MakeInteger(settings.mascotWidth));
    root.set("showActivity", Json::MakeBool(settings.showActivity));
    root.set("lookAtCursor", Json::MakeBool(settings.lookAtCursor));
    root.set("language", Json::MakeString(Narrow(settings.language)));
    root.set("theme", Json::MakeString(Narrow(settings.theme)));
    root.set("hotkey", Json::MakeString(Narrow(settings.hotkey)));
    root.set("lastMonitor", Json::MakeString(Narrow(settings.lastMonitorKey)));
    root.set("lastAlive", Json::MakeInteger(settings.lastAliveMs));
    Json positions = Json::MakeArray();
    for (const SavedPosition& position : settings.positions) {
        Json item = Json::MakeObject();
        item.set("monitor", Json::MakeString(Narrow(position.monitorKey)));
        item.set("x", Json::MakeInteger(position.anchor.x));
        item.set("y", Json::MakeInteger(position.anchor.y));
        positions.push(std::move(item));
    }
    root.set("positions", std::move(positions));
    Json greeted = Json::MakeArray();
    for (const std::wstring& pet : settings.greetedPets) {
        greeted.push(Json::MakeString(Narrow(pet)));
    }
    root.set("greetedPets", std::move(greeted));
    Json dismissed = Json::MakeArray();
    for (const auto& [id, turn] : settings.dismissed) {
        Json item = Json::MakeObject();
        item.set("id", Json::MakeString(id));
        item.set("turn", Json::MakeString(turn));
        dismissed.push(std::move(item));
    }
    root.set("dismissed", std::move(dismissed));
    Json muted = Json::MakeArray();
    for (const std::string& id : settings.muted) {
        muted.push(Json::MakeString(id));
    }
    root.set("muted", std::move(muted));
    if (!EnsureDirectory(AppDataDirectory())) {
        return false;
    }
    return WriteFileAtomically(SettingsPath(), SerializeJson(root, true));
}

const SavedPosition* FindPosition(const Settings& settings, const std::wstring& monitorKey) {
    for (const SavedPosition& position : settings.positions) {
        if (position.monitorKey == monitorKey) {
            return &position;
        }
    }
    return nullptr;
}

void StorePosition(Settings& settings, const std::wstring& monitorKey, PointI anchor) {
    auto existing = std::find_if(settings.positions.begin(), settings.positions.end(), [&](const SavedPosition& position) {
        return position.monitorKey == monitorKey;
    });
    if (existing != settings.positions.end()) {
        settings.positions.erase(existing);
    }
    settings.positions.push_back({monitorKey, anchor});
    if (settings.positions.size() > kPositionLimit) {
        settings.positions.erase(settings.positions.begin());
    }
    settings.lastMonitorKey = monitorKey;
}

bool IsDismissed(const Settings& settings, const std::string& id, const std::string& turnKey) {
    for (const auto& [dismissedId, dismissedTurn] : settings.dismissed) {
        if (dismissedId == id && dismissedTurn == turnKey) {
            return true;
        }
    }
    return false;
}

void Dismiss(Settings& settings, const std::string& id, const std::string& turnKey) {
    auto existing = std::find_if(settings.dismissed.begin(), settings.dismissed.end(), [&](const auto& entry) { return entry.first == id; });
    if (existing != settings.dismissed.end()) {
        settings.dismissed.erase(existing);
    }
    settings.dismissed.emplace_back(id, turnKey);
    if (settings.dismissed.size() > kDismissedLimit) {
        settings.dismissed.erase(settings.dismissed.begin());
    }
}

}
