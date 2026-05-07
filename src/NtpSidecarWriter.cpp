#include "NtpSidecarWriter.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

// Parse exactly nDigits ASCII decimal digits. Returns false if any non-digit.
bool parseUInt(const char* p, size_t nDigits, int& out) {
    int v = 0;
    for (size_t i = 0; i < nDigits; ++i) {
        if (!isdigit(static_cast<unsigned char>(p[i]))) return false;
        v = v * 10 + (p[i] - '0');
    }
    out = v;
    return true;
}

// Trim leading/trailing ASCII spaces and return null-terminated copy in dst.
void trimToCStr(
    const char* src, size_t srcLen, char* dst, size_t dstCap) {
    size_t lo = 0, hi = srcLen;
    while (lo < hi && src[lo] == ' ') ++lo;
    while (hi > lo && src[hi - 1] == ' ') --hi;
    size_t n = hi - lo;
    if (n + 1 > dstCap) n = dstCap - 1;
    memcpy(dst, src + lo, n);
    dst[n] = '\0';
}

bool isAllDigits(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!isdigit(static_cast<unsigned char>(p[i]))) return false;
    }
    return true;
}

bool endsWithEdfCaseInsensitive(const char* s, size_t len) {
    if (len < 4) return false;
    return (s[len-4] == '.') &&
           (tolower(static_cast<unsigned char>(s[len-3])) == 'e') &&
           (tolower(static_cast<unsigned char>(s[len-2])) == 'd') &&
           (tolower(static_cast<unsigned char>(s[len-1])) == 'f');
}

}  // namespace

bool NtpSidecarWriter::isDatalogEdf(const String& path) {
    const char* s = path.c_str();
    const size_t n = path.length();

    // Must start with "/DATALOG/"
    static const char kPrefix[] = "/DATALOG/";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;  // 9
    if (n < kPrefixLen) return false;
    for (size_t i = 0; i < kPrefixLen; ++i) {
        if (s[i] != kPrefix[i]) return false;
    }

    // Next 8 chars must be digits (the yyyymmdd folder)
    if (n < kPrefixLen + 8) return false;
    if (!isAllDigits(s + kPrefixLen, 8)) return false;

    // Then exactly one '/'
    if (s[kPrefixLen + 8] != '/') return false;

    const char* fname = s + kPrefixLen + 8 + 1;
    const size_t fnameLen = n - (kPrefixLen + 8 + 1);

    // Filename must not be empty before .edf
    if (fnameLen <= 4) return false;  // ".edf" alone is not a filename

    // Must end in .edf (case-insensitive)
    if (!endsWithEdfCaseInsensitive(fname, fnameLen)) return false;

    // No further path separators (rejects /DATALOG/20260507/sub/x.edf)
    for (size_t i = 0; i < fnameLen; ++i) {
        if (fname[i] == '/') return false;
    }

    return true;
}

#ifdef UNIT_TEST
static bool readHeaderBytes(MockFile& f, uint8_t* hdr) {
    return f.read(hdr, 256) == 256;
}
#else
static bool readHeaderBytes(fs::File& f, uint8_t* hdr) {
    return f.read(hdr, 256) == 256;
}
#endif

#ifdef UNIT_TEST
bool NtpSidecarWriter::parseEdfHeader(MockFile& f, EdfHeader& out)
#else
bool NtpSidecarWriter::parseEdfHeader(fs::File& f, EdfHeader& out)
#endif
{
    uint8_t hdr[256];
    if (!readHeaderBytes(f, hdr)) return false;

    // Date dd.MM.yy at offset 168 (8 bytes), Time HH.mm.ss at offset 176.
    const char* date = reinterpret_cast<const char*>(hdr + 168);
    const char* time = reinterpret_cast<const char*>(hdr + 176);
    if (date[2] != '.' || date[5] != '.') return false;
    if (time[2] != '.' || time[5] != '.') return false;

    int dd = 0, mm = 0, yy = 0, HH = 0, MM = 0, SS = 0;
    if (!parseUInt(date + 0, 2, dd)) return false;
    if (!parseUInt(date + 3, 2, mm)) return false;
    if (!parseUInt(date + 6, 2, yy)) return false;
    if (!parseUInt(time + 0, 2, HH)) return false;
    if (!parseUInt(time + 3, 2, MM)) return false;
    if (!parseUInt(time + 6, 2, SS)) return false;

    // EDF spec: yy 00-84 → 2000+yy, 85-99 → 1900+yy.
    int year = (yy <= 84) ? (2000 + yy) : (1900 + yy);

    if (mm < 1 || mm > 12) return false;
    if (dd < 1 || dd > 31) return false;
    if (HH > 23 || MM > 59 || SS > 60) return false;  // 60 = leap second

    snprintf(out.startNaiveStr, sizeof(out.startNaiveStr),
             "%04d-%02d-%02dT%02d:%02d:%02d",
             year, mm, dd, HH, MM, SS);

    // num_records at offset 236 (8 bytes), record_duration at 244.
    char numRecStr[16] = {0};
    char recDurStr[16] = {0};
    trimToCStr(reinterpret_cast<const char*>(hdr + 236), 8,
               numRecStr, sizeof(numRecStr));
    trimToCStr(reinterpret_cast<const char*>(hdr + 244), 8,
               recDurStr, sizeof(recDurStr));

    char* end = nullptr;
    long numRec = strtol(numRecStr, &end, 10);
    if (end == numRecStr || *end != '\0') return false;
    if (numRec < 0) return false;  // -1 means "unknown" — useless for us

    double recDur = strtod(recDurStr, &end);
    if (end == recDurStr || *end != '\0') return false;
    if (recDur < 0.0) return false;

    double total = static_cast<double>(numRec) * recDur;
    out.durationSeconds = static_cast<int>(std::lround(total));
    return true;
}
