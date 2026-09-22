#pragma once
#include "core/Engine.h"

#include <wx/wx.h>

namespace symcirc {

// Bottom-panel tab: magnitude + phase Bode plot of the (unpruned) H(s).
class BodePanel : public wxPanel {
public:
    explicit BodePanel(wxWindow* parent);

    // nullptr clears the plot. The caller owns the result; the panel only
    // reads it during paint, so keep the result alive while displayed.
    void set_result(const syms::AnalysisResult* r);

private:
    const syms::AnalysisResult* res_ = nullptr;

    void on_paint(wxPaintEvent& e);
    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
