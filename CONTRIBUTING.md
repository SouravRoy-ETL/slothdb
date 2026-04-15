# Contributing to SlothDB

Thank you for your interest in contributing to SlothDB!

## Getting Started

1. Fork the repository
2. Clone your fork: `git clone https://github.com/YOUR_USERNAME/slothdb.git`
3. Create a branch: `git checkout -b my-feature`
4. Build: `cmake -B build && cmake --build build --config Release`
5. Test: `./build/test/Release/slothdb_tests`
6. Make your changes
7. Run tests again to verify
8. Push and create a pull request

## Code Style

- C++20 standard
- Files: `snake_case.cpp` / `snake_case.hpp`
- Classes: `PascalCase`
- Methods: `PascalCase` for public, `camelCase` for private
- Constants: `UPPER_SNAKE_CASE`
- Namespace: `slothdb`
- Headers: `#pragma once`
- Include paths: `#include "slothdb/subsystem/file.hpp"`

## Architecture

```
src/
  common/       - Types, allocator, strings, thread pool
  catalog/      - Schema metadata (tables, columns)
  parser/       - SQL tokenizer and recursive descent parser
  binder/       - Name resolution and type inference
  planner/      - Logical operator tree construction
  optimizer/    - Query optimization passes
  execution/    - Physical operators and expression executor
  storage/      - Columnar storage, file I/O, compression
  transaction/  - MVCC transaction management
  main/         - Database and Connection classes
  api/          - C API (slothdb.h)
  extension/    - Extension loading and management
```

## Adding a New SQL Function

1. Add the function name to the binder's return type mapping in `src/binder/binder.cpp`
2. Add execution logic in `src/execution/expression_executor.cpp`
3. Write a test in `test/unit/execution/`

## Adding a New File Format

1. Create reader/writer in `src/storage/`
2. Add the format to the parser's table function list in `src/parser/parser.cpp`
3. Add handling in `src/main/connection.cpp` (table function + COPY support)
4. Write tests in `test/unit/storage/`

## Building Extensions

See the Extension API documentation in `include/slothdb/extension/extension_api.h`.

## Tests

All changes must pass the existing test suite. Add tests for new features.

```bash
# Run all tests
./build/test/Release/slothdb_tests

# Run specific tests
./build/test/Release/slothdb_tests -tc="YourTest*"
```
