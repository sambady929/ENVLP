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
    ID_ABOUT_APP,
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_MENU(ID_NEW, MainFrame::on_new)
    EVT_MENU(ID_OPEN, MainFrame::on_open)
    EVT_MENU(ID_SAVE, MainFrame::on_save)
    EVT_MENU(ID_SAVE_AS, MainFrame::on_save_as)
    EVT_MENU(ID_RUN, MainFrame::on_run)
    EVT_MENU(ID_ROTATE, MainFrame::on_rotate)
    EVT_MENU(ID_DELETE, MainFrame::on_delete)
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

    props_->on_edited = [this] {
        canvas_->Refresh();
        SetStatusText("Settings changed -- press F5 to (re)analyze.", 0);
    };

    canvas_->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (!canvas_->handle_key(e)) e.Skip();
    });

    props_->refresh(&doc_, canvas_->selection());
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
    std::string err;
    Document nd;
    if (!nd.load(dlg.GetPath().ToStdString(), err)) {
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
    SetStatusText("Loaded " + dlg.GetPath(), 0);
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
