#include "app/History.h"

namespace bp {

void History::snapshot(World &w) {
    beforeRestore_.reset();
    // HistoryPush: what is past the position goes, the snapshot is added, and the oldest go past the limit
    while (position_ < entries_.size())
        entries_.pop_back();
    entries_.push_back(w.snapshot());
    position_ = entries_.size();
    while (limit < entries_.size()) {
        entries_.pop_front();
        position_--;
    }
}

bool History::restore(World &w) {
    if (position_ == 0)
        return false;
    if (!beforeRestore_)
        beforeRestore_ = w.snapshot();
    position_--;
    w.restore(*entries_[position_]);
    return true;
}

bool History::forward(World &w) {
    if (position_ >= entries_.size())
        return false;
    position_++;
    // past the last snapshot: back where ctrl+Z started, which is then let go
    std::shared_ptr<const WorldSnapshot> current = position_ == entries_.size() ? beforeRestore_ : entries_[position_];
    if (current)
        w.restore(*current);
    if (current == beforeRestore_)
        beforeRestore_.reset();
    return true;
}

void History::clear() {
    entries_.clear();
    position_ = 0;
    beforeRestore_.reset();
}

} // namespace bp
