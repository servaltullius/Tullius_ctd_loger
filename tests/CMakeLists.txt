find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(skydiag_hang_suppression_tests
  hang_suppression_tests.cpp
)

target_include_directories(skydiag_hang_suppression_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../helper/include"
)

set_target_properties(skydiag_hang_suppression_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_hang_suppression_tests COMMAND skydiag_hang_suppression_tests)

add_executable(skydiag_crashlogger_parser_tests
  crashlogger_parser_tests.cpp
)

target_include_directories(skydiag_crashlogger_parser_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
)

set_target_properties(skydiag_crashlogger_parser_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_crashlogger_parser_tests COMMAND skydiag_crashlogger_parser_tests)

add_executable(skydiag_i18n_core_tests
  i18n_core_tests.cpp
)

target_include_directories(skydiag_i18n_core_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
)

set_target_properties(skydiag_i18n_core_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_i18n_core_tests COMMAND skydiag_i18n_core_tests)

add_executable(skydiag_bucket_tests
  bucket_tests.cpp
)

target_include_directories(skydiag_bucket_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
)

set_target_properties(skydiag_bucket_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_bucket_tests COMMAND skydiag_bucket_tests)

add_executable(skydiag_winui_xaml_tests
  winui_xaml_tests.cpp
)

set_target_properties(skydiag_winui_xaml_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_winui_xaml_tests COMMAND skydiag_winui_xaml_tests)

add_executable(skydiag_helper_crash_autopen_config_tests
  helper_crash_autopen_config_tests.cpp
)

set_target_properties(skydiag_helper_crash_autopen_config_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_helper_crash_autopen_config_tests COMMAND skydiag_helper_crash_autopen_config_tests)

add_executable(skydiag_crash_capture_false_positive_guard_tests
  crash_capture_false_positive_guard_tests.cpp
)

set_target_properties(skydiag_crash_capture_false_positive_guard_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_crash_capture_false_positive_guard_tests COMMAND skydiag_crash_capture_false_positive_guard_tests)

add_executable(skydiag_retention_tests
  retention_tests.cpp
)

target_include_directories(skydiag_retention_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../helper/include"
)

set_target_properties(skydiag_retention_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_retention_tests COMMAND skydiag_retention_tests)

add_executable(skydiag_summary_schema_fields_tests
  summary_schema_fields_tests.cpp
)

set_target_properties(skydiag_summary_schema_fields_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_summary_schema_fields_tests COMMAND skydiag_summary_schema_fields_tests)

add_executable(skydiag_hook_framework_guard_tests
  hook_framework_guard_tests.cpp
)

set_target_properties(skydiag_hook_framework_guard_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_hook_framework_guard_tests COMMAND skydiag_hook_framework_guard_tests)

add_executable(skydiag_hook_framework_json_tests
  hook_framework_json_tests.cpp
)

set_target_properties(skydiag_hook_framework_json_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_compile_definitions(skydiag_hook_framework_json_tests PRIVATE
  SKYDIAG_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

add_test(NAME skydiag_hook_framework_json_tests COMMAND skydiag_hook_framework_json_tests)
set_tests_properties(skydiag_hook_framework_json_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_system_module_guard_tests
  system_module_guard_tests.cpp
)

set_target_properties(skydiag_system_module_guard_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_system_module_guard_tests COMMAND skydiag_system_module_guard_tests)

add_executable(skydiag_scoring_logic_tests
  scoring_logic_tests.cpp
)

set_target_properties(skydiag_scoring_logic_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_scoring_logic_tests COMMAND skydiag_scoring_logic_tests)
set_tests_properties(skydiag_scoring_logic_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_stack_scan_weight_tests
  stack_scan_weight_tests.cpp
)

set_target_properties(skydiag_stack_scan_weight_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_stack_scan_weight_tests COMMAND skydiag_stack_scan_weight_tests)
set_tests_properties(skydiag_stack_scan_weight_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_signature_database_tests
  signature_database_tests.cpp
)

set_target_properties(skydiag_signature_database_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_signature_database_tests COMMAND skydiag_signature_database_tests)
set_tests_properties(skydiag_signature_database_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_address_resolver_tests
  address_resolver_tests.cpp
)

set_target_properties(skydiag_address_resolver_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_address_resolver_tests COMMAND skydiag_address_resolver_tests)
set_tests_properties(skydiag_address_resolver_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_crash_history_tests
  crash_history_tests.cpp
)

set_target_properties(skydiag_crash_history_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_crash_history_tests COMMAND skydiag_crash_history_tests)
set_tests_properties(skydiag_crash_history_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_triage_fields_tests
  triage_fields_tests.cpp
)

set_target_properties(skydiag_triage_fields_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_triage_fields_tests COMMAND skydiag_triage_fields_tests)
set_tests_properties(skydiag_triage_fields_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

include(CheckIncludeFileCXX)
check_include_file_cxx("nlohmann/json.hpp" SKYDIAG_HAS_NLOHMANN_JSON_HEADER)
if(SKYDIAG_HAS_NLOHMANN_JSON_HEADER)
  add_executable(skydiag_analysis_engine_runtime_tests
    analysis_engine_runtime_tests.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src/SignatureDatabase.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src/AddressResolver.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src/CrashHistory.cpp"
  )

  target_include_directories(skydiag_analysis_engine_runtime_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
  )

  set_target_properties(skydiag_analysis_engine_runtime_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )

  add_test(NAME skydiag_analysis_engine_runtime_tests COMMAND skydiag_analysis_engine_runtime_tests)
  set_tests_properties(skydiag_analysis_engine_runtime_tests PROPERTIES
    ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
  )
else()
  message(STATUS "skydiag_analysis_engine_runtime_tests skipped: nlohmann/json.hpp not found")
endif()

add_executable(skydiag_crash_recapture_policy_tests
  crash_recapture_policy_tests.cpp
)

target_include_directories(skydiag_crash_recapture_policy_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../helper/include"
)

set_target_properties(skydiag_crash_recapture_policy_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_crash_recapture_policy_tests COMMAND skydiag_crash_recapture_policy_tests)

add_executable(skydiag_symbol_privacy_controls_tests
  symbol_privacy_controls_tests.cpp
)

set_target_properties(skydiag_symbol_privacy_controls_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_symbol_privacy_controls_tests COMMAND skydiag_symbol_privacy_controls_tests)

add_executable(skydiag_crash_hook_mode_guard_tests
  crash_hook_mode_guard_tests.cpp
)

set_target_properties(skydiag_crash_hook_mode_guard_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_crash_hook_mode_guard_tests COMMAND skydiag_crash_hook_mode_guard_tests)

add_executable(skydiag_tier2_hybrid_config_tests
  tier2_hybrid_config_tests.cpp
)

set_target_properties(skydiag_tier2_hybrid_config_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_tier2_hybrid_config_tests COMMAND skydiag_tier2_hybrid_config_tests)

add_executable(skydiag_incident_manifest_schema_tests
  incident_manifest_schema_tests.cpp
)

set_target_properties(skydiag_incident_manifest_schema_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_incident_manifest_schema_tests COMMAND skydiag_incident_manifest_schema_tests)

add_executable(skydiag_dump_tool_cli_args_tests
  dump_tool_cli_args_tests.cpp
)

target_include_directories(skydiag_dump_tool_cli_args_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
)

set_target_properties(skydiag_dump_tool_cli_args_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_dump_tool_cli_args_tests COMMAND skydiag_dump_tool_cli_args_tests)

add_executable(skydiag_dump_tool_headless_resolver_tests
  dump_tool_headless_resolver_tests.cpp
)

target_include_directories(skydiag_dump_tool_headless_resolver_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../helper/include"
)

set_target_properties(skydiag_dump_tool_headless_resolver_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_dump_tool_headless_resolver_tests COMMAND skydiag_dump_tool_headless_resolver_tests)

add_executable(skydiag_headless_analysis_policy_tests
  headless_analysis_policy_tests.cpp
)

target_include_directories(skydiag_headless_analysis_policy_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../helper/include"
)

set_target_properties(skydiag_headless_analysis_policy_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_headless_analysis_policy_tests COMMAND skydiag_headless_analysis_policy_tests)

add_test(
  NAME skydiag_bucket_quality_script_tests
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bucket_quality_script_tests.py"
)

add_test(
  NAME skydiag_packaging_includes_cli_tests
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging_includes_cli_tests.py"
)

add_executable(skydiag_graphics_injection_rules_tests
  graphics_injection_rules_tests.cpp
)

set_target_properties(skydiag_graphics_injection_rules_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_graphics_injection_rules_tests COMMAND skydiag_graphics_injection_rules_tests)
set_tests_properties(skydiag_graphics_injection_rules_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_graphics_injection_diag_tests
  graphics_injection_diag_tests.cpp
)

set_target_properties(skydiag_graphics_injection_diag_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_graphics_injection_diag_tests COMMAND skydiag_graphics_injection_diag_tests)
set_tests_properties(skydiag_graphics_injection_diag_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_graphics_injection_integration_tests
  graphics_injection_integration_tests.cpp
)

set_target_properties(skydiag_graphics_injection_integration_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_graphics_injection_integration_tests COMMAND skydiag_graphics_injection_integration_tests)
set_tests_properties(skydiag_graphics_injection_integration_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_plugin_scanner_tests
  plugin_scanner_tests.cpp
)

set_target_properties(skydiag_plugin_scanner_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_plugin_scanner_tests COMMAND skydiag_plugin_scanner_tests)
set_tests_properties(skydiag_plugin_scanner_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_plugin_stream_tests
  plugin_stream_tests.cpp
)

set_target_properties(skydiag_plugin_stream_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_plugin_stream_tests COMMAND skydiag_plugin_stream_tests)
set_tests_properties(skydiag_plugin_stream_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)

add_executable(skydiag_plugin_rules_tests
  plugin_rules_tests.cpp
)

set_target_properties(skydiag_plugin_rules_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_plugin_rules_tests COMMAND skydiag_plugin_rules_tests)
set_tests_properties(skydiag_plugin_rules_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)
