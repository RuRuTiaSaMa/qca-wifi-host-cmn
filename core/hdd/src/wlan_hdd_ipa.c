/*
 * Copyright (c) 2013-2018 The Linux Foundation. All rights reserved.
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

/**
 * DOC: wlan_hdd_ipa.c
 *
 * WLAN HDD and ipa interface implementation
 */

/* Include Files */
#include <wlan_hdd_includes.h>
#include <wlan_hdd_ipa.h>
#include "wlan_policy_mgr_api.h"
#include "wlan_ipa_ucfg_api.h"
#include <wlan_hdd_softap_tx_rx.h>
#include <linux/inetdevice.h>

void hdd_ipa_set_tx_flow_info(void)
{
	struct hdd_adapter *adapter;
	struct hdd_station_ctx *sta_ctx;
	struct hdd_ap_ctx *hdd_ap_ctx;
	struct hdd_hostapd_state *hostapd_state;
	struct qdf_mac_addr staBssid = QDF_MAC_ADDR_ZERO_INIT;
	struct qdf_mac_addr p2pBssid = QDF_MAC_ADDR_ZERO_INIT;
	struct qdf_mac_addr apBssid = QDF_MAC_ADDR_ZERO_INIT;
	uint8_t staChannel = 0, p2pChannel = 0, apChannel = 0;
	const char *p2pMode = "DEV";
	struct hdd_context *hdd_ctx;
	struct cds_context *cds_ctx;
#ifdef QCA_LL_LEGACY_TX_FLOW_CONTROL
	uint8_t targetChannel = 0;
	uint8_t preAdapterChannel = 0;
	uint8_t channel24;
	uint8_t channel5;
	struct hdd_adapter *preAdapterContext = NULL;
	struct hdd_adapter *adapter2_4 = NULL;
	struct hdd_adapter *adapter5 = NULL;
	void *soc = cds_get_context(QDF_MODULE_ID_SOC);
#endif /* QCA_LL_LEGACY_TX_FLOW_CONTROL */
	struct wlan_objmgr_psoc *psoc;

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	if (!hdd_ctx) {
		hdd_err("HDD context is NULL");
		return;
	}

	cds_ctx = cds_get_context(QDF_MODULE_ID_QDF);
	if (!cds_ctx) {
		hdd_err("Invalid CDS Context");
		return;
	}

	psoc = hdd_ctx->hdd_psoc;

	hdd_for_each_adapter(hdd_ctx, adapter) {
		switch (adapter->device_mode) {
		case QDF_STA_MODE:
			sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(adapter);
			if (eConnectionState_Associated ==
			    sta_ctx->conn_info.connState) {
				staChannel =
					sta_ctx->conn_info.operationChannel;
				qdf_copy_macaddr(&staBssid,
						 &sta_ctx->conn_info.bssId);
#ifdef QCA_LL_LEGACY_TX_FLOW_CONTROL
				targetChannel = staChannel;
#endif /* QCA_LL_LEGACY_TX_FLOW_CONTROL */
			}
			break;
		case QDF_P2P_CLIENT_MODE:
			sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(adapter);
			if (eConnectionState_Associated ==
			    sta_ctx->conn_info.connState) {
				p2pChannel =
					sta_ctx->conn_info.operationChannel;
				qdf_copy_macaddr(&p2pBssid,
						&sta_ctx->conn_info.bssId);
				p2pMode = "CLI";
#ifdef QCA_LL_LEGACY_TX_FLOW_CONTROL
				targetChannel = p2pChannel;
#endif /* QCA_LL_LEGACY_TX_FLOW_CONTROL */
			}
			break;
		case QDF_P2P_GO_MODE:
			hdd_ap_ctx = WLAN_HDD_GET_AP_CTX_PTR(adapter);
			hostapd_state = WLAN_HDD_GET_HOSTAP_STATE_PTR(adapter);
			if (hostapd_state->bss_state == BSS_START
			    && hostapd_state->qdf_status ==
			    QDF_STATUS_SUCCESS) {
				p2pChannel = hdd_ap_ctx->operating_channel;
				qdf_copy_macaddr(&p2pBssid,
						 &adapter->mac_addr);
#ifdef QCA_LL_LEGACY_TX_FLOW_CONTROL
				targetChannel = p2pChannel;
#endif /* QCA_LL_LEGACY_TX_FLOW_CONTROL */
			}
			p2pMode = "GO";
			break;
		case QDF_SAP_MODE:
			hdd_ap_ctx = WLAN_HDD_GET_AP_CTX_PTR(adapter);
			hostapd_state = WLAN_HDD_GET_HOSTAP_STATE_PTR(adapter);
			if (hostapd_state->bss_state == BSS_START
			    && hostapd_state->qdf_status ==
			    QDF_STATUS_SUCCESS) {
				apChannel = hdd_ap_ctx->operating_channel;
				qdf_copy_macaddr(&apBssid,
						&adapter->mac_addr);
#ifdef QCA_LL_LEGACY_TX_FLOW_CONTROL
				targetChannel = apChannel;
#endif /* QCA_LL_LEGACY_TX_FLOW_CONTROL */
			}
			break;
		case QDF_IBSS_MODE:
		default:
			break;
		}
#ifdef QCA_LL_LEGACY_TX_FLOW_CONTROL
		if (targetChannel) {
			/*
			 * This is first adapter detected as active
			 * set as default for none concurrency case
			 */
			if (!preAdapterChannel) {
				/* If IPA UC data path is enabled,
				 * target should reserve extra tx descriptors
				 * for IPA data path.
				 * Then host data path should allow less TX
				 * packet pumping in case IPA
				 * data path enabled
				 */
				if (ucfg_ipa_uc_is_enabled() &&
				    (QDF_SAP_MODE == adapter->device_mode)) {
					adapter->tx_flow_low_watermark =
					hdd_ctx->config->TxFlowLowWaterMark +
					WLAN_TFC_IPAUC_TX_DESC_RESERVE;
				} else {
					adapter->tx_flow_low_watermark =
						hdd_ctx->config->
							TxFlowLowWaterMark;
				}
				adapter->tx_flow_high_watermark_offset =
				   hdd_ctx->config->TxFlowHighWaterMarkOffset;
				cdp_fc_ll_set_tx_pause_q_depth(soc,
					adapter->session_id,
					hdd_ctx->config->TxFlowMaxQueueDepth);
				hdd_info("MODE %d,CH %d,LWM %d,HWM %d,TXQDEP %d",
				    adapter->device_mode,
				    targetChannel,
				    adapter->tx_flow_low_watermark,
				    adapter->tx_flow_low_watermark +
				    adapter->tx_flow_high_watermark_offset,
				    hdd_ctx->config->TxFlowMaxQueueDepth);
				preAdapterChannel = targetChannel;
				preAdapterContext = adapter;
			} else {
				/*
				 * SCC, disable TX flow control for both
				 * SCC each adapter cannot reserve dedicated
				 * channel resource, as a result, if any adapter
				 * blocked OS Q by flow control,
				 * blocked adapter will lost chance to recover
				 */
				if (preAdapterChannel == targetChannel) {
					/* Current adapter */
					adapter->tx_flow_low_watermark = 0;
					adapter->
					tx_flow_high_watermark_offset = 0;
					cdp_fc_ll_set_tx_pause_q_depth(soc,
						adapter->session_id,
						hdd_ctx->config->
						TxHbwFlowMaxQueueDepth);
					hdd_info("SCC: MODE %s(%d), CH %d, LWM %d, HWM %d, TXQDEP %d",
					       hdd_device_mode_to_string(
							adapter->device_mode),
					       adapter->device_mode,
					       targetChannel,
					       adapter->tx_flow_low_watermark,
					       adapter->tx_flow_low_watermark +
					       adapter->
					       tx_flow_high_watermark_offset,
					       hdd_ctx->config->
					       TxHbwFlowMaxQueueDepth);

					if (!preAdapterContext) {
						hdd_err("SCC: Previous adapter context NULL");
						continue;
					}

					/* Previous adapter */
					preAdapterContext->
					tx_flow_low_watermark = 0;
					preAdapterContext->
					tx_flow_high_watermark_offset = 0;
					cdp_fc_ll_set_tx_pause_q_depth(soc,
						preAdapterContext->session_id,
						hdd_ctx->config->
						TxHbwFlowMaxQueueDepth);
					hdd_info("SCC: MODE %s(%d), CH %d, LWM %d, HWM %d, TXQDEP %d",
					       hdd_device_mode_to_string(
						preAdapterContext->device_mode
							  ),
					       preAdapterContext->device_mode,
					       targetChannel,
					       preAdapterContext->
					       tx_flow_low_watermark,
					       preAdapterContext->
					       tx_flow_low_watermark +
					       preAdapterContext->
					       tx_flow_high_watermark_offset,
					       hdd_ctx->config->
					       TxHbwFlowMaxQueueDepth);
				}
				/*
				 * MCC, each adapter will have dedicated
				 * resource
				 */
				else {
					/* current channel is 2.4 */
					if (targetChannel <=
				     WLAN_HDD_TX_FLOW_CONTROL_MAX_24BAND_CH) {
						channel24 = targetChannel;
						channel5 = preAdapterChannel;
						adapter2_4 = adapter;
						adapter5 = preAdapterContext;
					} else {
						/* Current channel is 5 */
						channel24 = preAdapterChannel;
						channel5 = targetChannel;
						adapter2_4 = preAdapterContext;
						adapter5 = adapter;
					}

					if (!adapter5) {
						hdd_err("MCC: 5GHz adapter context NULL");
						continue;
					}
					adapter5->tx_flow_low_watermark =
						hdd_ctx->config->
						TxHbwFlowLowWaterMark;
					adapter5->
					tx_flow_high_watermark_offset =
						hdd_ctx->config->
						TxHbwFlowHighWaterMarkOffset;
					cdp_fc_ll_set_tx_pause_q_depth(soc,
						adapter5->session_id,
						hdd_ctx->config->
						TxHbwFlowMaxQueueDepth);
					hdd_info("MCC: MODE %s(%d), CH %d, LWM %d, HWM %d, TXQDEP %d",
					    hdd_device_mode_to_string(
						    adapter5->device_mode),
					    adapter5->device_mode,
					    channel5,
					    adapter5->tx_flow_low_watermark,
					    adapter5->
					    tx_flow_low_watermark +
					    adapter5->
					    tx_flow_high_watermark_offset,
					    hdd_ctx->config->
					    TxHbwFlowMaxQueueDepth);

					if (!adapter2_4) {
						hdd_err("MCC: 2.4GHz adapter context NULL");
						continue;
					}
					adapter2_4->tx_flow_low_watermark =
						hdd_ctx->config->
						TxLbwFlowLowWaterMark;
					adapter2_4->
					tx_flow_high_watermark_offset =
						hdd_ctx->config->
						TxLbwFlowHighWaterMarkOffset;
					cdp_fc_ll_set_tx_pause_q_depth(soc,
						adapter2_4->session_id,
						hdd_ctx->config->
						TxLbwFlowMaxQueueDepth);
					hdd_info("MCC: MODE %s(%d), CH %d, LWM %d, HWM %d, TXQDEP %d",
						hdd_device_mode_to_string(
						    adapter2_4->device_mode),
						adapter2_4->device_mode,
						channel24,
						adapter2_4->
						tx_flow_low_watermark,
						adapter2_4->
						tx_flow_low_watermark +
						adapter2_4->
						tx_flow_high_watermark_offset,
						hdd_ctx->config->
						TxLbwFlowMaxQueueDepth);

				}
			}
		}
		targetChannel = 0;
#endif /* QCA_LL_LEGACY_TX_FLOW_CONTROL */
	}
}

#ifdef QCA_CONFIG_SMP
static int hdd_ipa_aggregated_rx_ind(qdf_nbuf_t skb)
{
	return netif_rx_ni(skb);
}
#else
static int hdd_ipa_aggregated_rx_ind(qdf_nbuf_t skb)
{
	struct iphdr *ip_h;
	static atomic_t softirq_mitigation_cntr =
		ATOMIC_INIT(IPA_WLAN_RX_SOFTIRQ_THRESH);
	int result;

	ip_h = (struct iphdr *)(skb->data);
	if ((skb->protocol == htons(ETH_P_IP)) &&
		(ip_h->protocol == IPPROTO_ICMP)) {
		result = netif_rx_ni(skb);
	} else {
		/* Call netif_rx_ni for every IPA_WLAN_RX_SOFTIRQ_THRESH packets
		 * to avoid excessive softirq's.
		 */
		if (atomic_dec_and_test(&softirq_mitigation_cntr)) {
			result = netif_rx_ni(skb);
			atomic_set(&softirq_mitigation_cntr,
					IPA_WLAN_RX_SOFTIRQ_THRESH);
		} else {
			result = netif_rx(skb);
		}
	}

	return result;
}
#endif

void hdd_ipa_send_skb_to_network(qdf_nbuf_t skb, qdf_netdev_t dev)
{
	struct hdd_adapter *adapter = (struct hdd_adapter *) netdev_priv(dev);
	int result;
	unsigned int cpu_index;
	uint8_t staid;

	if (hdd_validate_adapter(adapter)) {
		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_DEBUG,
			  "Invalid adapter: 0x%pK", adapter);
		kfree_skb(skb);
		return;
	}

	if (cds_is_driver_unloading()) {
		kfree_skb(skb);
		return;
	}

	if (adapter->device_mode == QDF_SAP_MODE) {
		/* Send DHCP Indication to FW */
		struct qdf_mac_addr *src_mac =
			(struct qdf_mac_addr *)(skb->data +
			QDF_NBUF_SRC_MAC_OFFSET);
		if (QDF_STATUS_SUCCESS ==
			hdd_softap_get_sta_id(adapter, src_mac, &staid))
			hdd_inspect_dhcp_packet(adapter, staid, skb, QDF_RX);
	}

	skb->dev = adapter->dev;
	skb->protocol = eth_type_trans(skb, skb->dev);
	skb->ip_summed = CHECKSUM_NONE;

	cpu_index = wlan_hdd_get_cpu();

	++adapter->hdd_stats.tx_rx_stats.rx_packets[cpu_index];

	/*
	 * Update STA RX exception packet stats.
	 * For SAP as part of IPA HW stats are updated.
	 */
	if (adapter->device_mode == QDF_STA_MODE) {
		++adapter->stats.rx_packets;
		adapter->stats.rx_bytes += skb->len;
	}

	result = hdd_ipa_aggregated_rx_ind(skb);
	if (result == NET_RX_SUCCESS)
		++adapter->hdd_stats.tx_rx_stats.rx_delivered[cpu_index];
	else
		++adapter->hdd_stats.tx_rx_stats.rx_refused[cpu_index];
}

void hdd_ipa_set_mcc_mode(bool mcc_mode)
{
	struct hdd_context *hdd_ctx;

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	if (!hdd_ctx) {
		hdd_err("HDD context is NULL");
		return;
	}

	ucfg_ipa_set_mcc_mode(hdd_ctx->hdd_pdev, mcc_mode);
}