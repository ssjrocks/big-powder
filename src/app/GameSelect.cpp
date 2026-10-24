// The game screen's selections, as TPT's GameView and GameController have them: choosing an area to copy (ctrl+C),
// cut (ctrl+X) or make a stamp of (S), and placing a save (ctrl+V, L and the stamp browser), with its preview moved by
// the arrows and turned by R.
#include "app/GameScreen.h"
#include "ui/Dialogs.h"

#include <algorithm>
#include <utility>

namespace bp {

namespace {

std::pair<int, int> floorDiv(int a, int b) {
    int q = a >= 0 ? a / b : -((-a + b - 1) / b);
    return {q, a - q * b};
}

} // namespace

void GameScreen::beginSelect(Select mode, const std::u32string &tip) {
    selectMode_ = mode;
    selectX1_ = selectY1_ = selectX2_ = selectY2_ = -1;
    selectDown_ = false;
    input_.cancel();
    ui_.setButtonTip(tip);
}

void GameScreen::setPlaceSave(std::optional<SaveArea> save) {
    placed_.reset();
    placeSave_ = std::move(save);
    if (placeSave_ && placeSave_->missingElements)
        report("Paste content has missing custom elements");
    // NotifyPlaceSaveChanged
    placeTransform_ = MAT_IDENTITY;
    placeTranslateX_ = placeTranslateY_ = 0;
    applyTransformPlaceSave();
}

void GameScreen::applyTransformPlaceSave() {
    const int remX = floorDiv(placeTranslateX_, CELL).second, remY = floorDiv(placeTranslateY_, CELL).second;
    // GameModel::TransformPlaceSave, cut to this world
    if (placeSave_) {
        placed_ = *placeSave_;
        tptTransform(*placed_, placeTransform_, remX, remY, int(world_->width()) / CELL, int(world_->height()) / CELL);
    }
    // NotifyTransformedPlaceSaveChanged
    if (placed_) {
        placeThumb_ = world_->renderSave(*placed_);
        thumbVersion_++;
        selectMode_ = Select::Place;
        worldPoint(mouseX_, mouseY_, selectX2_, selectY2_);
    } else {
        placeThumb_.clear();
        selectMode_ = Select::None;
    }
}

void GameScreen::translateSave(int dx, int dy) {
    placeTranslateX_ += dx;
    placeTranslateY_ += dy;
    applyTransformPlaceSave();
}

void GameScreen::transformSave(const Mat2i &m) {
    placeTranslateX_ = placeTranslateY_ = 0; // reset offset
    placeTransform_ = matMul(m, placeTransform_);
    applyTransformPlaceSave();
}

void GameScreen::placeSavePos(int &bx, int &by) const {
    auto [trQuoX, trRemX] = floorDiv(placeTranslateX_, CELL);
    auto [trQuoY, trRemY] = floorDiv(placeTranslateY_, CELL);
    int usefulX = placed_ ? placed_->cw * CELL : 0, usefulY = placed_ ? placed_->ch * CELL : 0;
    if (trRemX)
        usefulX -= CELL;
    if (trRemY)
        usefulY -= CELL;
    // stamp coordinates
    const int cursorX = (usefulX - CELL) / 2 - trQuoX * CELL, cursorY = (usefulY - CELL) / 2 - trQuoY * CELL;
    bx = floorDiv(selectX2_ - cursorX, CELL).first;
    by = floorDiv(selectY2_ - cursorY, CELL).first;
}

bool GameScreen::placeKey(SDL_Keycode k, SDL_Scancode sc, bool repeat, bool shift, bool ctrl) {
    switch (k) {
    case SDLK_RIGHT:
        translateSave(1, 0);
        return true;
    case SDLK_LEFT:
        translateSave(-1, 0);
        return true;
    case SDLK_UP:
        translateSave(0, -1);
        return true;
    case SDLK_DOWN:
        translateSave(0, 1);
        return true;
    default:
        break;
    }
    if (sc == SDL_SCANCODE_R && !repeat) {
        if (ctrl && shift)
            transformSave(MAT_MIRROR_Y); // vertical flip
        else if (!ctrl && shift)
            transformSave(MAT_MIRROR_X); // horizontal flip
        else
            transformSave(MAT_CCW); // rotate 90 degrees
        return true;
    }
    return false;
}

// GameView::OnMouseDown while selecting (any button; the left one starts the area)
void GameScreen::selectPress(int button) {
    selectDown_ = true;
    if (button == 0 && selectX1_ == -1) {
        worldPoint(mouseX_, mouseY_, selectX1_, selectY1_);
        selectX2_ = selectX1_, selectY2_ = selectY1_;
    }
}

void GameScreen::selectMove() {
    if (selectMode_ == Select::Place)
        worldPoint(mouseX_, mouseY_, selectX1_, selectY1_);
    if (selectX1_ != -1)
        worldPoint(mouseX_, mouseY_, selectX2_, selectY2_);
}

// GameView::OnMouseUp while selecting: the left button does it, any other lets go of it
void GameScreen::selectRelease(int button) {
    if (!selectDown_) {
        if (button != 0)
            selectMode_ = Select::None;
        return;
    }
    selectDown_ = false;
    if (button == 0 && selectX1_ != -1 && selectY1_ != -1 && selectX2_ != -1 && selectY2_ != -1) {
        if (selectMode_ == Select::Place) {
            // (not let go over the bottom bar's buttons)
            if (!placeThumb_.empty() && int(mouseY_) / uiScale() <= ui_.height() - GameUi::BARSIZE) {
                int bx, by;
                placeSavePos(bx, by);
                placeSave(bx, by);
            }
        } else {
            const int x2 = std::max(selectX1_, selectX2_), y2 = std::max(selectY1_, selectY2_);
            const int x1 = std::min(selectX1_, selectX2_), y1 = std::min(selectY1_, selectY2_);
            if (selectMode_ == Select::Copy)
                copyRegion(x1, y1, x2, y2);
            else if (selectMode_ == Select::Cut)
                cutRegion(x1, y1, x2, y2);
            else if (selectMode_ == Select::Stamp)
                stampRegion(x1, y1, x2, y2);
        }
    }
    selectMode_ = Select::None;
}

void GameScreen::copyRegion(int x1, int y1, int x2, int y2) {
    flushUses();
    clipboard_ = world_->copyArea(x1, y1, x2, y2, includePressure_ != shift_);
    clipboard_->paused = paused_;
}

void GameScreen::cutRegion(int x1, int y1, int x2, int y2) {
    copyRegion(x1, y1, x2, y2);
    historySnapshot();
    world_->clearArea(x1, y1, x2 - x1, y2 - y1);
}

void GameScreen::stampRegion(int x1, int y1, int x2, int y2) {
    flushUses();
    SaveArea s = world_->copyArea(x1, y1, x2, y2, includePressure_ != shift_);
    s.paused = paused_;
    std::string err;
    if (stamps_.add(s, err).empty())
        showWindow(std::make_unique<ErrorMessage>(U"Could not create stamp", utf32(err)));
}

void GameScreen::placeSave(int bx, int by) {
    if (!placed_)
        return;
    historySnapshot();
    std::string msg = world_->pasteArea(*placed_, !shift_, bx, by);
    if (!msg.empty())
        report(msg);
    paused_ = placed_->paused || paused_;
}

void GameScreen::loadLastStamp() {
    SaveArea s;
    std::string err;
    if (!stamps_.load(stamps_.names()[0], s, err)) {
        report("could not load stamp " + stamps_.names()[0] + ": " + err);
        return;
    }
    setPlaceSave(std::move(s));
    worldPoint(mouseX_, mouseY_, selectX1_, selectY1_);
    selectX2_ = selectX1_, selectY2_ = selectY1_;
    selectDown_ = false;
    input_.cancel();
}

// GameView::OnDraw's selection: the world darkened outside the area (all of it before one is started) with a dotted
// rectangle around it, or the save being placed shown half see-through where it would go
void GameScreen::drawSelectLayer(VkCommandBuffer cmd, int slot) {
    Overlay::Layer l;
    const int W = int(world_->width()), H = int(world_->height());
    l.worldW = uint32_t(W), l.worldH = uint32_t(H);
    if (selectMode_ == Select::Place) {
        if (!placed_ || placeThumb_.empty() || selectX2_ == -1)
            return;
        int bx, by;
        placeSavePos(bx, by);
        l.pixels = &placeThumb_;
        l.x0 = bx * CELL, l.y0 = by * CELL;
        l.w = uint32_t(placed_->cw * CELL), l.h = uint32_t(placed_->ch * CELL);
        l.opaque = true;
        l.alpha = 0x80;
        l.version = thumbVersion_;
        l.dotted = true;
        l.dx0 = l.x0, l.dy0 = l.y0, l.dx1 = l.x0 + int(l.w) - 1, l.dy1 = l.y0 + int(l.h) - 1;
    } else {
        l.dim = 100;
        if (selectX1_ != -1) {
            const int x2 = std::min(std::max(selectX1_, selectX2_), W - 1);
            const int y2 = std::min(std::max(selectY1_, selectY2_), H - 1);
            const int x1 = std::min(selectX1_, selectX2_), y1 = std::min(selectY1_, selectY2_);
            l.rx0 = x1, l.ry0 = y1, l.rx1 = x2, l.ry1 = y2;
            l.dotted = true;
            l.dx0 = x1, l.dy0 = y1, l.dx1 = x2, l.dy1 = y2;
        }
    }
    overlay_.recordLayer(cmd, slot, l, cam_.x, cam_.y, cam_.zoom, world_->frameBuffer(), sw_, sh_);
}

void GameScreen::openStamps() {
    StampBrowser::Store store;
    store.names = [this] { return stamps_.names(); };
    store.load = [this](const std::string &n, SaveArea &out, std::string &err) { return stamps_.load(n, out, err); };
    store.remove = [this](const std::string &n) { stamps_.remove(n); };
    store.rename = [this](const std::string &n, const std::string &to, std::string &err) {
        return stamps_.rename(n, to, err);
    };
    store.rescan = [this] { stamps_.rescan(); };
    showWindow(std::make_unique<StampBrowser>(ui_.width(), ui_.height(), store, [this](const std::string &n, bool front) {
        chosenStamp_ = {n, front};
    }));
}

void GameScreen::openChosenStamp() {
    auto [name, front] = *chosenStamp_;
    chosenStamp_.reset();
    windows_.collect(); // (the browser is gone first)
    SaveArea s;
    std::string err;
    if (!stamps_.load(name, s, err)) {
        showWindow(std::make_unique<ErrorMessage>(U"Error loading stamp", utf32(err)));
        setPlaceSave(std::nullopt);
        return;
    }
    if (front)
        stamps_.moveToFront(name);
    setPlaceSave(std::move(s)); // GameController::LoadStamp
}

} // namespace bp
