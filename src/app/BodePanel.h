#pragma once
#include "core/Engine.h"

#include <wx/wx.h>

#include <string>
#include <vector>

namespace symcirc {

// Which curve family the plot canvas draws.
enum class PlotMode { Bode, Nyquist, Nichols };

// The plotting canvas (Bode magnitude/phase, Nyquist, or Nichols) with
// SVG/PNG/CSV export.
class BodeCanvas : public wxPanel {
public:
    explicit BodeCanvas(wxWindow* parent);

    void set_result(const syms::AnalysisResult* r); // nullptr clears
    void set_mode(PlotMode m);
    PlotMode mode() const { return mode_; }

    bool save_csv(const std::string& path) const;
    bool save_svg(const std::string& path) const;
    bool save_png(const std::string& path) const;

    // sampled data (frequency Hz, magnitude dB, phase deg)
    void sample(std::vector<double>& f, std::vector<double>& mag,
                std::vector<double>& ph) const;

private:
    static std::string escape_xml(std::string s);
    const syms::AnalysisResult* res_ = nullptr;
    PlotMode mode_ = PlotMode::Bode;

    void paint_bode(wxDC& dc, const wxSize& sz) const;
    void paint_nyquist(wxDC& dc, const wxSize& sz) const;
    void paint_nichols(wxDC& dc, const wxSize& sz) const;
    void on_paint(wxPaintEvent& e);
    wxDECLARE_EVENT_TABLE();
};

// Bottom-panel tab: the plot canvas plus the mode selector and save buttons.
class BodePanel : public wxPanel {
public:
    explicit BodePanel(wxWindow* parent);

    void set_result(const syms::AnalysisResult* r);

private:
    BodeCanvas* plot_ = nullptr;
    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
