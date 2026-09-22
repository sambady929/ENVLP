#pragma once
#include <wx/wx.h>

#include <functional>
#include <string>

namespace symcirc {

// Bottom-panel tab: the human-readable analysis report + copy/clear.
class ResultsPanel : public wxPanel {
public:
    explicit ResultsPanel(wxWindow* parent);

    void set_text(const std::string& utf8);
    void append(const std::string& utf8);
    void clear();

private:
    wxTextCtrl* text_;
};

} // namespace symcirc
