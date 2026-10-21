// EMP's effect on electronics: TPT's Element_EMP_Trigger, which AfterSim runs when sparks set off EMP this tick
// (counters[CTR_EMP] times). TPT walks every spark, lit WIRE and switching SWCH (the centres) in id order: an
// electronic centre (on PSCN, NSCN, PTCT, NTCT, INST or SWCH, or WIRE or SWCH itself) heats and may break, and
// each centre damages the electronics within 2 px of it.
//
// Here each particle is one thread and gathers the centres around it instead, taking their effects in the same
// order (by centre id). Phase 0 marks the centres as they are before any change (in pad1), phase 1 applies the
// effects. A SWCH centre that an earlier centre breaks still counts as a centre here, which in TPT it doesn't.
#include "common.slang"
#include "elements.slang"

// Probability::binomial_gte1: the chance that n tries with chance p each succeed at least once.
float binomialGte1(int n, float p) { return 1.0 - pow(1.0 - p, float(n)); }

// EMP.cpp's DeltaTempGenerator: heats by a number of steps drawn from binomial(n, p), capped at what reaches
// MAX_TEMP (Probability::SmallKBinomialGenerator).
struct DeltaTemp
{
    float cdf[10];
    uint maxK;
    float step;

    [mutating]
    void init(uint n, float p, float stepSize)
    {
        step = stepSize;
        maxK = (MAX_TEMP / stepSize < 10.0) ? uint(MAX_TEMP / stepSize) + 1 : 10;
        float pTerm = pow(1.0 - p, float(n));
        uint coeffN = 1, coeffD = 1;
        float acc = 0.0;
        for (uint k = 0; k < 10; k++)
        {
            if (k < maxK)
            {
                acc += pTerm * (float(coeffN) / float(coeffD));
                cdf[k] = acc;
                pTerm *= p / (1.0 - p);
                coeffN *= n - k;
                coeffD *= k + 1;
            }
            else
                cdf[k] = 2.0;
        }
    }

    float delta(float u)
    {
        for (uint k = 0; k < maxK; k++)
            if (u < cdf[k])
                return float(k) * step;
        return float(maxK) * step;
    }

    void apply(inout Particle q) { q.temp = clamp(q.temp + delta(rngUniform01()), MIN_TEMP, MAX_TEMP); }
};

// 0: not a centre, 1: a centre, 2: an electronic centre.
uint centreKind(uint t, Particle q)
{
    if (!(t == PT_SPRK || (t == PT_SWCH && q.life != 0 && q.life != 10) || (t == PT_WIRE && q.ctype > 0)))
        return 0;
    int c = q.ctype;
    bool elec = c == int(PT_PSCN) || c == int(PT_NSCN) || c == int(PT_PTCT) || c == int(PT_NTCT) ||
                c == int(PT_INST) || c == int(PT_SWCH) || t == PT_WIRE || t == PT_SWCH;
    return elec ? 2 : 1;
}

static int g_count;
static DeltaTemp g_tempCenter, g_tempMetal, g_tempSWCH;

// create_part(n, x, y, PT_BREC) followed by temp += 1000.
void breakToBrec(inout uint t, inout Particle q, int x, int y)
{
    if (!gpuHas(PT_BREC))
        return;
    q = newParticle(PT_BREC, x, y, q.pass);
    t = PT_BREC;
    q.temp = clamp(q.temp + 1000.0, MIN_TEMP, MAX_TEMP);
}

// What one centre does to the particle on top within its 5x5 (TPT's neighbour loop).
void neighbourEffect(bool elec, inout uint t, inout Particle q, int x, int y)
{
    uint ntype = t;
    if (elec)
    {
        switch (ntype)
        {
        case PT_METL:
            g_tempMetal.apply(q);
            if (rngUniform01() < binomialGte1(g_count, 1.0 / 300.0))
            {
                t = PT_BMTL;
                if (rngUniform01() < binomialGte1(g_count / 2, 1.0 / 160.0))
                {
                    t = PT_BRMT;
                    q.temp = clamp(q.temp + 1000.0, MIN_TEMP, MAX_TEMP);
                }
            }
            break;
        case PT_BMTL:
            g_tempMetal.apply(q);
            if (rngUniform01() < binomialGte1(g_count, 1.0 / 160.0))
            {
                t = PT_BRMT;
                q.temp = clamp(q.temp + 1000.0, MIN_TEMP, MAX_TEMP);
            }
            break;
        case PT_WIFI:
            if (rngUniform01() < binomialGte1(g_count, 1.0 / 8.0))
                q.temp = float(rngBetween(0, int(MAX_TEMP) - 1)); // a random channel
            if (rngUniform01() < binomialGte1(g_count, 1.0 / 16.0))
                breakToBrec(t, q, x, y);
            return;
        default:
            break;
        }
    }
    switch (ntype)
    {
    case PT_SWCH:
        if (rngUniform01() < binomialGte1(g_count, 1.0 / 100.0))
            t = PT_BREC;
        g_tempSWCH.apply(q);
        break;
    case PT_ARAY:
        if (rngUniform01() < binomialGte1(g_count, 1.0 / 60.0))
            breakToBrec(t, q, x, y);
        break;
    case PT_DLAY:
        if (rngUniform01() < binomialGte1(g_count, 1.0 / 70.0))
            q.temp = float(rngBetween(0, 255)) + 273.15; // a random delay
        break;
    default:
        break;
    }
}

bool affectedAsNeighbour(uint t)
{
    return t == PT_METL || t == PT_BMTL || t == PT_WIFI || t == PT_SWCH || t == PT_ARAY || t == PT_DLAY;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint n = tid.x;
    if (n >= pc.capacity)
        return;
    uint t = ptype[n];
    if (t == PT_NONE)
        return;
    Particle q = parts[n];
    if (pc.colourIdx == 0)
    {
        parts[n].pad1 = centreKind(t, q);
        return;
    }
    uint selfKind = q.pad1;
    if (selfKind == 0 && !affectedAsNeighbour(t))
        return;
    int x = int(q.x + 0.5), y = int(q.y + 0.5);
    if (!inSim(x, y))
        return;
    bool onTop = !isEnergy(t) && pmapId(pmap[cellIndex(x, y)]) == int(n);
    if (!onTop && selfKind == 0)
        return;

    // The centres whose 5x5 covers this particle (itself included), in id order
    uint ids[25] = {};
    uint kinds[25] = {};
    uint m = 0;
    if (!onTop)
    {
        ids[0] = n; // a stacked centre only acts on itself and what is on top around it
        kinds[0] = selfKind;
        m = 1;
    }
    else
    {
        for (int dy = -2; dy <= 2; dy++)
        {
            for (int dx = -2; dx <= 2; dx++)
            {
                int cx = x + dx, cy = y + dy;
                if (!inSim(cx, cy))
                    continue;
                int cid = pmapId(pmap[cellIndex(cx, cy)]);
                if (cid < 0 || ptype[cid] == PT_NONE)
                    continue;
                uint kind = parts[cid].pad1;
                if (kind == 0)
                    continue;
                uint j = m++;
                while (j > 0 && ids[j - 1] > uint(cid))
                {
                    ids[j] = ids[j - 1];
                    kinds[j] = kinds[j - 1];
                    j--;
                }
                ids[j] = uint(cid);
                kinds[j] = kind;
            }
        }
    }
    if (m == 0)
        return;

    g_count = int(counters[CTR_EMP]);
    g_tempCenter.init(uint(g_count), 1.0 / 100.0, 3000.0);
    g_tempMetal.init(uint(g_count), 1.0 / 280.0, 3000.0);
    g_tempSWCH.init(uint(g_count), 1.0 / 100.0, 2000.0);
    rngSeed(n ^ 0xE3B7A1u);
    uint t0 = t;
    float temp0 = q.temp;
    for (uint k = 0; k < m; k++)
    {
        if (ids[k] == n)
        {
            // Its own turn as a centre, if it still is one
            uint kind = centreKind(t, q);
            if (kind == 0)
                continue;
            if (kind == 2)
            {
                g_tempCenter.apply(q);
                if (rngUniform01() < binomialGte1(g_count, 1.0 / 48.0))
                    t = rngChance(2, 5) ? PT_BREC : PT_NTCT;
            }
            if (onTop)
                neighbourEffect(kind == 2, t, q, x, y); // its own cell is in its 5x5
        }
        else
            neighbourEffect(kinds[k] == 2, t, q, x, y);
    }
    if (t != t0 || q.temp != temp0)
    {
        q.pad1 = selfKind; // the others still read it this phase
        parts[n] = q;
        if (t != t0)
            partChangeType(n, t);
    }
}
