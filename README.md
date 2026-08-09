# FM Audio Processor with MPX Encoder

# New version here: https://github.com/radiopushka/VostokRadio2

















A high-performance, low-latency FM audio processor and MPX encoder written in C. This software is designed for professional radio broadcasting applications, providing multiband compression, AGC, and MPX generation with minimal CPU usage.

## Features

- **Multiband Compression**: n-band compressor with configurable parameters per band
- **Advanced AGC**: Automatic gain control with adjustable target and response
- **MPX Encoding**: Stereo encoder with pilot tone generation for FM broadcasting
- **High Efficiency**: ~10-16% CPU usage on Intel i5 12th gen at 192kHz
- **ALSA Support**: Native Linux audio support
- **Lookahead Clipping**: Advanced clipping prevention algorithm
- **FM Composite Clipping**: boosts loudness by sacrificing stereo information for less distortion and louder sound. Uses hyperbolic waveshaping function tanh.
- **Advanced Clipper**: fast lookahead limiter works in conjunction with the clipper to minimize distortion
- **Web UI**: every setting is adjustable from a browser while the processor is
  running, with live level meters. The page is compiled into the binary, there
  is nothing to install and no runtime dependency beyond ALSA.
- **Built in stream player**: paste an mp3 stream url into the web ui and the
  processor runs and supervises the player itself, so a headless box needs
  nothing else to go from a webstream to a transmitter.

## Installation & Compilation

### Prerequisites
- GCC compiler
- ALSA development libraries
- Must be running Linux, no Mac or FreeBSD. Windows is out of question.
- Linux user must be in audio group
- Pulseaudio must be off if you are generating MPX signals (otherwise you can have it on and use it)
- 192khz S32 capable sound card (most laptop sound cards)
- Nothing else. The web ui needs no web server, no node, no python and no
  libraries, it is plain sockets and one html file compiled into the binary.
  `sed` regenerates that file, which every Linux already has.

### Building
1. Clone the repository:
   ```bash
   git clone https://github.com/radiopushka/RadioProcessor
   cd RadioProcessor
   ```

2. Set the audio interfaces in `DEFAULTS.h` (`RECORDING_IFACE` and
   `PLAYBACK_IFACE`). Everything else can be tuned later from the browser:
   ```bash
   nvim DEFAULTS.h  # or use your preferred editor
   ```

3. Compile the program:
   ```bash
   make
   ```

4. Run the processor:
   ```bash
   ./touhouradio
   ```

5. Open `http://<the machine running it>:8080/` and tune it while it plays.

### Command line options

```
./touhouradio [options]
  -c FILE   settings file (default vostok.conf in the working directory)
  -p PORT   web ui port, 0 or -n turns the web ui off
  -b ADDR   web ui bind address, 127.0.0.1 keeps it on this machine
  -n        do not start the web ui
  -h        help
```

## Configuration

There are two layers:

- **`DEFAULTS.h`** holds the compile time defaults. It is still the place to set
  the starting point of a fresh install, and it is still just a header of
  `#define`s and arrays.
- **`vostok.conf`** holds whatever you saved from the web ui. It is read at
  startup and it wins over `DEFAULTS.h`. Delete it to go back to the compiled
  in defaults. It is a plain `key = value` text file, per band values are
  comma separated, so you can also edit it by hand or copy it between machines.

Nothing needs a recompile any more, including the settings that used to be
`#define`s such as `MPX_ENABLE`, `HIGH_PASS`, `MULTIBAND_COMPRESSION`,
`MONO_COMPRESSION`, `BYPASS`, `FINAL_CLIP` and `TAPE_SAT_BYPASS`. They are all
switches in the web ui now.

Here's a detailed but outdated explanation of the parameters:

### Multiband Compression
```c
int fdef[]={190,500,3000,7000,15000}; // Frequency bands for multiband compression
int fdef_size=5; // Number of bands

float def_attack[]={0.0000000001,0.000001,0.0001,0.0002,0.001}; // Attack times per band
float def_release[]={0.00000000000001,0.000000001,0.000003,0.000003,0.0005}; // Release times per band
float def_target[]={15000,17000,17000,17000,24000}; // Target volume per band
float def_m_gain[]={400,400,400,400,400}; // Maximum gain per band
float pre_amp[]={1,2,1,4,40}; // Pre-compression gain per band
float def_gate[]={6000,3000,4000,4000,4000}; // Noise gate threshold per band
int bypass[]={0,0,0,0,0}; // Bypass bands (1 to bypass, 0 to process)
float post_amp[]={1,1,1,1,1}; // Post-compression gain per band
int types[]={COMP_RMS,COMP_RMS,COMP_PEAK,COMP_PEAK,COMP_PEAK}; // Compressor types
```

### Global Settings
```c
#define FINAL_AMP 1 // Global gain after multiband compression
#define FINAL_CLIP // Enable final clipping (comment to disable)
#define FINAL_CLIP_LOOKAHEAD 1000 // Lookahead samples for clipping prevention
#define FINAL_CLIP_LOOKAHEAD_RELEASE 0.0001 // Release coefficient for clipper
```

### ALSA Configuration
```c
#define RECORDING_IFACE "default" // Input interface
#define PLAYBACK_IFACE "default"  // Output interface
#define RATE 192000 // Output sample rate (for MPX)
```

### Stereo & AGC Settings
```c
#define STEREO 1 // Enable stereo processing (1) or mono (0)
#define STEREO_GAIN 1.6 // Stereo amplification coefficient
#define AGC_TARG 12000 // AGC target level
#define AGC_SPEED 0.0001 // AGC response coefficient
#define AGC_GATE 3000 // AGC noise gate threshold
```

### FM MPX Settings
```c
//#define MPX_ENABLE // Uncomment to enable MPX encoding
#define COMPOSITE_CLIPPER // Enable composite clipper
#define COMPOSITE_CLIPPER_LOOKAHEAD 100 // Lookahead samples
#define COMPOSITE_CLIPPER_LOOKAHEAD_RELEASE 0.001 // Release coefficient
#define PERCENT_PILOT 0.09 // Pilot tone percentage (19kHz)
#define PERCENT_MONO 0.45 // Mono signal percentage
```

### GUI Settings
```c
#define GUI 0 // the unfinished ANSI terminal meters, the web ui replaces them
#define WEB_PORT 8080
#define WEB_BIND "0.0.0.0"     // "127.0.0.1" to only allow control from this machine
#define CONFIG_FILE "vostok.conf"
```

## Web UI

Start the processor and point a browser at port 8080:

```
http://192.168.1.50:8080/      # from a laptop on the same network
http://localhost:8080/         # on the machine itself
```

The page is generated from the parameter table in the program, so it always
shows every setting the running binary actually has. Nothing is served from
disk, `webui/index.html` is baked into the executable at compile time.

### What you get

- **Level meters** for before/after AGC, before/after the final clipper, how
  hard the clipper is working, and the DSP load as a percentage of realtime.
  If the load goes near 100% the machine cannot keep up and you will hear
  dropouts.
- **A stream source**, see the section below.
- **A sound card status line** at the top of the page. The web ui starts before
  the audio devices are opened, so if a card is busy or missing you get told
  which one and why, instead of the program refusing to start.
- **Every parameter**, grouped: Audio I/O, AGC, Stereo, Downward expander,
  Multiband, Bands, Chain, Tape saturation, Final clipper, MPX, Interface.
- **A band table**, one column per band with a small level bar in the header,
  so you can see which band is doing the work while you tune it.
- **A filter box** in the header. Type `pilot`, `attack`, `mpx` and only the
  matching settings stay on screen.

### How changes are applied

Every control carries a badge telling you what it costs:

| Badge | Meaning |
|---|---|
| *(none)* | Live. Picked up on the next audio buffer, no interruption. |
| `rebuild` | The filters or compressors are rebuilt. Expect a short click. Band count, crossover frequencies, filter poles, lookahead sizes. |
| `cache` | The MPX wave cache is regenerated. Audio drops for a few seconds. Only the two phase offsets do this. |
| `restart` | Needs the process restarted. Audio devices, sample rates, buffer size, web ui address. |

Changes take effect immediately but live in memory only. **Save** writes them to
`vostok.conf`, **Reload file** throws away your edits and re-reads that file,
**Restart** saves and then re-executes the program, which is how the
restart-only settings take effect. The restart keeps the same process id and
the page reconnects on its own.

### Playing an internet radio stream

Fill in **Stream URL** under *Stream source*, switch **Play a stream** on, and
the processor starts a player and keeps it alive. A status line under the
controls says what it is doing: starting, playing, or why it stopped.

It needs two things on the machine:

```bash
# a player. mpv is preferred because it is the only one that buffers the stream
sudo apt install -y mpv          # or ffmpeg, or mpg123

# a virtual cable from the player back into the processor
echo snd-aloop | sudo tee /etc/modules-load.d/snd-aloop.conf
sudo modprobe snd-aloop
```

Then set the two devices so they are the opposite ends of the loopback:

| Setting | Value |
|---|---|
| `io.record_device` | `hw:CARD=Loopback,DEV=1` |
| `source.device` | `plughw:CARD=Loopback,DEV=0` |

The player is told to output at `io.input_rate`, in stereo, as 32 bit samples,
which is exactly what the processor asks the loopback for. Both ends therefore
agree no matter which one opens first, which is the usual reason a manual
loopback setup refuses to start.

Other settings in that group:

- **Player**: `auto` takes the first of mpv, ffmpeg or mpg123 that is installed.
  Name one explicitly if you want to pin it.
- **Buffer**: seconds of stream read ahead, mpv only. 20 to 30 seconds rides
  out network hiccups, and costs you the same amount of delay. Drop it to 5 if
  you want to stay closer to live.

Edit the url while it is playing and the player is swapped over within a
second. If the stream or the player dies it is restarted, backing off up to 30
seconds if it keeps failing. Correct a bad url and it retries immediately
rather than sitting out the backoff.

The url is passed to the player as a single argument through `execv`, never
through a shell, and it is rejected unless it starts with `http://` or
`https://` and contains no whitespace. That matters because the web ui has no
password: see below.

### Security

There is no password and no TLS. Anyone who can reach the port can retune your
transmitter. Treat it like any other rack control surface:

Anyone who can reach it can also point the stream player at a url of their
choosing. The url can only ever be an http address handed to the player as one
argument, so it is not a way to run commands, but it is still someone else
deciding what you transmit.

- Keep it on a trusted LAN, or
- bind it to localhost and reach it over an SSH tunnel:
  ```bash
  ./touhouradio -b 127.0.0.1
  ssh -N -L 8080:127.0.0.1:8080 pi@your-pi   # then browse to localhost:8080
  ```
- or firewall the port to the machines you control.

### HTTP API

Handy for scripting, presets and remote automation.

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | the control page |
| GET | `/api/schema` | every parameter with type, range, group and help text |
| GET | `/api/state` | current values |
| GET | `/api/meters` | live levels, distortion and DSP load |
| POST | `/api/set` | `name=value&other=value`, form encoded. Band values are indexed: `band.target[2]=12000` |
| POST | `/api/save` | write the settings file |
| POST | `/api/load` | re-read the settings file |
| POST | `/api/restart` | save and restart the processor |

```bash
curl -X POST -d "agc.target=9000&mpx.percent_stereo=5.5" http://pi:8080/api/set
curl -X POST -d "band.target[6]=26000&band.bypass[0]=1"  http://pi:8080/api/set
curl -s http://pi:8080/api/meters
```

## Usage Notes

1. The program always records at 48kHz but outputs at the rate specified by `RATE` (192kHz recommended for MPX).

2. For FM broadcasting with MPX:
   - Uncomment `#define MPX_ENABLE`
   - Set `RATE` to 192000
   - Ensure `STEREO` is set to 1

3. For AM broadcasting:
   - Comment out `#define MPX_ENABLE`
   - Set `STEREO` to 0
   - You can use a lower output sample rate

4. The compressor types can be:
   - `COMP_RMS`: RMS-based compression (smoother)
   - `COMP_PEAK`: Peak-based compression (more aggressive)
5. The Program already comes pre tuned for Pop music

## Pre-Tune
   - Tested and tuned on the Vostok Telecom hikari FM transmitter for pop music.

## Raspberry Pi

Full instructions, including every dependency, four build methods, a systemd
unit and the troubleshooting list, are in **[BUILDING-PI.md](BUILDING-PI.md)**.

The short version, on the Pi itself:

```bash
sudo apt update
sudo apt install -y build-essential libasound2-dev git
sudo usermod -aG audio $USER      # then log out and back in

git clone https://github.com/radiopushka/RadioProcessor
cd RadioProcessor
make pi                           # or pi64 / pi32 / pi0 for a specific model
./touhouradio
```

`libasound2-dev` is the only library dependency. The web ui adds none.

You end up with one file, `touhouradio`, with the DSP, the defaults and the
control page all inside it. Copying it to another Pi is the whole deployment.
`make pi-static` links it against nothing at all if you want it to run on a Pi
with no ALSA installed.

Two things that catch people out:

- The Pi's own audio output cannot do MPX. The headphone jack is a PWM output
  with no 192kHz and no S32, and HDMI will not do it either, so **MPX needs a
  USB sound card capable of 192kHz S32**. For AM, or for a transmitter with its
  own stereo encoder, any 48kHz card works: turn `mpx.enable` off and set
  `io.output_rate` to 48000.
- Turn PulseAudio or PipeWire off if you are generating MPX, they resample
  behind your back.

## Performance

The processor is highly optimized:
- ~10-16% CPU usage on one thread of an Intel i5 12th gen
- ~30-40% CPU usage on one thread of an Intel i5 6200U
- ~50-65% CPU usage on one thread of an Intel i5 520M and slightly faster on Intel i5 2nd gen
- 100% CPU usage on one thread of an old Intel Atom notebook processor.
- Processes audio at 192kHz sample rate
- Low latency processing suitable for live broadcasting

## Troubleshooting

1. If you get ALSA errors, check your audio interfaces with `arecord -L` and `aplay -L` and update `RECORDING_IFACE`/`PLAYBACK_IFACE` accordingly, or set them in the web ui under Audio I/O and press Restart.

2. If experiencing distortion, watch the "clipper activity" meter in the web ui while you:
   - Reduce `chain.final_amp`
   - Adjust compressor settings
   - Check that input levels are appropriate
   - Play with the final clipper settings
   - Use larger lookaheads

3. For CPU usage issues, watch "dsp load" in the web ui and see the Raspberry Pi section, the same advice applies to any slow machine:
   - Reduce `io.output_rate` (if not using MPX)
   - Simplify compression settings
   - Use smaller lookaheads

4. If the web page does not load, check the console output for the address it
   printed. Add `-b 0.0.0.0` if you set it to localhost earlier, and remember a
   saved `vostok.conf` overrides `WEB_PORT` from `DEFAULTS.h`.

## Notes on what changed with the web ui

Making every `#define` runtime adjustable turned up a few things worth knowing
about, especially if you have already tuned a station by ear.

**Fixed, and it changes the sound slightly**

- `get_48_19k()` and `get_48_38k()` indexed the 48kHz pilot tables with the
  192kHz iterator. Those tables are a quarter as long, so the composite clipper
  was reading up to four times past the end of the allocation, and the "19kHz
  reference" it limited against was mostly whatever else was on the heap. It now
  uses the 48kHz iterator the code already kept for exactly this. The clipper
  behaves consistently now, which is audible if you were tuned tightly against
  the old behaviour. It was also a real crash: regenerating the MPX cache moves
  the heap around and the old code segfaulted the moment it did.

**Fixed, no change with the default settings**

- The MPX call passed `PERCENT_STEREO` into the mono argument and vice versa.
  Both defaulted to 6.5, so nothing moved, but the two controls only mean what
  they say now.
- `LEFT_MPX` did nothing: both branches of the `#ifdef` assigned the composite
  signal to the left channel, so both channels always carried MPX no matter
  what you commented out. `mpx.left` and `mpx.right` both default to on, which
  is what the old binary really did. Turn `mpx.left` off if you want the
  composite on one channel only, which is the point of the setting.
- The non-MPX output path did not compile, it called the resampler with six
  arguments where it takes four. Nobody noticed because it was behind
  `#ifdef MPX_ENABLE`. It works now: clip, scale, resample, left and right stay
  left and right.
- Ctrl-C and SIGTERM now shut the ALSA pipe and the web server down cleanly
  instead of killing the process mid buffer.

**Settings that never did anything, and are therefore not in the web ui**

- `COMPOSITE_CLIPPER` and its two settings: the limiter object was created and
  freed and never once used on the audio.
- `PRE_CLIP_SATURATION`, `PRE_CLIP_SATURATION_LIMIT`, `POST_SAT_GAIN`: passed to
  `create_sigmoidal_limiter()`, which ignores its last three arguments.
- `DC_REMOVAL_COEFF`: not referenced anywhere.

**Still experimental**

- `mb.mono_compression` (the old `MONO_COMPRESSION`) is off by default and
  should stay that way for now. The callback treats the compressor's output
  sample as if it were a gain multiplier, so levels square and the clipper ends
  up working flat out. It is preserved exactly as it was rather than quietly
  redesigned.

**Under the hood**

- Settings live in a struct guarded by a mutex. The audio thread copies it once
  per buffer, so the browser cannot tear a value in the middle of the DSP loop
  and the hot loop never locks.
- The audio buffers moved from the stack to the heap, since a large
  `io.buffer_size` at a 1:1 rate ratio could otherwise overflow the stack.
- The web server is about 400 lines of plain POSIX sockets on one thread. No
  libraries were added, `-lpthread` is the only new linker flag.

## License

GPL, use at your own risk

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.

## Disclaimer

This software is intended for educational and experimental purposes. Ensure compliance with local regulations when transmitting radio signals.

<img width="1628" height="1091" alt="2025-09-24_10-55" src="https://github.com/user-attachments/assets/22844413-17af-4eae-ae71-2a6e21b36332" />
with these settings:
<img width="1867" height="790" alt="2025-09-27_23-45" src="https://github.com/user-attachments/assets/193cc903-ca14-4493-b557-b86169f94058" />
<img width="841" height="318" alt="2025-09-27_23-46" src="https://github.com/user-attachments/assets/846420e5-182f-4814-8d9f-c03e5f9aab91" />


# Building for Raspberry Pi

Step by step, including every dependency. Pick one of the four methods:

| | Method | Use it when |
|---|---|---|
| A | [Build on the Pi](#method-a-build-on-the-pi) | Almost always. Simplest and least to go wrong. |
| B | [Static binary](#method-b-static-binary-no-shared-libraries) | You want one file that runs on a Pi with nothing installed on it. |
| C | [Cross compile from a PC](#method-c-cross-compile-from-a-pc) | The Pi is slow (Zero), or you build often. |
| D | [Docker + emulation](#method-d-docker--emulation) | You want C without setting up a sysroot. |

Whichever you pick, the result is a single file called `touhouradio`. The DSP,
the defaults and the whole web control page are inside it. Deploying is
`scp touhouradio pi@host:` and nothing else. A `vostok.conf` next to it is
optional, without one the binary runs on its compiled in defaults.

---

## What you need to know first

### Which target is yours

```bash
uname -m
```

| `uname -m` | Board and OS | Make target |
|---|---|---|
| `aarch64` | Pi 3 / 4 / 5, 64 bit Raspberry Pi OS or Ubuntu | `make pi64` |
| `armv7l` | Pi 2 / 3 / 4, 32 bit Raspberry Pi OS | `make pi32` |
| `armv6l` | Pi Zero, Zero W, Pi 1 | `make pi0` |

`make pi` uses `-mcpu=native` and works out the right flags itself. Use it
first, fall back to the model specific target if your gcc rejects it.

### The sound card matters more than the build

The Pi's own audio output **cannot do MPX**. The headphone jack is a PWM output
that does not do 192kHz and does not do S32, and HDMI audio will not do it
either. For MPX you need a **USB sound card that supports 192kHz S32 playback**.

For AM, or for feeding a transmitter that has its own stereo encoder, any
48kHz card is fine: turn `mpx.enable` off and set `io.output_rate` to 48000 in
the web ui.

---

## Method A: build on the Pi

### 1. Install the dependencies

Raspberry Pi OS, Debian and Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential libasound2-dev git
```

That is the complete list:

| Package | Gives you | Why |
|---|---|---|
| `build-essential` | gcc, make, libc headers | compiling |
| `libasound2-dev` | `alsa/asoundlib.h`, `libasound.so` | the only library this program links against |
| `git` | git | only to clone the repo, skip it if you copy the source across |

Nothing else to *build* it. The web ui needs no web server, no node, no python
and no libraries. The page is turned into a C string by `sed`, which every
Linux already has.

If you want the processor to play an internet radio stream by itself, add a
player and the loopback module at runtime. Neither is needed to compile:

```bash
sudo apt install -y mpv                                   # or ffmpeg, or mpg123
echo snd-aloop | sudo tee /etc/modules-load.d/snd-aloop.conf
sudo modprobe snd-aloop
```

Other distributions:

```bash
# Arch / Manjaro ARM
sudo pacman -S --needed base-devel alsa-lib git
# Alpine
sudo apk add build-base alsa-lib-dev git
# Fedora on a Pi
sudo dnf install -y gcc make alsa-lib-devel git
```

### 2. Check the dependencies actually landed

```bash
gcc --version                     # gcc 8 or newer is plenty
ls /usr/include/alsa/asoundlib.h  # must exist, this is the usual failure
```

### 3. Put yourself in the audio group

```bash
sudo usermod -aG audio $USER
```

Log out and back in, then confirm:

```bash
groups | grep audio
```

### 4. Build

```bash
git clone https://github.com/radiopushka/RadioProcessor
cd RadioProcessor
make pi
```

If gcc complains about `-mcpu=native`, use your model's target instead:

```bash
make pi64      # or pi32, or pi0
```

Expect somewhere between ten seconds on a Pi 5 and a couple of minutes on a
Zero. Everything is compiled in one pass, so every `make` is a full rebuild and
there are no stale objects to worry about.

### 5. Check what you built

```bash
ls -la touhouradio
./touhouradio -h
```

### 6. Set the audio devices and run

```bash
arecord -L     # capture device names
aplay -L       # playback device names
```

Put those into `DEFAULTS.h` as `RECORDING_IFACE` and `PLAYBACK_IFACE` and
rebuild, or just start the program and set them under **Audio I/O** in the web
ui, press **Save**, then **Restart**.

Turn PulseAudio or PipeWire off if you are generating MPX, they will resample
behind your back:

```bash
systemctl --user disable --now pulseaudio.socket pulseaudio.service
systemctl --user disable --now pipewire.socket pipewire.service wireplumber.service
```

Then:

```bash
./touhouradio
```

It prints the address of the control page, something like:

```
web ui: http://localhost:8080/
        http://192.168.1.42:8080/  (wlan0)
```

The page comes up even when the sound card cannot be opened, for instance
because something else is using it. It tells you which device failed and why,
retries every three seconds, and picks up as soon as the card is free. So you
can set the device names in the browser and never touch `DEFAULTS.h`.

---

## Method B: static binary, no shared libraries

For a read only image, a minimal OS, or building once and dropping the same
file onto several Pis without installing ALSA on any of them.

### 1. Dependencies

Same as method A. Debian ships the static libraries inside `libc6-dev` (part of
`build-essential`) and `libasound2-dev`.

### 2. Check the static libraries are really there

```bash
ls /usr/lib/*/libasound.a /usr/lib/*/libc.a
```

If `libasound.a` is missing, build alsa-lib yourself, it takes a few minutes:

```bash
sudo apt install -y wget
# pick the current version from https://www.alsa-project.org/files/pub/lib/
V=1.2.11
wget https://www.alsa-project.org/files/pub/lib/alsa-lib-$V.tar.bz2
tar xf alsa-lib-$V.tar.bz2
cd alsa-lib-$V
./configure --enable-static --disable-shared --prefix=/usr/local
make -j$(nproc)
sudo make install
cd ..
```

`/usr/local/lib` is already on gcc's search path, so nothing else to configure.

### 3. Build

```bash
make pi-static
```

### 4. Check it

```bash
ldd touhouradio
# -> "not a dynamic executable"   that is what you want
```

### Static build caveat

A static binary cannot load the ALSA plugins that live in shared objects, so
use **real hardware device names** like `hw:1,0` or `hw:CARD=Device,DEV=0`.
`default`, `pulse` and `plug:...` will not resolve. This is no loss here: MPX
wants the raw hardware device anyway.

---

## Method C: cross compile from a PC

Much faster than a Pi Zero. You need the ARM compiler plus a copy of the Pi's
headers and libraries, because the linker needs the ARM build of libasound.

### 1. Install the cross compiler on the PC

```bash
# Debian / Ubuntu host
sudo apt install -y gcc-aarch64-linux-gnu        # for a 64 bit Pi
sudo apt install -y gcc-arm-linux-gnueabihf      # for a 32 bit Pi
sudo apt install -y rsync

# Fedora host
sudo dnf install -y gcc-aarch64-linux-gnu rsync
```

Check:

```bash
aarch64-linux-gnu-gcc --version
```

### 2. Copy the Pi's system files into a sysroot

With the Pi switched on and reachable over the network:

```bash
mkdir -p ~/pi-sysroot/usr
rsync -a --copy-links pi@raspberrypi.local:/usr/include ~/pi-sysroot/usr/
rsync -a --copy-links pi@raspberrypi.local:/usr/lib     ~/pi-sysroot/usr/
rsync -a --copy-links pi@raspberrypi.local:/lib         ~/pi-sysroot/
```

`--copy-links` matters: it turns the Pi's absolute symlinks into real files, so
the cross linker can follow them. Make sure `libasound2-dev` is installed **on
the Pi** before you copy, otherwise the sysroot will not contain
`libasound.so` and the link step will fail.

### 3. Build

```bash
make cross-pi64 SYSROOT=$HOME/pi-sysroot
# or
make cross-pi32 SYSROOT=$HOME/pi-sysroot
```

If your compiler has a different name, pass it in:

```bash
make cross-pi64 SYSROOT=$HOME/pi-sysroot CROSS64=aarch64-none-linux-gnu-gcc
```

### 4. Check and deploy

```bash
file touhouradio
# -> ELF 64-bit LSB ..., ARM aarch64, ...
scp touhouradio pi@raspberrypi.local:
```

---

## Method D: Docker + emulation

Method C without the sysroot dance. Slower than real cross compiling, still far
quicker than a Pi Zero.

### 1. Register the ARM emulator on the host

```bash
# Debian / Ubuntu
sudo apt install -y qemu-user-static binfmt-support
# Fedora
sudo dnf install -y qemu-user-static
# or, distribution independent
docker run --privileged --rm tonistiigi/binfmt --install all
```

### 2. Build inside an ARM container

64 bit:

```bash
docker run --rm --platform linux/arm64 -v "$PWD":/src -w /src arm64v8/debian:bookworm \
  bash -c "apt-get update && apt-get install -y build-essential libasound2-dev && make pi64"
```

32 bit:

```bash
docker run --rm --platform linux/arm/v7 -v "$PWD":/src -w /src arm32v7/debian:bookworm \
  bash -c "apt-get update && apt-get install -y build-essential libasound2-dev && make pi32"
```

The binary appears in the current directory owned by root:

```bash
sudo chown $USER:$USER touhouradio
file touhouradio
```

---

## Start it at boot

`/etc/systemd/system/touhouradio.service`:

```ini
[Unit]
Description=Vostok Radio FM processor
After=sound.target network.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi
ExecStart=/home/pi/touhouradio
Restart=always
RestartSec=3
# audio work should not wait behind the desktop
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=50
LimitRTPRIO=99
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now touhouradio
journalctl -u touhouradio -f
```

`WorkingDirectory` is where `vostok.conf` gets written when you press Save.

If you use the built in stream player there is no second service to write: the
player is a child of this one, so systemd starts, stops and restarts it along
with the processor.

The web ui Restart button works under systemd: the process re-executes itself
and systemd never sees it stop.

---

## Build problems

**`fatal error: alsa/asoundlib.h: No such file or directory`**
`libasound2-dev` is not installed. This is by far the most common one.
`sudo apt install libasound2-dev`.

**`cc1: error: unknown value 'native' for -mcpu`**
Your gcc cannot detect the CPU. Use `make pi64`, `make pi32` or `make pi0`.

**`/usr/bin/ld: cannot find -lasound`** while cross compiling
The sysroot has no ARM libasound. Install `libasound2-dev` on the Pi, then
re-run the rsync commands. Also check you passed `SYSROOT=`.

**`undefined reference to 'pthread_create'`**
You are compiling by hand without `-lpthread`. Use the Makefile, it is in
`LIBS`.

**`make` says `'touhouradio' is up to date`**
It should not, every target rebuilds. If you hit this, `make clean` first.

**The binary runs but the web page never loads**
Read the address the program printed at startup. If it says
`http://localhost:8080/` only, it was bound to loopback: run with `-b 0.0.0.0`,
and remember a saved `vostok.conf` overrides `WEB_BIND` in `DEFAULTS.h`.

**Cross compiled binary gives `cannot execute binary file`**
You built for the wrong architecture. `file touhouradio` and compare against
`uname -m` on the Pi.

---

## Making it keep up on a Pi

Watch **dsp load** in the web ui header. Under about 70% you are fine, near
100% you will get dropouts and crackles. In rough order of effectiveness:

1. Lock the clock up:
   `echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`
2. Fewer bands (`mb.bands`) and fewer splitter poles (`mb.poles`). These
   dominate the cost.
3. Shorter band lookaheads (`band.lookahead`) and a shorter final clipper
   lookahead (`clip.buffer`).
4. If you do not need MPX, turn `mpx.enable` off and drop `io.output_rate` to
   48000. The 192kHz output stage is a large part of the load.
5. Run headless, no desktop.
6. A larger `io.buffer_size` costs latency but wastes less time on overhead.

A Pi 4 or 5 handles the default 7 band 192kHz MPX chain comfortably. A Pi Zero
will not: drop to fewer bands with no MPX, or use it for AM only.

## Pi specific gotchas

- `hw:` numbers move around when USB cards are plugged in and out. Use the
  stable name from `aplay -L`, for example `hw:CARD=Device,DEV=0`.
- Underruns right after boot usually mean the card was not ready yet. The
  systemd unit above retries, or add `ExecStartPre=/bin/sleep 5`.
- Undervoltage throttles the CPU and looks exactly like "not enough CPU".
  Check with `vcgencmd get_throttled`, anything other than `0x0` means the
  power supply is the problem.
