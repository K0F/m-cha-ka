#define main michacka_main
#include "michacka.c"
#undef main

#include <math.h>

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks_run++;                                                   \
        if (!(cond)) {                                                  \
            checks_failed++;                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_CLOSE(a, b)                                               \
    do {                                                                \
        double _a = (double)(a), _b = (double)(b);                      \
        CHECK(fabs(_a - _b) < 1e-6);                                    \
    } while (0)

static void test_clampd(void)
{
    CHECK(clampd(5.0, 0.0, 10.0) == 5.0);
    CHECK(clampd(-1.0, 0.0, 10.0) == 0.0);
    CHECK(clampd(11.0, 0.0, 10.0) == 10.0);
    CHECK(clampd(0.0, 0.0, 10.0) == 0.0);
    CHECK(clampd(10.0, 0.0, 10.0) == 10.0);
}

static void test_rng_deterministic(void)
{
    uint64_t a[5], b[5];
    rng_state = 42;
    for (int i = 0; i < 5; i++) a[i] = rnd_next();
    rng_state = 42;
    for (int i = 0; i < 5; i++) b[i] = rnd_next();
    for (int i = 0; i < 5; i++) CHECK(a[i] == b[i]);
    rng_state = 43;
    CHECK(rnd_next() != a[0]);
}

static void test_rng_ranges(void)
{
    rng_state = 7;
    for (int i = 0; i < 2000; i++) {
        double u = rnd_unit();
        CHECK(u >= 0.0 && u < 1.0);
        double r = rnd_range(-2.0, 2.0);
        CHECK(r >= -2.0 && r <= 2.0);
        double z = rnd_range(3.0, 3.0);
        CHECK(z == 3.0);
    }
}

static void test_shuffle_is_permutation(void)
{
    enum { N = 64 };
    int a[N];
    int seen[N];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < N; i++) a[i] = i;
    rng_state = 1234;
    shuffle_ints(a, N);
    for (int i = 0; i < N; i++) {
        CHECK(a[i] >= 0 && a[i] < N);
        seen[a[i]]++;
    }
    for (int i = 0; i < N; i++) CHECK(seen[i] == 1);
}

static void test_has_audio_ext(void)
{
    CHECK(has_audio_ext("song.wav"));
    CHECK(has_audio_ext("song.WAV"));
    CHECK(has_audio_ext("a/b/c.flac"));
    CHECK(has_audio_ext("x.mp3"));
    CHECK(has_audio_ext("x.opus"));
    CHECK(has_audio_ext("x.ogg"));
    CHECK(has_audio_ext("x.m4a"));
    CHECK(!has_audio_ext("x.txt"));
    CHECK(!has_audio_ext("songwav"));
    CHECK(!has_audio_ext(".wav"));
    CHECK(!has_audio_ext("wav"));
}

static void test_has_img_ext(void)
{
    CHECK(has_img_ext("x.jpg"));
    CHECK(has_img_ext("x.JPG"));
    CHECK(has_img_ext("x.jpeg"));
    CHECK(has_img_ext("x.png"));
    CHECK(has_img_ext("x.webp"));
    CHECK(!has_img_ext("x.gif"));
    CHECK(!has_img_ext("x.bmp"));
    CHECK(!has_img_ext("x.wav"));
    CHECK(!has_img_ext("xjpg"));
    CHECK(!has_img_ext("jpg"));
}

static void test_sh_quote(void)
{
    char q[256];
    sh_quote(q, sizeof(q), "hello");
    CHECK(strcmp(q, "\"hello\"") == 0);
    sh_quote(q, sizeof(q), "");
    CHECK(strcmp(q, "\"\"") == 0);
    sh_quote(q, sizeof(q), "a\"b\\c$d`e");
    CHECK(strcmp(q, "\"a\\\"b\\\\c\\$d\\`e\"") == 0);
    sh_quote(q, sizeof(q), "no $HOME expansion");
    CHECK(strstr(q, "\\$") != NULL);
    char tiny[3];
    sh_quote(tiny, sizeof(tiny), "xyz");
    CHECK(strlen(tiny) < sizeof(tiny));
    char too_small[2];
    sh_quote(too_small, sizeof(too_small), "xyz");
    CHECK(too_small[0] == 0);
}

static void test_path_tail(void)
{
    CHECK(strcmp(path_tail("/a/b/c.wav"), "c.wav") == 0);
    CHECK(strcmp(path_tail("c.wav"), "c.wav") == 0);
    CHECK(strcmp(path_tail("/"), "") == 0);
}

static void test_role_for(void)
{
    CHECK(role_for("ambient", 0.9f, 0.9f, 0.9f) == ROLE_AMBIENT);
    CHECK(role_for("pulse", 0.0f, 0.0f, 0.0f) == ROLE_PULSE);
    CHECK(role_for("motion", 0.0f, 0.0f, 0.0f) == ROLE_MOTION);
    CHECK(role_for("unknown", 1.0f, 0.5f, 0.0f) == ROLE_PULSE);
    CHECK(role_for("unknown", 0.9f, 0.45f, 0.0f) == ROLE_PULSE);
    CHECK(role_for("unknown", 0.9f, 0.44f, 0.0f) != ROLE_PULSE);
    CHECK(role_for("unknown", 0.59f, 0.0f, 0.49f) == ROLE_AMBIENT);
    CHECK(role_for("unknown", 0.59f, 0.0f, 0.51f) == ROLE_MOTION);
    CHECK(role_for("unknown", 0.6f, 0.0f, 0.0f) == ROLE_MOTION);
}

static void test_env_eval(void)
{
    EnvPt e[] = { { 0.0f, 10.0f }, { 10.0f, 20.0f }, { 20.0f, 0.0f } };
    CHECK(env_eval(e, 0, 5.0f) == 0.0f);
    CHECK(env_eval(e, 3, -5.0f) == 10.0f);
    CHECK(env_eval(e, 3, 25.0f) == 0.0f);
    CHECK_CLOSE(env_eval(e, 3, 0.0f), 10.0);
    CHECK_CLOSE(env_eval(e, 3, 10.0f), 20.0);
    CHECK_CLOSE(env_eval(e, 3, 5.0f), 15.0);
    CHECK_CLOSE(env_eval(e, 3, 15.0f), 10.0);
    EnvPt flat[] = { { 1.0f, 3.0f } };
    CHECK(env_eval(flat, 1, 0.0f) == 3.0f);
    CHECK(env_eval(flat, 1, 9.0f) == 3.0f);
    EnvPt dup[] = { { 2.0f, 1.0f }, { 2.0f, 2.0f } };
    CHECK_CLOSE(env_eval(dup, 2, 2.0f), 1.0);
}

static void test_style_by_name(void)
{
    CHECK(style_by_name("day") == ST_DAY);
    CHECK(style_by_name("storm") == ST_STORM);
    CHECK(style_by_name("drift") == ST_DRIFT);
    CHECK(style_by_name("pulse") == ST_PULSE);
    CHECK(style_by_name("rupture") == ST_RUPTURE);
    CHECK(style_by_name("nope") == -1);
    CHECK(style_by_name("") == -1);
    CHECK(style_by_name("Day") == -1);
}

static void test_layer_count_parity(void)
{
    const StyleSpec *rup = &STYLES[ST_RUPTURE];
    CHECK(layer_count(rup, rup->fields, rup->nfl, 0.5f, 0) == 3);
    CHECK(layer_count(rup, rup->fields, rup->nfl, 0.5f, 1) == 1);
    CHECK(layer_count(rup, rup->fields, rup->nfl, 0.5f, 2) == 3);
    const StyleSpec *day = &STYLES[ST_DAY];
    CHECK(layer_count(day, day->beds, day->nb, 0.0f, 0) == 2);
    CHECK(layer_count(day, day->beds, day->nb, 0.5f, 0) == 1);
    CHECK(layer_count(day, day->beds, day->nb, 1.0f, 0) == 2);
    const StyleSpec *drift = &STYLES[ST_DRIFT];
    CHECK(layer_count(drift, drift->pulses, drift->np, 0.37f, 5) == 0);
}

static void test_find_and_collect(void)
{
    Track tr[3];
    memset(tr, 0, sizeof(tr));
    snprintf(tr[0].path, sizeof(tr[0].path), "/lib/alpha.wav");
    snprintf(tr[1].path, sizeof(tr[1].path), "/other/beta.wav");
    snprintf(tr[2].path, sizeof(tr[2].path), "/lib/gamma.wav");
    tr[0].role = ROLE_AMBIENT;
    tr[1].role = ROLE_MOTION;
    tr[2].role = ROLE_AMBIENT;

    TrackList lib = { tr, 3, 3 };
    CHECK(find_track(&lib, "/lib/alpha.wav") == 0);
    CHECK(find_track(&lib, "/other/beta.wav") == 1);
    CHECK(find_track(&lib, "beta.wav") == 1);
    CHECK(find_track(&lib, "gamma.wav") == 2);
    CHECK(find_track(&lib, "missing.wav") == -1);

    int out[3];
    CHECK(collect_roles(&lib, ROLE_AMBIENT, out) == 2);
    CHECK(out[0] == 0 && out[1] == 2);
    CHECK(collect_roles(&lib, ROLE_PULSE, out) == 0);
    CHECK(collect_roles(&lib, ROLE_MOTION, out) == 1);
}

static void test_pick_slice(void)
{
    Track t;
    memset(&t, 0, sizeof(t));

    rng_state = 99;
    snprintf(t.path, sizeof(t.path), "/x/a.wav");
    t.dur = 100.0;
    for (int i = 0; i < 100; i++) {
        double in_sec, sp;
        CHECK(pick_slice(&t, 30.0, &in_sec, &sp) == 1);
        CHECK(sp == 30.0);
        CHECK(in_sec >= 0.0 && in_sec <= 70.0);
    }

    rng_state = 98;
    for (int i = 0; i < 50; i++) {
        double in_sec, sp;
        CHECK(pick_slice(&t, 500.0, &in_sec, &sp) == 1);
        CHECK(sp <= 90.0);
        CHECK(in_sec >= 0.0 && in_sec + sp <= 100.0001);
    }

    t.dur = 2.0;
    double in_sec, sp;
    CHECK(pick_slice(&t, 30.0, &in_sec, &sp) == 0);

    t.dur = 4.0;
    CHECK(pick_slice(&t, 30.0, &in_sec, &sp) == 1);
    CHECK(sp <= 3.6);
}

static void test_edl_put(void)
{
    Edl e;
    edl_init(&e);
    CHECK(e.count == 0);
    edl_put(&e, "in%.1f out%.1f at%.1f v%.0f fin%.1f fout%.1f %s",
            0.0, 18.0, 0.0, -12.0, 14.0, 28.0, "/lib/alpha.wav");
    CHECK(e.count == 1);
    CHECK(strcmp(e.buf, "in0.0 out18.0 at0.0 v-12 fin14.0 fout28.0 /lib/alpha.wav") == 0);
    edl_put(&e, "%s", "second");
    CHECK(e.count == 2);
    CHECK(strchr(e.buf, ',') != NULL);
    CHECK(strcmp(strrchr(e.buf, ',') + 1, "second") == 0);
    edl_free(&e);

    Edl big;
    edl_init(&big);
    char path[256];
    memset(path, 'x', sizeof(path) - 6);
    memcpy(path + sizeof(path) - 6, ".wav\0", 6);
    for (int i = 0; i < 200; i++)
        edl_put(&big, "in0.0 out120.0 at%d.0 v-8 fin5.0 fout5.0 %d%s", i, i, path);
    CHECK(big.count == 200);
    CHECK(big.cap > EDL_CAP);
    CHECK(big.len == strlen(big.buf));
    CHECK(strstr(big.buf, "at199.0") != NULL);
    CHECK(strstr(big.buf, "in0.0") == big.buf);
    edl_free(&big);
}

static void test_build_arc(void)
{
    char arc[512];
    const StyleSpec *st = &STYLES[ST_DAY];
    build_arc(arc, sizeof(arc), st, 600.0);

    CHECK(strncmp(arc, "0:", 2) == 0);
    int commas = 0;
    for (const char *p = arc; *p; p++)
        if (*p == ',') commas++;
    CHECK(commas == 4);

    const char *last = strrchr(arc, ',');
    CHECK(last != NULL);
    CHECK(strncmp(last + 1, "600:", 4) == 0);
    double end_gain = atof(strchr(last + 1, ':') + 1);
    CHECK(end_gain >= 0.05 && end_gain <= 1.0);

    CHECK(strstr(arc, ",300:") != NULL);
}

static int run_cli(const char *args)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./michacka %s >/dev/null 2>&1", args);
    int rc = system(cmd);
    if (!WIFEXITED(rc)) return -1;
    return WEXITSTATUS(rc);
}

static int run_system_ok(const char *cmd)
{
    int rc = system(cmd);
    return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static void test_slide_planning(void)
{
    PathList imgs;
    memset(&imgs, 0, sizeof(imgs));
    enum { NIMG = 20 };
    for (int i = 0; i < NIMG; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/camera/IMG_%02d.jpg", i);
        push_path(&imgs.v, &imgs.n, &imgs.cap, xstrdup(path));
    }

    rng_state = 4242;
    double total = 86.594896;
    double dur = (double)SLIDE_MAX_FRAMES / (double)SLIDE_FPS;
    int need = (int)ceil(total / dur);
    SlidePlan sp;
    plan_slides(&sp, &imgs, 12, total);

    CHECK(sp.n == need);
    CHECK_CLOSE(sp.crossfade, 0.0);
    for (int i = 0; i < sp.n; i++) {
        CHECK_CLOSE(sp.v[i].dur, dur);
        CHECK(strlen(sp.v[i].path) > 0);
    }
    for (int i = 0; i < sp.n; i++)
        CHECK(strcmp(sp.v[i].path, imgs.v[i % 12]) == 0);
    CHECK_CLOSE(sp.total, total);
    slide_plan_free(&sp);

    for (int i = 0; i < imgs.n; i++) free(imgs.v[i]);
    free(imgs.v);
}

static void test_plan_slides_deterministic(void)
{
    PathList imgs;
    memset(&imgs, 0, sizeof(imgs));
    for (int i = 0; i < 8; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/camera/P%02d.png", i);
        push_path(&imgs.v, &imgs.n, &imgs.cap, xstrdup(path));
    }

    SlidePlan a, b, c;
    rng_state = 777;
    plan_slides(&a, &imgs, 8, 60.0);
    rng_state = 777;
    plan_slides(&b, &imgs, 8, 60.0);
    rng_state = 778;
    plan_slides(&c, &imgs, 8, 60.0);
    double dur = (double)SLIDE_MAX_FRAMES / (double)SLIDE_FPS;
    int need = (int)ceil(60.0 / dur);
    int same = 1;
    for (int i = 0; i < need; i++) {
        CHECK_CLOSE(a.v[i].dur, dur);
        CHECK(strcmp(a.v[i].path, b.v[i].path) == 0);
        CHECK(strcmp(a.v[i].path, c.v[i].path) == 0);
        CHECK(strcmp(a.v[i].path, imgs.v[i % 8]) == 0);
        same &= (strcmp(a.v[i].path, b.v[i].path) == 0);
    }
    CHECK(same == 1);
    slide_plan_free(&a);
    slide_plan_free(&b);
    slide_plan_free(&c);

    for (int i = 0; i < imgs.n; i++) free(imgs.v[i]);
    free(imgs.v);
}

static void test_plan_slides_sample_day(void)
{
    PathList imgs;
    memset(&imgs, 0, sizeof(imgs));
    enum { NIMG = 400 };
    for (int i = 0; i < NIMG; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/camera/IMG_%03d.jpg", i);
        push_path(&imgs.v, &imgs.n, &imgs.cap, xstrdup(path));
    }
    SlidePlan sp;
    plan_slides(&sp, &imgs, -1, 60.0);
    double dur = (double)SLIDE_MAX_FRAMES / (double)SLIDE_FPS;
    int need = (int)ceil(60.0 / dur);
    CHECK(sp.n == need);
    int last = -1;
    for (int i = 0; i < need; i++) {
        int idx = (int)((double)i * (double)NIMG / (double)need);
        char want[64];
        snprintf(want, sizeof(want), "/camera/IMG_%03d.jpg", idx);
        CHECK(strcmp(sp.v[i].path, want) == 0);
        CHECK(idx >= last);
        last = idx;
    }
    slide_plan_free(&sp);
    for (int i = 0; i < imgs.n; i++) free(imgs.v[i]);
    free(imgs.v);
}

static void test_slide_plan_underfill(void)
{
    PathList imgs;
    memset(&imgs, 0, sizeof(imgs));
    push_path(&imgs.v, &imgs.n, &imgs.cap, xstrdup("/camera/only.jpg"));
    SlidePlan sp;
    double dur = (double)SLIDE_MAX_FRAMES / (double)SLIDE_FPS;
    plan_slides(&sp, &imgs, 12, 60.0);
    CHECK(sp.n == (int)ceil(60.0 / dur));
    CHECK_CLOSE(sp.v[0].dur, dur);
    CHECK(strcmp(sp.v[0].path, "/camera/only.jpg") == 0);
    for (int i = 1; i < sp.n; i++)
        CHECK(strcmp(sp.v[i].path, "/camera/only.jpg") == 0);
    slide_plan_free(&sp);

    PathList none = { 0 };
    plan_slides(&sp, &none, 5, 60.0);
    CHECK(sp.n == 0 && sp.v == NULL);

    for (int i = 0; i < imgs.n; i++) free(imgs.v[i]);
    free(imgs.v);
}

static void test_style_defaults(void)
{
    for (size_t i = 0; i < sizeof(STYLES) / sizeof(STYLES[0]); i++) {
        CHECK((int)(STYLES[i].def_parts * STYLES[i].def_len) == 600);
        CHECK(STYLES[i].nslide >= 8 && STYLES[i].nslide <= 12);
    }
    CHECK(STYLES[ST_PULSE].bpm == 1 && STYLES[ST_PULSE].keylock == 0);
    CHECK(STYLES[ST_RUPTURE].bpm == 1 && STYLES[ST_RUPTURE].keylock == 1);
    CHECK(STYLES[ST_DAY].bpm == 0 && STYLES[ST_DAY].keylock == 0);
    CHECK(STYLES[ST_STORM].bpm == 0 && STYLES[ST_DRIFT].bpm == 0);
}

static void test_slide_encode_config(void)
{
    SlideEncode e;
    e = slide_encode_config(0.0, 88.0);
    CHECK(e.vkbps == 0 && e.w == SLIDE_W && e.h == SLIDE_H);

    e = slide_encode_config(10.0, 88.0);
    CHECK(e.w == 1280 && e.h == 720);
    CHECK(e.audio_kbps == 128);
    double a_bytes = (double)e.audio_kbps / 8.0 * 1000.0 * 88.0;
    double avail = 10.0 * 1000000.0 * 0.96 - a_bytes;
    CHECK(e.vkbps >= 600 && e.vkbps <= (int)(avail * 8.0 / 88.0 / 1000.0) + 1);
    CHECK(e.pw >= e.w && e.ph >= e.h);

    e = slide_encode_config(60.0, 88.0);
    CHECK(e.w == SLIDE_W && e.h == SLIDE_H);

    e = slide_encode_config(1.0, 120.0);
    CHECK(e.vkbps >= 600);
    CHECK(e.w == 1280 && e.h == 720);

    e = slide_encode_config(1000.0, 10.0);
    CHECK(e.vkbps == 24000);
    CHECK(e.w == 2560 && e.h == 1440);

    e = slide_encode_config(10.0, 0.0);
    CHECK(e.w == 1280 && e.h == 720 && e.vkbps > 0);
}

static void test_image_taken(void)
{
    system("rm -rf test_img_days && mkdir -p test_img_days");
    FILE *fp;
    fp = fopen("test_img_days/IMG_20240110_120000.jpg", "w");
    fclose(fp);
    fp = fopen("test_img_days/noise.txt", "w");
    fclose(fp);

    time_t t = image_taken("test_img_days/IMG_20240110_120000.jpg");
    CHECK(t != (time_t)-1);
    struct tm *tm = localtime(&t);
    CHECK(tm->tm_year + 1900 == 2024);
    CHECK(tm->tm_mon + 1 == 1 && tm->tm_mday == 10);

    t = image_taken("test_img_days/noise.txt");
    CHECK(t != (time_t)-1);

    system("rm -rf test_img_days");
}

static void test_load_images_window(void)
{
    system("rm -rf test_img_days && mkdir -p test_img_days");
    char p1[256], p2[256];
    time_t now = time(NULL);
    struct tm t;
    time_t d3 = now - 3 * 86400, d40 = now - 40 * 86400;
    localtime_r(&d3, &t);
    snprintf(p1, sizeof(p1), "test_img_days/IMG_%04d%02d%02d_120000.jpg",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    localtime_r(&d40, &t);
    snprintf(p2, sizeof(p2), "test_img_days/IMG_%04d%02d%02d_120000.jpg",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    FILE *fp;
    fp = fopen(p1, "w"); fclose(fp);
    fp = fopen(p2, "w"); fclose(fp);

    PathList pl;
    load_images(&pl, "test_img_days", 14);
    CHECK(pl.n == 1);
    CHECK(strcmp(pl.v[0], p1) == 0);
    free(pl.v[0]);
    free(pl.v);

    load_images(&pl, "test_img_days", 0);
    CHECK(pl.n == 2);
    for (int i = 0; i < pl.n; i++) free(pl.v[i]);
    free(pl.v);

    system("rm -rf test_img_days");
}

static void test_load_images_tod_sort(void)
{
    system("rm -rf test_img_days && mkdir -p test_img_days");
    const char *names[] = {
        "IMG_20240101_060000.jpg",
        "IMG_20240103_120000.jpg",
        "IMG_20240102_230000.jpg",
    };
    for (int i = 0; i < 3; i++) {
        char path[256];
        snprintf(path, sizeof(path), "test_img_days/%s", names[i]);
        FILE *fp = fopen(path, "w");
        fclose(fp);
    }
    PathList pl;
    load_images(&pl, "test_img_days", 0);
    CHECK(pl.n == 3);
    CHECK(strstr(pl.v[0], "_060000") != NULL);
    CHECK(strstr(pl.v[1], "_120000") != NULL);
    CHECK(strstr(pl.v[2], "_230000") != NULL);
    for (int i = 0; i < pl.n; i++) free(pl.v[i]);
    free(pl.v);
    system("rm -rf test_img_days");
}

static void test_title_text(void)
{
    char buf[32];
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = 126; t.tm_mon = 8; t.tm_mday = 10;
    time_t when = mktime(&t);
    CHECK(title_text(buf, sizeof(buf), when));
    CHECK(strcmp(buf, "Kof 26") == 0);
    memset(&t, 0, sizeof(t));
    t.tm_year = 104; t.tm_mon = 0; t.tm_mday = 1;
    when = mktime(&t);
    CHECK(title_text(buf, sizeof(buf), when));
    CHECK(strcmp(buf, "Kof 04") == 0);
    CHECK(!title_text(buf, 2, when));
}

static void test_title_font(void)
{
    FILE *fp = fopen("test_font_override.txt", "w");
    fclose(fp);
    setenv("MICHACKA_TITLE_FONT", "test_font_override.txt", 1);
    const char *p = title_font_path();
    CHECK(p && strcmp(p, "test_font_override.txt") == 0);
    unsetenv("MICHACKA_TITLE_FONT");
    CHECK(title_font_path() != NULL);
    remove("test_font_override.txt");
}

static void test_parse_len(void)
{
    CHECK_CLOSE(parse_len("600"), 600);
    CHECK_CLOSE(parse_len("90s"), 90);
    CHECK_CLOSE(parse_len("10m"), 600);
    CHECK_CLOSE(parse_len("10min"), 600);
    CHECK_CLOSE(parse_len("10MIN"), 600);
    CHECK_CLOSE(parse_len("1H"), 3600);
    CHECK_CLOSE(parse_len("1.5h"), 5400);
    CHECK_CLOSE(parse_len("1h10m"), 4200);
    CHECK_CLOSE(parse_len("1h 10min"), 4200);
    CHECK(parse_len("") < 0);
    CHECK(parse_len("   ") < 0);
    CHECK(parse_len("abc") < 0);
    CHECK(parse_len("-5") < 0);
    CHECK(parse_len("1x") < 0);
    CHECK(parse_len("1h bogus") < 0);
}

static void test_cli(void)
{
    CHECK(run_cli("--help") == 0);
    CHECK(run_cli("-h") == 0);
    CHECK(run_cli("--bogus") != 0);
    CHECK(run_cli("--parts") != 0);
    CHECK(run_cli("--out") != 0);
    CHECK(run_cli("nope") != 0);
    CHECK(run_cli("day abc") != 0);
    CHECK(run_cli("day 42 extra") != 0);
    CHECK(run_cli("--parts 97") != 0);
    CHECK(run_cli("--len") != 0);
    CHECK(run_cli("--len abc") != 0);
    CHECK(run_cli("--len 0") != 0);
    CHECK(run_cli("--len 100000") != 0);
    CHECK(run_cli("-p") != 0);
    CHECK(run_cli("-l") != 0);
    CHECK(run_cli("-l abc") != 0);
    CHECK(run_cli("-o") != 0);
    CHECK(run_cli("--slide abc") != 0);
    CHECK(run_cli("--slide 61") != 0);
    CHECK(run_cli("--slide -1") != 0);
    CHECK(run_cli("--slide") != 0);
    CHECK(run_cli("--slide-days") != 0);
    CHECK(run_cli("--slide-days abc") != 0);
    CHECK(run_cli("--slide-days -1") != 0);
    CHECK(run_cli("--slide-days 4000") != 0);
    CHECK(run_cli("--slide-mb") != 0);
    CHECK(run_cli("--slide-mb abc") != 0);
    CHECK(run_cli("--slide-mb -1") != 0);
    CHECK(run_cli("--slide-mb 1001") != 0);
    CHECK(run_cli("--dense") != 0);
    CHECK(run_cli("--dense abc") != 0);
    CHECK(run_cli("--dense 0") != 0);
    CHECK(run_cli("--dense 9") != 0);
    CHECK(run_cli("--dense -2") != 0);
    CHECK(run_cli("--slide-only") != 0);
    CHECK(run_cli("--slide-only --dry-run") != 0);
    CHECK(run_cli("--slide-only --dry-run --out /nonexistent_dir/zz") != 0);
}

static void test_cli_slide_dryrun(void)
{
    system("rm -rf test_slide_env && mkdir -p test_slide_env/mus test_slide_env/fld "
           "test_slide_env/img test_slide_env/.config");
    system("touch test_slide_env/mus/a.wav test_slide_env/fld/b.wav test_slide_env/img/c.jpg");
    system("echo \"mus=$PWD/test_slide_env/mus\" > test_slide_env/.config/michacka.conf && "
           "echo \"fld=$PWD/test_slide_env/fld\" >> test_slide_env/.config/michacka.conf && "
           "echo \"img=$PWD/test_slide_env/img\" >> test_slide_env/.config/michacka.conf");

    const char *base =
        "MICHACKA_CONF=\"$PWD/test_slide_env/.config/michacka.conf\" "
        "./michacka day 1 --parts 1 --len 1s --dry-run %s --out test_slide_out >/dev/null 2>&1";
    char cmd[640];
    snprintf(cmd, sizeof(cmd), base, "--slide 2");
    CHECK(run_system_ok(cmd));
    CHECK(access("test_slide_out_slides.edl", F_OK) == 0);
    CHECK(access("test_slide_out_part01_music.edl", F_OK) == 0);

    snprintf(cmd, sizeof(cmd), base, "--no-slide");
    system("rm -f test_slide_out_slides.edl");
    CHECK(run_system_ok(cmd));
    CHECK(access("test_slide_out_slides.edl", F_OK) != 0);

    snprintf(cmd, sizeof(cmd), base, "-s 2");
    CHECK(run_system_ok(cmd));
    CHECK(access("test_slide_out_slides.edl", F_OK) == 0);

    snprintf(cmd, sizeof(cmd), base, "--slide 25 --slide-days 14 --slide-mb 10");
    system("rm -f test_slide_out_slides.edl");
    CHECK(run_system_ok(cmd));
    CHECK(access("test_slide_out_slides.edl", F_OK) == 0);

    snprintf(cmd, sizeof(cmd), base, "--slide 1 --slide-days 0 --slide-mb 5");
    CHECK(run_system_ok(cmd));
    CHECK(access("test_slide_out_slides.edl", F_OK) == 0);

    system("rm -rf test_slide_env test_slide_out_*");
}

int main(void)
{
    test_clampd();
    test_rng_deterministic();
    test_rng_ranges();
    test_shuffle_is_permutation();
    test_has_audio_ext();
    test_has_img_ext();
    test_sh_quote();
    test_path_tail();
    test_role_for();
    test_env_eval();
    test_style_by_name();
    test_style_defaults();
    test_slide_encode_config();
    test_image_taken();
    test_load_images_window();
    test_load_images_tod_sort();
    test_parse_len();
    test_layer_count_parity();
    test_find_and_collect();
    test_pick_slice();
    test_edl_put();
    test_build_arc();
    test_slide_planning();
    test_plan_slides_deterministic();
    test_plan_slides_sample_day();
    test_slide_plan_underfill();
    test_title_text();
    test_title_font();
    test_cli();
    test_cli_slide_dryrun();

    printf("%d/%d checks passed\n", checks_run - checks_failed, checks_run);
    return checks_failed ? 1 : 0;
}
