#include "nei/xdr/xdr.h"

#include <string.h>

static int nei_xdr_has_space(size_t size, size_t offset, size_t need) {
  if (offset > size) {
    return 0;
  }
  return need <= (size - offset);
}

static uint32_t nei_xdr_padding(uint32_t length) {
  return (uint32_t)((4U - (length & 3U)) & 3U);
}

void nei_xdr_writer_init(struct nei_xdr_writer_st *writer, void *buffer, size_t size) {
  if (writer == NULL) {
    return;
  }
  writer->buffer = (uint8_t *)buffer;
  writer->size = size;
  writer->offset = 0;
}

void nei_xdr_reader_init(struct nei_xdr_reader_st *reader, const void *buffer, size_t size) {
  if (reader == NULL) {
    return;
  }
  reader->buffer = (const uint8_t *)buffer;
  reader->size = size;
  reader->offset = 0;
}

size_t nei_xdr_writer_tell(const struct nei_xdr_writer_st *writer) {
  if (writer == NULL) {
    return 0;
  }
  return writer->offset;
}

size_t nei_xdr_reader_tell(const struct nei_xdr_reader_st *reader) {
  if (reader == NULL) {
    return 0;
  }
  return reader->offset;
}

size_t nei_xdr_writer_remaining(const struct nei_xdr_writer_st *writer) {
  if (writer == NULL || writer->offset > writer->size) {
    return 0;
  }
  return writer->size - writer->offset;
}

size_t nei_xdr_reader_remaining(const struct nei_xdr_reader_st *reader) {
  if (reader == NULL || reader->offset > reader->size) {
    return 0;
  }
  return reader->size - reader->offset;
}

int nei_xdr_write_u32(struct nei_xdr_writer_st *writer, uint32_t value) {
  if (writer == NULL || writer->buffer == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (!nei_xdr_has_space(writer->size, writer->offset, 4U)) {
    return NEI_XDR_EBOUNDS;
  }

  writer->buffer[writer->offset + 0U] = (uint8_t)((value >> 24U) & 0xFFU);
  writer->buffer[writer->offset + 1U] = (uint8_t)((value >> 16U) & 0xFFU);
  writer->buffer[writer->offset + 2U] = (uint8_t)((value >> 8U) & 0xFFU);
  writer->buffer[writer->offset + 3U] = (uint8_t)(value & 0xFFU);
  writer->offset += 4U;
  return NEI_XDR_OK;
}

int nei_xdr_write_i32(struct nei_xdr_writer_st *writer, int32_t value) {
  return nei_xdr_write_u32(writer, (uint32_t)value);
}

int nei_xdr_write_u64(struct nei_xdr_writer_st *writer, uint64_t value) {
  if (writer == NULL || writer->buffer == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (!nei_xdr_has_space(writer->size, writer->offset, 8U)) {
    return NEI_XDR_EBOUNDS;
  }

  writer->buffer[writer->offset + 0U] = (uint8_t)((value >> 56U) & 0xFFU);
  writer->buffer[writer->offset + 1U] = (uint8_t)((value >> 48U) & 0xFFU);
  writer->buffer[writer->offset + 2U] = (uint8_t)((value >> 40U) & 0xFFU);
  writer->buffer[writer->offset + 3U] = (uint8_t)((value >> 32U) & 0xFFU);
  writer->buffer[writer->offset + 4U] = (uint8_t)((value >> 24U) & 0xFFU);
  writer->buffer[writer->offset + 5U] = (uint8_t)((value >> 16U) & 0xFFU);
  writer->buffer[writer->offset + 6U] = (uint8_t)((value >> 8U) & 0xFFU);
  writer->buffer[writer->offset + 7U] = (uint8_t)(value & 0xFFU);
  writer->offset += 8U;
  return NEI_XDR_OK;
}

int nei_xdr_write_i64(struct nei_xdr_writer_st *writer, int64_t value) {
  return nei_xdr_write_u64(writer, (uint64_t)value);
}

int nei_xdr_write_float(struct nei_xdr_writer_st *writer, float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return nei_xdr_write_u32(writer, bits);
}

int nei_xdr_write_double(struct nei_xdr_writer_st *writer, double value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return nei_xdr_write_u64(writer, bits);
}

int nei_xdr_write_opaque(struct nei_xdr_writer_st *writer, const void *data, uint32_t length) {
  const uint8_t kPad[3] = {0U, 0U, 0U};
  uint32_t padding = nei_xdr_padding(length);

  if (writer == NULL || writer->buffer == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (length > 0U && data == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (!nei_xdr_has_space(writer->size, writer->offset, (size_t)length + (size_t)padding)) {
    return NEI_XDR_EBOUNDS;
  }

  if (length > 0U) {
    memcpy(writer->buffer + writer->offset, data, length);
  }
  writer->offset += (size_t)length;

  if (padding > 0U) {
    memcpy(writer->buffer + writer->offset, kPad, padding);
    writer->offset += (size_t)padding;
  }
  return NEI_XDR_OK;
}

int nei_xdr_write_bytes(struct nei_xdr_writer_st *writer, const void *data, uint32_t length) {
  int rc = nei_xdr_write_u32(writer, length);
  if (rc != NEI_XDR_OK) {
    return rc;
  }
  return nei_xdr_write_opaque(writer, data, length);
}

int nei_xdr_write_string(struct nei_xdr_writer_st *writer, const char *str, uint32_t length) {
  return nei_xdr_write_bytes(writer, str, length);
}

int nei_xdr_read_u32(struct nei_xdr_reader_st *reader, uint32_t *value) {
  uint32_t out = 0;

  if (reader == NULL || value == NULL || reader->buffer == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (!nei_xdr_has_space(reader->size, reader->offset, 4U)) {
    return NEI_XDR_EBOUNDS;
  }

  out |= ((uint32_t)reader->buffer[reader->offset + 0U]) << 24U;
  out |= ((uint32_t)reader->buffer[reader->offset + 1U]) << 16U;
  out |= ((uint32_t)reader->buffer[reader->offset + 2U]) << 8U;
  out |= ((uint32_t)reader->buffer[reader->offset + 3U]);
  reader->offset += 4U;
  *value = out;
  return NEI_XDR_OK;
}

int nei_xdr_read_i32(struct nei_xdr_reader_st *reader, int32_t *value) {
  uint32_t raw = 0;
  int rc = 0;
  if (value == NULL) {
    return NEI_XDR_EINVAL;
  }
  rc = nei_xdr_read_u32(reader, &raw);
  if (rc != NEI_XDR_OK) {
    return rc;
  }
  *value = (int32_t)raw;
  return NEI_XDR_OK;
}

int nei_xdr_read_u64(struct nei_xdr_reader_st *reader, uint64_t *value) {
  uint64_t out = 0;

  if (reader == NULL || value == NULL || reader->buffer == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (!nei_xdr_has_space(reader->size, reader->offset, 8U)) {
    return NEI_XDR_EBOUNDS;
  }

  out |= ((uint64_t)reader->buffer[reader->offset + 0U]) << 56U;
  out |= ((uint64_t)reader->buffer[reader->offset + 1U]) << 48U;
  out |= ((uint64_t)reader->buffer[reader->offset + 2U]) << 40U;
  out |= ((uint64_t)reader->buffer[reader->offset + 3U]) << 32U;
  out |= ((uint64_t)reader->buffer[reader->offset + 4U]) << 24U;
  out |= ((uint64_t)reader->buffer[reader->offset + 5U]) << 16U;
  out |= ((uint64_t)reader->buffer[reader->offset + 6U]) << 8U;
  out |= ((uint64_t)reader->buffer[reader->offset + 7U]);
  reader->offset += 8U;
  *value = out;
  return NEI_XDR_OK;
}

int nei_xdr_read_i64(struct nei_xdr_reader_st *reader, int64_t *value) {
  uint64_t raw = 0;
  int rc = 0;
  if (value == NULL) {
    return NEI_XDR_EINVAL;
  }
  rc = nei_xdr_read_u64(reader, &raw);
  if (rc != NEI_XDR_OK) {
    return rc;
  }
  *value = (int64_t)raw;
  return NEI_XDR_OK;
}

int nei_xdr_read_float(struct nei_xdr_reader_st *reader, float *value) {
  uint32_t bits = 0;
  int rc = 0;
  if (value == NULL) {
    return NEI_XDR_EINVAL;
  }
  rc = nei_xdr_read_u32(reader, &bits);
  if (rc != NEI_XDR_OK) {
    return rc;
  }
  memcpy(value, &bits, sizeof(bits));
  return NEI_XDR_OK;
}

int nei_xdr_read_double(struct nei_xdr_reader_st *reader, double *value) {
  uint64_t bits = 0;
  int rc = 0;
  if (value == NULL) {
    return NEI_XDR_EINVAL;
  }
  rc = nei_xdr_read_u64(reader, &bits);
  if (rc != NEI_XDR_OK) {
    return rc;
  }
  memcpy(value, &bits, sizeof(bits));
  return NEI_XDR_OK;
}

int nei_xdr_skip_opaque(struct nei_xdr_reader_st *reader, uint32_t length) {
  uint32_t padding = nei_xdr_padding(length);
  if (reader == NULL || reader->buffer == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (!nei_xdr_has_space(reader->size, reader->offset, (size_t)length + (size_t)padding)) {
    return NEI_XDR_EBOUNDS;
  }
  reader->offset += (size_t)length + (size_t)padding;
  return NEI_XDR_OK;
}

int nei_xdr_read_opaque(struct nei_xdr_reader_st *reader, void *out, uint32_t length) {
  uint32_t padding = nei_xdr_padding(length);
  if (reader == NULL || reader->buffer == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (length > 0U && out == NULL) {
    return NEI_XDR_EINVAL;
  }
  if (!nei_xdr_has_space(reader->size, reader->offset, (size_t)length + (size_t)padding)) {
    return NEI_XDR_EBOUNDS;
  }

  if (length > 0U) {
    memcpy(out, reader->buffer + reader->offset, length);
  }
  reader->offset += (size_t)length + (size_t)padding;
  return NEI_XDR_OK;
}

int nei_xdr_read_bytes(struct nei_xdr_reader_st *reader, void *out, uint32_t out_capacity, uint32_t *out_length) {
  uint32_t length = 0;
  int rc = nei_xdr_read_u32(reader, &length);
  if (rc != NEI_XDR_OK) {
    return rc;
  }

  if (out_length != NULL) {
    *out_length = length;
  }
  if (length > out_capacity) {
    return NEI_XDR_EBOUNDS;
  }
  return nei_xdr_read_opaque(reader, out, length);
}

int nei_xdr_read_string(struct nei_xdr_reader_st *reader, char *out, uint32_t out_capacity, uint32_t *out_length) {
  uint32_t length = 0;
  size_t old_offset = 0;
  if (out == NULL) {
    return NEI_XDR_EINVAL;
  }
  int rc = nei_xdr_read_u32(reader, &length);
  if (rc != NEI_XDR_OK) {
    return rc;
  }

  if (out_length != NULL) {
    *out_length = length;
  }
  if (length + 1U > out_capacity) {
    return NEI_XDR_EBOUNDS;
  }

  old_offset = reader->offset;
  rc = nei_xdr_read_opaque(reader, out, length);
  if (rc != NEI_XDR_OK) {
    return rc;
  }
  out[length] = '\0';
  if (reader->offset < old_offset) {
    return NEI_XDR_EBOUNDS;
  }
  return NEI_XDR_OK;
}

int nei_xdr_skip_bytes(struct nei_xdr_reader_st *reader, uint32_t *out_length) {
  uint32_t length = 0;
  int rc = nei_xdr_read_u32(reader, &length);
  if (rc != NEI_XDR_OK) {
    return rc;
  }
  if (out_length != NULL) {
    *out_length = length;
  }
  return nei_xdr_skip_opaque(reader, length);
}
