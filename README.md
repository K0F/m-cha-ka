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
```

## Output files

Per run with prefix `P`, after automatic cleanup only these remain:

- `P_partNN_{music,field}.edl` — human-readable plans, reusable as tj input
- `P_mix.{wav,flac,mp3}` — final concatenation and exports

## Requirements

- `gcc`, GNU make
- [tj](tj) — vendored as a git submodule; auto-built when missing
  (`git clone --recursive` once, or `git submodule update --init`)
- `ffmpeg`, `ffprobe`; tj's beat/key features additionally need
  `soundstretch`, `keyfinder-cli`, `aubioonset`

## Repo layout

```
michacka.c       single-file C program (the whole composer)
test_michacka.c  unit tests for the planning logic + CLI checks
Makefile         make / make test / make smoke
tj/              tj renderer (git submodule)
```
