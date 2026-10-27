# The NCM link to the PlutoSDR dies under sustained request/response traffic

For the agent working on the RitterOS kernel (`kernel/usbnet.c`, xHCI bulk
transfers). Found on 2026-10-27 while testing a RitterOS app (plutosa, a
spectrum analyzer) against the user's PlutoSDR on the laptop.

## Summary

Under a long run of small TCP request/response pairs to the Pluto's iiod,
the USB NCM card `eth1` first stalls for about 25 s and recovers, then stops
moving frames **in both directions for good**. Nothing is logged. `net`
keeps the link up and keeps counting frames as sent ("0 not sent"), but the
Pluto receives none of them, and RitterOS's receive counter freezes while
the Pluto keeps sending. The Pluto itself is fine, and the other functions
of the same USB device (the CDC ACM serial port) keep working, so the slot
and the controller are alive. Only the NCM function is stuck.

## Setup

| | |
|---|---|
| RitterOS | commit `ac7edbe` (after 64g), installed on the user's laptop (AMD), reached over Wi-Fi at 192.168.0.195 (`wlan0`, MT7921) |
| Controller | xHCI 05:00.3 = `usb0`; the Pluto on `usb0 port 1`, high speed (480 Mbit/s) |
| Pluto | Nano PlutoSDR (Zynq-7010 + AD9363/4), Tezuka v0.3.21, Linux 6.12.77, `usb_ethernet_mode=ncm`, `maxcpus=2` |
| USB device | configuration 'NCM/MSD/ACM/IIOUSBD', 6 interfaces; NCM data presumably interface 1 (bulk IN 0x81, bulk OUT 0x01); mass storage 0x83/0x02; ACM data 0x84/0x03 (+ interrupt 0x85); IIO FFS interface 5 (0x86-0x88 / 0x04-0x06) |
| Card | `eth1`, MAC 00:e0:22:a4:87:79, 192.168.2.10/24 by DHCP; the Pluto is 192.168.2.1 (MAC 00:05:f7:6c:a2:2e, its `usb0`) |
| Boot | the laptop booted 15:44 UTC; the Pluto connected at 15:44:30, disconnected at 15:44:46 (its own boot), came back at 15:44:59 as eth1; storage medium ready 15:45:04 |

## The traffic

libiio's `iio_info` (our own port, github.com/eloyr77/ritteros-libiio)
against iiod on TCP port 30431. It reads every attribute of the Pluto one at
a time over **one TCP connection**: about 1,000 strict request/response
pairs. A request is one line of about 40-80 bytes. Most answers are
10-200 bytes, a few are several KB (the AD9361 gain table). Each read waits
up to 5 s for its answer. The Pluto's Linux NCM gadget can put several
datagrams into one NTB.

The Python spectrum analyzer on the Mac, using the same Pluto over the Mac's
NCM driver, sees occasional multi-second freezes of the link (about 0.25% of
frames lost at 5 MSPS of streaming). So the Pluto's gadget may be flaky too.
But on the Mac the link always recovers. Here it doesn't.

## Timeline

1. **`iio_info` run 0**: 542 lines, complete, but with a stall in the middle.
   Five reads in a row timed out (5 × 5 s), then the late answers arrived
   and the connection carried on. **The link stalled about 25 s and
   recovered.**
2. **plutosa**: connected, read the context XML (PRINT, several KB) and 5
   attributes. All fine. It then stayed connected and idle.
3. **`iio_info` run 1**: took 104 s. Fine up to output line 247, then 246
   reads failed with ETIMEDOUT. **The link died here.**
4. **Runs 2 and 3**: the TCP connect itself timed out (about 244 s each;
   `net` showed "1 SYN sent").
5. **Since then**: `ping 192.168.2.1` from RitterOS loses 100%, and so does
   the Pluto's ping of 192.168.2.10.

Just before it died, RitterOS's TCP counters read: segments 8186 sent, 6461
received; **sent again: 33 after a timeout, 0 fast; 438 kept after a gap**.
The 438 out-of-order arrivals suggest frames were already being lost on
the receive path. The retransmit timeouts would explain the 25 s stall.

## State after it died

RitterOS `net` (unchanged for the 20+ minutes since):

```
eth1   00:e0:22:a4:87:79  link up, 1000 Mbit/s, PlutoSDR (ADALM-PLUTO) (usbnet at usb0 port 1)
       address 192.168.2.10/24 (DHCP, lease 864000 s), gateway none, DNS none
       received 5790 frames (475068 bytes), sent 6367 frames (373015 bytes), 0 not sent
```

- **Receive frozen**: 5790 frames throughout, while the Pluto sent ARP
  requests for 192.168.2.10.
- **Send counted but not delivered**: "sent" rose 6346 → 6358 → 6367 with
  each ping and ARP, "0 not sent". The Pluto's receive counter did not move.

The Pluto's side, read over `/dev/usbserial0` (login root/analog), while
RitterOS pinged it and it pinged RitterOS:

```
usb0  inet addr:192.168.2.1
      RX packets:6352 errors:0 dropped:0 overruns:0 frame:0     <- frozen while RitterOS "sent" more
      TX packets:1399 -> 1402 (its 3 ARP requests for 192.168.2.10)
? (192.168.2.10) at <incomplete> on usb0
up 17 min (never rebooted), load 0.67, iiod running
```

I did not check whether Linux's gadget counts TX packets when it queues
them or when the USB transfer completes. So "1402" proves the Pluto tried
to send, not that the host read the bulk IN.

- **The kernel log has nothing**: `/system/logs/kernel.log` (394 lines)
  ends with the device's attach at 15:45:04 and two Wi-Fi lines at 15:47.
  There are no usbnet, xHCI, cancel, stall or timeout messages and no
  disconnect.
- **The rest of the device works**: after the death, `cat /dev/usbserial0`
  and writes to it (CDC ACM on the same slot) worked both ways. A fresh
  login on the Pluto's console worked.

## Where to look (guesses, not checked)

- **Receive**: the pending bulk IN read on the NCM data endpoint is never
  completed or never posted again. Candidates: an NTB that fails to parse,
  or one larger than the buffer sized from GET_NTB_PARAMETERS (16 KiB), or
  an error or short-packet completion. Any of these could take an exit that
  doesn't requeue the read. The 438 gaps before the death point at frames
  in multi-datagram NTBs being dropped.
- **Send**: frames counted as sent at submission, while the bulk OUT
  transfers never complete. A stopped or halted OUT endpoint is one way:
  after a cancelled transfer (Stop Endpoint, Set TR Dequeue, step 50d's
  timeout path) the endpoint isn't restarted or no doorbell is rung. A
  sender waiting forever on the per-endpoint lock is another.
- **Both at once**: both directions stopped together while the ACM port on
  the same slot works. That points at something the NCM driver's two
  endpoints share: a lock, a thread, or the driver's own state. The slot
  and the controller seem fine.
- **What would help**: `net` details for usbnet like the Realtek driver's:
  whether a bulk IN read is pending, completions per endpoint, the last
  completion code, NTB parse errors, OUT transfers outstanding, cancels.

## How to reproduce

1. The Pluto (Tezuka, NCM) plugged into the laptop; `eth1` up with
   192.168.2.10.
2. On the Mac, in ritteros-libiio: `make`, then copy `build/x86_64/iio_info`
   over (`ssh HOST 'cat > /tmp/iio_info && chmod 755 /tmp/iio_info' <
   build/x86_64/iio_info`).
3. `ssh HOST '/tmp/iio_info -u ip:192.168.2.1'` two or three times in a
   row (each about 1,000 round trips). Here the first run stalled once and
   recovered, and the second run killed the link.
4. Watch `net` (eth1's counters, the `tcp:` line). On the Pluto: `cu
   usbserial0`, root/analog, `ifconfig usb0`.

A smaller loop of TCP round trips (for example repeated `fetch
http://192.168.2.1/` of a small page) might reproduce it without libiio.
Untested. The 3.9 MB dashboard download in step 50d worked, so bulk
streaming alone seems fine. The trigger seems to be many small exchanges.

## Not the kernel's (for context)

The app's library (ritteros-libiio) also had a bug that this exposed. After
a timeout, it took the late answers for the answers to its next requests.
It is being fixed on the app side, so a stalled link no longer garbles
values there. That doesn't change the kernel issue above.
