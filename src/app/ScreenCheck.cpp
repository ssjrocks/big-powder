// --check-screen: the game screen (app/GameScreen) driven by made-up SDL events and no window, in Classic mode and
// on the GPU: TPT's keys and mouse through to its windows and tools, checked by what they do to the world and the
// interface, with pictures of some of the steps.
#include "app/App.h"
#include "app/GameScreen.h"
#include "ui/Dialogs.h"
#include "sim/ElementIds.h"
#include "sim/Signs.h"

#include <SDL3/SDL_events.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

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
    for (const char *mode : {"classic", "screen"}) {
        AppOptions o = base;
        o.mode = mode;
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
    }
    std::printf("%d of %d as expected\n", checks - failures, checks);
    return failures ? 1 : 0;
}

} // namespace bp
