#include "core/Eng.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace syms {
namespace eng {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool all_letters(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalpha(u)) continue;
        // allow UTF-8 continuation bytes so "µ" passes as a unit letter
        if (u >= 0x80) continue;
        return false;
    }
    return true;
}

double prefix_value(char c) {
    switch (c) {
        case 'T': return 1e12;
        case 'G': return 1e9;
        case 'M': return 1e6;
        case 'K':
        case 'k': return 1e3;
        case 'm': return 1e-3;
        case 'u':
        case '\xC2': return 1e-6; // 'µ' starts with 0xC2 in UTF-8
        case 'n': return 1e-9;
        case 'p': return 1e-12;
        case 'f': return 1e-15;
        default: return 0.0; // 0 = not a prefix
    }
}

} // namespace

// Exact decimal -> rational. Accepts an optional sign, digits with an optional
// single '.', and an optional e/E exponent. No SI suffix, no whitespace.
bool parse_exact_decimal(const std::string& text, long long& num, long long& den) {
    std::string t = trim(text);
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[i] == '+' || t[i] == '-') {
        neg = t[i] == '-';
        ++i;
    }
    std::string digits;
    int frac = 0;
    bool any = false;
    bool seen_dot = false;
    for (; i < t.size(); ++i) {
        char ch = t[i];
        if (ch >= '0' && ch <= '9') {
            digits += ch;
            any = true;
            if (seen_dot) ++frac;
        } else if (ch == '.') {
            if (seen_dot) return false; // second '.'
            seen_dot = true;
        } else {
            break;
        }
    }
    if (!any) return false;
    long long exp10 = 0;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
            eneg = t[i] == '-';
            ++i;
        }
        if (i >= t.size()) return false;
        long long e = 0;
        for (; i < t.size(); ++i) {
            if (t[i] < '0' || t[i] > '9') return false;
            e = e * 10 + (t[i] - '0');
        }
        exp10 = eneg ? -e : e;
    }
    if (i != t.size()) return false; // trailing junk (e.g. an SI suffix)

    // value = digits * 10^(exp10 - frac)
    long long n = 0, d = 1;
    for (char ch : digits) n = n * 10 + (ch - '0');
    if (n == 0) {
        num = 0;
        den = 1;
        return true;
    }
    long long net = exp10 - frac;
    while (net > 0) { n *= 10; --net; }
    while (net < 0) { d *= 10; ++net; }
    num = neg ? -n : n;
    den = d;
    return true;
}

bool parse_value(const std::string& text, double& out) {
    std::string t = trim(text);
    if (t.empty()) return false;

    const char* begin = t.c_str();
    char* endp = nullptr;
    double v = std::strtod(begin, &endp);
    if (endp == begin) return false;

    std::string suffix = trim(std::string(endp));
    if (suffix.empty()) {
        out = v;
        return true;
    }

    static const std::map<std::string, double> exact = {
        {"", 1.0},        {"T", 1e12},   {"G", 1e9},    {"M", 1e6},
        {"k", 1e3},       {"K", 1e3},    {"m", 1e-3},   {"u", 1e-6},
        {"n", 1e-9},      {"p", 1e-12},  {"f", 1e-15},  {"meg", 1e6},
        {"MEG", 1e6},     {"Meg", 1e6},  {"MEGA", 1e6},  {"mega", 1e6},
    };
    auto it = exact.find(suffix);
    if (it != exact.end()) {
        out = v * it->second;
        return true;
    }

    // prefix char + optional unit letters, e.g. "kOhm", "uF", "nH", "MHz"
    char p = suffix[0];
    double pv = prefix_value(p);
    if (pv != 0.0) {
        std::string rest = suffix.substr(1);
        // "meg..." after a leading 'm' would otherwise be milli+"eg"
        if (p == 'm' && (rest == "eg" || rest == "egs")) {
            out = v * 1e6;
            return true;
        }
        if (rest.empty() || all_letters(rest)) {
            out = v * pv;
            return true;
        }
        return false;
    }

    // pure unit, e.g. "F", "Ohm", "Hz", "V" -- value stands as written
    if (all_letters(suffix)) {
        out = v;
        return true;
    }
    return false;
}

std::string format_si(double v, int sig) {
    if (sig < 1) sig = 1;
    if (v == 0.0) return "0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*e", std::max(0, sig - 1), v);
    // "1.59e+05" -> "1.59e5"
    std::string s(buf);
    size_t e = s.find('e');
    if (e == std::string::npos) return s;
    std::string mant = s.substr(0, e);
    std::string exps = s.substr(e + 1);
    if (!exps.empty() && (exps[0] == '+' || exps[0] == '-')) {
        char sign = exps[0];
        exps = exps.substr(1);
        while (exps.size() > 1 && exps[0] == '0') exps.erase(0, 1);
        if (sign == '-') return mant + "e-" + exps;
        return mant + "e" + exps;
    }
    return mant + "e" + exps;
}

std::string format_eng(double v, int sig) {
    if (v == 0.0) return "0";
    double a = std::fabs(v);
    if (a >= 1e15 || a < 1e-18) return format_si(v, sig);

    struct Unit { double scale; const char* suffix; };
    static const Unit units[] = {
        {1e12, "T"}, {1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1.0, ""},
        {1e-3, "m"}, {1e-6, "\xC2\xB5"}, /* µ */ {1e-9, "n"}, {1e-12, "p"},
        {1e-15, "f"},
    };
    const int n_units = int(sizeof(units) / sizeof(units[0]));
    const Unit* u = &units[4]; // default: no prefix
    for (int i = 0; i < n_units; ++i) {
        if (a >= units[i].scale) { u = &units[i]; break; }
    }

    auto render = [&](double mant) {
        // digits in the integer part (1..3); decimals = remaining sig figs.
        int int_digits = mant >= 100.0 ? 3 : (mant >= 10.0 ? 2 : 1);
        int decimals = sig - int_digits;
        if (decimals < 0) decimals = 0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, mant);
        std::string s(buf);
        if (s.find('.') != std::string::npos) {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        return s;
    };

    double mant = a / u->scale;
    std::string s = render(mant);
    // Round-off overflow: 999.6 -> "1000" must bump to the next prefix.
    if (std::strtod(s.c_str(), nullptr) >= 1000.0 && u > units) {
        --u;
        s = render(a / u->scale);
    }
    if (v < 0.0) s = "-" + s;
    return s + u->suffix;
}

std::string format_hz(double hz, int sig) {
    if (hz == 0.0) return "0 Hz";
    std::string s = format_eng(hz, sig);
    std::string prefix;
    if (!s.empty() && (unsigned char)s.back() >= 0x80) {
        prefix = s.substr(s.size() - 2); // two-byte UTF-8 µ
        s = s.substr(0, s.size() - 2);
    } else if (!s.empty() && std::isalpha(static_cast<unsigned char>(s.back()))) {
        prefix = s.substr(s.size() - 1);
        s = s.substr(0, s.size() - 1);
    }
    if (prefix.empty()) return s + " Hz";
    return s + " " + prefix + "Hz";
}

std::string format_rads(double w, int sig) {
    if (w == 0.0) return "0 rad/s";
    return format_eng(w, sig) + "rad/s";
}

// Make a plain-text engineering string safe to embed in LaTeX: the SI micro
// prefix is emitted as a raw UTF-8 "\xC2\xB5" (µ) so it reads correctly in the
// text panel, but inside math mode it must be the control word `\mu`.
std::string latex_safe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xB5) {
            out += "\\mu ";
            ++i;
        } else {
            out += char(c);
        }
    }
    return out;
}

std::string format_percent(double pct, int sig) {
    // Percentages are small, human-scale numbers: never use scientific
    // notation. Show `sig` significant figures as plain decimals
    // (56.68, 0.57, 0.0084, ...).
    if (sig < 1) sig = 1;
    if (pct == 0.0) return "0";
    double a = std::fabs(pct);
    int int_digits = a >= 100.0 ? 3 : a >= 10.0 ? 2 : a >= 1.0 ? 1 : 0;
    int decimals = sig - int_digits;
    if (a < 1.0) {
        // count the leading zeros after the decimal point and add them
        decimals = sig - int_digits + int(std::floor(-std::log10(a)));
    }
    if (decimals < 0) decimals = 0;
    if (decimals > 12) decimals = 12;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, pct);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

std::string format_db(double db, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f dB", decimals, db);
    return buf;
}

double db_to_factor(double db) { return std::pow(10.0, db / 20.0); }

double factor_to_db(double factor) {
    if (factor <= 0.0) return -std::numeric_limits<double>::infinity();
    return 20.0 * std::log10(factor);
}

} // namespace eng
} // namespace syms
