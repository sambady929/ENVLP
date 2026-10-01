#include "MathPanel.h"

#include "LatexRender.h"

#include <wx/clipbrd.h>
#include <wx/sizer.h>

namespace envlp {

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
    "Run an analysis to see the result typeset here."
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
    latex_report_.clear();
    render();
}

void MathPanel::set_report(const std::string& latex,
                           const std::string& latex_report) {
    latex_ = latex;
    latex_report_ = latex_report;
    render();
}

void MathPanel::clear() {
    latex_.clear();
    latex_report_.clear();
    if (view_) view_->SetPage(kEmptyPage, "");
}

namespace {
// The Math tab shows the typeset expression first (the headline result:
// stacked fractions, proper subscripts, the parallel glyph), then the
// supporting report below it. The styling is deliberately compact -- tighter
// font, line height and spacing -- so a full result fits without a lot of
// scrolling, and each section reads as a small card.
const char* kReportCss =
    "html, body { margin: 0; padding: 0; background: #f4f5f8; }"
    "body { font-family: 'Segoe UI', sans-serif; color: #202634; }"
    // Fill the window width so the typeset page reflows as the results
    // frame is resized (a fixed max-width left it looking unchanged).
    ".wrap { padding: 10px 12px 16px 12px; max-width: none;"
    "        box-sizing: border-box; }"
    ".expr { margin: 0 0 10px 0; padding: 8px 12px;"
    "        background: #ffffff; border: 1px solid #e3e6ee;"
    "        border-radius: 6px; overflow-x: auto; }"
    ".math { font-family: 'Cambria Math', 'Latin Modern Math',"
    "        'Times New Roman', serif; font-size: 16px;"
    "        line-height: 1.5; color: #10141a; }"
    ".frac { display: inline-block; vertical-align: middle;"
    "        text-align: center; margin: 0 2px; }"
    ".frac > .num { display: block; padding: 0 3px 1px 3px;"
    "               border-bottom: 1px solid #10141a; }"
    ".frac > .den { display: block; padding: 1px 3px 0 3px; }"
    "sub, sup { font-size: 70%; }"
    ".sqrt { display: inline-flex; align-items: center; }"
    ".sqrt > .radic { margin-right: 1px; }"
    ".sqrt > .radicand { border-top: 1px solid #10141a;"
    "                   padding: 2px 3px 0 2px; }"
    ".overline { border-top: 1px solid #10141a; padding-top: 1px; }"
    ".mathrm, .text { font-style: normal; }"
    ".mathit { font-style: italic; }"
    "h2.section { font-size: 10.5px; font-weight: 600; text-transform:"
    "        uppercase; letter-spacing: .06em; color: #8890a0;"
    "        margin: 8px 0 3px 0; }"
    ".card { background: #ffffff; border: 1px solid #e6e9f0;"
    "        border-radius: 6px; padding: 6px 10px; margin: 0 0 8px 0; }"
    "p.line { margin: 2px 0; font-size: 12.5px; line-height: 1.35;"
    "        color: #2a3140; }"
    ".line { margin: 2px 0; }"
    "p.line.mono { font-family: 'Cascadia Mono', 'Consolas', monospace;"
    "        font-size: 11.5px; white-space: pre; }"
    ".note { color: #8a6d1a; background: #fdf6e0; border: 1px solid"
    "        #f0e2b0; border-radius: 4px; padding: 5px 8px;"
    "        font-size: 11.5px; margin: 6px 0; }";

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

// Render the typeset poles/zeros report. Lines ending in ':' are section
// headings; everything else is a typeset math line. A heading plus its
// following lines are wrapped together in a compact card.
std::string render_latex_report(const std::string& lr) {
    std::string out;
    std::string card;  // pending card body (heading + lines)
    std::string cur;
    auto close_card = [&]() {
        if (!card.empty()) {
            out += "<div class=\"card\">" + card + "</div>";
            card.clear();
        }
    };
    auto flush = [&]() {
        while (!cur.empty() &&
               (cur.back() == '\n' || cur.back() == '\r' || cur.back() == ' '))
            cur.pop_back();
        if (!cur.empty()) {
            std::string heading;
            if (latex_is_plain_heading(cur))
                heading = cur;
            else
                latex_heading_from_mathrm(cur, heading);
            if (!heading.empty()) {
                close_card();
                card += "<h2 class=\"section\">" + esc_html(heading) + "</h2>";
            } else {
                card += "<div class=\"line math\">" +
                        latex_render_line(cur) + "</div>";
            }
        }
        cur.clear();
    };
    for (char c : lr) {
        if (c == '\n') flush();
        else cur += c;
    }
    flush();
    close_card();
    return out;
}
} // namespace

void MathPanel::render() {
    if (!view_) return;
    if (latex_.empty() && latex_report_.empty()) {
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
    if (!latex_report_.empty()) body += render_latex_report(latex_report_);
    // The plain-text report is deliberately NOT rendered here: it lives in
    // the separate "Results (Text)" tab, so showing it again would duplicate
    // the poles/zeros the user already sees typeset above.
    std::string page;
    page += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>";
    page += kReportCss;
    page += "</style></head><body><div class=\"wrap\">";
    page += body;
    page += "</div></body></html>";
    view_->SetPage(page, "");
}

} // namespace envlp
