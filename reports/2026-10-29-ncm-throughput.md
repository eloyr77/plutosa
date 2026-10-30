# The NCM link to the PlutoSDR carries at most about 8 MB/s

For the agent working on the RitterOS kernel (`kernel/usbnet.c`, the TCP
receive path). Found on 2026-10-29 while preparing FM audio in a RitterOS
app (plutosa, a spectrum analyzer) on the user's laptop. Not a failure: a
ceiling, just under what the app needs.

## Summary

A single TCP stream from the Pluto's iiod (one 256 KiB answer after each
request) arrives at **8.0-8.3 MB/s at most**, whatever the request size
(64 KiB to 4 MiB). The app needs 8.4 MB/s without gaps: the AD9361's
lowest sample rate, 2.083 MS/s, times 4 bytes (I and Q, 16 bits each).
Above about 10 MB/s the app could play FM audio from the Pluto with no
dropouts. USB 2.0 high speed and NCM should carry several times this.

## Setup

| | |
|---|---|
| RitterOS | installed on the user's laptop (Ryzen 7 5800H, all 16 cores on since 65c-4), reached over Wi-Fi at 192.168.0.195 |
| Pluto | Nano PlutoSDR, Tezuka v0.3.21, `usb_ethernet_mode=ncm`, high speed on `usb0 port 1`; `eth1` 192.168.2.10, the Pluto 192.168.2.1 |
| Traffic | libiio's network protocol: `READBUF cf-ad9361-lpc N` on one TCP connection, the answer N bytes of samples, then the next request (one outstanding at a time) |

## Measured

`streamtest` (the app's repository, `tests/streamtest.c`; `make measure`
from the Mac) reads blocks for a few seconds and counts the samples that
come against the rate the Pluto makes them at. Nothing else runs.

| Rate set | Request (samples x 4 B) | Came | Refill time vs the block's samples |
|---|---|---|---|
| 2.1 MS/s | 16384 (64 KiB) | 1.91-2.07 MS/s (91-99%) | 7.9-8.6 ms for 7.8 ms |
| 2.1 MS/s | 65536 (256 KiB) | 2.01 MS/s (95.5%) | 32.7 ms for 31.2 ms |
| 2.1 MS/s | 262144 (1 MiB) | 2.02 MS/s (96.0%) | 130.0 ms for 124.8 ms |

The block size makes no difference, so the per-request round trip is not
the limit. The stream's throughput is. With 4 kernel buffers on the
Pluto's side, the samples that do not fit are dropped there. Inside the
app (`plutosa`) the same: 0-12% lost per 2 s at 2.1 MS/s, 1.9-2.1 MS/s.

## `net` while streaming (eth1, after about 5.6 GB received)

```
received 4123281 frames (5595932670 bytes), sent 4201042 frames (229049069 bytes), 0 not sent
usbnet: read 665411 NTB(s) (4123281 frames, 0 skipped; 0 after another in one read), 0 read(s) failed, ...
usbnet: NTBs read up to 16384 bytes (the adapter offers 16384)
usbnet: bulk IN 0x81: 665411 transfer(s), 0 error(s) ...
usbnet: bulk OUT 0x01: 4201042 transfer(s), 0 error(s) ...
tcp: ... segments 4218678 sent, 4137746 received; sent again: 35 after a timeout, 0 fast; 1 kept after a gap, 0 window probes
```

What these suggest (not checked in the kernel: only the app is ours):

- **The NTBs are half full:** 5,595,932,670 bytes in 665,411 NTBs is 8.4
  KB each (6.2 frames), while the adapter offers 16 KiB. If one bulk IN
  transfer is outstanding at a time, the Pluto's gadget sends what it has
  when the host asks, and the turnaround between transfers would cap the
  rate. Several IN transfers queued at once may lift it.
- **One ACK per segment, each its own USB transfer:** 4.20 M frames sent
  against 4.12 M received, and 4.20 M bulk OUT transfers. A delayed ACK
  (every second segment) would halve them, and NCM can carry several in
  one NTB.
- **The TCP receive window** (if it is small, window / round trip caps a
  single stream) and how fast the stack takes segments off the card.

## To reproduce

From the app's repository on the Mac (it builds on the laptop; the Pluto
on the laptop's USB):

```
make measure HOST=root@192.168.0.195 SSH_OPTS="-i ~/.ssh/ritteros" ARGS="ip:192.168.2.1 2.1 5 4 16384 65536 262144"
```

It sets the Pluto's sample rate (and RF bandwidth) to 2.1 MHz and puts
them back afterwards. 100% in the "Came" column means no gaps.
