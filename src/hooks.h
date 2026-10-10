#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cp {

inline constexpr wchar_t kHookArgument[] = L"--claude-pets-hook";

enum class HookState : uint8_t { NotInstalled, Installed, Outdated, Unreadable };

std::wstring ClaudeSettingsPath();
HookState QueryHookState(const std::wstring& executable);
bool InstallHooks(const std::wstring& executable);
bool UninstallHooks();
int RunHookRelay();

bool ReadHookInput(std::string& payload, size_t limit);
void DeliverHookEvent(const std::string& message);

}
