Plotly.js (interactive plots)
=============================

  plotly-basic.min.js   the Plotly "basic" distribution: scatter / lines /
                        markers + cartesian axes, which is all the Bode,
                        Nyquist, Nichols and noise charts need

Source
------
  https://plotly.com/javascript/
  https://cdn.jsdelivr.net/npm/plotly.js-basic-dist@2.35.2/plotly-basic.min.js

License
-------
  (c) Plotly, Inc.  MIT License -- https://github.com/plotly/plotly.js/blob/master/LICENSE

Notes
-----
  * The plot tab builds a small HTML page (traces + layout) and loads it in a
    wxWebView next to this bundle, so the plots are interactive (zoom, pan,
    hover, and Plotly's own camera button) with no network access.
  * "Save HTML..." writes a standalone page that pulls Plotly from the public
    CDN, so an exported page works anywhere.
  * "Save Python..." writes a plotly.py script containing the exact sample
    arrays the figure was drawn from.
  * If this file is missing (or WebView2 is unavailable) the plot tab falls
    back to the built-in wxDC painter, so the tab always shows something.
  * To update: replace plotly-basic.min.js with a newer basic-dist build.
