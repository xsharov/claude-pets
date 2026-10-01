#include "i18n.h"

#include "util.h"

#include <windows.h>

namespace cp {

namespace {

const Strings kEnglish = {
    L"Thinking",
    L"Writing a reply",
    L"Retrying after an API error",
    L"Running a command",
    L"Running: {}",
    L"Reading {}",
    L"Editing {}",
    L"Searching {}",
    L"Searching the web: {}",
    L"Fetching {}",
    L"Agent: {}",
    L"Updating the plan",
    L"Using skill {}",
    L"Calling {}",
    L"Has a question for you",
    L"Plan is ready for review",
    L"Waiting for permission",
    L"Needs permission: {}",
    L"Waiting for your input",
    L"Done",
    L"Needs your input",
    L"Stopped with an error",
    L"Hi, I'm {}",
    L"I'm here to help keep your Claude sessions moving",
    L"Pet",
    L"No pets found",
    L"Size",
    L"Small",
    L"Medium",
    L"Large",
    L"Huge",
    L"Show activity",
    L"Follow the cursor",
    L"Open Claude",
    L"Open pets folder",
    L"Start with Windows",
    L"Precise status (Claude Code hooks)",
    L"Language",
    L"Hide pet",
    L"Close pet",
    L"Open session",
    L"Dismiss",
    L"Mute this session",
    L"Unmute all sessions",
    L"Show pet",
    L"Claude Pets",
    L"Claude Pets",
    L"Claude Pets will add async hooks to ~/.claude/settings.json so the pet knows exactly when a session is waiting for your permission.\n\n"
    L"The hooks only forward events to this app and never block Claude.\n\nInstall them?",
    L"Remove Claude Pets hooks from ~/.claude/settings.json?",
    L"Could not update ~/.claude/settings.json.",
    L"This pet uses a WebP spritesheet, but the WebP codec is not installed.\nInstall \"WebP Image Extensions\" from Microsoft Store.",
    L"Could not load the pet spritesheet:\n{}",
};

const Strings kRussian = {
    L"Думает",
    L"Пишет ответ",
    L"Повтор после ошибки API",
    L"Выполняет команду",
    L"Выполняет: {}",
    L"Читает {}",
    L"Редактирует {}",
    L"Ищет {}",
    L"Ищет в сети: {}",
    L"Загружает {}",
    L"Агент: {}",
    L"Обновляет план",
    L"Использует скилл {}",
    L"Вызывает {}",
    L"Есть вопрос к вам",
    L"План готов к проверке",
    L"Ждёт разрешения",
    L"Нужно разрешение: {}",
    L"Ждёт вашего ввода",
    L"Готово",
    L"Нужно ваше участие",
    L"Остановлено с ошибкой",
    L"Привет, я {}",
    L"Я помогу следить за вашими сессиями Claude",
    L"Питомец",
    L"Питомцы не найдены",
    L"Размер",
    L"Маленький",
    L"Средний",
    L"Большой",
    L"Огромный",
    L"Показывать активность",
    L"Следить за курсором",
    L"Открыть Claude",
    L"Открыть папку питомцев",
    L"Запускать вместе с Windows",
    L"Точный статус (хуки Claude Code)",
    L"Язык",
    L"Спрятать питомца",
    L"Закрыть питомца",
    L"Открыть сессию",
    L"Скрыть уведомление",
    L"Не уведомлять об этой сессии",
    L"Снова уведомлять обо всех сессиях",
    L"Показать питомца",
    L"Claude Pets",
    L"Claude Pets",
    L"Claude Pets добавит асинхронные хуки в ~/.claude/settings.json, чтобы питомец точно знал, когда сессия ждёт вашего разрешения.\n\n"
    L"Хуки только пересылают события в это приложение и никогда не блокируют Claude.\n\nУстановить?",
    L"Удалить хуки Claude Pets из ~/.claude/settings.json?",
    L"Не удалось обновить ~/.claude/settings.json.",
    L"Спрайтшит питомца в формате WebP, но кодек WebP не установлен.\nУстановите «Расширения для изображений WebP» из Microsoft Store.",
    L"Не удалось загрузить спрайтшит питомца:\n{}",
};

Language g_language = Language::English;

}

void SetLanguage(Language language) {
    g_language = language;
}

Language CurrentLanguage() {
    return g_language;
}

Language SystemLanguage() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? Language::Russian : Language::English;
}

const Strings& Text() {
    return g_language == Language::Russian ? kRussian : kEnglish;
}

std::wstring Format(std::wstring_view pattern, std::wstring_view value) {
    std::wstring result(pattern);
    const size_t placeholder = result.find(L"{}");
    if (placeholder == std::wstring::npos) {
        return result;
    }
    if (!value.empty()) {
        result.replace(placeholder, 2, value);
        return result;
    }
    size_t start = placeholder;
    while (start > 0 && (result[start - 1] == L' ' || result[start - 1] == L':')) {
        --start;
    }
    result.erase(start, placeholder + 2 - start);
    return result;
}

std::wstring DescribeActivity(const Activity& activity) {
    const Strings& text = Text();
    const std::wstring detail = Widen(activity.detail);
    switch (activity.kind) {
    case ActivityKind::None:
    case ActivityKind::Thinking:
        return text.thinking;
    case ActivityKind::Writing:
        return detail.empty() ? std::wstring(text.writing) : detail;
    case ActivityKind::Retrying:
        return text.retrying;
    case ActivityKind::Command:
        return detail.empty() ? std::wstring(text.runningCommand) : Format(text.runningCommandDetail, detail);
    case ActivityKind::Read:
        return Format(text.reading, detail);
    case ActivityKind::Edit:
        return Format(text.editing, detail);
    case ActivityKind::Search:
        return Format(text.searching, detail);
    case ActivityKind::WebSearch:
        return Format(text.searchingWeb, detail);
    case ActivityKind::WebFetch:
        return Format(text.fetching, detail);
    case ActivityKind::Agent:
        return Format(text.agent, detail);
    case ActivityKind::Todo:
        return text.updatingPlan;
    case ActivityKind::Skill:
        return Format(text.usingSkill, detail);
    case ActivityKind::Tool:
        return Format(text.calling, detail);
    case ActivityKind::Question:
        return detail.empty() ? std::wstring(text.question) : detail;
    case ActivityKind::PlanReview:
        return text.planReview;
    case ActivityKind::Permission:
        return detail.empty() ? std::wstring(text.permission) : Format(text.permissionDetail, detail);
    case ActivityKind::Elicitation:
        return detail.empty() ? std::wstring(text.elicitation) : detail;
    }
    return text.thinking;
}

}
