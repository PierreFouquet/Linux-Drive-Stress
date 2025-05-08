#define _POSIX_C_SOURCE 200809L // Define this at the VERY TOP for fileno and nanosleep
// Or you could use #define _GNU_SOURCE for a wider set of features if needed

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>   // For nanosleep, time, srand
#include <errno.h>  // For errno and strerror
#include <unistd.h> // For fsync (fileno is in stdio.h but POSIX_C_SOURCE helps)
#include <sys/stat.h> // For file size check (optional, but good practice)

#define DEFAULT_FILE_SIZE_MB 10
#define DEFAULT_FILE_NAME "stress_test_file_linux.dat"
#define BUFFER_SIZE (1024 * 1024) // 1 MB buffer for read/write

// Function to make a delay (wrapper for nanosleep)
void precise_sleep(long seconds, long nanoseconds) {
    struct timespec req, rem;
    req.tv_sec = seconds;
    req.tv_nsec = nanoseconds;
    while (nanosleep(&req, &rem) == -1) {
        if (errno == EINTR) { // Interrupted by a signal, sleep for the remaining time
            req = rem;
        } else { // Some other error
            perror("nanosleep");
            break;
        }
    }
}

// Function to generate random data
void generate_random_data(char *buffer, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = rand() % 256; // Generate random bytes
    }
}

// Function to ensure data is written to disk on Linux
void sync_file_to_disk(FILE *fp, const char *filename) {
    if (fp == NULL) return;

    if (fflush(fp) != 0) {
        fprintf(stderr, "Error flushing C library buffer for %s: %s\n", filename, strerror(errno));
    }

    int fd = fileno(fp); // Should be fine with _POSIX_C_SOURCE defined
    if (fd == -1) {
        fprintf(stderr, "Error getting file descriptor for %s (fileno): %s\n", filename, strerror(errno));
        return;
    }

    if (fsync(fd) == -1) {
        fprintf(stderr, "Error syncing file to disk for %s (fsync): %s\n", filename, strerror(errno));
    }
}

// Function to verify written data by re-reading and comparing
int verify_data(const char *filename, long long expected_size, unsigned int seed_for_iteration) {
    FILE *fp_verify = fopen(filename, "rb");
    if (fp_verify == NULL) {
        fprintf(stderr, "  VERIFY: Error opening file %s for verification: %s\n", filename, strerror(errno));
        return 0; // Failure
    }

    char *original_chunk = (char *)malloc(BUFFER_SIZE);
    char *read_chunk = (char *)malloc(BUFFER_SIZE);
    if (!original_chunk || !read_chunk) {
        perror("  VERIFY: Failed to allocate memory for verification buffers");
        if (original_chunk) free(original_chunk);
        if (read_chunk) free(read_chunk);
        fclose(fp_verify);
        return 0; // Failure
    }

    srand(seed_for_iteration);

    long long bytes_verified = 0;
    int success = 1;
    for (bytes_verified = 0; bytes_verified < expected_size; bytes_verified += BUFFER_SIZE) {
        size_t current_chunk_size = (expected_size - bytes_verified < BUFFER_SIZE) ?
                                   (size_t)(expected_size - bytes_verified) : BUFFER_SIZE;

        generate_random_data(original_chunk, current_chunk_size);

        size_t num_read = fread(read_chunk, 1, current_chunk_size, fp_verify);
        if (num_read != current_chunk_size) {
            if (feof(fp_verify)) {
                fprintf(stderr, "  VERIFY: Premature end of file for %s. Expected %lld, got %lld + %zu\n",
                        filename, expected_size, bytes_verified, num_read);
            } else if (ferror(fp_verify)) {
                fprintf(stderr, "  VERIFY: Error reading file %s: %s\n", filename, strerror(errno));
            } else {
                 fprintf(stderr, "  VERIFY: Short read from %s. Expected %zu, got %zu\n",
                        filename, current_chunk_size, num_read);
            }
            success = 0;
            break;
        }

        if (memcmp(original_chunk, read_chunk, current_chunk_size) != 0) {
            fprintf(stderr, "  VERIFY: Data mismatch in %s at offset %lld!\n", filename, bytes_verified);
            success = 0;
            break;
        }
    }

    fclose(fp_verify);
    free(original_chunk);
    free(read_chunk);

    if (success && bytes_verified == expected_size) {
        printf("  Verification successful for %s.\n", filename);
    } else if (success && bytes_verified < expected_size) {
         fprintf(stderr, "  VERIFY: Verification incomplete for %s. Only verified %lld of %lld bytes.\n",
                filename, bytes_verified, expected_size);
        success = 0;
    }
    return success;
}


int main(int argc, char *argv[]) {
    long long fileSize = (long long)DEFAULT_FILE_SIZE_MB * 1024 * 1024;
    const char *fileName = DEFAULT_FILE_NAME;
    unsigned long long iterations = 0;
    char *dataBuffer = NULL;

    if (argc > 1) {
        long input_mb = atol(argv[1]);
        if (input_mb > 0) {
            fileSize = input_mb * 1024 * 1024;
        } else {
            fprintf(stderr, "Invalid file size provided, using default: %d MB\n", DEFAULT_FILE_SIZE_MB);
        }
    }
    if (argc > 2) {
        fileName = argv[2];
    }

    printf("Starting hard drive stress test on Linux...\n");
    printf("Target file: %s\n", fileName);
    printf("File size per iteration: %.2f MB (%lld bytes)\n", (double)fileSize / (1024 * 1024), fileSize);
    printf("Buffer size: %.2f MB (%d bytes)\n", (double)BUFFER_SIZE / (1024*1024), BUFFER_SIZE);
    printf("Press Ctrl+C to stop.\n\n");

    dataBuffer = (char *)malloc(BUFFER_SIZE);
    if (dataBuffer == NULL) {
        perror("Failed to allocate memory for data buffer");
        return EXIT_FAILURE;
    }

    unsigned int current_seed;

    while (1) {
        iterations++;
        current_seed = (unsigned int)time(NULL) + (unsigned int)iterations;
        srand(current_seed);

        printf("Iteration: %llu (Seed: %u)\n", iterations, current_seed);

        printf("  Writing file...\n");
        FILE *fp_write = fopen(fileName, "wb");
        if (fp_write == NULL) {
            fprintf(stderr, "  Error opening %s for writing: %s\n", fileName, strerror(errno));
            precise_sleep(1, 0); // Wait 1 second before retrying
            continue;
        }

        long long bytesWritten = 0;
        int error_occurred = 0;
        for (bytesWritten = 0; bytesWritten < fileSize; bytesWritten += BUFFER_SIZE) {
            size_t currentChunkSize = (fileSize - bytesWritten < BUFFER_SIZE) ?
                                      (size_t)(fileSize - bytesWritten) : BUFFER_SIZE;
            generate_random_data(dataBuffer, currentChunkSize);
            if (fwrite(dataBuffer, 1, currentChunkSize, fp_write) != currentChunkSize) {
                fprintf(stderr, "  Error writing data to %s: %s\n", fileName, strerror(errno));
                error_occurred = 1;
                break;
            }
        }

        if (!error_occurred) {
            printf("  Write phase complete. Bytes targeted: %lld\n", fileSize);
        }
        sync_file_to_disk(fp_write, fileName);
        if (fclose(fp_write) != 0) {
            fprintf(stderr, "  Error closing %s after writing: %s\n", fileName, strerror(errno));
        }
        if (error_occurred) {
             printf("  Skipping to next iteration due to write error.\n");
             printf("-------------------------------------\n");
             precise_sleep(1, 0); // Wait 1 second
             continue;
        }

        printf("  Verifying file contents...\n");
        if (!verify_data(fileName, fileSize, current_seed)) {
            fprintf(stderr, "  CRITICAL: Data verification FAILED for %s on iteration %llu.\n", fileName, iterations);
        }

        printf("  Deleting file %s...\n", fileName);
        if (remove(fileName) == 0) {
            printf("  File deleted successfully.\n");
        } else {
            fprintf(stderr, "  Error deleting %s: %s\n", fileName, strerror(errno));
        }
        printf("-------------------------------------\n");

        // Optional: Add a small delay between iterations if desired
        // precise_sleep(0, 100000000L); // 100ms delay (0 seconds, 100,000,000 nanoseconds)
    }

    free(dataBuffer);
    printf("Stress test finished (interrupted).\n");
    return EXIT_SUCCESS;
}