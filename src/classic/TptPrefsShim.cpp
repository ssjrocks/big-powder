// Stand-in for TPT's prefs/Prefs.cpp. The only preference the simulation reads is FftwPlanMeasure (in
// gravity/Fft.cpp), and TPT's own default for it is what we want. The real Prefs reads and writes
// powder.pref in the current folder, which Classic mode must not do.
#include "prefs/GlobalPrefs.h"

Prefs::Prefs(ByteString newPath) : path(newPath) {}

Json::Value Prefs::GetJson(const Json::Value &, ByteString) { return Json::nullValue; }

template <> bool Prefs::Bipacker<bool>::Unpack(const Json::Value &value) { return value.asBool(); }
