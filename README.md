# Míchačka

*míchačka* (Czech) — *mixer, blender.*

Generative composition driver for [`../tj`](../tj). Where tj is the renderer
(text EDL → mix), Míchačka is the composer brain: it scans your recording
libraries, plans layered movements algorithmically, writes the EDLs, and hands
them to tj for rendering, mastering, and export. Same seed in, same mix out.

## How it works

```
scan libraries ──► tj analyze ──► classify texture ──► plan movements ──► EDLs
  (music,          (BPM/key/       ambient / motion /    beds, layers,
   field)           density)        pulse roles           accents, arcs
                                                                       │
        mix.wav ◄── concat ◄── partNN_master.wav ◄── tj render ◄───────┘
        mix.flac                                    pass A: music
        mix.mp3                                     pass B: + field, master
```

1. **Scan** — recursively collects audio (`wav/flac/mp3/opus/ogg/m4a`) from a
   music library and a field-recording library.
2. **Analyze** — batch-calls `tj analyze` and parses BPM, key, and rhythmic
   texture per track (tj caches results in `~/.cache/tj/`, so this is instant
   after the first run). Each track gets a role:
   - `ambient` — sparse/steady → long bed layers, deep fades, low volume
   - `motion`  — moderate activity → mid-length moving layers
   - `pulse`   — steady beat/kick → staggered accent bursts
3. **Plan** — a style defines envelope curves (bed/motion/pulse/field counts +
   gain arc) sampled across the movement sequence. A seeded xorshift RNG picks
   sources, slices, positions, volumes, and fades. Plans respect tj's limits:
   ≤ 30 entries per EDL (tj caps at 32), positive spans, no overflow past the
   movement end.
4. **Render** — two passes per movement, mirroring `tj`'s day-cycle workflow:
   - *pass A*: music EDL → `partNN_music.wav`, with the style's generated
     master `--arc` and 0.5 s / 2 s global fades; optionally `--bpm auto
     --snap` (`--bpm`) and/or `--keylock auto` (`--keylock`)
   - *pass B*: rendered music as bed + field-recordings overlay (always native
     speed) → `partNN_master.wav` via `--master subtle`
5. **Finish** — lossless concat of all parts, FLAC/MP3 exports, ffprobe
   duration verification.

Movements land slightly under their nominal length (a 2 s safety margin plus
fade tails); the concat step handles variable part lengths losslessly.

## Usage

```sh
make
./michacka --style day                       # ~2 h day-cycle composition
./michacka --style storm --parts 6 --seed 42 # reproducible storm
./michacka --dry-run --limit 12              # plan only, tiny library sample
```

Re-run with the printed `--seed N` to reproduce a mix byte-for-byte in its EDL
plan (audio rendering is deterministic given identical inputs and tj flags).
The plan files are plain tj EDLs — edit one by hand and render it directly:

```sh
../tj/tj "$(cat P_part01_music.edl)" my_take.wav --bpm auto --snap --keylock auto
```

### Options

| Option | Meaning | Default |
|---|---|---|
| `--seed N` | RNG seed (printed when auto-generated) | random |
| `--parts N` | number of movements | style default |
| `--part-len SEC` | nominal length of each movement | style default |
| `--style NAME` | `day` `storm` `drift` `pulse` `rupture` | `day` |
| `--mus DIR` | music library | `~/recordings` |
| `--fld DIR` | field-recording library | `/mnt/data/recordings/field` |
| `--tj PATH` | tj renderer binary (env `MICHACKA_TJ`); runs `make -s -C ../tj` if missing | `../tj/tj` |
| `--out PREFIX` | output file prefix | `michacka_<style>_<min>min` |
| `--limit N` | use only N sampled files per library | all |
| `--bpm` | beat-match music pass (tj `--bpm auto --snap`) | off |
| `--keylock` | transpose music pass to shared key | off |
| `--no-master` | skip the mastering pass | mastering on (`subtle`) |
| `--jobs N` | render movements in parallel | 1 |
| `--dry-run` | write EDLs only, no audio | off |
| `--force` | re-render existing movements | skip |

### Styles

| Style | Shape |
|---|---|
| `day` | dawn → morning → midday peak → evening decay → night dissolve (default 12×600 s) |
| `storm` | fast ramp into dense sustained layers, abrupt ending (6×300 s) |
| `drift` | sparse ambient washes, no pulse, very long fades (8×600 s) |
| `pulse` | beat-driven from the start, steady accent density (10×600 s) |
| `rupture` | alternating dense/sparse movements (parity boost/cut, 8×450 s) |

## Output files

Per run with prefix `P`:

- `P_partNN_music.edl`, `P_partNN_field.edl` — human-readable plans (reusable
  directly as tj input)
- `P_partNN{,_music,_master}.wav` — intermediate renders
- `P_mix.{wav,flac,mp3}` — final concatenation and exports

Note: tj re-writes a sidecar `<output>.edl` after each render containing only
the first comma-segment of the received EDL (its `strtok` mangles the string
before dumping). Míchačka restores the full plan files after rendering, so the
`.edl` files listed above are always complete.

## Requirements

- `gcc`, GNU make
- [`../tj`](../tj) — Míchačka auto-builds it (`make -s -C ../tj`) when the
  binary is missing; tj itself needs `ffmpeg`, `soundstretch`, `keyfinder-cli`,
  `aubioonset`
- `ffprobe` (duration verification)

## Repo layout

```
michacka.c   single-file C program (the whole composer)
Makefile     gcc michacka.c -O2 -Wall
```
