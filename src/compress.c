#include "compress.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// Initialize a synchronized block queue
BlockQueue *queue_init(int capacity) {
    BlockQueue *q = safe_malloc(sizeof(BlockQueue));
    q->buffer = safe_malloc(capacity * sizeof(DataBlock*));
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->size = 0;
    q->active_producers = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

// Destroy queue resources
void queue_destroy(BlockQueue *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->buffer);
    free(q);
}

// Push a block to the queue (blocks if queue is full)
void queue_push(BlockQueue *q, DataBlock *block) {
    pthread_mutex_lock(&q->mutex);
    while (q->size == q->capacity) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }
    q->buffer[q->tail] = block;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

// Pop a block from the queue (blocks if queue is empty, returns NULL if all producers finished)
DataBlock *queue_pop(BlockQueue *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->size == 0 && q->active_producers > 0) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }
    if (q->size == 0 && q->active_producers == 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    DataBlock *block = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->size--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return block;
}

// Signal that one producer has finished pushing to this queue
void queue_decrement_producers(BlockQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->active_producers--;
    if (q->active_producers == 0) {
        pthread_cond_broadcast(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mutex);
}

// --- Thread Argument Structures ---
typedef struct {
    FILE *src;
    BlockQueue *input_queue;
    uint64_t limit;            // Non-zero if we need to limit the bytes read (for decompressing nested files)
    uint32_t *crc32_accum;     // Accumulates CRC32 for compression
    uint64_t total_bytes_read; // Output statistic
    int is_compression;        // Explicit operation flag
} ReaderArgs;

typedef struct {
    BlockQueue *input_queue;
    BlockQueue *output_queue;
    int is_compression;
} WorkerArgs;

typedef struct {
    FILE *dest;
    BlockQueue *output_queue;
    uint64_t total_written;
    int success;
    int is_compression;        // Explicit operation flag
} WriterArgs;

// --- Thread Worker Functions ---

// Reader thread: Reads uncompressed blocks (compress) or compressed blocks (decompress) from stream
static void *reader_thread_func(void *arg) {
    ReaderArgs *args = (ReaderArgs *)arg;
    uint32_t block_id = 0;
    args->total_bytes_read = 0;

    if (args->is_compression) {
        // --- Compression Case: Read standard file chunks ---
        unsigned char *buffer = safe_malloc(BLOCK_SIZE);
        size_t bytes_read;

        while ((bytes_read = fread(buffer, 1, BLOCK_SIZE, args->src)) > 0) {
            DataBlock *block = safe_malloc(sizeof(DataBlock));
            block->block_id = block_id++;
            block->uncompressed_size = bytes_read;
            block->compressed_size = 0;
            block->data = buffer;
            block->status = 0;

            // Accumulate CRC32 of original data
            if (args->crc32_accum) {
                *args->crc32_accum = calculate_crc32(*args->crc32_accum, buffer, bytes_read);
            }

            args->total_bytes_read += bytes_read;
            queue_push(args->input_queue, block);

            // Allocate next buffer
            buffer = safe_malloc(BLOCK_SIZE);
        }
        free(buffer); // Clean up unused buffer at EOF
    } else {
        // --- Decompression Case: Read serialized blocks ---
        uint64_t bytes_processed = 0;
        while (bytes_processed < args->limit) {
            uint32_t comp_size = 0;
            uint32_t uncomp_size = 0;

            // Read block metadata headers
            if (fread(&comp_size, sizeof(uint32_t), 1, args->src) != 1 ||
                fread(&uncomp_size, sizeof(uint32_t), 1, args->src) != 1) {
                break;
            }

            unsigned char *comp_data = safe_malloc(comp_size);
            if (fread(comp_data, 1, comp_size, args->src) != comp_size) {
                free(comp_data);
                break;
            }

            DataBlock *block = safe_malloc(sizeof(DataBlock));
            block->block_id = block_id++;
            block->uncompressed_size = uncomp_size;
            block->compressed_size = comp_size;
            block->data = comp_data;
            block->status = 0;

            bytes_processed += sizeof(uint32_t) * 2 + comp_size;
            args->total_bytes_read += bytes_processed;

            queue_push(args->input_queue, block);
        }
    }

    queue_decrement_producers(args->input_queue);
    return NULL;
}

// Worker thread: Pops blocks, performs zlib compression/decompression, and pushes results
static void *worker_thread_func(void *arg) {
    WorkerArgs *args = (WorkerArgs *)arg;

    while (1) {
        DataBlock *block = queue_pop(args->input_queue);
        if (block == NULL) {
            break; // No more blocks in input queue
        }

        if (args->is_compression) {
            // --- Compress Block ---
            z_stream strm;
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;

            int ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
            if (ret != Z_OK) {
                block->status = ret;
                queue_push(args->output_queue, block);
                continue;
            }

            strm.avail_in = block->uncompressed_size;
            strm.next_in = block->data;

            // Allocate upper bound size for compressed output
            uLong max_output = deflateBound(&strm, block->uncompressed_size);
            unsigned char *comp_buf = safe_malloc(max_output);

            strm.avail_out = max_output;
            strm.next_out = comp_buf;

            ret = deflate(&strm, Z_FINISH);
            if (ret != Z_STREAM_END) {
                block->status = (ret == Z_OK) ? Z_BUF_ERROR : ret;
                free(comp_buf);
                deflateEnd(&strm);
                queue_push(args->output_queue, block);
                continue;
            }

            block->compressed_size = strm.total_out;
            deflateEnd(&strm);

            // Reallocate to save memory and swap buffer
            unsigned char *resized = safe_realloc(comp_buf, block->compressed_size);
            free(block->data);
            block->data = resized;
        } else {
            // --- Decompress Block ---
            z_stream strm;
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;

            int ret = inflateInit(&strm);
            if (ret != Z_OK) {
                block->status = ret;
                queue_push(args->output_queue, block);
                continue;
            }

            strm.avail_in = block->compressed_size;
            strm.next_in = block->data;

            unsigned char *uncomp_buf = safe_malloc(block->uncompressed_size);
            strm.avail_out = block->uncompressed_size;
            strm.next_out = uncomp_buf;

            ret = inflate(&strm, Z_FINISH);
            if (ret != Z_STREAM_END && ret != Z_OK) {
                block->status = ret;
                free(uncomp_buf);
                inflateEnd(&strm);
                queue_push(args->output_queue, block);
                continue;
            }

            inflateEnd(&strm);
            free(block->data);
            block->data = uncomp_buf;
        }

        queue_push(args->output_queue, block);
    }

    queue_decrement_producers(args->output_queue);
    return NULL;
}

// Writer thread: Re-sequences blocks in order and writes them to the output stream
static void *writer_thread_func(void *arg) {
    WriterArgs *args = (WriterArgs *)arg;
    args->total_written = 0;
    args->success = 1;

    uint32_t expected_block_id = 0;
    int pending_capacity = 128;
    DataBlock **pending_blocks = safe_malloc(pending_capacity * sizeof(DataBlock*));
    memset(pending_blocks, 0, pending_capacity * sizeof(DataBlock*));

    while (1) {
        DataBlock *block = queue_pop(args->output_queue);
        if (block == NULL) {
            break; // All blocks processed
        }

        // Expand ordering buffer if needed
        if (block->block_id >= (uint32_t)pending_capacity) {
            int old_cap = pending_capacity;
            pending_capacity = block->block_id + 64;
            pending_blocks = safe_realloc(pending_blocks, pending_capacity * sizeof(DataBlock*));
            memset(pending_blocks + old_cap, 0, (pending_capacity - old_cap) * sizeof(DataBlock*));
        }

        pending_blocks[block->block_id] = block;

        // Write sequentially completed blocks
        while (expected_block_id < (uint32_t)pending_capacity && pending_blocks[expected_block_id] != NULL) {
            DataBlock *b = pending_blocks[expected_block_id];

            if (b->status != 0) {
                fprintf(stderr, "\nError processing block %u (zlib error: %d)\n", b->block_id, b->status);
                args->success = 0;
            }

            if (args->success) {
                if (args->is_compression) {
                    // --- Compression Case: Write header (comp/uncomp sizes) followed by data ---
                    fwrite(&b->compressed_size, sizeof(uint32_t), 1, args->dest);
                    fwrite(&b->uncompressed_size, sizeof(uint32_t), 1, args->dest);
                    fwrite(b->data, 1, b->compressed_size, args->dest);
                    args->total_written += sizeof(uint32_t) * 2 + b->compressed_size;
                } else {
                    // --- Decompression Case: Write raw inflated bytes ---
                    fwrite(b->data, 1, b->uncompressed_size, args->dest);
                    args->total_written += b->uncompressed_size;
                }
            }

            free(b->data);
            free(b);
            pending_blocks[expected_block_id] = NULL;
            expected_block_id++;
        }
    }

    // Double check that we didn't leak any blocks in our ordering buffer
    for (int i = 0; i < pending_capacity; i++) {
        if (pending_blocks[i] != NULL) {
            free(pending_blocks[i]->data);
            free(pending_blocks[i]);
        }
    }
    free(pending_blocks);

    return NULL;
}

// --- Public Compression / Decompression Streams Interfaces ---

uint64_t compress_stream(FILE *src, FILE *dest, int thread_count, uint32_t *out_crc32) {
    if (thread_count < 1) thread_count = 1;

    // Buffer queues capacity is sized to throttle disk I/O when CPU is busy
    int queue_capacity = thread_count * 4;
    if (queue_capacity < 8) queue_capacity = 8;

    BlockQueue *input_queue = queue_init(queue_capacity);
    BlockQueue *output_queue = queue_init(queue_capacity);

    // Setup active producer counts
    input_queue->active_producers = 1; // Reader thread
    output_queue->active_producers = thread_count; // Workers threads

    // Set output CRC accumulator to initial value
    if (out_crc32) *out_crc32 = crc32(0L, Z_NULL, 0);

    // Initialize threads
    pthread_t reader_tid;
    pthread_t *worker_tids = safe_malloc(thread_count * sizeof(pthread_t));
    pthread_t writer_tid;

    ReaderArgs r_args = {src, input_queue, 0, out_crc32, 0, 1};
    WorkerArgs *w_args = safe_malloc(thread_count * sizeof(WorkerArgs));
    WriterArgs wr_args = {dest, output_queue, 0, 1, 1};

    pthread_create(&reader_tid, NULL, reader_thread_func, &r_args);

    for (int i = 0; i < thread_count; i++) {
        w_args[i].input_queue = input_queue;
        w_args[i].output_queue = output_queue;
        w_args[i].is_compression = 1;
        pthread_create(&worker_tids[i], NULL, worker_thread_func, &w_args[i]);
    }

    pthread_create(&writer_tid, NULL, writer_thread_func, &wr_args);

    // Join all threads to block until execution completes
    pthread_join(reader_tid, NULL);
    for (int i = 0; i < thread_count; i++) {
        pthread_join(worker_tids[i], NULL);
    }
    pthread_join(writer_tid, NULL);

    // Cleanup memory
    queue_destroy(input_queue);
    queue_destroy(output_queue);
    free(worker_tids);
    free(w_args);

    return wr_args.success ? wr_args.total_written : 0;
}

uint64_t decompress_stream(FILE *src, FILE *dest, int thread_count, uint64_t compressed_size) {
    if (thread_count < 1) thread_count = 1;

    int queue_capacity = thread_count * 4;
    if (queue_capacity < 8) queue_capacity = 8;

    BlockQueue *input_queue = queue_init(queue_capacity);
    BlockQueue *output_queue = queue_init(queue_capacity);

    input_queue->active_producers = 1;
    output_queue->active_producers = thread_count;

    pthread_t reader_tid;
    pthread_t *worker_tids = safe_malloc(thread_count * sizeof(pthread_t));
    pthread_t writer_tid;

    ReaderArgs r_args = {src, input_queue, compressed_size, NULL, 0, 0};
    WorkerArgs *w_args = safe_malloc(thread_count * sizeof(WorkerArgs));
    WriterArgs wr_args = {dest, output_queue, 0, 1, 0};

    pthread_create(&reader_tid, NULL, reader_thread_func, &r_args);

    for (int i = 0; i < thread_count; i++) {
        w_args[i].input_queue = input_queue;
        w_args[i].output_queue = output_queue;
        w_args[i].is_compression = 0;
        pthread_create(&worker_tids[i], NULL, worker_thread_func, &w_args[i]);
    }

    pthread_create(&writer_tid, NULL, writer_thread_func, &wr_args);

    pthread_join(reader_tid, NULL);
    for (int i = 0; i < thread_count; i++) {
        pthread_join(worker_tids[i], NULL);
    }
    pthread_join(writer_tid, NULL);

    queue_destroy(input_queue);
    queue_destroy(output_queue);
    free(worker_tids);
    free(w_args);

    return wr_args.success ? wr_args.total_written : 0;
}
