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
#include <wx/textdlg.h>
#include <wx/toolbar.h>
#include <wx/checkbox.h>
#include <wx/choice.h>

namespace symcirc {

enum {
    ID_NEW = wxID_HIGHEST + 1,
    ID_OPEN,
    ID_SAVE,
    ID_SAVE_AS,
    ID_RUN,
    ID_ROTATE,
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
    ID_DC_SETTINGS,
    ID_SHOW_GRID,
    ID_PLACE_BASE = wxID_HIGHEST + 100,
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_MENU(ID_NEW, MainFrame::on_new)
    EVT_MENU(ID_OPEN, MainFrame::on_open)
    EVT_MENU(ID_SAVE, MainFrame::on_save)
    EVT_MENU(ID_SAVE_AS, MainFrame::on_save_as)
    EVT_MENU(ID_RUN, MainFrame::on_run)
    EVT_MENU(ID_ROTATE, MainFrame::on_rotate)
    EVT_MENU(ID_DELETE, MainFrame::on_delete)
    EVT_MENU(ID_UNDO, MainFrame::on_undo)
    EVT_MENU(ID_REDO, MainFrame::on_redo)
    EVT_MENU(ID_ABOUT_APP, MainFrame::on_about)
    EVT_MENU(ID_IGNORE_NEG, MainFrame::on_ignore_neg)
    EVT_MENU(ID_ZOOM_FIT, MainFrame::on_zoom_fit)
    EVT_MENU(ID_COPY, MainFrame::on_copy)
    EVT_MENU(ID_PASTE, MainFrame::on_paste)
    EVT_MENU(ID_DC_SETTINGS, MainFrame::on_dc_settings)
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
    // sensible default analysis request (before panels read it)
    doc_.req.input_ref = "V1";
    doc_.req.output = "V(out)";

    build_menu();
    build_toolbar();
    build_layout();

    update_title();
    CreateStatusBar(2);
    GetStatusBar()->SetBackgroundColour(theme::surface_muted);
    GetStatusBar()->SetForegroundColour(theme::text_muted);
    SetStatusText("Pick a component from the palette, then click the canvas.", 0);
}

void MainFrame::build_menu() {
    auto* file = new wxMenu;
    file->Append(ID_NEW, "&New\tCtrl+N", "New schematic");
    file->Append(ID_OPEN, "&Open...\tCtrl+O", "Open schematic");
    file->Append(ID_SAVE, "&Save\tCtrl+S", "Save schematic");
    file->Append(ID_SAVE_AS, "Save &As...\tCtrl+Shift+S");
    file->AppendSeparator();
    file->Append(wxID_EXIT, "E&xit\tAlt+F4");

    auto* edit = new wxMenu;
    edit->Append(ID_UNDO, "&Undo\tCtrl+Z", "Undo the last edit  (also U)");
    edit->Append(ID_REDO, "&Redo\tCtrl+Y",
                 "Redo the last undone edit  (also Shift+U)");
    edit->AppendSeparator();
    edit->Append(ID_ROTATE, "&Rotate\tCtrl+R", "Rotate the selection 90 deg");
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
    mi_ignore_->Check(doc_.req.prune);

    auto* run = new wxMenu;
    run->Append(ID_RUN, "&Analyze\tF5", "Run the symbolic analysis");

    auto* help = new wxMenu;
    help->Append(ID_ABOUT_APP, "&About SymCirc");

    auto* bar = new wxMenuBar;
    bar->Append(file, "&File");
    bar->Append(edit, "&Edit");
    bar->Append(view, "&View");
    bar->Append(run, "&Run");
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
    toolbar_->ToggleTool(ID_IGNORE_NEG, doc_.req.prune);
    toolbar_->AddSeparator();
    add(ID_DC_SETTINGS, "DC settings...",
        "Process values for the DC analysis (Vth, Is)");
    toolbar_->AddSeparator();
    add(ID_RUN, "Analyze (F5)", "Run the symbolic analysis");
    toolbar_->Realize();

    toolbar_->Bind(wxEVT_TOOL, [this](wxCommandEvent& e) {
        switch (e.GetId()) {
        case ID_SELECT_TOOL:
            canvas_->set_tool(Tool::Select);
            sync_palette();
            break;
        case ID_WIRE_TOOL:
            canvas_->set_tool(Tool::Wire);
            sync_palette();
            SetStatusText("Wire: click to start, click again to finish; Space "
                          "swaps the route; Esc cancels.",
                          0);
            break;
        case ID_NET_LABEL:
            prompt_net_labels();
            break;
        case ID_ZOOM_FIT:
            canvas_->zoom_to_fit();
            break;
        case ID_IGNORE_NEG:
            set_ignore_negligible(e.IsChecked());
            break;
        case ID_DC_SETTINGS:
            on_dc_settings(e);
            break;
        case ID_RUN:
            run_analysis();
            break;
        default:
            break;
        }
    });
}

void MainFrame::on_zoom_fit(wxCommandEvent&) {
    canvas_->zoom_to_fit();
    SetStatusText("Zoomed to fit the components.", 0);
}

void MainFrame::on_copy(wxCommandEvent&) { canvas_->copy_selection(); }
void MainFrame::on_paste(wxCommandEvent&) { canvas_->paste_clipboard(); }

// The "Ignore negligible terms" switch is shared by the View menu, the toolbar
// and every analysis card, so keep them in sync.
void MainFrame::set_ignore_negligible(bool on) {
    doc_.req.prune = on;
    if (mi_ignore_) mi_ignore_->Check(on);
    if (toolbar_) toolbar_->ToggleTool(ID_IGNORE_NEG, on);
    for (auto& c : analysis_->cards()) c.prune = on;
    doc_.analysis_cards = analysis_->serialize();
    analysis_->refresh(&doc_);
    doc_.dirty = true;
    update_title();
    SetStatusText(on ? "Negligible terms will be ignored (low entropy)."
                     : "Keeping every term (exact form).",
                  0);
}

void MainFrame::on_ignore_neg(wxCommandEvent& e) {
    set_ignore_negligible(e.IsChecked());
}

void MainFrame::on_show_grid(wxCommandEvent& e) {
    canvas_->set_show_grid(e.IsChecked());
}

// DC settings: process values for the large-signal DC analysis, and the model
// mode (gm/Id symbolic, square-law symbolic, or numeric from SPICE models).
void MainFrame::on_dc_settings(wxCommandEvent&) {
    wxDialog dlg(this, wxID_ANY, "DC settings");
    auto* top = new wxBoxSizer(wxVERTICAL);

    auto* grid = new wxFlexGridSizer(2, 2, 6, 10);
    auto add_row = [&](const wxString& label, wxWindow* w) {
        grid->Add(new wxStaticText(&dlg, wxID_ANY, label), 0,
                  wxALIGN_CENTER_VERTICAL);
        grid->Add(w, 1, wxEXPAND);
    };

    auto* mode = new wxChoice(&dlg, wxID_ANY);
    mode->Append("1. gm/Id symbolic (small-signal params)");
    mode->Append("2. Square law symbolic (uCox, W/L symbols)");
    mode->Append("3. Numeric (SPICE models)");
    mode->SetSelection(doc_.tech.dc_mode == syms::DcMode::GmOverId
                           ? 0
                           : doc_.tech.dc_mode == syms::DcMode::SquareLaw ? 1 : 2);
    add_row("Mode", mode);

    auto* vth = new wxTextCtrl(&dlg, wxID_ANY,
                               wxString::FromDouble(doc_.tech.vth, 6));
    add_row("Vth (V)", vth);

    auto* is = new wxTextCtrl(&dlg, wxID_ANY,
                              wxString::FromDouble(doc_.tech.is, 6));
    add_row("Is (A)", is);

    auto* uncox = new wxTextCtrl(&dlg, wxID_ANY,
                                 wxString::FromDouble(doc_.tech.uncox, 8));
    add_row("uN*Cox (A/V^2)", uncox);
    auto* upcox = new wxTextCtrl(&dlg, wxID_ANY,
                                 wxString::FromDouble(doc_.tech.upcox, 8));
    add_row("uP*Cox (A/V^2)", upcox);

    // Model file: a text field plus a Browse button. After a file is chosen the
    // NMOS/PMOS dropdowns are repopulated with every .model name the file
    // contains (so it is obvious that, e.g., "N_1u" is a model).
    auto* mfile = new wxTextCtrl(&dlg, wxID_ANY,
                                 wxString::FromUTF8(doc_.tech.model_file));
    add_row("Model file (.lib)", mfile);
    auto* nmname = new wxComboBox(&dlg, wxID_ANY,
                                  wxString::FromUTF8(doc_.tech.nmos_model),
                                  wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                  wxCB_DROPDOWN);
    add_row("NMOS model", nmname);
    auto* pmname = new wxComboBox(&dlg, wxID_ANY,
                                  wxString::FromUTF8(doc_.tech.pmos_model),
                                  wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                  wxCB_DROPDOWN);
    add_row("PMOS model", pmname);
    auto* browse = new wxButton(&dlg, wxID_ANY, "Browse... / reload models");
    grid->AddSpacer(1);
    grid->Add(browse, 1, wxEXPAND);

    // Split the file's models by polarity into the two dropdowns.
    std::vector<std::string> nm_names, pm_names;
    auto load_models = [&]() {
        // Remember the current selection: Clear() would otherwise blank the
        // field after the items are repopulated.
        wxString cur_nm = nmname->GetValue();
        wxString cur_pm = pmname->GetValue();
        if (cur_nm.IsEmpty())
            cur_nm = wxString::FromUTF8(doc_.tech.nmos_model);
        if (cur_pm.IsEmpty())
            cur_pm = wxString::FromUTF8(doc_.tech.pmos_model);
        nm_names.clear();
        pm_names.clear();
        std::string err;
        std::vector<syms::MosModel> ms =
            syms::parse_spice_models(mfile->GetValue().ToStdString(), err);
        for (const auto& m : ms)
            (m.pmos ? pm_names : nm_names).push_back(m.name);
        nmname->Clear();
        pmname->Clear();
        for (const auto& n : nm_names) nmname->Append(wxString::FromUTF8(n));
        for (const auto& n : pm_names) pmname->Append(wxString::FromUTF8(n));
        // Restore the selection; if it is not among the file's models, keep the
        // typed name so the user's choice is never silently lost.
        nmname->SetValue(cur_nm);
        pmname->SetValue(cur_pm);
        if (!ms.empty())
            SetStatusText(
                wxString::Format("Loaded %zu models (%zu NMOS, %zu PMOS).",
                                 ms.size(), nm_names.size(), pm_names.size()),
                0);
        else if (!err.empty())
            SetStatusText(wxString::FromUTF8(err), 0);
    };
    load_models();
    mfile->Bind(wxEVT_TEXT, [&](wxCommandEvent&) { load_models(); });
    browse->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        wxFileDialog fd(&dlg, "Choose a SPICE model file", "", "",
                        "SPICE models (*.lib;*.mod;*.sp;*.cir)|*.lib;*.mod;*.sp;*.cir|All files (*.*)|*.*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (fd.ShowModal() == wxID_OK) {
            mfile->ChangeValue(fd.GetPath());
            load_models();
        }
    });

    auto* ovr = new wxCheckBox(&dlg, wxID_ANY,
                               "Override small-signal params from numeric DC");
    ovr->SetValue(doc_.tech.override_small_signal);
    grid->AddSpacer(1);
    grid->Add(ovr, 1, wxEXPAND);

    top->Add(grid, 0, wxEXPAND | wxALL, 12);
    top->Add(new wxStaticText(
                 &dlg, wxID_ANY,
                 "Mode 1 uses gm per device. Mode 2 uses uCox and symbolic W/L.\n"
                 "Mode 3 reads a SPICE .lib/.mod (level 1 or 3) for a numeric\n"
                 "operating point; W/L are per-device numbers."),
             0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    auto* buttons = new wxStdDialogButtonSizer();
    auto* ok = new wxButton(&dlg, wxID_OK);
    auto* cancel = new wxButton(&dlg, wxID_CANCEL);
    buttons->AddButton(ok);
    buttons->AddButton(cancel);
    buttons->Realize();
    top->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 10);
    dlg.SetSizerAndFit(top);

    if (dlg.ShowModal() != wxID_OK) return;
    doc_.tech.dc_mode =
        mode->GetSelection() == 0 ? syms::DcMode::GmOverId
        : mode->GetSelection() == 1 ? syms::DcMode::SquareLaw
                                    : syms::DcMode::Numeric;
    double v = 0.0;
    if (vth->GetValue().ToDouble(&v)) doc_.tech.vth = v;
    if (is->GetValue().ToDouble(&v)) doc_.tech.is = v;
    if (uncox->GetValue().ToDouble(&v)) doc_.tech.uncox = v;
    if (upcox->GetValue().ToDouble(&v)) doc_.tech.upcox = v;
    doc_.tech.model_file = mfile->GetValue().ToStdString();
    doc_.tech.nmos_model = nmname->GetValue().ToStdString();
    doc_.tech.pmos_model = pmname->GetValue().ToStdString();
    doc_.tech.override_small_signal = ovr->GetValue();
    // W/L visibility depends on the mode, so rebuild the properties panel.
    props_->refresh(&doc_, canvas_->selection());
    SetStatusText("DC settings updated.", 0);
}

void MainFrame::build_layout() {
    // Three nested splitters, giving a draggable sash between every pane:
    //   sp_main:   palette  | sp_right            (vertical sash)
    //   sp_right:  sp_bottom | analysis           (vertical sash)
    //   sp_bottom: canvas   | props               (horizontal sash)
    // Sash gravity routes the frame's resize to the canvas: palette and the
    // analysis column keep their width, the props strip keeps its height, and
    // the canvas soaks up the rest.
    sp_main_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize,
                                    wxSP_LIVE_UPDATE);
    sp_main_->SetMinimumPaneSize(FromDIP(60));
    // gravity 0.0: the sash stays put, so the RIGHT pane (everything but the
    // fixed-width palette) absorbs the frame resize.
    sp_main_->SetSashGravity(0.0);

    palette_ = new PalettePanel(sp_main_, &doc_);

    sp_right_ = new wxSplitterWindow(sp_main_, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize,
                                     wxSP_LIVE_UPDATE);
    sp_right_->SetMinimumPaneSize(FromDIP(120));
    // gravity 1.0: the sash moves with the edge, so the LEFT pane (the canvas
    // centre) absorbs the resize while the analysis column keeps its width.
    sp_right_->SetSashGravity(1.0);

    sp_bottom_ = new wxSplitterWindow(sp_right_, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize,
                                      wxSP_LIVE_UPDATE);
    sp_bottom_->SetMinimumPaneSize(FromDIP(80));
    // gravity 1.0: the TOP pane (canvas) absorbs the resize; the props strip
    // keeps its height.
    sp_bottom_->SetSashGravity(1.0);

    canvas_ = new SchematicCanvas(sp_bottom_, &doc_);
    props_ = new PropertiesPanel(sp_bottom_);
    sp_bottom_->SplitHorizontally(canvas_, props_);

    analysis_ = new AnalysisPanel(sp_right_);
    sp_right_->SplitVertically(sp_bottom_, analysis_);

    sp_main_->SplitVertically(palette_, sp_right_);

    // Put the outer splitter in a sizer so it always fills the frame and
    // resizes with it (otherwise a resize can leave the panes mis-laid-out).
    auto* frame_sizer = new wxBoxSizer(wxVERTICAL);
    frame_sizer->Add(sp_main_, 1, wxEXPAND);
    SetSizer(frame_sizer);

    // A minimum frame size keeps all three panes usable: below it the analysis
    // column and the value strip would be squeezed to nothing.
    SetMinSize(FromDIP(wxSize(900, 600)));
    CallAfter([this] {
        wxSize cs = GetClientSize();
        Layout();
        sp_main_->SetSashPosition(FromDIP(200));
        sp_right_->SetSashPosition(
            std::max(FromDIP(160), cs.x - FromDIP(200) - FromDIP(360)));
        sp_bottom_->SetSashPosition(std::max(FromDIP(120), cs.y - FromDIP(210)));
        Layout();
        Refresh();
    });

    // ---- plumbing ----
    palette_->on_tool_changed = [this] {
        // Clicking a component glyph starts placement of that kind.
        canvas_->begin_place(palette_->place_kind(), 0);
        SetStatusText("Click the canvas to place. Space rotates, Esc leaves.",
                      0);
    };

    canvas_->on_document_changed = [this] { document_changed(); };
    canvas_->on_selection_changed = [this](const std::string& s) {
        selection_changed(s);
    };
    canvas_->on_status = [this](const std::string& s) {
        SetStatusText(wxString::FromUTF8(s), 0);
    };
    canvas_->on_view_changed = [this](double z, double vx, double vy) {
        SetStatusText(wxString::Format("zoom=%.2f  view=(%.0f, %.0f)", z, vx,
                                       vy),
                      1);
    };
    canvas_->on_push_undo = [this] { doc_.push_undo(); };
    canvas_->on_wire_selected = [this](int wi, std::string name) {
        // Selecting a wire lets the properties panel name its net (#9).
        props_->refresh(&doc_, "#wire" + std::to_string(wi));
        if (name.empty())
            SetStatusText("Wire selected -- name its net on the right (a "
                          "label is created above it).",
                          0);
        else
            SetStatusText("Net: " + name, 0);
    };

    props_->on_edited = [this] {
        canvas_->invalidate_nets(); // the props-panel delete button can
                                    // remove wires without going through the
                                    // canvas's notify_doc()
        canvas_->Refresh(false); // deferred: coalesce a fast typist's keystrokes
        SetStatusText("Settings changed -- press F5 to (re)analyze.", 0);
    };
    props_->on_selection_changed = [this](const std::string& s) {
        canvas_->set_selection(s);
        selection_changed(s);
    };
    props_->on_wire_name = [this](int wi, const std::string& name) {
        canvas_->set_wire_net_name(wi, name);
    };
    props_->on_label_font = [this](int li, int size) {
        canvas_->set_label_font_size(li, size);
    };

    // ---- analysis cards ----
    analysis_->refresh(&doc_);
    analysis_->on_changed = [this] {
        doc_.analysis_cards = analysis_->serialize();
        doc_.dirty = true;
        update_title();
    };
    analysis_->on_run_all = [this](int) { run_card(-1); };
    analysis_->on_run_one = [this](int i) { run_card(i); };
    analysis_->on_results = [this] {
        if (auto* f = ensure_results_frame()) {
            f->popup();
            f->select_page(0);
        }
    };

    props_->refresh(&doc_, canvas_->selection());
    // prime the status-bar view readout (zoom/view origin) before the first
    // paint, so it appears even if the user never pans or zooms.
    canvas_->report_view();
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
    if (canvas_->handle_key(e)) return;
    if (handle_shortcut(e)) return;

    // Let menu accelerators (Ctrl+N / F5 / Del ...) run.
    e.Skip();
}

// ---------------------------------------------------------------------------
// Keyboard placement map + quick transform keys.
// ---------------------------------------------------------------------------
bool MainFrame::handle_shortcut(wxKeyEvent& e) {
    const int code = e.GetKeyCode();
    const bool shift = e.ShiftDown();
    const bool ctrl = e.ControlDown();
    const bool alt = e.AltDown();

    // undo / redo: Ctrl+Z/Y plus Virtuoso-style U / Shift+U
    if (ctrl && !alt && code == 'Z') { on_undo_cmd(); return true; }
    if (ctrl && !alt && code == 'Y') { on_redo_cmd(); return true; }
    // copy / paste
    if (ctrl && !alt && code == 'C') { canvas_->copy_selection(); return true; }
    if (ctrl && !alt && code == 'V') { canvas_->paste_clipboard(); return true; }
    if (!ctrl && !alt && (code == 'U' || code == 'u')) {
        if (shift) on_redo_cmd();
        else on_undo_cmd();
        return true;
    }

    // Space: while wiring, swap the route orientation; while placing or with a
    // selection, rotate / flip.
    if (code == WXK_SPACE) {
        if (canvas_->wiring()) {
            canvas_->toggle_wire_orient();
            SetStatusText("Wire route: press Space again to swap.", 0);
        } else if (ctrl) {
            canvas_->flip_ghost(false);
        } else if (shift) {
            canvas_->flip_ghost(true);
        } else {
            canvas_->rotate_ghost(90);
        }
        sync_palette();
        return true;
    }
    if (ctrl || alt) return false; // leave Ctrl/Alt combos to menus

    auto place = [&](syms::Kind k) {
        canvas_->begin_place(k, 0);
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
        canvas_->set_tool(Tool::Wire);
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
        canvas_->zoom_to_fit();
        SetStatusText("Zoomed to fit the components.", 0);
        return true;
    case 'M': {
        // M selects the NMOS first; pressing M again (while placing) toggles
        // to the PMOS.
        syms::Kind cur = canvas_->tool() == Tool::Place
                             ? canvas_->place_kind()
                             : syms::Kind::PMOS; // first press -> NMOS
        syms::Kind nxt = (cur == syms::Kind::NMOS) ? syms::Kind::PMOS
                                                   : syms::Kind::NMOS;
        return place(nxt);
    }
    case 'Q': case 'q': {
        // Q selects the NPN BJT first; pressing Q again (while placing)
        // toggles to the PNP, mirroring the M / NMOS-PMOS behaviour.
        syms::Kind cur = canvas_->tool() == Tool::Place
                             ? canvas_->place_kind()
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
    wxTextEntryDialog dlg(this,
                          "Net name(s) to place, separated by spaces:",
                          "Place net label(s)");
    if (dlg.ShowModal() != wxID_OK) return;
    std::string names = dlg.GetValue().ToStdString();
    canvas_->begin_label(names);
    canvas_->SetFocus();
    sync_palette();
    SetStatusText("Net label: click a net to place the next name; Esc stops.", 0);
}

void MainFrame::sync_palette() {
    palette_->set_active(canvas_->tool(), canvas_->place_kind());
    // Keep the toolbar radio buttons in step with the canvas tool (e.g. after
    // the W / S keys), the way the reference's draw-tools reflect the mode.
    if (toolbar_) {
        Tool t = canvas_->tool();
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
        canvas_->begin_place(ents[i].kind, 0);
        sync_palette();
        SetStatusText("Placing -- click to drop another, Space rotates, Esc stops.",
                      0);
    });
    PopupMenu(&menu);
}

// ---------------------------------------------------------------------------
void MainFrame::update_title() {
    wxString name = doc_.path.empty()
                        ? wxString("untitled")
                        : wxString::FromUTF8(doc_.path)
                              .AfterLast('\\')
                              .AfterLast('/');
    SetTitle(wxString::Format("SymCirc -- %s%s", name,
                              doc_.dirty ? wxString(" *") : wxString("")));
}

// Document changed via the canvas (component moved, wire/label placed, etc.).
// The selection didn't necessarily change, so the props panel is *not*
// refreshed here -- that would tear down and rebuild all its widgets, causing
// visible re-layout of the canvas (the props panel's height affects the
// canvas's height). The props panel refreshes on selection changes, which is
// what it actually depends on; this handler only updates the title and status.
void MainFrame::document_changed() {
    update_title();
    SetStatusText("Circuit changed -- press F5 to (re)analyze.", 0);
}

void MainFrame::selection_changed(const std::string& sel) {
    props_->refresh(&doc_, sel);
    canvas_->Refresh(false); // deferred repaint
}

// ---------------------------------------------------------------------------
bool MainFrame::maybe_save() {
    if (!doc_.dirty) return true;
    wxMessageDialog dlg(this,
                        "The schematic has unsaved changes. Save them?",
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

void MainFrame::on_new(wxCommandEvent&) {
    if (!maybe_save()) return;
    doc_ = Document();
    doc_.req.input_ref = "V1";
    doc_.req.output = "V(out)";
    doc_.dirty = false;
    result_.reset();
    if (results_frame_) {
        results_frame_->bode()->set_result(nullptr);
        if (results_frame_->lua()) results_frame_->lua()->set_result(nullptr);
        results_frame_->results()->clear();
    }
    canvas_->set_selection("");
    canvas_->invalidate_nets();
    // Defer the fit until after the layout has settled (this event finishes
    // first), so the canvas has its real client size.
    CallAfter([this] { canvas_->zoom_to_fit(); });
    canvas_->Refresh();
    props_->refresh(&doc_, "");
    update_title();
}

void MainFrame::on_open(wxCommandEvent&) {
    if (!maybe_save()) return;
    wxFileDialog dlg(this, "Open schematic", "", "",
                     "SymCirc circuits (*.scx)|*.scx|All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    open_path(dlg.GetPath());
}

void MainFrame::open_path(const wxString& p) {
    std::string err;
    Document nd;
    if (!nd.load(p.ToStdString(), err)) {
        wxMessageBox(wxString::FromUTF8(err), "Open failed", wxICON_ERROR, this);
        return;
    }
    doc_ = std::move(nd);
    result_.reset();
    if (results_frame_) {
        results_frame_->bode()->set_result(nullptr);
        if (results_frame_->lua()) results_frame_->lua()->set_result(nullptr);
        results_frame_->results()->clear();
    }
    canvas_->set_selection("");
    canvas_->invalidate_nets();
    // Defer the fit until after the sash positions from build_layout have been
    // applied (CallAfter order is FIFO, so this runs after the parking block
    // that was queued first) and after any pending frame resize.
    CallAfter([this] { canvas_->zoom_to_fit(); });
    canvas_->Refresh();
    props_->refresh(&doc_, "");
    if (analysis_) {
        analysis_->deserialize(doc_.analysis_cards);
        analysis_->refresh(&doc_);
    }
    update_title();
    SetStatusText("Loaded " + p, 0);
}

void MainFrame::on_save(wxCommandEvent&) {
    if (doc_.path.empty()) {
        do_save_as();
        return;
    }
    std::string err;
    if (!doc_.save(doc_.path, err)) {
        wxMessageBox(wxString::FromUTF8(err), "Save failed", wxICON_ERROR, this);
        return;
    }
    update_title();
    SetStatusText("Saved " + wxString::FromUTF8(doc_.path), 0);
}

bool MainFrame::do_save_as() {
    wxFileDialog dlg(this, "Save schematic", "", "",
                     "SymCirc circuits (*.scx)|*.scx|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return false;
    std::string err;
    if (!doc_.save(dlg.GetPath().ToStdString(), err)) {
        wxMessageBox(wxString::FromUTF8(err), "Save failed", wxICON_ERROR, this);
        return false;
    }
    update_title();
    SetStatusText("Saved " + dlg.GetPath(), 0);
    return true;
}

void MainFrame::on_save_as(wxCommandEvent&) { do_save_as(); }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
AnalysisResultsFrame* MainFrame::ensure_results_frame() {
    if (!results_frame_) results_frame_ = new AnalysisResultsFrame(this);
    return results_frame_;
}

void MainFrame::on_undo(wxCommandEvent&) { on_undo_cmd(); }
void MainFrame::on_redo(wxCommandEvent&) { on_redo_cmd(); }

void MainFrame::on_undo_cmd() {
    if (!doc_.can_undo()) {
        SetStatusText("Nothing to undo.", 0);
        return;
    }
    doc_.undo();
    after_undo_redo();
    SetStatusText("Undo.", 0);
}

void MainFrame::on_redo_cmd() {
    if (!doc_.can_redo()) {
        SetStatusText("Nothing to redo.", 0);
        return;
    }
    doc_.redo();
    after_undo_redo();
    SetStatusText("Redo.", 0);
}

void MainFrame::after_undo_redo() {
    // the selection may point at something that no longer exists
    canvas_->set_selection("");
    canvas_->invalidate_nets(); // Document::undo/redo bypass the canvas
    canvas_->Refresh();
    props_->refresh(&doc_, "");
    update_title();
}

void MainFrame::on_rotate(wxCommandEvent&) { canvas_->rotate_selection(); }
void MainFrame::on_delete(wxCommandEvent&) { canvas_->delete_selection(); }

// ---------------------------------------------------------------------------
void MainFrame::on_run(wxCommandEvent&) { run_analysis(); }

void MainFrame::run_analysis() { run_card(-1); }

// Run one analysis card (index >= 0) or every enabled card (index < 0).
void MainFrame::run_card(int index) {
    std::string err;
    syms::Circuit c = doc_.resolved(err);
    if (!err.empty()) {
        wxMessageBox(wxString::FromUTF8(err), "Cannot analyze",
                     wxICON_ERROR, this);
        return;
    }

    auto& cards = analysis_->cards();
    if (cards.empty()) {
        // no cards configured: fall back to one transfer-function run
        AnalysisCard def;
        def.input_ref = doc_.req.input_ref;
        def.output = doc_.req.output;
        def.sweep = doc_.req.sweep;
        def.prune = doc_.req.prune;
        def.use_parallel = doc_.req.use_parallel;
        def.approx_factor = doc_.req.approx_factor;
        cards.push_back(def);
        analysis_->refresh(&doc_);
    }

    std::string report;
    std::string latex;
    std::string latex_report;
    std::unique_ptr<syms::AnalysisResult> keep;

    auto run_one = [&](int i) {
        const AnalysisCard& card = cards[i];
        syms::AnalysisSpec sp;
        sp.kind = card.kind;
        sp.input_ref = card.input_ref.empty() ? doc_.req.input_ref
                                              : card.input_ref;
        sp.output = card.output.empty() ? doc_.req.output : card.output;
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
        sp.tech = doc_.tech;

        syms::CardResult cr = syms::run_analysis(c, sp);
        // A numeric DC card can export small-signal overrides extracted from
        // its operating point (Mode 3 + the override checkbox). Apply them to
        // the working circuit so the FOLLOWING cards reflect the real bias
        // (e.g. a device's Cds shrinks and the threshold rule prunes it).
        for (const auto& kv : cr.ss_overrides) {
            for (auto& comp : c.comps) {
                if (comp.ref != kv.first) continue;
                for (const auto& pv : kv.second) {
                    comp.param_text[pv.first] =
                        syms::eng::format_eng(pv.second, 4);
                    comp.param_on[pv.first] = true;
                }
            }
        }
        report += cr.title + "\n";
        report += std::string(cr.title.size() + 8, '-') + "\n";
        report += cr.report;
        report += "\n";
        if (!cr.latex.empty()) latex = cr.latex;
        if (!cr.latex_report.empty()) latex_report = cr.latex_report;
        if (cr.has_transfer) {
            keep = std::make_unique<syms::AnalysisResult>(cr.transfer);
        }
        return cr;
    };

    try {
        if (index >= 0) {
            if (index >= int(cards.size())) return;
            run_one(index);
        } else {
            int n = 0;
            for (int i = 0; i < int(cards.size()); ++i) {
                if (!cards[i].enabled) continue;
                run_one(i);
                ++n;
            }
            if (n == 0 && !cards.empty()) {
                wxMessageBox("No analysis step is enabled.", "Nothing to run",
                             wxICON_INFORMATION, this);
                return;
            }
        }
    } catch (const std::exception& e) {
        wxMessageBox(wxString::FromUTF8(e.what()), "Analysis failed",
                     wxICON_ERROR, this);
        return;
    }

    auto* rf = ensure_results_frame();
    rf->results()->set_text(report);
    rf->results()->set_latex(latex);
    // Results tab = the plain-text report. Math tab = the same report
    // typeset (the transfer function as real stacked fractions with proper
    // subscripts, the poles/zeros as a formatted list).
    rf->set_report(latex, latex_report);
    result_ = std::move(keep);
    // The bode tab shows the most recent card's title so the user can tell
    // what each plot represents when they switch back to it. Run-all uses
    // the last enabled card so the title matches whatever transfer function
    // is sitting in `keep`.
    std::string last_title;
    if (index >= 0 && index < int(cards.size())) {
        last_title = cards[index].title;
    } else {
        for (int i = int(cards.size()) - 1; i >= 0; --i) {
            if (cards[i].enabled && !cards[i].title.empty()) {
                last_title = cards[i].title;
                break;
            }
        }
    }
    rf->bode()->set_title(last_title);
    rf->bode()->set_result(result_.get());
    if (rf->lua()) rf->lua()->set_result(result_.get());
    rf->popup();
    rf->select_page(0); // land on the "Results" (typeset) tab
    SetStatusText("Analysis OK -- see the results window.", 0);
}

// ---------------------------------------------------------------------------
void MainFrame::on_about(wxCommandEvent&) {
    wxMessageBox(
        "SymCirc -- symbolic circuit analysis with low-entropy forms.\n\n"
        "Draw a schematic, pick an input and output, press F5.\n"
        "Ranks terms by order-of-magnitude estimates and prunes the\n"
        "negligible ones so the transfer function stays readable.",
        "About SymCirc", wxICON_INFORMATION, this);
}

} // namespace symcirc
