/*
 * Phase 1 test framework — console + sdmc logging main() for the .nro tests.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "testfw.h"

#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

static void t_vemit(test_ctx *t, const char *prefix, const char *fmt,
                    va_list ap)
{
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    printf("%s%s\n", prefix, line);
    if (t->log) {
        fprintf(t->log, "%s%s\n", prefix, line);
        fflush(t->log);
    }
}

bool t_check(test_ctx *t, bool cond, const char *fmt, ...)
{
    if (cond)
        t->pass++;
    else
        t->fail++;

    va_list ap;
    va_start(ap, fmt);
    t_vemit(t, cond ? "  ok   " : "  FAIL ", fmt, ap);
    va_end(ap);
    return cond;
}

void t_note(test_ctx *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    t_vemit(t, "  note ", fmt, ap);
    va_end(ap);
}

int main(void)
{
    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    test_ctx t = { .pass = 0, .fail = 0, .log = NULL };

    mkdir("sdmc:/horizon_gpu_tests", 0777);
    char path[128];
    snprintf(path, sizeof(path), "sdmc:/horizon_gpu_tests/%s.log", test_name);
    t.log = fopen(path, "w");

    printf("== %s ==\n", test_name);
    if (t.log)
        fprintf(t.log, "== %s ==\n", test_name);
    else
        printf("  note (sdmc log unavailable: %s)\n", path);

    int aborted = run_test(&t);

    int total = t.pass + t.fail;
    const char *verdict = (t.fail == 0 && !aborted) ? "PASS" : "FAIL";
    printf("RESULT: %s (%d/%d)%s\n", verdict, t.pass, total,
           aborted ? " [aborted early]" : "");
    if (t.log) {
        fprintf(t.log, "RESULT: %s (%d/%d)%s\n", verdict, t.pass, total,
                aborted ? " [aborted early]" : "");
        fclose(t.log);
    }

    printf("\nLog: %s\nPress + to exit.\n", path);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return (t.fail == 0 && !aborted) ? 0 : 1;
}
