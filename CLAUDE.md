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
that target there over ssh (`HOST`, `SSH_OPTS`).
`.sdk/include` (git-ignored) is a copy of the machine's `/system/include`
for the editor's clangd only: `ssh HOST 'tar cf - -C /system include' | tar
xf - -C .sdk`.

## Layout

| Path | What |
|---|---|
| `src/plutosa.c` | the app: the acquisition, drawing and main threads, the keys and mouse; `plutosa [URI] [-p FILE]` (-p: the picture to FILE as PPM every 5 s, to look at it from the Mac) |
| `src/view.c`, `src/view.h` | the picture: text, spectrum with axes and marker, waterfall, drawn in our own pixel buffer (wfont_text) and shown with one window_blit |
| `src/radio.c`, `src/radio.h` | the receiver through libiio: settings, ranges, and the only writes the app makes (RX LO, rate, RF bandwidth, gain mode, gain) |
| `src/dsp.c`, `src/dsp.h` | the spectrum's arithmetic (FFT, windows, dBFS, average, max hold, peaks, floor): plain C, no RitterOS calls |
| `tests/dsptest.c` | dsp.c against DFT and published window values: `make check` (the Mac), `make check-machine` (RitterOS's cc) |
| `tests/radiotest.c` | radio.c on the real Pluto: each receive setting changed, read back, clamped, then put back (`make check-radio`) |
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
5. Controls: center frequency, span, RF bandwidth, gain/AGC, reference level (done 2026-10-27; radiotest passes on the Pluto; the keys and mouse themselves need a person at the laptop). Keys: digits + Enter tune (MHz), arrows/wheel tune by span/10 and move the reference level by 10 dB, double-click centers, s/S span (2.5-56 MHz), b/B RF bandwidth (= span, 1-56 MHz), g gain mode, +/- manual gain by 3 dB, f/w/a FFT size/window/averaging, m max hold, r reset. Changes are applied between blocks, clamped to the `_available` ranges and read back; 4 blocks are thrown away after each (iiod's kernel buffers).
Later: spans below 2.084 MSPS (software decimation; Tezuka v0.3.21 rejects FIR configs), offset tuning, FM audio to `/dev/pcm0`.

## The Pluto

PlutoNano with Tezuka (iiod 0.26) over USB CDC-NCM: on RitterOS it is a
network card (`eth1` on the laptop), RitterOS gets 192.168.2.10 by DHCP,
the Pluto is `ip:192.168.2.1` (iiod on TCP 30431). Tezuka must be set to
NCM (`fw_setenv usb_ethernet_mode ncm`), not its default RNDIS.

**The test machine** is the user's laptop, RitterOS at 192.168.0.195 over
Wi-Fi: `make deploy run HOST=root@192.168.0.195 SSH_OPTS="-i ~/.ssh/ritteros"`
(the key is installed; windowd must be started at the machine by the user).
Run windowed apps in the background: the ssh session stays open until the
window closes.

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
whole MHz. 70 MHz reads back as 69999998 (the PLL's resolution).

Check before blaming the app: RitterOS `net` (eth1's received/sent), and
the Pluto's `ifconfig usb0` over `/dev/usbserial0` (login root/analog).
