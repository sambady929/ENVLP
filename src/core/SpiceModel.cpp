#include "core/SpiceModel.h"
#include "core/Eng.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace syms {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Read a numeric value that may carry a SPICE scale suffix (e.g. "1.8u",
// "3.3p", "0.5", "1e-7"). SPICE also allows trailing unit letters which we
// strip. Returns false when the token is not numeric.
bool spice_number(const std::string& tok, double& out) {
    std::string t = trim(tok);
    if (t.empty()) return false;
    // strip a trailing non-suffix unit like "V", "F" beyond the first letter
    // group; eng::parse_value already tolerates a leading number + suffix.
    return eng::parse_value(t, out);
}

} // namespace

std::vector<MosModel> parse_spice_models(const std::string& path,
                                         std::string& err) {
    err.clear();
    std::vector<MosModel> out;
    std::ifstream f(path);
    if (!f) {
        err = "cannot open model file: " + path;
        return out;
    }
    // Join continuation lines ('+') into logical cards.
    std::vector<std::string> cards;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '*') continue; // comment
        if (t[0] == '+') {
            if (!cards.empty()) cards.back() += " " + trim(t.substr(1));
            continue;
        }
        cards.push_back(t);
    }
    for (const auto& card : cards) {
        std::istringstream ss(card);
        std::string kw;
        ss >> kw;
        if (lower(kw) != ".model") continue;
        MosModel m;
        ss >> m.name;
        std::string type;
        ss >> type;
        std::string tl = lower(type);
        if (tl == "pmos") m.pmos = true;
        else if (tl == "nmos") m.pmos = false;
        else continue; // not a MOSFET model
        // key=value pairs (possibly key = value with spaces)
        std::string key;
        while (ss >> key) {
            std::string val;
            if (key.find('=') != std::string::npos) {
                size_t eq = key.find('=');
                if (eq + 1 < key.size()) {
                    val = key.substr(eq + 1); // value was attached
                } else {
                    ss >> val;
                }
                key = key.substr(0, eq);
            } else {
                ss >> val; // "-- value" form
            }
            std::string k = lower(key);
            double v = 0.0;
            if (k == "level") { v = spice_number(val, v) ? v : 1; m.level = int(v); }
            else if (k == "vto" || k == "vt0") { if (spice_number(val, v)) m.vto = v; }
            else if (k == "kp") { if (spice_number(val, v)) m.kp = v; }
            else if (k == "lambda") { if (spice_number(val, v)) m.lambda = v; }
            else if (k == "gamma") { if (spice_number(val, v)) m.gamma = v; }
            else if (k == "phi") { if (spice_number(val, v)) m.phi = v; }
            else if (k == "theta") { if (spice_number(val, v)) m.theta = v; }
            else if (k == "eta") { if (spice_number(val, v)) m.eta = v; }
            else if (k == "vmax") { if (spice_number(val, v)) m.vmax = v; }
            else if (k == "cgso") { if (spice_number(val, v)) m.cgso = v; }
            else if (k == "cgdo") { if (spice_number(val, v)) m.cgdo = v; }
            else if (k == "cj") { if (spice_number(val, v)) m.cj = v; }
            else if (k == "cjsw") { if (spice_number(val, v)) m.cjsw = v; }
            else if (k == "tox") { if (spice_number(val, v)) m.tox = v; }
        }
        out.push_back(m);
    }
    if (out.empty()) err = "no .model NMOS/PMOS cards found in " + path;
    return out;
}

const MosModel* find_model(const std::vector<MosModel>& models,
                           const std::string& name) {
    std::string want = lower(name);
    for (const auto& m : models)
        if (lower(m.name) == want) return &m;
    return nullptr;
}

} // namespace syms
