# nghttp2 (vendored, lib-only)

- Upstream: https://github.com/nghttp2/nghttp2
- Version: v1.70.0
- License: MIT (see LICENSE)
- Scope: core library only (`lib/` sources + `include/nghttp2/nghttp2.h`
  and `nghttp2ver.h`).  Apps, tests, docs and bindings are not vendored.
- Build: static library target `nghttp2`, compiled as C99.  No generated
  `config.h` — the library guards all optional features behind `HAVE_*`
  macros; this CMakeLists defines only the clock macros
  (`HAVE_CLOCK_GETTIME` on POSIX, `HAVE_GETTICKCOUNT64` on Windows).
- Do NOT clang-format or otherwise modify files in this directory.
