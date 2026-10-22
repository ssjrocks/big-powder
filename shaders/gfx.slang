// One thread per particle: TPT's Renderer::render_parts up to the drawing. Each particle's pixel mode and colours
// come from its element's Graphics function (or Element::defaultGraphics), adjusted for the display preset, the
// colour mode and decoration, as in TPT. What it draws on its own pixel is kept in gfx[i] for resolve.slang; what
// it draws anywhere else becomes fragments (render_common.slang), as does everything a particle hidden under
// another draws.
#include "render_common.slang"

static const int GFX_PLNT_COLOUR = 6; // PLNT's and SEED's ctype fields (elements.slang)
static const int GFX_PLNT_LIFE = 12;

bool decoShown(uint dcolour)
{
    if (pc.decoLevel == DECO_DISABLED || dcolour == 0 || (dcolour & 0xFF000000u) == 0)
        return false;
    if (pc.decoLevel == DECO_ENABLED) // if black decorations is off, always show deco
        return true;
    return ((dcolour >> 24) & 0xFF) >= 250 && ((dcolour >> 16) & 0xFF) <= 5 && ((dcolour >> 8) & 0xFF) <= 5 &&
           (dcolour & 0xFF) <= 5; // otherwise only black deco
}

// A wavelength mask (PHOT, BRAY, C5, FILT, BIZR) as a colour
int3 wavelengthColour(int wl)
{
    int r = 0, g = 0, b = 0;
    for (int x = 0; x < 12; x++)
    {
        r += (wl >> (x + 18)) & 1;
        b += (wl >> x) & 1;
    }
    for (int x = 0; x < 12; x++)
        g += (wl >> (x + 9)) & 1;
    int m = 624 / (r + g + b + 1);
    return int3(r * m, g * m, b * m);
}

int3 tableColour(uint table, uint size, int index) { return unpackRGB(tableAt(table, size, index)); }

float restrictFlt2(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Element::defaultGraphics
void defaultGraphics(uint t, inout uint mode, inout int4 col, inout int4 fire)
{
    uint props = elems[t].properties;
    if ((props & PROP_RADIOACTIVE) != 0)
        mode |= PMODE_GLOW;
    if ((props & TYPE_LIQUID) != 0)
        mode |= PMODE_BLUR;
    if ((props & TYPE_GAS) != 0)
    {
        mode &= ~PMODE;
        mode |= FIRE_BLEND;
        fire = int4(col.x / 2, col.y / 2, col.z / 2, 125);
        mode |= DECO_FIRE;
    }
}

// The element Graphics functions (simulation/elements/*.cpp). p is the particle (or what a pipe holds).
void elementGraphics(uint t, Particle p, inout uint mode, inout int4 col, inout int4 fire)
{
    switch (t)
    {
    case PT_DUST:
    case PT_SAWD:
    case PT_DRAY:
        break; // no Graphics function at all
    case PT_ACEL:
    case PT_DCEL:
        if (p.tmp != 0)
            mode |= PMODE_GLOW;
        break;
    case PT_ACID:
    {
        int s = clamp(p.life, 49, 75);
        s = (s - 49) * 3;
        if (s == 0)
            s = 1;
        col.xyz += int3(s * 4, s, s * 2);
        mode |= PMODE_BLUR;
        break;
    }
    case PT_AMTR:
        break; // overrides the default (not drawn as a gas)
    case PT_BASE:
    {
        int s = p.life;
        if (s <= 25)
            col.xyz = int3(0x33, 0x4C, 0xD8);
        else if (s <= 50)
            col.xyz = int3(0x58, 0x83, 0xE8);
        else if (s <= 75)
            col.xyz = int3(0x7D, 0xBA, 0xF7);
        mode |= PMODE_BLUR;
        if (p.tmp == 1)
            mode |= PMODE_SPARK;
        break;
    }
    case PT_BOMB:
    case PT_DMG:
        mode |= PMODE_FLARE;
        break;
    case PT_BRAY:
    {
        int trans = 255;
        if (p.tmp == 0 || p.tmp == 1)
        {
            trans = p.tmp == 0 ? p.life * 7 : p.life / 4;
            if (trans > 255)
                trans = 255;
            if ((p.ctype & 0x3FFFFFFF) != 0)
                col.xyz = wavelengthColour(p.ctype);
        }
        else if (p.tmp == 2)
        {
            trans = p.life * 100;
            if (trans > 255)
                trans = 255;
            col.xyz = int3(255, 150, 50);
        }
        col.w = trans;
        mode &= ~PMODE;
        mode |= PMODE_BLEND | PMODE_GLOW;
        break;
    }
    case PT_BRCK:
        if (p.tmp == 1)
        {
            mode |= FIRE_ADD;
            col.z += 100;
            fire = int4(col.xyz, 40);
        }
        break;
    case PT_C5:
        if (p.ctype != 0)
        {
            col.xyz = wavelengthColour(p.ctype);
            fire = int4(col.xyz, 100);
            mode &= ~PMODE_FLAT;
            mode |= FIRE_ADD | PMODE_ADD | NO_DECO;
        }
        break;
    case PT_CBNW:
    {
        int z = p.tmp2 - 20; // speckles
        col.xyz += int3(z, z * 2, z * 8);
        break;
    }
    case PT_CFLM:
        col.xyz = tableColour(RT_CLFM, 200, p.life / 2);
        fire = int4(col.xyz, 255);
        mode = FIRE_ADD; // don't draw the pixel
        break;
    case PT_CLST:
    case PT_QRTZ:
    case PT_PQRT:
    {
        int z = (t == PT_CLST ? p.tmp - 5 : p.tmp2 - 5) * 16; // speckles
        col.xyz += int3(z, z, z);
        break;
    }
    case PT_CRMC:
    {
        int z = (p.tmp2 - 2) * 8;
        col.xyz += int3(z, z, z);
        break;
    }
    case PT_COAL:
    case PT_BCOL:
    {
        col.x += int((float(p.tmp2) - 295.15) / 3.0);
        if (col.x > 170)
            col.x = 170;
        if (col.x < col.y)
            col.x = col.y;
        col.y = col.x;
        col.z = col.x;
        if (p.temp > 395.15)
        {
            const float freq = 3.14159265 / (2.0 * 300.0 - (300.0 - 200.0));
            int q = int(p.temp > 595.15 ? 200.0 : p.temp - 395.15);
            col.x += int(sin(freq * float(q)) * 226.0);
            col.y += int(-sin(freq * float(q) * 4.55) * 34.0);
            col.z += int(-sin(freq * float(q) * 2.22) * 64.0);
        }
        break;
    }
    case PT_DEST:
        mode |= p.life != 0 ? PMODE_LFLARE : PMODE_SPARK;
        break;
    case PT_DEUT:
        if (p.life >= 240)
        {
            col.xyz += int3(255, 255, 255);
            fire = int4(col.xyz, 60);
            mode |= PMODE_GLOW | FIRE_ADD;
        }
        else
        {
            if (p.life > 0)
                col.xyz += int3(p.life, p.life * 2, p.life * 3);
            mode |= PMODE_BLUR;
        }
        break;
    case PT_DLAY:
    {
        int stage = int((float(p.life) / (p.temp - 273.15)) * 100.0);
        col.xyz += int3(stage, stage, stage);
        break;
    }
    case PT_ELEC:
        fire = int4(col.xyz, 70);
        mode |= FIRE_ADD;
        break;
    case PT_EMBR:
    {
        if ((p.ctype & 0xFFFFFF) != 0)
        {
            col.xyz = int3((p.ctype & 0xFF0000) >> 16, (p.ctype & 0x00FF00) >> 8, p.ctype & 0xFF);
            int maxComponent = max(col.x, max(col.y, col.z));
            if (maxComponent < 60) // not too dark to see
            {
                float multiplier = 60.0 / float(maxComponent);
                col.xyz = int3(float3(col.xyz) * multiplier);
            }
        }
        else if (p.tmp != 0)
            col.xyz = int3(255, 255, 255);
        if (decoShown(p.dcolour))
        {
            int a = int((p.dcolour >> 24) & 0xFF);
            int3 d = int3(int((p.dcolour >> 16) & 0xFF), int((p.dcolour >> 8) & 0xFF), int(p.dcolour & 0xFF));
            col.xyz = (a * d + (255 - a) * col.xyz) >> 8;
        }
        fire.xyz = col.xyz;
        if (p.tmp == 1)
        {
            mode = FIRE_ADD | PMODE_BLEND | PMODE_GLOW;
            fire.w = (p.life - 15) * 4;
            col.w = (p.life + 15) * 4;
        }
        else if (p.tmp == 2)
        {
            mode = PMODE_FLAT | FIRE_ADD;
            fire.w = 255;
        }
        else
        {
            mode = PMODE_SPARK | PMODE_ADD;
            if (p.life < 64)
                col.w = 4 * p.life;
        }
        break;
    }
    case PT_EMP:
        if (p.life != 0)
            col.xyz = int3(int(float(p.life) * 1.5), int(float(p.life) * 1.5), 200 - p.life);
        break;
    case PT_EXOT:
    {
        float q = p.temp;
        float b = float(p.tmp);
        float c = float(p.tmp2);
        if (p.life < 1001)
        {
            if (rngChance(p.tmp2 - 1, 1000))
            {
                float f = 0.04045;
                col.xyz = int3(int(sin(f * c + 4.0) * 127.0 + 150.0), int(sin(f * c + 6.0) * 127.0 + 150.0),
                               int(sin(f * c + 8.0) * 127.0 + 150.0));
                fire = int4(0, 0, 0, 100);
                mode |= PMODE_FLAT | PMODE_FLARE;
            }
            else
            {
                float f = 0.00045;
                col.xyz = int3(int(sin(f * q + 4.0) * 127.0 + (b / 1.7)), int(sin(f * q + 6.0) * 127.0 + (b / 1.7)),
                               int(sin(f * q + 8.0) * 127.0 + (b / 1.7)));
                col.w = p.tmp / 6;
                fire = int4(col.xyz, col.w);
                mode |= FIRE_ADD | PMODE_BLUR;
            }
        }
        else
        {
            float f = 0.01300;
            int v = int(sin(f * q + 6.0) * 127.0 + ((b / 2.9) + 80.0));
            col.xyz = int3(v, v, v);
            col.w = p.tmp / 6;
            fire = int4(col.xyz, col.w);
            mode |= FIRE_ADD | PMODE_BLUR;
        }
        break;
    }
    case PT_FILT:
    {
        col.xyz = wavelengthColour(filtWavelengths(p));
        col.w = (p.life > 0 && p.life <= 4) ? 127 + p.life * 30 : 127;
        mode &= ~PMODE;
        mode |= PMODE_BLEND;
        break;
    }
    case PT_FIRE:
        col.xyz = tableColour(RT_FLAME, 200, p.life);
        fire = int4(col.xyz, 255);
        mode = FIRE_ADD; // don't draw the pixel
        break;
    case PT_FIRW:
        if (p.tmp > 0)
            mode |= PMODE_GLOW;
        break;
    case PT_FRME:
        if (p.tmp != 0)
            col.xyz += int3(30, 30, 30);
        break;
    case PT_GBMB:
        mode |= p.life <= 0 ? PMODE_FLARE : PMODE_SPARK;
        break;
    case PT_GEL:
    {
        int q = p.tmp;
        col.xyz = int3(q * (32 - 255) / 120 + 255, q * (48 - 186) / 120 + 186, q * 208 / 120);
        mode |= PMODE_BLUR;
        break;
    }
    case PT_GLOW:
    {
        fire.x = 16 + int(restrictFlt2(p.temp - (273.15 + 34.0), 0.0, 128.0) / 2.0);
        fire.y = 16 + int(restrictFlt2(float(p.ctype), 0.0, 128.0) / 2.0);
        fire.z = 16 + int(restrictFlt2(float(p.tmp), 0.0, 128.0) / 2.0);
        fire.w = 64;
        col.x = int(restrictFlt2(64.0 + p.temp - (273.15 + 34.0), 0.0, 255.0));
        col.y = int(restrictFlt2(64.0 + float(p.ctype), 0.0, 255.0));
        col.z = int(restrictFlt2(64.0 + float(p.tmp), 0.0, 255.0));
        int rng = rngBetween(1, 32);
        if (col.x + col.y + col.z > 256 + rng)
        {
            col.xyz -= int3(54, 54, 54);
            mode |= FIRE_ADD | PMODE_GLOW | PMODE_ADD;
            mode &= ~PMODE_FLAT;
        }
        else
            mode |= PMODE_BLUR;
        break;
    }
    case PT_GOLD:
    {
        int rnd = int(rngGen()); // (TPT's gen(): any 32 bits; the speckle uses a few)
        col.x += (rnd % 10) - 5;
        rnd >>= 4;
        col.y += (rnd % 10) - 5;
        rnd >>= 4;
        col.z += (rnd % 10) - 5;
        break;
    }
    case PT_GPMP:
    {
        int lifemod = min(p.life, 10) * 19;
        col.y += lifemod;
        col.z += lifemod;
        break;
    }
    case PT_GRAV:
    {
        int tick = int(pc.tick);
        int gr = abs((tick % 120) - 60), gg = abs(((tick + 60) % 120) - 60), gb = abs(((tick + 120) % 120) - 60);
        int gr2 = abs((tick % 60) - 30), gg2 = abs(((tick + 30) % 60) - 30), gb2 = abs(((tick + 60) % 60) - 30);
        col.xyz = int3(20, 20, 20);
        if (p.vx > 0.0)
            col.xyz += int3(int(p.vx * float(gr)), int(p.vx * float(gg)), int(p.vx * float(gb)));
        if (p.vy > 0.0)
            col.xyz += int3(int(p.vy * float(gg)), int(p.vy * float(gb)), int(p.vy * float(gr)));
        if (p.vx < 0.0)
            col.xyz -= int3(int(p.vx * float(gb)), int(p.vx * float(gr)), int(p.vx * float(gg)));
        if (p.vy < 0.0)
            col.xyz -= int3(int(p.vy * float(gr2)), int(p.vy * float(gg2)), int(p.vy * float(gb2)));
        if (p.life != 0)
        {
            mode = FIRE_ADD | PMODE_ADD | PMODE_GLOW | PMODE_FLARE;
            fire = int4(min(col.x * 3, 255), min(col.y * 3, 255), min(col.z * 3, 255), (p.life + 15) * 4);
            col.w = (p.life + 15) * 4;
        }
        else
            mode = PMODE_ADD;
        break;
    }
    case PT_GRVT:
        fire = int4(0, 250, 170, 5);
        mode |= FIRE_BLEND;
        break;
    case PT_HSWC:
        col.x += min(p.life, 10) * 19;
        break;
    case PT_INVIS:
        if (p.tmp2 != 0)
        {
            col = int4(15, 0, 150, 100);
            mode = PMODE_BLEND;
        }
        break;
    case PT_LAVA:
        col.xyz = int3(min(p.life * 2 + 0xE0, 255), min(p.life + 0x50, 192), min(p.life / 2 + 0x10, 128));
        fire = int4(col.xyz, 40);
        mode |= FIRE_ADD | PMODE_BLUR;
        break;
    case PT_LCRY:
        if (decoShown(p.dcolour))
        {
            col.xyz = int3(int((p.dcolour >> 16) & 0xFF), int((p.dcolour >> 8) & 0xFF), int(p.dcolour & 0xFF));
            if (p.tmp2 < 10)
                col.xyz /= 10 - p.tmp2;
        }
        else
        {
            int v = 0x50 + min(p.tmp2, 10) * 10;
            col.xyz = int3(v, v, v);
        }
        mode |= NO_DECO;
        break;
    case PT_LIFE:
    {
        int3 colour1 = p.dcolour != 0 ? unpackRGB(p.dcolour) : int3(255, 255, 255);
        int3 colour2 = unpackRGB(uint(p.tmp));
        int ruleset = p.ctype;
        bool renderDeco = pc.decoLevel != DECO_ANTICLICKBAIT;
        if (ruleset >= 0 && ruleset < 24)
        {
            if (!renderDeco || pc.decoLevel == DECO_DISABLED)
            {
                colour1 = unpackRGB(renderTables[RT_GOL_COLOUR + uint(ruleset)]);
                colour2 = unpackRGB(renderTables[RT_GOL_COLOUR2 + uint(ruleset)]);
                renderDeco = true;
            }
            ruleset = int(renderTables[RT_GOL_RULESET + uint(ruleset)]);
        }
        if (renderDeco)
        {
            int states = ((ruleset >> 17) & 0xF) + 2;
            if (states == 2)
                col.xyz = colour1;
            else
            {
                float mul = float(p.tmp2 - 1) / float(states - 2);
                col.xyz = int3(float3(colour1) * mul + float3(colour2) * (1.0 - mul));
            }
        }
        mode |= NO_DECO;
        break;
    }
    case PT_LIGH:
        col.xyz = int3(235, 245, 255);
        fire = int4(col.xyz, 120);
        mode |= PMODE_GLOW | FIRE_ADD | DECO_FIRE;
        break;
    case PT_LITH:
        if (p.life >= 1000) // exploding
        {
            col.xyz = int3(0xFF, 0xA0, 0x40);
            mode |= PMODE_FLARE | PMODE_GLOW;
        }
        else if (p.ctype > 0) // charged
        {
            int mult = min(6, rngBetween(p.ctype / 3, p.ctype) / 15);
            col.x -= 30 * mult;
            col.z += 20 * mult;
            mode |= PMODE_FLARE | PMODE_GLOW;
        }
        break;
    case PT_NEUT:
        fire = int4(10, 80, 120, 120);
        mode |= FIRE_ADD;
        break;
    case PT_PBCN:
    {
        int lifemod = min(p.life, 10) * 10;
        col.x += lifemod;
        col.y += lifemod / 2;
        break;
    }
    case PT_PCLN:
    {
        int lifemod = min(p.life, 10) * 10;
        col.x += lifemod;
        col.y += lifemod;
        break;
    }
    case PT_PHOT:
        col.xyz = wavelengthColour(p.ctype);
        fire = int4(col.xyz, 100);
        mode &= ~PMODE_FLAT;
        mode |= FIRE_ADD | PMODE_ADD | NO_DECO;
        if ((p.flags & FLAG_PHOTDECO) != 0)
            mode &= ~NO_DECO;
        break;
    case PT_PLNT:
        if (p.ctype == 0)
        {
            float maxtemp = max(float(p.tmp2), p.temp);
            if (maxtemp > 300.0)
            {
                col.x += int(restrictFlt2((maxtemp - 300.0) / 5.0, 0.0, 58.0));
                col.y -= int(restrictFlt2((maxtemp - 300.0) / 2.0, 0.0, 102.0));
                col.z += int(restrictFlt2((maxtemp - 300.0) / 5.0, 0.0, 70.0));
            }
            if (maxtemp < 273.0)
            {
                col.y += int(restrictFlt2((273.0 - maxtemp) / 4.0, 0.0, 255.0));
                col.z += int(restrictFlt2((273.0 - maxtemp) / 1.5, 0.0, 255.0));
            }
        }
        else
        {
            int colour = (p.ctype >> GFX_PLNT_COLOUR) & 0x3F; // tree-grown PLNT
            int ind = 4 * int((colour & 0x30) != 0) + 2 * int((colour & 0x0C) != 0) + int((colour & 0x03) != 0);
            col.xyz = unpackRGB(renderTables[RT_LEAF + uint(ind)]);
        }
        break;
    case PT_PLSM:
        col.xyz = tableColour(RT_PLASMA, 200, p.life);
        fire = int4(col.xyz, 255);
        mode = PMODE_GLOW | PMODE_ADD | FIRE_ADD;
        break;
    case PT_POLO:
        if (p.tmp >= 5) // LIMIT
            col.xyz = int3(0x70, 0x70, 0x70);
        else
            mode |= PMODE_GLOW;
        break;
    case PT_PROT:
        fire = int4(250, 170, 170, 7);
        mode |= FIRE_BLEND;
        break;
    case PT_PRTI:
    case PT_PRTO:
        fire = t == PT_PRTI ? int4(255, 0, 0, 8) : int4(0, 0, 255, 8);
        mode |= EFFECT_DBGLINES | (t == PT_PRTI ? EFFECT_GRAVIN : EFFECT_GRAVOUT);
        mode &= ~PMODE;
        mode |= PMODE_ADD;
        break;
    case PT_PSTN:
        if (p.life != 0)
        {
            col.x -= 60;
            col.y -= 60;
        }
        break;
    case PT_PTNM:
        if (p.tmp != 0)
            mode |= PMODE_FLARE;
        break;
    case PT_PUMP:
        col.z += min(p.life, 10) * 19;
        break;
    case PT_PVOD:
        col.x += min(p.life, 10) * 16;
        break;
    case PT_ROCK:
    {
        int z = (p.tmp2 - 7) * 6; // colour noise
        col.xyz += int3(z, z, z);
        if (p.temp >= 810.15) // glows when hot, right before melting
        {
            mode |= FIRE_ADD;
            fire = int4(col.xyz, int((p.temp - 810.15) / 45.0));
        }
        break;
    }
    case PT_SEED:
    {
        int water = (p.ctype >> GFX_PLNT_LIFE) & 0xFF;
        if (water > 3) // ready to grow
            col.xyz -= int3(50, 50, 50);
        else if (water > 0)
            col.xyz -= int3(25, 25, 25);
        break;
    }
    case PT_SLCN:
    {
        int3 c = unpackRGB(renderTables[RT_SLCN + uint((p.tmp >> 12) & 15)]);
        if ((p.tmp & 0x800) != 0) // halfway to the next colour
            c = (c + unpackRGB(renderTables[RT_SLCN + uint((p.tmp >> 16) & 15)])) / 2;
        col.xyz = c;
        int rnd = (p.tmp & 0xFFFF) * ((p.tmp >> 16) & 0xFFFF);
        if (rnd % 887 == 0)
            mode |= PMODE_FLARE | PMODE_GLOW;
        if (rnd % 593 == 0)
            mode |= PMODE_SPARK;
        break;
    }
    case PT_SMKE:
        col.xyz = int3(55, 55, 55);
        fire = int4(55, 55, 55, 75);
        mode = FIRE_BLEND; // don't draw the pixel
        break;
    case PT_SOAP:
        mode |= EFFECT_LINES | PMODE_BLUR;
        break;
    case PT_SPNG:
        col.xyz -= int3(p.life * 15, p.life * 15, p.life * 15);
        col.x = max(col.x, 50);
        col.y = max(col.y, 50);
        col.z = max(col.z, 20);
        break;
    case PT_SPRK:
        fire = int4(col.x / 2, col.y / 2, col.z / 2, 60);
        mode |= FIRE_SPARK;
        break;
    case PT_STKM:
    case PT_STKM2:
    case PT_FIGH:
        col = int4(0, 0, 0, 0);
        mode = PSPEC_STICKMAN;
        break;
    case PT_STOR:
        if (p.tmp != 0)
        {
            mode |= PMODE_GLOW;
            col.xyz = int3(0x50, 0xDF, 0xDF);
        }
        else
            col.xyz = int3(0x20, 0xAF, 0xAF);
        break;
    case PT_SWCH:
        if (p.life >= 10)
        {
            col.xyz = int3(17, 217, 24);
            mode |= PMODE_GLOW;
        }
        break;
    case PT_THDR:
        fire = int4(144, 192, 255, 160);
        mode |= FIRE_ADD;
        break;
    case PT_TRON:
    {
        col.xyz = unpackRGB(renderTables[RT_TRON + uint((p.tmp & 0xF800) >> 11)]);
        if ((p.tmp & 1) != 0) // TRON_HEAD
            mode |= PMODE_GLOW;
        if ((p.tmp & 0x10) != 0) // TRON_DEATH
        {
            mode |= FIRE_ADD | PMODE_FLARE;
            fire = int4(col.xyz, 255);
        }
        if (p.life < p.tmp2 && (p.tmp & 1) == 0)
        {
            mode |= PMODE_BLEND;
            mode &= ~PMODE_FLAT;
            col.w = int((float(p.life) / float(p.tmp2)) * 255.0);
        }
        break;
    }
    case PT_TUNG:
    {
        float meltingPoint = elems[PT_TUNG].highTemperature;
        float startTemp = meltingPoint - 1500.0;
        float tempOver = ((p.temp - startTemp) / 1500.0) * 3.14159265 - 3.14159265 / 2.0;
        if (tempOver > -(3.14159265 / 2.0))
        {
            tempOver = min(tempOver, 3.14159265 / 2.0);
            float gradv = sin(tempOver) + 1.0;
            fire = int4(int(gradv * 258.0), int(gradv * 156.0), int(gradv * 112.0), 30);
            col.xyz += fire.xyz;
            mode |= FIRE_ADD;
        }
        break;
    }
    case PT_VIBR:
    case PT_BVBR:
    {
        int gradient = p.tmp / 10;
        if (gradient >= 100 || p.life != 0)
        {
            col.x = int(abs(sin(exp((750.0 - float(p.life)) / 170.0))) * 200.0);
            if (p.tmp2 != 0)
                col.yz = int2(col.x, 255);
            else
                col.yz = int2(255, col.x);
            fire = int4(col.xyz, 90);
            mode = FIRE_BLEND;
        }
        else
        {
            col.x += int(restrictFlt2(float(gradient) * 2.0, 0.0, 255.0));
            col.y += int(restrictFlt2(float(gradient) * 2.0, 0.0, 175.0));
            col.z += int(restrictFlt2(float(gradient) * 2.0, 0.0, 255.0));
            fire = int4(col.x / 2, col.y / 2, col.z / 2, int(restrictFlt2(float(gradient) * 0.6, 0.0, 60.0)));
            mode |= FIRE_BLEND;
        }
        break;
    }
    case PT_VINE:
    {
        float maxtemp = max(float(p.tmp2), p.temp);
        if (maxtemp > 300.0)
        {
            col.x += int(restrictFlt2((maxtemp - 300.0) / 5.0, 0.0, 58.0));
            col.y -= int(restrictFlt2((maxtemp - 300.0) / 2.0, 0.0, 102.0));
            col.z += int(restrictFlt2((maxtemp - 300.0) / 5.0, 0.0, 70.0));
        }
        if (maxtemp < 273.0)
        {
            col.y += int(restrictFlt2((273.0 - maxtemp) / 4.0, 0.0, 255.0));
            col.z += int(restrictFlt2((273.0 - maxtemp) / 1.5, 0.0, 255.0));
        }
        break;
    }
    case PT_VIRS:
        mode |= PMODE_BLUR | NO_DECO;
        break;
    case PT_VRSG:
        mode &= ~PMODE;
        mode |= FIRE_BLEND | NO_DECO;
        fire = int4(col.x / 2, col.y / 2, col.z / 2, 125);
        break;
    case PT_VRSS:
        mode |= NO_DECO;
        break;
    case PT_WARP:
        col = int4(0, 0, 0, 0);
        mode |= NO_DECO;
        break;
    case PT_WIFI:
    {
        const float freq = 0.0628;
        int q = int((p.temp - 73.15) / 100.0 + 1.0);
        col.xyz = int3(int(sin(freq * float(q)) * 127.0 + 128.0), int(sin(freq * float(q) + 2.0) * 127.0 + 128.0),
                       int(sin(freq * float(q) + 4.0) * 127.0 + 128.0));
        mode |= EFFECT_DBGLINES;
        break;
    }
    case PT_WIRE:
        if (p.ctype == 0)
            col.xyz = int3(255, 204, 0);
        else if (p.ctype == 1)
            col.xyz = int3(50, 100, 255);
        else if (p.ctype == 2)
            col.xyz = int3(255, 100, 50);
        break;
    case PT_WOOD:
    {
        float maxtemp = max(float(p.tmp), p.temp);
        if (maxtemp > 400.0)
        {
            col.x -= int(restrictFlt2((maxtemp - 400.0) / 3.0, 0.0, 172.0));
            col.y -= int(restrictFlt2((maxtemp - 400.0) / 4.0, 0.0, 140.0));
            col.z -= int(restrictFlt2((maxtemp - 400.0) / 20.0, 0.0, 44.0));
        }
        if (maxtemp < 273.0)
        {
            col.x -= int(restrictFlt2((273.0 - maxtemp) / 5.0, 0.0, 40.0));
            col.y += int(restrictFlt2((273.0 - maxtemp) / 4.0, 0.0, 40.0));
            col.z += int(restrictFlt2((273.0 - maxtemp) / 1.5, 0.0, 150.0));
        }
        break;
    }
    case PT_BIZR:
    case PT_BIZRG:
    case PT_BIZRS:
    {
        float brightness = abs(p.vx) + abs(p.vy);
        if ((p.ctype & 0x3FFFFFFF) != 0)
            col.xyz = wavelengthColour(p.ctype);
        if (brightness > 0.0)
        {
            brightness /= 5.0;
            fire = int4(int(float(col.x) * brightness), int(float(col.y) * brightness), int(float(col.z) * brightness),
                        255);
            mode |= FIRE_ADD;
        }
        mode |= PMODE_BLUR;
        break;
    }
    default:
        defaultGraphics(t, mode, col, fire);
        break;
    }
}

// Element_PIPE_graphics: a pipe holding something looks like what it holds
void pipeGraphics(Particle p, inout uint mode, inout int4 col, inout int4 fire)
{
    uint t = uint(p.ctype) & 0x1FFu;
    if (t > 0 && t < MAX_ELEMENTS && elems[t].existsInTpt != 0)
    {
        if (isStickman(t))
            return;
        col.xyz = unpackRGB(elems[t].colour);
        Particle q = (Particle)0; // props_pipe_to_part
        q.temp = p.temp;
        q.life = p.tmp2;
        q.tmp = p.tmp3;
        q.ctype = p.tmp4;
        elementGraphics(t, q, mode, col, fire);
    }
    else
    {
        int c = p.tmp & PFLAG_COLORS;
        if (c == PFLAG_COLOR_RED)
            col.xyz = int3(50, 1, 1);
        else if (c == PFLAG_COLOR_GREEN)
            col.xyz = int3(1, 50, 1);
        else if (c == PFLAG_COLOR_BLUE)
            col.xyz = int3(1, 1, 50);
    }
}

// A line of pixels: RasterizeLine<false> (common/RasterGeometry.h), as TPT's DrawLine and BlendLine
void fragLine(int ax, int ay, int bx, int by, uint key, uint op, int4 c)
{
    bool wide = abs(ax - bx) >= abs(ay - by);
    int sx, sy, dz, dw;
    if (wide ? ax < bx : ay < by)
    {
        sx = ax;
        sy = ay;
        dz = wide ? bx - ax : by - ay;
        dw = wide ? by - ay : bx - ax;
    }
    else
    {
        sx = bx;
        sy = by;
        dz = wide ? ax - bx : ay - by;
        dw = wide ? ay - by : ax - bx;
    }
    int incW = dw >= 0 ? 1 : -1, w = 0, err = 0;
    for (int z = 0; z <= dz && z < 4096; z++)
    {
        if (wide)
            drawPixel(sx + z, sy + w, key, op, c);
        else
            drawPixel(sx + w, sy + z, key, op, c);
        err += 2 * dw * incW;
        if (err >= dz)
        {
            w += incW;
            err -= 2 * dz;
        }
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.capacity)
        return;
    uint t = ptype[i];
    gfx[3 * i] = 0;
    if (t == PT_NONE)
        return;
    Particle p = parts[i];
    int nx = int(p.x + 0.5), ny = int(p.y + 0.5);
    if (!inVideo(nx, ny))
        return;
    uint c = cellIndex(nx, ny);
    bool energy = (elems[t].properties & TYPE_ENERGY) != 0;
    // Where there is an energy particle, only energy particles and stick men are drawn
    int pid = pmapId(photons[c]);
    if (pid >= 0 && ptype[pid] != PT_NONE && !energy && !isStickman(t))
        return;
    rngSeed(i * 0x9E3779B1u + pc.tick * 0x85EBCA77u + 0x27D4EB2Fu);

    uint mode = PMODE_FLAT;
    int4 col = int4(unpackRGB(elems[t].colour), 255);
    int4 fire = int4(0, 0, 0, 0);
    int deca = int((p.dcolour >> 24) & 0xFF), decr = int((p.dcolour >> 16) & 0xFF);
    int decg = int((p.dcolour >> 8) & 0xFF), decb = int(p.dcolour & 0xFF);
    if (pc.decoLevel == DECO_ANTICLICKBAIT)
    {
        if (deca < 250 || decr > 5 || decg > 5 || decb > 5)
            deca = 0;
        else
        {
            deca = 255;
            decr = decg = decb = 0;
        }
    }

    if ((pc.colorMode & COLOUR_BASC) == 0)
    {
        if (t == PT_PIPE || t == PT_PPIP)
            pipeGraphics(p, mode, col, fire);
        else
            elementGraphics(t, p, mode, col, fire);
    }
    ElementProps e = elems[t];
    if ((e.properties & PROP_HOT_GLOW) != 0 && p.temp > e.highTemperature - 800.0)
    {
        float gradv = 3.14159265 / (2.0 * e.highTemperature - (e.highTemperature - 800.0));
        int caddress = int(p.temp > e.highTemperature ? e.highTemperature - (e.highTemperature - 800.0)
                                                       : p.temp - (e.highTemperature - 800.0));
        col.x += int(sin(gradv * float(caddress)) * 226.0);
        col.y += int(-sin(gradv * float(caddress) * 4.55) * 34.0);
        col.z += int(-sin(gradv * float(caddress) * 2.22) * 64.0);
    }

    // What the display preset draws
    uint renderMode = pc.renderMode;
    if ((mode & FIRE_ADD) != 0 && (renderMode & FIRE_ADD) == 0)
        mode |= PMODE_GLOW;
    if ((mode & FIRE_BLEND) != 0 && (renderMode & FIRE_BLEND) == 0)
        mode |= PMODE_BLUR;
    if ((mode & PMODE_BLUR) != 0 && (renderMode & PMODE_BLUR) == 0)
        mode |= PMODE_FLAT;
    if ((mode & PMODE_GLOW) != 0 && (renderMode & PMODE_GLOW) == 0)
        mode |= PMODE_BLEND;
    if ((renderMode & PMODE_BLOB) != 0)
        mode |= PMODE_BLOB;
    mode &= renderMode;

    // Colour modes
    if ((pc.colorMode & COLOUR_HEAT) != 0)
    {
        float2 hr = hdispRange();
        int3 h = tableColour(RT_HEAT, 1024, int((p.temp - hr.x) / (hr.y - hr.x) * 1024.0));
        fire = int4(h, 255);
        col = int4(h, 255);
        if ((mode & (FIREMODE | PMODE_GLOW)) != 0)
            mode = (mode & ~(FIREMODE | PMODE_GLOW)) | PMODE_BLUR;
        else if ((mode & (PMODE_BLEND | PMODE_ADD)) == (PMODE_BLEND | PMODE_ADD))
            mode = (mode & ~(PMODE_BLEND | PMODE_ADD)) | PMODE_FLAT;
        else if (mode == 0)
            mode |= PMODE_FLAT;
    }
    else if ((pc.colorMode & COLOUR_LIFE) != 0)
    {
        float gradv = 0.4;
        int q = p.life < 5 ? p.life : int(sqrt(float(p.life)));
        int v = int(sin(gradv * float(q)) * 100.0 + 128.0);
        col = int4(v, v, v, 255);
        if ((mode & (FIREMODE | PMODE_GLOW)) != 0)
            mode = (mode & ~(FIREMODE | PMODE_GLOW)) | PMODE_BLUR;
        else if ((mode & (PMODE_BLEND | PMODE_ADD)) == (PMODE_BLEND | PMODE_ADD))
            mode = (mode & ~(PMODE_BLEND | PMODE_ADD)) | PMODE_FLAT;
        else if (mode == 0)
            mode |= PMODE_FLAT;
    }
    else if ((pc.colorMode & COLOUR_BASC) != 0)
    {
        col.xyz = unpackRGB(e.colour);
        mode = PMODE_FLAT;
    }

    // Decoration
    if ((pc.colorMode & ~COLOUR_GRAD) == 0 && pc.decoLevel != DECO_DISABLED && deca != 0)
    {
        deca++;
        int3 d = int3(decr, decg, decb);
        if ((mode & NO_DECO) == 0)
            col.xyz = (deca * d + (256 - deca) * col.xyz) >> 8;
        if ((mode & DECO_FIRE) != 0)
            fire.xyz = (deca * d + (256 - deca) * fire.xyz) >> 8;
    }
    if ((pc.colorMode & COLOUR_GRAD) != 0)
    {
        float frequency = 0.05;
        int q = int(p.temp - 40.0);
        col.xyz = int3(float3(col.xyz) + sin(frequency * float(q)) * 16.0);
        if ((mode & (FIREMODE | PMODE_GLOW)) != 0)
            mode = (mode & ~(FIREMODE | PMODE_GLOW)) | PMODE_BLUR;
    }
    col = clamp(col, int4(0, 0, 0, 0), int4(255, 255, 255, 255));
    fire = clamp(fire, int4(0, 0, 0, 0), int4(255, 255, 255, 255));

    // The top particle's own pixel is drawn by resolve.slang from here; a hidden one's become fragments.
    bool top = (energy ? photons[c] : pmap[c]) == pmapEnc(i);
    gfx[3 * i] = mode;
    gfx[3 * i + 1] = packRGBA(col.x, col.y, col.z, col.w);
    gfx[3 * i + 2] = packRGBA(fire.x, fire.y, fire.z, fire.w);
    uint key = i << 5;
    int4 opaque = int4(col.xyz, 255);
    if (top && (mode & (PMODE_GLOW | PMODE_BLUR)) != 0)
    {
        uint o;
        InterlockedOr(gfxFlags[uint(ny / CELL) * pc.CW + uint(nx / CELL)],
                      ((mode & PMODE_GLOW) != 0 ? GFX_FLAG_GLOW : 0u) | ((mode & PMODE_BLUR) != 0 ? GFX_FLAG_BLUR : 0u), o);
    }

    if ((mode & EFFECT_LINES) != 0 && t == PT_SOAP && (p.ctype & 3) == 3 && p.tmp >= 0 && uint(p.tmp) < pc.capacity)
    {
        // SOAP's links (Element_SOAP_neighourLoop: across the world, the short way round)
        float dx = parts[p.tmp].x - float(nx), dy = parts[p.tmp].y - float(ny);
        if (dx >= float(int(pc.W) / 2 - CELL))
            dx -= float(int(pc.W) - 2 * CELL);
        if (dy >= float(int(pc.H) / 2 - CELL))
            dy -= float(int(pc.H) - 2 * CELL);
        if (dx < -float(int(pc.W) / 2 - CELL))
            dx += float(int(pc.W) - 2 * CELL);
        if (dy < -float(int(pc.H) / 2 - CELL))
            dy += float(int(pc.H) - 2 * CELL);
        fragLine(nx, ny, int(float(nx) + dx + 0.5), int(float(ny) + dy + 0.5), key | STEP_LINES, OP_BLEND, col);
    }
    if ((mode & PSPEC_STICKMAN) != 0)
    {
        int s = t == PT_STKM ? 0 : t == PT_STKM2 ? 1 : (p.tmp >= 0 && p.tmp < int(MAX_FIGHTERS) ? 2 + p.tmp : -1);
        if (s >= 0 && stickmen[s].spwn != 0)
        {
            Stickman st = stickmen[s];
            int4 head, legs;
            if (st.fan != 0)
                head = int4(0x80, 0x80, 0xFF, 255);
            else if (st.elem > 0 && st.elem < int(MAX_ELEMENTS))
                head = int4(unpackRGB(elems[st.elem].colour), 255);
            else
                head = int4(0x80, 0x80, 0xFF, 255);
            if ((pc.colorMode & COLOUR_HEAT) != 0)
            {
                head = opaque; // (heat colour)
                legs = head;
            }
            else
                legs = t == PT_STKM2 ? int4(100, 100, 255, 255) : int4(255, 255, 255, 255);
            uint k = key | STEP_STKM_LINES;
            if (t == PT_FIGH)
            {
                fragLine(nx, ny + 2, nx + 2, ny, k, OP_SET, head);
                fragLine(nx + 2, ny, nx, ny - 2, k, OP_SET, head);
                fragLine(nx, ny - 2, nx - 2, ny, k, OP_SET, head);
                fragLine(nx - 2, ny, nx, ny + 2, k, OP_SET, head);
            }
            else
            {
                fragLine(nx - 2, ny + 2, nx + 2, ny + 2, k, OP_SET, head);
                fragLine(nx - 2, ny - 2, nx + 2, ny - 2, k, OP_SET, head);
                fragLine(nx - 2, ny - 2, nx - 2, ny + 2, k, OP_SET, head);
                fragLine(nx + 2, ny - 2, nx + 2, ny + 2, k, OP_SET, head);
            }
            fragLine(nx, ny + 3, int(st.legs[0]), int(st.legs[1]), k, OP_SET, legs);
            fragLine(int(st.legs[0]), int(st.legs[1]), int(st.legs[4]), int(st.legs[5]), k, OP_SET, legs);
            fragLine(nx, ny + 3, int(st.legs[8]), int(st.legs[9]), k, OP_SET, legs);
            fragLine(int(st.legs[8]), int(st.legs[9]), int(st.legs[12]), int(st.legs[13]), k, OP_SET, legs);
            if (st.rocketBoots != 0)
            {
                uint kb = key | STEP_STKM_BOOTS;
                for (int leg = 0; leg < 2; leg++)
                {
                    int fx = int(st.legs[leg * 8 + 4]), fy = int(st.legs[leg * 8 + 5]);
                    bool firing = (st.comm & 4) != 0 || ((st.comm & 1) != 0 && leg == 0) || ((st.comm & 2) != 0 && leg == 1);
                    drawPixel(fx, fy, kb, OP_SET, firing ? int4(0, 255, 0, 255) : int4(255, 0, 0, 255));
                    int4 flame = int4(255, 0, 255, 223);
                    drawPixel(fx + 1, fy, kb, OP_BLEND, flame);
                    drawPixel(fx - 1, fy, kb, OP_BLEND, flame);
                    drawPixel(fx, fy + 1, kb, OP_BLEND, flame);
                    drawPixel(fx, fy - 1, kb, OP_BLEND, flame);
                    flame.w = 112;
                    drawPixel(fx + 1, fy - 1, kb, OP_BLEND, flame);
                    drawPixel(fx - 1, fy - 1, kb, OP_BLEND, flame);
                    drawPixel(fx + 1, fy + 1, kb, OP_BLEND, flame);
                    drawPixel(fx - 1, fy + 1, kb, OP_BLEND, flame);
                }
            }
        }
    }
    if (!top)
    {
        if ((mode & PMODE_FLAT) != 0)
            drawPixel(nx, ny, key | STEP_FLAT, OP_SET, opaque);
        if ((mode & PMODE_BLEND) != 0)
            drawPixel(nx, ny, key | STEP_BLEND, OP_BLEND, col);
        if ((mode & PMODE_ADD) != 0)
            drawPixel(nx, ny, key | STEP_ADD, OP_ADD, col);
        if ((mode & PMODE_BLOB) != 0)
            drawPixel(nx, ny, key | STEP_BLOB, OP_SET, opaque);
    }
    if ((mode & PMODE_BLOB) != 0)
    {
        uint k = key | STEP_BLOB;
        int4 b = int4(col.xyz, 223);
        drawPixel(nx + 1, ny, k, OP_BLEND, b);
        drawPixel(nx - 1, ny, k, OP_BLEND, b);
        drawPixel(nx, ny + 1, k, OP_BLEND, b);
        drawPixel(nx, ny - 1, k, OP_BLEND, b);
        b.w = 112;
        drawPixel(nx + 1, ny - 1, k, OP_BLEND, b);
        drawPixel(nx - 1, ny - 1, k, OP_BLEND, b);
        drawPixel(nx + 1, ny + 1, k, OP_BLEND, b);
        drawPixel(nx - 1, ny + 1, k, OP_BLEND, b);
    }
    // (A top particle's glow and blur are gathered by resolve.slang; only hidden ones' become fragments)
    if ((mode & PMODE_GLOW) != 0 && !top)
    {
        uint k = key | STEP_GLOW;
        int cola1 = (5 * col.w) / 255;
        drawPixel(nx, ny, k, OP_ADD, int4(col.xyz, (192 * col.w) / 255));
        int4 g = int4(col.xyz, (96 * col.w) / 255);
        drawPixel(nx + 1, ny, k, OP_ADD, g);
        drawPixel(nx - 1, ny, k, OP_ADD, g);
        drawPixel(nx, ny + 1, k, OP_ADD, g);
        drawPixel(nx, ny - 1, k, OP_ADD, g);
        int4 g1 = int4(col.xyz, cola1);
        if (cola1 > 0)
            for (int x = 1; x < 6; x++)
            {
                drawPixel(nx, ny - x, k, OP_ADD, g1);
                drawPixel(nx, ny + x, k, OP_ADD, g1);
                drawPixel(nx - x, ny, k, OP_ADD, g1);
                drawPixel(nx + x, ny, k, OP_ADD, g1);
                for (int y = 1; y < 6; y++)
                {
                    if (x + y > 7)
                        continue;
                    drawPixel(nx + x, ny - y, k, OP_ADD, g1);
                    drawPixel(nx - x, ny + y, k, OP_ADD, g1);
                    drawPixel(nx + x, ny + y, k, OP_ADD, g1);
                    drawPixel(nx - x, ny - y, k, OP_ADD, g1);
                }
            }
    }
    if ((mode & PMODE_BLUR) != 0 && !top)
    {
        for (int x = -3; x < 4; x++)
            for (int y = -3; y < 4; y++)
            {
                int d = abs(x) + abs(y);
                if (d < 2 && !(abs(x) == 2 || abs(y) == 2))
                    drawPixel(x + nx, y + ny, key | STEP_BLUR, OP_BLEND, int4(col.xyz, 30));
                if (d <= 3 && d != 0)
                    drawPixel(x + nx, y + ny, key | (STEP_BLUR + 1), OP_BLEND, int4(col.xyz, 20));
                if (d == 2)
                    drawPixel(x + nx, y + ny, key | (STEP_BLUR + 2), OP_BLEND, int4(col.xyz, 10));
            }
    }
    if ((mode & PMODE_SPARK) != 0)
    {
        float flicker = float(rngNext() % 20);
        float gradv = 4.0 * float(p.life) + flicker;
        for (int x = 0; gradv > 0.5 && x < 1024; x++)
        {
            int4 s = int4(min(0xFF, col.x * int(gradv) / 255), min(0xFF, col.y * int(gradv) / 255),
                          min(0xFF, col.z * int(gradv) / 255), 255);
            drawPixel(nx + x, ny, key | STEP_SPARK, OP_ADD, s);
            drawPixel(nx - x, ny, key | STEP_SPARK, OP_ADD, s);
            drawPixel(nx, ny + x, key | STEP_SPARK, OP_ADD, s);
            drawPixel(nx, ny - x, key | STEP_SPARK, OP_ADD, s);
            gradv = gradv / 1.5;
        }
    }
    for (int f = 0; f < 2; f++)
    {
        uint which = f == 0 ? PMODE_FLARE : PMODE_LFLARE;
        if ((mode & which) == 0)
            continue;
        uint k = key | (f == 0 ? STEP_FLARE : STEP_LFLARE), kr = key | (f == 0 ? STEP_FLARE_RAYS : STEP_LFLARE_RAYS);
        float flicker = float(rngNext() % 20);
        float gradv = flicker + abs(p.vx) * 17.0 + abs(p.vy) * 17.0;
        int a4 = int(gradv * 4.0 > 255.0 ? 255.0 : gradv * 4.0), a2 = int(gradv * 2.0 > 255.0 ? 255.0 : gradv * 2.0);
        drawPixel(nx, ny, k, OP_BLEND, int4(col.xyz, a4));
        drawPixel(nx + 1, ny, k, OP_BLEND, int4(col.xyz, a2));
        drawPixel(nx - 1, ny, k, OP_BLEND, int4(col.xyz, a2));
        drawPixel(nx, ny + 1, k, OP_BLEND, int4(col.xyz, a2));
        drawPixel(nx, ny - 1, k, OP_BLEND, int4(col.xyz, a2));
        if (gradv > 255.0)
            gradv = 255.0;
        int4 d = int4(col.xyz, int(gradv));
        drawPixel(nx + 1, ny - 1, k, OP_BLEND, d);
        drawPixel(nx - 1, ny - 1, k, OP_BLEND, d);
        drawPixel(nx + 1, ny + 1, k, OP_BLEND, d);
        drawPixel(nx - 1, ny + 1, k, OP_BLEND, d);
        float fall = f == 0 ? 1.2 : 1.01;
        for (int x = 1; gradv > 0.5 && x < 1024; x++)
        {
            int4 r = int4(col.xyz, int(gradv));
            drawPixel(nx + x, ny, kr, OP_ADD, r);
            drawPixel(nx - x, ny, kr, OP_ADD, r);
            drawPixel(nx, ny + x, kr, OP_ADD, r);
            drawPixel(nx, ny - x, kr, OP_ADD, r);
            gradv = gradv / fall;
        }
    }
    for (int g = 0; g < 2; g++)
    {
        uint which = g == 0 ? EFFECT_GRAVIN : EFFECT_GRAVOUT;
        if ((mode & which) == 0)
            continue;
        uint avoid = g == 0 ? PT_PRTI : PT_PRTO;
        for (int r = 0; r < 4; r++)
        {
            int orbd = (p.life >> (8 * r)) & 0xFF, orbl = (p.ctype >> (8 * r)) & 0xFF;
            float ddist = float(orbd) / 16.0;
            float drad = (float(orbl) * 3.14159265 / 180.0) * 1.41421356;
            int nxo = int(ddist * cos(drad)), nyo = int(ddist * sin(drad));
            if (ny + nyo > 0 && ny + nyo < int(pc.H) && nx + nxo > 0 && nx + nxo < int(pc.W) &&
                pmapType(pmap[cellIndex(nx + nxo, ny + nyo)]) != avoid)
                drawPixel(nx + nxo, ny + nyo, key | (g == 0 ? STEP_GRAVIN : STEP_GRAVOUT), OP_ADD,
                          int4(col.xyz, 255 - orbd));
        }
    }

    // A hidden particle's fire goes into its cell's list (the top ones' are gathered by fire.slang)
    if (!top && fire.w != 0 && (mode & (FIRE_ADD | FIRE_BLEND | FIRE_SPARK)) != 0)
        pushFrag(fireList(uint(ny / CELL) * pc.CW + uint(nx / CELL)), key, 0u,
                 packRGBA(fire.x, fire.y, fire.z, fire.w), mode);
}
