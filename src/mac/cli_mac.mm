#include "diagnostics.h"
#include "hooks.h"

#include "mac/mac_util.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace cp {

namespace {

CFMessagePortRef OpenRemotePort() {
    CFStringRef name = CFStringCreateWithCString(nullptr, kInstancePortName, kCFStringEncodingUTF8);
    CFMessagePortRef port = CFMessagePortCreateRemote(nullptr, name);
    CFRelease(name);
    return port;
}

bool Send(CFMessagePortRef port, SInt32 message, const std::string& payload) {
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(payload.data()), static_cast<CFIndex>(payload.size()));
    const SInt32 status = CFMessagePortSendRequest(port, message, data, 2.0, 0.0, nullptr, nullptr);
    CFRelease(data);
    return status == kCFMessagePortSuccess;
}

}

void WriteConsoleText(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

bool ReadHookInput(std::string& payload, size_t limit) {
    char buffer[16384];
    for (;;) {
        const ssize_t received = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            break;
        }
        payload.append(buffer, static_cast<size_t>(received));
        if (payload.size() > limit) {
            return false;
        }
    }
    return true;
}

void DeliverHookEvent(const std::string& message) {
    if (message.size() > kPortMessageLimit) {
        return;
    }
    CFMessagePortRef port = OpenRemotePort();
    if (port == nullptr) {
        return;
    }
    Send(port, kPortMessageHook, message);
    CFMessagePortInvalidate(port);
    CFRelease(port);
}

bool SignalRunningInstance() {
    CFMessagePortRef port = OpenRemotePort();
    if (port == nullptr) {
        return false;
    }
    Send(port, kPortMessageShow, std::string());
    CFMessagePortInvalidate(port);
    CFRelease(port);
    return true;
}

}
