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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_EDL_ENTRIES 30
#define PLAN_MARGIN 2.0
#define ANALYZE_BATCH 12
#define MAX_ENV_PTS 10
#define EDL_CAP 16384
#define CMD_CAP 32768

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
} StyleSpec;

typedef struct {
    char *buf;
    size_t len, cap;
    int count;
} Edl;

typedef struct {
    uint64_t seed;
    int parts;
    double part_len;
    int style;
    const char *mus_dir;
    const char *fld_dir;
    char tj_path[1024];
    char out_prefix[512];
    int keylock;
    int bpm;
    int no_master;
    int jobs;
    int dry_run;
    int force;
    int limit;
    int limit_given;
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

static void push_path(char ***v, int *n, int *cap, char *path)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *v = xrealloc(*v, (size_t)*cap * sizeof(char *));
    }
    (*v)[(*n)++] = path;
}

static void scan_dir_rec(const char *dir, char ***v, int *n, int *cap)
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
                    scan_dir_rec(path, v, n, cap);
                } else if (has_audio_ext(name) && !strchr(name, ',') && !strchr(name, '"')) {
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
    { "day", 12, 600.0f,
      { {0.00f, 2}, {0.18f, 1}, {0.82f, 1}, {1.00f, 2} }, 4,
      { {0.00f, 1}, {0.25f, 2}, {0.42f, 3}, {0.58f, 3}, {0.78f, 2}, {1.00f, 1} }, 6,
      { {0.00f, 0}, {0.15f, 1}, {0.32f, 3}, {0.55f, 3}, {0.72f, 2}, {0.86f, 0}, {1.00f, 0} }, 7,
      { {0.00f, 2}, {0.38f, 3}, {0.62f, 3}, {0.85f, 2}, {1.00f, 3} }, 5,
      { {0.0f, 0.85f}, {0.5f, 1.0f}, {1.0f, 0.85f} }, 3,
      { 14.0f, 28.0f }, { 16.0f, 28.0f }, 0 },
    { "storm", 6, 300.0f,
      { {0.0f, 1}, {1.0f, 1} }, 2,
      { {0.0f, 2}, {0.25f, 4}, {0.75f, 4}, {1.0f, 2} }, 4,
      { {0.0f, 1}, {0.15f, 4}, {0.85f, 4}, {1.0f, 2} }, 4,
      { {0.0f, 1}, {0.5f, 2}, {1.0f, 1} }, 3,
      { {0.0f, 0.6f}, {0.12f, 1.0f}, {0.82f, 1.0f}, {1.0f, 0.65f} }, 4,
      { 3.0f, 6.0f }, { 4.0f, 8.0f }, 0 },
    { "drift", 8, 600.0f,
      { {0.0f, 2}, {1.0f, 2} }, 2,
      { {0.0f, 1}, {0.5f, 2}, {1.0f, 1} }, 3,
      { {0.0f, 0}, {1.0f, 0} }, 2,
      { {0.0f, 2}, {0.5f, 3}, {1.0f, 2} }, 3,
      { {0.0f, 0.8f}, {0.5f, 0.95f}, {1.0f, 0.8f} }, 3,
      { 18.0f, 35.0f }, { 20.0f, 35.0f }, 0 },
    { "pulse", 10, 600.0f,
      { {0.0f, 1}, {1.0f, 1} }, 2,
      { {0.0f, 2}, {1.0f, 2} }, 2,
      { {0.0f, 3}, {0.15f, 5}, {0.9f, 4}, {1.0f, 3} }, 4,
      { {0.0f, 1}, {0.5f, 2}, {1.0f, 1} }, 3,
      { {0.0f, 0.7f}, {0.1f, 1.0f}, {0.92f, 1.0f}, {1.0f, 0.75f} }, 4,
      { 4.0f, 8.0f }, { 5.0f, 10.0f }, 0 },
    { "rupture", 8, 450.0f,
      { {0.0f, 1}, {1.0f, 1} }, 2,
      { {0.0f, 2}, {0.5f, 3}, {1.0f, 2} }, 3,
      { {0.0f, 1}, {0.5f, 3}, {1.0f, 1} }, 3,
      { {0.0f, 1}, {0.5f, 2}, {1.0f, 1} }, 3,
      { {0.0f, 0.5f}, {0.1f, 1.0f}, {0.75f, 1.0f}, {1.0f, 0.6f} }, 4,
      { 2.0f, 4.0f }, { 3.0f, 6.0f }, 1 },
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

static void resolve_tj(Cfg *cfg)
{
    char cand[1024];
    const char *env = getenv("MICHACKA_TJ");
    if (access(cfg->tj_path, X_OK) == 0) return;
    if (env && access(env, X_OK) == 0) {
        snprintf(cfg->tj_path, sizeof(cfg->tj_path), "%s", env);
        return;
    }
    static const char *defaults[] = { "./tj", "../tj/tj", "../../tj/tj" };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        if (access(defaults[i], X_OK) == 0) {
            snprintf(cfg->tj_path, sizeof(cfg->tj_path), "%s", defaults[i]);
            return;
        }
    }
    if (access("../tj", X_OK) == 0) {
        fprintf(stderr, "michacka: building tj in ../tj ...\n");
        int rc = system("make -s -C ../tj");
        if (rc == 0 && access("../tj/tj", X_OK) == 0) {
            snprintf(cfg->tj_path, sizeof(cfg->tj_path), "../tj/tj");
            return;
        }
    }
    snprintf(cand, sizeof(cand), "%s", cfg->tj_path);
    die("tj renderer not found (tried %s, MICHACKA_TJ, ./tj, ../tj/tj)", cand);
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

    if (!cfg->force && access(master_wav, F_OK) == 0) {
        printf("[part %02d] exists, skipping (--force to rebuild)\n", idx + 1);
        return 0;
    }

    char qedl[EDL_CAP + 16];
    sh_quote(qedl, sizeof(qedl), music_edl);
    snprintf(cmd, sizeof(cmd), "%s %s %s%s%s --arc \"%s\" --fade-in 0.5 --fade-out 2",
             cfg->tj_path, qedl, music_wav,
             cfg->bpm ? " --bpm auto --snap" : "",
             cfg->keylock ? " --keylock auto" : "", arc);
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
    snprintf(cmd, sizeof(cmd), "%s %s %s --fade-in 0.5 --fade-out 2%s",
             cfg->tj_path, qedl, part_wav, cfg->no_master ? "" : " --master subtle");
    printf("[part %02d] pass B: + field recordings\n", idx + 1);
    fflush(stdout);
    if (system(cmd) != 0) {
        fprintf(stderr, "michacka: part %02d pass B failed\n", idx + 1);
        return 1;
    }
    return 0;
}

typedef struct {
    int lo, hi;
    int failed;
} JobRange;

static int run_range(const Cfg *cfg, const StyleSpec *st, char **music_edls, char **field_edls,
                     char **arcs, const JobRange *jr)
{
    for (int i = jr->lo; i <= jr->hi; i++)
        if (render_part(cfg, st, music_edls[i], field_edls[i], arcs[i], i)) return 1;
    return 0;
}

static int render_all_parts(const Cfg *cfg, const StyleSpec *st, char **music_edls,
                            char **field_edls, char **arcs)
{
    if (cfg->dry_run || cfg->jobs <= 1) {
        JobRange jr = { 0, cfg->parts - 1, 0 };
        return run_range(cfg, st, music_edls, field_edls, arcs, &jr);
    }
    int nj = cfg->jobs < cfg->parts ? cfg->jobs : cfg->parts;
    pid_t *pids = xmalloc(sizeof(pid_t) * (size_t)nj);
    JobRange *ranges = xmalloc(sizeof(JobRange) * (size_t)nj);
    int per = (cfg->parts + nj - 1) / nj;
    for (int k = 0; k < nj; k++) {
        ranges[k].lo = k * per;
        ranges[k].hi = ranges[k].lo + per - 1;
        if (ranges[k].hi >= cfg->parts) ranges[k].hi = cfg->parts - 1;
        ranges[k].failed = 0;
        pid_t pid = fork();
        if (pid < 0) die("fork failed: %s", strerror(errno));
        if (pid == 0) {
            _exit(run_range(cfg, st, music_edls, field_edls, arcs, &ranges[k]) ? 1 : 0);
        }
        pids[k] = pid;
    }
    int failed = 0;
    for (int k = 0; k < nj; k++) {
        int status = 0;
        waitpid(pids[k], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed = 1;
            fprintf(stderr, "michacka: worker %d (parts %02d-%02d) failed\n", k,
                    ranges[k].lo + 1, ranges[k].hi + 1);
        }
    }
    free(pids);
    free(ranges);
    return failed;
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

static void load_library(TrackList *lib, const char *dir, const char *label, int limit, int limit_given)
{
    char **paths = NULL;
    int n = 0, cap = 0;
    scan_dir_rec(dir, &paths, &n, &cap);
    if (n == 0) die("no audio files found in %s (%s)", dir, label);
    
    if (!limit_given && n > 1000) {
        limit = 1000;
        printf("  %s: found %d files, auto-limiting to %d (use --limit to override)\n", label, n, limit);
    }
    
    int keep = n;
    if (limit > 0 && limit < n) {
        int *idx = xmalloc(sizeof(int) * (size_t)n);
        for (int i = 0; i < n; i++) idx[i] = i;
        shuffle_ints(idx, n);
        for (int i = 0; i < limit; i++) {
            char *tmp = paths[i];
            paths[i] = paths[idx[i]];
            paths[idx[i]] = tmp;
        }
        free(idx);
        keep = limit;
        for (int i = keep; i < n; i++) free(paths[i]);
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
    printf("  %s: %d files\n", label, lib->n);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] [MUS_DIR] [FLD_DIR]\n"
            "\n"
            "Generative composition driver: plans layered movements, emits EDLs,\n"
            "and renders them through the tj compositing tool.\n"
            "\n"
            "  --seed N          RNG seed (default: random, printed for reproduction)\n"
            "  --parts N         number of movements (style default)\n"
            "  --part-len SEC    length of each movement in seconds (style default)\n"
            "  --style NAME      day | storm | drift | pulse | rupture (default day)\n"
            "  MUS_DIR, --mus    music library (default ~/recordings)\n"
            "  FLD_DIR, --fld    field-recording library (default /mnt/data/recordings/field)\n"
            "  --tj PATH         path to tj renderer (default ../tj/tj, env MICHACKA_TJ)\n"
            "  --out PREFIX      output prefix (default michacka_<style>_<min>min)\n"
            "  --limit N         sample only N files per library (defaults to 1000 for big libs)\n"
            "  --bpm             beat-match the music pass (tj --bpm auto --snap)\n"
            "  --keylock         transpose the music pass to a shared key (tj --keylock auto)\n"
            "  --no-master       skip the mastering pass on movement renders\n"
            "  --jobs N          render movements in parallel (default 1)\n"
            "  --dry-run         plan + write EDLs only, no audio\n"
            "  --force           re-render existing movements\n"
            "  -h, --help        this help\n",
            prog);
}

int main(int argc, char *argv[])
{
    Cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.style = ST_DAY;
    cfg.jobs = 1;
    snprintf(cfg.tj_path, sizeof(cfg.tj_path), "../tj/tj");

    static const char *style_names[] = { "day", "storm", "drift", "pulse", "rupture" };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--seed") == 0) {
            if (i + 1 >= argc) die("--seed requires a value");
            cfg.seed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(a, "--parts") == 0) {
            if (i + 1 >= argc) die("--parts requires a value");
            cfg.parts = atoi(argv[++i]);
        } else if (strcmp(a, "--part-len") == 0) {
            if (i + 1 >= argc) die("--part-len requires a value");
            cfg.part_len = atof(argv[++i]);
        } else if (strcmp(a, "--style") == 0) {
            if (i + 1 >= argc) die("--style requires a name");
            int s = style_by_name(argv[++i]);
            if (s < 0) die("unknown style '%s' (day|storm|drift|pulse|rupture)", argv[i]);
            cfg.style = s;
        } else if (strcmp(a, "--mus") == 0) {
            if (i + 1 >= argc) die("--mus requires a path");
            cfg.mus_dir = xstrdup(argv[++i]);
        } else if (strcmp(a, "--fld") == 0) {
            if (i + 1 >= argc) die("--fld requires a path");
            cfg.fld_dir = xstrdup(argv[++i]);
        } else if (strcmp(a, "--tj") == 0) {
            if (i + 1 >= argc) die("--tj requires a path");
            snprintf(cfg.tj_path, sizeof(cfg.tj_path), "%s", argv[++i]);
        } else if (strcmp(a, "--out") == 0) {
            if (i + 1 >= argc) die("--out requires a prefix");
            snprintf(cfg.out_prefix, sizeof(cfg.out_prefix), "%s", argv[++i]);
        } else if (strcmp(a, "--limit") == 0) {
            if (i + 1 >= argc) die("--limit requires a value");
            cfg.limit = atoi(argv[++i]);
            cfg.limit_given = 1;
        } else if (strcmp(a, "--jobs") == 0) {
            if (i + 1 >= argc) die("--jobs requires a value");
            cfg.jobs = atoi(argv[++i]);
        } else if (strcmp(a, "--bpm") == 0) cfg.bpm = 1;
        else if (strcmp(a, "--keylock") == 0) cfg.keylock = 1;
        else if (strcmp(a, "--no-master") == 0) cfg.no_master = 1;
        else if (strcmp(a, "--dry-run") == 0) cfg.dry_run = 1;
        else if (strcmp(a, "--force") == 0) cfg.force = 1;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        else if (a[0] != '-') {
            if (!cfg.mus_dir) cfg.mus_dir = xstrdup(a);
            else if (!cfg.fld_dir) cfg.fld_dir = xstrdup(a);
            else die("unexpected positional argument '%s'", a);
        }
        else die("unknown option '%s' (try --help)", a);
    }

    const StyleSpec *st = &STYLES[cfg.style];
    if (cfg.parts <= 0) cfg.parts = st->def_parts;
    if (cfg.part_len <= 0.0) cfg.part_len = st->def_len;
    if (cfg.part_len < 20.0) die("--part-len must be at least 20 s");
    if (cfg.parts < 1 || cfg.parts > 96) die("--parts must be 1..96");
    if (cfg.jobs < 1 || cfg.jobs > 16) die("--jobs must be 1..16");
    if (cfg.limit < 0) cfg.limit = 0;

    char home_mus[1024];
    if (!cfg.mus_dir) {
        const char *home = getenv("HOME");
        snprintf(home_mus, sizeof(home_mus), "%s/recordings", home ? home : ".");
        cfg.mus_dir = home_mus;
    }
    if (!cfg.fld_dir) cfg.fld_dir = "/mnt/data/recordings/field";

    if (!cfg.out_prefix[0]) {
        int mins = (int)lround(cfg.parts * cfg.part_len / 60.0);
        snprintf(cfg.out_prefix, sizeof(cfg.out_prefix), "michacka_%s_%dmin",
                 style_names[cfg.style], mins);
    }

    int seed_given = cfg.seed != 0;
    if (!seed_given) {
        cfg.seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        if (cfg.seed == 0) cfg.seed = 1;
    }
    rng_state = cfg.seed;
    for (int i = 0; i < 8; i++) rnd_next();

    resolve_tj(&cfg);

    printf("=== michacka ===\n");
    printf("style: %s | parts: %d x %.0f s | seed: %llu%s\n", st->name, cfg.parts,
           cfg.part_len, (unsigned long long)cfg.seed, seed_given ? "" : " (auto)");
    printf("tj: %s\n", cfg.tj_path);
    printf("libraries:\n");

    TrackList mus = { 0 }, fld = { 0 };
    load_library(&mus, cfg.mus_dir, "music", cfg.limit, cfg.limit_given);
    load_library(&fld, cfg.fld_dir, "field", cfg.limit, cfg.limit_given);

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
        int nb = layer_count(st, st->beds, st->nb, phase, p);
        int nm = layer_count(st, st->motion, st->nm, phase, p);
        int npp = layer_count(st, st->pulses, st->np, phase, p);
        int nf = layer_count(st, st->fields, st->nfl, phase, p);

        int budget = MAX_EDL_ENTRIES - 2;
        if (nb > budget) nb = budget;
        add_layered_entries(&m, &mus, ROLE_AMBIENT, nb, cfg.part_len,
                            cfg.part_len * 0.5, cfg.part_len * 0.75,
                            -14.0, -10.0, st->fade_in[0], st->fade_out[1],
                            &warned_bed);
        budget -= m.count;
        if (nm > budget) nm = budget;
        add_layered_entries(&m, &mus, ROLE_MOTION, nm, cfg.part_len,
                            120.0, 260.0, -8.0, -4.0, 6.0, 12.0, &warned_motion);
        budget -= m.count;
        if (npp > budget) npp = budget;
        add_layered_entries(&m, &mus, ROLE_PULSE, npp, cfg.part_len,
                            50.0, 150.0, -7.0, -4.0, 4.0, 8.0, &warned_pulse);

        Edl f;
        edl_init(&f);
        add_field_entries(&f, &fld, nf, cfg.part_len);

        char arc[512];
        build_arc(arc, sizeof(arc), st, cfg.part_len);

        music_edls[p] = xstrdup(m.buf);
        field_edls[p] = xstrdup(f.buf);
        arcs[p] = xstrdup(arc);

        printf("  part %02d: phase %.2f | music %d (bed%d/motion%d/pulse%d) | field %d | arc %s\n",
               p + 1, phase, m.count, nb, nm, npp, f.count, arc);

        edl_free(&m);
        edl_free(&f);
    }
    write_plan_files(&cfg, music_edls, field_edls);

    if (cfg.dry_run) {
        printf("dry-run: EDLs written, no audio rendered.\n");
        return 0;
    }

    int failed = render_all_parts(&cfg, st, music_edls, field_edls, arcs);
    if (failed) die("one or more movements failed to render");
    write_plan_files(&cfg, music_edls, field_edls);

    finish_mix(&cfg);

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

    printf("Done! Output: %s_mix.{wav,flac,mp3}\nReproduce with: --seed %llu\n",
           cfg.out_prefix, (unsigned long long)cfg.seed);
    return 0;
}
