# lfclk_probe

An `LFCLK` health probe for the `nRF54H20`. 
It counts `LFCLK` against an `HFXO`-derived reference and tells you whether the low frequency clock is running
on the crystal, on an RC source, or not at all.

`clock_control` resolves 20 ppm on this board, but that number is read back out of `BICR`, and a broken crystal
doesn't edit `BICR`. There aren't convenient XOSTATs to look at.

> [!IMPORTANT]
> I couldn't fully include an xtal failure, so that decision tree is still TBD.
> But it does catch an xtal slowdown/degrade.

# Requirements

Hardware

- `nRF54H20 DK`, `PCA10175`

Software

- `nRF Connect SDK v3.4.0` (`v3.4.0-99553055607b`)
- board target `nrf54h20dk/nrf54h20/cpuapp`
- sample memory: | flash / RAM | 58260 B / 15656 B |

# Overview

`rtc130` counts `LFCLK`. `timer130` counts `fll16m`, held in bypass so it is
`HFXO` straight through. `RTC COMPARE` opens and closes a window of known length
in LF ticks, and the HF ticks inside that window give you the ratio.

Sample opens a window a known number of LFCLK ticks wide, count HFCLK ticks
inside it, and compare against what the count should be.

```
  rtc130    counts the 32 kHz you do not trust
  timer130  counts the 16 MHz you do

  window 32768 LF ticks wide, expect 16000000 HF ticks

     got 16000000   clock is right
     got 26000000   clock is slow, by the ratio
     never closed   clock is not advancing
```

What the boot sequence does:

```
  boot
   |
   +-- log what devicetree claims        32768 Hz, BICR says 20 ppm
   +-- condition the reference           ask fll16m for its best, get 30 ppm
   |
   +-- 600 ms   long gate, mean only     dead or just slow to start?
   +-- 5 s      long gate + 64 short     past the LFXO calibration window
   |
   +-- every hour, short gate + 64 short
```

eval decision tree

```
  captured gate
   |
   +-- worked
   |     |
   |     +-- mean off by more than 2000 ppm?  yes -> LF_WRONG_SRC
   |     +-- spread over 150 ppm?             yes -> LF_WRONG_SRC
   |     +-- neither                              -> LF_OK
   |
   +-- failed
         |
         +-- software gate failed too  -> LF_ABSENT  clock not advancing
         +-- software gate fine        -> -EIO       routing broken, clock fine
```

1 bad verdict latches a fault flag, 2 good readings clear it.

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

The gate is the following window: N LF ticks wide, HF ticks counted inside it.

| gate | boundaries taken by |
| --- | --- |
| software | CPU register reads |
| captured | RTC COMPARE firing TIMER CAPTURE over DPPI |

Three decisions based on the gate

| verdict | means | threshold |
| --- | --- | --- |
| `LF_OK` | crystal present and running | within `LF_PPM_REJECT` |
| `LF_WRONG_SRC` | running on an RC source | mean outside `LF_PPM_REJECT` (2000 ppm), or spread outside `LF_PPM_SPREAD_REJECT` (150 ppm) |
| `LF_ABSENT` | `LFCLK` not advancing | no gate closed |


| time after boot | gate | why there |
| --- | --- | --- |
| 600 ms | 1 s, mean only | the board's declared startup budget |
| 5 s | 1 s plus 64 x 32 LF ticks | clears the `LFXO` calibration window |
| hourly (configurable) | 125 ms plus 64 x 32 LF ticks | nothing re-evaluates a satisfied clock request |

The early probe avg. At 20 ms into boot the spread read a 220 HF
tick range against 2 once settled, so judging it that early can be false alarm.
Probe is there to separate dead from slow-starting.

## SoC resources

| resource | what sample uses |
| --- | --- |
| `RTC` | `rtc130`, CC 0 and 1 |
| `TIMER` | `timer130`, CC 0 and 1 |
| DPPI | channels 2 and 3 on `DPPIC130` and `DPPIC133` |
| PPIB | `PPIB130` ch 18 and 19 to `PPIB134` ch 2 and 3 |
| `fll16m` | held for the gate only, released after |
| threads | one, 1024 B stack, `K_PRIO_PREEMPT(10)` |
| pins | N/A |


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
| `-EIO` | event route broken, clock fine | `rtc130` CC contention clearing `EVTEN` |

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

The ratio sits within 8 ppm of good across 14 boots
spread between 19 and 58 ppm against a 150 ppm threshold.

`fll16m` resolves at 30 ppm so the ref contributes more uncertainty. 


## the probe

Same checks as the late probe, short gate. Captured with
`LF_MONITOR_PERIOD_MS` shortened to 5 s.

```
[00:00:11.287,671] <inf> lf_probe: runtime probe: LF_OK at -1 ppm over 4096 LF ticks
[00:00:11.287,680] <inf> lf_probe: runtime spread 64 gates of 32 LF : mean 15625 HF, mad 0 HF (28 ppm), range 5 HF (320 ppm)
```

Four runtime probes read 20-28 ppm of spread on xtal.

## a crystal dragged off frequency while running

Scope probe held against the `XL1` pad of `X2`, the
32.768 kHz crystal, on a board that had been up 28 minutes.

Reasoning: If [this](https://docs.nordicsemi.com/r/bundle/ngl_001/page/gl/ngl_001/hfxo_test_circuit.html) exists for HFXO, maybe exists for LFXO.

<img width="420" height="395" alt="image" src="https://github.com/user-attachments/assets/23a90e9e-e3aa-4cdc-ac75-c13d2b6ad1f9" />

<img width="442" height="397" alt="image" src="https://github.com/user-attachments/assets/acd43bf5-c9f7-4fde-8bcc-81f77b3e92ef" />


```
00:28:31  runtime probe: LF_OK at -2 ppm over 4096 LF ticks
00:28:35  runtime probe: LF_WRONG_SRC at -335630 ppm over 4096 LF ticks
00:28:35  fault flag latched: LF_WRONG_SRC
00:28:40  fault flag still latched, 1 of 2 good readings
00:28:46  fault flag cleared after 2 good readings
00:28:51  runtime probe: LF_OK at -16 ppm over 4096 LF ticks
00:28:56  runtime probe: LF_WRONG_SRC at -629021 ppm over 4096 LF ticks
00:28:56  fault flag latched: LF_WRONG_SRC
00:29:01  runtime probe: LF_WRONG_SRC at -60842 ppm over 4096 LF ticks
00:29:06  runtime probe: LF_OK at 0 ppm over 4096 LF ticks
00:29:06  fault flag still latched, 1 of 2 good readings
00:29:11  fault flag cleared after 2 good readings
```

Gate counts HF ticks across a fixed number of LF ticks, so a longer window means a slower clock

| reading | implied LFCLK | share of nominal |
| --- | --- | --- |
| -335630 ppm | 24534 Hz | 75% |
| -629021 ppm | 20115 Hz | 61% |
| -60842 ppm | 30889 Hz | 94% |

The crystal did not stop and it did not hand over to `LFRC`. It stayed in the
loop and got pulled up to 12 kHz off resonance by probe capacitance, and the
drag scaled with how hard the pad was contacted. Detected within one probe
interval, 5 s here.

> `LF_PPM_REJECT` caught this, not the spread. At -629021 ppm it is 300x past the
> 2000 ppm threshold. The spread check exists for the calibrated RC case where
> the mean is useless; this is the coarse detector earning its place.

> No spread line appears on those two probes. `lf_verdict_get()` returns
> `LF_WRONG_SRC` from the mean and skips the spread measurement, so the missing
> line is correct behaviour rather than a dropped log.

`XL1` on `X2` is a small pad and wants deliberate contact, not a light touch.

## an LF counter that is not advancing

`counter_stop(rtc130)`, so the counter does not tick. To a gate that is
indistinguishable from a dead `LFCLK`: the value never changes.

```
[00:00:00.020,446] <wrn> lf_probe: SCRATCH: rtc130 stopped, LF counter will not advance
[00:00:06.622,022] <wrn> lf_probe: early probe: LF_ABSENT, no gate closed
[00:00:06.622,033] <err> lf_probe: fault flag latched: LF_ABSENT
[00:00:12.624,996] <wrn> lf_probe: late probe: LF_ABSENT, no gate closed
```

The early probe reports at 6.62 s. It tried the captured gate, timed out at 3 s,
fell back to the software gate, and timed out again. `LF_ABSENT` costs about 6 s per probe and needs both gates to
fail, which is the discriminator doing its job.

The late probe ran back to back at 12.62 s instead of being skipped, because its
absolute 5 s deadline had already passed when the early probe finished.

> A stopped `LFCLK` cannot be produced from inside. It stops the CPU and
> no application code runs, so that one is recognition only. `LF_ABSENT` exists to
> report a non-advancing LF counter as a verdict rather than hanging or printing a
> number it never measured.

## a broken event route

`-EIO` rather than a verdict. Produced by giving a second consumer a `rtc130` CC
channel: the Zephyr counter driver manages `EVTEN` for its own alarms and cleared
the `COMPARE` enables the captured gate needs.

```
capture failed (err -5) but the software gate read -12 ppm: DPPI route fault, not a clock fault
early probe: undetermined (err -5)
```

The captured gate died, the software gate read a healthy clock, so the probe
reports the route rather than condemning the crystal. Cause here was register
contention rather than a broken bridge, and the branch behaves the same either
way.

> Do not share `rtc130` or `timer130` with anything else. They are the
> measurement pair. `rtc131` and `timer131` are free and run on the same clocks.

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

`LF_WRONG_SRC` on a clock whose mean error is 1 ppm. A calibrated `LFRC` on this
DK reads better than the crystal in absolute terms and clears `LF_PPM_REJECT` by
three decades. The spread is what condemns it, and it feeds the verdict and the fault
latch rather than only printing a warning.

Both sides, one DK, room temperature, spread taken inside the probe:

| condition | mean at late probe | spread MAD | verdict |
| --- | --- | --- | --- |
| crystal, 14 boots | -6 to -7 ppm | 19-58 ppm | `LF_OK`, nothing latched |
| LFRC, 9 boots | -33 to +56 ppm | 248-622 ppm | `LF_WRONG_SRC`, latched |

The mean overlaps completely. MAD separates by 4.3x, which is where the 150 ppm
threshold comes from: 2.6x above the worst crystal reading, 1.65x below the
quietest RC one.

> Measure the spread inside the probe, right after the gate. An earlier version
> ran it from a fresh `lf_ref_acquire()` and read 76-110 ppm on the same
> crystal, because it was measuring the HFXO ramp along with the LF source.

## proof the measurement is real

The number that matters is not any single reading, it's that two gate lengths
differing by 8x agree. Five repeats each, `fll16m` held at 30 ppm.

| gate | mean | spread |
| --- | --- | --- |
| software, 32768 LF (1 s) | +6 ppm | 24 ppm |
| software, 4096 LF (125 ms) | +71 to +84 ppm | 26-30 ppm |
| captured, 32768 LF | -7 ppm | 2 HF ticks, 0.125 ppm |
| captured, 4096 LF | -7 ppm | 1 HF tick, 0.5 ppm |

The captured gates agree to the digit at both lengths, so the residual is a real
frequency offset. The software gate does not, because it carries a fixed
read-pair latency:

| gate | expected HF | mean actual | shortfall |
| --- | --- | --- | --- |
| 32768 LF | 16000000 | 15999901 | -99 ticks, 6.2 us |
| 4096 LF | 2000000 | 1999858 | -142 ticks, 8.9 us |

Same absolute shortfall either way. A fixed time error scales as 1/gate-length
in ppm, so 99 ticks of 16e6 is 6 ppm and 142 of 2e6 is 71 ppm. The DPPI gate
removes that offset.

> The software rows and the captured rows come from different commits, so these
> are two measurement campaigns. Read-pair latency accounts for about 6 of the
> 13 ppm between the two long-gate means. The remainder is unexplained and sits
> inside the reference's own 30 ppm.

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
| `lf_monitor_thread()` | early probe, late probe, then the hardcoded time based poll |

`lf_verdict_get()` tries the capture first because it's the
accurate one, and only returns `LF_ABSENT` when the software gate fails too. A
misconfigured DPPI route times out exactly like a dead clock, so if the capture
fails while the software gate still reads a healthy clock you get `-EIO` and the
software reading in the log.


>[!NOTE]
> Most of what follows cost a bench session to find. None of it is obvious from the
headers. These are just jostled into this readme as extra content, not core to the app.
>
>The following notes are overly verbose.

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

## nrf_clock_is_running() isn't available here

Coming from a 52, the first reach is `nrf_clock_is_running()` in
`<ncs>/modules/hal/nordic/nrfx/hal/nrf_clock.h:723`. It doesn't compile on this
part, and wouldn't answer the question if it did.

It wants an `NRF_CLOCK_Type *`. There is no `CLOCK` peripheral on nRF54H20 —
`NRF_CLOCK` gets zero hits in
`<ncs>/modules/hal/nordic/nrfx/bsp/stable/mdk/nrf54h20_global.h`, against 18 other
parts in that directory that define it (52-series, `nRF54L`, `nRF7120`). Zephyr
gates the include the same way, `#if defined(CONFIG_CLOCK_CONTROL_NRF)` at
`<ncs>/zephyr/include/zephyr/drivers/clock_control/nrf_clock_control.h:18`, which
is the 52/53 path and not set for this build. H20's `LFCLK` lives behind SCFW, so
you go through `nrfs_clock` and the `nordic,nrf-lfclk` driver.

On the parts where it does exist, its `p_clk_src` out-param reports which source
was selected and whether the domain started. It latches a request, same as the
two above. The `nRF54L15` PS spells that out for `LFCLK.STAT.SRC`: it holds
whatever `SRCCOPY` held when `LFCLKSTARTED` triggered.

That reads `SRC = XTAL` while the crystal sits at 61% of nominal, which is the
fault at
[a crystal dragged off frequency while running](#a-crystal-dragged-off-frequency-while-running).
Every register answer shares the flaw: it reports a decision software made, and a
dying crystal doesn't participate in decisions. Counting `LFCLK` against `HFXO`
asks nobody. Two oscillators, and the ratio can't agree with itself unless both
are right.

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

Declare `lfosc.source: LFRC` with autocalibration on. A board built without a
32 kHz crystal is configured that way, so the path is supported and it gives you
the hard case: a calibrated RC sitting near nominal. `EXT_SQUARE` rests on an
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
with 20-30 ppm of spread afterwards across six boots.

## EXT_SQUARE does not stop a fitted crystal

The handoff uses `BICR` `lfosc.lfxo.mode: EXT_SQUARE` with nothing driving XL1 as
its fault injection, on the reasoning that the amplifier gets bypassed. Measured
on nRF54H20 with the crystal still fitted:

| config | mean | spread MAD |
| --- | --- | --- |
| stock `CRYSTAL` | -2 to -9 ppm | 14-35 ppm |
| `EXT_SQUARE`, load caps off | +7 to +8 ppm | 50-59 ppm |

The oscillator kept running and five runtime probes returned `LF_OK`. Dropping
`builtInLoadCapacitors` removes the internal load capacitance, which pulls the
crystal about 12 ppm and doubles its spread.

For a non-crystal `LFCLK` use `lfosc.source: LFRC`. `LF_PPM_SPREAD_REJECT` is
calibrated against that.

> `bicrgen.py` reads `builtInLoadCapacitors` whatever the mode, while the schema
> only requires it under `CRYSTAL`. An `EXT_SQUARE` config without it dies on a
> KeyError.

## reading the log

> `mad 0 HF (30 ppm)` looks self-contradictory and isn't. One HF tick in a 32
> tick gate is already 64 ppm, so per-sample deviation truncates to 0 or 1 ticks
> on a healthy board. The ppm figure comes from the summed deviation before
> dividing, which keeps sub-tick resolution.

> On a crystal that figure is the instrument, not the clock. `mad_ppm` happens to
> equal the summed deviation in ticks exactly, because 15625 x 64 = 1e6, so a
> reading of 30 means 30 of 64 samples landed one tick off the mean. That counts
> how often the capture straddles a tick boundary: LF-to-HF phase, PPIB latency,
> quantization. The threshold works because an RC source deviates by 4 to 10
> ticks per sample, well clear of that floor, but do not read 30 ppm as the
> crystal's jitter.

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
| 20 ms | 3-7 HF ticks | 41 and 220 HF ticks |
| 5 s | 1 HF tick, 76-110 ppm | 9-19 HF ticks |
| 12 s | 0 HF ticks, 17-20 ppm | 2-3 HF ticks |

Taken inside the probe on this build, straight after the 1 s gate, the same
crystal reads 19-58 ppm and 2 to 5 ticks at the 5 s probe. The 220 tick range
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

A crystal that degrades while running does **not** trigger a fallback to `LFRC`
on this part. Measured at 61% of nominal with the system still running on it, no
handover, no complaint from anything but this probe. The handoff left that
open.

What is still open is a crystal that stops completely. Probe loading was not
enough to kill oscillation, only to drag it, so that branch is untested. If it
does stop, the monitor thread cannot report it: `k_sleep()` parks on the system
timer and GRTC SYSCOUNTER falls back to `LFCLK` while asleep, so the CPU never
wakes. Silence on the console is the signature.

The spread check catches a calibrated `LFRC`. It is measured from both sides, on
one DK at room temperature. The crystal side threw one 58 ppm
boot against a 19 to 29 ppm cluster, so re-check the margins over temperature and
on a second board before trusting them.

The software gate cannot measure spread, so a probe that has to fall back to it
returns `-EIO` rather than a verdict. There is no path where a mean-only reading
is reported as `LF_OK`. That `-EIO` branch has not been observed on hardware.

Anything better than about 30 ppm is beyond this rig, since `fll16m` resolves at
`hfxo`'s `accuracy-ppm`.
