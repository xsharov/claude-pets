#include "diagnostics.h"
#include "hooks.h"

#include "win/win_util.h"

namespace cp {

void WriteConsoleText(const std::string& text) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            output = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

bool ReadHookInput(std::string& payload, size_t limit) {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) {
        return false;
    }
    char buffer[16384];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(input, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        payload.append(buffer, read);
        if (payload.size() > limit) {
            return false;
        }
    }
    return true;
}

void DeliverHookEvent(const std::string& message) {
    const HWND target = FindWindowW(kOverlayWindowClass, nullptr);
    if (target == nullptr) {
        return;
    }
    COPYDATASTRUCT data = {};
    data.dwData = kHookCopyDataId;
    data.cbData = static_cast<DWORD>(message.size());
    data.lpData = const_cast<char*>(message.data());
    DWORD_PTR result = 0;
    SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 2000, &result);
}

}
