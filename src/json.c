/* json.c — minimal recursive-descent JSON parser for the hooks config.
 *
 * Scope is deliberately small: objects, arrays, strings, numbers, booleans,
 * null.  Depth-capped and node-capped so a hostile config cannot blow the
 * stack or exhaust memory.  \uXXXX escapes are decoded to UTF-8 (with
 * surrogate-pair handling).
 */
#include "usbmon.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define J_MAX_DEPTH 24
#define J_MAX_NODES 2048

typedef struct {
    const char *p;
    const char *end;
    int depth;
    int nodes;
    int failed;
} jparser;

static jval *jnew(jtype t)
{
    jval *v = (jval *)calloc(1, sizeof *v);
    if (v) v->t = t;
    return v;
}

void json_free(jval *v)
{
    int i;
    if (!v) return;
    if (v->t == J_STR) free(v->str);
    else if (v->t == J_ARR) {
        for (i = 0; i < v->n; i++) json_free(v->items[i]);
        free(v->items);
    } else if (v->t == J_OBJ) {
        for (i = 0; i < v->nk; i++) { free(v->keys[i]); json_free(v->vals[i]); }
        free(v->keys); free(v->vals);
    }
    free(v);
}

static void jskip(jparser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' ||
                               *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

/* returns 1 on success, 0 on allocation failure (caller must abort) */
static int append(char **out, size_t *cap, size_t *len, const void *bytes, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t new_cap = (*cap + n + 1) * 2;
        char *p = realloc(*out, new_cap);
        if (!p) return 0;
        *out = p;
        *cap = new_cap;
    }
    memcpy(*out + *len, bytes, n);
    *len += n;
    (*out)[*len] = '\0';
    return 1;
}

static int utf8_emit(char **out, size_t *cap, size_t *len, unsigned cp)
{
    unsigned char b[4];
    size_t n;
    if (!cp) return 0;  /* A hook argument cannot represent an embedded NUL. */
    if (cp < 0x80) {
        b[0] = (unsigned char)cp; n = 1;
    } else if (cp < 0x800) {
        b[0] = 0xc0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3f); n = 2;
    } else if (cp < 0x10000) {
        b[0] = 0xe0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3f);
        b[2] = 0x80 | (cp & 0x3f); n = 3;
    } else {
        b[0] = 0xf0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3f);
        b[2] = 0x80 | ((cp >> 6) & 0x3f); b[3] = 0x80 | (cp & 0x3f); n = 4;
    }
    return append(out, cap, len, b, n);
}

static int hex4(jparser *ps, unsigned int *out)
{
    unsigned int v = 0;
    int i;
    if (ps->end - ps->p < 4) return 0;
    for (i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    ps->p += 4;
    *out = v;
    return 1;
}

static char *parse_string_raw(jparser *ps)
{
    size_t cap = 32, len = 0;
    char *s = (char *)malloc(cap);
    if (!s) { ps->failed = 1; return NULL; }
    s[0] = '\0';

    if (ps->p >= ps->end || *ps->p != '"') { free(s); ps->failed = 1; return NULL; }
    ps->p++;

    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p;
        if ((unsigned char)c < 0x20) { ps->failed = 1; free(s); return NULL; }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) { ps->failed = 1; free(s); return NULL; }
            char e = *ps->p++;
            switch (e) {
            case '"':  if (!utf8_emit(&s, &cap, &len, '"'))  goto oom; break;
            case '\\': if (!utf8_emit(&s, &cap, &len, '\\')) goto oom; break;
            case '/':  if (!utf8_emit(&s, &cap, &len, '/'))  goto oom; break;
            case 'b':  if (!utf8_emit(&s, &cap, &len, '\b')) goto oom; break;
            case 'f':  if (!utf8_emit(&s, &cap, &len, '\f')) goto oom; break;
            case 'n':  if (!utf8_emit(&s, &cap, &len, '\n')) goto oom; break;
            case 'r':  if (!utf8_emit(&s, &cap, &len, '\r')) goto oom; break;
            case 't':  if (!utf8_emit(&s, &cap, &len, '\t')) goto oom; break;
            case 'u': {
                unsigned int cp;
                if (!hex4(ps, &cp)) { ps->failed = 1; free(s); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 &&
                    ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned int lo;
                    const char *save = ps->p;
                    ps->p += 2;
                    if (hex4(ps, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else { ps->p = save; cp = 0xFFFD; }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;  /* lone surrogate */
                }
                if (!utf8_emit(&s, &cap, &len, cp)) goto oom;
                break;
            }
            default: ps->failed = 1; free(s); return NULL;
            }
        } else {
            size_t n = um_utf8_sequence((const unsigned char *)ps->p,
                                         (size_t)(ps->end - ps->p));
            if (!n || !append(&s, &cap, &len, ps->p, n)) goto oom;
            ps->p += n;
        }
    }
    if (ps->p >= ps->end) { free(s); ps->failed = 1; return NULL; }
    ps->p++;  /* closing quote */
    return s;

    oom:
        free(s);
        ps->failed = 1;
        return NULL;
}

static jval *parse_value(jparser *ps);

static jval *parse_object(jparser *ps)
{
    jval *v = jnew(J_OBJ);
    if (!v) { ps->failed = 1; return NULL; }
    ps->p++;  /* '{' */
    jskip(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }

    for (;;) {
        jskip(ps);
        char *key = parse_string_raw(ps);
        if (ps->failed) { json_free(v); return NULL; }
        jskip(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(key); json_free(v); ps->failed = 1; return NULL; }
        ps->p++;
        jval *val = parse_value(ps);
        if (!val) { free(key); json_free(v); ps->failed = 1; return NULL; }

        {
            char **nkeys = (char **)realloc(v->keys, sizeof(char *) * (size_t)(v->nk + 1));
            jval **nvals = (jval **)realloc(v->vals, sizeof(jval *) * (size_t)(v->nk + 1));
            if (!nkeys || !nvals) {
                free(key);
                json_free(val);
                if (nkeys) { v->keys = nkeys; }   /* keep whichever realloc won */
                if (nvals) { v->vals = nvals; }
                json_free(v);
                ps->failed = 1;
                return NULL;
            }
            v->keys = nkeys;
            v->vals = nvals;
        }
        v->keys[v->nk] = key;
        v->vals[v->nk] = val;
        v->nk++;

        jskip(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
        ps->failed = 1; json_free(v); return NULL;
    }
}

static jval *parse_array(jparser *ps)
{
    jval *v = jnew(J_ARR);
    if (!v) { ps->failed = 1; return NULL; }
    ps->p++;  /* '[' */
    jskip(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }

    for (;;) {
        jval *item = parse_value(ps);
        if (!item) { json_free(v); ps->failed = 1; return NULL; }
        {
            jval **nitems = (jval **)realloc(v->items, sizeof(jval *) * (size_t)(v->n + 1));
            if (!nitems) { json_free(item); json_free(v); ps->failed = 1; return NULL; }
            v->items = nitems;
        }
        v->items[v->n++] = item;

        jskip(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
        ps->failed = 1; json_free(v); return NULL;
    }
}

static int digit(char c) { return c >= '0' && c <= '9'; }

static jval *parse_number(jparser *ps)
{
    const char *start = ps->p, *p = start;
    char *end;
    double number;
    jval *val;
    if (*p == '-') p++;
    if (*p == '0') p++;
    else {
        if (!digit(*p)) goto invalid;
        while (digit(*p)) p++;
    }
    if (*p == '.') {
        if (!digit(*++p)) goto invalid;
        while (digit(*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!digit(*p)) goto invalid;
        while (digit(*p)) p++;
    }
    number = strtod(start, &end);
    if (end != p || !isfinite(number)) goto invalid;
    ps->p = p;
    val = jnew(J_NUM);
    if (val) val->num = number;
    return val;

invalid:
    ps->failed = 1;
    return NULL;
}

static jval *parse_value(jparser *ps)
{
    jval *v;
    jskip(ps);
    if (ps->p >= ps->end) { ps->failed = 1; return NULL; }
    if (++ps->nodes > J_MAX_NODES) { ps->failed = 1; return NULL; }
    if (++ps->depth > J_MAX_DEPTH) { ps->failed = 1; return NULL; }
    ps->depth--;

    switch (*ps->p) {
    case '{': ps->depth++; v = parse_object(ps);  ps->depth--; return v;
    case '[': ps->depth++; v = parse_array(ps);   ps->depth--; return v;
    case '"': {
        char *s = parse_string_raw(ps);
        if (ps->failed) return NULL;
        v = jnew(J_STR);
        if (!v) { free(s); ps->failed = 1; return NULL; }
        v->str = s;
        return v;
    }
    default:
        if (!strncmp(ps->p, "true", 4) && ps->end - ps->p >= 4) {
            ps->p += 4; v = jnew(J_BOOL); if (v) v->b = 1; return v;
        }
        if (!strncmp(ps->p, "false", 5) && ps->end - ps->p >= 5) {
            ps->p += 5; return jnew(J_BOOL);
        }
        if (!strncmp(ps->p, "null", 4) && ps->end - ps->p >= 4) {
            ps->p += 4; return jnew(J_NULL);
        }
        if (*ps->p == '-' || digit(*ps->p)) return parse_number(ps);
        ps->failed = 1;
        return NULL;
    }
}

jval *json_parse(const char *text)
{
    jparser ps;
    jval *v;
    ps.p = text;
    ps.end = text + strlen(text);
    if (!strncmp(text, "\xef\xbb\xbf", 3)) ps.p += 3;
    ps.depth = 0;
    ps.nodes = 0;
    ps.failed = 0;

    jskip(&ps);
    v = parse_value(&ps);
    if (ps.failed) { json_free(v); return NULL; }
    jskip(&ps);
    if (ps.p != ps.end) { json_free(v); return NULL; }  /* trailing garbage */
    return v;
}

const jval *json_get(const jval *obj, const char *key)
{
    int i;
    if (!obj || obj->t != J_OBJ) return NULL;
    for (i = 0; i < obj->nk; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->vals[i];
    return NULL;
}

const char *json_str(const jval *v, const char *fallback)
{
    if (v && v->t == J_STR && v->str) return v->str;
    return fallback;
}
