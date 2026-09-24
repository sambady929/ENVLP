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
    // Show the full text report along with the math expression: lines that
    // look like LaTeX (start with `\` or contain `\frac` etc.) get rendered
    // as math; other lines render as plain text.
    void set_report(const std::string& report, const std::string& latex);
    void clear();
    // Re-render whatever latex was last set; called when the popup comes
    // back from hidden, because the Chromium-backed webview may have torn
    // its content down in between.
    void refresh() { render(); }

private:
    void render();

    wxWebView* view_ = nullptr;
    std::string latex_;
    std::string report_;
    bool ready_ = false;
};

} // namespace symcirc
