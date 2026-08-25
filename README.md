# lfclk_probe

An `LFCLK` health probe for the `nRF54H20`. It counts `LFCLK` against an
`HFXO`-derived reference and tells you whether the low frequency clock is running
on the crystal, on an RC source, or not at all.

Nothing in the clock stack will tell you this. `clock_control` resolves 20 ppm on
this board, but that number is read back out of `BICR`, and a broken crystal
doesn't edit `BICR`. So you measure.

# Requirements

Hardware

- `nRF54H20 DK`, `PCA10175`
- nothing else. No pins, no wiring, no external parts

Software

- `nRF Connect SDK v3.4.0` (`v3.4.0-99553055607b`)
- Zephyr `4.4.0` (`v4.4.0-bf801e4e3d19`)
- board target `nrf54h20dk/nrf54h20/cpuapp`

# Overview

`rtc130` counts `LFCLK`. `timer130` counts `fll16m`, held in bypass so it is
`HFXO` straight through. `RTC COMPARE` opens and closes a window of known length
in LF ticks, and the HF ticks inside that window give you the ratio.

The two counters sit on different peripheral buses, so the event crosses a PPIB
bridge:

```
rtc130 EVENTS_COMPARE[0,1]        bus 0x5f92_0000
   |
   +-- DPPIC130 ch 2,3
         |
         +-- PPIB130 ch 18,19  ==>  PPIB134 ch 2,3     bridge, set up by
               |                                       IronSide SE from UICR
               +-- DPPIC133 ch 2,3
                     |
                     +-- timer130 TASKS_CAPTURE[0,1]   bus 0x5f9a_0000
```

The gate is that window: N LF ticks wide, HF ticks counted inside it.

| gate | boundaries taken by |
| --- | --- |
| software | CPU register reads |
| captured | RTC COMPARE firing TIMER CAPTURE over DPPI |

Three verdicts come out:

| verdict | means | threshold |
| --- | --- | --- |
| `LF_OK` | crystal present and running | within `LF_PPM_REJECT` |
| `LF_WRONG_SRC` | running on an RC source | mean outside `LF_PPM_REJECT` (2000 ppm), or spread outside `LF_PPM_SPREAD_REJECT` (150 ppm) |
| `LF_ABSENT` | `LFCLK` not advancing | no gate closed |

Schedule, all measured from boot:

| when | gate | why there |
| --- | --- | --- |
| 600 ms | 1 s, mean only | the board's declared startup budget |
| 5 s | 1 s plus 64 x 32 LF ticks | clears the `LFXO` calibration window |
| hourly | 125 ms plus 64 x 32 LF ticks | nothing re-evaluates a satisfied clock request |

The early probe judges the mean only. At 20 ms into boot the spread read a 220 HF
tick range against 2 once settled, so judging it that early condemns good
hardware. That probe is there to separate dead from slow-starting, which the mean
already does.

## SoC resources

| resource | what it takes |
| --- | --- |
| `RTC` | `rtc130`, CC 0 and 1 |
| `TIMER` | `timer130`, CC 0 and 1 |
| DPPI | channels 2 and 3 on `DPPIC130` and `DPPIC133` |
| PPIB | `PPIB130` ch 18 and 19 to `PPIB134` ch 2 and 3 |
| `fll16m` | held for the gate only, released after |
| threads | one, 1024 B stack, `K_PRIO_PREEMPT(10)` |
| pins | none |
| flash / RAM | 58260 B / 15656 B |

Leaves you `rtc131`, `timer131` through `timer137`, and DPPI channels 0, 1, and
4 through 7.

# Building and Running

```
west build -b nrf54h20dk/nrf54h20/cpuapp
west flash
```

Console is `uart136` on VCOM0, 115200 baud. On the DK that's `/dev/ttyACM0`.

# Example Output

Every verdict below came off the shipping build under a real hardware condition.
What produced each one:

| verdict | condition | how it was produced |
| --- | --- | --- |
| `LF_OK` | crystal running | stock `BICR`, 14 boots |
| `LF_WRONG_SRC` | RC source | `BICR` `lfosc.source: LFRC`, 9 boots |
| `LF_ABSENT` | LF counter not advancing | `counter_stop(rtc130)` |
| `-EIO` | DPPI route broken, clock fine | `nrf_rtc_event_enable()` omitted |

## a healthy board

Stock `BICR`, crystal fitted and working.

```
*** Booting nRF Connect SDK v3.4.0-99553055607b ***
*** Using Zephyr OS v4.4.0-bf801e4e3d19 ***
[00:00:00.020,340] <inf> lf_probe: lf_probe on nrf54h20dk@0.9.0/nrf54h20/cpuapp
[00:00:00.020,345] <inf> lf_probe: lfclk nominal      : 32768 Hz
[00:00:00.020,348] <inf> lf_probe: lfrc   declared    : 500 ppm, 200 us startup
[00:00:00.020,350] <inf> lf_probe: lflprc declared    : 1000 ppm, 200 us startup
[00:00:00.020,358] <inf> lf_probe: lf counter rtc@928000 : 32768 Hz
[00:00:00.020,364] <inf> lf_probe: hf counter timer@9a2000 : 16000000 Hz
[00:00:00.020,374] <inf> lf_probe: fll16m resolved    : 16000000 Hz, 30 ppm, precision 0, 850 us startup
[00:00:01.621,079] <inf> lf_probe: early probe: LF_OK at -8 ppm over 32768 LF ticks
[00:00:06.091,353] <inf> lf_probe: late probe: LF_OK at -7 ppm over 32768 LF ticks
[00:00:06.091,362] <inf> lf_probe: late spread 64 gates of 32 LF : mean 15625 HF, mad 0 HF (30 ppm), range 3 HF (192 ppm)
```

This board's `LFXO` runs 6 to 8 ppm slow, inside its 20 ppm `BICR` claim. Across
14 boots the spread stayed between 19 and 58 ppm against a 150 ppm threshold.

## an LF counter that is not advancing

`counter_stop(rtc130)`, so the counter genuinely does not tick. To a gate that is
indistinguishable from a dead `LFCLK`: the value never changes.

```
[00:00:00.020,446] <wrn> lf_probe: SCRATCH: rtc130 stopped, LF counter will not advance
[00:00:06.622,022] <wrn> lf_probe: early probe: LF_ABSENT, no gate closed
[00:00:06.622,033] <err> lf_probe: fault flag latched: LF_ABSENT
[00:00:12.624,996] <wrn> lf_probe: late probe: LF_ABSENT, no gate closed
```

Read the timestamps. The early probe reports at 6.62 s rather than 1.62 s because
it tried the captured gate, timed out at 3 s, fell back to the software gate, and
timed out again. `LF_ABSENT` costs about 6 s per probe and needs both gates to
fail, which is the discriminator doing its job.

The late probe ran back to back at 12.62 s instead of being skipped, because its
absolute 5 s deadline had already passed when the early probe finished.

> A genuinely stopped `LFCLK` cannot be produced from inside. It stops the CPU and
> no application code runs, so that one is recognition only. `LF_ABSENT` exists to
> report a non-advancing LF counter as a verdict rather than hanging or printing a
> number it never measured.

## the fault latch clearing

The RC board never recovers, so clearing needs a fault that goes away.
`LF_PPM_REJECT` set to 3, between the long gate's -6 ppm and the short gate's
0 ppm, so the boot probes fail and the runtime probes pass.
`LF_MONITOR_PERIOD_MS` shortened to 4 s to make it watchable.

```
[00:00:01.620,866] <inf> lf_probe: early probe: LF_WRONG_SRC at -6 ppm over 32768 LF ticks
[00:00:01.620,876] <err> lf_probe: fault flag latched: LF_WRONG_SRC
[00:00:06.020,793] <inf> lf_probe: late probe: LF_WRONG_SRC at -6 ppm over 32768 LF ticks
[00:00:10.218,240] <inf> lf_probe: runtime probe: LF_OK at 0 ppm over 4096 LF ticks
[00:00:10.218,277] <wrn> lf_probe: fault flag still latched, 1 of 2 good readings
[00:00:14.344,401] <inf> lf_probe: runtime probe: LF_OK at 0 ppm over 4096 LF ticks
[00:00:14.344,439] <inf> lf_probe: fault flag cleared after 2 good readings
```

The second bad reading does not re-log. Two consecutive good readings are needed
to clear, so a caller sampling less often than the probe still sees that
something went wrong.

## a board running on LFRC

`BICR` edited to declare `source: LFRC`, so arbitration has no crystal to pick
and `LFCLK` runs on the internal RC with autocalibration on.

```
[00:00:01.620,834] <inf> lf_probe: early probe: LF_OK at 11 ppm over 32768 LF ticks
[00:00:06.091,169] <inf> lf_probe: late probe: LF_WRONG_SRC at 1 ppm over 32768 LF ticks
[00:00:06.091,179] <inf> lf_probe: late spread 64 gates of 32 LF : mean 15633 HF, mad 3 HF (248 ppm), range 28 HF (1792 ppm)
[00:00:06.091,184] <wrn> lf_probe: late spread 248 ppm exceeds 150 ppm: RC source, not a crystal
[00:00:06.091,191] <err> lf_probe: fault flag latched: LF_WRONG_SRC
```

`LF_WRONG_SRC` on a clock whose mean error is 1 ppm. That is the whole point: a
calibrated `LFRC` on this DK reads better than the crystal does in absolute
terms, so it clears `LF_PPM_REJECT` at 2000 ppm and would clear a 50 ppm noise
floor. The spread is what condemns it, and it feeds the verdict and the fault
latch rather than only printing a warning.

Both sides, one DK, room temperature, spread taken inside the probe:

| condition | mean at late probe | spread MAD | verdict |
| --- | --- | --- | --- |
| crystal, 14 boots | -6 to -7 ppm | 19 to 58 ppm | `LF_OK`, nothing latched |
| LFRC, 9 boots | -33 to +56 ppm | 248 to 622 ppm | `LF_WRONG_SRC`, latched |

The mean overlaps completely. MAD separates by 4.3x, which is where the 150 ppm
threshold comes from: 2.6x above the worst crystal reading, 1.65x below the
quietest RC one.

> Measure the spread inside the probe, right after the gate. An earlier version
> ran it from a fresh `lf_ref_acquire()` and read 76 to 110 ppm on the same
> crystal, because it was measuring the HFXO ramp along with the LF source.

## proof the measurement is real

The number that matters is not any single reading, it's that two gate lengths
differing by 8x agree. Five repeats each, `fll16m` held at 30 ppm.

| gate | mean | spread |
| --- | --- | --- |
| software, 32768 LF (1 s) | +6 ppm | 24 ppm |
| software, 4096 LF (125 ms) | +71 to +84 ppm | 26 to 30 ppm |
| captured, 32768 LF | -7 ppm | 2 HF ticks, 0.125 ppm |
| captured, 4096 LF | -7 ppm | 1 HF tick, 0.5 ppm |

The captured gates agree to the digit at both lengths, which is what tells you
the residual is a real frequency offset. The software gate doesn't, because it
carries a fixed read-pair latency:

| gate | expected HF | mean actual | shortfall |
| --- | --- | --- | --- |
| 32768 LF | 16000000 | 15999901 | -99 ticks, 6.2 us |
| 4096 LF | 2000000 | 1999858 | -142 ticks, 8.9 us |

Same absolute shortfall either way. A fixed time error scales as 1/gate-length
in ppm, so 99 ticks of 16e6 is 6 ppm and 142 of 2e6 is 71 ppm. That is the whole
argument for the DPPI gate.

# Software Description

| file | what's in it |
| --- | --- |
| `src/lf_probe.h` | every tunable with its origin, `enum lf_verdict`, `struct lf_spread` |
| `src/main.c` | measurement, verdict, spread check, scheduling |
| `boards/nrf54h20dk_nrf54h20_cpuapp.overlay` | counters, `fll16m`, DPPI channels, PPIB route |

| function | job |
| --- | --- |
| `lf_ref_check()` | boot config oracle. Resolved accuracy against what the board declares |
| `lf_ref_acquire()` / `lf_ref_release()` | hold `fll16m` at its best accuracy for the gate |
| `lf_gate_measure()` | software gate. Fallback, and the discriminator for DPPI route faults |
| `lf_capture_measure()` | DPPI-captured gate |
| `lf_verdict_get()` | capture first, software gate second, then a verdict |
| `lf_spread_measure()` | cycle-to-cycle deviation over `LF_JITTER_SAMPLES` short gates |
| `lf_probe_run()` | one scheduled probe plus the fault latch |
| `lf_monitor_thread()` | early probe, late probe, spread baseline, hourly poll |

Both gates ship. `lf_verdict_get()` tries the capture first because it's the
accurate one, and only returns `LF_ABSENT` when the software gate fails too. A
misconfigured DPPI route times out exactly like a dead clock, so if the capture
fails while the software gate still reads a healthy clock you get `-EIO` and the
software reading in the log.

# Notes

Most of what follows cost a bench session to find. None of it is obvious from the
headers.

## the clock API tells you what was asked for

Resolved accuracy from `clock_control` is `BICR` read back. `fll16m` resolves to
30 ppm on this DK because that's `hfxo`'s `accuracy-ppm` in
`nrf54h20dk_nrf54h20-common.dtsi:12`, not because anything measured it.
`lf_ref_check()` uses that on purpose as a config oracle: it catches a wrong
`BICR` or a wrong board variant, and says nothing about a live crystal.

The `nrfs` clock service is the same. `nrfs_clock_lfclk_src_set()` with
`NRFS_CLOCK_SRC_LFCLK_LFRC` returned `NRFS_CLOCK_EVT_APPLIED` reporting
`src = 4`, and the clock kept measuring -6 ppm. An `LFRC` can't be 6 ppm; its own
devicetree node declares 500.

```
UNVERIFIED: whether SCFW rejected the downgrade internally, or applied it to a
per-domain view while arbitration held the physical source at LFXO. The event
carries no effective-source field to tell these apart. What is measured is that
EVT_APPLIED with src=LFRC does not imply an RC clock.
```

`LFCLK` is shared and SCFW serves the tightest requirement across the radio core,
the secure domain, and `cpuapp`'s own tick via GRTC, so an application asking for
a worse clock gets subsumed. That also means you can't inject a fault this way.

## RTC compare events need EVTEN

> An nRF RTC won't set `EVENTS_COMPARE[n]` on match unless the matching `EVTEN`
> bit is set. TIMER does. The Zephyr counter driver sets `EVTEN` only when it
> hands out an alarm, and this sets none, so without `nrf_rtc_event_enable()` the
> compare matches, nothing publishes, and the captured gate reports `-ETIMEDOUT`
> looking exactly like a dead clock.

That one cost a flash cycle, and it's why `LF_ABSENT` requires both gates to
fail.

## don't give the ppib nodes a status

> PPIBs are secure domain owned. Setting `status` on them emits
> `SPU_PERIPH_PERM` writes that `periphconf-check` rejects as
> `UNRECOGNIZED_REGISTER`, which stops the device booting. Declare
> `sink-channels` on the leaf `DPPIC` and the periphconf generator emits both
> halves of the bridge for you.

The build catches this before you can flash, which is the only reason it was
cheap to find. Check `build/lfclk_probe/zephyr/periphconf_entries_generated.c` to
see what came out.

## picking the timer and the channels

`cpuapp` owns no core-local `TIMER`, `RTC`, or `DPPIC`. `nrf54h20_cpuapp.dtsi`
deletes `cpurad_peripherals`, and `cpuapp_peripherals` holds only the local
HSFLL, IPCT, two watchdogs, resetinfo, and the 802.15.4 node.

| candidate | why not |
| --- | --- |
| `timer020` to `timer022` | `cpurad` local, deleted from this build |
| `timer120`, `timer121` | clocked from DVFS-scaled `hsfll120`, IRQs go to FLPR by default |
| `timer130` to `timer137` | fine. `fll16m` at 16 MHz, `prescaler = 0` |

MPSL reserves `DPPIC130` channel 0 (`MPSL_RESERVED_DPPI_SINK_CHANNELS` in
`nrf/subsys/mpsl/init/mpsl_init.c`), so this uses 2 and 3 to stay out of a radio
application's way. The channel index is the same on both sides of the bridge and
the application enables both, for the reason given at `mpsl_init.c:441`:

> Secure domain no longer enables DPPI channels for local domains, MPSL now has
> to enable the ones it uses.

The RTC and TIMER CC channels sit behind the Zephyr counter drivers, which hand
those same channels out for alarms. This sets no alarms, and `rtc130` has
`cc-num = 4` against `timer130`'s `cc-num = 6`, so nothing gets squeezed out.

No `nordic,nrf-timer` node carries `clock-frequency`. The rate comes from the
clock parent as `NRF_PERIPH_GET_FREQUENCY(node) / BIT(prescaler)`, which is what
`counter_nrfx_timer.c:447` does and what `counter_get_frequency()` returns.

## injecting a non-crystal LFCLK

Needed to calibrate the spread threshold, and a `BICR` edit is the only way. A
runtime request gets subsumed by arbitration, as above.

Declare `lfosc.source: LFRC` with autocalibration on. That is how a board built
without a 32 kHz crystal is configured, so it is a supported path and it gives
you the hard case: a calibrated RC sitting near nominal. `EXT_SQUARE` rests on an
assumption about the crystal amplifier that the nRF54H20 PS was not available to
confirm.

> Do not generate the image straight from `bicr.json`. `LFOSC.LFXOCAL` at offset
> `0x1C` holds `0x00000000` on this DK and no generated image reproduces it,
> because it is calibration state rather than board config. Generating would
> rewrite it as a side effect of a change to `lfosc.source`. Patch the bytes you
> read off the device instead, and recompute the CRC with
> `bicrgen.crc32_bzip2_input_reversed()`.

Changing `source` from `LFXO` to `LFRC` moves exactly two registers plus the CRC:

| offset | register | LFXO | LFRC |
| --- | --- | --- | --- |
| `0x18` | `LFOSC.LFXOCONFIG` | `870F58F2` | `EFFFFFFF` |
| `0x20` | `LFOSC.LFRCAUTOCALCONFIG` | `FFFFFFFF` | `90C2E27F` |
| `0x4C` | CRC | unprogrammed on this DK | recomputed |

Validate before programming by feeding the patched hex back through
`bicrgen.py -i patched.hex -o decoded.json`. It verifies the CRC and shows you
that `power`, `ioPortPower`, `ioPortImpedance`, and `hfxo` came through
unchanged. If the CRC were wrong it refuses.

Program with `chip_erase_mode=ERASE_NONE`, pin reset, then read back `0x50`
bytes and diff against what you wrote. MRAM runs in Direct Write mode by default
under IronSide SE, so rewriting in place needs no erase and bit direction does
not matter.

> `BICR` is not erased by `ERASEALL`. A saved hex is the only way back, so take
> it first. `../bicr_backup/IFYOUBRICKEDUSETHISBICR.txt` has the recovery path.

Restore was verified byte-identical to the backup, and the crystal read -6 ppm
with 20 to 30 ppm of spread afterwards across six boots.

## reading the log

> `mad 0 HF (30 ppm)` looks self-contradictory and isn't. One HF tick in a 32
> tick gate is already 64 ppm, so per-sample deviation truncates to 0 or 1 ticks
> on a healthy board. The ppm figure comes from the summed deviation before
> dividing, which keeps sub-tick resolution.

> `lf_hz` only ever prints 32767 to 32770, because 1 Hz at 32768 Hz is 30 ppm.
> Read the ppm column.

## timing traps

The two boot deadlines are absolute (`K_TIMEOUT_ABS_MS`). A relative sleep runs
from the end of the previous probe, which put the late probe at 6 s instead of 5
and missed the window it was sized for.

The spread cannot be judged early. The mean settles fast and the spread does
not, which is why the 600 ms probe checks the mean alone.

Measured with a standalone spread run that acquired the reference itself, so
these figures carry an HFXO ramp the in-probe version does not:

| measured at | mad | range |
| --- | --- | --- |
| 20 ms | 3 to 7 HF ticks | 41 and 220 HF ticks |
| 5 s | 1 HF tick, 76 to 110 ppm | 9 to 19 HF ticks |
| 12 s | 0 HF ticks, 17 to 20 ppm | 2 to 3 HF ticks |

Taken inside the probe on this build, straight after the 1 s gate, the same
crystal reads 19 to 58 ppm and 2 to 5 ticks at the 5 s probe. The 220 tick range
at 20 ms is what the early probe would be judging.

```
UNVERIFIED: why the spread keeps tightening after the 5 s calibration window.
LFXO amplitude still stabilising is the obvious candidate but the nRF54H20 PS was
not available to confirm. Settle it by calling lf_spread_report() on the periodic
loop for several minutes and finding where it asymptotes.
```

`CONFIG_LOG_MODE_DEFERRED=y` is required. The software gate has log calls near it
and immediate mode puts UART driver time straight into the ppm figure. It isn't
sufficient either: the first long software gate of every boot was consistently
the worst of five, because the log backend is still draining.

## don't

- Use GRTC as the LF side. Its SYSCOUNTER runs from the 16 MHz clock while active
  and only falls back to `LFCLK` in sleep, so the reading is contaminated by the
  reference.
- Rely on the watchdog as a backstop. Every `wdt` node is `clocks = <&lfclk>`.
- Declare better than 20 ppm to MPSL. Known issue DRGN-23693: the sleep clock
  accuracy sent to the peer is wrong below 20 ppm.
- Enable `fll16m` closed loop. `FLL16M_MODE_CLOSED_LOOP` is annotated at
  `clock_control_nrf_fll16m.c:24` as "DO NOT IMPLEMENT, CAN CAUSE HARDWARE BUG".
- Pass a nonzero `precision`. `fll16m_resolve_spec_to_idx()` returns `-EINVAL`
  before it looks at accuracy.
- Reach for `onoff_client` and `sys_notify`. `nrf_clock_control_request_sync()`
  exists, declared at `nrf_clock_control.h:296`.

## what this can't detect

A completely stopped `LFCLK` is invisible from inside. The monitor thread needs
`LFCLK` to schedule itself, since `k_sleep()` parks on the system timer and GRTC
SYSCOUNTER falls back to `LFCLK` while asleep. Stop it outright and the CPU never
wakes. You recognise that one, you don't detect it.

The spread check is what catches a calibrated `LFRC`, and it is measured from
both sides, but on one DK at room temperature. The crystal side threw one 58 ppm
boot against a 19 to 29 ppm cluster, so re-check the margins over temperature and
on a second board before trusting them.

The software gate cannot measure spread, so a probe that falls back to it judges
the mean alone and will pass a calibrated RC. That only happens when the DPPI
route is broken, which is reported separately as `-EIO`.

Anything better than about 30 ppm is beyond this rig, since `fll16m` resolves at
`hfxo`'s `accuracy-ppm`.
