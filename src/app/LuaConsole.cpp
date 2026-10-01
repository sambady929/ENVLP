#include "LuaConsole.h"

#include <vector>

namespace envlp {

LuaConsole::LuaConsole(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    out_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                          wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 |
                              wxTE_DONTWRAP);
    wxFont f(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
             wxFONTWEIGHT_NORMAL);
    out_->SetFont(f);

    in_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                         wxTE_PROCESS_ENTER);
    in_->SetFont(f);

    auto* hint = new wxStaticText(
        this, wxID_ANY,
        "Lua: mag(f) phase(f) H() Hpoly() latex() report() roots() "
        "estimate(name) dropped() -- Enter runs, Up/Down recalls history");
    hint->SetForegroundColour(wxColour(115, 115, 120));

    sizer->Add(out_, 1, wxEXPAND | wxALL, 4);
    sizer->Add(in_, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
    sizer->Add(hint, 0, wxALL, 4);
    SetSizer(sizer);

    in_->Bind(wxEVT_TEXT_ENTER, &LuaConsole::on_enter, this);
    in_->Bind(wxEVT_CHAR_HOOK, &LuaConsole::on_key, this);

    out_->AppendText(wxString::FromUTF8(
        "envlp Lua console. Example:\n"
        "  > for f=10,1e6,10*3 do print(f, mag(f)) end\n"
        "  > for _,p in ipairs(roots()) do print(p.w, p.label) end\n\n"));

    // route Lua print() straight into the console
    vm_.set_sink([this](const std::string& s) {
        out_->AppendText(wxString::FromUTF8(s));
    });
}

void LuaConsole::echo(const std::string& utf8) {
    out_->AppendText(wxString::FromUTF8(utf8));
}

void LuaConsole::run_line(const std::string& line) {
    out_->AppendText(wxString::FromUTF8("> " + line + "\n"));
    std::string err;
    if (!vm_.run(line, err)) {
        out_->AppendText(wxString::FromUTF8("  error: " + err + "\n"));
    }
    std::string produced = vm_.take_output();
    if (!produced.empty()) out_->AppendText(wxString::FromUTF8(produced));
    out_->SetInsertionPointEnd();
}

void LuaConsole::on_enter(wxCommandEvent& e) {
    std::string line = e.GetString().ToStdString();
    if (line.empty()) return;
    history_.push_back(line);
    hist_pos_ = int(history_.size());
    in_->Clear();
    run_line(line);
}

void LuaConsole::on_key(wxKeyEvent& e) {
    switch (e.GetKeyCode()) {
    case WXK_UP:
        if (hist_pos_ > 0) {
            --hist_pos_;
            in_->SetValue(wxString::FromUTF8(history_[hist_pos_]));
            in_->SetInsertionPointEnd();
            return;
        }
        break;
    case WXK_DOWN:
        if (hist_pos_ < int(history_.size()) - 1) {
            ++hist_pos_;
            in_->SetValue(wxString::FromUTF8(history_[hist_pos_]));
            in_->SetInsertionPointEnd();
            return;
        }
        if (hist_pos_ == int(history_.size()) - 1) {
            hist_pos_ = int(history_.size());
            in_->Clear();
            return;
        }
        break;
    default:
        break;
    }
    e.Skip();
}

} // namespace envlp
