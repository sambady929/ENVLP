#pragma once
#include "core/Engine.h"

#include <wx/wx.h>
#include <wx/spinctrl.h>

#include <string>
#include <vector>

namespace symcirc {

// Which curve family the plot canvas draws.
enum class PlotMode { Bode, Nyquist, Nichols };

// The plotting canvas (Bode magnitude/phase, Nyquist, or Nichols) with
// SVG/PNG/CSV export and user-controllable axis ranges.
class BodeCanvas : public wxPanel {
public:
    explicit BodeCanvas(wxWindow* parent);

    void set_result(const syms::AnalysisResult* r); // nullptr clears
    void set_mode(PlotMode m);
    PlotMode mode() const { return mode_; }

    // Auto vs manual axis range. Manual lets the toolbar spin controls
    // override; Auto scales to the data each refresh.
    void set_auto_range(bool a) { auto_range_ = a; Refresh(); }
    void set_x_range(double lo, double hi);
    void set_y_range(double lo, double hi);
    bool auto_range() const { return auto_range_; }
    double x_lo() const { return x_lo_; }
    double x_hi() const { return x_hi_; }
    double y_lo() const { return y_lo_; }
    double y_hi() const { return y_hi_; }

    // Title shown above the plot (card name, e.g. "Transfer function (H(s))").
    void set_title(const std::string& t) { title_ = t; Refresh(); }
    const std::string& title() const { return title_; }

    bool save_csv(const std::string& path) const;
    bool save_svg(const std::string& path) const;
    bool save_png(const std::string& path) const;

    // sampled data (frequency Hz, magnitude dB, phase deg) -- phase unwrapped
    void sample(std::vector<double>& f, std::vector<double>& mag,
                std::vector<double>& ph) const;

    // Frequency band to plot (from the result's sweep, or a default).
    void band(double& f0, double& f1, int& n) const;

private:
    static std::string escape_xml(std::string s);
    const syms::AnalysisResult* res_ = nullptr;
    PlotMode mode_ = PlotMode::Bode;
    std::string title_;

    bool auto_range_ = true;
    double x_lo_ = 1.0, x_hi_ = 1e8;     // Hz bounds for Bode/Nichols x
    double y_lo_ = -40.0, y_hi_ = 40.0;   // dB bounds for Bode/Nichols y

    void paint_bode(wxDC& dc, const wxSize& sz) const;
    void paint_nyquist(wxDC& dc, const wxSize& sz) const;
    void paint_nichols(wxDC& dc, const wxSize& sz) const;
    void on_paint(wxPaintEvent& e);
    wxDECLARE_EVENT_TABLE();
};

// Bottom-panel tab: the plot canvas plus a toolbar with mode selector, axis
// spin controls, and save buttons.
class BodePanel : public wxPanel {
public:
    explicit BodePanel(wxWindow* parent);

    void set_result(const syms::AnalysisResult* r);
    void set_title(const std::string& t);

private:
    BodeCanvas* plot_ = nullptr;
    wxSpinCtrl* xmin_ = nullptr;
    wxSpinCtrl* xmax_ = nullptr;
    wxSpinCtrl* ymin_ = nullptr;
    wxSpinCtrl* ymax_ = nullptr;
    wxCheckBox* auto_box_ = nullptr;

    void sync_axis_controls();
};

} // namespace symcirc