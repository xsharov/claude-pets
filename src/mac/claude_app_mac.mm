#include "claude_app.h"

#include "log.h"
#include "mac/mac_util.h"
#include "util.h"

#import <AppKit/AppKit.h>

#include <libproc.h>
#include <sys/proc_info.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace cp {

namespace {

NSString* const kClaudeBundleId = @"com.anthropic.claudefordesktop";
constexpr char kClaudeExecutable[] = "Claude";
constexpr char kExecutableSuffix[] = ".app/Contents/MacOS/Claude";

std::atomic<pid_t> g_claudeActivePid{-1};
id g_activationObserver = nil;

bool IsClaudeBundle(NSRunningApplication* application) {
    return application != nil && [application.bundleIdentifier isEqualToString:kClaudeBundleId];
}

pid_t ClaudePidIfActive(NSRunningApplication* application) {
    return IsClaudeBundle(application) ? application.processIdentifier : 0;
}

bool HasOnScreenWindow(pid_t pid) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (windows == nullptr) {
        return false;
    }
    bool found = false;
    for (CFIndex i = 0; i < CFArrayGetCount(windows) && !found; ++i) {
        CFDictionaryRef window = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, i));
        int owner = 0;
        int layer = -1;
        CFNumberRef ownerNumber = static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowOwnerPID));
        CFNumberRef layerNumber = static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowLayer));
        if (ownerNumber != nullptr && layerNumber != nullptr && CFNumberGetValue(ownerNumber, kCFNumberIntType, &owner) &&
            CFNumberGetValue(layerNumber, kCFNumberIntType, &layer)) {
            found = owner == pid && layer == 0;
        }
    }
    CFRelease(windows);
    return found;
}

bool IsClaudeDesktopPath(const std::string& path) {
    static std::mutex cacheLock;
    static std::unordered_map<std::string, bool> cache;
    const size_t suffix = std::strlen(kExecutableSuffix);
    if (path.size() <= suffix || path.compare(path.size() - suffix, suffix, kExecutableSuffix) != 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(cacheLock);
        const auto found = cache.find(path);
        if (found != cache.end()) {
            return found->second;
        }
    }
    bool matches = false;
    @autoreleasepool {
        NSString* bundlePath = [[NSString stringWithUTF8String:path.c_str()] stringByDeletingLastPathComponent];
        bundlePath = [[bundlePath stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
        NSBundle* bundle = [NSBundle bundleWithPath:bundlePath];
        matches = bundle != nil && [bundle.bundleIdentifier isEqualToString:kClaudeBundleId];
    }
    std::lock_guard<std::mutex> lock(cacheLock);
    if (cache.size() > 64) {
        cache.clear();
    }
    cache[path] = matches;
    return matches;
}

NSURL* ClaudeApplicationUrl() {
    return [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:kClaudeBundleId];
}

bool OpenUrl(NSString* text) {
    NSURL* url = [NSURL URLWithString:text];
    return url != nil && [[NSWorkspace sharedWorkspace] openURL:url];
}

bool OpenClaudeApplication() {
    NSURL* application = ClaudeApplicationUrl();
    if (application == nil) {
        return false;
    }
    NSWorkspaceOpenConfiguration* configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:application configuration:configuration completionHandler:nil];
    return true;
}

}

void StartClaudeForegroundTracking() {
    if (g_activationObserver != nil) {
        return;
    }
    g_claudeActivePid = ClaudePidIfActive([NSWorkspace sharedWorkspace].frontmostApplication);
    g_activationObserver = [[NSWorkspace sharedWorkspace].notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification
                                                                                        object:nil
                                                                                         queue:[NSOperationQueue mainQueue]
                                                                                    usingBlock:^(NSNotification* notification) {
                                                                                        NSRunningApplication* application = notification.userInfo[NSWorkspaceApplicationKey];
                                                                                        g_claudeActivePid = ClaudePidIfActive(application);
                                                                                    }];
}

std::vector<std::wstring> ClaudeDesktopDataDirectories() {
    return {JoinPath(JoinPath(JoinPath(HomeDirectory(), L"Library"), L"Application Support"), L"Claude")};
}

int64_t ClaudeDesktopStartedMs() {
    const int capacity = proc_listallpids(nullptr, 0);
    if (capacity <= 0) {
        return 0;
    }
    std::vector<pid_t> pids(static_cast<size_t>(capacity) + 64);
    const int count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    int64_t earliest = 0;
    for (int i = 0; i < count && i < static_cast<int>(pids.size()); ++i) {
        const pid_t pid = pids[static_cast<size_t>(i)];
        char name[2 * MAXCOMLEN + 1] = {};
        if (pid <= 0 || proc_name(pid, name, sizeof(name)) <= 0 || std::strcmp(name, kClaudeExecutable) != 0) {
            continue;
        }
        char path[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (proc_pidpath(pid, path, sizeof(path)) <= 0 || !IsClaudeDesktopPath(path)) {
            continue;
        }
        proc_bsdinfo info = {};
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != static_cast<int>(sizeof(info))) {
            continue;
        }
        const int64_t started = static_cast<int64_t>(info.pbi_start_tvsec) * 1000 + static_cast<int64_t>(info.pbi_start_tvusec) / 1000;
        if (started > 0 && (earliest == 0 || started < earliest)) {
            earliest = started;
        }
    }
    return earliest;
}

bool IsClaudeDesktopRunning() {
    return ClaudeDesktopStartedMs() != 0;
}

bool IsClaudeForeground() {
    pid_t pid = g_claudeActivePid.load();
    if (pid < 0) {
        @autoreleasepool {
            pid = ClaudePidIfActive([NSWorkspace sharedWorkspace].frontmostApplication);
        }
    }
    return pid > 0 && HasOnScreenWindow(pid);
}

bool OpenClaudeSession(const std::string& sessionId) {
    NSString* identifier = [[NSString stringWithUTF8String:sessionId.c_str()] stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet];
    if (identifier == nil) {
        return false;
    }
    const bool opened = OpenUrl([@"claude://claude.ai/epitaxy/" stringByAppendingString:identifier]);
    LogLine("open session " + sessionId + " result=" + std::to_string(opened));
    return opened;
}

void BringClaudeToFront() {
    if (!IsClaudeDesktopRunning()) {
        return;
    }
    OpenClaudeApplication();
}

bool ActivateClaude() {
    if (OpenClaudeApplication()) {
        return true;
    }
    return OpenUrl(@"claude://code/continue?session=last");
}

}
