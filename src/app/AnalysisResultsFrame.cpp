#include "AnalysisResultsFrame.h"

namespace symcirc {

AnalysisResultsFrame::AnalysisResultsFrame(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, "Analysis results",
              wxDefaultPosition, wxSize(900, 600)) {
    book_ = new wxNotebook(this, wxID_ANY);
    results_ = new ResultsPanel(book_);
    math_ = new MathPanel(book_);
    bode_ = new BodePanel(book_);
    lua_ = new LuaConsole(book_);
    book_->AddPage(results_, "Results", true);
    book_->AddPage(math_, "Math");
    book_->AddPage(bode_, "Bode");
    book_->AddPage(lua_, "Lua");

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

void AnalysisResultsFrame::set_report(const std::string& report,
                                      const std::string& latex) {
    if (math_) math_->set_report(report, latex);
}

} // namespace symcirc
