#include "ui/SaveBrowser.h"

#include "classic/TptTools.h"
#include "ui/Dialogs.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cmath>

namespace bp {

namespace {

constexpr int XRES = 612, YRES = 384; // (TPT's world, which its save buttons and preview are laid out by)
constexpr int SAVES_PER_PAGE = 20, COMMENTS_PER_PAGE = 20;

std::u32string number(long long v) { return utf32(std::to_string(v)); }

} // namespace

// ---- OnlineSaveButton: ui::SaveButton with a SaveInfo ----

OnlineSaveButton::OnlineSaveButton(ui::Point position, ui::Point size, OnlineSave save, SavesSource &source)
    : ui::Component(position, size), save_(std::move(save)), source_(source) {
    // the name, cut to fit with "..."; the vote count in TPT's icon font
    name_ = save_.name;
    if (const Canvas *font = ui::host().font) {
        int tw, th;
        font->textSize(name_, tw, th);
        if (tw - 1 > size.x) {
            const int fit = size.x - 22;
            std::u32string cut;
            for (char32_t ch : name_) {
                font->textSize(cut + ch, tw, th);
                if (tw - 1 > fit)
                    break;
                cut += ch;
            }
            name_ = cut + U"...";
        }
    }
    std::u32string votes = number(save_.votesUp - save_.votesDown), icon;
    icon += char32_t(0xE03B);
    for (size_t j = 1; j < votes.size(); j++)
        icon += char32_t(0xE03C);
    icon += char32_t(0xE039);
    icon += char32_t(0xE03A);
    votesBackground_ = icon;
    for (char32_t &ch : icon)
        ch -= 14; // 0xE039 -> 0xE02B
    votesBackground2_ = icon;
    for (char32_t &ch : votes)
        if (ch != U'-')
            ch += 0xDFFF; // 0x30 -> 0xE02F
    votes_ = votes;
    const int voteMax = std::max(save_.votesUp, save_.votesDown);
    if (voteMax) {
        if (voteMax < 34) {
            float ry = 33.0f / float(voteMax);
            if (voteMax < 8)
                ry = ry / float(8 - voteMax);
            barUp_ = int(float(save_.votesUp) * ry) - 1;
            barDown_ = int(float(save_.votesDown) * ry) - 1;
        } else {
            float ry = float(voteMax) / 33.0f;
            barUp_ = int(float(save_.votesUp) / ry) - 1;
            barDown_ = int(float(save_.votesDown) / ry) - 1;
        }
    }
}

void OnlineSaveButton::thumbBox(int &w, int &h) const {
    const float scale = float(size.y - 25) / float(YRES);
    w = int(float(XRES) * scale), h = int(float(YRES) * scale);
}

// SaveButton::Tick: the thumbnail is asked for once the button has been drawn
void OnlineSaveButton::tick() {
    if (asked_ || !wantsDraw_ || !save_.id)
        return;
    asked_ = true;
    int w, h;
    thumbBox(w, h);
    std::weak_ptr<int> alive = alive_;
    source_.thumbnail(alive_, save_.id, save_.version, w, h, [this, alive](std::vector<uint32_t> px, int tw, int th) {
        if (alive.expired())
            return;
        thumb_ = std::move(px);
        thumbW_ = tw, thumbH_ = th;
    });
}

void OnlineSaveButton::draw(Canvas &c, ui::Point p) {
    int bw, bh;
    thumbBox(bw, bh);
    wantsDraw_ = true;
    if (!thumb_.empty()) { // (centred in the space above the text, left of the vote bars)
        const int x = p.x + (size.x - bw) / 2 - 3, y = p.y + (size.y - 21 - bh) / 2;
        c.drawImage(x + (bw - thumbW_) / 2, y + (bh - thumbH_) / 2, thumbW_, thumbH_, thumb_);
    }
    const uint32_t frame = inside_ ? 0xD2E6FF : 0xB4B4B4;
    const int bx = p.x - 3 + (size.x - bw) / 2, by = p.y + (size.y - bh - 21) / 2;
    c.drawRect(bx, by, bw, bh, frame);
    c.drawRect(bx + bw - 1, by, 7, bh, frame);
    const int barX = p.x - 3 + bw + (size.x - bw) / 2;
    c.drawFilledRect(barX, p.y + 1 + (size.y - 20 - bh) / 2, 5, (bh + 1) / 2 - 1, 0x006B0A);
    c.drawFilledRect(barX, p.y + (size.y - 20) / 2, 5, bh / 2 - 1, 0x6B0A00);
    c.drawFilledRect(barX + 1, p.y + (size.y - 20) / 2 - barUp_, 3, barUp_, 0x39BB39);
    c.drawFilledRect(barX + 1, p.y + (size.y - 20) / 2, 3, barDown_, 0xBB3939);
    int tw, th;
    c.textSize(name_, tw, th);
    c.blendText(p.x + (size.x - (tw - 1)) / 2, p.y + size.y - 21, name_,
                rgb(inside_ && !insideAuthor_ ? 0xFFFFFF : 0xB4B4B4));
    const std::u32string user = utf32(save_.username);
    c.textSize(user, tw, th);
    c.blendText(p.x + (size.x - (tw - 1)) / 2, p.y + size.y - 10, user, rgb(insideAuthor_ ? 0xC8E6FF : 0x6482A0));
    if (inside_) { // the history icon
        const int y = p.y - 15 + (size.y - bh) / 2 + bh;
        c.drawFilledRect(p.x + 1, y + 1, 7, 8, 0xFFFFFF);
        c.blendText(p.x, y, std::u32string(1, char32_t(0xE026)), rgb(insideHistory_ ? 0xC86450 : 0xA04632));
    }
    if (!save_.published) {
        c.blendText(p.x, p.y - 2, std::u32string(1, char32_t(0xE04D)), rgb(0xFFFFFF));
        c.blendText(p.x, p.y - 2, std::u32string(1, char32_t(0xE04E)), rgb(0xD49751));
    }
}

void OnlineSaveButton::onMouseDown(int, int, int button) {
    if (mouseDownInside && button == 1)
        buttonDown_ = true;
}

// SaveButton::OnMouseClick: the left button only
void OnlineSaveButton::onMouseClick(int, int, int button) {
    if (button != 1 || !buttonDown_)
        return;
    buttonDown_ = false;
    if (insideHistory_) {
        if (history)
            history();
    } else if (insideAuthor_) {
        if (author)
            author();
    } else if (action)
        action();
}

void OnlineSaveButton::onMouseMoved(int x, int y) {
    insideAuthor_ = insideHistory_ = false;
    if (inside_) {
        insideAuthor_ = y > size.y - 11;
        insideHistory_ = y > size.y - 29 && y < size.y - 18 && x > 0 && x < 9;
    }
}

// ---- SaveBrowserWindow: SearchView ----

SaveBrowserWindow::SaveBrowserWindow(int w, int h, SavesSource &source, Open onOpen)
    : ui::Window(ui::Point{0, 0}, ui::Point{w, h}), source_(source), open_(std::move(onOpen)) {
    auto label = [&](ui::Point at, ui::Point sz, const std::u32string &text, ui::Appearance::HAlign align) {
        auto l = std::make_unique<ui::Label>(at, sz, text);
        l->appearance.hAlign = align;
        l->appearance.vAlign = ui::Appearance::VAlign::Middle;
        return add(std::move(l));
    };
    auto button = [&](ui::Point at, ui::Point sz, const std::u32string &text) {
        auto b = std::make_unique<ui::Button>(at, sz, text);
        b->appearance.hAlign = ui::Appearance::HAlign::Centre;
        b->appearance.vAlign = ui::Appearance::VAlign::Middle;
        return add(std::move(b));
    };
    next_ = button({w - 52, h - 18}, {50, 16}, U"Next \U0000E015");
    next_->appearance.hAlign = ui::Appearance::HAlign::Right;
    next_->action = [this] { setPageRelative(1); };
    next_->visible = false;
    previous_ = button({2, h - 18}, {50, 16}, U"\U0000E016 Prev");
    previous_->appearance.hAlign = ui::Appearance::HAlign::Left;
    previous_->action = [this] { setPageRelative(-1); };
    previous_->visible = false;
    tagsLabel_ = label({270, h - 18}, {w - 540, 16}, U"\boPopular Tags:", ui::Appearance::HAlign::Centre);
    tagsLabel_->visible = false;
    {
        auto pb = std::make_unique<ui::Textbox>(ui::Point{283, h - 18}, ui::Point{41, 16}, U"");
        pb->setInputType(ui::Textbox::Input::Number);
        pb->change = [this] { // SearchView::textChanged
            int num = 0;
            for (char32_t ch : pageBox_->text())
                if (ch >= U'0' && ch <= U'9')
                    num = std::min(num * 10 + int(ch - U'0'), 1 << 24);
            if (num > pageCount())
                pageBox_->setText(number(pageCount()));
            pageChanged_ = true;
            pageChangedTime_ = ui::host().now + 600;
        };
        pageBox_ = add(std::move(pb));
    }
    pageLabel_ = label({0, h - 18}, {30, 16}, U"Page", ui::Appearance::HAlign::Right);
    pageCountLabel_ = label({w / 2 + 6, h - 18}, {50, 16}, U"", ui::Appearance::HAlign::Left);
    {
        auto sf = std::make_unique<ui::Textbox>(ui::Point{60, 10}, ui::Point{w - 283, 17}, U"",
                                                U"[search, F1 for help]");
        sf->appearance.icon = Icon::Search;
        sf->appearance.hAlign = ui::Appearance::HAlign::Left;
        sf->appearance.vAlign = ui::Appearance::VAlign::Middle;
        sf->setLimit(100);
        sf->change = [this] { // SearchView::doSearch
            const std::u32string &t = search_->text();
            if (t.size() > 3 || t.empty())
                doSearch(t, false);
        };
        search_ = add(std::move(sf));
        focus(search_);
    }
    {
        auto d = std::make_unique<ui::DropDown>(ui::Point{w - 185, 10}, ui::Point{36, 17});
        for (auto [text, v] : {std::pair{U"All", 0}, {U"Day", 1}, {U"Week", 2}, {U"Month", 3}, {U"Year", 4}})
            d->addOption(text, v);
        d->setOption(0);
        d->change = [this] { // SearchController::ChangePeriod
            query_.period = period_->option().second;
            update(1);
        };
        period_ = add(std::move(d));
    }
    sort_ = button({w - 140, 10}, {61, 17}, U"Sort");
    sort_->setIcon(Icon::VoteSort);
    sort_->setTogglable(true);
    sort_->action = [this] { // SearchController::ChangeSort
        query_.byDate = !query_.byDate;
        sortChanged();
        update(1);
    };
    own_ = button({w - 70, 10}, {61, 17}, U"My Own");
    own_->setIcon(Icon::MyOwn);
    own_->setTogglable(true);
    own_->enabled = false; // (needs logging in)
    fav_ = button({search_->position.x + search_->size.x + 15, 10}, {17, 17}, U"");
    fav_->setIcon(Icon::Favourite);
    fav_->setTogglable(true);
    fav_->appearance.margin.left += 2;
    fav_->appearance.borderInactive = rgb(0xAAAAAA);
    fav_->enabled = false;
    {
        ui::Button *clear = button({search_->position.x + search_->size.x - 1, 10}, {17, 17}, U"");
        clear->setIcon(Icon::Close);
        clear->appearance.margin.left += 2;
        clear->appearance.margin.top += 2;
        clear->appearance.borderInactive = rgb(0xAAAAAA);
        clear->action = [this] { // clearSearch
            search_->setText(U"");
            doSearch(U"", true);
        };
    }
    label({10, 10}, {50, 16}, U"Search:", ui::Appearance::HAlign::Left);
    message_ = label({0, h / 2 - 6}, {w, 12}, U"", ui::Appearance::HAlign::Centre);
    sortChanged();
    update(1); // (SearchController's constructor)
}

int SaveBrowserWindow::pageCount() const { // SearchModel::GetPageCount: the front page is repeated
    const bool fp = includesFp_;
    return std::max(1, int(std::ceil(float(total_) / float(SAVES_PER_PAGE))) + (fp ? 1 : 0));
}

void SaveBrowserWindow::searchFor(const std::u32string &text) {
    search_->setText(text);
    doSearch(text, true);
}

void SaveBrowserWindow::doSearch(const std::u32string &text, bool now) {
    nextQuery_ = text;
    if (!now) {
        nextQueryTime_ = ui::host().now + 600;
        nextQueryDone_ = false;
    } else {
        nextQueryDone_ = true;
        query_.text = text;
        update(1);
    }
}

// SearchModel::UpdateSaveList: a new search; what an older one brings back is dropped
void SaveBrowserWindow::update(int page) {
    query_.page = page;
    error_.clear();
    saves_.clear();
    loading_ = true;
    includesFp_ = query_.period == 0 && !query_.byDate && query_.text.empty();
    searchToken_ = std::make_shared<int>(0);
    std::weak_ptr<int> token = searchToken_;
    source_.search(searchToken_, query_, (page - 1) * SAVES_PER_PAGE, SAVES_PER_PAGE,
                   [this, token](bool ok, const std::string &error, int total, std::vector<OnlineSave> saves) {
                       if (token.expired())
                           return;
                       loading_ = false;
                       if (ok) {
                           total_ = total;
                           saves_ = std::move(saves);
                       } else
                           error_ = error;
                       saveListChanged();
                   });
    if (includesFp_ && page == 1 && tags_.empty() && !tagsAsked_) {
        tagsAsked_ = true;
        std::weak_ptr<int> alive = alive_;
        source_.tags(alive_, 24, [this, alive](bool ok, std::vector<std::pair<std::string, int>> tags) {
            if (alive.expired() || !ok)
                return;
            tags_ = std::move(tags);
            saveListChanged();
        });
    }
    saveListChanged();
}

void SaveBrowserWindow::setPageRelative(int offset) {
    const int page = std::clamp(query_.page + offset, 1, pageCount());
    if (page != query_.page)
        update(page);
}

void SaveBrowserWindow::sortChanged() { // NotifySortChanged
    sort_->setToggleState(query_.byDate);
    sort_->setText(query_.byDate ? U"By date" : U"By votes");
    sort_->setIcon(query_.byDate ? Icon::DateSort : Icon::VoteSort);
}

void SaveBrowserWindow::saveListChanged() {
    for (OnlineSaveButton *b : buttons_)
        removeComponent(b);
    buttons_.clear();
    for (ui::Button *b : tagButtons_)
        removeComponent(b);
    tagButtons_.clear();
    const bool showTags = includesFp_ && query_.page == 1;
    // NotifyPageChanged
    if (saves_.empty())
        pageLabel_->visible = pageCountLabel_->visible = pageBox_->visible = false;
    else {
        const std::u32string info = U"of " + number(pageCount());
        pageCountLabel_->setText(info);
        int tw = 0, th = 0;
        if (ui::host().font)
            ui::host().font->textSize(info, tw, th);
        const int width = tw - 1;
        pageLabel_->position.x = size.x / 2 - width - 20;
        pageBox_->position.x = size.x / 2 - width + 11;
        pageBox_->size.x = width - 4;
        pageLabel_->visible = pageCountLabel_->visible = pageBox_->visible = true;
        if (!isFocused(pageBox_))
            pageBox_->setText(number(query_.page));
    }
    previous_->visible = query_.page > 1;
    next_->visible = query_.page < pageCount();
    // NotifySaveListChanged's message and NotifyTagListChanged's tags
    message_->visible = saves_.empty();
    message_->setText(loading_ ? U"Loading..." : !error_.empty() ? U"\bo" + utf32(error_) : U"\boNo saves found");
    int savesY = 4;
    const int padding = 1;
    int buttonYOffset = 28, areaH = size.y - buttonYOffset - 18;
    tagsLabel_->visible = false;
    if (showTags) {
        buttonYOffset += areaH / savesY - padding * 2;
        areaH = size.y - buttonYOffset - 18;
        savesY--;
        const int tagsX = 6, tagsY = 4, tagYOffset = 60;
        const int tagAreaH = (areaH / savesY - padding * 2) - (tagYOffset - 28) - 5;
        const int tagW = size.x / tagsX - padding * 2, tagH = tagAreaH / tagsY - padding * 2;
        tagsLabel_->visible = true;
        tagsLabel_->position.y = tagYOffset - 16;
        int tx = 0, ty = 0;
        for (const auto &[tag, count] : tags_) {
            if (tx == tagsX) {
                if (ty == tagsY - 1)
                    break;
                tx = 0;
                ty++;
            }
            const int maxVotes = tags_[0].second;
            const int alpha = maxVotes ? 127 + (128 * count) / maxVotes : 192;
            auto b = std::make_unique<ui::Button>(
                ui::Point{padding + tx * (tagW + padding * 2), tagYOffset + padding + ty * (tagH + padding * 2)},
                ui::Point{tagW, tagH}, utf32(tag));
            b->appearance.borderInactive = b->appearance.borderHover = b->appearance.borderActive = rgb(0x000000);
            b->appearance.backgroundHover = rgb(0x000000);
            b->appearance.textInactive = rgb(uint32_t(alpha) << 16 | uint32_t(alpha) << 8 | uint32_t(alpha));
            b->appearance.textHover =
                rgb(uint32_t(alpha * 5 / 6) << 16 | uint32_t(alpha * 5 / 6) << 8 | uint32_t(alpha));
            const std::u32string text = utf32(tag);
            b->action = [this, text] { searchFor(text); };
            tagButtons_.push_back(add(std::move(b)));
            tx++;
        }
    }
    const int savesX = 5;
    const int bw = size.x / savesX - padding * 2, bh = areaH / savesY - padding * 2;
    int sx = 0, sy = 0;
    for (const OnlineSave &s : saves_) {
        if (sx == savesX) {
            if (sy == savesY - 1)
                break;
            sx = 0;
            sy++;
        }
        auto b = std::make_unique<OnlineSaveButton>(
            ui::Point{padding * 2 + sx * (bw + padding * 2), buttonYOffset + padding + sy * (bh + padding * 2)},
            ui::Point{bw, bh}, s, source_);
        OnlineSaveButton *p = b.get();
        b->action = [this, p] { open_(p->save(), instant_); };
        b->history = [this, p] { searchFor(U"history:" + number(p->save().id)); };
        b->author = [this, p] { searchFor(U"user:" + utf32(p->save().username)); };
        buttons_.push_back(add(std::move(b)));
        sx++;
    }
}

void SaveBrowserWindow::onTick() {
    source_.poll();
    // SearchController::Update: the search typed, once typing has paused
    if (!nextQueryDone_ && nextQueryTime_ < ui::host().now) {
        nextQueryDone_ = true;
        query_.text = nextQuery_;
        update(1);
    }
    if (pageChanged_ && pageChangedTime_ < ui::host().now) {
        pageChanged_ = false;
        int num = 0;
        for (char32_t ch : pageBox_->text())
            if (ch >= U'0' && ch <= U'9')
                num = std::min(num * 10 + int(ch - U'0'), 1 << 24);
        if (num != query_.page && num > 0 && num <= pageCount())
            update(num);
    }
}

void SaveBrowserWindow::onDraw(Canvas &c) {
    c.drawFilledRect(position.x, position.y, size.x, size.y, 0x000000);
    if (loading_ && saves_.empty()) { // ui::Spinner
        const int bx = size.x / 2 - 12 + 12, by = size.y / 2 + 12 + 12, inner = 12, outer = 15;
        const double cv = std::floor(ui::host().now * 0.015) * 0.25;
        for (double t = 0.0; t < 6.0; t += 0.25) {
            const int g = int((t / 6) * 255);
            c.drawLine(int(bx + std::sin(cv + t) * inner), int(by + std::cos(cv + t) * inner),
                       int(bx + std::sin(cv + t) * outer), int(by + std::cos(cv + t) * outer),
                       uint32_t(g) << 16 | uint32_t(g) << 8 | uint32_t(g));
        }
    }
}

void SaveBrowserWindow::onMouseWheel(int, int, int d) {
    if (d)
        setPageRelative(-d);
}

void SaveBrowserWindow::onKeyPress(int key, int, bool repeat, bool, bool, bool) {
    if (repeat)
        return;
    if (key == int(SDLK_ESCAPE))
        close();
    else if (key == int(SDLK_LCTRL) || key == int(SDLK_RCTRL))
        instant_ = true;
    else if (key == int(SDLK_F1))
        help();
}

void SaveBrowserWindow::onKeyRelease(int key, int, bool repeat, bool, bool, bool) {
    if (!repeat && (key == int(SDLK_LCTRL) || key == int(SDLK_RCTRL)))
        instant_ = false;
}

void SaveBrowserWindow::help() { // SearchView::searchHelp (without what needs logging in)
    openPending = std::make_unique<ConfirmPrompt>(
        U"Search Help",
        U"Type in the search bar to begin automatically searching save titles and tags. Search terms are ORed "
        U"together.\n\n"
        U"Sorting: click the \bt\"By Votes\"\bw / \bt\"By Date\"\bw buttons to change the order saves are displayed "
        U"in\n"
        U"Date Range: Click the dropdown to the right of the search box to select the date range for your search\n\n"
        U"Special search terms:\n"
        U"\btid:#######\bw - search by save id\n"
        U"\bthistory:#######\bw - see previous versions for a save id\n"
        U"\btuser:XXXXXX\bw - search for saves by a specific user\n"
        U"\btbefore:YYYY-MM-DD\bw - all saves originally created before a certain date. Month and Day portions are "
        U"both optional\n"
        U"\btafter:YYYY-MM-DD\bw - all saves originally created after a certain date. Month and Day portions are both "
        U"optional\n\n"
        U"Advanced search:\n"
        U"Start a search with \bt~\bw to do an advanced search. This search works across save titles, descriptions, "
        U"usernames, and tags, rather than only save titles and tags. It also concatenates search terms with AND "
        U"instead of OR.\n"
        U"Use \bt|\bw to OR together search terms, for example \bg~bomb | nuke | explosive\bw\n"
        U"Use \bt!\bw to negate terms, for example \bg~city !destroyable !desert\bw\n"
        U"Use \bt\"\bw to create multi-word search terms, for example \bg~\"power plant\" uran | plut | polo\bw\n"
        U"Use \bt@title\bw to limit search to only save titles, for example \bg~@title subframe\bw\n"
        U"Use \bt@description\bw to limit search to only save descriptions, for example \bg~@description \"No "
        U"description provided\"\bw\n"
        U"Use \bt@user\bw to limit search to only specific users, for example \bg~@user 117n00b | Catelite | "
        U"Fluttershy @title laser\bw\n"
        U"Use \bt@tags\bw to limit search to just save tags, for example \bg~@tags resistcup @title printer | "
        U"@description spider before:2024-06\bw\n"
        U"Parenthesis can be used to further complicate your searches. For example: \bg~(@user MG99 @description "
        U"complete) | (@user goglesq @tags tutorial)\bw",
        nullptr, U"OK");
}

// ---- SavePreviewWindow: PreviewView, PreviewModel and PreviewController ----

SavePreviewWindow::SavePreviewWindow(OnlineSave save, SavesSource &source, Render render, Open onOpen, bool instant,
                                     std::function<void(const std::string &)> openUrl)
    : ui::Window(ui::Point{-1, -1}, ui::Point{XRES / 2 + 210, YRES / 2 + 150}), save_(std::move(save)),
      source_(source), render_(std::move(render)), open_(std::move(onOpen)), instant_(instant), xres_(XRES),
      yres_(YRES) {
    auto button = [&](ui::Point at, ui::Point sz, const std::u32string &text, Icon icon) {
        auto b = std::make_unique<ui::Button>(at, sz, text);
        b->appearance.hAlign = ui::Appearance::HAlign::Left;
        b->appearance.vAlign = ui::Appearance::VAlign::Middle;
        b->setIcon(icon);
        return add(std::move(b));
    };
    ui::Button *fav = button({50, size.y - 19}, {51, 19}, U"Fav", Icon::Favourite);
    fav->setTogglable(true);
    fav->enabled = false; // (needs logging in)
    button({100, size.y - 19}, {51, 19}, U"Report", Icon::Report)->enabled = false;
    ui::Button *open = button({0, size.y - 19}, {51, 19}, U"Open", Icon::Open);
    open->action = [this] { doOpen(); };
    ui::Button *web = button({XRES / 2 - 107, size.y - 19}, {108, 19}, U"Open in browser", Icon::Open);
    const std::string url = source_.pageUrl(save_.id);
    web->action = [openUrl, url] {
        if (openUrl)
            openUrl(url);
    };
    auto label = [&](ui::Point at, ui::Point sz, ui::Appearance::HAlign align) {
        auto l = std::make_unique<ui::Label>(at, sz, U"");
        l->appearance.hAlign = align;
        l->appearance.vAlign = ui::Appearance::VAlign::Middle;
        return add(std::move(l));
    };
    nameLabel_ = label({5, YRES / 2 + 4}, {300, 16}, ui::Appearance::HAlign::Left);
    {
        auto d = std::make_unique<ui::Label>(ui::Point{5, YRES / 2 + 4 + 15 + 19},
                                             ui::Point{XRES / 2 - 10, size.y - (YRES / 2 + 4 + 15 + 17) - 23}, U"");
        d->setMultiline(true);
        d->appearance.hAlign = ui::Appearance::HAlign::Left;
        d->appearance.vAlign = ui::Appearance::VAlign::Top;
        d->setTextColour(0xB4B4B4);
        descriptionLabel_ = add(std::move(d));
    }
    authorLabel_ = label({5, YRES / 2 + 4 + 15}, {220, 16}, ui::Appearance::HAlign::Left);
    viewsLabel_ = label({XRES / 2 - 88, YRES / 2 + 4 + 15}, {88, 16}, ui::Appearance::HAlign::Right);
    pageInfo_ = label({XRES / 2 + 85, size.y + 1}, {70, 16}, ui::Appearance::HAlign::Centre);
    pageInfo_->setText(U"Page 1 of 1");
    commentsPanel_ = add(std::make_unique<ui::ScrollPanel>(ui::Point{XRES / 2 + 1, 1},
                                                           ui::Point{size.x - XRES / 2 - 2, size.y - 20}));
    {
        // (TPT's comment box, which needs logging in)
        auto box = std::make_unique<ui::Label>(ui::Point{XRES / 2 + 4, size.y - 19}, ui::Point{size.x - XRES / 2 - 8, 17},
                                               U"\bgLog in to comment");
        box->appearance.hAlign = ui::Appearance::HAlign::Left;
        box->appearance.vAlign = ui::Appearance::VAlign::Middle;
        add(std::move(box));
    }
    // PreviewView::AttachController: under the window, the save's ID in a box that copies it (ui::CopyTextButton)
    {
        auto width = [](const std::u32string &t) {
            int tw = 0, th = 0;
            if (ui::host().font)
                ui::host().font->textSize(t, tw, th);
            return tw - 1;
        };
        const std::u32string hint = U"Click the box below to copy the save ID", id = number(save_.id);
        ui::Label *copied = label({(size.x - width(hint) - 20) / 2, size.y + 5}, {width(hint) + 20, 16},
                                  ui::Appearance::HAlign::Centre);
        copied->setText(hint);
        copied->setTextColour(0x969696);
        label({(size.x - width(id) - 20) / 2 - 37, size.y + 22}, {40, 16}, ui::Appearance::HAlign::Centre)
            ->setText(U"Save ID:");
        auto b = std::make_unique<ui::Button>(ui::Point{(size.x - width(id) - 10) / 2, size.y + 20},
                                              ui::Point{width(id) + 10, 18}, id);
        b->appearance.textInactive = rgb(0x969696);
        b->appearance.textActive = b->appearance.borderActive = rgb(0xE6FFE6);
        ui::Button *box = b.get();
        b->action = [box, copied, id] {
            if (ui::host().clipboardSet)
                ui::host().clipboardSet(id);
            copied->setText(U"Copied!");
            box->appearance.textInactive = box->appearance.textHover = rgb(0xB4E6B4);
            box->appearance.borderInactive = box->appearance.borderHover = rgb(0xB4E6B4);
        };
        add(std::move(b));
    }
    // PreviewModel's constructor: the save's details, its data and its first page of comments
    std::weak_ptr<int> alive = alive_;
    source_.info(alive_, save_.id, save_.version, [this, alive](bool ok, const std::string &error, OnlineSave s) {
        if (alive.expired())
            return;
        if (!ok) {
            error_ = error.empty() ? "could not load the save's details" : error;
            return;
        }
        s.version = save_.version;
        save_ = std::move(s);
        infoLoaded_ = true;
        saveChanged();
        if (commentsLoaded_ || comments_.empty())
            updateComments(commentsPage_);
        if (doOpen_ && !data_.empty())
            doOpen();
    });
    source_.saveData(alive_, save_.id, save_.version,
                     [this, alive](bool ok, const std::string &error, std::vector<char> data) {
                         if (alive.expired())
                             return;
                         if (!ok || data.empty()) {
                             error_ = error.empty() ? "could not download the save" : error;
                             return;
                         }
                         data_ = std::move(data);
                         if (render_)
                             preview_ = render_(data_, previewW_, previewH_);
                         if (doOpen_ && infoLoaded_)
                             doOpen();
                     });
    commentsLoaded_ = true;
    if (instant_)
        doOpen();
}

void SavePreviewWindow::doOpen() {
    doOpen_ = true;
    if (infoLoaded_ && !data_.empty()) {
        close();
        if (open_)
            open_(save_, data_);
    }
}

void SavePreviewWindow::saveChanged() { // NotifySaveChanged
    nameLabel_->setText(save_.name);
    const std::u32string dateType = save_.updated == save_.created ? U"Created:" : U"Updated:";
    authorLabel_->setText(U"\bgAuthor:\bw " + utf32(save_.username) + U" \bg" + dateType + U" \bw" +
                          utf32(tptDateMini(save_.updated)));
    viewsLabel_->setText(U"\bgViews:\bw " + number(save_.views));
    descriptionLabel_->setText(save_.description);
}

int SavePreviewWindow::commentsPageCount() const {
    return std::max(1, (save_.comments + COMMENTS_PER_PAGE - 1) / COMMENTS_PER_PAGE);
}

void SavePreviewWindow::updateComments(int page) {
    if (!commentsLoaded_)
        return;
    commentsLoaded_ = false;
    commentsPage_ = page;
    pageInfo_->setText(U"Page " + number(commentsPage_) + U" of " + number(commentsPageCount()));
    std::weak_ptr<int> alive = alive_;
    source_.comments(alive_, save_.id, (page - 1) * COMMENTS_PER_PAGE, COMMENTS_PER_PAGE,
                     [this, alive](bool ok, std::vector<OnlineComment> comments) {
                         if (alive.expired())
                             return;
                         commentsLoaded_ = true;
                         comments_ = ok ? std::move(comments) : std::vector<OnlineComment>{};
                         commentsChanged();
                     });
}

void SavePreviewWindow::commentsChanged() { // NotifyCommentsChanged
    commentsPanel_->clear();
    commentTexts_.clear();
    int y = 0;
    const int w = size.x - (XRES / 2 + 13);
    for (const OnlineComment &cm : comments_) {
        std::u32string name = utf32(cm.username);
        if (cm.staff || cm.username == "jacobot")
            name = U"\bt" + name;
        else if (cm.banned)
            name = U"\bg" + name;
        else if (cm.username == save_.username)
            name = U"\bl" + name;
        auto user = std::make_unique<ui::Label>(ui::Point{5, y + 8}, ui::Point{w, 14}, name);
        user->appearance.hAlign = ui::Appearance::HAlign::Left;
        user->appearance.vAlign = ui::Appearance::VAlign::Middle;
        commentsPanel_->add(std::move(user));
        y += 16;
        auto text = std::make_unique<ui::Label>(ui::Point{5, y + 5}, ui::Point{w, -1}, U"");
        text->setMultiline(true);
        text->appearance.hAlign = ui::Appearance::HAlign::Left;
        text->appearance.vAlign = ui::Appearance::VAlign::Top;
        text->setTextColour(0xB4B4B4);
        text->setText(cm.text);
        ui::Label *t = commentsPanel_->add(std::move(text));
        y += t->size.y + 4;
        commentTexts_.push_back(t);
    }
    commentsPanel_->innerSize = ui::Point{commentsPanel_->size.x, y + 4};
    if (prevPage_) {
        prevPage_ = false;
        commentsPanel_->setScrollPosition(std::max(0, y + 4 - commentsPanel_->size.y));
    }
}

void SavePreviewWindow::onTick() { source_.poll(); }

void SavePreviewWindow::doDraw(Canvas &c) {
    ui::Window::doDraw(c);
    // the lines between the comments
    for (ui::Label *t : commentTexts_) {
        const int line = t->position.y + commentsPanel_->viewport.y + t->size.y + 4;
        if (line > 0 && line < size.y - 20)
            c.blendLine(position.x + 1 + XRES / 2, position.y + line, position.x + size.x - 2, position.y + line,
                        rgb(0xFFFFFF, 100));
    }
    if (doOpen_ && error_.empty()) {
        c.blendFilledRect(position.x + size.x / 2 - 101, position.y + size.y / 2 - 26, 202, 52, rgb(0x000000, 210));
        c.blendRect(position.x + size.x / 2 - 100, position.y + size.y / 2 - 25, 200, 50, rgb(0xFFFFFF, 180));
        int tw, th;
        c.textSize(U"Loading save...", tw, th);
        c.blendText(position.x + size.x / 2 - (tw - 1) / 2, position.y + size.y / 2 - 5, U"Loading save...",
                    rgb(ui::style::InformationTitle));
    }
    if (!error_.empty()) {
        const std::u32string e = U"\bo" + utf32(error_);
        int tw, th;
        c.textSize(e, tw, th);
        c.blendText(position.x + (XRES / 2 - tw) / 2, position.y + YRES / 4 - 4, e, rgb(0xFFFFFF));
    }
}

void SavePreviewWindow::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    if (!preview_.empty())
        c.drawImage(position.x + 1 + (XRES / 2 - previewW_) / 2, position.y + 1 + (YRES / 2 - previewH_) / 2, previewW_,
                    previewH_, preview_);
    c.blendRect(position.x, position.y, XRES / 2 + 1, YRES / 2 + 1, rgb(0xFFFFFF, 100));
    c.drawLine(position.x + XRES / 2, position.y + 1, position.x + XRES / 2, position.y + size.y - 2, 0xC8C8C8);
    const int up = save_.votesUp, down = save_.votesDown;
    if (up || down) {
        float ryf;
        int nyu, nyd;
        const int lv = std::max(std::max(up, down), 10);
        if (50 > lv) {
            ryf = 50.0f / float(lv);
            nyu = int(float(up) * ryf), nyd = int(float(down) * ryf);
        } else {
            ryf = float(lv) / 50.0f;
            nyu = int(float(up) / ryf), nyd = int(float(down) / ryf);
        }
        nyu = std::min(nyu, 50), nyd = std::min(nyd, 50);
        const int x = position.x + XRES / 2, y = position.y + YRES / 2;
        c.drawFilledRect(x - 56, y + 3, 54, 7, 0x006B0A);
        c.drawFilledRect(x - 56, y + 9, 54, 7, 0x6B0A00);
        c.drawRect(x - 56, y + 3, 54, 7, 0x808080);
        c.drawRect(x - 56, y + 9, 54, 7, 0x808080);
        c.drawFilledRect(x - 4 - nyu, y + 5, nyu, 3, 0x39BB39);
        c.drawFilledRect(x - 4 - nyd, y + 11, nyd, 3, 0xBB3939);
    }
}

// The comments go a page at a time: the wheel (or a click right of them) past their end or start
void SavePreviewWindow::onMouseWheel(int, int, int d) {
    if (commentsPanel_->scrollLimit() == 1 && d < 0 && commentsPage_ < commentsPageCount())
        updateComments(commentsPage_ + 1);
    if (commentsPanel_->scrollLimit() == -1 && d > 0 && commentsPage_ > 1) {
        prevPage_ = true;
        updateComments(commentsPage_ - 1);
    }
}

void SavePreviewWindow::onMouseUp(int x, int, int) {
    if (x > position.x + commentsPanel_->position.x + commentsPanel_->size.x - 6) {
        if (commentsPanel_->scrollLimit() == 1 && commentsPage_ < commentsPageCount())
            updateComments(commentsPage_ + 1);
        else if (commentsPanel_->scrollLimit() == -1 && commentsPage_ > 1) {
            prevPage_ = true;
            updateComments(commentsPage_ - 1);
        }
    }
}

void SavePreviewWindow::onKeyPress(int key, int, bool repeat, bool, bool, bool) {
    if (!repeat && (key == int(SDLK_RETURN) || key == int(SDLK_KP_ENTER)))
        doOpen();
}

} // namespace bp
