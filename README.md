# lfclk_probe

Measures whether `LFCLK` is actually running on the crystal, by counting it
against an `HFXO`-derived reference.

# Hardware

`nRF54H20 DK`, `PCA10175`, board target `nrf54h20dk/nrf54h20/cpuapp`.

> Measured on `NRF54H20_xxAA_ENGC`. Any nRF54H20 should work. Other parts need
> the fabric rework in the resource budget below, because the RTC and TIMER
> bus split is specific to this SoC.

# Software

`nRF Connect SDK v3.4.0` (`v3.4.0-99553055607b`), Zephyr `4.4.0`
(`v4.4.0-bf801e4e3d19`).

```
west build -b nrf54h20dk/nrf54h20/cpuapp
west flash
```

# Background

Nothing in the software stack reports a dead or absent `LFXO`.

`clock_control` resolves an accuracy request to 20 ppm on this board, but that
number originates in `BICR`. A broken crystal does not edit `BICR`, so the API
reports 20 ppm forever while the hardware may be running on `LFRC`.

The obvious fixes do not work:

- **Wait for an event.** There is no LF equivalent of the `HFXO` quality events
  (`EVENTS_XOTUNEERROR`, `EVENTS_XOTUNEFAILED`). No part in the family has one.
- **Read a status latch.** Event registers record transitions. A crystal that
  dies in steady state produces no transition, and nothing re-runs the
  arbitration, so every register reads its day-one value forever.
- **Ask the clock service.** See the two findings in Notes. Both the
  `clock_control` accuracy and the `nrfs` clock service source field report what
  was requested or declared, not what is oscillating.

So: count `LFCLK` against an `HFXO`-derived reference. That reads steady state,
is independent of who owns the oscillator, and works at boot and at month six.

`HFXO` is trustworthy for free. If the 32 MHz crystal were bad, MPSL asserts
during init, so a working radio is proof of the reference.

# Overview

| file | what is in it |
| --- | --- |
| `src/lf_probe.h` | every tunable, each with its origin. `enum lf_verdict`, `struct lf_spread` |
| `src/main.c` | measurement, verdict, spread check, scheduling |
| `boards/nrf54h20dk_nrf54h20_cpuapp.overlay` | counters, `fll16m`, and the DPPI fabric |

| function | job |
| --- | --- |
| `lf_ref_check()` | boot-time config oracle. Resolved accuracy vs what the board declares |
| `lf_ref_acquire()` / `lf_ref_release()` | hold `fll16m` at its best accuracy for the gate only |
| `lf_gate_measure()` | software-gated ratio. Fallback, and the discriminator for fabric faults |
| `lf_capture_measure()` | DPPI-captured gate. The accurate one |
| `lf_verdict_get()` | `LF_OK`, `LF_WRONG_SRC`, or `LF_ABSENT` |
| `lf_spread_measure()` | cycle-to-cycle deviation over `LF_JITTER_SAMPLES` short gates |
| `lf_probe_run()` | one scheduled probe, plus the fault latch |
| `lf_monitor_thread()` | early probe, late probe, spread baseline, then the hourly poll |

## two gates, and why both ship

```
lf_gate_measure()     software gate, polls the LF counter
  + works with no DPPI fabric at all
  + still reads a healthy clock when the fabric is misconfigured, which is
    what lets LF_ABSENT be distinguished from a wiring fault
  - a fixed 100 to 140 HF tick read-pair offset lands in every measurement

lf_capture_measure()  RTC COMPARE published to TIMER CAPTURE through DPPI
  + both boundaries on exact LF edges, CPU uninvolved, error is one HF tick
  + the offset is gone, so a 125 ms gate is as good as a 1 s one
  - needs a working PPIB bridge, which is board and SoC specific
```

`lf_verdict_get()` tries the capture first and falls back. That ordering is not
cosmetic: a capture timeout on its own does not mean the clock is dead, because
a misconfigured bridge times out identically. `LF_ABSENT` is only returned when
both gates fail. A capture that fails while the software gate still reads a
healthy clock returns `-EIO` and logs the software reading, rather than
condemning good hardware.

# Usage

No buttons, no pins, no shell. It logs and it latches.

```
lf_probe on nrf54h20dk@0.9.0/nrf54h20/cpuapp
lfclk nominal      : 32768 Hz
lfrc   declared    : 500 ppm, 200 us startup
lflprc declared    : 1000 ppm, 200 us startup
lf counter rtc@928000 : 32768 Hz
hf counter timer@9a2000 : 16000000 Hz
fll16m resolved    : 16000000 Hz, 30 ppm, precision 0, 850 us startup
early probe: LF_OK at -5 ppm over 32768 LF ticks
late probe: LF_OK at -6 ppm over 32768 LF ticks
spread 64 gates of 32 LF : mean 15624 HF, mad 1 HF (87 ppm), range 17 HF (1088 ppm)
```

Then a `runtime probe` line every `LF_MONITOR_PERIOD_MS`, one hour by default.

Schedule, all measured from boot:

| when | gate | why that moment |
| --- | --- | --- |
| `LF_PROBE_EARLY_MS`, 600 ms | long, 1 s | the board's declared startup budget. Dead and slow-starting differ only in time |
| `LF_PROBE_LATE_MS`, 5 s | long, 1 s | clears the `LFXO` calibration window, 3.5 to 4 s after a `BICR` write |
| just after the late probe | 64 short gates | spread baseline. Measured here because at 20 ms it is meaningless, see Testing |
| every hour | short, 125 ms | nothing re-evaluates a satisfied clock request |

Converged between the two boot probes means slow but present, and the `BICR`
declaration is wrong. Still bad at the late probe means gone.

> The deadlines are absolute (`K_TIMEOUT_ABS_MS`), not relative sleeps. A
> relative sleep runs from the end of the previous probe, which put the late
> probe at 6 s rather than 5 and missed the window it was sized for.

The fault flag latches on a bad reading and needs `LF_FAULT_CLEAR_STREAK` good
ones to clear. Two, not one: a single good gate after a bad one is as likely to
be a marginal crystal drifting back through tolerance as a real recovery.

# SoC resources

Declare what you take, because something else has to live with it.

| resource | what it takes |
| --- | --- |
| `RTC` | `rtc130`, CC 0 and 1 |
| `TIMER` | `timer130`, CC 0 and 1 |
| DPPI | channels 2 and 3 on both `DPPIC130` and `DPPIC133` |
| PPIB | `PPIB130` ch 18 and 19 to `PPIB134` ch 2 and 3, configured by IronSide SE from UICR |
| `fll16m` | held at its best accuracy for the gate only, released after |
| threads | one, 1024 B stack, `K_PRIO_PREEMPT(10)` |
| pins | none |
| flash / RAM | 58292 B / 15656 B |

That leaves `rtc131`, `timer131` through `timer137`, and DPPI channels 0, 1, and
4 through 7 free.

Notes on the choices, because two of them are not obvious:

- **Channels 2 and 3, not 0 and 1.** MPSL reserves `DPPIC130` channel 0
  (`MPSL_RESERVED_DPPI_SINK_CHANNELS` in `nrf/subsys/mpsl/init/mpsl_init.c`) and
  this probe is meant to coexist with a radio application.
- **The RTC and TIMER CC channels are squatted on** behind the Zephyr counter
  drivers, which hand the same channels out for alarms. The probe sets no
  alarms. `rtc130` has `cc-num = 4` and `timer130` has `cc-num = 6`, so nothing
  is squeezed out.

## why timer130

`cpuapp` owns no core-local `TIMER`, `RTC`, or `DPPIC` at all.
`nrf54h20_cpuapp.dtsi` deletes `cpurad_peripherals`, and `cpuapp_peripherals`
holds only the local HSFLL, IPCT, two watchdogs, resetinfo, and the 802.15.4
node. Everything below is in the global domain at `peripheral@5f000000`.

| candidate | verdict |
| --- | --- |
| `timer020` to `timer022` | `cpurad` local, deleted from this build |
| `timer120`, `timer121` | clocked from `hsfll120`, which DVFS scales, and their IRQs are routed to FLPR by default |
| `timer130` to `timer137` | clocked from `fll16m` at 16 MHz, `prescaler = 0` |

`timer130` it is. No `nordic,nrf-timer` node carries `clock-frequency`, so the
rate comes from the clock parent: `NRF_PERIPH_GET_FREQUENCY(node) / BIT(prescaler)`,
which is what `counter_nrfx_timer.c:447` does and what `counter_get_frequency()`
returns. Nothing is hardcoded.

## the bridge

`rtc130` is on the `0x5f92_0000` bus (SPU131) and `timer130` on `0x5f9a_0000`
(SPU134), confirmed by the generated `SPU_PERIPH_PERM` entries. No bus carries
both an LF-clocked counter and an HF-clocked one, so an RTC event reaching a
TIMER task must cross a PPIB bridge. That is not optional on this part.

`DPPIC130` is the hub and takes no link property of its own. The leaf declares
the route and the generator emits both halves:

```
&dppic133 {
        status = "okay";
        owned-channels = <2 3>;
        sink-channels = <2 3>;
};
```

produces, in `build/lfclk_probe/zephyr/periphconf_entries_generated.c`:

```
/* SUB: PPIB130 ch. 18 => PPIB134 ch. 2 */
/* PUB: PPIB130 ch. 18 => PPIB134 ch. 2 */
```

matching `offset = <16>` on the `ppib134` node.

> Do not give the `ppib` nodes a `status`. PPIBs are secure domain owned, and
> claiming them emits `SPU_PERIPH_PERM` writes that `periphconf-check` rejects
> as `UNRECOGNIZED_REGISTER`, which stops the device booting. The build catches
> this before you can flash it.

The channel index is the same on both sides of the bridge, and the application
has to enable both. MPSL relies on the same thing, with the reason stated at
`mpsl_init.c:441`: "Secure domain no longer enables DPPI channels for local
domains, MPSL now has to enable the ones it uses."

# Testing

## measured baseline

Good board, stock `BICR`, `fll16m` held at 30 ppm (the `hfxo` node's
`accuracy-ppm`), room temperature, five repeats per row.

| gate | mean | spread | note |
| --- | --- | --- | --- |
| software, 32768 LF (1 s) | +6 ppm | 24 ppm | inside `LF_PPM_NOISE_FLOOR` |
| software, 4096 LF (125 ms) | +71 to +84 ppm | 26 to 30 ppm | outside it, systematically |
| captured, 32768 LF | -7 ppm | 2 HF ticks, 0.125 ppm | |
| captured, 4096 LF | -7 ppm | 1 HF tick, 0.5 ppm | |

**The software gate's short-gate bias is a fixed time offset, not noise.**
Against the expected HF count:

| gate | expected HF | mean actual | shortfall |
| --- | --- | --- | --- |
| 32768 LF | 16000000 | 15999901 | -99 ticks, 6.2 us |
| 4096 LF | 2000000 | 1999858 | -142 ticks, 8.9 us |

Roughly the same absolute shortfall either way. That is the latency between the
LF read and the HF read at each boundary, and a fixed time error scales as
1/gate-length in ppm: 99 ticks of 16e6 is 6 ppm, 142 of 2e6 is 71 ppm. The
capture removes it, which is the whole argument for the DPPI gate.

**Both captured gate lengths agree at -7 ppm.** That is the real result. Two
gate lengths differing by 8x agreeing to the digit means what is left is an
actual frequency offset, not a measurement artifact. This board's `LFXO` is
about 7 ppm slow, comfortably inside its 20 ppm `BICR` claim and inside the
30 ppm reference floor.

> `lf_hz` only ever prints 32767 to 32770, because 1 Hz at 32768 Hz is 30 ppm.
> The ppm column is the output. The Hz column is decoration.

## spread, and its dependence on when you measure

| measured at | mad | range |
| --- | --- | --- |
| 20 ms after boot | 3 to 7 HF ticks | 41 and 220 HF ticks |
| 5 s, where the probe measures | 76 to 110 ppm, 1 HF tick | 9 to 17 HF ticks |
| 12 s | 17 to 20 ppm, 0 HF ticks | 2 to 3 HF ticks |

The mean settles fast and the spread settles slowly. The offset figure is
identical at 620 ms and 5 s, but the spread keeps tightening well past
`LF_PROBE_LATE_MS`. Measuring it at 20 ms reads a 220 tick range against 2 once
settled, which is exactly the intermittent condemnation of good hardware that
the late probe exists to avoid.

```
UNVERIFIED: why the spread keeps tightening after the 5 s calibration window.
LFXO amplitude still stabilising is the obvious candidate but the nRF54H20 PS
was not available to confirm. Settle it by calling lf_spread_report() on the
periodic loop for several minutes and finding where it asymptotes.
```

> `mad` prints as `1 HF` and `87 ppm` at the same time, which looks wrong and
> is not. One HF tick in a 32 tick gate is 64 ppm, so the per-sample deviation
> truncates to 0 or 1 ticks on any healthy board. The ppm figure is computed
> from the summed deviation before dividing, which keeps sub-tick resolution.
> Deriving it from the truncated tick count would report a flat 0 ppm forever.

## forcing each outcome

| outcome | how | result |
| --- | --- | --- |
| `LF_OK` | stock build | `LF_OK at -6 ppm` |
| `LF_ABSENT` | `LF_CAPTURE_TIMEOUT_MS` negative | all four gates report never closed, then `LF_ABSENT` |
| `LF_WRONG_SRC` | `LF_PPM_REJECT` below the reading | `LF_WRONG_SRC at -7 ppm, reject threshold 5 ppm` |
| fault latch, full cycle | `LF_PPM_REJECT = 3`, between the long gate's -6 ppm and the short gate's 0 ppm | both boot probes latch, two runtime probes clear it |

The latch cycle in full, which is the one worth reading:

```
early probe: LF_WRONG_SRC at -6 ppm    ->  fault flag latched: LF_WRONG_SRC
late  probe: LF_WRONG_SRC at -6 ppm    ->  (no duplicate log)
runtime probe: LF_OK at 0 ppm          ->  fault flag still latched, 1 of 2 good readings
runtime probe: LF_OK at 0 ppm          ->  fault flag cleared after 2 good readings
```

## what it cannot detect

- **A completely stopped `LFCLK`, from the inside.** The monitor thread depends
  on `LFCLK` for its own scheduling: `k_sleep()` parks on the system timer and
  GRTC SYSCOUNTER falls back to `LFCLK` while asleep. If `LFCLK` stops outright
  the CPU never wakes and nothing runs. Recognition only.
- **A calibrated `LFRC` sitting near nominal.** `LF_PPM_REJECT` at 2000 ppm will
  not catch it. That is what the spread check is for, and the spread threshold
  is only calibrated on one side. See below.
- **Better than about 30 ppm.** `fll16m` resolves at the `hfxo` node's
  `accuracy-ppm`, so `LF_PPM_NOISE_FLOOR` is 50 and a tighter claim would be
  the reference's error reported as the LF clock's.

# Fault injection

Needed to finish calibrating `LF_PPM_SPREAD_REJECT`. Three routes, two of which
do not work and are recorded so nobody repeats them.

## what does not work

**Asking for a worse clock at runtime.** `nrfs_clock_lfclk_src_set()` with
`NRFS_CLOCK_SRC_LFCLK_LFRC` returns `NRFS_CLOCK_EVT_APPLIED` and reports
`src = 4`, which is `LFRC`. The measurement on the same boot reads -6 ppm with
16 ticks of spread, byte for byte the untouched board. An `LFRC` cannot be
6 ppm; its own devicetree node declares 500.

`LFCLK` is shared, and SCFW serves the tightest requirement across the radio
core, the secure domain, and `cpuapp`'s own system tick via GRTC. A request to
degrade it is subsumed. Degrading a shared clock is not the application's
decision to make, which is the same structural reason a change notification
cannot detect a dead crystal.

**Leaving `fll16m` unconditioned.** Skipping `lf_ref_acquire()` leaves the
reference in open loop at a declared 20000 ppm. Measured effect:

| | mean | spread mad |
| --- | --- | --- |
| conditioned reference | -6 ppm | 76 to 110 ppm |
| open loop | +593 to +760 ppm | 110 ppm |

Two things learned, neither of them a calibration:

1. The verdict path detects a hundredfold degradation, so the arithmetic works
   on a real bad ratio and not just on a threshold moved by hand. But it stayed
   `LF_OK`, because 760 ppm is below `LF_PPM_REJECT`. A declared 20000 ppm
   delivered 760 in practice, so **the 2000 ppm reject threshold has less
   margin than it looks**.
2. The spread barely moved. That is the spread check working as designed:
   reference bias is common mode across samples and cancels in the deviation.
   Good confirmation, useless as a proxy for LF-source jitter.

## what does work

**A scope probe or a wire on XL1**, with stock `BICR`. Probe capacitance is
often enough to stall a 32.768 kHz oscillator. Zero writes, and it is the only
route that shows a crystal quitting while running, which is the month-six
failure the runtime monitor exists for. Needs a human at the bench.

**A `BICR` edit.** The only route that is both solo-drivable and produces a
genuinely non-crystal `LFCLK`, because it changes what arbitration is choosing
from rather than asking arbitration for a favour.

Prefer **declaring no `LFXO`** over the `EXT_SQUARE` trick. No-`LFXO` is how a
real board built without a 32 kHz crystal is configured, so it is a supported,
shipped path and arbitration is guaranteed to fall back to `LFRC`. Use
`EXT_SQUARE` only if you specifically want the "`BICR` claims a crystal that
is not there" scenario, and note:

```
UNVERIFIED: that EXT_SQUARE disables the crystal amplifier on nRF54H20. This is
reasoned across from the nRF54L LFXO external-clock-source binding, which
specifies a rail-to-rail 32768 Hz source on XL1 with XL2 unconnected and
internal load capacitors disabled. The nRF54H20 PS was not available to confirm
it. If the injection produces no measurable change, doubt this first.
```

Procedure, in order. `../bicr_backup/IFYOUBRICKEDUSETHISBICR.txt` has the
recovery path in full.

1. **Save `BICR` first, 0x50 bytes, not 0x40.**

   ```
   nrfutil device read --address 0x0FFF87B0 --bytes 0x50 --to-file bicr_stock.hex
   ```

   `BICR` is `0x50` bytes and the CRC is the last four of them, at
   `0x0FFF87FC`. Three sources agree: `bicrgen.py:1033` (`BICR_SIZE = 0x50`),
   `bicrgen.py:1066` (`buf[BICR_SIZE - BICR_CRC_SIZE:]`), and
   `nrf54h20_application.svd` (`BICR_NS` `baseAddress 0x0FFF87B0`,
   `addressBlock size 0x50`). A `0x40` read ends at `0x0FFF87EF` and misses the
   CRC, so it is not a restore path. Read it twice and diff the two.

   > Do not trust the MDK for the geometry. `bicrgen.py` says so at the
   > `BICR_SIZE` definition: "hardcoded here since the MDK is incomplete,
   > missing reserved fields and the BICR CRC field." That is why the SVD lists
   > nine `BICR` registers and none of them is the CRC.

2. **Confirm the lifecycle state is `RoT`.** `BICR` is only reprogrammable
   there, and changing `lfosc` does not change it.

   ```
   nrfutil device x-adac-discovery
   ```

   Look for `psa_lifecycle LIFECYCLE_ROT (0x2000)`.

3. **Edit only `lfosc`.** `<ncs>` is a read-only SDK install, so copy
   `zephyr/boards/nordic/nrf54h20dk/bicr.json` out before editing it.

   > Do not touch `power.scheme`, `ioPortPower`, or `ioPortImpedance`. Those are
   > the fields the docs warn can damage the device or leave it unrecoverable.
   > `lfosc` gets you a bad clock, not a dead part.

4. **Generate.** The toolchain launch is not optional, because `bicrgen.py`
   needs `intelhex` and the system python does not have it.

   ```
   nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- \
     python3 <ncs>/zephyr/soc/nordic/nrf54h/bicr/bicrgen.py \
       -i my_bicr.json \
       -s <ncs>/modules/hal/nordic/nrfx/bsp/stable/mdk/nrf54h20_application.svd \
       -o bicr_modified.hex
   ```

5. **Program with `ERASE_NONE`.** A chip erase here is how a recoverable board
   becomes a paperweight.

   ```
   nrfutil device program --firmware bicr_modified.hex \
     --options chip_erase_mode=ERASE_NONE --core Application
   ```

6. **Reset with the pin**, not a software reset.
   `--reset-kind RESET_PIN`.

7. **Restore from `bicr_stock.hex`** when done, same flags, then diff a fresh
   read against it.

Watch boot timing during the test. Calibration cannot complete with nothing
driving XL1 and sysctrl retries on the next boot, so a repeated multi-second
stall on every boot rather than only the first is itself an observable
signature, not a symptom of damage.

# Failure paths

Nominal case first, then what each failure costs at system level. The second
clause is the point.

**No `LFCLK` at all.** The tick never advances, execution dies in `__WFE`, no
application code runs. Nothing detects it from inside, this probe included.
Recognition only: a board that boots to silence with a live debugger is this.

**Silent RC fallback with a crystal declared.** The one that ships. BLE sizes
receive window widening from `BICR`'s 20 ppm while the clock is 500 ppm or
worse. Undersized widening, missed anchor points, drops under load and over
temperature. Works perfectly on a bench at room temperature. **This is what the
probe is for.**

**Wrong `BICR` or a depopulated `LFXO` on a board variant.** Same signature as
the above but systematic across every unit of that build. Caught by
`lf_ref_check()` at boot with no measurement at all, because the resolved
accuracy disagrees with what the firmware was built for.

**Slow crystal, correct part.** Fails the early probe, passes the late one.
Every MPSL timing assumption sized off the declared 600 ms budget is wrong
until `BICR` is fixed. The two boot probes exist to separate this from a dead
crystal, since the two differ only in time.

# Notes

## the clock API reports intent, not oscillation

Two independent findings, both on a good board, both worth more than the rest
of this README.

**`clock_control` resolved accuracy is `BICR` read back.** `fll16m` resolves to
30 ppm on this DK because that is `hfxo`'s `accuracy-ppm` in
`nrf54h20dk_nrf54h20-common.dtsi:12`, not because anything measured 30 ppm.
`lf_ref_check()` uses this deliberately, as a config oracle: it detects a wrong
`BICR` or a wrong board variant, and it detects nothing about a live crystal.

**The `nrfs` clock service source field is the request, not the oscillator.**
`nrfs_clock_lfclk_src_set(NRFS_CLOCK_SRC_LFCLK_LFRC)` returned
`NRFS_CLOCK_EVT_APPLIED` with `src = 4`, and the clock kept measuring -6 ppm.

```
UNVERIFIED: whether SCFW rejected the downgrade internally, or applied it to a
per-domain view while arbitration held the physical source at LFXO. The event
carries no effective-source field to distinguish these. What is measured is
that EVT_APPLIED with src=LFRC does not imply an RC clock.
```

This is exactly what the nRF54L PS documents for `LFCLK.STAT.SRC`, which it
defines as the value of `SRCCOPY` when `LFCLKSTARTED` triggered, that is, the
request. Now observed on nRF54H20 at the IPC service level.

Practical consequence, and the reason this sample exists: **you cannot learn the
effective `LFCLK` source from software. You have to measure it.**

## RTC compare events need EVTEN

Unlike TIMER, an nRF RTC does not set `EVENTS_COMPARE[n]` on match unless the
matching `EVTEN` bit is set. The Zephyr counter driver sets `EVTEN` only when it
hands out an alarm, and this probe sets no alarms.

> Without `nrf_rtc_event_enable()` the compare matches, nothing publishes, and
> the captured gate reports `-ETIMEDOUT`, which is indistinguishable from a dead
> clock. This cost a flash cycle to find and is the reason `LF_ABSENT` requires
> both gates to fail.

## other gotchas

- **Do not use GRTC as the LF side.** Its SYSCOUNTER runs from the 16 MHz clock
  while active and only falls back to `LFCLK` in sleep, so a reading would be
  contaminated by the reference.
- **Do not rely on the watchdog as a backstop.** Every `wdt` node is
  `clocks = <&lfclk>`. It stops counting too.
- **Do not declare better than 20 ppm to MPSL.** Known issue DRGN-23693: the
  sleep clock accuracy communicated to the peer is wrong when MPSL is
  initialised with an accuracy better than 20 ppm.
- **No floating point in the measurement path.** ppm is computed by comparing HF
  counts rather than frequencies, so there is one division and one rounding,
  with `uint64_t` intermediates because `lf_ticks * hf_hz` reaches 32768 * 16e6.
- **`fll16m` closed loop is off limits.** `FLL16M_MODE_CLOSED_LOOP` is annotated
  in `clock_control_nrf_fll16m.c:24` as "DO NOT IMPLEMENT, CAN CAUSE HARDWARE
  BUG". Bypass is the only path to a trustworthy 16 MHz.
- **`precision` must stay `NRF_CLOCK_CONTROL_PRECISION_DEFAULT`.**
  `fll16m_resolve_spec_to_idx()` returns `-EINVAL` for anything else before it
  even looks at accuracy.
- **`nrf_clock_control_request_sync()` exists** and is the simpler path in
  thread context. Declared at `nrf_clock_control.h:296`, implemented in
  `clock_control_nrf2_common.c:202`.

## deferred logging is required, not cosmetic

`CONFIG_LOG_MODE_DEFERRED=y` matters because the software gate has log calls
near it and immediate mode would put UART driver time directly into the ppm
figure. It is not sufficient on its own: the first long software gate of every
boot was consistently the worst of five, because the log backend is still
draining while it runs.

## the threshold that is only half calibrated

`LF_PPM_SPREAD_REJECT` is 500 ppm, which is five times the worst mean absolute
deviation seen on a known good board at the point the probe measures. It will
not condemn a working crystal.

It cannot promise the other direction. Nothing has been measured on a board
whose LF source is genuinely not a crystal, so the false negative rate is
unknown, and a calibrated `LFRC` quiet enough to sit under 500 ppm would pass.
Closing that needs one of the fault injection routes above.

Tighten it once the settling curve is known. A good board reads 17 to 20 ppm
settled against 76 to 110 ppm at 5 s, so measuring later would allow a much
tighter threshold than five times the early figure.
