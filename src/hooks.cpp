#include "hooks.h"

#include "json.h"
#include "util.h"

#include <iterator>
#include <string_view>

namespace cp {

namespace {

constexpr const char* kEvents[] = {
    "UserPromptSubmit", "PreToolUse",   "PostToolUse", "PostToolUseFailure", "PermissionRequest", "PermissionDenied", "Notification",
    "Stop",             "StopFailure",  "SubagentStart", "SessionStart",     "SessionEnd",        "Elicitation",      "ElicitationResult",
};

constexpr char kArgument[] = "--claude-pets-hook";
constexpr size_t kMaxPayload = size_t{8} << 20;

bool IsToolEvent(std::string_view event) {
    return event == "PreToolUse" || event == "PostToolUse" || event == "PostToolUseFailure" || event == "PermissionRequest" || event == "PermissionDenied";
}

bool IsOurHook(const Json& hook) {
    for (const Json& argument : hook["args"].items()) {
        if (argument.str() == kArgument) {
            return true;
        }
    }
    return hook["command"].str().find(kArgument) != std::string::npos;
}

bool LoadClaudeSettings(Json& root, bool& existed) {
    std::string text;
    existed = ReadWholeFile(ClaudeSettingsPath(), text, size_t{16} << 20);
    if (!existed || text.find_first_not_of(" \t\r\n") == std::string::npos) {
        root = Json::MakeObject();
        return true;
    }
    return ParseJson(text, root) && root.isObject();
}

Json MakeHookGroup(const std::string& executable, bool toolEvent) {
    Json hook = Json::MakeObject();
    hook.set("type", Json::MakeString("command"));
    hook.set("command", Json::MakeString(executable));
    Json arguments = Json::MakeArray();
    arguments.push(Json::MakeString(kArgument));
    hook.set("args", std::move(arguments));
    hook.set("async", Json::MakeBool(true));
    hook.set("timeout", Json::MakeInteger(10));
    Json hooks = Json::MakeArray();
    hooks.push(std::move(hook));
    Json group = Json::MakeObject();
    if (toolEvent) {
        group.set("matcher", Json::MakeString("*"));
    }
    group.set("hooks", std::move(hooks));
    return group;
}

bool RemoveOurHooks(Json& root) {
    Json* hooks = root.find("hooks");
    if (hooks == nullptr || !hooks->isObject()) {
        return false;
    }
    bool removedAny = false;
    std::vector<std::string> emptyEvents;
    for (const Json::Member& member : hooks->members()) {
        Json* groups = hooks->find(member.first);
        if (groups == nullptr || !groups->isArray()) {
            continue;
        }
        std::vector<Json>& groupItems = groups->mutableItems();
        bool removedInEvent = false;
        for (auto group = groupItems.begin(); group != groupItems.end();) {
            Json* entries = group->find("hooks");
            if (entries == nullptr || !entries->isArray()) {
                ++group;
                continue;
            }
            std::vector<Json>& entryItems = entries->mutableItems();
            const size_t before = entryItems.size();
            for (auto entry = entryItems.begin(); entry != entryItems.end();) {
                entry = IsOurHook(*entry) ? entryItems.erase(entry) : std::next(entry);
            }
            const bool removed = entryItems.size() != before;
            removedInEvent = removedInEvent || removed;
            group = removed && entryItems.empty() ? groupItems.erase(group) : std::next(group);
        }
        removedAny = removedAny || removedInEvent;
        if (removedInEvent && groupItems.empty()) {
            emptyEvents.push_back(member.first);
        }
    }
    for (const std::string& event : emptyEvents) {
        hooks->erase(event);
    }
    if (removedAny && hooks->size() == 0) {
        root.erase("hooks");
    }
    return removedAny;
}

bool SaveClaudeSettings(const Json& root) {
    const std::wstring path = ClaudeSettingsPath();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !EnsureDirectory(path.substr(0, slash))) {
        return false;
    }
    return WriteFileAtomically(path, SerializeJson(root, true));
}

void CopyString(const Json& source, Json& target, const char* key, size_t limit) {
    const Json& value = source[key];
    if (value.isString() && !value.str().empty()) {
        target.set(key, Json::MakeString(TruncateUtf8(value.str(), limit)));
    }
}

std::string MinimizePayload(const Json& payload) {
    Json result = Json::MakeObject();
    for (const char* key : {"session_id", "transcript_path", "cwd", "hook_event_name", "tool_name", "tool_use_id", "notification_type", "agent_type",
                            "mcp_server_name", "permission_mode"}) {
        CopyString(payload, result, key, 2048);
    }
    for (const char* key : {"message", "error", "error_message", "reason", "title"}) {
        CopyString(payload, result, key, 400);
    }
    const Json& input = payload["tool_input"];
    if (input.isObject()) {
        Json reduced = Json::MakeObject();
        for (const char* key : {"description", "file_path", "notebook_path", "path", "pattern", "query", "url", "skill", "subagent_type"}) {
            CopyString(input, reduced, key, 400);
        }
        CopyString(input, reduced, "command", 300);
        CopyString(input, reduced, "plan", 300);
        const Json& question = input["questions"][0];
        if (question.isObject()) {
            Json first = Json::MakeObject();
            CopyString(question, first, "question", 400);
            CopyString(question, first, "header", 200);
            Json questions = Json::MakeArray();
            questions.push(std::move(first));
            reduced.set("questions", std::move(questions));
        }
        result.set("tool_input", std::move(reduced));
    }
    return SerializeJson(result, false);
}

}

std::wstring ClaudeSettingsPath() {
    return JoinPath(ClaudeConfigDirectory(), L"settings.json");
}

HookState QueryHookState(const std::wstring& executable) {
    Json root;
    bool existed = false;
    if (!LoadClaudeSettings(root, existed)) {
        return HookState::Unreadable;
    }
    const Json& hooks = root["hooks"];
    const std::wstring expected = ToLower(executable);
    int found = 0;
    int current = 0;
    for (const char* event : kEvents) {
        bool eventCurrent = false;
        for (const Json& group : hooks[event].items()) {
            for (const Json& hook : group["hooks"].items()) {
                if (!IsOurHook(hook)) {
                    continue;
                }
                ++found;
                if (ToLower(Widen(hook["command"].str())) == expected) {
                    eventCurrent = true;
                }
            }
        }
        current += eventCurrent ? 1 : 0;
    }
    if (found == 0) {
        return HookState::NotInstalled;
    }
    return current == static_cast<int>(std::size(kEvents)) && found == current ? HookState::Installed : HookState::Outdated;
}

bool InstallHooks(const std::wstring& executable) {
    Json root;
    bool existed = false;
    if (!LoadClaudeSettings(root, existed)) {
        return false;
    }
    if (existed) {
        const std::wstring backup = ClaudeSettingsPath() + L".claude-pets.bak";
        if (!FileExists(backup)) {
            CopyFileW(ClaudeSettingsPath().c_str(), backup.c_str(), TRUE);
        }
    }
    RemoveOurHooks(root);
    Json* hooks = root.find("hooks");
    if (hooks == nullptr || !hooks->isObject()) {
        root.set("hooks", Json::MakeObject());
        hooks = root.find("hooks");
    }
    const std::string command = Narrow(executable);
    for (const char* event : kEvents) {
        Json* groups = hooks->find(event);
        if (groups == nullptr || !groups->isArray()) {
            hooks->set(event, Json::MakeArray());
            groups = hooks->find(event);
        }
        groups->push(MakeHookGroup(command, IsToolEvent(event)));
    }
    return SaveClaudeSettings(root);
}

bool UninstallHooks() {
    Json root;
    bool existed = false;
    if (!LoadClaudeSettings(root, existed)) {
        return false;
    }
    if (!existed || !RemoveOurHooks(root)) {
        return true;
    }
    return SaveClaudeSettings(root);
}

int RunHookRelay() {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) {
        return 0;
    }
    std::string payload;
    char buffer[16384];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(input, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        payload.append(buffer, read);
        if (payload.size() > kMaxPayload) {
            return 0;
        }
    }
    Json parsed;
    if (!ParseJson(payload, parsed) || !parsed.isObject()) {
        return 0;
    }
    const HWND target = FindWindowW(L"ClaudePetsOverlay", nullptr);
    if (target == nullptr) {
        return 0;
    }
    std::string message = MinimizePayload(parsed);
    COPYDATASTRUCT data = {};
    data.dwData = kHookCopyDataId;
    data.cbData = static_cast<DWORD>(message.size());
    data.lpData = message.data();
    DWORD_PTR result = 0;
    SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 2000, &result);
    return 0;
}

}
