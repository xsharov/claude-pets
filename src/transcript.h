#pragma once

#include "json.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
    BackgroundAgent,
    Reviewing,
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

struct BackgroundAgent {
    std::string id;
    std::string toolUseId;
    std::string type;
    std::string description;
    std::string workflowRunId;
    int64_t startedMs = 0;
    bool operator==(const BackgroundAgent&) const = default;
};

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
    std::vector<BackgroundAgent> backgroundAgents;
};

Activity ActivityForTool(std::string_view name, const Json& input);
bool AnalyzeTranscript(const std::wstring& path, TranscriptSnapshot& out);
std::wstring ProjectSlug(std::wstring_view cwd);

}
