/*
 * Copyright (c) 2004,2005 Voltaire Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id$
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>

#include <common.h>
#include <umad.h>
#include <mad.h>

#undef DEBUG
#define DEBUG	if (ibdebug)	WARN

void
mad_decode_field(uint8 *buf, int field, void *val)
{
	ib_field_t *f = ib_mad_f + field;

	if (!field) {
		*(int *)val = *(int *)buf;
		return;
	}
	if (f->bitlen <= 32) {
		*(uint32 *)val = _get_field(buf, 0, f);
		return;
	}
	if (f->bitlen == 64) {
		*(uint64 *)val = _get_field64(buf, 0, f);
		return;
	}
	_get_array(buf, 0, f, val);
}

void
mad_encode_field(uint8 *buf, int field, void *val)
{
	ib_field_t *f = ib_mad_f + field;

	if (!field) {
		*(int *)buf = *(int *)val;
		return;
	}
	if (f->bitlen <= 32) {
		_set_field(buf, 0, f, *(uint32 *)val);
		return;
	}
	if (f->bitlen == 64) {
		_set_field64(buf, 0, f, *(uint64 *)val);
		return;
	}
	_set_array(buf, 0, f, val);
}


uint64
mad_trid(void)
{
	static uint64 base;
	static uint64 trid;
	uint64 next;

	if (!base) {
		srandom(time(0)*getpid());
		base = random();
		trid = random();
	}
	next = ++trid | (base << 32);
	return next;
}

void *
mad_encode(void *buf, ib_rpc_t *rpc, ib_dr_path_t *drpath, void *data)
{
	int is_resp = rpc->method & IB_MAD_RESPONSE;

	/* first word */
	mad_set_field(buf, 0, IB_MAD_METHOD_F, rpc->method);
	mad_set_field(buf, 0, IB_MAD_RESPONSE_F, is_resp ? 1 : 0);
	mad_set_field(buf, 0, IB_MAD_CLASSVER_F, rpc->mgtclass == IB_SA_CLASS ? 2 : 1);
	mad_set_field(buf, 0, IB_MAD_MGMTCLASS_F, rpc->mgtclass);
	mad_set_field(buf, 0, IB_MAD_BASEVER_F, 1);

	/* second word */
	if (rpc->mgtclass == IB_SMI_DIRECT_CLASS) {
		if (!drpath) {
			WARN("encoding dr mad without drpath (null)");
			return 0;
		}
		mad_set_field(buf, 0, IB_DRSMP_HOPCNT_F, drpath->cnt);
		mad_set_field(buf, 0, IB_DRSMP_HOPPTR_F, is_resp ? drpath->cnt + 1 : 0x0);
		mad_set_field(buf, 0, IB_DRSMP_STATUS_F, rpc->rstatus);
		mad_set_field(buf, 0, IB_DRSMP_DIRECTION_F, is_resp ? 1 : 0);	/* out */
	} else
		mad_set_field(buf, 0, IB_MAD_STATUS_F, rpc->rstatus);

	/* words 3,4,5,6 */
	if (!rpc->trid)
		rpc->trid = mad_trid();

	mad_set_field64(buf, 0, IB_MAD_TRID_F, rpc->trid);
	mad_set_field(buf, 0, IB_MAD_ATTRID_F, rpc->attr.id);
	mad_set_field(buf, 0, IB_MAD_ATTRMOD_F, rpc->attr.mod);

	/* words 7,8 */
	mad_set_field(buf, 0, IB_MAD_MKEY_F, rpc->mkey >> 32);
	mad_set_field(buf, 4, IB_MAD_MKEY_F, rpc->mkey & 0xffffffff);

	if (rpc->mgtclass == IB_SMI_DIRECT_CLASS) {
		/* word 9 */
		mad_set_field(buf, 0, IB_DRSMP_DRDLID_F, drpath->drdlid ? drpath->drdlid : 0xffff);
		mad_set_field(buf, 0, IB_DRSMP_DRSLID_F, drpath->drslid ? drpath->drslid : 0xffff);

		/* bytes 128 - 256 - by default should be zero due memset*/
		if (is_resp)
			mad_set_array(buf, 0, IB_DRSMP_RPATH_F, drpath->p);
		else
			mad_set_array(buf, 0, IB_DRSMP_PATH_F, drpath->p);
	}

	if (rpc->mgtclass == IB_SA_CLASS)
		mad_set_field64(buf, 0, IB_SA_COMPMASK_F, rpc->mask);

	if (data)
		memcpy((char *)buf + rpc->dataoffs, data, rpc->datasz);

	// vendor mads range 2
	if (mad_is_vendor_range2(rpc->mgtclass))
		mad_set_field(buf, 0, IB_VEND2_OUI_F, rpc->oui);

	return (uint8 *)buf + IB_MAD_SIZE;
}

int
mad_build_pkt(void *umad, ib_rpc_t *rpc, ib_portid_t *dport, ib_rmpp_hdr_t *rmpp, void *data)
{
	uint8 *p, *mad;
	int lid_routed = rpc->mgtclass != IB_SMI_DIRECT_CLASS;
	int is_smi = (rpc->mgtclass == IB_SMI_CLASS || rpc->mgtclass == IB_SMI_DIRECT_CLASS);

	if (!is_smi)
		umad_set_addr(umad, dport->lid, dport->qp, dport->sl, dport->qkey);
	else if (lid_routed)
		umad_set_addr(umad, dport->lid, dport->qp, 0, 0);
	else
		umad_set_addr(umad, 0xffff, 0, 0, 0);	 /* direct routed smi */

	umad_set_grh(umad, (dport->grh && !is_smi) ? 0/*grh*/ : 0);	/* FIXME: GRH support */
	umad_set_pkey(umad, is_smi ? 0 : dport->pkey_idx);

	mad = umad_get_mad(umad);
	p = mad_encode(mad, rpc, lid_routed ? 0 : &dport->drpath, data);

	if (!is_smi && rmpp) {
		mad_set_field(mad, 0, IB_SA_RMPP_VERS_F, 1);
		mad_set_field(mad, 0, IB_SA_RMPP_TYPE_F, rmpp->type);
		mad_set_field(mad, 0, IB_SA_RMPP_RESP_F, 0x3f);
		mad_set_field(mad, 0, IB_SA_RMPP_FLAGS_F, rmpp->flags);
		mad_set_field(mad, 0, IB_SA_RMPP_STATUS_F, rmpp->status);
		mad_set_field(mad, 0, IB_SA_RMPP_D1_F, rmpp->d1.u);
		mad_set_field(mad, 0, IB_SA_RMPP_D2_F, rmpp->d2.u);
	}

	return p - mad;
}

