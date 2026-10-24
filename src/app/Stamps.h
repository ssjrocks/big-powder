// TPT's stamps (Client's AddStamp, GetStamp, DeleteStamp, RenameStamp, RescanStamps and MoveStampToFront): saves kept
// as .stm files in a folder, most recently used first, the order kept in its stamps.json as TPT keeps it. A stamp TPT
// can read is a TPT save; a bigger one (from a GPU world) is in our own format (classic/TptSave).
#pragma once
#include "classic/TptSave.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bp {

class Stamps {
public:
    explicit Stamps(std::string dir); // (the order read, then the folder rescanned)

    const std::vector<std::string> &names() const { return names_; }
    // AddStamp: written as a new stamp, named after the time, first in the order; its name ("" if it couldn't be)
    std::string add(const SaveArea &s, std::string &error);
    bool load(const std::string &name, SaveArea &out, std::string &error) const;
    void moveToFront(const std::string &name);
    void remove(const std::string &name);
    bool rename(const std::string &name, const std::string &newName, std::string &error);
    void rescan(); // RescanStamps: files gone are dropped from the order, new ones added at the end
    std::string path(const std::string &name) const;

private:
    void write(); // WriteStamps
    std::string dir_;
    std::vector<std::string> names_;
    uint64_t lastTime_ = 0; // (Client's lastStampTime and lastStampName)
    int lastName_ = 0;
};

} // namespace bp
