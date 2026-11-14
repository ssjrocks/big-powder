#include "app/OnlineSaves.h"

#include "app/Http.h"
#include "classic/TptTools.h"

#include <json/json.h>

#include <ctime>

namespace bp {

namespace {

constexpr const char *SERVER = "https://powdertoy.co.uk", *STATICSERVER = "https://static.powdertoy.co.uk";

bool parse(const std::string &text, Json::Value &out, std::string &error) {
    Json::CharReaderBuilder rb;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &out, &error)) {
        error = "Could not read response: " + error;
        return false;
    }
    return true;
}

// The query string of a URL, as format::Url builds it
std::string url(const std::string &base, const std::vector<std::pair<std::string, std::string>> &params) {
    std::string out = base;
    bool first = true;
    for (const auto &[k, v] : params) {
        out += (first ? '?' : '&') + k + "=" + urlEncode(v);
        first = false;
    }
    return out;
}

// ByteString::FromUtf8, for the names, descriptions and comments the server sends
std::u32string text(const Json::Value &v) { return utf32(v.asString()); }

} // namespace

std::string onlineSearchUrl(const SaveQuery &q, int start, int count) {
    // SearchSavesRequest's Url: the date range and the order become search terms
    std::string query = utf8(q.text);
    auto append = [&](const std::string &s) {
        if (!query.empty())
            query += " ";
        query += s;
    };
    if (q.period) {
        time_t now = time(nullptr);
        static const int days[] = {0, 1, 7, 31, 365};
        now -= 60 * 60 * 24 * days[std::clamp(q.period, 0, 4)];
        struct tm t{};
        localtime_r(&now, &t);
        char b[64];
        std::snprintf(b, sizeof b, "after:%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        append(b);
    }
    if (q.byDate)
        append("sort:date");
    std::vector<std::pair<std::string, std::string>> params{{"Start", std::to_string(start)},
                                                            {"Count", std::to_string(count)}};
    if (!query.empty())
        params.push_back({"Search_Query", query});
    return url(std::string(SERVER) + "/Browse.json", params);
}

OnlineSaves::OnlineSaves() {
    for (int k = 0; k < 6; k++) // (thumbnails come 20 at a time)
        workers_.emplace_back([this] { work(); });
}

OnlineSaves::~OnlineSaves() {
    {
        std::lock_guard g(m_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    for (std::thread &t : workers_)
        t.join();
}

void OnlineSaves::enqueue(Owner owner, std::function<std::function<void()>()> run) {
    {
        std::lock_guard g(m_);
        jobs_.push_back({std::move(owner), std::move(run)});
    }
    cv_.notify_one();
}

void OnlineSaves::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock lk(m_);
            cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
            if (stop_)
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        if (job.owner.expired()) // (its window or button has gone: not asked for any more)
            continue;
        std::function<void()> answer = job.run();
        std::lock_guard g(m_);
        answers_.push_back({std::move(job.owner), std::move(answer)});
    }
}

void OnlineSaves::poll() {
    std::vector<std::pair<Owner, std::function<void()>>> answers;
    {
        std::lock_guard g(m_);
        answers.swap(answers_);
    }
    for (auto &[owner, answer] : answers)
        if (!owner.expired() && answer)
            answer();
}

std::string OnlineSaves::pageUrl(int id) const { // PreviewController::OpenInBrowser
    return std::string(SERVER) + "/Browse/View.html?ID=" + std::to_string(id);
}

void OnlineSaves::search(Owner owner, const SaveQuery &q, int start, int count,
                         std::function<void(bool, const std::string &, int, std::vector<OnlineSave>)> done) {
    const std::string u = onlineSearchUrl(q, start, count);
    enqueue(owner, [u, done] {
        std::string body, error;
        Json::Value result;
        std::vector<OnlineSave> saves;
        int total = 0;
        bool ok = httpGet(u, body, error) && parse(body, result, error);
        if (ok) {
            try {
                total = result["Count"].asInt();
                for (const Json::Value &s : result["Saves"]) {
                    OnlineSave o;
                    o.id = s["ID"].asInt();
                    o.created = s["Created"].asInt64();
                    o.updated = s["Updated"].asInt64();
                    o.votesUp = s["ScoreUp"].asInt();
                    o.votesDown = s["ScoreDown"].asInt();
                    o.username = s["Username"].asString();
                    o.name = text(s["Name"]);
                    o.version = s["Version"].asInt();
                    o.published = s["Published"].asBool();
                    o.comments = s["Comments"].asInt();
                    saves.push_back(std::move(o));
                }
            } catch (const std::exception &e) {
                ok = false;
                error = std::string("Could not read response: ") + e.what();
            }
        }
        return [=]() mutable { done(ok, error, total, std::move(saves)); };
    });
}

void OnlineSaves::tags(Owner owner, int count, std::function<void(bool, std::vector<std::pair<std::string, int>>)> done) {
    const std::string u = url(std::string(SERVER) + "/Browse/Tags.json", {{"Start", "0"}, {"Count", std::to_string(count)}});
    enqueue(owner, [u, done] {
        std::string body, error;
        Json::Value result;
        std::vector<std::pair<std::string, int>> tags;
        bool ok = httpGet(u, body, error) && parse(body, result, error);
        if (ok) {
            try {
                for (const Json::Value &t : result["Tags"])
                    tags.push_back({t["Tag"].asString(), t["Count"].asInt()});
            } catch (const std::exception &) {
                ok = false;
            }
        }
        return [=]() mutable { done(ok, std::move(tags)); };
    });
}

void OnlineSaves::info(Owner owner, int id, int version, std::function<void(bool, const std::string &, OnlineSave)> done) {
    std::vector<std::pair<std::string, std::string>> params{{"ID", std::to_string(id)}};
    if (version)
        params.push_back({"Date", std::to_string(version)});
    const std::string u = url(std::string(SERVER) + "/Browse/View.json", params);
    enqueue(owner, [u, done] {
        std::string body, error;
        Json::Value d;
        OnlineSave o;
        bool ok = httpGet(u, body, error) && parse(body, d, error);
        if (ok) {
            try {
                o.id = d["ID"].asInt();
                o.created = d["DateCreated"].asInt64();
                o.updated = d["Date"].asInt64();
                o.votesUp = d["ScoreUp"].asInt();
                o.votesDown = d["ScoreDown"].asInt();
                o.username = d["Username"].asString();
                o.name = text(d["Name"]);
                o.description = text(d["Description"]);
                o.published = d["Published"].asBool();
                for (const Json::Value &t : d["Tags"])
                    o.tags.push_back(t.asString());
                o.comments = d["Comments"].asInt();
                o.views = d["Views"].asInt();
                o.version = d["Version"].asInt();
            } catch (const std::exception &e) {
                ok = false;
                error = std::string("Could not read response: ") + e.what();
            }
        }
        return [=]() mutable { done(ok, error, std::move(o)); };
    });
}

void OnlineSaves::comments(Owner owner, int id, int start, int count,
                           std::function<void(bool, std::vector<OnlineComment>)> done) {
    const std::string u = url(std::string(SERVER) + "/Browse/Comments.json",
                              {{"ID", std::to_string(id)}, {"Start", std::to_string(start)}, {"Count", std::to_string(count)}});
    enqueue(owner, [u, done] {
        std::string body, error;
        Json::Value result;
        std::vector<OnlineComment> comments;
        bool ok = httpGet(u, body, error) && parse(body, result, error);
        if (ok) {
            try {
                for (const Json::Value &c : result) {
                    OnlineComment o;
                    o.username = c["Username"].asString();
                    const std::string elevation = c["Elevation"].asString();
                    o.staff = !elevation.empty() && elevation != "None";
                    o.banned = c["IsBanned"].asBool();
                    o.text = text(c["Text"]);
                    comments.push_back(std::move(o));
                }
            } catch (const std::exception &) {
                ok = false;
            }
        }
        return [=]() mutable { done(ok, std::move(comments)); };
    });
}

void OnlineSaves::thumbnail(Owner owner, int id, int version, int w, int h,
                            std::function<void(std::vector<uint32_t>, int, int)> done) {
    // ThumbnailRequest: the small one, then ImageRequest's resampling to the button's box
    const std::string u = std::string(STATICSERVER) + "/" + std::to_string(id) +
                          (version ? "_" + std::to_string(version) : "") + "_small.png";
    enqueue(owner, [u, w, h, done] {
        std::string body, error;
        httpGet(u, body, error);
        int tw = 0, th = 0;
        std::vector<uint32_t> px = tptImageFromPng(std::vector<char>(body.begin(), body.end()), w, h, tw, th);
        return [=]() mutable { done(std::move(px), tw, th); };
    });
}

void OnlineSaves::saveData(Owner owner, int id, int version,
                           std::function<void(bool, const std::string &, std::vector<char>)> done) {
    const std::string u = std::string(STATICSERVER) + "/" + std::to_string(id) +
                          (version ? "_" + std::to_string(version) : "") + ".cps";
    enqueue(owner, [u, done] {
        std::string body, error;
        const bool ok = httpGet(u, body, error);
        std::vector<char> data(body.begin(), body.end());
        return [=]() mutable { done(ok, error, std::move(data)); };
    });
}

} // namespace bp
