#include "app/Updater.h"
#include "app/Http.h"

#include <json/json.h>
#include <openssl/evp.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

extern char **environ;

namespace bp {

namespace fs = std::filesystem;

namespace {

constexpr const char *API = "https://api.github.com/repos/ssjrocks/big-powder/releases/latest";
constexpr const char *DOWNLOADS = "https://github.com/ssjrocks/big-powder/releases/download/";
// What a release puts next to the program, all replaced by an update
const char *const INSTALLED[] = {"big-powder", "shaders", "big-powder.json", "README.md", "LICENSE"};

std::string sha256(const std::string &data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_Digest(data.data(), data.size(), md, &n, EVP_sha256(), nullptr);
    std::string out;
    char b[3];
    for (unsigned k = 0; k < n; k++) {
        std::snprintf(b, sizeof b, "%02x", md[k]);
        out += b;
    }
    return out;
}

bool parse(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder rb;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    std::string errs;
    return reader->parse(text.data(), text.data() + text.size(), &out, &errs);
}

// Runs a program (looked up on PATH) and waits; true if it exited with 0
bool run(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    for (const std::string &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
        return false;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

int compareVersions(const std::string &a, const std::string &b) {
    auto parts = [](std::string v) {
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
            v.erase(0, 1);
        std::vector<long> out;
        std::istringstream in(v);
        std::string p;
        while (std::getline(in, p, '.'))
            out.push_back(std::strtol(p.c_str(), nullptr, 10));
        while (out.size() < 3)
            out.push_back(0);
        return out;
    };
    std::vector<long> x = parts(a), y = parts(b);
    for (size_t k = 0; k < std::max(x.size(), y.size()); k++) {
        long p = k < x.size() ? x[k] : 0, q = k < y.size() ? y[k] : 0;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}

std::optional<UpdateInfo> checkForUpdate(const std::string &current, std::string &error) {
    std::string text;
    Json::Value release;
    if (!httpGet(API, text, error, HttpGet{true, nullptr}))
        return std::nullopt;
    if (!parse(text, release) || !release.isObject()) {
        error = "the releases page wasn't readable";
        return std::nullopt;
    }
    auto asset = [&](const std::string &name) -> std::string {
        for (const Json::Value &a : release["assets"])
            if (a["name"].asString() == name)
                return a["browser_download_url"].asString();
        return "";
    };
    const std::string metaUrl = asset("big-powder.json");
    if (metaUrl.rfind(DOWNLOADS, 0) != 0) {
        error = "the latest release has no big-powder.json";
        return std::nullopt;
    }
    std::string metaText;
    Json::Value meta;
    if (!httpGet(metaUrl, metaText, error))
        return std::nullopt;
    if (!parse(metaText, meta) || !meta.isObject()) {
        error = "the release's big-powder.json wasn't readable";
        return std::nullopt;
    }
    UpdateInfo u;
    u.version = meta["version"].asString();
    u.file = meta["file"].asString();
    u.sha256 = meta["sha256"].asString();
    u.size = meta["size"].asUInt64();
    u.url = asset(u.file);
    u.notes = release["body"].asString();
    u.releaseUrl = release["html_url"].asString();
    if (u.version.empty() || u.file.empty() || u.sha256.size() != 64 || u.url.rfind(DOWNLOADS, 0) != 0) {
        error = "the latest release doesn't say what to download";
        return std::nullopt;
    }
    if (compareVersions(u.version, current) <= 0)
        return std::nullopt;
    return u;
}

bool canUpdate(const fs::path &dir) {
    std::error_code ec;
    return fs::exists(dir / "big-powder.json", ec) && fs::exists(dir / "big-powder", ec) &&
           access(dir.c_str(), W_OK) == 0;
}

void installUpdate(const UpdateInfo &u, const fs::path &dir, UpdateProgress &p) {
    auto status = [&](const std::string &s, float progress) {
        std::lock_guard g(p.m);
        p.status = s;
        p.progress = progress;
    };
    auto fail = [&](const std::string &e) {
        std::lock_guard g(p.m);
        p.error = e;
        p.done = true;
    };
    std::error_code ec;
    const fs::path work = dir / ".update";
    fs::remove_all(work, ec);
    fs::create_directories(work / "unpacked", ec);

    // UpdateActivity's steps: downloading, unpacking, applying
    status("Downloading update", 0);
    std::string data, error;
    HttpGet how;
    how.progress = [&](double now, double total) {
        if (total > 0)
            status("Downloading update", float(now / total));
    };
    if (!httpGet(u.url, data, error, how))
        return fail("Could not download update: " + error);
    if (data.size() != u.size || sha256(data) != u.sha256)
        return fail("Downloaded update is corrupted\nIts checksum doesn't match the release's.");

    status("Unpacking update", -1);
    const fs::path tarball = work / u.file;
    {
        std::ofstream out(tarball, std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(data.size()));
        if (!out)
            return fail("Could not write " + tarball.string());
    }
    if (!run({"tar", "-xzf", tarball.string(), "-C", (work / "unpacked").string(), "--no-same-owner"}))
        return fail("Unable to unpack update (tar failed)");
    // the tarball holds one folder, big-powder-<version>
    fs::path from;
    for (const auto &e : fs::directory_iterator(work / "unpacked", ec))
        if (e.is_directory())
            from = e.path();
    if (from.empty() || !fs::exists(from / "big-powder", ec) || !fs::is_directory(from / "shaders", ec))
        return fail("Invalid update format");

    status("Applying update", -1);
    // each installed file to <name>.old, the new one in its place; back as it was if any of it fails
    std::vector<std::string> moved, placed;
    auto undo = [&] {
        for (const std::string &n : placed)
            fs::remove_all(dir / n, ec);
        for (const std::string &n : moved)
            fs::rename(dir / (n + ".old"), dir / n, ec);
    };
    for (const char *name : INSTALLED) {
        if (!fs::exists(from / name, ec))
            continue;
        fs::remove_all(dir / (std::string(name) + ".old"), ec);
        if (fs::exists(dir / name, ec)) {
            fs::rename(dir / name, dir / (std::string(name) + ".old"), ec);
            if (ec) {
                undo();
                return fail("Update failed - try downloading a new version.\n" + ec.message());
            }
            moved.push_back(name);
        }
        fs::rename(from / name, dir / name, ec);
        if (ec) {
            undo();
            return fail("Update failed - try downloading a new version.\n" + ec.message());
        }
        placed.push_back(name);
    }
    fs::remove_all(work, ec);
    std::lock_guard g(p.m);
    p.status = "Update installed";
    p.ok = p.done = true;
}

void cleanUpUpdate(const fs::path &dir) {
    std::error_code ec;
    fs::remove_all(dir / ".update", ec);
    for (const char *name : INSTALLED)
        fs::remove_all(dir / (std::string(name) + ".old"), ec);
}

} // namespace bp
