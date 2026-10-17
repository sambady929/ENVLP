#pragma once
#include "core/Engine.h"

#include <wx/wx.h>
#include <wx/webview.h>

#include <string>
#include <vector>

namespace envlp {

// Which curve family the plot canvas draws.
enum class PlotMode { Bode, Nyquist, Nichols, Noise };

// The plotting canvas (Bode magnitude/phase, Nyquist, or Nichols) with
// SVG/PNG/CSV export and user-controllable axis ranges.
class BodeCanvas : public wxPanel {
public:
    explicit BodeCanvas(wxWindow* parent);
    // Stop any in-flight page load before the control goes away: destroying a
    // WebView2 mid-navigation is a reliable way to take the process down.
    ~BodeCanvas() override;

    void set_result(const syms::AnalysisResult* r); // nullptr clears
    void set_mode(PlotMode m);
    PlotMode mode() const { return mode_; }

    // Auto vs manual axis range. Manual lets the toolbar spin controls
    // override; Auto scales to the data each refresh.
    void set_auto_range(bool a) { auto_range_ = a; refresh_any(); }
    void set_x_range(double lo, double hi);
    void set_y_range(double lo, double hi);
    bool auto_range() const { return auto_range_; }
    double x_lo() const { return x_lo_; }
    double x_hi() const { return x_hi_; }
    double y_lo() const { return y_lo_; }
    double y_hi() const { return y_hi_; }

    // Toggle the major (decade / dB) and minor (sub-decade) grid lines.
    void set_grid(bool major, bool minor) {
        show_major_grid_ = major;
        show_minor_grid_ = minor;
        refresh_any();
    }
    bool show_major_grid() const { return show_major_grid_; }
    bool show_minor_grid() const { return show_minor_grid_; }

    // Title shown above the plot (card name, e.g. "Transfer function (H(s))").
    void set_title(const std::string& t) { title_ = t; refresh_any(); }
    const std::string& title() const { return title_; }

    bool save_csv(const std::string& path) const;
    bool save_svg(const std::string& path) const;
    bool save_png(const std::string& path) const;
    // Plotly renders in a wxWebView; these export the same figure.
    bool save_html(const std::string& path) const;
    bool save_python(const std::string& path) const;
    // True when the interactive Plotly view is live (otherwise the canvas
    // falls back to the built-in wxDC painter).
    bool interactive() const { return view_ != nullptr; }

    // sampled data (frequency Hz, magnitude dB, phase deg) -- phase unwrapped
    void sample(std::vector<double>& f, std::vector<double>& mag,
                std::vector<double>& ph) const;

    // Frequency band to plot (from the result's sweep, or a default).
    void band(double& f0, double& f1, int& n) const;

private:
    static std::string escape_xml(std::string s);

    // Build the Plotly traces/layout (and the equivalent Python script) for the
    // current mode. Shared by the live view, the HTML export and the Python
    // export so all three always agree.
    struct PlotSpec {
        std::string traces_js;  // "var traces = [ ... ];"
        std::string layout_js;  // "var layout = { ... };"
        std::string py;         // a complete plotly.py script
        std::string title;
    };
    PlotSpec plot_spec() const;
    void render_plot();      // push the spec into the web view
    void relayout_plot();    // in-place axis/grid change (no page reload)
    void refresh_now();      // the coalesced body of refresh_any()
    std::string axis_relayout_js() const;
    void refresh_any();      // web view when live, else a repaint
    bool pending_ = false;   // a render is already queued
    // What the live page currently shows, so a pure axis/grid tweak can be
    // applied with Plotly.relayout instead of reloading the page.
    const void* last_res_ = nullptr;
    PlotMode last_mode_ = PlotMode::Bode;
    std::string last_title_;

    const syms::AnalysisResult* res_ = nullptr;
    PlotMode mode_ = PlotMode::Bode;
    std::string title_;
    wxWebView* view_ = nullptr;

    bool auto_range_ = true;
    double x_lo_ = 1.0, x_hi_ = 1e8;     // Hz bounds for Bode/Nichols x
    double y_lo_ = -40.0, y_hi_ = 40.0;   // dB bounds for Bode/Nichols y
    bool show_major_grid_ = true;
    bool show_minor_grid_ = true;

    void paint_bode(wxDC& dc, const wxSize& sz) const;
    void paint_nyquist(wxDC& dc, const wxSize& sz) const;
    void paint_nichols(wxDC& dc, const wxSize& sz) const;
    void paint_noise(wxDC& dc, const wxSize& sz) const;
    void on_paint(wxPaintEvent& e);
    void on_size(wxSizeEvent& e);
    wxDECLARE_EVENT_TABLE();
};

// Bottom-panel tab: the plot canvas plus a toolbar with mode selector, axis
// range text fields, and save buttons.
class BodePanel : public wxPanel {
public:
    explicit BodePanel(wxWindow* parent);

    void set_result(const syms::AnalysisResult* r);
    void set_title(const std::string& t);

private:
    BodeCanvas* plot_ = nullptr;
    wxChoice* mode_ = nullptr;
    wxTextCtrl* xmin_ = nullptr;
    wxTextCtrl* xmax_ = nullptr;
    wxTextCtrl* ymin_ = nullptr;
    wxTextCtrl* ymax_ = nullptr;
    wxCheckBox* auto_box_ = nullptr;
    wxCheckBox* major_grid_ = nullptr;
    wxCheckBox* minor_grid_ = nullptr;

    void apply_axis();
    void sync_axis_controls();
};

} // namespace envlp