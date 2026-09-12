# Míchačka

*míchačka* (Czech) — *mixer, blender.*

Generative composition driver for [tj](tj): scans your recording libraries,
plans layered movements algorithmically, writes tj EDLs, and renders them into
a finished mix. Same seed in, same mix out.

## How it works

```
scan libraries → tj analyze → classify texture → plan movements → EDLs → tj render → concat/export
```

1. **Scan** — collects audio (`wav/flac/mp3/opus/ogg/m4a`) from a music library
   and a field-recording library; huge libraries are sampled down to 1000 files.
2. **Analyze** — batch-calls `tj analyze` (cached by tj) and assigns each track
   a role: `ambient` (beds), `motion` (moving layers), or `pulse` (accents).
3. **Plan** — a style defines layer-count and gain envelopes; a seeded xorshift
   RNG picks sources, slices, positions, volumes, and fades. Plans respect
   tj's EDL limits.
4. **Render** — per movement: pass A renders the music EDL (styles may add
   `--bpm auto --snap` / `--keylock auto`), pass B overlays field recordings
   and masters (`--master subtle`).
5. **Finish** — lossless concat of all parts, FLAC/MP3 exports, duration
   check. Intermediate renders are deleted automatically.
6. **Slides** — scans the photo library, keeps only photos from the last 14
   days (override with `--slide-days`), sorts them by time of day (ignoring the
   date, so a "day" plays dawn→night), and cuts a rapid hard-cut montage that
   fills the whole mix: every photo is held for at most 7 frames (0.28 s at
   25 fps), and when the window has fewer photos than the duration needs the
   day-ordered pool cycles. `--slide N` caps the number of distinct photos
   (default: all in the window); `--no-slide` / `--slide 0` disables. Cuts are
   static — no transitions, fades, or zoom. `--slide-mb MB` fills a target mp4
   size with a 2-pass VBR encode, choosing resolution/tier (720p, 1080p,
   1440p) automatically. The video opens with a "Kof YY" title in the Gomotor
   font (found in `~/.fonts`, `~/.local/share/fonts`, or `~/src/gomotor`;
   override with `MICHACKA_TITLE_FONT`).

Default runs are 10 minutes (one 600 s movement); `--parts N` extends a style
into its full multi-movement arc, and `--len DUR` sets the per-movement
length (`600`, `90s`, `15min`, `1h30m` — bare numbers are seconds).

## Usage

```sh
make
./michacka                    # 10-min day cycle, random seed
./michacka storm              # 10-min storm
./michacka day --parts 12     # full dawn→night arc
./michacka drift 42           # reproducible drift
./michacka pulse --dry-run    # write EDLs only
```

The seed is printed after each run — rerun with the same STYLE and SEED to get
an identical plan (and byte-for-byte mix given identical inputs).

### Config

Libraries and the tj binary come from a conf file (created on demand),
resolved in order: `$MICHACKA_CONF` → `$XDG_CONFIG_HOME/michacka.conf` →
`~/.config/michacka.conf`:

```ini
mus=~/recordings              # music library
fld=/mnt/data/recordings/field  # field-recording library
img=~/Dcim                   # photo library for the slideshow
tj=tj/tj                      # renderer; env MICHACKA_TJ overrides this line
```

Missing file → built-in defaults. Unknown keys abort with the line number.
The tj path resolves in order: `MICHACKA_TJ` env → conf → `tj/tj` submodule
(auto-built) → sibling checkouts.

### Options

| Argument | Short | Meaning | Default |
|---|---|---|---|
| `[STYLE]` | | `day` `storm` `drift` `pulse` `rupture` | `day` |
| `[SEED]` | | RNG seed | random |
| `--parts N` | `-p N` | number of movements | style default |
| `--len DUR` | `-l DUR` | per-movement length in seconds or `90s` / `15min` / `1h30m` | style default |
| `--out PREFIX` | `-o PREFIX` | output file prefix | `michacka_<style>_<min>min` |
| `--dense N` | | multiply every layer count by N (1–8); 2–3 layers "everything to hell" | 3 |
| `--slide N` | `-s N` | cap distinct photos in the slideshow (`0`/`--no-slide` disables); default uses every photo in the window, montage fills the mix length | all in window |
| `--slide-days N` | `-d N` | only photos from the last N days (`0` = all) | 14 |
| `--slide-mb MB` | `-m MB` | 2-pass VBR fill the slideshow mp4 to MB MB | unlimited |
| `--no-slide` | | no slideshow video | off |
| `--slide-only` | | slides from an existing `<prefix>_mix.wav`, no audio render | off |
| `--dry-run` | `-n` | write EDLs only, no audio | off |

### Styles

All styles default to a ~10-minute mix; `pulse` snaps layers to the beat,
`rupture` also locks them to one key.

| Style | Shape at `--parts N` |
|---|---|
| `day` | dawn → midday peak → night dissolve |
| `storm` | fast ramp into dense sustained layers, abrupt ending |
| `drift` | sparse ambient washes, very long fades, no pulse |
| `pulse` | beat-driven, steady accent density |
| `rupture` | alternating dense/sparse movements |

Plans are plain tj EDLs (`P_partNN_{music,field}.edl`) — edit one by hand and
render it directly:

```sh
tj/tj "$(cat P_part01_music.edl)" my_take.wav
```

### Examples

First run, then reproduce it exactly:

```sh
./michacka
# ... Done! Output: michacka_day_10min_mix.{wav,flac,mp3}
#     Reproduce with: ./michacka day 1753119601
./michacka day 1753119601     # identical plan, byte-for-byte mix
```

Quick smoke test without audio:

```sh
./michacka storm --parts 4 --seed 42 --dry-run --out demo
./michacka drift --parts 2 --len "45min"   # 2 x 45 min movements
./michacka day --len 90s                   # ~90 s mix + rapid full-window slideshow
./michacka day 42 --len 90s --slide-mb 10  # fill a ~10 MB mp4 with all recent photos
./michacka --dense 2 --slide 25 --slide-mb 10  # denser (2x) mix, up to 25 photos
./michacka --slide-only                   # render slides + mp4 for the last run's mix
```

## Output files

Per run with prefix `P`, after automatic cleanup only these remain:

- `P_partNN_{music,field}.edl` — human-readable plans, reusable as tj input
- `P_slides.edl` — the slideshow plan (day-sorted photos, frames per slide)
- `P_mix.{wav,flac,mp3}` — final concatenation and exports
- `P_slides.mp4` — rapid-cut (7-frame, no-transition) montage muxed with the mix

## Requirements

- `gcc`, GNU make
- [tj](tj) — vendored as a git submodule; auto-built when missing
  (`git clone --recursive` once, or `git submodule update --init`)
- `ffmpeg`, `ffprobe`; tj's beat/key features additionally need
  `soundstretch`, `keyfinder-cli`, `aubioonset`
- `ffmpeg` with `libx264` only if you use `--slide` (slideshow video output)

### Termux (Android)

Everything builds and runs under Termux; the config file and the CLI
are identical to desktop Linux. Install the toolchain and renderers:

```sh
pkg install clang make git ffmpeg soundtouch aubio
```

Then clone with the tj submodule and build:

```sh
git clone --recursive git@github.com:K0F/m-cha-ka.git
cd m-cha-ka
git submodule update --init tj   # if not cloned with --recursive
make
```

Notes for Android:

- Termux has `soundstretch` (from `soundtouch`) and `aubioonset` (from
  `aubio`), but **no `keyfinder-cli` package** — key detection reports `?` and
  `--keylock` is unavailable. BPM matching and everything else still works.
- Audio libraries (`mus`, `fld`) and photo libraries (`img`) are whatever you
  point the config at; Termux stores them under `~/` (or shared storage via
  `termux-setup-storage`).
- `/tmp` may not exist by default — keep outputs (`--out`) relative or under
  `~/`.
- `pkg install` line above also pulls the runtime tools tj shells out to.

## Repo layout

```
michacka.c       single-file C program (the whole composer)
test_michacka.c  unit tests for the planning logic + CLI checks
Makefile         make / make test / make smoke
tj/              tj renderer (git submodule)
```
