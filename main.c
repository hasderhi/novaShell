/*
 * novaShell 2.0.0 - a small Windows command shell.
 *
 * Build (use a 64-bit toolchain: a 32-bit build cannot see System32\OpenSSH\ssh.exe):
 *   MSVC:  cl /O2 main.c
 *   MinGW: gcc -O2 main.c -o novashell.exe
 *
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* GetTickCount64 needs Vista+ */
#endif
#include <windows.h>
#include <shellapi.h>
#include <conio.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define APP_NAME       "novaShell"
#define APP_VERSION    "2.0.0"
#define INPUT_CAP      4096
#define PATH_CAP       (MAX_PATH * 4)
#define HISTORY_CAP    500
#define ALIAS_CAP      64
#define ALIAS_NAME_CAP 64
#define ALIAS_CMD_CAP  512
#define SSH_CAP        128
#define BM_CAP         64
#define COLOR_DEFAULT  (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define COLOR_PROMPT   (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COLOR_DIR      (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_EXE      (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_ARCHIVE  (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COLOR_ERROR    (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COLOR_ACCENT   (FOREGROUND_BLUE | FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COLOR_WARN     (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_DIM      (FOREGROUND_INTENSITY)

#define EXIT_CTRL_C    0xC000013Au

#define JOB_CAP        32




static HANDLE g_out;
static WORD g_default_attr = COLOR_DEFAULT;
static char g_history[HISTORY_CAP][INPUT_CAP];
static size_t g_history_count = 0;
static char g_history_path[PATH_CAP];
static char g_config_path[PATH_CAP];
static char g_prev_dir[PATH_CAP];                       /* target of "cd -" */
static char g_cwd_display[PATH_CAP];                    /* cwd as shown in the prompt (with ~) */
static volatile LONG g_cancel_requested = 0;
static DWORD g_last_exit = 0;                           /* exit status of the last command */
static char g_git_branch[128];                          /* current git branch for the prompt, or empty */
static char g_prompt_fmt[256] = "{code}{cwd}{git}> ";   /* see the "prompt" command */
static char g_self_path[PATH_CAP];                      /* full path of novashell.exe (child instances need it) */
static BOOL g_exec_mode = FALSE;                        /* TRUE in the helper child started with --nova-exec / --nova-line */

typedef struct Job { int id; HANDLE process, job_object; DWORD pid; char cmd[120]; } Job;
static Job g_jobs[JOB_CAP];
static int g_job_next_id = 1;

static int job_running_count(void) {
    int n = 0;
    for (int i = 0; i < JOB_CAP; ++i)
        if (g_jobs[i].process && WaitForSingleObject(g_jobs[i].process, 0) == WAIT_TIMEOUT) ++n;
    return n;
}

typedef struct Alias { char name[ALIAS_NAME_CAP]; char command[ALIAS_CMD_CAP]; } Alias;
static Alias g_aliases[ALIAS_CAP];
static size_t g_alias_count = 0;

typedef struct SshHost {
    char name[64];
    char user[64];
    char host[256];
    int  port;              /* 0 = let ssh decide (default / ssh_config) */
    char key[MAX_PATH];     /* identity file, may be empty */
    char extra[256];        /* e.g. -L 8080:localhost:80 -o ServerAliveInterval=30 */
} SshHost;
static SshHost g_ssh[SSH_CAP];
static size_t g_ssh_count = 0;

typedef struct Bookmark { char name[64]; char path[MAX_PATH]; } Bookmark;
static Bookmark g_bm[BM_CAP];
static size_t g_bm_count = 0;

/* Built-in command names used by "which" and Tab completion */
static const char *const g_builtins[] = {
    "about", "alias", "bm", "cat", "cd", "clear", "cls", "config", "dir", "env", "exit", "export", "help",
    "fg", "grep", "head", "history", "history-clear", "jobs", "kill", "mkcd", "nvs-about", "nvs-help", "nvs-history", "prompt", "pwd", "quit",
    "set", "source", "sshm", "sudo", "tail", "time", "title", "touch", "unalias", "unset", "wc", "which", NULL
};

typedef struct LineEditor {
    char text[INPUT_CAP];
    size_t len, pos;
    size_t drawn;                   /* characters currently on screen (to blank leftovers) */
    COORD origin;                   /* console cell where the editable text starts */
    size_t history_cursor;
    char scratch[INPUT_CAP];        /* what the user was typing before browsing history */
    BOOL browsing_history;
    char **matches;                 /* Tab-completion candidates */
    size_t match_count, match_index;
    char tab_text[INPUT_CAP];       /* line contents right after the last completion */
    char head[INPUT_CAP];           /* text before the fragment being completed */
    char fragment[INPUT_CAP];       /* the fragment being completed */
    BOOL no_ghost;                  /* suppress the history suggestion (search mode, Enter) */
    BOOL dirs_only;                 /* path completion lists directories only (cd) */
} LineEditor;

static BOOL run_command(const char *line);




static void set_color(WORD c) {
    fflush(stdout); fflush(stderr);
    SetConsoleTextAttribute(g_out, c);
}
static void reset_color(void) { set_color(g_default_attr); }

static void print_error(const char *message) {
    g_last_exit = 1;
    set_color(COLOR_ERROR); fprintf(stderr, "%s\n", message); reset_color();
}

static void print_win_error(const char *operation) {
    DWORD e = GetLastError(); char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, e, 0, (LPSTR)&msg, 0, NULL);
    g_last_exit = 1;
    set_color(COLOR_ERROR);
    fprintf(stderr, "%s failed (%lu)%s%s\n", operation, e, msg ? ": " : "", msg ? msg : "");
    reset_color();
    if (msg) LocalFree(msg);
}

static int is_space_char(char c) { return c == ' ' || c == '\t'; }

static char *trim(char *s) {
    char *end;
    while (*s && is_space_char(*s)) ++s;
    end = s + strlen(s);
    while (end > s && is_space_char(end[-1])) --end;
    *end = '\0';
    return s;
}

static int starts_with_ci(const char *s, const char *prefix) { return _strnicmp(s, prefix, strlen(prefix)) == 0; }
static BOOL is_word_boundary(char c) { return c == '\0' || is_space_char(c); }

static char *strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s[n - 1] = '\0'; return s + 1; }
    return s;
}

static BOOL match_cmd(char *cmd, const char *name, char **args) {
    size_t n = strlen(name);
    if (_strnicmp(cmd, name, n) != 0 || !is_word_boundary(cmd[n])) return FALSE;
    *args = trim(cmd + n);
    return TRUE;
}

static BOOL in_list(const char *s, const char *const *list) {
    for (; *list; ++list) if (!_stricmp(s, *list)) return TRUE;
    return FALSE;
}

static int tokenize(char *s, char **argv, int max) {
    int argc = 0;
    while (*s && argc < max) {
        while (is_space_char(*s)) ++s;
        if (!*s) break;
        if (*s == '"') { ++s; argv[argc++] = s; while (*s && *s != '"') ++s; }
        else { argv[argc++] = s; while (*s && !is_space_char(*s)) ++s; }
        if (*s) *s++ = '\0';
    }
    return argc;
}

static BOOL append_raw(char *buf, size_t cap, const char *s) {
    size_t used = strlen(buf), add = strlen(s) + (used ? 1 : 0);
    if (used + add + 1 > cap) return FALSE;
    if (used) buf[used++] = ' ';
    strcpy(buf + used, s);
    return TRUE;
}

static BOOL append_arg(char *buf, size_t cap, const char *arg) {
    if (!*arg || strpbrk(arg, " \t")) {
        char q[INPUT_CAP + 3];
        if (strlen(arg) >= INPUT_CAP) return FALSE;
        snprintf(q, sizeof(q), "\"%s\"", arg);
        return append_raw(buf, cap, q);
    }
    return append_raw(buf, cap, arg);
}

static void expand_tilde(const char *in, char *out, size_t cap) {
    char home[MAX_PATH];
    if (in[0] == '~' && (in[1] == '\0' || in[1] == '\\' || in[1] == '/') &&
        GetEnvironmentVariableA("USERPROFILE", home, sizeof(home)) > 0) {
        snprintf(out, cap, "%s%s", home, in + 1);
    } else {
        snprintf(out, cap, "%s", in);
    }
}

static BOOL ensure_parent_dir(const char *path) {
    char tmp[PATH_CAP]; size_t i;
    if (strlen(path) >= sizeof(tmp)) return FALSE;
    strcpy(tmp, path);
    for (i = 1; tmp[i]; ++i)
        if (tmp[i] == '\\' || tmp[i] == '/') { char c = tmp[i]; tmp[i] = '\0'; CreateDirectoryA(tmp, NULL); tmp[i] = c; }
    return TRUE;
}

static void config_file(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s\\%s", g_config_path, name); }

static int split_tabs(char *line, char **f, int max) {
    int n = 0; f[n++] = line;
    for (char *p = line; *p && n < max; ++p) if (*p == '\t') { *p = '\0'; f[n++] = p + 1; }
    return n;
}

static char *find_and_and(char *s) {
    BOOL quoted = FALSE;
    for (; *s; ++s) {
        if (*s == '"') quoted = !quoted;
        else if (!quoted && s[0] == '&' && s[1] == '&') return s;
    }
    return NULL;
}

static BOOL valid_token(const char *s, size_t cap, const char *allowed) {
    size_t n = strlen(s);
    if (n == 0 || n >= cap || s[0] == '-') return FALSE;
    for (; *s; ++s) if (!isalnum((unsigned char)*s) && !strchr(allowed, *s)) return FALSE;
    return TRUE;
}











static void init_paths(void) {
    char base[MAX_PATH * 2] = {0};
    DWORD n = GetEnvironmentVariableA("APPDATA", base, sizeof(base));
    if (!n || n >= sizeof(base)) GetTempPathA(sizeof(base), base);
    snprintf(g_config_path, sizeof(g_config_path), "%s\\novaShell", base);
    CreateDirectoryA(g_config_path, NULL);
    config_file(g_history_path, sizeof(g_history_path), "history.txt");
}

static int alias_find(const char *name) {
    for (size_t i = 0; i < g_alias_count; ++i) if (!_stricmp(g_aliases[i].name, name)) return (int)i;
    return -1;
}

static BOOL alias_set(const char *name, const char *command) {
    int i = alias_find(name);
    if (strlen(name) >= ALIAS_NAME_CAP || strlen(command) >= ALIAS_CMD_CAP) return FALSE;
    if (i < 0) { if (g_alias_count >= ALIAS_CAP) return FALSE; i = (int)g_alias_count++; }
    strcpy(g_aliases[i].name, name);
    strcpy(g_aliases[i].command, command);
    return TRUE;
}

static void save_aliases(void) {
    char path[PATH_CAP]; config_file(path, sizeof(path), "aliases.txt");
    FILE *f = fopen(path, "w");
    if (!f) { print_error("Could not save aliases."); return; }
    for (size_t i = 0; i < g_alias_count; ++i) fprintf(f, "%s=%s\n", g_aliases[i].name, g_aliases[i].command);
    fclose(f);
}

static void load_aliases(void) {
    char path[PATH_CAP]; config_file(path, sizeof(path), "aliases.txt");
    FILE *f = fopen(path, "r");
    if (!f) {
        static const char *defaults[][2] = {
            {"ls", "dir"}, {"list", "dir"}, {"ll", "dir"}, {"clr", "clear"}, {"..", "cd .."},
            {"...", "cd ..\\.."}, {"cp", "copy"}, {"mv", "move"}, {"rm", "del"}
        };
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) alias_set(defaults[i][0], defaults[i][1]);
        save_aliases();
        return;
    }
    char line[ALIAS_CMD_CAP + ALIAS_NAME_CAP + 8];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq++ = '\0';
        char *name = trim(line), *command = trim(eq);
        if (*name && *command) alias_set(name, command);
    }
    fclose(f);
    /* 1.3 migration: "cat" and "grep" used to default to cmd.exe's type/findstr; they are real built-ins now. 
    don't worry about it. */
    static const char *const stale[][2] = {{"cat", "type"}, {"grep", "findstr"}};
    for (int k = 0; k < 2; ++k) {
        int i = alias_find(stale[k][0]);
        if (i >= 0 && !_stricmp(g_aliases[i].command, stale[k][1])) {
            memmove(&g_aliases[i], &g_aliases[i + 1], (g_alias_count - (size_t)i - 1) * sizeof(Alias));
            --g_alias_count;
            save_aliases();
        }
    }
}

static void history_push(const char *line) {
    if (g_history_count == HISTORY_CAP) {
        memmove(g_history, g_history + 1, (HISTORY_CAP - 1) * sizeof(g_history[0]));
        --g_history_count;
    }
    strncpy(g_history[g_history_count], line, INPUT_CAP - 1);
    g_history[g_history_count][INPUT_CAP - 1] = '\0';
    ++g_history_count;
}

static void history_load(void) {
    FILE *f = fopen(g_history_path, "r"); char line[INPUT_CAP];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (*line) history_push(line);
    }
    fclose(f);
}

static void history_save(void) {
    ensure_parent_dir(g_history_path);
    FILE *f = fopen(g_history_path, "w"); if (!f) return;
    for (size_t i = 0; i < g_history_count; ++i) fprintf(f, "%s\n", g_history[i]);
    fclose(f);
}

static void history_add(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (g_history_count && strcmp(g_history[g_history_count - 1], cmd) == 0) return;   /* skip repeats */
    history_push(cmd);
    history_save();
}

static BOOL expand_history(char *line, size_t cap) {
    size_t idx;
    if (line[0] != '!') return TRUE;
    if (!strcmp(line, "!!")) {
        if (!g_history_count) { print_error("History is empty."); return FALSE; }
        idx = g_history_count - 1;
    } else {
        char *end; unsigned long v = strtoul(line + 1, &end, 10);
        if (end == line + 1 || *end) return TRUE;               /* not MY syntax */
        if (v < 1 || v > g_history_count) { print_error("No such history entry."); return FALSE; }
        idx = (size_t)v - 1;
    }
    snprintf(line, cap, "%s", g_history[idx]);
    set_color(COLOR_ACCENT); printf("%s\n", line); reset_color();
    return TRUE;
}

static void save_ssh(void) {
    char path[PATH_CAP]; config_file(path, sizeof(path), "ssh_hosts.txt");
    FILE *f = fopen(path, "w");
    if (!f) { print_error("Could not save SSH profiles."); return; }
    for (size_t i = 0; i < g_ssh_count; ++i) {
        const SshHost *h = &g_ssh[i];
        fprintf(f, "%s\t%s\t%s\t%d\t%s\t%s\n", h->name, h->user, h->host, h->port, h->key, h->extra);
    }
    fclose(f);
}

static void load_ssh(void) {
    char path[PATH_CAP]; config_file(path, sizeof(path), "ssh_hosts.txt");
    FILE *f = fopen(path, "r"); char line[1400];
    int bad = 0;
    if (!f) return;
    while (g_ssh_count < SSH_CAP && fgets(line, sizeof(line), f)) {
        char *fld[6] = {"", "", "", "", "", ""};
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        if (split_tabs(line, fld, 6) < 3) { ++bad; continue; }
        SshHost h; memset(&h, 0, sizeof(h));
        snprintf(h.name, sizeof(h.name), "%s", fld[0]);
        snprintf(h.user, sizeof(h.user), "%s", fld[1]);
        snprintf(h.host, sizeof(h.host), "%s", fld[2]);
        h.port = atoi(fld[3]);
        snprintf(h.key, sizeof(h.key), "%s", fld[4]);
        snprintf(h.extra, sizeof(h.extra), "%s", fld[5]);
        if (!valid_token(h.name, sizeof(h.name), "._-") || !valid_token(h.host, sizeof(h.host), "._-:[]%")) { ++bad; continue; }
        if (*h.user && !valid_token(h.user, sizeof(h.user), "._-")) { ++bad; continue; }
        g_ssh[g_ssh_count++] = h;
    }
    fclose(f);
    if (bad && !g_exec_mode) {
        set_color(COLOR_WARN);
        printf("ssh_hosts.txt: ignored %d unusable line(s) (bad name, host or user).\n", bad);
        reset_color();
    }
}

static void save_bookmarks(void) {
    char path[PATH_CAP]; config_file(path, sizeof(path), "bookmarks.txt");
    FILE *f = fopen(path, "w");
    if (!f) { print_error("Could not save bookmarks."); return; }
    for (size_t i = 0; i < g_bm_count; ++i) fprintf(f, "%s\t%s\n", g_bm[i].name, g_bm[i].path);
    fclose(f);
}

static void load_bookmarks(void) {
    char path[PATH_CAP]; config_file(path, sizeof(path), "bookmarks.txt");
    FILE *f = fopen(path, "r"); char line[MAX_PATH + 100];
    if (!f) return;
    while (g_bm_count < BM_CAP && fgets(line, sizeof(line), f)) {
        char *fld[2] = {"", ""};
        line[strcspn(line, "\r\n")] = '\0';
        if (split_tabs(line, fld, 2) < 2 || !*fld[0] || !*fld[1]) continue;
        snprintf(g_bm[g_bm_count].name, sizeof(g_bm[0].name), "%s", fld[0]);
        snprintf(g_bm[g_bm_count].path, sizeof(g_bm[0].path), "%s", fld[1]);
        ++g_bm_count;
    }
    fclose(f);
}

static BOOL run_script_file(const char *path) {
    FILE *f = fopen(path, "r"); char line[INPUT_CAP];
    if (!f) return FALSE;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *cmd = trim(line);
        if (*cmd && *cmd != '#') run_command(cmd);
    }
    fclose(f);
    return TRUE;
}

/* %APPDATA%\novaShell\startup.txt runs once at launch. */
static void run_startup_script(void) {
    char path[PATH_CAP]; config_file(path, sizeof(path), "startup.txt");
    run_script_file(path);
    g_last_exit = 0;
}

static unsigned long long fnv1a_file(const char *path) {
    FILE *f = fopen(path, "rb"); int c; unsigned long long h = 14695981039346656037ULL;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
    fclose(f);
    return h ? h : 1;
}

static BOOL rc_is_trusted(const char *path, unsigned long long hash) {
    char tp[PATH_CAP], line[PATH_CAP + 40]; BOOL ok = FALSE;
    config_file(tp, sizeof(tp), "trusted_rc.txt");
    FILE *f = fopen(tp, "r");
    if (!f) return FALSE;
    while (!ok && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        ok = strtoull(line, NULL, 16) == hash && !_stricmp(tab + 1, path);
    }
    fclose(f);
    return ok;
}

static void rc_trust(const char *path, unsigned long long hash) {
    char tp[PATH_CAP]; config_file(tp, sizeof(tp), "trusted_rc.txt");
    FILE *f = fopen(tp, "a");
    if (f) { fprintf(f, "%llx\t%s\n", hash, path); fclose(f); }
}

static void maybe_run_novarc(void) {
    static int depth = 0; /* because a .novarc may cd into another folder with a .novarc */
    char cwd[PATH_CAP], rc[PATH_CAP + 16];
    if (depth >= 3 || g_exec_mode || GetEnvironmentVariableA("NOVA_NORC", NULL, 0) > 0) return;
    if (!GetCurrentDirectoryA(sizeof(cwd), cwd)) return;
    snprintf(rc, sizeof(rc), "%s\\.novarc", cwd);
    if (GetFileAttributesA(rc) == INVALID_FILE_ATTRIBUTES) return;
    unsigned long long hash = fnv1a_file(rc);
    if (!hash) return;

    if (!rc_is_trusted(rc, hash)) {
        if (!_isatty(_fileno(stdin))) return;
        FILE *f = fopen(rc, "r"); char line[512]; int shown = 0;
        set_color(COLOR_DIM); printf("---- %s ----\n", rc);
        while (f && shown < 20 && fgets(line, sizeof(line), f)) { line[strcspn(line, "\r\n")] = '\0'; puts(line); ++shown; }
        if (f && fgets(line, sizeof(line), f)) printf("... (more)\n");
        if (f) fclose(f);
        printf("----\n");
        set_color(COLOR_WARN); printf("Run this .novarc?  [y] once   [a] always (this version)   [N] no: "); reset_color();
        int ch = _getch();
        printf("%c\n", (ch >= 32 && ch < 127) ? ch : ' ');
        if (ch == 'a' || ch == 'A') rc_trust(rc, hash);
        else if (ch != 'y' && ch != 'Y') return;
    }
    DWORD saved = g_last_exit;
    ++depth; run_script_file(rc); --depth;
    g_last_exit = saved;
}







static BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_cancel_requested, 1);
        return TRUE;
    }
    return FALSE;
}

static BOOL find_git_dir(char *out, size_t cap) {
    char dir[PATH_CAP], gitpath[PATH_CAP + 8], line[PATH_CAP];
    if (!GetCurrentDirectoryA(sizeof(dir), dir)) return FALSE;
    for (;;) {
        snprintf(gitpath, sizeof(gitpath), "%s\\.git", dir);
        DWORD attr = GetFileAttributesA(gitpath);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (attr & FILE_ATTRIBUTE_DIRECTORY) { snprintf(out, cap, "%s", gitpath); return TRUE; }
            FILE *gf = fopen(gitpath, "r");
            if (!gf) return FALSE;
            BOOL got = fgets(line, sizeof(line), gf) != NULL;
            fclose(gf);
            if (!got || !starts_with_ci(line, "gitdir:")) return FALSE;
            line[strcspn(line, "\r\n")] = '\0';
            char *g = trim(line + 7);
            if (g[0] && g[1] == ':') snprintf(out, cap, "%s", g); /* absolute */
            else snprintf(out, cap, "%s\\%s", dir, g); /* relative */
            return TRUE;
        }
        char *slash = strrchr(dir, '\\');
        if (!slash || slash < dir + 2) return FALSE;
        if (slash == dir + 2) { if (dir[3] == '\0') return FALSE; dir[3] = '\0'; }
        else *slash = '\0';
    }
}

static BOOL git_branch(char *out, size_t cap) {
    char gitdir[PATH_CAP], headpath[PATH_CAP + 16], line[PATH_CAP];
    out[0] = '\0';
    if (!find_git_dir(gitdir, sizeof(gitdir))) return FALSE;
    snprintf(headpath, sizeof(headpath), "%s\\HEAD", gitdir);
    FILE *hf = fopen(headpath, "r");
    if (!hf) return FALSE;
    BOOL ok = fgets(line, sizeof(line), hf) != NULL;
    fclose(hf);
    if (!ok) return FALSE;
    line[strcspn(line, "\r\n")] = '\0';
    if (starts_with_ci(line, "ref:")) {
        char *ref = trim(line + 4);
        if (starts_with_ci(ref, "refs/heads/")) ref += 11;
        snprintf(out, cap, "%s", ref);
    } else {
        snprintf(out, cap, "%.7s", line);
    }
    return TRUE;
}

static void update_prompt(void) {
    g_git_branch[0] = '\0';
    char cwd[PATH_CAP], home[MAX_PATH];
    DWORD n = GetCurrentDirectoryA(sizeof(cwd), cwd);
    if (!n || n >= sizeof(cwd)) { strcpy(g_cwd_display, ""); return; }
    DWORD h = GetEnvironmentVariableA("USERPROFILE", home, sizeof(home));
    if (h && h < sizeof(home) && _strnicmp(cwd, home, h) == 0 && (cwd[h] == '\0' || cwd[h] == '\\'))
        snprintf(g_cwd_display, sizeof(g_cwd_display), "~%s", cwd + h);
    else
        snprintf(g_cwd_display, sizeof(g_cwd_display), "%s", cwd);
    if (GetEnvironmentVariableA("NOVA_NOGIT", NULL, 0) == 0) git_branch(g_git_branch, sizeof(g_git_branch));
}

static BOOL prompt_token(const char *tok) {
    char buf[128];
    if (!strcmp(tok, "code")) {
        if (g_last_exit) {
            set_color(COLOR_ERROR);
            if (g_last_exit > 0xFFFF) printf("[0x%lX] ", (unsigned long)g_last_exit);
            else printf("[%lu] ", (unsigned long)g_last_exit);
            set_color(COLOR_PROMPT);
        }
    }
    else if (!strcmp(tok, "cwd")) printf("%s", g_cwd_display);
    else if (!strcmp(tok, "git")) {
        if (g_git_branch[0]) { set_color(COLOR_ACCENT); printf(" (%s)", g_git_branch); set_color(COLOR_PROMPT); }
    }
    else if (!strcmp(tok, "time")) { SYSTEMTIME st; GetLocalTime(&st); printf("%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond); }
    else if (!strcmp(tok, "user")) { if (GetEnvironmentVariableA("USERNAME", buf, sizeof(buf))) printf("%s", buf); }
    else if (!strcmp(tok, "host")) { if (GetEnvironmentVariableA("COMPUTERNAME", buf, sizeof(buf))) printf("%s", buf); }
    else if (!strcmp(tok, "jobs")) { int n = job_running_count(); if (n) printf(" [%d bg]", n); }
    else if (!strcmp(tok, "nl")) printf("\n");
    else return FALSE;
    return TRUE;
}

static void print_prompt(void) {
    set_color(COLOR_PROMPT);
    for (const char *f = g_prompt_fmt; *f; ) {
        if (*f == '{') {
            const char *close = strchr(f, '}');
            if (close && close - f <= 8) {
                char tok[10]; size_t tl = (size_t)(close - f) - 1;
                memcpy(tok, f + 1, tl); tok[tl] = '\0';
                if (prompt_token(tok)) { f = close + 1; continue; }
            }
        }
        putchar(*f++);
    }
    fflush(stdout);
}

static int console_width(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    return GetConsoleScreenBufferInfo(g_out, &info) ? info.dwSize.X : 80;
}


static const char *ghost_find(const char *text, size_t len) {
    if (!len) return NULL;
    for (size_t i = g_history_count; i-- > 0; )
        if (strncmp(g_history[i], text, len) == 0 && g_history[i][len] != '\0') return g_history[i];
    return NULL;
}

static void redraw_line(LineEditor *e) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(g_out, &info)) return;
    long width = info.dwSize.X;

    const char *ghost = NULL; size_t glen = 0;
    if (!e->no_ghost && e->pos == e->len && !e->browsing_history) {
        const char *full = ghost_find(e->text, e->len);
        if (full) { ghost = full + e->len; glen = strlen(ghost); if (glen > 200) glen = 200; }
    }
    size_t total = e->len + glen;
    size_t pad = e->drawn > total ? e->drawn - total : 0;

    SetConsoleCursorPosition(g_out, e->origin);
    printf("%s", e->text);
    if (glen) { set_color(COLOR_DIM); printf("%.*s", (int)glen, ghost); set_color(COLOR_PROMPT); }
    printf("%*s", (int)pad, "");
    fflush(stdout);

    GetConsoleScreenBufferInfo(g_out, &info);
    long expected = (long)e->origin.Y * width + e->origin.X + (long)(total + pad);
    long actual = (long)info.dwCursorPosition.Y * width + info.dwCursorPosition.X;
    if (actual < expected) e->origin.Y = (SHORT)(e->origin.Y - (expected - actual) / width);

    long target = (long)e->origin.Y * width + e->origin.X + (long)e->pos;
    COORD c; c.X = (SHORT)(target % width); c.Y = (SHORT)(target / width);
    SetConsoleCursorPosition(g_out, c);
    e->drawn = total;
}

static void editor_replace(LineEditor *e, const char *s) {
    strncpy(e->text, s ? s : "", sizeof(e->text) - 1); e->text[sizeof(e->text) - 1] = '\0';
    e->len = e->pos = strlen(e->text);
    redraw_line(e);
}

static BOOL accept_ghost(LineEditor *e) {
    if (e->no_ghost || e->pos != e->len) return FALSE;
    const char *full = ghost_find(e->text, e->len);
    if (!full) return FALSE;
    editor_replace(e, full);
    return TRUE;
}





static void free_matches(LineEditor *e) {
    for (size_t i = 0; i < e->match_count; ++i) free(e->matches[i]);
    free(e->matches);
    e->matches = NULL; e->match_count = e->match_index = 0;
}

static BOOL add_match(LineEditor *e, const char *s) {
    for (size_t i = 0; i < e->match_count; ++i) if (!_stricmp(e->matches[i], s)) return TRUE;   /* de-duplicate */
    char **next = (char **)realloc(e->matches, (e->match_count + 1) * sizeof(char *));
    if (!next) return FALSE;
    e->matches = next;
    e->matches[e->match_count] = _strdup(s);
    if (!e->matches[e->match_count]) return FALSE;
    ++e->match_count;
    return TRUE;
}

static void add_candidate_ex(LineEditor *e, const char *name, const char *suffix) {
    char cand[INPUT_CAP];
    if (!starts_with_ci(name, e->fragment)) return;
    if (strlen(e->head) + strlen(name) + strlen(suffix) + 1 >= sizeof(cand)) return;
    strcpy(cand, e->head); strcat(cand, name); strcat(cand, suffix);
    add_match(e, cand);
}

static void add_candidate(LineEditor *e, const char *name, BOOL is_dir) { add_candidate_ex(e, name, is_dir ? "\\" : ""); }

static void collect_path_matches(LineEditor *e, const char *dirpart) {
    char dir[PATH_CAP], tmp[PATH_CAP], search[PATH_CAP + 2];
    expand_tilde(dirpart, tmp, sizeof(tmp));
    DWORD en = ExpandEnvironmentStringsA(tmp, dir, sizeof(dir)); /* %USERPROFILE%\Doc<Tab> */
    if (!en || en > sizeof(dir)) snprintf(dir, sizeof(dir), "%s", tmp);
    snprintf(search, sizeof(search), "%s*", dir); /* dirpart is "" or ends in a separator */
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        BOOL is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!strcmp(fd.cFileName, ".")) continue;
        if (fd.cFileName[0] == '.' && e->fragment[0] != '.') continue;
        if (e->dirs_only && !is_dir) continue;
        add_candidate(e, fd.cFileName, is_dir);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void collect_command_matches(LineEditor *e) {
    for (int i = 0; g_builtins[i]; ++i) add_candidate(e, g_builtins[i], FALSE);
    for (size_t i = 0; i < g_alias_count; ++i) add_candidate(e, g_aliases[i].name, FALSE);

    DWORD need = GetEnvironmentVariableA("PATH", NULL, 0);
    if (!need) return;
    char *path = (char *)malloc(need);
    if (!path) return;
    GetEnvironmentVariableA("PATH", path, need);
    static const char *patterns[] = {"*.exe", "*.bat", "*.cmd"};
    char *ctx = NULL;
    for (char *dir = strtok_s(path, ";", &ctx); dir; dir = strtok_s(NULL, ";", &ctx)) {
        for (int p = 0; p < 3; ++p) {
            char search[PATH_CAP]; WIN32_FIND_DATAA fd;
            snprintf(search, sizeof(search), "%s\\%s", dir, patterns[p]);
            HANDLE h = FindFirstFileA(search, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                char *dot = strrchr(fd.cFileName, '.');
                if (dot) *dot = '\0';
                add_candidate(e, fd.cFileName, FALSE);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    free(path);
}

static void collect_env_names(LineEditor *e, const char *suffix) {
    char *block = GetEnvironmentStringsA();
    if (!block) return;
    for (char *p = block; *p; p += strlen(p) + 1) {
        char name[256], *eq = strchr(p, '=');
        if (*p == '=' || !eq || (size_t)(eq - p) >= sizeof(name)) continue;
        memcpy(name, p, (size_t)(eq - p)); name[eq - p] = '\0';
        add_candidate_ex(e, name, suffix);
    }
    FreeEnvironmentStringsA(block);
}

static const char *const g_git_subs[] = {
    "add", "bisect", "branch", "checkout", "cherry-pick", "clone", "commit", "diff", "fetch", "init", "log", "merge",
    "mv", "pull", "push", "rebase", "reset", "restore", "revert", "rm", "show", "stash", "status", "switch", "tag", NULL
};
static const char *const g_git_ref_subs[] = {
    "checkout", "switch", "merge", "rebase", "branch", "cherry-pick", "diff", "log", "reset", "show", NULL
};

static void git_collect_refs(LineEditor *e, const char *dir, const char *prefix, int depth) {
    char search[PATH_CAP]; WIN32_FIND_DATAA fd;
    snprintf(search, sizeof(search), "%s\\*", dir);
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char name[256]; snprintf(name, sizeof(name), "%s%s", prefix, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 6) {
                char sub[PATH_CAP], next[256];
                snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
                snprintf(next, sizeof(next), "%s/", name);
                git_collect_refs(e, sub, next, depth + 1);
            }
        } else {
            add_candidate(e, name, FALSE);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void collect_git_refs(LineEditor *e) {
    char gitdir[PATH_CAP], common[PATH_CAP], path[PATH_CAP + 24], line[PATH_CAP];
    if (!find_git_dir(gitdir, sizeof(gitdir))) return;
    snprintf(common, sizeof(common), "%s", gitdir);
    snprintf(path, sizeof(path), "%s\\commondir", gitdir);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *c = trim(line);
            if (c[0] && c[1] == ':') snprintf(common, sizeof(common), "%s", c);
            else if (c[0]) snprintf(common, sizeof(common), "%s\\%s", gitdir, c);
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s\\refs\\heads", common);   git_collect_refs(e, path, "", 0);
    snprintf(path, sizeof(path), "%s\\refs\\remotes", common); git_collect_refs(e, path, "", 0);
    snprintf(path, sizeof(path), "%s\\refs\\tags", common);    git_collect_refs(e, path, "", 0);
    snprintf(path, sizeof(path), "%s\\packed-refs", common);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == '^') continue;
            line[strcspn(line, "\r\n")] = '\0';
            char *sp = strchr(line, ' ');
            if (!sp) continue;
            const char *ref = sp + 1;
            if (starts_with_ci(ref, "refs/heads/")) add_candidate(e, ref + 11, FALSE);
            else if (starts_with_ci(ref, "refs/remotes/")) add_candidate(e, ref + 13, FALSE);
            else if (starts_with_ci(ref, "refs/tags/")) add_candidate(e, ref + 10, FALSE);
        }
        fclose(f);
    }
}


static void collect_ssh_hosts(LineEditor *e) {
    char path[PATH_CAP], line[2048], *ctx = NULL;
    char *at = strrchr(e->fragment, '@');
    if (at) {
        size_t k = (size_t)(at + 1 - e->fragment);
        if (strlen(e->head) + k < sizeof(e->head)) {
            strncat(e->head, e->fragment, k);
            memmove(e->fragment, at + 1, strlen(at + 1) + 1);
        }
    }
    expand_tilde("~\\.ssh\\known_hosts", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '|' || line[0] == '@' || line[0] == '#') continue;
            line[strcspn(line, " \t\r\n")] = '\0';
            for (char *tok = strtok_s(line, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
                if (tok[0] == '[') { char *rb = strchr(tok, ']'); if (rb) { *rb = '\0'; ++tok; } }
                if (strpbrk(tok, "*?!")) continue;
                add_candidate(e, tok, FALSE);
            }
        }
        fclose(f);
    }
    expand_tilde("~\\.ssh\\config", path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *p = trim(line);
            if (_strnicmp(p, "Host", 4) != 0 || !is_space_char(p[4])) continue;
            char *tok[16]; int n = tokenize(p + 4, tok, 16);
            for (int i = 0; i < n; ++i) if (!strpbrk(tok[i], "*?!")) add_candidate(e, tok[i], FALSE);
        }
        fclose(f);
    }
}

static BOOL collect_name_matches(LineEditor *e, int n, char **av) {
    static const char *const sshm_subs[] = {"add", "list", "show", "rm", "put", "get", "connect", "test", "tunnel", "copy-id", "import", "rename", "last", NULL};
    static const char *const sshm_take_name[] = {"connect", "show", "rm", "remove", "del", "put", "get", "test", "tunnel", "copy-id", "rename", NULL};
    static const char *const bm_subs[] = {"add", "list", "rm", NULL};
    if (!_stricmp(av[0], "sshm")) {
        if (n == 1) {
            for (int i = 0; sshm_subs[i]; ++i) add_candidate(e, sshm_subs[i], FALSE);
            for (size_t i = 0; i < g_ssh_count; ++i) add_candidate(e, g_ssh[i].name, FALSE);
            return TRUE;
        }
        if (n == 2 && !_stricmp(av[1], "add")) return TRUE;
        if (n == 2 && in_list(av[1], sshm_take_name)) {
            for (size_t i = 0; i < g_ssh_count; ++i) add_candidate(e, g_ssh[i].name, FALSE);
            return TRUE;
        }
        return FALSE;
    }
    if (!_stricmp(av[0], "bm")) {
        if (n == 1 || (n == 2 && !_stricmp(av[1], "rm"))) {
            if (n == 1) for (int i = 0; bm_subs[i]; ++i) add_candidate(e, bm_subs[i], FALSE);
            for (size_t i = 0; i < g_bm_count; ++i) add_candidate(e, g_bm[i].name, FALSE);
            return TRUE;
        }
        return FALSE;
    }
    if ((!_stricmp(av[0], "unalias") || !_stricmp(av[0], "alias")) && n == 1) {
        for (size_t i = 0; i < g_alias_count; ++i) add_candidate(e, g_aliases[i].name, FALSE);
        return TRUE;
    }
    if (!_stricmp(av[0], "git")) {
        if (n == 1) { for (int i = 0; g_git_subs[i]; ++i) add_candidate(e, g_git_subs[i], FALSE); return TRUE; }
        if (in_list(av[1], g_git_ref_subs)) { collect_git_refs(e); return FALSE; }
        return FALSE;
    }
    if ((!_stricmp(av[0], "ssh") || !_stricmp(av[0], "sftp")) && n == 1 && e->fragment[0] != '-') {
        collect_ssh_hosts(e);
        return TRUE;
    }
    if (!_stricmp(av[0], "scp")) { collect_ssh_hosts(e); return FALSE; }
    if ((!_stricmp(av[0], "export") || !_stricmp(av[0], "set")) && n == 1) { collect_env_names(e, "="); return TRUE; }
    if (!_stricmp(av[0], "unset") && n == 1) { collect_env_names(e, ""); return TRUE; }
    return FALSE;
}

static int cmp_ci(const void *a, const void *b) { return _stricmp(*(char *const *)a, *(char *const *)b); }

static void apply_completion(LineEditor *e, const char *text) {
    editor_replace(e, text);
    snprintf(e->tab_text, sizeof(e->tab_text), "%s", e->text);
}

static void complete_input(LineEditor *e) {
    if (e->match_count > 1 && strcmp(e->text, e->tab_text) == 0) {
        apply_completion(e, e->matches[e->match_index]);
        e->match_index = (e->match_index + 1) % e->match_count;
        return;
    }
    free_matches(e);
    e->dirs_only = FALSE;

    char *space = strrchr(e->text, ' ');
    char *wstart = space ? space + 1 : e->text;
    char *word = (*wstart == '"') ? wstart + 1 : wstart;

    char before[INPUT_CAP]; size_t blen = (size_t)(wstart - e->text);
    memcpy(before, e->text, blen); before[blen] = '\0';
    char *av[32]; int n = tokenize(before, av, 32);

    char *sep = strrchr(word, '\\'), *sep2 = strrchr(word, '/');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
    if (n >= 2 && !_stricmp(av[0], "git") && in_list(av[1], g_git_ref_subs)) sep = NULL;
    size_t pct = 0; for (const char *q = word; *q; ++q) if (*q == '%') ++pct;
    BOOL env_ctx = (pct % 2) == 1;
    char *frag = env_ctx ? strrchr(word, '%') + 1 : (sep ? sep + 1 : word);

    size_t head_len = (size_t)(frag - e->text);
    memcpy(e->head, e->text, head_len); e->head[head_len] = '\0';
    snprintf(e->fragment, sizeof(e->fragment), "%s", frag);
    e->dirs_only = n >= 1 && (!_stricmp(av[0], "cd") || !_stricmp(av[0], "pushd"));

    BOOL handled = FALSE;
    if (env_ctx) { collect_env_names(e, "%"); handled = TRUE; }
    else if (!sep) {
        if (n == 0) { if (*e->fragment) collect_command_matches(e); handled = TRUE; }
        else handled = collect_name_matches(e, n, av);
    }
    if (!handled) {
        char dirpart[PATH_CAP]; size_t dl = (size_t)(frag - word);
        if (dl >= sizeof(dirpart)) return;
        memcpy(dirpart, word, dl); dirpart[dl] = '\0';
        collect_path_matches(e, dirpart);
    }

    if (!e->match_count) return;
    qsort(e->matches, e->match_count, sizeof(char *), cmp_ci);
    if (e->match_count == 1) { apply_completion(e, e->matches[0]); return; }

    size_t common = strlen(e->matches[0]);
    for (size_t i = 1; i < e->match_count; ++i) {
        size_t k = 0;
        while (k < common && tolower((unsigned char)e->matches[0][k]) == tolower((unsigned char)e->matches[i][k])) ++k;
        common = k;
    }
    if (common > e->len) {
        char prefix[INPUT_CAP]; snprintf(prefix, sizeof(prefix), "%.*s", (int)common, e->matches[0]);
        editor_replace(e, prefix);
    }
    snprintf(e->tab_text, sizeof(e->tab_text), "%s", e->text);
    e->match_index = 0;

    size_t colw = 0;
    for (size_t i = 0; i < e->match_count; ++i) if (strlen(e->matches[i]) > colw) colw = strlen(e->matches[i]);
    colw += 2;
    size_t cols = (size_t)console_width() / colw; if (!cols) cols = 1;
    printf("\n");
    for (size_t i = 0; i < e->match_count; ++i)
        printf("%-*s%s", (int)colw, e->matches[i], ((i + 1) % cols == 0 || i + 1 == e->match_count) ? "\n" : "");

    print_prompt();
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_out, &info)) e->origin = info.dwCursorPosition;
    e->drawn = 0;
    redraw_line(e);
}

static BOOL contains_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return TRUE;
    for (; *hay; ++hay) if (_strnicmp(hay, needle, n) == 0) return TRUE;
    return FALSE;
}

static size_t history_find(const char *query, size_t from) {
    for (size_t i = from; i-- > 0; ) if (contains_ci(g_history[i], query)) return i;
    return g_history_count;
}

static void history_search(LineEditor *e) {
    char query[128] = "", saved[INPUT_CAP], label[INPUT_CAP + 160];
    size_t qlen = 0, hit = g_history_count;
    snprintf(saved, sizeof(saved), "%s", e->text);
    for (;;) {
        snprintf(label, sizeof(label), "(search '%s'): %s", query, hit < g_history_count ? g_history[hit] : "");
        editor_replace(e, label);
        int ch = _getch();
        if (ch == 27 || ch == 7 || ch == 3) { editor_replace(e, saved); return; }
        if (ch == '\r' || ch == 0 || ch == 0xE0) {
            if (ch == 0 || ch == 0xE0) _getch();
            editor_replace(e, hit < g_history_count ? g_history[hit] : saved);
            e->browsing_history = FALSE; e->history_cursor = g_history_count;
            return;
        }
        if (ch == 18) { if (qlen && hit > 0) hit = history_find(query, hit < g_history_count ? hit : g_history_count); }  /* older hit */
        else if (ch == 8) { if (qlen) { query[--qlen] = '\0'; hit = qlen ? history_find(query, g_history_count) : g_history_count; } }
        else if (ch >= 32 && ch < 256 && qlen + 1 < sizeof(query)) { query[qlen++] = (char)ch; query[qlen] = '\0'; hit = history_find(query, g_history_count); }
    }
}

static BOOL read_line(char *out, size_t cap) {
    if (!_isatty(_fileno(stdin))) {
        if (!fgets(out, (int)cap, stdin)) return FALSE;
        out[strcspn(out, "\r\n")] = '\0';
        return TRUE;
    }
    LineEditor e; memset(&e, 0, sizeof(e));
    e.history_cursor = g_history_count;
    print_prompt();
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_out, &info)) e.origin = info.dwCursorPosition;

    for (;;) {
        int ch = _getch();
        if (ch == '\r') {
            e.pos = e.len; e.no_ghost = TRUE; redraw_line(&e); printf("\n");
            strncpy(out, e.text, cap - 1); out[cap - 1] = '\0';
            free_matches(&e);
            return TRUE;
        }
        if (ch == 3) { printf("^C\n"); out[0] = '\0'; free_matches(&e); return TRUE; }
        if (ch == 4 && e.len == 0) { printf("\n"); free_matches(&e); return FALSE; }
        if (ch == 21 || ch == 27) { e.text[0] = '\0'; e.len = e.pos = 0; redraw_line(&e); continue; }
        if (ch == 18) { e.no_ghost = TRUE; history_search(&e); e.no_ghost = FALSE; redraw_line(&e); continue; }
        if (ch == 1) { e.pos = 0; redraw_line(&e); continue; }
        if (ch == 5) { if (e.pos == e.len) accept_ghost(&e); else { e.pos = e.len; redraw_line(&e); } continue; }
        if (ch == 23) {
            size_t s = e.pos;
            while (s && e.text[s - 1] == ' ') --s;
            while (s && e.text[s - 1] != ' ') --s;
            memmove(e.text + s, e.text + e.pos, e.len - e.pos + 1);
            e.len -= e.pos - s; e.pos = s;
            free_matches(&e); redraw_line(&e);
            continue;
        }
        if (ch == 8) {
            if (e.pos) { memmove(e.text + e.pos - 1, e.text + e.pos, e.len - e.pos + 1); --e.pos; --e.len; redraw_line(&e); }
            continue;
        }
        if (ch == '\t') { complete_input(&e); continue; }
        if (ch == 0 || ch == 0xE0) {
            ch = _getch();
            if (ch == 0x48) {
                if (g_history_count && e.history_cursor > 0) {
                    if (!e.browsing_history) { strcpy(e.scratch, e.text); e.browsing_history = TRUE; }
                    --e.history_cursor; editor_replace(&e, g_history[e.history_cursor]);
                }
            } else if (ch == 0x50) {
                if (e.browsing_history && e.history_cursor + 1 < g_history_count) { ++e.history_cursor; editor_replace(&e, g_history[e.history_cursor]); }
                else if (e.browsing_history) { e.history_cursor = g_history_count; e.browsing_history = FALSE; editor_replace(&e, e.scratch); }
            }
            else if (ch == 0x4B && e.pos) { --e.pos; redraw_line(&e); }
            else if (ch == 0x4D) { if (e.pos < e.len) { ++e.pos; redraw_line(&e); } else accept_ghost(&e); }
            else if (ch == 0x47) { e.pos = 0; redraw_line(&e); }
            else if (ch == 0x4F) { if (e.pos == e.len) accept_ghost(&e); else { e.pos = e.len; redraw_line(&e); } }
            else if (ch == 0x73) {
                size_t s = e.pos;
                while (s && e.text[s - 1] == ' ') --s;
                while (s && e.text[s - 1] != ' ') --s;
                e.pos = s; redraw_line(&e);
            }
            else if (ch == 0x74) {
                size_t s = e.pos;
                while (s < e.len && e.text[s] != ' ') ++s;
                while (s < e.len && e.text[s] == ' ') ++s;
                e.pos = s; redraw_line(&e);
            }
            else if (ch == 0x53 && e.pos < e.len) {
                memmove(e.text + e.pos, e.text + e.pos + 1, e.len - e.pos); --e.len; redraw_line(&e);
            }
            continue;
        }
        if (ch >= 32 && ch <= 255 && e.len + 1 < sizeof(e.text)) {
            memmove(e.text + e.pos + 1, e.text + e.pos, e.len - e.pos + 1);
            e.text[e.pos++] = (char)ch; ++e.len;
            free_matches(&e);
            redraw_line(&e);
        }
    }
}





static BOOL find_executable(const char *name, char *out, size_t cap) {
    if (!name || !*name) return FALSE;
    DWORD n = SearchPathA(NULL, name, NULL, (DWORD)cap, out, NULL);
    if (n && n < cap) return TRUE;
    const char *exts[] = {".exe", ".com", ".bat", ".cmd", NULL};
    for (int i = 0; exts[i]; ++i) {
        char candidate[INPUT_CAP];
        if (strlen(name) + strlen(exts[i]) >= sizeof(candidate)) continue;
        strcpy(candidate, name); strcat(candidate, exts[i]);
        n = SearchPathA(NULL, candidate, NULL, (DWORD)cap, out, NULL);
        if (n && n < cap) return TRUE;
    }
    return FALSE;
}


static DWORD run_process_ex(char *cmdline, HANDLE input) {
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); memset(&pi, 0, sizeof(pi)); si.cb = sizeof(si);
    if (input) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = input;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    InterlockedExchange(&g_cancel_requested, 0);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { print_win_error("CreateProcess"); return 1; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code;
}

static DWORD run_process(char *cmdline) { return run_process_ex(cmdline, NULL); }

/* Printed after every external command: "Process exited with code N (Meaning)".
 * Success is green, errors and Ctrl+C red, and any other code (no known meaning) yellow.
 * To teach it a new code, add a case below. */
static void print_exit_status(DWORD code) {
    const char *meaning = NULL; WORD color = COLOR_WARN;
    switch (code) {
        case 0:             meaning = "Success";                    color = COLOR_EXE;   break;
        case 1:             meaning = "General Error";              color = COLOR_ERROR; break;
        case EXIT_CTRL_C:   meaning = "Process terminated by CTRL-C"; color = COLOR_ERROR; break;
        default: break;
    }
    set_color(color);
    if (code > 0xFFFF) printf("Process exited with code 0x%lX", (unsigned long)code);
    else printf("Process exited with code %lu", (unsigned long)code);
    if (meaning) printf(" (%s)", meaning);
    printf("\n");
    reset_color();
}

static BOOL quote_crt_arg(char *out, size_t cap, const char *arg) {
    size_t o = 0;
#define PUTC(c) do { if (o + 2 >= cap) return FALSE; out[o++] = (c); } while (0)
    PUTC('"');
    for (const char *p = arg; ; ++p) {
        size_t bs = 0;
        while (*p == '\\') { ++bs; ++p; }
        if (!*p) { for (size_t k = 0; k < bs * 2; ++k) PUTC('\\'); break; }
        if (*p == '"') { for (size_t k = 0; k < bs * 2 + 1; ++k) PUTC('\\'); PUTC('"'); }
        else { for (size_t k = 0; k < bs; ++k) PUTC('\\'); PUTC(*p); }
    }
    PUTC('"');
    out[o] = '\0';
#undef PUTC
    return TRUE;
}


static const char *const g_filter_cmds[] = {"cat", "grep", "head", "tail", "wc", NULL};

static int rewrite_filters(const char *cmd, char *out, size_t cap) {
    char self[PATH_CAP + 4]; snprintf(self, sizeof(self), "\"%s\"", g_self_path);
    size_t o = 0; BOOL quoted = FALSE, at_start = TRUE, changed = FALSE;
    const char *p = cmd;
#define OUTC(c) do { if (o + 1 >= cap) return -1; out[o++] = (c); } while (0)
    while (*p) {
        if (at_start && !quoted) {
            at_start = FALSE;
            while (*p == ' ' || *p == '\t') OUTC(*p++);
            const char *w = p;
            while (*w && !strchr(" \t|&<>()\"", *w)) ++w;
            size_t wl = (size_t)(w - p);
            if (wl > 0 && wl < 8) {
                char word[8]; memcpy(word, p, wl); word[wl] = '\0';
                if (in_list(word, g_filter_cmds)) {
                    for (const char *q = self; *q; ++q) OUTC(*q);
                    for (const char *q = " --nova-exec "; *q; ++q) OUTC(*q);
                    changed = TRUE;
                }
            }
            continue;
        }
        char c = *p++;
        if (c == '"') quoted = !quoted;
        OUTC(c);
        if (!quoted && (c == '|' || c == '&')) at_start = TRUE;
    }
#undef OUTC
    out[o] = '\0';
    return changed ? 1 : 0;
}

static int edit_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    int prev[42], cur[42];
    if (la > 40 || lb > 40) return 99;
    for (size_t j = 0; j <= lb; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= la; ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; ++j) {
            int cost = tolower((unsigned char)a[i - 1]) == tolower((unsigned char)b[j - 1]) ? 0 : 1;
            int best = prev[j - 1] + cost;
            if (prev[j] + 1 < best) best = prev[j] + 1;
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;
            cur[j] = best;
        }
        memcpy(prev, cur, (lb + 1) * sizeof(int));
    }
    return prev[lb];
}

typedef struct Suggest { char name[3][64]; int dist[3]; int n; } Suggest;

// this is very semi good
static void suggest_consider(Suggest *s, const char *word, const char *cand) {
    int limit = strlen(word) <= 4 ? 1 : 2;
    int d = edit_distance(word, cand);
    if (d == 2) {
        char sw[64]; snprintf(sw, sizeof(sw), "%s", word);
        for (size_t i = 0; sw[i] && sw[i + 1]; ++i) {
            char t = sw[i]; sw[i] = sw[i + 1]; sw[i + 1] = t;
            if (!_stricmp(sw, cand)) { d = 1; break; }
            t = sw[i]; sw[i] = sw[i + 1]; sw[i + 1] = t;
        }
    }
    if (d == 0 || d > limit) return;
    for (int i = 0; i < s->n; ++i) if (!_stricmp(s->name[i], cand)) return;
    int idx;
    if (s->n < 3) idx = s->n++;
    else if (d < s->dist[2]) idx = 2;
    else return;
    snprintf(s->name[idx], sizeof(s->name[idx]), "%s", cand);
    s->dist[idx] = d;
    while (idx > 0 && s->dist[idx] < s->dist[idx - 1]) {
        char tn[64]; int td = s->dist[idx];
        memcpy(tn, s->name[idx], sizeof(tn)); memcpy(s->name[idx], s->name[idx - 1], sizeof(tn)); memcpy(s->name[idx - 1], tn, sizeof(tn));
        s->dist[idx] = s->dist[idx - 1]; s->dist[idx - 1] = td;
        --idx;
    }
}

static void suggest_command(const char *cmd) {
    static const char *const cmd_internals[] = {
        "assoc", "call", "cd", "color", "copy", "date", "del", "dir", "echo", "erase", "for", "ftype", "if", "md", "mkdir",
        "mklink", "move", "path", "pause", "popd", "pushd", "rd", "ren", "rename", "rmdir", "set", "start", "type", "ver", "vol", NULL
    };
    char word[64]; size_t k = 0;
    while (*cmd == ' ' || *cmd == '\t') ++cmd;
    while (cmd[k] && !strchr(" \t|&<>()\"\\/:", cmd[k]) && k + 1 < sizeof(word)) { word[k] = cmd[k]; ++k; }
    word[k] = '\0';
    if (!k) return;

    Suggest s; s.n = 0;
    for (int i = 0; g_builtins[i]; ++i) suggest_consider(&s, word, g_builtins[i]);
    for (int i = 0; cmd_internals[i]; ++i) suggest_consider(&s, word, cmd_internals[i]);
    for (size_t i = 0; i < g_alias_count; ++i) suggest_consider(&s, word, g_aliases[i].name);

    DWORD need = GetEnvironmentVariableA("PATH", NULL, 0);
    char *path = need ? (char *)malloc(need) : NULL;
    if (path && GetEnvironmentVariableA("PATH", path, need)) {
        static const char *const patterns[] = {"*.exe", "*.bat", "*.cmd"};
        char *ctx = NULL;
        for (char *dir = strtok_s(path, ";", &ctx); dir; dir = strtok_s(NULL, ";", &ctx)) {
            for (int p = 0; p < 3; ++p) {
                char search[PATH_CAP]; WIN32_FIND_DATAA fd;
                snprintf(search, sizeof(search), "%s\\%s", dir, patterns[p]);
                HANDLE h = FindFirstFileA(search, &fd);
                if (h == INVALID_HANDLE_VALUE) continue;
                do {
                    char *dot = strrchr(fd.cFileName, '.');
                    if (dot) *dot = '\0';
                    suggest_consider(&s, word, fd.cFileName);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
    }
    free(path);
    if (!s.n) return;
    set_color(COLOR_WARN); printf("Did you mean: ");
    for (int i = 0; i < s.n; ++i) printf("%s%s", i ? ", " : "", s.name[i]);
    printf("?\n"); reset_color();
}

static void notify_if_slow(ULONGLONG ms) {
    char buf[16]; DWORD n = GetEnvironmentVariableA("NOVA_NOTIFY_SECS", buf, sizeof(buf));
    unsigned long limit = (n && n < sizeof(buf)) ? strtoul(buf, NULL, 10) : 30;
    if (!limit || ms < (ULONGLONG)limit * 1000) return;
    unsigned long s = (unsigned long)(ms / 1000);
    set_color(COLOR_DIM);
    if (s >= 3600) printf("Took %luh %lum %lus", s / 3600, (s % 3600) / 60, s % 60);
    else if (s >= 60) printf("Took %lum %lus", s / 60, s % 60);
    else printf("Took %lus", s);
    printf("\n\a"); // ding dong
    reset_color();
}

static void execute_external(const char *cmd) {
    char rewritten[INPUT_CAP * 2]; const char *effective = cmd;
    if (strpbrk(cmd, "|<>&") && rewrite_filters(cmd, rewritten, sizeof(rewritten)) > 0) effective = rewritten;
    char full[INPUT_CAP * 2 + 64];
    int n = snprintf(full, sizeof(full), "cmd.exe /D /S /C \"%s\"", effective);
    if (n < 0 || (size_t)n >= sizeof(full)) { print_error("Command is too long."); return; }
    ULONGLONG t0 = GetTickCount64();
    DWORD code = run_process(full);
    ULONGLONG elapsed = GetTickCount64() - t0;
    g_last_exit = code;
    InterlockedExchange(&g_cancel_requested, 0);
    print_exit_status(code);
    if (code == 9009) suggest_command(cmd);
    notify_if_slow(elapsed);
}








static void job_start(char *cmdline, const char *label) {
    int slot = -1;
    for (int i = 0; i < JOB_CAP; ++i) if (!g_jobs[i].process) { slot = i; break; }
    if (slot < 0) { print_error("Too many background jobs."); return; }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { print_win_error("CreateProcess"); return; }

    HANDLE jo = CreateJobObjectA(NULL, NULL);
    if (jo) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof(li));
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(jo, JobObjectExtendedLimitInformation, &li, sizeof(li));
        if (!AssignProcessToJobObject(jo, pi.hProcess)) { CloseHandle(jo); jo = NULL; }
    }
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);

    Job *j = &g_jobs[slot]; memset(j, 0, sizeof(*j));
    j->id = g_job_next_id++; j->process = pi.hProcess; j->job_object = jo; j->pid = pi.dwProcessId;
    snprintf(j->cmd, sizeof(j->cmd), "%s", label);
    set_color(COLOR_ACCENT); printf("[%d] %lu\n", j->id, (unsigned long)j->pid); reset_color();
}

static Job *job_find(int id) {
    for (int i = 0; i < JOB_CAP; ++i) if (g_jobs[i].process && g_jobs[i].id == id) return &g_jobs[i];
    return NULL;
}

static Job *job_latest(void) {
    Job *best = NULL;
    for (int i = 0; i < JOB_CAP; ++i) if (g_jobs[i].process && (!best || g_jobs[i].id > best->id)) best = &g_jobs[i];
    return best;
}

static Job *job_from_arg(const char *arg) {
    if (*arg == '%') ++arg;
    if (!*arg) return job_latest();
    char *end; long id = strtol(arg, &end, 10);
    return (*end || id <= 0) ? NULL : job_find((int)id);
}

static void job_release(Job *j) {
    if (j->job_object) CloseHandle(j->job_object);
    CloseHandle(j->process);
    memset(j, 0, sizeof(*j));
}

static void jobs_notify(void) {
    for (int i = 0; i < JOB_CAP; ++i) {
        Job *j = &g_jobs[i]; DWORD code = 0;
        if (!j->process || WaitForSingleObject(j->process, 0) == WAIT_TIMEOUT) continue;
        GetExitCodeProcess(j->process, &code);
        set_color(code ? COLOR_ERROR : COLOR_EXE);
        printf("[%d]+ Done (exit %lu)  %s\n", j->id, (unsigned long)code, j->cmd);
        reset_color();
        job_release(j);
    }
}







static SshHost *ssh_find(const char *name) {
    for (size_t i = 0; i < g_ssh_count; ++i) if (!_stricmp(g_ssh[i].name, name)) return &g_ssh[i];
    return NULL;
}

static void ssh_target(const SshHost *h, char *out, size_t cap) {
    if (*h->user) snprintf(out, cap, "%s@%s", h->user, h->host);
    else snprintf(out, cap, "%s", h->host);
}

static BOOL ssh_build_opts(const SshHost *h, char *buf, size_t cap, BOOL for_scp) {
    buf[0] = '\0';
    if (h->port) {
        char num[16]; snprintf(num, sizeof(num), "%d", h->port);
        if (!append_raw(buf, cap, for_scp ? "-P" : "-p") || !append_raw(buf, cap, num)) return FALSE;
    }
    if (*h->key && (!append_raw(buf, cap, "-i") || !append_arg(buf, cap, h->key))) return FALSE;
    if (!for_scp && *h->extra && !append_raw(buf, cap, h->extra)) return FALSE;
    return TRUE;
}

static void ssh_set_last(const char *name) {
    char p[PATH_CAP]; config_file(p, sizeof(p), "ssh_last.txt");
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s\n", name); fclose(f); }
}

static BOOL ssh_get_last(char *out, size_t cap) {
    char p[PATH_CAP], line[128]; config_file(p, sizeof(p), "ssh_last.txt");
    FILE *f = fopen(p, "r");
    if (!f) return FALSE;
    BOOL ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok) return FALSE;
    line[strcspn(line, "\r\n")] = '\0';
    snprintf(out, cap, "%s", line);
    return out[0] != '\0';
}

static void sshm_unknown(const char *name) {
    char msg[200]; snprintf(msg, sizeof(msg), "Unknown SSH profile '%.80s'. Try 'sshm list'.", name);
    print_error(msg);
}

static void sshm_help(void) {
    printf("sshm - saved SSH connections\n"
           "  sshm list                              Show saved profiles\n"
           "  sshm add <name> [user@]host [-p port] [-i keyfile] [ssh options...]\n"
           "                                         Save (or update) a profile\n"
           "  sshm connect <name> [command...]       Connect (also: sshm <name>)\n"
           "  sshm show <name>                       Show details and the exact command\n"
           "  sshm rm <name>                         Delete a profile\n"
           "  sshm rename <old> <new>                Rename a profile\n"
           "  sshm last [command...]                 Reconnect to the profile you used most recently\n"
           "  sshm test <name>                       Check the host is reachable and key login works\n"
           "  sshm tunnel <name> [-L|-R|-D] <spec>   Port forward only, e.g. sshm tunnel web 8080:localhost:80\n"
           "  sshm copy-id <name> [pubkey]           Install your public key on a Linux/Unix host\n"
           "  sshm import [file]                     Import Host entries from ~\\.ssh\\config\n"
           "  sshm put <name> [-r] <local> <remote>  Upload with scp\n"
           "  sshm get <name> [-r] <remote> <local>  Download with scp\n"
           "Example: sshm add web admin@example.com -p 2222 -i ~\\.ssh\\id_ed25519 -L 8080:localhost:80\n"
           "Profiles live in %s\\ssh_hosts.txt\n", g_config_path);
}

static void sshm_list(void) {
    if (!g_ssh_count) { printf("No SSH profiles yet. Add one with: sshm add <name> [user@]host [-p port] [-i key]\n"); return; }
    set_color(COLOR_ACCENT); printf("%-16s %-34s %-6s %s\n", "NAME", "TARGET", "PORT", "KEY"); reset_color();
    for (size_t i = 0; i < g_ssh_count; ++i) {
        const SshHost *h = &g_ssh[i]; char target[320], port[16] = "-";
        ssh_target(h, target, sizeof(target));
        if (h->port) snprintf(port, sizeof(port), "%d", h->port);
        printf("%-16s %-34s %-6s %s\n", h->name, target, port, *h->key ? h->key : "-");
    }
}

static void sshm_add(int argc, char **argv) {
    if (argc < 2) { print_error("Usage: sshm add <name> [user@]host [-p port] [-i keyfile] [ssh options...]"); return; }
    SshHost h; memset(&h, 0, sizeof(h));
    char *user = "", *host = argv[1], *at = strchr(argv[1], '@');
    if (at) { *at = '\0'; user = argv[1]; host = at + 1; }

    if (!valid_token(argv[0], sizeof(h.name), "._-")) { print_error("Invalid profile name (letters, digits, . _ - only)."); return; }
    if (*user && !valid_token(user, sizeof(h.user), "._-")) { print_error("Invalid user name."); return; }
    if (!valid_token(host, sizeof(h.host), "._-:[]%")) { print_error("Invalid host name."); return; }
    snprintf(h.name, sizeof(h.name), "%s", argv[0]);
    snprintf(h.user, sizeof(h.user), "%s", user);
    snprintf(h.host, sizeof(h.host), "%s", host);

    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "-i")) {
            if (i + 1 >= argc) { print_error("Option is missing its value."); return; }
            if (argv[i][1] == 'p') {
                int p = atoi(argv[++i]);
                if (p < 1 || p > 65535) { print_error("Port must be 1-65535."); return; }
                h.port = p;
            } else {
                expand_tilde(argv[++i], h.key, sizeof(h.key));
            }
        } else if (strpbrk(argv[i], "\t\r\n") || !append_arg(h.extra, sizeof(h.extra), argv[i])) {
            print_error("Extra ssh options are invalid or too long."); return;
        }
    }
    if (strpbrk(h.key, "\t\r\n")) { print_error("Invalid key path."); return; }

    SshHost *existing = ssh_find(h.name);
    if (existing) *existing = h;
    else if (g_ssh_count < SSH_CAP) g_ssh[g_ssh_count++] = h;
    else { print_error("Too many SSH profiles."); return; }
    save_ssh();

    char target[320]; ssh_target(&h, target, sizeof(target));
    printf("%s SSH profile '%s' (%s).\n", existing ? "Updated" : "Saved", h.name, target);
    if (*h.key && GetFileAttributesA(h.key) == INVALID_FILE_ATTRIBUTES) printf("Note: key file '%s' does not exist (yet).\n", h.key);
}

static void sshm_remove(const char *name) {
    SshHost *h = ssh_find(name);
    if (!h) { sshm_unknown(name); return; }
    size_t idx = (size_t)(h - g_ssh);
    memmove(&g_ssh[idx], &g_ssh[idx + 1], (g_ssh_count - idx - 1) * sizeof(SshHost));
    --g_ssh_count;
    save_ssh();
    printf("Removed SSH profile '%s'.\n", name);
}

static void sshm_show(const char *name) {
    SshHost *h = ssh_find(name);
    if (!h) { sshm_unknown(name); return; }
    char target[320], opts[1024];
    ssh_target(h, target, sizeof(target));
    ssh_build_opts(h, opts, sizeof(opts), FALSE);
    printf("Name:    %s\nTarget:  %s\nPort:    %s\nKey:     %s\nOptions: %s\n", h->name, target,
           h->port ? "custom" : "default", *h->key ? h->key : "(none)", *h->extra ? h->extra : "(none)");
    printf("Command: ssh %s%s%s\n", opts, *opts ? " " : "", target);
}

static void sshm_connect(int argc, char **argv) {
    SshHost *h = ssh_find(argv[0]);
    if (!h) { sshm_unknown(argv[0]); return; }
    ssh_set_last(h->name);
    char exe[PATH_CAP];
    if (!find_executable("ssh", exe, sizeof(exe))) {
        print_error("ssh.exe not found. Install 'OpenSSH Client' under Settings > Optional features.");
        return;
    }
    char cmdline[INPUT_CAP + PATH_CAP] = "", opts[1024], target[320];
    ssh_target(h, target, sizeof(target));
    BOOL ok = append_arg(cmdline, sizeof(cmdline), exe) && ssh_build_opts(h, opts, sizeof(opts), FALSE) &&
              (!*opts || append_raw(cmdline, sizeof(cmdline), opts)) && append_arg(cmdline, sizeof(cmdline), target);
    for (int i = 1; ok && i < argc; ++i) ok = append_arg(cmdline, sizeof(cmdline), argv[i]);
    if (!ok) { print_error("Command line too long."); return; }

    set_color(COLOR_ACCENT); printf("Connecting to %s ...\n", target); reset_color();
    char title[128]; snprintf(title, sizeof(title), "ssh: %s", h->name);
    SetConsoleTitleA(title);
    g_last_exit = run_process(cmdline);
    SetConsoleTitleA(APP_NAME);
}

static void sshm_transfer(BOOL upload, int argc, char **argv) {
    const char *usage = upload ? "Usage: sshm put <name> [-r] <local> <remote>" : "Usage: sshm get <name> [-r] <remote> <local>";
    if (argc < 3) { print_error(usage); return; }
    SshHost *h = ssh_find(argv[0]);
    if (!h) { sshm_unknown(argv[0]); return; }

    char flags[256] = "", *paths[2]; int np = 0;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') { if (!append_arg(flags, sizeof(flags), argv[i])) { print_error("Too many flags."); return; } }
        else if (np < 2) paths[np++] = argv[i];
        else { print_error(usage); return; }
    }
    if (np != 2) { print_error(usage); return; }

    char exe[PATH_CAP];
    if (!find_executable("scp", exe, sizeof(exe))) { print_error("scp.exe not found. Install 'OpenSSH Client' under Optional features."); return; }

    char cmdline[INPUT_CAP + PATH_CAP] = "", opts[1024], target[320], remote[INPUT_CAP];
    ssh_target(h, target, sizeof(target));
    snprintf(remote, sizeof(remote), "%s:%s", target, paths[upload ? 1 : 0]);
    BOOL ok = append_arg(cmdline, sizeof(cmdline), exe) && ssh_build_opts(h, opts, sizeof(opts), TRUE) &&
              (!*opts || append_raw(cmdline, sizeof(cmdline), opts)) && (!*flags || append_raw(cmdline, sizeof(cmdline), flags)) &&
              append_arg(cmdline, sizeof(cmdline), upload ? paths[0] : remote) &&
              append_arg(cmdline, sizeof(cmdline), upload ? remote : paths[1]);
    if (!ok) { print_error("Command line too long."); return; }
    g_last_exit = run_process(cmdline);
}

static BOOL need_tool(const char *name, char *exe, size_t cap) {
    if (find_executable(name, exe, cap)) return TRUE;
    char msg[160]; snprintf(msg, sizeof(msg), "%s.exe not found. Install 'OpenSSH Client' under Settings > Optional features.", name);
    print_error(msg);
    return FALSE;
}

static BOOL ssh_build_basic_opts(const SshHost *h, char *buf, size_t cap) {
    SshHost tmp = *h; tmp.extra[0] = '\0';
    return ssh_build_opts(&tmp, buf, cap, FALSE);
}

static void sshm_test(const char *name) {
    SshHost *h = ssh_find(name);
    if (!h) { sshm_unknown(name); return; }
    char exe[PATH_CAP], opts[1024], target[320], cmdline[INPUT_CAP + PATH_CAP] = "";
    if (!need_tool("ssh", exe, sizeof(exe))) return;
    ssh_target(h, target, sizeof(target));
    BOOL ok = append_arg(cmdline, sizeof(cmdline), exe) && append_raw(cmdline, sizeof(cmdline), "-o BatchMode=yes -o ConnectTimeout=5") &&
              ssh_build_basic_opts(h, opts, sizeof(opts)) && (!*opts || append_raw(cmdline, sizeof(cmdline), opts)) &&
              append_arg(cmdline, sizeof(cmdline), target) && append_raw(cmdline, sizeof(cmdline), "exit");
    if (!ok) { print_error("Command line too long."); return; }
    printf("Testing %s ...\n", target);
    DWORD code = run_process(cmdline);
    g_last_exit = code;
    if (code == 0) { set_color(COLOR_EXE); printf("OK: reachable, key login works.\n"); reset_color(); }
    else { set_color(COLOR_ERROR); printf("Failed (ssh exit code %lu). Password-only hosts also fail here, since BatchMode never prompts.\n", code); reset_color(); }
}

static void sshm_tunnel(int argc, char **argv) {
    const char *usage = "Usage: sshm tunnel <name> [-L|-R|-D] <spec>   (default -L), e.g. 8080:localhost:80";
    if (argc < 2) { print_error(usage); return; }
    SshHost *h = ssh_find(argv[0]);
    if (!h) { sshm_unknown(argv[0]); return; }
    const char *flag = "-L", *spec = argv[1];
    if (!strcmp(argv[1], "-L") || !strcmp(argv[1], "-R") || !strcmp(argv[1], "-D")) {
        if (argc < 3) { print_error(usage); return; }
        flag = argv[1]; spec = argv[2];
    }
    if (!valid_token(spec, 128, ".:-[]")) { print_error("Invalid forward specification."); return; }

    char exe[PATH_CAP], opts[1024], target[320], cmdline[INPUT_CAP + PATH_CAP] = "";
    if (!need_tool("ssh", exe, sizeof(exe))) return;
    ssh_target(h, target, sizeof(target));
    BOOL ok = append_arg(cmdline, sizeof(cmdline), exe) && ssh_build_opts(h, opts, sizeof(opts), FALSE) &&
              (!*opts || append_raw(cmdline, sizeof(cmdline), opts)) &&
              append_raw(cmdline, sizeof(cmdline), "-N -o ExitOnForwardFailure=yes") &&
              append_raw(cmdline, sizeof(cmdline), flag) && append_raw(cmdline, sizeof(cmdline), spec) &&
              append_arg(cmdline, sizeof(cmdline), target);
    if (!ok) { print_error("Command line too long."); return; }
    set_color(COLOR_ACCENT); printf("Tunnel %s %s via %s - press Ctrl+C to close.\n", flag, spec, target); reset_color();
    g_last_exit = run_process(cmdline);
}

static void sshm_copy_id(int argc, char **argv) {
    if (argc < 1) { print_error("Usage: sshm copy-id <name> [pubkey-file]"); return; }
    SshHost *h = ssh_find(argv[0]);
    if (!h) { sshm_unknown(argv[0]); return; }

    char pub[PATH_CAP] = "";
    if (argc > 1) expand_tilde(argv[1], pub, sizeof(pub));
    else if (*h->key) snprintf(pub, sizeof(pub), "%s.pub", h->key);
    else {
        expand_tilde("~\\.ssh\\id_ed25519.pub", pub, sizeof(pub));
        if (GetFileAttributesA(pub) == INVALID_FILE_ATTRIBUTES) expand_tilde("~\\.ssh\\id_rsa.pub", pub, sizeof(pub));
    }
    if (GetFileAttributesA(pub) == INVALID_FILE_ATTRIBUTES) { print_error("Public key not found. Create one with: ssh-keygen -t ed25519"); return; }

    char exe[PATH_CAP], opts[1024], target[320], cmdline[INPUT_CAP + PATH_CAP] = "";
    if (!need_tool("ssh", exe, sizeof(exe))) return;
    ssh_target(h, target, sizeof(target));
    static const char *remote =
        "umask 077; mkdir -p ~/.ssh; cat > ~/.ssh/.nova_key.tmp; "
        "grep -qxFf ~/.ssh/.nova_key.tmp ~/.ssh/authorized_keys 2>/dev/null || cat ~/.ssh/.nova_key.tmp >> ~/.ssh/authorized_keys; "
        "rm -f ~/.ssh/.nova_key.tmp";
    BOOL ok = append_arg(cmdline, sizeof(cmdline), exe) && ssh_build_basic_opts(h, opts, sizeof(opts)) &&
              (!*opts || append_raw(cmdline, sizeof(cmdline), opts)) &&
              append_arg(cmdline, sizeof(cmdline), target) && append_arg(cmdline, sizeof(cmdline), remote);
    if (!ok) { print_error("Command line too long."); return; }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE in = CreateFileA(pub, GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) { print_win_error("Opening public key"); return; }
    printf("Installing %s on %s ...\n", pub, target);
    DWORD code = run_process_ex(cmdline, in);
    CloseHandle(in);
    g_last_exit = code;
    if (code == 0) { set_color(COLOR_EXE); printf("Done. Try: sshm test %s\n", h->name); reset_color(); }
}

static void import_commit(SshHost *cur, int *added, int *skipped) {
    if (!cur->name[0]) return;
    if (!cur->host[0]) snprintf(cur->host, sizeof(cur->host), "%s", cur->name);
    if (ssh_find(cur->name) || g_ssh_count >= SSH_CAP || !valid_token(cur->host, sizeof(cur->host), "._-:[]%") ||
        (cur->user[0] && !valid_token(cur->user, sizeof(cur->user), "._-"))) { ++*skipped; return; }
    g_ssh[g_ssh_count++] = *cur;
    ++*added;
}

static void sshm_import(const char *file) {
    char path[PATH_CAP];
    expand_tilde((file && *file) ? file : "~\\.ssh\\config", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) { char msg[PATH_CAP + 40]; snprintf(msg, sizeof(msg), "Could not open %s", path); print_error(msg); return; }

    SshHost cur; memset(&cur, 0, sizeof(cur));
    int added = 0, skipped = 0; char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *key = p, *val = p + strcspn(p, " \t=");
        if (*val) { *val++ = '\0'; val = trim(val); if (*val == '=') val = trim(val + 1); }

        if (!_stricmp(key, "Host") || !_stricmp(key, "Match")) {
            import_commit(&cur, &added, &skipped);
            memset(&cur, 0, sizeof(cur));
            if (!_stricmp(key, "Host")) {
                char *tok[8]; int n = tokenize(val, tok, 8);
                for (int i = 0; i < n; ++i)
                    if (!strpbrk(tok[i], "*?!") && valid_token(tok[i], sizeof(cur.name), "._-")) { snprintf(cur.name, sizeof(cur.name), "%s", tok[i]); break; }
            }
        } else if (cur.name[0]) {
            val = strip_quotes(val);
            if (!_stricmp(key, "HostName")) snprintf(cur.host, sizeof(cur.host), "%s", val);
            else if (!_stricmp(key, "User")) snprintf(cur.user, sizeof(cur.user), "%s", val);
            else if (!_stricmp(key, "Port")) { int port = atoi(val); if (port > 0 && port < 65536) cur.port = port; }
            else if (!_stricmp(key, "IdentityFile") && !cur.key[0]) expand_tilde(val, cur.key, sizeof(cur.key));
        }
    }
    import_commit(&cur, &added, &skipped);
    fclose(f);
    if (added) save_ssh();
    printf("Imported %d profile(s), skipped %d (already exist or unusable).\n", added, skipped);
}

static void sshm_rename(const char *from, const char *to) {
    SshHost *h = ssh_find(from);
    if (!h) { sshm_unknown(from); return; }
    if (!valid_token(to, sizeof(h->name), "._-")) { print_error("Invalid profile name (letters, digits, . _ - only)."); return; }
    SshHost *other = ssh_find(to);
    if (other && other != h) { print_error("A profile with that name already exists."); return; }
    char last[64];
    BOOL was_last = ssh_get_last(last, sizeof(last)) && !_stricmp(last, h->name);
    snprintf(h->name, sizeof(h->name), "%s", to);
    save_ssh();
    if (was_last) ssh_set_last(h->name);
    printf("Renamed '%s' to '%s'.\n", from, to);
}

static void builtin_sshm(const char *args) {
    char copy[INPUT_CAP]; snprintf(copy, sizeof(copy), "%s", args);
    char *argv[32]; int argc = tokenize(copy, argv, 32);
    if (argc == 0 || !_stricmp(argv[0], "list") || !_stricmp(argv[0], "ls")) { sshm_list(); return; }
    const char *sub = argv[0];
    if (!_stricmp(sub, "help") || !strcmp(sub, "-h") || !strcmp(sub, "--help")) sshm_help();
    else if (!_stricmp(sub, "add") || !_stricmp(sub, "set") || !_stricmp(sub, "edit")) sshm_add(argc - 1, argv + 1);
    else if (!_stricmp(sub, "rm") || !_stricmp(sub, "remove") || !_stricmp(sub, "del")) {
        if (argc < 2) print_error("Usage: sshm rm <name>"); else sshm_remove(argv[1]);
    }
    else if (!_stricmp(sub, "show") || !_stricmp(sub, "info")) {
        if (argc < 2) print_error("Usage: sshm show <name>"); else sshm_show(argv[1]);
    }
    else if (!_stricmp(sub, "test")) { if (argc < 2) print_error("Usage: sshm test <name>"); else sshm_test(argv[1]); }
    else if (!_stricmp(sub, "rename") || !_stricmp(sub, "mv")) {
        if (argc < 3) print_error("Usage: sshm rename <old> <new>"); else sshm_rename(argv[1], argv[2]);
    }
    else if (!_stricmp(sub, "last")) {
        char last[64];
        if (!ssh_get_last(last, sizeof(last))) print_error("No previous SSH connection.");
        else { argv[0] = last; sshm_connect(argc, argv); }
    }
    else if (!_stricmp(sub, "tunnel")) sshm_tunnel(argc - 1, argv + 1);
    else if (!_stricmp(sub, "copy-id")) sshm_copy_id(argc - 1, argv + 1);
    else if (!_stricmp(sub, "import")) sshm_import(argc > 1 ? argv[1] : "");
    else if (!_stricmp(sub, "put")) sshm_transfer(TRUE, argc - 1, argv + 1);
    else if (!_stricmp(sub, "get")) sshm_transfer(FALSE, argc - 1, argv + 1);
    else if (!_stricmp(sub, "connect") || !_stricmp(sub, "c")) {
        if (argc < 2) print_error("Usage: sshm connect <name>"); else sshm_connect(argc - 1, argv + 1);
    }
    else sshm_connect(argc, argv);
}











static void print_help(void) {
    set_color(COLOR_ACCENT); printf("novaShell %s\n", APP_VERSION); reset_color();
    printf("A lightweight Windows command shell. Unknown commands are passed to cmd.exe.\n\n"
           "Navigation:\n"
           "  cd [path]          Change directory (\"cd -\" = previous, \"~\" = home, \"cd /d\" accepted)\n"
           "  bm [add|rm|name]   Directory bookmarks: \"bm add proj\", then \"bm proj\" jumps there\n"
           "  mkcd <path>        Create a directory (with parents) and enter it\n"
           "  pwd                Print current directory\n"
           "  dir [path|glob]    List files and directories (dir /b etc. goes to cmd.exe)\n\n"
           "SSH:\n"
           "  sshm ...           Saved SSH connections - run \"sshm help\" for details\n\n"
           "Environment and scripting:\n"
           "  export NAME=value  Set an environment variable (also: set NAME=value); %%VAR%% is expanded\n"
           "  unset NAME         Remove an environment variable\n"
           "  env                List environment variables\n"
           "  time <command>     Run a command and show how long it took\n"
           "  a && b             Run b only if a succeeded (works with built-ins too)\n"
           "  touch <files...>   Create files or update their timestamp\n"
           "  source <file>      Run commands from a file (one per line, # = comment)\n"
           "  sudo [command]     Run elevated via UAC (no argument = elevated novaShell)\n"
           "  config [...]       Show, open, edit or reload the config files\n"
           "  prompt [format]    Show or set the prompt ({code} {cwd} {git} {time} {user} {host} {jobs} {nl})\n\n"
           "Text tools (also in pipes, e.g. dir | grep txt):\n"
           "  cat [-n] [files]           Print files or stdin\n"
           "  head|tail [-n N] [files]   First/last lines (tail -f follows a file, Ctrl+C stops)\n"
           "  grep [-ivnclrH] pat [files]  Substring search; -r recurses, wildcards work (no regex)\n"
           "  wc [-lwc] [files]          Count lines, words, bytes\n\n"
           "Background jobs:\n"
           "  command &          Run in the background (also built-ins, e.g. sshm tunnel web 8080:localhost:80 &)\n"
           "  jobs, fg [n], kill %%n   List, wait for, or stop jobs (jobs end when novaShell exits)\n\n"
           "Shell:\n"
           "  alias [name [cmd]] List, define or inspect aliases (also: alias name=cmd)\n"
           "                     Aliases take arguments: alias mk=mkcd ~\\code\\$1 && git init   ($1..$9, $*)\n"
           "  unalias <name>     Remove an alias\n"
           "  history [n]        Show history (!! repeats the last command, !N entry N)\n"
           "  history-clear      Erase history (start a line with a space to keep it out of history)\n"
           "  which <command>    Locate a built-in, alias or executable\n"
           "  title <text>       Set console title\n"
           "  clear, cls         Clear the console\n"
           "  help, about, exit\n\n"
           "Line editing: Up/Down history, Left/Right, Home/End, Delete, Backspace, Tab completion\n"
           "(press Tab again to cycle), Ctrl+R searches history, Ctrl+Left/Right jump words, Ctrl+W deletes a word,\n"
           "Ctrl+U/Esc clears the line, Ctrl+D quits on an empty line. In a git repo the prompt shows the\n"
           "branch (set NOVA_NOGIT=1 to turn that off). A dimmed suggestion from history follows your typing;\n"
           "Right or End accepts it. Folders with a .novarc ask before running it (NOVA_NORC=1 disables).\n"
           "Commands that take NOVA_NOTIFY_SECS seconds or longer (default 30, 0 = off) ring the bell.\n"
           "Config folder: %s\n"
           "  aliases.txt (name=command), startup.txt (commands run at launch), ssh_hosts.txt, bookmarks.txt, prompt.txt\n",
           g_config_path);
}

static void builtin_dir(const char *path) {
    char p[PATH_CAP], expanded[PATH_CAP], search[PATH_CAP + 4];
    snprintf(p, sizeof(p), "%s", (path && *path) ? path : ".");
    expand_tilde(strip_quotes(p), expanded, sizeof(expanded));

    DWORD attr = GetFileAttributesA(expanded);
    if (strpbrk(expanded, "*?") || (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)))
        snprintf(search, sizeof(search), "%s", expanded);
    else {
        size_t n = strlen(expanded);
        snprintf(search, sizeof(search), "%s%s*", expanded, (n && (expanded[n - 1] == '\\' || expanded[n - 1] == '/')) ? "" : "\\");
    }
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) { print_win_error("Directory listing"); return; }
    printf(" Directory of %s\n\n", expanded);
    unsigned long long files = 0, dirs = 0, bytes = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        FILETIME local; SYSTEMTIME st;
        FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local); FileTimeToSystemTime(&local, &st);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { set_color(COLOR_DIR); printf("<DIR>       "); ++dirs; }
        else {
            ULARGE_INTEGER sz; sz.HighPart = fd.nFileSizeHigh; sz.LowPart = fd.nFileSizeLow;
            printf("%10llu  ", sz.QuadPart); ++files; bytes += sz.QuadPart;
            char *ext = strrchr(fd.cFileName, '.');
            if (ext && (!_stricmp(ext, ".exe") || !_stricmp(ext, ".bat") || !_stricmp(ext, ".cmd"))) set_color(COLOR_EXE);
            else if (ext && (!_stricmp(ext, ".zip") || !_stricmp(ext, ".7z") || !_stricmp(ext, ".rar"))) set_color(COLOR_ARCHIVE);
        }
        printf("%04u-%02u-%02u %02u:%02u  %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, fd.cFileName);
        reset_color();
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("\n%llu file(s), %llu dir(s), %llu bytes\n", files, dirs, bytes);
}

static void builtin_history(const char *arg) {
    size_t n = g_history_count;
    if (arg && *arg) {
        char *end; unsigned long v = strtoul(arg, &end, 10);
        if (*end || v == 0) { print_error("Usage: history [positive-count]"); return; }
        if (v < n) n = (size_t)v;
    }
    for (size_t i = g_history_count - n; i < g_history_count; ++i) printf("%5llu  %s\n", (unsigned long long)(i + 1), g_history[i]);
}

static void builtin_alias(const char *args) {
    char copy[INPUT_CAP]; snprintf(copy, sizeof(copy), "%s", args ? args : "");
    char *p = trim(copy);
    if (!*p) { for (size_t i = 0; i < g_alias_count; ++i) printf("%s=%s\n", g_aliases[i].name, g_aliases[i].command); return; }

    char *name = p; char *sep = name + strcspn(name, " \t=");
    char *rest = sep;
    if (*sep) { *sep = '\0'; rest = trim(sep + 1); if (*rest == '=' && *sep != '=') rest = trim(rest + 1); }
    rest = strip_quotes(rest);

    int i = alias_find(name);
    if (!*rest) {
        if (i >= 0) printf("%s=%s\n", g_aliases[i].name, g_aliases[i].command);
        else printf("Alias '%s' not found.\n", name);
        return;
    }
    if (!alias_set(name, rest)) { print_error("Alias limit reached or alias is too long."); return; }
    save_aliases();
    printf("%s alias %s.\n", i >= 0 ? "Updated" : "Added", name);
}

static void builtin_unalias(const char *name) {
    int i = alias_find(name);
    if (!*name) { print_error("Usage: unalias <name>"); return; }
    if (i < 0) { printf("Alias '%s' not found.\n", name); return; }
    memmove(&g_aliases[i], &g_aliases[i + 1], (g_alias_count - (size_t)i - 1) * sizeof(Alias));
    --g_alias_count;
    save_aliases();
    printf("Removed alias %s.\n", name);
}

static void change_dir(const char *arg) {
    char cur[PATH_CAP], target[PATH_CAP];
    if (!GetCurrentDirectoryA(sizeof(cur), cur)) cur[0] = '\0';
    if (!strcmp(arg, "-")) {
        if (!g_prev_dir[0]) { print_error("No previous directory."); return; }
        snprintf(target, sizeof(target), "%s", g_prev_dir);
    } else {
        expand_tilde(arg, target, sizeof(target));
    }
    if (SetCurrentDirectoryA(target)) { snprintf(g_prev_dir, sizeof(g_prev_dir), "%s", cur); maybe_run_novarc(); }
    else print_win_error("cd");
}

static void builtin_cd(char *path) {
    if (starts_with_ci(path, "/d") && is_word_boundary(path[2])) path = trim(path + 2);
    path = strip_quotes(path);
    if (!*path) {
        char cwd[PATH_CAP];
        if (GetCurrentDirectoryA(sizeof(cwd), cwd)) puts(cwd); else print_win_error("GetCurrentDirectory");
        return;
    }
    change_dir(path);
}

static void builtin_bm(const char *args) {
    char copy[INPUT_CAP]; snprintf(copy, sizeof(copy), "%s", args);
    char *argv[8]; int argc = tokenize(copy, argv, 8);
    if (argc == 0 || !_stricmp(argv[0], "list")) {
        if (!g_bm_count) printf("No bookmarks. Add one with: bm add <name> [path]\n");
        for (size_t i = 0; i < g_bm_count; ++i) printf("%-16s %s\n", g_bm[i].name, g_bm[i].path);
        return;
    }
    if (!_stricmp(argv[0], "add")) {
        if (argc < 2) { print_error("Usage: bm add <name> [path]"); return; }
        if (!valid_token(argv[1], sizeof(g_bm[0].name), "._-")) { print_error("Invalid bookmark name."); return; }
        char raw[PATH_CAP], full[MAX_PATH];
        expand_tilde(argc > 2 ? argv[2] : ".", raw, sizeof(raw));
        DWORD n = GetFullPathNameA(raw, sizeof(full), full, NULL);
        DWORD attr = n ? GetFileAttributesA(full) : INVALID_FILE_ATTRIBUTES;
        if (!n || n >= sizeof(full) || attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { print_error("Not a directory."); return; }
        size_t i; for (i = 0; i < g_bm_count; ++i) if (!_stricmp(g_bm[i].name, argv[1])) break;
        if (i == g_bm_count) { if (g_bm_count >= BM_CAP) { print_error("Too many bookmarks."); return; } ++g_bm_count; }
        snprintf(g_bm[i].name, sizeof(g_bm[i].name), "%s", argv[1]);
        snprintf(g_bm[i].path, sizeof(g_bm[i].path), "%s", full);
        save_bookmarks();
        printf("Bookmarked %s -> %s\n", g_bm[i].name, g_bm[i].path);
        return;
    }
    if (!_stricmp(argv[0], "rm") || !_stricmp(argv[0], "remove")) {
        if (argc < 2) { print_error("Usage: bm rm <name>"); return; }
        for (size_t i = 0; i < g_bm_count; ++i) if (!_stricmp(g_bm[i].name, argv[1])) {
            memmove(&g_bm[i], &g_bm[i + 1], (g_bm_count - i - 1) * sizeof(Bookmark));
            --g_bm_count; save_bookmarks(); printf("Removed bookmark %s.\n", argv[1]); return;
        }
        print_error("No such bookmark.");
        return;
    }
    for (size_t i = 0; i < g_bm_count; ++i) if (!_stricmp(g_bm[i].name, argv[0])) { change_dir(g_bm[i].path); return; }
    print_error("No such bookmark. Use 'bm' to list them.");
}

static void builtin_export(char *args) {
    char *eq = strchr(args, '=');
    if (!eq || eq == args) { print_error("Usage: export NAME=value"); return; }
    *eq = '\0';
    char *name = trim(args), *value = strip_quotes(trim(eq + 1)), expanded[INPUT_CAP];
    DWORD n = ExpandEnvironmentStringsA(value, expanded, sizeof(expanded));
    if (!n || n > sizeof(expanded)) { print_error("Value is too long."); return; }
    if (!SetEnvironmentVariableA(name, expanded)) print_win_error("export");
}

static void builtin_unset(const char *name) {
    if (!*name) { print_error("Usage: unset NAME"); return; }
    SetEnvironmentVariableA(name, NULL);
}

static void builtin_env(void) {
    char *block = GetEnvironmentStringsA();
    if (!block) return;
    for (char *p = block; *p; p += strlen(p) + 1) if (*p != '=') puts(p);
    FreeEnvironmentStringsA(block);
}

static void builtin_touch(char *args) {
    char *argv[32]; int argc = tokenize(args, argv, 32);
    if (!argc) { print_error("Usage: touch <file>..."); return; }
    FILETIME now; GetSystemTimeAsFileTime(&now);
    for (int i = 0; i < argc; ++i) {
        char p[PATH_CAP]; expand_tilde(argv[i], p, sizeof(p));
        HANDLE h = CreateFileA(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE) { print_win_error(p); continue; }
        SetFileTime(h, NULL, NULL, &now);
        CloseHandle(h);
    }
}

static void builtin_mkcd(char *arg) {
    char p[PATH_CAP]; arg = strip_quotes(arg);
    if (!*arg) { print_error("Usage: mkcd <path>"); return; }
    expand_tilde(arg, p, sizeof(p));
    ensure_parent_dir(p);
    if (!CreateDirectoryA(p, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { print_win_error("mkcd"); return; }
    change_dir(p);
}

static void builtin_source(char *arg) {
    char p[PATH_CAP]; arg = strip_quotes(arg);
    if (!*arg) { print_error("Usage: source <file>"); return; }
    expand_tilde(arg, p, sizeof(p));
    if (!run_script_file(p)) print_error("Cannot open script.");
}

static void builtin_sudo(const char *args) {
    char exe[PATH_CAP], cwd[PATH_CAP], params[INPUT_CAP + 64];
    SHELLEXECUTEINFOA sei; memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei); sei.fMask = SEE_MASK_NOCLOSEPROCESS; sei.lpVerb = "runas"; sei.nShow = SW_SHOWNORMAL;
    if (GetCurrentDirectoryA(sizeof(cwd), cwd)) sei.lpDirectory = cwd;
    if (*args) {
        snprintf(params, sizeof(params), "/S /C \"%s & pause\"", args);
        sei.lpFile = "cmd.exe"; sei.lpParameters = params;
    } else {
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        sei.lpFile = exe;
    }
    if (!ShellExecuteExA(&sei)) { print_win_error("sudo"); return; }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 0; if (GetExitCodeProcess(sei.hProcess, &code)) g_last_exit = code;
        CloseHandle(sei.hProcess);
    }
}







static void builtin_config(char *args) {
    if (!*args) {
        printf("Config folder: %s\n  aliases.txt  startup.txt  ssh_hosts.txt  bookmarks.txt  history.txt\n"
               "Use: config open | config edit [aliases|startup|ssh|bookmarks] | config reload\n", g_config_path);
        return;
    }
    char *argv[4]; int argc = tokenize(args, argv, 4);
    if (!_stricmp(argv[0], "open")) { ShellExecuteA(NULL, "open", g_config_path, NULL, NULL, SW_SHOWNORMAL); return; }
    if (!_stricmp(argv[0], "reload")) {
        g_alias_count = g_ssh_count = g_bm_count = 0;
        load_aliases(); load_ssh(); load_bookmarks();
        puts("Configuration reloaded.");
        return;
    }
    if (!_stricmp(argv[0], "edit")) {
        const char *what = argc > 1 ? argv[1] : "startup", *file = NULL;
        if (!_stricmp(what, "startup")) file = "startup.txt";
        else if (!_stricmp(what, "aliases")) file = "aliases.txt";
        else if (!_stricmp(what, "ssh")) file = "ssh_hosts.txt";
        else if (!_stricmp(what, "bookmarks")) file = "bookmarks.txt";
        if (!file) { print_error("Usage: config edit [aliases|startup|ssh|bookmarks]"); return; }
        char path[PATH_CAP], quoted[PATH_CAP + 4];
        config_file(path, sizeof(path), file);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { FILE *f = fopen(path, "a"); if (f) fclose(f); } //create if missing
        snprintf(quoted, sizeof(quoted), "\"%s\"", path);
        ShellExecuteA(NULL, "open", "notepad.exe", quoted, NULL, SW_SHOWNORMAL);
        return;
    }
    print_error("Usage: config [open | edit [file] | reload]");
}

static void builtin_which(const char *name) {
    if (!*name) { print_error("Usage: which <command>"); return; }
    int a = alias_find(name);
    if (a >= 0) { printf("%s is aliased to '%s'.\n", name, g_aliases[a].command); return; }
    if (in_list(name, g_builtins)) { printf("%s is a novaShell built-in.\n", name); return; }
    char found[PATH_CAP];
    if (find_executable(name, found, sizeof(found))) puts(found);
    else { printf("Could not find '%s' in PATH.\n", name); g_last_exit = 1; }
}







typedef void (*FileFn)(FILE *f, const char *name, void *ctx);

static BOOL read_dyn(FILE *f, char **buf, size_t *cap) {
    size_t len = 0;
    if (!*buf) { *cap = 256; *buf = (char *)malloc(*cap); if (!*buf) return FALSE; }
    (*buf)[0] = '\0';
    for (;;) {
        if (!fgets(*buf + len, (int)(*cap - len), f)) return len > 0;
        len += strlen(*buf + len);
        if (len > 0 && (*buf)[len - 1] == '\n') return TRUE;
        if (len + 1 < *cap) return TRUE;
        char *bigger = (char *)realloc(*buf, *cap * 2);
        if (!bigger) return TRUE;
        *buf = bigger; *cap *= 2;
    }
}

static void put_line(const char *s) {
    size_t n = strlen(s);
    fputs(s, stdout);
    if (!n || s[n - 1] != '\n') fputc('\n', stdout);
}

static const char *find_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return hay;
    for (; *hay; ++hay) if (_strnicmp(hay, needle, n) == 0) return hay;
    return NULL;
}

static int open_and_run(const char *path, FileFn fn, void *ctx) {
    FILE *f = fopen(path, "r");
    if (!f) { char msg[PATH_CAP + 40]; snprintf(msg, sizeof(msg), "%s: cannot open file", path); print_error(msg); return 0; }
    fn(f, path, ctx);
    fclose(f);
    return 1;
}

static int walk_dir(const char *dir, const char *mask, BOOL recursive, FileFn fn, void *ctx) {
    char search[PATH_CAP], path[PATH_CAP]; WIN32_FIND_DATAA fd; int count = 0;
    const char *sep = *dir ? "\\" : "";
    snprintf(search, sizeof(search), "%s%s%s", dir, sep, mask);
    HANDLE h = FindFirstFileA(search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(path, sizeof(path), "%s%s%s", dir, sep, fd.cFileName);
            count += open_and_run(path, fn, ctx);
            if (g_cancel_requested) break;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (recursive && !g_cancel_requested) {
        snprintf(search, sizeof(search), "%s%s*", dir, sep);
        h = FindFirstFileA(search, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                snprintf(path, sizeof(path), "%s%s%s", dir, sep, fd.cFileName);
                count += walk_dir(path, mask, TRUE, fn, ctx);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    return count;
}

static int each_file(const char *arg, BOOL recursive, FileFn fn, void *ctx) {
    char pat[PATH_CAP], msg[PATH_CAP + 40];
    expand_tilde(arg, pat, sizeof(pat));
    if (strpbrk(pat, "*?")) {
        char *sep = strrchr(pat, '\\'), *sep2 = strrchr(pat, '/');
        if (sep2 && (!sep || sep2 > sep)) sep = sep2;
        if (!sep) return walk_dir("", pat, recursive, fn, ctx);
        char dir[PATH_CAP]; size_t dl = (size_t)(sep - pat);
        memcpy(dir, pat, dl); dir[dl] = '\0';
        return walk_dir(dir, sep + 1, recursive, fn, ctx);
    }
    DWORD attr = GetFileAttributesA(pat);
    if (attr == INVALID_FILE_ATTRIBUTES) { snprintf(msg, sizeof(msg), "%s: No such file or directory", pat); print_error(msg); return 0; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (!recursive) { snprintf(msg, sizeof(msg), "%s: Is a directory", pat); print_error(msg); return 0; }
        return walk_dir(pat, "*", TRUE, fn, ctx);
    }
    return open_and_run(pat, fn, ctx);
}

static void run_on_inputs(char **files, int nfiles, BOOL recursive, FileFn fn, void *ctx) {
    InterlockedExchange(&g_cancel_requested, 0);
    if (nfiles == 0) {
        if (recursive) each_file(".", TRUE, fn, ctx);
        else fn(stdin, NULL, ctx);
        return;
    }
    for (int i = 0; i < nfiles && !g_cancel_requested; ++i) each_file(files[i], recursive, fn, ctx);
}

typedef struct CatCtx { BOOL number; unsigned long n; } CatCtx;

static void cat_file(FILE *f, const char *name, void *vctx) {
    CatCtx *c = (CatCtx *)vctx; char *buf = NULL; size_t cap = 0; (void)name;
    while (!g_cancel_requested && read_dyn(f, &buf, &cap)) {
        if (c->number) printf("%6lu  ", ++c->n);
        fputs(buf, stdout);
    }
    free(buf);
}

static void text_cat(char *args) {
    char *argv[64], *files[64]; int argc = tokenize(args, argv, 64), nfiles = 0; CatCtx c = {FALSE, 0};
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-n")) c.number = TRUE;
        else if (nfiles < 64) files[nfiles++] = argv[i];
    }
    run_on_inputs(files, nfiles, FALSE, cat_file, &c);
}

typedef struct HeadTailCtx { long n; BOOL follow, header; int seen; } HeadTailCtx;

static void head_file(FILE *f, const char *name, void *vctx) {
    HeadTailCtx *c = (HeadTailCtx *)vctx; char *buf = NULL; size_t cap = 0; long i = 0;
    if (c->header && name) printf("%s==> %s <==\n", c->seen ? "\n" : "", name);
    ++c->seen;
    while (i < c->n && !g_cancel_requested && read_dyn(f, &buf, &cap)) { put_line(buf); ++i; }
    free(buf);
}

static void tail_file(FILE *f, const char *name, void *vctx) {
    HeadTailCtx *c = (HeadTailCtx *)vctx; char *buf = NULL; size_t cap = 0;
    char **ring = c->n > 0 ? (char **)calloc((size_t)c->n, sizeof(char *)) : NULL;
    long count = 0;
    if (c->header && name) printf("%s==> %s <==\n", c->seen ? "\n" : "", name);
    ++c->seen;
    while (!g_cancel_requested && read_dyn(f, &buf, &cap)) {
        if (ring) { long idx = count % c->n; free(ring[idx]); ring[idx] = _strdup(buf); }
        ++count;
    }
    if (ring) {
        for (long i = count > c->n ? count - c->n : 0; i < count; ++i) put_line(ring[i % c->n]);
        for (long i = 0; i < c->n; ++i) free(ring[i]);
        free(ring);
    }
    if (c->follow && name) {
        fflush(stdout);
        while (!g_cancel_requested) {
            if (read_dyn(f, &buf, &cap)) { fputs(buf, stdout); fflush(stdout); }
            else { clearerr(f); Sleep(200); }
        }
    }
    free(buf);
}

static void text_headtail(BOOL is_tail, char *args) {
    char *argv[64], *files[64]; int argc = tokenize(args, argv, 64), nfiles = 0;
    HeadTailCtx c; memset(&c, 0, sizeof(c)); c.n = 10;
    BOOL quiet = FALSE, opts = TRUE;
    for (int i = 0; i < argc; ++i) {
        char *a = argv[i];
        if (opts && !strcmp(a, "--")) { opts = FALSE; continue; }
        if (opts && a[0] == '-' && a[1]) {
            const char *num = NULL;
            if (is_tail && !strcmp(a, "-f")) { c.follow = TRUE; continue; }
            if (!strcmp(a, "-q")) { quiet = TRUE; continue; }
            if (!strcmp(a, "-n")) { if (i + 1 >= argc) { print_error("Option -n needs a number."); return; } num = argv[++i]; }
            else if (a[1] == 'n') num = a + 2;
            else if (isdigit((unsigned char)a[1])) num = a + 1;
            else { char m[40]; snprintf(m, sizeof(m), "Unknown option %s", a); print_error(m); return; }
            char *end; long v = strtol(num, &end, 10);
            if (!*num || *end || v < 0) { print_error("Invalid line count."); return; }
            c.n = v;
            continue;
        }
        if (nfiles < 64) files[nfiles++] = a;
    }
    BOOL wild = FALSE;
    for (int i = 0; i < nfiles; ++i) if (strpbrk(files[i], "*?")) wild = TRUE;
    c.header = !quiet && (nfiles > 1 || wild);
    run_on_inputs(files, nfiles, FALSE, is_tail ? tail_file : head_file, &c);
}

typedef struct WcCtx { BOOL l, w, c, header; int files; unsigned long long tl, tw, tc; } WcCtx;

static void wc_print(const WcCtx *c, unsigned long long l, unsigned long long w, unsigned long long b, const char *name) {
    if (c->l) printf("%8llu", l);
    if (c->w) printf("%8llu", w);
    if (c->c) printf("%8llu", b);
    if (name) printf(" %s", name);
    printf("\n");
}

static void wc_file(FILE *f, const char *name, void *vctx) {
    WcCtx *c = (WcCtx *)vctx; unsigned long long l = 0, w = 0, b = 0; int ch; BOOL in_word = FALSE;
    while ((ch = fgetc(f)) != EOF) {
        ++b;
        if (ch == '\n') ++l;
        if (isspace(ch)) in_word = FALSE;
        else if (!in_word) { in_word = TRUE; ++w; }
    }
    c->tl += l; c->tw += w; c->tc += b; ++c->files;
    wc_print(c, l, w, b, name);
}

static void text_wc(char *args) {
    char *argv[64], *files[64]; int argc = tokenize(args, argv, 64), nfiles = 0;
    WcCtx c; memset(&c, 0, sizeof(c));
    for (int i = 0; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *p = argv[i] + 1; *p; ++p) {
                if (*p == 'l') c.l = TRUE; else if (*p == 'w') c.w = TRUE; else if (*p == 'c' || *p == 'm') c.c = TRUE;
                else { char m[40]; snprintf(m, sizeof(m), "Unknown option -%c", *p); print_error(m); return; }
            }
        } else if (nfiles < 64) files[nfiles++] = argv[i];
    }
    if (!c.l && !c.w && !c.c) c.l = c.w = c.c = TRUE;
    run_on_inputs(files, nfiles, FALSE, wc_file, &c);
    if (c.files > 1) wc_print(&c, c.tl, c.tw, c.tc, "total");
}

typedef struct GrepCtx {
    const char *pat;
    BOOL icase, invert, number, count_only, list_only, show_name, color, any_match;
} GrepCtx;

static void grep_file(FILE *f, const char *name, void *vctx) {
    GrepCtx *c = (GrepCtx *)vctx; char *buf = NULL; size_t cap = 0;
    unsigned long ln = 0, matches = 0; size_t pl = strlen(c->pat);
    while (!g_cancel_requested && read_dyn(f, &buf, &cap)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
        ++ln;
        BOOL hit = (c->icase ? find_ci(buf, c->pat) : strstr(buf, c->pat)) != NULL;
        if (hit == c->invert) continue;
        ++matches; c->any_match = TRUE;
        if (c->list_only) break;
        if (c->count_only) continue;
        if (c->show_name && name) printf("%s:", name);
        if (c->number) printf("%lu:", ln);
        if (c->color && !c->invert && pl) {
            const char *p = buf;
            for (;;) {
                const char *m = c->icase ? find_ci(p, c->pat) : strstr(p, c->pat);
                if (!m) break;
                fwrite(p, 1, (size_t)(m - p), stdout);
                set_color(COLOR_ERROR); fwrite(m, 1, pl, stdout); reset_color();
                p = m + pl;
            }
            puts(p);
        } else {
            puts(buf);
        }
    }
    if (c->list_only) { if (matches) puts(name ? name : "(standard input)"); }
    else if (c->count_only) { if (c->show_name && name) printf("%s:", name); printf("%lu\n", matches); }
    free(buf);
}

static void text_grep(char *args) {
    char *argv[64], *files[64]; int argc = tokenize(args, argv, 64), nfiles = 0;
    GrepCtx c; memset(&c, 0, sizeof(c));
    const char *pattern = NULL; BOOL recursive = FALSE, opts = TRUE;
    for (int i = 0; i < argc; ++i) {
        char *a = argv[i];
        if (opts && !strcmp(a, "--")) { opts = FALSE; continue; }
        if (opts && a[0] == '-' && a[1]) {
            if (!strcmp(a, "-e")) { if (i + 1 >= argc) { print_error("Option -e needs a pattern."); return; } pattern = argv[++i]; continue; }
            for (const char *p = a + 1; *p; ++p) {
                switch (*p) {
                    case 'i': c.icase = TRUE; break;
                    case 'v': c.invert = TRUE; break;
                    case 'n': c.number = TRUE; break;
                    case 'c': c.count_only = TRUE; break;
                    case 'l': c.list_only = TRUE; break;
                    case 'r': case 'R': recursive = TRUE; break;
                    case 'H': c.show_name = TRUE; break;
                    default: { char m[40]; snprintf(m, sizeof(m), "Unknown option -%c", *p); print_error(m); return; }
                }
            }
            continue;
        }
        if (!pattern) pattern = a; else if (nfiles < 64) files[nfiles++] = a;
    }
    if (!pattern) { print_error("Usage: grep [-ivnclrH] [-e] pattern [file...]"); return; }
    c.pat = pattern;
    BOOL wild = FALSE;
    for (int i = 0; i < nfiles; ++i) if (strpbrk(files[i], "*?")) wild = TRUE;
    if (nfiles > 1 || recursive || wild) c.show_name = TRUE;
    c.color = _isatty(_fileno(stdout));
    run_on_inputs(files, nfiles, recursive, grep_file, &c);
    if (!c.any_match && g_last_exit == 0) g_last_exit = 1;
}

static BOOL run_text_builtin(char *cmd) {
    char *args;
    if (match_cmd(cmd, "cat", &args)) text_cat(args);
    else if (match_cmd(cmd, "head", &args)) text_headtail(FALSE, args);
    else if (match_cmd(cmd, "tail", &args)) text_headtail(TRUE, args);
    else if (match_cmd(cmd, "wc", &args)) text_wc(args);
    else if (match_cmd(cmd, "grep", &args)) text_grep(args);
    else return FALSE;
    return TRUE;
}

static void builtin_jobs(void) {
    int shown = 0;
    for (int i = 0; i < JOB_CAP; ++i) {
        Job *j = &g_jobs[i];
        if (!j->process) continue;
        BOOL running = WaitForSingleObject(j->process, 0) == WAIT_TIMEOUT;
        printf("[%d]  %-8s %s  (pid %lu)\n", j->id, running ? "Running" : "Done", j->cmd, (unsigned long)j->pid);
        ++shown;
    }
    if (!shown) puts("No background jobs.");
}

static void builtin_fg(const char *arg) {
    Job *j = job_from_arg(arg);
    if (!j) { print_error("No such job."); return; }
    printf("%s\n", j->cmd);
    InterlockedExchange(&g_cancel_requested, 0);
    while (WaitForSingleObject(j->process, 100) == WAIT_TIMEOUT) {
        if (InterlockedExchange(&g_cancel_requested, 0)) { printf("\n[%d] is still running in the background.\n", j->id); return; }
    }
    DWORD code = 1; GetExitCodeProcess(j->process, &code);
    job_release(j);
    g_last_exit = code;
    print_exit_status(code);
}

static void builtin_kill(const char *arg) {
    if (!*arg) { print_error("Usage: kill %n   (background job)   or   kill <pid>"); return; }
    if (arg[0] == '%') {
        Job *j = job_from_arg(arg);
        if (!j) { print_error("No such job."); return; }
        if (j->job_object) TerminateJobObject(j->job_object, 1); else TerminateProcess(j->process, 1);
        printf("Killed job %d.\n", j->id);
        return;
    }
    char *end; unsigned long pid = strtoul(arg, &end, 10);
    if (*end || !pid) { print_error("Usage: kill %n   (background job)   or   kill <pid>"); return; }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) { print_win_error("kill"); return; }
    if (TerminateProcess(h, 1)) printf("Killed process %lu.\n", pid); else print_win_error("kill");
    CloseHandle(h);
}

static BOOL strip_background(char *cmd) {
    size_t n = strlen(cmd);
    if (!n || cmd[n - 1] != '&' || (n >= 2 && cmd[n - 2] == '&')) return FALSE;
    cmd[n - 1] = '\0';
    trim(cmd);
    return TRUE;
}

static void run_background(const char *cmd) {
    char first[64], line[INPUT_CAP + PATH_CAP + 64], quoted[INPUT_CAP + 8]; size_t k = 0;
    if (!*cmd) { print_error("Nothing to run in the background."); return; }
    while (cmd[k] && !is_space_char(cmd[k]) && k + 1 < sizeof(first)) { first[k] = cmd[k]; ++k; }
    first[k] = '\0';
    if (in_list(first, g_builtins)) {
        if (!quote_crt_arg(quoted, sizeof(quoted), cmd)) { print_error("Command is too long."); return; }
        snprintf(line, sizeof(line), "\"%s\" --nova-line %s", g_self_path, quoted);
    } else {
        int n = snprintf(line, sizeof(line), "cmd.exe /D /S /C \"%s\"", cmd);
        if (n < 0 || (size_t)n >= sizeof(line)) { print_error("Command is too long."); return; }
    }
    job_start(line, cmd);
}

static void load_prompt(void) {
    char p[PATH_CAP], line[sizeof(g_prompt_fmt) + 8]; config_file(p, sizeof(p), "prompt.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0]) snprintf(g_prompt_fmt, sizeof(g_prompt_fmt), "%s", line);
    }
    fclose(f);
}

static void builtin_prompt(char *args) {
    char path[PATH_CAP]; config_file(path, sizeof(path), "prompt.txt");
    if (!*args) {
        printf("Current prompt: %s\n", g_prompt_fmt);
        printf("Tokens: {code} last exit code, {cwd}, {git}, {time}, {user}, {host}, {jobs}, {nl}\n");
        printf("Examples:  prompt \"{user}@{host} {cwd}{git}> \"   |   prompt reset\n");
        return;
    }
    if (!_stricmp(args, "reset")) {
        snprintf(g_prompt_fmt, sizeof(g_prompt_fmt), "%s", "{code}{cwd}{git}> ");
        remove(path);
        puts("Prompt reset.");
        return;
    }
    args = strip_quotes(args);
    if (strlen(args) >= sizeof(g_prompt_fmt)) { print_error("Prompt format is too long."); return; }
    snprintf(g_prompt_fmt, sizeof(g_prompt_fmt), "%s", args);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", g_prompt_fmt); fclose(f); }
}






static BOOL alias_uses_args(const char *text) {
    for (; *text; ++text) {
        if (*text != '$') continue;
        if ((text[1] >= '1' && text[1] <= '9') || text[1] == '*') return TRUE;
        if (text[1] == '$') ++text;
    }
    return FALSE;
}

static int split_raw_args(char *s, char **argv, int max) {
    int argc = 0; BOOL quoted = FALSE;
    while (*s && argc < max) {
        while (is_space_char(*s)) ++s;
        if (!*s) break;
        argv[argc++] = s;
        while (*s && (quoted || !is_space_char(*s))) { if (*s == '"') quoted = !quoted; ++s; }
        if (*s) *s++ = '\0';
    }
    return argc;
}

static BOOL expand_alias(char *cmd, size_t cap, int *last) {
    char *p = cmd; while (*p && is_space_char(*p)) ++p;
    char *end = p; while (*end && !is_space_char(*end)) ++end;
    size_t wordlen = (size_t)(end - p);
    if (!wordlen) return FALSE;
    for (size_t i = 0; i < g_alias_count; ++i) {
        if (strlen(g_aliases[i].name) != wordlen || _strnicmp(p, g_aliases[i].name, wordlen) != 0) continue;
        if ((int)i == *last) return FALSE;
        const char *text = g_aliases[i].command;
        char replacement[INPUT_CAP];
        if (alias_uses_args(text)) {
            char rest[INPUT_CAP], tmp[INPUT_CAP], *av[10];
            snprintf(rest, sizeof(rest), "%s", end);
            char *all = trim(rest);
            snprintf(tmp, sizeof(tmp), "%s", all);
            int ac = split_raw_args(tmp, av, 9);
            size_t o = 0;
            for (const char *t = text; *t; ++t) {
                const char *ins; char one[2] = {0, 0};
                if (*t == '$' && t[1] == '$') { one[0] = '$'; ins = one; ++t; }
                else if (*t == '$' && t[1] == '*') { ins = all; ++t; }
                else if (*t == '$' && t[1] >= '1' && t[1] <= '9') { int k = t[1] - '1'; ins = k < ac ? av[k] : ""; ++t; }
                else { one[0] = *t; ins = one; }
                size_t l = strlen(ins);
                if (o + l + 1 >= sizeof(replacement)) { print_error("Alias expansion exceeds command buffer."); return FALSE; }
                memcpy(replacement + o, ins, l); o += l;
            }
            replacement[o] = '\0';
        } else {
            int n = snprintf(replacement, sizeof(replacement), "%s%s", text, end);
            if (n < 0 || (size_t)n >= sizeof(replacement)) { print_error("Alias expansion exceeds command buffer."); return FALSE; }
        }
        if (strlen(replacement) >= cap) { print_error("Alias expansion exceeds command buffer."); return FALSE; }
        memmove(cmd, replacement, strlen(replacement) + 1);
        *last = (int)i;
        return TRUE;
    }
    return FALSE;
}

static BOOL run_command_ex(const char *line, int in_last_alias);

static BOOL run_chain(char *cmd, char *chain, int last_alias) {
    *chain = '\0';
    if (!run_command_ex(cmd, last_alias)) return FALSE;
    return g_last_exit == 0 ? run_command_ex(chain + 2, -1) : TRUE;
}

static BOOL run_command_ex(const char *line, int in_last_alias) {
    char work[INPUT_CAP];
    snprintf(work, sizeof(work), "%s", line);
    char *cmd = trim(work), *args;
    if (!*cmd || *cmd == '#') return TRUE;

    BOOL no_split = match_cmd(cmd, "alias", &args) || strpbrk(cmd, "()") != NULL;
    char *chain = no_split ? NULL : find_and_and(cmd);
    if (chain) return run_chain(cmd, chain, in_last_alias);

    g_last_exit = 0;

    int last_alias = in_last_alias;
    for (int depth = 0; depth < 5; ++depth) {
        if (match_cmd(cmd, "alias", &args) || match_cmd(cmd, "unalias", &args)) break;
        if (!expand_alias(cmd, sizeof(work) - (size_t)(cmd - work), &last_alias)) break;
        cmd = trim(cmd);
    }

    if (!match_cmd(cmd, "alias", &args) && !strpbrk(cmd, "()") && (chain = find_and_and(cmd)) != NULL)
        return run_chain(cmd, chain, last_alias);

    if (!match_cmd(cmd, "alias", &args) && !match_cmd(cmd, "unalias", &args) && strip_background(cmd)) {
        run_background(cmd);
        return TRUE;
    }

    if (!_stricmp(cmd, "exit") || !_stricmp(cmd, "quit")) return FALSE;
    if (!_stricmp(cmd, "help") || !_stricmp(cmd, "nvs-help")) { print_help(); return TRUE; }
    if (!_stricmp(cmd, "about") || !_stricmp(cmd, "nvs-about")) {
        set_color(COLOR_ACCENT); printf("novaShell %s\n", APP_VERSION); reset_color();
        printf("A small Windows shell by Annabeth Kisling.\nhttps://tk-dev-software.com/software.novashell\n");
        return TRUE;
    }
    if (!_stricmp(cmd, "clear") || !_stricmp(cmd, "cls")) { system("cls"); return TRUE; }
    if (!_stricmp(cmd, "pwd")) {
        char cwd[PATH_CAP];
        if (GetCurrentDirectoryA(sizeof(cwd), cwd)) puts(cwd); else print_win_error("GetCurrentDirectory");
        return TRUE;
    }
    if (!_stricmp(cmd, "nvs-history")) { builtin_history(""); return TRUE; }
    if (!_stricmp(cmd, "history-clear")) { g_history_count = 0; history_save(); puts("Command history cleared."); return TRUE; }
    if (!_stricmp(cmd, "env")) { builtin_env(); return TRUE; }
    if (!_stricmp(cmd, "jobs")) { builtin_jobs(); return TRUE; }

    if (match_cmd(cmd, "history", &args)) { builtin_history(args); return TRUE; }
    if (match_cmd(cmd, "alias", &args)) { builtin_alias(args); return TRUE; }
    if (match_cmd(cmd, "unalias", &args)) { builtin_unalias(args); return TRUE; }
    if (match_cmd(cmd, "sshm", &args)) { builtin_sshm(args); return TRUE; }
    if (match_cmd(cmd, "bm", &args)) { builtin_bm(args); return TRUE; }
    if (match_cmd(cmd, "export", &args)) { builtin_export(args); return TRUE; }
    if (match_cmd(cmd, "unset", &args)) { builtin_unset(args); return TRUE; }
    if (match_cmd(cmd, "touch", &args)) { builtin_touch(args); return TRUE; }
    if (match_cmd(cmd, "mkcd", &args)) { builtin_mkcd(args); return TRUE; }
    if (match_cmd(cmd, "which", &args)) { builtin_which(args); return TRUE; }
    if (match_cmd(cmd, "source", &args)) { builtin_source(args); return TRUE; }
    if (match_cmd(cmd, "sudo", &args)) { builtin_sudo(args); return TRUE; }
    if (match_cmd(cmd, "config", &args)) { builtin_config(args); return TRUE; }
    if (match_cmd(cmd, "prompt", &args)) { builtin_prompt(args); return TRUE; }
    if (match_cmd(cmd, "fg", &args)) { builtin_fg(args); return TRUE; }
    if (match_cmd(cmd, "kill", &args)) { builtin_kill(args); return TRUE; }
    if (match_cmd(cmd, "title", &args)) { SetConsoleTitleA(args); return TRUE; }

    if (!strpbrk(cmd, "|<>&") && run_text_builtin(cmd)) return TRUE;

    if (match_cmd(cmd, "set", &args) && strchr(args, '=') && args[0] != '/') { builtin_export(args); return TRUE; }

    if (starts_with_ci(cmd, "cd") && (is_word_boundary(cmd[2]) || cmd[2] == '.' || cmd[2] == '\\' || cmd[2] == '/')) {
        builtin_cd(trim(cmd + 2));
        return TRUE;
    }

    if (match_cmd(cmd, "dir", &args) && args[0] != '/' && !strpbrk(args, "|<>&")) { builtin_dir(args); return TRUE; }

    if (match_cmd(cmd, "time", &args) && *args) {
        ULONGLONG t0 = GetTickCount64();
        BOOL keep_running = run_command(args);
        printf("\nElapsed: %.3f s\n", (double)(GetTickCount64() - t0) / 1000.0);
        return keep_running;
    }

    SetConsoleTitleA(cmd);
    execute_external(cmd);
    SetConsoleTitleA(APP_NAME);
    return TRUE;
}

static BOOL run_command(const char *line) { return run_command_ex(line, -1); }

static int exec_mode(int argc, char **argv) {
    char line[INPUT_CAP] = "";
    g_exec_mode = TRUE;
    SetConsoleCtrlHandler(console_handler, TRUE);
    if (!strcmp(argv[1], "--nova-line")) {
        snprintf(line, sizeof(line), "%s", argv[2]);
        init_paths(); load_ssh(); load_bookmarks();
        run_command(line);
    } else {
        for (int i = 2; i < argc; ++i) if (!append_arg(line, sizeof(line), argv[i])) return 1;
        if (!run_text_builtin(trim(line))) return 1;
    }
    fflush(stdout);
    return (int)g_last_exit;
}

int main(int argc, char **argv) {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    GetModuleFileNameA(NULL, g_self_path, sizeof(g_self_path));
    if (argc >= 3 && (!strcmp(argv[1], "--nova-exec") || !strcmp(argv[1], "--nova-line"))) return exec_mode(argc, argv);

    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_out, &info)) g_default_attr = info.wAttributes;
    SetConsoleTitleA(APP_NAME);
    SetConsoleCtrlHandler(console_handler, TRUE);

    init_paths(); load_aliases(); history_load(); load_ssh(); load_bookmarks(); load_prompt();

    set_color(COLOR_ACCENT); printf("novaShell %s\n", APP_VERSION); reset_color();
    printf("Type 'help' for commands. History is saved in %s\n\n", g_history_path);
    run_startup_script();
    maybe_run_novarc();

    char input[INPUT_CAP]; BOOL running = TRUE;
    while (running) {
        jobs_notify();
        update_prompt();
        BOOL got = read_line(input, sizeof(input));
        reset_color();
        if (!got) break;

        BOOL private_line = (input[0] == ' ');
        char line[INPUT_CAP];
        snprintf(line, sizeof(line), "%s", trim(input));
        if (!*line) continue;
        if (!expand_history(line, sizeof(line))) continue;
        if (!private_line) history_add(line);
        running = run_command(line);
    }
    history_save(); reset_color(); printf("novaShell closed.\n");
    return 0;
}