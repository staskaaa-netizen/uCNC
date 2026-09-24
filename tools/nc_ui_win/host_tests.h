#ifndef NC_UI_HOST_TESTS_H
#define NC_UI_HOST_TESTS_H

/* The station's headless checks - the `--*test` and `--dump*` flags - as one
   owner. `main()` hands over its whole command line and takes the exit code
   back; -1 means no flag named a check, so the station opens its window. */
int host_tests_run(int argc, char **argv);

#endif
