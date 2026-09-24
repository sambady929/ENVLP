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

// Find the extent of a single script argument following a `_` or `^` at
// position `i` in `s`. Returns the number of characters the script arg spans.
//   "{ab cd}"      -> balanced braces, count to the matching `}`
//   "\name"        -> the backslash command name (letters only)
//   "M1"           -> one token: all letters/digits/UTF-8 continuation
//                    bytes until a non-identifier char (space, comma, brace,
//                    the next `_`/`^`, another `\\` command, etc.)
//   "_M1 ^2 \cdot" -> one token each, since each starts at i and runs to
//                    the next script delimiter
static size_t script_arg_len(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    if (s[i] == '{') {
        int depth = 1;
        for (size_t k = i + 1; k < s.size(); ++k) {
            if (s[k] == '{') ++depth;
            else if (s[k] == '}') { --depth; if (depth == 0) return k + 1 - i; }
        }
        return s.size() - i;
    }
    if (s[i] == '\\') {
        size_t k = i + 1;
        while (k < s.size() && ((s[k] >= 'a' && s[k] <= 'z') ||
                                (s[k] >= 'A' && s[k] <= 'Z')))
            ++k;
        return k - i;
    }
    // Run until a delimiter: end of string, `_`/`^` (next script), `{`/`}`
    // (group), `\\` (next control word), or whitespace. Multi-byte UTF-8
    // characters (≥0x80) are *not* extended through -- they belong to the
    // previous ASCII identifier or stand on their own (e.g. the `·` emitted
    // by \cdot would already be a `\cdot` command; a bare UTF-8 char is
    // a single visible glyph and shouldn't swallow the following tokens).
    size_t k = i;
    while (k < s.size()) {
        unsigned char c = (unsigned char)s[k];
        if (c == '_' || c == '^' || c == '{' || c == '}' || c == '\\') break;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        if (c >= 0x80) break;
        ++k;
    }
    return k - i;
}

std::string latex_normalize(const std::string& latex) {
    std::string out;
    out.reserve(latex.size());
    size_t i = 0;
    while (i < latex.size()) {
        char c = latex[i];
        // Skip past control words when scanning -- they can contain braces too.
        if (c == '\\') {
            size_t j = i + 1;
            while (j < latex.size() && ((latex[j] >= 'a' && latex[j] <= 'z') ||
                                       (latex[j] >= 'A' && latex[j] <= 'Z')))
                ++j;
            out.append(latex, i, j - i);
            i = j;
            continue;
        }
        if (c == '{') {
            // Copy balanced braces verbatim.
            int depth = 1;
            out += '{';
            ++i;
            while (i < latex.size() && depth > 0) {
                if (latex[i] == '{') ++depth;
                else if (latex[i] == '}') { --depth; }
                out += latex[i++];
            }
            continue;
        }
        if ((c == '_' || c == '^') && i + 1 < latex.size()) {
            // The script argument: a single character (no braces), a single
            // command starting with \, or a balanced brace group. Anything
            // longer than one *character* needs to be wrapped in {} so the
            // typesetter doesn't split it into one sub/super char + plain.
            out += c;
            size_t alen = script_arg_len(latex, i + 1);
            char first = latex[i + 1];
            bool is_braced = first == '{';
            bool single_char = !is_braced && alen == 1;
            bool single_token = !is_braced && alen > 1 &&
                                (first == '\\' || !std::isalnum((unsigned char)first));
            // Wrap when the argument is multiple characters that aren't
            // already a brace group or a single TeX token (control word).
            if (!is_braced && !single_char && !single_token) {
                out += '{';
                out.append(latex, i + 1, alen);
                out += '}';
                i += 1 + alen;
                continue;
            }
            // Otherwise (single char, single control word, or already braced)
            // copy verbatim.
            out.append(latex, i + 1, alen);
            i += 1 + alen;
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
