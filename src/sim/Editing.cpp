#include "sim/Editing.h"
#include "sim/ElementIds.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

namespace bp {

namespace {

constexpr int PMAPBITS = 9;
constexpr uint32_t TOOL_PROPERTY = 100; // (sim_tool.slang)
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

template <class F>
void GpuEditor::PixelCounts::rows(F &&f) const {
    if (spans_.empty())
        return;
    const int w = x1_ - x0_ + 1, h = y1_ - y0_ + 1;
    // The spans row by row (a counting sort), then each row's counts from a difference array
    std::vector<int> start(size_t(h) + 1, 0);
    for (const Span &s : spans_)
        start[size_t(s.y - y0_) + 1]++;
    for (int r = 0; r < h; r++)
        start[size_t(r) + 1] += start[size_t(r)];
    std::vector<int> order(spans_.size()), next(start.begin(), start.end() - 1);
    for (size_t k = 0; k < spans_.size(); k++)
        order[size_t(next[size_t(spans_[k].y - y0_)]++)] = int(k);
    std::vector<int> diff(size_t(w) + 1);
    std::vector<int> counts(static_cast<size_t>(w));
    for (int r = 0; r < h; r++) {
        if (start[size_t(r)] == start[size_t(r) + 1])
            continue;
        std::fill(diff.begin(), diff.end(), 0);
        for (int k = start[size_t(r)]; k < start[size_t(r) + 1]; k++) {
            const Span &s = spans_[size_t(order[size_t(k)])];
            diff[size_t(s.xa - x0_)] += s.times;
            diff[size_t(s.xb - x0_) + 1] -= s.times;
        }
        int n = 0;
        for (int x = 0; x < w; x++) {
            n += diff[size_t(x)];
            counts[size_t(x)] = n;
        }
        f(r, counts);
    }
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
    // Two calls are all that can matter (spawn.slang)
    rows([&](int r, const std::vector<int> &counts) {
        for (int x = 0; x < e.w; x++)
            if (counts[size_t(x)] > 0) {
                size_t i = size_t(r) * size_t(e.w) + size_t(x);
                e.counts[i >> 4] |= uint32_t(std::min(counts[size_t(x)], 2)) << ((i & 15) * 2);
            }
    });
    return e;
}

bool GpuEditor::PixelCounts::packed(int bits, std::vector<uint32_t> &out, int &x0, int &y0, int &w, int &h) const {
    if (spans_.empty())
        return false;
    x0 = x0_, y0 = y0_, w = x1_ - x0_ + 1, h = y1_ - y0_ + 1;
    const size_t perWord = size_t(32 / bits), head = out.size();
    const uint32_t most = bits == 16 ? 0xFFFF : 3;
    out.resize(head + (size_t(w) * size_t(h) + perWord - 1) / perWord, 0);
    const int bw = w;
    rows([&](int r, const std::vector<int> &counts) {
        for (int x = 0; x < bw; x++)
            if (counts[size_t(x)] > 0) {
                size_t i = size_t(r) * size_t(bw) + size_t(x);
                out[head + i / perWord] |= std::min(uint32_t(counts[size_t(x)]), most) << ((i % perWord) * size_t(bits));
            }
    });
    return true;
}

ToolEdit GpuEditor::PixelCounts::toolEdit(uint32_t tool, int bits, uint32_t p0, uint32_t p1, uint32_t p2) const {
    ToolEdit e;
    int x0, y0, w, h;
    std::vector<uint32_t> counts;
    if (!packed(bits, counts, x0, y0, w, h))
        return e;
    e.words = {2 /* OP_PIXELS */, tool, uint32_t(x0), uint32_t(y0), uint32_t(w), uint32_t(h), uint32_t(bits), p0, p1, p2};
    e.words.insert(e.words.end(), counts.begin(), counts.end());
    e.w = uint32_t(w);
    e.h = uint32_t(h);
    return e;
}

std::vector<uint32_t> GpuEditor::PixelCounts::cellCounts() const {
    std::vector<uint32_t> out;
    if (spans_.empty())
        return out;
    const int CW = W_ / CELL;
    const int cx0 = x0_ / CELL, cy0 = y0_ / CELL, cw = x1_ / CELL - cx0 + 1, ch = y1_ / CELL - cy0 + 1;
    std::vector<uint64_t> sums(size_t(cw) * size_t(ch), 0);
    rows([&](int r, const std::vector<int> &counts) {
        const int y = y0_ + r;
        for (size_t x = 0; x < counts.size(); x++)
            sums[size_t(y / CELL - cy0) * size_t(cw) + size_t((x0_ + int(x)) / CELL - cx0)] += uint64_t(counts[x]);
    });
    for (int cy = 0; cy < ch; cy++)
        for (int cx = 0; cx < cw; cx++)
            if (uint64_t n = sums[size_t(cy) * size_t(cw) + size_t(cx)])
                out.insert(out.end(), {uint32_t((cy0 + cy) * CW + cx0 + cx), uint32_t(std::min<uint64_t>(n, 0xFFFFFFFFu))});
    return out;
}

bool GpuEditor::needsWorld(const std::vector<ToolUse> &uses) {
    for (const ToolUse &u : uses) {
        if ((u.kind == ToolUse::Kind::Element || u.kind == ToolUse::Kind::Property || u.kind == ToolUse::Kind::Deco) &&
            u.action == ToolUse::Action::DrawFill)
            return true;
        if (u.kind == ToolUse::Kind::Deco && u.tool == DECO_SMUDGE)
            return true;
    }
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
        else if (auto *t = std::get_if<std::vector<ToolEdit>>(&s))
            bytes = std::max(bytes, GpuSim::editBytes(*t));
        else
            bytes = std::max(bytes, GpuSim::editBytes(std::get<WallStep>(s).edits));
    }
    sim_.reserveEditSpace(bytes);
    auto record = [&](VkCommandBuffer c) {
        for (const Step &s : steps_) {
            if (auto *p = std::get_if<std::vector<PixelEdit>>(&s))
                sim_.recordEdits(c, *p);
            else if (auto *t = std::get_if<std::vector<ToolEdit>>(&s))
                sim_.recordToolEdits(c, *t);
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

void GpuEditor::addTool(ToolEdit &&e) {
    if (e.words.empty())
        return;
    if (steps_.empty() || !std::holds_alternative<std::vector<ToolEdit>>(steps_.back()))
        steps_.emplace_back(std::vector<ToolEdit>{});
    std::get<std::vector<ToolEdit>>(steps_.back()).push_back(std::move(e));
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
    if (u.action == ToolUse::Action::Create) {
        // create_part(-1, ...): only a spark is made this way (a button sign's)
        if (u.kind == ToolUse::Kind::Element && typ(u.tool) == PT_SPRK) {
            PixelCounts one(W_, H_);
            one.span(u.y1, u.x1, u.x1);
            addPixels(one.edit(PT_SPRK, EDIT_SPARK, 0));
        }
        return;
    }
    switch (u.kind) {
    case ToolUse::Kind::Wall: useWall(u); break;
    case ToolUse::Kind::Element: useElement(u); break;
    case ToolUse::Kind::Sim: useSim(u); break;
    case ToolUse::Kind::Property: useProperty(u); break;
    case ToolUse::Kind::Deco: useDeco(u); break;
    }
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
    tptLinePoints(x1, y1, x2, y2, LineKind::Parts, brush_.rx() + brush_.ry() == 0, points);
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
    case A::Drag:
    case A::Create:
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
    PixelCounts counts(W_, H_);
    if (c)
        scanFill(counts, bits, x, y, CELL, W_ - CELL - 1, CELL, H_ - CELL - 1);
    else
        scanFill(counts, bits, x, y, 0, W_ - 1, 0, H_ - 1);
    if (c == 0)
        addPixels(counts.edit(0, EDIT_FLOOD_KILL, cm));
    else
        addPixels(counts.edit(fullc, uint32_t(flags), selected));
}

// TPT's span fill pushes every pixel of the rows above and below that it could go on to; pushing one per run of them
// reaches the same pixels (those joined to the start, side by side or one above the other), in far fewer steps
void GpuEditor::scanFill(PixelCounts &counts, const std::vector<uint32_t> &bits, int x, int y, int xMin, int xMax,
                         int yMin, int yMax) const {
    std::vector<uint64_t> seen((size_t(W_) * size_t(H_) + 63) / 64, 0);
    auto open = [&](int px, int py) {
        size_t i = size_t(py) * size_t(W_) + size_t(px);
        return ((seen[i >> 6] >> (i & 63)) & 1) == 0 && ((bits[i >> 5] >> (i & 31)) & 1) != 0;
    };
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
}

std::vector<int> GpuEditor::stamps(const ToolUse &u) const {
    std::vector<int> points;
    if (u.action == ToolUse::Action::Draw)
        points = {u.x1, u.y1};
    else if (u.action == ToolUse::Action::DrawLine)
        tptLinePoints(u.x1, u.y1, u.x2, u.y2, u.kind == ToolUse::Kind::Deco ? LineKind::Deco : LineKind::Parts,
                      brush_.rx() + brush_.ry() == 0, points);
    return points;
}

// SimTool: its Draw, DrawLine and DrawRect call Perform at each pixel they reach (with the brush's centre), except
// WIND's, which blows along the line from its first point; none has a fill or a click
void GpuEditor::useSim(const ToolUse &u) {
    using A = ToolUse::Action;
    const float s = u.strength;
    if (u.tool == TOOL_MIX) {
        mix(u);
        return;
    }
    if (u.tool == TOOL_CYCL) {
        cyclone(u);
        return;
    }
    PixelCounts counts(W_, H_);
    if (u.tool == TOOL_WIND) {
        // Tool_WIND's performDrawLine: dragging (or Drag) blows harder than a line drawn with shift
        if (u.action != A::DrawLine && u.action != A::Drag)
            return;
        float strength = u.action == A::DrawLine && u.dragging ? 0.01f : 0.002f;
        strength *= s;
        stamp(counts, u.x1, u.y1);
        float a = float(u.x2 - u.x1) * strength, b = float(u.y2 - u.y1) * strength;
        std::vector<uint32_t> cells = counts.cellCounts();
        ToolEdit e;
        e.words = {1 /* OP_CELLS */, uint32_t(TOOL_WIND), uint32_t(cells.size() / 2), std::bit_cast<uint32_t>(a),
                   std::bit_cast<uint32_t>(b)};
        e.words.insert(e.words.end(), cells.begin(), cells.end());
        e.w = uint32_t(cells.size() / 2);
        e.list = true;
        addTool(std::move(e));
        return;
    }
    if (u.action == A::Draw || u.action == A::DrawLine) {
        std::vector<int> points = stamps(u);
        for (size_t k = 0; k + 1 < points.size(); k += 2)
            stamp(counts, points[k], points[k + 1]);
    } else if (u.action == A::DrawRect) {
        int x1 = std::min(u.x1, u.x2), x2 = std::max(u.x1, u.x2), y1 = std::min(u.y1, u.y2), y2 = std::max(u.y1, u.y2);
        for (int y = y1; y <= y2; y++)
            counts.span(y, x1, x2);
    } else
        return;
    if (counts.empty())
        return;
    if (u.tool == TOOL_HEAT || u.tool == TOOL_COOL) {
        float sign = u.tool == TOOL_HEAT ? 1.0f : -1.0f;
        float d = sign * (s * 2.0f), pump = sign * (s * .1f);
        addTool(counts.toolEdit(uint32_t(u.tool), 16, std::bit_cast<uint32_t>(d), std::bit_cast<uint32_t>(pump)));
        return;
    }
    float a;
    switch (u.tool) {
    case TOOL_AIR: a = s * 0.05f; break;
    case TOOL_VAC: a = -(s * 0.05f); break;
    case TOOL_AMBP: a = s * 2.0f; break;
    case TOOL_AMBM: a = -(s * 2.0f); break;
    case TOOL_PGRV: a = s * 5.0f; break;
    case TOOL_NGRV: a = s * -5.0f; break;
    default: return;
    }
    std::vector<uint32_t> cells = counts.cellCounts();
    ToolEdit e;
    e.words = {1 /* OP_CELLS */, uint32_t(u.tool), uint32_t(cells.size() / 2), std::bit_cast<uint32_t>(a), 0};
    e.words.insert(e.words.end(), cells.begin(), cells.end());
    e.w = uint32_t(cells.size() / 2);
    e.list = true;
    addTool(std::move(e));
}

// MIX: at each pixel it reaches, one time in a hundred, the particle there swaps places with one of the same state a
// little way off (strength 1: up to 5 pixels). Which times is random in TPT too; here they are drawn ahead, in TPT's
// order (the brush's points from the bottom row up, the stamps along the line, a box row by row), and sim_tool.slang
// makes the swaps one after another.
void GpuEditor::mix(const ToolUse &u) {
    using A = ToolUse::Action;
    const int distance = int(std::pow(u.strength, .5f) * 10);
    if (distance <= 0)
        return;
    std::geometric_distribution<long long> gap(0.01);
    std::uniform_int_distribution<int> offset(0, distance - 1);
    long long skip = gap(rng_); // pixels to pass over before the next one that mixes
    std::vector<uint32_t> swaps;
    auto run = [&](int y, int xa, int xb) { // Perform at (xa..xb, y), in that order
        if (y < 0 || y >= H_)
            return;
        xa = std::max(xa, 0);
        xb = std::min(xb, W_ - 1);
        for (int x = xa; x <= xb;) {
            if (skip > xb - x) {
                skip -= xb - x + 1;
                return;
            }
            x += int(skip);
            int nx = x + offset(rng_) - distance / 2, ny = y + offset(rng_) - distance / 2;
            swaps.insert(swaps.end(), {uint32_t(x), uint32_t(y), uint32_t(nx), uint32_t(ny)});
            skip = gap(rng_);
            x++;
        }
    };
    if (u.action == A::Draw || u.action == A::DrawLine) {
        std::vector<int> points = stamps(u);
        const int ry = brush_.ry();
        for (size_t k = 0; k + 1 < points.size(); k += 2)
            for (int j = int(brushRows_.size()) - 1; j >= 0; j--)
                for (const auto &r : brushRows_[size_t(j)])
                    run(points[k + 1] + j - ry, points[k] + r[0], points[k] + r[1]);
    } else if (u.action == A::DrawRect) {
        int x1 = std::min(u.x1, u.x2), x2 = std::max(u.x1, u.x2), y1 = std::min(u.y1, u.y2), y2 = std::max(u.y1, u.y2);
        for (int y = y1; y <= y2; y++)
            run(y, x1, x2);
    }
    if (swaps.empty())
        return;
    ToolEdit e;
    e.words = {5 /* OP_MIX */, uint32_t(swaps.size() / 4)};
    e.words.insert(e.words.end(), swaps.begin(), swaps.end());
    addTool(std::move(e));
}

// CYCL: at each cell's corner pixel it reaches (other than the brush's centre) it turns the air around the centre;
// what it adds depends on where the corner is from the centre, so it is worked out once over the brush
void GpuEditor::cyclone(const ToolUse &u) {
    using A = ToolUse::Action;
    const float s = u.strength;
    if (u.action == A::DrawRect) {
        // defaultPerformDrawRect: every pixel of the box once, with the box's centre
        int bx = (u.x1 + u.x2) / 2, by = (u.y1 + u.y2) / 2;
        int x1 = std::min(u.x1, u.x2), x2 = std::max(u.x1, u.x2), y1 = std::min(u.y1, u.y2), y2 = std::max(u.y1, u.y2);
        x1 = std::max(x1, 0), y1 = std::max(y1, 0), x2 = std::min(x2, W_ - 1), y2 = std::min(y2, H_ - 1);
        ToolEdit e;
        e.words = {4 /* OP_CYCLONE_LIST */, 0};
        for (int y = (y1 + CELL - 1) / CELL * CELL; y <= y2; y += CELL)
            for (int x = (x1 + CELL - 1) / CELL * CELL; x <= x2; x += CELL) {
                if (x == bx && y == by)
                    continue;
                float dvx, dvy;
                tptCycloneDelta(s, bx, by, x, y, dvx, dvy);
                e.words.insert(e.words.end(), {uint32_t((y / CELL) * CW_ + x / CELL), std::bit_cast<uint32_t>(dvx),
                                               std::bit_cast<uint32_t>(dvy)});
            }
        e.words[1] = uint32_t((e.words.size() - 2) / 3);
        e.w = e.words[1];
        e.list = true;
        if (e.w)
            addTool(std::move(e));
        return;
    }
    std::vector<int> points = stamps(u);
    if (points.empty())
        return;
    const int rx = brush_.rx(), ry = brush_.ry(), bw = 2 * rx + 1, bh = 2 * ry + 1;
    int minX = points[0], maxX = points[0], minY = points[1], maxY = points[1];
    for (size_t k = 0; k + 1 < points.size(); k += 2) {
        minX = std::min(minX, points[k]), maxX = std::max(maxX, points[k]);
        minY = std::min(minY, points[k + 1]), maxY = std::max(maxY, points[k + 1]);
    }
    int cx0 = std::max(0, (minX - rx + CELL - 1) / CELL), cx1 = std::min(CW_ - 1, (maxX + rx) / CELL);
    int cy0 = std::max(0, (minY - ry + CELL - 1) / CELL), cy1 = std::min(CH_ - 1, (maxY + ry) / CELL);
    if (minX - rx < 0)
        cx0 = 0;
    if (minY - ry < 0)
        cy0 = 0;
    if (cx1 < cx0 || cy1 < cy0)
        return;
    ToolEdit e;
    e.words = {3 /* OP_CYCLONE */, uint32_t(points.size() / 2), uint32_t(rx), uint32_t(ry), uint32_t(cx0), uint32_t(cy0),
               uint32_t(cx1 - cx0 + 1), uint32_t(cy1 - cy0 + 1)};
    for (int p : points)
        e.words.push_back(uint32_t(p));
    const size_t bitsAt = e.words.size(), n = size_t(bw) * size_t(bh);
    e.words.resize(bitsAt + (n + 31) / 32, 0);
    const size_t tableAt = e.words.size();
    e.words.resize(tableAt + 2 * n, 0);
    const std::vector<uint8_t> &bm = brush_.bitmap();
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) {
            size_t k = size_t(j) * size_t(bw) + size_t(i);
            if (!bm[k])
                continue;
            e.words[bitsAt + (k >> 5)] |= 1u << (k & 31);
            int ox = i - rx, oy = j - ry;
            if (ox == 0 && oy == 0)
                continue;
            float dvx, dvy;
            tptCycloneDelta(s, 0, 0, ox, oy, dvx, dvy);
            e.words[tableAt + 2 * k] = std::bit_cast<uint32_t>(dvx);
            e.words[tableAt + 2 * k + 1] = std::bit_cast<uint32_t>(dvy);
        }
    e.w = uint32_t(cx1 - cx0 + 1);
    e.h = uint32_t(cy1 - cy0 + 1);
    addTool(std::move(e));
}

// PropertyTool: SetProperty at each pixel its brush reaches (along a line, or each of a box's), or flood_prop
void GpuEditor::useProperty(const ToolUse &u) {
    using A = ToolUse::Action;
    if (u.property.field < 0 || u.property.field >= ParticleProperty::COUNT)
        return;
    PixelCounts counts(W_, H_);
    if (u.action == A::Draw || u.action == A::DrawLine) {
        std::vector<int> points = stamps(u);
        for (size_t k = 0; k + 1 < points.size(); k += 2)
            stamp(counts, points[k], points[k + 1]);
    } else if (u.action == A::DrawRect) {
        int x1 = std::min(u.x1, u.x2), x2 = std::max(u.x1, u.x2), y1 = std::min(u.y1, u.y2), y2 = std::max(u.y1, u.y2);
        for (int y = y1; y <= y2; y++)
            counts.span(y, x1, x2);
    } else if (u.action == A::DrawFill) {
        floodProperty(u.x1, u.y1, u.property);
        return;
    }
    if (!counts.empty())
        addTool(counts.toolEdit(TOOL_PROPERTY, 2, uint32_t(u.property.field), u.property.bits));
}

// Simulation::flood_prop: over the particles of the type at (x, y) (in pmap, else an energy particle) joined to it, as
// far as one pixel inside the border across and the CELL border up and down
void GpuEditor::floodProperty(int x, int y, const ParticleProperty &p) {
    if (x < 0 || x >= W_ || y < 0 || y >= H_)
        return;
    flush(VK_NULL_HANDLE);
    int cm;
    std::vector<uint32_t> bits = sim_.readFillMap(x, y, 0, cm); // (FloodFillPmapCheck, no walls)
    if (cm < 0)
        return;
    PixelCounts counts(W_, H_);
    scanFill(counts, bits, x, y, CELL - 1, W_ - CELL, CELL, H_ - CELL - 1);
    addTool(counts.toolEdit(TOOL_PROPERTY, 2, uint32_t(p.field), p.bits));
}

// DecorationTool: ApplyDecorationPoint with the brush (along ApplyDecorationLine's line), ApplyDecorationBox and
// ApplyDecorationFill. It has no click.
void GpuEditor::useDeco(const ToolUse &u) {
    using A = ToolUse::Action;
    if (u.action == A::DrawFill) {
        decoFill(u);
        return;
    }
    if (u.tool == DECO_SMUDGE) {
        smudge(u);
        return;
    }
    PixelCounts counts(W_, H_);
    if (u.action == A::Draw || u.action == A::DrawLine) {
        std::vector<int> points = stamps(u);
        for (size_t k = 0; k + 1 < points.size(); k += 2)
            stamp(counts, points[k], points[k + 1]);
    } else if (u.action == A::DrawRect) {
        int x1 = std::min(u.x1, u.x2), x2 = std::max(u.x1, u.x2), y1 = std::min(u.y1, u.y2), y2 = std::max(u.y1, u.y2);
        for (int y = y1; y <= y2; y++)
            counts.span(y, x1, x2);
    }
    decorate(counts, u.decoColour, u.tool);
}

void GpuEditor::decorate(const PixelCounts &counts, uint32_t colour, int mode) {
    ToolEdit e;
    e.words = {6 /* OP_DECO */, 0, 0, 0, 0};
    // what each value of each channel becomes (all four channels are worked out from their own old values)
    for (uint32_t v = 0; v < 256; v++)
        e.words.push_back(tptDecorate(v * 0x01010101u, colour, mode));
    int x0, y0, w, h;
    if (!counts.packed(16, e.words, x0, y0, w, h))
        return;
    e.words[1] = uint32_t(x0), e.words[2] = uint32_t(y0), e.words[3] = uint32_t(w), e.words[4] = uint32_t(h);
    e.w = uint32_t(w);
    e.h = uint32_t(h);
    addTool(std::move(e));
}

// SMUDGE averages the colours around each pixel as TPT's loop leaves them, so it runs here in TPT's order (the brush's
// points from its bottom row up, the stamps along the line, a box row by row) on what is read back, and the colours
// it changes go back to the GPU
void GpuEditor::smudge(const ToolUse &u) {
    using A = ToolUse::Action;
    std::vector<int> at; // the pixels, x, y pairs, in order
    if (u.action == A::Draw || u.action == A::DrawLine) {
        std::vector<int> points = stamps(u);
        const int ry = brush_.ry();
        for (size_t k = 0; k + 1 < points.size(); k += 2)
            for (int j = int(brushRows_.size()) - 1; j >= 0; j--)
                for (const auto &r : brushRows_[size_t(j)])
                    for (int dx = r[0]; dx <= r[1]; dx++) {
                        int x = points[k] + dx, y = points[k + 1] + j - ry;
                        if (x >= 0 && y >= 0 && x < W_ && y < H_)
                            at.insert(at.end(), {x, y});
                    }
    } else if (u.action == A::DrawRect) {
        int x1 = std::min(u.x1, u.x2), x2 = std::max(u.x1, u.x2), y1 = std::min(u.y1, u.y2), y2 = std::max(u.y1, u.y2);
        for (int y = y1; y <= y2; y++)
            for (int x = x1; x <= x2; x++)
                at.insert(at.end(), {x, y});
    }
    if (at.empty())
        return;
    int bx0 = at[0], bx1 = at[0], by0 = at[1], by1 = at[1];
    for (size_t k = 0; k + 1 < at.size(); k += 2) {
        bx0 = std::min(bx0, at[k]), bx1 = std::max(bx1, at[k]);
        by0 = std::min(by0, at[k + 1]), by1 = std::max(by1, at[k + 1]);
    }
    bx0 = std::max(bx0 - 2, 0), by0 = std::max(by0 - 2, 0), bx1 = std::min(bx1 + 2, W_ - 1), by1 = std::min(by1 + 2, H_ - 1);
    const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    flush(VK_NULL_HANDLE); // (what was drawn before is in the world to read)
    std::vector<uint32_t> box = sim_.readDecoBox(bx0, by0, bw, bh);
    // (each particle is at one pixel of the box, so the colours are kept by pixel as they change: words 2 and 3)
    auto cell = [&](int x, int y) { return (size_t(y - by0) * size_t(bw) + size_t(x - bx0)) * 4; };
    std::vector<uint8_t> touched(box.size() / 4, 0); // 1: the one in pmap changed, 2: the energy particle
    for (size_t k = 0; k + 1 < at.size(); k += 2) {
        const int x = at[k], y = at[k + 1];
        size_t c = cell(x, y);
        const size_t own = box[c] ? c + 2 : c + 3; // (in pmap, else an energy particle)
        if (!box[own - 2])
            continue;
        const bool inside = x >= CELL && x < W_ - CELL && y >= CELL && y < H_ - CELL;
        uint32_t around[25] = {};
        if (inside)
            for (int rx = -2; rx < 3; rx++)
                for (int ry = -2; ry < 3; ry++) {
                    size_t n = cell(x + rx, y + ry);
                    if (box[n]) // (what is in pmap there)
                        around[(rx + 2) * 5 + (ry + 2)] = box[n + 2];
                }
        bool changed;
        uint32_t next = tptSmudge(box[own], around, inside, u.decoSpace, changed);
        if (changed) {
            box[own] = next;
            touched[c / 4] |= own == c + 2 ? 1 : 2;
        }
    }
    ToolEdit e;
    e.words = {8 /* OP_DCOLOUR_WRITE */, 0};
    for (size_t c = 0; c < touched.size(); c++)
        for (int which = 0; which < 2; which++)
            if (touched[c] & (1 << which))
                e.words.insert(e.words.end(), {box[c * 4 + size_t(which)] - 1, box[c * 4 + 2 + size_t(which)]});
    e.words[1] = uint32_t((e.words.size() - 2) / 2);
    e.w = e.words[1];
    e.list = true;
    if (e.w)
        addTool(std::move(e));
}

// ApplyDecorationFill: over the pixels joined to (x, y) whose colour in the picture is within 15 of its (the three
// channels' differences added up), it sets the colour (DECO_CLEAR clears it)
void GpuEditor::decoFill(const ToolUse &u) {
    const int x = u.x1, y = u.y1;
    if (x < 0 || x >= W_ || y < 0 || y >= H_)
        return;
    flush(VK_NULL_HANDLE);
    std::vector<uint32_t> picture = sim_.readWorldImage();
    const uint32_t loc = picture[size_t(y) * size_t(W_) + size_t(x)];
    const int lr = int(loc >> 16) & 0xFF, lg = int(loc >> 8) & 0xFF, lb = int(loc) & 0xFF;
    std::vector<uint32_t> bits((size_t(W_) * size_t(H_) + 31) / 32, 0);
    for (size_t i = 0; i < picture.size(); i++) {
        const uint32_t p = picture[i];
        int diff = std::abs(lr - int((p >> 16) & 0xFF)) + std::abs(lg - int((p >> 8) & 0xFF)) + std::abs(lb - int(p & 0xFF));
        if (diff < 15) // (Simulation::ColorCompare)
            bits[i >> 5] |= 1u << (i & 31);
    }
    PixelCounts counts(W_, H_);
    scanFill(counts, bits, x, y, 0, W_ - 1, 0, H_ - 1);
    decorate(counts, u.tool == DECO_CLEAR ? 0u : u.decoColour, DECO_DRAW);
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
            tptLinePoints(u.x1, u.y1, u.x2, u.y2, LineKind::Walls, false, points);
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
    case A::Drag:
    case A::Create:
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
