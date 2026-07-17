# Label scheme (drives CI and the Makefile):
#   scaffold  — Claude-Code-owned plumbing; must ALWAYS be green.
#   checkpoint;cpN — fail until the human implements checkpoint N (by design).
#   m3 — integration tests for the Tier 1 command set (green after M3).

include(FetchContent)
FetchContent_Declare(
  catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.5.4.tar.gz
)
FetchContent_MakeAvailable(catch2)

# ---- unit -------------------------------------------------------------------
add_executable(scaffold_tests
  unit/resp_writer_test.cpp
  unit/config_test.cpp
)
target_link_libraries(scaffold_tests PRIVATE hermit_core Catch2::Catch2WithMain)
add_test(NAME scaffold_tests COMMAND scaffold_tests)
set_tests_properties(scaffold_tests PROPERTIES LABELS "scaffold")

add_executable(m3_db_tests unit/m3_db_test.cpp)
target_link_libraries(m3_db_tests PRIVATE hermit_core Catch2::Catch2WithMain)
add_test(NAME m3_db_tests COMMAND m3_db_tests)
set_tests_properties(m3_db_tests PROPERTIES LABELS "m3")

add_executable(m3_command_tests
  unit/m3_commands_test.cpp
  unit/m3_list_commands_test.cpp
)
target_link_libraries(m3_command_tests PRIVATE hermit_core Catch2::Catch2WithMain)
add_test(NAME m3_command_tests COMMAND m3_command_tests)
set_tests_properties(m3_command_tests PROPERTIES LABELS "m3")

add_executable(m3_snapshot_tests unit/m3_snapshot_test.cpp)
target_link_libraries(m3_snapshot_tests PRIVATE hermit_core Catch2::Catch2WithMain)
add_test(NAME m3_snapshot_tests COMMAND m3_snapshot_tests)
set_tests_properties(m3_snapshot_tests PROPERTIES LABELS "m3")

# Complete serializer + CP3-stub expiry: fails until CP3 lands. Informational.
add_executable(post_cp3_snapshot_ttl_tests unit/post_cp3_snapshot_ttl_test.cpp)
target_link_libraries(post_cp3_snapshot_ttl_tests PRIVATE hermit_core Catch2::Catch2WithMain)
add_test(NAME post_cp3_snapshot_ttl_tests COMMAND post_cp3_snapshot_ttl_tests)
set_tests_properties(post_cp3_snapshot_ttl_tests PROPERTIES LABELS "post-cp3")

add_executable(cp1_parser_tests unit/cp1_resp_parser_test.cpp)
target_link_libraries(cp1_parser_tests PRIVATE hermit_core Catch2::Catch2WithMain)
add_test(NAME cp1_parser_tests COMMAND cp1_parser_tests)
set_tests_properties(cp1_parser_tests PROPERTIES LABELS "checkpoint;cp1")

add_executable(cp3_expiry_tests unit/cp3_expiry_test.cpp)
target_link_libraries(cp3_expiry_tests PRIVATE hermit_core Catch2::Catch2WithMain)
add_test(NAME cp3_expiry_tests COMMAND cp3_expiry_tests)
set_tests_properties(cp3_expiry_tests PROPERTIES LABELS "checkpoint;cp3")

add_executable(cp4_wal_tests unit/cp4_wal_test.cpp)
target_link_libraries(cp4_wal_tests PRIVATE hermit_core Catch2::Catch2WithMain)
add_test(NAME cp4_wal_tests COMMAND cp4_wal_tests)
set_tests_properties(cp4_wal_tests PROPERTIES LABELS "checkpoint;cp4")

# ---- integration (spawn the real binary, drive real sockets) ----------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  set(INTEG ${CMAKE_CURRENT_SOURCE_DIR}/integration)
  function(hermit_integration_test name script labels)
    add_test(NAME ${name}
             COMMAND ${Python3_EXECUTABLE} ${INTEG}/${script} $<TARGET_FILE:hermitdb>)
    set_tests_properties(${name} PROPERTIES LABELS "${labels}" TIMEOUT 120)
  endfunction()

  hermit_integration_test(integration_ping        test_ping.py              "checkpoint;cp2")
  hermit_integration_test(integration_concurrency test_pipeline_clients.py  "checkpoint;cp2")
  hermit_integration_test(integration_slow_reader test_slow_reader.py       "checkpoint;cp2")
  # Built now, skipped (exit 77) until CP2 provides a running server; the
  # command set itself is covered by the m3 unit tests without sockets.
  hermit_integration_test(integration_tier1       test_tier1.py             "post-cp2")
  set_tests_properties(integration_tier1 PROPERTIES SKIP_RETURN_CODE 77)
  hermit_integration_test(integration_ttl         test_ttl.py               "checkpoint;cp3")
  hermit_integration_test(integration_kill9       test_kill9_recovery.py    "checkpoint;cp4")
  hermit_integration_test(integration_incr        test_incr_concurrency.py  "checkpoint;cp5")
endif()
