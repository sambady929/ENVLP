cmake_minimum_required(VERSION 3.21)
# RC is required: the GUI embeds a manifest (Common-Controls 6.0) without
# which the wxWidgets DLLs fail to load on Windows.
project(SymCirc VERSION 0.1.0 LANGUAGES CXX)

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
  set(SYM_GMP_TARGET PkgConfig::GMP)
else()
  set(SYM_GMP_TARGET gmp)
endif()

pkg_search_module(LUA QUIET IMPORTED_TARGET lua lua5.4 lua-5.4 lua54 lua5.3 lua53)
if(LUA_FOUND)
  set(SYM_LUA_TARGET PkgConfig::LUA)
else()
  find_path(LUA_INCLUDE_DIR NAMES lua.h PATH_SUFFIXES lua5.4 lua54 lua5.3 lua53 lua)
  find_library(LUA_LIBRARY NAMES lua54 lua5.4 lua53 lua5.3 lua)
  if(NOT LUA_INCLUDE_DIR OR NOT LUA_LIBRARY)
    message(FATAL_ERROR "Lua not found. Install mingw-w64-ucrt-x86_64-lua.")
  endif()
  add_library(sym_lua UNKNOWN IMPORTED)
  set_target_properties(sym_lua PROPERTIES
    IMPORTED_LOCATION "${LUA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}")
  set(SYM_LUA_TARGET sym_lua)
endif()

# wxWidgets: MSYS2 ships a CMake config package (wx::core, ...) alongside a
# wx-config shell script that native CMake cannot execute -- so prefer CONFIG
# mode, fall back to the classic FindwxWidgets module.
find_package(wxWidgets CONFIG QUIET COMPONENTS core base aui adv)
if(wxWidgets_FOUND)
  if(NOT wxWidgets_LIBRARIES)
    set(wxWidgets_LIBRARIES wx::core wx::base wx::aui wx::adv)
  endif()
else()
  find_package(wxWidgets REQUIRED COMPONENTS core base aui adv)
  include(${wxWidgets_USE_FILE})
endif()

# ---------------------------------------------------------------------------
# symcore: GiNaC symbolic engine (no GUI dependencies)
# ---------------------------------------------------------------------------
add_library(symcore STATIC
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
)
target_include_directories(symcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(symcore PUBLIC PkgConfig::GINAC ${SYM_GMP_TARGET})
target_compile_options(symcore PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# symscript: Lua VM + analysis/plot API
# ---------------------------------------------------------------------------
add_library(symscript STATIC
  src/script/LuaVm.cpp
)
target_include_directories(symscript PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(symscript PUBLIC symcore ${SYM_LUA_TARGET})

# ---------------------------------------------------------------------------
# SymCirc: wxWidgets GUI application
# ---------------------------------------------------------------------------
add_executable(SymCirc WIN32
  src/app/SymCirc.rc
  src/app/App.cpp
  src/app/MainFrame.cpp
  src/app/Document.cpp
  src/app/Symbols.cpp
  src/app/SchematicCanvas.cpp
  src/app/PalettePanel.cpp
  src/app/PropertiesPanel.cpp
  src/app/AnalysisPanel.cpp
  src/app/ResultsPanel.cpp
  src/app/BodePanel.cpp
  src/app/LuaConsole.cpp
)
target_include_directories(SymCirc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(NOT TARGET wx::core)
  target_link_libraries(SymCirc PRIVATE symcore symscript ${wxWidgets_LIBRARIES})
else()
  target_link_libraries(SymCirc PRIVATE symcore symscript wx::core wx::base
                         wx::aui wx::adv)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
