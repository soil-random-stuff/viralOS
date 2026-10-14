#pragma once

#include <stddef.h>
#include <stdint.h>

struct tar_result {
    uint64_t files;
    uint64_t directories;
    uint64_t bytes;
    uint64_t skipped;   /* entry types we don't support (links, devices, ...) */
};

/*
 * Unpack a USTAR archive into the VFS, relative to the root.
 * Returns 0 or a negative errno (-EINVAL for a malformed archive).
 */
int tar_unpack(const void *archive, size_t size, struct tar_result *result);
