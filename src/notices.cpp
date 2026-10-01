#include "notices.h"

#include "i18n.h"
#include "util.h"

#include <algorithm>
#include <climits>

namespace cp {

namespace {

constexpr int64_t kFailedLifetimeMs = 60LL * 60 * 1000;
constexpr int64_t kWaitingLifetimeMs = 24LL * 60 * 60 * 1000;
constexpr int64_t kReviewLifetimeMs = 7LL * 24 * 60 * 60 * 1000;
constexpr int64_t kGreetingLifetimeMs = 8000;

int Priority(const Notice& notice) {
    if (notice.kind == NoticeKind::Greeting) {
        return 4;
    }
    switch (notice.status) {
    case SessionStatus::Waiting:
        return 0;
    case SessionStatus::Failed:
        return 1;
    case SessionStatus::Review:
        return 2;
    case SessionStatus::Running:
        return 3;
    default:
        return 4;
    }
}

std::wstring DetailOr(const std::string& detail, const wchar_t* fallback) {
    return detail.empty() ? std::wstring(fallback) : Widen(detail);
}

}

std::vector<Notice> BuildNotices(const std::vector<SessionView>& sessions, const Settings& settings, const std::optional<Greeting>& greeting, int64_t nowMs) {
    const Strings& text = Text();
    std::vector<Notice> notices;
    for (const SessionView& session : sessions) {
        if (settings.muted.count(session.sessionId) != 0 || IsDismissed(settings, session.sessionId, session.turnKey)) {
            continue;
        }
        Notice notice;
        notice.id = session.sessionId;
        notice.turnKey = session.turnKey;
        notice.status = session.status;
        notice.title = Widen(session.title);
        notice.updatedAtMs = session.updatedAtMs;
        notice.completedTurn = session.completedTurn;
        switch (session.status) {
        case SessionStatus::Running:
            notice.subtitle = DescribeActivity(session.activity);
            notice.loading = true;
            break;
        case SessionStatus::Waiting:
            notice.subtitle = session.activity.kind != ActivityKind::None ? DescribeActivity(session.activity) : DetailOr(session.detail, text.needsInputFallback);
            notice.icon = PillIcon::Warning;
            notice.expiresAtMs = session.updatedAtMs + kWaitingLifetimeMs;
            break;
        case SessionStatus::Failed:
            notice.subtitle = DetailOr(session.detail, text.failedFallback);
            notice.icon = PillIcon::Danger;
            notice.expiresAtMs = session.updatedAtMs + kFailedLifetimeMs;
            break;
        case SessionStatus::Review:
            notice.subtitle = DetailOr(session.detail, text.readyFallback);
            notice.icon = PillIcon::Check;
            notice.expiresAtMs = session.updatedAtMs + kReviewLifetimeMs;
            break;
        case SessionStatus::Idle:
            continue;
        }
        if (notice.expiresAtMs != 0 && nowMs >= notice.expiresAtMs) {
            continue;
        }
        notices.push_back(std::move(notice));
    }
    if (greeting.has_value() && nowMs < greeting->startedAtMs + kGreetingLifetimeMs) {
        Notice notice;
        notice.id = "greeting";
        notice.kind = NoticeKind::Greeting;
        notice.title = Format(text.greetingTitle, greeting->petName);
        notice.subtitle = text.greetingBody;
        notice.updatedAtMs = greeting->startedAtMs;
        notice.expiresAtMs = greeting->startedAtMs + kGreetingLifetimeMs;
        notices.push_back(std::move(notice));
    }
    std::sort(notices.begin(), notices.end(), [](const Notice& a, const Notice& b) {
        const int pa = Priority(a);
        const int pb = Priority(b);
        if (pa != pb) {
            return pa < pb;
        }
        if (a.updatedAtMs != b.updatedAtMs) {
            return a.updatedAtMs > b.updatedAtMs;
        }
        return a.id < b.id;
    });
    return notices;
}

PetAnim MascotAnimFor(const std::vector<Notice>& notices) {
    if (notices.empty()) {
        return PetAnim::Idle;
    }
    const Notice& top = notices.front();
    if (top.kind == NoticeKind::Greeting) {
        return PetAnim::Waving;
    }
    switch (top.status) {
    case SessionStatus::Running:
        return PetAnim::Running;
    case SessionStatus::Waiting:
        return PetAnim::Waiting;
    case SessionStatus::Failed:
        return PetAnim::Failed;
    case SessionStatus::Review:
        return PetAnim::Review;
    default:
        return PetAnim::Idle;
    }
}

int64_t NextExpiryMs(const std::vector<Notice>& notices) {
    int64_t next = LLONG_MAX;
    for (const Notice& notice : notices) {
        if (notice.expiresAtMs > 0) {
            next = std::min(next, notice.expiresAtMs);
        }
    }
    return next;
}

}
