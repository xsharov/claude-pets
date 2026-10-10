#pragma once

#include "geometry.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <string>

#ifdef __OBJC__
@class NSString;
#endif

namespace cp {

inline constexpr char kInstancePortName[] = "io.github.xsharov.claude-pets.port";
inline constexpr SInt32 kPortMessageHook = 1;
inline constexpr SInt32 kPortMessageShow = 2;
inline constexpr size_t kPortMessageLimit = size_t{16} << 20;

bool SignalRunningInstance();
void StartClaudeForegroundTracking();

double PrimaryScreenHeight();
CGRect ToCocoaRect(const RectI& rect);
RectI FromCocoaRect(CGRect rect);

#ifdef __OBJC__
NSString* ToNSString(const std::wstring& text);
std::wstring FromNSString(NSString* text);
#endif

}
