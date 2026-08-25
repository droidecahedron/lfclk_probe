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
#define LF_GATE_TICKS_LONG  32768 /* 1 s, expected 16000000 HF ticks exactly */
#define LF_GATE_TICKS_SHORT 4096  /* 125 ms, expected 2000000 HF ticks exactly */

/* Repeats per gate length. Enough to see the spread that separates a software
 * gate from a hardware-captured one, without turning boot into a long stall.
 */
#define LF_GATE_REPEATS 5

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

#endif /* LF_PROBE_H_ */
