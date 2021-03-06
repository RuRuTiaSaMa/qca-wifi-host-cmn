/*
 * Copyright (c) 2017 The Linux Foundation. All rights reserved.
 */
/*
 * CTR with CBC-MAC Protocol (CCMP)
 * Copyright (c) 2010-2012, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include <qdf_types.h>
#include <qdf_mem.h>
#include <qdf_util.h>
#include "wlan_crypto_aes_i.h"
#include "wlan_crypto_def_i.h"

static void ccmp_aad_nonce(const struct ieee80211_hdr *hdr, const uint8_t *data,
			   uint8_t *aad, size_t *aad_len, uint8_t *nonce)
{
	uint16_t fc, stype, seq;
	int qos = 0, addr4 = 0;
	uint8_t *pos;

	nonce[0] = 0;

	fc = qdf_le16_to_cpu(hdr->frame_control);
	stype = WLAN_FC_GET_STYPE(fc);
	if ((fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) ==
	    (WLAN_FC_TODS | WLAN_FC_FROMDS))
		addr4 = 1;

	if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_DATA) {
		fc &= ~0x0070; /* Mask subtype bits */
		if (stype & 0x08) {
			const uint8_t *qc;
			qos = 1;
			fc &= ~WLAN_FC_ORDER;
			qc = (const uint8_t *) (hdr + 1);
			if (addr4)
				qc += WLAN_ALEN;
			nonce[0] = qc[0] & 0x0f;
		}
	} else if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT)
		nonce[0] |= 0x10; /* Management */

	fc &= ~(WLAN_FC_RETRY | WLAN_FC_PWRMGT | WLAN_FC_MOREDATA);
	fc |= WLAN_FC_ISWEP;
	wlan_crypto_put_le16(aad, fc);
	pos = aad + 2;
	qdf_mem_copy(pos, hdr->addr1, 3 * WLAN_ALEN);
	pos += 3 * WLAN_ALEN;
	seq = qdf_le16_to_cpu(hdr->seq_ctrl);
	seq &= ~0xfff0; /* Mask Seq#; do not modify Frag# */
	wlan_crypto_put_le16(pos, seq);
	pos += 2;

	qdf_mem_copy(pos, hdr + 1, addr4 * WLAN_ALEN + qos * 2);
	pos += addr4 * WLAN_ALEN;
	if (qos) {
		pos[0] &= ~0x70;
		if (1 /* FIX: either device has SPP A-MSDU Capab = 0 */)
			pos[0] &= ~0x80;
		pos++;
		*pos++ = 0x00;
	}

	*aad_len = pos - aad;

	qdf_mem_copy(nonce + 1, hdr->addr2, WLAN_ALEN);
	nonce[7] = data[7]; /* PN5 */
	nonce[8] = data[6]; /* PN4 */
	nonce[9] = data[5]; /* PN3 */
	nonce[10] = data[4]; /* PN2 */
	nonce[11] = data[1]; /* PN1 */
	nonce[12] = data[0]; /* PN0 */
}


uint8_t *wlan_crypto_ccmp_decrypt(const uint8_t *tk,
					const struct ieee80211_hdr *hdr,
					uint8_t *data, size_t data_len){
	uint8_t aad[30], nonce[13];
	size_t aad_len;
	size_t mlen;
	uint8_t *plain;

	if (data_len < 8 + 8)
		return NULL;

	plain = qdf_mem_malloc(data_len + AES_BLOCK_SIZE);
	if (plain == NULL) {
		qdf_print("%s[%d] mem alloc failed\n", __func__, __LINE__);
		return NULL;
	}

	mlen = data_len - 8 - 8;

	qdf_mem_set(aad, sizeof(aad), 0);
	ccmp_aad_nonce(hdr, data, aad, &aad_len, nonce);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP AAD", aad, aad_len);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP nonce", nonce, 13);

	if (wlan_crypto_aes_ccm_ad(tk, 16, nonce, 8, data + 8, mlen,
				aad, aad_len, data + 8 + mlen, plain) < 0) {
		/*uint16_t seq_ctrl = qdf_le16_to_cpu(hdr->seq_ctrl);
		wpa_printf(MSG_INFO, "Invalid CCMP MIC in frame: A1=" MACSTR
			" A2=" MACSTR " A3=" MACSTR " seq=%u frag=%u",
			MAC2STR(hdr->addr1), MAC2STR(hdr->addr2),
			MAC2STR(hdr->addr3),
			WLAN_GET_SEQ_SEQ(seq_ctrl),
			WLAN_GET_SEQ_FRAG(seq_ctrl));*/
		qdf_mem_free(plain);
		return NULL;
	}
	wpa_hexdump(MSG_EXCESSIVE, "CCMP decrypted", plain, mlen);

	qdf_mem_copy(data, plain, data_len);
	qdf_mem_free(plain);
	return data;
}


void ccmp_get_pn(uint8_t *pn, const uint8_t *data)
{
	pn[0] = data[7]; /* PN5 */
	pn[1] = data[6]; /* PN4 */
	pn[2] = data[5]; /* PN3 */
	pn[3] = data[4]; /* PN2 */
	pn[4] = data[1]; /* PN1 */
	pn[5] = data[0]; /* PN0 */
}


uint8_t *wlan_crypto_ccmp_encrypt(const uint8_t *tk, uint8_t *frame,
					size_t len, size_t hdrlen){
	uint8_t aad[30], nonce[13];
	size_t aad_len, plen;
	uint8_t *crypt, *pos;
	struct ieee80211_hdr *hdr;

	if (len < hdrlen || hdrlen < 24)
		return NULL;
	plen = len - hdrlen;

	crypt = qdf_mem_malloc(hdrlen + 8 + plen + 8 + AES_BLOCK_SIZE);
	if (crypt == NULL) {
		qdf_print("%s[%d] mem alloc failed\n", __func__, __LINE__);
		return NULL;
	}

	qdf_mem_copy(crypt, frame, hdrlen);

	hdr = (struct ieee80211_hdr *) crypt;
	hdr->frame_control |= qdf_cpu_to_le16(WLAN_FC_ISWEP);
	pos = crypt + hdrlen + 8;

	qdf_mem_set(aad, sizeof(aad), 0);
	ccmp_aad_nonce(hdr, crypt + hdrlen, aad, &aad_len, nonce);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP AAD", aad, aad_len);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP nonce", nonce, 13);

	if (wlan_crypto_aes_ccm_ae(tk, 16, nonce, 8, frame + hdrlen,
			plen, aad, aad_len, pos, pos + plen) < 0) {
		qdf_mem_free(crypt);
		return NULL;
	}

	qdf_mem_copy(frame, crypt, len);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP encrypted", crypt + hdrlen + 8, plen);
	qdf_mem_free(crypt);

	return frame;
}


uint8_t *wlan_crypto_ccmp_256_decrypt(const uint8_t *tk,
					const struct ieee80211_hdr *hdr,
					const uint8_t *data, size_t data_len,
					size_t *decrypted_len){
	uint8_t aad[30], nonce[13];
	size_t aad_len;
	size_t mlen;
	uint8_t *plain;

	if (data_len < 8 + 16)
		return NULL;

	plain = qdf_mem_malloc(data_len + AES_BLOCK_SIZE);
	if (plain == NULL) {
		qdf_print("%s[%d] mem alloc failed\n", __func__, __LINE__);
		return NULL;
	}

	mlen = data_len - 8 - 16;

	qdf_mem_set(aad, sizeof(aad), 0);
	ccmp_aad_nonce(hdr, data, aad, &aad_len, nonce);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP-256 AAD", aad, aad_len);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP-256 nonce", nonce, 13);

	if (wlan_crypto_aes_ccm_ad(tk, 32, nonce, 16, data + 8, mlen,
			aad, aad_len, data + 8 + mlen, plain) < 0) {
		/*uint16_t seq_ctrl = qdf_le16_to_cpu(hdr->seq_ctrl);
		wpa_printf(MSG_INFO, "Invalid CCMP-256 MIC in frame: A1=" MACSTR
			   " A2=" MACSTR " A3=" MACSTR " seq=%u frag=%u",
			   MAC2STR(hdr->addr1), MAC2STR(hdr->addr2),
			   MAC2STR(hdr->addr3),
			   WLAN_GET_SEQ_SEQ(seq_ctrl),
			   WLAN_GET_SEQ_FRAG(seq_ctrl));*/
		qdf_mem_free(plain);
		return NULL;
	}
	wpa_hexdump(MSG_EXCESSIVE, "CCMP-256 decrypted", plain, mlen);

	*decrypted_len = mlen;
	return plain;
}


uint8_t *wlan_crypto_ccmp_256_encrypt(const uint8_t *tk, uint8_t *frame,
					size_t len, size_t hdrlen, uint8_t *qos,
					uint8_t *pn, int keyid,
					size_t *encrypted_len){
	uint8_t aad[30], nonce[13];
	size_t aad_len, plen;
	uint8_t *crypt, *pos;
	struct ieee80211_hdr *hdr;

	if (len < hdrlen || hdrlen < 24)
		return NULL;
	plen = len - hdrlen;

	crypt = qdf_mem_malloc(hdrlen + 8 + plen + 16 + AES_BLOCK_SIZE);
	if (crypt == NULL) {
		qdf_print("%s[%d] mem alloc failed\n", __func__, __LINE__);
		return NULL;
	}

	qdf_mem_copy(crypt, frame, hdrlen);
	hdr = (struct ieee80211_hdr *) crypt;
	hdr->frame_control |= qdf_cpu_to_le16(WLAN_FC_ISWEP);
	pos = crypt + hdrlen;
	*pos++ = pn[5]; /* PN0 */
	*pos++ = pn[4]; /* PN1 */
	*pos++ = 0x00; /* Rsvd */
	*pos++ = 0x20 | (keyid << 6);
	*pos++ = pn[3]; /* PN2 */
	*pos++ = pn[2]; /* PN3 */
	*pos++ = pn[1]; /* PN4 */
	*pos++ = pn[0]; /* PN5 */

	qdf_mem_set(aad, sizeof(aad), 0);
	ccmp_aad_nonce(hdr, crypt + hdrlen, aad, &aad_len, nonce);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP-256 AAD", aad, aad_len);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP-256 nonce", nonce, 13);

	if (wlan_crypto_aes_ccm_ae(tk, 32, nonce, 16, frame + hdrlen, plen,
				aad, aad_len, pos, pos + plen) < 0) {
		qdf_mem_free(crypt);
		return NULL;
	}

	wpa_hexdump(MSG_EXCESSIVE, "CCMP-256 encrypted", crypt + hdrlen + 8,
		    plen);

	*encrypted_len = hdrlen + 8 + plen + 16;

	return crypt;
}
