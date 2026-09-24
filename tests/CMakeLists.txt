cmake_minimum_required(VERSION 3.21)

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE symcore)

add_test(NAME core COMMAND test_core)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE symcore)

add_test(NAME integration COMMAND test_integration)

# Document tests: net-name resolution, label persistence, and serialisation.
# Compiles the (non-GUI) parts of app/Document directly so we can exercise the
# net_map API without dragging in wxWidgets for the runtime.
add_executable(test_document test_document.cpp
               ../src/app/Document.cpp ../src/app/SymbolGeom.cpp)
target_include_directories(test_document PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_document PRIVATE symcore)
add_test(NAME document COMMAND test_document)

# Debug harness: load a real .scx through Document and dump resolved nets,
# then run every analysis card the way the GUI does, and round-trip the
# LaTeX through the offline Math-tab renderer.
add_executable(test_scx_load test_scx_load.cpp
               ../src/app/Document.cpp ../src/app/SymbolGeom.cpp
               ../src/app/LatexRender.cpp)
target_include_directories(test_scx_load PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_scx_load PRIVATE symcore)
add_test(NAME scx_load COMMAND test_scx_load)

# LaTeX-subset -> HTML renderer (pure string work, no wxWidgets needed).
add_executable(test_latex test_latex.cpp ../src/app/LatexRender.cpp)
target_include_directories(test_latex PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME latex COMMAND test_latex)

# Debug harness: print the source-follower TF to verify the gm*ro term is
# produced (or discover why it isn't).
add_executable(test_cd_test test_cd_test.cpp)
target_link_libraries(test_cd_test PRIVATE symcore)
add_test(NAME cd_test COMMAND test_cd_test)

# Debug harness for cs_test topology: dump every pin's world pos and net root
# so we can see why R1 ends up in mismatched (or missing) nets.
add_executable(test_cs_test_netmap test_cs_test_netmap.cpp
               ../src/app/Document.cpp ../src/app/SymbolGeom.cpp)
target_include_directories(test_cs_test_netmap PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_cs_test_netmap PRIVATE symcore)
