#pragma once
#include <wx/wx.h>
#include <wx/webview.h>

#include <string>

namespace symcirc {

// Renders the LaTeX produced by the engine as real typeset math, using an
// embedded wxWebView and the offline LaTeX->HTML converter (no CDN/MathJax
// required, so it works without a network connection).
//
// Important: this control must NOT re-render on wxEVT_WEBVIEW_LOADED. The
// Edge / EdgeLegacy backend fires that event for *every* SetPage, and if the
// handler triggers another SetPage the result is unbounded recursion that
// crashes the process with 0xC00000FD (stack overflow). Treat LOADED purely
// as a "page is now on screen" notification, and rely on the frame to drive
// the next render through set_latex() or refresh().
class MathPanel : public wxPanel {
public:
    explicit MathPanel(wxWindow* parent);

    // Show one LaTeX expression (may contain newlines for several lines).
    void set_latex(const std::string& latex);
    // Show the typeset result: the transfer-function LaTeX plus the
    // poles/zeros LaTeX report. The plain-text version lives in the separate
    // "Results (Text)" tab and is not rendered here (no duplicate).
    void set_report(const std::string& latex, const std::string& latex_report);
    void clear();
    // Re-render whatever latex was last set; called when the popup comes
    // back from hidden, because the Chromium-backed webview may have torn
    // its content down in between.
    void refresh() { render(); }

private:
    void render();

    wxWebView* view_ = nullptr;
    std::string latex_;
    std::string latex_report_;
    bool ready_ = false;
};

} // namespace symcirc
