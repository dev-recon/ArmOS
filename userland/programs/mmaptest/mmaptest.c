/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/mmaptest/mmaptest.c
 * Layer: Userland / diagnostics
 * Description: Smoke test for anonymous private mmap/munmap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arm_os_abi.h>

#define COLOR_GREEN "\033[32m"
#define COLOR_RED   "\033[31m"
#define COLOR_RESET "\033[0m"

static struct sysinfo_response sysinfo_scratch;

static int read_self_rss_kb(unsigned *rss_kb)
{
    pid_t self = getpid();

    if (getsysinfo(&sysinfo_scratch) < 0)
        return -1;
    for (int i = 0; i < sysinfo_scratch.proc_count; i++) {
        if (sysinfo_scratch.procs[i].pid == self) {
            *rss_kb = sysinfo_scratch.procs[i].rss_kb;
            return 0;
        }
    }
    return -1;
}

static int fail(const char *msg)
{
    printf(COLOR_RED "[KO]" COLOR_RESET " %s errno=%d\n", msg, errno);
    return 1;
}

static void ok(const char *msg)
{
    printf(COLOR_GREEN "[OK]" COLOR_RESET " %s\n", msg);
}

static void *thread_mmap_main(void *opaque)
{
    unsigned char *mapping;

    (void)opaque;
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return (void *)1;
    mapping[0] = 0x5au;
    if (mapping[0] != 0x5au || munmap(mapping, 4096) < 0)
        return (void *)2;
    return NULL;
}

int main(void)
{
    char *p;
    char *q;
    char *sparse;
    unsigned rss_before;
    unsigned rss_after_map;
    unsigned rss_after_touch;
    int status = 0;
    void *thread_result = NULL;
    pthread_t thread;
    struct rlimit stack_limit;
    pid_t pid;

    printf("mmaptest: anonymous private mapping smoke test\n");
    if (getrlimit(RLIMIT_STACK, &stack_limit) < 0 ||
        stack_limit.rlim_cur != 8u * 1024u * 1024u ||
        stack_limit.rlim_max != 8u * 1024u * 1024u)
        return fail("RLIMIT_STACK is not 8 MiB");
    ok("RLIMIT_STACK reports 8 MiB");

    p = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return fail("mmap 8KB");
    ok("mmap 8KB");

    p[0] = 'A';
    p[4095] = 'B';
    p[4096] = 'C';
    p[8191] = 'D';
    if (p[0] != 'A' || p[4095] != 'B' || p[4096] != 'C' || p[8191] != 'D')
        return fail("read/write across pages");
    ok("read/write across pages");

    pid = fork();
    if (pid < 0)
        return fail("fork after mmap");
    if (pid == 0) {
        p[0] = 'Z';
        return (p[0] == 'Z' && p[4096] == 'C') ? 23 : 24;
    }

    if (waitpid(pid, &status, 0) != pid)
        return fail("wait child");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 23)
        return fail("child sees private mmap");
    if (p[0] != 'A')
        return fail("parent COW preserved");
    ok("fork/COW preserves parent page");

    if (munmap(p + 4096, 4096) < 0)
        return fail("partial munmap second page");
    if (p[0] != 'A' || p[4095] != 'B')
        return fail("first page remains mapped after partial munmap");
    ok("partial munmap keeps first page");

    q = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED)
        return fail("second mmap 4KB");
    strcpy(q, "hello from mmap");
    if (strcmp(q, "hello from mmap") != 0)
        return fail("second mmap read/write");
    ok("second mmap read/write");

    if (munmap(p, 4096) < 0)
        return fail("munmap first mapping");
    if (munmap(q, 4096) < 0)
        return fail("munmap second mapping");
    ok("munmap cleanup");

    if (read_self_rss_kb(&rss_before) < 0)
        return fail("sysinfo before sparse mmap");

    sparse = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sparse == MAP_FAILED)
        return fail("sparse mmap 1MB");

    if (read_self_rss_kb(&rss_after_map) < 0)
        return fail("sysinfo after sparse mmap");
    if (rss_after_map > rss_before + 16)
        return fail("sparse mmap should not allocate full RSS eagerly");
    ok("sparse mmap does not grow RSS eagerly");

    sparse[0] = 'L';
    sparse[(1024 * 1024) - 1] = 'Z';
    if (sparse[0] != 'L' || sparse[(1024 * 1024) - 1] != 'Z')
        return fail("sparse mmap touched pages");

    if (read_self_rss_kb(&rss_after_touch) < 0)
        return fail("sysinfo after sparse touch");
    if (rss_after_touch < rss_after_map + 8)
        return fail("sparse mmap did not fault in touched pages");
    ok("sparse mmap faults in touched pages only");

    if (munmap(sparse, 1024 * 1024) < 0)
        return fail("munmap sparse mapping");
    ok("munmap sparse mapping");

    if (pthread_create(&thread, NULL, thread_mmap_main, NULL) != 0)
        return fail("mmap thread create");
    if (pthread_join(thread, &thread_result) != 0 || thread_result != NULL)
        return fail("mmap from process thread");
    ok("mmap/munmap from process thread");

    printf("mmaptest: all tests passed\n");
    return 0;
}
