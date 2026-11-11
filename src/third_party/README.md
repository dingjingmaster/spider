# Vendored C libraries

- CivetWeb **v1.16**, https://github.com/civetweb/civetweb/tree/v1.16 — MIT.
  Only the C server, headers, required include fragments and license are retained;
  optional scripting runtimes and C++ wrappers are not built or shipped.
- Lexbor **v3.0.0**, https://github.com/lexbor/lexbor/tree/v3.0.0 — Apache-2.0.
  Upstream library sources and CMake configuration are retained; examples/tests
  are omitted. See the upstream LICENSE and NOTICE files.

No third-party downloads occur during configure or build. System dependencies:
libcurl >= 7.85, SQLite >= 3.35, json-c >= 0.15, OpenSSL >= 1.1.1 and pthreads.
