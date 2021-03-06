/*
 * Copyright (c) 2015-2017 The Linux Foundation. All rights reserved.
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
#include <cds_api.h>

/* OS abstraction libraries */
#include <qdf_nbuf.h>           /* qdf_nbuf_t, etc. */
#include <qdf_atomic.h>         /* qdf_atomic_read, etc. */
#include <qdf_util.h>           /* qdf_unlikely */
#include "dp_types.h"
#include "dp_tx_desc.h"

#include <cdp_txrx_handle.h>
#include "dp_internal.h"
#define INVALID_FLOW_ID 0xFF
#define MAX_INVALID_BIN 3

/**
 * dp_tx_dump_flow_pool_info() - dump global_pool and flow_pool info
 *
 * @ctx: Handle to struct dp_soc.
 *
 * Return: none
 */
void dp_tx_dump_flow_pool_info(void *ctx)
{
	struct dp_soc *soc = ctx;
	struct dp_txrx_pool_stats *pool_stats = &soc->pool_stats;
	struct dp_tx_desc_pool_s *pool = NULL;
	struct dp_tx_desc_pool_s tmp_pool;
	int i;

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		"No of pool map received %d", pool_stats->pool_map_count);
	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		"No of pool unmap received %d",	pool_stats->pool_unmap_count);
	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		"Pkt dropped due to unavailablity of pool %d",
		pool_stats->pkt_drop_no_pool);

	/*
	 * Nested spin lock.
	 * Always take in below order.
	 * flow_pool_array_lock -> flow_pool_lock
	 */
	qdf_spin_lock_bh(&soc->flow_pool_array_lock);
	for (i = 0; i < MAX_TXDESC_POOLS; i++) {
		pool = &soc->tx_desc[i];
		if (pool->status > FLOW_POOL_INVALID)
			continue;
		qdf_spin_lock_bh(&pool->flow_pool_lock);
		qdf_mem_copy(&tmp_pool, pool, sizeof(tmp_pool));
		qdf_spin_unlock_bh(&pool->flow_pool_lock);
		qdf_spin_unlock_bh(&soc->flow_pool_array_lock);
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR, "\n");
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"Flow_pool_id %d :: status %d",
			tmp_pool.flow_pool_id, tmp_pool.status);
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"Total %d :: Available %d",
			tmp_pool.pool_size, tmp_pool.avail_desc);
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"Start threshold %d :: Stop threshold %d",
			 tmp_pool.start_th, tmp_pool.stop_th);
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"Member flow_id  %d :: flow_type %d",
			tmp_pool.flow_pool_id, tmp_pool.flow_type);
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"Pkt dropped due to unavailablity of descriptors %d",
			tmp_pool.pkt_drop_no_desc);
		qdf_spin_lock_bh(&soc->flow_pool_array_lock);
	}
	qdf_spin_unlock_bh(&soc->flow_pool_array_lock);
}

/**
 * dp_tx_clear_flow_pool_stats() - clear flow pool statistics
 *
 * @soc: Handle to struct dp_soc.
 *
 * Return: None
 */
void dp_tx_clear_flow_pool_stats(struct dp_soc *soc)
{

	if (!soc) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"%s: soc is null\n", __func__);
		return;
	}
	qdf_mem_zero(&soc->pool_stats, sizeof(soc->pool_stats));
}

/**
 * dp_tx_create_flow_pool() - create flow pool
 * @soc: Handle to struct dp_soc
 * @flow_pool_id: flow pool id
 * @flow_pool_size: flow pool size
 *
 * Return: flow_pool pointer / NULL for error
 */
struct dp_tx_desc_pool_s *dp_tx_create_flow_pool(struct dp_soc *soc,
	uint8_t flow_pool_id, uint16_t flow_pool_size)
{
	struct dp_tx_desc_pool_s *pool;
	uint32_t stop_threshold;
	uint32_t start_threshold;

	if (!soc) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		   "%s: soc is NULL\n", __func__);
		return NULL;
	}
	pool = &soc->tx_desc[flow_pool_id];
	qdf_spin_lock_bh(&pool->flow_pool_lock);
	if (pool->status == FLOW_POOL_INVALID) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		   "%s: flow pool already allocated\n", __func__);
		if (pool->avail_desc > pool->start_th)
			pool->status = FLOW_POOL_ACTIVE_UNPAUSED;
		else
			pool->status = FLOW_POOL_ACTIVE_PAUSED;
		qdf_spin_unlock_bh(&pool->flow_pool_lock);
		return pool;
	}

	if (dp_tx_desc_pool_alloc(soc, flow_pool_id, flow_pool_size)) {
		qdf_spin_unlock_bh(&pool->flow_pool_lock);
		return NULL;
	}

	stop_threshold = wlan_cfg_get_tx_flow_stop_queue_th(soc->wlan_cfg_ctx);
	start_threshold = stop_threshold +
		wlan_cfg_get_tx_flow_start_queue_offset(soc->wlan_cfg_ctx);

	pool->flow_pool_id = flow_pool_id;
	pool->pool_size = flow_pool_size;
	pool->avail_desc = flow_pool_size;
	pool->status = FLOW_POOL_ACTIVE_UNPAUSED;
	/* INI is in percentage so divide by 100 */
	pool->start_th = (start_threshold * flow_pool_size)/100;
	pool->stop_th = (stop_threshold * flow_pool_size)/100;

	qdf_spin_unlock_bh(&pool->flow_pool_lock);

	return pool;
}

/**
 * dp_tx_delete_flow_pool() - delete flow pool
 * @soc: Handle to struct dp_soc
 * @pool: flow pool pointer
 * @force: free pool forcefully
 *
 * Delete flow_pool if all tx descriptors are available.
 * Otherwise put it in FLOW_POOL_INVALID state.
 * If force is set then pull all available descriptors to
 * global pool.
 *
 * Return: 0 for success or error
 */
int dp_tx_delete_flow_pool(struct dp_soc *soc, struct dp_tx_desc_pool_s *pool,
	bool force)
{
	if (!soc || !pool) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		   "%s: pool or soc is NULL\n", __func__);
		QDF_ASSERT(0);
		return ENOMEM;
	}

	qdf_spin_lock_bh(&pool->flow_pool_lock);
	if (pool->avail_desc < pool->pool_size) {
		pool->status = FLOW_POOL_INVALID;
		qdf_spin_unlock_bh(&pool->flow_pool_lock);
		return EAGAIN;
	}

	/* We have all the descriptors for the pool, we can delete the pool */
	dp_tx_desc_pool_free(soc, pool->flow_pool_id);
	qdf_spin_unlock_bh(&pool->flow_pool_lock);
	return 0;
}

/**
 * dp_tx_flow_pool_vdev_map() - Map flow_pool with vdev
 * @pdev: Handle to struct dp_pdev
 * @pool: flow_pool
 * @vdev_id: flow_id /vdev_id
 *
 * Return: none
 */
static void dp_tx_flow_pool_vdev_map(struct dp_pdev *pdev,
	struct dp_tx_desc_pool_s *pool, uint8_t vdev_id)
{
	struct dp_vdev *vdev;
	struct dp_soc *soc = pdev->soc;

	vdev = (struct dp_vdev *)cdp_get_vdev_from_vdev_id((void *)soc,
					(struct cdp_pdev *)pdev, vdev_id);
	if (!vdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		   "%s: invalid vdev_id %d\n",
		   __func__, vdev_id);
		return;
	}

	vdev->pool = pool;
	qdf_spin_lock_bh(&pool->flow_pool_lock);
	pool->pool_owner_ctx = soc;
	pool->flow_pool_id = vdev_id;
	qdf_spin_unlock_bh(&pool->flow_pool_lock);
}

/**
 * dp_tx_flow_pool_vdev_unmap() - Unmap flow_pool from vdev
 * @pdev: Handle to struct dp_pdev
 * @pool: flow_pool
 * @vdev_id: flow_id /vdev_id
 *
 * Return: none
 */
static void dp_tx_flow_pool_vdev_unmap(struct dp_pdev *pdev,
		struct dp_tx_desc_pool_s *pool, uint8_t vdev_id)
{
	struct dp_vdev *vdev;
	struct dp_soc *soc = pdev->soc;

	vdev = (struct dp_vdev *)cdp_get_vdev_from_vdev_id((void *)soc,
					(struct cdp_pdev *)pdev, vdev_id);
	if (!vdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		   "%s: invalid vdev_id %d\n",
		   __func__, vdev_id);
		return;
	}

	vdev->pool = NULL;
	qdf_spin_lock_bh(&pool->flow_pool_lock);
	pool->flow_pool_id = INVALID_FLOW_ID;
	qdf_spin_unlock_bh(&pool->flow_pool_lock);
}

/**
 * dp_tx_flow_pool_map_handler() - Map flow_id with pool of descriptors
 * @pdev: Handle to struct dp_pdev
 * @flow_id: flow id
 * @flow_type: flow type
 * @flow_pool_id: pool id
 * @flow_pool_size: pool size
 *
 * Process below target to host message
 * HTT_T2H_MSG_TYPE_FLOW_POOL_MAP
 *
 * Return: none
 */
QDF_STATUS dp_tx_flow_pool_map_handler(struct dp_pdev *pdev, uint8_t flow_id,
	uint8_t flow_type, uint8_t flow_pool_id, uint16_t flow_pool_size)
{
	struct dp_soc *soc = pdev->soc;
	struct dp_tx_desc_pool_s *pool;
	enum htt_flow_type type = flow_type;


	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
		"%s: flow_id %d flow_type %d flow_pool_id %d flow_pool_size %d\n",
		__func__, flow_id, flow_type, flow_pool_id, flow_pool_size);

	if (qdf_unlikely(!soc)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"%s: soc is NULL", __func__);
		return QDF_STATUS_E_FAULT;
	}
	soc->pool_stats.pool_map_count++;

	pool = dp_tx_create_flow_pool(soc, flow_pool_id,
			flow_pool_size);
	if (pool == NULL) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			   "%s: creation of flow_pool %d size %d failed\n",
			   __func__, flow_pool_id, flow_pool_size);
		return QDF_STATUS_E_RESOURCES;
	}

	switch (type) {

	case FLOW_TYPE_VDEV:
		dp_tx_flow_pool_vdev_map(pdev, pool, flow_id);
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		   "%s: flow type %d not supported !!!\n",
		   __func__, type);
		break;
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_tx_flow_pool_unmap_handler() - Unmap flow_id from pool of descriptors
 * @pdev: Handle to struct dp_pdev
 * @flow_id: flow id
 * @flow_type: flow type
 * @flow_pool_id: pool id
 *
 * Process below target to host message
 * HTT_T2H_MSG_TYPE_FLOW_POOL_UNMAP
 *
 * Return: none
 */
void dp_tx_flow_pool_unmap_handler(struct dp_pdev *pdev, uint8_t flow_id,
	uint8_t flow_type, uint8_t flow_pool_id)
{
	struct dp_soc *soc = pdev->soc;
	struct dp_tx_desc_pool_s *pool;
	enum htt_flow_type type = flow_type;

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
		"%s: flow_id %d flow_type %d flow_pool_id %d\n",
		__func__, flow_id, flow_type, flow_pool_id);

	if (qdf_unlikely(!pdev)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"%s: pdev is NULL", __func__);
		return;
	}
	soc->pool_stats.pool_unmap_count++;

	pool = &soc->tx_desc[flow_pool_id];
	if (!pool) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		   "%s: flow_pool not available flow_pool_id %d\n",
		   __func__, type);
		return;
	}

	switch (type) {

	case FLOW_TYPE_VDEV:
		dp_tx_flow_pool_vdev_unmap(pdev, pool, flow_id);
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
		   "%s: flow type %d not supported !!!\n",
		   __func__, type);
		return;
	}

	/* only delete if all descriptors are available */
	dp_tx_delete_flow_pool(soc, pool, false);
}

/**
 * dp_tx_flow_control_init() - Initialize tx flow control
 * @tx_desc_pool: Handle to flow_pool
 *
 * Return: none
 */
void dp_tx_flow_control_init(struct dp_soc *soc)
{
	qdf_spinlock_create(&soc->flow_pool_array_lock);
}

/**
 * dp_tx_flow_control_deinit() - Deregister fw based tx flow control
 * @tx_desc_pool: Handle to flow_pool
 *
 * Return: none
 */
void dp_tx_flow_control_deinit(struct dp_soc *soc)
{
	qdf_spinlock_destroy(&soc->flow_pool_array_lock);
}

/**
 * dp_txrx_register_pause_cb() - Register pause callback
 * @ctx: Handle to struct dp_soc
 * @pause_cb: Tx pause_cb
 *
 * Return: none
 */
QDF_STATUS dp_txrx_register_pause_cb(struct cdp_soc_t *handle,
	tx_pause_callback pause_cb)
{
	struct dp_soc *soc = (struct dp_soc *)handle;

	if (!soc || !pause_cb) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("soc or pause_cb is NULL"));
		return QDF_STATUS_E_INVAL;
	}
	soc->pause_cb = pause_cb;

	return QDF_STATUS_SUCCESS;
}
