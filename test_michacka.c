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

static void test_style_defaults(void)
{
    for (size_t i = 0; i < sizeof(STYLES) / sizeof(STYLES[0]); i++)
        CHECK((int)(STYLES[i].def_parts * STYLES[i].def_len) == 600);
    CHECK(STYLES[ST_PULSE].bpm == 1 && STYLES[ST_PULSE].keylock == 0);
    CHECK(STYLES[ST_RUPTURE].bpm == 1 && STYLES[ST_RUPTURE].keylock == 1);
    CHECK(STYLES[ST_DAY].bpm == 0 && STYLES[ST_DAY].keylock == 0);
    CHECK(STYLES[ST_STORM].bpm == 0 && STYLES[ST_DRIFT].bpm == 0);
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
}

int main(void)
{
    test_clampd();
    test_rng_deterministic();
    test_rng_ranges();
    test_shuffle_is_permutation();
    test_has_audio_ext();
    test_sh_quote();
    test_path_tail();
    test_role_for();
    test_env_eval();
    test_style_by_name();
    test_style_defaults();
    test_parse_len();
    test_layer_count_parity();
    test_find_and_collect();
    test_pick_slice();
    test_edl_put();
    test_build_arc();
    test_cli();

    printf("%d/%d checks passed\n", checks_run - checks_failed, checks_run);
    return checks_failed ? 1 : 0;
}
