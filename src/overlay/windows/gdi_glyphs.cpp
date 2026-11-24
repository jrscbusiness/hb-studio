#include "overlay/windows/gdi_glyphs.hpp"

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace hb::overlay {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

class GdiGlyphs : public IGlyphSource {
public:
    GdiGlyphs() { dc_ = CreateCompatibleDC(nullptr); }
    ~GdiGlyphs() override {
        for (auto& kv : fonts_) DeleteObject(kv.second);
        if (dc_) DeleteDC(dc_);
    }

    int advance(const std::string& line, const TextStyle& st) override {
        std::lock_guard lock(mutex_);
        return advanceLocked(line, st);
    }

    int lineHeight(const TextStyle& st) override {
        std::lock_guard lock(mutex_);
        HFONT f = fontFor(st);
        if (!dc_ || !f) return st.px;
        HGDIOBJ old = SelectObject(dc_, f);
        TEXTMETRICW tm{};
        const BOOL ok = GetTextMetricsW(dc_, &tm);
        SelectObject(dc_, old);
        return ok && tm.tmHeight > 0 ? static_cast<int>(tm.tmHeight) : st.px;
    }

    bool drawMask(const std::string& line, const TextStyle& st, std::vector<std::uint8_t>& mask, int& w, int& h) override {
        std::lock_guard lock(mutex_);
        const std::wstring ws = widen(line);
        HFONT f = fontFor(st);
        if (!dc_ || !f || ws.empty()) return false;
        w = advanceLocked(line, st);
        TEXTMETRICW tm{};
        HGDIOBJ old = SelectObject(dc_, f);
        const BOOL tmOk = GetTextMetricsW(dc_, &tm);
        h = tmOk && tm.tmHeight > 0 ? static_cast<int>(tm.tmHeight) : st.px;
        if (w <= 0 || h <= 0 || w > 8192 || h > 1024) { SelectObject(dc_, old); return false; }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(dc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) { SelectObject(dc_, old); return false; }
        HGDIOBJ oldBmp = SelectObject(dc_, dib);
        std::memset(bits, 0, static_cast<std::size_t>(w) * h * 4);  // black
        SetBkMode(dc_, TRANSPARENT);
        SetTextColor(dc_, RGB(255, 255, 255));
        TextOutW(dc_, 0, 0, ws.c_str(), static_cast<int>(ws.size()));
        GdiFlush();
        mask.resize(static_cast<std::size_t>(w) * h);
        const auto* src = static_cast<const std::uint8_t*>(bits);
        for (std::size_t i = 0; i < mask.size(); ++i) mask[i] = src[i * 4 + 1];  // green channel = coverage
        SelectObject(dc_, oldBmp);
        DeleteObject(dib);
        SelectObject(dc_, old);
        return true;
    }

private:
    int advanceLocked(const std::string& line, const TextStyle& st) {
        const std::wstring ws = widen(line);
        HFONT f = fontFor(st);
        if (!dc_ || !f || ws.empty()) return 0;
        HGDIOBJ old = SelectObject(dc_, f);
        SIZE sz{};
        const BOOL ok = GetTextExtentPoint32W(dc_, ws.c_str(), static_cast<int>(ws.size()), &sz);
        SelectObject(dc_, old);
        return ok ? static_cast<int>(sz.cx) + (st.bold ? 1 : 0) : 0;
    }

    HFONT fontFor(const TextStyle& st) {
        const int px = st.px < 6 ? 6 : (st.px > 600 ? 600 : st.px);
        const std::string key = st.font + "|" + std::to_string(px) + (st.bold ? "|b" : "|r");
        auto it = fonts_.find(key);
        if (it != fonts_.end()) return it->second;
        if (fonts_.size() >= 48) {  // bounded cache; fonts are never selected into the DC between calls
            for (auto& kv : fonts_) DeleteObject(kv.second);
            fonts_.clear();
        }
        std::wstring face = widen(st.font);
        if (face.empty() || face.size() >= LF_FACESIZE) face = L"Segoe UI";
        HFONT f = CreateFontW(-px, 0, 0, 0, st.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              face.c_str());
        if (f) fonts_[key] = f;
        return f;
    }

    std::mutex mutex_;
    HDC dc_ = nullptr;
    std::map<std::string, HFONT> fonts_;
};

}  // namespace

std::shared_ptr<IGlyphSource> makeGdiGlyphSource() { return std::make_shared<GdiGlyphs>(); }
std::shared_ptr<ITextRasterizer> makeGdiTextRasterizer() { return std::make_shared<MaskTextRasterizer>(makeGdiGlyphSource()); }

}  // namespace hb::overlay
