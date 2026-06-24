#include "utils.h"
#include <time.h>
#include <string.h>
#include <zlib.h>

void *safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0) {
        fprintf(stderr, "Fatal error: memory allocation failed for %zu bytes\n", size);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void *safe_realloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0) {
        fprintf(stderr, "Fatal error: memory reallocation failed for %zu bytes\n", size);
        exit(EXIT_FAILURE);
    }
    return new_ptr;
}

double get_time_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void print_progress(double percentage, double speed_mb_s, double ratio, int threads_active) {
    const int bar_width = 30;
    int progress_chars = (int)((percentage / 100.0) * bar_width);
    if (progress_chars < 0) progress_chars = 0;
    if (progress_chars > bar_width) progress_chars = bar_width;

    char bar[32];
    int i;
    for (i = 0; i < bar_width; i++) {
        if (i < progress_chars) {
            bar[i] = '=';
        } else if (i == progress_chars && progress_chars < bar_width) {
            bar[i] = '>';
        } else {
            bar[i] = ' ';
        }
    }
    bar[bar_width] = '\0';

    printf("\r\033[36mProgress:\033[0m [%s] \033[1;32m%.1f%%\033[0m | \033[33mSpeed:\033[0m %.2f MB/s | \033[35mRatio:\033[0m %.2fx | \033[34mWorkers:\033[0m %d", 
           bar, percentage, speed_mb_s, ratio, threads_active);
    fflush(stdout);
}

uint32_t calculate_crc32(uint32_t current_crc, const unsigned char *buf, size_t len) {
    return crc32(current_crc, buf, len);
}
