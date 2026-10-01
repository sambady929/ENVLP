// ENVLP application entry point.
#include "MainFrame.h"

#include <wx/wx.h>

#include <wx/cmdline.h>
#include <wx/icon.h>

#include <vector>

class envlpApp : public wxApp {
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
        auto* frame = new envlp::MainFrame();
        // Window / taskbar icon: load the multi-size .ico compiled into the
        // executable (resource id 1). The executable's own icon (Explorer,
        // shortcuts) comes from the ICON statement in ENVLP.rc.
        wxIcon ico;
        if (ico.LoadFile(wxT("ENVLP.ico"), wxBITMAP_TYPE_ICO_RESOURCE))
            frame->SetIcon(ico);
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

wxIMPLEMENT_APP(envlpApp);
