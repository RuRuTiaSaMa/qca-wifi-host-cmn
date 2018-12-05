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
 * DOC: Implement API's specific to NAN component.
 */

#include <wmi_unified_priv.h>
#include <wmi_unified_nan_api.h>

QDF_STATUS wmi_unified_ndp_initiator_req_cmd_send(void *wmi_hdl,
					struct nan_datapath_initiator_req *req)
{
	wmi_unified_t wmi_handle = wmi_hdl;

	if (wmi_handle->ops->send_ndp_initiator_req_cmd)
		return wmi_handle->ops->send_ndp_initiator_req_cmd(wmi_handle,
								   req);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_unified_ndp_responder_req_cmd_send(void *wmi_hdl,
				struct nan_datapath_responder_req *req)
{
	wmi_unified_t wmi_handle = wmi_hdl;

	if (wmi_handle->ops->send_ndp_responder_req_cmd)
		return wmi_handle->ops->send_ndp_responder_req_cmd(wmi_handle,
								   req);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_unified_ndp_end_req_cmd_send(void *wmi_hdl,
					    struct nan_datapath_end_req *req)
{
	wmi_unified_t wmi_handle = wmi_hdl;

	if (wmi_handle->ops->send_ndp_end_req_cmd)
		return wmi_handle->ops->send_ndp_end_req_cmd(wmi_handle,
							     req);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_extract_ndp_initiator_rsp(wmi_unified_t wmi_handle,
			uint8_t *data, struct nan_datapath_initiator_rsp *rsp)
{
	if (wmi_handle->ops->extract_ndp_initiator_rsp)
		return wmi_handle->ops->extract_ndp_initiator_rsp(wmi_handle,
								  data, rsp);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_extract_ndp_ind(wmi_unified_t wmi_handle, uint8_t *data,
			       struct nan_datapath_indication_event *ind)
{
	if (wmi_handle->ops->extract_ndp_ind)
		return wmi_handle->ops->extract_ndp_ind(wmi_handle,
							data, ind);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_extract_ndp_confirm(wmi_unified_t wmi_handle, uint8_t *data,
				   struct nan_datapath_confirm_event *ev)
{
	if (wmi_handle->ops->extract_ndp_confirm)
		return wmi_handle->ops->extract_ndp_confirm(wmi_handle,
							    data, ev);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_extract_ndp_responder_rsp(wmi_unified_t wmi_handle,
					 uint8_t *data,
					 struct nan_datapath_responder_rsp *rsp)
{
	if (wmi_handle->ops->extract_ndp_responder_rsp)
		return wmi_handle->ops->extract_ndp_responder_rsp(wmi_handle,
								  data, rsp);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_extract_ndp_end_rsp(wmi_unified_t wmi_handle, uint8_t *data,
				   struct nan_datapath_end_rsp_event *rsp)
{
	if (wmi_handle->ops->extract_ndp_end_rsp)
		return wmi_handle->ops->extract_ndp_end_rsp(wmi_handle,
							    data, rsp);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_extract_ndp_end_ind(wmi_unified_t wmi_handle, uint8_t *data,
				struct nan_datapath_end_indication_event **ind)
{
	if (wmi_handle->ops->extract_ndp_end_ind)
		return wmi_handle->ops->extract_ndp_end_ind(wmi_handle,
							    data, ind);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS wmi_extract_ndp_sch_update(wmi_unified_t wmi_handle, uint8_t *data,
				    struct nan_datapath_sch_update_event *ind)
{
	if (wmi_handle->ops->extract_ndp_sch_update)
		return wmi_handle->ops->extract_ndp_sch_update(wmi_handle,
							       data, ind);

	return QDF_STATUS_E_FAILURE;
}
