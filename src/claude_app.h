#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace cp {

bool IsClaudeDesktopPath(const std::wstring& imagePath);
int64_t ClaudeDesktopStartedMs();
bool IsClaudeDesktopRunning();
HWND FindClaudeMainWindow();
bool IsClaudeForeground();
bool OpenClaudeSession(const std::string& sessionId);
bool ActivateClaude();
void BringClaudeToFront();

}
