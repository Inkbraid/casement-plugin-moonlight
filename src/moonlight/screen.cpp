#include "moonlight/screen.h"

#include <windows.h>

#include <cstring>

#include "casement/log.h"
#include "casement/text.h"

namespace moonlight {

namespace {

constexpr COLORREF background = RGB(28, 30, 34);
constexpr COLORREF foreground = RGB(236, 238, 241);
constexpr COLORREF muted = RGB(150, 156, 166);
constexpr COLORREF accent = RGB(255, 255, 255);

HFONT make_font(int pixels, int weight) {
    // Grey-scale anti-aliasing: ClearType's colour fringes look wrong once
    // the picture is scaled.
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void line(HDC dc, const std::wstring& text, int pixels, int weight, COLORREF colour, RECT r) {
    if (text.empty()) return;
    HFONT font = make_font(pixels, weight);
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, colour);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    DeleteObject(font);
}

}  // namespace

void draw_screen(const Screen& screen, std::uint8_t* bgrx, int width, int height, std::size_t stride) {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top row first
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap || !bits) {
        casement::log::error("can't draw text: {}", casement::system_message(GetLastError()));
        for (int y = 0; y < height; ++y) std::memset(bgrx + y * stride, 0, std::size_t(width) * 4);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    RECT all{0, 0, width, height};
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(dc, &all, brush);
    DeleteObject(brush);
    SetBkMode(dc, TRANSPARENT);

    // Proportions for 960x540, scaled with the height.
    auto px = [&](int v) { return v * height / 540; };
    line(dc, screen.title, px(34), FW_SEMIBOLD, foreground, RECT{px(20), px(70), width - px(20), px(150)});
    line(dc, screen.big, px(170), FW_BOLD, accent, RECT{0, px(150), width, px(350)});
    line(dc, screen.text, px(28), FW_NORMAL, foreground, RECT{px(20), px(360), width - px(20), px(410)});
    line(dc, screen.note, px(22), FW_NORMAL, muted, RECT{px(20), px(440), width - px(20), px(490)});
    GdiFlush();

    const auto* src = static_cast<const std::uint8_t*>(bits);
    for (int y = 0; y < height; ++y) {
        std::uint8_t* d = bgrx + y * stride;
        std::memcpy(d, src + std::size_t(y) * width * 4, std::size_t(width) * 4);
        for (int x = 0; x < width; ++x) d[x * 4 + 3] = 255;
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

Screen pin_screen(const std::string& pin, const std::string& host) {
    Screen s;
    std::wstring h = casement::widen(host);
    s.title = L"Pair with " + h;
    // Spaced digits read better at this size.
    for (wchar_t c : casement::widen(pin)) {
        if (!s.big.empty()) s.big += L' ';
        s.big += c;
    }
    s.text = L"Enter PIN " + casement::widen(pin) + L" on " + h + L" (Sunshine web UI → PIN)";
    s.note = L"Waiting for the PIN to be entered on the host…";
    return s;
}

}  // namespace moonlight
