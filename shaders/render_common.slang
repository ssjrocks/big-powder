// Shared by the renderer kernels (gfx.slang, resolve.slang, fire.slang): TPT's Renderer drawn at one pixel per
// cell into a world-sized image, with TPT's integer pixel arithmetic and its drawing order.
//
// TPT draws particles one by one in id order. What a particle draws on its own pixel is kept with it (gfx); what
// it draws elsewhere, and what a particle hidden under another draws, is appended as a fragment to a per-pixel
// list (order-independent transparency's A-buffer). resolve.slang then applies each pixel's operations sorted by
// (particle id, step), so overlapping glows, blends and flat pixels come out as TPT's do.
#include "common.slang"

// Pixel modes (ElementGraphics.h)
static const uint PMODE = 0x00000FFF;
static const uint PMODE_FLAT = 0x1, PMODE_BLOB = 0x2, PMODE_BLUR = 0x4, PMODE_GLOW = 0x8, PMODE_SPARK = 0x10;
static const uint PMODE_FLARE = 0x20, PMODE_LFLARE = 0x40, PMODE_ADD = 0x80, PMODE_BLEND = 0x100;
static const uint PSPEC_STICKMAN = 0x200;
static const uint NO_DECO = 0x1000, DECO_FIRE = 0x2000;
static const uint FIREMODE = 0x00FF0000, FIRE_ADD = 0x10000, FIRE_BLEND = 0x20000, FIRE_SPARK = 0x40000;
static const uint EFFECT = 0xFF000000, EFFECT_GRAVIN = 0x1000000, EFFECT_GRAVOUT = 0x2000000;
static const uint EFFECT_LINES = 0x4000000, EFFECT_DBGLINES = 0x8000000;
static const uint COLOUR_HEAT = 1, COLOUR_LIFE = 2, COLOUR_GRAD = 4, COLOUR_BASC = 8;
static const uint DISPLAY_AIRC = 0x1, DISPLAY_AIRP = 0x2, DISPLAY_AIRV = 0x4, DISPLAY_AIRH = 0x8, DISPLAY_AIR = 0x8F;
static const uint DISPLAY_WARP = 0x10, DISPLAY_PERS = 0x20, DISPLAY_EFFE = 0x40, DISPLAY_AIRW = 0x80;
static const uint DECO_DISABLED = 0, DECO_ENABLED = 1, DECO_ANTICLICKBAIT = 2;
static const uint PROP_HOT_GLOW = 0x800;
static const uint FLAG_PHOTDECO = 0x8;

// Renderer tables (sim/RenderTables.h)
static const uint RT_FLAME = 0, RT_PLASMA = 200, RT_HEAT = 400, RT_CLFM = 1424, RT_FIRW = 1624;
static const uint RT_FIRE_ALPHA = 1824, RT_GOL_COLOUR = 1968, RT_GOL_COLOUR2 = 2000, RT_GOL_RULESET = 2032;
static const uint RT_WALL_COLOUR = 2064, RT_WALL_EGLOW = 2096, RT_WALL_STYLE = 2128, RT_TRON = 2160;
static const uint RT_SLCN = 2192, RT_LEAF = 2208;
static const uint RT_GLYPH_STREAM = 2216, RT_FONT = 2996; // TPT's font (src/sim/RenderTables.h)
static const int FONT_H = 12;
uint tableAt(uint table, uint size, int index) { return renderTables[table + uint(clamp(index, 0, int(size) - 1))]; }

// The world-sized image, 0xRRGGBB per pixel
[[vk::binding(21, 0)]] RWStructuredBuffer<uint> video;
// Each particle's graphics this frame, 3 words: pixel mode, colour (RGBA, R in the low byte), fire colour (RGBA)
[[vk::binding(22, 0)]] RWStructuredBuffer<uint> gfx;
// List heads, one per pixel then one per fire cell (fragment index + 1, 0 for none)
[[vk::binding(23, 0)]] RWStructuredBuffer<uint> fragHeads;
// Fragments: key (particle id << 5 | step), colour (RGBA), next (index + 1) | operation << 28, and for a fire
// cell the particle's pixel mode
[[vk::binding(24, 0)]] RWStructuredBuffer<uint4> frags;
// TPT's fire_r/g/b, one 0xRRGGBB per cell, kept from frame to frame
[[vk::binding(25, 0)]] RWStructuredBuffer<uint> fireBuf;
// The persistent display's last frame, faded by one step
[[vk::binding(26, 0)]] RWStructuredBuffer<uint> persistVideo;
// Per cell (CELL x CELL pixels), this frame: whether a top particle there glows or blurs (gfx.slang), so resolve.slang
// knows which pixels need the full treatment
[[vk::binding(27, 0)]] RWStructuredBuffer<uint> gfxFlags;
static const uint GFX_FLAG_GLOW = 1, GFX_FLAG_BLUR = 2;
static const uint PIXEL_LATER = 0xFF000000u; // marks a pixel for resolve_heavy.slang

static const uint CTR_HDISP = 1938, CTR_HDISP_SCAN = 1940;
static const uint CTR_FIRE_BOX = 1944; // cells with fire this frame: lowest x, y, highest x, y (fire_smooth.slang)
static const uint CTR_FOUND = 1948;    // particles found this frame (ctrl+F)
// The heat display's range: the preset's, or (hdispMin < 0) the one hdisp.slang adjusts every frame
float2 hdispRange()
{
    if (pc.hdispMin >= 0.0)
        return float2(pc.hdispMin, pc.hdispMax);
    return float2(asfloat(counters[CTR_HDISP]), asfloat(counters[CTR_HDISP + 1]));
}

static const uint CTR_FRAG_COUNT = 1936; // fragments appended this frame (counters)
static const uint FRAG_NEXT = 0x0FFFFFFFu;
static const uint FRAG_CAP = 1u << 22;

// Pixel operations
static const uint OP_SET = 0, OP_BLEND = 1, OP_ADD = 2;
// Operations drawn among the walls (DrawWalls: streamlines), keyed by cell: a saturating white add of the colour's
// value (any size), and a blend
static const uint OP_WALL_ADD = 4, OP_WALL_BLEND = 5;
// Fire operations (a fragment for a fire cell carries the particle's pixel mode instead)

// Steps within one particle's drawing, in TPT's order (Renderer::render_parts)
static const uint STEP_LINES = 0, STEP_STKM_LINES = 1, STEP_STKM_BOOTS = 2, STEP_FLAT = 3, STEP_BLEND = 4;
static const uint STEP_ADD = 5, STEP_BLOB = 6, STEP_GLOW = 7, STEP_BLUR = 8; // BLUR takes 8, 9 and 10
static const uint STEP_SPARK = 11, STEP_FLARE = 12, STEP_FLARE_RAYS = 13, STEP_LFLARE = 14, STEP_LFLARE_RAYS = 15;
static const uint STEP_GRAVIN = 16, STEP_GRAVOUT = 17;

uint packRGBA(int r, int g, int b, int a) { return uint(r) | (uint(g) << 8) | (uint(b) << 16) | (uint(a) << 24); }
int4 unpackRGBA(uint c) { return int4(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF), int(c >> 24)); }
int3 unpackRGB(uint c) { return int3(int((c >> 16) & 0xFF), int((c >> 8) & 0xFF), int(c & 0xFF)); }

// RendererSettings::findingElement (ctrl+F): whether anything is being found, and whether a particle is it (its element,
// and its ctype if one is given; or a property's value, as the property tool would set it)
bool findActive() { return (pc.findMode & 3) != 0; }
bool findMatches(uint t, Particle p)
{
    uint kind = pc.findMode & 3, v = pc.findValue;
    if (kind == 1)
        return t == (v & 0x1FF) && ((v >> 9) == 0 || uint(p.ctype) == (v >> 9));
    uint bits = 0;
    switch ((pc.findMode >> 2) & 15)
    {
    case 0: bits = t; break;
    case 1: bits = uint(p.life); break;
    case 2: bits = uint(p.ctype); break;
    case 3: bits = asuint(p.x); break;
    case 4: bits = asuint(p.y); break;
    case 5: bits = asuint(p.vx); break;
    case 6: bits = asuint(p.vy); break;
    case 7: bits = asuint(p.temp); break;
    case 8: bits = p.flags; break;
    case 9: bits = uint(p.tmp); break;
    case 10: bits = uint(p.tmp2); break;
    case 11: bits = uint(p.tmp3); break;
    case 12: bits = uint(p.tmp4); break;
    case 13: bits = p.dcolour; break;
    }
    return kind == 3 ? asfloat(bits) == asfloat(v) : bits == v;
}
uint packRGB(int3 c) { return (uint(c.x) << 16) | (uint(c.y) << 8) | uint(c.z); }

// RGB::Blend and RGB::Add (graphics/Pixel.h)
int3 blendRGB(int3 px, int4 c)
{
    if (c.w == 0xFF)
        return c.xyz;
    return (c.w * c.xyz + (0xFF - c.w) * px) / 0xFF;
}
int3 addRGB(int3 px, int4 c) { return min(int3(0xFF, 0xFF, 0xFF), px + (c.w * c.xyz) / 0xFF); }

int3 applyOp(int3 px, uint op, uint colour)
{
    int4 c = unpackRGBA(colour);
    if (op == OP_SET)
        return c.xyz;
    if (op == OP_BLEND)
        return blendRGB(px, c);
    return addRGB(px, c);
}

bool inVideo(int x, int y) { return x >= 0 && y >= 0 && x < int(pc.W) && y < int(pc.H); }

// TPT's font (FontReader): a glyph's width and its pixel (i, j), 0..3
uint fontByte(uint at) { return (renderTables[RT_FONT + at / 4] >> (8 * (at % 4))) & 0xFFu; }
int glyphWidth(uint glyph) { return int(fontByte(glyph)); }
int glyphPixel(uint glyph, int i, int j)
{
    uint p = uint(j * glyphWidth(glyph) + i);
    return int((fontByte(glyph + 1 + p / 4) >> (2 * (p % 4))) & 3u);
}

// Appends a fragment for pixel (x, y) (or, with fire, for fire cell index x).
void pushFrag(uint list, uint key, uint op, uint colour, uint extra = 0)
{
    uint slot;
    InterlockedAdd(counters[CTR_FRAG_COUNT], 1u, slot);
    if (slot >= FRAG_CAP)
        return;
    uint prev;
    InterlockedExchange(fragHeads[list], slot + 1, prev);
    frags[slot] = uint4(key, colour, prev | (op << 28), extra);
}

uint gfxMode(uint i) { return gfx[3 * i]; }
uint gfxColour(uint i) { return gfx[3 * i + 1]; }
uint gfxFire(uint i) { return gfx[3 * i + 2]; }

void drawPixel(int x, int y, uint key, uint op, int4 c)
{
    if (inVideo(x, y))
        pushFrag(uint(y) * pc.W + uint(x), key, op, packRGBA(c.x, c.y, c.z, c.w));
}

uint fireList(uint cell) { return pc.W * pc.H + cell; }
