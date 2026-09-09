/*
 * memscan — list every region of this process's address space that
 * carries a memory attribute, over nxlink.
 *
 * WHY THIS EXISTS. nx-hbloader runs one .nro after another in the SAME
 * process: the heap it hands each of them is the same range, and a fresh
 * newlib malloc starts again from its beginning. Anything the previous
 * program left on those pages that is not data survives — in particular
 * svcSetMemoryAttribute(MemAttr_IsUncached), which horizon_gpu applies to
 * every HORIZON_GPU_MEM_UNCACHED object and removes only when the object
 * is destroyed. A program that exits with such objects alive leaves
 * uncached pages in the middle of the next program's heap, and the next
 * program finds out inside whatever memset happens to land on them.
 *
 * So this runs right after such a program, from the same hbmenu, and
 * prints what it left behind. It links libnx and nothing else, allocates
 * nothing on the heap before scanning, and changes nothing: it is a
 * measurement.
 *
 *   nxlink -s -a <ip> memscan.nro          # regions with attr != 0
 *   nxlink -s -a <ip> memscan.nro all      # every region
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <switch.h>

/* MemoryAttribute bits, libnx switch/kernel/svc.h:
 * 1 IsBorrowed, 2 IsIpcMapped, 4 IsDeviceMapped, 8 IsUncached. */
static void attr_str(u32 attr, char out[48])
{
    out[0] = '\0';
    if (attr & MemAttr_IsBorrowed)
        strcat(out, "borrowed ");
    if (attr & MemAttr_IsIpcMapped)
        strcat(out, "ipc ");
    if (attr & MemAttr_IsDeviceMapped)
        strcat(out, "device ");
    if (attr & MemAttr_IsUncached)
        strcat(out, "UNCACHED ");
    if (attr & ~(u32)(MemAttr_IsBorrowed | MemAttr_IsIpcMapped |
                      MemAttr_IsDeviceMapped | MemAttr_IsUncached))
        strcat(out, "other ");
}

static void scan(bool all)
{
    u64 addr = 0;
    u64 n_regions = 0, n_attr = 0;
    u64 bytes_uncached = 0, bytes_device = 0;

    printf("===== memscan (%s) =====\n", all ? "all" : "attr != 0");
    printf("%-18s %-12s %-6s %-5s %-5s %s\n",
           "addr", "size", "type", "attr", "perm", "flags");
    for (;;) {
        MemoryInfo mi;
        u32 page_info;
        Result rc = svcQueryMemory(&mi, &page_info, addr);
        if (R_FAILED(rc)) {
            printf("svcQueryMemory(0x%" PRIx64 ") failed: 0x%08x\n",
                   addr, rc);
            break;
        }
        n_regions++;
        const u32 type = mi.type & 0xff;
        if (mi.attr != 0) {
            n_attr++;
            if (mi.attr & MemAttr_IsUncached)
                bytes_uncached += mi.size;
            if (mi.attr & MemAttr_IsDeviceMapped)
                bytes_device += mi.size;
        }
        if (all || mi.attr != 0) {
            char a[48];
            attr_str(mi.attr, a);
            printf("0x%016" PRIx64 " 0x%010" PRIx64 " 0x%02x   0x%02x  0x%02x  "
                   "%s\n", mi.addr, mi.size, type, mi.attr, mi.perm, a);
        }
        /* The last region ends at the top of the address space; the
         * addition wraps to 0 there and the loop must stop, not restart. */
        const u64 next = mi.addr + mi.size;
        if (next <= addr || next == 0)
            break;
        addr = next;
    }
    printf("===== END memscan: %" PRIu64 " regions, %" PRIu64 " with "
           "attributes, UNCACHED bytes 0x%" PRIx64 ", device-mapped bytes "
           "0x%" PRIx64 " =====\n", n_regions, n_attr, bytes_uncached,
           bytes_device);
    fflush(stdout);
}

int main(int argc, char **argv)
{
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

    scan(argc >= 2 && strcmp(argv[1], "all") == 0);

    /* Same drain as tools/logcat/logcat.c: shutdown(SHUT_WR) so the FIN
     * follows the queued data, then a moment for TCP before the socket
     * driver goes away. */
    if (streaming) {
        shutdown(sock, SHUT_WR);
        svcSleepThread(UINT64_C(1000000000));
        socketExit();
        consoleExit(NULL);
        return 0;
    }

    printf("Press + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
