#include "pets.h"

#include "json.h"
#include "util.h"

#include <algorithm>
#include <charconv>

namespace cp {

namespace {

std::wstring CodexHome() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"CODEX_HOME", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::wstring(buffer, length);
    }
    return JoinPath(HomeDirectory(), L".codex");
}

struct PetRoot {
    std::wstring path;
    std::wstring source;
};

std::vector<PetRoot> Roots() {
    return {
        {JoinPath(CodexHome(), L"pets"), L"codex"},
        {JoinPath(ClaudeConfigDirectory(), L"pets"), L"claude"},
    };
}

bool LoadPet(const std::wstring& directory, const std::wstring& folderName, const std::wstring& source, PetInfo& pet) {
    std::string manifestText;
    Json manifest;
    if (ReadWholeFile(JoinPath(directory, L"pet.json"), manifestText, 1u << 20)) {
        ParseJson(manifestText, manifest);
    }
    std::wstring sheetName = Widen(manifest["spritesheetPath"].str());
    if (sheetName.empty()) {
        sheetName = L"spritesheet.webp";
    }
    std::wstring sheet = JoinPath(directory, sheetName);
    const std::wstring png = JoinPath(directory, L"spritesheet.png");
    if (!FileExists(sheet)) {
        if (!FileExists(png)) {
            return false;
        }
        sheet = png;
    }
    pet.key = source + L":" + folderName;
    pet.directory = directory;
    pet.id = Widen(manifest["id"].str());
    pet.displayName = Widen(manifest["displayName"].str());
    if (pet.displayName.empty()) {
        pet.displayName = pet.id.empty() ? folderName : pet.id;
    }
    pet.description = Widen(manifest["description"].str());
    pet.spritesheet = sheet;
    pet.fallbackSpritesheet = (FileExists(png) && ToLower(png) != ToLower(sheet)) ? png : std::wstring();
    pet.spriteVersion = static_cast<int>(manifest["spriteVersionNumber"].integer(1));
    return true;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool ReadTomlValue(std::string_view config, std::string_view key, std::string& value) {
    size_t start = 0;
    while (start < config.size()) {
        size_t end = config.find('\n', start);
        if (end == std::string_view::npos) {
            end = config.size();
        }
        std::string_view line = Trim(config.substr(start, end - start));
        start = end + 1;
        if (!StartsWith(line, key)) {
            continue;
        }
        line = Trim(line.substr(key.size()));
        if (line.empty() || line.front() != '=') {
            continue;
        }
        line = Trim(line.substr(1));
        if (!line.empty() && (line.front() == '"' || line.front() == '\'')) {
            const char quote = line.front();
            const size_t close = line.find(quote, 1);
            if (close == std::string_view::npos) {
                return false;
            }
            value.assign(line.substr(1, close - 1));
            return true;
        }
        const size_t comment = line.find('#');
        value.assign(Trim(line.substr(0, comment)));
        return true;
    }
    return false;
}

std::wstring ResolveCloudPetId(const std::string& cloudId) {
    std::string stateText;
    if (!ReadWholeFile(JoinPath(CodexHome(), L".codex-global-state.json"), stateText, 32u << 20)) {
        return {};
    }
    Json state;
    if (!ParseJson(stateText, state)) {
        return {};
    }
    const Json& migrated = state["electron-persisted-atom-state"]["migrated-cloud-pet-ids-v1"];
    for (const Json::Member& account : migrated.members()) {
        for (const Json::Member& mapping : account.second.members()) {
            if (mapping.second.str() == cloudId && StartsWith(mapping.first, "custom:")) {
                return L"codex:" + Widen(mapping.first.substr(7));
            }
        }
    }
    return {};
}

}

std::vector<std::wstring> PetRoots() {
    std::vector<std::wstring> result;
    for (const PetRoot& root : Roots()) {
        result.push_back(root.path);
    }
    return result;
}

std::vector<PetInfo> DiscoverPets() {
    std::vector<PetInfo> pets;
    for (const PetRoot& root : Roots()) {
        if (!DirectoryExists(root.path)) {
            continue;
        }
        for (const std::wstring& folder : ListDirectories(root.path)) {
            PetInfo pet;
            if (LoadPet(JoinPath(root.path, folder), folder, root.source, pet)) {
                pets.push_back(std::move(pet));
            }
        }
    }
    std::sort(pets.begin(), pets.end(), [](const PetInfo& a, const PetInfo& b) {
        return ToLower(a.displayName) < ToLower(b.displayName);
    });
    return pets;
}

const PetInfo* FindPet(const std::vector<PetInfo>& pets, const std::wstring& key) {
    for (const PetInfo& pet : pets) {
        if (pet.key == key) {
            return &pet;
        }
    }
    return nullptr;
}

CodexPetPreference ReadCodexPetPreference() {
    CodexPetPreference preference;
    std::string config;
    if (!ReadWholeFile(JoinPath(CodexHome(), L"config.toml"), config, 4u << 20)) {
        return preference;
    }
    std::string width;
    if (ReadTomlValue(config, "avatar-overlay-mascot-width-px", width)) {
        int parsed = 0;
        const auto result = std::from_chars(width.data(), width.data() + width.size(), parsed);
        if (result.ec == std::errc()) {
            preference.mascotWidth = parsed;
        }
    }
    std::string selected;
    if (!ReadTomlValue(config, "selected-avatar-id", selected)) {
        return preference;
    }
    if (StartsWith(selected, "custom:")) {
        preference.petKey = L"codex:" + Widen(selected.substr(7));
    } else if (StartsWith(selected, "pet_")) {
        preference.petKey = ResolveCloudPetId(selected);
    }
    return preference;
}

}
