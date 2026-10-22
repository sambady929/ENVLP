// The browser-facing entry point: a tiny C ABI over the analysis engine.
//
// The web build compiles src/core plus the .scx loader (Document / SymbolGeom)
// to WebAssembly and exposes exactly these two functions; the page calls
// envlp_run() with the schematic text plus a flat options object and gets a
// JSON result back. Nothing here depends on wxWidgets, so the same translation
// unit also builds natively (see tests/test_web_api.cpp).
#include "app/Document.h"
#include "core/Analysis.h"
#include "core/Engine.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// --- a minimal flat JSON reader -------------------------------------------
//
// The options object is flat: string / number / boolean values only. A full
// parser would be dead weight here.
std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            switch (c) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                default: o += c; break;
            }
        } else {
            o += s[i];
        }
    }
    return o;
}

std::map<std::string, std::string> parse_flat_json(const std::string& j) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    auto skip_ws = [&] { while (i < j.size() && std::isspace((unsigned char)j[i])) ++i; };
    skip_ws();
    if (i >= j.size() || j[i] != '{') return out;
    ++i;
    while (i < j.size()) {
        skip_ws();
        if (i < j.size() && j[i] == '}') break;
        if (i >= j.size() || j[i] != '"') { ++i; continue; }
        std::string key;
        ++i;
        while (i < j.size() && j[i] != '"') {
            if (j[i] == '\\' && i + 1 < j.size()) { key += j[i]; key += j[i + 1]; i += 2; continue; }
            key += j[i++];
        }
        ++i; // closing quote
        skip_ws();
        if (i < j.size() && j[i] == ':') ++i;
        skip_ws();
        std::string val;
        if (i < j.size() && j[i] == '"') {
            ++i;
            while (i < j.size() && j[i] != '"') {
                if (j[i] == '\\' && i + 1 < j.size()) { val += j[i]; val += j[i + 1]; i += 2; continue; }
                val += j[i++];
            }
            ++i;
            val = unescape(val);
        } else {
            while (i < j.size() && j[i] != ',' && j[i] != '}') val += j[i++];
        }
        out[unescape(key)] = val;
        skip_ws();
        if (i < j.size() && j[i] == ',') ++i;
    }
    return out;
}

std::string get_s(const std::map<std::string, std::string>& m,
                  const std::string& k, const std::string& d = "") {
    auto it = m.find(k);
    return it == m.end() ? d : it->second;
}
double get_n(const std::map<std::string, std::string>& m, const std::string& k,
             double d) {
    auto it = m.find(k);
    if (it == m.end() || it->second.empty()) return d;
    try {
        return std::stod(it->second);
    } catch (...) {
        return d;
    }
}
bool get_b(const std::map<std::string, std::string>& m, const std::string& k,
           bool d) {
    auto it = m.find(k);
    if (it == m.end()) return d;
    const std::string& v = it->second;
    return v == "true" || v == "1" || v == "yes";
}

// --- JSON output ----------------------------------------------------------
std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o + "\"";
}
std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    char b[40];
    std::snprintf(b, sizeof(b), "%.9g", v);
    return b;
}
std::string jarr(const std::vector<double>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += jnum(v[i]);
    }
    return s + "]";
}

// Continuous phase unwrap (degrees in, degrees out).
std::vector<double> unwrap(const std::vector<double>& in) {
    std::vector<double> out(in.size(), 0.0);
    if (in.empty()) return out;
    out[0] = in[0];
    double prev = in[0];
    for (size_t i = 1; i < in.size(); ++i) {
        double cur = in[i], d = cur - prev;
        while (d > 180.0) { cur -= 360.0; d -= 360.0; }
        while (d < -180.0) { cur += 360.0; d += 360.0; }
        out[i] = cur;
        prev = cur;
    }
    return out;
}

syms::AnalysisKind kind_from(const std::string& s) {
    using K = syms::AnalysisKind;
    if (s == "AC") return K::AC;
    if (s == "DC") return K::DC;
    if (s == "PSRR") return K::PSRR;
    if (s == "LoopGain") return K::LoopGain;
    if (s == "ShortCircuitCurrent") return K::ShortCircuitCurrent;
    if (s == "InputImpedance") return K::InputImpedance;
    if (s == "OutputImpedance") return K::OutputImpedance;
    if (s == "Noise") return K::Noise;
    if (s == "Differential") return K::Differential;
    return K::TransferFunction;
}

} // namespace

extern "C" {

const char* envlp_run(const char* scx_text, const char* options_json) {
    std::string out;
    auto fail = [&](const std::string& msg) -> const char* {
        std::string r = "{\"ok\":false,\"error\":" + jstr(msg) + "}";
        char* p = (char*)std::malloc(r.size() + 1);
        std::memcpy(p, r.c_str(), r.size() + 1);
        return p;
    };
    if (!scx_text) return fail("no schematic text");

    std::map<std::string, std::string> o = parse_flat_json(options_json ? options_json : "{}");

    envlp::Document doc;
    std::string err;
    if (!doc.deserialize(std::string(scx_text), err))
        return fail(err.empty() ? std::string("could not read the schematic") : err);

    syms::Circuit c = doc.resolved(err);
    if (!err.empty()) return fail(err);

    syms::AnalysisSpec sp;
    sp.kind = kind_from(get_s(o, "kind", "TransferFunction"));
    sp.input_ref = get_s(o, "input");
    sp.output = get_s(o, "output");
    sp.input_port_p = get_s(o, "port_p");
    sp.input_port_n = get_s(o, "port_n");
    sp.probe_ref = get_s(o, "probe");
    sp.sweep.f_start_hz = get_n(o, "f0", 1.0);
    sp.sweep.f_stop_hz = get_n(o, "f1", 1e9);
    sp.sweep.points_per_interval = int(get_n(o, "points", 10));
    if (get_s(o, "sweep", "decade") == "octave") sp.sweep.type = syms::SweepType::Octave;
    else if (get_s(o, "sweep") == "linear") sp.sweep.type = syms::SweepType::Linear;
    sp.f0_hz = get_n(o, "f_tune", 1000.0);
    sp.tech.vthn = get_n(o, "vthn", 0.6);
    sp.tech.vthp = get_n(o, "vthp", 0.6);
    sp.tech.uncox = get_n(o, "uncox", 200e-6);
    sp.tech.upcox = get_n(o, "upcox", 100e-6);
    sp.tech.vdd = get_n(o, "vdd", 5.0);
    sp.tech.dc_mode = (get_n(o, "dc_mode", 1) == 2) ? syms::DcMode::SquareLaw
                                                    : syms::DcMode::GmOverId;
    sp.tech.assume_saturation = get_b(o, "assume_saturation", true);
    sp.tech.include_clm = get_b(o, "include_clm", false);
    sp.prune = get_b(o, "prune", true);
    sp.component_threshold_ratio = get_n(o, "comp_ratio", 10.0);
    sp.pole_zero_threshold_ratio = get_n(o, "pz_ratio", 1000.0);
    // Which pole/zero the pruning threshold is judged against: the dominant
    // one, or the unity-gain bandwidth.
    sp.pole_ref = (get_s(o, "pole_ref", "dominant") == "ugbw")
                      ? syms::AnalysisSpec::PoleRef::UgBw
                      : syms::AnalysisSpec::PoleRef::Dominant;
    sp.global_ref = get_b(o, "global_ref", false);

    syms::CardResult cr;
    try {
        cr = syms::run_analysis(c, sp);
    } catch (const std::exception& e) {
        return fail(e.what());
    }

    // The plotted curve, when the card produced a transfer function.
    std::vector<double> pf, pmag, pph;
    if (cr.has_transfer && !cr.transfer.has_noise) {
        std::vector<double> fr = syms::sweep_points(sp.sweep);
        std::vector<double> raw;
        for (double f : fr) {
            double w = 2.0 * M_PI * f;
            try {
                pf.push_back(f);
                pmag.push_back(syms::mag_db_at(cr.transfer, w));
                raw.push_back(syms::phase_deg_at(cr.transfer, w));
            } catch (...) {
                pf.pop_back();
            }
        }
        pph = unwrap(raw);
    } else if (cr.transfer.has_noise) {
        pf = cr.transfer.noise_f_hz;
        for (double v : cr.transfer.noise_vout)
            pmag.push_back(20.0 * std::log10(v > 0.0 ? v : 1e-30));
    }

    std::ostringstream js;
    js << "{\"ok\":true";
    js << ",\"title\":" << jstr(cr.title);
    js << ",\"summary\":" << jstr(cr.summary);
    js << ",\"text\":" << jstr(cr.text);
    js << ",\"latex\":" << jstr(cr.latex);
    js << ",\"report\":" << jstr(cr.report);
    js << ",\"latex_report\":" << jstr(cr.latex_report);
    js << ",\"has_transfer\":" << (cr.has_transfer ? "true" : "false");
    js << ",\"kind\":" << jstr(get_s(o, "kind", "TransferFunction"));
    js << ",\"values\":[";
    for (size_t i = 0; i < cr.values.size(); ++i) {
        if (i) js << ",";
        js << "[" << jstr(cr.values[i].first) << "," << jstr(cr.values[i].second)
           << "]";
    }
    js << "]";
    js << ",\"plot\":{\"f\":" << jarr(pf) << ",\"mag\":" << jarr(pmag)
       << ",\"phase\":" << jarr(pph) << "}";
    js << "}";
    out = js.str();

    char* p = (char*)std::malloc(out.size() + 1);
    std::memcpy(p, out.c_str(), out.size() + 1);
    return p;
}

void envlp_free(const char* p) { std::free((void*)p); }

} // extern "C"
