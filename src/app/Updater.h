// Updates from the project's GitHub releases (TPT's update notification and updater, with the releases in place of
// TPT's server). Each release carries big-powder-<version>-linux-x86_64.tar.gz and big-powder.json (version, file,
// sha256, size). The tarball must come from the project's own release downloads and match the checksum; it is
// unpacked next to the program and put in place over it, keeping the old files until the next start.
#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace bp {

constexpr const char *RELEASES_PAGE = "https://github.com/ssjrocks/big-powder/releases";

struct UpdateInfo {
    std::string version, notes, file, url, sha256, releaseUrl;
    uint64_t size = 0;
};

// Below 0, 0 or above 0 as version a is older than, the same as or newer than b ("0.6.0", "v0.6.1")
int compareVersions(const std::string &a, const std::string &b);
// The latest release if it is newer than `current`; nullopt if not, or with `error` set if it couldn't tell
std::optional<UpdateInfo> checkForUpdate(const std::string &current, std::string &error);

// Whether the program in `dir` came from a release (its big-powder.json is there) and `dir` can be written
bool canUpdate(const std::filesystem::path &dir);

// installUpdate's progress, read from another thread
struct UpdateProgress {
    std::mutex m;
    std::string status;
    float progress = -1; // 0-1, or below 0 when it isn't known
    bool done = false, ok = false;
    std::string error;
};
// Downloads, checks, unpacks and puts the release in place in `dir` (blocking)
void installUpdate(const UpdateInfo &u, const std::filesystem::path &dir, UpdateProgress &p);
// What an earlier update left in `dir` (the old program and shaders), removed
void cleanUpUpdate(const std::filesystem::path &dir);

// The background check the viewer starts, and what it found
struct UpdateCheck {
    std::mutex m;
    bool ready = false;
    std::optional<UpdateInfo> info;
    std::string error;
};

} // namespace bp
