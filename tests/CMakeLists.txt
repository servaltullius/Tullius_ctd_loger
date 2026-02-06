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
