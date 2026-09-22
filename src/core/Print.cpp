#include "core/Print.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace syms {
namespace {

using GiNaC::ex;
using GiNaC::is_a;
using GiNaC::numeric;

// unicode superscript: 2 -> "²"
std::string sup(int k) {
    static const char* supd[] = {
        "\xE2\x81\xB0", /*0*/ "\xC2\xB9", /*1*/ "\xC2\xB2", /*2*/ "\xC2\xB3", /*3*/
        "\xE2\x81\xB4", "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7",
        "\xE2\x81\xB8", "\xE2\x81\xB9"};
    std::string ds = std::to_string(k);
    std::string out;
    for (char c : ds) {
        if (c >= '0' && c <= '9') out += supd[c - '0'];
        else out += c;
    }
    return out;
}

// GiNaC's default stream output ("print_dflt") as a string.
std::string ginac_str(const ex& e) {
    std::ostringstream os;
    os << e;
    return os.str();
}

std::string fmt_number(const numeric& n) {
    if (n.is_integer()) return ginac_str(n);
    double d = n.to_double();
    char buf[64];
    if (d == std::floor(d) && std::fabs(d) < 1e15)
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
    else
        std::snprintf(buf, sizeof(buf), "%.6g", d);
    return buf;
}

// prec: 0 = top level, 1 = term of a sum, 2 = operand of a product /
//       parenthesized, 3 = power base / strict operand
std::string p(const ex& e, int prec) {
    if (is_a<numeric>(e)) return fmt_number(GiNaC::ex_to<numeric>(e));
    if (is_a<GiNaC::symbol>(e)) return GiNaC::ex_to<GiNaC::symbol>(e).get_name();

    if (is_a<GiNaC::add>(e)) {
        std::string out;
        for (size_t i = 0; i < e.nops(); ++i) {
            std::string ts = p(e.op(i), 1);
            bool neg = !ts.empty() && ts[0] == '-';
            if (neg) ts.erase(0, 1);
            if (i == 0)
                out += (neg ? "-" : "") + ts;
            else
                out += (neg ? " - " : " + ") + ts;
        }
        if (out.empty()) out = "0";
        if (prec >= 2) out = "(" + out + ")";
        return out;
    }

    if (is_a<GiNaC::mul>(e)) {
        bool negative = false;
        std::string coeff;
        std::vector<ex> nums, dens;

        for (size_t i = 0; i < e.nops(); ++i) {
            const ex& f = e.op(i);
            if (is_a<numeric>(f)) {
                numeric n = GiNaC::ex_to<numeric>(f);
                if (n.is_negative()) {
                    negative = !negative;
                    n = -n;
                }
                if (!n.is_equal(numeric(1))) coeff = ginac_str(n);
            } else if (is_a<GiNaC::power>(f)) {
                const ex& xp = f.op(1);
                if (is_a<numeric>(xp) && GiNaC::ex_to<numeric>(xp).is_negative())
                    dens.push_back(GiNaC::pow(f.op(0), -xp));
                else
                    nums.push_back(f);
            } else {
                nums.push_back(f);
            }
        }

        std::string num_str = coeff;
        for (const ex& f : nums) {
            std::string fs = p(f, 3);
            if (num_str.empty())
                num_str = fs;
            else
                num_str += "\xC2\xB7" + fs; // '·'
        }
        if (num_str.empty()) num_str = "1";

        std::string den_str;
        for (const ex& f : dens) {
            std::string fs = p(f, 0);
            if (den_str.empty())
                den_str = fs;
            else
                den_str += "\xC2\xB7" + fs;
        }

        std::string out = negative ? "-" : "";
        out += num_str;
        if (!den_str.empty()) {
            bool wrap = dens.size() > 1 || is_a<GiNaC::add>(dens[0]);
            out += "/" + (wrap ? "(" + den_str + ")" : den_str);
        }
        if (prec >= 3) out = "(" + out + ")";
        return out;
    }

    if (is_a<GiNaC::power>(e)) {
        const ex& b = e.op(0);
        const ex& xp = e.op(1);
        if (is_a<numeric>(xp)) {
            numeric xn = GiNaC::ex_to<numeric>(xp);
            if (xn.is_negative()) {
                ex pos = GiNaC::pow(b, -xn);
                std::string ps = p(pos, 0);
                bool wrap = is_a<GiNaC::add>(pos) || is_a<GiNaC::mul>(pos);
                return "1/" + (wrap ? "(" + ps + ")" : ps);
            }
            if (xn.is_integer()) {
                long k = xn.to_int();
                if (k == 1) return p(b, prec);
                if (k >= 2 && k <= 9) {
                    std::string bs = p(b, 3);
                    return bs + sup(static_cast<int>(k));
                }
                return p(b, 3) + "^" + std::to_string(k);
            }
            if (xn.to_double() == 0.5) return "sqrt(" + p(b, 0) + ")";
            return p(b, 3) + "^(" + p(xp, 0) + ")";
        }
        return p(b, 3) + "^(" + p(xp, 0) + ")";
    }

    // functions (abs, sin, ...) and anything exotic: GiNaC default text
    return ginac_str(e);
}

} // namespace

std::string pretty(const ex& e) { return p(e, 0); }

std::string pretty_factor(const ex& e) {
    std::string s = p(e, 0);
    if (is_a<GiNaC::add>(e)) return "(" + s + ")";
    if (is_a<GiNaC::mul>(e) && !s.empty() && s[0] == '-') return "(" + s + ")";
    return s;
}

std::string pretty_product(const std::vector<ex>& factors) {
    std::string out;
    for (const ex& f : factors) {
        std::string fs = pretty_factor(f);
        if (fs == "1" && !out.empty()) continue;
        if (out.empty())
            out = fs;
        else
            out += "\xC2\xB7" + fs;
    }
    return out.empty() ? "1" : out;
}

std::string pretty_in_s(const ex& e, const ex& s) {
    ex pe = e.expand();
    if (!pe.has(s)) return p(pe, 0);

    int deg = 0;
    try {
        deg = pe.degree(s);
    } catch (...) {
        return p(pe, 0);
    }
    if (deg < 0 || deg > 64) return p(pe, 0);

    std::string out;
    bool first = true;
    // ascending powers: constant term first reads as "1 + s*R1*C1"
    for (int k = 0; k <= deg; ++k) {
        ex c = pe.coeff(s, k);
        if (c.is_zero()) continue;

        std::string body;
        bool neg = false;
        if (k == 0) {
            std::string cs = p(c, 1);
            neg = !cs.empty() && cs[0] == '-';
            if (neg) cs.erase(0, 1);
            body = cs;
        } else {
            std::string cs = p(c, 2);
            neg = !cs.empty() && cs[0] == '-';
            if (neg) cs.erase(0, 1);
            // the power belongs to s, not to the coefficient:
            //   k=1 -> "s*R1*C1",  k=2 -> "s^2*R1*C1"
            std::string spow = (k == 1) ? "s" : ("s" + sup(k));
            if (cs == "1")
                body = spow;
            else
                body = spow + "\xC2\xB7" + cs;
        }
        if (body.empty()) continue;
        if (first) {
            out += (neg ? "-" : "") + body;
            first = false;
        } else {
            out += (neg ? " - " : " + ") + body;
        }
    }
    return out.empty() ? "0" : out;
}

std::string pretty_ratio(const ex& num, const ex& den, const ex& s) {
    std::string ns = pretty_in_s(num, s);
    std::string ds = pretty_in_s(den, s);
    if (is_a<GiNaC::add>(den) || is_a<GiNaC::mul>(den)) ds = "(" + ds + ")";
    return ns + " / " + ds;
}

} // namespace syms
