/* logjson.c — JSONL event log with size-based rotation. */
#include "usbmon.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Escape a C string into a JSON string body (quotes not included). */
void um_json_escape(const char *in, char *out, size_t out_n)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    if (out_n < 4) { if (out_n) out[0] = '\0'; return; }

    for (; *p && o + 7 < out_n - 1; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
        case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                const char *hex = "0123456789abcdef";
                out[o++] = '\\'; out[o++] = 'u';
                out[o++] = '0';  out[o++] = '0';
                out[o++] = hex[(c >> 4) & 0xf];
                out[o++] = hex[c & 0xf];
            } else {
                out[o++] = (char)c;   /* pass UTF-8 through verbatim */
            }
        }
    }
    out[o] = '\0';
}

/* ---------------------------------------------------- platform file bits -- */

static long long file_size(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[512];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    LARGE_INTEGER li;
    if (!path) return -1;
    memset(wpath, 0, sizeof wpath);
    um_utf8_to_wide(path, wpath, 512);
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &fa)) return -1;
    li.LowPart = fa.nFileSizeLow;
    li.HighPart = fa.nFileSizeHigh;
    return (long long)li.QuadPart;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
#endif
}

static FILE *open_log_file(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[512];
    memset(wpath, 0, sizeof wpath);
    um_utf8_to_wide(path, wpath, 512);
    return _wfopen(wpath, L"a");
#else
    return fopen(path, "a");
#endif
}

static int remove_file(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[512];
    memset(wpath, 0, sizeof wpath);
    um_utf8_to_wide(path, wpath, 512);
    return DeleteFileW(wpath) ? 0 : -1;
#else
    return remove(path);
#endif
}

static int rename_file(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t wfrom[512], wto[512];
    memset(wfrom, 0, sizeof wfrom);
    memset(wto, 0, sizeof wto);
    um_utf8_to_wide(from, wfrom, 512);
    um_utf8_to_wide(to, wto, 512);
    return MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

/* Rotate: events.jsonl -> .1 -> .2 -> .3 (oldest dropped). */
static void rotate(um_logger *lg)
{
    char from[560], to[560];
    int i;
    for (i = UM_LOG_KEEP - 1; i >= 1; i--) {
        snprintf(from, sizeof from, "%s.%d", lg->path, i);
        snprintf(to,   sizeof to,   "%s.%d", lg->path, i + 1);
        (void)remove_file(to);
        (void)rename_file(from, to);
    }
    snprintf(to, sizeof to, "%s.1", lg->path);
    (void)rename_file(lg->path, to);
}

int um_log_open(um_logger *lg, const char *path, int raw, int verbose)
{
    memset(lg, 0, sizeof *lg);
    um_copy_str(lg->path, sizeof lg->path, path);
    lg->raw = raw;
    lg->verbose = verbose;
    lg->fp = open_log_file(lg->path);
    if (!lg->fp) return -1;
    return 0;
}

void um_log_close(um_logger *lg)
{
    if (lg->fp) fclose(lg->fp);
    lg->fp = NULL;
}

void um_log_line(um_logger *lg, const char *line)
{
    if (!lg->fp) {
        if (lg->verbose) fprintf(stderr, "%s\n", line);
        return;
    }

    /* rotate if needed */
    long long sz = file_size(lg->path);
    if (sz >= 0 && (unsigned long long)sz >= UM_LOG_ROTATE_BYTES) {
        fclose(lg->fp);
        rotate(lg);
        lg->fp = open_log_file(lg->path);
        if (!lg->fp) {
            if (lg->verbose) fprintf(stderr, "usbmon: log rotate failed: %s\n",
                                     strerror(errno));
            return;
        }
    }

    fprintf(lg->fp, "%s\n", line);
    fflush(lg->fp);
    if (lg->verbose) fprintf(stderr, "%s\n", line);
}
