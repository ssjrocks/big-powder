// Classic mode: The Powder Toy's own simulation (third_party/tpt, compiled with TPT's flags), driven
// headlessly. It behaves exactly like TPT and is also the reference the GPU engine is tested against.
//
// This header deliberately includes nothing from TPT, so the rest of the app is built independently of
// TPT's compile settings (TPT builds with -fno-rtti and -ffast-math).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bp {

struct ClassicParticle {
    int type, life, ctype;
    float x, y, vx, vy, temp;
    int tmp, tmp2, tmp3, tmp4;
    uint32_t flags, dcolour;
};

struct ClassicOptions {
    bool heat = true;              // false = TPT's "legacy" heat mode
    bool ambientHeat = false;
    bool newtonianGravity = false;
    bool waterEqualisation = false;
    int edgeMode = 0;              // 0 void, 1 solid, 2 loop
    int gravityMode = 0;           // 0 vertical, 1 off, 2 radial, 3 custom
    int airMode = 0;               // 0 on, 1 pressure off, 2 velocity off, 3 off, 4 no update
};

// TPT keeps its element tables in a process-wide singleton, so only one ClassicSim may exist at a time.
class ClassicSim {
public:
    static constexpr int W = 612; // TPT's XRES, YRES (checked against TPT in the .cpp)
    static constexpr int H = 384;
    static constexpr int CELL = 4;

    ClassicSim();
    ~ClassicSim();
    ClassicSim(const ClassicSim &) = delete;
    ClassicSim &operator=(const ClassicSim &) = delete;

    void setOptions(const ClassicOptions &o);
    const ClassicOptions &options() const { return opts_; }

    // One TPT frame: BeforeSim, UpdateParticles over all particles, AfterSim.
    void step();
    void clear();
    void seed(uint32_t s); // TPT's simulation RNG (also used when elements are created)

    // Loads a .cps/.stm save (OPS or the old PSv format). The save is placed at block (bx, by), in units
    // of CELL pixels, like TPT's stamps.
    bool load(const std::vector<char> &data, std::string &error, int bx = 0, int by = 0);
    bool loadFile(const std::string &path, std::string &error);
    bool save(std::vector<char> &out, std::string &error) const;
    bool saveFile(const std::string &path, std::string &error) const;

    // Circle brush through TPT's CreatePartFlags; type 0 deletes.
    void brush(int x, int y, int r, int type);
    // Creates one particle with TPT's create_part (defaults set by the element); returns its id or -1.
    int createPart(int x, int y, int type);

    // Renders with one of TPT's display presets (0..10, see presetName) into W*H pixels, 0xRRGGBB.
    void render(int preset, std::vector<uint32_t> &out);
    static int presetCount();
    static std::string presetName(int preset);

    int particleCount() const;
    int elementCount(int type) const;
    std::vector<ClassicParticle> particles() const;
    float pressure(int cx, int cy) const;     // air map cell
    // Air maps, (W/CELL) x (H/CELL) each: pressure, velocity x/y, ambient heat. Testing.
    void airMaps(std::vector<float> &pv, std::vector<float> &vx, std::vector<float> &vy, std::vector<float> &hv) const;
    void setAirMaps(const std::vector<float> &pv, const std::vector<float> &vx, const std::vector<float> &vy,
                    const std::vector<float> &hv);
    float ambientHeat(int cx, int cy) const;
    std::string elementName(int type) const;
    int elementByName(const std::string &name) const; // -1 if unknown
    // Walls: one TPT wall id per CELL block, (W/CELL) x (H/CELL). setWalls also prepares TPT's air blocking maps
    // as TPT does when it loads a save.
    std::vector<uint8_t> walls() const;
    void setWalls(const std::vector<uint8_t> &bmap);
    void paintWalls(int cx, int cy, int r, uint8_t wall); // circle of radius r cells, wall 0 erases
    std::vector<float> fanVX() const;
    std::vector<float> fanVY() const;
    // Testing: change how an element couples to air (TPT's Advection and AirDrag properties).
    void setAirCoupling(int type, float advection, float airDrag);
    uint64_t tick() const { return tick_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ClassicOptions opts_;
    uint64_t tick_ = 0;
};

} // namespace bp
