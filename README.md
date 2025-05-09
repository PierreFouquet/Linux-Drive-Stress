# Linux-Drive-Stress

To use, first clone this repo. Ensure you have Git installed first (or you can download the ZIP).

`git clone git@github.com:PierreFouquet/Linux-Drive-Stress.git`

Then navigate to the Linux-Drive-Stress folder.

`cd Linux-Drive-Stress/`

See below for each test.

## Single File Stress - drive_stress_linux

Compiled with - `gcc -Wall -Wextra -O2 -o drive_stress_linux drive_stress_linux.c -std=c99`

Usage:

`./drive_stress_linux` - Uses default 10MB file size and name.

`./drive_stress_linux 100` - Uses 100MB file size, default name.

`./drive_stress_linux 50` - /mnt/my_drive/test_file.bin # 50MB file on a specific drive/path.

## Multifile Stress - stress_test_multi

Compiled with - `gcc stress_test_multi.c -o stress_test_multi -lpthread`

`./stress_test_multi <file_size_in_mb> <number_of_files>` - the default values are 10MB and 2 files.

`./stress_test_multi 20 5` - 20MB file size, and 5 files.
