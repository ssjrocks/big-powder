#include "app/Stamps.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace bp {

namespace fs = std::filesystem;

namespace {

std::vector<char> readAll(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The strings in stamps.json's "MostRecentlyUsedFirst" (as TPT's Prefs writes it)
std::vector<std::string> readOrder(const std::string &json) {
    std::vector<std::string> out;
    size_t at = json.find("\"MostRecentlyUsedFirst\"");
    if (at == std::string::npos)
        return out;
    at = json.find('[', at);
    size_t end = at == std::string::npos ? at : json.find(']', at);
    if (end == std::string::npos)
        return out;
    for (size_t i = at; i < end; i++) {
        if (json[i] != '"')
            continue;
        std::string s;
        for (i++; i < end && json[i] != '"'; i++) {
            if (json[i] == '\\' && i + 1 < end)
                i++;
            s += json[i];
        }
        out.push_back(s);
    }
    return out;
}

std::string jsonString(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

} // namespace

Stamps::Stamps(std::string dir) : dir_(std::move(dir)) {
    std::vector<char> json = readAll(dir_ + "/stamps.json");
    names_ = readOrder(std::string(json.begin(), json.end()));
    rescan();
}

std::string Stamps::path(const std::string &name) const { return dir_ + "/" + name + ".stm"; }

void Stamps::write() {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    std::string json = "{\n\t\"MostRecentlyUsedFirst\" : \n\t[";
    for (size_t i = 0; i < names_.size(); i++)
        json += std::string(i ? ",\n\t\t" : "\n\t\t") + jsonString(names_[i]);
    json += "\n\t]\n}\n";
    const std::string file = dir_ + "/stamps.json", tmp = file + ".new";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << json;
    }
    fs::rename(tmp, file, ec);
}

void Stamps::rescan() {
    std::set<std::string> files;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir_, ec))
        if (e.path().extension() == ".stm")
            files.insert(e.path().stem().string());
    std::vector<std::string> kept;
    bool changed = false;
    for (const std::string &n : names_) {
        if (files.count(n))
            kept.push_back(n);
        else
            changed = true;
    }
    std::set<std::string> listed(names_.begin(), names_.end());
    for (const std::string &n : files)
        if (!listed.count(n)) {
            kept.push_back(n);
            changed = true;
        }
    if (changed) {
        names_ = kept;
        write();
    }
}

void Stamps::moveToFront(const std::string &name) {
    auto it = std::find(names_.begin(), names_.end(), name);
    bool changed = false;
    if (it == names_.end()) {
        names_.push_back(name);
        it = names_.end() - 1;
        changed = true;
    } else if (it != names_.begin())
        changed = true;
    if (changed) {
        std::rotate(names_.begin(), it, it + 1);
        write();
    }
}

std::string Stamps::add(const SaveArea &s, std::string &error) {
    const uint64_t now = uint64_t(std::time(nullptr));
    if (lastTime_ != now) {
        lastTime_ = now;
        lastName_ = 0;
    } else
        lastName_ += 1;
    std::string name;
    std::error_code ec;
    for (;;) {
        char b[32];
        std::snprintf(b, sizeof b, "%08llx%02x", static_cast<unsigned long long>(lastTime_), unsigned(lastName_));
        name = b;
        if (!fs::exists(path(name), ec))
            break;
        lastName_ += 1;
    }
    fs::create_directories(dir_, ec);
    std::vector<char> data;
    if (!tptWriteSave(s, data, error) || data.empty()) {
        if (error.empty())
            error = "Error serializing save file";
        return "";
    }
    {
        std::ofstream f(path(name), std::ios::binary);
        f.write(data.data(), std::streamsize(data.size()));
        if (!f) {
            error = "could not write " + path(name);
            return "";
        }
    }
    moveToFront(name);
    return name;
}

bool Stamps::load(const std::string &name, SaveArea &out, std::string &error) const {
    std::error_code ec;
    if (!fs::exists(path(name), ec)) {
        error = "does not exist";
        return false;
    }
    return tptReadSave(readAll(path(name)), out, error);
}

void Stamps::remove(const std::string &name) {
    auto it = std::remove(names_.begin(), names_.end(), name);
    if (it != names_.end()) {
        names_.erase(it, names_.end());
        std::error_code ec;
        fs::remove(path(name), ec);
        write();
    }
}

bool Stamps::rename(const std::string &name, const std::string &newName, std::string &error) {
    std::error_code ec;
    if (fs::exists(path(newName), ec)) {
        error = "A stamp with this name already exists.";
        return false;
    }
    fs::rename(path(name), path(newName), ec);
    if (ec) {
        error = "Could not rename the stamp.";
        return false;
    }
    std::replace(names_.begin(), names_.end(), name, newName);
    write();
    return true;
}

} // namespace bp
