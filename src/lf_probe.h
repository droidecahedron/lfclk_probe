/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Tunables for the LFCLK health probe, each with its origin.
 *
 * Anything a reviewer would otherwise have to derive lives here rather than
 * inline at the point of use.
 */

#ifndef LF_PROBE_H_
#define LF_PROBE_H_

#include <zephyr/devicetree.h>

/* What every measurement is compared against, straight from the lfclk node so
 * a board that declares something else does not need a code change.
 */
#define LF_NOMINAL_HZ DT_PROP(DT_NODELABEL(lfclk), clock_frequency)

/* Gate lengths in LF ticks. Multiples of 32 are what make the expected HF
 * count integral: 16e6 / 32768 = 488.28125, and 488.28125 * 32 = 15625, so any
 * multiple of 32 LF ticks lands on a whole number of HF ticks and the
 * arithmetic stays in integers.
 */
#define LF_GATE_TICKS_LONG   32768 /* 1 s, expected 16000000 HF ticks exactly */
#define LF_GATE_TICKS_SHORT  4096  /* 125 ms, expected 2000000 HF ticks exactly */
#define LF_GATE_TICKS_JITTER 32    /* ~1 ms, expected 15625 HF ticks exactly */

/* Gates per spread run. One 32 tick gate resolves only to 1 part in 15625, or
 * 64 ppm, so the spread figure comes from the summed deviation across all
 * samples rather than from any single gate. 64 samples takes the aggregate
 * resolution to about 1 ppm.
 */
#define LF_JITTER_SAMPLES 64

/* Margin allowed on top of a gate's own expected duration before the gate is
 * called dead. A gate that never closes means LFCLK is not advancing, which is
 * an outcome rather than an error.
 */
#define LF_CAPTURE_TIMEOUT_MS 2000

/* fll16m resolves at the hfxo node's accuracy-ppm, which is 30 on this DK, so
 * a tighter claim than this is the reference's own error being reported as the
 * LF clock's. Do not narrow it without a better reference.
 */
#define LF_PPM_NOISE_FLOOR 50

/* Above this, the LF source is not a crystal. Deliberately loose: lfclk
 * declares lflprc-accuracy-ppm at exactly 1000, so any threshold at or below
 * that sits on top of a legal mode. An uncalibrated LFRC is off by percent, so
 * 2000 keeps two decades of margin either side. It does not catch a calibrated
 * LFRC sitting near nominal, which is what the spread check is for.
 */
#define LF_PPM_REJECT 2000

/* Two-sided, measured on one DK at room temperature with the spread taken
 * inside the probe, straight after the 1 s gate, so the reference is already
 * settled.
 *
 *   crystal   mad 19 to  58 ppm    8 boots
 *   LFRC      mad 248 to 622 ppm   5 boots
 *
 * 150 sits 2.6x above the worst crystal reading and 1.65x below the quietest RC
 * one. The bias toward not raising false alarms is deliberate, because a bad
 * reading here latches the fault flag.
 *
 * Measure it inside the probe, not on its own. An earlier version ran the spread
 * from a fresh lf_ref_acquire() and read 76 to 110 ppm on the same crystal,
 * because it was measuring the HFXO ramp as well as the LF source.
 *
 * The RC side came from a BICR edit declaring source LFRC, which is the only way
 * to get a non-crystal LFCLK on this part. A runtime request is subsumed by SCFW
 * arbitration. See the README.
 *
 * TODO: re-check over temperature and on more than one board. Both figures come
 * from a single DK, and the crystal side threw one 58 ppm boot against a 19 to
 * 29 ppm cluster.
 */
#define LF_PPM_SPREAD_REJECT 150

/* The board's declared LFCLK startup budget, which is what
 * nrf_clock_control_get_startup_time() reports for an LFXO accuracy request on
 * this DK. A probe at this point separates dead from slow-starting, because the
 * two differ only in time.
 */
#define LF_PROBE_EARLY_MS 600

/* Clears the LFXO calibration window. Calibration takes roughly 3.5 to 4 s
 * after a BICR write and timing is not accurate until it finishes, so probing
 * before this condemns good hardware intermittently.
 */
#define LF_PROBE_LATE_MS 5000

/* Hourly re-probe. A poll rather than a callback because nothing re-evaluates a
 * satisfied clock request: a crystal that dies at month six leaves every
 * clock_control API returning its day-one value.
 */
#define LF_MONITOR_PERIOD_MS 3600000

/* Consecutive good readings needed to clear a latched fault. A single good gate
 * after a bad one is as likely to be a marginal crystal drifting back through
 * tolerance as a real recovery, and clearing on it would hide the fault from
 * anything polling less often than the probe.
 */
#define LF_FAULT_CLEAR_STREAK 2

/* The monitor gets its own thread because a gate can busy-wait for a second,
 * which has no business on the system work queue. Low priority and preemptible:
 * the captured gate does not care about being preempted, since both boundaries
 * are taken in hardware.
 */
#define LF_MONITOR_STACK_SIZE 1024
#define LF_MONITOR_PRIO       K_PRIO_PREEMPT(10)

/* Lead time between arming the two compare values and the first one firing.
 * One LF tick is 30.5 us, so four is roughly 122 us, comfortably more than two
 * CC register writes even if something preempts between them. Too small and the
 * start boundary is already in the past and never fires.
 */
#define LF_CAPTURE_ARM_TICKS 4

/* DPPI channels for the captured gate, read back from the overlay so the
 * numbers are declared once. The first opens the window, the second closes it.
 */
#define LF_DPPI_CH_START DT_PROP_BY_IDX(DT_NODELABEL(dppic133), sink_channels, 0)
#define LF_DPPI_CH_END   DT_PROP_BY_IDX(DT_NODELABEL(dppic133), sink_channels, 1)

/* rtc130 CC channels publishing the two boundaries, and the timer130 CC
 * channels capturing them. Both peripherals are also bound to Zephyr counter
 * devices, which hand these same channels out for alarms, so the probe sets no
 * alarms and owns these outright. rtc130 has cc-num = 4 and timer130 cc-num = 6,
 * so nothing else is squeezed out.
 */
#define LF_RTC_CC_START   0
#define LF_RTC_CC_END     1
#define LF_TIMER_CC_START 0
#define LF_TIMER_CC_END   1

/** @brief Cycle-to-cycle spread across one run of short gates. */
struct lf_spread {
	/** Gates that completed. */
	uint32_t samples;
	/** Mean HF count across the run. */
	uint32_t hf_mean;
	/** Mean absolute deviation in HF ticks, truncated. */
	uint32_t hf_mad;
	/** Largest minus smallest HF count. */
	uint32_t hf_range;
	/** Deviation against the expected count, in ppm. */
	uint32_t mad_ppm;
	/** Range against the expected count, in ppm. */
	uint32_t range_ppm;
};

/** @brief What the probe concluded about LFCLK. */
enum lf_verdict {
	/** Ratio within tolerance. A crystal is present and running. */
	LF_OK,
	/** Ratio off by more than LF_PPM_REJECT. Running on an RC source. */
	LF_WRONG_SRC,
	/** Gate never completed. LFCLK is not advancing at all. */
	LF_ABSENT,
};

#endif /* LF_PROBE_H_ */
