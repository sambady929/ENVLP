#pragma once
#include "Document.h"
#include "core/Engine.h"

#include <wx/wx.h>
#include <wx/notebook.h>

#include <memory>

namespace symcirc {

class SchematicCanvas;
class PalettePanel;
class PropertiesPanel;
class ResultsPanel;
class BodePanel;
class LuaConsole;

class MainFrame : public wxFrame {
public:
    MainFrame();

private:
    Document doc_;
    std::unique_ptr<syms::AnalysisResult> result_;

    SchematicCanvas* canvas_ = nullptr;
    PalettePanel* palette_ = nullptr;
    PropertiesPanel* props_ = nullptr;
    ResultsPanel* results_ = nullptr;
    BodePanel* bode_ = nullptr;
    LuaConsole* lua_ = nullptr;
    wxNotebook* bottom_ = nullptr;

    void build_menu();
    void build_layout();

    void update_title();

    // file ops
    void on_new(wxCommandEvent&);
    void on_open(wxCommandEvent&);
    void on_save(wxCommandEvent&);
    void on_save_as(wxCommandEvent&);
    bool maybe_save(); // false = user cancelled
    bool do_save_as();

    // editing
    void on_rotate(wxCommandEvent&);
    void on_delete(wxCommandEvent&);

    // analysis
    void on_run(wxCommandEvent&);
    void run_analysis();

    void on_about(wxCommandEvent&);

    // plumbing
    void document_changed();
    void selection_changed(const std::string& sel);

    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
