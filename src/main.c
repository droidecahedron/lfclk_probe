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
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lf_probe, LOG_LEVEL_INF);

#define LFCLK_NODE DT_NODELABEL(lfclk)

BUILD_ASSERT(DT_NODE_EXISTS(LFCLK_NODE), "no lfclk node — wrong SoC?");

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

	return 0;
}