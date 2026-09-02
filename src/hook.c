/* hook.c — opt-in automation hooks (USB insertion → external command).
 *
 * SECURITY MODEL (inherited from the Python original, see its README):
 *   - hooks are entirely opt-in; the default config has none;
 *   - commands are argv arrays, executed WITHOUT a shell on every platform
 *     (execv on POSIX, CreateProcessW with an explicit argv on Windows);
 *   - on Windows, BatBadBut (CVE-2024-24576) and CmdHijack guards reject
 *     .bat/.cmd/.ps1 executables, cmd.exe metacharacters and ../ sequences,
 *     because CreateProcessW implicitly spawns cmd.exe for those;
 *   - the child's stdio is redirected to /dev/null (NUL);
 *   - a reaper kills children that outlive UM_HOOK_TIMEOUT_S seconds.
 * This is NOT a sandbox: a hook runs with the full rights of the user.
 */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#define strcasecmp_ strcasecmp
#endif

#ifdef _WIN32
#define strcasecmp_ _stricmp
#endif

/* ------------------------------------------------------------ parsing ---- */

static void hook_patterns(const jval *obj, const char *key,
                          char out[UM_HOOK_MAX_PATTERNS][UM_HOOK_PATTERN_MAX],
                          int *n)
{
    const jval *arr = json_get(obj, key);
    int i;
    *n = 0;
    if (!arr || arr->t != J_ARR) return;
    for (i = 0; i < arr->n && *n < UM_HOOK_MAX_PATTERNS; i++) {
        const char *s = json_str(arr->items[i], "");
        const char *t = s;
        /* skip leading whitespace, cap length */
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) continue;
        snprintf(out[*n], UM_HOOK_PATTERN_MAX, "%s", t);
        (*n)++;
    }
}

int um_hooks_load(um_hooks *hk, const char *path)
{
    char text[64 * 1024];
    jval *root;
    const jval *arr;
    int i;

    memset(hk, 0, sizeof *hk);
    hk->reap_n = 0;
    hk->fired_n = 0;
    if (!path || !*path) return 0;
    um_copy_str(hk->path, sizeof hk->path, path);

    if (um_read_file_str(path, text, sizeof text) < 0) {
        /* missing file = no hooks (not an error: hooks are opt-in) */
        hk->count = 0;
        return 0;
    }
    if (!text[0]) { hk->count = 0; return 0; }

    root = json_parse(text);
    if (!root) {
        fprintf(stderr, "usbmon: hooks config %s: invalid JSON — hooks disabled\n", path);
        return -1;
    }
    arr = json_get(root, "hooks");
    if (arr && arr->t == J_ARR) {
        for (i = 0; i < arr->n && hk->count < UM_MAX_HOOKS; i++) {
            const jval *o = arr->items[i];
            um_hook *h = &hk->hooks[hk->count];
            const jval *cmd, *en;
            const jval *db;
            int j;

            if (!o || o->t != J_OBJ) continue;

            /* name: required, unique */
            {
                const char *nm = json_str(json_get(o, "name"), "");
                const char *t = nm;
                while (*t == ' ' || *t == '\t') t++;
                if (!*t) continue;                    /* nameless → ignore */
                snprintf(h->name, sizeof h->name, "%.63s", t);
            }
            {   /* duplicate name? (linear scan, hooks are capped at 20) */
                int dup = 0, k;
                for (k = 0; k < hk->count; k++)
                    if (strcasecmp_(hk->hooks[k].name, h->name) == 0) { dup = 1; break; }
                if (dup) continue;
            }

            /* command: required non-empty argv array */
            cmd = json_get(o, "command");
            if (!cmd || cmd->t != J_ARR || cmd->n < 1 || cmd->n > UM_HOOK_MAX_ARGS)
                continue;
            {
                int ok = 1;
                h->argc = 0;
                for (j = 0; j < cmd->n; j++) {
                    const char *s = json_str(cmd->items[j], "");
                    const char *t = s;
                    while (*t == ' ' || *t == '\t') t++;
                    if (!*t || strlen(t) > UM_HOOK_ARG_MAX - 1) { ok = 0; break; }
                    if (j == 0)
                        snprintf(h->command, sizeof h->command, "%s", t);
                    else
                        snprintf(h->argv[j - 1], sizeof h->argv[0], "%s", t);
                    h->argc++;
                }
                if (!ok || h->argc < 1) continue;
            }

            hook_patterns(o, "match_keys", h->match_keys, &h->n_keys);
            hook_patterns(o, "match_models", h->match_models, &h->n_models);

            db = json_get(o, "debounce_seconds");
            h->debounce_s = db && db->t == J_NUM ? db->num : 2.0;
            if (!(h->debounce_s >= 0.1)) h->debounce_s = 0.1;
            if (h->debounce_s > 3600.0)  h->debounce_s = 3600.0;

            en = json_get(o, "enabled");
            h->enabled = (en && en->t == J_BOOL) ? en->b : 1;

            hk->count++;
        }
    }
    json_free(root);
    return 0;
}

void um_hooks_shutdown(um_hooks *hk, int max_wait_ms)
{
    int i;
    /* Give children a grace period to finish. --once callers pass the full
     * hook timeout (their children may still be mid-run); SIGTERM shutdown
     * uses a shorter budget so daemon exit stays responsive. */
    int waited_ms = 0;
    while (hk->reap_n > 0 && waited_ms < max_wait_ms) {
        um_hooks_reap(hk);
        if (hk->reap_n == 0) break;
#ifdef _WIN32
        Sleep(25);
#else
        {
            const struct timespec ts = { 0, 25 * 1000000L };
            nanosleep(&ts, NULL);
        }
#endif
        waited_ms += 25;
    }
    for (i = 0; i < hk->reap_n; i++) {
#ifdef _WIN32
        if (hk->reap[i].h) {
            TerminateProcess((HANDLE)hk->reap[i].h, 1);
            WaitForSingleObject((HANDLE)hk->reap[i].h, 2000);
            CloseHandle((HANDLE)hk->reap[i].h);
            hk->reap[i].h = NULL;
        }
#else
        if (hk->reap[i].pid > 0) {
            kill(hk->reap[i].pid, SIGKILL);
            waitpid(hk->reap[i].pid, NULL, 0);
            hk->reap[i].pid = 0;
        }
#endif
    }
    hk->reap_n = 0;
}

/* ------------------------------------------------------------ security --- */

#ifdef _WIN32
static const char *CMD_SUFFIXES[] = { ".bat", ".cmd", ".ps1", NULL };
static const char CMD_METACHARS[] = "&|<>()%^";

static const char *validate_command_win(char **argv, int argc)
{
    int i;
    const char *exec_lower;
    size_t el;
    int s;

    /* 1) shell-script executables implicitly invoke cmd.exe (BatBadBut) */
    exec_lower = argv[0];
    el = strlen(exec_lower);
    for (s = 0; CMD_SUFFIXES[s]; s++) {
        size_t sl = strlen(CMD_SUFFIXES[s]);
        if (el >= sl && _stricmp(exec_lower + el - sl, CMD_SUFFIXES[s]) == 0)
            return "shell_script_executable";
    }
    /* 2) cmd.exe metacharacters + traversal in any token.  Also reject
     * '"' and trailing '\\': with our naive quoting those would let the
     * child's argument parser shift token boundaries (argument injection). */
    for (i = 0; i < argc; i++) {
        const char *p;
        size_t tl = strlen(argv[i]);
        for (p = argv[i]; *p; p++) {
            if (strchr(CMD_METACHARS, *p)) return "cmd_metachar";
            if (*p == '"') return "embedded_quote";
        }
        if (tl > 0 && argv[i][tl-1] == '\\') return "trailing_backslash";
        if (strstr(argv[i], "../") || strstr(argv[i], "\\..")) return "path_traversal";
        if (tl >= 2 && argv[i][0] == '.' && argv[i][1] == '.' &&
            (argv[i][2] == '\\' || argv[i][2] == '/')) return "path_traversal";
    }
    return NULL;
}
#endif

/* ----------------------------------------------------------- dispatch ---- */

static int fired_recently(um_hooks *hk, const char *hook_name, const char *dev_key,
                          double debounce)
{
    char key[UM_KEY_MAX + UM_NAME_MAX];
    double now = um_monotonic();
    int i, oldest = 0;
    const int CAP = (int)(sizeof hk->fired / sizeof hk->fired[0]);
    snprintf(key, sizeof key, "%s:%s", hook_name, dev_key);
    for (i = 0; i < hk->fired_n; i++) {
        if (strcmp(hk->fired[i].key, key) == 0) {
            if (now - hk->fired[i].at < debounce) return 1;
            hk->fired[i].at = now;
            return 0;
        }
    }
    if (hk->fired_n < CAP) {
        snprintf(hk->fired[hk->fired_n].key, sizeof hk->fired[0].key, "%s", key);
        hk->fired[hk->fired_n].at = now;
        hk->fired_n++;
        return 0;
    }
    /* table full: evict the stalest entry so debounce keeps working
     * instead of silently matching nothing (which would over-fire) */
    for (i = 1; i < CAP; i++)
        if (hk->fired[i].at < hk->fired[oldest].at) oldest = i;
    snprintf(hk->fired[oldest].key, sizeof hk->fired[0].key, "%s", key);
    hk->fired[oldest].at = now;
    return 0;
}

static void spawn_child(um_hooks *hk, const um_hook *h, char **argv, int argc,
                        const um_device *dev)
{
    if (hk->reap_n >= UM_REAP_MAX) {
        /* Refuse rather than fork an untracked child: an untracked pid can
         * never be reaped or timed out (zombie / runaway). */
        fprintf(stderr, "usbmon: hook queue full (%d), '%s' not spawned\n",
                UM_REAP_MAX, h->name);
        return;
    }

#ifdef _WIN32
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE nul = NULL;
    wchar_t wargv[UM_HOOK_MAX_ARGS][UM_HOOK_ARG_MAX];
    wchar_t cmdline[UM_HOOK_MAX_ARGS * (size_t)UM_HOOK_ARG_MAX];
    const size_t CAP = sizeof cmdline / sizeof cmdline[0];
    int i;
    size_t off = 0;

    for (i = 0; i < argc; i++) {
        memset(wargv[i], 0, sizeof wargv[i]);
        um_utf8_to_wide(argv[i], wargv[i], UM_HOOK_ARG_MAX);
    }
    /* Build one command line with every argument quoted; argv semantics are
     * preserved because validate_command_win() already rejected '"' and
     * trailing '\' inside tokens. */
    for (i = 0; i < argc; i++) {
        int ret = _snwprintf_s(cmdline + off, CAP - off, _TRUNCATE,
                               L"\"%s\"%s", wargv[i], i + 1 < argc ? L" " : L"");
        if (ret < 0 || off + (size_t)ret >= CAP) {
            fprintf(stderr, "usbmon: hook '%s' command line too long\n", h->name);
            return;
        }
        off += (size_t)ret;
    }

    /* stdio -> NUL, no console window flash */
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (nul != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = nul;
        si.hStdError = nul;
    }
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        fprintf(stderr, "usbmon: hook '%s' spawn failed (CreateProcessW)\n", h->name);
        return;
    }
    CloseHandle(pi.hThread);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    hk->reap[hk->reap_n].h = pi.hProcess;   /* keep handle: PID reuse-proof */
    hk->reap[hk->reap_n].deadline = um_monotonic() + UM_HOOK_TIMEOUT_S;
    snprintf(hk->reap[hk->reap_n].hook, sizeof hk->reap[0].hook, "%s", h->name);
    hk->reap_n++;

#else /* POSIX */
    char *nargv[UM_HOOK_MAX_ARGS + 1];
    pid_t pid;
    int i;
    int devnull;

    for (i = 0; i < argc; i++) nargv[i] = argv[i];
    nargv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "usbmon: hook '%s' fork failed: %s\n", h->name, strerror(errno));
        return;
    }
    if (pid == 0) {
        /* child: detach session, silence stdio, exec */
        setsid();
        devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        execv(nargv[0], nargv);
        _exit(127);
    }
    /* parent: track for reaping (spawn refused above when full) */
    hk->reap[hk->reap_n].pid = (int)pid;
    hk->reap[hk->reap_n].deadline = um_monotonic() + UM_HOOK_TIMEOUT_S;
    snprintf(hk->reap[hk->reap_n].hook, sizeof hk->reap[0].hook, "%s", h->name);
    hk->reap_n++;
#endif
    (void)dev;
}

void um_hooks_dispatch(um_hooks *hk, const um_device *dev, const char *path_hint)
{
    int i;
    char argvbuf[UM_HOOK_MAX_ARGS][UM_HOOK_ARG_MAX];
    char *argv[UM_HOOK_MAX_ARGS];

    if (hk->hooks_disabled || hk->count == 0) return;

    for (i = 0; i < hk->count; i++) {
        const um_hook *h = &hk->hooks[i];
        int j, matched = 1;
        char token[UM_HOOK_ARG_MAX * 2];

        if (!h->enabled) continue;

        if (h->n_keys > 0) {
            matched = 0;
            for (j = 0; j < h->n_keys; j++)
                if (um_glob_match(h->match_keys[j], dev->key, 1)) { matched = 1; break; }
        }
        if (matched && h->n_models > 0) {
            matched = 0;
            for (j = 0; j < h->n_models; j++)
                if (um_glob_match(h->match_models[j], dev->model, 1)) { matched = 1; break; }
        }
        if (!matched) continue;

        if (fired_recently(hk, h->name, dev->key, h->debounce_s)) continue;

        /* substitute {key} {model} {path} placeholders, argv-array only */
        {
            int ok = 1;
            const char *path = (path_hint && *path_hint) ? path_hint
                        : (dev->mount[0] ? dev->mount : "");
            for (j = 0; j < h->argc; j++) {
                const char *src = j == 0 ? h->command : h->argv[j - 1];
                const char *p = src;
                size_t o = 0;
                token[0] = '\0';
                while (*p && o < sizeof token - 1) {
                    if (*p == '{') {
                        if (!strncmp(p, "{key}", 5)) {
                            o += (size_t)snprintf(token + o, sizeof token - o, "%s", dev->key);
                            p += 5;
                        } else if (!strncmp(p, "{model}", 7)) {
                            o += (size_t)snprintf(token + o, sizeof token - o, "%s", dev->model);
                            p += 7;
                        } else if (!strncmp(p, "{path}", 6)) {
                            o += (size_t)snprintf(token + o, sizeof token - o, "%s", path);
                            p += 6;
                        } else { ok = 0; break; }   /* unknown placeholder → reject */
                    } else {
                        token[o++] = *p++;
                    }
                }
                if (!ok || o >= sizeof token - 1) { matched = 0; break; }
                token[o] = '\0';
                if (strlen(token) >= sizeof argvbuf[0] - 1) {
                    /* expanded token no longer fits an argv slot: reject
                     * rather than silently truncate (review #19) */
                    matched = 0; break;
                }
                snprintf(argvbuf[j], sizeof argvbuf[0], "%s", token);
                argv[j] = argvbuf[j];
            }
            if (!matched || !ok) continue;
        }

#ifdef _WIN32
        {
            const char *reject = validate_command_win(argv, h->argc);
            if (reject) {
                fprintf(stderr, "usbmon: hook '%s' rejected (%s)\n", h->name, reject);
                continue;
            }
        }
#endif

        fprintf(stderr, "usbmon: hook '%s' fired for device %s\n", h->name, dev->key);
        spawn_child(hk, h, argv, h->argc, dev);
    }
}

/* ------------------------------------------------------------ reaper ----- */

void um_hooks_reap(um_hooks *hk)
{
    int i = 0;
    while (i < hk->reap_n) {
        int gone = 0;
#ifdef _WIN32
        if (hk->reap[i].h) {
            HANDLE ph = (HANDLE)hk->reap[i].h;
            DWORD wr = WaitForSingleObject(ph, 0);
            if (wr == WAIT_OBJECT_0) {
                CloseHandle(ph);
                hk->reap[i].h = NULL;
                gone = 1;
            } else if (wr == WAIT_FAILED) {
                CloseHandle(ph);
                hk->reap[i].h = NULL;
                gone = 1;               /* handle invalid: nothing we can do */
            } else if (um_monotonic() > hk->reap[i].deadline) {
                TerminateProcess(ph, 1);
                WaitForSingleObject(ph, 2000);
                fprintf(stderr, "usbmon: hook '%s' killed (timeout)\n",
                        hk->reap[i].hook);
                CloseHandle(ph);
                hk->reap[i].h = NULL;
                gone = 1;
            }
        } else gone = 1;
#else
        if (hk->reap[i].pid > 0) {
            int status;
            pid_t r = waitpid((pid_t)hk->reap[i].pid, &status, WNOHANG);
            if (r == (pid_t)hk->reap[i].pid) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
                    fprintf(stderr,
                            "usbmon: hook '%s' child exited 127 (exec failed?)\n",
                            hk->reap[i].hook);
                gone = 1;
            } else if (r < 0) {
                gone = 1;               /* ECHILD: already reaped elsewhere */
            } else if (r == 0 && um_monotonic() > hk->reap[i].deadline) {
                kill((pid_t)hk->reap[i].pid, SIGKILL);
                waitpid((pid_t)hk->reap[i].pid, &status, 0);
                fprintf(stderr, "usbmon: hook '%s' killed (timeout)\n",
                        hk->reap[i].hook);
                gone = 1;
            }
        } else gone = 1;
#endif
        if (gone) {
            /* swap-remove */
            hk->reap[i] = hk->reap[hk->reap_n - 1];
            hk->reap_n--;
        } else {
            i++;
        }
    }
}
