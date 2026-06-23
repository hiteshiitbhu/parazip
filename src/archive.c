#include "archive.h"
#include "compress.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <libgen.h>
#include <zlib.h>

// Struct for representing files discovered during scanning
typedef struct {
    char *path;
    uint32_t mode;
    uint64_t orig_size;
} DiscoveredFile;

static void add_to_list(DiscoveredFile **list, int *count, int *cap, const char *path, uint32_t mode, uint64_t size) {
    if (*count >= *cap) {
        *cap = (*cap == 0) ? 16 : *cap * 2;
        *list = safe_realloc(*list, *cap * sizeof(DiscoveredFile));
    }
    (*list)[*count].path = strdup(path);
    (*list)[*count].mode = mode;
    (*list)[*count].orig_size = size;
    (*count)++;
}

// Recursive directory scanning
static void scan_recursive(DiscoveredFile **list, int *count, int *cap, const char *base_path) {
    struct stat st;
    if (stat(base_path, &st) != 0) {
        fprintf(stderr, "Warning: Cannot access path '%s'\n", base_path);
        return;
    }

    if (S_ISREG(st.st_mode)) {
        add_to_list(list, count, cap, base_path, st.st_mode, st.st_size);
    } else if (S_ISDIR(st.st_mode)) {
        // Record the directory itself first
        add_to_list(list, count, cap, base_path, st.st_mode, 0);

        DIR *dir = opendir(base_path);
        if (!dir) {
            fprintf(stderr, "Warning: Cannot open directory '%s'\n", base_path);
            return;
        }

        struct dirent *entry;
        char sub_path[1024];
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            snprintf(sub_path, sizeof(sub_path), "%s/%s", base_path, entry->d_name);
            scan_recursive(list, count, cap, sub_path);
        }
        closedir(dir);
    }
}

// Recursively create directory components
static void make_directories_recursive(const char *dir_path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    int len = strlen(tmp);
    if (len == 0) return;
    
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

// Extracts parent folder path and creates directories
static void create_parent_dirs(const char *file_path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", file_path);
    char *dir = dirname(tmp);
    if (strcmp(dir, ".") != 0 && strcmp(dir, "/") != 0) {
        make_directories_recursive(dir);
    }
}

// --- Public Archiver Functions ---

int create_archive(const char *archive_path, const char **src_paths, int src_count, int thread_count) {
    FILE *archive = fopen(archive_path, "wb");
    if (!archive) {
        fprintf(stderr, "Error: Cannot open output archive '%s' for writing\n", archive_path);
        return -1;
    }

    DiscoveredFile *discovered = NULL;
    int file_count = 0;
    int file_cap = 0;

    // Scan all sources
    for (int i = 0; i < src_count; i++) {
        scan_recursive(&discovered, &file_count, &file_cap, src_paths[i]);
    }

    if (file_count == 0) {
        fprintf(stderr, "Error: No files or directories found to archive\n");
        fclose(archive);
        return -1;
    }

    FileMetadata *metadata_table = safe_malloc(file_count * sizeof(FileMetadata));

    printf("Archiving and compressing %d items using %d worker threads...\n", file_count, thread_count);
    
    for (int i = 0; i < file_count; i++) {
        DiscoveredFile *df = &discovered[i];
        
        metadata_table[i].path = df->path;
        metadata_table[i].mode = df->mode;
        metadata_table[i].orig_size = df->orig_size;
        metadata_table[i].comp_size = 0;
        metadata_table[i].crc32 = 0;
        metadata_table[i].data_offset = ftell(archive);

        if (S_ISREG(df->mode) && df->orig_size > 0) {
            FILE *src_file = fopen(df->path, "rb");
            if (!src_file) {
                fprintf(stderr, "\nWarning: Cannot open file '%s'. Skipping.\n", df->path);
                continue;
            }

            printf("Compressing %s... ", df->path);
            fflush(stdout);

            double start_t = get_time_seconds();
            uint32_t crc = 0;
            uint64_t written = compress_stream(src_file, archive, thread_count, &crc);
            double end_t = get_time_seconds();

            fclose(src_file);

            metadata_table[i].comp_size = written;
            metadata_table[i].crc32 = crc;

            double elapsed = end_t - start_t;
            if (elapsed <= 0.0) elapsed = 0.001;
            double speed = (df->orig_size / (1024.0 * 1024.0)) / elapsed;
            double ratio = (double)df->orig_size / (written > 0 ? written : 1);

            printf(" [Done] - Ratio: %.2fx, Speed: %.2f MB/s\n", ratio, speed);
        } else {
            // Directory or empty file
            metadata_table[i].comp_size = 0;
            metadata_table[i].crc32 = 0;
        }
    }

    // Write FileMetadata Central Directory Table
    uint64_t table_start = ftell(archive);
    for (int i = 0; i < file_count; i++) {
        FileMetadata *m = &metadata_table[i];
        uint16_t path_len = strlen(m->path);
        
        fwrite(&path_len, sizeof(uint16_t), 1, archive);
        fwrite(m->path, 1, path_len, archive);
        fwrite(&m->mode, sizeof(uint32_t), 1, archive);
        fwrite(&m->orig_size, sizeof(uint64_t), 1, archive);
        fwrite(&m->comp_size, sizeof(uint64_t), 1, archive);
        fwrite(&m->crc32, sizeof(uint32_t), 1, archive);
        fwrite(&m->data_offset, sizeof(uint64_t), 1, archive);
    }

    // Write Archive Footer
    ArchiveFooter footer;
    footer.table_offset = table_start;
    footer.file_count = file_count;
    footer.version = PZIP_VERSION;
    memcpy(footer.magic, PZIP_MAGIC, 4);

    fwrite(&footer.table_offset, sizeof(uint64_t), 1, archive);
    fwrite(&footer.file_count, sizeof(uint32_t), 1, archive);
    fwrite(&footer.version, sizeof(uint16_t), 1, archive);
    fwrite(footer.magic, 1, 4, archive);

    fclose(archive);

    // Free resources
    for (int i = 0; i < file_count; i++) {
        free(discovered[i].path);
    }
    free(discovered);
    free(metadata_table);

    printf("\033[1;32mArchive successfully created: %s\033[0m\n", archive_path);
    return 0;
}

int extract_archive(const char *archive_path) {
    FILE *archive = fopen(archive_path, "rb");
    if (!archive) {
        fprintf(stderr, "Error: Cannot open archive '%s' for extraction\n", archive_path);
        return -1;
    }

    fseek(archive, 0, SEEK_END);
    long size = ftell(archive);
    if (size < 18) {
        fprintf(stderr, "Error: Invalid archive file structure\n");
        fclose(archive);
        return -1;
    }

    fseek(archive, size - 18, SEEK_SET);

    ArchiveFooter footer;
    if (fread(&footer.table_offset, sizeof(uint64_t), 1, archive) != 1 ||
        fread(&footer.file_count, sizeof(uint32_t), 1, archive) != 1 ||
        fread(&footer.version, sizeof(uint16_t), 1, archive) != 1 ||
        fread(footer.magic, 1, 4, archive) != 4) {
        fprintf(stderr, "Error: Failed to read archive footer\n");
        fclose(archive);
        return -1;
    }

    if (memcmp(footer.magic, PZIP_MAGIC, 4) != 0) {
        fprintf(stderr, "Error: Archive header magic signature mismatch\n");
        fclose(archive);
        return -1;
    }

    fseek(archive, footer.table_offset, SEEK_SET);
    FileMetadata *files = safe_malloc(footer.file_count * sizeof(FileMetadata));

    for (uint32_t i = 0; i < footer.file_count; i++) {
        uint16_t path_len;
        if (fread(&path_len, sizeof(uint16_t), 1, archive) != 1) {
            fprintf(stderr, "Error: Archive directory table corrupted\n");
            for (uint32_t j = 0; j < i; j++) free(files[j].path);
            free(files);
            fclose(archive);
            return -1;
        }
        files[i].path = safe_malloc(path_len + 1);
        if (fread(files[i].path, 1, path_len, archive) != path_len) {
            fprintf(stderr, "Error: Archive directory table corrupted\n");
            free(files[i].path);
            for (uint32_t j = 0; j < i; j++) free(files[j].path);
            free(files);
            fclose(archive);
            return -1;
        }
        files[i].path[path_len] = '\0';

        if (fread(&files[i].mode, sizeof(uint32_t), 1, archive) != 1 ||
            fread(&files[i].orig_size, sizeof(uint64_t), 1, archive) != 1 ||
            fread(&files[i].comp_size, sizeof(uint64_t), 1, archive) != 1 ||
            fread(&files[i].crc32, sizeof(uint32_t), 1, archive) != 1 ||
            fread(&files[i].data_offset, sizeof(uint64_t), 1, archive) != 1) {
            fprintf(stderr, "Error: Archive directory table corrupted\n");
            for (uint32_t j = 0; j <= i; j++) free(files[j].path);
            free(files);
            fclose(archive);
            return -1;
        }
    }

    printf("Extracting %u items...\n", footer.file_count);
    int thread_count = 4; // Extraction decompression concurrency

    for (uint32_t i = 0; i < footer.file_count; i++) {
        FileMetadata *m = &files[i];

        if (S_ISDIR(m->mode)) {
            printf("Creating directory %s... ", m->path);
            make_directories_recursive(m->path);
            chmod(m->path, m->mode);
            printf("\033[1;32m[OK]\033[0m\n");
        } else {
            printf("Extracting %s... ", m->path);
            fflush(stdout);

            create_parent_dirs(m->path);
            
            FILE *dest_file = fopen(m->path, "wb");
            if (!dest_file) {
                fprintf(stderr, "\033[1;31m[FAILED (open write error)]\033[0m\n");
                continue;
            }

            fseek(archive, m->data_offset, SEEK_SET);
            decompress_stream(archive, dest_file, thread_count, m->comp_size);
            fclose(dest_file);

            // Restore permission attributes
            chmod(m->path, m->mode);

            // Verify integrity via CRC32
            FILE *verify = fopen(m->path, "rb");
            if (verify) {
                uint32_t accum_crc = crc32(0L, Z_NULL, 0);
                unsigned char buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), verify)) > 0) {
                    accum_crc = calculate_crc32(accum_crc, buf, n);
                }
                fclose(verify);

                if (accum_crc == m->crc32) {
                    printf("\033[1;32m[OK]\033[0m\n");
                } else {
                    printf("\033[1;31m[CORRUPTED (CRC: expected %08X, got %08X)]\033[0m\n", m->crc32, accum_crc);
                }
            } else {
                printf("\033[1;33m[VERIFY SKIPPED]\033[0m\n");
            }
        }
    }

    for (uint32_t i = 0; i < footer.file_count; i++) {
        free(files[i].path);
    }
    free(files);
    fclose(archive);

    printf("\033[1;32mExtraction complete.\033[0m\n");
    return 0;
}

int list_archive(const char *archive_path) {
    FILE *archive = fopen(archive_path, "rb");
    if (!archive) {
        fprintf(stderr, "Error: Cannot open archive '%s' for listing\n", archive_path);
        return -1;
    }

    fseek(archive, 0, SEEK_END);
    long size = ftell(archive);
    if (size < 18) {
        fprintf(stderr, "Error: Invalid archive structure\n");
        fclose(archive);
        return -1;
    }

    fseek(archive, size - 18, SEEK_SET);

    ArchiveFooter footer;
    if (fread(&footer.table_offset, sizeof(uint64_t), 1, archive) != 1 ||
        fread(&footer.file_count, sizeof(uint32_t), 1, archive) != 1 ||
        fread(&footer.version, sizeof(uint16_t), 1, archive) != 1 ||
        fread(footer.magic, 1, 4, archive) != 4) {
        fprintf(stderr, "Error: Failed to read archive footer\n");
        fclose(archive);
        return -1;
    }

    if (memcmp(footer.magic, PZIP_MAGIC, 4) != 0) {
        fprintf(stderr, "Error: Archive header magic signature mismatch\n");
        fclose(archive);
        return -1;
    }

    fseek(archive, footer.table_offset, SEEK_SET);

    printf("\033[1;36m%-50s %-12s %-12s %-8s %-10s\033[0m\n", "File Path", "Orig Size", "Comp Size", "Ratio", "CRC32");
    printf("-------------------------------------------------------------------------------------------------\n");

    uint64_t total_orig = 0;
    uint64_t total_comp = 0;

    for (uint32_t i = 0; i < footer.file_count; i++) {
        uint16_t path_len = 0;
        if (fread(&path_len, sizeof(uint16_t), 1, archive) != 1) {
            fprintf(stderr, "Error: Failed to read directory table path length\n");
            fclose(archive);
            return -1;
        }
        
        char *path = safe_malloc(path_len + 1);
        if (fread(path, 1, path_len, archive) != path_len) {
            fprintf(stderr, "Error: Failed to read directory table path string\n");
            free(path);
            fclose(archive);
            return -1;
        }
        path[path_len] = '\0';

        uint32_t mode;
        uint64_t orig_size;
        uint64_t comp_size;
        uint32_t crc32;
        uint64_t data_offset;

        if (fread(&mode, sizeof(uint32_t), 1, archive) != 1 ||
            fread(&orig_size, sizeof(uint64_t), 1, archive) != 1 ||
            fread(&comp_size, sizeof(uint64_t), 1, archive) != 1 ||
            fread(&crc32, sizeof(uint32_t), 1, archive) != 1 ||
            fread(&data_offset, sizeof(uint64_t), 1, archive) != 1) {
            fprintf(stderr, "Error: Failed to read directory table metadata\n");
            free(path);
            fclose(archive);
            return -1;
        }

        double ratio = (double)orig_size / (comp_size > 0 ? comp_size : 1);
        char ratio_str[16];
        if (S_ISDIR(mode)) {
            strcpy(ratio_str, "DIR");
        } else if (orig_size == 0) {
            strcpy(ratio_str, "EMPTY");
        } else {
            snprintf(ratio_str, sizeof(ratio_str), "%.2fx", ratio);
        }

        char crc_str[16];
        if (S_ISDIR(mode)) {
            strcpy(crc_str, "-");
        } else {
            snprintf(crc_str, sizeof(crc_str), "%08X", crc32);
        }

        printf("%-50s %-12lu %-12lu %-8s %-10s\n", path, (unsigned long)orig_size, (unsigned long)comp_size, ratio_str, crc_str);

        total_orig += orig_size;
        total_comp += comp_size;

        free(path);
    }
    printf("-------------------------------------------------------------------------------------------------\n");
    double total_ratio = (double)total_orig / (total_comp > 0 ? total_comp : 1);
    printf("\033[1;32m%-50s %-12lu %-12lu %.2fx\033[0m\n", "TOTALS", (unsigned long)total_orig, (unsigned long)total_comp, total_ratio);

    fclose(archive);
    return 0;
}
