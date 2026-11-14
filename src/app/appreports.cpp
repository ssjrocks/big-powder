// See appreports.h. Source of truth: ~/dev/app-reports/cpp/appreports.cpp; don't edit the per-app copies.
#include "appreports.h"

#include <curl/curl.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <typeinfo>
#include <vector>

#ifndef APPREPORTS_URL
#define APPREPORTS_URL ""
#endif

namespace appreports {

// (a variable, not the macro, so a build without a URL doesn't have the compiler reason that nothing is ever set up)
extern const char *reportUrl;
const char *reportUrl = APPREPORTS_URL;

namespace fs = std::filesystem;

namespace {

constexpr int LIB_VERSION = 1;
constexpr size_t MAX_TEXT = 512 * 1024;
constexpr size_t MAX_PENDING = 20;
constexpr int64_t PENDING_MAX_AGE_MS = 7LL * 24 * 3600 * 1000;
constexpr size_t MAX_ERRORS_PER_RUN = 10;
constexpr size_t RING = 256 * 1024;
constexpr int MAX_FRAMES = 64;

// Never destroyed: a background upload may still be running while the process exits.
struct State {
    std::string app, name, version;
    fs::path dir;
    std::mutex lock;      // the queue and uploads
    std::mutex logLock;   // the ring
    std::mutex attachLock;
    std::map<std::string, std::function<std::string()>> attachments;
    std::set<std::string> sentErrors;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};
State *st = nullptr;

// What the signal handler writes, prepared before it is needed
char crashPath[4096];
char crashVersion[128];
char ring[RING];
std::atomic<size_t> ringPos{0}; // total bytes ever written (the ring holds the last RING of them)
char terminateText[2048];
volatile sig_atomic_t handling = 0;
const int SIGNALS[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
struct sigaction previous[32];
std::terminate_handler previousTerminate = nullptr;

// --- async-signal-safe writing ------------------------------------------------------------------

void put(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, s, n);
        if (w <= 0)
            return;
        s += w, n -= size_t(w);
    }
}
void put(int fd, const char *s) { put(fd, s, std::strlen(s)); }
void putNumber(int fd, unsigned long long v, int base) {
    char b[32];
    int i = 31;
    b[i] = 0;
    do {
        int d = int(v % unsigned(base));
        b[--i] = char(d < 10 ? '0' + d : 'a' + d - 10);
        v /= unsigned(base);
    } while (v && i > 0);
    put(fd, b + i);
}

void onSignal(int sig, siginfo_t *info, void *) {
    if (!handling) {
        handling = 1;
        int fd = ::open(crashPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            struct timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            put(fd, "appreports-crash 1\nversion ");
            put(fd, crashVersion);
            put(fd, "\ntime ");
            putNumber(fd, (unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000, 10);
            put(fd, "\nsignal ");
            putNumber(fd, (unsigned long long)sig, 10);
            put(fd, "\naddress ");
            putNumber(fd, (unsigned long long)(uintptr_t)(info ? info->si_addr : nullptr), 16);
            put(fd, "\nthread ");
            putNumber(fd, (unsigned long long)syscall(SYS_gettid), 10);
            put(fd, "\npid ");
            putNumber(fd, (unsigned long long)getpid(), 10);
            put(fd, "\n");
            if (terminateText[0]) {
                put(fd, "exception ");
                put(fd, terminateText);
                put(fd, "\n");
            }
            void *frames[MAX_FRAMES];
            int n = backtrace(frames, MAX_FRAMES);
            put(fd, "frames\n");
            for (int k = 0; k < n; k++) {
                putNumber(fd, (unsigned long long)(uintptr_t)frames[k], 16);
                put(fd, "\n");
            }
            put(fd, "maps\n");
            int maps = ::open("/proc/self/maps", O_RDONLY);
            if (maps >= 0) {
                char b[4096];
                ssize_t r;
                while ((r = ::read(maps, b, sizeof b)) > 0)
                    put(fd, b, size_t(r));
                ::close(maps);
            }
            put(fd, "log\n");
            size_t end = ringPos.load(), len = std::min(end, RING);
            for (size_t k = end - len; k < end; k++)
                put(fd, &ring[k % RING], 1);
            ::close(fd);
        }
    }
    // let it die as it would have (the previous handler, or the default)
    struct sigaction &p = previous[sig];
    if (!(p.sa_flags & SA_SIGINFO) && (p.sa_handler == SIG_DFL || p.sa_handler == SIG_IGN)) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else {
        sigaction(sig, &p, nullptr);
        raise(sig);
    }
}

void onTerminate() {
    // the uncaught exception's type and message, for the SIGABRT that follows
    std::string text = "std::terminate";
    if (std::exception_ptr e = std::current_exception()) {
        const std::type_info *t = abi::__cxa_current_exception_type();
        std::string type = "?";
        if (t) {
            int status = 0;
            char *d = abi::__cxa_demangle(t->name(), nullptr, nullptr, &status);
            type = status == 0 && d ? d : t->name();
            std::free(d);
        }
        std::string what;
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &x) {
            what = x.what();
        } catch (...) {
        }
        text = type + ": " + what;
    }
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::snprintf(terminateText, sizeof terminateText, "%s", text.c_str());
    if (previousTerminate)
        previousTerminate();
    std::abort();
}

// --- report building ----------------------------------------------------------------------------

std::string json(const std::string &s) {
    std::string o = "\"";
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        // valid UTF-8 passes through; anything else becomes U+FFFD
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + size_t(len) <= s.size();
        for (int k = 1; ok && k < len; k++)
            ok = ((unsigned char)s[i + size_t(k)] >> 6) == 2;
        if (!ok) {
            o += "\\ufffd";
            i++;
            continue;
        }
        if (len > 1) {
            o.append(s, i, size_t(len));
            i += size_t(len);
            continue;
        }
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else
                o += char(c);
        }
        i++;
    }
    return o + "\"";
}

std::string tail(const std::string &s, size_t n = MAX_TEXT) { return s.size() > n ? s.substr(s.size() - n) : s; }

std::string readFile(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string firstLine(const fs::path &p) {
    std::ifstream in(p);
    std::string l;
    std::getline(in, l);
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r'))
        l.pop_back();
    return l;
}

std::string osName() {
    std::ifstream in("/etc/os-release");
    std::string l;
    while (std::getline(in, l))
        if (l.rfind("PRETTY_NAME=", 0) == 0) {
            l = l.substr(12);
            l.erase(std::remove(l.begin(), l.end(), '"'), l.end());
            return l;
        }
    return "Linux";
}

std::string iso(int64_t ms) {
    time_t t = time_t(ms / 1000);
    struct tm g{};
    gmtime_r(&t, &g);
    char b[96];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                  g.tm_hour, g.tm_min, g.tm_sec, int(ms % 1000));
    return b;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string randomHex(int bytes) {
    std::string out;
    std::ifstream r("/dev/urandom", std::ios::binary);
    for (int k = 0; k < bytes; k++) {
        unsigned char c = (unsigned char)r.get();
        char b[3];
        std::snprintf(b, sizeof b, "%02x", c);
        out += b;
    }
    return out;
}

std::string installId() {
    fs::path p = st->dir / "install-id";
    std::string id = firstLine(p);
    if (id.size() >= 16)
        return id;
    std::string h = randomHex(16);
    id = h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
    std::ofstream(p) << id;
    return id;
}

std::string ringText() {
    std::lock_guard g(st->logLock);
    size_t end = ringPos.load(), len = std::min(end, RING);
    std::string out;
    out.reserve(len);
    for (size_t k = end - len; k < end; k++)
        out += ring[k % RING];
    return out;
}

// The fields every report has; `fields` (already JSON, starting with a comma) are added
std::string report(const std::string &kind, int64_t happened, const std::string &log, const std::string &fields,
                   bool withAttachments, const std::string &version) {
    struct utsname u{};
    uname(&u);
    std::string vendor = firstLine("/sys/class/dmi/id/sys_vendor"), model = firstLine("/sys/class/dmi/id/product_name");
    std::string cpu;
    {
        std::ifstream in("/proc/cpuinfo");
        std::string l;
        while (std::getline(in, l))
            if (l.rfind("model name", 0) == 0) {
                cpu = l.substr(l.find(':') + 2);
                break;
            }
    }
    const auto up = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - st->started);
    std::ostringstream r;
    r << "{\"schema\":1,\"lib\":" << LIB_VERSION << ",\"app\":" << json(st->app) << ",\"appName\":" << json(st->name)
      << ",\"version\":" << json(version) << ",\"versionCode\":null,\"debuggable\":false,\"kind\":" << json(kind)
      << ",\"happenedAt\":" << json(iso(happened)) << ",\"installId\":" << json(installId())
      << ",\"process\":" << json("native " + std::to_string(getpid())) << ",\"uptimeSec\":" << up.count()
      << ",\"device\":{\"manufacturer\":" << json(vendor.empty() ? "PC" : vendor)
      << ",\"model\":" << json(model.empty() ? u.machine : model) << ",\"os\":" << json(osName())
      << ",\"kernel\":" << json(u.release) << ",\"arch\":" << json(u.machine) << ",\"cpu\":" << json(cpu) << "}";
    if (!log.empty())
        r << ",\"log\":" << json(tail(log));
    if (withAttachments) {
        std::map<std::string, std::function<std::string()>> providers;
        {
            std::lock_guard g(st->attachLock);
            providers = st->attachments;
        }
        std::string a;
        for (auto &[name, provider] : providers) {
            std::string text;
            try {
                text = provider();
            } catch (const std::exception &e) {
                text = std::string("(attachment failed: ") + e.what() + ")";
            }
            if (!text.empty())
                a += (a.empty() ? "" : ",") + json(name) + ":" + json(tail(text));
        }
        if (!a.empty())
            r << ",\"attachments\":{" << a << "}";
    }
    r << fields << "}";
    return r.str();
}

std::string exceptionJson(const std::string &type, const std::string &message, const std::string &stack) {
    return ",\"exception\":{\"type\":" + json(type) + ",\"message\":" + json(message) + ",\"stack\":" +
           json(tail(stack)) + "}";
}

fs::path pendingDir() { return st->dir / "pending"; }

fs::path save(const std::string &kind, const std::string &body) {
    fs::path d = pendingDir();
    fs::path p = d / (std::to_string(nowMs()) + "-" + kind + "-" + randomHex(4) + ".json");
    fs::path tmp = p;
    tmp.replace_extension(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << body;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    std::vector<fs::path> files;
    for (const auto &e : fs::directory_iterator(d, ec))
        if (e.path().extension() == ".json")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (size_t k = 0; k + MAX_PENDING < files.size(); k++)
        fs::remove(files[k], ec);
    return p;
}

// A crash written by onSignal, made into a report: the frames as "at function(module+0xoffset)", named where the
// module is this executable and still the same version
std::string crashReport(const std::string &text) {
    std::istringstream in(text);
    std::string line, section, version, exception, log;
    int64_t time = nowMs();
    int sig = 0;
    unsigned long long address = 0;
    std::string thread;
    std::vector<uintptr_t> frames;
    struct Map {
        uintptr_t start, end, offset;
        std::string path;
    };
    std::vector<Map> maps;
    while (std::getline(in, line)) {
        if (section == "log") {
            log += line + "\n";
            continue;
        }
        if (line == "frames" || line == "maps" || line == "log") {
            section = line;
            continue;
        }
        if (section == "frames") {
            frames.push_back(uintptr_t(std::strtoull(line.c_str(), nullptr, 16)));
        } else if (section == "maps") {
            Map m{};
            char perms[8] = {};
            unsigned long long a = 0, b = 0, off = 0;
            int pathAt = 0;
            if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &a, &b, perms, &off, &pathAt) >= 4) {
                m.start = uintptr_t(a), m.end = uintptr_t(b), m.offset = uintptr_t(off);
                if (pathAt > 0 && size_t(pathAt) < line.size())
                    m.path = line.substr(size_t(pathAt));
                maps.push_back(m);
            }
        } else if (line.rfind("version ", 0) == 0)
            version = line.substr(8);
        else if (line.rfind("time ", 0) == 0)
            time = std::strtoll(line.c_str() + 5, nullptr, 10);
        else if (line.rfind("signal ", 0) == 0)
            sig = std::atoi(line.c_str() + 7);
        else if (line.rfind("address ", 0) == 0)
            address = std::strtoull(line.c_str() + 8, nullptr, 16);
        else if (line.rfind("thread ", 0) == 0)
            thread = line.substr(7);
        else if (line.rfind("exception ", 0) == 0)
            exception = line.substr(10);
    }
    // where this executable is loaded now, to name the frames in it
    std::error_code ec;
    const std::string self = fs::canonical("/proc/self/exe", ec).string();
    uintptr_t selfBase = 0;
    dl_iterate_phdr(
        [](struct dl_phdr_info *info, size_t, void *data) {
            if (info->dlpi_name == nullptr || info->dlpi_name[0] == 0) { // (the executable)
                *static_cast<uintptr_t *>(data) = uintptr_t(info->dlpi_addr);
                return 1;
            }
            return 0;
        },
        &selfBase);
    auto moduleBase = [&](const std::string &path) {
        uintptr_t base = UINTPTR_MAX;
        for (const Map &m : maps)
            if (m.path == path && m.offset == 0)
                base = std::min(base, m.start);
        return base;
    };
    std::ostringstream stack;
    const char *names[] = {"", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT", "SIGBUS", "SIGFPE",
                           "SIGKILL", "SIGUSR1", "SIGSEGV"};
    const std::string sigName = sig > 0 && sig < 12 ? names[sig] : "signal " + std::to_string(sig);
    std::string type = sigName, message;
    if (!exception.empty()) {
        size_t colon = exception.find(": ");
        type = colon == std::string::npos ? exception : exception.substr(0, colon);
        message = colon == std::string::npos ? "" : exception.substr(colon + 2);
    } else {
        char b[64];
        std::snprintf(b, sizeof b, "%s at 0x%llx", sigName.c_str(), address);
        message = b;
    }
    stack << type << ": " << message << " (thread " << thread << ")\n";
    for (uintptr_t f : frames) {
        const Map *m = nullptr;
        for (const Map &x : maps)
            if (f >= x.start && f < x.end)
                m = &x;
        if (!m || m->path.empty()) {
            char b[64];
            std::snprintf(b, sizeof b, "\tat ?(0x%llx)\n", (unsigned long long)f);
            stack << b;
            continue;
        }
        const uintptr_t off = f - moduleBase(m->path);
        const std::string file = fs::path(m->path).filename().string();
        std::string name = "?";
        if (m->path == self && version == st->version && selfBase) {
            Dl_info di{};
            ElfW(Sym) *sym = nullptr;
            void *here = reinterpret_cast<void *>(selfBase + off);
            if (dladdr1(here, &di, reinterpret_cast<void **>(&sym), RTLD_DL_SYMENT) && di.dli_sname && sym &&
                uintptr_t(here) >= uintptr_t(di.dli_saddr) &&
                uintptr_t(here) < uintptr_t(di.dli_saddr) + std::max<uintptr_t>(sym->st_size, 1)) {
                int status = 0;
                char *d = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &status);
                name = status == 0 && d ? d : di.dli_sname;
                std::free(d);
            }
        }
        char b[64];
        std::snprintf(b, sizeof b, "+0x%llx)\n", (unsigned long long)off);
        stack << "\tat " << name << "(" << file << b;
    }
    // (the report is about the version that crashed)
    return report("crash", time, log, ",\"thread\":" + json(thread) + exceptionJson(type, message, stack.str()), false,
                  version.empty() ? st->version : version);
}

// --- upload -------------------------------------------------------------------------------------

std::string gzip(const std::string &in) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    std::string out(deflateBound(&zs, uLong(in.size())) + 32, '\0');
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    int r = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return r == Z_STREAM_END ? out : std::string{};
}

long post(const std::string &body) { // the HTTP status, or 0 if it didn't get there
    const std::string z = gzip(body);
    if (z.empty())
        return 0;
    CURL *c = curl_easy_init();
    if (!c)
        return 0;
    curl_slist *h = nullptr;
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, "Content-Encoding: gzip");
    const std::string ua = "AppReports/" + std::to_string(LIB_VERSION) + " " + st->app;
    curl_easy_setopt(c, CURLOPT_URL, reportUrl);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, z.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(z.size()));
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char *, size_t s, size_t n, void *) { return s * n; });
    long code = 0;
    if (curl_easy_perform(c) == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

void flushAsync() { std::thread([] { flush(); }).detach(); }

} // namespace

bool enabled() { return st != nullptr; }

void init(const std::string &appId, const std::string &appName, const std::string &version,
          const std::string &stateDir) {
    if (st || reportUrl == nullptr || reportUrl[0] == 0)
        return;
    st = new State;
    st->app = appId, st->name = appName, st->version = version;
    st->dir = fs::path(stateDir) / "app-reports";
    std::error_code ec;
    fs::create_directories(st->dir / "pending", ec);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::snprintf(crashPath, sizeof crashPath, "%s", (st->dir / "crash").c_str());
    std::snprintf(crashVersion, sizeof crashVersion, "%s", version.c_str());

    // the last run's crash, if it had one
    if (fs::exists(crashPath, ec)) {
        std::string text = readFile(crashPath);
        fs::remove(crashPath, ec);
        if (text.rfind("appreports-crash 1\n", 0) == 0)
            save("crash", crashReport(text));
    }

    {
        void *warm[2];
        backtrace(warm, 2); // (loads libgcc now, not in the signal handler)
    }
    static char altStack[64 * 1024]; // (a stack overflow still gets reported)
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    sigaltstack(&ss, nullptr);
    for (int sig : SIGNALS) {
        struct sigaction sa{};
        sa.sa_sigaction = onSignal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &previous[sig]);
    }
    previousTerminate = std::set_terminate(onTerminate);
    flushAsync();
}

void log(const std::string &line) {
    if (!st)
        return;
    std::lock_guard g(st->logLock);
    std::string l = line;
    if (l.empty() || l.back() != '\n')
        l += '\n';
    size_t p = ringPos.load();
    for (char c : l)
        ring[p++ % RING] = c;
    ringPos.store(p);
}

void attach(const std::string &name, std::function<std::string()> provider) {
    if (!st)
        return;
    std::lock_guard g(st->attachLock);
    st->attachments[name] = std::move(provider);
}

void reportError(const std::string &type, const std::string &message, const std::string &note) {
    if (!st)
        return;
    {
        std::lock_guard g(st->lock);
        const std::string key = type + "|" + message;
        if (st->sentErrors.size() >= MAX_ERRORS_PER_RUN || st->sentErrors.count(key))
            return;
        st->sentErrors.insert(key);
    }
    std::string fields = exceptionJson(type, message, type + ": " + message + "\n");
    if (!note.empty())
        fields += ",\"note\":" + json(note);
    save("error", report("error", nowMs(), ringText(), fields, true, st->version));
    flushAsync();
}

void sendReport(const std::string &note, const std::map<std::string, std::string> &extra,
                std::function<void(bool)> onDone) {
    if (!st) {
        if (onDone)
            onDone(false);
        return;
    }
    std::thread([note, extra, onDone] {
        bool ok = false;
        try {
            std::string fields;
            if (!note.empty())
                fields += ",\"note\":" + json(note);
            if (!extra.empty()) {
                std::string e;
                for (auto &[k, v] : extra)
                    e += (e.empty() ? "" : ",") + json(k) + ":" + json(v);
                fields += ",\"extra\":{" + e + "}";
            }
            fs::path p = save("report", report("report", nowMs(), ringText(), fields, true, st->version));
            flush();
            std::error_code ec;
            ok = !fs::exists(p, ec);
        } catch (const std::exception &) {
        }
        if (onDone)
            onDone(ok);
    }).detach();
}

void flush() {
    if (!st)
        return;
    std::lock_guard g(st->lock);
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto &e : fs::directory_iterator(pendingDir(), ec))
        if (e.path().extension() == ".json")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path &f : files) {
        const int64_t made = std::strtoll(f.filename().c_str(), nullptr, 10);
        if (nowMs() - made > PENDING_MAX_AGE_MS) {
            fs::remove(f, ec);
            continue;
        }
        const long code = post(readFile(f));
        if (code >= 200 && code < 300)
            fs::remove(f, ec);
        else if (code == 0 || code == 408 || code == 429 || code >= 500)
            return; // (try again on the next start)
        else
            fs::remove(f, ec); // rejected: it won't be accepted later either
    }
}

} // namespace appreports
