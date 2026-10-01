#pragma once

#include <windows.h>

#include <string>

namespace cp {

bool IsClaudeDesktopPath(const std::wstring& imagePath);
bool IsClaudeDesktopRunning();
HWND FindClaudeMainWindow();
bool IsClaudeForeground();
bool OpenClaudeSession(const std::string& sessionId);
bool ActivateClaude();
void BringClaudeToFront();

}
