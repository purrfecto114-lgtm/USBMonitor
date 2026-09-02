/* util.c — time, glob, fingerprint (SHA-256), small fs helpers. */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#ifdef _WIN32
void um_utf8_to_wide(const char *in, wchar_t *out, size_t out_n)
{
    if (out_n == 0) return;
    out[0] = L'\0';
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, (int)out_n);
    out[out_n - 1] = L'\0';   /* guard against truncation without NUL */
}
void um_wide_to_utf8(const wchar_t *in, char *out, size_t out_n)
{
    if (out_n == 0) return;
    out[0] = '\0';
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)out_n, NULL, NULL);
    out[out_n - 1] = '\0';
}
#endif

/* ------------------------------------------------------------- time ------ */

void um_now_utc_iso(char *buf, size_t n)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    _snprintf_s(buf, n, _TRUNCATE, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tmv);
#endif
}

double um_monotonic(void)
{
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;  /* ms ticks, monotonic */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* -------------------------------------------------------------- glob ----- */

static int fold(int c, int fold_case)
{
    return fold_case ? tolower((unsigned char)c) : c;
}

/* Minimal glob: '*' and '?'.  No regex engine, no recursion blowups:
 * '*' handled iteratively with backtracking bounded by text length. */
int um_glob_match(const char *pattern, const char *text, int fold_case)
{
    const char *p = pattern, *t = text;
    const char *star = NULL, *star_t = NULL;

    while (*t) {
        if (*p == '?' || fold(*p, fold_case) == fold(*t, fold_case)) {
            p++; t++;
        } else if (*p == '*') {
            star = p++;
            star_t = t;
        } else if (star) {
            p = star + 1;
            t = ++star_t;
        } else {
            return 0;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/* --------------------------------------------------------------- SHA-256 - */
/* Compact FIPS 180-4 implementation.  Only used for a 12-hex-char
 * correlation fingerprint (same semantics as the Python original:
 * NOT encryption, NOT anonymization, not salted). */

typedef struct { unsigned int h[8]; unsigned long long len; unsigned char buf[64]; size_t fill; } sha256_ctx;

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const unsigned int K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len = 0; c->fill = 0;
}

static void sha256_block(sha256_ctx *c, const unsigned char *p)
{
    unsigned int w[64], a, b, cc, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
               ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
    for (i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3]; e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
    for (i = 0; i < 64; i++) {
        unsigned int t1 = h + EP1(e) + CH(e,f,g) + K[i] + w[i];
        unsigned int t2 = EP0(a) + MAJ(a,b,cc);
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_update(sha256_ctx *c, const unsigned char *data, size_t n)
{
    c->len += n;
    while (n) {
        size_t take = 64 - c->fill;
        if (take > n) take = n;
        memcpy(c->buf + c->fill, data, take);
        c->fill += take; data += take; n -= take;
        if (c->fill == 64) { sha256_block(c, c->buf); c->fill = 0; }
    }
}

static void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    unsigned long long bits = c->len * 8ULL;
    unsigned char pad = 0x80;
    unsigned char lenb[8];
    int i;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56) sha256_update(c, &pad, 1);
    for (i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)(c->h[i]);
    }
}

void um_fingerprint(const char *value, char *out, size_t out_n)
{
    unsigned char digest[32];
    sha256_ctx c;
    char hex[65];
    int i;
    sha256_init(&c);
    sha256_update(&c, (const unsigned char *)value, strlen(value));
    sha256_final(&c, digest);
    for (i = 0; i < 32; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);
    snprintf(out, out_n, "sha256:%.12s", hex);
}

/* ------------------------------------------------------------- fs helpers */

void um_copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

void um_human_size(unsigned long long bytes, char *out, size_t n)
{
    double v = (double)bytes;
    const char *unit[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    if (u <= 1) snprintf(out, n, "%.0f %s", v, unit[u]);
    else snprintf(out, n, "%.1f %s", v, unit[u]);
}

int um_mkdir_p(const char *path)
{
    char tmp[512];
    char *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
#ifdef _WIN32
            if (_mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
#endif
            *p = saved;
        }
    }
#ifdef _WIN32
    if (_mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
#endif
    return 0;
}

int um_read_file_str(const char *path, char *buf, size_t n)
{
    FILE *f;
    size_t got;
    buf[0] = '\0';
    f = fopen(path, "rb");
    if (!f) return -1;
    got = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got] = '\0';
    /* trim trailing whitespace/newlines sysfs loves to add */
    while (got > 0 && (buf[got-1] == '\n' || buf[got-1] == '\r' || buf[got-1] == ' ' || buf[got-1] == '\t'))
        buf[--got] = '\0';
    return (int)got;
}

/* ------------------------------------------------------------- dirs ------ */

const char *um_state_dir(char *buf, size_t n)
{
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base) base = getenv("APPDATA");
    if (!base) base = ".";
    snprintf(buf, n, "%s\\usbmon", base);
#else
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/usbmon", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        snprintf(buf, n, "%s/.local/state/usbmon", home);
    }
#endif
    um_mkdir_p(buf);
    return buf;
}

const char *um_config_dir(char *buf, size_t n)
{
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base) base = getenv("LOCALAPPDATA");
    if (!base) base = ".";
    snprintf(buf, n, "%s\\usbmon", base);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/usbmon", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        snprintf(buf, n, "%s/.config/usbmon", home);
    }
#endif
    um_mkdir_p(buf);
    return buf;
}
