#include "transcript.h"

#include "util.h"

#include <vector>

namespace cp {

namespace {

constexpr size_t kTailWindows[] = {size_t{256} << 10, size_t{2} << 20, size_t{12} << 20};
constexpr size_t kMaxEntriesScanned = 800;
constexpr size_t kTextLimit = 160;

enum class UserKind : uint8_t { Prompt, ToolResult, Interrupt, LocalCommand };

std::vector<std::string_view> SplitLines(std::string_view data, bool dropFirst) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    if (dropFirst) {
        const size_t newline = data.find('\n');
        start = newline == std::string_view::npos ? data.size() : newline + 1;
    }
    while (start < data.size()) {
        size_t end = data.find('\n', start);
        if (end == std::string_view::npos) {
            end = data.size();
        }
        std::string_view line = data.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
        start = end + 1;
    }
    return lines;
}

std::string TextOf(const Json& content) {
    if (content.isString()) {
        return content.str();
    }
    std::string text;
    for (const Json& block : content.items()) {
        if (block["type"].str() == "text") {
            if (!text.empty()) {
                text.push_back('\n');
            }
            text.append(block["text"].str());
        }
    }
    return text;
}

std::string Summarize(std::string_view text) {
    return TruncateUtf8(FirstMeaningfulLine(text), kTextLimit);
}

std::string HostOf(std::string_view url) {
    const size_t scheme = url.find("://");
    std::string_view rest = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    const size_t slash = rest.find_first_of("/?#");
    if (slash != std::string_view::npos) {
        rest = rest.substr(0, slash);
    }
    if (StartsWith(rest, "www.")) {
        rest.remove_prefix(4);
    }
    return std::string(rest);
}

std::string HumanizeToolName(std::string_view name) {
    const size_t separator = name.rfind("__");
    std::string_view base = separator == std::string_view::npos ? name : name.substr(separator + 2);
    std::string result(base);
    for (char& c : result) {
        if (c == '_') {
            c = ' ';
        }
    }
    return result;
}

std::string PathLeaf(const Json& input) {
    for (const char* key : {"file_path", "notebook_path", "path"}) {
        const std::string& value = input[key].str();
        if (!value.empty()) {
            return FileNameOf(std::string_view(value));
        }
    }
    return {};
}

UserKind ClassifyUser(const Json& content) {
    if (content.isArray()) {
        for (const Json& block : content.items()) {
            if (block["type"].str() == "tool_result") {
                return UserKind::ToolResult;
            }
        }
    }
    const std::string text = TextOf(content);
    std::string_view view(text);
    while (!view.empty() && (view.front() == ' ' || view.front() == '\n' || view.front() == '\r')) {
        view.remove_prefix(1);
    }
    if (StartsWith(view, "[Request interrupted by user")) {
        return UserKind::Interrupt;
    }
    if (StartsWith(view, "<local-command-stdout>") || StartsWith(view, "<local-command-stderr>") || StartsWith(view, "<bash-stdout>") ||
        StartsWith(view, "<bash-stderr>") || StartsWith(view, "<bash-input>") || StartsWith(view, "<local-command-caveat>")) {
        return UserKind::LocalCommand;
    }
    return UserKind::Prompt;
}

void ClassifyAssistant(const Json& entry, TranscriptSnapshot& out, bool& needFinalText) {
    const Json& message = entry["message"];
    const Json& content = message["content"];
    if (entry["isApiErrorMessage"].boolean()) {
        out.phase = TurnPhase::Failed;
        out.finalText = Summarize(TextOf(content));
        return;
    }
    const std::string& stop = message["stop_reason"].str();
    if (stop == "end_turn" || stop == "stop_sequence" || stop == "max_tokens" || stop == "refusal" || stop == "model_context_window_exceeded") {
        out.phase = TurnPhase::Completed;
        needFinalText = true;
        return;
    }
    const Json& last = content[content.size() == 0 ? 0 : content.size() - 1];
    const std::string& blockType = last["type"].str();
    if (blockType == "tool_use" || blockType == "server_tool_use") {
        out.activity = ActivityForTool(last["name"].str(), last["input"]);
        out.pendingToolUseId = last["id"].str();
        const bool interactive = out.activity.kind == ActivityKind::Question || out.activity.kind == ActivityKind::PlanReview;
        out.phase = interactive ? TurnPhase::Waiting : TurnPhase::Running;
        return;
    }
    out.phase = TurnPhase::Running;
    if (blockType == "text") {
        out.activity = {ActivityKind::Writing, Summarize(last["text"].str())};
        return;
    }
    out.activity = {ActivityKind::Thinking, {}};
}

bool AnalyzeLines(const std::vector<std::string_view>& lines, TranscriptSnapshot& out) {
    bool terminalFound = false;
    bool needFinalText = false;
    size_t scanned = 0;
    for (size_t index = lines.size(); index-- > 0;) {
        const std::string_view line = lines[index];
        if (line.front() != '{') {
            continue;
        }
        Json entry;
        if (!ParseJson(line, entry)) {
            continue;
        }
        const std::string& type = entry["type"].str();
        if (type != "assistant" && type != "user" && type != "system") {
            continue;
        }
        if (entry["isSidechain"].boolean()) {
            continue;
        }
        if (++scanned > kMaxEntriesScanned) {
            break;
        }
        const int64_t timestamp = ParseIsoTimestampMs(entry["timestamp"].str());
        if (type == "system") {
            if (terminalFound) {
                continue;
            }
            const std::string& subtype = entry["subtype"].str();
            if (subtype == "turn_duration" || subtype == "stop_hook_summary") {
                terminalFound = true;
                out.phase = TurnPhase::Completed;
                out.completedMs = timestamp;
                out.lastEntryMs = timestamp;
                needFinalText = true;
            } else if (subtype == "api_error") {
                terminalFound = true;
                out.phase = TurnPhase::Running;
                out.activity = {ActivityKind::Retrying, {}};
                out.lastEntryMs = timestamp;
            }
            continue;
        }
        if (type == "assistant") {
            if (!terminalFound) {
                terminalFound = true;
                out.lastEntryMs = timestamp;
                ClassifyAssistant(entry, out, needFinalText);
                if (out.phase == TurnPhase::Completed) {
                    out.completedMs = timestamp;
                }
            }
            if (out.lastAssistantUuid.empty()) {
                out.lastAssistantUuid = entry["uuid"].str();
            }
            if (needFinalText && out.finalText.empty()) {
                out.finalText = Summarize(TextOf(entry["message"]["content"]));
            }
            continue;
        }
        if (entry["isMeta"].boolean() || entry["isCompactSummary"].boolean() || entry["isVisibleInTranscriptOnly"].boolean()) {
            continue;
        }
        const UserKind kind = ClassifyUser(entry["message"]["content"]);
        if (!terminalFound) {
            terminalFound = true;
            out.lastEntryMs = timestamp;
            switch (kind) {
            case UserKind::ToolResult:
            case UserKind::Prompt:
                out.phase = TurnPhase::Running;
                out.activity = {ActivityKind::Thinking, {}};
                break;
            case UserKind::Interrupt:
            case UserKind::LocalCommand:
                out.phase = TurnPhase::Interrupted;
                break;
            }
        }
        if (kind == UserKind::Prompt) {
            out.turnKey = entry["uuid"].str();
            out.turnStartedMs = timestamp;
            break;
        }
    }
    return terminalFound;
}

}

Activity ActivityForTool(std::string_view name, const Json& input) {
    if (name == "Bash" || name == "PowerShell" || name == "BashOutput" || name == "Monitor") {
        std::string detail = input["description"].str();
        if (detail.empty()) {
            detail = Summarize(input["command"].str());
        }
        return {ActivityKind::Command, TruncateUtf8(detail, 80)};
    }
    if (name == "Read") {
        return {ActivityKind::Read, PathLeaf(input)};
    }
    if (name == "Edit" || name == "MultiEdit" || name == "Write" || name == "NotebookEdit") {
        return {ActivityKind::Edit, PathLeaf(input)};
    }
    if (name == "Grep" || name == "Glob") {
        return {ActivityKind::Search, TruncateUtf8(input["pattern"].str(), 60)};
    }
    if (name == "WebSearch" || name == "web_search") {
        return {ActivityKind::WebSearch, TruncateUtf8(input["query"].str(), 60)};
    }
    if (name == "WebFetch" || name == "web_fetch") {
        return {ActivityKind::WebFetch, HostOf(input["url"].str())};
    }
    if (name == "Task" || name == "Agent") {
        return {ActivityKind::Agent, TruncateUtf8(input["description"].str(), 80)};
    }
    if (name == "TodoWrite" || name == "TaskCreate" || name == "TaskUpdate") {
        return {ActivityKind::Todo, {}};
    }
    if (name == "Skill") {
        std::string skill = input["skill"].str();
        if (skill.empty()) {
            skill = input["command"].str();
        }
        return {ActivityKind::Skill, skill};
    }
    if (name == "AskUserQuestion") {
        const Json& question = input["questions"][0];
        std::string text = question["question"].str();
        if (text.empty()) {
            text = question["header"].str();
        }
        return {ActivityKind::Question, Summarize(text)};
    }
    if (name == "ExitPlanMode") {
        return {ActivityKind::PlanReview, Summarize(input["plan"].str())};
    }
    if (name == "ToolSearch") {
        return {ActivityKind::Thinking, {}};
    }
    return {ActivityKind::Tool, HumanizeToolName(name)};
}

bool AnalyzeTranscript(const std::wstring& path, TranscriptSnapshot& out) {
    out = TranscriptSnapshot{};
    for (const size_t window : kTailWindows) {
        std::string data;
        uint64_t fileSize = 0;
        if (!ReadFileTail(path, window, data, fileSize)) {
            return false;
        }
        const bool truncated = data.size() < fileSize;
        const std::vector<std::string_view> lines = SplitLines(data, truncated);
        if (AnalyzeLines(lines, out)) {
            return true;
        }
        out = TranscriptSnapshot{};
        if (!truncated) {
            break;
        }
    }
    return false;
}

std::wstring ProjectSlug(std::wstring_view cwd) {
    std::wstring slug(cwd);
    for (wchar_t& c : slug) {
        const bool alphanumeric = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (!alphanumeric) {
            c = L'-';
        }
    }
    return slug;
}

}
