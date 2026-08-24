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
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lf_probe, LOG_LEVEL_INF);

#define LFCLK_NODE DT_NODELABEL(lfclk)

/* LF side counts the clock under test, HF side is the reference. Both are
 * enabled in boards/nrf54h20dk_nrf54h20_cpuapp.overlay, which is where the
 * instance choice is justified.
 */
#define LF_COUNTER_NODE DT_NODELABEL(rtc130)
#define HF_COUNTER_NODE DT_NODELABEL(timer130)

BUILD_ASSERT(DT_NODE_EXISTS(LFCLK_NODE), "no lfclk node, wrong SoC?");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LF_COUNTER_NODE), "rtc130 disabled, board overlay missing?");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(HF_COUNTER_NODE), "timer130 disabled, board overlay missing?");

static const struct device *const lf_counter = DEVICE_DT_GET(LF_COUNTER_NODE);
static const struct device *const hf_counter = DEVICE_DT_GET(HF_COUNTER_NODE);

int main(void)
{
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

	return 0;
}