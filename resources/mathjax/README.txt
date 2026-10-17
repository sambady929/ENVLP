MathJax (typesetting engine for the "Results" tab)
==================================================

  tex-svg.js   the MathJax v3 bundle that renders TeX to self-contained SVG
               (no web fonts, no network access, no CDN)

Source
------
  https://www.mathjax.org/           (v3, es5/tex-svg.js bundle)

License
-------
  (c) The MathJax Consortium
  Apache License, Version 2.0  --  https://www.apache.org/licenses/LICENSE-2.0

Notes
-----
  * The engine's LaTeX (GiNaC's print_latex plus the \parallel extension) is
    normalized in src/app/LatexRender.cpp (latex_normalize) and then typeset by
    this bundle inside the wxWebView of the Math tab.
  * The panel reads this file at start-up and inlines it into the page, because
    the WebView loads the page with NavigateToString (base URI about:blank), so
    a relative <script src="..."> would never resolve.
  * If this file is missing the panel falls back to the built-in subset
    renderer, so the program still runs -- just with simpler typography.
  * To update: replace tex-svg.js with a newer v3 es5 bundle from the source
    above; nothing else needs to change.
