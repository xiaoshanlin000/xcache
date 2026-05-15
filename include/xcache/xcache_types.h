#ifndef XCACHE_XCACHE_TYPES_H_
#define XCACHE_XCACHE_TYPES_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
    XCACHE_TEXT    = 0,
    XCACHE_INT64   = 1,
    XCACHE_INT32   = 2,
    XCACHE_FLOAT   = 3,
    XCACHE_DOUBLE  = 4,
    XCACHE_BOOLEAN = 5,
    XCACHE_BLOB    = 6,
    XCACHE_VECTOR  = 7,
    XCACHE_SET     = 8,
    XCACHE_MAP     = 9,
} xcache_value_type_t;

typedef enum {
    XCACHE_OK            = 0,
    XCACHE_NOT_FOUND     = 1,
    XCACHE_TYPE_MISMATCH = 2,
    XCACHE_EXPIRED       = 3,
    XCACHE_NO_SPACE      = 4,
    XCACHE_IO_ERROR      = 5,
    XCACHE_INVALID_ARG   = 6,
} xcache_error_t;

typedef struct {
    unsigned char* data;
    size_t         len;
} xcache_blob_t;

#endif  // XCACHE_XCACHE_TYPES_H_
