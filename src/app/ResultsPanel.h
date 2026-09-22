#pragma once
#include <wx/wx.h>

#include <functional>
#include <string>

namespace symcirc {

// Bottom-panel tab: the human-readable analysis report + copy/clear, plus a
// button to copy just the LaTeX form.
class ResultsPanel : public wxPanel {
public:
    explicit ResultsPanel(wxWindow* parent);

    void set_text(const std::string& utf8);
    void set_latex(const std::string& latex); // stored for the copy button
    void append(const std::string& utf8);
    void clear();

private:
    wxTextCtrl* text_;
    std::string latex_;
};

} // namespace symcirc
