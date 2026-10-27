# CLAUDE.md

`plutosa`: a spectrum analyzer for the ADALM-Pluto, a windowed RitterOS app
(runs under windowd, installed in `/system/<arch>/apps`). Built with
RitterOS's SDK: read `../RitterOS/sdk/CLAUDE.md` first (toolchain, `make
deploy run HOST=...`, what RitterOS's C library and `window.h` offer).

## Layout

| Path | What |
|---|---|
| `src/plutosa.c` | the app |
| `libiio/` | git submodule: eloyr77/ritteros-libiio (libiio 0.26's interface, iiod's network backend only); its sources are compiled into the program (`Makefile`'s `LIBIIO`) |
| `compile_flags.txt` | include paths for the editor's clangd only (the build uses the SDK's) |
| `reports/` | findings handed to other projects (e.g. the NCM link report for RitterOS's kernel) |

`git submodule update --init` after a fresh clone. RitterOS is a sibling
checkout at `../RitterOS` (`RITTEROS=` for another place).

## Working style (as RitterOS's)

- Small steps, each one visible working; explain the plan before coding.
- Names first: agree new names (programs, settings, files) with the user.
- Our own code. The Python analyzer in `../TezukaSDR/spectrum_analyzer`
  is the design reference (decimation, offset tuning, DC blocker, dBm
  calibration, FM demod), not code to copy. libad9361's FIR taps are ADI's
  (LGPL): using them is the user's decision.

## Roadmap (the user's choice: steps 1-5 first)

1. Skeleton: window + connection thread, the receiver's settings shown (done; works on the laptop with the real Pluto, 2026-10-27).
2. Samples: an acquisition thread reading I/Q from `cf-ad9361-lpc` (voltage0/1, `le:S12/16`).
3. DSP: our own FFT, windows (Hann, Blackman-Harris, flat-top), dBFS, averaging, max hold.
4. Display: spectrum trace, waterfall, axes, peak marker (`window_blit` of native pixels).
5. Controls: center frequency, span, RF bandwidth, gain/AGC, reference level.
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

**The NCM link is not reliable yet** (2026-10-27,
`reports/2026-10-27-ncm-link-dies.md`): under many small request/response
exchanges it stalled once for ~25 s, then stopped in both directions for
good, with nothing logged, until the Pluto is replugged. That is RitterOS's
kernel (usbnet), handed to its agent. Until it is fixed: a stalled link
must not garble values (libiio now drops and reopens a connection after a
failed exchange) and the app must survive a dead one (show it, retry).
Check before blaming the app: RitterOS `net` (eth1's received/sent), and
the Pluto's `ifconfig usb0` over `/dev/usbserial0` (login root/analog).
