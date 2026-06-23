#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// Safe memory allocation wrappers
void *safe_malloc(size_t size);
void *safe_realloc(void *ptr, size_t size);

// Timing utilities for performance benchmarking
double get_time_seconds(void);

// Terminal display utility for live progress
void print_progress(double percentage, double speed_mb_s, double ratio, int threads_active);

// CRC32 checksum utility
uint32_t calculate_crc32(uint32_t current_crc, const unsigned char *buf, size_t len);

#endif // UTILS_H
