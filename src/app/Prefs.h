// TPT's preferences (prefs/Prefs): powder.pref, a JSON file in which a dotted path ("Renderer.ColourMode") is nested
// objects. Read when made; each change is written at once, unless a Defer is held (then when the last one goes).
// The getters give the default for a value that is missing or of another type, as Prefs::Get does.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bp {

class Prefs {
public:
    explicit Prefs(std::string path); // "" for none: nothing is read or written
    ~Prefs();
    Prefs(const Prefs &) = delete;
    Prefs &operator=(const Prefs &) = delete;

    bool getBool(const std::string &path, bool def) const;
    int getInt(const std::string &path, int def) const;
    uint32_t getUInt(const std::string &path, uint32_t def) const;
    float getFloat(const std::string &path, float def) const;
    std::string getString(const std::string &path, const std::string &def) const;
    std::vector<std::string> getStrings(const std::string &path) const;
    // Prefs::Get for an enum: values outside [0, count) give the default
    int getEnum(const std::string &path, int count, int def) const;
    bool has(const std::string &path) const;

    void setBool(const std::string &path, bool v);
    void setInt(const std::string &path, int v);
    void setUInt(const std::string &path, uint32_t v);
    void setFloat(const std::string &path, float v);
    void setString(const std::string &path, const std::string &v);
    void setStrings(const std::string &path, const std::vector<std::string> &v);

    // Prefs::DeferWrite: the changes while it is held are written once, when it goes
    class Defer {
    public:
        explicit Defer(Prefs &p) : p_(p) { p_.defer_++; }
        ~Defer() {
            p_.defer_--;
            p_.write();
        }

    private:
        Prefs &p_;
    };

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int defer_ = 0;
    bool shouldWrite_ = false;
    void changed();
    void write();
};

} // namespace bp
