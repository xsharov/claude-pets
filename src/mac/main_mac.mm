#include "app.h"
#include "diagnostics.h"
#include "hooks.h"
#include "mac/mac_util.h"
#include "util.h"
#include "version.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    @autoreleasepool {
        const std::vector<std::string> arguments(argv + 1, argv + argc);
        auto has = [&](const char* flag) { return std::find(arguments.begin(), arguments.end(), flag) != arguments.end(); };
        if (has(cp::Narrow(cp::kHookArgument).c_str())) {
            return cp::RunHookRelay();
        }
        if (has("--version")) {
            cp::WriteConsoleText(std::string("claude-pets ") + CP_VERSION_TEXT + "\n");
            return 0;
        }
        if (has("--install-hooks") || has("--uninstall-hooks")) {
            const bool install = has("--install-hooks");
            const bool ok = install ? cp::InstallHooks(cp::ExecutablePath()) : cp::UninstallHooks();
            cp::WriteConsoleText(std::string(ok ? "ok: " : "failed: ") + cp::Narrow(cp::ClaudeSettingsPath()) + "\n");
            return ok ? 0 : 1;
        }
        if (has("--diagnose")) {
            return cp::RunDiagnostics();
        }
        const bool startHidden = has("--hidden");
        if (cp::SignalRunningInstance()) {
            return 0;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        int exitCode = 0;
        {
            cp::App app;
            exitCode = app.Run(startHidden);
        }
        return exitCode;
    }
}
