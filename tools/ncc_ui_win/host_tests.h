#ifndef NC_UI_HOST_TESTS_H
#define NC_UI_HOST_TESTS_H

/* The station's headless flags as one owner: the checks (`--*test`), the dumps
   (`--dump`, `--dump-bench`), the questions a script asks about the machine or
   the build (`--state`, `--version`). `main()` hands over its whole command
   line and takes the exit code back; -1 means no flag was named, so the station
   opens its window. */
int host_tests_run(int argc, char **argv);

#endif
