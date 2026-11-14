// Shared crash and problem reporter for the ssjrocks desktop (C++) apps.
//
// Source of truth: ~/dev/app-reports/cpp/appreports.h and .cpp (github.com/ssjrocks/app-reports). Copy both into
// an app unchanged with tools/add-to-cpp-app.sh; don't edit the per-app copies.
//
// Same server and report format as android/AppReports.kt and python/appreports.py (schema 1). The report URL comes
// from the build: compile appreports.cpp with -DAPPREPORTS_URL="\"https://...\"" (a build without it works but
// sends nothing). Needs libcurl and zlib; link with -rdynamic so crash stacks name the app's functions.
//
//  - init(): fatal signals (SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL) and uncaught C++ exceptions are written to
//    disk as the process dies and sent as `crash` reports on the next start; queued reports are sent in the
//    background.
//  - log(): lines kept in memory (the last 256 KB) and sent with every report, crash reports included.
//  - reportError(): a caught, non-fatal error (once per run per type and message).
//  - sendReport(): the "Report a problem" button.
//  - attach(): extra text (app state) included in error and problem reports.
//
// Reports are queued as JSON files in <stateDir>/app-reports/pending and posted gzipped.
#pragma once
#include <functional>
#include <map>
#include <string>

namespace appreports {

// appId is a dotted id like io.github.ssjrocks.BigPowder; stateDir a folder of the app's own
void init(const std::string &appId, const std::string &appName, const std::string &version,
          const std::string &stateDir);
bool enabled(); // init() was called and the build has a report URL
void log(const std::string &line);
void attach(const std::string &name, std::function<std::string()> provider);
void reportError(const std::string &type, const std::string &message, const std::string &note = "");
// Sends a problem report in the background. onDone(sent) runs on that thread: true once the server has it, false
// if it is queued for the next start (or reporting is off).
void sendReport(const std::string &note, const std::map<std::string, std::string> &extra,
                std::function<void(bool)> onDone);
void flush(); // sends what is queued, oldest first (blocking)

} // namespace appreports
