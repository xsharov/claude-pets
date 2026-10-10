#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cp {

std::vector<std::wstring> ClaudeDesktopDataDirectories();
int64_t ClaudeDesktopStartedMs();
bool IsClaudeDesktopRunning();
bool IsClaudeForeground();
bool OpenClaudeSession(const std::string& sessionId);
bool ActivateClaude();
void BringClaudeToFront();

}
