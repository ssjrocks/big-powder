#include "app/Prefs.h"

#include <json/json.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

namespace bp {

struct Prefs::Impl {
    std::string path;
    Json::Value root;

    // Prefs::GetJson and SetJson
    static Json::Value getJson(const Json::Value &node, const std::string &path) {
        if (node.type() != Json::objectValue)
            return Json::nullValue;
        size_t dot = path.find('.');
        if (dot == std::string::npos)
            return node[path];
        return getJson(node[path.substr(0, dot)], path.substr(dot + 1));
    }
    static void setJson(Json::Value &node, const std::string &path, Json::Value value) {
        if (node.type() != Json::objectValue)
            node = Json::objectValue;
        size_t dot = path.find('.');
        if (dot == std::string::npos) {
            node[path] = std::move(value);
            return;
        }
        setJson(node[path.substr(0, dot)], path.substr(dot + 1), std::move(value));
    }
    template <class T, class F>
    T get(const std::string &path, T def, F unpack) const {
        Json::Value v = getJson(root, path);
        if (v.isNull())
            return def;
        try {
            return unpack(v);
        } catch (const std::exception &) {
            return def;
        }
    }
};

Prefs::Prefs(std::string path) : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    if (impl_->path.empty())
        return;
    std::ifstream in(impl_->path, std::ios::binary);
    if (!in)
        return;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty())
        return;
    Json::CharReaderBuilder rb;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    std::string errs;
    Json::Value root;
    if (reader->parse(data.data(), data.data() + data.size(), &root, &errs))
        impl_->root = std::move(root);
    else
        std::fprintf(stderr, "%s: %s\n", impl_->path.c_str(), errs.c_str());
}

Prefs::~Prefs() = default;

bool Prefs::getBool(const std::string &p, bool def) const {
    return impl_->get(p, def, [](const Json::Value &v) { return v.asBool(); });
}
int Prefs::getInt(const std::string &p, int def) const {
    return impl_->get(p, def, [](const Json::Value &v) { return v.asInt(); });
}
uint32_t Prefs::getUInt(const std::string &p, uint32_t def) const {
    return impl_->get(p, def, [](const Json::Value &v) { return uint32_t(v.asUInt()); });
}
float Prefs::getFloat(const std::string &p, float def) const {
    return impl_->get(p, def, [](const Json::Value &v) { return v.asFloat(); });
}
std::string Prefs::getString(const std::string &p, const std::string &def) const {
    return impl_->get(p, def, [](const Json::Value &v) { return v.asString(); });
}
std::vector<std::string> Prefs::getStrings(const std::string &p) const {
    return impl_->get(p, std::vector<std::string>{}, [](const Json::Value &v) {
        std::vector<std::string> out;
        for (const auto &item : v) // (Bipacker<std::vector<Item>>: each item as a string)
            out.push_back(item.asString());
        return out;
    });
}
int Prefs::getEnum(const std::string &p, int count, int def) const {
    int v = getInt(p, def);
    return v >= 0 && v < count ? v : def;
}
bool Prefs::has(const std::string &p) const { return !Impl::getJson(impl_->root, p).isNull(); }

void Prefs::setBool(const std::string &p, bool v) {
    Impl::setJson(impl_->root, p, Json::Value(v));
    changed();
}
void Prefs::setInt(const std::string &p, int v) {
    Impl::setJson(impl_->root, p, Json::Value(v));
    changed();
}
void Prefs::setUInt(const std::string &p, uint32_t v) {
    Impl::setJson(impl_->root, p, Json::Value(Json::UInt(v)));
    changed();
}
void Prefs::setFloat(const std::string &p, float v) {
    Impl::setJson(impl_->root, p, Json::Value(v));
    changed();
}
void Prefs::setString(const std::string &p, const std::string &v) {
    Impl::setJson(impl_->root, p, Json::Value(v));
    changed();
}
void Prefs::setStrings(const std::string &p, const std::vector<std::string> &v) {
    Json::Value a = Json::arrayValue;
    for (const auto &s : v)
        a.append(Json::Value(s));
    Impl::setJson(impl_->root, p, std::move(a));
    changed();
}

void Prefs::changed() {
    shouldWrite_ = true;
    write();
}

// Prefs::Write: tab-indented, as TPT writes it
void Prefs::write() {
    if (defer_ || !shouldWrite_ || impl_->path.empty())
        return;
    shouldWrite_ = false;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "\t";
    const std::string data = Json::writeString(wb, impl_->root);
    const std::string tmp = impl_->path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out << data;
        if (!out)
            return;
    }
    std::rename(tmp.c_str(), impl_->path.c_str());
}

} // namespace bp
