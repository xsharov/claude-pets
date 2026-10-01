#pragma once

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

struct CodexPetPreference {
    std::wstring petKey;
    int mascotWidth = 0;
};

std::vector<std::wstring> PetRoots();
std::vector<PetInfo> DiscoverPets();
const PetInfo* FindPet(const std::vector<PetInfo>& pets, const std::wstring& key);
CodexPetPreference ReadCodexPetPreference();

}
