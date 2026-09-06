#pragma once

#include "miniz.h"

typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;

typedef struct {
  Bytef *next_in;
  uInt avail_in;
  Bytef *next_out;
  uInt avail_out;
  uLong total_in;
  uLong total_out;
} z_stream;

#define MAX_WBITS 15
#define Z_FINISH 4
#define Z_OK 0
#define Z_STREAM_END 1

static inline int inflateInit2(z_stream *stream, int window_bits) {
  (void)stream;
  (void)window_bits;
  return Z_OK;
}

static inline int inflate(z_stream *stream, int flush) {
  (void)flush;
  const size_t output_size = tinfl_decompress_mem_to_mem(
      stream->next_out, stream->avail_out, stream->next_in, stream->avail_in, 0);
  if (output_size == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ||
      output_size > stream->avail_out) {
    return -1;
  }
  stream->total_in = stream->avail_in;
  stream->total_out = (uLong)output_size;
  stream->avail_out -= (uInt)output_size;
  return Z_STREAM_END;
}

static inline int inflateEnd(z_stream *stream) {
  (void)stream;
  return Z_OK;
}

static inline uLong crc32(uLong crc, const Bytef *data, uInt size) {
  return mz_crc32(crc, data, size);
}
