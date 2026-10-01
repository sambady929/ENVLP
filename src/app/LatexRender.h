#pragma once
#include <string>

namespace envlp {

// Convert the LaTeX subset emitted by the envlp engine into a standalone
// HTML document (with a small inline CSS stylesheet) that wxWebView can show
// with real typeset math -- fractions stacked, proper sub/superscripts, the
// parallel symbol, etc. Fully offline: no MathJax/CDN needed.
//
// Supported: \frac, \left..\right delimiters, _ ^ scripts (grouped or single
// token), \parallel, \cdot, \times, \pm, \le, \ge, \sqrt, \mathrm, a handful
// of Greek names, spacing commands, and escaped punctuation. Anything unknown
// degrades to its literal text rather than failing.
std::string latex_to_html(const std::string& latex);

// The HTML <body> fragment only (no <html>/<style> wrapper) -- handy for tests.
std::string latex_to_html_fragment(const std::string& latex);

// Render a single line of LaTeX (no newline handling) into the same HTML
// fragment latex_to_html_fragment emits for each line. Useful when embedding
// the LaTeX within a larger HTML document the caller is composing itself.
std::string latex_render_line(const std::string& latex);

// Normalize the engine's LaTeX before rendering: GiNaC's print_latex emits
// identifiers like ro_M1 as one literal token, so the resulting "subscript"
// is just one character and the rest hangs bare. Wrap every script argument
// in braces when it has more than one character, so typesetters parse
// "ro_{M1}" instead of "ro_M1".
std::string latex_normalize(const std::string& latex);

// Classify a report line as a plain-text section heading (styled as a small
// caps title) rather than a typeset math line. The engine emits both; a math
// line can also end in ':' -- e.g. `\mathrm{Density}\ S_{v}(f):` -- and
// treating that as a heading leaks raw LaTeX to the user. A heading must be
// bare text: no backslash, script or brace. `heading_from_mathrm` peels the
// plain text out of a `\mathrm{Contribution:}` line.
bool latex_is_plain_heading(const std::string& line);
bool latex_heading_from_mathrm(const std::string& line, std::string& out);

} // namespace envlp
