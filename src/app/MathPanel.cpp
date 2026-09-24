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
// Decide whether a report line is a math expression that should be typeset,
// or plain text. Heuristic: contains `\` and either contains a LaTeX
// command (`\frac`, `\mathrm`, etc.) or has at least one `\cmd{...}` shape.
bool looks_like_latex(const std::string& line) {
    if (line.find('\\') == std::string::npos) return false;
    static const char* kCmds[] = {"\\frac", "\\mathrm", "\\parallel",
                                  "\\cdot", "\\times", "\\right", "\\begin",
                                  "\\le",  "\\pm",     "\\sqrt",   "\\pi"};
    for (const char* c : kCmds)
        if (line.find(c) != std::string::npos) return true;
    return false;
}
} // namespace

void MathPanel::render() {
    if (!view_) return;
    // Compute the page we want right now and send it exactly once. wxWebView
    // calls LOADED when it finishes, but we don't loop back -- the next
    // render() call (from set_latex or refresh()) is what re-pushes content.
    std::string page;
    if (latex_.empty() && report_.empty()) {
        page = kEmptyPage;
    } else if (report_.empty()) {
        page = latex_to_html(latex_);
    } else {
        // Combine report + math. Walk the report line by line: text lines
        // render as <p>, LaTeX lines render as typeset math.
        std::string body = "<div class=\"report\">";
        std::string cur;
        auto flush_text = [&]() {
            if (cur.empty()) return;
            // escape < > & for safe inclusion
            std::string esc;
            for (char c : cur) {
                if (c == '<') esc += "&lt;";
                else if (c == '>') esc += "&gt;";
                else if (c == '&') esc += "&amp;";
                else esc += c;
            }
            body += "<p class=\"text\">" + esc + "</p>";
            cur.clear();
        };
        for (char c : report_) {
            if (c == '\n') {
                std::string trimmed = cur;
                while (!trimmed.empty() && (trimmed.back() == '\r' ||
                                            trimmed.back() == ' '))
                    trimmed.pop_back();
                if (looks_like_latex(trimmed)) {
                    flush_text();
                    body += "<div class=\"line\">" +
                            latex_render_line(trimmed) + "</div>";
                } else {
                    cur += '\n'; // keep the line break
                }
            } else {
                cur += c;
            }
        }
        flush_text();
        body += "</div>";
        page = std::string("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                          "<style>") +
               "body{margin:0;padding:16px;font-family:'Segoe UI',sans-serif;"
               "color:#55606e;}"
               ".math{font-family:'Cambria Math',serif;font-size:19px;"
               "line-height:2.1;color:#10141a;}"
               ".frac{display:inline-block;vertical-align:middle;text-align:center;"
               "margin:0 3px;}.frac>.num{display:block;padding:0 4px 1px 4px;"
               "border-bottom:1.4px solid #10141a;}.frac>.den{display:block;"
               "padding:1px 4px 0 4px;}sub,sup{font-size:72%;}"
               ".overline{border-top:1.3px solid #10141a;padding-top:1px;}"
               ".report p.text{margin:6px 0;color:#3a4452;font-size:13px;"
               "line-height:1.5;font-family:'Segoe UI',sans-serif;}"
               ".report .line{margin:6px 0;}"
               "</style></head><body>" + body + "</body></html>";
    }
    view_->SetPage(page, "");
}

} // namespace symcirc
