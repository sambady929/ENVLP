#pragma once
#include "Document.h"
#include "core/Engine.h"

#include <wx/wx.h>
#include <wx/aui/auibook.h>
#include <wx/splitter.h>

#include <memory>
#include <vector>

namespace envlp {

class SchematicCanvas;
class PalettePanel;
class PropertiesPanel;
class AnalysisPanel;
class AnalysisResultsFrame;

// One open schematic: the document, its panes and its right-hand column.
// Each tab in the main notebook owns one of these, so every schematic keeps
// its own cards, results and view state (LTSpice-style multi-schematic).
//
// The right-hand column is ONE pane that swaps between the analysis cards and
// the properties editor: clicking a component brings up that component's
// editor in place of the analysis cards, so everything is visible at once.
struct SchematicPage {
    Document doc;
    std::unique_ptr<syms::AnalysisResult> result;
    SchematicCanvas* canvas = nullptr;
    PropertiesPanel* props = nullptr;
    AnalysisPanel* analysis = nullptr;
    // One splitter for the right-hand column; it holds a container that swaps
    // between the analysis cards and the properties editor.
    wxSplitterWindow* sp_right = nullptr;
    wxPanel* right_host = nullptr;
    bool props_shown = false; // right column currently shows the properties
};

class MainFrame : public wxFrame {
public:
    MainFrame();

    void open_path(const wxString& p); // open a .scx in a new tab

protected:
    // Application-wide key handling: runs before the focused child so that
    // shortcuts work while the canvas is placing/wiring and so Escape is never
    // swallowed by a child.
    void on_char_hook(wxKeyEvent& e);

private:
    // All open schematics. pages_[active_] is the current tab.
    std::vector<std::unique_ptr<SchematicPage>> pages_;
    int active_ = -1;

    PalettePanel* palette_ = nullptr;
    AnalysisResultsFrame* results_frame_ = nullptr;
    wxToolBar* toolbar_ = nullptr;
    wxAuiNotebook* book_ = nullptr;
    wxSplitterWindow* sp_main_ = nullptr;

    SchematicPage* page();
    const SchematicPage* page() const;
    SchematicPage* make_page(int insert_at);
    void bind_page(SchematicPage* pg);
    void activate_page(int i);
    void close_page(int i);
    // Swap the right-hand column between the analysis cards and the
    // properties editor. show_props(true) reveals the properties editor for
    // the current selection; the analysis cards come back when the selection
    // is cleared.
    void show_props(SchematicPage* pg, bool on);

    void build_menu();
    void build_toolbar();
    void build_layout();
    void update_title();

    // file ops
    void on_new(wxCommandEvent&);
    void on_open(wxCommandEvent&);
    void on_save(wxCommandEvent&);
    void on_save_as(wxCommandEvent&);
    bool maybe_save();
    bool do_save_as();

    // editing
    void on_delete(wxCommandEvent&);
    void on_undo(wxCommandEvent&);
    void on_redo(wxCommandEvent&);
    void on_undo_cmd();
    void on_redo_cmd();
    void after_undo_redo();

    // analysis
    void run_card(int index); // run one card
    void on_ignore_neg(wxCommandEvent&);   // opens the "Negligible terms" dialog

    // keyboard placement map / instance menu
    bool handle_shortcut(wxKeyEvent& e);
    void sync_palette();
    void show_instance_menu();
    void prompt_net_labels();

    void on_about(wxCommandEvent&);
    void on_shortcuts(wxCommandEvent&);
    void on_howto(wxCommandEvent&);
    void on_experimental(wxCommandEvent&);
    void on_show_grid(wxCommandEvent&);
    void on_zoom_fit(wxCommandEvent&);
    void on_copy(wxCommandEvent&);
    void on_paste(wxCommandEvent&);
    void on_tab_close(wxCommandEvent&);

    // plumbing
    void document_changed();
    void selection_changed(const std::string& sel);

    // Is the focused control a text-entry widget (so shortcuts must not fire)?
    bool focus_is_text_entry() const;

    // Lazily build the floating analysis-results window.
    AnalysisResultsFrame* ensure_results_frame();

    wxDECLARE_EVENT_TABLE();
};

} // namespace envlp
