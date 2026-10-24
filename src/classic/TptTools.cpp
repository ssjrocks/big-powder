// TPT's brushes for TptBrush, built with TPT's settings (see ClassicSim.h).
#include "classic/TptToolsImpl.h"

#include "gui/game/BitmapBrush.h"
#include "gui/game/EllipseBrush.h"
#include "gui/game/RectangleBrush.h"
#include "gui/game/TriangleBrush.h"

#include <algorithm>
#include <cmath>
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
void tptLinePoints(int x1, int y1, int x2, int y2, bool wallLine, bool singlePixel, std::vector<int> &out)
{
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
			if ((wallLine || singlePixel) && ((y1<y2) ? (y<=y2) : (y>=y2)))
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

TptBrush::TptBrush(bool perfectCircle) : impl_(std::make_unique<Impl>()) {
    impl_->perfectCircle = perfectCircle;
    impl_->brushes.push_back(std::make_unique<EllipseBrush>(perfectCircle));
    impl_->brushes.push_back(std::make_unique<RectangleBrush>());
    impl_->brushes.push_back(std::make_unique<TriangleBrush>());
    impl_->brushes[0]->SetRadius(ui::Point(4, 4));
    impl_->refresh(0);
}

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

} // namespace bp
