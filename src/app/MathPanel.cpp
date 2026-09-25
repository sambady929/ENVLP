#include "MathPanel.h"

#include "LatexRender.h"

#include <wx/clipbrd.h>
#include <wx/sizer.h>

namespace symcirc {

namespace {
// Two distinct empty pages so we can tell them apart in debug logs: one is
// the placeholder we show before any analysis has run; the other is what we
// use after a `clear()`. Both contain the same inline stylesheet the engine's
// LaTeX page uses, so the first SetPage and subsequent ones share fonts.
const char* kEmptyPage =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>"
    "body{margin:0;padding:16px;font-family:'Segoe UI',sans-serif;"
    "color:#55606e;} .math{font-family:'Cambria Math',serif;font-size:19px;"
    "line-height:2.1;color:#10141a;} .frac{display:inline-block;"
    "vertical-align:middle;text-align:center;margin:0 3px;} "
    ".frac>.num{display:block;padding:0 4px 1px 4px;"
    "border-bottom:1.4px solid #10141a;} .frac>.den{display:block;"
    "padding:1px 4px 0 4px;} sub,sup{font-size:72%;}"
    ".overline{border-top:1.3px solid #10141a;padding-top:1px;}"
    "</style></head><body><div class=\"math\">"
    "Run an analysis (F5) to see the result typeset here."
    "</div></body></html>";
} // namespace

MathPanel::MathPanel(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    view_ = wxWebView::New(this, wxID_ANY);
    if (view_) {
        // Edge (Chromium) backend is present on Windows 10/11; on older
        // systems wxWidgets falls back to the built-in IE/EdgeLegacy backend.
        // Either way SetPage is async: the load completes after the event
        // loop spins.
        view_->SetPage(kEmptyPage, "");
        // CRITICAL: do NOT re-render on LOADED. The Edge backend fires
        // wxEVT_WEBVIEW_LOADED for every SetPage, including programmatic ones;
        // if we re-render here, every render() calls SetPage, which fires
        // LOADED again -- an infinite stack-unwinding that crashes the
        // process with 0xC00000FD (stack overflow, faulting in ieframe.dll).
        // We only use LOADED to flip a one-shot "page is loaded" flag, which
        // the next render() checks so the placeholder is visible before the
        // first analysis even if the first SetPage was queued behind others.
        view_->Bind(wxEVT_WEBVIEW_LOADED,
                     [this](wxWebViewEvent&) { ready_ = true; });
        sizer->Add(view_, 1, wxEXPAND);
    } else {
        sizer->Add(new wxStaticText(this, wxID_ANY,
                                    "Web view not available in this build."),
                   1, wxALL, 8);
    }
    SetSizer(sizer);
}

void MathPanel::set_latex(const std::string& latex) {
    latex_ = latex;
    report_.clear();
    render();
}

void MathPanel::set_report(const std::string& report, const std::string& latex) {
    report_ = report;
    latex_ = latex;
    render();
}

void MathPanel::clear() {
    latex_.clear();
    report_.clear();
    if (view_) view_->SetPage(kEmptyPage, "");
}

namespace {
// The Math tab shows the typeset expression first (the headline result:
// stacked fractions, proper subscripts, the parallel glyph), then the
// supporting text report below it in a clean document style -- so it reads
// as a typeset page rather than a dump of raw LaTeX.
const char* kReportCss =
    "html, body { margin: 0; padding: 0; background: #ffffff; }"
    "body { font-family: 'Segoe UI', sans-serif; color: #202634; }"
    ".wrap { padding: 22px 26px 36px 26px; max-width: 860px; }"
    ".expr { margin: 0 0 20px 0; padding: 16px 20px;"
    "        background: #f7f8fb; border: 1px solid #e3e6ee;"
    "        border-radius: 6px; overflow-x: auto; }"
    ".math { font-family: 'Cambria Math', 'Latin Modern Math',"
    "        'Times New Roman', serif; font-size: 21px;"
    "        line-height: 2.5; color: #10141a; }"
    ".frac { display: inline-block; vertical-align: middle;"
    "        text-align: center; margin: 0 4px; }"
    ".frac > .num { display: block; padding: 0 5px 2px 5px;"
    "               border-bottom: 1.5px solid #10141a; }"
    ".frac > .den { display: block; padding: 2px 5px 0 5px; }"
    "sub, sup { font-size: 72%; }"
    ".overline { border-top: 1.3px solid #10141a; padding-top: 1px; }"
    ".mathrm, .text { font-style: normal; }"
    ".mathit { font-style: italic; }"
    "h2.section { font-size: 12px; font-weight: 600; text-transform:"
    "        uppercase; letter-spacing: .08em; color: #8890a0;"
    "        margin: 22px 0 6px 0; }"
    "p.line { margin: 5px 0; font-size: 13.5px; line-height: 1.55;"
    "        color: #2a3140; }"
    "p.line.mono { font-family: 'Cascadia Mono', 'Consolas', monospace;"
    "        font-size: 12.5px; white-space: pre; }"
    ".note { color: #8a6d1a; background: #fdf6e0; border: 1px solid"
    "        #f0e2b0; border-radius: 4px; padding: 8px 10px;"
    "        font-size: 12.5px; margin: 10px 0; }";

std::string esc_html(const std::string& s) {
    std::string e;
    for (char c : s) {
        if (c == '<') e += "&lt;";
        else if (c == '>') e += "&gt;";
        else if (c == '&') e += "&amp;";
        else e += c;
    }
    return e;
}

// Turn the plain-text report into formatted HTML. The transfer-function
// line is skipped -- the Math tab already shows it typeset above, so
// repeating it as text would be the duplicate the user flagged. Short lines
// ending in ':' become section headings; the rest are body lines.
std::string render_report_html(const std::string& report) {
    std::string out;
    std::string cur;
    auto flush = [&]() {
        while (!cur.empty() &&
               (cur.back() == '\n' || cur.back() == '\r' || cur.back() == ' '))
            cur.pop_back();
        if (!cur.empty()) {
            bool heading = cur.size() < 40 && cur.back() == ':';
            bool equation = cur.rfind("H(s) = ", 0) == 0 ||
                            cur.rfind("Zin = ", 0) == 0 ||
                            cur.rfind("Zout = ", 0) == 0 ||
                            cur.rfind("Isc", 0) == 0 ||
                            cur.rfind("T(s) = ", 0) == 0;
            if (heading)
                out += "<h2 class=\"section\">" + esc_html(cur) + "</h2>";
            else if (!equation)
                out += "<p class=\"line\">" + esc_html(cur) + "</p>";
        }
        cur.clear();
    };
    for (char c : report) {
        if (c == '\n') flush();
        else cur += c;
    }
    flush();
    return out;
}
} // namespace

void MathPanel::render() {
    if (!view_) return;
    if (latex_.empty() && report_.empty()) {
        view_->SetPage(kEmptyPage, "");
        return;
    }
    std::string body;
    if (!latex_.empty()) {
        // The engine's LaTeX already begins with "H(s) = " (or "Zout = "
        // etc.); that's exactly what we want as the typeset lead-in, so we
        // pass it through unchanged.
        body += "<div class=\"expr math\">";
        body += latex_render_line(latex_);
        body += "</div>";
    }
    if (!report_.empty()) body += render_report_html(report_);
    std::string page;
    page += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>";
    page += kReportCss;
    page += "</style></head><body><div class=\"wrap\">";
    page += body;
    page += "</div></body></html>";
    view_->SetPage(page, "");
}

} // namespace symcirc
