#include "ui/Widgets.h"
#include "sim/RenderTables.h"
#include "classic/TptTools.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cmath>

namespace bp::ui {

Host &host() {
    static Host h;
    return h;
}

namespace {

Point textSizeOf(const std::u32string &s) {
    int w = 0, h = 0;
    if (host().font)
        host().font->textSize(s, w, h);
    return {w, h};
}

int charWidthOf(char32_t c) { return host().font ? host().font->charWidth(c) : 0; }

std::u32string maskedText(const std::u32string &s) { return std::u32string(s.size(), 0xE00D); }

} // namespace

// ---- Component ----

bool Component::isFocused() const { return window_ && window_->isFocused(this); }

Point Component::containerPos() const {
    Point p;
    if (window_)
        p.x += window_->position.x, p.y += window_->position.y;
    if (panel_)
        p.x += panel_->position.x + panel_->viewport.x, p.y += panel_->position.y + panel_->viewport.y;
    return p;
}

Point Component::screenPos() const {
    Point c = containerPos();
    return {c.x + position.x, c.y + position.y};
}

void Component::placeText(const std::u32string &text) {
    textPos_ = {0, 0};
    textSize_ = textSizeOf(text);
    int textWidth = textSize_.x - 1, textHeight = textSize_.y - 3;
    const Appearance &a = appearance;
    if (a.icon != Icon::None)
        textWidth += 13;
    int areaW = size.x - (a.margin.right + a.margin.left), areaH = size.y - (a.margin.top + a.margin.bottom);
    switch (a.vAlign) {
    case Appearance::VAlign::Top: textPos_.y = a.margin.top + 2; break;
    case Appearance::VAlign::Middle: textPos_.y = a.margin.top + (areaH - textHeight) / 2; break;
    case Appearance::VAlign::Bottom: textPos_.y = size.y - (textHeight + a.margin.bottom); break;
    }
    switch (a.hAlign) {
    case Appearance::HAlign::Left: textPos_.x = a.margin.left; break;
    case Appearance::HAlign::Centre: textPos_.x = a.margin.left + (areaW - textWidth) / 2; break;
    case Appearance::HAlign::Right: textPos_.x = size.x - (textWidth + a.margin.right); break;
    }
    if (a.icon != Icon::None) {
        iconPos_ = {textPos_.x, textPos_.y - 1};
        textPos_.x += 15;
    }
}

// ---- TextWrapper ----

int TextWrapper::update(const std::u32string &text, bool doWrapping, int maxWidth) {
    rawSize_ = int(text.size());
    struct Record {
        char32_t character;
        int width, raw, clear;
        bool wraps, mayEatSpace;
    };
    int lineWidth = 0;
    std::vector<Record> records;
    int wordBeginsAt = -1; // an index into records; not in a word
    int wordWidth = 0, lines = 0, charWidth = 0, clearCount = 0;
    wrappedWidth_ = 0;
    auto resetLine = [&] {
        wrappedWidth_ = std::max(wrappedWidth_, lineWidth);
        lineWidth = 0;
        lines += 1;
    };
    auto wrapIfNeeded = [&](int widthToConsider) {
        if (doWrapping && widthToConsider + charWidth > maxWidth) {
            // a newline that makes the line wrap when drawn, unseen by the click map, that may eat a space after it
            records.push_back({U'\n', 0, 0, 0, true, true});
            resetLine();
            return true;
        }
        return false;
    };
    for (size_t it = 0; it < text.size(); ++it) {
        char32_t ch = text[it];
        charWidth = charWidthOf(ch);
        int sequenceLength = 0;
        switch (ch) {
        case U'\b': sequenceLength = 2; break;
        case 0x0E: sequenceLength = 1; break;
        case 0x0F: sequenceLength = 4; break;
        }
        switch (ch) {
        case U' ':
            wrapIfNeeded(lineWidth);
            if (!records.empty() && records.back().mayEatSpace)
                records.back().mayEatSpace = false;
            else {
                records.push_back({ch, charWidth, int(it), clearCount, false, false});
                lineWidth += charWidth;
            }
            wordBeginsAt = -1;
            ++clearCount;
            break;
        case U'\n':
            records.push_back({ch, maxWidth - lineWidth, int(it), clearCount, true, false});
            resetLine();
            wordBeginsAt = -1;
            ++clearCount;
            break;
        default:
            if (sequenceLength) {
                if (text.size() - it < size_t(sequenceLength)) {
                    it = text.size() - 1;
                    continue; // broken text: skip the rest
                }
                for (size_t skip = it + size_t(sequenceLength); it != skip; ++it)
                    records.push_back({text[it], 0, 0, 0, false, false});
                --it;
            } else {
                if (wordBeginsAt == -1) {
                    wordBeginsAt = int(records.size());
                    wordWidth = 0;
                }
                if (wrapIfNeeded(wordWidth)) {
                    wordBeginsAt = int(records.size());
                    wordWidth = 0;
                }
                if (wrapIfNeeded(lineWidth)) {
                    // break the line before the word instead: move the newline just added to where the word begins
                    std::rotate(records.begin() + wordBeginsAt, records.end() - 1, records.end());
                    wordBeginsAt += 1;
                    lineWidth = wordWidth;
                }
                records.push_back({ch, charWidth, int(it), clearCount, false, false});
                wordWidth += charWidth;
                lineWidth += charWidth;
                ++clearCount;
                switch (ch) {
                case U'?': case U';': case U',': case U':': case U'.': case U'-': case U'!':
                    wordBeginsAt = -1; // these end a word
                    break;
                }
            }
            break;
        }
    }
    regions_.clear();
    wrapped_.clear();
    int x = 0, l = 0, counter = 0;
    for (const Record &r : records) {
        regions_.push_back({x, l * FONT_H, r.width, l + 1, Index{r.raw, counter, r.clear}});
        ++counter;
        x += r.width;
        if (r.wraps) {
            x = 0;
            l += 1;
        }
        wrapped_.push_back(r.character);
    }
    resetLine();
    clearSize_ = clearCount;
    lines_ = lines;
    return lines;
}

TextWrapper::Index TextWrapper::pointToIndex(int x, int y) const {
    if (y < 0)
        return begin();
    if (!regions_.empty()) {
        size_t curr = 0;
        auto nextNonEmpty = [&](size_t it) {
            ++it;
            while (it != regions_.size() && !regions_[it].width)
                ++it;
            return it;
        };
        while (true) {
            const Region &c = regions_[curr];
            if (c.y + FONT_H > y && c.x + c.width / 2 > x)
                return c.index;
            size_t next = nextNonEmpty(curr);
            if (next == regions_.size())
                break;
            const Region &n = regions_[next];
            if (c.y + FONT_H > y) {
                if (c.x + c.width / 2 <= x && n.x + n.width / 2 > x)
                    return n.index;
                if (c.x + c.width / 2 <= x && n.y > c.y)
                    return n.index;
            }
            curr = next;
        }
    }
    return end();
}

int TextWrapper::indexToPoint(Index index, int &x, int &y) const {
    if (index.wrapped < 0 || index.wrapped > int(regions_.size()) || regions_.empty())
        return -1;
    if (index.wrapped == int(regions_.size())) {
        const Region &r = regions_[size_t(index.wrapped) - 1];
        x = r.x + r.width;
        y = r.y;
        return r.line;
    }
    const Region &r = regions_[size_t(index.wrapped)];
    x = r.x;
    y = r.y;
    return r.line;
}

TextWrapper::Index TextWrapper::clearToIndex(int clear) const {
    if (clear < 0)
        return begin();
    for (const Region &r : regions_)
        if (r.index.clear >= clear)
            return r.index;
    return end();
}

// ---- Label ----

Label::Label(Point position, Point size, const std::u32string &text) : Component(position, size) {
    autoHeight_ = size.y == -1;
    if (!text.empty())
        Label::setText(text);
}

void Label::setMultiline(bool on) {
    multiline_ = on;
    updateWrapper();
    updateSelection();
    placeText(displayWrapper_.wrappedText());
}

void Label::setText(const std::u32string &text) {
    text_ = text;
    updateWrapper();
    updateSelection();
    placeText(displayWrapper_.wrappedText());
}

void Label::autoHeight() {
    bool old = autoHeight_;
    autoHeight_ = true;
    updateWrapper();
    autoHeight_ = old;
}

void Label::updateWrapper() {
    const int width = size.x - appearance.margin.left - appearance.margin.right;
    int lines = wrapper_.update(text_, multiline_, width);
    displayWrapper_.update(displayText_.empty() ? text_ : displayText_, multiline_, width);
    if (autoHeight_)
        size.y = lines * FONT_H + 3;
}

void Label::onMouseDown(int x, int y, int button) {
    if (!mouseDownInside || button == 3) // (the right button's copy menu is left out)
        return;
    selecting_ = true;
    Point tp{textPos_.x - scrollX_, textPos_.y};
    sel0_ = wrapper_.pointToIndex(x - position.x - tp.x, y - position.y - tp.y);
    selL_ = selH_ = sel0_;
    updateSelection();
}

void Label::copySelection() {
    if (!host().clipboardSet)
        return;
    if (hasSelection())
        host().clipboardSet(text_.substr(size_t(selL_.raw), size_t(selH_.raw - selL_.raw)));
    else
        host().clipboardSet(text_);
}

void Label::onMouseUp(int, int, int) { selecting_ = false; }

void Label::onKeyPress(int, int scan, bool repeat, bool, bool ctrl, bool) {
    if (repeat)
        return;
    if (ctrl && scan == SDL_SCANCODE_C)
        copySelection();
    if (ctrl && scan == SDL_SCANCODE_A)
        selectAll();
}

void Label::onMouseMoved(int x, int y) {
    if (!selecting_)
        return;
    Point tp{textPos_.x - scrollX_, textPos_.y};
    sel1_ = wrapper_.pointToIndex(x - tp.x, y - tp.y);
    if (sel1_.raw < sel0_.raw)
        selL_ = sel1_, selH_ = sel0_;
    else
        selL_ = sel0_, selH_ = sel1_;
    updateSelection();
}

void Label::tick() {
    if (multiline_)
        scrollX_ = 0;
    if (!isFocused() && (hasSelection() || selecting_))
        clearSelection();
}

void Label::clearSelection() {
    selecting_ = false;
    selL_ = selH_ = wrapper_.begin();
    updateSelection();
}

void Label::selectAll() {
    selL_ = wrapper_.begin();
    selH_ = wrapper_.end();
    updateSelection();
}

void Label::updateSelection() {
    if (selL_.raw < 0)
        selL_ = wrapper_.begin();
    if (selL_.raw > int(text_.size()))
        selL_ = wrapper_.end();
    if (selH_.raw < 0)
        selH_ = wrapper_.begin();
    if (selH_.raw > int(text_.size()))
        selH_ = wrapper_.end();
    displayWithSelection_ = displayWrapper_.wrappedText();
    if (hasSelection()) {
        auto l = displayWrapper_.clearToIndex(selL_.clear), h = displayWrapper_.clearToIndex(selH_.clear);
        displayWithSelection_.insert(size_t(l.wrapped), 1, 0x01);
        displayWithSelection_.insert(size_t(h.wrapped) + 1, 1, 0x01);
    }
}

void Label::draw(Canvas &c, Point screenPos) {
    if (!drawn_) {
        placeText(displayWrapper_.wrappedText());
        updateWrapper();
        updateSelection();
        drawn_ = true;
    }
    auto l = displayWrapper_.clearToIndex(selL_.clear), h = displayWrapper_.clearToIndex(selH_.clear);
    int xl = 0, yl = 0, xh = 0, yh = 0;
    int lineL = displayWrapper_.indexToPoint(l, xl, yl), lineH = displayWrapper_.indexToPoint(h, xh, yh);
    Canvas::Clip saved = c.clip();
    c.narrowClip(screenPos.x + 1, screenPos.y + 1, size.x - 2, size.y - 2);
    const int tx = screenPos.x + textPos_.x - scrollX_, ty = screenPos.y + textPos_.y;
    if (hasSelection()) {
        if (lineH == lineL)
            c.drawFilledRect(tx + xl - 1, ty + yl - 2, xh - xl + 1, FONT_H, 0xFFFFFF);
        else {
            c.drawFilledRect(tx + xl - 1, ty + yl - 2, textSize_.x - xl + 1, FONT_H, 0xFFFFFF);
            for (int i = 1; i < lineH - lineL; ++i)
                c.drawFilledRect(tx - 1, ty + yl - 2 + i * FONT_H, textSize_.x + 1, FONT_H, 0xFFFFFF);
            c.drawFilledRect(tx - 1, ty + yh - 2, xh + 1, FONT_H, 0xFFFFFF);
        }
    }
    c.blendText(tx, ty, displayWithSelection_, rgb(textColour_));
    c.setClip(saved);
}

// ---- Button ----

Button::Button(Point position, Point size, const std::u32string &text, const std::u32string &toolTip)
    : Component(position, size), text_(text), toolTip_(toolTip) {
    placeButtonText();
}

// Button::TextPosition: text too wide ends in "..."
void Button::placeButtonText() {
    displayText_ = text_;
    if (!displayText_.empty()) {
        bool icon = appearance.icon != Icon::None;
        if (textSizeOf(displayText_).x - 1 > size.x - (icon ? 22 : 0) && host().font) {
            displayText_.erase(host().font->textFit(displayText_, size.x - (icon ? 38 : 22)));
            displayText_ += U"...";
        }
    }
    placeText(displayText_);
}

void Button::setText(const std::u32string &text) {
    text_ = text;
    placeButtonText();
}

void Button::setIcon(Icon icon) {
    appearance.icon = icon;
    placeButtonText();
}

void Button::draw(Canvas &c, Point p) {
    if (!drawn_) {
        placeButtonText();
        drawn_ = true;
    }
    const Appearance &a = appearance;
    Rgba text = a.textInactive, border = a.borderInactive, background = a.backgroundInactive;
    if (enabled) {
        if ((buttonDown_ && mouseDownInside) || (togglable_ && toggle_))
            text = a.textActive, border = a.borderActive, background = a.backgroundActive;
        else if (mouseInside)
            text = a.textHover, border = a.borderHover, background = a.backgroundHover;
    } else
        text = a.textDisabled, border = a.borderDisabled, background = a.backgroundDisabled;
    c.blendFilledRect(p.x + 1, p.y + 1, size.x - 2, size.y - 2, background);
    if (a.border.all(1))
        c.blendRect(p.x, p.y, size.x, size.y, border);
    else {
        if (a.border.top)
            c.blendLine(p.x, p.y, p.x + size.x - 1, p.y, border);
        if (a.border.bottom)
            c.blendLine(p.x, p.y + size.y - 1, p.x + size.x - 1, p.y + size.y - 1, border);
        if (a.border.left)
            c.blendLine(p.x, p.y, p.x, p.y + size.y - 1, border);
        if (a.border.right)
            c.blendLine(p.x + size.x - 1, p.y, p.x + size.x - 1, p.y + size.y - 1, border);
    }
    c.blendText(p.x + textPos_.x, p.y + textPos_.y, displayText_, text);
    bool invert = (background.b + 3 * background.g + 2 * background.r) > 544;
    if (a.icon != Icon::None)
        c.drawIcon(p.x + iconPos_.x, p.y + iconPos_.y, a.icon, enabled ? 255 : 180, invert);
}

void Button::doAction() {
    if (enabled && action)
        action();
}

void Button::onMouseClick(int, int, int button) {
    if (button == 1) {
        if (buttonDown_) {
            if (togglable_)
                toggle_ = !toggle_;
            buttonDown_ = false;
            doAction();
        }
    } else if (button == 3 && altButtonDown_) {
        altButtonDown_ = false;
        if (enabled && altAction)
            altAction();
    }
}

void Button::onMouseUp(int, int, int) { buttonDown_ = altButtonDown_ = false; }

void Button::onMouseDown(int, int, int button) {
    if (!mouseDownInside || !enabled)
        return;
    if (button == 1)
        buttonDown_ = true;
    else if (button == 3)
        altButtonDown_ = true;
}

void Button::onMouseEnter(int, int) {
    if (enabled && mouseEnterAction)
        mouseEnterAction();
}

void Button::onMouseHover(int, int) {
    if (enabled && !toolTip_.empty() && parentWindow())
        parentWindow()->toolTip(position, toolTip_);
}

void Button::onMouseLeave(int, int) { buttonDown_ = false; }

// ---- Textbox ----

Textbox::Textbox(Point position, Point size, const std::u32string &text, const std::u32string &placeholder)
    : Label(position, size, U""), placeholder_(placeholder) {
    doesTextInput = true;
    Textbox::setText(text);
    cursor_ = int(text_.size());
}

void Textbox::showText() {
    if (masked_)
        Label::setText(maskedText(backing_));
    else
        text_ = backing_;
}

void Textbox::setText(const std::u32string &text) {
    backing_ = text;
    if (masked_)
        Label::setText(maskedText(text));
    else
        Label::setText(text);
    cursor_ = int(text.size());
    resetCursorPosition();
}

void Textbox::resetCursorPosition() {
    if (cursor_)
        wrapper_.indexToPoint(wrapper_.clearToIndex(cursor_), cursorX_, cursorY_);
    else
        cursorX_ = cursorY_ = 0;
}

void Textbox::tabFocus() {
    if (parentWindow())
        parentWindow()->focus(this);
    selectAll();
}

void Textbox::cutSelection() {
    if (hasSelection()) {
        if (selL_.raw < 0 || selH_.raw > int(backing_.size()))
            return;
        if (host().clipboardSet)
            host().clipboardSet(backing_.substr(size_t(selL_.raw), size_t(selH_.raw - selL_.raw)));
        backing_.erase(size_t(selL_.raw), size_t(selH_.raw - selL_.raw));
        cursor_ = selL_.raw;
    } else {
        if (backing_.empty())
            return;
        if (host().clipboardSet)
            host().clipboardSet(backing_);
        backing_.clear();
        cursor_ = 0;
    }
    clearSelection();
    showText();
    updateWrapper();
    updateSelection();
    placeText(displayWrapper_.wrappedText());
    resetCursorPosition();
    if (change)
        change();
}

void Textbox::pasteIntoSelection() {
    std::u32string newText = host().clipboardGet ? host().clipboardGet() : U"";
    // format::CleanString: no formatting codes, and for a one-line box no line breaks; numbers only in number boxes
    std::u32string clean;
    for (char32_t ch : newText) {
        if (ch == U'\n' && input_ != Input::Multiline)
            continue;
        if (ch < U' ' && ch != U'\n')
            continue;
        if ((input_ == Input::Number || input_ == Input::Numeric) && (ch < U'0' || ch > U'9'))
            continue;
        clean.push_back(ch);
    }
    if (hasSelection()) {
        if (selL_.raw < 0 || selH_.raw > int(backing_.size()))
            return;
        backing_.erase(size_t(selL_.raw), size_t(selH_.raw - selL_.raw));
        cursor_ = selL_.raw;
    }
    if (limit_ != std::u32string::npos)
        clean = clean.substr(0, limit_ > backing_.size() ? limit_ - backing_.size() : 0);
    backing_.insert(size_t(cursor_), clean);
    cursor_ += int(clean.size());
    clearSelection();
    showText();
    updateWrapper();
    updateSelection();
    placeText(displayWrapper_.wrappedText());
    resetCursorPosition();
    if (change)
        change();
}

bool Textbox::characterValid(char32_t c) const {
    switch (input_) {
    case Input::Numeric:
        if (c == U'-' && cursor_ == 0 && (backing_.empty() || backing_[0] != U'-'))
            return true;
        [[fallthrough]];
    case Input::Number:
        return c >= U'0' && c <= U'9';
    case Input::Multiline:
        if (c == U'\n')
            return true;
        [[fallthrough]];
    case Input::All:
    default:
        return c >= U' ' && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF) && !(c >= 0xFDD0 && c <= 0xFDEF) &&
               !((c & 0xFFFF) >= 0xFFFE);
    }
}

bool Textbox::stringValid(const std::u32string &s) const {
    for (char32_t c : s)
        if (!characterValid(c))
            return false;
    return true;
}

void Textbox::tick() {
    Label::tick();
    if (!multiline_) {
        int regionWidth = size.x;
        if (appearance.icon != Icon::None)
            regionWidth -= 13;
        regionWidth -= appearance.margin.left + appearance.margin.right;
        if (scrollX_ > displayWrapper_.wrappedWidth() - regionWidth)
            scrollX_ = displayWrapper_.wrappedWidth() - regionWidth;
        if (scrollX_ < cursorX_ - regionWidth)
            scrollX_ = cursorX_ - regionWidth;
        if (scrollX_ > cursorX_)
            scrollX_ = cursorX_;
        if (scrollX_ < 0)
            scrollX_ = 0;
    }
}

void Textbox::onKeyPress(int key, int scan, bool repeat, bool, bool ctrl, bool) {
    bool changed = false;
    if (ctrl && scan == SDL_SCANCODE_C && !masked_ && !repeat) {
        copySelection();
        return;
    }
    if (ctrl && scan == SDL_SCANCODE_V && !readOnly) {
        pasteIntoSelection();
        return;
    }
    if (ctrl && scan == SDL_SCANCODE_X && !masked_ && !repeat && !readOnly) {
        cutSelection();
        return;
    }
    if (ctrl && scan == SDL_SCANCODE_A) {
        selectAll();
        return;
    }
    static const std::u32string stops = U" .,!?\n";
    auto eraseSelection = [&] {
        int lo = selL_.raw, hi = selH_.raw;
        if (lo < 0 || hi > int(backing_.size()))
            return false;
        backing_.erase(size_t(lo), size_t(hi - lo));
        cursor_ = lo;
        return true;
    };
    switch (key) {
    case SDLK_HOME:
        cursor_ = 0;
        clearSelection();
        break;
    case SDLK_END:
        cursor_ = int(backing_.size());
        clearSelection();
        break;
    case SDLK_LEFT:
        if (cursor_ > 0)
            cursor_--;
        clearSelection();
        break;
    case SDLK_RIGHT:
        if (cursor_ < int(backing_.size()))
            cursor_++;
        clearSelection();
        break;
    case SDLK_DELETE:
        if (readOnly)
            break;
        if (hasSelection()) {
            if (!eraseSelection())
                return;
            changed = true;
        } else if (!backing_.empty() && cursor_ < int(backing_.size())) {
            if (ctrl) {
                size_t stop = backing_.find_first_not_of(stops, size_t(cursor_));
                stop = backing_.find_first_of(stops, stop);
                if (size_t(cursor_) < stop)
                    backing_.erase(size_t(cursor_), stop == std::u32string::npos ? std::u32string::npos
                                                                                 : stop - size_t(cursor_));
            } else
                backing_.erase(size_t(cursor_), 1);
            changed = true;
        }
        clearSelection();
        break;
    case SDLK_BACKSPACE:
        if (readOnly)
            break;
        if (hasSelection()) {
            if (!eraseSelection())
                return;
            changed = true;
        } else if (!backing_.empty() && cursor_ > 0) {
            if (ctrl) {
                long stop;
                size_t s = backing_.find_last_not_of(stops, size_t(cursor_));
                if (s == std::u32string::npos)
                    stop = -1;
                else {
                    size_t a = backing_.find_last_of(stops, s);
                    stop = a == std::u32string::npos ? -1 : long(a);
                }
                if (stop + 1 < cursor_)
                    backing_.erase(size_t(stop + 1), size_t(cursor_ - (stop + 1)));
                cursor_ = int(stop + 1);
            } else {
                backing_.erase(size_t(cursor_) - 1, 1);
                cursor_--;
            }
            changed = true;
        }
        clearSelection();
        break;
    case SDLK_RETURN:
        onTextInput(U"\n");
        break;
    }
    afterTextChange(changed);
}

void Textbox::afterTextChange(bool changed) {
    if (cursor_ > int(backing_.size()))
        cursor_ = int(backing_.size());
    if (changed) {
        if (input_ == Input::Number)
            while (backing_.size() > 1 && backing_[0] == U'0') // no leading zeros
                backing_.erase(backing_.begin());
        showText();
    }
    updateWrapper();
    updateSelection();
    placeText(displayWrapper_.wrappedText());
    resetCursorPosition();
    if (changed && change)
        change();
}

void Textbox::onTextInput(const std::u32string &text) { insertText(text); }

void Textbox::insertText(const std::u32string &s) {
    if (!stringValid(s) || readOnly)
        return;
    if (hasSelection()) {
        if (selL_.raw < 0 || selH_.raw > int(backing_.size()))
            return;
        backing_.erase(size_t(selL_.raw), size_t(selH_.raw - selL_.raw));
        cursor_ = selL_.raw;
    }
    if (limit_ == std::u32string::npos || backing_.size() < limit_) {
        backing_.insert(size_t(cursor_), s);
        cursor_ += int(s.size());
    }
    clearSelection();
    afterTextChange(true);
}

void Textbox::onMouseDown(int x, int y, int button) {
    if (mouseDownInside && button != 3) {
        mouseDown_ = true;
        auto index = wrapper_.pointToIndex(x - position.x - (textPos_.x - scrollX_), y - position.y - textPos_.y);
        cursor_ = index.raw;
        resetCursorPosition();
    }
    Label::onMouseDown(x, y, button);
}

void Textbox::onMouseUp(int x, int y, int button) {
    mouseDown_ = false;
    Label::onMouseUp(x, y, button);
}

void Textbox::onMouseMoved(int x, int y) {
    if (mouseDown_) {
        auto index = wrapper_.pointToIndex(x - (textPos_.x - scrollX_), y - textPos_.y);
        cursor_ = index.raw;
        resetCursorPosition();
    }
    Label::onMouseMoved(x, y);
}

void Textbox::onDefocus() {
    if (defocus)
        defocus();
}

void Textbox::draw(Canvas &c, Point screenPos) {
    Label::draw(c, screenPos);
    Canvas::Clip saved = c.clip();
    c.narrowClip(screenPos.x + 1, screenPos.y + 1, size.x - 2, size.y - 2);
    const int tx = screenPos.x + textPos_.x - scrollX_, ty = screenPos.y + textPos_.y;
    if (isFocused())
        c.drawLine(tx + cursorX_, ty + cursorY_ - 2, tx + cursorX_, ty + cursorY_ + 9, 0xFFFFFF);
    if (text_.empty())
        c.blendText(tx + 3, ty, placeholder_, rgb(textColour_, 170));
    if (appearance.icon != Icon::None)
        c.drawIcon(screenPos.x + iconPos_.x, screenPos.y + iconPos_.y, appearance.icon);
    c.setClip(saved);
    if (border_)
        c.drawRect(screenPos.x, screenPos.y, size.x, size.y, isFocused() ? 0xFFFFFF : 0xA0A0A0);
}

// ---- DropDown ----

namespace {

// DropDownWindow: the options as buttons, over the drop down (lifted up so the chosen one is where the drop down is,
// if there is room)
class DropDownWindow : public Window {
public:
    explicit DropDownWindow(DropDown *d)
        : Window(Point{d->screenPos().x - 1,
                       d->screenPos().y - 1 - (d->optionIndex() * 16 < d->screenPos().y ? d->optionIndex() * 16 : 0)},
                 Point{d->size.x + 2, 2 + int(d->options().size()) * 16}),
          dropDown_(d) {
        int y = 1;
        for (size_t i = 0; i < d->options().size(); i++) {
            auto b = std::make_unique<Button>(Point{1, y}, Point{size.x - 2, 16}, d->options()[i].first);
            b->appearance = d->appearance;
            if (i)
                b->appearance.border = Border(0, 1, 1, 1);
            std::u32string option = d->options()[i].first;
            b->action = [this, option] {
                close();
                dropDown_->setOptionText(option);
                if (dropDown_->change)
                    dropDown_->change();
            };
            add(std::move(b));
            y += 16;
        }
    }
    void onDraw(Canvas &c) override { c.drawFilledRect(position.x, position.y, size.x, size.y, 0x000000); }
    void onTryExit(Exit) override { close(); }

private:
    DropDown *dropDown_;
};

} // namespace

void DropDown::addOption(const std::u32string &text, int value) {
    for (const auto &o : options_)
        if (o.first == text && o.second == value)
            return;
    options_.emplace_back(text, value);
}

void DropDown::setOption(int value) {
    for (size_t i = 0; i < options_.size(); i++)
        if (options_[i].second == value) {
            index_ = int(i);
            placeText(options_[i].first);
            return;
        }
}

void DropDown::setOptionText(const std::u32string &text) {
    for (size_t i = 0; i < options_.size(); i++)
        if (options_[i].first == text) {
            index_ = int(i);
            placeText(options_[i].first);
            return;
        }
}

std::pair<std::u32string, int> DropDown::option() const {
    if (index_ >= 0)
        return options_[size_t(index_)];
    return {U"", -1};
}

void DropDown::onMouseClick(int, int, int) {
    if (Window *w = parentWindow())
        w->openPending = std::make_unique<DropDownWindow>(this);
}

void DropDown::draw(Canvas &c, Point p) {
    if (!drawn_) {
        if (index_ >= 0)
            placeText(options_[size_t(index_)].first);
        drawn_ = true;
    }
    const Appearance &a = appearance;
    Rgba text = inside_ ? a.textHover : a.textInactive, border = inside_ ? a.borderHover : a.borderInactive,
         background = inside_ ? a.backgroundHover : a.backgroundInactive;
    c.blendFilledRect(p.x - 1, p.y - 1, size.x + 2, size.y + 2, background);
    c.blendRect(p.x, p.y, size.x, size.y, border);
    if (index_ >= 0)
        c.blendText(p.x + textPos_.x, p.y + textPos_.y, options_[size_t(index_)].first, text);
}

// ---- Slider ----

void Slider::updatePosition(int position) {
    position = std::clamp(position, 3, size.x - 3);
    auto fPosition = float(position - 3);
    auto fSize = float(size.x - 6);
    int next = int((fPosition / fSize) * float(steps_));
    if (next == value_)
        return;
    value_ = next;
    if (change)
        change();
}

void Slider::onMouseMoved(int x, int) {
    if (down_)
        updatePosition(x);
}

void Slider::onMouseDown(int x, int, int) {
    if (mouseDownInside) {
        down_ = true;
        updatePosition(x - position.x);
    }
}

void Slider::onMouseUp(int, int, int) { down_ = false; }

void Slider::setColour(uint32_t from, uint32_t to) { gradient_ = tptGradient(from, to, size.x - 7); }

void Slider::draw(Canvas &c, Point p) {
    if (!gradient_.empty())
        for (int j = 3; j < size.y - 7; j++)
            for (int i = 3; i < size.x - 7; i++)
                c.drawPixel(p.x + i + 2, p.y + j + 2, gradient_[size_t(i - 3)]);
    c.drawRect(p.x + 3, p.y + 3, size.x - 6, size.y - 6, 0xFFFFFF);
    auto fSliderX = (float(size.x - 6) / float(steps_)) * float(value_);
    int sliderX = int(fSliderX) + 3;
    c.drawFilledRect(p.x + sliderX - 2, p.y + 1, 4, size.y - 2, 0x141414);
    c.drawRect(p.x + sliderX - 2, p.y + 1, 4, size.y - 2, 0xC8C8C8);
}

// ---- Panel ----

Panel::~Panel() = default;

Component *Panel::add(std::unique_ptr<Component> c) {
    c->window_ = parentWindow();
    c->panel_ = this;
    children_.push_back(std::move(c));
    return children_.back().get();
}

void Panel::clear() { children_.clear(); }

void Panel::draw(Canvas &c, Point screenPos) {
    Canvas::Clip saved = c.clip();
    c.narrowClip(screenPos.x, screenPos.y, size.x, size.y);
    xDraw(c, screenPos);
    for (auto &child : children_) {
        child->window_ = parentWindow();
        int x = child->position.x + viewport.x, y = child->position.y + viewport.y;
        if (child->visible && x + child->size.x >= 0 && y + child->size.y >= 0 && x < size.x && y < size.y)
            child->draw(c, Point{screenPos.x + x, screenPos.y + y});
    }
    c.setClip(saved);
}

void Panel::tick() {
    xTick();
    for (auto &child : children_)
        child->tick();
}

void Panel::onMouseClick(int x, int y, int button) {
    // (the children's clicks come from onMouseUp, as in TPT's Panel)
    (void)x, (void)y, (void)button;
}

// (only when pressed inside it: the child pressed takes the focus, then they are all told)
void Panel::onMouseDown(int x, int y, int button) {
    if (!mouseDownInside)
        return;
    const int lx = x - position.x, ly = y - position.y;
    for (int i = int(children_.size()) - 1; i >= 0; --i) {
        Component &c = *children_[size_t(i)];
        if (c.enabled && lx >= c.position.x + viewport.x && ly >= c.position.y + viewport.y &&
            lx < c.position.x + viewport.x + c.size.x && ly < c.position.y + viewport.y + c.size.y) {
            if (Window *w = parentWindow())
                w->focus(&c);
            c.mouseDownInside = true;
            break;
        }
    }
    xOnMouseDown(x, y, button);
    for (auto &child : children_)
        if (child->enabled)
            child->onMouseDown(lx - viewport.x, ly - viewport.y, button);
}

void Panel::onMouseHover(int x, int y) {
    for (int i = int(children_.size()) - 1; i >= 0; --i) {
        Component &c = *children_[size_t(i)];
        int px = c.position.x + viewport.x, py = c.position.y + viewport.y;
        if (c.enabled && x >= px && y >= py && x < px + c.size.x && y < py + c.size.y) {
            c.onMouseHover(x - px, y - py);
            break;
        }
    }
}

void Panel::onMouseMoved(int x, int y) {
    xOnMouseMoved(x, y);
    for (auto &child : children_) {
        if (!child->enabled)
            continue;
        int lx = x - child->position.x - viewport.x, ly = y - child->position.y - viewport.y;
        child->onMouseMoved(lx, ly);
        bool in = lx >= 0 && ly >= 0 && lx < child->size.x && ly < child->size.y;
        if (in && !child->mouseInside) {
            child->mouseInside = true;
            child->onMouseEnter(lx, ly);
        } else if (!in && child->mouseInside) {
            child->mouseInside = false;
            child->onMouseLeave(lx, ly);
        }
    }
}

void Panel::propagateMouseMove() {
    Point p = screenPos();
    onMouseMoved(host().mouseX - p.x, host().mouseY - p.y);
}

void Panel::onMouseLeave(int x, int y) {
    for (auto &child : children_)
        if (child->mouseInside) {
            child->mouseInside = false;
            child->onMouseLeave(x - child->position.x - viewport.x, y - child->position.y - viewport.y);
        }
}

void Panel::onMouseUp(int x, int y, int button) {
    const int lx = x - position.x, ly = y - position.y;
    for (int i = int(children_.size()) - 1; i >= 0; --i) {
        Component &c = *children_[size_t(i)];
        if (c.enabled && c.mouseDownInside && lx >= c.position.x + viewport.x && ly >= c.position.y + viewport.y &&
            lx < c.position.x + viewport.x + c.size.x && ly < c.position.y + viewport.y + c.size.y) {
            c.onMouseClick(lx - c.position.x - viewport.x, ly - c.position.y - viewport.y, button);
            break;
        }
    }
    for (auto &child : children_)
        child->mouseDownInside = false;
    xOnMouseUp(x, y, button);
    for (auto &child : children_)
        if (child->enabled)
            child->onMouseUp(lx - viewport.x, ly - viewport.y, button);
}

void Panel::onMouseWheel(int, int, int) {}

void Panel::onMouseWheelInside(int x, int y, int d) { xOnMouseWheelInside(x, y, d); }

// ---- ScrollPanel ----

void ScrollPanel::xOnMouseWheelInside(int, int, int d) {
    if (d)
        velocity_ -= float(d) * 20.0f;
}

void ScrollPanel::xDraw(Canvas &c, Point p) {
    if (maxOffsetY_ > 0 && innerSize.y > 0) {
        float height = float(size.y) * (float(size.y) / float(innerSize.y));
        float pos = 0;
        if (-viewport.y > 0)
            pos = float(size.y - height) * (offsetY_ / float(maxOffsetY_));
        c.blendFilledRect(p.x + size.x - barWidth_, p.y, barWidth_, size.y, rgb(0x7D7D7D, 100));
        c.drawFilledRect(p.x + size.x - barWidth_, p.y + int(pos), barWidth_, int(height) + 1, 0xFFFFFF);
    }
}

void ScrollPanel::xOnMouseDown(int, int y, int) {
    if (!mouseDownInside)
        return;
    velocity_ = 0;
    if (insideBar_) {
        barSelected_ = true;
        barInitialOffset_ = int(offsetY_);
    }
    barInitialClick_ = y - position.y;
    barClickLocation_ = 100;
}

void ScrollPanel::xOnMouseUp(int, int, int) {
    barSelected_ = false;
    velocity_ = 0;
    insideBarArea_ = false;
    barClickLocation_ = 0;
}

void ScrollPanel::xOnMouseMoved(int x, int y) {
    if (maxOffsetY_ <= 0 || innerSize.y <= 0)
        return;
    int oldViewport = viewport.y;
    float height = float(size.y) * (float(size.y) / float(innerSize.y));
    float pos = 0;
    if (-viewport.y > 0)
        pos = float(size.y - height) * (offsetY_ / float(maxOffsetY_));
    if (barSelected_) {
        if (x > 0) {
            int scrollY = int(float(y - barInitialClick_) / float(size.y) * float(innerSize.y) + float(barInitialOffset_));
            viewport.y = -scrollY;
            offsetY_ = float(scrollY);
        } else {
            viewport.y = -barInitialOffset_;
            offsetY_ = float(barInitialOffset_);
        }
    }
    if (x > size.x - barWidth_ && x < size.x) {
        insideBar_ = float(y) > pos && float(y) < pos + height;
        insideBarArea_ = true;
    } else
        insideBar_ = insideBarArea_ = false;
    if (oldViewport != viewport.y)
        propagateMouseMove();
}

void ScrollPanel::xTick() {
    int oldViewport = viewport.y;
    double now = host().now;
    double diff = lastTick_ < 0 ? 0 : now - lastTick_;
    lastTick_ = now;
    maxOffsetY_ = std::max(0, innerSize.y - size.y);
    int oldOffset = int(offsetY_);
    offsetY_ += velocity_ * float(diff) * 0.06f;
    velocity_ = 0; // (no momentum scrolling, TPT's default)
    if (oldOffset != int(offsetY_)) {
        if (offsetY_ < 0)
            offsetY_ = 0;
        else if (offsetY_ > float(maxOffsetY_))
            offsetY_ = float(maxOffsetY_);
        viewport.y = -int(offsetY_);
    } else if (offsetY_ < 0) {
        offsetY_ = 0;
        viewport.y = 0;
    } else if (offsetY_ > float(maxOffsetY_)) {
        offsetY_ = float(maxOffsetY_);
        viewport.y = -int(offsetY_);
    }
    if (mouseInside && barWidth_ < 6)
        barWidth_++;
    else if (!mouseInside && barWidth_ > 0 && !barSelected_)
        barWidth_--;
    if (insideBarArea_ && barClickLocation_ && !barSelected_) {
        float height = float(size.y) * (float(size.y) / float(innerSize.y));
        float pos = 0;
        if (-viewport.y > 0)
            pos = float(size.y - height) * (offsetY_ / float(maxOffsetY_));
        if (float(barInitialClick_) <= pos)
            barClickLocation_ = -1;
        else if (float(barInitialClick_) >= pos + height)
            barClickLocation_ = 1;
        else
            barClickLocation_ = 0;
        offsetY_ += float(barClickLocation_) * height / 10;
        viewport.y -= int(float(barClickLocation_) * height / 10);
    }
    if (oldViewport != viewport.y)
        propagateMouseMove();
}

// ---- ToolButton ----

ToolButton::ToolButton(Point position, Point size, const std::u32string &text, const std::u32string &toolTip,
                       uint32_t colour, std::function<void(Canvas &, int, int)> drawTexture)
    : Button(position, size, text, toolTip), drawTexture_(std::move(drawTexture)) {
    setSelection(-1);
    appearance.borderActive = rgb(0xFF0000);
    appearance.backgroundInactive = rgb(colour);
    displayText_ = text_.substr(0, 7); // (long names are cut, not given "...")
    placeText(displayText_);
    drawn_ = true;
}

void ToolButton::setSelection(int slot) {
    selection_ = slot;
    static const uint32_t borders[4] = {0xFF0000, 0x0000FF, 0x00FF00, 0x00FFFF};
    appearance.borderInactive = rgb(slot >= 0 && slot < 4 ? borders[slot] : 0x000000);
}

void ToolButton::onMouseDown(int, int, int) {
    if (mouseDownInside)
        buttonDown_ = true;
}

void ToolButton::onMouseClick(int, int, int button) {
    if (!buttonDown_)
        return;
    buttonDown_ = false;
    if (button == 1)
        setSelection(0);
    if (button == 3)
        setSelection(1);
    if (button == 2)
        setSelection(2);
    doAction();
}

void ToolButton::onMouseUp(int, int, int) { buttonDown_ = false; }

void ToolButton::draw(Canvas &c, Point p) {
    const Rgba bg = appearance.backgroundInactive;
    int total = bg.b + 3 * bg.g + 2 * bg.r;
    if (drawTexture_)
        drawTexture_(c, p.x + 2, p.y + 2);
    else
        c.blendFilledRect(p.x + 2, p.y + 2, size.x - 4, size.y - 4, bg);
    c.blendRect(p.x, p.y, size.x, size.y,
                mouseInside && selection_ == -1 ? appearance.borderActive : appearance.borderInactive);
    if (favourite)
        c.blendChar(p.x, p.y, 0xE068, appearance.borderFavourite);
    if (!drawTexture_)
        c.blendText(p.x + textPos_.x, p.y + textPos_.y, displayText_, rgb(total < 544 ? 0xFFFFFF : 0x000000));
}

// ---- Window ----

void Window::addComponent(std::unique_ptr<Component> c) {
    if (c->window_)
        return;
    c->window_ = this;
    c->mouseInside = c->mouseDownInside = false;
    Component &r = *c;
    components_.push_back(std::move(c));
    if (auto *p = dynamic_cast<Panel *>(&r))
        for (auto &child : p->children_)
            child->window_ = this;
    const int mx = host().mouseX, my = host().mouseY;
    if (mx > position.x + r.position.x && mx < position.x + r.position.x + r.size.x && my > position.y + r.position.y &&
        my < position.y + r.position.y + r.size.y) {
        r.mouseInside = true;
        r.onMouseEnter(mx - (position.x + r.position.x), my - (position.y + r.position.y));
    }
}

void Window::removeComponent(Component *c) {
    if (focused_ == c)
        focused_ = nullptr;
    if (hover_ == c)
        hover_ = nullptr;
    std::erase_if(components_, [c](const std::unique_ptr<Component> &p) { return p.get() == c; });
}

void Window::focus(Component *c) {
    if (focused_ == c)
        return;
    if (focused_)
        focused_->onDefocus();
    focused_ = c;
    if (c)
        c->onFocus();
}

bool Window::wantsTextInput() const {
    return doesTextInput || (focused_ && focused_->visible && focused_->enabled && focused_->doesTextInput);
}

void Window::onTryExit(Exit how) {
    if (cancelButton && how != Exit::MouseOutside)
        cancelButton->doAction();
}

void Window::onTryOkay() {
    if (okayButton)
        okayButton->doAction();
}

void Window::doDraw(Canvas &c) {
    onDraw(c);
    auto drawChild = [&](Component *child) {
        if (child->visible)
            child->draw(c, Point{position.x + child->position.x, position.y + child->position.y});
    };
    for (auto &child : components_)
        if (child.get() != focused_ && child.get() != hover_)
            drawChild(child.get());
    // what the mouse is over and what has the focus are drawn last
    if (hover_)
        drawChild(hover_);
    if (focused_ && focused_ != hover_)
        drawChild(focused_);
}

void Window::doTick() {
    const int mx = host().mouseX, my = host().mouseY;
    for (int i = int(components_.size()) - 1; i >= 0; --i) {
        Component &c = *components_[size_t(i)];
        if (c.enabled && mx >= c.position.x + position.x && my >= c.position.y + position.y &&
            mx < c.position.x + position.x + c.size.x && my < c.position.y + position.y + c.size.y) {
            c.onMouseHover(mx - (c.position.x + position.x), my - (c.position.y + position.y));
            break;
        }
    }
    for (size_t i = 0; i < components_.size() && !closing_; ++i)
        components_[i]->tick();
    onTick();
}

void Window::doKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) {
    if (focused_ && focused_->enabled && focused_->visible)
        focused_->onKeyPress(key, scan, repeat, shift, ctrl, alt);
    if (closing_)
        return;
    onKeyPress(key, scan, repeat, shift, ctrl, alt);
    if (closing_)
        return;
    if (key == SDLK_ESCAPE || key == SDLK_AC_BACK)
        onTryExit(Exit::Escape);
    if (closing_)
        return;
    if (key == SDLK_KP_ENTER || key == SDLK_RETURN)
        onTryOkay();
}

void Window::doKeyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) {
    if (focused_ && focused_->enabled && focused_->visible)
        focused_->onKeyRelease(key, scan, repeat, shift, ctrl, alt);
    if (!closing_)
        onKeyRelease(key, scan, repeat, shift, ctrl, alt);
}

void Window::doTextInput(const std::u32string &text) {
    if (focused_ && focused_->enabled && focused_->visible)
        focused_->onTextInput(text);
}

void Window::doMouseDown(int mx, int my, int button) {
    const int x = mx - position.x, y = my - position.y;
    bool clicked = false;
    for (int i = int(components_.size()) - 1; i >= 0; --i) {
        Component &c = *components_[size_t(i)];
        if (c.enabled && c.visible && x >= c.position.x && y >= c.position.y && x < c.position.x + c.size.x &&
            y < c.position.y + c.size.y) {
            focus(&c);
            c.mouseDownInside = true;
            clicked = true;
            break;
        }
    }
    if (!clicked)
        focus(nullptr);
    for (int i = int(components_.size()) - 1; i >= 0 && !closing_; --i) {
        Component &c = *components_[size_t(i)];
        if (c.enabled && c.visible)
            c.onMouseDown(x, y, button);
    }
    if (closing_)
        return;
    onMouseDown(mx, my, button);
    if (!clicked && (mx < position.x || my < position.y || mx > position.x + size.x || my > position.y + size.y))
        onTryExit(Exit::MouseOutside);
}

void Window::doMouseMove(int mx, int my) {
    const int x = mx - position.x, y = my - position.y;
    for (int i = int(components_.size()) - 1; i >= 0 && !closing_; --i) {
        Component &c = *components_[size_t(i)];
        if (!c.enabled || !c.visible)
            continue;
        int lx = x - c.position.x, ly = y - c.position.y;
        c.onMouseMoved(lx, ly);
        if (lx >= 0 && ly >= 0 && lx < c.size.x && ly < c.size.y) {
            if (!c.mouseInside) {
                c.mouseInside = true;
                c.onMouseEnter(lx, ly);
            }
            hover_ = &c;
        } else if (c.mouseInside) {
            c.mouseInside = false;
            c.onMouseLeave(lx, ly);
        }
    }
    if (!closing_)
        onMouseMove(mx, my);
}

void Window::doMouseUp(int mx, int my, int button) {
    const int x = mx - position.x, y = my - position.y;
    for (int i = int(components_.size()) - 1; i >= 0 && !closing_; --i) {
        Component &c = *components_[size_t(i)];
        if (c.enabled && c.visible && c.mouseDownInside && x >= c.position.x && y >= c.position.y &&
            x < c.position.x + c.size.x && y < c.position.y + c.size.y) {
            c.onMouseClick(x - c.position.x, y - c.position.y, button);
            break;
        }
    }
    for (auto &c : components_)
        c->mouseDownInside = false;
    for (int i = int(components_.size()) - 1; i >= 0 && !closing_; --i) {
        Component &c = *components_[size_t(i)];
        if (c.enabled && c.visible)
            c.onMouseUp(x, y, button);
    }
    if (!closing_)
        onMouseUp(mx, my, button);
}

void Window::doMouseWheel(int mx, int my, int d) {
    const int x = mx - position.x, y = my - position.y;
    for (int i = int(components_.size()) - 1; i >= 0; --i) {
        Component &c = *components_[size_t(i)];
        if (x >= c.position.x && y >= c.position.y && x < c.position.x + c.size.x && y < c.position.y + c.size.y) {
            if (c.enabled && c.visible)
                c.onMouseWheelInside(x - c.position.x, y - c.position.y, d);
            break;
        }
    }
    for (int i = int(components_.size()) - 1; i >= 0 && !closing_; --i) {
        Component &c = *components_[size_t(i)];
        if (c.enabled && c.visible)
            c.onMouseWheel(x - c.position.x, y - c.position.y, d);
    }
    if (!closing_)
        onMouseWheel(mx, my, d);
}

// ---- Engine ----

void Engine::show(std::unique_ptr<Window> w, int screenW, int screenH) {
    if (w->position.x == -1)
        w->position.x = (screenW - w->size.x) / 2;
    if (w->position.y == -1)
        w->position.y = (screenH - w->size.y) / 2;
    w->size.x = std::min(w->size.x, screenW);
    w->size.y = std::min(w->size.y, screenH);
    w->position.x = std::clamp(w->position.x, 0, std::max(screenW, 0));
    w->position.y = std::clamp(w->position.y, 0, std::max(screenH, 0));
    windows_.push_back(std::move(w));
    shownMs_.push_back(0);
    ignoreEvents_ = true;
}

int Engine::collect() {
    int removed = 0;
    for (size_t i = 0; i < windows_.size(); i++)
        if (windows_[i]->closing()) {
            removed = int(windows_.size() - i);
            windows_.resize(i);
            shownMs_.resize(i);
            break;
        }
    if (removed) {
        ignoreEvents_ = true;
        if (Window *w = top())
            w->doMouseMove(host().mouseX, host().mouseY);
    }
    // windows that asked for one to open over them (a drop down's list)
    for (size_t i = 0; i < windows_.size(); i++)
        if (windows_[i]->openPending) {
            auto w = std::move(windows_[i]->openPending);
            windows_.push_back(std::move(w));
            shownMs_.push_back(0);
            ignoreEvents_ = true;
            break;
        }
    return removed;
}

uint8_t Engine::fadeAlpha() const {
    if (shownMs_.empty())
        return 0;
    int frames = std::min(20, int(shownMs_.back() * 60.0 / 1000.0)); // (TPT counts frames, at 60 a second)
    return uint8_t((1 - std::pow(0.98, frames)) * 0xFF);
}

void Engine::tick(double ms) {
    if (!shownMs_.empty())
        shownMs_.back() += ms;
    if (Window *w = top())
        w->doTick();
    ignoreEvents_ = false;
    collect();
}

void Engine::draw(Canvas &c) {
    if (Window *w = top())
        w->doDraw(c);
}

#define BP_ENGINE_EVENT(call)                                                                                          \
    if (ignoreEvents_ || !top())                                                                                       \
        return;                                                                                                        \
    top()->call;                                                                                                       \
    collect();

void Engine::mouseDown(int x, int y, int button) { BP_ENGINE_EVENT(doMouseDown(x, y, button)) }
void Engine::mouseUp(int x, int y, int button) { BP_ENGINE_EVENT(doMouseUp(x, y, button)) }
void Engine::mouseMove(int x, int y) { BP_ENGINE_EVENT(doMouseMove(x, y)) }
void Engine::mouseWheel(int x, int y, int d) { BP_ENGINE_EVENT(doMouseWheel(x, y, d)) }
void Engine::keyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) {
    BP_ENGINE_EVENT(doKeyPress(key, scan, repeat, shift, ctrl, alt))
}
void Engine::keyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) {
    BP_ENGINE_EVENT(doKeyRelease(key, scan, repeat, shift, ctrl, alt))
}
void Engine::textInput(const std::u32string &text) { BP_ENGINE_EVENT(doTextInput(text)) }

#undef BP_ENGINE_EVENT

Checkbox::Checkbox(Point position, Point size, const std::u32string &text, const std::u32string &toolTip)
    : Component(position, size), text_(text), toolTip_(toolTip) {}

void Checkbox::onMouseClick(int, int, int) {
    checked = !checked;
    if (action)
        action();
}

void Checkbox::onMouseHover(int, int) {
    if (!toolTip_.empty() && parentWindow())
        parentWindow()->toolTip(position, toolTip_);
}

void Checkbox::draw(Canvas &c, Point p) {
    if (checked)
        c.drawFilledRect(p.x + 5, p.y + 5, 6, 6, 0xFFFFFF);
    const uint8_t a = over_ ? 255 : 200;
    if (over_) {
        c.drawRect(p.x + 2, p.y + 2, 12, 12, 0xFFFFFF);
        c.blendFilledRect(p.x + 5, p.y + 5, 6, 6, rgb(0xFFFFFF, 170));
    } else
        c.blendRect(p.x + 2, p.y + 2, 12, 12, rgb(0xFFFFFF, 200));
    if (appearance.icon == Icon::None)
        c.blendText(p.x + 18, p.y + 4, text_, rgb(0xFFFFFF, a));
    else
        c.drawIcon(p.x + 16, p.y + 3, appearance.icon, a);
}

void Separator::draw(Canvas &c, Point p) { c.blendRect(p.x, p.y, size.x, size.y, rgb(0xFFFFFF, 180)); }

DirectionSelector::DirectionSelector(Point position, float scale, int radius, int handleRadius, int snapPointRadius,
                                     int snapPointEffectRadius)
    : Component(position, Point{radius * 5 / 2, radius * 5 / 2}), scale_(scale), radius_(radius),
      handleRadius_(handleRadius), snapRadius_(snapPointRadius), snapEffectRadius_(snapPointEffectRadius) {}

DirectionSelector::Value DirectionSelector::fromVector(float x, float y) const {
    return {Point{int(x / scale_), int(y / scale_)}, x, y};
}

DirectionSelector::Value DirectionSelector::fromPosition(Point p) const {
    const float length = std::hypot(float(p.x), float(p.y));
    if (length > float(radius_)) {
        p.x = int(float(p.x) / length * float(radius_));
        p.y = int(float(p.y) / length * float(radius_));
    }
    return {p, float(p.x) * scale_, float(p.y) * scale_};
}

void DirectionSelector::setSnapPoints(int effectRadius, int points, float maxMagnitude) {
    snapEffectRadius_ = effectRadius;
    snapPoints_.clear();
    snapPoints_.push_back(fromVector(0, 0));
    for (int i = 1; i < points; i++) {
        const float d = float(i) / float(points - 1) * maxMagnitude;
        snapPoints_.push_back(fromVector(d, 0));
        snapPoints_.push_back(fromVector(0, d));
        snapPoints_.push_back(fromVector(-d, 0));
        snapPoints_.push_back(fromVector(0, -d));
    }
    useSnap_ = true;
}

void DirectionSelector::setPosition(Point p) {
    value_ = fromPosition(p);
    if (useSnap_ && !alt_)
        for (const Value &s : snapPoints_)
            if (std::hypot(float(s.offset.x - p.x), float(s.offset.y - p.y)) <= float(snapEffectRadius_))
                value_ = s;
    if (update)
        update(value_.x, value_.y);
}

void DirectionSelector::setPositionAbs(Point p) {
    setPosition(Point{p.x - (radius_ + handleRadius_), p.y - (radius_ + handleRadius_)});
}

void DirectionSelector::setValues(float x, float y) {
    value_.x = x, value_.y = y;
    setPosition(fromVector(x, y).offset);
}

void DirectionSelector::checkHovering(int x, int y) {
    hover_ = std::hypot(float(value_.offset.x + radius_ - x), float(value_.offset.y + radius_ - y)) <
             float(handleRadius_);
}

void DirectionSelector::draw(Canvas &c, Point p) {
    const int track = radius_ + handleRadius_;
    const int cx = p.x + track, cy = p.y + track;
    c.blendFilledEllipse(cx, cy, track, track, Rgba{0, 0, 0, 63});
    c.blendEllipse(cx, cy, track, track, rgb(0xFFFFFF));
    for (const Value &s : snapPoints_) // (BlendFilledRect of RectBetween)
        c.blendFilledRect(cx + s.offset.x - snapRadius_, cy + s.offset.y - snapRadius_, 2 * snapRadius_ + 1,
                          2 * snapRadius_ + 1, Rgba{63, 63, 63, uint8_t(alt_ ? 127 / 2 : 127)});
    const uint8_t a = (hover_ || down_) ? uint8_t(std::min(int(127 * 1.5f), 255)) : 127;
    c.blendFilledEllipse(cx + value_.offset.x, cy + value_.offset.y, handleRadius_, handleRadius_, Rgba{63, 63, 63, a});
    c.blendEllipse(cx + value_.offset.x, cy + value_.offset.y, handleRadius_, handleRadius_, rgb(0xFFFFFF));
}

void DirectionSelector::onMouseMoved(int x, int y) {
    if (down_)
        setPositionAbs(Point{x, y});
    checkHovering(x, y);
}

void DirectionSelector::onMouseDown(int x, int y, int) {
    if (mouseDownInside) {
        down_ = true;
        setPositionAbs(Point{x - position.x, y - position.y});
        checkHovering(x - position.x, y - position.y);
    }
}

void DirectionSelector::onMouseUp(int x, int y, int) {
    down_ = false;
    checkHovering(x - position.x, y - position.y);
    if (change)
        change(value_.x, value_.y);
}

void DirectionSelector::onKeyPress(int, int, bool, bool, bool, bool alt) { alt_ = alt; }

void DirectionSelector::onKeyRelease(int, int, bool, bool, bool, bool alt) { alt_ = alt; }

} // namespace bp::ui
