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
class AnalysisPanel;

class MainFrame : public wxFrame {
public:
    MainFrame();

    void open_path(const wxString& p); // open a .scx (command line / recent)

protected:
    // Application-wide key handling: runs before the focused child so that
    // shortcuts work while the canvas is placing/wiring and so Escape is never
    // swallowed by a child.
    void on_char_hook(wxKeyEvent& e);

private:
    Document doc_;
    std::unique_ptr<syms::AnalysisResult> result_;

    SchematicCanvas* canvas_ = nullptr;
    PalettePanel* palette_ = nullptr;
    PropertiesPanel* props_ = nullptr;
    ResultsPanel* results_ = nullptr;
    BodePanel* bode_ = nullptr;
    LuaConsole* lua_ = nullptr;
    AnalysisPanel* analysis_ = nullptr;
    wxNotebook* bottom_ = nullptr;
    wxToolBar* toolbar_ = nullptr;
    wxMenuItem* mi_ignore_ = nullptr;

    void build_menu();
    void build_toolbar();
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
    void on_undo(wxCommandEvent&);
    void on_redo(wxCommandEvent&);
    void on_undo_cmd();
    void on_redo_cmd();
    void after_undo_redo();

    // analysis
    void on_run(wxCommandEvent&);
    void run_analysis();
    void run_card(int index); // -1 = all enabled cards
    void set_ignore_negligible(bool on);

    // keyboard placement map / instance menu
    bool handle_shortcut(wxKeyEvent& e);
    void sync_palette();
    void show_instance_menu();
    void prompt_net_labels();

    void on_about(wxCommandEvent&);
    void on_ignore_neg(wxCommandEvent&);
    void on_zoom_fit(wxCommandEvent&);

    // plumbing
    void document_changed();
    void selection_changed(const std::string& sel);

    // Is the focused control a text-entry widget (so shortcuts must not fire)?
    bool focus_is_text_entry() const;

    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
