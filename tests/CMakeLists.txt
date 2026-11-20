add_executable(test_common test_common.cpp)
target_link_libraries(test_common PRIVATE ntr_common)
add_test(NAME common COMMAND test_common)

# Frame interpolation's stream matching and blending (no melonDS or game data needed).
add_executable(test_gx test_gx.cpp ${PROJECT_SOURCE_DIR}/runtime/src/gx_match.cpp)
target_include_directories(test_gx PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
add_test(NAME gx COMMAND test_gx)

# What the DS sees of the host's buttons (taps, mashing) against a game that reads them every second frame.
add_executable(test_input test_input.cpp)
target_include_directories(test_input PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
add_test(NAME input COMMAND test_input)

# Mod system: .phmod bundles, Native C++ SDK (services, topological deps, HookService, SaveService),
# Lua 5.4 sandbox & hooks, config persistence, inter-mod bus, and NitroFS overlay overrides.
if(TARGET lua_core AND TARGET miniz_core AND TARGET ph_mods_sdk)
  set(TEST_MOD_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_bundles")
  file(MAKE_DIRECTORY "${TEST_MOD_OUT_DIR}")

  ph_add_mod(test_provider_mod
    SOURCES mods_fixtures/provider_mod.cpp
    MOD_JSON "${CMAKE_CURRENT_SOURCE_DIR}/mods_fixtures/provider_mod.json"
    RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mods_fixtures/provider_res"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/mods_fixtures"
    OUT_DIR "${TEST_MOD_OUT_DIR}")

  ph_add_mod(test_consumer_mod
    SOURCES mods_fixtures/consumer_mod.cpp
    MOD_JSON "${CMAKE_CURRENT_SOURCE_DIR}/mods_fixtures/consumer_mod.json"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/mods_fixtures"
    OUT_DIR "${TEST_MOD_OUT_DIR}")

  add_executable(test_mods test_mods.cpp
                 ${PROJECT_SOURCE_DIR}/frontend/src/mod_bundle.cpp
                 ${PROJECT_SOURCE_DIR}/frontend/src/mod_catalog.cpp
                 ${PROJECT_SOURCE_DIR}/frontend/src/mod_manager.cpp
                 ${PROJECT_SOURCE_DIR}/frontend/src/mod_romfs.cpp)
  add_dependencies(test_mods test_provider_mod test_consumer_mod)
  target_compile_definitions(test_mods PRIVATE PH_TEST_BUNDLES_DIR="${TEST_MOD_OUT_DIR}")
  target_include_directories(test_mods PRIVATE
                             ${PROJECT_SOURCE_DIR}/frontend/src
                             ${PROJECT_SOURCE_DIR}/runtime/include
                             ${PROJECT_SOURCE_DIR}/common/include)
  target_link_libraries(test_mods PRIVATE ntr_common ph_mods_sdk lua_core miniz_core SDL3::Headers)
  if(WIN32)
    target_link_libraries(test_mods PRIVATE winhttp)
  else()
    target_link_libraries(test_mods PRIVATE dl)
  endif()
  add_test(NAME mods COMMAND test_mods)
endif()
