/*
 * Copyright (c) 2011-2018 The Linux Foundation. All rights reserved.
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

/* OS abstraction libraries */
#include <qdf_nbuf.h>           /* qdf_nbuf_t, etc. */
#include <qdf_atomic.h>         /* qdf_atomic_read, etc. */
#include <qdf_util.h>           /* qdf_unlikely */

/* APIs for other modules */
#include <htt.h>                /* HTT_TX_EXT_TID_MGMT */
#include <ol_htt_tx_api.h>      /* htt_tx_desc_tid */

/* internal header files relevant for all systems */
#include <ol_txrx_internal.h>   /* TXRX_ASSERT1 */
#include <ol_tx_desc.h>         /* ol_tx_desc */
#include <ol_tx_send.h>         /* ol_tx_send */
#include <ol_txrx.h>

/* internal header files relevant only for HL systems */
#include <ol_tx_classify.h>   /* ol_tx_classify, ol_tx_classify_mgmt */
#include <ol_tx_queue.h>        /* ol_tx_enqueue */
#include <ol_tx_sched.h>      /* ol_tx_sched */


/* internal header files relevant only for specific systems (Pronto) */
#include <ol_txrx_encap.h>      /* OL_TX_ENCAP, etc */
#include <ol_tx.h>
#include <cdp_txrx_ipa.h>

/*
 * The TXRX module doesn't accept tx frames unless the target has
 * enough descriptors for them.
 * For LL, the TXRX descriptor pool is sized to match the target's
 * descriptor pool.  Hence, if the descriptor allocation in TXRX
 * succeeds, that guarantees that the target has room to accept
 * the new tx frame.
 */
struct ol_tx_desc_t *
ol_tx_prepare_ll(ol_txrx_vdev_handle vdev,
		 qdf_nbuf_t msdu,
		 struct ol_txrx_msdu_info_t *msdu_info)
{
	struct ol_tx_desc_t *tx_desc;
	struct ol_txrx_pdev_t *pdev = vdev->pdev;

	(msdu_info)->htt.info.frame_type = pdev->htt_pkt_type;
	tx_desc = ol_tx_desc_ll(pdev, vdev, msdu, msdu_info);
	if (qdf_unlikely(!tx_desc)) {
		/*
		 * If TSO packet, free associated
		 * remaining TSO segment descriptors
		 */
		if (qdf_nbuf_is_tso(msdu))
			ol_free_remaining_tso_segs(
					vdev, msdu_info, true);
		TXRX_STATS_MSDU_LIST_INCR(
				pdev, tx.dropped.host_reject, msdu);
		return NULL;
	}

	return tx_desc;
}

#if defined(FEATURE_TSO)
void ol_free_remaining_tso_segs(ol_txrx_vdev_handle vdev,
				       struct ol_txrx_msdu_info_t *msdu_info,
				       bool is_tso_seg_mapping_done)
{
	struct qdf_tso_seg_elem_t *next_seg;
	struct qdf_tso_seg_elem_t *free_seg = msdu_info->tso_info.curr_seg;
	struct ol_txrx_pdev_t *pdev;
	bool is_last_seg = false;

	if (qdf_unlikely(!vdev)) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
			"%s:vdev is null", __func__);
		return;
	} else {
		pdev = vdev->pdev;
		if (qdf_unlikely(!pdev)) {
			QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
				"%s:pdev is null", __func__);
			return;
		}
	}

	if (is_tso_seg_mapping_done) {
		/*
		 * TSO segment are mapped already, therefore,
		 * 1. unmap the tso segments,
		 * 2. free tso num segment if it is a last segment, and
		 * 3. free the tso segments.
		 */
		 struct qdf_tso_num_seg_elem_t *tso_num_desc =
				msdu_info->tso_info.tso_num_seg_list;

		if (qdf_unlikely(tso_num_desc == NULL)) {
			QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
			  "%s %d TSO common info is NULL!",
			  __func__, __LINE__);
			return;
		}

		while (free_seg) {
			qdf_spin_lock_bh(&pdev->tso_seg_pool.tso_mutex);
			tso_num_desc->num_seg.tso_cmn_num_seg--;

			is_last_seg = (tso_num_desc->num_seg.tso_cmn_num_seg ==
				       0) ? true : false;
			qdf_nbuf_unmap_tso_segment(pdev->osdev, free_seg,
						   is_last_seg);
			qdf_spin_unlock_bh(&pdev->tso_seg_pool.tso_mutex);

			if (is_last_seg) {
				ol_tso_num_seg_free(pdev,
					msdu_info->tso_info.tso_num_seg_list);
				msdu_info->tso_info.tso_num_seg_list = NULL;
			}

			next_seg = free_seg->next;
			free_seg->force_free = 1;
			ol_tso_free_segment(pdev, free_seg);
			free_seg = next_seg;
		}
	} else {
		/*
		 * TSO segment are not mapped therefore,
		 * free the tso segments only.
		 */
		while (free_seg) {
			next_seg = free_seg->next;
			free_seg->force_free = 1;
			ol_tso_free_segment(pdev, free_seg);
			free_seg = next_seg;
		}
	}
}

/**
 * ol_tx_prepare_tso() - Given a jumbo msdu, prepare the TSO
 * related information in the msdu_info meta data
 * @vdev: virtual device handle
 * @msdu: network buffer
 * @msdu_info: meta data associated with the msdu
 *
 * Return: 0 - success, >0 - error
 */
uint8_t ol_tx_prepare_tso(ol_txrx_vdev_handle vdev,
			  qdf_nbuf_t msdu,
			  struct ol_txrx_msdu_info_t *msdu_info)
{
	msdu_info->tso_info.curr_seg = NULL;
	if (qdf_nbuf_is_tso(msdu)) {
		int num_seg = qdf_nbuf_get_tso_num_seg(msdu);
		struct qdf_tso_num_seg_elem_t *tso_num_seg;

		msdu_info->tso_info.tso_num_seg_list = NULL;
		msdu_info->tso_info.tso_seg_list = NULL;
		msdu_info->tso_info.num_segs = num_seg;
		while (num_seg) {
			struct qdf_tso_seg_elem_t *tso_seg =
				ol_tso_alloc_segment(vdev->pdev);
			if (tso_seg) {
				qdf_tso_seg_dbg_record(tso_seg,
						       TSOSEG_LOC_PREPARETSO);
				tso_seg->next =
					msdu_info->tso_info.tso_seg_list;
				msdu_info->tso_info.tso_seg_list
					= tso_seg;
				num_seg--;
			} else {
				/* Free above alocated TSO segements till now */
				msdu_info->tso_info.curr_seg =
					msdu_info->tso_info.tso_seg_list;
				ol_free_remaining_tso_segs(vdev, msdu_info,
							   false);
				return 1;
			}
		}
		tso_num_seg = ol_tso_num_seg_alloc(vdev->pdev);
		if (tso_num_seg) {
			tso_num_seg->next = msdu_info->tso_info.
						tso_num_seg_list;
			msdu_info->tso_info.tso_num_seg_list = tso_num_seg;
		} else {
			/* Free the already allocated num of segments */
			msdu_info->tso_info.curr_seg =
				msdu_info->tso_info.tso_seg_list;
			ol_free_remaining_tso_segs(vdev, msdu_info, false);
			return 1;
		}

		if (qdf_unlikely(!qdf_nbuf_get_tso_info(vdev->pdev->osdev,
					msdu, &(msdu_info->tso_info)))) {
			/* Free the already allocated num of segments */
			msdu_info->tso_info.curr_seg =
				msdu_info->tso_info.tso_seg_list;
			ol_free_remaining_tso_segs(vdev, msdu_info, false);
			return 1;
		}

		msdu_info->tso_info.curr_seg =
			msdu_info->tso_info.tso_seg_list;
		num_seg = msdu_info->tso_info.num_segs;
	} else {
		msdu_info->tso_info.is_tso = 0;
		msdu_info->tso_info.num_segs = 1;
	}
	return 0;
}
#endif

/**
 * ol_tx_data() - send data frame
 * @vdev: virtual device handle
 * @skb: skb
 *
 * Return: skb/NULL for success
 */
qdf_nbuf_t ol_tx_data(void *data_vdev, qdf_nbuf_t skb)
{
	struct ol_txrx_pdev_t *pdev;
	qdf_nbuf_t ret;
	ol_txrx_vdev_handle vdev = data_vdev;

	if (qdf_unlikely(!vdev)) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_DEBUG,
			"%s:vdev is null", __func__);
		return skb;
	}

	pdev = vdev->pdev;

	if (qdf_unlikely(!pdev)) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_DEBUG,
			"%s:pdev is null", __func__);
		return skb;
	}

	if ((ol_cfg_is_ip_tcp_udp_checksum_offload_enabled(pdev->ctrl_pdev))
		&& (qdf_nbuf_get_protocol(skb) == htons(ETH_P_IP))
		&& (qdf_nbuf_get_ip_summed(skb) == CHECKSUM_PARTIAL))
		qdf_nbuf_set_ip_summed(skb, CHECKSUM_COMPLETE);

	/* Terminate the (single-element) list of tx frames */
	qdf_nbuf_set_next(skb, NULL);
	ret = OL_TX_SEND(vdev, skb);
	if (ret) {
		ol_txrx_dbg("%s: Failed to tx", __func__);
		return ret;
	}

	return NULL;
}

#ifdef IPA_OFFLOAD
qdf_nbuf_t ol_tx_send_ipa_data_frame(struct cdp_vdev *vdev, qdf_nbuf_t skb)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	qdf_nbuf_t ret;

	if (qdf_unlikely(!pdev)) {
		qdf_net_buf_debug_acquire_skb(skb, __FILE__, __LINE__);

		ol_txrx_err("%s: pdev is NULL", __func__);
		return skb;
	}

	if ((ol_cfg_is_ip_tcp_udp_checksum_offload_enabled(pdev->ctrl_pdev))
		&& (qdf_nbuf_get_protocol(skb) == htons(ETH_P_IP))
		&& (qdf_nbuf_get_ip_summed(skb) == CHECKSUM_PARTIAL))
		qdf_nbuf_set_ip_summed(skb, CHECKSUM_COMPLETE);

	/* Terminate the (single-element) list of tx frames */
	qdf_nbuf_set_next(skb, NULL);

	/*
	 * Add SKB to internal tracking table before further processing
	 * in WLAN driver.
	 */
	qdf_net_buf_debug_acquire_skb(skb, __FILE__, __LINE__);

	ret = OL_TX_SEND((struct ol_txrx_vdev_t *)vdev, skb);
	if (ret) {
		ol_txrx_dbg("%s: Failed to tx", __func__);
		return ret;
	}

	return NULL;
}
#endif

#if defined(FEATURE_TSO)
/**
 * ol_tx_tso_update_stats() - update TSO stats
 * @pdev: pointer to ol_txrx_pdev_t structure
 * @msdu_info: tso msdu_info for the msdu
 * @msdu: tso mdsu for which stats are updated
 * @tso_msdu_idx: stats index in the global TSO stats array where stats will be
 *                updated
 *
 * Return: None
 */
void ol_tx_tso_update_stats(struct ol_txrx_pdev_t *pdev,
			    struct qdf_tso_info_t  *tso_info, qdf_nbuf_t msdu,
			    uint32_t tso_msdu_idx)
{
	TXRX_STATS_TSO_HISTOGRAM(pdev, tso_info->num_segs);
	TXRX_STATS_TSO_GSO_SIZE_UPDATE(pdev, tso_msdu_idx,
					qdf_nbuf_tcp_tso_size(msdu));
	TXRX_STATS_TSO_TOTAL_LEN_UPDATE(pdev,
					tso_msdu_idx, qdf_nbuf_len(msdu));
	TXRX_STATS_TSO_NUM_FRAGS_UPDATE(pdev, tso_msdu_idx,
					qdf_nbuf_get_nr_frags(msdu));
}

/**
 * ol_tx_tso_get_stats_idx() - retrieve global TSO stats index and increment it
 * @pdev: pointer to ol_txrx_pdev_t structure
 *
 * Retrieve  the current value of the global variable and increment it. This is
 * done in a spinlock as the global TSO stats may be accessed in parallel by
 * multiple TX streams.
 *
 * Return: The current value of TSO stats index.
 */
uint32_t ol_tx_tso_get_stats_idx(struct ol_txrx_pdev_t *pdev)
{
	uint32_t msdu_stats_idx = 0;

	qdf_spin_lock_bh(&pdev->stats.pub.tx.tso.tso_stats_lock);
	msdu_stats_idx = pdev->stats.pub.tx.tso.tso_info.tso_msdu_idx;
	pdev->stats.pub.tx.tso.tso_info.tso_msdu_idx++;
	pdev->stats.pub.tx.tso.tso_info.tso_msdu_idx &=
					NUM_MAX_TSO_MSDUS_MASK;
	qdf_spin_unlock_bh(&pdev->stats.pub.tx.tso.tso_stats_lock);

	TXRX_STATS_TSO_RESET_MSDU(pdev, msdu_stats_idx);

	return msdu_stats_idx;
}
#endif

static inline int ol_txrx_tx_is_raw(enum ol_tx_spec tx_spec)
{
	return
		tx_spec &
		(OL_TX_SPEC_RAW | OL_TX_SPEC_NO_AGGR | OL_TX_SPEC_NO_ENCRYPT);
}

static inline uint8_t ol_txrx_tx_raw_subtype(enum ol_tx_spec tx_spec)
{
	uint8_t sub_type = 0x1; /* 802.11 MAC header present */

	if (tx_spec & OL_TX_SPEC_NO_AGGR)
		sub_type |= 0x1 << HTT_TX_MSDU_DESC_RAW_SUBTYPE_NO_AGGR_S;
	if (tx_spec & OL_TX_SPEC_NO_ENCRYPT)
		sub_type |= 0x1 << HTT_TX_MSDU_DESC_RAW_SUBTYPE_NO_ENCRYPT_S;
	if (tx_spec & OL_TX_SPEC_NWIFI_NO_ENCRYPT)
		sub_type |= 0x1 << HTT_TX_MSDU_DESC_RAW_SUBTYPE_NO_ENCRYPT_S;
	return sub_type;
}

static qdf_nbuf_t
ol_tx_non_std_ll(struct ol_txrx_vdev_t *vdev,
		 enum ol_tx_spec tx_spec,
		 qdf_nbuf_t msdu_list)
{
	qdf_nbuf_t msdu = msdu_list;
	htt_pdev_handle htt_pdev = vdev->pdev->htt_pdev;
	struct ol_txrx_msdu_info_t msdu_info;

	msdu_info.htt.info.l2_hdr_type = vdev->pdev->htt_pkt_type;
	msdu_info.htt.action.tx_comp_req = 0;

	/*
	 * The msdu_list variable could be used instead of the msdu var,
	 * but just to clarify which operations are done on a single MSDU
	 * vs. a list of MSDUs, use a distinct variable for single MSDUs
	 * within the list.
	 */
	while (msdu) {
		qdf_nbuf_t next;
		struct ol_tx_desc_t *tx_desc = NULL;

		msdu_info.htt.info.ext_tid = qdf_nbuf_get_tid(msdu);
		msdu_info.peer = NULL;
		msdu_info.tso_info.is_tso = 0;

		tx_desc = ol_tx_prepare_ll(vdev, msdu, &msdu_info);
		if (!tx_desc)
			return msdu;

		/*
		 * The netbuf may get linked into a different list inside the
		 * ol_tx_send function, so store the next pointer before the
		 * tx_send call.
		 */
		next = qdf_nbuf_next(msdu);

		if (tx_spec != OL_TX_SPEC_STD) {
			if (tx_spec & OL_TX_SPEC_NO_FREE) {
				tx_desc->pkt_type = OL_TX_FRM_NO_FREE;
			} else if (tx_spec & OL_TX_SPEC_TSO) {
				tx_desc->pkt_type = OL_TX_FRM_TSO;
			} else if (tx_spec & OL_TX_SPEC_NWIFI_NO_ENCRYPT) {
				uint8_t sub_type =
					ol_txrx_tx_raw_subtype(tx_spec);
				htt_tx_desc_type(htt_pdev, tx_desc->htt_tx_desc,
						htt_pkt_type_native_wifi,
						sub_type);
			} else if (ol_txrx_tx_is_raw(tx_spec)) {
				/* different types of raw frames */
				uint8_t sub_type =
					ol_txrx_tx_raw_subtype(tx_spec);
				htt_tx_desc_type(htt_pdev, tx_desc->htt_tx_desc,
						htt_pkt_type_raw, sub_type);
			}
		}
		/*
		 * If debug display is enabled, show the meta-data being
		 * downloaded to the target via the HTT tx descriptor.
		 */
		htt_tx_desc_display(tx_desc->htt_tx_desc);
		ol_tx_send(vdev->pdev, tx_desc, msdu, vdev->vdev_id);
		msdu = next;
	}
	return NULL;            /* all MSDUs were accepted */
}

#ifdef QCA_SUPPORT_SW_TXRX_ENCAP
static inline int ol_tx_encap_wrapper(struct ol_txrx_pdev_t *pdev,
				      ol_txrx_vdev_handle vdev,
				      struct ol_tx_desc_t *tx_desc,
				      qdf_nbuf_t msdu,
				      struct ol_txrx_msdu_info_t *tx_msdu_info)
{
	if (OL_TX_ENCAP(vdev, tx_desc, msdu, tx_msdu_info) != A_OK) {
		qdf_atomic_inc(&pdev->tx_queue.rsrc_cnt);
		ol_tx_desc_frame_free_nonstd(pdev, tx_desc, 1);
		if (tx_msdu_info->peer) {
			/* remove the peer reference added above */
			ol_txrx_peer_release_ref(tx_msdu_info->peer,
						 PEER_DEBUG_ID_OL_INTERNAL);
		}
		return -EINVAL;
	}

	return 0;
}
#else
static inline int ol_tx_encap_wrapper(struct ol_txrx_pdev_t *pdev,
				      ol_txrx_vdev_handle vdev,
				      struct ol_tx_desc_t *tx_desc,
				      qdf_nbuf_t msdu,
				      struct ol_txrx_msdu_info_t *tx_msdu_info)
{
	/* no-op */
	return 0;
}
#endif

/* tx filtering is handled within the target FW */
#define TX_FILTER_CHECK(tx_msdu_info) 0 /* don't filter */

/**
 * parse_ocb_tx_header() - Function to check for OCB
 * @msdu:   Pointer to OS packet (qdf_nbuf_t)
 * @tx_ctrl: TX control header on a packet and extract it if present
 *
 * Return: true if ocb parsing is successful
 */
#define OCB_HEADER_VERSION     1
static bool parse_ocb_tx_header(qdf_nbuf_t msdu,
				struct ocb_tx_ctrl_hdr_t *tx_ctrl)
{
	struct ether_header *eth_hdr_p;
	struct ocb_tx_ctrl_hdr_t *tx_ctrl_hdr;

	/* Check if TX control header is present */
	eth_hdr_p = (struct ether_header *)qdf_nbuf_data(msdu);
	if (eth_hdr_p->ether_type != QDF_SWAP_U16(ETHERTYPE_OCB_TX))
		/* TX control header is not present. Nothing to do.. */
		return true;

	/* Remove the ethernet header */
	qdf_nbuf_pull_head(msdu, sizeof(struct ether_header));

	/* Parse the TX control header */
	tx_ctrl_hdr = (struct ocb_tx_ctrl_hdr_t *)qdf_nbuf_data(msdu);

	if (tx_ctrl_hdr->version == OCB_HEADER_VERSION) {
		if (tx_ctrl)
			qdf_mem_copy(tx_ctrl, tx_ctrl_hdr,
				     sizeof(*tx_ctrl_hdr));
	} else {
		/* The TX control header is invalid. */
		return false;
	}

	/* Remove the TX control header */
	qdf_nbuf_pull_head(msdu, tx_ctrl_hdr->length);
	return true;
}


#if defined(CONFIG_HL_SUPPORT) && defined(CONFIG_TX_DESC_HI_PRIO_RESERVE)

/**
 * ol_tx_hl_desc_alloc() - Allocate and initialize a tx descriptor
 *			   for a HL system.
 * @pdev: the data physical device sending the data
 * @vdev: the virtual device sending the data
 * @msdu: the tx frame
 * @msdu_info: the tx meta data
 *
 * Return: the tx decriptor
 */
static inline
struct ol_tx_desc_t *ol_tx_hl_desc_alloc(struct ol_txrx_pdev_t *pdev,
	struct ol_txrx_vdev_t *vdev,
	qdf_nbuf_t msdu,
	struct ol_txrx_msdu_info_t *msdu_info)
{
	struct ol_tx_desc_t *tx_desc = NULL;

	if (qdf_atomic_read(&pdev->tx_queue.rsrc_cnt) >
			TXRX_HL_TX_DESC_HI_PRIO_RESERVED) {
		tx_desc = ol_tx_desc_hl(pdev, vdev, msdu, msdu_info);
	} else if (qdf_nbuf_is_ipv4_pkt(msdu) == true) {
		if ((QDF_NBUF_CB_GET_PACKET_TYPE(msdu) ==
				QDF_NBUF_CB_PACKET_TYPE_DHCP) ||
		    (QDF_NBUF_CB_GET_PACKET_TYPE(msdu) ==
			QDF_NBUF_CB_PACKET_TYPE_EAPOL)) {
			tx_desc = ol_tx_desc_hl(pdev, vdev, msdu, msdu_info);
			ol_txrx_info("Provided tx descriptor from reserve pool for DHCP/EAPOL\n");
		}
	}
	return tx_desc;
}
#elif defined(CONFIG_HL_SUPPORT) && defined(QCA_HL_NETDEV_FLOW_CONTROL)
bool ol_tx_desc_is_high_prio(qdf_nbuf_t msdu)
{
	enum qdf_proto_subtype proto_subtype;
	bool high_prio = false;

	if (qdf_nbuf_is_ipv4_pkt(msdu) == true) {
		if ((QDF_NBUF_CB_GET_PACKET_TYPE(msdu) ==
			QDF_NBUF_CB_PACKET_TYPE_DHCP) ||
		    (QDF_NBUF_CB_GET_PACKET_TYPE(msdu) ==
			QDF_NBUF_CB_PACKET_TYPE_EAPOL))
			high_prio = true;
	} else if (QDF_NBUF_CB_GET_PACKET_TYPE(msdu) ==
			QDF_NBUF_CB_PACKET_TYPE_ARP) {
		high_prio = true;
	} else if ((QDF_NBUF_CB_GET_PACKET_TYPE(msdu) ==
		   QDF_NBUF_CB_PACKET_TYPE_ICMPv6)) {
		proto_subtype = qdf_nbuf_get_icmpv6_subtype(msdu);
		switch (proto_subtype) {
		case QDF_PROTO_ICMPV6_NA:
		case QDF_PROTO_ICMPV6_NS:
			high_prio = true;
		default:
			high_prio = false;
		}
	}
	return high_prio;
}

static inline
struct ol_tx_desc_t *ol_tx_hl_desc_alloc(struct ol_txrx_pdev_t *pdev,
					 struct ol_txrx_vdev_t *vdev,
					 qdf_nbuf_t msdu,
					 struct ol_txrx_msdu_info_t *msdu_info)
{
	struct ol_tx_desc_t *tx_desc =
		ol_tx_desc_hl(pdev, vdev, msdu, msdu_info);

	if (!tx_desc)
		return NULL;

	qdf_spin_lock_bh(&pdev->tx_mutex);
	/* return if TX flow control disabled */
	if (vdev->tx_desc_limit == 0) {
		qdf_spin_unlock_bh(&pdev->tx_mutex);
		return tx_desc;
	}

	if (!qdf_atomic_read(&vdev->os_q_paused) &&
	    (qdf_atomic_read(&vdev->tx_desc_count) >= vdev->queue_stop_th)) {
		/*
		 * Pause normal priority
		 * netdev queues if tx desc limit crosses
		 */
		pdev->pause_cb(vdev->vdev_id,
			       WLAN_STOP_NON_PRIORITY_QUEUE,
			       WLAN_DATA_FLOW_CONTROL);
		qdf_atomic_set(&vdev->os_q_paused, 1);
	} else if (ol_tx_desc_is_high_prio(msdu) && !vdev->prio_q_paused &&
		   (qdf_atomic_read(&vdev->tx_desc_count)
		    == vdev->tx_desc_limit)) {
			/* Pause high priority queue */
		pdev->pause_cb(vdev->vdev_id,
			       WLAN_NETIF_PRIORITY_QUEUE_OFF,
			       WLAN_DATA_FLOW_CONTROL_PRIORITY);
		vdev->prio_q_paused = 1;
	}
	qdf_spin_unlock_bh(&pdev->tx_mutex);

	return tx_desc;
}
#else
static inline
struct ol_tx_desc_t *ol_tx_hl_desc_alloc(struct ol_txrx_pdev_t *pdev,
					 struct ol_txrx_vdev_t *vdev,
					 qdf_nbuf_t msdu,
					 struct ol_txrx_msdu_info_t *msdu_info)
{
	struct ol_tx_desc_t *tx_desc = NULL;

	tx_desc = ol_tx_desc_hl(pdev, vdev, msdu, msdu_info);
	return tx_desc;
}
#endif /* CONFIG_HL_SUPPORT && QCA_HL_NETDEV_FLOW_CONTROL */

#if defined(CONFIG_HL_SUPPORT)

/**
 * ol_txrx_mgmt_tx_desc_alloc() - Allocate and initialize a tx descriptor
 *				 for management frame
 * @pdev: the data physical device sending the data
 * @vdev: the virtual device sending the data
 * @tx_mgmt_frm: the tx management frame
 * @tx_msdu_info: the tx meta data
 *
 * Return: the tx decriptor
 */
static inline
struct ol_tx_desc_t *
ol_txrx_mgmt_tx_desc_alloc(
	struct ol_txrx_pdev_t *pdev,
	struct ol_txrx_vdev_t *vdev,
	qdf_nbuf_t tx_mgmt_frm,
	struct ol_txrx_msdu_info_t *tx_msdu_info)
{
	struct ol_tx_desc_t *tx_desc;

	tx_msdu_info->htt.action.tx_comp_req = 1;
	tx_desc = ol_tx_desc_hl(pdev, vdev, tx_mgmt_frm, tx_msdu_info);
	return tx_desc;
}

/**
 * ol_txrx_mgmt_send_frame() - send a management frame
 * @vdev: virtual device sending the frame
 * @tx_desc: tx desc
 * @tx_mgmt_frm: management frame to send
 * @tx_msdu_info: the tx meta data
 * @chanfreq: download change frequency
 *
 * Return:
 *      0 -> the frame is accepted for transmission, -OR-
 *      1 -> the frame was not accepted
 */
static inline
int ol_txrx_mgmt_send_frame(
	struct ol_txrx_vdev_t *vdev,
	struct ol_tx_desc_t *tx_desc,
	qdf_nbuf_t tx_mgmt_frm,
	struct ol_txrx_msdu_info_t *tx_msdu_info,
	uint16_t chanfreq)
{
	struct ol_txrx_pdev_t *pdev = vdev->pdev;
	struct ol_tx_frms_queue_t *txq;
	int status = 1;

	/*
	 * 1.  Look up the peer and queue the frame in the peer's mgmt queue.
	 * 2.  Invoke the download scheduler.
	 */
	txq = ol_tx_classify_mgmt(vdev, tx_desc, tx_mgmt_frm, tx_msdu_info);
	if (!txq) {
		/* TXRX_STATS_MSDU_LIST_INCR(vdev->pdev, tx.dropped.no_txq,
		 *			     msdu);
		 */
		qdf_atomic_inc(&pdev->tx_queue.rsrc_cnt);
		ol_tx_desc_frame_free_nonstd(vdev->pdev, tx_desc,
					     1 /* error */);
		goto out; /* can't accept the tx mgmt frame */
	}
	/* Initialize the HTT tx desc l2 header offset field.
	 * Even though tx encap does not apply to mgmt frames,
	 * htt_tx_desc_mpdu_header still needs to be called,
	 * to specifiy that there was no L2 header added by tx encap,
	 * so the frame's length does not need to be adjusted to account for
	 * an added L2 header.
	 */
	htt_tx_desc_mpdu_header(tx_desc->htt_tx_desc, 0);
	if (qdf_unlikely(htt_tx_desc_init(
			pdev->htt_pdev, tx_desc->htt_tx_desc,
			tx_desc->htt_tx_desc_paddr,
			ol_tx_desc_id(pdev, tx_desc),
			tx_mgmt_frm,
			&tx_msdu_info->htt, &tx_msdu_info->tso_info, NULL, 0)))
		goto out;
	htt_tx_desc_display(tx_desc->htt_tx_desc);
	htt_tx_desc_set_chanfreq(tx_desc->htt_tx_desc, chanfreq);

	ol_tx_enqueue(vdev->pdev, txq, tx_desc, tx_msdu_info);
	ol_tx_sched(vdev->pdev);
	status = 0;
out:
	if (tx_msdu_info->peer) {
		/* remove the peer reference added above */
		ol_txrx_peer_release_ref(tx_msdu_info->peer,
					 PEER_DEBUG_ID_OL_INTERNAL);
	}

	return status;
}

#else

static inline
struct ol_tx_desc_t *
ol_txrx_mgmt_tx_desc_alloc(
	struct ol_txrx_pdev_t *pdev,
	struct ol_txrx_vdev_t *vdev,
	qdf_nbuf_t tx_mgmt_frm,
	struct ol_txrx_msdu_info_t *tx_msdu_info)
{
	struct ol_tx_desc_t *tx_desc;

	/* For LL tx_comp_req is not used so initialized to 0 */
	tx_msdu_info->htt.action.tx_comp_req = 0;
	tx_desc = ol_tx_desc_ll(pdev, vdev, tx_mgmt_frm, tx_msdu_info);
	/* FIX THIS -
	 * The FW currently has trouble using the host's fragments table
	 * for management frames.  Until this is fixed, rather than
	 * specifying the fragment table to the FW, specify just the
	 * address of the initial fragment.
	 */
#if defined(HELIUMPLUS)
	/* ol_txrx_dump_frag_desc("ol_txrx_mgmt_send(): after ol_tx_desc_ll",
	 *			  tx_desc);
	 */
#endif /* defined(HELIUMPLUS) */
	if (tx_desc) {
		/*
		 * Following the call to ol_tx_desc_ll, frag 0 is the
		 * HTT tx HW descriptor, and the frame payload is in
		 * frag 1.
		 */
		htt_tx_desc_frags_table_set(
				pdev->htt_pdev,
				tx_desc->htt_tx_desc,
				qdf_nbuf_get_frag_paddr(tx_mgmt_frm, 1),
				0, 0);
#if defined(HELIUMPLUS) && defined(HELIUMPLUS_DEBUG)
		ol_txrx_dump_frag_desc(
				"after htt_tx_desc_frags_table_set",
				tx_desc);
#endif /* defined(HELIUMPLUS) */
	}

	return tx_desc;
}

static inline
int ol_txrx_mgmt_send_frame(
	struct ol_txrx_vdev_t *vdev,
	struct ol_tx_desc_t *tx_desc,
	qdf_nbuf_t tx_mgmt_frm,
	struct ol_txrx_msdu_info_t *tx_msdu_info,
	uint16_t chanfreq)
{
	struct ol_txrx_pdev_t *pdev = vdev->pdev;

	htt_tx_desc_set_chanfreq(tx_desc->htt_tx_desc, chanfreq);
	QDF_NBUF_CB_TX_PACKET_TRACK(tx_desc->netbuf) =
					QDF_NBUF_TX_PKT_MGMT_TRACK;
	ol_tx_send_nonstd(pdev, tx_desc, tx_mgmt_frm,
			  htt_pkt_type_mgmt);

	return 0;
}
#endif

/**
 * ol_tx_hl_base() - send tx frames for a HL system.
 * @vdev: the virtual device sending the data
 * @tx_spec: indicate what non-standard transmission actions to apply
 * @msdu_list: the tx frames to send
 * @tx_comp_req: tx completion req
 *
 * Return: NULL if all MSDUs are accepted
 */
static inline qdf_nbuf_t
ol_tx_hl_base(
	ol_txrx_vdev_handle vdev,
	enum ol_tx_spec tx_spec,
	qdf_nbuf_t msdu_list,
	int tx_comp_req)
{
	struct ol_txrx_pdev_t *pdev = vdev->pdev;
	qdf_nbuf_t msdu = msdu_list;
	struct ol_txrx_msdu_info_t tx_msdu_info;
	struct ocb_tx_ctrl_hdr_t tx_ctrl;
	htt_pdev_handle htt_pdev = pdev->htt_pdev;

	tx_msdu_info.tso_info.is_tso = 0;

	/*
	 * The msdu_list variable could be used instead of the msdu var,
	 * but just to clarify which operations are done on a single MSDU
	 * vs. a list of MSDUs, use a distinct variable for single MSDUs
	 * within the list.
	 */
	while (msdu) {
		qdf_nbuf_t next;
		struct ol_tx_frms_queue_t *txq;
		struct ol_tx_desc_t *tx_desc = NULL;

		qdf_mem_zero(&tx_ctrl, sizeof(tx_ctrl));
		tx_msdu_info.peer = NULL;
		/*
		 * The netbuf will get stored into a (peer-TID) tx queue list
		 * inside the ol_tx_classify_store function or else dropped,
		 * so store the next pointer immediately.
		 */
		next = qdf_nbuf_next(msdu);

		tx_desc = ol_tx_hl_desc_alloc(pdev, vdev, msdu, &tx_msdu_info);

		if (!tx_desc) {
			/*
			 * If we're out of tx descs, there's no need to try
			 * to allocate tx descs for the remaining MSDUs.
			 */
			TXRX_STATS_MSDU_LIST_INCR(pdev, tx.dropped.host_reject,
						  msdu);
			return msdu; /* the list of unaccepted MSDUs */
		}

		/* OL_TXRX_PROT_AN_LOG(pdev->prot_an_tx_sent, msdu);*/

		if (tx_spec != OL_TX_SPEC_STD) {
#if defined(FEATURE_WLAN_TDLS)
			if (tx_spec & OL_TX_SPEC_NO_FREE) {
				tx_desc->pkt_type = OL_TX_FRM_NO_FREE;
			} else if (tx_spec & OL_TX_SPEC_TSO) {
#else
				if (tx_spec & OL_TX_SPEC_TSO) {
#endif
					tx_desc->pkt_type = OL_TX_FRM_TSO;
				}
				if (ol_txrx_tx_is_raw(tx_spec)) {
					/* CHECK THIS: does this need
					 * to happen after htt_tx_desc_init?
					 */
					/* different types of raw frames */
					u_int8_t sub_type =
						ol_txrx_tx_raw_subtype(
								tx_spec);
					htt_tx_desc_type(htt_pdev,
							 tx_desc->htt_tx_desc,
							 htt_pkt_type_raw,
							 sub_type);
				}
			}

			tx_msdu_info.htt.info.ext_tid = qdf_nbuf_get_tid(msdu);
			tx_msdu_info.htt.info.vdev_id = vdev->vdev_id;
			tx_msdu_info.htt.info.frame_type = htt_frm_type_data;
			tx_msdu_info.htt.info.l2_hdr_type = pdev->htt_pkt_type;
			tx_msdu_info.htt.action.tx_comp_req = tx_comp_req;

			/* If the vdev is in OCB mode,
			 * parse the tx control header.
			 */
			if (vdev->opmode == wlan_op_mode_ocb) {
				if (!parse_ocb_tx_header(msdu, &tx_ctrl)) {
					/* There was an error parsing
					 * the header.Skip this packet.
					 */
					goto MSDU_LOOP_BOTTOM;
				}
			}

			txq = ol_tx_classify(vdev, tx_desc, msdu,
							&tx_msdu_info);

			if ((!txq) || TX_FILTER_CHECK(&tx_msdu_info)) {
				/* drop this frame,
				 * but try sending subsequent frames
				 */
				/*TXRX_STATS_MSDU_LIST_INCR(pdev,
							tx.dropped.no_txq,
							msdu);*/
				qdf_atomic_inc(&pdev->tx_queue.rsrc_cnt);
				ol_tx_desc_frame_free_nonstd(pdev, tx_desc, 1);
				if (tx_msdu_info.peer) {
					/* remove the peer reference
					 * added above */
					ol_txrx_peer_release_ref(
						tx_msdu_info.peer,
						PEER_DEBUG_ID_OL_INTERNAL);
				}
				goto MSDU_LOOP_BOTTOM;
			}

			if (tx_msdu_info.peer) {
				/*
				 * If the state is not associated then drop all
				 * the data packets received for that peer
				 */
				if (tx_msdu_info.peer->state ==
						OL_TXRX_PEER_STATE_DISC) {
					qdf_atomic_inc(
						&pdev->tx_queue.rsrc_cnt);
					ol_tx_desc_frame_free_nonstd(pdev,
								     tx_desc,
								     1);
					ol_txrx_peer_release_ref(
						tx_msdu_info.peer,
						PEER_DEBUG_ID_OL_INTERNAL);
					msdu = next;
					continue;
				} else if (tx_msdu_info.peer->state !=
						OL_TXRX_PEER_STATE_AUTH) {
					if (tx_msdu_info.htt.info.ethertype !=
						ETHERTYPE_PAE &&
						tx_msdu_info.htt.info.ethertype
							!= ETHERTYPE_WAI) {
						qdf_atomic_inc(
							&pdev->tx_queue.
								rsrc_cnt);
						ol_tx_desc_frame_free_nonstd(
								pdev,
								tx_desc, 1);
						ol_txrx_peer_release_ref(
						 tx_msdu_info.peer,
						 PEER_DEBUG_ID_OL_INTERNAL);
						msdu = next;
						continue;
					}
				}
			}
			/*
			 * Initialize the HTT tx desc l2 header offset field.
			 * htt_tx_desc_mpdu_header  needs to be called to
			 * make sure, the l2 header size is initialized
			 * correctly to handle cases where TX ENCAP is disabled
			 * or Tx Encap fails to perform Encap
			 */
			htt_tx_desc_mpdu_header(tx_desc->htt_tx_desc, 0);

			/*
			 * Note: when the driver is built without support for
			 * SW tx encap,the following macro is a no-op.
			 * When the driver is built with support for SW tx
			 * encap, it performs encap, and if an error is
			 * encountered, jumps to the MSDU_LOOP_BOTTOM label.
			 */
			if (ol_tx_encap_wrapper(pdev, vdev, tx_desc, msdu,
						&tx_msdu_info))
				goto MSDU_LOOP_BOTTOM;

			/* initialize the HW tx descriptor */
			htt_tx_desc_init(
					pdev->htt_pdev, tx_desc->htt_tx_desc,
					tx_desc->htt_tx_desc_paddr,
					ol_tx_desc_id(pdev, tx_desc),
					msdu,
					&tx_msdu_info.htt,
					&tx_msdu_info.tso_info,
					&tx_ctrl,
					vdev->opmode == wlan_op_mode_ocb);
			/*
			 * If debug display is enabled, show the meta-data
			 * being downloaded to the target via the
			 * HTT tx descriptor.
			 */
			htt_tx_desc_display(tx_desc->htt_tx_desc);

			ol_tx_enqueue(pdev, txq, tx_desc, &tx_msdu_info);
			if (tx_msdu_info.peer) {
				OL_TX_PEER_STATS_UPDATE(tx_msdu_info.peer,
							msdu);
				/* remove the peer reference added above */
				ol_txrx_peer_release_ref
						(tx_msdu_info.peer,
						 PEER_DEBUG_ID_OL_INTERNAL);
			}
MSDU_LOOP_BOTTOM:
			msdu = next;
		}
		ol_tx_sched(pdev);
		return NULL; /* all MSDUs were accepted */
}

qdf_nbuf_t
ol_tx_hl(ol_txrx_vdev_handle vdev, qdf_nbuf_t msdu_list)
{
	struct ol_txrx_pdev_t *pdev = vdev->pdev;
	int tx_comp_req = pdev->cfg.default_tx_comp_req ||
		pdev->cfg.request_tx_comp;

	return ol_tx_hl_base(vdev, OL_TX_SPEC_STD, msdu_list, tx_comp_req);
}

static qdf_nbuf_t
ol_tx_non_std_hl(struct ol_txrx_vdev_t *vdev,
		 enum ol_tx_spec tx_spec,
		 qdf_nbuf_t msdu_list)
{
	struct ol_txrx_pdev_t *pdev = vdev->pdev;
	int tx_comp_req = pdev->cfg.default_tx_comp_req ||
		pdev->cfg.request_tx_comp;

	if (!tx_comp_req) {
		if ((tx_spec == OL_TX_SPEC_NO_FREE) &&
		    (pdev->tx_data_callback.func))
			tx_comp_req = 1;
	}
	return ol_tx_hl_base(vdev, tx_spec, msdu_list, tx_comp_req);
}

qdf_nbuf_t
ol_tx_non_std(struct cdp_vdev *pvdev,
	      enum ol_tx_spec tx_spec, qdf_nbuf_t msdu_list)
{
	struct ol_txrx_vdev_t *vdev = (struct ol_txrx_vdev_t *)pvdev;

	if (vdev->pdev->cfg.is_high_latency)
		return ol_tx_non_std_hl(vdev, tx_spec, msdu_list);
	else
		return ol_tx_non_std_ll(vdev, tx_spec, msdu_list);
}

void
ol_txrx_data_tx_cb_set(struct cdp_vdev *pvdev,
		       ol_txrx_data_tx_cb callback, void *ctxt)
{
	struct ol_txrx_vdev_t *vdev = (struct ol_txrx_vdev_t *)pvdev;
	struct ol_txrx_pdev_t *pdev = vdev->pdev;

	pdev->tx_data_callback.func = callback;
	pdev->tx_data_callback.ctxt = ctxt;
}

void
ol_txrx_mgmt_tx_cb_set(struct cdp_pdev *ppdev, uint8_t type,
		       ol_txrx_mgmt_tx_cb download_cb,
		       ol_txrx_mgmt_tx_cb ota_ack_cb, void *ctxt)
{
	struct ol_txrx_pdev_t *pdev = (struct ol_txrx_pdev_t *)ppdev;

	TXRX_ASSERT1(type < OL_TXRX_MGMT_NUM_TYPES);
	pdev->tx_mgmt_cb.download_cb = download_cb;
	pdev->tx_mgmt_cb.ota_ack_cb = ota_ack_cb;
	pdev->tx_mgmt_cb.ctxt = ctxt;
}

#if defined(HELIUMPLUS)
void ol_txrx_dump_frag_desc(char *msg, struct ol_tx_desc_t *tx_desc)
{
	uint32_t                *frag_ptr_i_p;
	int                     i;

	qdf_print("OL TX Descriptor 0x%pK msdu_id %d\n",
		 tx_desc, tx_desc->id);
	qdf_print("HTT TX Descriptor vaddr: 0x%pK paddr: %pad",
		 tx_desc->htt_tx_desc, &tx_desc->htt_tx_desc_paddr);
	qdf_print("%s %d: Fragment Descriptor 0x%pK (paddr=%pad)",
		 __func__, __LINE__, tx_desc->htt_frag_desc,
		 &tx_desc->htt_frag_desc_paddr);

	/*
	 * it looks from htt_tx_desc_frag() that tx_desc->htt_frag_desc
	 * is already de-referrable (=> in virtual address space)
	 */
	frag_ptr_i_p = tx_desc->htt_frag_desc;

	/* Dump 6 words of TSO flags */
	print_hex_dump(KERN_DEBUG, "MLE Desc:TSO Flags:  ",
		       DUMP_PREFIX_NONE, 8, 4,
		       frag_ptr_i_p, 24, true);

	frag_ptr_i_p += 6; /* Skip 6 words of TSO flags */

	i = 0;
	while (*frag_ptr_i_p) {
		print_hex_dump(KERN_DEBUG, "MLE Desc:Frag Ptr:  ",
			       DUMP_PREFIX_NONE, 8, 4,
			       frag_ptr_i_p, 8, true);
		i++;
		if (i > 5) /* max 6 times: frag_ptr0 to frag_ptr5 */
			break;
		/* jump to next  pointer - skip length */
		frag_ptr_i_p += 2;
	}
}
#endif /* HELIUMPLUS */

int
ol_txrx_mgmt_send_ext(struct cdp_vdev *pvdev,
		  qdf_nbuf_t tx_mgmt_frm,
		  uint8_t type, uint8_t use_6mbps, uint16_t chanfreq)
{
	struct ol_txrx_vdev_t *vdev =
				(struct ol_txrx_vdev_t *)pvdev;
	struct ol_txrx_pdev_t *pdev = vdev->pdev;
	struct ol_tx_desc_t *tx_desc;
	struct ol_txrx_msdu_info_t tx_msdu_info;
	int result = 0;

	tx_msdu_info.tso_info.is_tso = 0;

	tx_msdu_info.htt.action.use_6mbps = use_6mbps;
	tx_msdu_info.htt.info.ext_tid = HTT_TX_EXT_TID_MGMT;
	tx_msdu_info.htt.info.vdev_id = vdev->vdev_id;
	tx_msdu_info.htt.action.do_tx_complete =
		pdev->tx_mgmt_cb.ota_ack_cb ? 1 : 0;

	/*
	 * FIX THIS: l2_hdr_type should only specify L2 header type
	 * The Peregrine/Rome HTT layer provides the FW with a "pkt type"
	 * that is a combination of L2 header type and 802.11 frame type.
	 * If the 802.11 frame type is "mgmt", then the HTT pkt type is "mgmt".
	 * But if the 802.11 frame type is "data", then the HTT pkt type is
	 * the L2 header type (more or less): 802.3 vs. Native WiFi
	 * (basic 802.11).
	 * (Or the header type can be "raw", which is any version of the 802.11
	 * header, and also implies that some of the offloaded tx data
	 * processing steps may not apply.)
	 * For efficiency, the Peregrine/Rome HTT uses the msdu_info's
	 * l2_hdr_type field to program the HTT pkt type.  Thus, this txrx SW
	 * needs to overload the l2_hdr_type to indicate whether the frame is
	 * data vs. mgmt, as well as 802.3 L2 header vs. 802.11 L2 header.
	 * To fix this, the msdu_info's l2_hdr_type should be left specifying
	 * just the L2 header type.  For mgmt frames, there should be a
	 * separate function to patch the HTT pkt type to store a "mgmt" value
	 * rather than the L2 header type.  Then the HTT pkt type can be
	 * programmed efficiently for data frames, and the msdu_info's
	 * l2_hdr_type field won't be confusingly overloaded to hold the 802.11
	 * frame type rather than the L2 header type.
	 */
	/*
	 * FIX THIS: remove duplication of htt_frm_type_mgmt and
	 * htt_pkt_type_mgmt
	 * The htt module expects a "enum htt_pkt_type" value.
	 * The htt_dxe module expects a "enum htt_frm_type" value.
	 * This needs to be cleaned up, so both versions of htt use a
	 * consistent method of specifying the frame type.
	 */
#ifdef QCA_SUPPORT_INTEGRATED_SOC
	/* tx mgmt frames always come with a 802.11 header */
	tx_msdu_info.htt.info.l2_hdr_type = htt_pkt_type_native_wifi;
	tx_msdu_info.htt.info.frame_type = htt_frm_type_mgmt;
#else
	tx_msdu_info.htt.info.l2_hdr_type = htt_pkt_type_mgmt;
	tx_msdu_info.htt.info.frame_type = htt_pkt_type_mgmt;
#endif

	tx_msdu_info.peer = NULL;

	tx_desc = ol_txrx_mgmt_tx_desc_alloc(pdev, vdev, tx_mgmt_frm,
							&tx_msdu_info);
	if (!tx_desc)
		return -EINVAL;       /* can't accept the tx mgmt frame */

	TXRX_STATS_MSDU_INCR(pdev, tx.mgmt, tx_mgmt_frm);
	TXRX_ASSERT1(type < OL_TXRX_MGMT_NUM_TYPES);
	tx_desc->pkt_type = type + OL_TXRX_MGMT_TYPE_BASE;

	result = ol_txrx_mgmt_send_frame(vdev, tx_desc, tx_mgmt_frm,
						&tx_msdu_info, chanfreq);

	return 0;               /* accepted the tx mgmt frame */
}

qdf_nbuf_t ol_tx_reinject(struct ol_txrx_vdev_t *vdev,
			  qdf_nbuf_t msdu, uint16_t peer_id)
{
	struct ol_tx_desc_t *tx_desc = NULL;
	struct ol_txrx_msdu_info_t msdu_info;

	msdu_info.htt.info.l2_hdr_type = vdev->pdev->htt_pkt_type;
	msdu_info.htt.info.ext_tid = HTT_TX_EXT_TID_INVALID;
	msdu_info.peer = NULL;
	msdu_info.htt.action.tx_comp_req = 0;
	msdu_info.tso_info.is_tso = 0;

	tx_desc = ol_tx_prepare_ll(vdev, msdu, &msdu_info);
	if (!tx_desc)
		return msdu;

	HTT_TX_DESC_POSTPONED_SET(*((uint32_t *) (tx_desc->htt_tx_desc)), true);

	htt_tx_desc_set_peer_id(tx_desc->htt_tx_desc, peer_id);

	ol_tx_send(vdev->pdev, tx_desc, msdu, vdev->vdev_id);

	return NULL;
}

#if defined(FEATURE_TSO)
/**
 * ol_tso_seg_list_init() - function to initialise the tso seg freelist
 * @pdev: the data physical device sending the data
 * @num_seg: number of segments needs to be intialised
 *
 * Return: none
 */
void ol_tso_seg_list_init(struct ol_txrx_pdev_t *pdev, uint32_t num_seg)
{
	int i = 0;
	struct qdf_tso_seg_elem_t *c_element;

	/* Host should not allocate any c_element. */
	if (num_seg <= 0) {
		ol_txrx_err("%s: ERROR: Pool size passed is 0",
			   __func__);
		QDF_BUG(0);
		pdev->tso_seg_pool.pool_size = i;
		qdf_spinlock_create(&pdev->tso_seg_pool.tso_mutex);
		return;
	}

	c_element = qdf_mem_malloc(sizeof(struct qdf_tso_seg_elem_t));
	pdev->tso_seg_pool.freelist = c_element;
	for (i = 0; i < (num_seg - 1); i++) {
		if (qdf_unlikely(!c_element)) {
			ol_txrx_err("%s: ERROR: c_element NULL for seg %d",
				   __func__, i);
			QDF_BUG(0);
			pdev->tso_seg_pool.pool_size = i;
			pdev->tso_seg_pool.num_free = i;
			qdf_spinlock_create(&pdev->tso_seg_pool.tso_mutex);
			return;
		}
		/* set the freelist bit and magic cookie*/
		c_element->on_freelist = 1;
		c_element->cookie = TSO_SEG_MAGIC_COOKIE;
#ifdef TSOSEG_DEBUG
		c_element->dbg.txdesc = NULL;
		qdf_atomic_init(&c_element->dbg.cur); /* history empty */
		qdf_tso_seg_dbg_record(c_element, TSOSEG_LOC_INIT1);
#endif /* TSOSEG_DEBUG */
		c_element->next =
			qdf_mem_malloc(sizeof(struct qdf_tso_seg_elem_t));
		c_element = c_element->next;
	}
	/*
	 * NULL check for the last c_element of the list or
	 * first c_element if num_seg is equal to 1.
	 */
	if (qdf_unlikely(!c_element)) {
		ol_txrx_err("%s: ERROR: c_element NULL for seg %d",
			   __func__, i);
		QDF_BUG(0);
		pdev->tso_seg_pool.pool_size = i;
		pdev->tso_seg_pool.num_free = i;
		qdf_spinlock_create(&pdev->tso_seg_pool.tso_mutex);
		return;
	}
	c_element->on_freelist = 1;
	c_element->cookie = TSO_SEG_MAGIC_COOKIE;
#ifdef TSOSEG_DEBUG
	qdf_tso_seg_dbg_init(c_element);
	qdf_tso_seg_dbg_record(c_element, TSOSEG_LOC_INIT2);
#endif /* TSOSEG_DEBUG */
	c_element->next = NULL;
	pdev->tso_seg_pool.pool_size = num_seg;
	pdev->tso_seg_pool.num_free = num_seg;
	qdf_spinlock_create(&pdev->tso_seg_pool.tso_mutex);
}

/**
 * ol_tso_seg_list_deinit() - function to de-initialise the tso seg freelist
 * @pdev: the data physical device sending the data
 *
 * Return: none
 */
void ol_tso_seg_list_deinit(struct ol_txrx_pdev_t *pdev)
{
	int i;
	struct qdf_tso_seg_elem_t *c_element;
	struct qdf_tso_seg_elem_t *temp;

	/* pool size 0 implies that tso seg list is not initialised*/
	if (pdev->tso_seg_pool.freelist == NULL &&
	    pdev->tso_seg_pool.pool_size == 0)
		return;

	qdf_spin_lock_bh(&pdev->tso_seg_pool.tso_mutex);
	c_element = pdev->tso_seg_pool.freelist;
	i = pdev->tso_seg_pool.pool_size;

	pdev->tso_seg_pool.freelist = NULL;
	pdev->tso_seg_pool.num_free = 0;
	pdev->tso_seg_pool.pool_size = 0;

	qdf_spin_unlock_bh(&pdev->tso_seg_pool.tso_mutex);
	qdf_spinlock_destroy(&pdev->tso_seg_pool.tso_mutex);

	while (i-- > 0 && c_element) {
		temp = c_element->next;
		if (c_element->on_freelist != 1) {
			qdf_tso_seg_dbg_bug("seg already freed (double?)");
			return;
		} else if (c_element->cookie != TSO_SEG_MAGIC_COOKIE) {
			qdf_tso_seg_dbg_bug("seg cookie is bad (corruption?)");
			return;
		}
		/* free this seg, so reset the cookie value*/
		c_element->cookie = 0;
		qdf_mem_free(c_element);
		c_element = temp;
	}
}

/**
 * ol_tso_num_seg_list_init() - function to initialise the freelist of elements
 *				use to count the num of tso segments in jumbo
 *				skb packet freelist
 * @pdev: the data physical device sending the data
 * @num_seg: number of elements needs to be intialised
 *
 * Return: none
 */
void ol_tso_num_seg_list_init(struct ol_txrx_pdev_t *pdev, uint32_t num_seg)
{
	int i = 0;
	struct qdf_tso_num_seg_elem_t *c_element;

	/* Host should not allocate any c_element. */
	if (num_seg <= 0) {
		ol_txrx_err("%s: ERROR: Pool size passed is 0",
			   __func__);
		QDF_BUG(0);
		pdev->tso_num_seg_pool.num_seg_pool_size = i;
		qdf_spinlock_create(&pdev->tso_num_seg_pool.tso_num_seg_mutex);
		return;
	}

	c_element = qdf_mem_malloc(sizeof(struct qdf_tso_num_seg_elem_t));
	pdev->tso_num_seg_pool.freelist = c_element;
	for (i = 0; i < (num_seg - 1); i++) {
		if (qdf_unlikely(!c_element)) {
			ol_txrx_err("%s: ERROR: c_element NULL for num of seg %d",
				__func__, i);
			QDF_BUG(0);
			pdev->tso_num_seg_pool.num_seg_pool_size = i;
			pdev->tso_num_seg_pool.num_free = i;
			qdf_spinlock_create(&pdev->tso_num_seg_pool.
							tso_num_seg_mutex);
			return;
		}
		c_element->next =
			qdf_mem_malloc(sizeof(struct qdf_tso_num_seg_elem_t));
		c_element = c_element->next;
	}
	/*
	 * NULL check for the last c_element of the list or
	 * first c_element if num_seg is equal to 1.
	 */
	if (qdf_unlikely(!c_element)) {
		ol_txrx_err("%s: ERROR: c_element NULL for num of seg %d",
			   __func__, i);
		QDF_BUG(0);
		pdev->tso_num_seg_pool.num_seg_pool_size = i;
		pdev->tso_num_seg_pool.num_free = i;
		qdf_spinlock_create(&pdev->tso_num_seg_pool.tso_num_seg_mutex);
		return;
	}
	c_element->next = NULL;
	pdev->tso_num_seg_pool.num_seg_pool_size = num_seg;
	pdev->tso_num_seg_pool.num_free = num_seg;
	qdf_spinlock_create(&pdev->tso_num_seg_pool.tso_num_seg_mutex);
}

/**
 * ol_tso_num_seg_list_deinit() - function to de-initialise the freelist of
 *				  elements use to count the num of tso segment
 *				  in a jumbo skb packet freelist
 * @pdev: the data physical device sending the data
 *
 * Return: none
 */
void ol_tso_num_seg_list_deinit(struct ol_txrx_pdev_t *pdev)
{
	int i;
	struct qdf_tso_num_seg_elem_t *c_element;
	struct qdf_tso_num_seg_elem_t *temp;

	/* pool size 0 implies that tso num seg list is not initialised*/
	if (pdev->tso_num_seg_pool.freelist == NULL &&
	    pdev->tso_num_seg_pool.num_seg_pool_size == 0)
		return;

	qdf_spin_lock_bh(&pdev->tso_num_seg_pool.tso_num_seg_mutex);
	c_element = pdev->tso_num_seg_pool.freelist;
	i = pdev->tso_num_seg_pool.num_seg_pool_size;

	pdev->tso_num_seg_pool.freelist = NULL;
	pdev->tso_num_seg_pool.num_free = 0;
	pdev->tso_num_seg_pool.num_seg_pool_size = 0;

	qdf_spin_unlock_bh(&pdev->tso_num_seg_pool.tso_num_seg_mutex);
	qdf_spinlock_destroy(&pdev->tso_num_seg_pool.tso_num_seg_mutex);

	while (i-- > 0 && c_element) {
		temp = c_element->next;
		qdf_mem_free(c_element);
		c_element = temp;
	}
}
#endif /* FEATURE_TSO */