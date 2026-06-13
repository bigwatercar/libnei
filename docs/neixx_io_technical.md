# neixx IO Technical Notes

## Scope

This document provides a lightweight technical index for the `neixx/io` area and points to focused deep-dive documents.

## AsyncFile

The `AsyncFile` subsystem provides cross-platform asynchronous file I/O with
unified error handling, positional read/write, and platform-optimized backends
(IOCP on Windows, pread/pwrite + worker thread on POSIX).

Full documentation including API usage guide, error model, platform
implementation details, threading model, and best practices:

- [docs/neixx_async_file_technical.md](docs/neixx_async_file_technical.md)

## References

- [modules/neixx/io/include/neixx/io/async_file.h](../modules/neixx/io/include/neixx/io/async_file.h)
- [modules/neixx/io/include/neixx/io/async_line_reader.h](../modules/neixx/io/include/neixx/io/async_line_reader.h)
- [modules/neixx/io/include/neixx/io/io_buffer.h](../modules/neixx/io/include/neixx/io/io_buffer.h)
- [modules/neixx/io/src/internal/async_file_error_code.h](../modules/neixx/io/src/internal/async_file_error_code.h)
