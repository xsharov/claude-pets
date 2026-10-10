#include "renderer.h"

#include <algorithm>
#include <cmath>

namespace cp {

namespace {

constexpr float kTitleSize = 13.0f;
constexpr float kSubtitleSize = 13.0f;
constexpr float kBadgeSize = 11.5f;
constexpr float kPillPaddingX = 16.0f;
constexpr float kPillPaddingY = 12.0f;
constexpr float kLineHeight = 16.0f;
constexpr float kIconSize = 14.0f;
constexpr float kIconGap = 5.0f;
constexpr int kSurfaceGranularity = 64;
constexpr size_t kLayoutCacheLimit = 160;

D2D1_COLOR_F Rgba(uint32_t rgb, float alpha) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float factor) {
    color.a *= factor;
    return color;
}

D2D1_RECT_F Inflate(const D2D1_RECT_F& rect, float dx, float dy) {
    return D2D1::RectF(rect.left - dx, rect.top - dy, rect.right + dx, rect.bottom + dy);
}

D2D1_RECT_F ToD2D(const RectF& rect) {
    return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

RectF ComputeContentBounds(IWICBitmap* bitmap, UINT width, UINT height, int rows, float cellWidth, float cellHeight) {
    const RectF full = {0.0f, 0.0f, 1.0f, 1.0f};
    const WICRect area = {0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    ComPtr<IWICBitmapLock> lock;
    UINT stride = 0;
    UINT size = 0;
    BYTE* data = nullptr;
    if (FAILED(bitmap->Lock(&area, WICBitmapLockRead, &lock)) || FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || data == nullptr) {
        return full;
    }
    const int cellW = static_cast<int>(cellWidth);
    const int cellH = static_cast<int>(cellHeight);
    if (cellW <= 0 || cellH <= 0) {
        return full;
    }
    int minX = cellW;
    int minY = cellH;
    int maxX = -1;
    int maxY = -1;
    const UINT usedHeight = std::min<UINT>(height, static_cast<UINT>(rows * cellH));
    for (UINT y = 0; y < usedHeight; ++y) {
        const BYTE* row = data + static_cast<size_t>(y) * stride;
        const int cellY = static_cast<int>(y) % cellH;
        for (UINT x = 0; x < width; ++x) {
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

std::wstring PickFontFamily(IDWriteFactory* factory) {
    ComPtr<IDWriteFontCollection> collection;
    if (SUCCEEDED(factory->GetSystemFontCollection(&collection, FALSE))) {
        for (const wchar_t* family : {L"Segoe UI Variable Text", L"Segoe UI"}) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(collection->FindFamilyName(family, &index, &exists)) && exists) {
                return family;
            }
        }
    }
    return L"Segoe UI";
}

bool CreateFormat(IDWriteFactory* factory, const std::wstring& family, DWRITE_FONT_WEIGHT weight, float size, ComPtr<IDWriteTextFormat>& format) {
    if (FAILED(factory->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) {
        return false;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kLineHeight, kLineHeight * 0.8f);
    ComPtr<IDWriteInlineObject> sign;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format.Get(), &sign))) {
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        format->SetTrimming(&trimming, sign.Get());
    }
    return true;
}

}

Renderer::~Renderer() {
    ReleaseSurface();
    if (memoryDc_ != nullptr) {
        DeleteDC(memoryDc_);
    }
}

bool Renderer::Initialize() {
    D2D1_FACTORY_OPTIONS options = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, reinterpret_cast<void**>(d2dFactory_.GetAddressOf())))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) {
        return false;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory_)))) {
        return false;
    }
    const std::wstring family = PickFontFamily(writeFactory_.Get());
    if (!CreateFormat(writeFactory_.Get(), family, DWRITE_FONT_WEIGHT_BOLD, kTitleSize, titleFormat_) ||
        !CreateFormat(writeFactory_.Get(), family, DWRITE_FONT_WEIGHT_NORMAL, kSubtitleSize, subtitleFormat_) ||
        !CreateFormat(writeFactory_.Get(), family, DWRITE_FONT_WEIGHT_SEMI_BOLD, kBadgeSize, badgeFormat_)) {
        return false;
    }
    badgeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    badgeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    memoryDc_ = CreateCompatibleDC(nullptr);
    return memoryDc_ != nullptr && CreateTarget();
}

bool Renderer::CreateTarget() {
    target_.Reset();
    context_.Reset();
    brush_.Reset();
    sheetBitmap_.Reset();
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(d2dFactory_->CreateDCRenderTarget(&properties, &target_))) {
        return false;
    }
    target_.As(&context_);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_));
}

void Renderer::ReleaseSurface() {
    if (dib_ != nullptr) {
        SelectObject(memoryDc_, previousBitmap_);
        DeleteObject(dib_);
        dib_ = nullptr;
        previousBitmap_ = nullptr;
    }
    surfaceWidth_ = 0;
    surfaceHeight_ = 0;
}

bool Renderer::EnsureSurface(int widthPx, int heightPx) {
    const bool fits = widthPx <= surfaceWidth_ && heightPx <= surfaceHeight_;
    const bool wasteful = static_cast<long long>(surfaceWidth_) * surfaceHeight_ > 4LL * widthPx * heightPx + 65536;
    if (dib_ != nullptr && fits && !wasteful) {
        return true;
    }
    ReleaseSurface();
    const int width = (widthPx + kSurfaceGranularity - 1) / kSurfaceGranularity * kSurfaceGranularity;
    const int height = (heightPx + kSurfaceGranularity - 1) / kSurfaceGranularity * kSurfaceGranularity;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    dib_ = CreateDIBSection(memoryDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib_ == nullptr) {
        return false;
    }
    previousBitmap_ = SelectObject(memoryDc_, dib_);
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    return true;
}

bool Renderer::LoadSpritesheet(const std::wstring& path, int declaredVersion, std::wstring& error) {
    UnloadSpritesheet();
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        error = hr == WINCODEC_ERR_COMPONENTNOTFOUND ? L"codec" : L"open";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(wicFactory_->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
        error = L"decode";
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    converter->GetSize(&width, &height);
    if (width < kSpriteColumns || height == 0) {
        error = L"size";
        return false;
    }
    cellWidth_ = static_cast<float>(width) / kSpriteColumns;
    const float expectedCell = cellWidth_ / kCellAspect;
    const int rowsFromImage = static_cast<int>(std::lround(static_cast<float>(height) / expectedCell));
    spriteRows_ = rowsFromImage >= kSpriteRowsV2 || (declaredVersion >= 2 && rowsFromImage > kSpriteRowsV1) ? kSpriteRowsV2 : kSpriteRowsV1;
    cellHeight_ = static_cast<float>(height) / spriteRows_;
    ComPtr<IWICBitmap> cached;
    if (FAILED(wicFactory_->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &cached))) {
        error = L"decode";
        return false;
    }
    contentBounds_ = ComputeContentBounds(cached.Get(), width, height, spriteRows_, cellWidth_, cellHeight_);
    sheetSource_ = cached;
    sheetPath_ = path;
    sheetVersion_ = declaredVersion;
    return true;
}

void Renderer::UnloadSpritesheet() {
    sheetSource_.Reset();
    sheetBitmap_.Reset();
    sheetPath_.clear();
    contentBounds_ = RectF{0.0f, 0.0f, 1.0f, 1.0f};
    spriteRows_ = kSpriteRowsV1;
}

bool Renderer::HasSprite() const {
    return sheetSource_ != nullptr || sheetBitmap_ != nullptr;
}

bool Renderer::UploadSprite() {
    if (sheetBitmap_ != nullptr) {
        return true;
    }
    if (target_ == nullptr) {
        return false;
    }
    if (sheetSource_ == nullptr && !sheetPath_.empty()) {
        std::wstring error;
        const std::wstring path = sheetPath_;
        if (!LoadSpritesheet(path, sheetVersion_, error)) {
            return false;
        }
    }
    if (sheetSource_ == nullptr || FAILED(target_->CreateBitmapFromWicBitmap(sheetSource_.Get(), nullptr, &sheetBitmap_))) {
        return false;
    }
    sheetSource_.Reset();
    return true;
}

bool Renderer::BeginFrame(int widthPx, int heightPx, float scale, const RectI* partial) {
    if (target_ == nullptr && !CreateTarget()) {
        return false;
    }
    if (widthPx <= 0 || heightPx <= 0) {
        return false;
    }
    if (partial != nullptr && (dib_ == nullptr || widthPx > surfaceWidth_ || heightPx > surfaceHeight_)) {
        partial = nullptr;
    }
    if (partial == nullptr && !EnsureSurface(widthPx, heightPx)) {
        return false;
    }
    partial_ = partial != nullptr;
    partialRect_ = partial_ ? RECT{partial->left, partial->top, partial->right, partial->bottom} : RECT{0, 0, widthPx, heightPx};
    if (FAILED(target_->BindDC(memoryDc_, &partialRect_))) {
        return false;
    }
    scale_ = scale;
    frameWidth_ = widthPx;
    frameHeight_ = heightPx;
    target_->SetDpi(96.0f * scale, 96.0f * scale);
    UploadSprite();
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Translation(-partialRect_.left / scale, -partialRect_.top / scale));
    target_->Clear(D2D1::ColorF(0, 0, 0, 0));
    drawing_ = true;
    return true;
}

bool Renderer::EndFrame(HWND window, POINT windowOrigin) {
    if (!drawing_) {
        return false;
    }
    drawing_ = false;
    const HRESULT hr = target_->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        CreateTarget();
        return false;
    }
    if (FAILED(hr)) {
        return false;
    }
    GdiFlush();
    POINT source = {0, 0};
    SIZE size = {frameWidth_, frameHeight_};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info = {};
    info.cbSize = sizeof(info);
    info.pptDst = &windowOrigin;
    info.psize = &size;
    info.hdcSrc = memoryDc_;
    info.pptSrc = &source;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = partial_ ? &partialRect_ : nullptr;
    return UpdateLayeredWindowIndirect(window, &info) != FALSE;
}

Renderer::Palette Renderer::CurrentPalette() const {
    if (dark_) {
        return {Rgba(0x202020, 0.94f), Rgba(0xFFFFFF, 0.13f), Rgba(0x000000, 1.0f), Rgba(0xF3F3F3, 1.0f), Rgba(0xAFAFAF, 1.0f),
                Rgba(0x5C5C5C, 1.0f), Rgba(0x30D158, 1.0f), Rgba(0xFF9230, 1.0f), Rgba(0xFF453A, 1.0f), Rgba(0xFFFFFF, 0.06f)};
    }
    return {Rgba(0xFFFFFF, 0.94f), Rgba(0x000000, 0.10f), Rgba(0x000000, 1.0f), Rgba(0x0D0D0D, 1.0f), Rgba(0x5D5D5D, 1.0f),
            Rgba(0xD2D2D2, 1.0f), Rgba(0x34C759, 1.0f), Rgba(0xFF8D28, 1.0f), Rgba(0xFF3B30, 1.0f), Rgba(0x000000, 0.04f)};
}

ID2D1SolidColorBrush* Renderer::Brush(const D2D1_COLOR_F& color, float opacity) {
    brush_->SetColor(WithAlpha(color, opacity));
    return brush_.Get();
}

IDWriteTextLayout* Renderer::Layout(const std::wstring& text, IDWriteTextFormat* format, float width, bool bold) {
    std::wstring key = text;
    key.push_back(L'\x1f');
    key.append(std::to_wstring(static_cast<int>(std::lround(width * 4.0f))));
    key.push_back(bold ? L'b' : (format == badgeFormat_.Get() ? L'g' : L'r'));
    auto found = layouts_.find(key);
    if (found != layouts_.end()) {
        return found->second.Get();
    }
    if (layouts_.size() >= kLayoutCacheLimit) {
        layouts_.clear();
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(writeFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, std::max(1.0f, width), kLineHeight, &layout))) {
        return nullptr;
    }
    IDWriteTextLayout* raw = layout.Get();
    layouts_.emplace(std::move(key), std::move(layout));
    return raw;
}

void Renderer::FillMaterial(const D2D1_ROUNDED_RECT& shape, const Palette& palette, float opacity, bool hovered) {
    static constexpr float kShadowWeights[] = {0.075f, 0.05f, 0.032f, 0.018f, 0.008f};
    const float shadowScale = dark_ ? 1.8f : 1.0f;
    for (int i = static_cast<int>(std::size(kShadowWeights)); i >= 1; --i) {
        D2D1_ROUNDED_RECT shadow = shape;
        shadow.rect = Inflate(shape.rect, static_cast<float>(i) * 0.9f, static_cast<float>(i) * 0.9f);
        shadow.rect.top += 1.5f;
        shadow.rect.bottom += 2.0f;
        shadow.radiusX += static_cast<float>(i) * 0.9f;
        shadow.radiusY += static_cast<float>(i) * 0.9f;
        target_->FillRoundedRectangle(shadow, Brush(palette.shadow, kShadowWeights[i - 1] * shadowScale * opacity));
    }
    target_->FillRoundedRectangle(shape, Brush(palette.material, opacity));
    if (hovered) {
        target_->FillRoundedRectangle(shape, Brush(palette.hoverOverlay, opacity));
    }
    D2D1_ROUNDED_RECT border = shape;
    border.rect = Inflate(shape.rect, -0.5f, -0.5f);
    target_->DrawRoundedRectangle(border, Brush(palette.border, opacity), 1.0f);
}

void Renderer::DrawIcon(PillIcon icon, const D2D1_RECT_F& box, const Palette& palette, float opacity) {
    const float size = box.right - box.left;
    const D2D1_POINT_2F center = D2D1::Point2F((box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f);
    const D2D1_ELLIPSE circle = D2D1::Ellipse(center, size * 0.5f, size * 0.5f);
    auto at = [&](float x, float y) { return D2D1::Point2F(box.left + x * size, box.top + y * size); };
    switch (icon) {
    case PillIcon::Check: {
        target_->FillEllipse(circle, Brush(palette.green, opacity));
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure(at(0.29f, 0.52f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(at(0.44f, 0.67f));
            sink->AddLine(at(0.72f, 0.37f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            ComPtr<ID2D1StrokeStyle> style;
            const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            d2dFactory_->CreateStrokeStyle(properties, nullptr, 0, &style);
            target_->DrawGeometry(path.Get(), Brush(D2D1::ColorF(1, 1, 1, 1), opacity), size * 0.12f, style.Get());
        }
        break;
    }
    case PillIcon::Warning:
    case PillIcon::Danger: {
        target_->FillEllipse(circle, Brush(icon == PillIcon::Warning ? palette.orange : palette.red, opacity));
        ComPtr<ID2D1StrokeStyle> style;
        const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
        d2dFactory_->CreateStrokeStyle(properties, nullptr, 0, &style);
        target_->DrawLine(at(0.5f, 0.27f), at(0.5f, 0.56f), Brush(D2D1::ColorF(1, 1, 1, 1), opacity), size * 0.13f, style.Get());
        target_->FillEllipse(D2D1::Ellipse(at(0.5f, 0.74f), size * 0.075f, size * 0.075f), Brush(D2D1::ColorF(1, 1, 1, 1), opacity));
        break;
    }
    case PillIcon::Clock: {
        const D2D1_ELLIPSE ring = D2D1::Ellipse(center, size * 0.43f, size * 0.43f);
        target_->DrawEllipse(ring, Brush(palette.secondary, opacity), size * 0.1f);
        target_->DrawLine(center, at(0.5f, 0.27f), Brush(palette.secondary, opacity), size * 0.1f);
        target_->DrawLine(center, at(0.68f, 0.6f), Brush(palette.secondary, opacity), size * 0.1f);
        break;
    }
    case PillIcon::None:
        break;
    }
}

void Renderer::DrawChevron(const D2D1_RECT_F& box, bool down, const D2D1_COLOR_F& color, float opacity) {
    const float size = box.right - box.left;
    auto at = [&](float x, float y) { return D2D1::Point2F(box.left + x * size, box.top + y * size); };
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2dFactory_->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) {
        return;
    }
    const float y0 = down ? 0.4f : 0.6f;
    const float y1 = down ? 0.6f : 0.4f;
    sink->BeginFigure(at(0.32f, y0), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(at(0.5f, y1));
    sink->AddLine(at(0.68f, y0));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ComPtr<ID2D1StrokeStyle> style;
    const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    d2dFactory_->CreateStrokeStyle(properties, nullptr, 0, &style);
    target_->DrawGeometry(path.Get(), Brush(color, opacity), 1.6f, style.Get());
}

void Renderer::DrawPet(const RectF& rect, const SpriteFrame& frame, float opacity) {
    if (!UploadSprite()) {
        DrawPetPlaceholder(rect);
        return;
    }
    const D2D1_RECT_F source = D2D1::RectF(frame.column * cellWidth_, frame.row * cellHeight_, (frame.column + 1) * cellWidth_, (frame.row + 1) * cellHeight_);
    const float leftPx = std::round(rect.left * scale_);
    const float topPx = std::round(rect.top * scale_);
    const float widthPx = std::round((rect.right - rect.left) * scale_);
    const float heightPx = std::round((rect.bottom - rect.top) * scale_);
    const D2D1_RECT_F snapped = D2D1::RectF(leftPx / scale_, topPx / scale_, (leftPx + widthPx) / scale_, (topPx + heightPx) / scale_);
    const bool upscale = widthPx >= cellWidth_ - 0.5f;
    if (context_ != nullptr) {
        context_->DrawBitmap(sheetBitmap_.Get(), snapped, opacity,
                             upscale ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &source, nullptr);
    } else {
        target_->DrawBitmap(sheetBitmap_.Get(), snapped, opacity,
                            upscale ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
    }
}

void Renderer::DrawPetPlaceholder(const RectF& rect) {
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const D2D1_RECT_F body = D2D1::RectF(rect.left + width * 0.18f, rect.top + height * 0.38f, rect.right - width * 0.18f, rect.bottom - height * 0.12f);
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(body, width * 0.18f, width * 0.18f);
    target_->FillRoundedRectangle(shape, Brush(Rgba(0xD97757, 1.0f), 1.0f));
    const float eye = width * 0.06f;
    const float eyeY = body.top + (body.bottom - body.top) * 0.4f;
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + width * 0.38f, eyeY), eye, eye * 1.4f), Brush(Rgba(0x1F1F1F, 1.0f), 1.0f));
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + width * 0.62f, eyeY), eye, eye * 1.4f), Brush(Rgba(0x1F1F1F, 1.0f), 1.0f));
}

void Renderer::FillHitArea(const RectF& rect) {
    target_->FillRectangle(ToD2D(rect), Brush(D2D1::ColorF(0, 0, 0, 1.0f / 255.0f), 1.0f));
}

RectF Renderer::DismissRect(const RectF& pill) {
    return RectF{pill.left - 4.0f, pill.top - 4.0f, pill.left + 16.0f, pill.top + 16.0f};
}

void Renderer::DrawPill(const PillVisual& pill) {
    if (pill.opacity <= 0.01f) {
        return;
    }
    const Palette palette = CurrentPalette();
    const float height = pill.rect.bottom - pill.rect.top;
    const float radius = std::min(28.0f, height * 0.5f);
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(ToD2D(pill.rect), radius, radius);
    FillMaterial(shape, palette, pill.opacity, pill.hovered);
    if (pill.contentVisible) {
        DrawPillContent(pill, palette);
    }
    if (pill.showDismiss) {
        const D2D1_RECT_F box = ToD2D(DismissRect(pill.rect));
        const float size = box.right - box.left;
        FillMaterial(D2D1::RoundedRect(box, size * 0.5f, size * 0.5f), palette, pill.opacity, pill.dismissHovered);
        const D2D1_RECT_F cross = Inflate(box, -size * 0.32f, -size * 0.32f);
        ComPtr<ID2D1StrokeStyle> style;
        const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
        d2dFactory_->CreateStrokeStyle(properties, nullptr, 0, &style);
        ID2D1SolidColorBrush* ink = Brush(palette.text, pill.opacity * 0.85f);
        target_->DrawLine(D2D1::Point2F(cross.left, cross.top), D2D1::Point2F(cross.right, cross.bottom), ink, 1.5f, style.Get());
        target_->DrawLine(D2D1::Point2F(cross.right, cross.top), D2D1::Point2F(cross.left, cross.bottom), ink, 1.5f, style.Get());
    }
}

void Renderer::DrawPillContent(const PillVisual& pill, const Palette& palette) {
    const float left = pill.rect.left + kPillPaddingX;
    const float right = pill.rect.right - kPillPaddingX;
    const float top = pill.rect.top + kPillPaddingY;
    float titleLeft = left;
    if (pill.icon != PillIcon::None) {
        const float iconTop = top + (kLineHeight - kIconSize) * 0.5f;
        DrawIcon(pill.icon, D2D1::RectF(left, iconTop, left + kIconSize, iconTop + kIconSize), palette, pill.opacity);
        titleLeft += kIconSize + kIconGap;
    }
    const D2D1_DRAW_TEXT_OPTIONS textOptions = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT;
    if (IDWriteTextLayout* title = Layout(pill.title, titleFormat_.Get(), right - titleLeft, true)) {
        target_->DrawTextLayout(D2D1::Point2F(titleLeft, top), title, Brush(palette.text, pill.opacity), textOptions);
    }
    if (pill.subtitle.empty()) {
        return;
    }
    IDWriteTextLayout* subtitle = Layout(pill.subtitle, subtitleFormat_.Get(), right - left, false);
    if (subtitle == nullptr) {
        return;
    }
    const D2D1_POINT_2F origin = D2D1::Point2F(left, top + kLineHeight);
    if (!pill.loading) {
        target_->DrawTextLayout(origin, subtitle, Brush(palette.secondary, pill.opacity), textOptions);
        return;
    }
    DWRITE_TEXT_METRICS metrics = {};
    subtitle->GetMetrics(&metrics);
    const float textWidth = std::max(24.0f, std::min(metrics.widthIncludingTrailingWhitespace, right - left));
    const float band = std::max(28.0f, textWidth * 0.35f);
    const float center = left - band + pill.shimmerPhase * (textWidth + band * 2.0f);
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, WithAlpha(palette.secondary, pill.opacity)},
        {0.5f, WithAlpha(palette.shimmer, pill.opacity)},
        {1.0f, WithAlpha(palette.secondary, pill.opacity)},
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection)) &&
        SUCCEEDED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(center - band * 0.5f, origin.y), D2D1::Point2F(center + band * 0.5f, origin.y)),
                                                     collection.Get(), &gradient))) {
        target_->DrawTextLayout(origin, subtitle, gradient.Get(), textOptions);
    } else {
        target_->DrawTextLayout(origin, subtitle, Brush(palette.secondary, pill.opacity), textOptions);
    }
}

void Renderer::DrawBadge(const BadgeVisual& badge) {
    const Palette palette = CurrentPalette();
    const float size = badge.rect.right - badge.rect.left;
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(ToD2D(badge.rect), size * 0.5f, size * 0.5f);
    FillMaterial(shape, palette, 1.0f, badge.hovered);
    D2D1_COLOR_F tone = palette.text;
    switch (badge.tone) {
    case SessionStatus::Waiting:
        tone = palette.orange;
        break;
    case SessionStatus::Failed:
        tone = palette.red;
        break;
    case SessionStatus::Review:
        tone = palette.green;
        break;
    default:
        break;
    }
    if (badge.chevronDown || badge.chevronUp) {
        DrawChevron(Inflate(ToD2D(badge.rect), -3.0f, -3.0f), badge.chevronDown, palette.text, 1.0f);
        return;
    }
    if (IDWriteTextLayout* layout = Layout(badge.text, badgeFormat_.Get(), size, false)) {
        layout->SetMaxHeight(size);
        target_->DrawTextLayout(D2D1::Point2F(badge.rect.left, badge.rect.top), layout, Brush(tone, 1.0f), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

}
