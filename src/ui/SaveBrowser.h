// TPT's online save browser (gui/search: SearchView, SearchModel and SearchController; gui/interface's SaveButton)
// and its save preview (gui/preview), read only: searching, sorting, the date range, pages, the popular tags, a save's
// details and comments, and opening it. What needs an account (logging in, voting, favourites, commenting, your own
// saves, reporting) shows disabled, as TPT shows it to someone who isn't logged in. The saves come from a SavesSource
// (app/OnlineSaves: TPT's servers), whose answers arrive between frames, so the interface never waits on the network.
#pragma once
#include "ui/Widgets.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bp {

// SaveInfo as the browser and the preview know it
struct OnlineSave {
    int id = 0, version = 0; // (version: the save's date, 0 for its latest; its thumbnail and data go by both)
    int64_t created = 0, updated = 0;
    int votesUp = 0, votesDown = 0, comments = 0, views = 0;
    bool published = true;
    std::string username;
    std::u32string name, description;
    std::vector<std::string> tags;
};

struct OnlineComment {
    std::string username;
    std::u32string text;
    bool staff = false, banned = false; // (User::Elevation above none; IsBanned)
};

// SearchModel's search: the page (from 1), the text, the date range (0 all, 1 day, 2 week, 3 month, 4 year), the order
struct SaveQuery {
    int page = 1;
    std::u32string text;
    int period = 0;
    bool byDate = false;
};

// Where the saves come from. Each request's answer runs on the interface's thread, from poll(); a request whose owner
// has gone (its weak_ptr expired) isn't made, or its answer is dropped.
class SavesSource {
public:
    virtual ~SavesSource() = default;
    using Owner = std::weak_ptr<void>;
    // Browse.json: one page of saves, and how many the search found
    virtual void search(Owner owner, const SaveQuery &q, int start, int count,
                        std::function<void(bool ok, const std::string &error, int total, std::vector<OnlineSave>)>) = 0;
    // Browse/Tags.json: the popular tags and how many saves have each
    virtual void tags(Owner owner, int count,
                      std::function<void(bool ok, std::vector<std::pair<std::string, int>>)>) = 0;
    // Browse/View.json: everything about one save
    virtual void info(Owner owner, int id, int version, std::function<void(bool ok, const std::string &error, OnlineSave)>) = 0;
    // Browse/Comments.json
    virtual void comments(Owner owner, int id, int start, int count,
                          std::function<void(bool ok, std::vector<OnlineComment>)>) = 0;
    // The save's small thumbnail, as ImageRequest resamples it to w x h (0xRRGGBB)
    virtual void thumbnail(Owner owner, int id, int version, int w, int h,
                           std::function<void(std::vector<uint32_t> pixels, int w, int h)>) = 0;
    // The save itself (a .cps)
    virtual void saveData(Owner owner, int id, int version,
                          std::function<void(bool ok, const std::string &error, std::vector<char>)>) = 0;
    virtual void poll() = 0;
    virtual std::string pageUrl(int id) const = 0; // the save's page on the website
};

// ui::SaveButton for a save on the server: its thumbnail with the vote bars beside it, its name and its author, the
// history icon when hovered, and its unpublished mark. A click opens it; on its author's name, that author's saves; on
// the history icon, its older versions.
class OnlineSaveButton : public ui::Component {
public:
    OnlineSaveButton(ui::Point position, ui::Point size, OnlineSave save, SavesSource &source);
    std::function<void()> action, history, author;
    const OnlineSave &save() const { return save_; }
    const std::vector<uint32_t> &thumbnail(int &w, int &h) const {
        w = thumbW_, h = thumbH_;
        return thumb_;
    }

    void tick() override;
    void draw(Canvas &c, ui::Point screenPos) override;
    void onMouseDown(int x, int y, int button) override;
    void onMouseClick(int x, int y, int button) override;
    void onMouseEnter(int, int) override { inside_ = true; }
    void onMouseLeave(int, int) override { inside_ = insideAuthor_ = insideHistory_ = false; }
    void onMouseMoved(int x, int y) override;

private:
    void thumbBox(int &w, int &h) const;
    OnlineSave save_;
    SavesSource &source_;
    std::u32string name_, votesBackground_, votesBackground2_, votes_;
    int barUp_ = 0, barDown_ = 0;
    std::vector<uint32_t> thumb_;
    int thumbW_ = 0, thumbH_ = 0;
    bool asked_ = false, wantsDraw_ = false, buttonDown_ = false;
    bool inside_ = false, insideAuthor_ = false, insideHistory_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// SearchView: the whole screen. Opening a save asks for its preview (onOpen; with ctrl held, TPT's instant open).
class SaveBrowserWindow : public ui::Window {
public:
    using Open = std::function<void(const OnlineSave &save, bool instant)>;
    SaveBrowserWindow(int w, int h, SavesSource &source, Open onOpen);

    void onTick() override;
    void onDraw(Canvas &c) override;
    void onMouseWheel(int x, int y, int d) override;
    void onKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void onKeyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void onTryExit(Exit) override {}
    void onTryOkay() override { doSearch(search_->text(), true); }
    // SearchView::Search: the text put in the box and searched for at once
    void searchFor(const std::u32string &text);
    const std::vector<OnlineSaveButton *> &saveButtons() const { return buttons_; }
    bool loading() const { return loading_; }
    int pageCount() const;
    int page() const { return query_.page; }

private:
    void doSearch(const std::u32string &text, bool now); // SearchController::DoSearch
    void update(int page);                               // SearchModel::UpdateSaveList
    void setPageRelative(int offset);
    void saveListChanged(); // NotifySaveListChanged, NotifyTagListChanged and NotifyPageChanged
    void sortChanged();
    void help();
    SavesSource &source_;
    Open open_;
    SaveQuery query_;
    int total_ = 0;
    bool includesFp_ = true, loading_ = false, instant_ = false;
    std::string error_;
    std::vector<OnlineSave> saves_;
    std::vector<std::pair<std::string, int>> tags_;
    bool tagsAsked_ = false;
    std::u32string nextQuery_;
    double nextQueryTime_ = 0;
    bool nextQueryDone_ = true, pageChanged_ = false;
    double pageChangedTime_ = 0;
    std::vector<OnlineSaveButton *> buttons_;
    std::vector<ui::Button *> tagButtons_;
    ui::Textbox *search_, *pageBox_;
    ui::Button *next_, *previous_, *sort_, *own_, *fav_;
    ui::DropDown *period_;
    ui::Label *pageLabel_, *pageCountLabel_, *tagsLabel_, *message_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0), searchToken_;
};

// PreviewView: the save drawn at half size (once its data has come), its name, author and date, views and votes, its
// description, and its comments on the right a page at a time. Open opens it (onOpen gets its data); Open in browser
// shows its page on the website.
class SavePreviewWindow : public ui::Window {
public:
    using Open = std::function<void(const OnlineSave &save, const std::vector<char> &data)>;
    using Render = std::function<std::vector<uint32_t>(const std::vector<char> &data, int &w, int &h)>;
    SavePreviewWindow(OnlineSave save, SavesSource &source, Render render, Open onOpen, bool instant,
                      std::function<void(const std::string &url)> openUrl);

    void onTick() override;
    void onDraw(Canvas &c) override;
    void doDraw(Canvas &c) override;
    void onMouseWheel(int x, int y, int d) override;
    void onMouseUp(int x, int y, int button) override;
    void onKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    bool infoLoaded() const { return infoLoaded_; }
    bool dataLoaded() const { return !data_.empty(); }
    int commentCount() const { return int(comments_.size()); }

private:
    void saveChanged();
    void updateComments(int page); // PreviewModel::UpdateComments
    void commentsChanged();
    void doOpen(); // PreviewController::DoOpen: opens it once both have come
    int commentsPageCount() const;
    OnlineSave save_;
    SavesSource &source_;
    Render render_;
    Open open_;
    bool instant_, infoLoaded_ = false, doOpen_ = false, prevPage_ = false, commentsLoaded_ = false;
    std::vector<char> data_;
    std::string error_;
    std::vector<uint32_t> preview_;
    int previewW_ = 0, previewH_ = 0, commentsPage_ = 1, xres_, yres_;
    std::vector<OnlineComment> comments_;
    std::vector<ui::Label *> commentTexts_;
    ui::ScrollPanel *commentsPanel_;
    ui::Label *nameLabel_, *authorLabel_, *viewsLabel_, *descriptionLabel_, *pageInfo_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace bp
