# CLAUDE.md

`plutosa`: a spectrum analyzer for the ADALM-Pluto, a windowed RitterOS app
(runs under windowd, installed in `/system/<arch>/apps`).

## Building: on the RitterOS machine, never here

The user's rules: only this app's code is ours. Never read, build in or
use the local `~/ClaudeProjects/RitterOS` (the OS's own working tree) or
anything of the kernel and drivers. The SDK is read from GitHub
(`gh api repos/eloyr77/RitterOS/contents/sdk/CLAUDE.md -H "Accept:
application/vnd.github.raw"`, likewise `sdk/API.md`, `sdk/ritteros.mk`),
and the app is built on the machine with its own `cc` and
`/system/sdk/ritteros.mk` (the SDK's "Building on the machine"). From the
Mac, `make`, `make deploy`, `make run`, `make stop`, `make check-machine`,
`make check-radio` copy only the app's code (`Makefile`, `src/`, `tests/`,
`libiio/include`, `libiio/src`) to `/home/root/plutosa` with tar and run
that target there over ssh (`HOST`, `SSH_OPTS`). `make deploy` also
brings the built program back to `dist/x86_64/plutosa` (git-ignored);
`make upload` installs that file as it is, for a fresh install (no sources,
no build): the shared libraries stay compatible, so it runs on newer
systems, while an older one may refuse it at the start ("needs FUNCTION":
`make deploy` then).
`.sdk/include` (git-ignored) is a copy of the machine's `/system/include`
for the editor's clangd only: `ssh HOST 'tar cf - -C /system include' | tar
xf - -C .sdk`.

## Layout

| Path | What |
|---|---|
| `src/plutosa.c` | the app: the acquisition (only reads blocks into a ring), spectrum, drawing and main threads, the keys and mouse; `plutosa [URI] [-p FILE]` (-p: the picture to FILE as PPM every 5 s, to look at it from the Mac) |
| `src/view.c`, `src/view.h` | the picture: text, spectrum with axes and marker, waterfall, drawn in our own pixel buffer (wfont_text) and shown with one window_blit |
| `src/radio.c`, `src/radio.h` | the receiver through libiio: settings, ranges, and the only writes the app makes (RX LO, rate, RF bandwidth, gain mode, gain) |
| `src/dsp.c`, `src/dsp.h` | the spectrum's arithmetic (FFT, windows, dBFS, average, max hold, peaks, floor): plain C, no RitterOS calls |
| `src/demod.c`, `src/demod.h` | the narrow FM demodulator's arithmetic: mix to 0 Hz, CIC (4th order, integers) by rate / ~50 kHz, channel filter (129-tap windowed sinc), discriminator (5 kHz = full scale), DC removal, 4 kHz low-pass, resampling to 48 kHz (with a pace), volume, 16-bit stereo; the channel before its filter for the FM window's spectrum. Plain C, no RitterOS calls |
| `tests/demodtest.c` | demod.c against made signals: tones through the mixer and CIC at several rates and offsets, the filter's pass and stop, an FM tone's amplitude, phase (the chain's delays), distortion and DC, blocks of odd sizes, frames per second, pace, volume and clipping: `make check` and `make check-machine` (with a speed line) |
| `tests/dsptest.c` | dsp.c against DFT and published window values: `make check` (the Mac), `make check-machine` (RitterOS's cc) |
| `tests/radiotest.c` | radio.c on the real Pluto: each receive setting changed, read back, clamped, then put back (`make check-radio`) |
| `tests/streamtest.c` | how many of the Pluto's samples the link carries (refills only, per block size) and the rate they really come at (peaks moved by an LO step): `make measure ARGS="URI MSPS SECONDS KBUFS SIZES..."` |
| `libiio/` | git submodule: eloyr77/ritteros-libiio (libiio 0.26's interface, iiod's network backend only); its sources are compiled into the program (`Makefile`'s `LIBIIO`) |
| `compile_flags.txt` | include paths for the editor's clangd only (`.sdk/include`; the build is the machine's) |
| `reports/` | findings handed to other projects (e.g. the NCM link report for RitterOS's kernel) |

`git submodule update --init` after a fresh clone.

## Working style (as RitterOS's)

- Small steps, each one visible working; explain the plan before coding.
- Names first: agree new names (programs, settings, files) with the user.
- Our own code. The Python analyzer in `../TezukaSDR/spectrum_analyzer`
  is the design reference (decimation, offset tuning, DC blocker, dBm
  calibration, FM demod), not code to copy. libad9361's FIR taps are ADI's
  (LGPL): using them is the user's decision.

## Roadmap (the user's choice: steps 1-5 first)

1. Skeleton: window + connection thread, the receiver's settings shown (done; works on the laptop with the real Pluto, 2026-10-27).
2. Samples: an acquisition thread reading I/Q from `cf-ad9361-lpc` (voltage0/1, `le:S12/16`) in contiguous blocks of 16384, measured (blocks/s, MS/s received, power and peak dBFS, DC offset), reconnecting after a failure (done 2026-10-27: 30-120 blocks/s, 0.3-2.4 MS/s of the Pluto's 30.72 MSPS; 150 s without a loss).
3. DSP: our own FFT, windows (Hann, Blackman-Harris, flat-top), dBFS, averaging, max hold (done 2026-10-27, not yet checked against a known signal: 30 checks pass on the Mac and on the laptop, each proven by a planted bug; on the Pluto 240-410 frames/s of 4096 points; keys f/w/a/r).
4. Display: spectrum trace, waterfall, axes, peak marker (`window_blit` of native pixels) (done 2026-10-27, seen through `-p`: 960x640, up to 25 pictures/s).
5. Controls: center frequency, span, RF bandwidth, gain/AGC, reference level (done 2026-10-27; radiotest passes on the Pluto; tuning by typing was used at the laptop on 2026-10-29, the other keys and the mouse not yet confirmed). Values (2026-10-29): a click on LO, span, BW (settings line) or gain (the line below) opens the SDK's text field (`window_field_*`) over it, its present value selected: typed in MHz or dB, Enter sets it (refused, with the range in red, if not a number or outside the Pluto's `_available` range; the box stays open), Esc or a click elsewhere gives up; a typed BW stops following the span, a typed gain is manual. Typing a digit opens the LO's box. The drawing thread draws the field over each picture before window_flush (it is not in `-p`'s picture). Key bytes inside an escape sequence (an arrow's ESC [ B) are skipped: until then the down arrow also stepped the bandwidth (its B). Keys: arrows/wheel tune by span/10 and move the reference level by 10 dB, double-click centers, s/S span (2.5-56 MHz), b/B RF bandwidth (= span, 1-56 MHz), g gain mode, +/- manual gain by 3 dB, f/w/a FFT size/window/averaging, m max hold, r reset. Changes are applied between blocks, clamped to the `_available` ranges and read back; 4 blocks are thrown away after each (iiod's kernel buffers).

FM demodulator (the user's plan of 2026-10-29): a second window `plutosa FM` (key `d`), its channel taken in software out of the full-rate stream (the user's choice: the main window keeps the full span; no decimation on the Pluto), a ±10 kHz spectrum of the channel, values frequency (inside the span), bandwidth (kHz), volume; audio to `/dev/pcm0` (48 kHz 16-bit stereo). Steps: D1 gap-free samples, D2 `src/demod.c` (mix, decimate to ~50 kHz, channel filter, discriminator, audio filter, resample, volume; tested on the Mac), D3 the window, D4 the sound. Names confirmed 2026-10-29: `src/demod.c`, `src/fmview.c`, `tests/demodtest.c`, key `d`, window `plutosa FM`. Squelch maybe later (D5).

- D1 (2026-10-29): the acquisition thread only reads blocks of 65536 into a ring of 4 (no lock the drawing holds: `pending` is atomic); the spectrum thread takes the newest. The loss (samples that came against rate x time, over 2 s) is shown: "1.88 MS/s (10% lost)". Result: the link itself is the limit, 8.0-8.3 MB/s (2.0-2.07 MS/s), just under the 2.083 MS/s (8.4 MB/s) the AD9361's lowest rate needs: 0-12% lost at 2.1 MS/s, in the app as with streamtest alone. Report for the kernel: `reports/2026-10-29-ncm-throughput.md` (half-full NTBs, one USB transfer per ACK). Until the link is faster, FM audio has small gaps even at the narrowest span. **The kernel was fixed the same day** (one ACK per ~7 segments now): streamtest 100.0% at 2.1, 3, 4 and 5 MS/s, 98.7% at 6, 75.5% at 8 (the link carries ~6 MS/s, 24 MB/s): spans up to 5 MHz are gap-free.
- D2 (2026-10-29): `src/demod.c`; demodtest's 48 checks pass on the Mac (ASan/UBSan) and the laptop: tones at 1000.0000 Hz with the CIC's theoretical gain, stopband -97 dB, the FM tone within 0.1% and 0.0002 rad of the predicted delay, 0.09% distortion. Planted bugs caught: mixer sign 20, discriminator conjugate 3, a comb missing 20, resampler inverse 13, filter twice as wide 1, no DC removal 6, wrapping 2, pace ignored 1, state restarted each block 29/14/4 (resetting the oscillator's renormalization counter changes nothing: below the CIC's rounding). Speed with RitterOS's cc: 6x real time at 2.1 MS/s, 3.5x at 5 MS/s (one core).
- The FPGA's decimation (measured, then left out at the user's word): `cf-ad9361-lpc` voltage0 `sampling_frequency` = rate / 8 switches it on (ADI's cf_axi_adc_core.c, receive only; the user OK'd writing it), but on Tezuka v0.3.21 the samples then come at rate / **20** (sample count and LO-step frequency scale agree), not the / 8 the driver reports.

Later: offset tuning.

## The Pluto

PlutoNano with Tezuka (iiod 0.26) over USB CDC-NCM: on RitterOS it is a
network card (`eth1` on the laptop), RitterOS gets 192.168.2.10 by DHCP,
the Pluto is `ip:192.168.2.1` (iiod on TCP 30431). Tezuka must be set to
NCM (`fw_setenv usb_ethernet_mode ncm`), not its default RNDIS.

**The test machine** is the user's laptop, RitterOS at 192.168.0.195 over
Wi-Fi: `make deploy run HOST=root@192.168.0.195 SSH_OPTS="-i ~/.ssh/ritteros"`
(the key is installed; windowd must be started at the machine by the user).
Run windowed apps in the background: the ssh session stays open until the
window closes. Since RitterOS 65c every core runs threads (the laptop's 16;
`cpus`, `threads` shows each thread's core and ticks): plutosa's threads do
run in parallel, so everything they share stays under `lock` (stdout's
printf too). Measured 2026-10-29: the acquisition thread uses about 20% of
one core and waits for iiod the rest (1.3-1.7 MS/s, 300-425 FFT/s of 4096,
as on one core); the drawing thread under 1%. A fresh install needs the key
again (`root`'s password once, then `~/.ssh/ritteros`), then `make upload`.

**The NCM link** (`reports/2026-10-27-ncm-link-dies.md`): it once stalled,
then died for good under many small exchanges; the user says the kernel is
fixed since (step 2 streamed 150 s without a loss). Whatever the link
does, a stall must not garble values (libiio drops and reopens a
connection after a failed exchange) and the app survives a dead link (it
shows it and connects again every 2 s).
**Writing to the Pluto:** only what the app's own controls set. Anything
else (debug attributes, test tones, anything on the TX side) only with the
user's OK and after checking its meaning in ADI's driver: `bist_tone` mode
1 injects into *TX* (2 is RX), and was once written by mistake.

The RX LO's reported bottom (46875001 Hz on Tezuka v0.3.21) is refused
("Invalid argument"); 47 MHz is taken: radio.c starts the range at the next
whole MHz. 70 MHz reads back as 69999998 (the PLL's resolution). The
lowest rate likewise: the reported 2083333 is refused (12 times it is under
the ADC's 25 MHz); radio.c starts at 2083334 (it reads back 2083333).

Check before blaming the app: RitterOS `net` (eth1's received/sent), and
the Pluto's `ifconfig usb0` over `/dev/usbserial0` (login root/analog).
