// ESP-SR's packed model file (srmodels.bin, made by esp-sr's movemodel.py): how many bytes of it to read from the
// models partition. The partition lies above 16 MB, where flash cannot be memory-mapped, so the models are read
// into PSRAM and handed to srmodel_load(). Pure C, host tested.
//   u32 count, then per model: name[32], u32 files, then per file: name[32], u32 start, u32 size (little endian;
//   start is from the beginning of the file).
#pragma once

#include <stddef.h>
#include <stdint.h>

#define YK_SRMODELS_MAX_MODELS 16
#define YK_SRMODELS_MAX_FILES  32

/** Bytes the header itself needs, or 0 if `have` bytes are not enough to know yet (read more and call again). */
size_t yk_srmodels_header_size(const uint8_t *data, size_t have);

/** The whole file's size (the end of its last file), or 0 when `have` bytes do not hold the complete header or it is
 *  not a model file (no models, too many, or an entry pointing outside limit). */
size_t yk_srmodels_size(const uint8_t *data, size_t have, size_t limit);
