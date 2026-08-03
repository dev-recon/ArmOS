/* Portable utility layer for the FreeBSD top frontend on ArmOS. */
#include "armos_top_compat.h"

#include "top.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int atoiwi(const char *text)
{
    size_t length = strlen(text);
    if (length == 0)
        return 0;
    if (strncmp(text, "infinity", length) == 0 ||
        strncmp(text, "all", length) == 0 ||
        strncmp(text, "maximum", length) == 0)
        return Infinity;
    if (text[0] == '-')
        return Invalid;
    return (int)strtol(text, NULL, 10);
}

char *armos_top_itoa(unsigned int value)
{
    static char buffer[16];
    snprintf(buffer, sizeof(buffer), "%u", value);
    return buffer;
}

char *itoa7(int value)
{
    static char buffer[16];
    snprintf(buffer, sizeof(buffer), "%6d", value);
    return buffer;
}

int digits(int value)
{
    int count = 1;
    while (value >= 10) {
        value /= 10;
        count++;
    }
    return count;
}

const char **argparse(char *line, int *count)
{
    static const char *args[1024];
    const char **next = &args[1];
    char *word;

    *count = 1;
    while ((word = strsep(&line, " \t")) != NULL) {
        if (*word == '\0')
            continue;
        if (*count >= (int)nitems(args) - 1)
            break;
        *next++ = word;
        (*count)++;
    }
    *next = NULL;
    return args;
}

long percentages(int count, int *output, long *current, long *previous,
    long *difference)
{
    long total = 0;
    int index;

    for (index = 0; index < count; index++) {
        long change = current[index] - previous[index];
        if (change < 0)
            change = 0;
        difference[index] = change;
        previous[index] = current[index];
        total += change;
    }
    if (total == 0)
        total = 1;
    for (index = 0; index < count; index++)
        output[index] = (int)((difference[index] * 1000 + total / 2) / total);
    return total;
}

const char *format_time(long seconds)
{
    static char buffer[16];
    if (seconds < 0)
        seconds = 0;
    if (seconds < 60000)
        snprintf(buffer, sizeof(buffer), "%3ld:%02ld", seconds / 60,
            seconds % 60);
    else
        snprintf(buffer, sizeof(buffer), "%5.1fH", (double)seconds / 3600.0);
    return buffer;
}

char *format_k(int64_t kilobytes)
{
    static char buffers[8][16];
    static unsigned int slot;
    char *buffer = buffers[slot++ % nitems(buffers)];

    if (kilobytes >= 1024 * 1024)
        snprintf(buffer, 16, "%5lldG", (long long)(kilobytes / (1024 * 1024)));
    else if (kilobytes >= 1024)
        snprintf(buffer, 16, "%5lldM", (long long)(kilobytes / 1024));
    else
        snprintf(buffer, 16, "%5lldK", (long long)kilobytes);
    return buffer;
}

int find_pid(pid_t pid)
{
    char path[48];
    FILE *file;
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    file = fopen(path, "r");
    if (file == NULL)
        return 0;
    fclose(file);
    return 1;
}
