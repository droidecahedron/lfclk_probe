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
