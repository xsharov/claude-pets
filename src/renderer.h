#pragma once

#include "animation.h"
#include "geometry.h"
#include "session_model.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace cp {

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#endif

enum class PillIcon : uint8_t { None, Check, Warning, Danger, Clock };

struct PillVisual {
    RectF rect = {};
    float opacity = 1.0f;
    std::wstring title;
    std::wstring subtitle;
    PillIcon icon = PillIcon::None;
    bool loading = false;
    float shimmerPhase = 0.0f;
    bool hovered = false;
    bool showDismiss = false;
    bool dismissHovered = false;
    bool contentVisible = true;
};

struct BadgeVisual {
    RectF rect = {};
    std::wstring text;
    bool chevronDown = false;
    bool chevronUp = false;
    SessionStatus tone = SessionStatus::Idle;
    bool hovered = false;
};

class Renderer {
public:
#if defined(__APPLE__)
    Renderer();
#else
    Renderer() = default;
#endif
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Initialize();
    void SetDarkTheme(bool dark) { dark_ = dark; }
    bool DarkTheme() const { return dark_; }

    bool LoadSpritesheet(const std::wstring& path, int declaredVersion, std::wstring& error);
    void UnloadSpritesheet();
    bool HasSprite() const;
    const RectF& SpriteContentBounds() const { return contentBounds_; }
    void FillHitArea(const RectF& rect);
    int SpriteRows() const { return spriteRows_; }

    bool BeginFrame(int widthPx, int heightPx, float scale, const RectI* partial);
#if defined(_WIN32)
    bool EndFrame(HWND window, POINT windowOrigin);
#elif defined(__APPLE__)
    bool EndFrame();
    void AttachLayer(void* layer);
#endif

    void DrawPet(const RectF& rect, const SpriteFrame& frame, float opacity);
    void DrawPetPlaceholder(const RectF& rect);
    void DrawPill(const PillVisual& pill);
    void DrawBadge(const BadgeVisual& badge);

    static RectF DismissRect(const RectF& pill);

private:
#if defined(_WIN32)
    struct Palette {
        D2D1_COLOR_F material;
        D2D1_COLOR_F border;
        D2D1_COLOR_F shadow;
        D2D1_COLOR_F text;
        D2D1_COLOR_F secondary;
        D2D1_COLOR_F shimmer;
        D2D1_COLOR_F green;
        D2D1_COLOR_F orange;
        D2D1_COLOR_F red;
        D2D1_COLOR_F hoverOverlay;
    };

    bool CreateTarget();
    bool EnsureSurface(int widthPx, int heightPx);
    void ReleaseSurface();
    bool UploadSprite();
    Palette CurrentPalette() const;
    IDWriteTextLayout* Layout(const std::wstring& text, IDWriteTextFormat* format, float width, bool bold);
    ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color, float opacity);
    void FillMaterial(const D2D1_ROUNDED_RECT& shape, const Palette& palette, float opacity, bool hovered);
    void DrawPillContent(const PillVisual& pill, const Palette& palette);
    void DrawIcon(PillIcon icon, const D2D1_RECT_F& box, const Palette& palette, float opacity);
    void DrawChevron(const D2D1_RECT_F& box, bool down, const D2D1_COLOR_F& color, float opacity);

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1DeviceContext> context_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> subtitleFormat_;
    ComPtr<IDWriteTextFormat> badgeFormat_;
    ComPtr<IDWriteInlineObject> ellipsis_;
    ComPtr<IWICBitmapSource> sheetSource_;
    ComPtr<ID2D1Bitmap> sheetBitmap_;
    std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> layouts_;

    HDC memoryDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    RECT partialRect_ = {};
    int sheetVersion_ = 1;
    std::wstring sheetPath_;
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
    bool partial_ = false;
#elif defined(__APPLE__)
    struct Scene;

    std::unique_ptr<Scene> scene_;
    CGColorSpaceRef colorSpace_ = nullptr;
    CGImageRef sheet_ = nullptr;
    std::vector<CGImageRef> cells_;
#endif

    int frameWidth_ = 0;
    int frameHeight_ = 0;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool drawing_ = false;
    int spriteRows_ = kSpriteRowsV1;
    float cellWidth_ = 192.0f;
    float cellHeight_ = 208.0f;
    RectF contentBounds_ = {0.0f, 0.0f, 1.0f, 1.0f};
};

}
