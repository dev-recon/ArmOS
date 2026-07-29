/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/lib/string.c
 * Layer: Kernel / support library
 *
 * Responsibilities:
 * - Provide freestanding helpers unavailable from libc.
 * - Keep formatting, string, math, and debug helpers deterministic.
 *
 * Notes:
 * - Must remain safe before userland and full runtime services exist.
 */

#include <kernel/string.h>
#include <kernel/memory.h>
#include <kernel/stdarg.h>
#include <kernel/kprintf.h>

/*
 * GCC's may_alias keeps the word-at-a-time fast paths valid when the caller's
 * storage has a different declared type.  Raspberry Pi framebuffer updates
 * move several hundred KiB per frame, so byte-at-a-time copies are far too
 * expensive even in a diagnostic -O0 kernel build.
 */
typedef uintptr_t string_word_t __attribute__((__may_alias__));

void* memset(void* dest, int val, size_t len)
{
    unsigned char* d = (unsigned char*)dest;
    unsigned char byte = (unsigned char)val;
    string_word_t word = byte;

    while (len != 0u &&
           ((uintptr_t)d & (sizeof(string_word_t) - 1u)) != 0u) {
        *d++ = byte;
        len--;
    }
    for (size_t shift = 8u;
         shift < sizeof(string_word_t) * 8u; shift <<= 1u)
        word |= word << shift;
    while (len >= sizeof(string_word_t) * 4u) {
        string_word_t *words = (string_word_t *)(void *)d;

        words[0] = word;
        words[1] = word;
        words[2] = word;
        words[3] = word;
        d += sizeof(string_word_t) * 4u;
        len -= sizeof(string_word_t) * 4u;
    }
    while (len >= sizeof(string_word_t)) {
        *(string_word_t *)(void *)d = word;
        d += sizeof(string_word_t);
        len -= sizeof(string_word_t);
    }
    while (len-- != 0u)
        *d++ = byte;

    return dest;
}

void* memcpy(void* dest, const void* src, size_t len)
{
    const unsigned char* s = (const unsigned char*)src;
    unsigned char* d = (unsigned char*)dest;

    if ((((uintptr_t)d ^ (uintptr_t)s) &
         (sizeof(string_word_t) - 1u)) == 0u) {
        while (len != 0u &&
               ((uintptr_t)d & (sizeof(string_word_t) - 1u)) != 0u) {
            *d++ = *s++;
            len--;
        }
        while (len >= sizeof(string_word_t) * 4u) {
            string_word_t *destination =
                (string_word_t *)(void *)d;
            const string_word_t *source =
                (const string_word_t *)(const void *)s;

            destination[0] = source[0];
            destination[1] = source[1];
            destination[2] = source[2];
            destination[3] = source[3];
            d += sizeof(string_word_t) * 4u;
            s += sizeof(string_word_t) * 4u;
            len -= sizeof(string_word_t) * 4u;
        }
        while (len >= sizeof(string_word_t)) {
            *(string_word_t *)(void *)d =
                *(const string_word_t *)(const void *)s;
            d += sizeof(string_word_t);
            s += sizeof(string_word_t);
            len -= sizeof(string_word_t);
        }
    }
    while (len-- != 0u)
        *d++ = *s++;

    return dest;
}

void* memmove(void* dest, const void* src, size_t len)
{
    const unsigned char* s = (const unsigned char*)src;
    unsigned char* d = (unsigned char*)dest;

    if (d == s || len == 0)
        return dest;

    if ((uintptr_t)d < (uintptr_t)s) {
        if ((((uintptr_t)d ^ (uintptr_t)s) &
             (sizeof(string_word_t) - 1u)) == 0u) {
            while (len != 0u &&
                   ((uintptr_t)d & (sizeof(string_word_t) - 1u)) != 0u) {
                *d++ = *s++;
                len--;
            }
            while (len >= sizeof(string_word_t)) {
                *(string_word_t *)(void *)d =
                    *(const string_word_t *)(const void *)s;
                d += sizeof(string_word_t);
                s += sizeof(string_word_t);
                len -= sizeof(string_word_t);
            }
        }
        while (len-- != 0u)
            *d++ = *s++;
    } else {
        d += len;
        s += len;
        while (len != 0u &&
               (((uintptr_t)d | (uintptr_t)s) &
                (sizeof(string_word_t) - 1u)) != 0u) {
            *--d = *--s;
            len--;
        }
        while (len >= sizeof(string_word_t)) {
            d -= sizeof(string_word_t);
            s -= sizeof(string_word_t);
            *(string_word_t *)(void *)d =
                *(const string_word_t *)(const void *)s;
            len -= sizeof(string_word_t);
        }
        while (len-- != 0u)
            *--d = *--s;
    }

    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n)
{
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    size_t i; /* Declaration GNU89 */
    
    for (i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    
    return 0;
}

size_t strlen(const char* str)
{
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

int strcmp(const char* s1, const char* s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n)
{
    size_t i; /* Declaration GNU89 */
    
    for (i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == 0) {
            return s1[i] - s2[i];
        }
    }
    return 0;
}

char* strcpy(char* dest, const char* src)
{
    char* d = dest;
    while ((*dest++ = *src++));
    return d;
}

char* strncpy(char* dest, const char* src, size_t n)
{
    size_t i; /* Declaration GNU89 */
    
    for (i = 0; i < n && src[i] != 0; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = 0;
    }
    return dest;
}

char* strdup(const char* str)
{
    size_t len = strlen(str) + 1;
    char* dup = kmalloc(len);
    if (dup) {
        memcpy(dup, str, len);
    }
    return dup;
}

char* strchr(const char* s, int c)
{
    while (*s) {
        if (*s == c) {
            return (char*)s;
        }
        s++;
    }
    return (c == '\0') ? (char*)s : NULL;
}

char* strtok(char* str, const char* delim)
{
    static char* last_token = NULL;
    char* token_start;
    
    if (str != NULL) {
        last_token = str;
    } else {
        str = last_token;
    }
    
    if (str == NULL) {
        return NULL;
    }
    
    /* Skip leading delimiters */
    while (*str && strchr(delim, *str)) {
        str++;
    }
    
    if (*str == '\0') {
        last_token = NULL;
        return NULL;
    }
    
    token_start = str;
    
    /* Find end of token */
    while (*str && !strchr(delim, *str)) {
        str++;
    }
    
    if (*str) {
        *str = '\0';
        last_token = str + 1;
    } else {
        last_token = NULL;
    }
    
    return token_start;
}

char* strtok_r(char* str, const char* delim, char** saveptr)
{
    char* token_start;

    if (!saveptr) {
        return NULL;
    }

    if (str != NULL) {
        *saveptr = str;
    } else {
        str = *saveptr;
    }

    if (str == NULL) {
        return NULL;
    }

    while (*str && strchr(delim, *str)) {
        str++;
    }

    if (*str == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    token_start = str;

    while (*str && !strchr(delim, *str)) {
        str++;
    }

    if (*str) {
        *str = '\0';
        *saveptr = str + 1;
    } else {
        *saveptr = NULL;
    }

    return token_start;
}

char tolower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

char toupper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return c - ('a' - 'A');
    }
    return c;
}

int isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

char* strstr(const char* haystack, const char* needle)
{
    size_t needle_len = strlen(needle);
    size_t i, j;
    
    if (needle_len == 0) return (char*)haystack;
    
    for (i = 0; haystack[i]; i++) {
        for (j = 0; j < needle_len && haystack[i + j] == needle[j]; j++);
        if (j == needle_len) return (char*)&haystack[i];
    }
    return NULL;
}

char* strrchr(const char* s, int c)
{
    char* last = NULL;
    while (*s) {
        if (*s == c) last = (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : last;
}


/* Ajouter a kernel/lib/string.c */

static unsigned long long udivmod_small(unsigned long long value,
                                        unsigned int divisor,
                                        unsigned int* remainder)
{
    unsigned long long quotient = 0u;
    unsigned int rest = 0u;
    int bit;

    for (bit = 63; bit >= 0; bit--) {
        rest = (rest << 1) | (unsigned int)((value >> bit) & 1u);
        if (rest >= divisor) {
            rest -= divisor;
            quotient |= 1ull << bit;
        }
    }
    if (remainder)
        *remainder = rest;
    return quotient;
}

static int itoa_helper(long long value, char* str, int base)
{
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    unsigned long long magnitude;
    unsigned int digit;
    int len = 0;
    
    /* Handle 0 explicitly */
    if (value == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return 1;
    }
    
    /* Handle negative numbers for base 10 */
    int is_negative = 0;
    if (value < 0 && base == 10) {
        is_negative = 1;
        magnitude = (unsigned long long)(-(value + 1)) + 1u;
    } else {
        magnitude = (unsigned long long)value;
    }
    
    /* Process individual digits */
    while (magnitude != 0u) {
        magnitude = udivmod_small(magnitude, (unsigned int)base, &digit);
        *ptr++ = "0123456789abcdef"[digit];
        len++;
    }
    
    /* Add negative sign */
    if (is_negative) {
        *ptr++ = '-';
        len++;
    }
    
    *ptr-- = '\0';
    
    /* Reverse string */
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
    
    return len;
}

static int utoa_helper(unsigned long long value, char* str, int base)
{
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    unsigned int digit;
    int len = 0;
    
    /* Handle 0 explicitly */
    if (value == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return 1;
    }
    
    /* Process individual digits */
    while (value != 0) {
        value = udivmod_small(value, (unsigned int)base, &digit);
        *ptr++ = "0123456789abcdef"[digit];
        len++;
    }
    
    *ptr-- = '\0';
    
    /* Reverse string */
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
    
    return len;
}

int vsnprintf(char* str, size_t size, const char* format, va_list args)
{
    const char* p;
    char* out;
    char* out_end;
    int written = 0;
    int temp_len;
    char temp_buf[32];
    const char* s;
    char c;
    
    if (!str || size == 0) return 0;

    out = str;
    out_end = str + size - 1; /* Reserve space for null terminator */
    
    for (p = format; *p; p++) {
        if (*p != '%') {
            if (out < out_end) {
                *out++ = *p;
            }
            written++;
            continue;
        }
        
        p++; /* Skip '%' */
        if (!*p) {
            break;
        }

        {
            int left_align = 0;
            int zero_pad = 0;
            int width = 0;
            int precision = -1;
            int length_modifier = 0;
            char spec;
            char pad_char;
            int i;

            while (*p == '-' || *p == '0' || *p == '+' || *p == ' ') {
                if (*p == '-') left_align = 1;
                else if (*p == '0') zero_pad = 1;
                p++;
            }

            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }

            if (*p == '.') {
                p++;
                precision = 0;
                if (*p == '*') {
                    precision = va_arg(args, int);
                    p++;
                } else {
                    while (*p >= '0' && *p <= '9') {
                        precision = precision * 10 + (*p - '0');
                        p++;
                    }
                }
            }

            if (*p == 'l') {
                length_modifier = 1;
                p++;
                if (*p == 'l') {
                    length_modifier = 2;
                    p++;
                }
            } else if (*p == 'z') {
                length_modifier = 1;
                p++;
            }

            spec = *p;
            pad_char = (zero_pad && !left_align) ? '0' : ' ';
        
            /* Handle format specifiers */
            switch (spec) {
            case 'c':
                c = (char)va_arg(args, int);
                if (!left_align && width > 1) {
                    for (i = 0; i < width - 1; i++) {
                        if (out < out_end) *out++ = ' ';
                        written++;
                    }
                }
                if (out < out_end) {
                    *out++ = c;
                }
                written++;
                if (left_align && width > 1) {
                    for (i = 0; i < width - 1; i++) {
                        if (out < out_end) *out++ = ' ';
                        written++;
                    }
                }
                break;
                
            case 's':
                s = va_arg(args, const char*);
                if (!s) s = "(null)";
                temp_len = 0;
                while (s[temp_len] && (precision < 0 || temp_len < precision)) {
                    temp_len++;
                }
                if (!left_align && width > temp_len) {
                    for (i = 0; i < width - temp_len; i++) {
                        if (out < out_end) *out++ = ' ';
                        written++;
                    }
                }
                for (i = 0; i < temp_len; i++) {
                    if (out < out_end) *out++ = s[i];
                    written++;
                }
                if (left_align && width > temp_len) {
                    for (i = 0; i < width - temp_len; i++) {
                        if (out < out_end) *out++ = ' ';
                        written++;
                    }
                }
                break;
                
            case 'd':
            case 'i': {
                long long val;

                if (length_modifier == 2)
                    val = va_arg(args, long long);
                else if (length_modifier == 1)
                    val = va_arg(args, long);
                else
                    val = va_arg(args, int);
                temp_len = itoa_helper(val, temp_buf, 10);
                if (!left_align && width > temp_len) {
                    if (pad_char == '0' && temp_buf[0] == '-') {
                        if (out < out_end) *out++ = '-';
                        written++;
                        for (i = 0; i < width - temp_len; i++) {
                            if (out < out_end) *out++ = '0';
                            written++;
                        }
                        for (i = 1; i < temp_len; i++) {
                            if (out < out_end) *out++ = temp_buf[i];
                            written++;
                        }
                        break;
                    }
                    for (i = 0; i < width - temp_len; i++) {
                        if (out < out_end) *out++ = pad_char;
                        written++;
                    }
                }
                for (i = 0; i < temp_len; i++) {
                    if (out < out_end) *out++ = temp_buf[i];
                    written++;
                }
                if (left_align && width > temp_len) {
                    for (i = 0; i < width - temp_len; i++) {
                        if (out < out_end) *out++ = ' ';
                        written++;
                    }
                }
                break;
            }
                
            case 'u': {
                unsigned long long val;

                if (length_modifier == 2)
                    val = va_arg(args, unsigned long long);
                else if (length_modifier == 1)
                    val = va_arg(args, unsigned long);
                else
                    val = va_arg(args, unsigned int);
                temp_len = utoa_helper(val, temp_buf, 10);
                if (!left_align && width > temp_len) {
                    for (i = 0; i < width - temp_len; i++) {
                        if (out < out_end) *out++ = pad_char;
                        written++;
                    }
                }
                for (i = 0; i < temp_len; i++) {
                    if (out < out_end) *out++ = temp_buf[i];
                    written++;
                }
                if (left_align && width > temp_len) {
                    for (i = 0; i < width - temp_len; i++) {
                        if (out < out_end) *out++ = ' ';
                        written++;
                    }
                }
                break;
            }
                
            case 'p': {
                unsigned long val =
                    (unsigned long)(uintptr_t)va_arg(args, void*);
                temp_buf[0] = '0';
                temp_buf[1] = 'x';
                temp_len = utoa_helper(val, temp_buf + 2, 16) + 2;
                if (!left_align && width > temp_len) {
                    for (i = 0; i < width - temp_len; i++) {
                        if (out < out_end) *out++ = ' ';
                        written++;
                    }
                }
                for (i = 0; i < temp_len; i++) {
                    if (out < out_end) *out++ = temp_buf[i];
                    written++;
                }
                if (left_align && width > temp_len) {
                    for (i = 0; i < width - temp_len; i++) {
                        if (out < out_end) *out++ = ' ';
                        written++;
                    }
                }
                break;
            }
                
            case 'x':
            case 'X':
                {
                    unsigned long long val;

                    if (length_modifier == 2)
                        val = va_arg(args, unsigned long long);
                    else if (length_modifier == 1)
                        val = va_arg(args, unsigned long);
                    else
                        val = va_arg(args, unsigned int);
                    temp_len = utoa_helper(val, temp_buf, 16);
                    if (spec == 'X') {
                        for (i = 0; i < temp_len; i++) {
                            if (temp_buf[i] >= 'a' && temp_buf[i] <= 'f')
                                temp_buf[i] = temp_buf[i] - 'a' + 'A';
                        }
                    }
                    if (!left_align && width > temp_len) {
                        for (i = 0; i < width - temp_len; i++) {
                            if (out < out_end) *out++ = pad_char;
                            written++;
                        }
                    }
                    for (i = 0; i < temp_len; i++) {
                        char ch = temp_buf[i];
                        if (out < out_end) *out++ = ch;
                        written++;
                    }
                    if (left_align && width > temp_len) {
                        for (i = 0; i < width - temp_len; i++) {
                            if (out < out_end) *out++ = ' ';
                            written++;
                        }
                    }
                }
                break;
                
            case '%':
                if (out < out_end) {
                    *out++ = '%';
                }
                written++;
                break;
                
            default:
                /* Unknown format specifier, just copy it */
                if (out < out_end) {
                    *out++ = '%';
                }
                written++;
                if (out < out_end) {
                    *out++ = spec;
                }
                written++;
                break;
            }
        }
    }
    
    *out = '\0';
    return written;
}

int snprintf(char* str, size_t size, const char* format, ...)
{
    va_list args;
    int written;

    va_start(args, format);
    written = vsnprintf(str, size, format, args);
    va_end(args);

    return written;
}


/**
 * strcat - Concatene src a la fin de dest
 * @dest: Chaine de destination (doit avoir assez d'espace)
 * @src: Chaine source a ajouter
 * 
 * Retourne: Pointeur vers dest
 * 
 * ATTENTION: dest doit avoir assez d'espace pour dest + src + '\0'
 * Cette fonction est dangereuse (buffer overflow possible)
 */
char* strcat(char* dest, const char* src)
{
    char* original_dest = dest;
    
    if (!dest || !src) {
        return dest;
    }
    
    /* Aller a la fin de dest */
    while (*dest != '\0') {
        dest++;
    }
    
    /* Copier src a la fin de dest */
    while (*src != '\0') {
        *dest = *src;
        dest++;
        src++;
    }
    
    /* Terminer par null */
    *dest = '\0';
    
    return original_dest;
}

/* ========================================================================
 * FONCTION SeCURISeE POSIX : strncat()
 * ======================================================================== */

/**
 * strncat - Concatene au maximum n caracteres de src a dest
 * @dest: Chaine de destination
 * @src: Chaine source
 * @n: Nombre maximum de caracteres a copier
 * 
 * Retourne: Pointeur vers dest
 * 
 * Plus s-re que strcat() car limite le nombre de caracteres
 */
char* strncat(char* dest, const char* src, size_t n)
{
    char* original_dest = dest;
    
    if (!dest || !src || n == 0) {
        return dest;
    }
    
    /* Aller a la fin de dest */
    while (*dest != '\0') {
        dest++;
    }
    
    /* Copier au maximum n caracteres de src */
    while (*src != '\0' && n > 0) {
        *dest = *src;
        dest++;
        src++;
        n--;
    }
    
    /* Toujours terminer par null */
    *dest = '\0';
    
    return original_dest;
}
