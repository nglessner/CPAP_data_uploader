#include "NtpSidecarWriter.h"

#include <cctype>

namespace {

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
