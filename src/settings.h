#pragma once

#include "geometry.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cp {

struct SavedPosition {
    std::wstring monitorKey;
    PointI anchor = {};
};

struct Settings {
    std::wstring petKey;
    std::wstring petsFolder = L"auto";
    int mascotWidth = 112;
    bool showActivity = true;
    bool lookAtCursor = true;
    std::wstring language = L"auto";
    std::wstring theme = L"auto";
    std::wstring hotkey = L"Ctrl+Alt+P";
    std::wstring lastMonitorKey;
    int64_t lastAliveMs = 0;
    std::vector<SavedPosition> positions;
    std::vector<std::wstring> greetedPets;
    std::vector<std::pair<std::string, std::string>> dismissed;
    std::set<std::string> muted;
    bool existed = false;
};

std::wstring SettingsPath();
Settings LoadSettings();
bool SaveSettings(const Settings& settings);

const SavedPosition* FindPosition(const Settings& settings, const std::wstring& monitorKey);
void StorePosition(Settings& settings, const std::wstring& monitorKey, PointI anchor);
bool IsDismissed(const Settings& settings, const std::string& id, const std::string& turnKey);
void Dismiss(Settings& settings, const std::string& id, const std::string& turnKey);

}
