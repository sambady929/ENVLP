#include "MathPanel.h"

#include "LatexRender.h"

#include <wx/clipbrd.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

namespace {
// Defined below, together with the other MathJax helpers.
wxString mathjax_file();
} // namespace

MathPanel::MathPanel(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    // Typesetting needs MathJax, which is modern JavaScript and therefore needs
    // the Chromium (Edge/WebView2) backend. wxWidgets silently falls back to
    // the legacy IE engine when WebView2Loader.dll is missing next to the
    // executable, and MathJax cannot run there at all: the raw \(..\) would be
    // left on screen. So ask for Edge explicitly, and take the MathJax path
    // only when that view was actually created (and the bundle is installed).
    bool on_edge = false;
    if (wxWebView::IsBackendAvailable(wxWebViewBackendEdge)) {
        view_ = wxWebView::New(this, wxID_ANY, wxWebViewDefaultURLStr,
                               wxDefaultPosition, wxDefaultSize,
                               wxWebViewBackendEdge);
        on_edge = view_ != nullptr;
    }
    if (!view_) view_ = wxWebView::New(this, wxID_ANY);
    typeset_ = on_edge && !mathjax_file().empty();
    if (view_) {
        // Loading is async: the load completes after the event loop spins.
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
    ".sqrt { display: inline-flex; align-items: stretch; }"
    ".sqrt > .radic { position: relative; align-self: stretch;"
    "                 width: 0.55em; min-height: 1em; margin-right: -1px; }"
    ".sqrt > .radic > svg { position: absolute; top: 0; left: 0;"
    "                       width: 100%; height: 100%; overflow: visible; }"
    ".sqrt > .radicand { border-top: 1px solid #10141a;"
    "                   padding: 3px 4px 2px 3px; }"
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
    "        font-size: 11.5px; margin: 6px 0; }"
    // MathJax (SVG) output: keep a long expression scrollable inside its card
    // and align it on the surrounding baseline.
    "mjx-container { vertical-align: middle; max-width: 100%; }"
    "mjx-container svg { max-width: 100%; }"
    ".line.math, .expr.math { overflow-x: auto; overflow-y: hidden; }";

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

// Locate the vendored MathJax bundle: next to the executable (the build stages
// it there and the portable release ships it), or in the source tree for a
// development run. Empty when it is not installed.
wxString mathjax_file() {
    static wxString cached;
    static bool done = false;
    if (done) return cached;
    done = true;
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    wxFileName staged(exe.GetPathWithSep() + "mathjax", "tex-svg.js");
    if (staged.FileExists()) {
        cached = staged.GetFullPath();
        return cached;
    }
    wxFileName dev(wxGetCwd(), "tex-svg.js");
    dev.AppendDir("resources");
    dev.AppendDir("mathjax");
    if (dev.FileExists()) cached = dev.GetFullPath();
    return cached;
}

// Stage the page and the MathJax bundle as real files and return the page's
// path. Loading a file document instead of SetPage matters: the Edge backend
// renders SetPage through NavigateToString, whose HTML is capped at 2 MB, and
// the bundle alone is bigger than that -- the page used to be truncated and
// MathJax never ran, leaving raw \(..\) on screen. A file document has no such
// limit, and the browser caches the bundle between renders.
bool stage_page(const wxString& js, const std::string& page, wxString& out_url) {
    wxFileName dir(wxStandardPaths::Get().GetTempDir(), "");
    dir.AppendDir("envlp-mathjax");
    if (!dir.DirExists() &&
        !wxFileName::Mkdir(dir.GetFullPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL))
        return false;
    wxFileName bundle(dir.GetFullPath(), "tex-svg.js");
    if (!bundle.FileExists() || bundle.GetSize().GetValue() !=
                                    wxFileName(js).GetSize().GetValue()) {
        if (!wxCopyFile(js, bundle.GetFullPath(), /*overwrite=*/true))
            return false;
    }
    // A fresh file name every render: WebView2 serves a file:// URL from its
    // cache, so re-writing the same page.html and reloading it can show the
    // previous analysis (or nothing) after the content changes.
    static int page_seq = 0;
    char name[48];
    std::snprintf(name, sizeof(name), "page_%d.html", ++page_seq);
    wxFileName pg(dir.GetFullPath(), name);
    {
        std::ofstream f(pg.GetFullPath().ToStdString().c_str(), std::ios::binary);
        if (!f) return false;
        f << page;
        if (!f) return false;
    }
    wxString url = pg.GetFullPath();
    url.Replace("\\", "/");
    out_url = "file:///" + url;
    return true;
}

// One line of engine LaTeX as HTML. With MathJax present it is real
// typesetting; otherwise the built-in subset renderer keeps the app usable.
std::string math_html(const std::string& tex, bool use_mathjax) {
    if (!use_mathjax) return latex_render_line(tex);
    // \displaystyle keeps fractions full size inside inline math, which unlike
    // display math stays left aligned (so report lines line up in a column).
    return "\\(\\displaystyle " + esc_html(latex_normalize(tex)) + "\\)";
}

// Render the typeset poles/zeros report. Lines ending in ':' are section
// headings; everything else is a typeset math line. A heading plus its
// following lines are wrapped together in a compact card.
std::string render_latex_report(const std::string& lr, bool use_mathjax) {
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
                        math_html(cur, use_mathjax) + "</div>";
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
    auto build = [this](bool use_mj) {
        std::string body;
        if (!latex_.empty()) {
            // The engine's LaTeX already begins with "H(s) = " (or "Zout = "
            // etc.); that's exactly what we want as the typeset lead-in, so we
            // pass it through unchanged.
            body += "<div class=\"expr math\">";
            body += math_html(latex_, use_mj);
            body += "</div>";
        }
        if (!latex_report_.empty())
            body += render_latex_report(latex_report_, use_mj);
        // The plain-text report is deliberately NOT rendered here: it lives in
        // the separate "Results (Text)" tab, so showing it again would
        // duplicate the poles/zeros the user already sees typeset above.
        std::string page;
        page += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>";
        page += kReportCss;
        page += "</style>";
        if (use_mj) {
            // MathJax's default inline delimiter \( \) is what math_html emits.
            // The bundle sits beside the page (staged in stage_page).
            page += "<script>window.MathJax={svg:{fontCache:\"global\"},"
                    "options:{enableMenu:false}};</script>"
                    "<script src=\"tex-svg.js\"></script>";
        }
        page += "</head><body><div class=\"wrap\">";
        page += body;
        page += "</div></body></html>";
        return page;
    };

    const wxString js = mathjax_file();
    if (typeset_ && !js.empty()) {
        wxString url;
        if (stage_page(js, build(true), url)) {
            view_->LoadURL(url);
            return;
        }
    }
    // No Chromium backend, no MathJax bundle, or an unwritable staging
    // directory: the built-in renderer keeps the tab readable (and never leaves
    // raw \(..\) on screen).
    view_->SetPage(build(false), "");
}

} // namespace envlp
