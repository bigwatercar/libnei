# neixx IO Technical Notes

## Scope

This document provides a lightweight technical index for the `neixx/io` area and points to focused deep-dive documents.

## AsyncFile Error Model

`AsyncFile` now uses a two-layer error model in callbacks:

- Unified semantic code: `AsyncFile::ErrorCode`
- Native diagnostic code: `AsyncFile::Error::native_code`

Recommended usage:

- Branch business logic by `error.code` or `error.ok()`.
- Keep `error.native_code` for logs and diagnostics.

Detailed mapping table and migration guidance are documented in:

- [docs/neixx_async_file_error_model.md](docs/neixx_async_file_error_model.md)

## References

- [modules/neixx/io/include/neixx/io/async_file.h](modules/neixx/io/include/neixx/io/async_file.h)
- [modules/neixx/io/src/internal/async_file_error_code.h](modules/neixx/io/src/internal/async_file_error_code.h)
