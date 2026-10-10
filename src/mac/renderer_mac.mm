#include "renderer.h"

#include "util.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace cp {

namespace {

constexpr float kTitleSize = 13.0f;
constexpr float kSubtitleSize = 13.0f;
constexpr float kBadgeTextSize = 11.5f;
constexpr float kPillPaddingX = 16.0f;
constexpr float kPillPaddingY = 12.0f;
constexpr float kLineHeight = 16.0f;
constexpr float kBaseline = kLineHeight * 0.8f;
constexpr float kIconSize = 14.0f;
constexpr float kIconGap = 5.0f;
constexpr int kShadowLayers = 5;
constexpr float kShadowWeights[kShadowLayers] = {0.075f, 0.05f, 0.032f, 0.018f, 0.008f};
constexpr size_t kImageCacheLimit = 192;

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct Palette {
    Color material;
    Color border;
    Color shadow;
    Color text;
    Color secondary;
    Color shimmer;
    Color green;
    Color orange;
    Color red;
    Color hoverOverlay;
};

Color Rgba(uint32_t rgb, float alpha) {
    return Color{((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
}

Palette PaletteFor(bool dark) {
    if (dark) {
        return {Rgba(0x202020, 0.94f), Rgba(0xFFFFFF, 0.13f), Rgba(0x000000, 1.0f), Rgba(0xF3F3F3, 1.0f), Rgba(0xAFAFAF, 1.0f),
                Rgba(0x5C5C5C, 1.0f), Rgba(0x30D158, 1.0f), Rgba(0xFF9230, 1.0f), Rgba(0xFF453A, 1.0f), Rgba(0xFFFFFF, 0.06f)};
    }
    return {Rgba(0xFFFFFF, 0.94f), Rgba(0x000000, 0.10f), Rgba(0x000000, 1.0f), Rgba(0x0D0D0D, 1.0f), Rgba(0x5D5D5D, 1.0f),
            Rgba(0xD2D2D2, 1.0f), Rgba(0x34C759, 1.0f), Rgba(0xFF8D28, 1.0f), Rgba(0xFF3B30, 1.0f), Rgba(0x000000, 0.04f)};
}

id MakeColor(const Color& color, float factor = 1.0f) {
    return (__bridge_transfer id)CGColorCreateSRGB(color.r, color.g, color.b, color.a * factor);
}

CGRect ToCG(const RectF& rect) {
    return CGRectMake(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

RectF ComputeContentBounds(const uint8_t* data, size_t stride, size_t width, size_t height, int rows, float cellWidth, float cellHeight) {
    const RectF full = {0.0f, 0.0f, 1.0f, 1.0f};
    const int cellW = static_cast<int>(cellWidth);
    const int cellH = static_cast<int>(cellHeight);
    if (data == nullptr || cellW <= 0 || cellH <= 0) {
        return full;
    }
    int minX = cellW;
    int minY = cellH;
    int maxX = -1;
    int maxY = -1;
    const size_t usedHeight = std::min<size_t>(height, static_cast<size_t>(rows * cellH));
    for (size_t y = 0; y < usedHeight; ++y) {
        const uint8_t* row = data + y * stride;
        const int cellY = static_cast<int>(y) % cellH;
        for (size_t x = 0; x < width; ++x) {
            if (row[x * 4 + 3] <= 24) {
                continue;
            }
            const int cellX = static_cast<int>(x) % cellW;
            minX = std::min(minX, cellX);
            maxX = std::max(maxX, cellX);
            minY = std::min(minY, cellY);
            maxY = std::max(maxY, cellY);
        }
    }
    if (maxX < minX || maxY < minY) {
        return full;
    }
    return RectF{static_cast<float>(minX) / cellW, static_cast<float>(minY) / cellH, static_cast<float>(maxX + 1) / cellW, static_cast<float>(maxY + 1) / cellH};
}

CALayer* NewLayer(CALayer* parent) {
    CALayer* layer = [CALayer layer];
    layer.anchorPoint = CGPointZero;
    layer.contentsGravity = kCAGravityResize;
    if (parent != nil) {
        [parent addSublayer:layer];
    }
    return layer;
}

void Place(CALayer* layer, CGRect frame) {
    layer.bounds = CGRectMake(0, 0, frame.size.width, frame.size.height);
    layer.position = frame.origin;
}

CGFloat Snap(CGFloat value, float scale) {
    return std::round(value * scale) / scale;
}

struct MaterialLayers {
    CALayer* shadows[kShadowLayers] = {};
    CALayer* fill = nil;
    CALayer* hover = nil;
};

struct TextImage {
    id image = nil;
    float width = 0.0f;
    float height = 0.0f;
};

struct PillLayers {
    CALayer* container = nil;
    MaterialLayers material;
    CALayer* icon = nil;
    CALayer* title = nil;
    CALayer* subtitle = nil;
    CALayer* shimmerHost = nil;
    CALayer* shimmerMask = nil;
    CALayer* shimmerBase = nil;
    CAGradientLayer* shimmerBand = nil;
    CALayer* dismiss = nil;
    MaterialLayers dismissMaterial;
    CALayer* cross = nil;
    int version = -1;
    float width = -1.0f;
    float height = -1.0f;
    bool hovered = false;
    bool dismissHovered = false;
    bool showDismiss = false;
    bool contentVisible = false;
    bool loading = false;
    PillIcon iconKind = PillIcon::None;
    std::wstring titleText;
    std::wstring subtitleText;
    float subtitleWidth = 0.0f;
};

struct BadgeLayers {
    CALayer* container = nil;
    MaterialLayers material;
    CALayer* content = nil;
    int version = -1;
    float size = -1.0f;
    bool hovered = false;
    std::wstring text;
    bool chevronDown = false;
    bool chevronUp = false;
    SessionStatus tone = SessionStatus::Idle;
};

void CreateMaterial(MaterialLayers& material, CALayer* parent) {
    for (int i = kShadowLayers; i >= 1; --i) {
        material.shadows[i - 1] = NewLayer(parent);
    }
    material.fill = NewLayer(parent);
    material.fill.borderWidth = 1.0;
    material.hover = NewLayer(material.fill);
}

}

struct Renderer::Scene {
    void SetTheme(bool dark, float scale);
    void ConfigureMaterial(MaterialLayers& material, CGRect rect, float radius, bool hovered);
    TextImage Text(const std::wstring& text, CTFontRef font, float maxWidth, const Color& color, wchar_t tag);
    id Drawing(const std::wstring& key, CGSize size, void (^draw)(CGContextRef));
    void Remember(const std::wstring& key, const TextImage& image);

    CALayer* root = nil;
    std::vector<PillLayers> pills;
    size_t pillsUsed = 0;
    CALayer* pet = nil;
    BadgeLayers badge;
    NSMutableArray* order = [NSMutableArray array];
    NSArray* applied = @[];
    float scale = 2.0f;
    bool dark = false;
    int version = 0;
    Palette palette = PaletteFor(false);
    id materialColor = nil;
    id borderColor = nil;
    id hoverColor = nil;
    id secondaryColor = nil;
    id shimmerColor = nil;
    id shadowColors[kShadowLayers] = {};
    id titleFont = nil;
    id subtitleFont = nil;
    id badgeFont = nil;
    std::unordered_map<std::wstring, TextImage> images;
};

void Renderer::Scene::SetTheme(bool darkTheme, float pixelScale) {
    if (version > 0 && darkTheme == dark && pixelScale == scale) {
        return;
    }
    dark = darkTheme;
    scale = pixelScale;
    ++version;
    palette = PaletteFor(dark);
    materialColor = MakeColor(palette.material);
    borderColor = MakeColor(palette.border);
    hoverColor = MakeColor(palette.hoverOverlay);
    secondaryColor = MakeColor(palette.secondary);
    shimmerColor = MakeColor(palette.shimmer);
    const float shadowScale = dark ? 1.8f : 1.0f;
    for (int i = 0; i < kShadowLayers; ++i) {
        shadowColors[i] = MakeColor(palette.shadow, kShadowWeights[i] * shadowScale);
    }
    images.clear();
}

void Renderer::Scene::ConfigureMaterial(MaterialLayers& material, CGRect rect, float radius, bool hovered) {
    const CGFloat limit = std::min(rect.size.width, rect.size.height) * 0.5;
    for (int i = 1; i <= kShadowLayers; ++i) {
        const CGFloat spread = i * 0.9;
        CALayer* shadow = material.shadows[i - 1];
        const CGRect frame = CGRectMake(rect.origin.x - spread, rect.origin.y - spread + 1.5, rect.size.width + spread * 2.0, rect.size.height + spread * 2.0 + 0.5);
        Place(shadow, frame);
        shadow.cornerRadius = std::min<CGFloat>(radius + spread, std::min(frame.size.width, frame.size.height) * 0.5);
        shadow.backgroundColor = (__bridge CGColorRef)shadowColors[i - 1];
    }
    Place(material.fill, rect);
    material.fill.cornerRadius = std::min<CGFloat>(radius, limit);
    material.fill.backgroundColor = (__bridge CGColorRef)materialColor;
    material.fill.borderColor = (__bridge CGColorRef)borderColor;
    Place(material.hover, CGRectMake(0, 0, rect.size.width, rect.size.height));
    material.hover.cornerRadius = material.fill.cornerRadius;
    material.hover.backgroundColor = (__bridge CGColorRef)hoverColor;
    material.hover.hidden = !hovered;
}

void Renderer::Scene::Remember(const std::wstring& key, const TextImage& image) {
    if (images.size() >= kImageCacheLimit) {
        images.clear();
    }
    images[key] = image;
}

TextImage Renderer::Scene::Text(const std::wstring& text, CTFontRef font, float maxWidth, const Color& color, wchar_t tag) {
    std::wstring key = text;
    key.push_back(L'\x1f');
    key.append(std::to_wstring(static_cast<int>(std::lround(maxWidth * 4.0f))));
    key.push_back(tag);
    const auto found = images.find(key);
    if (found != images.end()) {
        return found->second;
    }
    TextImage result;
    @autoreleasepool {
        NSDictionary* attributes = @{
            static_cast<NSString*>(kCTFontAttributeName) : (__bridge id)font,
            static_cast<NSString*>(kCTForegroundColorFromContextAttributeName) : @YES,
        };
        const std::string utf8 = Narrow(text);
        NSString* string = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
        NSAttributedString* attributed = [[NSAttributedString alloc] initWithString:string != nil ? string : @"" attributes:attributes];
        CTLineRef line = CTLineCreateWithAttributedString(static_cast<CFAttributedStringRef>(attributed));
        const double available = std::max(1.0f, maxWidth);
        double width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
        if (width > available + 0.01) {
            NSAttributedString* ellipsis = [[NSAttributedString alloc] initWithString:@"…" attributes:attributes];
            CTLineRef token = CTLineCreateWithAttributedString(static_cast<CFAttributedStringRef>(ellipsis));
            CTLineRef truncated = CTLineCreateTruncatedLine(line, available, kCTLineTruncationEnd, token);
            CFRelease(token);
            if (truncated != nullptr) {
                CFRelease(line);
                line = truncated;
                width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
            }
        }
        const bool badgeText = tag == L'g' || tag == L'G' || tag == L'o' || tag == L'r' || tag == L'v';
        const CGFloat ascent = CTFontGetAscent(font);
        const CGFloat descent = CTFontGetDescent(font);
        const CGFloat boxHeight = badgeText ? std::ceil(ascent + descent) : kLineHeight;
        const CGFloat baselineFromTop = badgeText ? ascent + (boxHeight - ascent - descent) * 0.5 : kBaseline;
        const size_t pixelWidth = static_cast<size_t>(std::max<double>(1.0, std::ceil(width * scale)));
        const size_t pixelHeight = static_cast<size_t>(std::ceil(boxHeight * scale));
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(nullptr, pixelWidth, pixelHeight, 8, 0, space,
                                                     static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
        CGColorSpaceRelease(space);
        if (context != nullptr) {
            CGContextScaleCTM(context, scale, scale);
            CGContextSetRGBFillColor(context, color.r, color.g, color.b, color.a);
            CGContextSetShouldSmoothFonts(context, false);
            CGContextSetTextMatrix(context, CGAffineTransformIdentity);
            CGContextSetTextPosition(context, 0, boxHeight - baselineFromTop);
            CTLineDraw(line, context);
            result.image = (__bridge_transfer id)CGBitmapContextCreateImage(context);
            CGContextRelease(context);
        }
        CFRelease(line);
        result.width = static_cast<float>(static_cast<double>(pixelWidth) / scale);
        result.height = static_cast<float>(static_cast<double>(pixelHeight) / scale);
    }
    Remember(key, result);
    return result;
}

id Renderer::Scene::Drawing(const std::wstring& key, CGSize size, void (^draw)(CGContextRef)) {
    const auto found = images.find(key);
    if (found != images.end()) {
        return found->second.image;
    }
    TextImage result;
    const size_t pixelWidth = static_cast<size_t>(std::max<double>(1.0, std::ceil(size.width * scale)));
    const size_t pixelHeight = static_cast<size_t>(std::max<double>(1.0, std::ceil(size.height * scale)));
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(nullptr, pixelWidth, pixelHeight, 8, 0, space,
                                                 static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(space);
    if (context != nullptr) {
        CGContextTranslateCTM(context, 0, pixelHeight);
        CGContextScaleCTM(context, scale, -scale);
        draw(context);
        result.image = (__bridge_transfer id)CGBitmapContextCreateImage(context);
        CGContextRelease(context);
    }
    result.width = static_cast<float>(size.width);
    result.height = static_cast<float>(size.height);
    Remember(key, result);
    return result.image;
}

Renderer::Renderer() = default;

Renderer::~Renderer() {
    UnloadSpritesheet();
    if (colorSpace_ != nullptr) {
        CGColorSpaceRelease(colorSpace_);
        colorSpace_ = nullptr;
    }
}

bool Renderer::Initialize() {
    colorSpace_ = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    scene_ = std::make_unique<Scene>();
    scene_->titleFont = [NSFont systemFontOfSize:kTitleSize weight:NSFontWeightBold];
    scene_->subtitleFont = [NSFont systemFontOfSize:kSubtitleSize weight:NSFontWeightRegular];
    scene_->badgeFont = [NSFont systemFontOfSize:kBadgeTextSize weight:NSFontWeightSemibold];
    return colorSpace_ != nullptr && scene_->titleFont != nil && scene_->subtitleFont != nil && scene_->badgeFont != nil;
}

void Renderer::AttachLayer(void* layer) {
    if (scene_ == nullptr) {
        return;
    }
    scene_->root = (__bridge CALayer*)layer;
    scene_->applied = @[];
}

bool Renderer::LoadSpritesheet(const std::wstring& path, int declaredVersion, std::wstring& error) {
    UnloadSpritesheet();
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:Narrow(path).c_str()]];
        CGImageSourceRef source = url != nil ? CGImageSourceCreateWithURL(static_cast<CFURLRef>(url), nullptr) : nullptr;
        if (source == nullptr) {
            error = L"open";
            return false;
        }
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
        CFRelease(source);
        if (image == nullptr) {
            error = L"decode";
            return false;
        }
        const size_t width = CGImageGetWidth(image);
        const size_t height = CGImageGetHeight(image);
        if (width < static_cast<size_t>(kSpriteColumns) || height == 0) {
            CGImageRelease(image);
            error = L"size";
            return false;
        }
        cellWidth_ = static_cast<float>(width) / kSpriteColumns;
        const float expectedCell = cellWidth_ / kCellAspect;
        const int rowsFromImage = static_cast<int>(std::lround(static_cast<float>(height) / expectedCell));
        spriteRows_ = rowsFromImage >= kSpriteRowsV2 || (declaredVersion >= 2 && rowsFromImage > kSpriteRowsV1) ? kSpriteRowsV2 : kSpriteRowsV1;
        cellHeight_ = static_cast<float>(height) / spriteRows_;
        CGContextRef bitmap = CGBitmapContextCreate(nullptr, width, height, 8, 0, colorSpace_,
                                                    static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
        if (bitmap == nullptr) {
            CGImageRelease(image);
            error = L"decode";
            return false;
        }
        CGContextSetBlendMode(bitmap, kCGBlendModeCopy);
        CGContextDrawImage(bitmap, CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)), image);
        CGImageRelease(image);
        contentBounds_ = ComputeContentBounds(static_cast<const uint8_t*>(CGBitmapContextGetData(bitmap)), CGBitmapContextGetBytesPerRow(bitmap), width, height,
                                              spriteRows_, cellWidth_, cellHeight_);
        sheet_ = CGBitmapContextCreateImage(bitmap);
        CGContextRelease(bitmap);
        if (sheet_ == nullptr) {
            error = L"decode";
            UnloadSpritesheet();
            return false;
        }
        cells_.assign(static_cast<size_t>(spriteRows_ * kSpriteColumns), nullptr);
    }
    return true;
}

void Renderer::UnloadSpritesheet() {
    for (CGImageRef cell : cells_) {
        if (cell != nullptr) {
            CGImageRelease(cell);
        }
    }
    cells_.clear();
    if (sheet_ != nullptr) {
        CGImageRelease(sheet_);
        sheet_ = nullptr;
    }
    contentBounds_ = RectF{0.0f, 0.0f, 1.0f, 1.0f};
    spriteRows_ = kSpriteRowsV1;
}

bool Renderer::HasSprite() const {
    return sheet_ != nullptr;
}

bool Renderer::BeginFrame(int widthPx, int heightPx, float scale, const RectI*) {
    if (scene_ == nullptr || scene_->root == nil || widthPx <= 0 || heightPx <= 0) {
        return false;
    }
    frameWidth_ = widthPx;
    frameHeight_ = heightPx;
    scale_ = scale;
    scene_->SetTheme(dark_, scale);
    scene_->pillsUsed = 0;
    [scene_->order removeAllObjects];
    drawing_ = true;
    return true;
}

bool Renderer::EndFrame() {
    if (!drawing_) {
        return false;
    }
    drawing_ = false;
    Scene& scene = *scene_;
    bool same = scene.order.count == scene.applied.count;
    for (NSUInteger i = 0; same && i < scene.order.count; ++i) {
        same = scene.order[i] == scene.applied[i];
    }
    if (!same) {
        scene.root.sublayers = [scene.order copy];
        scene.applied = [scene.order copy];
    }
    return true;
}

void Renderer::FillHitArea(const RectF&) {}

RectF Renderer::DismissRect(const RectF& pill) {
    return RectF{pill.left - 4.0f, pill.top - 4.0f, pill.left + 16.0f, pill.top + 16.0f};
}

void Renderer::DrawPet(const RectF& rect, const SpriteFrame& frame, float opacity) {
    if (sheet_ == nullptr) {
        DrawPetPlaceholder(rect);
        return;
    }
    if (frame.row < 0 || frame.column < 0 || frame.column >= kSpriteColumns || frame.row >= spriteRows_) {
        return;
    }
    CGImageRef& cell = cells_[static_cast<size_t>(frame.row * kSpriteColumns + frame.column)];
    if (cell == nullptr) {
        const CGFloat left = std::round(frame.column * cellWidth_);
        const CGFloat top = std::round(frame.row * cellHeight_);
        cell = CGImageCreateWithImageInRect(sheet_, CGRectMake(left, top, std::round((frame.column + 1) * cellWidth_) - left, std::round((frame.row + 1) * cellHeight_) - top));
        if (cell == nullptr) {
            return;
        }
    }
    Scene& scene = *scene_;
    if (scene.pet == nil) {
        scene.pet = NewLayer(nil);
    }
    const float leftPx = std::round(rect.left * scale_);
    const float topPx = std::round(rect.top * scale_);
    const float widthPx = std::round((rect.right - rect.left) * scale_);
    const float heightPx = std::round((rect.bottom - rect.top) * scale_);
    const bool upscale = widthPx >= cellWidth_ - 0.5f;
    Place(scene.pet, CGRectMake(leftPx / scale_, topPx / scale_, widthPx / scale_, heightPx / scale_));
    scene.pet.contents = (__bridge id)cell;
    scene.pet.magnificationFilter = upscale ? kCAFilterNearest : kCAFilterLinear;
    scene.pet.minificationFilter = upscale ? kCAFilterNearest : kCAFilterTrilinear;
    scene.pet.opacity = opacity;
    [scene.order addObject:scene.pet];
}

void Renderer::DrawPetPlaceholder(const RectF& rect) {
    Scene& scene = *scene_;
    if (scene.pet == nil) {
        scene.pet = NewLayer(nil);
    }
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const std::wstring key = L"placeholder" + std::to_wstring(static_cast<int>(width * 4)) + L"x" + std::to_wstring(static_cast<int>(height * 4));
    id image = scene.Drawing(key, CGSizeMake(width, height), ^(CGContextRef context) {
        const CGRect body = CGRectMake(width * 0.18f, height * 0.38f, width * 0.64f, height * 0.5f);
        CGPathRef path = CGPathCreateWithRoundedRect(body, std::min<CGFloat>(width * 0.18f, body.size.height * 0.5), std::min<CGFloat>(width * 0.18f, body.size.height * 0.5), nullptr);
        CGContextSetRGBFillColor(context, 0xD9 / 255.0, 0x77 / 255.0, 0x57 / 255.0, 1.0);
        CGContextAddPath(context, path);
        CGContextFillPath(context);
        CGPathRelease(path);
        const CGFloat eye = width * 0.06f;
        const CGFloat eyeY = body.origin.y + body.size.height * 0.4f;
        CGContextSetRGBFillColor(context, 0x1F / 255.0, 0x1F / 255.0, 0x1F / 255.0, 1.0);
        for (const CGFloat x : {width * 0.38f, width * 0.62f}) {
            CGContextFillEllipseInRect(context, CGRectMake(x - eye, eyeY - eye * 1.4f, eye * 2.0f, eye * 2.8f));
        }
    });
    Place(scene.pet, ToCG(rect));
    scene.pet.contents = image;
    scene.pet.magnificationFilter = kCAFilterLinear;
    scene.pet.minificationFilter = kCAFilterLinear;
    scene.pet.opacity = 1.0f;
    [scene.order addObject:scene.pet];
}

void Renderer::DrawPill(const PillVisual& pill) {
    if (pill.opacity <= 0.01f) {
        return;
    }
    Scene& scene = *scene_;
    if (scene.pillsUsed == scene.pills.size()) {
        PillLayers created;
        created.container = NewLayer(nil);
        CreateMaterial(created.material, created.container);
        created.icon = NewLayer(created.container);
        created.title = NewLayer(created.container);
        created.subtitle = NewLayer(created.container);
        created.shimmerHost = NewLayer(created.container);
        created.shimmerBase = NewLayer(created.shimmerHost);
        created.shimmerBand = [CAGradientLayer layer];
        created.shimmerBand.anchorPoint = CGPointZero;
        created.shimmerBand.startPoint = CGPointMake(0.0, 0.5);
        created.shimmerBand.endPoint = CGPointMake(1.0, 0.5);
        [created.shimmerHost addSublayer:created.shimmerBand];
        created.shimmerMask = NewLayer(nil);
        created.shimmerHost.mask = created.shimmerMask;
        created.dismiss = NewLayer(created.container);
        CreateMaterial(created.dismissMaterial, created.dismiss);
        created.cross = NewLayer(created.dismiss);
        scene.pills.push_back(std::move(created));
    }
    PillLayers& layers = scene.pills[scene.pillsUsed++];
    const float width = pill.rect.right - pill.rect.left;
    const float height = pill.rect.bottom - pill.rect.top;
    Place(layers.container, ToCG(pill.rect));
    layers.container.opacity = std::clamp(pill.opacity, 0.0f, 1.0f);
    const bool reshaped = layers.version != scene.version || layers.width != width || layers.height != height;
    if (reshaped || layers.hovered != pill.hovered) {
        scene.ConfigureMaterial(layers.material, CGRectMake(0, 0, width, height), std::min(28.0f, height * 0.5f), pill.hovered);
        layers.hovered = pill.hovered;
    }
    const float left = kPillPaddingX;
    const float right = width - kPillPaddingX;
    const float top = kPillPaddingY;
    const bool contentChanged = reshaped || layers.contentVisible != pill.contentVisible || layers.iconKind != pill.icon || layers.titleText != pill.title ||
                                layers.subtitleText != pill.subtitle || layers.loading != pill.loading;
    if (contentChanged) {
        layers.contentVisible = pill.contentVisible;
        layers.iconKind = pill.icon;
        layers.titleText = pill.title;
        layers.subtitleText = pill.subtitle;
        layers.loading = pill.loading;
        float titleLeft = left;
        const bool hasIcon = pill.contentVisible && pill.icon != PillIcon::None;
        layers.icon.hidden = !hasIcon;
        if (hasIcon) {
            const PillIcon icon = pill.icon;
            const Palette palette = scene.palette;
            const std::wstring key = L"icon" + std::to_wstring(static_cast<int>(icon));
            layers.icon.contents = scene.Drawing(key, CGSizeMake(kIconSize, kIconSize), ^(CGContextRef context) {
                const CGFloat size = kIconSize;
                auto at = [&](CGFloat x, CGFloat y) { return CGPointMake(x * size, y * size); };
                auto fill = [&](const Color& color) { CGContextSetRGBFillColor(context, color.r, color.g, color.b, color.a); };
                auto stroke = [&](const Color& color, CGFloat lineWidth) {
                    CGContextSetRGBStrokeColor(context, color.r, color.g, color.b, color.a);
                    CGContextSetLineWidth(context, lineWidth);
                };
                const Color white = {1.0f, 1.0f, 1.0f, 1.0f};
                switch (icon) {
                case PillIcon::Check: {
                    fill(palette.green);
                    CGContextFillEllipseInRect(context, CGRectMake(0, 0, size, size));
                    stroke(white, size * 0.12);
                    CGContextSetLineCap(context, kCGLineCapRound);
                    CGContextSetLineJoin(context, kCGLineJoinRound);
                    const CGPoint points[] = {at(0.29, 0.52), at(0.44, 0.67), at(0.72, 0.37)};
                    CGContextAddLines(context, points, std::size(points));
                    CGContextStrokePath(context);
                    break;
                }
                case PillIcon::Warning:
                case PillIcon::Danger: {
                    fill(icon == PillIcon::Warning ? palette.orange : palette.red);
                    CGContextFillEllipseInRect(context, CGRectMake(0, 0, size, size));
                    stroke(white, size * 0.13);
                    CGContextSetLineCap(context, kCGLineCapRound);
                    const CGPoint line[] = {at(0.5, 0.27), at(0.5, 0.56)};
                    CGContextStrokeLineSegments(context, line, 2);
                    fill(white);
                    const CGFloat radius = size * 0.075;
                    CGContextFillEllipseInRect(context, CGRectMake(size * 0.5 - radius, size * 0.74 - radius, radius * 2.0, radius * 2.0));
                    break;
                }
                case PillIcon::Clock: {
                    stroke(palette.secondary, size * 0.1);
                    const CGFloat ring = size * 0.43;
                    CGContextStrokeEllipseInRect(context, CGRectMake(size * 0.5 - ring, size * 0.5 - ring, ring * 2.0, ring * 2.0));
                    const CGPoint hands[] = {at(0.5, 0.5), at(0.5, 0.27), at(0.5, 0.5), at(0.68, 0.6)};
                    CGContextStrokeLineSegments(context, hands, 4);
                    break;
                }
                case PillIcon::None:
                    break;
                }
            });
            Place(layers.icon, CGRectMake(left, top + (kLineHeight - kIconSize) * 0.5f, kIconSize, kIconSize));
            titleLeft += kIconSize + kIconGap;
        }
        layers.title.hidden = !pill.contentVisible;
        if (pill.contentVisible) {
            const TextImage title = scene.Text(pill.title, (__bridge CTFontRef)scene.titleFont, right - titleLeft, scene.palette.text, L'b');
            layers.title.contents = title.image;
            Place(layers.title, CGRectMake(Snap(titleLeft, scale_), top, title.width, title.height));
        }
        const bool hasSubtitle = pill.contentVisible && !pill.subtitle.empty();
        layers.subtitle.hidden = !hasSubtitle || pill.loading;
        layers.shimmerHost.hidden = !hasSubtitle || !pill.loading;
        if (hasSubtitle) {
            const TextImage subtitle = scene.Text(pill.subtitle, (__bridge CTFontRef)scene.subtitleFont, right - left, scene.palette.secondary, L's');
            const CGRect frame = CGRectMake(left, top + kLineHeight, subtitle.width, subtitle.height);
            layers.subtitleWidth = subtitle.width;
            if (pill.loading) {
                Place(layers.shimmerHost, frame);
                Place(layers.shimmerMask, CGRectMake(0, 0, frame.size.width, frame.size.height));
                layers.shimmerMask.contents = subtitle.image;
                Place(layers.shimmerBase, CGRectMake(0, 0, frame.size.width, frame.size.height));
                layers.shimmerBase.backgroundColor = (__bridge CGColorRef)scene.secondaryColor;
                layers.shimmerBand.colors = @[ scene.secondaryColor, scene.shimmerColor, scene.secondaryColor ];
            } else {
                layers.subtitle.contents = subtitle.image;
                Place(layers.subtitle, frame);
            }
        }
        layers.version = scene.version;
        layers.width = width;
        layers.height = height;
    }
    if (pill.contentVisible && pill.loading && !pill.subtitle.empty()) {
        const float textWidth = std::max(24.0f, std::min(layers.subtitleWidth, right - left));
        const float band = std::max(28.0f, textWidth * 0.35f);
        const float center = left - band + pill.shimmerPhase * (textWidth + band * 2.0f);
        Place(layers.shimmerBand, CGRectMake(center - band * 0.5f - left, 0, band, kLineHeight));
    }
    if (reshaped || layers.showDismiss != pill.showDismiss || layers.dismissHovered != pill.dismissHovered) {
        layers.showDismiss = pill.showDismiss;
        layers.dismissHovered = pill.dismissHovered;
        layers.dismiss.hidden = !pill.showDismiss;
        if (pill.showDismiss) {
            const RectF box = DismissRect(RectF{0.0f, 0.0f, width, height});
            const float size = box.right - box.left;
            Place(layers.dismiss, ToCG(box));
            scene.ConfigureMaterial(layers.dismissMaterial, CGRectMake(0, 0, size, size), size * 0.5f, pill.dismissHovered);
            const Color ink = scene.palette.text;
            layers.cross.contents = scene.Drawing(L"cross", CGSizeMake(size, size), ^(CGContextRef context) {
                const CGFloat inset = size * 0.32;
                CGContextSetRGBStrokeColor(context, ink.r, ink.g, ink.b, ink.a * 0.85);
                CGContextSetLineWidth(context, 1.5);
                CGContextSetLineCap(context, kCGLineCapRound);
                const CGPoint lines[] = {CGPointMake(inset, inset), CGPointMake(size - inset, size - inset), CGPointMake(size - inset, inset), CGPointMake(inset, size - inset)};
                CGContextStrokeLineSegments(context, lines, 4);
            });
            Place(layers.cross, CGRectMake(0, 0, size, size));
        }
    }
    [scene.order addObject:layers.container];
}

void Renderer::DrawBadge(const BadgeVisual& badge) {
    Scene& scene = *scene_;
    BadgeLayers& layers = scene.badge;
    if (layers.container == nil) {
        layers.container = NewLayer(nil);
        CreateMaterial(layers.material, layers.container);
        layers.content = NewLayer(layers.container);
    }
    const float size = badge.rect.right - badge.rect.left;
    Place(layers.container, ToCG(badge.rect));
    const bool reshaped = layers.version != scene.version || layers.size != size;
    if (reshaped || layers.hovered != badge.hovered) {
        scene.ConfigureMaterial(layers.material, CGRectMake(0, 0, size, size), size * 0.5f, badge.hovered);
        layers.hovered = badge.hovered;
    }
    if (reshaped || layers.text != badge.text || layers.chevronDown != badge.chevronDown || layers.chevronUp != badge.chevronUp || layers.tone != badge.tone) {
        layers.text = badge.text;
        layers.chevronDown = badge.chevronDown;
        layers.chevronUp = badge.chevronUp;
        layers.tone = badge.tone;
        if (badge.chevronDown || badge.chevronUp) {
            const bool down = badge.chevronDown;
            const Color ink = scene.palette.text;
            const float box = size - 6.0f;
            layers.content.contents = scene.Drawing(down ? L"chevron-down" : L"chevron-up", CGSizeMake(box, box), ^(CGContextRef context) {
                const CGFloat y0 = down ? 0.4 : 0.6;
                const CGFloat y1 = down ? 0.6 : 0.4;
                const CGPoint points[] = {CGPointMake(0.32 * box, y0 * box), CGPointMake(0.5 * box, y1 * box), CGPointMake(0.68 * box, y0 * box)};
                CGContextSetRGBStrokeColor(context, ink.r, ink.g, ink.b, ink.a);
                CGContextSetLineWidth(context, 1.6);
                CGContextSetLineCap(context, kCGLineCapRound);
                CGContextSetLineJoin(context, kCGLineJoinRound);
                CGContextAddLines(context, points, std::size(points));
                CGContextStrokePath(context);
            });
            Place(layers.content, CGRectMake(3.0f, 3.0f, box, box));
        } else {
            Color tone = scene.palette.text;
            wchar_t tag = L'g';
            switch (badge.tone) {
            case SessionStatus::Waiting:
                tone = scene.palette.orange;
                tag = L'o';
                break;
            case SessionStatus::Failed:
                tone = scene.palette.red;
                tag = L'r';
                break;
            case SessionStatus::Review:
                tone = scene.palette.green;
                tag = L'v';
                break;
            default:
                break;
            }
            const TextImage text = scene.Text(badge.text, (__bridge CTFontRef)scene.badgeFont, size, tone, tag);
            layers.content.contents = text.image;
            Place(layers.content, CGRectMake(Snap((size - text.width) * 0.5f, scale_), Snap((size - text.height) * 0.5f, scale_), text.width, text.height));
        }
        layers.version = scene.version;
        layers.size = size;
    }
    [scene.order addObject:layers.container];
}

}
