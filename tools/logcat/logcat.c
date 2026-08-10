/*
 * logcat — print a file from the console's SD card over nxlink.
 *
 * WHY THIS EXISTS. Every test in tests/ writes its record to
 * sdmc:/horizon_gpu_tests/<name>.log, and the tests that matter most in
 * Phase 6 set `test_uses_display`, which means no console: the log file
 * IS the whole record and nothing of it reaches a screen. testfw used to
 * stream itself over nxlink and that was removed at the user's direction
 * (STATUS.md, 2026-08-08) — the socket driver was the one variable that
 * correlated with run 14's MMU fault, and streaming per line also puts
 * network I/O inside the very loops whose pacing the swapchain tests
 * measure.
 *
 * So the log stays on the SD card, and this reads it back afterwards.
 * A separate process, launched separately, after the measured run has
 * ended: the .nro under test never links a socket, and nothing about
 * this program can perturb a measurement that is already on disk.
 *
 * It links libnx and nothing else — no horizon_gpu, no Mesa, no testfw.
 *
 *   nxlink -s -a <ip> logcat.nro                    # list the directory
 *   nxlink -s -a <ip> logcat.nro t_vk_immediate     # one log, by stem
 *   nxlink -s -a <ip> logcat.nro sdmc:/some/file    # any path
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <switch.h>

/* Where testfw.c puts every log (tests/common/testfw.c, main). */
#define LOGCAT_DIR "sdmc:/horizon_gpu_tests"

/* One read at a time. Small on purpose: this runs in applet mode, where
 * the heap is about 400 MiB and a log is a few tens of kilobytes. */
#define LOGCAT_CHUNK 4096

/* A bare stem — "t_vk_immediate" — is resolved against LOGCAT_DIR and
 * given the .log suffix. Anything containing ':' is taken as a whole
 * path, because that is what a Horizon device-qualified path looks like
 * and there is no other way to name a file outside the log directory.
 */
static void logcat_resolve(const char *arg, char *out, size_t out_size)
{
    if (strchr(arg, ':') != NULL) {
        snprintf(out, out_size, "%s", arg);
        return;
    }
    if (strstr(arg, ".log") != NULL)
        snprintf(out, out_size, "%s/%s", LOGCAT_DIR, arg);
    else
        snprintf(out, out_size, "%s/%s.log", LOGCAT_DIR, arg);
}

/* Returns false only when the file could not be opened or read, which
 * is reported by the caller as the finding it is: a run whose log is
 * missing is not a run whose log is empty. */
static bool logcat_dump(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        printf("logcat: cannot open %s\n", path);
        return false;
    }

    printf("===== BEGIN %s =====\n", path);

    char buf[LOGCAT_CHUNK];
    size_t total = 0;
    for (;;) {
        const size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0)
            break;
        fwrite(buf, 1, n, stdout);
        total += n;
    }

    const bool ok = ferror(f) == 0;
    fclose(f);

    /* The newline is unconditional: a log whose last line has no
     * terminator would otherwise run into the marker below and the
     * marker is what a reader greps for. */
    printf("\n===== END %s (%zu bytes%s) =====\n", path, total,
           ok ? "" : ", READ ERROR");
    fflush(stdout);
    return ok;
}

static void logcat_list(void)
{
    DIR *d = opendir(LOGCAT_DIR);
    if (d == NULL) {
        printf("logcat: cannot open %s\n", LOGCAT_DIR);
        return;
    }

    printf("===== %s =====\n", LOGCAT_DIR);
    for (;;) {
        const struct dirent *e = readdir(d);
        if (e == NULL)
            break;
        if (e->d_name[0] == '.')
            continue;

        char path[320];
        snprintf(path, sizeof(path), "%s/%s", LOGCAT_DIR, e->d_name);

        struct stat st;
        if (stat(path, &st) == 0)
            printf("%10lld  %s\n", (long long)st.st_size, e->d_name);
        else
            printf("         ?  %s\n", e->d_name);
    }
    closedir(d);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    /* The console first, so a run launched from hbmenu without nxlink
     * still shows something rather than writing into a closed
     * descriptor. nxlinkStdio() dup2s over stdout afterwards when there
     * is a host, which is the normal case here. */
    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    int sock = -1;
    bool streaming = false;
    if (R_SUCCEEDED(socketInitializeDefault())) {
        sock = nxlinkStdio();
        streaming = sock >= 0;
        if (!streaming)
            socketExit();
    }

    bool ok = true;
    if (argc >= 2) {
        for (int i = 1; i < argc; i++) {
            char path[320];
            logcat_resolve(argv[i], path, sizeof(path));
            if (!logcat_dump(path))
                ok = false;
        }
    } else {
        logcat_list();
    }

    printf("logcat: %s\n", ok ? "done" : "done, with errors above");
    fflush(stdout);

    /* WHEN STREAMING, CLOSE THE WRITE SIDE AND LET IT DRAIN BEFORE
     * EXITING, and that is not politeness — it is the difference between
     * a whole log and most of one.
     *
     * fflush() only hands the bytes to the socket. socketExit() tears
     * the whole bsdsockets client down and the process then exits, and
     * whatever TCP had not yet put on the wire goes with it. Measured
     * twice on hardware before this was here: a 30 KB log arrived cut
     * off mid-line, at a different line each time, with the trailing
     * "===== END" marker missing — which reads exactly like a homebrew
     * that hung at the point the text stops, and cost a run being
     * diagnosed as a driver hang that had not happened.
     *
     * shutdown(SHUT_WR) sends the FIN after the queued data, so the
     * peer sees the whole stream and then EOF; the sleep gives the
     * retransmit path a moment before the driver goes away. The END
     * marker above is what a reader checks: if it is there, nothing was
     * lost.
     *
     * Exiting rather than waiting for a button is deliberate: nxlink's
     * log server ends when this side closes, so waiting would leave the
     * operator's terminal hanging on a program that has already said
     * everything it has to say. Without a host there is nobody but the
     * screen to read it, so then it waits. */
    if (streaming) {
        shutdown(sock, SHUT_WR);
        svcSleepThread(UINT64_C(1000000000));
        socketExit();
        consoleExit(NULL);
        return ok ? 0 : 1;
    }

    printf("Press + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return ok ? 0 : 1;
}
