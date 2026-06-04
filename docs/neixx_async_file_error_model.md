# neixx AsyncFile Error Model

## 1. Design Goal

AsyncFile callbacks return a two-layer error model:

- Unified semantic code: `AsyncFile::ErrorCode`
- Native diagnostic code: `AsyncFile::Error::native_code`

This keeps business logic platform-agnostic while preserving enough platform detail for diagnosis.

## 2. Callback Contract

All AsyncFile callbacks use:

- Open: `void(bool success, AsyncFile::Error error)`
- Read: `void(bool success, size_t bytes_read, AsyncFile::Error error)`
- Write: `void(bool success, size_t bytes_written, AsyncFile::Error error)`

`error.ok()` is equivalent to `error.code == AsyncFile::ErrorCode::kOk`.

## 3. Mapping Table

The normalized mapping is implemented in internal error normalization logic.

| AsyncFile::ErrorCode | Semantic meaning | Typical std::errc source | Common native examples |
| --- | --- | --- | --- |
| kOk | Success | 0 | Windows 0 / POSIX 0 |
| kInvalidArgument | Invalid arguments | invalid_argument | ERROR_INVALID_PARAMETER / EINVAL |
| kNotFound | Path or object not found | no_such_file_or_directory | ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND / ENOENT |
| kPermissionDenied | Permission denied | permission_denied | ERROR_ACCESS_DENIED / EACCES |
| kBusy | Resource busy | device_or_resource_busy | ERROR_BUSY, ERROR_SHARING_VIOLATION / EBUSY |
| kAlreadyExists | Already exists | file_exists | ERROR_FILE_EXISTS, ERROR_ALREADY_EXISTS / EEXIST |
| kBadFileDescriptor | Invalid handle/fd | bad_file_descriptor | ERROR_INVALID_HANDLE / EBADF |
| kCanceled | Operation canceled | operation_canceled | ERROR_OPERATION_ABORTED / ECANCELED |
| kInvalidData | Data/encoding/seek-related invalid state | illegal_byte_sequence, invalid_seek, result_out_of_range | ERROR_INVALID_DATA / EILSEQ, ESPIPE, ERANGE |
| kIoError | Generic I/O failure | io_error or fallback | ERROR_WRITE_FAULT / EIO |
| kUnknown | Reserved internal bucket before fallback | non-generic condition before normalization fallback | platform-specific unmatched code |

Note:

- If platform mapping cannot resolve to a known generic semantic, implementation falls back to kIoError.
- `native_code` always keeps the original platform value for diagnostics.

## 4. Recommended Usage Pattern

### 4.1 Business logic

Business logic should branch only on `error.code` and avoid platform constants.

Example policy:

- Retry on: kBusy, kCanceled (based on operation and idempotency)
- User-facing fail-fast on: kInvalidArgument, kPermissionDenied, kNotFound
- Infrastructure alarms on: kIoError, kUnknown

### 4.2 Diagnostics and logging

Log both fields together:

- semantic: `error.code`
- native: `error.native_code`

This gives stable cross-platform behavior and keeps root-cause diagnostics possible.

## 5. Migration Checklist

When migrating old callback sites:

1. Replace direct numeric checks (`error_code == 0`) with `error.ok()`.
2. Replace platform constant branches (`ERROR_*` / `errno`) with `error.code` branches.
3. Keep `error.native_code` in logs, telemetry, and debug dumps.
4. Prefer tests asserting `error.code` semantics instead of raw platform values.
