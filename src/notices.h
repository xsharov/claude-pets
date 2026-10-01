#pragma once

#include "animation.h"
#include "renderer.h"
#include "session_model.h"
#include "settings.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cp {

enum class NoticeKind : uint8_t { Session, Greeting };

struct Notice {
    std::string id;
    std::string turnKey;
    NoticeKind kind = NoticeKind::Session;
    SessionStatus status = SessionStatus::Idle;
    std::wstring title;
    std::wstring subtitle;
    PillIcon icon = PillIcon::None;
    bool loading = false;
    bool completedTurn = false;
    int64_t updatedAtMs = 0;
    int64_t expiresAtMs = 0;
    bool operator==(const Notice&) const = default;
};

struct Greeting {
    std::wstring petName;
    int64_t startedAtMs = 0;
};

std::vector<Notice> BuildNotices(const std::vector<SessionView>& sessions, const Settings& settings, const std::optional<Greeting>& greeting, int64_t nowMs);
PetAnim MascotAnimFor(const std::vector<Notice>& notices);
int64_t NextExpiryMs(const std::vector<Notice>& notices);

}
