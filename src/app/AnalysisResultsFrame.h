#pragma once
#include "BodePanel.h"
#include "LuaConsole.h"
#include "MathPanel.h"
#include "ResultsPanel.h"

#include <wx/wx.h>
#include <wx/notebook.h>

namespace envlp {

// Floating window that holds the analysis output (Results / Math / Bode /
// Lua). Lives independently of the main frame so the user can keep it open
// while editing, drag it to a second monitor, or close it. Created lazily on
// the first analysis run; if the user closes it, the next analysis brings it
// back.
class AnalysisResultsFrame : public wxFrame {
public:
    AnalysisResultsFrame(wxWindow* parent);

    ResultsPanel* results() { return results_; }
    MathPanel* math() { return math_; }
    BodePanel* bode() { return bode_; }
    LuaConsole* lua() { return lua_; }

    // Show / raise the window without changing the current notebook page.
    void popup();
    void select_page(int idx); // 0 results (LaTeX), 1 results (text), 2 bode, 3 lua

    // Feed the typeset-math tab (LaTeX from the engine).
    void set_latex(const std::string& latex);
    // Feed the typeset-math tab with the transfer-function LaTeX and the
    // poles/zeros LaTeX report.
    void set_report(const std::string& latex, const std::string& latex_report);

private:
    wxNotebook* book_ = nullptr;
    ResultsPanel* results_ = nullptr;
    MathPanel* math_ = nullptr;
    BodePanel* bode_ = nullptr;
    LuaConsole* lua_ = nullptr;
};

} // namespace envlp
