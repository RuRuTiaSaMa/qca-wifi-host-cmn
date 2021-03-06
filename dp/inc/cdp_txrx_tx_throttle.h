/*
 * Copyright (c) 2016-2017 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */
/**
 * @file cdp_txrx_tx_throttle.h
 * @brief Define the host data path transmit throttle API
 * functions called by the host control SW and the OS interface
 * module
 */
#ifndef _CDP_TXRX_TX_THROTTLE_H_
#define _CDP_TXRX_TX_THROTTLE_H_
#include <cdp_txrx_ops.h>
#include "cdp_txrx_handle.h"
/**
 * cdp_throttle_init_period() - init tx throttle period
 * @soc: data path soc handle
 * @pdev: physical device instance
 * @period: throttle period
 * @dutycycle_level: duty cycle level
 *
 * Return: NONE
 */
static inline void
cdp_throttle_init_period(ol_txrx_soc_handle soc, struct cdp_pdev *pdev,
		int period, uint8_t *dutycycle_level)
{
	if (!soc || !soc->ops || !soc->ops->throttle_ops) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_FATAL,
			"%s invalid instance", __func__);
		return;
	}

	if (soc->ops->throttle_ops->throttle_init_period)
		return soc->ops->throttle_ops->throttle_init_period(pdev,
				period, dutycycle_level);
	return;
}

/**
 * cdp_throttle_init_period() - init tx throttle period
 * @soc: data path soc handle
 * @pdev: physical device instance
 * @level: throttle level
 *
 * Return: NONE
 */
static inline void
cdp_throttle_set_level(ol_txrx_soc_handle soc, struct cdp_pdev *pdev, int level)
{
	if (!soc || !soc->ops || !soc->ops->throttle_ops) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_FATAL,
			"%s invalid instance", __func__);
		return;
	}

	if (soc->ops->throttle_ops->throttle_set_level)
		return soc->ops->throttle_ops->throttle_set_level(pdev, level);
	return;
}

#endif /* _CDP_TXRX_TX_THROTTLE_H_ */
