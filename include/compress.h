#ifndef COMPRESS_H
#define COMPRESS_H

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

// 128 KB block size: balances compression ratio (larger blocks compress better) 
// and memory consumption under high thread counts (smaller blocks use less RAM).
#define BLOCK_SIZE (128 * 1024)


// Data block representation in the processing pipeline
typedef struct {
    uint32_t block_id;
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    unsigned char *data;        // Buffer containing block payload (uncompressed or compressed)
    int status;                 // Execution status (0 for OK, non-zero for error)
} DataBlock;

// Synchronized thread-safe queue for worker threads (producer-consumer)
typedef struct {
    DataBlock **buffer;
    int capacity;
    int head;
    int tail;
    int size;
    int active_producers;       // Count of active producers to coordinate termination
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} BlockQueue;

// BlockQueue operations
BlockQueue *queue_init(int capacity);
void queue_destroy(BlockQueue *q);
void queue_push(BlockQueue *q, DataBlock *block);
DataBlock *queue_pop(BlockQueue *q);
void queue_decrement_producers(BlockQueue *q);

// High-level pipeline functions for compressing and decompressing individual file streams
uint64_t compress_stream(FILE *src, FILE *dest, int thread_count, uint32_t *out_crc32);
uint64_t decompress_stream(FILE *src, FILE *dest, int thread_count, uint64_t compressed_size);

#endif // COMPRESS_H
