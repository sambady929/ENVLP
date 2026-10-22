// Symbol artwork as path data, for the browser canvas.
//
// Symbols.cpp draws every symbol through SymbolTarget; this target turns those
// calls into SVG path strings (which a browser's Path2D understands directly),
// grouped by stroke width, so the web editor renders exactly the same art as
// the desktop. A Ctx with an identity placement is used, so the coordinates are
// the symbol's own frame and the page only has to translate/rotate/scale.
#include "app/SymbolTarget.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string n(double v) {
    if (!std::isfinite(v)) v = 0;
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", v);
    // trim trailing zeros ("12.500" -> "12.5", "12.000" -> "12")
    std::string s(b);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o + "\"";
}

const char* dup(const std::string& s) {
    char* p = (char*)std::malloc(s.size() + 1);
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

struct PathTarget : envlp::SymbolTarget {
    std::string cur;
    double width = 1.6;
    std::vector<std::pair<std::string, double>> strokes;
    std::vector<std::string> fills;
    std::vector<std::string> texts;

    void flush() {
        if (!cur.empty()) {
            strokes.push_back({cur, width});
            cur.clear();
        }
    }
    void set_width(double w) override {
        flush();
        width = w;
    }
    void set_colour(envlp::Rgb) override {}
    void set_emphasis(bool on) override { bold = on; }
    bool bold = false;

    void line(double x0, double y0, double x1, double y1) override {
        cur += "M" + n(x0) + " " + n(y0) + "L" + n(x1) + " " + n(y1);
    }
    void fill_poly(const std::vector<envlp::Pt>& pts) override {
        if (pts.size() < 3) return;
        flush();
        std::string d = "M" + n(pts[0].first) + " " + n(pts[0].second);
        for (size_t i = 1; i < pts.size(); ++i)
            d += "L" + n(pts[i].first) + " " + n(pts[i].second);
        d += "Z";
        fills.push_back(d);
    }
    void circle(double x, double y, double r) override {
        cur += "M" + n(x - r) + " " + n(y) + "A" + n(r) + " " + n(r) +
               " 0 1 0 " + n(x + r) + " " + n(y) + "A" + n(r) + " " + n(r) +
               " 0 1 0 " + n(x - r) + " " + n(y);
    }
    void text(const std::string& s, double x, double y, bool centered) override {
        texts.push_back("{\"s\":" + jstr(s) + ",\"x\":" + n(x) + ",\"y\":" + n(y) +
                        ",\"c\":" + (centered ? "true" : "false") + ",\"b\":" +
                        (bold ? "true" : "false") + "}");
    }
};

} // namespace

extern "C" {

// Every placeable kind's artwork: {"R":{"stroke":[{"d":..,"w":..}],"fill":[..],
// "text":[{...}]}, ...}
const char* envlp_symbol_art() {
    static const syms::Kind kinds[] = {
        syms::Kind::R,       syms::Kind::C,       syms::Kind::L,
        syms::Kind::V,       syms::Kind::I,       syms::Kind::D,
        syms::Kind::NMOS,    syms::Kind::PMOS,    syms::Kind::NPN,
        syms::Kind::PNP,     syms::Kind::GND,     syms::Kind::VDD,
        syms::Kind::E,       syms::Kind::G,       syms::Kind::OPAMP,
        syms::Kind::OTA,     syms::Kind::FDOPAMP, syms::Kind::AMP,
        syms::Kind::GM,      syms::Kind::T,       syms::Kind::K,
        syms::Kind::NULLOR,  syms::Kind::IS,      syms::Kind::SBLK,
        syms::Kind::CCCS,    syms::Kind::CCVS};
    std::string js = "{\"ok\":true,\"kinds\":{";
    bool first = true;
    for (syms::Kind k : kinds) {
        PathTarget t;
        envlp::draw_body_kind(t, k);
        t.flush();
        if (!first) js += ",";
        first = false;
        js += jstr(syms::kind_token(k)) + ":{\"stroke\":[";
        for (size_t i = 0; i < t.strokes.size(); ++i) {
            if (i) js += ",";
            js += "{\"d\":" + jstr(t.strokes[i].first) + ",\"w\":" +
                  n(t.strokes[i].second) + "}";
        }
        js += "],\"fill\":[";
        for (size_t i = 0; i < t.fills.size(); ++i) {
            if (i) js += ",";
            js += jstr(t.fills[i]);
        }
        js += "],\"text\":[" + [](const std::vector<std::string>& v) {
            std::string s;
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) s += ",";
                s += v[i];
            }
            return s;
        }(t.texts) + "]}";
    }
    js += "}}";
    return dup(js);
}

} // extern "C"

