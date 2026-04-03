#pragma once
#ifndef NEI_XDR_XDR_H
#define NEI_XDR_XDR_H

#include <nei/macros/nei_export.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nei_xdr_status_e { NEI_XDR_OK = 0, NEI_XDR_EINVAL = -1, NEI_XDR_EBOUNDS = -2 };

struct nei_xdr_writer_st {
  uint8_t *buffer;
  size_t size;
  size_t offset;
};

struct nei_xdr_reader_st {
  const uint8_t *buffer;
  size_t size;
  size_t offset;
};

NEI_API void nei_xdr_writer_init(struct nei_xdr_writer_st *writer, void *buffer, size_t size);
NEI_API void nei_xdr_reader_init(struct nei_xdr_reader_st *reader, const void *buffer, size_t size);

NEI_API size_t nei_xdr_writer_tell(const struct nei_xdr_writer_st *writer);
NEI_API size_t nei_xdr_reader_tell(const struct nei_xdr_reader_st *reader);
NEI_API size_t nei_xdr_writer_remaining(const struct nei_xdr_writer_st *writer);
NEI_API size_t nei_xdr_reader_remaining(const struct nei_xdr_reader_st *reader);

NEI_API int nei_xdr_write_u32(struct nei_xdr_writer_st *writer, uint32_t value);
NEI_API int nei_xdr_write_i32(struct nei_xdr_writer_st *writer, int32_t value);
NEI_API int nei_xdr_write_u64(struct nei_xdr_writer_st *writer, uint64_t value);
NEI_API int nei_xdr_write_i64(struct nei_xdr_writer_st *writer, int64_t value);
NEI_API int nei_xdr_write_float(struct nei_xdr_writer_st *writer, float value);
NEI_API int nei_xdr_write_double(struct nei_xdr_writer_st *writer, double value);

NEI_API int nei_xdr_write_opaque(struct nei_xdr_writer_st *writer, const void *data, uint32_t length);
NEI_API int nei_xdr_write_bytes(struct nei_xdr_writer_st *writer, const void *data, uint32_t length);
NEI_API int nei_xdr_write_string(struct nei_xdr_writer_st *writer, const char *str, uint32_t length);

NEI_API int nei_xdr_read_u32(struct nei_xdr_reader_st *reader, uint32_t *value);
NEI_API int nei_xdr_read_i32(struct nei_xdr_reader_st *reader, int32_t *value);
NEI_API int nei_xdr_read_u64(struct nei_xdr_reader_st *reader, uint64_t *value);
NEI_API int nei_xdr_read_i64(struct nei_xdr_reader_st *reader, int64_t *value);
NEI_API int nei_xdr_read_float(struct nei_xdr_reader_st *reader, float *value);
NEI_API int nei_xdr_read_double(struct nei_xdr_reader_st *reader, double *value);

NEI_API int nei_xdr_read_opaque(struct nei_xdr_reader_st *reader, void *out, uint32_t length);
NEI_API int
nei_xdr_read_bytes(struct nei_xdr_reader_st *reader, void *out, uint32_t out_capacity, uint32_t *out_length);
NEI_API int
nei_xdr_read_string(struct nei_xdr_reader_st *reader, char *out, uint32_t out_capacity, uint32_t *out_length);
NEI_API int nei_xdr_skip_opaque(struct nei_xdr_reader_st *reader, uint32_t length);
NEI_API int nei_xdr_skip_bytes(struct nei_xdr_reader_st *reader, uint32_t *out_length);

#ifdef __cplusplus
}
#endif

#endif // NEI_XDR_XDR_H
