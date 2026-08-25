/*
 * Copyright (c) 2026 <Nordic Semi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief LFCLK health probe for nRF54H20 — scaffolding.
 *
 * Detects whether LFCLK is actually running off the LFXO by counting it
 * against an HFXO-derived reference. Nothing in the clock stack reports a
 * dead crystal: the resolved accuracy from clock_control comes from BICR,
 * which a broken crystal does not edit.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/logging/log.h>

#include "lf_probe.h"

LOG_MODULE_REGISTER(lf_probe, LOG_LEVEL_INF);

#define LFCLK_NODE DT_NODELABEL(lfclk)

/* LF side counts the clock under test, HF side is the reference. Both are
 * enabled in boards/nrf54h20dk_nrf54h20_cpuapp.overlay, which is where the
 * instance choice is justified.
 */
#define LF_COUNTER_NODE DT_NODELABEL(rtc130)
#define HF_COUNTER_NODE DT_NODELABEL(timer130)

/* timer130's clock parent, and the crystal that parent bypasses to. */
#define FLL16M_NODE DT_NODELABEL(fll16m)
#define HFXO_NODE   DT_NODELABEL(hfxo)

BUILD_ASSERT(DT_NODE_EXISTS(LFCLK_NODE), "no lfclk node, wrong SoC?");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LF_COUNTER_NODE), "rtc130 disabled, board overlay missing?");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(HF_COUNTER_NODE), "timer130 disabled, board overlay missing?");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(FLL16M_NODE), "fll16m disabled, board overlay missing?");

/* What a conditioned reference is supposed to resolve to. FLL16M has exactly
 * two options on this part: open loop at open-loop-accuracy-ppm (20000 on this
 * SoC), and bypass, which routes HFXO straight through at the hfxo node's
 * accuracy-ppm. See clock_options[] in
 * zephyr/drivers/clock_control/clock_control_nrf_fll16m.c.
 */
#define LF_REF_ACCURACY_PPM DT_PROP(HFXO_NODE, accuracy_ppm)

/* Bounds a lost request, nothing more. The real ramp is whatever
 * get_startup_time() reports, which in bypass mode is HFXO's startup time out
 * of BICR.
 */
#define LF_REF_REQUEST_TIMEOUT_MS 500

static const struct device *const lf_counter = DEVICE_DT_GET(LF_COUNTER_NODE);
static const struct device *const hf_counter = DEVICE_DT_GET(HF_COUNTER_NODE);
static const struct device *const lf_ref_clock = DEVICE_DT_GET(FLL16M_NODE);

/* accuracy here is a sentinel, not a ppm figure: ACCURACY_MAX asks the driver
 * for its best option instead of making this file guess a number. precision has
 * to stay DEFAULT, fll16m_resolve_spec_to_idx() rejects anything else with
 * -EINVAL.
 */
static const struct nrf_clock_spec lf_ref_spec = {
	.frequency = DT_PROP(FLL16M_NODE, clock_frequency),
	.accuracy = NRF_CLOCK_CONTROL_ACCURACY_MAX,
	.precision = NRF_CLOCK_CONTROL_PRECISION_DEFAULT,
};

/* Condition the reference and report what the board actually gave back. Kept
 * separate from the release so the caller can hold it across a gate only,
 * because bypass mode keeps HFXO running and that costs current.
 */
static int lf_ref_acquire(void)
{
	struct nrf_clock_spec resolved;
	uint32_t startup_time_us;
	int err;

	err = nrf_clock_control_resolve(lf_ref_clock, &lf_ref_spec, &resolved);
	if (err) {
		LOG_ERR("fll16m resolve failed (err %d)", err);
		return err;
	}

	err = nrf_clock_control_get_startup_time(lf_ref_clock, &lf_ref_spec, &startup_time_us);
	if (err) {
		LOG_ERR("fll16m get_startup_time failed (err %d)", err);
		return err;
	}

	LOG_INF("fll16m resolved    : %u Hz, %u ppm, precision %u, %u us startup",
		resolved.frequency, resolved.accuracy, resolved.precision, startup_time_us);

	/* Config oracle, and it costs no measurement. An accuracy worse than the
	 * hfxo node's own figure means the request landed on open loop at 20000
	 * ppm, which is not a reference. A wrong BICR and a board variant with no
	 * 32 MHz crystal both come out here.
	 */
	if (resolved.accuracy > LF_REF_ACCURACY_PPM) {
		LOG_ERR("reference resolved %u ppm, board declares %u ppm: BICR or board variant?",
			resolved.accuracy, LF_REF_ACCURACY_PPM);
		return -ENOTSUP;
	}

	err = nrf_clock_control_request_sync(lf_ref_clock, &lf_ref_spec,
					    K_MSEC(LF_REF_REQUEST_TIMEOUT_MS));
	if (err) {
		LOG_ERR("fll16m request failed (err %d)", err);
		return err;
	}

	return 0;
}

struct lf_measurement {
	uint32_t lf_ticks; /* LF ticks the gate actually spanned */
	uint32_t hf_ticks; /* HF ticks counted across the same window */
	uint32_t lf_hz;    /* LF frequency implied by the ratio */
	int32_t ppm;       /* signed error of lf_hz against LF_NOMINAL_HZ */
};

/* Counters wrap at their top value, which is not always 2^n - 1, so the span
 * has to close modulo top + 1 rather than by masking. The 64 bit intermediate
 * is there because top is 0xFFFFFFFF on timer130 and top + 1 would overflow.
 */
static uint32_t counter_delta(uint32_t top, uint32_t start, uint32_t now)
{
	if (now >= start) {
		return now - start;
	}

	return (uint32_t)((uint64_t)now + (uint64_t)top + 1U - (uint64_t)start);
}

/* Software gate. Polls the LF counter and reads the HF counter the moment the
 * window closes, so the error is one pair of register reads instead of a sleep
 * granularity. The busy wait is deliberate: k_sleep() parks on the system
 * timer, and GRTC SYSCOUNTER falls back to LFCLK while asleep, which is the
 * clock under test. Thread and ISR jitter still land inside the measurement,
 * so 100 us of it over a 1 s gate is 100 ppm of error.
 */
static int lf_gate_measure(uint32_t gate_ticks, struct lf_measurement *out)
{
	const uint32_t lf_top = counter_get_top_value(lf_counter);
	const uint32_t hf_top = counter_get_top_value(hf_counter);
	const uint32_t hf_hz = counter_get_frequency(hf_counter);
	uint32_t lf_start, lf_now, hf_start, hf_now;
	uint32_t lf_delta;
	int64_t deadline;
	uint64_t expected_hf;
	int err;

	err = counter_get_value(lf_counter, &lf_start);
	if (err) {
		return err;
	}

	err = counter_get_value(hf_counter, &hf_start);
	if (err) {
		return err;
	}

	/* The gate's own expected duration plus the margin. k_uptime_get() is
	 * safe to lean on with LFCLK stopped because SYSCOUNTER runs off the
	 * 16 MHz clock whenever the core is awake, and this loop never sleeps.
	 */
	deadline = k_uptime_get() +
		   ((int64_t)gate_ticks * MSEC_PER_SEC) / LF_NOMINAL_HZ +
		   LF_CAPTURE_TIMEOUT_MS;

	do {
		if (k_uptime_get() > deadline) {
			return -ETIMEDOUT;
		}

		err = counter_get_value(lf_counter, &lf_now);
		if (err) {
			return err;
		}

		lf_delta = counter_delta(lf_top, lf_start, lf_now);
	} while (lf_delta < gate_ticks);

	err = counter_get_value(hf_counter, &hf_now);
	if (err) {
		return err;
	}

	out->lf_ticks = lf_delta;
	out->hf_ticks = counter_delta(hf_top, hf_start, hf_now);

	/* A stalled HF side makes the ratio meaningless rather than merely
	 * wrong, so it is worth separating from a bad reading.
	 */
	if (out->hf_ticks == 0) {
		return -EIO;
	}

	/* lf_ticks * hf_hz reaches 32768 * 16e6, so 64 bits is not optional. */
	out->lf_hz = (uint32_t)(((uint64_t)out->lf_ticks * hf_hz) / out->hf_ticks);

	expected_hf = ((uint64_t)out->lf_ticks * hf_hz) / LF_NOMINAL_HZ;
	if (expected_hf == 0) {
		return -EIO;
	}

	/* Compared as HF counts rather than as frequencies so there is only one
	 * division and one rounding. A slow LFCLK spends more HF ticks inside
	 * the same number of LF ticks, which comes out negative.
	 */
	out->ppm = (int32_t)((((int64_t)expected_hf - (int64_t)out->hf_ticks) *
			      1000000) / (int64_t)expected_hf);

	return 0;
}

static void lf_gate_report(uint32_t gate_ticks)
{
	struct lf_measurement meas;
	int err;

	for (int i = 0; i < LF_GATE_REPEATS; i++) {
		err = lf_gate_measure(gate_ticks, &meas);
		if (err == -ETIMEDOUT) {
			LOG_ERR("gate %u: never closed, LFCLK is not advancing", gate_ticks);
			return;
		}

		if (err) {
			LOG_ERR("gate %u: failed (err %d)", gate_ticks, err);
			return;
		}

		LOG_INF("gate %5u LF : %8u HF, %6u Hz, %5d ppm",
			meas.lf_ticks, meas.hf_ticks, meas.lf_hz, meas.ppm);

		if (meas.ppm > LF_PPM_NOISE_FLOOR || meas.ppm < -LF_PPM_NOISE_FLOOR) {
			LOG_WRN("%d ppm is outside the %d ppm reference noise floor",
				meas.ppm, LF_PPM_NOISE_FLOOR);
		}
	}
}

static int lf_ref_release(void)
{
	int err;

	/* Has to be the same spec that was requested. The driver tracks the
	 * attributes, not just a reference count, so a mismatched spec leaks it.
	 * Success is a non-negative onoff state, not zero.
	 */
	err = nrf_clock_control_release(lf_ref_clock, &lf_ref_spec);
	if (err < 0) {
		LOG_ERR("fll16m release failed (err %d)", err);
		return err;
	}

	return 0;
}

int main(void)
{
	int err;

	LOG_INF("lf_probe on %s", CONFIG_BOARD_TARGET);

	/* Declared modes, straight from nrf54h20.dtsi. There is deliberately
	 * no lfxo row here : the LFXO node carries no accuracy property
	 * the 20ppm seen resolved by clock_control originates in BICR.
	 */
	LOG_INF("lfclk nominal      : %u Hz", DT_PROP(LFCLK_NODE, clock_frequency));
	LOG_INF("lfrc   declared    : %u ppm, %u us startup",
		DT_PROP(LFCLK_NODE, lfrc_accuracy_ppm),
		DT_PROP(LFCLK_NODE, lfrc_startup_time_us));
	LOG_INF("lflprc declared    : %u ppm, %u us startup",
		DT_PROP(LFCLK_NODE, lflprc_accuracy_ppm),
		DT_PROP(LFCLK_NODE, lflprc_startup_time_us));
		// TODO: BICR mod

	if (!device_is_ready(lf_counter)) {
		LOG_ERR("%s not ready", lf_counter->name);
		return -ENODEV;
	}

	if (!device_is_ready(hf_counter)) {
		LOG_ERR("%s not ready", hf_counter->name);
		return -ENODEV;
	}

	/* Nominals, not measurements. counter_get_frequency() returns what the
	 * driver derived from the node's clock parent and prescaler at build
	 * time, so the HF figure is FLL16M's declared rate rather than its
	 * current one. FLL16M is power optimised until accuracy is requested on
	 * it, so this reference is not yet trustworthy.
	 */
	LOG_INF("lf counter %s : %u Hz", lf_counter->name, counter_get_frequency(lf_counter));
	LOG_INF("hf counter %s : %u Hz", hf_counter->name, counter_get_frequency(hf_counter));

	err = counter_start(lf_counter);
	if (err) {
		LOG_ERR("%s start failed (err %d)", lf_counter->name, err);
		return err;
	}

	err = counter_start(hf_counter);
	if (err) {
		LOG_ERR("%s start failed (err %d)", hf_counter->name, err);
		return err;
	}

	err = lf_ref_acquire();
	if (err) {
		return err;
	}

	lf_gate_report(LF_GATE_TICKS_LONG);
	lf_gate_report(LF_GATE_TICKS_SHORT);

	/* Released whatever the gates reported. Holding HFXO past the
	 * measurement only burns current.
	 */
	err = lf_ref_release();
	if (err) {
		return err;
	}

	LOG_INF("fll16m released");

	return 0;
}