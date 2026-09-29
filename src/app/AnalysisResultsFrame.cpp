#include "AnalysisResultsFrame.h"
#include "Theme.h"

#include <wx/sizer.h>

namespace symcirc {

AnalysisResultsFrame::AnalysisResultsFrame(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, "Analysis results",
              wxDefaultPosition, wxSize(900, 600)) {
    SetBackgroundColour(theme::chrome_bg);
    book_ = new wxNotebook(this, wxID_ANY);
    book_->SetBackgroundColour(theme::surface_muted);
    book_->SetForegroundColour(theme::text);
    results_ = new ResultsPanel(book_);
    math_ = new MathPanel(book_);
    bode_ = new BodePanel(book_);
    // "Results" is the typeset (LaTeX) view and comes first; the plain-text
    // dump is the secondary "Results (Text)" tab.
    book_->AddPage(math_, "Results", true);
    book_->AddPage(results_, "Results (Text)");
    book_->AddPage(bode_, "Plot");
    // The Lua console is hidden for now (messy): lua_ stays null, so the
    // lua()->...() plumbing in MainFrame is a no-op via the null guard.

    // The notebook must fill the frame and follow every resize, so the typeset
    // results page (and the text/plot tabs) reflow with the window instead of
    // staying at their construction size.
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(book_, 1, wxEXPAND);
    SetSizer(sizer);

    // The RICH2 text controls report a huge best-size, which would make the
    // frame taller than the screen; pin it to a sensible client size.
    SetSize(900, 600);
    SetMinSize(wxSize(320, 240));

    // Closing the window just hides it; the next analysis brings it back.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        e.Veto();
        Hide();
    });
}

void AnalysisResultsFrame::popup() {
    Show();
    Raise();
    // Refresh so a long analysis report lays out correctly before the user
    // sees the window.
    book_->GetCurrentPage()->Layout();
    // wxWebView's Chromium process is lazy: a tab that's been shown once and
    // then hidden for a while may need to re-render when it comes back, and
    // the engine's LaTeX might have changed in the meantime. Re-push.
    if (math_) math_->refresh();
}

void AnalysisResultsFrame::select_page(int idx) {
    if (!book_) return;
    if (idx >= 0 && idx < int(book_->GetPageCount()))
        book_->SetSelection(idx);
}
void AnalysisResultsFrame::set_latex(const std::string& latex) {
    if (math_) math_->set_latex(latex);
}

void AnalysisResultsFrame::set_report(const std::string& latex,
                                      const std::string& latex_report) {
    if (math_) math_->set_report(latex, latex_report);
}

} // namespace symcirc
