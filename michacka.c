#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_EDL_ENTRIES 96
#define PLAN_MARGIN 2.0
#define ANALYZE_BATCH 12
#define MAX_ENV_PTS 10
#define EDL_CAP 16384
#define CMD_CAP 32768
#define CMDFULL_CAP (1u << 22)

enum { ROLE_AMBIENT, ROLE_MOTION, ROLE_PULSE };
enum { ST_DAY, ST_STORM, ST_DRIFT, ST_PULSE, ST_RUPTURE };

typedef struct {
    char path[1024];
    double dur;
    float bpm;
    char key[16];
    float density, pulse, steady;
    char tex[16];
    int role;
} Track;

typedef struct {
    Track *v;
    int n, cap;
} TrackList;

typedef struct {
    float t, v;
} EnvPt;

typedef struct {
    const char *name;
    int def_parts;
    float def_len;
    EnvPt beds[MAX_ENV_PTS];
    int nb;
    EnvPt motion[MAX_ENV_PTS];
    int nm;
    EnvPt pulses[MAX_ENV_PTS];
    int np;
    EnvPt fields[MAX_ENV_PTS];
    int nfl;
    EnvPt gain[MAX_ENV_PTS];
    int ng;
    float fade_in[2];
    float fade_out[2];
    int parity;
    int bpm;
    int keylock;
    int nslide;
} StyleSpec;

typedef struct {
    char *buf;
    size_t len, cap;
    int count;
} Edl;

typedef struct {
    uint64_t seed;
    int have_seed;
    int parts;
    double len;
    int style;
    const char *mus_dir;
    const char *fld_dir;
    const char *img_dir;
    char tj_path[1024];
    char out_prefix[512];
    int dry_run;
    int slide;
    int slide_only;
    double slide_max_mb;
    int slide_days;
    int dense;
} Cfg;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "michacka: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    char *p = xmalloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

static double clampd(double x, double lo, double hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t rnd_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 2685821657736338717ULL;
}

static double rnd_unit(void)
{
    return (double)(rnd_next() >> 11) * (1.0 / 9007199254740992.0);
}

static double rnd_range(double lo, double hi)
{
    return lo + (hi - lo) * rnd_unit();
}

static void shuffle_ints(int *a, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rnd_next() % (uint64_t)(i + 1));
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static int has_audio_ext(const char *name)
{
    static const char *exts[] = { ".wav", ".flac", ".mp3", ".opus", ".ogg", ".m4a" };
    size_t len = strlen(name);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (len > el && strcasecmp(name + len - el, exts[i]) == 0) return 1;
    }
    return 0;
}

static int has_img_ext(const char *name)
{
    static const char *exts[] = { ".jpg", ".jpeg", ".png", ".webp" };
    size_t len = strlen(name);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (len > el && strcasecmp(name + len - el, exts[i]) == 0) return 1;
    }
    return 0;
}

static void push_path(char ***v, int *n, int *cap, char *path)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *v = xrealloc(*v, (size_t)*cap * sizeof(char *));
    }
    (*v)[(*n)++] = path;
}

static void scan_dir_rec(const char *dir, char ***v, int *n, int *cap,
                         int (*want)(const char *name))
{
    struct dirent **ents = NULL;
    int cnt = scandir(dir, &ents, NULL, alphasort);
    if (cnt < 0) {
        fprintf(stderr, "michacka: warning: cannot read %s: %s\n", dir, strerror(errno));
        return;
    }
    for (int i = 0; i < cnt; i++) {
        const char *name = ents[i]->d_name;
        if (name[0] != '.') {
            char path[1024];
            if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, name) < sizeof(path)) {
                struct stat st;
                if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    scan_dir_rec(path, v, n, cap, want);
                } else if (want(name) && !strchr(name, ',') && !strchr(name, '"')) {
                    push_path(v, n, cap, xstrdup(path));
                }
            }
        }
        free(ents[i]);
    }
    free(ents);
}

static void sh_quote(char *dst, size_t n, const char *src)
{
    size_t w = 0;
    if (n < 4) { dst[0] = 0; return; }
    dst[w++] = '"';
    for (const char *p = src; *p && w + 2 < n; p++) {
        if (*p == '"' || *p == '\\' || *p == '$' || *p == '`') dst[w++] = '\\';
        dst[w++] = *p;
    }
    dst[w++] = '"';
    dst[w] = 0;
}

static const char *path_tail(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int find_track(const TrackList *lib, const char *path)
{
    for (int i = 0; i < lib->n; i++)
        if (strcmp(lib->v[i].path, path) == 0) return i;
    const char *tail = path_tail(path);
    for (int i = 0; i < lib->n; i++)
        if (strcmp(path_tail(lib->v[i].path), tail) == 0) return i;
    return -1;
}

static int role_for(const char *label, float density, float pulse, float steady)
{
    if (strcmp(label, "ambient") == 0) return ROLE_AMBIENT;
    if (strcmp(label, "pulse") == 0) return ROLE_PULSE;
    if (strcmp(label, "motion") == 0) return ROLE_MOTION;
    if (pulse >= 0.45f && density >= 0.9f) return ROLE_PULSE;
    if (density < 0.6f && steady < 0.5f) return ROLE_AMBIENT;
    return ROLE_MOTION;
}

static void cat_cmd(char *cmd, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    if (*off >= cap) die("command line overflow");
    va_start(ap, fmt);
    int w = vsnprintf(cmd + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - *off) die("command line overflow");
    *off += (size_t)w;
}

static void analyze_batch(const char *tjbin, TrackList *lib, int start, int count)
{
    char cmd[CMD_CAP];
    size_t off = 0;
    cat_cmd(cmd, sizeof(cmd), &off, "%s analyze", tjbin);
    for (int i = start; i < start + count; i++) {
        char q[2100];
        sh_quote(q, sizeof(q), lib->v[i].path);
        cat_cmd(cmd, sizeof(cmd), &off, " %s", q);
    }
    FILE *fp = popen(cmd, "r");
    if (!fp) die("cannot run %s analyze", tjbin);
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        char *toks[1024];
        int nt = 0;
        char *tok = strtok(line, " \t\r\n");
        while (tok && nt < 1024) {
            toks[nt++] = tok;
            tok = strtok(NULL, " \t\r\n");
        }
        if (nt < 8 || strcmp(toks[nt - 6], "BPM") != 0) continue;
        char path[2048];
        size_t off2 = 0;
        path[0] = 0;
        for (int i = 0; i < nt - 7; i++) {
            int w = snprintf(path + off2, sizeof(path) - off2, "%s%s", i ? " " : "", toks[i]);
            if (w < 0 || (size_t)w >= sizeof(path) - off2) break;
            off2 += (size_t)w;
        }
        int ti = find_track(lib, path);
        if (ti < 0) continue;
        Track *t = &lib->v[ti];
        t->bpm = (float)atof(toks[nt - 7]);
        snprintf(t->key, sizeof(t->key), "%s", toks[nt - 5]);
        t->density = (float)atof(toks[nt - 4] + 2);
        t->pulse = (float)atof(strchr(toks[nt - 3], '=') + 1);
        t->steady = (float)atof(strchr(toks[nt - 2], '=') + 1);
        snprintf(t->tex, sizeof(t->tex), "%s", toks[nt - 1]);
        t->role = role_for(t->tex, t->density, t->pulse, t->steady);
    }
    int rc = pclose(fp);
    if (rc != 0) fprintf(stderr, "michacka: warning: tj analyze exited %d\n", rc);
}

static void analyze_lib(const char *tjbin, TrackList *lib, const char *label)
{
    int done = 0;
    for (int start = 0; start < lib->n; start += ANALYZE_BATCH) {
        int c = lib->n - start;
        if (c > ANALYZE_BATCH) c = ANALYZE_BATCH;
        analyze_batch(tjbin, lib, start, c);
        done += c;
        printf("\r  %s: analyzed %d/%d", label, done, lib->n);
        fflush(stdout);
    }
    printf("\n");
}

static double probe_duration(const Track *t)
{
    char cmd[2200], q[2048];
    sh_quote(q, sizeof(q), t->path);
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration -of default=nk=1:nw=1 %s", q);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1.0;
    char line[128] = "";
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        break;
    }
    int rc = pclose(fp);
    double d = atof(line);
    if (rc != 0 || d <= 0.0) return -1.0;
    return d;
}

static float env_eval(const EnvPt *e, int n, float t)
{
    if (n == 0) return 0.0f;
    if (t <= e[0].t) return e[0].v;
    if (t >= e[n - 1].t) return e[n - 1].v;
    for (int i = 0; i + 1 < n; i++) {
        if (t >= e[i].t && t <= e[i + 1].t) {
            float span = e[i + 1].t - e[i].t;
            float f = span > 0.0f ? (t - e[i].t) / span : 0.0f;
            return e[i].v + f * (e[i + 1].v - e[i].v);
        }
    }
    return e[n - 1].v;
}

static const StyleSpec STYLES[] = {
    { "day", 1, 600.0f,
      { {0.00f, 2}, {0.18f, 1}, {0.82f, 1}, {1.00f, 2} }, 4,
      { {0.00f, 1}, {0.25f, 2}, {0.42f, 3}, {0.58f, 3}, {0.78f, 2}, {1.00f, 1} }, 6,
      { {0.00f, 0}, {0.15f, 1}, {0.32f, 3}, {0.55f, 3}, {0.72f, 2}, {0.86f, 0}, {1.00f, 0} }, 7,
      { {0.00f, 2}, {0.38f, 3}, {0.62f, 3}, {0.85f, 2}, {1.00f, 3} }, 5,
      { {0.0f, 0.85f}, {0.5f, 1.0f}, {1.0f, 0.85f} }, 3,
      { 14.0f, 28.0f }, { 16.0f, 28.0f }, 0, 0, 0, 12 },
    { "storm", 2, 300.0f,
      { {0.0f, 1}, {1.0f, 1} }, 2,
      { {0.0f, 2}, {0.25f, 4}, {0.75f, 4}, {1.0f, 2} }, 4,
      { {0.0f, 1}, {0.15f, 4}, {0.85f, 4}, {1.0f, 2} }, 4,
      { {0.0f, 1}, {0.5f, 2}, {1.0f, 1} }, 3,
      { {0.0f, 0.6f}, {0.12f, 1.0f}, {0.82f, 1.0f}, {1.0f, 0.65f} }, 4,
      { 3.0f, 6.0f }, { 4.0f, 8.0f }, 0, 0, 0, 10 },
    { "drift", 1, 600.0f,
      { {0.0f, 2}, {1.0f, 2} }, 2,
      { {0.0f, 1}, {0.5f, 2}, {1.0f, 1} }, 3,
      { {0.0f, 0}, {1.0f, 0} }, 2,
      { {0.0f, 2}, {0.5f, 3}, {1.0f, 2} }, 3,
      { {0.0f, 0.8f}, {0.5f, 0.95f}, {1.0f, 0.8f} }, 3,
      { 18.0f, 35.0f }, { 20.0f, 35.0f }, 0, 0, 0, 10 },
    { "pulse", 1, 600.0f,
      { {0.0f, 1}, {1.0f, 1} }, 2,
      { {0.0f, 2}, {1.0f, 2} }, 2,
      { {0.0f, 3}, {0.15f, 5}, {0.9f, 4}, {1.0f, 3} }, 4,
      { {0.0f, 1}, {0.5f, 2}, {1.0f, 1} }, 3,
      { {0.0f, 0.7f}, {0.1f, 1.0f}, {0.92f, 1.0f}, {1.0f, 0.75f} }, 4,
      { 4.0f, 8.0f }, { 5.0f, 10.0f }, 0, 1, 0, 12 },
    { "rupture", 2, 300.0f,
      { {0.0f, 1}, {1.0f, 1} }, 2,
      { {0.0f, 2}, {0.5f, 3}, {1.0f, 2} }, 3,
      { {0.0f, 1}, {0.5f, 3}, {1.0f, 1} }, 3,
      { {0.0f, 1}, {0.5f, 2}, {1.0f, 1} }, 3,
      { {0.0f, 0.5f}, {0.1f, 1.0f}, {0.75f, 1.0f}, {1.0f, 0.6f} }, 4,
      { 2.0f, 4.0f }, { 3.0f, 6.0f }, 1, 1, 1, 10 },
};

static int style_by_name(const char *name)
{
    for (size_t i = 0; i < sizeof(STYLES) / sizeof(STYLES[0]); i++)
        if (strcmp(STYLES[i].name, name) == 0) return (int)i;
    return -1;
}

static int layer_count(const StyleSpec *st, const EnvPt *e, int n, float phase, int part_idx)
{
    float v = env_eval(e, n, phase);
    if (st->parity) v = (part_idx % 2 == 0) ? v * 1.5f : v * 0.5f;
    int c = (int)lroundf(v);
    return c > MAX_EDL_ENTRIES ? MAX_EDL_ENTRIES : c;
}

static int collect_roles(const TrackList *lib, int role, int *out)
{
    int c = 0;
    for (int i = 0; i < lib->n; i++)
        if (lib->v[i].role == role) out[c++] = i;
    return c;
}

static int pick_slice(Track *t, double want_span, double *in_sec, double *span)
{
    if (t->dur == 0.0) t->dur = probe_duration(t);
    double dur = t->dur > 0.0 ? t->dur : 0.0;
    if (dur <= 0.0) {
        *span = clampd(want_span, 3.0, 90.0);
        *in_sec = rnd_range(0.0, 30.0);
        return 1;
    }
    double s = want_span;
    if (s > dur * 0.9) s = dur * 0.9;
    if (s < 3.0) return 0;
    *span = s;
    *in_sec = dur > s ? rnd_range(0.0, dur - s) : 0.0;
    return 1;
}

static void edl_init(Edl *e)
{
    e->cap = EDL_CAP;
    e->buf = xmalloc((size_t)e->cap);
    e->buf[0] = 0;
    e->len = 0;
    e->count = 0;
}

static void edl_free(Edl *e)
{
    free(e->buf);
    e->buf = NULL;
}

static void edl_put(Edl *e, const char *fmt, ...)
{
    va_list ap;
    char entry[1600];
    va_start(ap, fmt);
    int w = vsnprintf(entry, sizeof(entry), fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= sizeof(entry)) die("EDL entry too long");
    size_t need = e->len + (size_t)w + 2;
    if (need > (size_t)e->cap) {
        while (e->cap < need) e->cap *= 2;
        e->buf = xrealloc(e->buf, (size_t)e->cap);
    }
    if (e->count > 0) e->buf[e->len++] = ',';
    memcpy(e->buf + e->len, entry, (size_t)w + 1);
    e->len += (size_t)w;
    e->count++;
}

static int add_layered_entries(Edl *edl, TrackList *lib, int role, int want,
                               double len, double span_lo, double span_hi,
                               double vol_lo, double vol_hi, double fin_lo, double fin_hi,
                               int *warned_no_role)
{
    if (want <= 0 || lib->n == 0) return 0;
    int *order = xmalloc(sizeof(int) * (size_t)lib->n);
    int avail = collect_roles(lib, role, order);
    if (avail == 0) {
        if (!*warned_no_role) {
            fprintf(stderr, "michacka: warning: no %s-role source found, layer skipped\n",
                    role == ROLE_AMBIENT ? "ambient" : role == ROLE_MOTION ? "motion" : "pulse");
            *warned_no_role = 1;
        }
        free(order);
        return 0;
    }
    shuffle_ints(order, (size_t)avail);
    double max_sp = len - PLAN_MARGIN;
    double slo = span_lo, shi = span_hi;
    if (role != ROLE_AMBIENT) {
        if (shi > len * 0.45) shi = len * 0.45;
        if (slo > shi) slo = shi * 0.6;
    }
    int pos = 0;
    int added = 0;
    for (int k = 0; k < want; k++) {
        if (edl->count >= MAX_EDL_ENTRIES) break;
        if (pos >= avail) {
            if (avail >= want || pos == 0) break;
            pos = 0;
        }
        Track *t = &lib->v[order[pos]];
        pos++;
        double in_sec, sp;
        if (!pick_slice(t, rnd_range(slo, shi), &in_sec, &sp)) continue;
        if (sp > max_sp) {
            sp = max_sp;
            if (t->dur > 0.0 && in_sec > t->dur - sp) in_sec = t->dur > sp ? t->dur - sp : 0.0;
        }
        if (sp < 3.0) continue;
        double at = 0.0;
        if (role != ROLE_AMBIENT) {
            double slot = ((double)added + 0.5) * len / (double)want;
            at = clampd(slot + rnd_range(-0.07, 0.07) * len, 0.0, max_sp - sp);
            if (at < 0.0) at = 0.0;
        }
        double vol = rnd_range(vol_lo, vol_hi);
        double fin = rnd_range(fin_lo, fin_hi);
        double fout = rnd_range(fin_lo, fin_hi);
        edl_put(edl, "in%.1f out%.1f at%.1f v%.0f fin%.1f fout%.1f %s",
                in_sec, in_sec + sp, at, vol, fin, fout, t->path);
        added++;
    }
    free(order);
    return added;
}

static int add_field_entries(Edl *edl, TrackList *fld, int want, double len)
{
    if (want <= 0 || fld->n == 0) return 0;
    int *order = xmalloc(sizeof(int) * (size_t)fld->n);
    for (int i = 0; i < fld->n; i++) order[i] = i;
    shuffle_ints(order, (size_t)fld->n);
    double max_sp = len - PLAN_MARGIN;
    double slo = 90.0, shi = 300.0;
    if (shi > len * 0.5) shi = len * 0.5;
    if (slo > shi) slo = shi * 0.6;
    int added = 0;
    for (int k = 0; k < want && added < want; k++) {
        if (edl->count >= MAX_EDL_ENTRIES) break;
        Track *t = &fld->v[order[k % fld->n]];
        double in_sec, sp;
        if (!pick_slice(t, rnd_range(slo, shi), &in_sec, &sp)) continue;
        if (sp > max_sp) {
            sp = max_sp;
            if (t->dur > 0.0 && in_sec > t->dur - sp) in_sec = t->dur > sp ? t->dur - sp : 0.0;
        }
        if (sp < 3.0) continue;
        double slot = ((double)added + rnd_range(0.35, 0.65)) * len / (double)want;
        double at = clampd(slot, 0.0, max_sp - sp);
        if (at < 0.0) at = 0.0;
        double vol = rnd_range(-8.0, -2.0);
        double fin = rnd_range(6.0, 20.0);
        double fout = rnd_range(6.0, 20.0);
        edl_put(edl, "in%.1f out%.1f at%.1f v%.0f fin%.1f fout%.1f %s",
                in_sec, in_sec + sp, at, vol, fin, fout, t->path);
        added++;
    }
    free(order);
    return added;
}

static void build_arc(char *buf, size_t cap, const StyleSpec *st, double len)
{
    size_t off = 0;
    int K = 5;
    buf[0] = 0;
    for (int i = 0; i < K; i++) {
        float t = (float)i / (float)(K - 1);
        float g = env_eval(st->gain, st->ng, t) + (float)rnd_range(-0.04, 0.04);
        g = clampd(g, 0.05, 1.0);
        int w = snprintf(buf + off, cap - off, "%s%.0f:%.2f", i ? "," : "", t * len, g);
        if (w < 0 || (size_t)w >= cap - off) die("arc overflow");
        off += (size_t)w;
    }
}

static void write_edl_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write %s: %s", path, strerror(errno));
    fprintf(fp, "%s\n", content);
    fclose(fp);
}

static int xfile(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISREG(sb.st_mode) && access(path, X_OK) == 0;
}

static void resolve_tj(Cfg *cfg)
{
    char cand[1024];
    const char *env = getenv("MICHACKA_TJ");
    if (env && xfile(env)) {
        snprintf(cfg->tj_path, sizeof(cfg->tj_path), "%s", env);
        return;
    }
    if (xfile(cfg->tj_path)) return;
    static const char *defaults[] = { "./tj", "tj/tj", "../tj/tj", "../../tj/tj" };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        if (xfile(defaults[i])) {
            snprintf(cfg->tj_path, sizeof(cfg->tj_path), "%s", defaults[i]);
            return;
        }
    }
    if (access("tj", F_OK) == 0) {
        fprintf(stderr, "michacka: building tj submodule ...\n");
        int rc = system("make -s -C tj");
        if (rc == 0 && xfile("tj/tj")) {
            snprintf(cfg->tj_path, sizeof(cfg->tj_path), "%s", "tj/tj");
            return;
        }
    }
    if (access("../tj", X_OK) == 0) {
        fprintf(stderr, "michacka: building tj in ../tj ...\n");
        int rc = system("make -s -C ../tj");
        if (rc == 0 && xfile("../tj/tj")) {
            snprintf(cfg->tj_path, sizeof(cfg->tj_path), "%s", "../tj/tj");
            return;
        }
    }
    snprintf(cand, sizeof(cand), "%s", cfg->tj_path);
    die("tj renderer not found (tried %s, MICHACKA_TJ; hint: git submodule update --init)",
        cand);
}

static void write_plan_files(const Cfg *cfg, char **music_edls, char **field_edls)
{
    for (int p = 0; p < cfg->parts; p++) {
        char fp_[700];
        snprintf(fp_, sizeof(fp_), "%s_part%02d_music.edl", cfg->out_prefix, p + 1);
        write_edl_file(fp_, music_edls[p]);
        snprintf(fp_, sizeof(fp_), "%s_part%02d_field.edl", cfg->out_prefix, p + 1);
        write_edl_file(fp_, field_edls[p]);
    }
}

static int render_part(const Cfg *cfg, const StyleSpec *st, const char *music_edl,
                       const char *field_edl, const char *arc, int idx)
{
    char base[600], music_wav[700], part_wav[700], master_wav[700], cmd[CMD_CAP];
    snprintf(base, sizeof(base), "%s_part%02d", cfg->out_prefix, idx + 1);
    snprintf(music_wav, sizeof(music_wav), "%s_music.wav", base);
    snprintf(part_wav, sizeof(part_wav), "%s.wav", base);
    snprintf(master_wav, sizeof(master_wav), "%s_master.wav", base);

    if (access(master_wav, F_OK) == 0) {
        printf("[part %02d] exists, skipping\n", idx + 1);
        return 0;
    }

    char qedl[EDL_CAP + 16];
    sh_quote(qedl, sizeof(qedl), music_edl);
    snprintf(cmd, sizeof(cmd), "%s %s %s%s%s --arc \"%s\" --fade-in 0.5 --fade-out 2",
             cfg->tj_path, qedl, music_wav,
             st->bpm ? " --bpm auto --snap" : "",
             st->keylock ? " --keylock auto" : "", arc);
    printf("[part %02d] pass A: music (%s)\n", idx + 1, st->name);
    fflush(stdout);
    if (system(cmd) != 0) {
        fprintf(stderr, "michacka: part %02d pass A failed\n", idx + 1);
        return 1;
    }

    char bed[1024], combined[EDL_CAP + 2048];
    snprintf(bed, sizeof(bed), "in0 at0 v0 fin0.5 fout0.5 %s", music_wav);
    snprintf(combined, sizeof(combined), "%s,%s", bed, field_edl);
    sh_quote(qedl, sizeof(qedl), combined);
    snprintf(cmd, sizeof(cmd), "%s %s %s --fade-in 0.5 --fade-out 2 --master subtle",
             cfg->tj_path, qedl, part_wav);
    printf("[part %02d] pass B: + field recordings\n", idx + 1);
    fflush(stdout);
    if (system(cmd) != 0) {
        fprintf(stderr, "michacka: part %02d pass B failed\n", idx + 1);
        return 1;
    }
    return 0;
}

static int render_all_parts(const Cfg *cfg, const StyleSpec *st, char **music_edls,
                            char **field_edls, char **arcs)
{
    for (int i = 0; i < cfg->parts; i++)
        if (render_part(cfg, st, music_edls[i], field_edls[i], arcs[i], i)) return 1;
    return 0;
}

static double ffprobe_duration_file(const char *path)
{
    char cmd[2200], q[2048];
    sh_quote(q, sizeof(q), path);
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration -of default=nk=1:nw=1 %s", q);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1.0;
    char line[128] = "";
    if (fgets(line, sizeof(line), fp)) line[strcspn(line, "\r\n")] = 0;
    pclose(fp);
    double d = atof(line);
    return d > 0.0 ? d : -1.0;
}

static void finish_mix(const Cfg *cfg)
{
    char mix_wav[700], list_path[700];
    snprintf(mix_wav, sizeof(mix_wav), "%s_mix.wav", cfg->out_prefix);
    snprintf(list_path, sizeof(list_path), "%s_concat.txt", cfg->out_prefix);

    printf("=== concatenating %d parts ===\n", cfg->parts);
    FILE *lp = fopen(list_path, "w");
    if (!lp) die("cannot write %s", list_path);
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) die("getcwd failed");
    for (int i = 0; i < cfg->parts; i++)
        fprintf(lp, "file '%s/%s_part%02d_master.wav'\n", cwd, cfg->out_prefix, i + 1);
    fclose(lp);

    char cmd[CMD_CAP], qlist[760];
    sh_quote(qlist, sizeof(qlist), list_path);
    unlink(mix_wav);
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -y -f concat -safe 0 -i %s -c copy %s", qlist, mix_wav);
    if (system(cmd) != 0) die("concat failed");
    unlink(list_path);

    printf("=== exports ===\n");
    char flac[700], mp3[700], qmix[760];
    snprintf(flac, sizeof(flac), "%s_mix.flac", cfg->out_prefix);
    snprintf(mp3, sizeof(mp3), "%s_mix.mp3", cfg->out_prefix);
    sh_quote(qmix, sizeof(qmix), mix_wav);
    if (access(flac, F_OK) != 0) {
        snprintf(cmd, sizeof(cmd), "ffmpeg -v error -y -i %s -c:a flac %s", qmix, flac);
        if (system(cmd) != 0) fprintf(stderr, "michacka: flac export failed\n");
    }
    if (access(mp3, F_OK) != 0) {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -v error -y -i %s -c:a libmp3lame -b:a 320k %s", qmix, mp3);
        if (system(cmd) != 0) fprintf(stderr, "michacka: mp3 export failed\n");
    }

    const char *names[] = { mix_wav, flac, mp3 };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (access(names[i], F_OK) != 0) continue;
        double d = ffprobe_duration_file(names[i]);
        if (d > 0) printf("%-40s %.1f s (%.1f min)\n", names[i], d, d / 60.0);
    }
}

static void cleanup_parts(const Cfg *cfg)
{
    printf("=== cleaning intermediate renders ===\n");
    for (int p = 0; p < cfg->parts; p++) {
        char path[760];
        snprintf(path, sizeof(path), "%s_part%02d_music.wav", cfg->out_prefix, p + 1);
        unlink(path);
        snprintf(path, sizeof(path), "%s_part%02d.wav", cfg->out_prefix, p + 1);
        unlink(path);
        snprintf(path, sizeof(path), "%s_part%02d_master.wav", cfg->out_prefix, p + 1);
        unlink(path);
        snprintf(path, sizeof(path), "%s_part%02d.edl", cfg->out_prefix, p + 1);
        unlink(path);
        snprintf(path, sizeof(path), "%s_part%02d_master.edl", cfg->out_prefix, p + 1);
        unlink(path);
        snprintf(path, sizeof(path), "%s_part%02d.edl", cfg->out_prefix, p + 1);
        unlink(path);
    }
}

static void load_library(TrackList *lib, const char *dir, const char *label)
{
    char **paths = NULL;
    int n = 0, cap = 0;
    scan_dir_rec(dir, &paths, &n, &cap, has_audio_ext);
    if (n == 0) die("no audio files found in %s (%s)", dir, label);
    int keep = n;
    if (n > 1000) {
        int *idx = xmalloc(sizeof(int) * (size_t)n);
        for (int i = 0; i < n; i++) idx[i] = i;
        shuffle_ints(idx, n);
        for (int i = 0; i < 1000; i++) {
            char *tmp = paths[i];
            paths[i] = paths[idx[i]];
            paths[idx[i]] = tmp;
        }
        free(idx);
        keep = 1000;
        for (int i = keep; i < n; i++) free(paths[i]);
        printf("  %s: found %d files, sampling %d\n", label, n, keep);
    }
    lib->v = xmalloc(sizeof(Track) * (size_t)keep);
    lib->cap = keep;
    lib->n = keep;
    for (int i = 0; i < keep; i++) {
        memset(&lib->v[i], 0, sizeof(Track));
        snprintf(lib->v[i].path, sizeof(lib->v[i].path), "%s", paths[i]);
        lib->v[i].tex[0] = '?';
        lib->v[i].key[0] = '?';
    }
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
    if (keep == n) printf("  %s: %d files\n", label, lib->n);
}

typedef struct {
    char **v;
    int n, cap;
} PathList;

static time_t image_taken(const char *path)
{
    const char *base = path_tail(path);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (strncmp(base, "IMG_", 4) == 0 &&
        sscanf(base + 4, "%4d%2d%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6) {
        struct tm t;
        memset(&t, 0, sizeof(t));
        t.tm_year = y - 1900;
        t.tm_mon = mo - 1;
        t.tm_mday = d;
        t.tm_hour = h;
        t.tm_min = mi;
        t.tm_sec = s;
        t.tm_isdst = -1;
        time_t v = mktime(&t);
        if (v != (time_t)-1) return v;
    }
    struct stat st;
    if (stat(path, &st) == 0) return st.st_mtime;
    return (time_t)-1;
}

static long tod_minutes(time_t t)
{
    if (t == (time_t)-1) return -1;
    struct tm tm;
    if (localtime_r(&t, &tm) == NULL) return -1;
    return (long)tm.tm_hour * 60 + (long)tm.tm_min;
}

static int cmp_images_tod(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    long ma = tod_minutes(image_taken(pa));
    long mb = tod_minutes(image_taken(pb));
    if (ma != mb) return ma < mb ? -1 : 1;
    return strcmp(pa, pb);
}

static void load_images(PathList *pl, const char *dir, int days)
{
    pl->v = NULL;
    pl->n = 0;
    pl->cap = 0;
    scan_dir_rec(dir, &pl->v, &pl->n, &pl->cap, has_img_ext);
    if (days > 0 && pl->n > 0) {
        time_t now = time(NULL);
        time_t cut = now - (time_t)days * 86400;
        int w = 0;
        int before = pl->n;
        for (int i = 0; i < pl->n; i++) {
            time_t t = image_taken(pl->v[i]);
            if (t == (time_t)-1 || t >= cut)
                pl->v[w++] = pl->v[i];
            else
                free(pl->v[i]);
        }
        pl->n = w;
        if (pl->n == 0) {
            fprintf(stderr, "michacka: warning: no images from the last %d day(s) in %s, slides skipped\n",
                    days, dir);
            free(pl->v);
            pl->v = NULL;
            pl->cap = 0;
            return;
        }
        if (before > pl->n)
            printf("  slides: %d of %d images fall in the last %d day(s)\n",
                   pl->n, before, days);
    }
    if (pl->n == 0) {
        fprintf(stderr, "michacka: warning: no images found in %s, slides skipped\n", dir);
        return;
    }
    if (pl->n > 1000) {
        int *idx = xmalloc(sizeof(int) * (size_t)pl->n);
        for (int i = 0; i < pl->n; i++) idx[i] = i;
        shuffle_ints(idx, pl->n);
        for (int i = 0; i < 1000; i++) {
            char *tmp = pl->v[i];
            pl->v[i] = pl->v[idx[i]];
            pl->v[idx[i]] = tmp;
        }
        free(idx);
        for (int i = 1000; i < pl->n; i++) free(pl->v[i]);
        pl->n = 1000;
    }
    qsort(pl->v, (size_t)pl->n, sizeof(char *), cmp_images_tod);
}

typedef struct {
    char *path;
    double dur;
} Slide;

typedef struct {
    Slide *v;
    int n;
    double crossfade;
    double total;
} SlidePlan;

#define SLIDE_FPS 25
#define SLIDE_MAX_FRAMES 7
#define SLIDE_W 1920
#define SLIDE_H 1080
#define SLIDE_PAD_W 2700
#define SLIDE_PAD_H 1520

static void slide_plan_init(SlidePlan *sp, int n, double total)
{
    sp->v = xmalloc(sizeof(Slide) * (size_t)n);
    sp->n = n;
    sp->crossfade = 0.0;
    sp->total = total;
}

static void slide_plan_free(SlidePlan *sp)
{
    for (int i = 0; i < sp->n; i++) free(sp->v[i].path);
    free(sp->v);
    sp->v = NULL;
    sp->n = 0;
}

/* Rapid montage: every photo gets at most SLIDE_MAX_FRAMES frames of screen
 * time.  The list is already ordered by time-of-day (day ignored), so the cut
 * sequence walks the day; when the pool is larger than the number of cuts
 * needed it is sampled evenly across the whole day, otherwise it cycles so the
 * montage still fills the full mix duration. */
static void plan_slides(SlidePlan *sp, const PathList *imgs, int want, double total)
{
    int m = imgs ? imgs->n : 0;
    if (m < 1) {
        sp->v = NULL;
        sp->n = 0;
        sp->crossfade = 0.0;
        sp->total = 0.0;
        return;
    }
    if (total <= 0.0) total = 60.0;
    double dur = (double)SLIDE_MAX_FRAMES / (double)SLIDE_FPS;
    int need = (int)ceil(total / dur);
    if (need < 1) need = 1;
    int distinct = want > 0 && want < m ? want : m;
    slide_plan_init(sp, need, total);
    for (int i = 0; i < need; i++) {
        int idx;
        if (distinct >= need)
            idx = (int)((double)i * (double)distinct / (double)need);
        else
            idx = i % distinct;
        sp->v[i].path = xstrdup(imgs->v[idx]);
        sp->v[i].dur = dur;
    }
}

static void write_slide_plan(const Cfg *cfg, const SlidePlan *sp)
{
    char path[700];
    snprintf(path, sizeof(path), "%s_slides.edl", cfg->out_prefix);
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write %s: %s", path, strerror(errno));
    for (int i = 0; i < sp->n; i++)
        fprintf(fp, "slide%04d dur%.2f photo %s\n", i + 1, sp->v[i].dur, sp->v[i].path);
    fclose(fp);
}

typedef struct {
    int w, h;
    int pw, ph;
    int vkbps;
    int audio_kbps;
} SlideEncode;

static SlideEncode slide_encode_config(double max_mb, double total)
{
    SlideEncode e;
    memset(&e, 0, sizeof(e));
    if (total <= 0.0) total = 60.0;
    e.audio_kbps = 128;
    if (max_mb <= 0.0) {
        e.w = SLIDE_W;
        e.h = SLIDE_H;
        e.pw = SLIDE_PAD_W;
        e.ph = SLIDE_PAD_H;
        e.vkbps = 0;
        return e;
    }
    double a_bytes = (double)e.audio_kbps / 8.0 * 1000.0 * total;
    double avail = max_mb * 1000000.0 * 0.96 - a_bytes;
    if (avail < 1000000.0) avail = 1000000.0;
    double kbps = avail * 8.0 / total / 1000.0;
    if (kbps < 600.0) kbps = 600.0;
    if (kbps > 24000.0) kbps = 24000.0;
    e.vkbps = (int)kbps;
    if (e.vkbps < 1400) {
        e.w = 1280;
        e.h = 720;
    } else if (e.vkbps < 6000) {
        e.w = SLIDE_W;
        e.h = SLIDE_H;
    } else {
        e.w = 2560;
        e.h = 1440;
    }
    e.pw = (int)(e.w * 1.4 / 2) * 2;
    e.ph = (int)(e.h * 1.4 / 2) * 2;
    return e;
}

static int title_text(char *dst, size_t n, time_t when)
{
    struct tm t;
    if (localtime_r(&when, &t) == NULL) return 0;
    int yy = (t.tm_year + 1900) % 100;
    int w = snprintf(dst, n, "Kof %02d", yy);
    return w > 0 && (size_t)w < n;
}

static const char *title_font_path(void)
{
    static char buf[1024];
    const char *env = getenv("MICHACKA_TITLE_FONT");
    if (env && access(env, R_OK) == 0) return env;
    const char *home = getenv("HOME");
    if (home) {
        snprintf(buf, sizeof(buf), "%s/.fonts/gomotor.ttf", home);
        if (access(buf, R_OK) == 0) return buf;
        snprintf(buf, sizeof(buf), "%s/.local/share/fonts/gomotor.ttf", home);
        if (access(buf, R_OK) == 0) return buf;
        snprintf(buf, sizeof(buf), "%s/src/gomotor/GoMotor.ttf", home);
        if (access(buf, R_OK) == 0) return buf;
    }
    snprintf(buf, sizeof(buf), "/usr/share/fonts/truetype/gomotor/gomotor.ttf");
    if (access(buf, R_OK) == 0) return buf;
    return NULL;
}

static void render_slides(const Cfg *cfg, const SlidePlan *sp)
{
    char out[700], qout[760], mix[700], qmix[760];
    snprintf(out, sizeof(out), "%s_slides.mp4", cfg->out_prefix);
    sh_quote(qout, sizeof(qout), out);
    snprintf(mix, sizeof(mix), "%s_mix.wav", cfg->out_prefix);
    sh_quote(qmix, sizeof(qmix), mix);

    SlideEncode enc = slide_encode_config(cfg->slide_max_mb, sp->total);

    int nin = 0, ncap = 0;
    char **ins = NULL;
    int *imap = xmalloc(sizeof(int) * (size_t)sp->n);
    for (int i = 0; i < sp->n; i++) {
        int j = 0;
        for (; j < nin; j++)
            if (strcmp(ins[j], sp->v[i].path) == 0) break;
        if (j == nin) {
            push_path(&ins, &nin, &ncap, xstrdup(sp->v[i].path));
            imap[i] = nin - 1;
        } else {
            imap[i] = j;
        }
    }

    const char *tt = NULL;
    char tn[32];
    const char *font = title_font_path();
    if (font) {
        tt = tn;
        if (!title_text(tn, sizeof(tn), time(NULL)))
            tt = NULL;
    }

    char *base = xmalloc(CMDFULL_CAP);
    size_t off = 0;
    cat_cmd(base, CMDFULL_CAP, &off, "ffmpeg -v error -y");
    for (int i = 0; i < nin; i++) {
        char q[2100];
        sh_quote(q, sizeof(q), ins[i]);
        cat_cmd(base, CMDFULL_CAP, &off, " -i %s", q);
    }
    cat_cmd(base, CMDFULL_CAP, &off, " -i %s", qmix);

    cat_cmd(base, CMDFULL_CAP, &off, " -filter_complex \"");
    for (int i = 0; i < sp->n; i++)
        cat_cmd(base, CMDFULL_CAP, &off,
                "[%d:v]scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d,"
                "zoompan=z='1':d=%d:s=%dx%d:fps=%d,setsar=1,format=yuv420p[vs%d];",
                imap[i], enc.w, enc.h, enc.w, enc.h, SLIDE_MAX_FRAMES,
                enc.w, enc.h, SLIDE_FPS, i);
    cat_cmd(base, CMDFULL_CAP, &off, "[vs0]");
    for (int i = 1; i < sp->n; i++)
        cat_cmd(base, CMDFULL_CAP, &off, "[vs%d]", i);
    cat_cmd(base, CMDFULL_CAP, &off, "concat=n=%d:v=1:a=0,format=yuv420p[vout]", sp->n);
    if (tt && font)
        cat_cmd(base, CMDFULL_CAP, &off,
                ";[vout]drawtext=fontfile='%s':text='%s':fontsize=%d:fontcolor=white:"
                "borderw=3:bordercolor=black@0.6:x=(w-text_w)/2:y=(h-text_h)/2"
                ":alpha='if(lt(t,0.5),t/0.5,if(lt(t,3.2),1,if(lt(t,3.8),(3.8-t)/0.6,0)))'[vt]",
                font, tt, (int)(enc.h * 0.13));
    else
        cat_cmd(base, CMDFULL_CAP, &off, ";[vout]null[vt]");
    cat_cmd(base, CMDFULL_CAP, &off, "\"");

    char *cmd = xmalloc(CMDFULL_CAP);
    if (enc.vkbps > 0) {
        char plog[820], qplog[880], p1[820], qp1[880];
        snprintf(plog, sizeof(plog), "/tmp/opencode/michacka_%s_pass", path_tail(cfg->out_prefix));
        sh_quote(qplog, sizeof(qplog), plog);
        snprintf(p1, sizeof(p1), "/tmp/opencode/michacka_%s_pass1.mp4", path_tail(cfg->out_prefix));
        sh_quote(qp1, sizeof(qp1), p1);
        snprintf(cmd, CMDFULL_CAP, "%s", base);
        off = strlen(cmd);
        cat_cmd(cmd, CMDFULL_CAP, &off,
                " -map [vt] -an -c:v libx264 -preset faster -b:v %dk -pass 1 "
                "-passlogfile %s -t %.3f -y %s",
                enc.vkbps, qplog, sp->total, qp1);
        printf("=== rendering slides: %d photos, %.1f s (2-pass to fill ~%g MB) ===\n",
               sp->n, sp->total, cfg->slide_max_mb);
        fflush(stdout);
        if (system(cmd) != 0) die("slides render pass 1 failed");
        remove(p1);
        snprintf(cmd, CMDFULL_CAP, "%s", base);
        off = strlen(cmd);
        cat_cmd(cmd, CMDFULL_CAP, &off,
                " -map [vt] -map %d:a -c:v libx264 -preset faster -b:v %dk -maxrate %dk "
                "-bufsize %dk -pass 2 -passlogfile %s -c:a aac -b:a %dk "
                "-t %.3f -movflags +faststart %s",
                nin, enc.vkbps, (int)(enc.vkbps * 1.1), enc.vkbps * 2, qplog,
                enc.audio_kbps, sp->total, qout);
        if (system(cmd) != 0) die("slides render pass 2 failed");
    } else {
        snprintf(cmd, CMDFULL_CAP, "%s", base);
        off = strlen(cmd);
        cat_cmd(cmd, CMDFULL_CAP, &off,
                " -map [vt] -map %d:a -c:v libx264 -crf 19 -preset faster "
                "-pix_fmt yuv420p -c:a aac -b:a 192k -t %.3f -movflags +faststart %s",
                nin, sp->total, qout);
        printf("=== rendering slides: %d photos, %.1f s ===\n", sp->n, sp->total);
        fflush(stdout);
        if (system(cmd) != 0) die("slides render failed");
    }

    for (int i = 0; i < nin; i++) free(ins[i]);
    free(ins);
    free(imap);
    free(base);
    free(cmd);
    double d = ffprobe_duration_file(out);
    if (d > 0) printf("%-40s %.1f s (%.1f min)\n", out, d, d / 60.0);
}

static int run_slides_only(const Cfg *cfg, int want)
{
    char mix[700];
    snprintf(mix, sizeof(mix), "%s_mix.wav", cfg->out_prefix);
    double total = ffprobe_duration_file(mix);
    if (total <= 0.0) die("--slide-only needs an existing %s", mix);
    PathList imgs = { 0 };
    load_images(&imgs, cfg->img_dir, cfg->slide_days);
    if (imgs.n == 0) return 0;
    printf("=== slides only ===\n");
    SlidePlan sp;
    plan_slides(&sp, &imgs, want, total);
    write_slide_plan(cfg, &sp);
    if (!cfg->dry_run) render_slides(cfg, &sp);
    printf("Done! Output: %s_slides.mp4 (photos %d, audio %s)\n", cfg->out_prefix, sp.n, mix);
    slide_plan_free(&sp);
    for (int i = 0; i < imgs.n; i++) free(imgs.v[i]);
    free(imgs.v);
    return 0;
}

static void expand_tilde(char *dst, size_t cap, const char *val)
{
    if (val[0] == '~' && (val[1] == '/' || val[1] == '\0')) {
        const char *home = getenv("HOME");
        snprintf(dst, cap, "%s%s", home ? home : "", val + 1);
    } else {
        snprintf(dst, cap, "%s", val);
    }
}

static double parse_len(const char *s)
{
    double total = 0;
    int seen = 0;
    while (*s) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        char *end;
        errno = 0;
        double v = strtod(s, &end);
        if (end == s || !isfinite(v) || v < 0) return -1;
        s = end;
        while (isspace((unsigned char)*s)) s++;
        double mult = 1.0;
        if (isalpha((unsigned char)*s)) {
            switch (tolower((unsigned char)*s)) {
            case 'h': mult = 3600.0; break;
            case 'm': mult = 60.0; break;
            case 's': mult = 1.0; break;
            default: return -1;
            }
            while (isalpha((unsigned char)*s)) s++;
        }
        total += v * mult;
        seen = 1;
    }
    return (seen && isfinite(total)) ? total : -1.0;
}

static void load_conf(Cfg *cfg)
{
    char path[1024];
    const char *ovr = getenv("MICHACKA_CONF");
    if (ovr && ovr[0])
        snprintf(path, sizeof(path), "%s", ovr);
    else {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0])
            snprintf(path, sizeof(path), "%s/michacka.conf", xdg);
        else {
            const char *home = getenv("HOME");
            snprintf(path, sizeof(path), "%s/.config/michacka.conf", home ? home : ".");
        }
    }
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[1200];
    int ln = 0;
    while (fgets(line, sizeof(line), fp)) {
        ln++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\0') continue;
        char *eq = strchr(s, '=');
        if (!eq) die("%s:%d: expected key=value", path, ln);
        *eq = '\0';
        char *key = s, *val = eq + 1;
        char *e = key + strlen(key);
        while (e > key && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        while (*val == ' ' || *val == '\t') val++;
        e = val + strlen(val);
        while (e > val && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
            *--e = '\0';
        char exp[1024];
        expand_tilde(exp, sizeof(exp), val);
        if (strcmp(key, "mus") == 0) cfg->mus_dir = xstrdup(exp);
        else if (strcmp(key, "fld") == 0) cfg->fld_dir = xstrdup(exp);
        else if (strcmp(key, "tj") == 0) snprintf(cfg->tj_path, sizeof(cfg->tj_path), "%s", exp);
        else if (strcmp(key, "img") == 0) cfg->img_dir = xstrdup(exp);
        else die("%s:%d: unknown key '%s' (expected mus|fld|img|tj)", path, ln, key);
    }
    fclose(fp);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [STYLE] [SEED] [options]\n"
            "\n"
            "Generative composition driver: plans layered movements, emits EDLs,\n"
            "and renders them through the tj compositing tool.\n"
            "\n"
            "  STYLE             day | storm | drift | pulse | rupture (default day)\n"
            "  SEED              RNG seed (default: random, printed for reproduction)\n"
            "  -p, --parts N     number of movements (style default)\n"
            "  -l, --len DUR     per-movement length: 600 | 90s | 15min | \"1h 10min\"\n"
            "                    (style default; max 86400)\n"
            "  -o, --out PREFIX  output prefix (default michacka_<style>_<min>min)\n"
            "  -s, --slide N     distinct photos in the rapid slideshow: 0 disables,\n"
            "                    default uses every photo in the window (cycled to\n"
            "                    fill the whole mix; each photo holds at most 7 frames)\n"
            "  -d, --slide-days N  only photos from the last N days (default 14, 0=all)\n"
            "  -m, --slide-mb MB  fill the slideshow mp4 to ~MB MB (2-pass VBR, auto\n"
            "                    res/bitrate; hard cuts, no transitions between photos)\n"
            "      --dense N      make the sound texture Nx denser (more layers per\n"
            "                    movement, default 3, 1..8)\n"
            "      --no-slide    no slideshow video\n"
            "      --slide-only  slides for an existing <prefix>_mix.wav, no audio render\n"
            "  -n, --dry-run     plan + write EDLs only, no audio\n"
            "  -h, --help        this help\n"
            "\n"
            "Config ~/.config/michacka.conf (key=value):\n"
            "  mus=DIR           music library (default ~/recordings)\n"
            "  fld=DIR           field-recording library (default /mnt/data/recordings/field)\n"
            "  img=DIR           photo library for slides (default ~/DCIM/Camera)\n"
            "  tj=PATH           tj renderer (default tj/ submodule; env MICHACKA_TJ wins)\n",
            prog);
}

int main(int argc, char *argv[])
{
    Cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.style = ST_DAY;
    cfg.slide = -1;
    cfg.slide_days = 14;
    cfg.dense = 3;
    snprintf(cfg.tj_path, sizeof(cfg.tj_path), "tj/tj");

    int pos = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--parts") == 0 || strcmp(a, "-p") == 0) {
            if (i + 1 >= argc) die("--parts/-p requires a value");
            cfg.parts = atoi(argv[++i]);
        } else if (strcmp(a, "--len") == 0 || strcmp(a, "-l") == 0) {
            if (i + 1 >= argc) die("--len/-l requires a value");
            cfg.len = parse_len(argv[++i]);
            if (cfg.len < 1 || cfg.len > 86400)
                die("invalid length '%s' (try e.g. 600 | 90s | 15min | 1h30m; max 86400)",
                    argv[i]);
        } else if (strcmp(a, "--out") == 0 || strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) die("--out/-o requires a prefix");
            snprintf(cfg.out_prefix, sizeof(cfg.out_prefix), "%s", argv[++i]);
        } else if (strcmp(a, "--slide") == 0 || strcmp(a, "-s") == 0) {
            if (i + 1 >= argc) die("--slide/-s requires a value");
            const char *sv = argv[i + 1];
            char *end;
            errno = 0;
            long v = strtol(sv, &end, 10);
            if (end == sv || *end != '\0' || v < 0 || v > 60)
                die("--slide must be an integer 0..60");
            cfg.slide = (int)v;
            i++;
        } else if (strcmp(a, "--no-slide") == 0) {
            cfg.slide = 0;
        } else if (strcmp(a, "--slide-mb") == 0 || strcmp(a, "-m") == 0) {
            if (i + 1 >= argc) die("--slide-mb/-m requires a value");
            const char *mv = argv[i + 1];
            char *end;
            errno = 0;
            double v = strtod(mv, &end);
            if (end == mv || *end != '\0' || v < 0.0 || v > 1000.0)
                die("--slide-mb must be 0..1000 (max MB for the slideshow mp4)");
            cfg.slide_max_mb = v;
            i++;
        } else if (strcmp(a, "--slide-days") == 0 || strcmp(a, "-d") == 0) {
            if (i + 1 >= argc) die("--slide-days/-d requires a value");
            const char *dv = argv[i + 1];
            char *end;
            errno = 0;
            long v = strtol(dv, &end, 10);
            if (end == dv || *end != '\0' || v < 0 || v > 3650)
                die("--slide-days must be an integer 0..3650 (0 = all photos)");
            cfg.slide_days = (int)v;
            i++;
        } else if (strcmp(a, "--dense") == 0) {
            if (i + 1 >= argc) die("--dense requires a value");
            const char *ev = argv[i + 1];
            char *end;
            errno = 0;
            long v = strtol(ev, &end, 10);
            if (end == ev || *end != '\0' || v < 1 || v > 8)
                die("--dense must be an integer 1..8 (layer density multiplier)");
            cfg.dense = (int)v;
            i++;
        } else if (strcmp(a, "--slide-only") == 0) {
            cfg.slide_only = 1;
        } else if (strcmp(a, "--dry-run") == 0 || strcmp(a, "-n") == 0) {
            cfg.dry_run = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (a[0] != '-') {
            if (pos == 0) {
                int s = style_by_name(a);
                if (s < 0) die("unknown style '%s' (day|storm|drift|pulse|rupture)", a);
                cfg.style = s;
            } else if (pos == 1) {
                char *end;
                errno = 0;
                cfg.seed = strtoull(a, &end, 10);
                if (end == a || *end != '\0')
                    die("invalid seed '%s' (positive integer expected)", a);
                cfg.have_seed = 1;
            } else {
                die("unexpected argument '%s' (usage: STYLE SEED)", a);
            }
            pos++;
        } else {
            die("unknown option '%s' (try --help)", a);
        }
    }

    load_conf(&cfg);

    const StyleSpec *st = &STYLES[cfg.style];
    double part_len = cfg.len > 0 ? cfg.len : st->def_len;
    if (cfg.parts <= 0) cfg.parts = st->def_parts;
    if (cfg.parts < 1 || cfg.parts > 96) die("--parts must be 1..96");

    char home_mus[1024];
    if (!cfg.mus_dir) {
        const char *home = getenv("HOME");
        snprintf(home_mus, sizeof(home_mus), "%s/recordings", home ? home : ".");
        cfg.mus_dir = home_mus;
    }
    if (!cfg.fld_dir) cfg.fld_dir = "/mnt/data/recordings/field";

    char home_img[1024];
    if (!cfg.img_dir) {
        const char *home = getenv("HOME");
        snprintf(home_img, sizeof(home_img), "%s/DCIM/Camera", home ? home : ".");
        cfg.img_dir = home_img;
    }

    if (!cfg.out_prefix[0]) {
        int mins = (int)lround(cfg.parts * part_len / 60.0);
        snprintf(cfg.out_prefix, sizeof(cfg.out_prefix), "michacka_%s_%dmin",
                 st->name, mins);
    }

    if (!cfg.have_seed) {
        cfg.seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        if (cfg.seed == 0) cfg.seed = 1;
    }
    rng_state = cfg.seed;
    for (int i = 0; i < 8; i++) rnd_next();

    if (cfg.slide_only)
        return run_slides_only(&cfg, cfg.slide >= 0 ? cfg.slide : -1);

    resolve_tj(&cfg);

    printf("=== michacka ===\n");
    printf("style: %s | parts: %d x %.0f s | dense: %dx | seed: %llu%s\n", st->name, cfg.parts,
           part_len, cfg.dense, (unsigned long long)cfg.seed, cfg.have_seed ? "" : " (auto)");
    printf("tj: %s\n", cfg.tj_path);
    printf("libraries:\n");

    TrackList mus = { 0 }, fld = { 0 };
    load_library(&mus, cfg.mus_dir, "music");
    load_library(&fld, cfg.fld_dir, "field");

    printf("analyzing (tj cache):\n");
    analyze_lib(cfg.tj_path, &mus, "music");
    analyze_lib(cfg.tj_path, &fld, "field");

    int roles[3] = { 0, 0, 0 };
    for (int i = 0; i < mus.n + fld.n; i++) {
        const Track *t = i < mus.n ? &mus.v[i] : &fld.v[i - mus.n];
        roles[t->role]++;
    }
    printf("texture roles: ambient=%d motion=%d pulse=%d\n", roles[0], roles[1], roles[2]);

    char **music_edls = xmalloc(sizeof(char *) * (size_t)cfg.parts);
    char **field_edls = xmalloc(sizeof(char *) * (size_t)cfg.parts);
    char **arcs = xmalloc(sizeof(char *) * (size_t)cfg.parts);

    int warned_bed = 0, warned_motion = 0, warned_pulse = 0;

    printf("planning movements:\n");
    for (int p = 0; p < cfg.parts; p++) {
        float phase = cfg.parts > 1 ? (float)p / (float)(cfg.parts - 1) : 0.0f;

        Edl m;
        edl_init(&m);
        int nb = layer_count(st, st->beds, st->nb, phase, p) * cfg.dense;
        int nm = layer_count(st, st->motion, st->nm, phase, p) * cfg.dense;
        int npp = layer_count(st, st->pulses, st->np, phase, p) * cfg.dense;
        int nf = layer_count(st, st->fields, st->nfl, phase, p) * cfg.dense;

        int budget = MAX_EDL_ENTRIES - 2;
        if (nb > budget) nb = budget;
        add_layered_entries(&m, &mus, ROLE_AMBIENT, nb, part_len,
                            part_len * 0.5, part_len * 0.75,
                            -14.0, -10.0, st->fade_in[0], st->fade_out[1],
                            &warned_bed);
        budget -= m.count;
        if (nm > budget) nm = budget;
        add_layered_entries(&m, &mus, ROLE_MOTION, nm, part_len,
                            120.0, 260.0, -8.0, -4.0, 6.0, 12.0, &warned_motion);
        budget -= m.count;
        if (npp > budget) npp = budget;
        add_layered_entries(&m, &mus, ROLE_PULSE, npp, part_len,
                            50.0, 150.0, -7.0, -4.0, 4.0, 8.0, &warned_pulse);

        Edl f;
        edl_init(&f);
        add_field_entries(&f, &fld, nf, part_len);

        char arc[512];
        build_arc(arc, sizeof(arc), st, part_len);

        music_edls[p] = xstrdup(m.buf);
        field_edls[p] = xstrdup(f.buf);
        arcs[p] = xstrdup(arc);

        printf("  part %02d: phase %.2f | music %d (bed%d/motion%d/pulse%d) | field %d | arc %s\n",
               p + 1, phase, m.count, nb, nm, npp, f.count, arc);

        edl_free(&m);
        edl_free(&f);
    }
    write_plan_files(&cfg, music_edls, field_edls);

    int do_slides = cfg.slide >= 0 ? cfg.slide : -1;

    if (cfg.dry_run) {
        if (do_slides != 0) {
            PathList imgs = { 0 };
            load_images(&imgs, cfg.img_dir, cfg.slide_days);
            if (imgs.n > 0) {
                SlidePlan sp;
                plan_slides(&sp, &imgs, do_slides, part_len * (double)cfg.parts);
                write_slide_plan(&cfg, &sp);
                printf("dry-run: slides plan written, no video rendered.\n");
                slide_plan_free(&sp);
            }
            for (int i = 0; i < imgs.n; i++) free(imgs.v[i]);
            free(imgs.v);
        }
        printf("dry-run: EDLs written, no audio rendered.\n");
        return 0;
    }

    int failed = render_all_parts(&cfg, st, music_edls, field_edls, arcs);
    if (failed) die("one or more movements failed to render");
    write_plan_files(&cfg, music_edls, field_edls);

    finish_mix(&cfg);

    int rendered_slides = 0;
    if (do_slides != 0) {
        PathList imgs = { 0 };
        load_images(&imgs, cfg.img_dir, cfg.slide_days);
        if (imgs.n > 0) {
            char mix[700];
            snprintf(mix, sizeof(mix), "%s_mix.wav", cfg.out_prefix);
            double total = ffprobe_duration_file(mix);
            if (total <= 0.0) total = part_len * (double)cfg.parts;
            SlidePlan sp;
            plan_slides(&sp, &imgs, do_slides, total);
            write_slide_plan(&cfg, &sp);
            render_slides(&cfg, &sp);
            rendered_slides = 1;
            slide_plan_free(&sp);
        }
        for (int i = 0; i < imgs.n; i++) free(imgs.v[i]);
        free(imgs.v);
    }

    cleanup_parts(&cfg);

    for (int p = 0; p < cfg.parts; p++) {
        free(music_edls[p]);
        free(field_edls[p]);
        free(arcs[p]);
    }
    free(music_edls);
    free(field_edls);
    free(arcs);
    free(mus.v);
    free(fld.v);

    printf("Done! Output: %s_mix.{wav,flac,mp3}", cfg.out_prefix);
    if (rendered_slides) printf(" + %s_slides.mp4", cfg.out_prefix);
    printf("\nReproduce with: ./michacka %s %llu\n",
           st->name, (unsigned long long)cfg.seed);
    return 0;
}
