/*
 * stress_test_multi.c - multi-file, multi-threaded drive stress test.
 *
 * Each worker thread owns one file and issues synchronous O_DIRECT I/O, so the
 * file count is also the approximate device queue depth. See drive_stress.c
 * for the engine.
 */
#include <stdio.h>

#include "drive_stress.h"

#define DEFAULT_FILE_NAME_PREFIX "stress_test_file_"
#define DEFAULT_NUM_FILES 2

int main(int argc, char *argv[])
{
    DsConfig cfg;
    int rc;

    ds_config_defaults(&cfg);
    cfg.multi_mode = 1;
    cfg.num_files = DEFAULT_NUM_FILES;
    cfg.path = DEFAULT_FILE_NAME_PREFIX;

    rc = ds_parse_args(&cfg, argc, argv);
    if (rc != 0) {
        return rc < 0 ? 1 : 0;
    }
    return ds_run(&cfg);
}
