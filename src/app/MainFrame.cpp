#include "MainFrame.h"

#include "AnalysisPanel.h"
#include "AnalysisResultsFrame.h"
#include "PalettePanel.h"
#include "PropertiesPanel.h"
#include "SchematicCanvas.h"
#include "Theme.h"
#include "core/Analysis.h"
#include "core/Eng.h"
#include "core/SpiceModel.h"

#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/aui/auibook.h>
#include <wx/textdlg.h>
#include <wx/toolbar.h>
#include <wx/checkbox.h>
#include <wx/choice.h>

namespace symcirc {

namespace {

// Pretty, scrollable Help content. A plain wxMessageBox wraps long lines
// badly and has no scrolling, so the Help items share one read-only,
// monospaced text window with a copy button.

const char* kAboutText =
    "SymCirc -- symbolic circuit analysis with low-entropy forms.\n"
    "\n"
    "Draw a schematic, add an analysis card, then press its Run button.\n"
    "SymCirc ranks terms by order-of-magnitude estimates and prunes the\n"
    "negligible ones, so the transfer function stays as readable as the\n"
    "hand analysis you would do on a whiteboard.\n"
    "\n"
    "See Help > How to use (F1) for the analysis cards and\n"
    "Help > Experimental features for what is still in flux.";

const char* kShortcutsText =
    "CANVAS\n"
    "  R C L            resistor, capacitor, inductor\n"
    "  V B              voltage source, current source\n"
    "  M                N-MOSFET   (press M again) P-MOSFET\n"
    "  Q                NPN        (press Q again) PNP\n"
    "  D  or  Y         diode\n"
    "  O                op-amp\n"
    "  G                ground     (Shift+G) supply rail (VDD)\n"
    "  T  K             transformer, coupled inductors\n"
    "  W                wire tool\n"
    "  N                place net label(s)\n"
    "  I                component menu (every part)\n"
    "\n"
    "  Space            rotate the ghost / selection 90 deg\n"
    "  Shift+Space      flip horizontally  (wiring: swap the route)\n"
    "  Ctrl+Space       flip vertically\n"
    "  F                fit the components to the view\n"
    "  F7               toggle the dot grid\n"
    "  Del              delete the selection\n"
    "  Ctrl+C / Ctrl+V  copy / paste\n"
    "  Esc              cancel the current action\n"
    "\n"
    "EDITING\n"
    "  U                undo\n"
    "  Shift+U          redo\n"
    "\n"
    "FILE / TABS\n"
    "  Ctrl+N           new schematic tab\n"
    "  Ctrl+O           open schematic(s)\n"
    "  Ctrl+S           save        Ctrl+Shift+S  save as\n"
    "  Ctrl+W           close the current tab\n"
    "\n"
    "HELP\n"
    "  F1               how to use";

const char* kHowtoText =
    "Add analysis cards with the + Add picker on the right; each card has\n"
    "its own Run button. Fill in the input source (in) and the output\n"
    "expression (out) -- e.g. V(out), or V(a)-V(b) for a differential\n"
    "output. Wording in quotes below is the card title.\n"
    "\n"
    "  \"Transfer function (H(s))\"\n"
    "      V(out)/V(in): the small-signal gain. Reports the DC gain, the\n"
    "      pole/zero corner frequencies and the dominant time constant.\n"
    "\n"
    "  \"AC (small-signal)\"\n"
    "      Drives every independent source and superposes the responses,\n"
    "      so there is no 'in' field.\n"
    "\n"
    "  \"PSR / PSRR\"\n"
    "      Ripple on the supply rail referred to the output; set which\n"
    "      supply symbol to perturb.\n"
    "\n"
    "  \"Loop gain (return ratio)\"\n"
    "      Rosenstark return ratio: name the amplifier in 'probe'.\n"
    "\n"
    "  \"Short-circuit current\"\n"
    "      The output current into a short.\n"
    "\n"
    "  \"Input impedance\"   \"Output impedance\"\n"
    "      Zin at the input port; Zout with every source turned off (the\n"
    "      output-impedance card has no 'in' field).\n"
    "\n"
    "  \"Noise\"\n"
    "      Output and input-referred noise density, integrated over the\n"
    "      sweep band, with a per-source contribution breakdown.\n"
    "\n"
    "\"ignore negligible\" (on each card, or the toolbar) drops terms far\n"
    "below the dominant one so the result stays readable.";

const char* kExperimentalText =
    "These features work but their interface or output may still change\n"
    "between versions. Cards that are experimental are labelled in the\n"
    "Analysis panel.\n"
    "\n"
    "  DC analysis\n"
    "      Large-signal operating point (three model modes). The card also\n"
    "      holds the process settings: Vth, Is, uCox, the SPICE model file\n"
    "      and per-device W/L. An ideal current-source output with no\n"
    "      resistive load is genuinely floating and is reported as such.\n"
    "\n"
    "  Differential / common-mode analysis\n"
    "      Adm, Acm and CMRR from the in+ / in- port nodes.\n"
    "\n"
    "  Plotting\n"
    "      The frequency-response plot in the results window.\n"
    "\n"
    "  Switched-capacitor / discrete-time analysis\n"
    "      Not implemented yet: clocked (z-domain) analysis of SC circuits.";

// A read-only, scrollable text window for the Help entries.
void show_text_window(wxWindow* parent, const wxString& title,
                      const wxString& body) {
    auto* frame = new wxFrame(parent, wxID_ANY, title, wxDefaultPosition,
                              parent->FromDIP(wxSize(560, 620)));
    frame->SetBackgroundColour(theme::chrome_bg);
    auto* tc = new wxTextCtrl(frame, wxID_ANY, body, wxDefaultPosition,
                              wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 |
                                  wxTE_DONTWRAP);
    wxFont mono = wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
    if (mono.IsOk()) tc->SetFont(mono);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(tc, 1, wxEXPAND | wxALL, frame->FromDIP(8));
    frame->SetSizer(sizer);
    frame->Show();
}

} // namespace

enum {
    ID_NEW = wxID_HIGHEST + 1,
    ID_OPEN,
    ID_SAVE,
    ID_SAVE_AS,
    ID_DELETE,
    ID_UNDO,
    ID_REDO,
    ID_ABOUT_APP,
    ID_INSTANCE,
    ID_IGNORE_NEG,
    ID_WIRE_TOOL,
    ID_SELECT_TOOL,
    ID_NET_LABEL,
    ID_ZOOM_FIT,
    ID_COPY,
    ID_PASTE,
    ID_SHOW_GRID,
    ID_SHORTCUTS,
    ID_HOWTO,
    ID_EXPERIMENTAL,
    ID_TAB_CLOSE,
    ID_PLACE_BASE = wxID_HIGHEST + 100,
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_MENU(ID_NEW, MainFrame::on_new)
    EVT_MENU(ID_OPEN, MainFrame::on_open)
    EVT_MENU(ID_SAVE, MainFrame::on_save)
    EVT_MENU(ID_SAVE_AS, MainFrame::on_save_as)
    EVT_MENU(ID_DELETE, MainFrame::on_delete)
    EVT_MENU(ID_UNDO, MainFrame::on_undo)
    EVT_MENU(ID_REDO, MainFrame::on_redo)
    EVT_MENU(ID_ABOUT_APP, MainFrame::on_about)
    EVT_MENU(ID_SHORTCUTS, MainFrame::on_shortcuts)
    EVT_MENU(ID_HOWTO, MainFrame::on_howto)
    EVT_MENU(ID_EXPERIMENTAL, MainFrame::on_experimental)
    EVT_MENU(ID_IGNORE_NEG, MainFrame::on_ignore_neg)
    EVT_MENU(ID_ZOOM_FIT, MainFrame::on_zoom_fit)
    EVT_MENU(ID_COPY, MainFrame::on_copy)
    EVT_MENU(ID_PASTE, MainFrame::on_paste)
    EVT_MENU(ID_SHOW_GRID, MainFrame::on_show_grid)
    EVT_CHAR_HOOK(MainFrame::on_char_hook)
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "SymCirc", wxDefaultPosition,
              wxDefaultSize) {
    // Size in DIP so the window has the intended visual size at any display
    // scaling (a raw pixel size is applied as physical pixels and comes out
    // half-size on a 200% display).
    SetSize(FromDIP(wxSize(1400, 900)));
    SetBackgroundColour(theme::chrome_bg);

    build_menu();
    build_toolbar();
    build_layout();
    make_page(0); // the first (empty) tab

    update_title();
    CreateStatusBar(2);
    GetStatusBar()->SetBackgroundColour(theme::surface_muted);
    GetStatusBar()->SetForegroundColour(theme::text_muted);
    SetStatusText("Pick a component from the palette, then click the canvas.", 0);
}

SchematicPage* MainFrame::page() {
    if (active_ < 0 || active_ >= int(pages_.size())) return nullptr;
    return pages_[active_].get();
}
const SchematicPage* MainFrame::page() const {
    if (active_ < 0 || active_ >= int(pages_.size())) return nullptr;
    return pages_[active_].get();
}

void MainFrame::build_menu() {
    auto* file = new wxMenu;
    file->Append(ID_NEW, "&New\tCtrl+N", "New schematic");
    file->Append(ID_OPEN, "&Open...\tCtrl+O", "Open schematic in a new tab");
    file->AppendSeparator();
    file->Append(ID_SAVE, "&Save\tCtrl+S", "Save schematic");
    file->Append(ID_SAVE_AS, "Save &As...\tCtrl+Shift+S");
    file->AppendSeparator();
    file->Append(ID_TAB_CLOSE, "&Close tab\tCtrl+W",
                 "Close the current schematic tab");
    file->Append(wxID_EXIT, "E&xit\tAlt+F4");

    auto* edit = new wxMenu;
    // Undo/redo use the Virtuoso-style bare letters. They are NOT real menu
    // accelerators (wxWidgets would fire them while the user types "U" into a
    // property field); the frame's CHAR_HOOK owns them, and the label just
    // advertises them, like the net-label item.
    edit->Append(ID_UNDO, "&Undo  (U)", "Undo the last edit (U)");
    edit->Append(ID_REDO, "&Redo  (Shift+U)",
                 "Redo the last undone edit (Shift+U)");
    edit->AppendSeparator();
    edit->Append(ID_DELETE, "&Delete\tDel", "Delete the selection");
    edit->AppendSeparator();
    edit->Append(ID_COPY, "&Copy\tCtrl+C", "Copy the selected components");
    edit->Append(ID_PASTE, "&Paste\tCtrl+V", "Paste at the cursor");
    edit->AppendSeparator();
    // No bare-letter accelerators here: wxWidgets consults menu accelerators
    // before the focused control, so "N"/"F" would fire while the user types
    // those letters into a property field. The frame's CHAR_HOOK handles the
    // bare letters (only when focus is NOT a text entry).
    edit->Append(ID_NET_LABEL, "Place &net label(s)...",
                 "Name one or more nets (also N)");

    auto* view = new wxMenu;
    view->Append(ID_ZOOM_FIT, "&Fit components",
                 "Zoom to frame every component (also F)");
    view->AppendSeparator();
    view->AppendCheckItem(ID_SHOW_GRID, "Show &grid\tF7",
                          "Toggle the 10-unit dot grid");
    view->Check(ID_SHOW_GRID, true);
    view->AppendSeparator();
    mi_ignore_ = view->AppendCheckItem(
        ID_IGNORE_NEG, "&Ignore negligible terms",
        "Drop terms that are far below the dominant one (low entropy)");
    mi_ignore_->Check(true);

    auto* help = new wxMenu;
    help->Append(ID_HOWTO, "&How to use...\tF1",
                 "What each analysis does and what it needs");
    help->Append(ID_SHORTCUTS, "&Keyboard shortcuts...",
                 "Every canvas and app shortcut");
    help->Append(ID_EXPERIMENTAL, "&Experimental features...",
                 "Experimental features and the roadmap");
    help->AppendSeparator();
    help->Append(ID_ABOUT_APP, "&About SymCirc");

    auto* bar = new wxMenuBar;
    bar->Append(file, "&File");
    bar->Append(edit, "&Edit");
    bar->Append(view, "&View");
    bar->Append(help, "&Help");
    SetMenuBar(bar);
}

void MainFrame::build_toolbar() {
    // Use flat text tools on the muted chrome background (analog-canvas's
    // toolbar-row). Select / Wire are radio-style check tools so the active
    // tool reads as "pressed" (accent) the way the reference draw-tools do.
    toolbar_ = CreateToolBar(wxTB_HORIZONTAL | wxTB_TEXT | wxTB_NOICONS |
                             wxTB_FLAT | wxTB_NODIVIDER);
    toolbar_->SetBackgroundColour(theme::surface_muted);

    auto add = [&](int id, const wxString& label, const wxString& help) {
        toolbar_->AddTool(id, label, wxBitmapBundle(), help);
    };
    toolbar_->AddRadioTool(ID_SELECT_TOOL, "Select", wxBitmapBundle(),
                           wxBitmapBundle(), "Box-select and drag components");
    toolbar_->AddRadioTool(ID_WIRE_TOOL, "Wire (W)", wxBitmapBundle(),
                           wxBitmapBundle(), "Draw a wire");
    toolbar_->ToggleTool(ID_SELECT_TOOL, true);
    toolbar_->AddSeparator();
    add(ID_NET_LABEL, "Net label (N)", "Name one or more nets");
    add(ID_ZOOM_FIT, "Fit (F)", "Zoom to frame every component");
    toolbar_->AddSeparator();
    toolbar_->AddCheckTool(ID_IGNORE_NEG, "Ignore negligible",
                           wxBitmapBundle(), wxBitmapBundle(),
                           "Drop terms far below the dominant one");
    toolbar_->ToggleTool(ID_IGNORE_NEG, true);
    toolbar_->Realize();

    toolbar_->Bind(wxEVT_TOOL, [this](wxCommandEvent& e) {
        SchematicPage* pg = page();
        switch (e.GetId()) {
        case ID_SELECT_TOOL:
            if (pg) pg->canvas->set_tool(Tool::Select);
            sync_palette();
            break;
        case ID_WIRE_TOOL:
            if (pg) pg->canvas->set_tool(Tool::Wire);
            sync_palette();
            SetStatusText("Wire: click to start, click again to finish; Space "
                          "swaps the route; Esc cancels.",
                          0);
            break;
        case ID_NET_LABEL:
            prompt_net_labels();
            break;
        case ID_ZOOM_FIT:
            if (pg) pg->canvas->zoom_to_fit();
            break;
        case ID_IGNORE_NEG:
            set_ignore_negligible(e.IsChecked());
            break;
        default:
            break;
        }
    });
}

void MainFrame::on_zoom_fit(wxCommandEvent&) {
    if (page()) page()->canvas->zoom_to_fit();
    SetStatusText("Zoomed to fit the components.", 0);
}

void MainFrame::on_copy(wxCommandEvent&) {
    if (page()) page()->canvas->copy_selection();
}
void MainFrame::on_paste(wxCommandEvent&) {
    if (page()) page()->canvas->paste_clipboard();
}

// The "Ignore negligible terms" switch is shared by the View menu, the toolbar
// and every analysis card of the active page, so keep them in sync.
void MainFrame::set_ignore_negligible(bool on) {
    SchematicPage* pg = page();
    if (!pg) return;
    pg->doc.req.prune = on;
    if (mi_ignore_) mi_ignore_->Check(on);
    if (toolbar_) toolbar_->ToggleTool(ID_IGNORE_NEG, on);
    for (auto& c : pg->analysis->cards()) c.prune = on;
    pg->doc.analysis_cards = pg->analysis->serialize();
    pg->analysis->refresh(&pg->doc);
    pg->doc.dirty = true;
    update_title();
    SetStatusText(on ? "Negligible terms will be ignored (low entropy)."
                     : "Keeping every term (exact form).",
                  0);
}

void MainFrame::on_ignore_neg(wxCommandEvent& e) {
    set_ignore_negligible(e.IsChecked());
}

void MainFrame::on_show_grid(wxCommandEvent& e) {
    if (page()) page()->canvas->set_show_grid(e.IsChecked());
}

// ---------------------------------------------------------------------------
// Tabbed layout: the palette and the analysis column are shared; the middle
// pane is a notebook with one tab per open schematic. Each page owns its
// canvas, properties strip, splitters and analysis cards.
// ---------------------------------------------------------------------------
void MainFrame::build_layout() {
    sp_main_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize,
                                    wxSP_LIVE_UPDATE);
    sp_main_->SetMinimumPaneSize(FromDIP(60));
    sp_main_->SetSashGravity(0.0);

    // The palette is shared by every tab, so it binds to the active page's
    // document lazily (it only needs the document when a tile is clicked).
    palette_ = new PalettePanel(sp_main_, nullptr);

    book_ = new wxAuiNotebook(sp_main_, wxID_ANY, wxDefaultPosition,
                              wxDefaultSize,
                              wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE |
                                  wxAUI_NB_CLOSE_ON_ALL_TABS |
                                  wxAUI_NB_SCROLL_BUTTONS);
    book_->SetBackgroundColour(theme::surface_muted);

    sp_main_->SplitVertically(palette_, book_);

    auto* frame_sizer = new wxBoxSizer(wxVERTICAL);
    frame_sizer->Add(sp_main_, 1, wxEXPAND);
    SetSizer(frame_sizer);

    SetMinSize(FromDIP(wxSize(900, 600)));

    // Switching tabs re-points the palette and toolbar at the new document.
    book_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, [this](wxAuiNotebookEvent& e) {
        int sel = e.GetSelection();
        if (sel >= 0 && sel < int(pages_.size()) && sel != active_) {
            active_ = sel;
            if (page()) {
                palette_->SetDocument(&page()->doc);
                if (toolbar_)
                    toolbar_->ToggleTool(ID_IGNORE_NEG, page()->doc.req.prune);
                if (mi_ignore_) mi_ignore_->Check(page()->doc.req.prune);
                page()->props->refresh(&page()->doc, page()->canvas->selection());
                show_props(page(), page()->props_shown &&
                                       !page()->canvas->selection().empty());
                page()->canvas->Refresh();
            }
            update_title();
        }
        e.Skip();
    });
    book_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& e) {
        // Take over deletion entirely: veto wxAuiNotebook's default and let
        // close_page() decide (it also refuses to leave zero tabs).
        e.Veto();
        close_page(e.GetSelection());
    });

    // ---- palette / shared plumbing ----
    palette_->on_tool_changed = [this] {
        if (!page()) return;
        page()->canvas->begin_place(palette_->place_kind(), 0);
        SetStatusText("Click the canvas to place. Space rotates, Esc leaves.",
                      0);
    };

    CallAfter([this] {
        wxSize cs = GetClientSize();
        Layout();
        sp_main_->SetSashPosition(FromDIP(200));
        if (page()) {
            page()->sp_right->SetSashPosition(
                std::max(FromDIP(160), cs.x - FromDIP(200) - FromDIP(400)));
        }
        Layout();
        Refresh();
    });
}

// Create a page (schematic + panes) and its notebook tab. `insert_at` is the
// tab index; -1 appends. Returns the new page.
SchematicPage* MainFrame::make_page(int insert_at) {
    auto* pg = new SchematicPage();
    // sensible defaults before any panel reads the request
    pg->doc.req.input_ref = "V1";
    pg->doc.req.output = "V(out)";
    pg->doc.req.prune = true;

    auto* host = new wxWindow(book_, wxID_ANY);
    host->SetBackgroundColour(theme::surface_muted);
    auto* hs = new wxBoxSizer(wxVERTICAL);

    pg->sp_right = new wxSplitterWindow(host, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, wxSP_LIVE_UPDATE);
    pg->sp_right->SetMinimumPaneSize(FromDIP(120));
    pg->sp_right->SetSashGravity(1.0);

    // The right column hosts both the analysis cards and the properties
    // editor in one container; toggling which child is shown swaps the column
    // instantly without disturbing the canvas.
    pg->canvas = new SchematicCanvas(pg->sp_right, &pg->doc);
    pg->right_host = new wxPanel(pg->sp_right);
    pg->right_host->SetBackgroundColour(theme::chrome_bg);
    auto* rs = new wxBoxSizer(wxVERTICAL);
    pg->analysis = new AnalysisPanel(pg->right_host);
    pg->props = new PropertiesPanel(pg->right_host);
    rs->Add(pg->analysis, 1, wxEXPAND);
    rs->Add(pg->props, 1, wxEXPAND);
    pg->right_host->SetSizer(rs);
    pg->props->Hide();
    pg->sp_right->SplitVertically(pg->canvas, pg->right_host);

    hs->Add(pg->sp_right, 1, wxEXPAND);
    host->SetSizer(hs);

    int idx = insert_at < 0 ? int(pages_.size()) : insert_at;
    pages_.insert(pages_.begin() + idx, std::unique_ptr<SchematicPage>(pg));
    // Indices shifted: adjust the active tab if it was after the insertion.
    if (active_ >= idx) ++active_;
    book_->InsertPage(idx, host, "untitled", true);

    bind_page(pg);
    active_ = idx;
    book_->SetSelection(idx);
    palette_->SetDocument(&pg->doc);
    update_title();
    // Defer the fit until the host has a real client size.
    CallAfter([pg] { pg->canvas->zoom_to_fit(); });
    return pg;
}

// Swap the right-hand column between the analysis cards and the properties
// editor. Both are children of the same splitter, so we only toggle which one
// wxSizer/the splitter shows.
void MainFrame::show_props(SchematicPage* pg, bool on) {
    if (!pg || pg->props_shown == on) return;
    pg->props_shown = on;
    if (on) {
        pg->analysis->Hide();
        pg->props->Show();
        pg->props->Layout();
    } else {
        pg->props->Hide();
        pg->analysis->Show();
        pg->analysis->Layout();
    }
    if (pg->right_host) {
        pg->right_host->Layout();
        pg->right_host->Refresh();
    }
}

void MainFrame::bind_page(SchematicPage* pg) {
    pg->canvas->on_document_changed = [this] { document_changed(); };    pg->canvas->on_selection_changed = [this](const std::string& s) {
        selection_changed(s);
    };
    pg->canvas->on_status = [this](const std::string& s) {
        SetStatusText(wxString::FromUTF8(s), 0);
    };
    pg->canvas->on_view_changed = [this](double z, double vx, double vy) {
        SetStatusText(wxString::Format("zoom=%.2f  view=(%.0f, %.0f)", z, vx,
                                       vy),
                      1);
    };
    pg->canvas->on_push_undo = [pg] { pg->doc.push_undo(); };
    pg->canvas->on_wire_selected = [this, pg](int wi, std::string name) {
        pg->props->refresh(&pg->doc, "#wire" + std::to_string(wi));
        show_props(pg, true);
        if (name.empty())
            SetStatusText("Wire selected -- name its net on the right (a "
                          "label is created above it).",
                          0);
        else
            SetStatusText("Net: " + name, 0);
    };

    pg->props->on_edited = [this, pg] {
        pg->canvas->invalidate_nets();
        pg->canvas->Refresh(false);
        SetStatusText("Circuit changed -- press Run on an analysis card.", 0);
    };
    pg->props->on_selection_changed = [this, pg](const std::string& s) {
        pg->canvas->set_selection(s);
        selection_changed(s);
    };
    pg->props->on_wire_name = [this, pg](int wi, const std::string& name) {
        pg->canvas->set_wire_net_name(wi, name);
    };
    pg->props->on_label_font = [this, pg](int li, int size) {
        pg->canvas->set_label_font_size(li, size);
    };

    pg->analysis->refresh(&pg->doc);
    pg->analysis->on_changed = [this, pg] {
        pg->doc.analysis_cards = pg->analysis->serialize();
        pg->doc.dirty = true;
        update_title();
    };
    pg->analysis->on_run_one = [this](int i) { run_card(i); };
    pg->analysis->on_results = [this] {
        if (auto* f = ensure_results_frame()) {
            f->popup();
            f->select_page(0);
        }
    };
    pg->analysis->on_tech_changed = [this, pg] {
        // W/L visibility depends on the mode, so rebuild the properties panel.
        pg->props->refresh(&pg->doc, pg->canvas->selection());
    };

    pg->props->refresh(&pg->doc, pg->canvas->selection());
    pg->canvas->report_view();
}

void MainFrame::activate_page(int i) {
    if (i < 0 || i >= int(pages_.size())) return;
    book_->SetSelection(i);
    active_ = i;
    if (page()) page()->canvas->SetFocus();
    update_title();
}

void MainFrame::close_page(int i) {
    if (i < 0 || i >= int(pages_.size())) return;
    if (pages_.size() == 1) {
        // Never leave zero tabs: clear the last one instead of closing it.
        SchematicPage* pg = pages_[0].get();
        pg->doc = Document();
        pg->doc.req.input_ref = "V1";
        pg->doc.req.output = "V(out)";
        pg->doc.dirty = false;
        pg->result.reset();
        pg->canvas->set_selection("");
        pg->canvas->invalidate_nets();
        pg->analysis->deserialize("");
        pg->analysis->refresh(&pg->doc);
        pg->props->refresh(&pg->doc, "");
        show_props(pg, false);
        pg->canvas->Refresh();
        book_->SetPageText(0, "untitled");
        update_title();
        return;
    }
    // Drop the page object first so the canvas's callbacks (fired during
    // destruction) cannot reach a half-deleted page.
    pages_.erase(pages_.begin() + i);
    book_->DeletePage(i);
    active_ = book_->GetSelection();
    if (active_ < 0) active_ = 0;
    if (page()) {
        palette_->SetDocument(&page()->doc);
        if (toolbar_)
            toolbar_->ToggleTool(ID_IGNORE_NEG, page()->doc.req.prune);
        if (mi_ignore_) mi_ignore_->Check(page()->doc.req.prune);
    }
    update_title();
}

// ---------------------------------------------------------------------------
// Application-wide key handling. This is bound at the frame level via
// wxEVT_CHAR_HOOK, so it runs before the focused child gets the key. That is
// what makes Escape (and Space) reliable while placing/wiring, and lets N open
// the net-label prompt from anywhere.
// ---------------------------------------------------------------------------
bool MainFrame::focus_is_text_entry() const {
    wxWindow* w = wxWindow::FindFocus();
    if (!w) return false;
    // wxTextCtrl / wxComboBox (with a text part) / spin text should keep their
    // own keystrokes; everything else lets shortcuts through.
    return dynamic_cast<wxTextCtrl*>(w) != nullptr ||
           dynamic_cast<wxComboBox*>(w) != nullptr ||
           dynamic_cast<wxSpinCtrl*>(w) != nullptr;
}

void MainFrame::on_char_hook(wxKeyEvent& e) {
    // The Lua console / text fields own ordinary typing.
    if (focus_is_text_entry()) {
        e.Skip();
        return;
    }

    // Otherwise the canvas owns Space / Escape / single-letter keys while a
    // ghost or wire is in progress, and the app shortcuts run everywhere else.
    if (page() && page()->canvas->handle_key(e)) return;
    if (handle_shortcut(e)) return;

    // Let menu accelerators (Ctrl+N / F5 / Del ...) run.
    e.Skip();
}

// ---------------------------------------------------------------------------
// Keyboard placement map + quick transform keys.
// ---------------------------------------------------------------------------
bool MainFrame::handle_shortcut(wxKeyEvent& e) {
    SchematicPage* pg = page();
    if (!pg) return false;
    SchematicCanvas* cv = pg->canvas;
    const int code = e.GetKeyCode();
    const bool shift = e.ShiftDown();
    const bool ctrl = e.ControlDown();
    const bool alt = e.AltDown();

    // undo / redo: Virtuoso-style U / Shift+U only (Ctrl+Z/Ctrl+Y are
    // deliberately not bound -- the user asked for a single scheme).
    if (!ctrl && !alt && (code == 'u' || code == 'U')) {
        if (shift) on_redo_cmd();
        else on_undo_cmd();
        return true;
    }
    if (code == WXK_F1) {
        wxCommandEvent dummy;
        on_howto(dummy);
        return true;
    }
    // copy / paste
    if (ctrl && !alt && code == 'C') { cv->copy_selection(); return true; }
    if (ctrl && !alt && code == 'V') { cv->paste_clipboard(); return true; }

    // Space: while wiring, swap the route orientation; while placing or with a
    // selection, rotate / flip.
    if (code == WXK_SPACE) {
        if (cv->wiring()) {
            cv->toggle_wire_orient();
            SetStatusText("Wire route: press Space again to swap.", 0);
        } else if (ctrl) {
            cv->flip_ghost(false);
        } else if (shift) {
            cv->flip_ghost(true);
        } else {
            cv->rotate_ghost(90);
        }
        sync_palette();
        return true;
    }
    if (ctrl || alt) return false; // leave Ctrl/Alt combos to menus

    auto place = [&](syms::Kind k) {
        cv->begin_place(k, 0);
        sync_palette();
        SetStatusText("Placing -- click to drop another, Space rotates, Esc stops.",
                      0);
        return true;
    };
    switch (code) {
    case 'R': return place(syms::Kind::R);
    case 'C': return place(syms::Kind::C);
    case 'L': return place(syms::Kind::L);
    case 'V': return place(syms::Kind::V);
    case 'B': case 'b': return place(syms::Kind::I);
    case 'G': case 'g':
        return place(shift ? syms::Kind::VDD : syms::Kind::GND);
    case 'D': return place(syms::Kind::D);
    case 'T': return place(syms::Kind::T);
    case 'K': return place(syms::Kind::K);
    case 'Y': return place(syms::Kind::D); // common alternate for diode
    case 'O': case 'o': return place(syms::Kind::OPAMP);
    case 'W': case 'w':
        cv->set_tool(Tool::Wire);
        sync_palette();
        SetStatusText("Wire: click to place a segment; click a pin to "
                      "terminate; Enter ends, Esc cancels.",
                      0);
        return true;
    case 'N': case 'n':
        prompt_net_labels();
        return true;
    case 'F': case 'f':
        // F frames all components (repeated F toggles fit / back).
        cv->zoom_to_fit();
        SetStatusText("Zoomed to fit the components.", 0);
        return true;
    case 'M': {
        // M selects the NMOS first; pressing M again (while placing) toggles
        // to the PMOS.
        syms::Kind cur = cv->tool() == Tool::Place
                             ? cv->place_kind()
                             : syms::Kind::PMOS; // first press -> NMOS
        syms::Kind nxt = (cur == syms::Kind::NMOS) ? syms::Kind::PMOS
                                                   : syms::Kind::NMOS;
        return place(nxt);
    }
    case 'Q': case 'q': {
        // Q selects the NPN BJT first; pressing Q again (while placing)
        // toggles to the PNP, mirroring the M / NMOS-PMOS behaviour.
        syms::Kind cur = cv->tool() == Tool::Place
                             ? cv->place_kind()
                             : syms::Kind::PNP; // first press -> NPN
        syms::Kind nxt = (cur == syms::Kind::NPN) ? syms::Kind::PNP
                                                   : syms::Kind::NPN;
        return place(nxt);
    }
    case 'I':
        show_instance_menu();
        return true;
    }
    return false;
}

// Ask for one or more net names; each is placed on the next clicked net.
void MainFrame::prompt_net_labels() {
    SchematicPage* pg = page();
    if (!pg) return;
    wxTextEntryDialog dlg(this,
                          "Net name(s) to place, separated by spaces:",
                          "Place net label(s)");
    if (dlg.ShowModal() != wxID_OK) return;
    std::string names = dlg.GetValue().ToStdString();
    pg->canvas->begin_label(names);
    pg->canvas->SetFocus();
    sync_palette();
    SetStatusText("Net label: click a net to place the next name; Esc stops.", 0);
}

void MainFrame::sync_palette() {
    SchematicPage* pg = page();
    if (!pg) return;
    palette_->set_active(pg->canvas->tool(), pg->canvas->place_kind());
    // Keep the toolbar radio buttons in step with the canvas tool (e.g. after
    // the W / S keys), the way the reference's draw-tools reflect the mode.
    if (toolbar_) {
        Tool t = pg->canvas->tool();
        toolbar_->ToggleTool(ID_SELECT_TOOL, t == Tool::Select);
        toolbar_->ToggleTool(ID_WIRE_TOOL, t == Tool::Wire);
        toolbar_->Refresh();
    }
}

void MainFrame::show_instance_menu() {
    // Cadence-style popup listing every component, keyed by mnemonic.
    struct Ent { const char* label; syms::Kind kind; };
    static const Ent entries[] = {
        {"Resistor\tR", syms::Kind::R},
        {"Capacitor\tC", syms::Kind::C},
        {"Inductor\tL", syms::Kind::L},
        {"Voltage source\tV", syms::Kind::V},
        {"Current source\tB", syms::Kind::I},
        {"Ground\tG", syms::Kind::GND},
        {"Supply rail (VDD)\tShift+G", syms::Kind::VDD},
        {"Diode\tD", syms::Kind::D},
        {"N-MOSFET\tM", syms::Kind::NMOS},
        {"P-MOSFET\tM (again)", syms::Kind::PMOS},
        {"NPN BJT\tQ", syms::Kind::NPN},
        {"PNP BJT\tQ (again)", syms::Kind::PNP},
        {"Transformer\tT", syms::Kind::T},
        {"Inductor coupling\tK", syms::Kind::K},
        {"Op-amp (single out)", syms::Kind::OPAMP},
        {"Fully differential op-amp", syms::Kind::FDOPAMP},
        {"Amplifier (gain block)", syms::Kind::AMP},
        {"Nullor", syms::Kind::NULLOR},
        {"VCVS (E)", syms::Kind::E},
        {"VCCS (G)", syms::Kind::G},
        {"CCVS (H)", syms::Kind::CCVS},
        {"CCCS (F)", syms::Kind::CCCS},
        {"Ideal 1/s block", syms::Kind::IS},
        {"Ideal s block", syms::Kind::SBLK},
    };
    wxMenu menu;
    const int n = int(sizeof(entries) / sizeof(entries[0]));
    for (int i = 0; i < n; ++i)
        menu.Append(ID_PLACE_BASE + i, entries[i].label);
    // capture the table by pointer to a static, not by reference to a local
    const Ent* ents = entries;
    menu.Bind(wxEVT_MENU, [this, ents, n](wxCommandEvent& ev) {
        int i = ev.GetId() - ID_PLACE_BASE;
        if (i < 0 || i >= n) return;
        if (!page()) return;
        page()->canvas->begin_place(ents[i].kind, 0);
        sync_palette();
        SetStatusText("Placing -- click to drop another, Space rotates, Esc stops.",
                      0);
    });
    PopupMenu(&menu);
}

// ---------------------------------------------------------------------------
void MainFrame::update_title() {
    const SchematicPage* pg = page();
    wxString name = (!pg || pg->doc.path.empty())
                        ? wxString("untitled")
                        : wxString::FromUTF8(pg->doc.path)
                              .AfterLast('\\')
                              .AfterLast('/');
    bool dirty = pg && pg->doc.dirty;
    SetTitle(wxString::Format("SymCirc -- %s%s", name,
                              dirty ? wxString(" *") : wxString("")));
    // Keep the tab label in step with the dirty marker.
    if (book_ && active_ >= 0 && active_ < int(book_->GetPageCount()))
        book_->SetPageText(active_, name + (dirty ? " *" : ""));
}

// Document changed via the canvas (component moved, wire/label placed, etc.).
// The selection didn't necessarily change, so the props panel is *not*
// refreshed here -- that would tear down and rebuild all its widgets, causing
// visible re-layout of the canvas (the props panel's height affects the
// canvas's height). The props panel refreshes on selection changes, which is
// what it actually depends on; this handler only updates the title and status.
void MainFrame::document_changed() {
    update_title();
    SetStatusText("Circuit changed -- press Run on an analysis card.", 0);
}

void MainFrame::selection_changed(const std::string& sel) {
    SchematicPage* pg = page();
    if (!pg) return;
    pg->props->refresh(&pg->doc, sel);
    // Clicking a component (or wire/label) swaps the right column to the
    // properties editor; clearing the selection brings the analysis cards
    // back.
    show_props(pg, !sel.empty());
    pg->canvas->Refresh(false); // deferred repaint
}

// ---------------------------------------------------------------------------
bool MainFrame::maybe_save() {
    SchematicPage* pg = page();
    if (!pg || !pg->doc.dirty) return true;
    wxMessageDialog dlg(this,
                        "This schematic has unsaved changes. Save them?",
                        "Unsaved changes",
                        wxYES_NO | wxCANCEL | wxICON_QUESTION);
    int r = dlg.ShowModal();
    if (r == wxID_CANCEL) return false;
    if (r == wxID_YES) {
        wxCommandEvent dummy;
        on_save(dummy);
    }
    return true;
}

void MainFrame::on_new(wxCommandEvent&) { make_page(-1); }

void MainFrame::on_open(wxCommandEvent&) {
    wxFileDialog dlg(this, "Open schematic", "", "",
                     "SymCirc circuits (*.scx)|*.scx|All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dlg.ShowModal() != wxID_OK) return;
    wxArrayString paths;
    dlg.GetPaths(paths);
    for (const auto& p : paths) open_path(p);
}

void MainFrame::open_path(const wxString& p) {
    std::string err;
    Document nd;
    if (!nd.load(p.ToStdString(), err)) {
        wxMessageBox(wxString::FromUTF8(err), "Open failed", wxICON_ERROR, this);
        return;
    }
    // Reuse the current tab if it is an untouched, untitled, empty schematic;
    // otherwise open a fresh tab so the user can compare schematics.
    SchematicPage* pg = page();
    bool reuse = pg && pg->doc.path.empty() && !pg->doc.dirty &&
                 pg->doc.circuit.comps.empty() && pages_.size() == 1;
    if (!reuse) {
        pg = make_page(-1);
    }
    pg->doc = std::move(nd);
    pg->result.reset();
    pg->canvas->set_selection("");
    pg->canvas->invalidate_nets();
    pg->analysis->deserialize(pg->doc.analysis_cards);
    pg->analysis->refresh(&pg->doc);
    pg->props->refresh(&pg->doc, "");
    CallAfter([this, pg] { pg->canvas->zoom_to_fit(); });
    pg->canvas->Refresh();
    // Tab label = file name.
    wxString name = p.AfterLast('\\').AfterLast('/');
    book_->SetPageText(active_, name);
    update_title();
    SetStatusText("Loaded " + p, 0);
}

void MainFrame::on_save(wxCommandEvent&) {
    SchematicPage* pg = page();
    if (!pg) return;
    if (pg->doc.path.empty()) {
        do_save_as();
        return;
    }
    std::string err;
    if (!pg->doc.save(pg->doc.path, err)) {
        wxMessageBox(wxString::FromUTF8(err), "Save failed", wxICON_ERROR, this);
        return;
    }
    update_title();
    SetStatusText("Saved " + wxString::FromUTF8(pg->doc.path), 0);
}

bool MainFrame::do_save_as() {
    SchematicPage* pg = page();
    if (!pg) return false;
    wxFileDialog dlg(this, "Save schematic", "", "",
                     "SymCirc circuits (*.scx)|*.scx|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return false;
    std::string err;
    if (!pg->doc.save(dlg.GetPath().ToStdString(), err)) {
        wxMessageBox(wxString::FromUTF8(err), "Save failed", wxICON_ERROR, this);
        return false;
    }
    book_->SetPageText(active_, dlg.GetPath().AfterLast('\\').AfterLast('/'));
    update_title();
    SetStatusText("Saved " + dlg.GetPath(), 0);
    return true;
}

void MainFrame::on_save_as(wxCommandEvent&) { do_save_as(); }

void MainFrame::on_tab_close(wxCommandEvent&) { close_page(active_); }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
AnalysisResultsFrame* MainFrame::ensure_results_frame() {
    if (!results_frame_) results_frame_ = new AnalysisResultsFrame(this);
    return results_frame_;
}

void MainFrame::on_undo(wxCommandEvent&) { on_undo_cmd(); }
void MainFrame::on_redo(wxCommandEvent&) { on_redo_cmd(); }

void MainFrame::on_undo_cmd() {
    SchematicPage* pg = page();
    if (!pg) return;
    if (!pg->doc.can_undo()) {
        SetStatusText("Nothing to undo.", 0);
        return;
    }
    pg->doc.undo();
    after_undo_redo();
    SetStatusText("Undo.", 0);
}

void MainFrame::on_redo_cmd() {
    SchematicPage* pg = page();
    if (!pg) return;
    if (!pg->doc.can_redo()) {
        SetStatusText("Nothing to redo.", 0);
        return;
    }
    pg->doc.redo();
    after_undo_redo();
    SetStatusText("Redo.", 0);
}

void MainFrame::after_undo_redo() {
    SchematicPage* pg = page();
    if (!pg) return;
    // the selection may point at something that no longer exists
    pg->canvas->set_selection("");
    pg->canvas->invalidate_nets(); // Document::undo/redo bypass the canvas
    pg->canvas->Refresh();
    pg->props->refresh(&pg->doc, "");
    update_title();
}

void MainFrame::on_delete(wxCommandEvent&) {
    if (page()) page()->canvas->delete_selection();
}

// Run one analysis card. (There is no "run all": use Run on each card.)
void MainFrame::run_card(int index) {
    SchematicPage* pg = page();
    if (!pg) return;
    std::string err;
    syms::Circuit c = pg->doc.resolved(err);
    if (!err.empty()) {
        wxMessageBox(wxString::FromUTF8(err), "Cannot analyze",
                     wxICON_ERROR, this);
        return;
    }

    auto& cards = pg->analysis->cards();
    if (cards.empty()) {
        // no cards configured: fall back to one transfer-function run
        AnalysisCard def;
        def.input_ref = pg->doc.req.input_ref;
        def.output = pg->doc.req.output;
        def.sweep = pg->doc.req.sweep;
        def.prune = pg->doc.req.prune;
        def.use_parallel = pg->doc.req.use_parallel;
        def.approx_factor = pg->doc.req.approx_factor;
        cards.push_back(def);
        pg->analysis->refresh(&pg->doc);
    }
    if (index < 0 || index >= int(cards.size())) return;
    if (!cards[index].enabled) {
        wxMessageBox("This analysis step is disabled.", "Nothing to run",
                     wxICON_INFORMATION, this);
        return;
    }

    const AnalysisCard& card = cards[index];
    syms::AnalysisSpec sp;
    sp.kind = card.kind;
    sp.input_ref = card.input_ref.empty() ? pg->doc.req.input_ref
                                          : card.input_ref;
    sp.output = card.output.empty() ? pg->doc.req.output : card.output;
    sp.probe_ref = card.probe_ref;
    sp.input_port_p = card.in_port_p;
    sp.input_port_n = card.in_port_n;
    sp.sweep = card.sweep;
    sp.f0_hz = card.sweep.f_start_hz;
    sp.threshold_db = card.threshold_db;
    sp.global_ref = card.global_ref;
    sp.prune = card.prune;
    sp.use_parallel = card.use_parallel;
    sp.approx_factor = card.approx_factor;
    sp.tech = pg->doc.tech;

    syms::CardResult cr;
    try {
        cr = syms::run_analysis(c, sp);
    } catch (const std::exception& e) {
        wxMessageBox(wxString::FromUTF8(e.what()), "Analysis failed",
                     wxICON_ERROR, this);
        return;
    }

    auto* rf = ensure_results_frame();
    std::string report = cr.title + "\n" +
                         std::string(cr.title.size() + 8, '-') + "\n" +
                         cr.report + "\n";
    rf->results()->set_text(report);
    rf->results()->set_latex(cr.latex);
    rf->set_report(cr.latex, cr.latex_report);
    if (cr.has_transfer)
        pg->result = std::make_unique<syms::AnalysisResult>(cr.transfer);
    rf->bode()->set_title(card.title);
    rf->bode()->set_result(pg->result.get());
    if (rf->lua()) rf->lua()->set_result(pg->result.get());
    rf->popup();
    rf->select_page(0); // land on the "Results" (typeset) tab
    SetStatusText("Analysis OK -- see the results window.", 0);
}

// ---------------------------------------------------------------------------
void MainFrame::on_shortcuts(wxCommandEvent&) {
    show_text_window(this, "Keyboard shortcuts", kShortcutsText);
}

void MainFrame::on_howto(wxCommandEvent&) {
    show_text_window(this, "How to use", kHowtoText);
}

void MainFrame::on_experimental(wxCommandEvent&) {
    show_text_window(this, "Experimental features & roadmap", kExperimentalText);
}

void MainFrame::on_about(wxCommandEvent&) {
    show_text_window(this, "About SymCirc", kAboutText);
}

} // namespace symcirc
