// The interface's drawing surface: TPT's drawing operations (graphics/RasterDrawMethods) on a CPU image in TPT's
// pixels, composited over the world view on the GPU (gpu/Overlay). Each pixel keeps what has been drawn on it as
// premultiplied colour and coverage, so blending onto it and then compositing it over the world gives what TPT
// gets by blending straight onto the picture.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace bp {

struct ElementTable;

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};
inline Rgba rgb(uint32_t c, uint8_t a = 255) { return {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c), a}; }

// graphics/Icons.h, as drawn by Graphics::draw_icon
enum class Icon {
    None, Open, Reload, Save, VoteUp, VoteDown, Tag, New, Login, SimulationSettings, RenderSettings, Pause,
    Favourite, Report, Username, Password, Close, VoteSort, DateSort, MyOwn, Search, Delete, Add, Velocity,
    Pressure, Persistant, Fire, Blob, Heat, Blur, Gradient, Life, Effect, Glow, Alternate, Basic,
};

class Canvas {
public:
    explicit Canvas(const ElementTable &elements) : elements_(elements) {}

    void resize(int w, int h);
    void clear(); // transparent: the world shows through
    int width() const { return w_; }
    int height() const { return h_; }
    // Premultiplied RGBA, r | g << 8 | b << 16 | a << 24, row by row
    const std::vector<uint32_t> &pixels() const { return px_; }

    // Drawing is clipped to this rectangle (and the canvas)
    void setClip(int x, int y, int w, int h);
    void resetClip() { setClip(0, 0, w_, h_); }
    struct Clip {
        int x0, y0, x1, y1; // [x0, x1) x [y0, y1)
    };
    Clip clip() const { return {cx0_, cy0_, cx1_, cy1_}; }
    void setClip(const Clip &c) { cx0_ = c.x0, cy0_ = c.y0, cx1_ = c.x1, cy1_ = c.y1; }
    // Clips further to the rectangle, within the current clip (Graphics::SwapClipRect with an intersection)
    void narrowClip(int x, int y, int w, int h) {
        Clip c = clip();
        setClip(x, y, w, h);
        cx0_ = std::max(cx0_, c.x0), cy0_ = std::max(cy0_, c.y0), cx1_ = std::min(cx1_, c.x1), cy1_ = std::min(cy1_, c.y1);
    }

    void drawPixel(int x, int y, uint32_t colour); // 0xRRGGBB, replacing what is there
    void xorPixel(int x, int y);                   // RasterDrawMethods::XorPixel: light or dark grey against it
    void xorLine(int x0, int y0, int x1, int y1);
    void blendPixel(int x, int y, Rgba c);         // RGB::Blend
    void addPixel(int x, int y, Rgba c);           // RGB::Add

    void drawRect(int x, int y, int w, int h, uint32_t colour); // the outline (RasterizeRect)
    void blendRect(int x, int y, int w, int h, Rgba c);
    void drawFilledRect(int x, int y, int w, int h, uint32_t colour);
    void blendFilledRect(int x, int y, int w, int h, Rgba c);
    void drawLine(int x0, int y0, int x1, int y1, uint32_t colour); // RasterizeLine<false>
    void drawImage(int x, int y, int w, int h, const std::vector<uint32_t> &rgb); // BlendImage at 255: 0xRRGGBB
    void xorDottedRect(int x, int y, int w, int h);                               // RasterizeDottedRect's pixels
    void blendLine(int x0, int y0, int x1, int y1, Rgba c);

    // TPT's font: BlendChar/AddChar return the character's width; BlendText follows TPT's escape codes (\n, \x0F r g
    // b, \x0E, \x01, \b and a letter) and returns where it ended, relative to (x, y)
    int blendChar(int x, int y, char32_t ch, Rgba c);
    int addChar(int x, int y, char32_t ch, Rgba c);
    void blendText(int x, int y, const std::u32string &text, Rgba c, int *endX = nullptr, int *endY = nullptr);
    int charWidth(char32_t ch) const;
    void textSize(const std::u32string &text, int &w, int &h) const; // Graphics::TextSize
    size_t textFit(const std::u32string &text, int width) const;     // Graphics::TextFit: how much fits
    void drawIcon(int x, int y, Icon icon, uint8_t alpha = 255, bool invert = false); // Graphics::draw_icon

private:
    bool inClip(int x, int y) const { return x >= cx0_ && y >= cy0_ && x < cx1_ && y < cy1_; }
    const ElementTable &elements_;
    int w_ = 0, h_ = 0;
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;
    std::vector<uint32_t> px_;
};

std::u32string utf32(const std::string &utf8);
std::string utf8(const std::u32string &text);

} // namespace bp
