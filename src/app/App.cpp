// SymCirc application entry point.
#include "MainFrame.h"

#include <wx/wx.h>

#include <wx/cmdline.h>

class SymCircApp : public wxApp {
public:
    void OnInitCmdLine(wxCmdLineParser& p) override {
        wxApp::OnInitCmdLine(p);
        p.AddParam("file (schematic .scx to open)", wxCMD_LINE_VAL_STRING,
                   wxCMD_LINE_PARAM_OPTIONAL);
    }

    bool OnCmdLineParsed(wxCmdLineParser& p) override {
        if (!wxApp::OnCmdLineParsed(p)) return false;
        if (p.GetParamCount() > 0) file_ = p.GetParam(0);
        return true;
    }

    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        auto* frame = new symcirc::MainFrame();
        frame->Show(true);
        if (!file_.empty()) {
            try {
                frame->open_path(file_);
            } catch (...) {
                // never let a bad file take the app down
            }
        }
        return true;
    }

private:
    wxString file_;
};

wxIMPLEMENT_APP(SymCircApp);
