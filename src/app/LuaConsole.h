#pragma once
#include "script/LuaVm.h"

#include <wx/wx.h>

#include <string>
#include <vector>

namespace envlp {

// Bottom-panel tab: interactive Lua console.
class LuaConsole : public wxPanel {
public:
    explicit LuaConsole(wxWindow* parent);

    LuaVm& vm() { return vm_; }
    void set_result(const syms::AnalysisResult* r) { vm_.set_result(r); }
    void echo(const std::string& utf8); // show engine output in the console

private:
    LuaVm vm_;
    wxTextCtrl* out_;
    wxTextCtrl* in_;
    std::vector<std::string> history_;
    int hist_pos_ = -1;

    void run_line(const std::string& line);
    void on_enter(wxCommandEvent& e);
    void on_key(wxKeyEvent& e);
};

} // namespace envlp
