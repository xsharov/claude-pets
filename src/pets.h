#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cp {

struct PetInfo {
    std::wstring key;
    std::wstring directory;
    std::wstring id;
    std::wstring displayName;
    std::wstring description;
    std::wstring spritesheet;
    std::wstring fallbackSpritesheet;
    int spriteVersion = 1;
};

enum class PetSource : uint8_t { Claude, Codex };

struct CodexPetPreference {
    std::wstring petKey;
    int mascotWidth = 0;
};

std::wstring PetFolder(PetSource source);
const wchar_t* PetSourceKey(PetSource source);
PetSource ResolvePetSource(const std::wstring& setting);
std::vector<PetInfo> DiscoverPets(PetSource source);
std::wstring PetFolderName(const std::wstring& key);
const PetInfo* FindPet(const std::vector<PetInfo>& pets, const std::wstring& key);
CodexPetPreference ReadCodexPetPreference();

}
