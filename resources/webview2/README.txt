WebView2Loader.dll
==================

  x64/WebView2Loader.dll   the WebView2 loader for 64-bit builds

Why it is here
---------------
  wxWidgets' Edge backend loads this DLL at run time (by name, not as a
  static import), so it is not visible in the executable's import table and
  the portable-packaging DLL scan cannot find it. Without it
  wxWebView::IsBackendAvailable(wxWebViewBackendEdge) is false, wxWidgets
  falls back to the legacy IE (mshtml) engine, and the Math tab cannot
  typeset: MathJax is modern JavaScript and does not run there. The symptom is
  the raw \(..\) markup showing up as plain text.

  The WebView2 *runtime* itself ships with Windows 10/11 (via Edge); this is
  only the tiny loader shim that a native app must carry.

Source
------
  NuGet package Microsoft.Web.WebView2
    https://www.nuget.org/packages/Microsoft.Web.WebView2
    build/native/x64/WebView2Loader.dll

License
-------
  (c) Microsoft Corporation. Distributed under the Microsoft WebView2 SDK
  license terms: https://aka.ms/webview2-sdk-license

Updating
--------
  Replace x64/WebView2Loader.dll with the one from a newer SDK package.
  Nothing else needs to change (the build copies it next to ENVLP.exe and
  tools/make_portable.ps1 ships it).
