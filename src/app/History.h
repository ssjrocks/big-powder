// TPT's undo history (GameModel::HistoryPush, HistoryRestore and HistoryForward, with GameController's beforeRestore):
// snapshots of the world taken before each change, the oldest dropped past the limit. TPT keeps all but the newest as
// differences to save memory; these are whole snapshots (compressed on the GPU), which behave the same.
#pragma once
#include "app/World.h"

#include <deque>
#include <memory>

namespace bp {

class History {
public:
    unsigned limit = 5; // Simulation.UndoHistoryLimit

    // GameController::HistorySnapshot: the world as it is now goes on the history, and whatever ctrl+Z went back
    // from is forgotten
    void snapshot(World &w);
    // ctrl+Z: back one snapshot (the first time since the last HistorySnapshot, the world as it is is kept for ctrl+Y)
    bool restore(World &w);
    // ctrl+Y and ctrl+shift+Z: forward one, to where ctrl+Z started last
    bool forward(World &w);
    void clear(); // (for another world)

    size_t size() const { return entries_.size(); }
    size_t position() const { return position_; }

private:
    std::deque<std::shared_ptr<const WorldSnapshot>> entries_;
    size_t position_ = 0;
    std::shared_ptr<const WorldSnapshot> beforeRestore_;
};

} // namespace bp
