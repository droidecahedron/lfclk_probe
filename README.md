# lfclk_probe

Counts `LFCLK` against an `HFXO`-derived reference so you can tell whether it's
actually running on the crystal.

# Hardware

`nRF54H20 DK`, `PCA10175`, board target `nrf54h20dk/nrf54h20/cpuapp`.

> Measured on `NRF54H20_xxAA_ENGC`. Any nRF54H20 should be fine. Other parts
> need the DPPI and PPIB rework described under SoC resources, because which bus
> holds the RTC and which holds the TIMER is specific to this SoC.

# Software

`nRF Connect SDK v3.4.0` (`v3.4.0-99553055607b`), Zephyr `4.4.0`
(`v4.4.0-bf801e4e3d19`).

```
west build -b nrf54h20dk/nrf54h20/cpuapp
west flash
```

# Background

Nothing in the software stack will tell you the `LFXO` is dead.

Ask `clock_control` for accuracy on this board and it resolves 20 ppm, but that
number comes out of `BICR`. A broken crystal doesn't edit `BICR`, so you get
20 ppm forever while the hardware may be sitting on `LFRC`.

The obvious approaches and why each one fails:

- **Wait for an event.** There's no LF equivalent of the `HFXO` quality events
  (`EVENTS_XOTUNEERROR`, `EVENTS_XOTUNEFAILED`). No part in the family has one.
- **Read a status latch.** Event registers record transitions. A crystal that
  dies in steady state never produces one, and nothing re-runs the arbitration,
  so every register keeps reading its day-one value.
- **Ask the clock service.** Both the `clock_control` accuracy and the `nrfs`
  clock service source field report what got requested or declared. See Notes
  for what happens when you try.

So count `LFCLK` against something you trust. That reads steady state, doesn't
care who owns the oscillator, and works at boot and at month six.

You get `HFXO` for free as the reference. If the 32 MHz crystal were bad, MPSL
asserts during init, so a working radio is proof of the reference.

The gate is the measurement window: N LF ticks wide, HF ticks counted inside it.

| gate | boundaries taken by |
| --- | --- |
| software | CPU register reads |
| captured | RTC COMPARE firing TIMER CAPTURE over DPPI |

# Overview

| file | what's in it |
| --- | --- |
| `src/lf_probe.h` | every tunable, each with its origin. `enum lf_verdict`, `struct lf_spread` |
| `src/main.c` | measurement, verdict, spread check, scheduling |
| `boards/nrf54h20dk_nrf54h20_cpuapp.overlay` | counters, `fll16m`, DPPI channels, and the PPIB bridge |

| function | job |
| --- | --- |
| `lf_ref_check()` | boot-time config oracle. Resolved accuracy against what the board declares |
| `lf_ref_acquire()` / `lf_ref_release()` | hold `fll16m` at its best accuracy for the gate only |
| `lf_gate_measure()` | software-gated ratio. Fallback, and the discriminator for DPPI route faults |
| `lf_capture_measure()` | DPPI-captured gate. The accurate one |
| `lf_verdict_get()` | `LF_OK`, `LF_WRONG_SRC`, or `LF_ABSENT` |
| `lf_spread_measure()` | cycle-to-cycle deviation over `LF_JITTER_SAMPLES` short gates |
| `lf_probe_run()` | one scheduled probe, plus the fault latch |
| `lf_monitor_thread()` | early probe, late probe, spread baseline, then the hourly poll |

## two gates

```
lf_gate_measure()     software gate, polls the LF counter
  + works with no DPPI route configured at all
  + still reads a healthy clock when the DPPI route is misconfigured, which is
    what lets LF_ABSENT be told apart from a wiring fault
  - a fixed 100 to 140 HF tick read-pair offset lands in every measurement

lf_capture_measure()  RTC COMPARE published to TIMER CAPTURE through DPPI
  + both boundaries on exact LF edges, CPU uninvolved, error is one HF tick
  + no offset, so a 125 ms gate is as good as a 1 s one
  - needs a working PPIB bridge, which is board and SoC specific
```

`lf_verdict_get()` tries the capture first and falls back to the software gate.
A capture timeout on its own doesn't mean the clock is
dead, because a misconfigured bridge times out exactly the same way, so
`LF_ABSENT` only comes back when both gates fail. If the capture fails while the
software gate still reads a healthy clock you get `-EIO` and a log line with the
software reading, rather than a good board being condemned.

# Usage

No buttons, no pins, no shell. It logs, and it latches a fault flag.

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

After that you get a `runtime probe` line every `LF_MONITOR_PERIOD_MS`, one hour
by default.

| when, from boot | gate | why there |
| --- | --- | --- |
| `LF_PROBE_EARLY_MS`, 600 ms | long, 1 s | the board's declared startup budget. Dead and slow-starting only differ in time |
| `LF_PROBE_LATE_MS`, 5 s | long, 1 s | clears the `LFXO` calibration window, 3.5 to 4 s after a `BICR` write |
| right after the late probe | 64 short gates | spread baseline. Measured here because at 20 ms it's meaningless, see Testing |
| every hour | short, 125 ms | nothing re-evaluates a satisfied clock request |

If the verdict is bad at the early probe and good at the late one, the crystal is
present but slower than `BICR` claims. Still bad at the late probe means it's
gone.

> Both deadlines are absolute (`K_TIMEOUT_ABS_MS`). A relative sleep runs from
> the end of the previous probe, which puts the late probe at 6 s instead of 5
> and misses the window it was sized for.

The fault flag latches on a bad reading and wants `LF_FAULT_CLEAR_STREAK` good
ones to clear. Two rather than one, because a single good gate after a bad one is
as likely to be a marginal crystal drifting back through tolerance as a real
recovery.

# SoC resources

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

That leaves you `rtc131`, `timer131` through `timer137`, and DPPI channels 0, 1,
and 4 through 7.

MPSL reserves `DPPIC130` channel 0 (`MPSL_RESERVED_DPPI_SINK_CHANNELS` in
`nrf/subsys/mpsl/init/mpsl_init.c`), so the probe sits on channels 2 and 3 and
stays out of the way of a radio application.

The RTC and TIMER CC channels sit behind the Zephyr counter drivers, which hand
those same channels out for alarms. This sets no alarms. `rtc130` has
`cc-num = 4` and `timer130` has `cc-num = 6`, so nothing gets squeezed out.

## picking the timer

`cpuapp` owns no core-local `TIMER`, `RTC`, or `DPPIC`.
`nrf54h20_cpuapp.dtsi` deletes `cpurad_peripherals`, and `cpuapp_peripherals`
holds only the local HSFLL, IPCT, two watchdogs, resetinfo, and the 802.15.4
node. Everything else lives in the global domain at `peripheral@5f000000`.

| candidate | why not |
| --- | --- |
| `timer020` to `timer022` | `cpurad` local, deleted from this build |
| `timer120`, `timer121` | clocked from `hsfll120`, which DVFS scales, and their IRQs go to FLPR by default |
| `timer130` to `timer137` | fine. Clocked from `fll16m` at 16 MHz, `prescaler = 0` |

No `nordic,nrf-timer` node carries `clock-frequency`, so the rate comes from the
clock parent as `NRF_PERIPH_GET_FREQUENCY(node) / BIT(prescaler)`. That's what
`counter_nrfx_timer.c:447` does and what `counter_get_frequency()` gives you, so
nothing here is hardcoded.

## the bridge

`rtc130` sits on the `0x5f92_0000` bus (SPU131) and `timer130` on `0x5f9a_0000`
(SPU134), which you can confirm from the generated `SPU_PERIPH_PERM` entries. No
bus carries both an LF-clocked counter and an HF-clocked one, so getting an RTC
event into a TIMER task means crossing a PPIB bridge on this part.

`DPPIC130` is the hub and takes no link property of its own. Declare the route on
the leaf and the generator emits both halves:

```
&dppic133 {
        status = "okay";
        owned-channels = <2 3>;
        sink-channels = <2 3>;
};
```

gives you, in `build/lfclk_probe/zephyr/periphconf_entries_generated.c`:

```
/* SUB: PPIB130 ch. 18 => PPIB134 ch. 2 */
/* PUB: PPIB130 ch. 18 => PPIB134 ch. 2 */
```

which matches `offset = <16>` on the `ppib134` node.

> Don't give the `ppib` nodes a `status`. PPIBs are secure domain owned, and
> claiming them emits `SPU_PERIPH_PERM` writes that `periphconf-check` rejects as
> `UNRECOGNIZED_REGISTER`, which stops the device booting. The build catches it
> before you can flash, which is the only reason this was cheap to find.

The channel index is the same on both sides of the bridge and you have to enable
both. MPSL leans on the same thing, with the reason at `mpsl_init.c:441`:

> Secure domain no longer enables DPPI channels for local domains, MPSL now has
> to enable the ones it uses.

# Testing

## measured baseline

Good board, stock `BICR`, `fll16m` held at 30 ppm (the `hfxo` node's
`accuracy-ppm`), room temperature, five repeats per row.

| gate | mean | spread | |
| --- | --- | --- | --- |
| software, 32768 LF (1 s) | +6 ppm | 24 ppm | inside `LF_PPM_NOISE_FLOOR` |
| software, 4096 LF (125 ms) | +71 to +84 ppm | 26 to 30 ppm | outside it, every time |
| captured, 32768 LF | -7 ppm | 2 HF ticks, 0.125 ppm | |
| captured, 4096 LF | -7 ppm | 1 HF tick, 0.5 ppm | |

Look at the raw HF counts and the short-gate bias turns out to be a fixed time
offset:

| gate | expected HF | mean actual | shortfall |
| --- | --- | --- | --- |
| 32768 LF | 16000000 | 15999901 | -99 ticks, 6.2 us |
| 4096 LF | 2000000 | 1999858 | -142 ticks, 8.9 us |

Roughly the same absolute shortfall either way. That's the latency between the LF
read and the HF read at each boundary, and a fixed time error scales as
1/gate-length once you express it in ppm: 99 ticks of 16e6 is 6 ppm, 142 of 2e6
is 71 ppm. The capture takes it out, which is the whole argument for the DPPI
gate.

Both captured gate lengths land on -7 ppm. Two gates differing by 8x agreeing to
the digit is what tells you the leftover error is a real frequency offset and
measurement artifact. This board's `LFXO` runs about 7 ppm slow, inside its
20 ppm `BICR` claim and inside the 30 ppm reference floor.

> `lf_hz` only ever prints 32767 to 32770, because 1 Hz at 32768 Hz is already
> 30 ppm. Read the ppm column. The Hz column is decoration.

## spread depends on when you measure it

| measured at | mad | range |
| --- | --- | --- |
| 20 ms after boot | 3 to 7 HF ticks | 41 and 220 HF ticks |
| 5 s, where the probe measures | 76 to 110 ppm, 1 HF tick | 9 to 17 HF ticks |
| 12 s | 17 to 20 ppm, 0 HF ticks | 2 to 3 HF ticks |

The mean settles quickly and the spread doesn't. You get an identical offset at
620 ms and at 5 s, while the spread keeps tightening well past
`LF_PROBE_LATE_MS`. Measure it at 20 ms and you read a 220 tick range against 2
once settled, which is the intermittent condemnation of good hardware that the
late probe exists to avoid.

```
UNVERIFIED: why the spread keeps tightening after the 5 s calibration window.
LFXO amplitude still stabilising is the obvious candidate but the nRF54H20 PS
was not available to confirm. Settle it by calling lf_spread_report() on the
periodic loop for several minutes and finding where it asymptotes.
```

> `mad` printing as `1 HF` and `87 ppm` together looks wrong and isn't. One HF
> tick in a 32 tick gate is 64 ppm, so per-sample deviation truncates to 0 or 1
> ticks on any healthy board. The ppm figure comes from the summed deviation
> before dividing, which keeps sub-tick resolution. Derive it from the truncated
> tick count and you'd report a flat 0 ppm forever.

## forcing each outcome

| outcome | how | what you see |
| --- | --- | --- |
| `LF_OK` | stock build | `LF_OK at -6 ppm` |
| `LF_ABSENT` | `LF_CAPTURE_TIMEOUT_MS` negative | all four gates report never closed, then `LF_ABSENT` |
| `LF_WRONG_SRC` | `LF_PPM_REJECT` below the reading | `LF_WRONG_SRC at -7 ppm, reject threshold 5 ppm` |
| fault latch, full cycle | `LF_PPM_REJECT = 3`, between the long gate's -6 ppm and the short gate's 0 ppm | both boot probes latch, two runtime probes clear it |

The latch cycle is the one worth watching:

```
early probe: LF_WRONG_SRC at -6 ppm    ->  fault flag latched: LF_WRONG_SRC
late  probe: LF_WRONG_SRC at -6 ppm    ->  (no duplicate log)
runtime probe: LF_OK at 0 ppm          ->  fault flag still latched, 1 of 2 good readings
runtime probe: LF_OK at 0 ppm          ->  fault flag cleared after 2 good readings
```

## scope limits

Three things this can't do for you.

A completely stopped `LFCLK` is invisible from inside. The monitor thread needs
`LFCLK` for its own scheduling, since `k_sleep()` parks on the system timer and
GRTC SYSCOUNTER falls back to `LFCLK` while asleep. Stop `LFCLK` outright and the
CPU never wakes, so nothing runs. You recognise that one, you don't detect it.

A calibrated `LFRC` sitting near nominal gets past `LF_PPM_REJECT` at 2000 ppm.
That's what the spread check is for, and the spread threshold is only calibrated
on one side. See the end of Notes.

Anything better than about 30 ppm is beyond the rig. `fll16m` resolves at the
`hfxo` node's `accuracy-ppm`, so `LF_PPM_NOISE_FLOOR` is 50 and a tighter claim
would just be the reference's error reported as the LF clock's.

# Fault injection

You need this to finish calibrating `LF_PPM_SPREAD_REJECT`. Two of the three
routes don't work, and they're written down so you don't repeat them.

## asking for a worse clock at runtime

`nrfs_clock_lfclk_src_set()` with `NRFS_CLOCK_SRC_LFCLK_LFRC` returns
`NRFS_CLOCK_EVT_APPLIED` and reports `src = 4`, which is `LFRC`. Measure on the
same boot and you get -6 ppm with 16 ticks of spread, identical to the untouched
board. An `LFRC` can't be 6 ppm; its own devicetree node declares 500.

`LFCLK` is shared, and SCFW serves the tightest requirement across the radio
core, the secure domain, and `cpuapp`'s own system tick via GRTC. Your request to
degrade it gets subsumed. Degrading a shared clock isn't the application's
decision to make, which is the same reason a change notification can't detect a
dead crystal.

## leaving fll16m unconditioned

Skip `lf_ref_acquire()` and the reference stays in open loop at a declared
20000 ppm.

| | mean | spread mad |
| --- | --- | --- |
| conditioned reference | -6 ppm | 76 to 110 ppm |
| open loop | +593 to +760 ppm | 110 ppm |

The verdict path catches a hundredfold degradation, so the arithmetic works on a
real bad ratio and not only on a threshold you moved by hand. It still came back
`LF_OK`, because 760 ppm is under `LF_PPM_REJECT`. A reference declaring
20000 ppm delivered 760 in practice, so the 2000 ppm threshold has less margin
than it looks. None of that calibrates the spread check.

The spread barely moved, which is the spread check doing its job: reference bias
is common mode across samples and cancels in the deviation. Good confirmation of
the design, useless as a proxy for LF-source jitter.

## a scope probe or a wire on XL1

Stock `BICR`, probe capacitance is often enough to stall a 32.768 kHz oscillator.
No writes, and it's the only route that shows you a crystal quitting while
running, which is the month-six failure the runtime monitor exists for. Needs a
human at the bench.

## a BICR edit

The only route that's both scriptable and produces a genuinely non-crystal
`LFCLK`, because it changes what arbitration is choosing from instead of asking
arbitration for a favour.

Prefer declaring no `LFXO` over the `EXT_SQUARE` trick. No-`LFXO` is how a real
board built without a 32 kHz crystal gets configured, so it's a supported path
and arbitration is guaranteed to fall back to `LFRC`. Reach for `EXT_SQUARE` only
if you specifically want the "`BICR` claims a crystal that isn't there" scenario.

```
UNVERIFIED: that EXT_SQUARE disables the crystal amplifier on nRF54H20. Reasoned
across from the nRF54L LFXO external-clock-source binding, which specifies a
rail-to-rail 32768 Hz source on XL1 with XL2 unconnected and internal load
capacitors disabled. The nRF54H20 PS was not available to confirm it. If the
injection produces no measurable change, doubt this first.
```

`../bicr_backup/IFYOUBRICKEDUSETHISBICR.txt` has the recovery path in full.

1. Save `BICR` first, 0x50 bytes.

   ```
   nrfutil device read --address 0x0FFF87B0 --bytes 0x50 --to-file bicr_stock.hex
   ```

   `BICR` is `0x50` bytes and the CRC is the last four of them, at `0x0FFF87FC`.
   Three sources agree: `bicrgen.py:1033` (`BICR_SIZE = 0x50`),
   `bicrgen.py:1066` (`buf[BICR_SIZE - BICR_CRC_SIZE:]`), and
   `nrf54h20_application.svd` (`BICR_NS` `baseAddress 0x0FFF87B0`,
   `addressBlock size 0x50`). Read `0x40` and you stop at `0x0FFF87EF`, miss the
   CRC, and don't have a restore path. Read it twice and diff the two.

   > Don't take the geometry from the MDK. `bicrgen.py` says why at the
   > `BICR_SIZE` definition: "hardcoded here since the MDK is incomplete,
   > missing reserved fields and the BICR CRC field." That's why the SVD lists
   > nine `BICR` registers and none of them is the CRC.

2. Check the lifecycle state is `RoT`, since that's what makes `BICR`
   reprogrammable. Changing `lfosc` doesn't affect it.

   ```
   nrfutil device x-adac-discovery
   ```

   Look for `psa_lifecycle LIFECYCLE_ROT (0x2000)`.

3. Edit only `lfosc`. `<ncs>` is a read-only SDK install, so copy
   `zephyr/boards/nordic/nrf54h20dk/bicr.json` out before you touch it.

   > Leave `power.scheme`, `ioPortPower`, and `ioPortImpedance` alone. Those are
   > the fields the docs warn can damage the device or leave it unrecoverable.
   > `lfosc` gets you a bad clock, not a dead part.

4. Generate. The toolchain launch isn't optional, because `bicrgen.py` needs
   `intelhex` and the system python doesn't have it.

   ```
   nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- \
     python3 <ncs>/zephyr/soc/nordic/nrf54h/bicr/bicrgen.py \
       -i my_bicr.json \
       -s <ncs>/modules/hal/nordic/nrfx/bsp/stable/mdk/nrf54h20_application.svd \
       -o bicr_modified.hex
   ```

5. Program with `ERASE_NONE`. A chip erase here is how a recoverable board turns
   into a paperweight.

   ```
   nrfutil device program --firmware bicr_modified.hex \
     --options chip_erase_mode=ERASE_NONE --core Application
   ```

6. Reset with the pin: `--reset-kind RESET_PIN`.

7. Restore from `bicr_stock.hex` when you're done, same flags, then diff a fresh
   read against it.

Watch boot timing while the fault is in. Calibration can't complete with nothing
driving XL1 and sysctrl retries on the next boot, so a repeated multi-second
stall on every boot rather than only the first is itself the signature and not a
symptom of damage.

# Failure paths

**No `LFCLK` at all.** The tick never advances, execution dies in `__WFE`, no
application code runs. Nothing detects it from inside, this probe included. A
board that boots to silence with a live debugger is this one.

**Silent RC fallback with a crystal declared.** The one that ships. BLE sizes
receive window widening from `BICR`'s 20 ppm while the clock is 500 ppm or worse,
so you get undersized widening, missed anchor points, and drops under load and
over temperature. Works perfectly on a bench at room temperature. This is the one
the probe exists for.

**Wrong `BICR` or a depopulated `LFXO` on a board variant.** Same signature as
above but systematic across every unit of that build. `lf_ref_check()` catches it
at boot with no measurement at all, because the resolved accuracy disagrees with
what the firmware was built for.

**Slow crystal, correct part.** Fails the early probe, passes the late one. Every
MPSL timing assumption sized off the declared 600 ms budget is wrong until `BICR`
is fixed. The two boot probes are there to separate this from a dead crystal,
since the two only differ in time.

# Notes

## what the clock API actually reports

Both of these came off a good board with no fault injected.

Resolved accuracy from `clock_control` is `BICR` read back. `fll16m` resolves to
30 ppm on this DK because that's `hfxo`'s `accuracy-ppm` in
`nrf54h20dk_nrf54h20-common.dtsi:12`, and not because anything measured 30 ppm.
`lf_ref_check()` uses that on purpose, as a config oracle: it catches a wrong
`BICR` or a wrong board variant, and it tells you nothing about a live crystal.

The `nrfs` clock service reports the source you asked for. It does not check
what the oscillator is doing.
`nrfs_clock_lfclk_src_set(NRFS_CLOCK_SRC_LFCLK_LFRC)` came back
`NRFS_CLOCK_EVT_APPLIED` with `src = 4` while the clock kept measuring -6 ppm.

```
UNVERIFIED: whether SCFW rejected the downgrade internally, or applied it to a
per-domain view while arbitration held the physical source at LFXO. The event
carries no effective-source field to distinguish these. What is measured is that
EVT_APPLIED with src=LFRC does not imply an RC clock.
```

The nRF54L PS documents the same shape for `LFCLK.STAT.SRC`, which it defines as
the value of `SRCCOPY` when `LFCLKSTARTED` triggered, so the request. Now
observed on nRF54H20 at the IPC service level.

What that leaves you with: you can't learn the effective `LFCLK` source from
software, so you have to measure it.

## RTC compare events need EVTEN

An nRF RTC won't set `EVENTS_COMPARE[n]` on match unless the matching `EVTEN` bit
is set, which is where it differs from TIMER. The Zephyr counter driver sets
`EVTEN` only when it hands out an alarm, and this sets no alarms.

> Without `nrf_rtc_event_enable()` the compare matches, nothing publishes, and
> the captured gate reports `-ETIMEDOUT`, which you can't tell apart from a dead
> clock. That cost a flash cycle to find and it's why `LF_ABSENT` needs both
> gates to fail.

## other gotchas

- Don't use GRTC as the LF side. Its SYSCOUNTER runs from the 16 MHz clock while
  active and only falls back to `LFCLK` in sleep, so the reading is contaminated
  by the reference.
- Don't rely on the watchdog as a backstop. Every `wdt` node is
  `clocks = <&lfclk>`, so it stops counting too.
- Don't declare better than 20 ppm to MPSL. Known issue DRGN-23693: the sleep
  clock accuracy communicated to the peer is wrong when MPSL is initialised with
  an accuracy better than 20 ppm.
- No floating point in the measurement path. ppm compares HF counts rather than
  frequencies, so there's one division and one rounding, with `uint64_t`
  intermediates because `lf_ticks * hf_hz` reaches 32768 * 16e6.
- `fll16m` closed loop is off limits. `FLL16M_MODE_CLOSED_LOOP` is annotated at
  `clock_control_nrf_fll16m.c:24` as "DO NOT IMPLEMENT, CAN CAUSE HARDWARE BUG".
  Bypass is your only path to a trustworthy 16 MHz.
- Keep `precision` at `NRF_CLOCK_CONTROL_PRECISION_DEFAULT`.
  `fll16m_resolve_spec_to_idx()` returns `-EINVAL` for anything else before it
  even looks at accuracy.
- `nrf_clock_control_request_sync()` exists and is the simpler path in thread
  context. Declared at `nrf_clock_control.h:296`, implemented in
  `clock_control_nrf2_common.c:202`.

## why deferred logging

`CONFIG_LOG_MODE_DEFERRED=y` matters because the software gate has log calls near
it and immediate mode would put UART driver time straight into the ppm figure.
It's not sufficient on its own: the first long software gate of every boot was
consistently the worst of five, because the log backend is still draining while
it runs.

## the threshold that's only half calibrated

`LF_PPM_SPREAD_REJECT` is 500 ppm, five times the worst mean absolute deviation
seen on a known good board at the point the probe measures. It won't condemn a
working crystal.

It can't promise you the other direction. Nothing has been measured on a board
whose LF source is genuinely not a crystal, so the false negative rate is
unknown, and a calibrated `LFRC` quiet enough to sit under 500 ppm would pass.
Closing that needs one of the fault injection routes above.

Tighten it once you know the settling curve. A good board reads 17 to 20 ppm
settled against 76 to 110 ppm at 5 s, so measuring later would let you go much
tighter than five times the early figure.
