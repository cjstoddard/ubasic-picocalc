#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "pico/stdlib.h"

/* PicoCalc drivers */
#include "drivers/picocalc.h"
#include "drivers/keyboard.h"
#include "drivers/fat32.h"   // for fat32_dir_create()

/* uBASIC */
#include "ubasic/ubasic.h"
#include "ubasic/tokenizer.h"

/* Toggled by keyboard driver (BREAK/ATTN) */
volatile bool user_interrupt = false;

/* ============================ Keyboard helpers ============================ */
static inline void kb_service(void) { keyboard_poll(); }

static int kb_getch_nonblock(void) {
    kb_service();
    if (keyboard_key_available()) return (int)(unsigned char)keyboard_get_key();
    return -1;
}

static void read_line(char *buf, size_t maxlen) {
    size_t i = 0;
    for (;;) {
        int c = kb_getch_nonblock();
        if (c < 0) { tight_loop_contents(); continue; }

        if (c == '\r' || c == '\n') { putchar('\n'); buf[i] = 0; return; }
        if (c == 0x08 || c == 0x7f) { if (i) { i--; printf("\b \b"); } continue; }
        if ((unsigned)c >= 0x80) continue;
        if (c >= 32 && (i + 1) < maxlen) { buf[i++] = (char)c; putchar(c); }
    }
}

/* ============================ Program store / editor ============================ */
#define MAX_LINES       512
#define MAX_LINE_CHARS  160

typedef struct { int number; char *text; } prog_line_t;
static prog_line_t g_lines[MAX_LINES];
static int g_line_count = 0;

static void prog_clear(void) {
    for (int i = 0; i < g_line_count; ++i) { free(g_lines[i].text); g_lines[i].text = NULL; g_lines[i].number = 0; }
    g_line_count = 0;
}
static int prog_find_index(int number) {
    for (int i = 0; i < g_line_count; ++i) { if (g_lines[i].number == number) return i; if (g_lines[i].number > number) break; }
    return -1;
}
static void prog_insert_or_replace(int number, const char *text) {
    while (*text==' '||*text=='\t') text++;
    if (!*text) { // delete
        int idx = prog_find_index(number);
        if (idx >= 0) { free(g_lines[idx].text); for (int j=idx+1;j<g_line_count;++j) g_lines[j-1]=g_lines[j]; g_line_count--; }
        return;
    }
    int idx = prog_find_index(number);
    if (idx >= 0) { free(g_lines[idx].text); g_lines[idx].text = strdup(text); return; }
    if (g_line_count >= MAX_LINES) { printf("ERROR: program full\r\n"); return; }
    int ins = 0; while (ins < g_line_count && g_lines[ins].number < number) ins++;
    for (int j = g_line_count; j > ins; --j) g_lines[j] = g_lines[j-1];
    g_lines[ins].number = number; g_lines[ins].text = strdup(text); g_line_count++;
}
static void prog_list_range(int a, int b) {
    if (!g_line_count) { printf("(empty)\r\n"); return; }
    for (int i=0;i<g_line_count;++i) { int n=g_lines[i].number; if ((a>=0&&n<a)||(b>=0&&n>b)) continue; printf("%d %s\r\n", n, g_lines[i].text?g_lines[i].text:""); }
}
static int prog_max_line(void) {
    return g_line_count ? g_lines[g_line_count-1].number : 0;
}

/* Lowercase keywords outside of strings for this uBASIC build */
static void lowercase_outside_strings(char *s) {
    int in_str=0; for (; *s; ++s) { if (*s=='"'){ in_str=!in_str; continue; } if (!in_str) *s=(char)tolower((unsigned char)*s); }
}

/* Build program text WITH line numbers + LF endings + lowercased keywords */
static char *prog_build_buffer(size_t *out_size) {
    size_t total = 1;
    for (int i=0;i<g_line_count;++i) {
        const char *t=g_lines[i].text?g_lines[i].text:"";
        total += 6 + strlen(t) + 1; // "65535 "+text+"\n"
    }
    char *buf = (char*)malloc(total+1); if(!buf) return NULL;
    size_t pos=0;
    for (int i=0;i<g_line_count;++i) {
        const char *t=g_lines[i].text?g_lines[i].text:""; int n=g_lines[i].number;
        int w = snprintf(buf+pos, total-pos, "%d %s\n", n, t); if (w<0){ free(buf); return NULL; } pos += (size_t)w;
    }
    buf[pos]=0;
    lowercase_outside_strings(buf);
    if (out_size) *out_size = pos;
    return buf;
}

/* Ensure a NUMBERED 'end' exists (e.g., "65535 end") so ubasic_finished() becomes true. */
static char *ensure_program_has_numbered_end(char *buf) {
    const char *s = buf;
    while (*s) {
        const char *line = s, *e = strchr(s, '\n'); if(!e) e=s+strlen(s);
        while (line<e && (*line==' '||*line=='\t'||*line=='\r')) line++;
        const char *p=line; while (p<e && isdigit((unsigned char)*p)) p++;
        while (p<e && (*p==' '||*p=='\t')) p++;
        if ((e-p)>=3 && tolower((unsigned char)p[0])=='e' && tolower((unsigned char)p[1])=='n' && tolower((unsigned char)p[2])=='d')
            return buf;
        s = (*e ? e+1 : e);
    }
    int last = prog_max_line();
    int end_line = last < 65500 ? last + 10 : 65535;
    char tail[32];
    snprintf(tail, sizeof(tail), "%d end\n", end_line);
    size_t old = strlen(buf), add = strlen(tail);
    char *nbuf = (char*)realloc(buf, old + add + 1);
    if(!nbuf) return buf;
    memcpy(nbuf + old, tail, add + 1);
    return nbuf;
}

/* ============================ stdio-based SAVE / LOAD ============================ */

/* Create /ubasic if missing (ignore "already exists" errors). Uses FAT32 API directly. */
static void ensure_ubasic_dir(void) {
    fat32_file_t d;
    fat32_error_t rc = fat32_dir_create(&d, "/ubasic");
    if (rc == FAT32_OK) fat32_close(&d);
}

/* fopen helpers that try a few reasonable paths */
static FILE *try_open_for_write(const char *name, char out_path[256]) {
    static const char *prefixes[] = { "/ubasic/", "/", "" };
    for (int i=0;i<3;++i) {
        snprintf(out_path, 256, "%s%s", prefixes[i], name);
        FILE *fp = fopen(out_path, "wb");   // create/overwrite
        if (fp) return fp;
    }
    return NULL;
}
static FILE *try_open_for_read(const char *name, char out_path[256]) {
    static const char *prefixes[] = { "/ubasic/", "/", "" };
    char cand[256];

    // Try as typed, then uppercase (helps if 8.3 uppercase names are present)
    for (int pass=0; pass<2; ++pass) {
        const char *target = name;
        char up[256];
        if (pass==1) {
            size_t n = strlen(name);
            for (size_t i=0;i<n && i<255;i++) up[i] = (char)toupper((unsigned char)name[i]);
            up[n]=0;
            target = up;
        }
        for (int i=0;i<3;++i) {
            snprintf(cand, sizeof cand, "%s%s", prefixes[i], target);
            FILE *fp = fopen(cand, "rb");
            if (fp) { strncpy(out_path, cand, 256); return fp; }
        }
    }
    return NULL;
}

static int save_program_file(const char *typed_name) {
    size_t sz = 0;
    char *buf = prog_build_buffer(&sz);
    if (!buf) { printf("ERROR: out of memory\r\n"); return -1; }

    /* Save exactly what you wrote; RUN will append a numbered END if needed */
    ensure_ubasic_dir();

    char path[256];
    FILE *fp = try_open_for_write(typed_name, path);
    if (!fp) { printf("ERROR: SAVE failed (cannot create file)\r\n"); free(buf); return -1; }

    size_t wr = fwrite(buf, 1, sz, fp);
    fflush(fp);
    int cerr = ferror(fp);
    fclose(fp);
    free(buf);

    if (cerr) { printf("ERROR: SAVE I/O error\r\n"); return -1; }
    if (wr != sz) { printf("ERROR: SAVE short write (%u/%u)\r\n", (unsigned)wr, (unsigned)sz); return -1; }

    printf("Saved to %s\r\n", path);
    return 0;
}

static int load_program_file(const char *typed_name) {
    char path[256];
    FILE *fp = try_open_for_read(typed_name, path);
    if (!fp) { printf("ERROR: LOAD failed (file not found)\r\n"); return -1; }

    // Read line-by-line (works with LF/CRLF/CR)
    prog_line_t tmp[MAX_LINES];
    int tmp_count = 0;
    for (int i = 0; i < MAX_LINES; ++i) { tmp[i].number = 0; tmp[i].text = NULL; }

    char linebuf[512];
    int next_num = 10;
    while (fgets(linebuf, sizeof linebuf, fp)) {
        // strip trailing CR/LF
        size_t n = strlen(linebuf);
        while (n && (linebuf[n-1] == '\n' || linebuf[n-1] == '\r')) linebuf[--n] = 0;

        // skip empty lines
        char *p = linebuf;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        // parse optional leading line number
        char *q = p;
        while (isdigit((unsigned char)*q)) q++;
        int num = -1;
        if (q > p && (*q == ' ' || *q == '\t')) {
            num = atoi(p);
            while (*q == ' ' || *q == '\t') q++;
        }
        if (num < 0) { num = next_num; next_num += 10; }

        if (tmp_count >= MAX_LINES) { printf("WARNING: Truncated at %d lines\r\n", tmp_count); break; }
        tmp[tmp_count].number = num;
        tmp[tmp_count].text   = strdup(q ? q : "");
        tmp_count++;
    }
    int ioerr = ferror(fp);
    fclose(fp);

    if (ioerr) {
        for (int i = 0; i < tmp_count; ++i) free(tmp[i].text);
        printf("ERROR: LOAD I/O error while reading\r\n");
        return -1;
    }
    if (tmp_count == 0) {
        printf("ERROR: file empty or unreadable\r\n");
        return -1;
    }

    // Commit to program store
    prog_clear();
    for (int i = 0; i < tmp_count; ++i) {
        prog_insert_or_replace(tmp[i].number, tmp[i].text);
        free(tmp[i].text);
    }
    printf("Loaded %d line(s) from %s\r\n", tmp_count, path);
    return 0;
}

/* Handy: show raw file contents */
static void type_file(const char *name) {
    char path[256];
    FILE *fp = try_open_for_read(name, path);
    if (!fp) { printf("ERROR: not found\r\n"); return; }
    printf("----- %s -----\r\n", path);
    char buf[256];
    while (fgets(buf, sizeof buf, fp)) fputs(buf, stdout);
    fclose(fp);
    printf("\r\n---------------\r\n");
}

/* ============================ Helpers ============================ */
static void trim_inplace(char *s) {
    char *p=s; while(*p && isspace((unsigned char)*p)) p++; if(p!=s) memmove(s,p,strlen(p)+1);
    size_t n=strlen(s); while(n && isspace((unsigned char)s[n-1])) s[--n]=0;
}
static void print_help(void){
    printf("Commands:\r\n");
    printf("  NEW                 - clear program\r\n");
    printf("  LIST [a[-b]]        - list program (optional range)\r\n");
    printf("  RUN                 - run program\r\n");
    printf("  SAVE <name>         - save to /ubasic/<name> (fallback /<name>, <name>)\r\n");
    printf("  LOAD <name>         - load from those same locations\r\n");
    printf("  TYPE <name>         - display a file\r\n");
    printf("  HELP                - this message\r\n");
    printf("  (Or: <num> <text> to add/replace; '<num>' alone deletes.)\r\n");
}

/* ============================ Run ============================ */
static void run_current_program(void) {
    if (!g_line_count) { printf("(no program)\r\n"); return; }
    user_interrupt = false;

    size_t sz=0;
    char *buf=prog_build_buffer(&sz); if(!buf){ printf("ERROR: out of memory\r\n"); return; }
    buf = ensure_program_has_numbered_end(buf);

    printf("RUN\r\n");

    ubasic_init(buf);
    uint32_t steps=0;
    while(!ubasic_finished()){
        if(user_interrupt){ printf("\r\n** BREAK **\r\n"); break; }
        kb_service();
        ubasic_run();
        if((++steps & 0x3FFFu)==0) tight_loop_contents();
        if(steps > 5000000u){ printf("\r\n** Too many steps, aborting **\r\n"); break; }
    }
    free(buf);
    printf("\r\nREADY.\r\n");
}

/* ============================ Main / REPL ============================ */
int main(void){
    stdio_init_all();

    /* Sets up display/keyboard, wires stdio to SD via drivers/clib.c, etc. */
    picocalc_init();

    keyboard_init();
    keyboard_set_background_poll(true);

    printf("\x1b[2J\x1b[H");
    printf("uBASIC on PicoCalc (RP2350)\r\n");
    printf("--------------------------------\r\n");
    printf("READY.\r\n");

    char line[256];
    for(;;){
        printf("> ");
        read_line(line,sizeof(line));
        trim_inplace(line);
        if(!*line) continue;

        char *p=line;
        if(isdigit((unsigned char)*p)){       // program line
            long num=strtol(p,&p,10);
            if(num<=0||num>65535){ printf("ERROR: line number 1..65535\r\n"); continue; }
            while(*p==' '||*p=='\t') p++;
            prog_insert_or_replace((int)num,p);
            continue;
        }

        char cmd[16]={0}; int ci=0;
        p=line; while(*p && !isspace((unsigned char)*p) && ci<(int)sizeof(cmd)-1) cmd[ci++]=(char)toupper((unsigned char)*p++);
        while(*p && isspace((unsigned char)*p)) p++;

        if(!strcmp(cmd,"HELP")||!strcmp(cmd,"?"))            print_help();
        else if(!strcmp(cmd,"NEW"))                          (prog_clear(), printf("READY.\r\n"));
        else if(!strcmp(cmd,"LIST")) { int a=-1,b=-1; if(*p){ a=atoi(p); char *d=strchr(p,'-'); if(d) b=atoi(d+1);} prog_list_range(a,b); }
        else if(!strcmp(cmd,"RUN"))                          run_current_program();
        else if(!strcmp(cmd,"SAVE")) { if(!*p) printf("Usage: SAVE <name>\r\n"); else save_program_file(p); }
        else if(!strcmp(cmd,"LOAD")) { if(!*p) printf("Usage: LOAD <name>\r\n"); else if(load_program_file(p)==0) printf("READY.\r\n"); }
        else if(!strcmp(cmd,"TYPE")) { if(!*p) printf("Usage: TYPE <name>\r\n"); else type_file(p); }
        else                                                  printf("Unknown: %s  (type HELP)\r\n", cmd);

        fflush(stdout);
    }
    return 0;
}

