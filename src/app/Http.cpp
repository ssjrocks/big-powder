#include "app/Http.h"

#include <curl/curl.h>

#include <cstdio>

namespace bp {

bool httpGet(const std::string &url, std::string &out, std::string &error, const HttpGet &how) {
    CURL *c = curl_easy_init();
    if (!c) {
        error = "could not start curl";
        return false;
    }
    curl_slist *h = nullptr;
    if (how.json)
        h = curl_slist_append(h, "Accept: application/vnd.github+json");
    const std::string ua = std::string("BigPowder/") + BP_VERSION + " (Linux)";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L); // (gives up after 60 s without data)
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // (gzip and the like, if the server offers them)
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char *p, size_t s, size_t n, void *d) {
        static_cast<std::string *>(d)->append(p, s * n);
        return s * n;
    });
    if (how.progress) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &how.progress);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION,
                         +[](void *d, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
                             (*static_cast<const std::function<void(double, double)> *>(d))(double(now), double(total));
                             return 0;
                         });
    }
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) {
        error = curl_easy_strerror(r);
        return false;
    }
    if (code != 200) {
        error = "the server answered " + std::to_string(code);
        return false;
    }
    return true;
}

std::string urlEncode(const std::string &s) {
    std::string out;
    for (unsigned char ch : s) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
            out += char(ch);
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", ch);
            out += b;
        }
    }
    return out;
}

} // namespace bp
