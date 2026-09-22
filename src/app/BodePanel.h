#pragma once
#include "core/Engine.h"

#include <wx/wx.h>

#include <string>
#include <vector>

namespace symcirc {

// The plotting canvas (magnitude + phase Bode) with SVG/CSV export.
class BodeCanvas : public wxPanel {
public:
    explicit BodeCanvas(wxWindow* parent);

    void set_result(const syms::AnalysisResult* r); // nullptr clears
    bool save_csv(const std::string& path) const;
    bool save_svg(const std::string& path) const;

    // sampled data (frequency Hz, magnitude dB, phase deg)
    void sample(std::vector<double>& f, std::vector<double>& mag,
                std::vector<double>& ph) const;

private:
    static std::string escape_xml(std::string s);
    const syms::AnalysisResult* res_ = nullptr;

    void on_paint(wxPaintEvent& e);
    wxDECLARE_EVENT_TABLE();
};

// Bottom-panel tab: the Bode canvas plus save buttons.
class BodePanel : public wxPanel {
public:
    explicit BodePanel(wxWindow* parent);

    void set_result(const syms::AnalysisResult* r);

private:
    BodeCanvas* plot_ = nullptr;
    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
