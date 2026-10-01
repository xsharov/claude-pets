#pragma once

#include "transcript.h"

#include <cstdint>
#include <string>

namespace cp {

enum class SessionStatus : uint8_t { Idle, Running, Waiting, Failed, Review };

struct SessionView {
    std::string sessionId;
    std::string title;
    std::wstring cwd;
    SessionStatus status = SessionStatus::Idle;
    Activity activity;
    std::string detail;
    std::string turnKey;
    int64_t updatedAtMs = 0;
    bool completedTurn = false;
    bool operator==(const SessionView&) const = default;
};

}
