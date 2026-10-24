#include "sim/Editing.h"
#include "sim/ElementIds.h"

#include <algorithm>

namespace bp {

namespace {

constexpr int PMAPBITS = 9;
int typ(int c) { return c & ((1 << PMAPBITS) - 1); }
int pmap(int v, int t) { return (v << PMAPBITS) | t; }

} // namespace

GpuEditor::GpuEditor(VkContext &ctx, GpuSim &sim)
    : ctx_(ctx), sim_(sim), W_(int(sim.width())), H_(int(sim.height())), CW_(W_ / CELL), CH_(H_ / CELL),
      walls_(sim.editWalls()), erased_(W_, H_) {
    wallEditAt_.assign(size_t(CW_) * CH_, 0);
}

void GpuEditor::PixelCounts::span(int y, int xa, int xb, int times) {
    if (y < 0 || y >= H_)
        return;
    xa = std::max(xa, 0);
    xb = std::min(xb, W_ - 1);
    if (xa > xb)
        return;
    spans_.push_back({y, xa, xb, times});
    x0_ = std::min(x0_, xa);
    x1_ = std::max(x1_, xb);
    y0_ = std::min(y0_, y);
    y1_ = std::max(y1_, y);
}

PixelEdit GpuEditor::PixelCounts::edit(int type, uint32_t flags, int selected) const {
    PixelEdit e;
    e.type = type;
    e.flags = flags;
    e.selected = selected;
    if (spans_.empty())
        return e;
    e.x0 = x0_, e.y0 = y0_, e.w = x1_ - x0_ + 1, e.h = y1_ - y0_ + 1;
    e.counts.assign((size_t(e.w) * e.h + 15) / 16, 0);
    // The spans row by row (a counting sort), then each row's counts from a difference array. Two calls are all
    // that can matter (spawn.slang).
    std::vector<int> start(size_t(e.h) + 1, 0);
    for (const Span &s : spans_)
        start[size_t(s.y - y0_) + 1]++;
    for (int r = 0; r < e.h; r++)
        start[size_t(r) + 1] += start[size_t(r)];
    std::vector<int> order(spans_.size()), next(start.begin(), start.end() - 1);
    for (size_t k = 0; k < spans_.size(); k++)
        order[size_t(next[size_t(spans_[k].y - y0_)]++)] = int(k);
    std::vector<int> diff(size_t(e.w) + 1);
    for (int r = 0; r < e.h; r++) {
        if (start[size_t(r)] == start[size_t(r) + 1])
            continue;
        std::fill(diff.begin(), diff.end(), 0);
        for (int k = start[size_t(r)]; k < start[size_t(r) + 1]; k++) {
            const Span &s = spans_[size_t(order[size_t(k)])];
            diff[size_t(s.xa - x0_)] += s.times;
            diff[size_t(s.xb - x0_) + 1] -= s.times;
        }
        int n = 0;
        for (int x = 0; x < e.w; x++) {
            n += diff[size_t(x)];
            if (n > 0) {
                size_t i = size_t(r) * size_t(e.w) + size_t(x);
                e.counts[i >> 4] |= uint32_t(std::min(n, 2)) << ((i & 15) * 2);
            }
        }
    }
    return e;
}

bool GpuEditor::needsWorld(const std::vector<ToolUse> &uses) {
    for (const ToolUse &u : uses)
        if (u.kind == ToolUse::Kind::Element && u.action == ToolUse::Action::DrawFill)
            return true;
    return false;
}

void GpuEditor::apply(VkCommandBuffer cmd, const std::vector<ToolUse> &uses) {
    if (uses.empty())
        return;
    immediate_ = needsWorld(uses);
    if (immediate_)
        vkDeviceWaitIdle(ctx_.device());
    for (const ToolUse &u : uses)
        use(u);
    flush(cmd);
}

void GpuEditor::flush(VkCommandBuffer cmd) {
    if (steps_.empty())
        return;
    VkDeviceSize bytes = 0;
    for (const Step &s : steps_) {
        if (auto *p = std::get_if<std::vector<PixelEdit>>(&s))
            bytes = std::max(bytes, GpuSim::editBytes(*p));
        else
            bytes = std::max(bytes, GpuSim::editBytes(std::get<WallStep>(s).edits));
    }
    sim_.reserveEditSpace(bytes);
    auto record = [&](VkCommandBuffer c) {
        for (const Step &s : steps_) {
            if (auto *p = std::get_if<std::vector<PixelEdit>>(&s))
                sim_.recordEdits(c, *p);
            else
                sim_.recordWallEdits(c, std::get<WallStep>(s).edits, std::get<WallStep>(s).gravity);
        }
    };
    if (immediate_)
        ctx_.immediate(record);
    else
        record(cmd);
    steps_.clear();
}

void GpuEditor::addPixels(PixelEdit &&e) {
    if (e.w <= 0 || e.h <= 0)
        return;
    if (steps_.empty() || !std::holds_alternative<std::vector<PixelEdit>>(steps_.back()))
        steps_.emplace_back(std::vector<PixelEdit>{});
    std::get<std::vector<PixelEdit>>(steps_.back()).push_back(std::move(e));
}

void GpuEditor::use(const ToolUse &u) {
    int index = brush_.index(), rx = brush_.rx(), ry = brush_.ry();
    brush_.setIndex(u.brush);
    brush_.setRadius(u.rx, u.ry);
    if (brushRows_.empty() || brush_.index() != index || brush_.rx() != rx || brush_.ry() != ry) {
        // the brush's points as runs, row by row (x relative to the centre)
        const int w = 2 * brush_.rx() + 1, h = 2 * brush_.ry() + 1;
        const std::vector<uint8_t> &bm = brush_.bitmap();
        brushRows_.assign(size_t(h), {});
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w;) {
                if (!bm[size_t(j) * w + size_t(i)]) {
                    i++;
                    continue;
                }
                int a = i;
                while (i < w && bm[size_t(j) * w + size_t(i)])
                    i++;
                brushRows_[size_t(j)].push_back({a - brush_.rx(), i - 1 - brush_.rx()});
            }
    }
    if (u.kind == ToolUse::Kind::Wall)
        useWall(u);
    else
        useElement(u);
}

// Simulation::CreateParts' loop over the brush (CreatePartFlags at each of its points in the world)
void GpuEditor::stamp(PixelCounts &counts, int x, int y) {
    const int ry = brush_.ry();
    for (size_t j = 0; j < brushRows_.size(); j++)
        for (const auto &run : brushRows_[j])
            counts.span(y + int(j) - ry, x + run[0], x + run[1]);
}

// Simulation::CreateLine: CreateParts with the brush along the line
void GpuEditor::createLine(PixelCounts &counts, int x1, int y1, int x2, int y2) {
    std::vector<int> points;
    tptLinePoints(x1, y1, x2, y2, false, brush_.rx() + brush_.ry() == 0, points);
    for (size_t k = 0; k + 1 < points.size(); k += 2)
        stamp(counts, points[k], points[k + 1]);
}

// The element tools: ElementTool, and for LIGH, TESC and the stick men their own
void GpuEditor::useElement(const ToolUse &u) {
    using A = ToolUse::Action;
    const int t = typ(u.tool), rx = brush_.rx(), ry = brush_.ry();
    const uint32_t flags = uint32_t(u.replaceFlags);
    const int selected = u.replaceSelected;
    if (t == PT_STKM || t == PT_STKM2 || t == PT_FIGH) {
        // PlopTool: one, where the button was let go (create_part, not CreatePartFlags)
        if (u.action == A::Click) {
            PixelCounts one(W_, H_);
            one.span(u.y1, u.x1, u.x1);
            addPixels(one.edit(u.tool, 0, 0));
        }
        return;
    }
    if (u.tool == PT_LIGH) {
        // Element_LIGH_Tool: CreateParts' lightning, one bolt at a time at the brush's centre, as strong as the
        // brush is big, and not again until a few ticks have passed; dragging makes it at the last point
        if (u.action != A::Draw && !(u.action == A::DrawLine && u.dragging))
            return;
        if (int64_t(sim_.tick()) < lightningRecreate_)
            return;
        int newlife = std::min(rx + ry, 55);
        lightningRecreate_ = int64_t(sim_.tick()) + std::max(newlife / 4, 1);
        PixelCounts one(W_, H_);
        one.span(u.y1, u.x1, u.x1);
        addPixels(one.edit(pmap(newlife, PT_LIGH), flags, selected));
        return;
    }
    // CreateParts with the brush gives TESC its range from the brush's size; its tool's boxes and fills do so too,
    // uncapped (TESC's create caps it)
    const int brushType = u.tool == PT_TESC ? pmap(std::min(rx * 4 + ry * 4 + 7, 300), PT_TESC) : u.tool;
    const int areaType = u.tool == PT_TESC ? pmap(rx * 4 + ry * 4 + 7, PT_TESC) : u.tool;
    PixelCounts counts(W_, H_);
    switch (u.action) {
    case A::Draw:
        stamp(counts, u.x1, u.y1);
        addPixels(counts.edit(brushType, flags, selected));
        break;
    case A::DrawLine:
        createLine(counts, u.x1, u.y1, u.x2, u.y2);
        addPixels(counts.edit(brushType, flags, selected));
        break;
    case A::DrawRect: {
        // Simulation::CreateBox: each pixel once, whatever the brush
        int x1 = std::min(u.x1, u.x2), x2 = std::max(u.x1, u.x2), y1 = std::min(u.y1, u.y2), y2 = std::max(u.y1, u.y2);
        for (int y = y1; y <= y2; y++)
            counts.span(y, x1, x2);
        addPixels(counts.edit(areaType, flags, selected));
        break;
    }
    case A::DrawFill:
        floodParts(u.x1, u.y1, areaType, int(flags), selected);
        break;
    case A::Click:
        break;
    }
}

// Simulation::FloodParts: a span fill over what its check passes, read back from the GPU. With something to draw it
// keeps inside the border and creates (CreateParts with no brush) at each pixel it reaches; with nothing it removes
// the particles of the type it started on, or, if it started on nothing but a wall, flood-erases the walls.
void GpuEditor::floodParts(int x, int y, int fullc, int flags, int selected) {
    const int c = typ(fullc);
    if (c != 0 && (x < CELL || x >= W_ - CELL || y < CELL || y >= H_ - CELL || c == PT_SPRK))
        return;
    if (x < 0 || x >= W_ || y < 0 || y >= H_)
        return;
    flush(VK_NULL_HANDLE); // (what was drawn before is in the world to check)
    int cm;
    std::vector<uint32_t> bits = sim_.readFillMap(x, y, c, cm);
    if (c == 0 && cm < 0) {
        if (bmap(x, y)) {
            beginWalls();
            floodWalls(x, y, WL_ERASE, -1);
            finishWalls();
        }
        return;
    }
    auto ok = [&](int px, int py) {
        size_t i = size_t(py) * size_t(W_) + size_t(px);
        return ((bits[i >> 5] >> (i & 31)) & 1) != 0;
    };
    if (!ok(x, y))
        return;
    // TPT's span fill pushes every pixel of the rows above and below that it could go on to; pushing one per run of
    // them reaches the same pixels (those joined to the start, side by side or one above the other), in far fewer
    // steps
    std::vector<uint64_t> seen((size_t(W_) * size_t(H_) + 63) / 64, 0);
    auto open = [&](int px, int py) {
        size_t i = size_t(py) * size_t(W_) + size_t(px);
        return ((seen[i >> 6] >> (i & 63)) & 1) == 0 && ((bits[i >> 5] >> (i & 31)) & 1) != 0;
    };
    const int xMin = c ? CELL : 0, xMax = c ? W_ - CELL - 1 : W_ - 1;
    const int yMin = c ? CELL : 0, yMax = c ? H_ - CELL - 1 : H_ - 1;
    PixelCounts counts(W_, H_);
    std::vector<std::array<int, 2>> stack{{x, y}};
    while (!stack.empty()) {
        auto [sx, sy] = stack.back();
        stack.pop_back();
        if (!open(sx, sy))
            continue;
        int x1 = sx, x2 = sx;
        while (x1 > xMin && open(x1 - 1, sy))
            x1--;
        while (x2 < xMax && open(x2 + 1, sy))
            x2++;
        counts.span(sy, x1, x2);
        for (int px = x1; px <= x2; px++) {
            size_t i = size_t(sy) * size_t(W_) + size_t(px);
            seen[i >> 6] |= uint64_t(1) << (i & 63);
        }
        for (int ny : {sy - 1, sy + 1}) {
            if (ny < yMin || ny > yMax)
                continue;
            for (int px = x1; px <= x2; px++)
                if (open(px, ny)) {
                    stack.push_back({px, ny});
                    while (px < x2 && open(px + 1, ny))
                        px++;
                }
        }
    }
    if (c == 0)
        addPixels(counts.edit(0, EDIT_FLOOD_KILL, cm));
    else
        addPixels(counts.edit(fullc, uint32_t(flags), selected));
}

// WallTool
void GpuEditor::useWall(const ToolUse &u) {
    using A = ToolUse::Action;
    const int wall = u.tool;
    beginWalls();
    switch (u.action) {
    case A::Draw:
        createWalls(u.x1, u.y1, 1, 1, wall, true);
        break;
    case A::DrawLine:
        if (!u.dragging && wall == WL_FAN && bmap(u.x1, u.y1) == WL_FAN) {
            // a line drawn from a fan sets the velocity of the whole fan
            float vx = float(u.x2 - u.x1) * 0.005f;
            vx *= u.strength;
            float vy = float(u.y2 - u.y1) * 0.005f;
            vy *= u.strength;
            floodWalls(u.x1, u.y1, WL_FLOODHELPER, WL_FAN);
            for (int cell = 0; cell < CW_ * CH_; cell++)
                if (walls_[size_t(cell)] == WL_FLOODHELPER) {
                    WallEdit &e = wallEdit(uint32_t(cell));
                    e.fan = true;
                    e.fvx = vx;
                    e.fvy = vy;
                    setWall(uint32_t(cell), WL_FAN);
                }
        } else {
            // Simulation::CreateWallLine
            std::vector<int> points;
            tptLinePoints(u.x1, u.y1, u.x2, u.y2, true, false, points);
            for (size_t k = 0; k + 1 < points.size(); k += 2)
                createWalls(points[k], points[k + 1], 1, 1, wall, true);
        }
        break;
    case A::DrawRect:
        createWallBox(u.x1, u.y1, u.x2, u.y2, wall);
        break;
    case A::DrawFill:
        if (wall != WL_STREAM)
            floodWalls(u.x1, u.y1, wall, -1);
        break;
    case A::Click:
        break;
    }
    finishWalls();
}

WallEdit &GpuEditor::wallEdit(uint32_t cell) {
    int &at = wallEditAt_[cell];
    if (!at) {
        WallEdit e;
        e.cell = cell;
        wallEdits_.push_back(e);
        at = int(wallEdits_.size());
    }
    return wallEdits_[size_t(at) - 1];
}

void GpuEditor::setWall(uint32_t cell, int wall) {
    walls_[cell] = uint8_t(wall);
    wallEdit(cell).type = true;
}

void GpuEditor::beginWalls() {
    wallEdits_.clear();
    gravityWalls_ = signsChanged_ = false;
    erased_ = PixelCounts(W_, H_);
}

void GpuEditor::finishWalls() {
    for (const WallEdit &e : wallEdits_)
        wallEditAt_[e.cell] = 0;
    if (!wallEdits_.empty()) {
        WallStep step;
        step.edits = std::move(wallEdits_);
        step.gravity = gravityWalls_;
        steps_.emplace_back(std::move(step));
    }
    wallEdits_.clear();
    if (!erased_.empty())
        addPixels(erased_.edit(0, 0, 0)); // (delete_part)
    if (signsChanged_)
        sim_.setSigns(signs_);
}

// Simulation::CreateWalls: a box of cells around the one at (x, y), its half sizes the radius in cells
void GpuEditor::createWalls(int x, int y, int rx, int ry, int wall, bool brush) {
    if (brush) {
        rx = brush_.rx();
        ry = brush_.ry();
    }
    ry = ry / CELL;
    rx = rx / CELL;
    x = x / CELL;
    y = y / CELL;
    x -= rx;
    y -= ry;
    for (int wallX = x; wallX <= x + rx + rx; wallX++) {
        for (int wallY = y; wallY <= y + ry + ry; wallY++) {
            if (wallX >= 0 && wallX < CW_ && wallY >= 0 && wallY < CH_) {
                if (wall == WL_FAN) {
                    WallEdit &e = wallEdit(uint32_t(wallY * CW_ + wallX));
                    e.fan = true;
                    e.fvx = e.fvy = 0.0f;
                } else if (wall == WL_STREAM) {
                    wallX = x + rx;
                    wallY = y + ry;
                    // streamlines can't be drawn next to each other
                    for (int tempY = wallY - 1; tempY < wallY + 2; tempY++)
                        for (int tempX = wallX - 1; tempX < wallX + 2; tempX++)
                            if (tempX >= 0 && tempX < CW_ && tempY >= 0 && tempY < CH_ &&
                                walls_[size_t(tempY) * CW_ + size_t(tempX)] == WL_STREAM)
                                return;
                }
                const uint32_t cell = uint32_t(wallY * CW_ + wallX);
                if (wall == WL_GRAV || walls_[cell] == WL_GRAV)
                    gravityWalls_ = true;
                if (wall == WL_ERASEALL) {
                    for (int j = 0; j < CELL; j++)
                        erased_.span(wallY * CELL + j, wallX * CELL, wallX * CELL + CELL - 1);
                    if (!signsChanged_)
                        signs_ = sim_.signs();
                    for (int i = int(signs_.size()) - 1; i >= 0; i--) {
                        const Sign &s = signs_[size_t(i)];
                        if (s.x >= wallX * CELL && s.y >= wallY * CELL && s.x <= (wallX + 1) * CELL &&
                            s.y <= (wallY + 1) * CELL) {
                            signs_.erase(signs_.begin() + i);
                            signsChanged_ = true;
                        }
                    }
                    setWall(cell, 0);
                } else
                    setWall(cell, wall);
            }
        }
    }
}

// Simulation::CreateWallBox: CreateWalls at every pixel of the box. Done cell by cell (with how many of the box's
// pixels each has), except for streamlines, which depend on what was drawn just before.
void GpuEditor::createWallBox(int x1, int y1, int x2, int y2, int wall) {
    if (x1 > x2)
        std::swap(x1, x2);
    if (y1 > y2)
        std::swap(y1, y2);
    if (wall == WL_STREAM) {
        for (int j = y1; j <= y2; j++)
            for (int i = x1; i <= x2; i++)
                createWalls(i, j, 0, 0, wall, false);
        return;
    }
    for (int cy = y1 / CELL; cy <= y2 / CELL; cy++)
        for (int cx = x1 / CELL; cx <= x2 / CELL; cx++) {
            int nx = std::min(x2, cx * CELL + CELL - 1) - std::max(x1, cx * CELL) + 1;
            int ny = std::min(y2, cy * CELL + CELL - 1) - std::max(y1, cy * CELL) + 1;
            int times = nx * ny;
            for (int k = 0; k < std::min(times, 2); k++) // (twice is all an erasing can use)
                createWalls(cx * CELL, cy * CELL, 0, 0, wall, false);
        }
}

// Simulation::FloodWalls, with its recursion kept on a stack of its own (TPT's would run out of stack in a big world)
void GpuEditor::floodWalls(int x, int y, int wall, int bm) {
    if (bm == -1) {
        if (wall == WL_ERASE || wall == WL_ERASEALL) {
            bm = bmap(x, y);
            if (!bm)
                return;
        } else
            bm = 0;
    }
    if (bmap(x, y) != bm)
        return;
    struct Frame {
        int x1, x2, y, xi;
        bool down;
    };
    std::vector<Frame> stack;
    // FloodWalls up to its children: go left and right as far as possible, fill that span
    auto enter = [&](int sx, int sy) {
        int x1 = sx, x2 = sx;
        while (x1 >= CELL) {
            if (bmap(x1 - 1, sy) != bm)
                break;
            x1--;
        }
        while (x2 < W_ - CELL) {
            if (bmap(x2 + 1, sy) != bm)
                break;
            x2++;
        }
        for (int px = x1; px <= x2; px++)
            createWalls(px, sy, 0, 0, wall, false);
        stack.push_back({x1, x2, sy, x1, false});
    };
    enter(x, y);
    while (!stack.empty()) {
        Frame f = stack.back();
        // the children: first the row above, then the row below
        int cy = f.down ? f.y + CELL : f.y - CELL;
        bool rowExists = f.down ? f.y < H_ - CELL : f.y >= CELL;
        if (!rowExists || f.xi > f.x2) {
            if (f.down) {
                stack.pop_back();
            } else {
                stack.back().down = true;
                stack.back().xi = f.x1;
            }
            continue;
        }
        stack.back().xi++;
        if (bmap(f.xi, cy) == bm)
            enter(f.xi, cy);
    }
}

} // namespace bp
