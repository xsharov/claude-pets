#pragma once

#include "animation.h"
#include "session_model.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace cp {

using Microsoft::WRL::ComPtr;

enum class PillIcon : uint8_t { None, Check, Warning, Danger, Clock };

struct PillVisual {
    D2D1_RECT_F rect = {};
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
    D2D1_RECT_F rect = {};
    std::wstring text;
    bool chevronDown = false;
    bool chevronUp = false;
    SessionStatus tone = SessionStatus::Idle;
    bool hovered = false;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Initialize();
    void SetDarkTheme(bool dark) { dark_ = dark; }
    bool DarkTheme() const { return dark_; }

    bool LoadSpritesheet(const std::wstring& path, int declaredVersion, std::wstring& error);
    void UnloadSpritesheet();
    bool HasSprite() const { return sheetSource_ != nullptr || sheetBitmap_ != nullptr; }
    int SpriteRows() const { return spriteRows_; }

    bool BeginFrame(int widthPx, int heightPx, float scale, const RECT* partial);
    bool EndFrame(HWND window, POINT windowOrigin);

    void DrawPet(const D2D1_RECT_F& rect, const SpriteFrame& frame, float opacity);
    void DrawPetPlaceholder(const D2D1_RECT_F& rect);
    void DrawPill(const PillVisual& pill);
    void DrawBadge(const BadgeVisual& badge);

    static D2D1_RECT_F DismissRect(const D2D1_RECT_F& pill);
    uint8_t AlphaAt(int x, int y) const;

private:
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
    const uint8_t* bits_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool drawing_ = false;
    bool partial_ = false;
    RECT partialRect_ = {};
    int spriteRows_ = kSpriteRowsV1;
    int sheetVersion_ = 1;
    std::wstring sheetPath_;
    float cellWidth_ = 192.0f;
    float cellHeight_ = 208.0f;
};

}
