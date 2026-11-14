// The save browser's source: The Powder Toy's servers (TPT's client/http requests: SearchSavesRequest,
// SearchTagsRequest, GetSaveRequest, GetCommentsRequest, ThumbnailRequest and GetSaveDataRequest), read only. A few
// threads do the requests; their answers wait until poll() runs them on the interface's thread.
#pragma once
#include "ui/SaveBrowser.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace bp {

class OnlineSaves : public SavesSource {
public:
    OnlineSaves();
    ~OnlineSaves() override;

    void search(Owner owner, const SaveQuery &q, int start, int count,
                std::function<void(bool, const std::string &, int, std::vector<OnlineSave>)> done) override;
    void tags(Owner owner, int count, std::function<void(bool, std::vector<std::pair<std::string, int>>)> done) override;
    void info(Owner owner, int id, int version, std::function<void(bool, const std::string &, OnlineSave)> done) override;
    void comments(Owner owner, int id, int start, int count,
                  std::function<void(bool, std::vector<OnlineComment>)> done) override;
    void thumbnail(Owner owner, int id, int version, int w, int h,
                   std::function<void(std::vector<uint32_t>, int, int)> done) override;
    void saveData(Owner owner, int id, int version,
                  std::function<void(bool, const std::string &, std::vector<char>)> done) override;
    void poll() override;
    std::string pageUrl(int id) const override;

private:
    // A request runs on a worker and gives back what to run on the interface's thread
    struct Job {
        Owner owner;
        std::function<std::function<void()>()> run;
    };
    void enqueue(Owner owner, std::function<std::function<void()>()> run);
    void work();
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<std::pair<Owner, std::function<void()>>> answers_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

// SearchSavesRequest's URL for a search (exposed for the checks)
std::string onlineSearchUrl(const SaveQuery &q, int start, int count);

} // namespace bp
