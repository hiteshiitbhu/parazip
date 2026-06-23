#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stdint.h>
#include <sys/types.h>

#define PZIP_MAGIC "PZIP"
#define PZIP_VERSION 0x0100

// Individual file metadata stored in memory and serialized to the end of the archive
typedef struct {
    char *path;
    uint32_t mode;
    uint64_t orig_size;
    uint64_t comp_size;
    uint32_t crc32;
    uint64_t data_offset;
} FileMetadata;

// Archive footer written at the absolute end of the .pzip file
typedef struct {
    uint64_t table_offset; // Absolute file offset where FileMetadata table starts
    uint32_t file_count;   // Total files in archive
    uint16_t version;      // PZIP version (0x0100)
    char magic[4];         // Magic bytes "PZIP"
} ArchiveFooter;

// Main archiver interface
int create_archive(const char *archive_path, const char **src_paths, int src_count, int thread_count);
int extract_archive(const char *archive_path);
int list_archive(const char *archive_path);

#endif // ARCHIVE_H
