#pragma once

#include <windows.h>

#include <string>

namespace cp {

inline constexpr ULONG_PTR kHookCopyDataId = 0x43504554;
inline constexpr wchar_t kHookArgument[] = L"--claude-pets-hook";

enum class HookState : uint8_t { NotInstalled, Installed, Outdated, Unreadable };

std::wstring ClaudeSettingsPath();
HookState QueryHookState(const std::wstring& executable);
bool InstallHooks(const std::wstring& executable);
bool UninstallHooks();
int RunHookRelay();

}
