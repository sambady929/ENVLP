// SymCirc application entry point.
#include "MainFrame.h"

#include <wx/wx.h>

#include <wx/cmdline.h>

#include <vector>

class SymCircApp : public wxApp {
public:
    void OnInitCmdLine(wxCmdLineParser& p) override {
        wxApp::OnInitCmdLine(p);
        p.AddParam("file (schematic .scx to open)", wxCMD_LINE_VAL_STRING,
                   wxCMD_LINE_PARAM_OPTIONAL | wxCMD_LINE_PARAM_MULTIPLE);
    }

    bool OnCmdLineParsed(wxCmdLineParser& p) override {
        if (!wxApp::OnCmdLineParsed(p)) return false;
        for (size_t i = 0; i < p.GetParamCount(); ++i)
            files_.push_back(p.GetParam(i));
        return true;
    }

    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        wxInitAllImageHandlers(); // needed for PNG plot export
        auto* frame = new symcirc::MainFrame();
        frame->Show(true);
        for (const auto& f : files_) {
            try {
                frame->open_path(f);
            } catch (...) {
                // never let a bad file take the app down
            }
        }
        return true;
    }

private:
    std::vector<wxString> files_;
};

wxIMPLEMENT_APP(SymCircApp);
