cmake_minimum_required(VERSION 3.21)
# RC is required: the GUI embeds a manifest (Common-Controls 6.0) without
# which the wxWidgets DLLs fail to load on Windows.
project(ENVLP VERSION 0.2.0 LANGUAGES CXX)

if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MinGW hides M_PI etc. under -std=c++17; _USE_MATH_DEFINES brings them back.
add_compile_definitions(_USE_MATH_DEFINES)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Dependencies (all expected from MSYS2 UCRT64: pkgconf, ginac, lua, wxwidgets)
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(GINAC REQUIRED IMPORTED_TARGET ginac)

# cln (GiNaC's backend) keeps -lgmp in Libs.private -- only surfaced by
# pkg-config --static -- so add GMP to the link explicitly.
pkg_check_modules(GMP QUIET IMPORTED_TARGET gmp)
if(TARGET PkgConfig::GMP)
  set(ENVLP_GMP_TARGET PkgConfig::GMP)
else()
  set(ENVLP_GMP_TARGET gmp)
endif()

pkg_search_module(LUA QUIET IMPORTED_TARGET lua lua5.4 lua-5.4 lua54 lua5.3 lua53)
if(LUA_FOUND)
  set(ENVLP_LUA_TARGET PkgConfig::LUA)
else()
  find_path(LUA_INCLUDE_DIR NAMES lua.h PATH_SUFFIXES lua5.4 lua54 lua5.3 lua53 lua)
  find_library(LUA_LIBRARY NAMES lua54 lua5.4 lua53 lua5.3 lua)
  if(NOT LUA_INCLUDE_DIR OR NOT LUA_LIBRARY)
    message(FATAL_ERROR "Lua not found. Install mingw-w64-ucrt-x86_64-lua.")
  endif()
  add_library(ENVLP_lua UNKNOWN IMPORTED)
  set_target_properties(ENVLP_lua PROPERTIES
    IMPORTED_LOCATION "${LUA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}")
  set(ENVLP_LUA_TARGET ENVLP_lua)
endif()

# wxWidgets: MSYS2 ships a CMake config package (wx::core, ...) alongside a
# wx-config shell script that native CMake cannot execute -- so prefer CONFIG
# mode, fall back to the classic FindwxWidgets module.
find_package(wxWidgets CONFIG QUIET COMPONENTS core base aui adv webview)
if(wxWidgets_FOUND)
  if(NOT wxWidgets_LIBRARIES)
    set(wxWidgets_LIBRARIES wx::core wx::base wx::aui wx::adv wx::webview)
  endif()
else()
  find_package(wxWidgets REQUIRED COMPONENTS core base aui adv webview)
  include(${wxWidgets_USE_FILE})
endif()
# Some wx builds expose the webview target only via the config package; link
# it explicitly when present so the Math tab's wxWebView resolves.
if(TARGET wx::webview)
  list(APPEND ENVLP_WX_WEBVIEW wx::webview)
endif()

# ---------------------------------------------------------------------------
# envlpcore: GiNaC symbolic engine (no GUI dependencies)
# ---------------------------------------------------------------------------
add_library(envlpcore STATIC
  src/core/Eng.cpp
  src/core/Netlist.cpp
  src/core/Par.cpp
  src/core/MNA.cpp
  src/core/Solver.cpp
  src/core/Prune.cpp
  src/core/LowEntropy.cpp
  src/core/Analysis.cpp
  src/core/Print.cpp
  src/core/Engine.cpp
  src/core/SpiceModel.cpp
  src/core/DcNumeric.cpp
)
target_include_directories(envlpcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(envlpcore PUBLIC PkgConfig::GINAC ${ENVLP_GMP_TARGET})
target_compile_options(envlpcore PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# envlpscript: Lua VM + analysis/plot API
# ---------------------------------------------------------------------------
add_library(envlpscript STATIC
  src/script/LuaVm.cpp
)
target_include_directories(envlpscript PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(envlpscript PUBLIC envlpcore ${ENVLP_LUA_TARGET})

# ---------------------------------------------------------------------------
# ENVLP: wxWidgets GUI application
# ---------------------------------------------------------------------------
set(ENVLP_PUBLISHER "sambady929" CACHE STRING "Publisher shown in file metadata")
set(ENVLP_COPYRIGHT "MIT License" CACHE STRING "Copyright string")
# Windows resource metadata: a 4-part version (n.0.0.0) and a dotted string.
set(ENVLP_VER_COMMA "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
set(ENVLP_VER_STR   "${PROJECT_VERSION}.0")
# A 4-part version for the assembly manifest.
set(ENVLP_VER_4 "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
# The .rc and the manifest are generated so the version/publisher live in one
# place (the project() version). Both land in the build dir; the .rc refers to
# the manifest by file name, so they must sit together.
configure_file(src/app/ENVLP.rc.in       ${CMAKE_BINARY_DIR}/ENVLP.rc @ONLY)
configure_file(src/app/ENVLP.manifest.in ${CMAKE_BINARY_DIR}/ENVLP.manifest @ONLY)
# The .rc references the icon by relative name, so it must sit in the build dir.
configure_file(src/app/ENVLP.ico ${CMAKE_BINARY_DIR}/ENVLP.ico COPYONLY)

add_executable(ENVLP WIN32
  ${CMAKE_BINARY_DIR}/ENVLP.rc
  ${CMAKE_BINARY_DIR}/ENVLP.manifest
  src/app/ENVLP.ico
  src/app/App.cpp
  src/app/MainFrame.cpp
  src/app/Document.cpp
  src/app/SymbolGeom.cpp
  src/app/Symbols.cpp
  src/app/SymbolDraw.cpp
  src/app/SchematicCanvas.cpp
  src/app/PalettePanel.cpp
  src/app/PropertiesPanel.cpp
  src/app/AnalysisPanel.cpp
  src/app/AnalysisResultsFrame.cpp
  src/app/LatexRender.cpp
  src/app/MathPanel.cpp
  src/app/ResultsPanel.cpp
  src/app/BodePanel.cpp
  src/app/LuaConsole.cpp
)
# The icon file is referenced relative to the generated .rc.
set_source_files_properties(${CMAKE_BINARY_DIR}/ENVLP.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ENVLP.ico")
target_include_directories(ENVLP PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(NOT TARGET wx::core)
  target_link_libraries(ENVLP PRIVATE envlpcore envlpscript
                        ${wxWidgets_LIBRARIES} ${ENVLP_WX_WEBVIEW})
else()
  target_link_libraries(ENVLP PRIVATE envlpcore envlpscript wx::core wx::base
                         wx::aui wx::adv ${ENVLP_WX_WEBVIEW})
endif()

# MathJax (the typesetting engine the Math tab runs in its wxWebView). It is
# vendored under resources/ and copied next to the executable; the panel reads
# it at run time and falls back to the built-in renderer if it is missing.
# WebView2Loader.dll is what lets wxWidgets use the Chromium (Edge/WebView2)
# backend at all -- without it the web view silently degrades to the legacy IE
# engine, where MathJax cannot run. Both are copied next to the executable.
add_custom_command(TARGET ENVLP POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:ENVLP>/mathjax"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/mathjax/tex-svg.js"
          "$<TARGET_FILE_DIR:ENVLP>/mathjax/tex-svg.js"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/webview2/x64/WebView2Loader.dll"
          "$<TARGET_FILE_DIR:ENVLP>/WebView2Loader.dll"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:ENVLP>/plotly"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/plotly/plotly-basic.min.js"
          "$<TARGET_FILE_DIR:ENVLP>/plotly/plotly-basic.min.js"
  COMMENT "Staging MathJax, Plotly and the WebView2 loader")

# ---------------------------------------------------------------------------
# Tests (optional: the tests directory is not part of the public source tree)
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  enable_testing()
  add_subdirectory(tests)
endif()

