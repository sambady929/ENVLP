#pragma once
#include "Document.h"
#include "core/Analysis.h"

#include <wx/wx.h>
#include <wx/scrolwin.h>

#include <functional>
#include <string>
#include <vector>

namespace symcirc {

// One analysis "card": a kind, an input/output, and per-card options. The
// analysis panel stacks these so several analyses can be configured and run
// from one place.
struct AnalysisCard {
    syms::AnalysisKind kind = syms::AnalysisKind::TransferFunction;
    std::string title;
    std::string input_ref;
    std::string output;
    std::string probe_ref;
    // Standard SPICE-style frequency sweep.
    syms::SweepSpec sweep;
    // "Ignore negligible terms": when off the exact (unpruned) form is shown.
    bool prune = true;
    // dB below the dominant term at which a term is considered negligible.
    double threshold_db = 20.0;
    bool global_ref = false;
    bool use_parallel = true;
    bool gm_ro = true;
    bool approx_factor = true;
    bool enabled = true;
};

// Right-side "Analysis" panel: a list of cards with add/remove and run.
class AnalysisPanel : public wxScrolledWindow {
public:
    explicit AnalysisPanel(wxWindow* parent);

    std::vector<AnalysisCard>& cards() { return cards_; }
    const std::vector<AnalysisCard>& cards() const { return cards_; }

    void refresh(Document* doc); // rebuild the UI from doc + cards
    Document* doc() const { return doc_; }

    std::function<void()> on_changed;   // a card changed
    std::function<void(int)> on_run_all; // run every enabled card
    std::function<void(int)> on_run_one; // run one card index
    std::function<void()> on_results;    // jump to the results tab

    // Load/save the card list alongside the document.
    std::string serialize() const;
    bool deserialize(const std::string& data);

private:
    Document* doc_ = nullptr;
    std::vector<AnalysisCard> cards_;
    bool rebuilding_ = false;

    void add_card(syms::AnalysisKind kind);
    void remove_card(int i);
    void on_add_choice();
    wxDECLARE_EVENT_TABLE();
};

// Human-readable name / default for a kind.
const char* analysis_kind_name(syms::AnalysisKind k);
syms::AnalysisKind analysis_kind_from_name(const std::string& n);

} // namespace symcirc
