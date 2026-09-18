/* nc_send - desktop NC program tool and Grbl/uCNC sender.

   Expands a lathe NC program (G71/G72 cycles) into plain controller code and,
   optionally, streams it to a controller over a serial port.

   Usage:
     nc_send --expand out.nc in.nc        write the expanded program
     nc_send --check in.nc                parse and expand, report problems
     nc_send --port COM5 --target grbl in.nc
   */

#include "nc_sender.h"
#include "nc_send_job.h"
#include "grbl_port_win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    puts("usage: nc_send [options] <program.nc>\n"
         "  --port <device>    stream to a controller (for example COM5)\n"
         "  --baud <rate>      serial rate, default 115200\n"
         "  --target <name>    grbl (default) or ucnc\n"
         "  --expand <file>    write the expanded program\n"
         "  --comments         keep comments in expanded output\n"
         "  --check            expand only, print what would be sent\n"
         "  --quiet            do not print expanded lines\n");
}

static const char *target_name(const nc_sender_target_t *target)
{
    return target->supports_lathe_words ? "ucnc" : "grbl";
}

static int expand_document(const nc_document_t *doc,
                           const nc_sender_target_t *target,
                           FILE *out,
                           bool print_lines,
                           unsigned *sent_lines)
{
    nc_sender_t sender;
    char line[NC_MAX_LINE_LEN];
    unsigned sent = 0u;

    nc_sender_begin(&sender, doc, target);
    for (;;) {
        size_t source_line = 0u;
        nc_sender_result_t result = nc_sender_next(&sender, line, sizeof(line),
                                                   &source_line);
        if (result == NC_SENDER_DONE)
            break;
        if (result == NC_SENDER_ERROR) {
            fprintf(stderr, "nc_send: line %lu: %s\n",
                    (unsigned long)(source_line + 1u),
                    g7x_result_text(nc_sender_last_error(&sender)));
            return 2;
        }
        if (!nc_sender_line_sendable(line))
            continue;
        if (out)
            fprintf(out, "%s\n", line);
        if (print_lines)
            printf("%s\n", line);
        sent++;
    }
    if (sent_lines)
        *sent_lines = sent;
    return 0;
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *expand_path = NULL;
    const char *port_name = NULL;
    unsigned baud = 115200u;
    bool check = false;
    bool quiet = false;
    bool comments = false;
    nc_sender_target_t target = nc_sender_grbl_target();
    nc_document_t doc;
    nc_result_t load_result;
    int i;
    int status;
    unsigned sent_lines = 0u;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--port") == 0 && i + 1 < argc)
            port_name = argv[++i];
        else if (strcmp(arg, "--baud") == 0 && i + 1 < argc)
            baud = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(arg, "--expand") == 0 && i + 1 < argc)
            expand_path = argv[++i];
        else if (strcmp(arg, "--target") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            if (strcmp(name, "ucnc") == 0 || strcmp(name, "uCNC") == 0)
                target = nc_sender_ucnc_target();
            else if (strcmp(name, "grbl") != 0) {
                fprintf(stderr, "nc_send: unknown target '%s'\n", name);
                return 1;
            }
        } else if (strcmp(arg, "--check") == 0)
            check = true;
        else if (strcmp(arg, "--quiet") == 0)
            quiet = true;
        else if (strcmp(arg, "--comments") == 0)
            comments = true;
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage();
            return 0;
        } else if (arg[0] == '-') {
            fprintf(stderr, "nc_send: unknown option '%s'\n", arg);
            usage();
            return 1;
        } else if (!input) {
            input = arg;
        } else {
            fprintf(stderr, "nc_send: only one program at a time\n");
            return 1;
        }
    }

    if (!input) {
        usage();
        return 1;
    }
    target.keep_comments = comments;

    load_result = nc_sender_load_file(&doc, input);
    if (load_result != NC_OK) {
        fprintf(stderr, "nc_send: cannot load %s (%s)\n", input,
                nc_result_text(load_result));
        return 1;
    }
    printf("nc_send: %s, %lu lines, target %s\n", input,
           (unsigned long)doc.line_count, target_name(&target));

    if (expand_path) {
        FILE *out = fopen(expand_path, "w");
        if (!out) {
            fprintf(stderr, "nc_send: cannot write %s\n", expand_path);
            return 1;
        }
        status = expand_document(&doc, &target, out, !quiet && !check,
                                 &sent_lines);
        fclose(out);
        if (status != 0)
            return status;
        printf("nc_send: wrote %u lines to %s\n", sent_lines, expand_path);
    } else {
        status = expand_document(&doc, &target, NULL, !quiet && !check,
                                 &sent_lines);
        if (status != 0)
            return status;
        printf("nc_send: %u lines ready\n", sent_lines);
    }

    if (check && !port_name)
        return 0;

    if (port_name) {
        grbl_port_win32_t win32_port;
        const grbl_port_t *port = grbl_port_win32(&win32_port);
        nc_send_job_t job;

        if (!port) {
            fprintf(stderr, "nc_send: serial ports are only available on Windows\n");
            return 1;
        }
        if (!nc_send_job_start(&job, &doc, &target, port, &win32_port,
                               port_name, baud)) {
            fprintf(stderr, "nc_send: %s\n", nc_send_job_status(&job));
            return 1;
        }
        printf("nc_send: sending to %s at %u baud\n", port_name, baud);
        while (nc_send_job_active(&job)) {
            nc_send_job_pump(&job);
            if (nc_send_job_active(&job))
                printf("\r  %-52s", nc_send_job_status(&job));
        }
        printf("\r  %-52s\n", nc_send_job_status(&job));
        status = job.finished ? 0 : 2;
        nc_send_job_close(&job);
        return status;
    }

    return 0;
}
