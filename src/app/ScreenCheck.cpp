// --check-screen: the game screen (app/GameScreen) driven by made-up SDL events and no window, in Classic mode and
// on the GPU: TPT's keys and mouse through to its windows and tools, checked by what they do to the world and the
// interface, with pictures of some of the steps.
#include "app/App.h"
#include "app/GameScreen.h"
#include "app/Prefs.h"
#include "ui/Dialogs.h"
#include "sim/ElementIds.h"
#include "sim/Signs.h"

#include <SDL3/SDL_events.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <unistd.h>

namespace bp {

namespace {

void writePicture(const std::string &path, const std::vector<uint32_t> &px, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (uint32_t p : px) {
        char rgb[3] = {char((p >> 16) & 0xFF), char((p >> 8) & 0xFF), char(p & 0xFF)};
        f.write(rgb, 3);
    }
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

class Driver {
public:
    Driver(VkContext &ctx, GameScreen &s, uint32_t w, uint32_t h) : ctx_(ctx), s_(s), w_(w), h_(h) { s_.resize(w, h); }

    void frames(int n = 1) {
        for (int k = 0; k < n; k++)
            ctx_.immediate([&](VkCommandBuffer cmd) { s_.record(cmd, 0, 1.0 / 60.0, VK_NULL_HANDLE); });
    }
    void key(SDL_Keycode k, SDL_Scancode sc, SDL_Keymod mod = SDL_KMOD_NONE) {
        SDL_Event e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = k;
        e.key.scancode = sc;
        e.key.mod = mod;
        s_.event(e);
        e.type = SDL_EVENT_KEY_UP;
        s_.event(e);
        frames();
    }
    void text(const char *utf8) {
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.text = utf8;
        s_.event(e);
        frames();
    }
    // The mouse to a world pixel, or a screen one
    void moveToWorld(int wx, int wy) {
        const Camera &c = s_.camera();
        move((float(wx) + 0.5f - c.x) * c.zoom, (float(wy) + 0.5f - c.y) * c.zoom);
    }
    void move(float x, float y) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = x, e.motion.y = y;
        e.motion.xrel = x - x_, e.motion.yrel = y - y_;
        x_ = x, y_ = y;
        s_.event(e);
    }
    void button(bool down, uint8_t b = SDL_BUTTON_LEFT) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.button = b;
        e.button.x = x_, e.button.y = y_;
        s_.event(e);
    }
    void click(uint8_t b = SDL_BUTTON_LEFT) {
        button(true, b);
        frames();
        button(false, b);
        frames();
    }
    // In interface pixels
    void moveUi(int x, int y) {
        float us = float(s_.uiScale());
        move((float(x) + 0.5f) * us, (float(y) + 0.5f) * us);
    }
    // Modifier keys held down (or let go: SDL_KMOD_NONE) for the clicks that follow
    void hold(SDL_Keymod mod) {
        SDL_Event e{};
        e.type = mod == SDL_KMOD_NONE ? SDL_EVENT_KEY_UP : SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_LCTRL;
        e.key.scancode = SDL_SCANCODE_LCTRL;
        e.key.mod = mod;
        s_.event(e);
    }
    std::vector<uint32_t> picture() {
        std::vector<uint32_t> px(size_t(w_) * h_);
        ctx_.download(s_.frameBuffer(), px.data(), px.size() * 4);
        for (auto &p : px)
            p &= 0xFFFFFF;
        return px;
    }
    void shot(const std::string &path) {
        if (!path.empty())
            writePicture(path, picture(), int(w_), int(h_));
    }
    std::string tool(int slot) {
        const UiTool *t = s_.ui().activeTool(slot);
        return t ? utf8(t->name) : "-";
    }
    bool particle(int x, int y, ClassicParticle &p) { return s_.world().particleAt(x, y, false, p); }
    // The particles of a type in a box, and where they are
    struct Box {
        int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        int w() const { return x1 - x0; }
        int h() const { return y1 - y0; }
    };
    Box find(int x0, int y0, int x1, int y1, int type) {
        Box b;
        ClassicParticle p;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                if (particle(x, y, p) && p.type == type) {
                    b.n++;
                    b.x0 = std::min(b.x0, x), b.y0 = std::min(b.y0, y), b.x1 = std::max(b.x1, x), b.y1 = std::max(b.y1, y);
                }
        return b;
    }
    // The screen pixel at a world pixel's middle
    uint32_t screenPixel(const std::vector<uint32_t> &pic, int wx, int wy) {
        const Camera &c = s_.camera();
        int sx = int((float(wx) + 0.5f - c.x) * c.zoom), sy = int((float(wy) + 0.5f - c.y) * c.zoom);
        if (sx < 0 || sy < 0 || sx >= int(w_) || sy >= int(h_))
            return 0;
        return pic[size_t(sy) * w_ + size_t(sx)];
    }

private:
    VkContext &ctx_;
    GameScreen &s_;
    uint32_t w_, h_;
    float x_ = 0, y_ = 0;
};

} // namespace

int runScreenCheck(const AppOptions &base) {
    VkContext ctx({}, base.validate);
    ctx.createDevice();
    std::printf("GPU: %s\n", ctx.properties().deviceName);
    int failures = 0, checks = 0;
    const ElementTable table = ElementTable::build(); // (for the signs' boxes)
    // Stamps and saves go to a folder of the check's own, removed at the end
    const std::filesystem::path dataDir =
        std::filesystem::temp_directory_path() / ("big-powder-check-" + std::to_string(::getpid()));
    for (const char *mode : {"classic", "screen"}) {
        AppOptions o = base;
        o.mode = mode;
        o.dataDir = (dataDir / mode).string();
        if (!o.sizeSet)
            o.W = 1280, o.H = 720;
        o.scenes = {"empty"};
        GameScreen screen(ctx, o);
        Driver d(ctx, screen, 1280, 720);
        std::printf("== %s (%ux%u world)\n", mode, screen.world().width(), screen.world().height());
        auto check = [&](bool ok, const std::string &what) {
            checks++;
            failures += !ok;
            std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        };
        const std::string shots = base.uiSnapshot.empty() ? "" : base.uiSnapshot + "-" + mode;
        d.frames(2);
        d.key(SDLK_SPACE, SDL_SCANCODE_SPACE); // paused, so what is drawn stays put
        const int cx = int(screen.world().width()) / 2, cy = int(screen.world().height()) / 2;

        // E: element search; typing; Enter chooses the first result for the left button
        d.key(SDLK_E, SDL_SCANCODE_E);
        bool open = screen.windows().depth() == 1;
        d.text("wat");
        d.shot(shots.empty() ? "" : shots + "-search.ppm");
        // while a window is up the game doesn't run and its picture stays as it was (once TPT's fade is done)
        d.frames(25);
        std::vector<uint32_t> a = d.picture();
        d.frames(3);
        bool still = a == d.picture();
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        check(open && still && screen.windows().empty() && d.tool(0) == "WATR",
              "E, \"wat\", Enter: element search chose " + d.tool(0) + (still ? "" : " (the picture changed under it)"));

        // a press over the world draws with it
        d.moveToWorld(cx, cy);
        d.click();
        ClassicParticle p{};
        check(d.particle(cx, cy, p) && p.type == PT_WATR, "a click draws WATR");

        // ctrl+P: the property tool and its window; down to temp, "1000", Enter; then drawing sets it
        d.key(SDLK_P, SDL_SCANCODE_P, SDL_KMOD_LCTRL);
        open = screen.windows().depth() == 1 && d.tool(0) == "PROP";
        for (int k = 0; k < ParticleProperty::TEMP; k++)
            d.key(SDLK_DOWN, SDL_SCANCODE_DOWN);
        d.text("1000");
        d.shot(shots.empty() ? "" : shots + "-property.ppm");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        d.moveToWorld(cx, cy);
        d.click();
        bool set = d.particle(cx, cy, p) && p.temp == 1273.15f;
        check(open && screen.windows().empty() && set, "ctrl+P, temp 1000, Enter, a click: temp " + std::to_string(p.temp));

        // the sample tool from element search, then a click on the water picks WATR; with shift it opens the property
        // window with the water's temperature
        d.key(SDLK_E, SDL_SCANCODE_E);
        d.text("smpl");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        std::string sampler = d.tool(0);
        d.moveToWorld(cx, cy);
        d.click();
        check(sampler == "SMPL" && d.tool(0) == "WATR", "SMPL from element search, a click: " + d.tool(0));
        d.key(SDLK_E, SDL_SCANCODE_E);
        d.text("smpl");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        {
            SDL_Event e{};
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = SDLK_LSHIFT;
            e.key.scancode = SDL_SCANCODE_LSHIFT;
            e.key.mod = SDL_KMOD_LSHIFT;
            screen.event(e);
        }
        d.moveToWorld(cx, cy);
        d.click();
        {
            SDL_Event e{};
            e.type = SDL_EVENT_KEY_UP;
            e.key.key = SDLK_LSHIFT;
            e.key.scancode = SDL_SCANCODE_LSHIFT;
            screen.event(e);
        }
        d.shot(shots.empty() ? "" : shots + "-sampled.ppm");
        bool sampled = screen.windows().depth() == 1 && d.tool(0) == "PROP";
        d.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        check(sampled && screen.windows().empty() && screen.running(),
              "shift+click with SMPL: the property window (Esc closes it, not the game)");

        // HEAT, held down over the water for a few frames
        d.key(SDLK_E, SDL_SCANCODE_E);
        d.text("heat");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        float before = d.particle(cx, cy, p) ? p.temp : 0.0f;
        d.moveToWorld(cx, cy);
        d.button(true);
        d.frames(5);
        d.button(false);
        d.frames();
        float after = d.particle(cx, cy, p) ? p.temp : 0.0f;
        check(d.tool(0) == "HEAT" && after > before,
              "HEAT held for 5 frames: " + std::to_string(before) + " K -> " + std::to_string(after) + " K");

        // B: the deco menu, with its own tools (opening it with no colour preset chosen puts SET on the left button)
        d.key(SDLK_B, SDL_SCANCODE_B);
        std::string decoTool = d.tool(0);
        d.moveToWorld(cx, cy);
        d.click();
        d.particle(cx, cy, p);
        check(screen.ui().colourSelectorVisible() && decoTool == "SET" && p.dcolour == 0xFFC86432u,
              "B: the deco menu, " + decoTool + " draws TPT's first colour: " + hex(p.dcolour));
        const int uw = int(1280 / screen.uiScale()), uh = int(720 / screen.uiScale());
        const int vw = uw - GameUi::BARSIZE, vh = uh - GameUi::MENUSIZE;
        // the second colour preset (cyan), then drawing with it
        d.moveUi(5 + 31 + 10, vh + 8);
        d.click();
        d.moveToWorld(cx + 3, cy);
        d.click();
        d.particle(cx + 3, cy, p);
        check(screen.ui().decoColour() == 0xFF00FFFFu && p.dcolour == 0xFF00FFFFu, "a colour preset: " + hex(p.dcolour));
        // the colour picker: its button, then the four channels typed in (Tab goes from one to the next), Enter
        d.moveUi(vw / 2, vh + 8);
        d.click();
        bool picker = dynamic_cast<ColourPickerWindow *>(screen.windows().top()) != nullptr;
        d.shot(shots.empty() ? "" : shots + "-picker.ppm");
        {
            // focus the red box, then replace each with its value
            ui::Window *w = screen.windows().top();
            const int bx = w ? w->position.x + 5 + 15 : 0, by = w ? w->position.y + w->size.y - 23 + 8 : 0;
            d.moveUi(bx, by);
            d.click();
        }
        for (const char *v : {"10", "20", "30", "200"}) {
            d.key(SDLK_A, SDL_SCANCODE_A, SDL_KMOD_LCTRL); // (select what is there)
            d.text(v);
            d.key(SDLK_TAB, SDL_SCANCODE_TAB);
        }
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        check(picker && screen.windows().empty() && screen.ui().decoColour() == 0xC80A141Eu,
              "the colour picker, 10 20 30 200 typed in: " + hex(screen.ui().decoColour()));
        // SMPL in the deco menu takes the picture's colour
        d.key(SDLK_E, SDL_SCANCODE_E);
        d.text("smpl");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        d.moveToWorld(cx, cy);
        d.click();
        uint32_t picture = screen.world().picturePixel(cx, cy);
        check(screen.ui().decoColour() == (0xFF000000u | picture) && picture != 0,
              "SMPL in the deco menu takes the picture's colour: " + hex(screen.ui().decoColour()));
        d.shot(shots.empty() ? "" : shots + "-deco.ppm");
        // B again: the last menu, with the regular tools as they were
        d.key(SDLK_B, SDL_SCANCODE_B);
        check(!screen.ui().colourSelectorVisible() && d.tool(0) == "HEAT", "B again: the tools menu, " + d.tool(0));

        // CUST: its window (the rule box has the focus); a rule and a name typed in, Enter; then drawing with it makes
        // LIFE of that rule in its colour, and ctrl+shift+right click on it in the Life menu (and Confirm) removes it
        d.key(SDLK_E, SDL_SCANCODE_E);
        d.text("cust");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        bool golWindow = dynamic_cast<GolWindow *>(screen.windows().top()) != nullptr;
        d.key(SDLK_A, SDL_SCANCODE_A, SDL_KMOD_LCTRL);
        d.text("B36/S23");
        if (ui::Window *w = screen.windows().top()) { // the name box
            d.moveUi(w->position.x + 20, w->position.y + 25 + 8);
            d.click();
        }
        d.key(SDLK_A, SDL_SCANCODE_A, SDL_KMOD_LCTRL);
        d.text("highl");
        d.shot(shots.empty() ? "" : shots + "-gol.ppm");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        const UiTool *custom = screen.ui().activeTool(0);
        bool added = custom && custom->identifier == "DEFAULT_PT_LIFECUST_HIGHL";
        uint32_t customColour = custom ? custom->colour : 0;
        d.moveToWorld(cx - 40, cy);
        d.click();
        bool drawn = d.particle(cx - 40, cy, p) && p.type == PT_LIFE && p.ctype == tptParseGolRule(U"B36/S23") &&
                     (p.dcolour & 0xFFFFFF) == customColour;
        check(golWindow && added && drawn && screen.windows().empty(),
              "CUST, B36/S23 and \"highl\": " + d.tool(0) + " draws LIFE " + std::to_string(p.ctype) + " in " +
                  hex(p.dcolour));
        {
            int sx, sy, bx, by;
            screen.ui().sectionButton(12 /* SC_LIFE */, sx, sy);
            d.moveUi(sx, sy);
            d.frames();
            int tool = screen.ui().toolIndex("DEFAULT_PT_LIFECUST_HIGHL");
            bool there = screen.ui().paletteButton(tool, bx, by);
            SDL_Event e{};
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = SDLK_LCTRL;
            e.key.scancode = SDL_SCANCODE_LCTRL;
            e.key.mod = SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
            screen.event(e);
            d.moveUi(bx, by);
            d.click(SDL_BUTTON_RIGHT);
            e.type = SDL_EVENT_KEY_UP;
            e.key.mod = SDL_KMOD_NONE;
            screen.event(e);
            bool prompt = dynamic_cast<ConfirmPrompt *>(screen.windows().top()) != nullptr;
            d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
            check(there && prompt && screen.ui().toolIndex("DEFAULT_PT_LIFECUST_HIGHL") < 0 && d.tool(0) == "DUST",
                  "ctrl+shift+right click on it, Confirm: removed, the left button back to " + d.tool(0));
        }

        // SIGN: a click on nothing makes a new sign (its window); typed text, Enter
        auto choose = [&](const char *name) {
            d.key(SDLK_E, SDL_SCANCODE_E);
            d.text(name);
            d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        };
        choose("sign");
        const int sx = cx + 60, sy = cy - 60;
        d.moveToWorld(sx, sy);
        d.click();
        bool signWindow = dynamic_cast<SignWindow *>(screen.windows().top()) != nullptr;
        d.text("hello");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        std::vector<Sign> signs = screen.world().signs();
        check(d.tool(0) == "SIGN" && signWindow && signs.size() == 1 && signs[0].text == U"hello" && signs[0].x == sx &&
                  signs[0].y == sy,
              "SIGN, a click, \"hello\", Enter: " + std::to_string(signs.size()) + " sign" +
                  (signs.empty() ? "" : " \"" + utf8(signs[0].text) + "\""));
        // a click on it edits it: the text changes as it is typed, Move takes it to where the mouse clicks next
        int bx, by, bw, bh;
        signDisplayText(table, signs[0], SignSample{}, bx, by, bw, bh);
        d.moveToWorld(bx + 3, by + 3);
        d.click();
        bool edit = dynamic_cast<SignWindow *>(screen.windows().top()) != nullptr;
        d.text(" there");
        bool live = !screen.world().signs().empty() && screen.world().signs()[0].text == U"hello there";
        d.shot(shots.empty() ? "" : shots + "-sign.ppm");
        if (ui::Window *w = screen.windows().top()) { // Move (by Delete, on the right of the pointer drop down)
            d.moveUi(w->position.x + 52 + 55 + 3 + 20, w->position.y + 48 + 8);
            d.click();
        }
        d.moveToWorld(sx - 30, sy + 20);
        d.frames();
        d.moveToWorld(sx - 31, sy + 21);
        d.click();
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        signs = screen.world().signs();
        check(edit && live && screen.windows().empty() && signs.size() == 1 && signs[0].text == U"hello there" &&
                  signs[0].x == sx - 31 && signs[0].y == sy + 21,
              "a click on it: edited as it is typed, then moved to (" +
                  (signs.empty() ? std::string("-") : std::to_string(signs[0].x) + "," + std::to_string(signs[0].y)) +
                  ")");
        // Delete
        signDisplayText(table, signs[0], SignSample{}, bx, by, bw, bh);
        d.moveToWorld(bx + 3, by + 3);
        d.click();
        if (ui::Window *w = screen.windows().top()) {
            d.moveUi(w->position.x + w->size.x - 30, w->position.y + 48 + 8);
            d.click();
        }
        check(screen.windows().empty() && screen.world().signs().empty(), "Delete removes it");
        // a button sign {b|...} on METL: a click on it with any other tool sparks its spot, and draws nothing
        choose("metl");
        d.moveToWorld(sx, sy);
        d.click();
        choose("sign");
        d.moveToWorld(sx, sy);
        d.click();
        d.text("{b|go}");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        choose("dust");
        signs = screen.world().signs();
        if (!signs.empty())
            signDisplayText(table, signs[0], SignSample{}, bx, by, bw, bh);
        d.moveToWorld(bx + 3, by + 3);
        d.click();
        ClassicParticle q{};
        bool sparked = d.particle(sx, sy, p) && p.type == PT_SPRK && p.ctype == PT_METL;
        bool noDust = !d.particle(bx + 3, by + 3, q) || q.type != PT_DUST;
        check(signs.size() == 1 && sparked && noDust,
              "a button sign on METL, clicked with DUST: its spot is " + screen.world().elementName(p.type) +
                  (noDust ? ", no DUST drawn" : ", DUST drawn"));

        // choosing the property tool in element search opens its window over the search, which closes once that
        // one has (as in TPT: the search only finds out it is done when it is on top again)
        d.key(SDLK_E, SDL_SCANCODE_E);
        d.text("prop");
        d.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        size_t depth = screen.windows().depth();
        bool property = dynamic_cast<PropertyWindow *>(screen.windows().top()) != nullptr;
        d.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        d.frames();
        check(depth == 2 && property && screen.windows().empty() && d.tool(0) == "PROP",
              "PROP from element search: its window over the search (" + std::to_string(depth) +
                  " up), Esc closes both");

        // undo: ctrl+Z takes the world back to before the last press, ctrl+Y and ctrl+shift+Z forward again
        choose("dust");
        const int ux = cx - 80, uy = cy + 40;
        auto dustAt = [&](int x, int y) { return d.particle(x, y, p) && p.type == PT_DUST; };
        d.moveToWorld(ux, uy);
        d.click();
        bool drew = dustAt(ux, uy);
        d.key(SDLK_Z, SDL_SCANCODE_Z, SDL_KMOD_LCTRL);
        bool undone = !dustAt(ux, uy);
        d.key(SDLK_Y, SDL_SCANCODE_Y, SDL_KMOD_LCTRL);
        bool redone = dustAt(ux, uy);
        d.key(SDLK_Z, SDL_SCANCODE_Z, SDL_KMOD_LCTRL);
        d.key(SDLK_Z, SDL_SCANCODE_Z, SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT));
        bool shiftRedone = dustAt(ux, uy);
        check(drew && undone && redone && shiftRedone,
              std::string("DUST, ctrl+Z, ctrl+Y, ctrl+Z, ctrl+shift+Z: ") + (drew ? "drawn" : "not drawn") +
                  (undone ? ", gone" : ", still there") + (redone ? ", back" : ", not back") +
                  (shiftRedone ? ", gone and back" : ", not back the second time"));
        // a press after ctrl+Z forgets what was undone
        d.key(SDLK_Z, SDL_SCANCODE_Z, SDL_KMOD_LCTRL);
        d.moveToWorld(ux + 15, uy);
        d.click();
        d.key(SDLK_Y, SDL_SCANCODE_Y, SDL_KMOD_LCTRL);
        check(!dustAt(ux, uy) && dustAt(ux + 15, uy), "ctrl+Z, a press elsewhere, ctrl+Y: nothing to go forward to");
        // five snapshots at most: of seven presses, seven ctrl+Z take back the last five
        for (int k = 0; k < 7; k++) {
            d.moveToWorld(ux + 30 + 15 * k, uy);
            d.click();
        }
        for (int k = 0; k < 7; k++)
            d.key(SDLK_Z, SDL_SCANCODE_Z, SDL_KMOD_LCTRL);
        int left = 0;
        for (int k = 0; k < 7; k++)
            left += dustAt(ux + 30 + 15 * k, uy);
        check(screen.history().size() == 5 && left == 2 && dustAt(ux + 15, uy),
              "7 presses, 7 ctrl+Z: " + std::to_string(left) + " of them left (" +
                  std::to_string(screen.history().size()) + " snapshots kept)");

        // ctrl+C: the world darkens; an area dragged out and let go is copied
        using Select = GameScreen::Select;
        const int ax = cx - 120, ay = cy - 60;
        d.moveToWorld(ax - 5, ay);
        d.click();
        d.moveToWorld(ax + 5, ay);
        d.click(); // (two blobs side by side: wider than high)
        const Driver::Box src = d.find(ax - 20, ay - 20, ax + 20, ay + 20, PT_DUST);
        d.frames();
        std::vector<uint32_t> lit = d.picture();
        d.key(SDLK_C, SDL_SCANCODE_C, SDL_KMOD_LCTRL);
        bool copying = screen.selectMode() == Select::Copy;
        d.frames();
        std::vector<uint32_t> dim = d.picture();
        auto darker = [](uint32_t was, uint32_t is) { // (BlendFilledRect of black at 100)
            for (int sh : {0, 8, 16}) {
                int a = int((was >> sh) & 0xFF), b = int((is >> sh) & 0xFF);
                if (std::abs(b - a * 155 / 255) > 2)
                    return false;
            }
            return was != 0;
        };
        bool dark = darker(d.screenPixel(lit, cx, cy), d.screenPixel(dim, cx, cy));
        d.moveToWorld(ax - 15, ay - 12);
        d.button(true);
        d.frames();
        d.moveToWorld(ax + 15, ay + 12);
        d.frames();
        std::vector<uint32_t> chosen = d.picture();
        d.shot(shots.empty() ? "" : shots + "-copy.ppm");
        bool inside = d.screenPixel(chosen, ax, ay) == d.screenPixel(lit, ax, ay); // (the area isn't darkened)
        d.button(false);
        d.frames();
        const int cw = (ax + 15) / CELL - (ax - 15) / CELL + 1, ch = (ay + 12) / CELL - (ay - 12) / CELL + 1;
        bool copied = screen.clipboard() && screen.clipboard()->cw == cw && screen.clipboard()->ch == ch;
        check(copying && dark && inside && copied && screen.selectMode() == Select::None,
              std::string("ctrl+C, dragged and let go: ") + (dark ? "darkened" : "not darkened") +
                  (inside ? "" : " (the area too)") + ", " +
                  (screen.clipboard() ? std::to_string(screen.clipboard()->cw) + "x" +
                                            std::to_string(screen.clipboard()->ch) + " cells copied"
                                      : std::string("nothing copied")));
        // ctrl+V: it follows the mouse until a click places it
        const int px = cx + 120, py = cy - 60;
        d.moveToWorld(px, py);
        d.key(SDLK_V, SDL_SCANCODE_V, SDL_KMOD_LCTRL);
        bool placing = screen.selectMode() == Select::Place;
        d.frames();
        d.shot(shots.empty() ? "" : shots + "-paste.ppm");
        d.click();
        const Driver::Box dst = d.find(px - 25, py - 25, px + 25, py + 25, PT_DUST);
        check(placing && dst.n == src.n && dst.w() == src.w() && dst.h() == src.h() &&
                  screen.selectMode() == Select::None,
              "ctrl+V, a click: " + std::to_string(dst.n) + " of " + std::to_string(src.n) + " DUST pasted, " +
                  std::to_string(dst.w() + 1) + "x" + std::to_string(dst.h() + 1));
        // R turns it; the arrows move it a pixel at a time
        d.moveToWorld(px, py + 70);
        d.key(SDLK_V, SDL_SCANCODE_V, SDL_KMOD_LCTRL);
        d.key(SDLK_R, SDL_SCANCODE_R);
        d.click();
        const Driver::Box turned = d.find(px - 25, py + 45, px + 25, py + 95, PT_DUST);
        d.moveToWorld(px, py + 140);
        d.key(SDLK_V, SDL_SCANCODE_V, SDL_KMOD_LCTRL);
        for (int k = 0; k < 3; k++)
            d.key(SDLK_RIGHT, SDL_SCANCODE_RIGHT);
        d.click();
        const Driver::Box moved = d.find(px - 25, py + 115, px + 25, py + 165, PT_DUST);
        check(turned.n == src.n && turned.w() == src.h() && turned.h() == src.w() && moved.n == src.n &&
                  moved.x0 == dst.x0 + 3 && moved.y0 == dst.y0 + 140,
              "R: turned to " + std::to_string(turned.w() + 1) + "x" + std::to_string(turned.h() + 1) +
                  "; three rights: " + std::to_string(moved.x0 - dst.x0) + " pixels right");
        // ctrl+X: copied, then cleared; ctrl+Z brings it back
        d.key(SDLK_X, SDL_SCANCODE_X, SDL_KMOD_LCTRL);
        d.moveToWorld(ax - 15, ay - 12);
        d.button(true);
        d.frames();
        d.moveToWorld(ax + 15, ay + 12);
        d.frames();
        d.button(false);
        d.frames();
        const int cutLeft = d.find(ax - 20, ay - 20, ax + 20, ay + 20, PT_DUST).n;
        d.key(SDLK_Z, SDL_SCANCODE_Z, SDL_KMOD_LCTRL);
        const int back = d.find(ax - 20, ay - 20, ax + 20, ay + 20, PT_DUST).n;
        check(cutLeft == 0 && back == src.n && screen.clipboard() && screen.clipboard()->cw == cw,
              "ctrl+X: " + std::to_string(cutLeft) + " left, ctrl+Z: " + std::to_string(back) + " back");
        // a right click lets go of a selection
        d.key(SDLK_C, SDL_SCANCODE_C, SDL_KMOD_LCTRL);
        d.click(SDL_BUTTON_RIGHT);
        bool cancelled = screen.selectMode() == Select::None;
        d.key(SDLK_V, SDL_SCANCODE_V, SDL_KMOD_LCTRL);
        d.click(SDL_BUTTON_RIGHT);
        check(cancelled && screen.selectMode() == Select::None, "a right click lets go of copying and of pasting");
        // S: a stamp of an area; L places the last one; K shows the stamps, and a click on one places it
        d.key(SDLK_S, SDL_SCANCODE_S);
        bool stamping = screen.selectMode() == Select::Stamp;
        d.moveToWorld(ax - 15, ay - 12);
        d.button(true);
        d.frames();
        d.moveToWorld(ax + 15, ay + 12);
        d.frames();
        d.button(false);
        d.frames();
        const size_t stamps = screen.stamps().names().size();
        d.moveToWorld(px, py + 210);
        d.key(SDLK_L, SDL_SCANCODE_L);
        bool lastStamp = screen.selectMode() == Select::Place;
        d.click();
        const int fromL = d.find(px - 25, py + 185, px + 25, py + 235, PT_DUST).n;
        check(stamping && stamps == 1 && lastStamp && fromL == src.n,
              "S, dragged: " + std::to_string(stamps) + " stamp; L, a click: " + std::to_string(fromL) + " DUST");
        d.key(SDLK_K, SDL_SCANCODE_K);
        bool browser = dynamic_cast<StampBrowser *>(screen.windows().top()) != nullptr;
        d.frames(25); // (its pictures are made a tick at a time; the fade)
        d.shot(shots.empty() ? "" : shots + "-stamps.ppm");
        {
            const int bw = uw / 5 - 4, bh = (uh - 50 - 18) / 4 - 4;
            d.moveUi(2 + bw / 2, 52 + bh / 2);
            d.frames();
            d.click();
        }
        bool fromBrowser = screen.windows().empty() && screen.selectMode() == Select::Place;
        d.moveToWorld(px - 60, py + 210);
        d.click();
        const int fromK = d.find(px - 85, py + 185, px - 35, py + 235, PT_DUST).n;
        check(browser && fromBrowser && fromK == src.n,
              std::string("K: the stamp browser") + (browser ? "" : " didn't open") +
                  (fromBrowser ? ", its stamp clicked: " : ", clicking its stamp placed nothing: ") +
                  std::to_string(fromK) + " DUST");

        // Step 7: TPT's other keys. G and shift+G change the grid, H hides the HUD, D the debug HUD, F1 the intro
        d.key(SDLK_G, SDL_SCANCODE_G);
        const int grid1 = screen.view().gridSize;
        d.key(SDLK_G, SDL_SCANCODE_G, SDL_KMOD_LSHIFT);
        const int grid0 = screen.view().gridSize;
        d.key(SDLK_H, SDL_SCANCODE_H);
        const bool hudOff = !screen.hudShown();
        d.key(SDLK_H, SDL_SCANCODE_H);
        const bool debugBefore = screen.debugShown();
        d.key(SDLK_D, SDL_SCANCODE_D);
        const bool debugToggled = screen.debugShown() != debugBefore;
        d.shot(shots.empty() ? "" : shots + "-debug.ppm");
        d.key(SDLK_F1, SDL_SCANCODE_F1);
        if (screen.introText() == 0) // (it was still up from the start: the first F1 hid it)
            d.key(SDLK_F1, SDL_SCANCODE_F1);
        const bool intro = screen.introText() > 50;
        d.shot(shots.empty() ? "" : shots + "-intro.ppm");
        d.key(SDLK_F1, SDL_SCANCODE_F1);
        check(grid1 == 1 && grid0 == 0 && hudOff && screen.hudShown() && debugToggled && intro &&
                  screen.introText() == 0,
              "G: grid " + std::to_string(grid1) + ", shift+G: " + std::to_string(grid0) +
                  "; H hides the HUD; D the debug HUD; F1 the introduction");
        // ctrl+F: what the left button's element is, found (with DUST in the world); again: not
        screen.ui().chooseTool(0, screen.ui().toolIndex("DEFAULT_PT_DUST"));
        d.key(SDLK_F, SDL_SCANCODE_F, SDL_KMOD_LCTRL);
        d.frames(3);
        const bool finding = screen.view().findKind == 1 && screen.view().findValue == PT_DUST;
        const uint32_t found = screen.world().foundParticles();
        d.shot(shots.empty() ? "" : shots + "-find.ppm");
        d.key(SDLK_F, SDL_SCANCODE_F, SDL_KMOD_LCTRL);
        check(finding && found > 0 && screen.view().findKind == 0,
              "ctrl+F: finding DUST, " + std::to_string(found) + " found; ctrl+F again: not");
        // The renderer options (its button): the world drawn live under it, so a preset key changes the picture
        d.moveUi(uw - 32 + 7, uh - 16 + 7);
        d.click();
        const bool renderWindow = dynamic_cast<RenderOptionsWindow *>(screen.windows().top()) != nullptr;
        d.frames(3);
        const std::vector<uint32_t> plainPicture = d.picture();
        d.key(SDLK_6, SDL_SCANCODE_6);
        d.frames(2);
        const std::vector<uint32_t> heatPicture = d.picture();
        d.shot(shots.empty() ? "" : shots + "-render.ppm");
        const bool heatView = screen.view().colorMode == tptPresetView(6).colorMode && screen.view().colorMode != 0;
        d.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        check(renderWindow && heatView && plainPicture != heatPicture && screen.windows().empty(),
              std::string("the renderer options: ") + (renderWindow ? "" : "didn't open, ") +
                  (heatView ? "6 loads the heat display" : "6 didn't load the heat display") +
                  (plainPicture != heatPicture ? ", drawn live" : ", the picture didn't change") + ", Escape closes it");
        // The settings (its button): a checkbox and a text box in its list, each taking effect as it changes
        d.moveUi(uw - 48 + 7, uh - 16 + 7);
        d.click();
        {
            auto *w = dynamic_cast<OptionsWindow *>(screen.windows().top());
            ui::Panel *panel = nullptr;
            ui::Checkbox *water = nullptr;
            ui::Textbox *airTemp = nullptr;
            int airTempY = -1;
            if (w)
                for (const auto &c : w->components())
                    if (auto *sp = dynamic_cast<ui::ScrollPanel *>(c.get()))
                        panel = sp;
            if (panel) {
                for (const auto &c : panel->children()) {
                    if (auto *cb = dynamic_cast<ui::Checkbox *>(c.get()); cb && cb->text().rfind(U"Water", 0) == 0)
                        water = cb;
                    if (auto *l = dynamic_cast<ui::Label *>(c.get());
                        l && !dynamic_cast<ui::Textbox *>(c.get()) && l->text() == U"Ambient air temperature")
                        airTempY = l->position.y;
                }
                for (const auto &c : panel->children())
                    if (auto *t = dynamic_cast<ui::Textbox *>(c.get()); t && t->position.y == airTempY)
                        airTemp = t;
            }
            auto at = [&](const ui::Component *c, int dx, int dy) {
                d.moveUi(w->position.x + panel->position.x + panel->viewport.x + c->position.x + dx,
                         w->position.y + panel->position.y + panel->viewport.y + c->position.y + dy);
            };
            const bool waterBefore = screen.settings().waterEqualisation;
            if (water) {
                at(water, 6, 7);
                d.click();
            }
            const bool waterOn = screen.settings().waterEqualisation && !waterBefore;
            if (airTemp) {
                at(airTemp, 20, 8);
                d.click();
                d.key(SDLK_A, SDL_SCANCODE_A, SDL_KMOD_LCTRL);
                d.text("50");
            }
            if (w) { // (a click on its title: the box loses the focus)
                d.moveUi(w->position.x + 60, w->position.y + 10);
                d.click();
            }
            const float temp = screen.settings().ambientTemp;
            d.shot(shots.empty() ? "" : shots + "-settings.ppm");
            d.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
            check(w && waterOn && std::fabs(temp - (50.0f + 273.15f)) < 0.01f && screen.windows().empty(),
                  std::string("the settings: ") + (w ? "" : "didn't open; ") +
                      (waterOn ? "water equalisation on" : "the water checkbox did nothing") +
                      ", ambient air \"50\" C: " + std::to_string(temp) + " K");
        }
        // Favourites: ctrl+shift+click on DUST adds it, and the Favorites menu shows it
        {
            const int dust = screen.ui().toolIndex("DEFAULT_PT_DUST");
            screen.ui().setActiveMenu(screen.ui().tools()[size_t(dust)].section);
            d.frames();
            int bx = 0, by = 0;
            const bool there = screen.ui().paletteButton(dust, bx, by);
            d.moveUi(bx, by);
            d.frames();
            d.hold(SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT));
            d.click();
            d.hold(SDL_KMOD_NONE);
            const auto &fav = screen.ui().favourites();
            screen.ui().setActiveMenu(SC_FAVORITES);
            d.frames();
            int fx = 0, fy = 0;
            const bool inMenu = screen.ui().paletteButton(dust, fx, fy);
            screen.ui().setActiveMenu(screen.ui().tools()[size_t(dust)].section);
            check(there && fav.size() == 1 && fav[0] == "DEFAULT_PT_DUST" && inMenu,
                  "ctrl+shift+click on DUST: " + std::to_string(fav.size()) + " favourite" +
                      (inMenu ? ", in the Favorites menu" : ", not in the Favorites menu"));
        }
        // P: a screenshot, a PNG of the world in the data folder
        {
            d.key(SDLK_P, SDL_SCANCODE_P);
            d.frames();
            std::string png;
            std::error_code ec;
            for (const auto &e : std::filesystem::directory_iterator(screen.dataFolder(), ec)) {
                const std::string n = e.path().filename().string();
                if (n.rfind("screenshot ", 0) == 0 && e.path().extension() == ".png")
                    png = e.path().string();
            }
            std::ifstream f(png, std::ios::binary);
            char sig[8] = {};
            f.read(sig, 8);
            check(!png.empty() && std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0,
                  "P: " + (png.empty() ? std::string("no screenshot") : std::filesystem::path(png).filename().string()));
        }
    }
    // The prefs (powder.pref) the runs above left: a new game screen starts with them. With fast quit off, closing
    // the window closes the window on top, then the game.
    for (const char *mode : {"classic", "screen"}) {
        AppOptions o = base;
        o.mode = mode;
        o.dataDir = (dataDir / mode).string();
        if (!o.sizeSet)
            o.W = 1280, o.H = 720;
        o.scenes = {"empty"};
        const std::string pref = o.dataDir + "/powder.pref";
        const bool file = std::filesystem::exists(pref);
        {
            Prefs p(pref);
            p.setBool("FastQuit", false);
        }
        GameScreen screen(ctx, o);
        Driver d(ctx, screen, 1280, 720);
        d.frames(2);
        std::printf("== %s again\n", mode);
        auto check = [&](bool ok, const std::string &what) {
            checks++;
            failures += !ok;
            std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        };
        const auto &fav = screen.ui().favourites();
        const bool favOk = fav.size() == 1 && fav[0] == "DEFAULT_PT_DUST";
        const bool tempOk = std::fabs(screen.settings().ambientTemp - (50.0f + 273.15f)) < 0.01f;
        const bool viewOk = screen.view().colorMode == tptPresetView(6).colorMode;
        check(file && favOk && tempOk && viewOk,
              std::string("powder.pref") + (file ? "" : " missing") + ": the favourite" + (favOk ? "" : " lost") +
                  ", the air temperature" + (tempOk ? "" : " lost") + ", the heat display" + (viewOk ? "" : " lost"));
        d.key(SDLK_E, SDL_SCANCODE_E);
        const bool search = screen.windows().depth() == 1;
        SDL_Event q{};
        q.type = SDL_EVENT_QUIT;
        screen.event(q);
        d.frames();
        const bool closedWindow = screen.windows().empty() && screen.running();
        screen.event(q);
        check(search && closedWindow && !screen.running(),
              std::string("fast quit off: closing the window ") + (closedWindow ? "closes the search" : "didn't close the search") +
                  (screen.running() ? ", then didn't quit" : ", then quits"));
    }
    std::error_code ec;
    std::filesystem::remove_all(dataDir, ec);
    std::printf("%d of %d as expected\n", checks - failures, checks);
    return failures ? 1 : 0;
}

} // namespace bp
