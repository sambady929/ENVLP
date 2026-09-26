// A small, self-contained LaTeX-subset -> HTML+CSS renderer.
//
// The engine produces a narrow LaTeX dialect (GiNaC's print_latex plus the
// \parallel extension), so a full TeX parser is unnecessary. We translate the
// handful of constructs that matter:
//   \frac{a}{b}          -> stack a over b with a horizontal rule
//   x_{ab}^{cd}          -> <sub>/<sup>
//   \left( x \right)     -> bracket glyphs (sized by CSS)
//   \cdot \parallel ...  -> the right Unicode glyph
// Everything else is emitted as text so nothing is never lost.
//
// GiNaC's print_latex does NOT wrap multi-char script args in braces, so
// identifiers like ro_M1 come out as the literal string `ro_M1`; we wrap
// them with `latex_normalize` before rendering so the script picks up all of
// `M1`, not just `M`.
#include "LatexRender.h"

#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace symcirc {
namespace {

// --- command/delimiter tables ---------------------------------------------

std::string html_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

// Unicode replacement for a LaTeX control word (without the backslash).
// Returns false when we don't recognise it (caller decides the fallback).
bool symbol_for(const std::string& name, std::string& out) {
    static const std::map<std::string, std::string> m = {
        {"cdot", "&middot;"},   {"times", "&times;"},  {"parallel", "&#8741;"},
        {"varepsilon", "&epsilon;"}, {"zeta", "&zeta;"},
        {"pm", "&plusmn;"},     {"mp", "&#8723;"},     {"leq", "&le;"},
        {"le", "&le;"},         {"geq", "&ge;"},       {"ge", "&ge;"},
        {"neq", "&ne;"},        {"ne", "&ne;"},        {"approx", "&asymp;"},
        {"infty", "&infin;"},   {"pi", "&pi;"},        {"alpha", "&alpha;"},
        {"beta", "&beta;"},     {"gamma", "&gamma;"},  {"delta", "&delta;"},
        {"epsilon", "&epsilon;"}, {"mu", "&mu;"},      {"omega", "&omega;"},
        {"tau", "&tau;"},       {"phi", "&phi;"},      {"rho", "&rho;"},
        {"sigma", "&sigma;"},   {"omega", "&omega;"},  {"Omega", "&Omega;"},
        {"Delta", "&Delta;"},   {"Gamma", "&Gamma;"},  {"Sigma", "&Sigma;"},
        {"Phi", "&Phi;"},       {"Theta", "&Theta;"},  {"lambda", "&lambda;"},
        {"to", "&rarr;"},       {"rightarrow", "&rarr;"},
        {"leftarrow", "&larr;"},{"cdots", "&middot;&middot;&middot;"},
        {"ldots", "&#8230;"},
        {"circ", "&deg;"},      {"degree", "&deg;"},
        {"infty", "&infin;"},   {"partial", "&part;"},
        {",", "&thinsp;"},      {";", "&thinsp;"},     {" ", "&nbsp;"},
        {"quad", "&emsp;"},     {"qquad", "&emsp;&emsp;"},
        {"!", ""},
    };
    auto it = m.find(name);
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

// Is this character a bare token that a script can attach to?
bool is_script_atom(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == ')' || c == ']' || c == '}';
}

class Renderer {
public:
    explicit Renderer(const std::string& src) : s_(src) {}

    std::string run() {
        i_ = 0;
        return render_group(false);
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    bool eof() const { return i_ >= s_.size(); }

    // Render until '}' (consume it) or end-of-input when stop==false.
    std::string render_group(bool stop_at_brace) {
        std::string out;
        while (!eof()) {
            char c = s_[i_];
            if (c == '}') {
                if (stop_at_brace) { ++i_; return out; }
                ++i_; // stray brace: skip
                continue;
            }
            if (c == '{') { ++i_; out += render_group(true); continue; }
            if (c == '\\') { out += render_command(); continue; }
            if (c == '_' || c == '^') { out += render_script(c); continue; }
            if (c == '&') { out += "&amp;"; ++i_; continue; }
            if (c == '<') { out += "&lt;"; ++i_; continue; }
            if (c == '>') { out += "&gt;"; ++i_; continue; }
            if (c == '~') { out += "&nbsp;"; ++i_; continue; }
            out += c;
            ++i_;
        }
        return out;
    }

    // A single "atom" for a script: a {group} or one token.
    std::string render_atom() {
        if (eof()) return "";
        if (s_[i_] == '{') { ++i_; return render_group(true); }
        // a control word counts as one atom
        if (s_[i_] == '\\') return render_command();
        std::string out(1, s_[i_]);
        ++i_;
        return out;
    }

    std::string render_script(char kind) {
        ++i_; // consume _ or ^
        std::string arg = render_atom();
        if (kind == '_') return "<sub>" + arg + "</sub>";
        return "<sup>" + arg + "</sup>";
    }

    // Read a balanced {..} argument as raw LaTeX (for \frac, \sqrt...).
    // The braces themselves are not part of the returned text.
    std::string read_raw_arg() {
        if (eof()) return "";
        if (s_[i_] != '{') {
            // single token
            std::string out(1, s_[i_]);
            ++i_;
            return out;
        }
        ++i_; // skip the opening '{'
        int depth = 1;
        std::string out;
        while (!eof()) {
            char c = s_[i_];
            if (c == '{') { ++depth; out += c; ++i_; }
            else if (c == '}') {
                --depth;
                if (depth == 0) { ++i_; break; }
                out += c;
                ++i_;
            } else { out += c; ++i_; }
        }
        return out;
    }

    std::string read_control_word() {
        // assumes s_[i_] == '\\'; returns the word without the backslash
        ++i_;
        std::string w;
        while (!eof() && ((s_[i_] >= 'a' && s_[i_] <= 'z') ||
                          (s_[i_] >= 'A' && s_[i_] <= 'Z'))) {
            w += s_[i_];
            ++i_;
        }
        return w;
    }

    std::string render_command() {
        size_t save = i_;
        std::string w = read_control_word();
        if (w.empty()) {
            // escaped punctuation like \{ \} \% \, handled via symbol table
            if (eof()) return "";
            std::string one(1, s_[i_]);
            ++i_;
            std::string sym;
            if (symbol_for(one, sym)) return sym;
            return html_escape(one);
        }

        if (w == "frac") {
            std::string a = read_raw_arg();
            std::string b = read_raw_arg();
            return "<span class=\"frac\"><span class=\"num\">" +
                   render_tex(a) + "</span><span class=\"den\">" +
                   render_tex(b) + "</span></span>";
        }
        if (w == "sqrt") {
            std::string a = read_raw_arg();
            return "&radic;<span class=\"overline\">" + render_tex(a) +
                   "</span>";
        }
        if (w == "mathrm" || w == "text" || w == "operatorname" ||
            w == "mathbf" || w == "mathit") {
            std::string a = read_raw_arg();
            return "<span class=\"" + w + "\">" + render_tex(a) + "</span>";
        }
        if (w == "left" || w == "right") {
            if (eof()) return "";
            char d = s_[i_];
            ++i_;
            std::string sym;
            if (symbol_for(std::string(1, d), sym)) return sym;
            if (d == '.') return "";
            return html_escape(std::string(1, d));
        }
        if (w == "begin" || w == "end") {
            // environments aren't used by the engine; drop the argument
            read_raw_arg();
            return "";
        }

        std::string sym;
        if (symbol_for(w, sym)) return sym;

        // Unknown control word: show its name (never lose information).
        i_ = save;
        ++i_; // skip backslash
        std::string raw;
        while (!eof() && s_[i_] != ' ' && s_[i_] != '{' && s_[i_] != '}' &&
               s_[i_] != '_' && s_[i_] != '^')
            raw += s_[i_++];
        return html_escape(raw);
    }

    // Re-render a raw LaTeX chunk (used for nested arguments).
    std::string render_tex(const std::string& tex) {
        Renderer sub(tex);
        return sub.run();
    }
};

const char* kStyle =
    "html, body { margin: 0; padding: 12px 16px; background: #ffffff;"
    "  color: #10141a; font-family: 'Segoe UI', sans-serif; }"
    ".math { font-family: 'Cambria Math', 'Latin Modern Math', "
    "        'Times New Roman', serif; font-size: 19px; line-height: 2.1; }"
    ".line { margin: 10px 0; }"
    ".lhs { color: #0b3d91; }"
    ".frac { display: inline-block; vertical-align: middle; text-align: "
    "        center; margin: 0 3px; }"
    ".frac > .num { display: block; padding: 0 4px 1px 4px;"
    "               border-bottom: 1.4px solid #10141a; }"
    ".frac > .den { display: block; padding: 1px 4px 0 4px; }"
    "sub, sup { font-size: 72%; }"
    ".overline { border-top: 1.3px solid #10141a; padding-top: 1px; }"
    ".mathrm, .text { font-style: normal; }"
    ".mathit { font-style: italic; }"
    ".label { color: #55606e; font-family: 'Segoe UI', sans-serif;"
    "         font-size: 12px; }";

} // namespace

// Normalize the engine's LaTeX before rendering. Two jobs:
//
//  1. GiNaC prints an identifier like `ro_M1` as the literal string
//     `ro_M1` -- `_M1` is a single token whose subscript body is two
//     characters. TeX would typeset that as `<sub>M</sub>1`; we wrap it as
//     `ro_{M1}` so the whole token lands in the subscript.
//
//  2. Brace groups must be normalized *recursively*: `\frac{ R1 ro_M1}{...}`
//     carries the same `ro_M1` inside the numerator group, so copying the
//     group verbatim leaves it broken. We walk into every group.
//
// An unbraced script argument runs only over ASCII alphanumerics -- so
// `Cds_M1))` becomes `Cds_{M1})` (the `))` stays outside the subscript), and
// `ro_M1+` becomes `ro_{M1}+`. A leading backslash is a control word
// (`\cdot`, `\parallel`) and is copied as a single atom.
std::string latex_normalize(const std::string& latex) {
    auto is_name_char = [](unsigned char c) { return std::isalnum(c) != 0; };
    std::string out;
    out.reserve(latex.size());
    size_t i = 0;
    while (i < latex.size()) {
        char c = latex[i];
        if (c == '\\') {
            // Control word: backslash + letters. \left( etc. leave the
            // following delimiter to the main loop.
            size_t j = i + 1;
            while (j < latex.size() &&
                   std::isalpha((unsigned char)latex[j]))
                ++j;
            out.append(latex, i, j - i);
            i = j;
            if (i < latex.size() && latex[i] != '\\' &&
                !std::isalpha((unsigned char)latex[i]) &&
                latex[i] != '{' && latex[i] != '}' &&
                latex[i] != '_' && latex[i] != '^') {
                // \left( and friends: the delimiter is a separate token and
                // must not be swallowed by anything. Just let it fall through.
            }
            continue;
        }
        if (c == '{') {
            // Balanced group: normalize the inside, keep the braces.
            int depth = 1;
            size_t j = i + 1;
            while (j < latex.size() && depth > 0) {
                if (latex[j] == '{') ++depth;
                else if (latex[j] == '}') { --depth; if (depth == 0) break; }
                ++j;
            }
            std::string inner = latex.substr(i + 1, j - (i + 1));
            out += '{';
            out += latex_normalize(inner);
            out += '}';
            i = (j < latex.size()) ? j + 1 : latex.size();
            continue;
        }
        if (c == '_' || c == '^') {
            out += c;
            ++i;
            if (i >= latex.size()) break;
            if (latex[i] == '{') {
                // Already braced: recurse into it on the next iteration.
                continue;
            }
            if (latex[i] == '\\') {
                // Control-word atom (e.g. `^\circ`).
                size_t j = i + 1;
                while (j < latex.size() &&
                       std::isalpha((unsigned char)latex[j]))
                    ++j;
                out.append(latex, i, j - i);
                i = j;
                continue;
            }
            // Unbraced argument: run of ASCII alphanumerics.
            size_t j = i;
            while (j < latex.size() &&
                   is_name_char((unsigned char)latex[j]))
                ++j;
            size_t len = j - i;
            if (len == 0) {
                // Stray `_` at a boundary: emit and move on.
                if (i < latex.size()) out += latex[i++];
                continue;
            }
            if (len == 1) {
                out += latex[i];
            } else {
                out += '{';
                out.append(latex, i, len);
                out += '}';
            }
            i = j;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

std::string latex_to_html_fragment(const std::string& latex) {
    // Split on newlines; render each as its own display line. The first
    // pass wraps multi-char script args in {} so typesetters parse "M1" as a
    // single subscript instead of one character.
    std::string norm = latex_normalize(latex);
    std::string out = "<div class=\"math\">";
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        out += "<div class=\"line\">" + Renderer(cur).run() + "</div>";
        cur.clear();
    };
    for (char c : norm) {
        if (c == '\n') flush();
        else cur += c;
    }
    flush();
    out += "</div>";
    return out;
}

std::string latex_render_line(const std::string& latex) {
    // Same per-line output as latex_to_html_fragment, without the
    // surrounding <div class="math"> wrapper and without newline splitting.
    return Renderer(latex_normalize(latex)).run();
}

std::string latex_to_html(const std::string& latex) {
    std::string body = latex_to_html_fragment(latex);
    std::string html;
    html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
    html += "<style>";
    html += kStyle;
    html += "</style></head><body>";
    html += body;
    html += "</body></html>";
    return html;
}

} // namespace symcirc
