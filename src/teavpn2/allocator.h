// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021  Ammar Faizi
 */

#ifndef TEAVPN2__ALLOCATOR_H
#define TEAVPN2__ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

extern void *al64_calloc(size_t nmemb, size_t size);
extern void *al64_malloc(size_t size);
extern void al64_free(void *user);
extern void *al64_realloc(void *user, size_t new_size);

#endif /* #ifndef TEAVPN2__ALLOCATOR_H */
