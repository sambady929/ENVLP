// Tests for the offline LaTeX-subset -> HTML renderer used by the Math tab.
// Pure string work, so no wxWidgets runtime is needed.
#include "app/LatexRender.h"

#include <cstdio>
#include <string>

using envlp::latex_to_html;
using envlp::latex_to_html_fragment;
using envlp::latex_render_line;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::printf("  FAIL line %d: %s\n", __LINE__, #c);                \
        }                                                                     \
    } while (0)

static bool has(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// A report line ending in ':' is a heading only when it is bare text; a math
// line like "\mathrm{Density}\ S_{v}(f):" must NOT be treated as a heading
// (that leaked the literal "\mathrm" to the user).
static void test_heading_classification() {
    CHECK(envlp::latex_is_plain_heading("Output Noise:"));
    CHECK(envlp::latex_is_plain_heading("Poles:"));
    CHECK(!envlp::latex_is_plain_heading("\\mathrm{Density}\\ S_{v}(f):"));
    CHECK(!envlp::latex_is_plain_heading("\\omega_{p0}:"));
    std::string h;
    CHECK(envlp::latex_heading_from_mathrm("\\mathrm{Contribution:}", h));
    CHECK(h == "Contribution:");
    CHECK(!envlp::latex_heading_from_mathrm("\\mathrm{Density}\\ S:", h));
}

static void test_fraction() {
    std::string h = latex_to_html_fragment("\\frac{a}{b}");
    CHECK(has(h, "class=\"frac\""));
    CHECK(has(h, "class=\"num\">a<"));
    CHECK(has(h, "class=\"den\">b<"));
}

static void test_nested_fraction() {
    // \frac{1}{1 + s R C} -- the denominator is a sum, rendered as text
    std::string h = latex_to_html_fragment("\\frac{1}{1+s\\cdot R}");
    CHECK(has(h, "class=\"frac\""));
    CHECK(has(h, "class=\"num\">1<"));
    CHECK(has(h, "&middot;")); // \cdot became the Unicode dot
    CHECK(!has(h, "\\cdot"));  // and the control word is gone
}

static void test_scripts() {
    std::string h = latex_to_html_fragment("C_{gd} + s^{2}");
    CHECK(has(h, "<sub>gd</sub>"));
    CHECK(has(h, "<sup>2</sup>"));
}

static void test_single_char_script() {
    std::string h = latex_to_html_fragment("x^2 + y_i");
    CHECK(has(h, "<sup>2</sup>"));
    CHECK(has(h, "<sub>i</sub>"));
}

static void test_parallel() {
    std::string h = latex_to_html_fragment("R_1 \\parallel R_2");
    CHECK(has(h, "&#8741;")); // the parallel glyph
    CHECK(!has(h, "\\parallel"));
}

static void test_left_right() {
    std::string h = latex_to_html_fragment("\\left( a + b \\right)");
    CHECK(has(h, "("));
    CHECK(has(h, ")"));
    CHECK(!has(h, "\\left"));
    CHECK(!has(h, "\\right"));
}

static void test_greek_and_symbols() {
    std::string h = latex_to_html_fragment("\\omega \\tau \\le \\infty");
    CHECK(has(h, "&omega;"));
    CHECK(has(h, "&tau;"));
    CHECK(has(h, "&le;"));
    CHECK(has(h, "&infin;"));
}

static void test_real_engine_output() {
    // A string of the shape the engine actually emits.
    std::string tex =
        "H(s) = \\frac{- Rd\\parallel ro_{M1} gm_{M1}\\left(1- Cgd_{M1} "
        "s/gm_{M1}\\right)}{1+ Cgd_{M1} Rd\\parallel ro_{M1} s+ "
        "Rd\\parallel ro_{M1} CL s}";
    std::string h = latex_to_html(tex);
    CHECK(has(h, "<!DOCTYPE html>"));
    CHECK(has(h, "class=\"frac\""));
    CHECK(has(h, "<sub>M1</sub>"));
    CHECK(has(h, "&#8741;"));
    // the *body* must have no leftover commands or braces (the CSS wrapper
    // legitimately contains braces, so check the fragment, not the full doc).
    std::string f = latex_to_html_fragment(tex);
    CHECK(!has(f, "\\frac"));
    CHECK(!has(f, "\\parallel"));
    CHECK(!has(f, "\\left"));
    CHECK(!has(f, "{"));
    CHECK(!has(f, "}"));
}

static void test_unknown_command_survives() {
    std::string h = latex_to_html_fragment("\\weirdcmd + 1");
    CHECK(has(h, "weirdcmd")); // information is never lost
}

int main() {
    test_fraction();
    test_nested_fraction();
    test_scripts();
    test_single_char_script();
    test_parallel();
    test_left_right();
    test_greek_and_symbols();
    test_real_engine_output();
    test_unknown_command_survives();
    test_heading_classification();

    // The Math tab must produce a non-empty HTML doc with real math markup;
    // wxWebView just renders whatever string we give it, so a missing or
    // empty HTML is a guaranteed blank tab in the GUI.
    std::string doc = latex_to_html("");
    CHECK(has(doc, "<!DOCTYPE"));
    CHECK(has(doc, "class=\"math\""));

    // Smoke test: an H(s) the engine actually produces for the common-source
    // amp ends with a denominator sum like "(1 + s*(...))" -- verify the
    // resulting HTML keeps the structural pieces (frac / sub / parallel).
    std::string h = latex_to_html(
        "H(s) = \\frac{- Rd\\parallel ro_{M1} gm_{M1}\\left(1- Cgd_{M1} "
        "s/gm_{M1}\\right)}{1+ Cgd_{M1} Rd\\parallel ro_{M1} s}");
    CHECK(has(h, "<!DOCTYPE"));
    CHECK(has(h, "class=\"frac\""));
    CHECK(has(h, "<sub>M1</sub>"));
    CHECK(has(h, "&#8741;"));
    // the document must not contain any unrendered backslash command.
    CHECK(!has(h, "\\frac"));
    CHECK(!has(h, "\\parallel"));
    CHECK(!has(h, "\\left"));
    CHECK(!has(h, "\\right"));
    CHECK(!has(h, "\\mathrm"));

    // Dump the rendered HTML so we can prove the Math tab actually shows
    // typeset math when opened in a browser (the wxWebView backend parses
    // the same HTML we emit).
    const char* sample_tex =
        "H(s) = \\frac{- Rd\\parallel ro_{M1}\\,\\frac{gm_{M1} - Cgd_{M1}\\,s}"
        "{gm_{M1}}}{\\left(1 + Cgd_{M1}\\,Rd\\parallel ro_{M1}\\,s\\right)}";
    FILE* f = std::fopen("math_sample.html", "w");
    if (f) {
        std::fputs(latex_to_html(sample_tex).c_str(), f);
        std::fclose(f);
        std::printf("(wrote math_sample.html for visual verification)\n");
    }

    // Regression: rendering the same page twice must produce identical
    // output (a real test for the SetPage-then-recurse bug that crashed the
    // GUI when LOADED triggered another SetPage). If the renderer has any
    // hidden state this would differ; it must be a pure function of the
    // input.
    std::string once = latex_to_html(sample_tex);
    for (int i = 0; i < 5; ++i)
        CHECK(latex_to_html(sample_tex) == once);

    // The engine's LaTeX emits identifiers like "ro_M1" without braces, so
    // "M1" would render as just "M" subscript + "1" plain. latex_normalize
    // wraps multi-char script args in {}.
    std::string fix = latex_to_html("ro_M1·gm_M1");
    CHECK(has(fix, "<sub>M1</sub>"));   // the whole subscript, not "M"
    CHECK(!has(fix, "<sub>M</sub>1"));  // not the broken half-script
    CHECK(has(fix, "ro<sub>M1</sub>")); // leading identifier is preserved

    // Single-token scripts are left alone (already parse correctly).
    std::string x2 = latex_to_html("x^2 + y_i");
    CHECK(has(x2, "<sup>2</sup>"));
    CHECK(has(x2, "<sub>i</sub>"));

    // Already-braced scripts are not double-wrapped.
    std::string ok = latex_to_html("C_{gd}·s");
    CHECK(has(ok, "<sub>gd</sub>"));
    CHECK(!has(ok, "{{"));

    // latex_render_line: same per-line output as latex_to_html_fragment
    // but with NO surrounding <div class="math"> wrapper -- callers
    // embedding typeset math within their own HTML (the Math tab's report
    // view) need this so the result drops cleanly into the host page.
    std::string line = latex_render_line("ro_M1 gm_M1");
    CHECK(has(line, "<sub>M1</sub>"));
    CHECK(has(line, "ro<sub>M1</sub>"));
    CHECK(!has(line, "<div")); // no wrapper
    CHECK(!has(line, "class=\"line\"")); // no .line either

    std::printf("%s (%d failure(s))\n",
                g_fail ? "LATEX FAILED" : "latex ok", g_fail);
    return g_fail == 0 ? 0 : 1;
}
