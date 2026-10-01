#pragma once

#include "transcript.h"

#include <string>
#include <string_view>

namespace cp {

enum class Language : uint8_t { English, Russian };

struct Strings {
    const wchar_t* thinking;
    const wchar_t* writing;
    const wchar_t* retrying;
    const wchar_t* runningCommand;
    const wchar_t* runningCommandDetail;
    const wchar_t* reading;
    const wchar_t* editing;
    const wchar_t* searching;
    const wchar_t* searchingWeb;
    const wchar_t* fetching;
    const wchar_t* agent;
    const wchar_t* updatingPlan;
    const wchar_t* usingSkill;
    const wchar_t* calling;
    const wchar_t* question;
    const wchar_t* planReview;
    const wchar_t* permission;
    const wchar_t* permissionDetail;
    const wchar_t* elicitation;
    const wchar_t* readyFallback;
    const wchar_t* needsInputFallback;
    const wchar_t* failedFallback;
    const wchar_t* greetingTitle;
    const wchar_t* greetingBody;
    const wchar_t* menuPet;
    const wchar_t* menuNoPets;
    const wchar_t* menuSize;
    const wchar_t* sizeSmall;
    const wchar_t* sizeMedium;
    const wchar_t* sizeLarge;
    const wchar_t* sizeHuge;
    const wchar_t* menuShowActivity;
    const wchar_t* menuLookAtCursor;
    const wchar_t* menuOpenClaude;
    const wchar_t* menuOpenPetsFolder;
    const wchar_t* menuPetsFolder;
    const wchar_t* menuStartWithWindows;
    const wchar_t* menuHooks;
    const wchar_t* menuLanguage;
    const wchar_t* menuHide;
    const wchar_t* menuClose;
    const wchar_t* menuOpenSession;
    const wchar_t* menuDismiss;
    const wchar_t* menuMute;
    const wchar_t* menuUnmuteAll;
    const wchar_t* trayShow;
    const wchar_t* trayTip;
    const wchar_t* hooksInstallTitle;
    const wchar_t* hooksInstallBody;
    const wchar_t* hooksRemoveBody;
    const wchar_t* hooksFailed;
    const wchar_t* codecMissing;
    const wchar_t* spriteFailed;
};

void SetLanguage(Language language);
Language CurrentLanguage();
Language SystemLanguage();
const Strings& Text();
std::wstring Format(std::wstring_view pattern, std::wstring_view value);
std::wstring DescribeActivity(const Activity& activity);

}
