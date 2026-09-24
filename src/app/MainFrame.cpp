#include "MainFrame.h"

#include "AnalysisPanel.h"
#include "BodePanel.h"
#include "LuaConsole.h"
#include "PalettePanel.h"
#include "PropertiesPanel.h"
#include "ResultsPanel.h"
#include "SchematicCanvas.h"
#include "core/Analysis.h"

#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <wx/toolbar.h>

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
    EVT_CHAR_HOOK(MainFrame::on_char_hook)
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "SymCirc", wxDefaultPosition,
              wxSize(1360, 900)) {
    // sensible default analysis request (before panels read it)
    doc_.req.input_ref = "V1";
    doc_.req.output = "V(out)";

    build_menu();
    build_toolbar();
    build_layout();

    update_title();
    CreateStatusBar(2);
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
    edit->Append(ID_UNDO, "&Undo\tCtrl+Z", "Undo the last edit");
    edit->Append(ID_REDO, "&Redo\tCtrl+Y", "Redo the last undone edit");
    edit->AppendSeparator();
    edit->Append(ID_ROTATE, "&Rotate\tCtrl+R", "Rotate the selection 90 deg");
    edit->Append(ID_DELETE, "&Delete\tDel", "Delete the selection");
    edit->AppendSeparator();
    edit->Append(ID_NET_LABEL, "Place &net label(s)...\tN",
                 "Name one or more nets");

    auto* view = new wxMenu;
    view->Append(ID_ZOOM_FIT, "&Fit components\tF",
                 "Zoom to frame every component");
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
    toolbar_ = CreateToolBar(wxTB_HORIZONTAL | wxTB_TEXT | wxTB_NOICONS);
    auto add = [&](int id, const wxString& label, const wxString& help) {
        toolbar_->AddTool(id, label, wxBitmapBundle(), help);
    };
    add(ID_SELECT_TOOL, "Select", "Box-select and drag components");
    add(ID_WIRE_TOOL, "Wire (W)", "Draw a wire");
    toolbar_->AddSeparator();
    add(ID_NET_LABEL, "Net label (N)", "Name one or more nets");
    add(ID_ZOOM_FIT, "Fit (F)", "Zoom to frame every component");
    toolbar_->AddSeparator();
    toolbar_->AddCheckTool(ID_IGNORE_NEG, "Ignore negligible",
                           wxBitmapBundle(), wxBitmapBundle(),
                           "Drop terms far below the dominant one");
    toolbar_->ToggleTool(ID_IGNORE_NEG, doc_.req.prune);
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

void MainFrame::build_layout() {
    auto* root = new wxBoxSizer(wxHORIZONTAL);

    palette_ = new PalettePanel(this, &doc_);
    root->Add(palette_, 0, wxEXPAND | wxALL, 4);

    auto* mid = new wxBoxSizer(wxVERTICAL);

    canvas_ = new SchematicCanvas(this, &doc_);
    mid->Add(canvas_, 1, wxEXPAND | wxLEFT | wxRIGHT, 2);

    bottom_ = new wxNotebook(this, wxID_ANY);
    results_ = new ResultsPanel(bottom_);
    bode_ = new BodePanel(bottom_);
    lua_ = new LuaConsole(bottom_);
    bottom_->AddPage(results_, "Results", true);
    bottom_->AddPage(bode_, "Bode");
    bottom_->AddPage(lua_, "Lua");
    mid->Add(bottom_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 2);

    root->Add(mid, 1, wxEXPAND);

    // Right-hand column: properties on top, analysis cards below. Both are
    // scrolled windows that stretch to the frame height, so added cards are
    // reachable by scrolling instead of being clipped (#5).
    auto* right = new wxBoxSizer(wxVERTICAL);
    props_ = new PropertiesPanel(this);
    props_->SetMinSize(wxSize(250, -1));
    right->Add(props_, 1, wxEXPAND | wxALL, 4);

    analysis_ = new AnalysisPanel(this);
    analysis_->SetMinSize(wxSize(300, -1));
    right->Add(analysis_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    root->Add(right, 0, wxEXPAND);

    SetSizer(root);

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
        canvas_->Refresh();
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
    analysis_->on_results = [this] { bottom_->SetSelection(0); };

    props_->refresh(&doc_, canvas_->selection());
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

    // undo / redo
    if (ctrl && !alt && code == 'Z') { on_undo_cmd(); return true; }
    if (ctrl && !alt && code == 'Y') { on_redo_cmd(); return true; }

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
        SetStatusText("Placing -- Space rotates, Shift/Space flips, Esc cancels.",
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
        SetStatusText("Wire: click to start, click again to finish; Space "
                      "swaps the route; Esc cancels.",
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
        {"NPN BJT", syms::Kind::NPN},
        {"PNP BJT", syms::Kind::PNP},
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
        SetStatusText("Placing -- Space rotates, Shift/Space flips, Esc cancels.",
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

void MainFrame::document_changed() {
    props_->refresh(&doc_, canvas_->selection());
    update_title();
    SetStatusText("Circuit changed -- press F5 to (re)analyze.", 0);
}

void MainFrame::selection_changed(const std::string& sel) {
    props_->refresh(&doc_, sel);
    canvas_->Refresh();
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
    bode_->set_result(nullptr);
    lua_->set_result(nullptr);
    results_->clear();
    canvas_->set_selection("");
    canvas_->zoom_to_fit();
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
    bode_->set_result(nullptr);
    lua_->set_result(nullptr);
    results_->clear();
    canvas_->set_selection("");
    canvas_->zoom_to_fit();
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
    std::unique_ptr<syms::AnalysisResult> keep;

    auto run_one = [&](int i) {
        const AnalysisCard& card = cards[i];
        syms::AnalysisSpec sp;
        sp.kind = card.kind;
        sp.input_ref = card.input_ref.empty() ? doc_.req.input_ref
                                              : card.input_ref;
        sp.output = card.output.empty() ? doc_.req.output : card.output;
        sp.probe_ref = card.probe_ref;
        sp.sweep = card.sweep;
        sp.f0_hz = card.sweep.f_start_hz;
        sp.threshold_db = card.threshold_db;
        sp.global_ref = card.global_ref;
        sp.prune = card.prune;
        sp.use_parallel = card.use_parallel;
        sp.gm_ro_assume = card.gm_ro;
        sp.approx_factor = card.approx_factor;

        syms::CardResult cr = syms::run_analysis(c, sp);
        report += "==================================================\n";
        report += cr.title + "\n";
        report += "==================================================\n";
        report += cr.report;
        report += "\n";
        if (!cr.latex.empty()) latex = cr.latex;
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

    results_->set_text(report);
    results_->set_latex(latex);
    result_ = std::move(keep);
    bode_->set_result(result_.get());
    lua_->set_result(result_.get());
    bottom_->SetSelection(0);
    SetStatusText("Analysis OK -- see Results / Bode.", 0);
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
