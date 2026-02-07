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
