/*
 * Copyright (c) 2026 Chrispine Tinega <dev@chrispinetinega.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The smallest thing that proves the toolchain, the board and the boot path.
 *
 * The board target am62x_m4_bl350/am6254/m4 comes from mainline Zephyr
 * (zephyrproject-rtos/zephyr#113302), so this builds with no fork and no
 * BOARD_ROOT.
 *
 * Output goes to the RAM console, not a UART. Read it from Linux with:
 *   cat /sys/kernel/debug/remoteproc/<M4F>/trace0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hello, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	LOG_INF("Hello World from Zephyr on the Cortex-M4F!");
	LOG_INF("board: " CONFIG_BOARD_TARGET);

	for (unsigned int tick = 1;; tick++) {
		k_sleep(K_SECONDS(5));
		LOG_INF("still alive — tick %u, uptime %lld ms", tick,
			k_uptime_get());
	}

	return 0;
}
