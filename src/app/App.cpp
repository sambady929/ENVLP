// ENVLP application entry point.
#include "MainFrame.h"

#include <wx/wx.h>

#include <wx/cmdline.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>

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
        // Window / taskbar icon: load the icon resource compiled into the
        // executable. The resource is declared under the *name* ENVLP_APP in
        // ENVLP.rc (wxWidgets looks icons up by name); the same file is also
        // declared with integer id 1, which is what Explorer uses for the
        // .exe. A bundle lets the title bar (16 px) and the taskbar / alt-tab
        // (32 px+) each get a frame of the right size.
        // Window / taskbar icon: load the icon resource compiled into the
        // executable. It is declared under the *name* ENVLP_APP in ENVLP.rc
        // (wxWidgets looks icons up by name, not id); the same file is also
        // declared with integer id 1, which is what Explorer shows for the
        // .exe. Windows picks the right frame size from the multi-size .ico.
        wxIconBundle icons;
        wxIcon ico;
        if (ico.LoadFile(wxT("ENVLP_APP"), wxBITMAP_TYPE_ICO_RESOURCE))
            icons.AddIcon(ico);
        if (!icons.IsEmpty())
            frame->SetIcons(icons);
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
