#include "MainFrame.h"

#include "BodePanel.h"
#include "LuaConsole.h"
#include "PalettePanel.h"
#include "PropertiesPanel.h"
#include "ResultsPanel.h"
#include "SchematicCanvas.h"

#include <wx/filedlg.h>
#include <wx/msgdlg.h>

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
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "SymCirc", wxDefaultPosition,
              wxSize(1280, 860)) {
    // sensible default analysis request (before panels read it)
    doc_.req.input_ref = "V1";
    doc_.req.output = "V(out)";

    build_menu();
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
    edit->Append(ID_ROTATE, "&Rotate\tCtrl+R", "Rotate the selection90 deg");
    edit->Append(ID_DELETE, "&Delete\tDel", "Delete the selection");

    auto* run = new wxMenu;
    run->Append(ID_RUN, "&Analyze\tF5", "Run the symbolic analysis");

    auto* help = new wxMenu;
    help->Append(ID_ABOUT_APP, "&About SymCirc");

    auto* bar = new wxMenuBar;
    bar->Append(file, "&File");
    bar->Append(edit, "&Edit");
    bar->Append(run, "&Run");
    bar->Append(help, "&Help");
    SetMenuBar(bar);
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

    props_ = new PropertiesPanel(this);
    root->Add(props_, 0, wxEXPAND | wxALL, 4);

    SetSizer(root);

    // ---- plumbing ----
    palette_->on_tool_changed = [this] {
        canvas_->set_tool(palette_->tool(), palette_->place_kind());
        if (palette_->tool() == Tool::Place)
            SetStatusText("Click the canvas to place. R rotates, Esc leaves.",
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

    props_->on_edited = [this] {
        canvas_->Refresh();
        SetStatusText("Settings changed -- press F5 to (re)analyze.", 0);
    };
    props_->on_selection_changed = [this](const std::string& s) {
        canvas_->set_selection(s);
        selection_changed(s);
    };

    canvas_->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (handle_shortcut(e)) return;
        if (!canvas_->handle_key(e)) e.Skip();
    });

    props_->refresh(&doc_, canvas_->selection());
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

    // Space: rotate / flip the ghost (or the selection)
    if (code == WXK_SPACE) {
        if (ctrl) canvas_->flip_ghost(false);
        else if (shift) canvas_->flip_ghost(true);
        else canvas_->rotate_ghost(90);
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
    case 'H': case 'h': return place(syms::Kind::CCVS);
    case 'F': case 'f': return place(syms::Kind::CCCS);
    case 'O': case 'o': return place(syms::Kind::OPAMP);
    case 'W': case 'w':
        canvas_->set_tool(Tool::Wire);
        sync_palette();
        SetStatusText("Wire: click a start pin, route, click the end pin.", 0);
        return true;
    case 'M': {
        // M again while placing a MOSFET toggles NMOS <-> PMOS
        syms::Kind cur = canvas_->tool() == Tool::Place
                             ? canvas_->place_kind()
                             : syms::Kind::NMOS;
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
    menu.Bind(wxEVT_MENU, [this, &entries](wxCommandEvent& ev) {
        int i = ev.GetId() - ID_PLACE_BASE;
        canvas_->begin_place(entries[i].kind, 0);
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
    canvas_->Refresh();
    props_->refresh(&doc_, "");
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

void MainFrame::run_analysis() {
    std::string err;
    syms::Circuit c = doc_.resolved(err);
    if (!err.empty()) {
        wxMessageBox(wxString::FromUTF8(err), "Cannot analyze",
                     wxICON_ERROR, this);
        return;
    }
    try {
        auto res = std::make_unique<syms::AnalysisResult>(
            syms::analyze(c, doc_.req));
        result_ = std::move(res);
        results_->set_text(result_->report);
        bode_->set_result(result_.get());
        lua_->set_result(result_.get());
        bottom_->SetSelection(0);
        SetStatusText("Analysis OK -- see Results / Bode / Lua tabs.", 0);
    } catch (const std::exception& e) {
        wxMessageBox(wxString::FromUTF8(e.what()), "Analysis failed",
                     wxICON_ERROR, this);
    }
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
