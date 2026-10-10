#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace cp {

inline constexpr wchar_t kOverlayWindowClass[] = L"ClaudePetsOverlay";
inline constexpr ULONG_PTR kHookCopyDataId = 0x43504554;
inline constexpr UINT kShowPetMessage = WM_APP + 2;

std::wstring KnownFolderPath(const GUID& folderId);
int64_t FileTimeToUnixMs(const FILETIME& fileTime);

}
