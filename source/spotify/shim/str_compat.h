// Names the PS3 libstdc++ lacks (it is built without the C99 string conversions).  Forced into every Spotify source with -include.
#pragma once
#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
namespace std {
using ::snprintf;
using ::strtof;
using ::strtol;
using ::strtold;
using ::strtoll;
using ::strtoul;
using ::strtoull;
inline string to_string(long long v) { char b[32]; snprintf(b, sizeof b, "%lld", v); return b; }
inline string to_string(int v) { return to_string((long long)v); }
inline string to_string(unsigned v) { return to_string((long long)v); }
inline string to_string(long v) { return to_string((long long)v); }
inline string to_string(unsigned long v) { return to_string((long long)v); }
inline string to_string(unsigned long long v) { char b[32]; snprintf(b, sizeof b, "%llu", v); return b; }
inline int stoi(const string& s, size_t* = nullptr, int base = 10) { return (int)strtol(s.c_str(), nullptr, base); }
inline long stol(const string& s, size_t* = nullptr, int base = 10) { return strtol(s.c_str(), nullptr, base); }
inline unsigned long long stoull(const string& s, size_t* = nullptr, int base = 10) { return strtoull(s.c_str(), nullptr, base); }
enum class endian { little = 1234, big = 4321, native = big };
}  // namespace std
