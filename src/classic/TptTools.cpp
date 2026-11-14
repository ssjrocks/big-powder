// TPT's brushes for TptBrush, built with TPT's settings (see ClassicSim.h).
#include "classic/TptToolsImpl.h"

#include "gui/game/BitmapBrush.h"
#include "gui/game/EllipseBrush.h"
#include "gui/game/RectangleBrush.h"
#include "gui/game/TriangleBrush.h"
#include "graphics/Gradient.h"
#include "graphics/Renderer.h"
#include "graphics/VideoBuffer.h"
#include "common/String.h"
#include "Format.h"
#include "Misc.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <filesystem>
#include <fstream>

// gui/game/Brush.cpp without its drawing functions, which need TPT's Graphics (not built here); the cursor is drawn
// by ui/BrushCursor instead.
void Brush::InitBitmap()
{
	bitmap = GenerateBitmap();
}

void Brush::InitOutline()
{
	InitBitmap();
	ui::Point bounds = GetSize();
	outline = PlaneAdapter<std::vector<unsigned char>>(bounds);
	for (int j = 0; j < bounds.Y; j++)
	{
		for (int i = 0; i < bounds.X; i++)
		{
			bool value = false;
			if (bitmap[{ i, j }])
			{
				if (i == 0 || j == 0 || i == bounds.X - 1 || j == bounds.Y - 1)
					value = true;
				else if (!bitmap[{ i + 1, j }])
					value = true;
				else if (!bitmap[{ i - 1, j }])
					value = true;
				else if (!bitmap[{ i, j + 1 }])
					value = true;
				else if (!bitmap[{ i, j - 1 }])
					value = true;
			}
			outline[{ i, j }] = value ? 0xFF : 0;
		}
	}
}

void Brush::SetRadius(ui::Point newRadius)
{
	if (newRadius.X < 0)
		newRadius.X = 0;
	if (newRadius.Y < 0)
		newRadius.Y = 0;
	if (newRadius.X > 200)
		newRadius.X = 200;
	if (newRadius.Y > 200)
		newRadius.Y = 200;
	radius = newRadius;
	InitOutline();
}

void Brush::AdjustSize(int delta, bool logarithmic, bool keepX, bool keepY)
{
	if (keepX && keepY)
		return;

	ui::Point newSize(0, 0);
	ui::Point oldSize = GetRadius();
	if (logarithmic)
		newSize = oldSize + ui::Point(delta * std::max(oldSize.X / 5, 1), delta * std::max(oldSize.Y / 5, 1));
	else
		newSize = oldSize + ui::Point(delta, delta);
	if (keepY)
		SetRadius(ui::Point(newSize.X, oldSize.Y));
	else if (keepX)
		SetRadius(ui::Point(oldSize.X, newSize.Y));
	else
		SetRadius(newSize);
}

namespace bp {

// Simulation::CreateLine and CreateWallLine (simulation/Editing.cpp), calling back instead of drawing
void tptLinePoints(int x1, int y1, int x2, int y2, LineKind kind, bool singlePixel, std::vector<int> &out)
{
	bool wallLine = kind == LineKind::Walls;
	auto at = [&](int x, int y) {
		out.push_back(x);
		out.push_back(y);
	};
	int x, y, dx, dy, sy;
	bool reverseXY = abs(y2-y1) > abs(x2-x1);
	float e = 0.0f, de;
	if (reverseXY)
	{
		y = x1;
		x1 = y1;
		y1 = y;
		y = x2;
		x2 = y2;
		y2 = y;
	}
	if (x1 > x2)
	{
		y = x1;
		x1 = x2;
		x2 = y;
		y = y1;
		y1 = y2;
		y2 = y;
	}
	dx = x2 - x1;
	dy = abs(y2 - y1);
	de = dx ? dy/(float)dx : 0.0f;
	y = y1;
	sy = (y1<y2) ? 1 : -1;
	for (x=x1; x<=x2; x++)
	{
		if (reverseXY)
			at(y, x);
		else
			at(x, y);
		e += de;
		if (e >= 0.5f)
		{
			y += sy;
			if (kind == LineKind::Deco ? singlePixel : (wallLine || singlePixel) && ((y1<y2) ? (y<=y2) : (y>=y2)))
			{
				if (reverseXY)
					at(y, x);
				else
					at(x, y);
			}
			e -= 1.0f;
		}
	}
}

void TptBrush::Impl::refresh(int index) {
    const Brush &b = *brushes[size_t(index)];
    ui::Point r = b.GetRadius();
    const int w = 2 * r.X + 1, h = 2 * r.Y + 1;
    bitmap.assign(size_t(w) * h, 0);
    for (ui::Point p : b) // (Brush's own iterator: the points it draws)
        bitmap[size_t(p.Y + r.Y) * w + size_t(p.X + r.X)] = 0xFF;
    // Brush::InitOutline's, from the same bitmap
    outline.assign(bitmap.size(), 0);
    auto at = [&](int i, int j) { return bitmap[size_t(j) * w + size_t(i)] != 0; };
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (at(i, j) && (i == 0 || j == 0 || i == w - 1 || j == h - 1 || !at(i + 1, j) || !at(i - 1, j) ||
                             !at(i, j + 1) || !at(i, j - 1)))
                outline[size_t(j) * w + size_t(i)] = 0xFF;
}

namespace {

std::string &bitmapFolder() {
    static std::string dir;
    return dir;
}

} // namespace

TptBrush::TptBrush(bool perfectCircle) : impl_(std::make_unique<Impl>()) {
    impl_->perfectCircle = perfectCircle;
    impl_->brushes.push_back(std::make_unique<EllipseBrush>(perfectCircle));
    impl_->brushes.push_back(std::make_unique<RectangleBrush>());
    impl_->brushes.push_back(std::make_unique<TriangleBrush>());
    impl_->brushes[0]->SetRadius(ui::Point(4, 4));
    impl_->refresh(0);
    if (!bitmapFolder().empty())
        loadBitmaps(bitmapFolder());
}

void TptBrush::setBitmapFolder(const std::string &dir) { bitmapFolder() = dir; }

void TptBrush::setPerfectCircle(bool on) {
    if (on == impl_->perfectCircle)
        return;
    impl_->perfectCircle = on;
    const ui::Point radius = impl_->brushes[size_t(index_)]->GetRadius();
    impl_->brushes[0] = std::make_unique<EllipseBrush>(on);
    impl_->brushes[0]->SetRadius(ui::Point(4, 4));
    impl_->brushes[size_t(index_)]->SetRadius(radius);
    impl_->refresh(index_);
}

bool TptBrush::perfectCircle() const { return impl_->perfectCircle; }

TptBrush::~TptBrush() = default;

TptBrush::TptBrush(const TptBrush &other) : impl_(std::make_unique<Impl>()), index_(other.index_) {
    impl_->perfectCircle = other.impl_->perfectCircle;
    for (const auto &b : other.impl_->brushes)
        impl_->brushes.push_back(b->Clone());
    impl_->bitmap = other.impl_->bitmap;
    impl_->outline = other.impl_->outline;
}

TptBrush &TptBrush::operator=(const TptBrush &other) {
    if (this != &other) {
        TptBrush copy(other);
        std::swap(impl_, copy.impl_);
        index_ = other.index_;
    }
    return *this;
}

int TptBrush::loadBitmaps(const std::string &dir) {
    // GameModel::BuildBrushList: the files in the folder (sorted here; TPT takes them in the folder's order)
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path().extension() == ".ptb")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    int added = 0;
    for (const auto &f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto dimension = size_t(std::sqrt(data.size()));
        if (data.empty() || dimension * dimension != data.size())
            continue; // TPT skips it: "Invalid bitmap size"
        impl_->brushes.push_back(std::make_unique<BitmapBrush>(ui::Point(int(dimension), int(dimension)),
                                                               reinterpret_cast<const unsigned char *>(data.data())));
        added++;
    }
    return added;
}

int TptBrush::count() const { return int(impl_->brushes.size()); }
int TptBrush::rx() const { return impl_->brushes[size_t(index_)]->GetRadius().X; }
int TptBrush::ry() const { return impl_->brushes[size_t(index_)]->GetRadius().Y; }
const std::vector<uint8_t> &TptBrush::bitmap() const { return impl_->bitmap; }
const std::vector<uint8_t> &TptBrush::outline() const { return impl_->outline; }

void TptBrush::setIndex(int i) {
    ui::Point radius = impl_->brushes[size_t(index_)]->GetRadius();
    int n = count();
    int next = ((i % n) + n) % n;
    if (next == index_)
        return;
    index_ = next;
    impl_->brushes[size_t(index_)]->SetRadius(radius);
    impl_->refresh(index_);
}

void TptBrush::setRadius(int rx, int ry) {
    Brush &b = *impl_->brushes[size_t(index_)];
    if (b.GetRadius() == ui::Point(rx, ry))
        return;
    b.SetRadius(ui::Point(rx, ry));
    impl_->refresh(index_);
}

void TptBrush::adjustSize(int delta, bool logarithmic, bool keepX, bool keepY) {
    Brush &b = *impl_->brushes[size_t(index_)];
    ui::Point old = b.GetRadius();
    b.AdjustSize(delta, logarithmic, keepX, keepY);
    if (b.GetRadius() != old)
        impl_->refresh(index_);
}

namespace {

// SimTool CYCL's perform (simulation/simtools/CYCL.cpp) on a cell's velocity, kept out of line so it is compiled as
// TPT compiles it there (GCC turns 1/sqrtf into an estimate and a refinement with these flags)
__attribute__((noipa)) void cyclonePerform(float *vx, float *vy, int x, int y, int brushX, int brushY, float strength)
{
	auto dvx = float(brushX - x);
	auto dvy = float(brushY - y);
	float invsqr = 1/sqrtf(dvx*dvx + dvy*dvy);

	*vx -= (strength / 16) * (-dvy)*invsqr;
	*vy -= (strength / 16) * dvx*invsqr;
}

} // namespace

void tptCycloneDelta(float strength, int brushX, int brushY, int x, int y, float &dvx, float &dvy) {
    float vx = 0.0f, vy = 0.0f;
    cyclonePerform(&vx, &vy, x, y, brushX, brushY, strength);
    dvx = vx;
    dvy = vy;
}

namespace {

// Simulation::ApplyDecoration (simulation/Editing.cpp) from where it has found the particle, on its dcolour
__attribute__((noipa)) uint32_t applyDecoration(uint32_t dcolour, int colR_, int colG_, int colB_, int colA_, int mode,
                                                const uint32_t *around, bool inside, int deco_space, bool &changed)
{
	float tr, tg, tb, ta, colR = float(colR_), colG = float(colG_), colB = float(colB_), colA = float(colA_);
	float strength = 0.01f;
	changed = true;

	ta = float((dcolour>>24)&0xFF);
	tr = float((dcolour>>16)&0xFF);
	tg = float((dcolour>>8)&0xFF);
	tb = float((dcolour)&0xFF);

	ta /= 255.0f; tr /= 255.0f; tg /= 255.0f; tb /= 255.0f;
	colR /= 255.0f; colG /= 255.0f; colB /= 255.0f; colA /= 255.0f;

	if (mode == DECO_DRAW)
	{
		ta = colA;
		tr = colR;
		tg = colG;
		tb = colB;
	}
	else if (mode == DECO_CLEAR)
	{
		ta = tr = tg = tb = 0.0f;
	}
	else if (mode == DECO_ADD)
	{
		//ta += (colA*strength)*colA;
		tr += (colR*strength)*colA;
		tg += (colG*strength)*colA;
		tb += (colB*strength)*colA;
	}
	else if (mode == DECO_SUBTRACT)
	{
		//ta -= (colA*strength)*colA;
		tr -= (colR*strength)*colA;
		tg -= (colG*strength)*colA;
		tb -= (colB*strength)*colA;
	}
	else if (mode == DECO_MULTIPLY)
	{
		tr *= 1.0f+(colR*strength)*colA;
		tg *= 1.0f+(colG*strength)*colA;
		tb *= 1.0f+(colB*strength)*colA;
	}
	else if (mode == DECO_DIVIDE)
	{
		tr /= 1.0f+(colR*strength)*colA;
		tg /= 1.0f+(colG*strength)*colA;
		tb /= 1.0f+(colB*strength)*colA;
	}
	else if (mode == DECO_SMUDGE)
	{
		if (inside)
		{
			float tas = 0.0f, trs = 0.0f, tgs = 0.0f, tbs = 0.0f;

			int rx, ry;
			float num = 0;
			for (rx=-2; rx<3; rx++)
				for (ry=-2; ry<3; ry++)
				{
					uint32_t partDcolour = around[(rx + 2) * 5 + (ry + 2)];
					if (abs(rx)+abs(ry) > 2 && partDcolour)
					{
						num += 1.0f;
						float pa = ((float)((partDcolour>>24)&0xFF)) / 255.f;
						float pr = ((float)((partDcolour>>16)&0xFF)) / 255.f;
						float pg = ((float)((partDcolour>> 8)&0xFF)) / 255.f;
						float pb = ((float)((partDcolour    )&0xFF)) / 255.f;
						switch (deco_space)
						{
						case DECOSPACE_SRGB:
							pa = (pa <= 0.04045f) ? (pa / 12.92f) : pow((pa + 0.055f) / 1.055f, 2.4f);
							pr = (pr <= 0.04045f) ? (pr / 12.92f) : pow((pr + 0.055f) / 1.055f, 2.4f);
							pg = (pg <= 0.04045f) ? (pg / 12.92f) : pow((pg + 0.055f) / 1.055f, 2.4f);
							pb = (pb <= 0.04045f) ? (pb / 12.92f) : pow((pb + 0.055f) / 1.055f, 2.4f);
							break;

						case DECOSPACE_LINEAR:
							break;

						case DECOSPACE_GAMMA22:
							pa = pow(pa, 2.2f);
							pr = pow(pr, 2.2f);
							pg = pow(pg, 2.2f);
							pb = pow(pb, 2.2f);
							break;

						case DECOSPACE_GAMMA18:
							pa = pow(pa, 1.8f);
							pr = pow(pr, 1.8f);
							pg = pow(pg, 1.8f);
							pb = pow(pb, 1.8f);
							break;
						}
						tas += pa;
						trs += pr;
						tgs += pg;
						tbs += pb;
					}
				}
			if (num == 0)
			{
				changed = false;
				return dcolour;
			}
			ta = tas / num;
			tr = trs / num;
			tg = tgs / num;
			tb = tbs / num;
			switch (deco_space)
			{
			case DECOSPACE_SRGB:
				ta = (ta <= 0.0031308f) ? (ta * 12.92f) : (1.055f * pow(ta, 1.f / 2.4f) - 0.055f);
				tr = (tr <= 0.0031308f) ? (tr * 12.92f) : (1.055f * pow(tr, 1.f / 2.4f) - 0.055f);
				tg = (tg <= 0.0031308f) ? (tg * 12.92f) : (1.055f * pow(tg, 1.f / 2.4f) - 0.055f);
				tb = (tb <= 0.0031308f) ? (tb * 12.92f) : (1.055f * pow(tb, 1.f / 2.4f) - 0.055f);
				break;

			case DECOSPACE_LINEAR:
				break;

			case DECOSPACE_GAMMA22:
				ta = pow(ta, 1.f / 2.2f);
				tr = pow(tr, 1.f / 2.2f);
				tg = pow(tg, 1.f / 2.2f);
				tb = pow(tb, 1.f / 2.2f);
				break;

			case DECOSPACE_GAMMA18:
				ta = pow(ta, 1.f / 1.8f);
				tr = pow(tr, 1.f / 1.8f);
				tg = pow(tg, 1.f / 1.8f);
				tb = pow(tb, 1.f / 1.8f);
				break;
			}
			if (!dcolour)
				ta -= 3/255.0f;
		}
	}

	ta *= 255.0f; tr *= 255.0f; tg *= 255.0f; tb *= 255.0f;
	ta += .5f; tr += .5f; tg += .5f; tb += .5f;

	colA_ = int(ta);
	colR_ = int(tr);
	colG_ = int(tg);
	colB_ = int(tb);

	if(colA_ > 255)
		colA_ = 255;
	else if(colA_ < 0)
		colA_ = 0;
	if(colR_ > 255)
		colR_ = 255;
	else if(colR_ < 0)
		colR_ = 0;
	if(colG_ > 255)
		colG_ = 255;
	else if(colG_ < 0)
		colG_ = 0;
	if(colB_ > 255)
		colB_ = 255;
	else if(colB_ < 0)
		colB_ = 0;
	return ((colA_<<24)|(colR_<<16)|(colG_<<8)|colB_);
}

} // namespace

uint32_t tptDecorate(uint32_t dcolour, uint32_t colour, int mode) {
    static const uint32_t none[25] = {};
    bool changed;
    return applyDecoration(dcolour, int((colour >> 16) & 0xFF), int((colour >> 8) & 0xFF), int(colour & 0xFF),
                           int(colour >> 24), mode, none, false, DECOSPACE_SRGB, changed);
}

uint32_t tptSmudge(uint32_t dcolour, const uint32_t around[25], bool inside, int decoSpace, bool &changed) {
    return applyDecoration(dcolour, 0, 0, 0, 0, DECO_SMUDGE, around, inside, decoSpace, changed);
}

void tptHsvToRgb(int h, int s, int v, int &r, int &g, int &b) { HSV_to_RGB(h, s, v, &r, &g, &b); }
void tptRgbToHsv(int r, int g, int b, int &h, int &s, int &v) { RGB_to_HSV(r, g, b, &h, &s, &v); }

unsigned tptClampFlt(float f, float min, float max) { return clamp_flt(f, min, max); }

std::u32string tptColourHex(int a, int r, int g, int b) {
    String s = String::Build(Format::Hex(), Format::Uppercase(), Format::Width(2), a, r, g, b);
    return std::u32string(s.begin(), s.end());
}

std::vector<uint32_t> tptGradient(uint32_t from, uint32_t to, int resolution) {
    std::vector<uint32_t> out;
    for (RGB c : Gradient({{RGB::Unpack(from), 0.f}, {RGB::Unpack(to), 1.f}}, resolution))
        out.push_back(c.Pack() & 0xFFFFFF);
    return out;
}

int tptSandColour(uint64_t frame) { return int(20.0f * sin(float(frame) * std::numbers::pi_v<float> / 180.0f)); }

std::u32string tptTemperatureText(float temp, int temperatureScale) {
    StringBuilder sb;
    sb << Format::Precision(2);
    format::RenderTemperature(sb, temp, TempScale(temperatureScale));
    String out = sb.Build();
    return std::u32string(out.begin(), out.end());
}

bool tptParseTemperature(const std::u32string &text, int temperatureScale, float &out) {
    try {
        out = format::StringToTemperature(String(text.data(), text.size()), TempScale(temperatureScale));
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool tptParseNumber(const std::u32string &text, float &out) {
    try {
        out = String(text.data(), text.size()).ToNumber<float>();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool tptParseNumber(const std::u32string &text, int &out) {
    try {
        out = String(text.data(), text.size()).ToNumber<int>();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

uint32_t tptHeatColour(float temp) {
    Renderer::PopulateTables();
    return HeatToColour(temp, MIN_TEMP, MAX_TEMP).Pack() & 0xFFFFFF;
}

uint32_t tptPressureColour(float pressure) { return PressureToColour(pressure).Pack() & 0xFFFFFF; }

std::vector<char> tptPng(const uint32_t *pixels, int w, int h) {
    std::vector<pixel> px(pixels, pixels + size_t(w) * size_t(h));
    for (auto &p : px)
        p &= 0xFFFFFF;
    auto data = VideoBuffer(px.data(), Vec2<int>(w, h)).ToPNG();
    return data ? std::move(*data) : std::vector<char>{};
}

std::string tptDate(int64_t unixTime, const std::string &format) {
    return format::UnixtimeToDate(time_t(unixTime), ByteString(format)).c_str();
}

std::vector<uint32_t> tptImageFromPng(const std::vector<char> &png, int w, int h, int &outW, int &outH) {
    auto vb = VideoBuffer::FromPNG(std::span<const char>(png.data(), png.size()));
    if (vb)
        vb->Resize(Vec2<int>(w, h), true);
    else {
        vb = std::make_unique<VideoBuffer>(Vec2(15, 16));
        vb->BlendChar(Vec2(2, 4), 0xE06E, 0xFFFFFF_rgb .WithAlpha(0xFF));
    }
    outW = vb->Size().X, outH = vb->Size().Y;
    return std::vector<uint32_t>(vb->Data(), vb->Data() + size_t(outW) * size_t(outH));
}

std::string tptDateMini(int64_t unixTime) { return format::UnixtimeToDateMini(time_t(unixTime)).c_str(); }

} // namespace bp
