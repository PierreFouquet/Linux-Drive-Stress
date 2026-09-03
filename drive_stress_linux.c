/*
 * drive_stress_linux.c - single-file drive stress test.
 *
 * Writes, re-reads and verifies one file using O_DIRECT, then hammers it with
 * random reads and writes. See drive_stress.c for the engine.
 */
#include <stdio.h>

#include "drive_stress.h"

#define DEFAULT_FILE_NAME "stress_test_file_linux.dat"

int main(int argc, char *argv[])
{
    DsConfig cfg;
    int rc;

    ds_config_defaults(&cfg);
    cfg.multi_mode = 0;
    cfg.num_files = 1;
    cfg.path = DEFAULT_FILE_NAME;

    rc = ds_parse_args(&cfg, argc, argv);
    if (rc != 0) {
        return rc < 0 ? 1 : 0;
    }
    return ds_run(&cfg);
}
