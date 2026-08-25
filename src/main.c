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

#include <nrfx.h>
#include <hal/nrf_dppi.h>
#include <hal/nrf_rtc.h>
#include <hal/nrf_timer.h>

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

/* Hardware-gated variant. rtc130 COMPARE publishes to timer130 CAPTURE through
 * DPPI, so both boundaries land on exact LF edges with the CPU uninvolved and
 * the read-pair latency that biases the software gate disappears.
 *
 * Two channels rather than one: a single channel would drive both capture tasks
 * from both compare events. Channel LF_DPPI_CH_START opens the window,
 * LF_DPPI_CH_END closes it, and the HF span is one CC subtraction.
 */
static void lf_capture_setup(void)
{
	/* Publish and subscribe carry a channel index plus an enable bit. The
	 * PPIB side is already configured by IronSide SE from the UICR
	 * periphconf the overlay generates, and cannot be touched from here:
	 * only the secure core may set PPIB direction on this part.
	 */
	/* RTC is not TIMER here: an RTC compare event is only generated when the
	 * matching EVTEN bit is set, so without this the compare matches, nothing
	 * fires, and the gate times out looking healthy. The Zephyr counter
	 * driver sets EVTEN only when it hands out an alarm, and the probe sets
	 * no alarms.
	 */
	nrf_rtc_event_enable(NRF_RTC130,
			     NRF_RTC_CHANNEL_INT_MASK(LF_RTC_CC_START) |
			     NRF_RTC_CHANNEL_INT_MASK(LF_RTC_CC_END));

	nrf_rtc_publish_set(NRF_RTC130, NRF_RTC_EVENT_COMPARE_0, LF_DPPI_CH_START);
	nrf_rtc_publish_set(NRF_RTC130, NRF_RTC_EVENT_COMPARE_1, LF_DPPI_CH_END);

	nrf_timer_subscribe_set(NRF_TIMER130, NRF_TIMER_TASK_CAPTURE0, LF_DPPI_CH_START);
	nrf_timer_subscribe_set(NRF_TIMER130, NRF_TIMER_TASK_CAPTURE1, LF_DPPI_CH_END);

	/* Both ends of the bridge have to be enabled. The event crosses
	 * DPPIC130 -> PPIB130 ch 16 -> PPIB134 ch 0 -> DPPIC133, and a channel
	 * left disabled on either side simply drops it.
	 */
	nrf_dppi_channels_enable(NRF_DPPIC130, BIT(LF_DPPI_CH_START) | BIT(LF_DPPI_CH_END));
	nrf_dppi_channels_enable(NRF_DPPIC133, BIT(LF_DPPI_CH_START) | BIT(LF_DPPI_CH_END));
}

static void lf_capture_teardown(void)
{
	nrf_dppi_channels_disable(NRF_DPPIC130, BIT(LF_DPPI_CH_START) | BIT(LF_DPPI_CH_END));
	nrf_dppi_channels_disable(NRF_DPPIC133, BIT(LF_DPPI_CH_START) | BIT(LF_DPPI_CH_END));

	nrf_rtc_event_disable(NRF_RTC130,
			      NRF_RTC_CHANNEL_INT_MASK(LF_RTC_CC_START) |
			      NRF_RTC_CHANNEL_INT_MASK(LF_RTC_CC_END));

	nrf_rtc_publish_clear(NRF_RTC130, NRF_RTC_EVENT_COMPARE_0);
	nrf_rtc_publish_clear(NRF_RTC130, NRF_RTC_EVENT_COMPARE_1);

	nrf_timer_subscribe_clear(NRF_TIMER130, NRF_TIMER_TASK_CAPTURE0);
	nrf_timer_subscribe_clear(NRF_TIMER130, NRF_TIMER_TASK_CAPTURE1);
}

static int lf_capture_measure(uint32_t gate_ticks, struct lf_measurement *out)
{
	uint32_t start_cc, end_cc, hf_start, hf_end;
	int64_t deadline;
	uint64_t expected_hf;

	nrf_rtc_event_clear(NRF_RTC130, NRF_RTC_EVENT_COMPARE_0);
	nrf_rtc_event_clear(NRF_RTC130, NRF_RTC_EVENT_COMPARE_1);

	/* Both boundaries armed before either fires, so the window length is
	 * exactly gate_ticks LF ticks regardless of when the CPU gets around to
	 * looking. LF_CAPTURE_ARM_TICKS of lead time keeps the first compare in
	 * the future while these writes land.
	 */
	start_cc = nrf_rtc_counter_get(NRF_RTC130) + LF_CAPTURE_ARM_TICKS;
	end_cc = start_cc + gate_ticks;

	nrf_rtc_cc_set(NRF_RTC130, LF_RTC_CC_START, start_cc & NRF_RTC_COUNTER_MAX);
	nrf_rtc_cc_set(NRF_RTC130, LF_RTC_CC_END, end_cc & NRF_RTC_COUNTER_MAX);

	/* Same reasoning as the software gate: never sleep, so k_uptime_get()
	 * keeps running off the 16 MHz clock even with LFCLK stopped.
	 */
	deadline = k_uptime_get() +
		   ((int64_t)(gate_ticks + LF_CAPTURE_ARM_TICKS) * MSEC_PER_SEC) / LF_NOMINAL_HZ +
		   LF_CAPTURE_TIMEOUT_MS;

	while (!nrf_rtc_event_check(NRF_RTC130, NRF_RTC_EVENT_COMPARE_1)) {
		if (k_uptime_get() > deadline) {
			return -ETIMEDOUT;
		}
	}

	/* A closing capture without an opening one means the bridge is passing
	 * nothing on the start channel, which is a wiring fault rather than a
	 * clock fault and must not be reported as a ppm figure.
	 */
	if (!nrf_rtc_event_check(NRF_RTC130, NRF_RTC_EVENT_COMPARE_0)) {
		return -EIO;
	}

	hf_start = nrf_timer_cc_get(NRF_TIMER130, LF_TIMER_CC_START);
	hf_end = nrf_timer_cc_get(NRF_TIMER130, LF_TIMER_CC_END);

	out->lf_ticks = gate_ticks;
	out->hf_ticks = counter_delta(counter_get_top_value(hf_counter), hf_start, hf_end);

	if (out->hf_ticks == 0) {
		return -EIO;
	}

	out->lf_hz = (uint32_t)(((uint64_t)out->lf_ticks *
				 counter_get_frequency(hf_counter)) / out->hf_ticks);

	expected_hf = ((uint64_t)out->lf_ticks *
		       counter_get_frequency(hf_counter)) / LF_NOMINAL_HZ;
	if (expected_hf == 0) {
		return -EIO;
	}

	out->ppm = (int32_t)((((int64_t)expected_hf - (int64_t)out->hf_ticks) *
			      1000000) / (int64_t)expected_hf);

	return 0;
}

/* Both variants report through here so the two are directly comparable on the
 * bench, which is the only way to see whether the capture actually tightened
 * anything.
 */
static void lf_gate_report(const char *label, uint32_t gate_ticks,
			   int (*measure)(uint32_t, struct lf_measurement *))
{
	struct lf_measurement meas;
	int err;

	for (int i = 0; i < LF_GATE_REPEATS; i++) {
		err = measure(gate_ticks, &meas);
		if (err == -ETIMEDOUT) {
			LOG_ERR("%s %u: never closed, LFCLK is not advancing", label, gate_ticks);
			return;
		}

		if (err) {
			LOG_ERR("%s %u: failed (err %d)", label, gate_ticks, err);
			return;
		}

		LOG_INF("%s %5u LF : %8u HF, %6u Hz, %5d ppm",
			label, meas.lf_ticks, meas.hf_ticks, meas.lf_hz, meas.ppm);

		if (meas.ppm > LF_PPM_NOISE_FLOOR || meas.ppm < -LF_PPM_NOISE_FLOOR) {
			LOG_WRN("%d ppm is outside the %d ppm reference noise floor",
				meas.ppm, LF_PPM_NOISE_FLOOR);
		}
	}
}

/* Mean frequency cannot see a calibrated LFRC. sysctrl keeps it within a few
 * hundred ppm and at room temperature it can sit inside the noise floor, so the
 * absolute measurement passes it. Cycle-to-cycle spread can see it: calibration
 * corrects the average and cannot correct the jitter.
 *
 * The reference's own bias is common mode across every sample and cancels in
 * the deviation, so this is tighter than the absolute figure despite each gate
 * being 1000 times shorter.
 *
 * Mean absolute deviation rather than variance: no squaring, so no risk of
 * overflow and no temptation to reach for a square root. All integer.
 */
static int lf_spread_measure(struct lf_spread *out)
{
	static uint32_t hf[LF_JITTER_SAMPLES];
	struct lf_measurement meas;
	uint64_t sum = 0;
	uint64_t mad_sum = 0;
	uint64_t expected_hf;
	uint32_t lo = UINT32_MAX;
	uint32_t hi = 0;
	int err;

	for (int i = 0; i < LF_JITTER_SAMPLES; i++) {
		err = lf_capture_measure(LF_GATE_TICKS_JITTER, &meas);
		if (err) {
			return err;
		}

		hf[i] = meas.hf_ticks;
		sum += hf[i];
		lo = MIN(lo, hf[i]);
		hi = MAX(hi, hf[i]);
	}

	out->samples = LF_JITTER_SAMPLES;
	out->hf_mean = (uint32_t)(sum / LF_JITTER_SAMPLES);
	out->hf_range = hi - lo;

	for (int i = 0; i < LF_JITTER_SAMPLES; i++) {
		mad_sum += (hf[i] > out->hf_mean) ? (hf[i] - out->hf_mean)
						  : (out->hf_mean - hf[i]);
	}

	out->hf_mad = (uint32_t)(mad_sum / LF_JITTER_SAMPLES);

	/* Against the expected count, not the measured mean, so a clock that is
	 * off frequency does not also shift its own spread figure.
	 */
	expected_hf = ((uint64_t)LF_GATE_TICKS_JITTER *
		       counter_get_frequency(hf_counter)) / LF_NOMINAL_HZ;
	if (expected_hf == 0) {
		return -EIO;
	}

	/* From mad_sum rather than the truncated hf_mad, which keeps sub-tick
	 * resolution: one HF tick in a 32 tick gate is already 64 ppm.
	 */
	out->mad_ppm = (uint32_t)((mad_sum * 1000000) /
				  (expected_hf * LF_JITTER_SAMPLES));
	out->range_ppm = (uint32_t)(((uint64_t)out->hf_range * 1000000) / expected_hf);

	return 0;
}

static const char *lf_verdict_str(enum lf_verdict verdict)
{
	switch (verdict) {
	case LF_OK:
		return "LF_OK";
	case LF_WRONG_SRC:
		return "LF_WRONG_SRC";
	case LF_ABSENT:
		return "LF_ABSENT";
	default:
		return "?";
	}
}

/* Turns a measurement into a state, which is what a caller can act on.
 *
 * LF_ABSENT is not an error path. Whether a dead crystal parks the SoC on LFRC
 * or leaves LFCLK stopped is unconfirmed on this part, so both are reported
 * without assuming which happens.
 *
 * The captured gate is tried first because it is the accurate one, but a
 * capture timeout on its own does not mean the clock is dead: a misconfigured
 * bridge produces the identical -ETIMEDOUT, which is exactly how the missing
 * EVTEN bit presented on the bench. So LF_ABSENT is only returned when the
 * software gate fails too. A capture that times out while the software gate
 * still reads a healthy clock is a fabric fault, and this returns an errno for
 * that rather than condemning good hardware.
 *
 * @retval 0       Verdict determined, written to @p verdict.
 * @retval -EIO    Fabric fault. The clock is running, the capture path is not.
 * @retval negative Other measurement failure.
 */
static int lf_verdict_get(uint32_t gate_ticks, enum lf_verdict *verdict,
			  struct lf_measurement *meas)
{
	int cap_err;
	int soft_err;

	cap_err = lf_capture_measure(gate_ticks, meas);
	if (cap_err == 0) {
		*verdict = (meas->ppm > LF_PPM_REJECT || meas->ppm < -LF_PPM_REJECT)
			 ? LF_WRONG_SRC : LF_OK;
		return 0;
	}

	soft_err = lf_gate_measure(gate_ticks, meas);
	if (soft_err == -ETIMEDOUT) {
		/* Neither gate closed. The clock itself is not advancing. */
		*verdict = LF_ABSENT;
		return 0;
	}

	if (soft_err) {
		return soft_err;
	}

	LOG_ERR("capture failed (err %d) but the software gate read %d ppm: "
		"fabric fault, not a clock fault", cap_err, meas->ppm);

	return -EIO;
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
	struct lf_measurement meas;
	struct lf_spread spread;
	enum lf_verdict verdict;
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

	lf_gate_report("soft", LF_GATE_TICKS_LONG, lf_gate_measure);
	lf_gate_report("soft", LF_GATE_TICKS_SHORT, lf_gate_measure);

	/* Kept alongside the software gate rather than replacing it. The bridge
	 * is the part most likely to be wrong on a board or SoC variant, and a
	 * capture that never fires reports -ETIMEDOUT rather than hanging, so the
	 * software figures stay available as the fallback.
	 */
	lf_capture_setup();
	lf_gate_report("cap ", LF_GATE_TICKS_LONG, lf_capture_measure);
	lf_gate_report("cap ", LF_GATE_TICKS_SHORT, lf_capture_measure);

	err = lf_verdict_get(LF_GATE_TICKS_LONG, &verdict, &meas);
	if (err) {
		LOG_ERR("verdict undetermined (err %d)", err);
	} else if (verdict == LF_ABSENT) {
		LOG_WRN("verdict %s: no gate closed", lf_verdict_str(verdict));
	} else {
		LOG_INF("verdict %s at %d ppm, reject threshold %d ppm",
			lf_verdict_str(verdict), meas.ppm, LF_PPM_REJECT);
	}

	err = lf_spread_measure(&spread);
	if (err) {
		LOG_ERR("spread run failed (err %d)", err);
	} else {
		LOG_INF("spread %u gates of %u LF : mean %u HF, mad %u HF (%u ppm), "
			"range %u HF (%u ppm)",
			spread.samples, LF_GATE_TICKS_JITTER, spread.hf_mean,
			spread.hf_mad, spread.mad_ppm, spread.hf_range, spread.range_ppm);

		if (LF_PPM_SPREAD_REJECT == 0) {
			LOG_WRN("spread threshold unset, reading is recorded not judged");
		} else if (spread.mad_ppm > LF_PPM_SPREAD_REJECT) {
			LOG_WRN("spread %u ppm exceeds %d ppm: calibrated RC rather than a crystal",
				spread.mad_ppm, LF_PPM_SPREAD_REJECT);
		}
	}

	lf_capture_teardown();

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