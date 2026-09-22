// SymCirc application entry point.
#include "MainFrame.h"

#include <wx/wx.h>

class SymCircApp : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        auto* frame = new symcirc::MainFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(SymCircApp);
