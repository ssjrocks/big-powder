// HTTPS GETs (libcurl) for the update check, the online saves and their pictures: redirects followed, HTTPS only,
// Big Powder's own user agent. Blocking: call from a thread of its own.
#pragma once
#include <functional>
#include <string>

namespace bp {

struct HttpGet {
    bool json = false;                                // Accept: the GitHub API's JSON
    std::function<void(double done, double total)> progress; // (bytes, as they come)
};
// True with the body in `out` if the server answered 200; false with `error` set otherwise
bool httpGet(const std::string &url, std::string &out, std::string &error, const HttpGet &how = {});
// %-encoding for a URL's query, as format::URLEncode does it (all but letters and digits)
std::string urlEncode(const std::string &s);

} // namespace bp
