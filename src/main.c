#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "archive.h"

void print_help(const char *prog_name) {
    printf("ParaZip (pzip) - High-Performance Parallel Block-Compression & Archiving Tool\n\n");
    printf("Usage:\n");
    printf("  %s -c [-t threads] <archive.pzip> <source_paths...>\n", prog_name);
    printf("  %s -x <archive.pzip>\n", prog_name);
    printf("  %s -l <archive.pzip>\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -c            Create a new archive from source paths\n");
    printf("  -x            Extract files from an existing archive\n");
    printf("  -l            List details of files inside the archive\n");
    printf("  -t <threads>  Specify number of worker threads (default: system CPU count)\n");
    printf("  -h            Show this help menu\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s -c -t 8 my_backup.pzip src/ include/ README.md\n", prog_name);
    printf("  %s -x my_backup.pzip\n", prog_name);
}

int main(int argc, char **argv) {
    int opt;
    char mode = '\0';
    int thread_count = -1;

    // Parse command line options using POSIX getopt
    while ((opt = getopt(argc, argv, "cxlht:")) != -1) {
        switch (opt) {
            case 'c':
                mode = 'c';
                break;
            case 'x':
                mode = 'x';
                break;
            case 'l':
                mode = 'l';
                break;
            case 't':
                thread_count = atoi(optarg);
                break;
            case 'h':
                print_help(argv[0]);
                return 0;
            default:
                print_help(argv[0]);
                return 1;
        }
    }

    if (mode == '\0') {
        fprintf(stderr, "Error: You must specify an action flag (-c, -x, or -l)\n\n");
        print_help(argv[0]);
        return 1;
    }

    // Determine default worker thread count based on active CPU cores
    if (thread_count <= 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (cores > 0) {
            thread_count = (int)cores;
        } else {
            thread_count = 4; // Default fallback
        }
    }

    // Get index of positional arguments after options
    int pos_argc = argc - optind;
    char **pos_argv = argv + optind;

    if (mode == 'c') {
        if (pos_argc < 2) {
            fprintf(stderr, "Error: Create mode requires an archive output path and at least one source file/dir.\n");
            fprintf(stderr, "Usage: %s -c [-t threads] <archive.pzip> <source_paths...>\n", argv[0]);
            return 1;
        }
        const char *archive_path = pos_argv[0];
        const char **sources = (const char **)&pos_argv[1];
        int source_count = pos_argc - 1;

        return create_archive(archive_path, sources, source_count, thread_count);
    } else if (mode == 'x') {
        if (pos_argc < 1) {
            fprintf(stderr, "Error: Extract mode requires the path to the archive file.\n");
            fprintf(stderr, "Usage: %s -x <archive.pzip>\n", argv[0]);
            return 1;
        }
        const char *archive_path = pos_argv[0];
        return extract_archive(archive_path);
    } else if (mode == 'l') {
        if (pos_argc < 1) {
            fprintf(stderr, "Error: List mode requires the path to the archive file.\n");
            fprintf(stderr, "Usage: %s -l <archive.pzip>\n", argv[0]);
            return 1;
        }
        const char *archive_path = pos_argv[0];
        return list_archive(archive_path);
    }

    return 0;
}
