# TESTS KNOWLEDGE BASE

## OVERVIEW
`tests/` uses standalone assert-based C++ executables plus Python script tests, all registered in CTest.

## STRUCTURE
```text
tests/
|- *.cpp                 # unit/guard/policy executables
|- *.py                  # script/package behavior tests
|- data/                 # small fixture assets
|- SourceGuardTestUtils.h
`- CMakeLists.txt        # test target registry
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Test registry | `tests/CMakeLists.txt` | authoritative add_executable/add_test list |
| JSON/schema guards | `tests/json_schema_validation_guard_tests.cpp` | data/schema lock tests |
| Runtime policy guards | `tests/*_guard_tests.cpp` | regression/protection constraints |
| Analyzer/runtime checks | `tests/analysis_engine_runtime_tests.cpp` | core behavior assertions |
| Packaging scripts tests | `tests/packaging_includes_cli_tests.py` | zip content policy |
| Plugin/dump rules logic | `tests/plugin_rules_logic_tests.cpp` | rules consistency checks |

## CONVENTIONS
- Keep tests self-contained and assert-based; no gtest migration in this tree.
- Register every new test target in `tests/CMakeLists.txt`.
- Use `SKYDIAG_PROJECT_ROOT` env where existing tests depend on repo data files.
- Preserve naming convention: `skydiag_<domain>_tests` for CTest targets.

## COMMANDS
```bash
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure
```

## ANTI-PATTERNS
- Do not delete guard tests to pass CI.
- Do not bypass schema/policy checks without explicit policy update.
- Do not add tests outside CTest registration.
- Do not add flaky time-sensitive assertions without mitigation.

## NOTES
- Many tests enforce release policy, schema compatibility, and packaging guarantees.
- A green test run is a release prerequisite in this repository.
