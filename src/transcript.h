#pragma once

#include "json.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace cp {

enum class ActivityKind : uint8_t {
    None,
    Thinking,
    Writing,
    Retrying,
    Command,
    Read,
    Edit,
    Search,
    WebSearch,
    WebFetch,
    Agent,
    Todo,
    Skill,
    Tool,
    Question,
    PlanReview,
    Permission,
    Elicitation,
};

struct Activity {
    ActivityKind kind = ActivityKind::None;
    std::string detail;
    bool operator==(const Activity&) const = default;
};

enum class TurnPhase : uint8_t { Unknown, Running, Waiting, Completed, Interrupted, Failed };

struct TranscriptSnapshot {
    TurnPhase phase = TurnPhase::Unknown;
    Activity activity;
    std::string turnKey;
    int64_t turnStartedMs = 0;
    int64_t lastEntryMs = 0;
    int64_t completedMs = 0;
    std::string finalText;
    std::string lastAssistantUuid;
    std::string pendingToolUseId;
};

Activity ActivityForTool(std::string_view name, const Json& input);
bool AnalyzeTranscript(const std::wstring& path, TranscriptSnapshot& out);
std::wstring ProjectSlug(std::wstring_view cwd);

}
