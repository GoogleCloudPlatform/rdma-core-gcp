// SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB
/* Copyright (C) 2019 - 2023 Intel Corporation */
#if HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <malloc.h>
#include <linux/if_ether.h>
#include <infiniband/opcode.h>

#include "umain.h"
#include "abi.h"

static int irdma_validate_pd(struct ibv_pd *pd)
{
	struct irdma_upd *iwupd, *next;
	struct irdma_uvcontext *iwvctx = container_of(pd->context, struct irdma_uvcontext,
						      ibv_ctx.context);
	int ret;

	ret = irdma_spin_lock(&iwvctx->pd_lock);
	if (ret)
		return ret;

	list_for_each_safe(&iwvctx->pd_list, iwupd, next, list) {
		if (&iwupd->ibv_pd == pd) {
			irdma_spin_unlock(&iwvctx->pd_lock);
			return 0;
		}
	}

	irdma_spin_unlock(&iwvctx->pd_lock);

	return EINVAL;
}

static inline void print_fw_ver(uint64_t fw_ver, char *str, size_t len)
{
	uint16_t major, minor;

	major = fw_ver >> 32 & 0xffff;
	minor = fw_ver & 0xffff;

	snprintf(str, len, "%d.%d", major, minor);
}

/**
 * irdma_uquery_device_ex - query device attributes including extended properties
 * @context: user context for the device
 * @input: extensible input struct for ibv_query_device_ex verb
 * @attr: extended device attribute struct
 * @attr_size: size of extended device attribute struct
 **/
int irdma_uquery_device_ex(struct ibv_context *context,
			   const struct ibv_query_device_ex_input *input,
			   struct ibv_device_attr_ex *attr, size_t attr_size)
{
	struct ib_uverbs_ex_query_device_resp resp = {};
	size_t resp_size = sizeof(resp);
	int ret;

	ret = ibv_cmd_query_device_any(context, input, attr, attr_size,
				       &resp, &resp_size);
	if (ret)
		return ret;

	print_fw_ver(resp.base.fw_ver, attr->orig_attr.fw_ver, sizeof(attr->orig_attr.fw_ver));

	return 0;
}

struct ibv_mr *irdma_ureg_mr_dmabuf(struct ibv_pd *pd, uint64_t offset,
				    size_t length, uint64_t iova, int fd,
				    int access)
{
	struct verbs_mr *vmr;
	int err;

	vmr = malloc(sizeof(*vmr));
	if (!vmr)
		return NULL;

	err = ibv_cmd_reg_dmabuf_mr(pd, offset, length, iova, fd, access,
#if 1
				    vmr, NULL);
#else
				    vmr);
#endif
	if (err) {
		free(vmr);
		errno = err;
		return NULL;
	}

	return &vmr->ibv_mr;
}

/**
 * irdma_uquery_port - get port attributes (msg size, lnk, mtu...)
 * @context: user context of the device
 * @port: port for the attributes
 * @attr: to return port attributes
 **/
int irdma_uquery_port(struct ibv_context *context, uint8_t port,
		      struct ibv_port_attr *attr)
{
	struct ibv_query_port cmd;

	return ibv_cmd_query_port(context, port, attr, &cmd, sizeof(cmd));
}

struct ibv_pd *irdma_ualloc_parent_domain(struct ibv_context *context,
					  struct ibv_parent_domain_init_attr *init_attr)
{
	struct irdma_uparent_domain *iw_parent_domain;
	struct irdma_uvcontext *iwvctx;
	int ret;

	if (ibv_check_alloc_parent_domain(init_attr))
		return NULL;

	if (!check_comp_mask(init_attr->comp_mask,
			     IBV_PARENT_DOMAIN_INIT_ATTR_ALLOCATORS |
			     IBV_PARENT_DOMAIN_INIT_ATTR_PD_CONTEXT)) {
		errno = EINVAL;
		return NULL;
	}

	iw_parent_domain = calloc(1, sizeof(*iw_parent_domain));
	if (!iw_parent_domain)
		return NULL;

	if (init_attr->td) {
		iw_parent_domain->iwutd =
			container_of(init_attr->td, struct irdma_utd, ibv_td);
		atomic_fetch_add(&iw_parent_domain->iwutd->refcount, 1);
	}

	iw_parent_domain->iwupd.container_iwupd =
		container_of(init_attr->pd, struct irdma_upd, ibv_pd);

	atomic_fetch_add(&iw_parent_domain->iwupd.container_iwupd->refcount, 1);
	atomic_init(&iw_parent_domain->iwupd.refcount, 1);

	ibv_initialize_parent_domain(&iw_parent_domain->iwupd.ibv_pd,
				     &iw_parent_domain->iwupd.container_iwupd->ibv_pd);
	iwvctx = container_of(context, struct irdma_uvcontext, ibv_ctx.context);
	ret = irdma_spin_lock(&iwvctx->pd_lock);
	if (ret) {
		if (iw_parent_domain->iwutd)
			atomic_fetch_sub(&iw_parent_domain->iwutd->refcount, 1);

		atomic_fetch_sub(&iw_parent_domain->iwupd.container_iwupd->refcount, 1);
		free(iw_parent_domain);
		errno = ret;
		return NULL;
	}

	list_add_tail(&iwvctx->pd_list, &iw_parent_domain->iwupd.list);
	irdma_spin_unlock(&iwvctx->pd_lock);

	if (init_attr->comp_mask & IBV_PARENT_DOMAIN_INIT_ATTR_ALLOCATORS) {
		iw_parent_domain->alloc = init_attr->alloc;
		iw_parent_domain->free = init_attr->free;
	}

	if (init_attr->comp_mask & IBV_PARENT_DOMAIN_INIT_ATTR_PD_CONTEXT)
		iw_parent_domain->pd_context = init_attr->pd_context;

	return &iw_parent_domain->iwupd.ibv_pd;
}

static int irdma_udealloc_parent_domain(struct irdma_uparent_domain *iw_parent_domain)
{
	struct irdma_uvcontext *iwvctx;
	int ret;
	if (atomic_load(&iw_parent_domain->iwupd.refcount) > 1)
		return EBUSY;

	atomic_fetch_sub(&iw_parent_domain->iwupd.container_iwupd->refcount, 1);

	if (iw_parent_domain->iwutd)
		atomic_fetch_sub(&iw_parent_domain->iwutd->refcount, 1);

	iwvctx = container_of(iw_parent_domain->iwupd.ibv_pd.context,
			      struct irdma_uvcontext, ibv_ctx.context);
	ret = irdma_spin_lock(&iwvctx->pd_lock);
	if (ret)
		return ret;
	list_del(&iw_parent_domain->iwupd.list);
	irdma_spin_unlock(&iwvctx->pd_lock);

	free(iw_parent_domain);

	return 0;
}

/**
 * irdma_ualloc_pd - allocates protection domain and return pd ptr
 * @context: user context of the device
 **/
struct ibv_pd *irdma_ualloc_pd(struct ibv_context *context)
{
	struct ibv_alloc_pd cmd;
	struct irdma_ualloc_pd_resp resp = {};
	struct irdma_upd *iwupd;
	struct irdma_uvcontext *iwvctx = container_of(context, struct irdma_uvcontext,
						      ibv_ctx.context);
	int err;

	iwupd = calloc(1, sizeof(*iwupd));
	if (!iwupd)
		return NULL;

	err = ibv_cmd_alloc_pd(context, &iwupd->ibv_pd, &cmd, sizeof(cmd),
			       &resp.ibv_resp, sizeof(resp));
	if (err)
		goto err_free;

	iwupd->pd_id = resp.pd_id;
	err = irdma_spin_lock(&iwvctx->pd_lock);
	if (err)
		goto err_del_pd;

	list_add_tail(&iwvctx->pd_list, &iwupd->list);
	irdma_spin_unlock(&iwvctx->pd_lock);
	atomic_init(&iwupd->refcount, 1);

	return &iwupd->ibv_pd;

err_del_pd:
	ibv_cmd_dealloc_pd(&iwupd->ibv_pd);
err_free:
	free(iwupd);

	errno = err;
	return NULL;
}

/**
 * irdma_ufree_pd - free pd resources
 * @pd: pd to free resources
 */
int irdma_ufree_pd(struct ibv_pd *pd)
{
	struct irdma_uvcontext *iwvctx = container_of(pd->context, struct irdma_uvcontext,
						      ibv_ctx.context);
	struct irdma_uparent_domain *iw_parent_domain;
	struct irdma_upd *iwupd;
	int ret;

	iw_parent_domain = to_iw_uparent_domain(pd);
	if (iw_parent_domain)
		return irdma_udealloc_parent_domain(iw_parent_domain);

	iwupd = container_of(pd, struct irdma_upd, ibv_pd);
	if (atomic_load(&iwupd->refcount) > 1)
		return EBUSY;

	ret = ibv_cmd_dealloc_pd(pd);
	if (ret)
		return ret;

	ret = irdma_spin_lock(&iwvctx->pd_lock);
	if (ret)
		return ret;
	list_del(&iwupd->list);
	irdma_spin_unlock(&iwvctx->pd_lock);

	free(iwupd);

	return 0;
}

/**
 * irdma_ureg_mr - register user memory region
 * @pd: pd for the mr
 * @addr: user address of the memory region
 * @length: length of the memory
 * @hca_va: hca_va
 * @access: access allowed on this mr
 */
struct ibv_mr *irdma_ureg_mr(struct ibv_pd *pd, void *addr, size_t length,
			     uint64_t hca_va, int access)
{
	struct verbs_mr *vmr;
	struct irdma_ureg_mr cmd = {};
	struct ib_uverbs_reg_mr_resp resp;
	int err;

	err = irdma_validate_pd(pd);
	if (err) {
		errno = err;
		return NULL;
	}

	vmr = malloc(sizeof(*vmr));
	if (!vmr)
		return NULL;

	cmd.reg_type = IRDMA_MEMREG_TYPE_MEM;
	err = ibv_cmd_reg_mr(pd, addr, length,
			     hca_va, access, vmr, &cmd.ibv_cmd,
			     sizeof(cmd), &resp, sizeof(resp));
	if (err) {
		free(vmr);
		errno = err;
		return NULL;
	}

	return &vmr->ibv_mr;
}

/*
 * irdma_urereg_mr - re-register memory region
 * @vmr: mr that was allocated
 * @flags: bit mask to indicate which of the attr's of MR modified
 * @pd: pd of the mr
 * @addr: user address of the memory region
 * @length: length of the memory
 * @access: access allowed on this mr
 */
int irdma_urereg_mr(struct verbs_mr *vmr, int flags, struct ibv_pd *pd,
		    void *addr, size_t length, int access)
{
	struct irdma_urereg_mr cmd = {};
	struct ib_uverbs_rereg_mr_resp resp = {};

	cmd.reg_type = IRDMA_MEMREG_TYPE_MEM;
	return ibv_cmd_rereg_mr(vmr, flags, addr, length, (uintptr_t)addr,
				access, pd, &cmd.ibv_cmd, sizeof(cmd), &resp,
				sizeof(resp));
}

/**
 * irdma_udereg_mr - re-register memory region
 * @vmr: mr that was allocated
 */
int irdma_udereg_mr(struct verbs_mr *vmr)
{
	int ret;

	ret = ibv_cmd_dereg_mr(vmr);
	if (ret)
		return ret;

	free(vmr);

	return 0;
}

/**
 * irdma_ualloc_mw - allocate memory window
 * @pd: protection domain
 * @type: memory window type
 */
struct ibv_mw *irdma_ualloc_mw(struct ibv_pd *pd, enum ibv_mw_type type)
{
	struct ibv_mw *mw;
	struct ibv_alloc_mw cmd;
	struct ib_uverbs_alloc_mw_resp resp = {};
	int err;

	err = irdma_validate_pd(pd);
	if (err) {
		errno = err;
		return NULL;
	}

	mw = calloc(1, sizeof(*mw));
	if (!mw)
		return NULL;

	err = ibv_cmd_alloc_mw(pd, type, mw, &cmd, sizeof(cmd), &resp,
			       sizeof(resp));
	if (err) {
		fprintf(stderr, PFX "%s: Failed to alloc memory window\n",
			__func__);
		free(mw);
		errno = err;
		return NULL;
	}

	return mw;
}

/**
 * irdma_ubind_mw - bind a memory window
 * @qp: qp to post WR
 * @mw: memory window to bind
 * @mw_bind: bind info
 */
int irdma_ubind_mw(struct ibv_qp *qp, struct ibv_mw *mw,
		   struct ibv_mw_bind *mw_bind)
{
	struct ibv_mw_bind_info	*bind_info = &mw_bind->bind_info;
	struct verbs_mr *vmr;

	struct ibv_send_wr wr = {};
	struct ibv_send_wr *bad_wr;
	int err;

	if (!bind_info->mr && (bind_info->addr || bind_info->length))
		return EINVAL;

	if (bind_info->mr) {
		vmr = verbs_get_mr(bind_info->mr);
		if (vmr->mr_type != IBV_MR_TYPE_MR)
			return ENOTSUP;

		if (vmr->access & IBV_ACCESS_ZERO_BASED)
			return EINVAL;

		if (mw->pd != bind_info->mr->pd)
			return EPERM;
	}

	wr.opcode = IBV_WR_BIND_MW;
	wr.bind_mw.bind_info = mw_bind->bind_info;
	wr.bind_mw.mw = mw;
	wr.bind_mw.rkey = ibv_inc_rkey(mw->rkey);

	wr.wr_id = mw_bind->wr_id;
	wr.send_flags = mw_bind->send_flags;

	err = irdma_upost_send(qp, &wr, &bad_wr);
	if (!err)
		mw->rkey = wr.bind_mw.rkey;

	return err;
}

/**
 * irdma_udealloc_mw - deallocate memory window
 * @mw: memory window to dealloc
 */
int irdma_udealloc_mw(struct ibv_mw *mw)
{
	int ret;

	ret = ibv_cmd_dealloc_mw(mw);
	if (ret)
		return ret;
	free(mw);

	return 0;
}

static void *irdma_calloc_hw_buf_sz(size_t size, size_t alignment)
{
	void *buf;

	buf = memalign(alignment, size);

	if (!buf)
		return NULL;
	if (ibv_dontfork_range(buf, size)) {
		free(buf);
		return NULL;
	}
	memset(buf, 0, size);

	return buf;
}

static void irdma_free_hw_buf(void *buf, size_t size)
{
	ibv_dofork_range(buf, size);
	free(buf);
}

static void irdma_get_alloc_type(enum irdma_resource_type resc_type, struct ibv_pd *pd,
				 enum irdma_alloc_type *type, enum irdma_alloc_type default_type)
{
	int i;
	uint32_t exp_size;
	struct irdma_resc_alloc_config *config;
	struct irdma_uparent_domain *parent_domain;

	*type = default_type;
	parent_domain = to_iw_uparent_domain(pd);
	if (!parent_domain || !parent_domain->pd_context)
		return;
	config = (struct irdma_resc_alloc_config *)parent_domain->pd_context;
	/* Validate the input */
	if (config->magic != IRDMA_RESC_ALLOC_MAGIC ||
	    config->entry_size != sizeof(struct irdma_resc_alloc_spec))
		return;
	exp_size = sizeof(*config) + config->entry_size * config->num_entries;
	if (exp_size != config->total_size)
		return;
	for (i = 0; i < config->num_entries; i++) {
		if ((resc_type == config->entries[i].resc_type ||
		     config->entries[i].resc_type == IRDMA_RESOURCE_TYPE_ANY) &&
		    config->entries[i].alloc_type < IRDMA_ALLOC_TYPE_LAST) {
			*type = config->entries[i].alloc_type;
			return;
		}
	}
}

static void irdma_set_command(int command, off_t *offset)
{
	*offset |= (command << IRDMA_MMAP_CMD_SHIFT);
}

static void *irdma_alloc_buf_kmem(int fd, size_t size)
{
	void *addr;
	off_t offset = 0;

	/*
	 * To allocate a buffer in kernel, we need to pass the requirement to
	 * the kernel driver. The buffer length is provided in the length
	 * parameter in mmap() while the command is encoded in the offset
	 * parameter. If needed, other information could also be encoded in
	 * offset. It should be reminded that offset must be shifted left
	 * by IRDMA_HW_PAGE_SHIFT, which is shifted right by the same number
	 * in kernel to generate vma->vm_pgoff. This is really what the irdma
	 * driver can see.
	 */
	irdma_set_command(IRDMA_MMAP_GET_KMEM, &offset);
	addr = irdma_mmap(fd, size, offset << IRDMA_HW_PAGE_SHIFT);
	if (addr == MAP_FAILED)
		return NULL;

	return addr;
}

static void irdma_free_buf_kmem(void *buf, size_t size)
{
	irdma_munmap(buf, size);
}

static void *irdma_alloc_buf_sz(enum irdma_resource_type resc_type, struct ibv_pd *pd,
				size_t size, int fd, enum irdma_alloc_type *alloc_type,
				size_t alignment)
{
	void *buf = NULL;
	enum irdma_alloc_type type;

	irdma_get_alloc_type(resc_type, pd, &type, IRDMA_ALLOC_TYPE_DEFAULT);
	switch (type) {
	case IRDMA_ALLOC_TYPE_KMEM:
		buf = irdma_alloc_buf_kmem(fd, size);
		break;
	case IRDMA_ALLOC_TYPE_DEFAULT:
	default:
		buf = irdma_calloc_hw_buf_sz(size, alignment);
	}
	if (buf && alloc_type)
		*alloc_type = type;

	return buf;
}

static void *irdma_alloc_buf(enum irdma_resource_type resc_type, struct ibv_pd *pd, size_t size,
			     int fd, enum irdma_alloc_type *alloc_type)
{
	return irdma_alloc_buf_sz(resc_type, pd, size, fd, alloc_type, IRDMA_HW_PAGE_SIZE);
}

static void irdma_free_buf(void *buf, size_t size, enum irdma_alloc_type alloc_type)
{
	switch (alloc_type) {
	case IRDMA_ALLOC_TYPE_KMEM:
		irdma_free_buf_kmem(buf, size);
		break;
	case IRDMA_ALLOC_TYPE_DEFAULT:
	default:
		irdma_free_hw_buf(buf, size);
		break;
	}
}

/**
 * irdma_uquery_srq - query srq
 * @ibsrq: ib srq structure
 * @attr: srq attributes to fill in
 */
int irdma_uquery_srq(struct ibv_srq *ibsrq, struct ibv_srq_attr *attr)
{
	struct ibv_query_srq cmd;

	return ibv_cmd_query_srq(ibsrq, attr, &cmd, sizeof(cmd));
}

/**
 * irdma_umodify_srq - modify srq
 * @ibsrq: ib srq structure
 * @attr: srq attributes to use
 * @attr_mask: mask of the attributes
 */
int irdma_umodify_srq(struct ibv_srq *ibsrq,
		      struct ibv_srq_attr *attr,
		      int attr_mask)
{
	struct ibv_modify_srq cmd;

	return ibv_cmd_modify_srq(ibsrq, attr, attr_mask, &cmd, sizeof(cmd));
}

/**
 * irdma_udestroy_srq - destroy srq
 * @ibsrq: ib srq structure
 */
int irdma_udestroy_srq(struct ibv_srq *ibsrq)
{
	struct irdma_usrq *iwusrq;
	struct verbs_srq *vsrq;
	int ret;

	vsrq = container_of(ibsrq, struct verbs_srq, srq);
	iwusrq = container_of(vsrq, struct irdma_usrq, v_srq);

	ret = irdma_spin_destroy(&iwusrq->lock);
	if (ret)
		goto err;

	ret = ibv_cmd_destroy_srq(ibsrq);
	if (ret)
		return ret;

	ibv_cmd_dereg_mr(&iwusrq->vmr);
	irdma_free_buf(iwusrq->srq.srq_base, iwusrq->buf_size, iwusrq->alloc_type);
	free(iwusrq);
	return 0;
err:
	return ret;
}

/**
 * irdma_ucreate_srq - create srq on user app
 * @pd: pd for the qp
 * @initattr: attributes of the srq to be created
 */
struct ibv_srq *irdma_ucreate_srq(struct ibv_pd *pd,
				  struct ibv_srq_init_attr *initattr)
{
	struct ib_uverbs_reg_mr_resp reg_mr_resp = {};
	struct irdma_srq_uk_init_info info = {};
	struct irdma_ucreate_srq_resp resp = {};
	struct irdma_ureg_mr reg_mr_cmd = {};
	struct irdma_ucreate_srq cmd = {};
	struct irdma_uk_attrs *uk_attrs;
	struct irdma_uvcontext *iwvctx;
	struct irdma_usrq *iwusrq;
	struct ibv_srq_attr *attr;
	size_t total_size;
	size_t size;
	__u32 depth;
	__u8 shift;
	int ret;
	enum irdma_alloc_type alloc_type;

	iwvctx = container_of(pd->context, struct irdma_uvcontext, ibv_ctx.context);
	uk_attrs = &iwvctx->uk_attrs;
	attr = &initattr->attr;

	if (attr->max_sge > uk_attrs->max_hw_wq_frags ||
	    attr->max_wr > uk_attrs->max_hw_srq_quanta) {
		errno = EINVAL;
		return NULL;
	}

	irdma_get_wqe_shift(uk_attrs, attr->max_sge, 0, &shift);

	ret = irdma_get_srqdepth(uk_attrs, attr->max_wr, shift, &depth);
	if (ret) {
		errno = ret;
		fprintf(stderr, PFX "%s: invalid SRQ attributes, max_wr=%d max_recv_sge=%d\n",
			__func__, attr->max_wr, attr->max_sge);
		return NULL;
	}

	iwusrq = calloc(1, sizeof(*iwusrq));
	if (!iwusrq)
		return NULL;

	ret = irdma_spin_init_pd(&iwusrq->lock, pd);
	if (ret)
		goto err_lock;

	info.uk_attrs = uk_attrs;
	info.max_srq_frag_cnt = attr->max_sge;

	size = roundup(depth * IRDMA_QP_WQE_MIN_SIZE, IRDMA_HW_PAGE_SIZE);
	total_size = size + IRDMA_DB_SHADOW_AREA_SIZE;
	iwusrq->buf_size = total_size;
	info.srq = irdma_alloc_buf(IRDMA_RESOURCE_TYPE_SRQ, pd, total_size, pd->context->cmd_fd,
				   &alloc_type);

	if (!info.srq) {
		ret = ENOMEM;
		goto err_sges;
	}
	iwusrq->alloc_type = alloc_type;

	reg_mr_cmd.reg_type = IRDMA_MEMREG_TYPE_SRQ;
	reg_mr_cmd.rq_pages = size >> IRDMA_HW_PAGE_SHIFT;

	ret = ibv_cmd_reg_mr(pd, info.srq, total_size,
			     (uintptr_t)info.srq, IBV_ACCESS_LOCAL_WRITE,
			     &iwusrq->vmr, &reg_mr_cmd.ibv_cmd,
			     sizeof(reg_mr_cmd), &reg_mr_resp,
			     sizeof(reg_mr_resp));
	if (ret)
		goto err_cmd_reg;

	iwusrq->vmr.ibv_mr.pd = pd;
	info.shadow_area = (__le64 *)((__u8 *)info.srq + size);

	cmd.user_srq_buf = (__u64)((uintptr_t)info.srq);
	cmd.user_shadow_area = (__u64)((uintptr_t)info.shadow_area);
	ret = ibv_cmd_create_srq(pd, &iwusrq->v_srq.srq, initattr, &cmd.ibv_cmd,
				 sizeof(cmd), &resp.ibv_resp, sizeof(resp));
	if (ret)
		goto err_create_srq;

	info.uk_attrs = uk_attrs;
	info.max_srq_frag_cnt = attr->max_sge;
	info.srq_id = resp.srq_id;
	info.srq_size = resp.srq_size;

	iwusrq->srq.lock = &iwusrq->lock;

	ret = irdma_uk_srq_init(&iwusrq->srq, &info);
	if (ret)
		goto err_srq_init;

	attr->max_wr = (depth - IRDMA_RQ_RSVD) >> shift;

	return &iwusrq->v_srq.srq;

err_srq_init:
	ibv_cmd_destroy_srq(&iwusrq->v_srq.srq);
err_create_srq:
	ibv_cmd_dereg_mr(&iwusrq->vmr);
err_cmd_reg:
	irdma_free_buf(info.srq, total_size, alloc_type);
err_sges:
	irdma_spin_destroy(&iwusrq->lock);
err_lock:
	fprintf(stderr, PFX "%s: failed to create SRQ, status %d\n", __func__, ret);
	free(iwusrq);

	errno = ret;
	return NULL;
}

/**
 * get_cq_size - returns actual cqe needed by HW
 * @ncqe: minimum cqes requested by application
 * @hw_rev: HW generation
 * @cqe_64byte_ena: enable 64byte cqe
 */
static inline int get_cq_size(int ncqe, __u8 hw_rev, bool cqe_64byte_ena)
{
	__u8 cqe_size = cqe_64byte_ena ? 64 : 32;

	ncqe += 2;

	/* Completions with immediate require 1 extra entry */
	if (!cqe_64byte_ena && hw_rev > IRDMA_GEN_1)
		ncqe *= 2;
	if (ncqe & 1)
		ncqe += 1; /* cq size must be an even number */

	if (ncqe * cqe_size == IRDMA_HW_PAGE_SIZE)
		ncqe += 2;

	if (ncqe < IRDMA_U_MINCQ_SIZE)
		ncqe = IRDMA_U_MINCQ_SIZE;

	return ncqe;
}

static inline size_t get_cq_total_bytes(__u32 cq_size, bool cqe_64byte_ena)
{
	if (cqe_64byte_ena)
		return roundup(cq_size * sizeof(struct irdma_extended_cqe), IRDMA_HW_PAGE_SIZE);
	else
		return roundup(cq_size * sizeof(struct irdma_cqe), IRDMA_HW_PAGE_SIZE);
}

/**
 * ucreate_cq - irdma util function to create a CQ
 * @context: ibv context
 * @attr_ex: CQ init attributes
 * @ext_cq: flag to create an extendable or normal CQ
 */
static struct ibv_cq_ex *ucreate_cq(struct ibv_context *context,
				    struct ibv_cq_init_attr_ex *attr_ex,
				    bool ext_cq)
{
	struct irdma_cq_uk_init_info info = {};
	struct irdma_ureg_mr reg_mr_cmd = {};
	struct irdma_ucreate_cq_ex cmd = {};
	struct irdma_ucreate_cq_ex_resp resp = {};
	struct ib_uverbs_reg_mr_resp reg_mr_resp = {};
	struct irdma_ureg_mr reg_mr_shadow_cmd = {};
	struct ib_uverbs_reg_mr_resp reg_mr_shadow_resp = {};
	struct irdma_uk_attrs *uk_attrs;
	struct irdma_uvcontext *iwvctx;
	struct irdma_ucq *iwucq;
	size_t total_size;
	__u32 cq_pages;
	int ret, ncqe;
	__u8 hw_rev;
	bool cqe_64byte_ena;
	enum irdma_alloc_type alloc_type;
	struct ibv_pd *pd = NULL;

	iwvctx = container_of(context, struct irdma_uvcontext, ibv_ctx.context);
	uk_attrs = &iwvctx->uk_attrs;
	hw_rev = uk_attrs->hw_rev;

	if (ext_cq) {
		__u32 supported_flags = hw_rev >= IRDMA_GEN_3 ?
			IRDMA_GEN3_WC_FLAGS_EX : IRDMA_STANDARD_WC_FLAGS_EX;

		if (hw_rev == IRDMA_GEN_1 || attr_ex->wc_flags & ~supported_flags) {
			errno = EOPNOTSUPP;
			return NULL;
		}
	}

	if (env.cq_size_override)
		attr_ex->cqe = env.cq_size_override;

	if (attr_ex->cqe < uk_attrs->min_hw_cq_size || attr_ex->cqe > uk_attrs->max_hw_cq_size - 1) {
		errno = EINVAL;
		return NULL;
	}

	if (attr_ex->comp_mask & IBV_CQ_INIT_ATTR_MASK_PD) {
		struct irdma_uparent_domain *parent_domain =
			to_iw_uparent_domain(attr_ex->parent_domain);

		if (!parent_domain) {
			errno = EINVAL;
			return NULL;
		}
		pd = attr_ex->parent_domain;
	}

	/* save the cqe requested by application */
	ncqe = attr_ex->cqe;

	iwucq = calloc(1, sizeof(*iwucq));
	if (!iwucq)
		return NULL;

	iwucq->parent_domain = pd;
	ret = irdma_spin_init(&iwucq->lock,
			      attr_ex->flags & IBV_CREATE_CQ_ATTR_SINGLE_THREADED ? true : false);
	if (ret) {
		free(iwucq);
		errno = ret;
		return NULL;
	}

	cqe_64byte_ena = uk_attrs->feature_flags & IRDMA_FEATURE_64_BYTE_CQE ? true : false;
	info.cq_size = get_cq_size(attr_ex->cqe, hw_rev, cqe_64byte_ena);
	total_size = get_cq_total_bytes(info.cq_size, cqe_64byte_ena);
	iwucq->comp_vector = attr_ex->comp_vector;
	list_head_init(&iwucq->resize_list);
	cq_pages = total_size >> IRDMA_HW_PAGE_SHIFT;

	if (!(uk_attrs->feature_flags & IRDMA_FEATURE_CQ_RESIZE))
		total_size = (cq_pages << IRDMA_HW_PAGE_SHIFT) + IRDMA_DB_SHADOW_AREA_SIZE;

	iwucq->buf_size = total_size;
	info.cq_base = irdma_alloc_buf(IRDMA_RESOURCE_TYPE_CQ, pd, total_size, context->cmd_fd,
				       &alloc_type);
	if (!info.cq_base) {
		ret = ENOMEM;
		goto err_cq_base;
	}
	iwucq->alloc_type = alloc_type;

	reg_mr_cmd.reg_type = IRDMA_MEMREG_TYPE_CQ;
	reg_mr_cmd.cq_pages = cq_pages;

	ret = ibv_cmd_reg_mr(&iwvctx->iwupd->ibv_pd, info.cq_base,
			     total_size, (uintptr_t)info.cq_base,
			     IBV_ACCESS_LOCAL_WRITE, &iwucq->vmr,
			     &reg_mr_cmd.ibv_cmd, sizeof(reg_mr_cmd),
			     &reg_mr_resp, sizeof(reg_mr_resp));
	if (ret)
		goto err_dereg_mr;

	iwucq->vmr.ibv_mr.pd = &iwvctx->iwupd->ibv_pd;

	if (uk_attrs->feature_flags & IRDMA_FEATURE_CQ_RESIZE) {
		info.shadow_area = irdma_alloc_buf(IRDMA_RESOURCE_TYPE_CQ, pd,
						   IRDMA_DB_SHADOW_AREA_SIZE,
						   context->cmd_fd, NULL);
		if (!info.shadow_area) {
			ret = ENOMEM;
			goto err_alloc_shadow;
		}

		memset(info.shadow_area, 0, IRDMA_DB_SHADOW_AREA_SIZE);
		reg_mr_shadow_cmd.reg_type = IRDMA_MEMREG_TYPE_CQ;
		reg_mr_shadow_cmd.cq_pages = 1;

		ret = ibv_cmd_reg_mr(&iwvctx->iwupd->ibv_pd, info.shadow_area,
				     IRDMA_DB_SHADOW_AREA_SIZE, (uintptr_t)info.shadow_area,
				     IBV_ACCESS_LOCAL_WRITE, &iwucq->vmr_shadow_area,
				     &reg_mr_shadow_cmd.ibv_cmd, sizeof(reg_mr_shadow_cmd),
				     &reg_mr_shadow_resp, sizeof(reg_mr_shadow_resp));
		if (ret) {
			irdma_free_buf(info.shadow_area, IRDMA_DB_SHADOW_AREA_SIZE, alloc_type);
			goto err_alloc_shadow;
		}

		iwucq->vmr_shadow_area.ibv_mr.pd = &iwvctx->iwupd->ibv_pd;

	} else {
		info.shadow_area = (__le64 *)((__u8 *)info.cq_base + (cq_pages << IRDMA_HW_PAGE_SHIFT));
	}

	attr_ex->cqe = info.cq_size;
	cmd.user_cq_buf = (__u64)((uintptr_t)info.cq_base);
	cmd.user_shadow_area = (__u64)((uintptr_t)info.shadow_area);

#ifdef IBV_CMD_CREATE_CQ_EX_VER_2
	ret = ibv_cmd_create_cq_ex(context, attr_ex, NULL, &iwucq->verbs_cq,
				   &cmd.ibv_cmd, sizeof(cmd), &resp.ibv_resp,
				   sizeof(resp), 0);
#else
	ret = ibv_cmd_create_cq_ex(context, attr_ex, &iwucq->verbs_cq,
				   &cmd.ibv_cmd, sizeof(cmd), &resp.ibv_resp,
				   sizeof(resp), 0);
#endif /* IBV_CMD_CREATE_CQ_EX_VER_2 */
	attr_ex->cqe = ncqe;
	if (ret)
		goto err_create_cq;

	if (ext_cq)
		irdma_ibvcq_ex_fill_priv_funcs(iwucq, attr_ex);
	info.cq_id = resp.cq_id;
	/* Do not report the CQE's reserved for immediate and burned by HW */
	iwucq->verbs_cq.cq.cqe = ncqe;
	if (cqe_64byte_ena)
		info.avoid_mem_cflct = true;
	info.cqe_alloc_db = (__u32 *)((__u8 *)iwvctx->db + IRDMA_DB_CQ_OFFSET);
	irdma_uk_cq_init(&iwucq->cq, &info);
	pthread_mutex_lock(&sigusr1_wait_mutex);
	list_add(&dbg_ucq_list, &iwucq->dbg_entry);
	pthread_mutex_unlock(&sigusr1_wait_mutex);
	if (iwucq->parent_domain)
		atomic_fetch_add(&to_iw_uparent_domain(iwucq->parent_domain)->iwupd.refcount, 1);
	return &iwucq->verbs_cq.cq_ex;

err_create_cq:
	if (iwucq->vmr_shadow_area.ibv_mr.handle) {
		ibv_cmd_dereg_mr(&iwucq->vmr_shadow_area);
		irdma_free_buf(info.shadow_area, IRDMA_DB_SHADOW_AREA_SIZE, alloc_type);
	}
err_alloc_shadow:
	ibv_cmd_dereg_mr(&iwucq->vmr);
err_dereg_mr:
	irdma_free_buf(info.cq_base, total_size, alloc_type);
err_cq_base:
	fprintf(stderr, PFX "%s: failed to initialize CQ\n", __func__);
	irdma_spin_destroy(&iwucq->lock);

	free(iwucq);

	errno = ret;
	return NULL;
}

struct ibv_cq *irdma_ucreate_cq(struct ibv_context *context, int cqe,
				struct ibv_comp_channel *channel,
				int comp_vector)
{
	struct ibv_cq_init_attr_ex attr_ex = {
		.cqe = cqe,
		.channel = channel,
		.comp_vector = comp_vector,
	};
	struct ibv_cq_ex *ibvcq_ex;

	ibvcq_ex = ucreate_cq(context, &attr_ex, false);

	return ibvcq_ex ? ibv_cq_ex_to_cq(ibvcq_ex) : NULL;
}

struct ibv_cq_ex *irdma_ucreate_cq_ex(struct ibv_context *context,
				      struct ibv_cq_init_attr_ex *attr_ex)
{
	return ucreate_cq(context, attr_ex, true);
}

/**
 * irdma_free_cq_buf - free memory for cq buffer
 * @cq_buf: cq buf to free
 */
static void irdma_free_cq_buf(struct irdma_cq_buf *cq_buf)
{
	ibv_cmd_dereg_mr(&cq_buf->vmr);
	irdma_free_buf(cq_buf->cq.cq_base, cq_buf->buf_size, cq_buf->alloc_type);
	free(cq_buf);
}

/**
 * irdma_process_resize_list - process the cq list to remove buffers
 * @iwucq: cq which owns the list
 * @lcqe_buf: cq buf where the last cqe is found
 */
static int irdma_process_resize_list(struct irdma_ucq *iwucq,
				     struct irdma_cq_buf *lcqe_buf)
{
	struct irdma_cq_buf *cq_buf, *next;
	int cq_cnt = 0;

	list_for_each_safe(&iwucq->resize_list, cq_buf, next, list) {
		if (cq_buf == lcqe_buf)
			return cq_cnt;

		list_del(&cq_buf->list);
		irdma_free_cq_buf(cq_buf);
		cq_cnt++;
	}

	return cq_cnt;
}

/**
 * irdma_udestroy_cq - destroys cq
 * @cq: ptr to cq to be destroyed
 */
int irdma_udestroy_cq(struct ibv_cq *cq)
{
	struct irdma_uk_attrs *uk_attrs;
	struct irdma_uvcontext *iwvctx;
	struct irdma_ucq *iwucq;
	int ret;

	iwucq = container_of(cq, struct irdma_ucq, verbs_cq.cq);
	iwvctx = container_of(cq->context, struct irdma_uvcontext,
			      ibv_ctx.context);
	uk_attrs = &iwvctx->uk_attrs;
	pthread_mutex_lock(&sigusr1_wait_mutex);
	ret = ibv_cmd_destroy_cq(cq);
	if (ret) {
		pthread_mutex_unlock(&sigusr1_wait_mutex);
		return ret;
	}
	list_del(&iwucq->dbg_entry);
	pthread_mutex_unlock(&sigusr1_wait_mutex);
	ibv_cmd_dereg_mr(&iwucq->vmr);
	irdma_free_buf(iwucq->cq.cq_base, iwucq->buf_size, iwucq->alloc_type);

	if (uk_attrs->feature_flags & IRDMA_FEATURE_CQ_RESIZE) {
		ibv_cmd_dereg_mr(&iwucq->vmr_shadow_area);
		irdma_free_buf(iwucq->cq.shadow_area, IRDMA_DB_SHADOW_AREA_SIZE, iwucq->alloc_type);
	}

	irdma_process_resize_list(iwucq, NULL);

	ret = irdma_spin_destroy(&iwucq->lock);
	if (ret)
		return ret;

	if (iwucq->parent_domain)
		atomic_fetch_sub(&to_iw_uparent_domain(iwucq->parent_domain)->iwupd.refcount, 1);

	free(iwucq);
	return 0;
}

static enum ibv_wc_status irdma_flush_err_to_ib_wc_status(enum irdma_flush_opcode opcode)
{
	switch (opcode) {
	case FLUSH_PROT_ERR:
		return IBV_WC_LOC_PROT_ERR;
	case FLUSH_REM_ACCESS_ERR:
		return IBV_WC_REM_ACCESS_ERR;
	case FLUSH_LOC_QP_OP_ERR:
		return IBV_WC_LOC_QP_OP_ERR;
	case FLUSH_REM_OP_ERR:
		return IBV_WC_REM_OP_ERR;
	case FLUSH_LOC_LEN_ERR:
		return IBV_WC_LOC_LEN_ERR;
	case FLUSH_GENERAL_ERR:
		return IBV_WC_WR_FLUSH_ERR;
	case FLUSH_MW_BIND_ERR:
		return IBV_WC_MW_BIND_ERR;
	case FLUSH_REM_INV_REQ_ERR:
		return IBV_WC_REM_INV_REQ_ERR;
	case FLUSH_RETRY_EXC_ERR:
		return IBV_WC_RETRY_EXC_ERR;
	case FLUSH_RNR_RETRY_EXC_ERR:
		return IBV_WC_RNR_RETRY_EXC_ERR;
	case FLUSH_FATAL_ERR:
	default:
		return IBV_WC_FATAL_ERR;
	}
}

static inline void set_ib_wc_op_sq(struct irdma_cq_poll_info *cur_cqe, struct ibv_wc *entry)
{
	switch (cur_cqe->op_type) {
	case IRDMA_OP_TYPE_RDMA_WRITE:
	case IRDMA_OP_TYPE_RDMA_WRITE_SOL:
		entry->opcode = IBV_WC_RDMA_WRITE;
		break;
	case IRDMA_OP_TYPE_RDMA_READ:
		entry->opcode = IBV_WC_RDMA_READ;
		break;
	case IRDMA_OP_TYPE_SEND_SOL:
	case IRDMA_OP_TYPE_SEND_SOL_INV:
	case IRDMA_OP_TYPE_SEND_INV:
	case IRDMA_OP_TYPE_SEND:
		entry->opcode = IBV_WC_SEND;
		break;
	case IRDMA_OP_TYPE_BIND_MW:
		entry->opcode = IBV_WC_BIND_MW;
		break;
	case IRDMA_OP_TYPE_ATOMIC_COMPARE_AND_SWAP:
		entry->opcode = IBV_WC_COMP_SWAP;
		break;
	case IRDMA_OP_TYPE_ATOMIC_FETCH_AND_ADD:
		entry->opcode = IBV_WC_FETCH_ADD;
		break;
	case IRDMA_OP_TYPE_INV_STAG:
		entry->opcode = IBV_WC_LOCAL_INV;
		break;
	default:
		entry->status = IBV_WC_GENERAL_ERR;
		fprintf(stderr, PFX "%s: Invalid opcode = %d in CQE\n",
			__func__, cur_cqe->op_type);
	}
}

static inline void set_ib_wc_op_rq_gen_3(struct irdma_cq_poll_info *cur_cqe, struct ibv_wc *entry)
{
	switch (cur_cqe->op_type) {
	case IRDMA_OP_TYPE_RDMA_WRITE:
	case IRDMA_OP_TYPE_RDMA_WRITE_SOL:
		entry->opcode = IBV_WC_RECV_RDMA_WITH_IMM;
		break;
	default:
		entry->opcode = IBV_WC_RECV;
	}
}

static inline void set_ib_wc_op_rq(struct irdma_cq_poll_info *cur_cqe,
				   struct ibv_wc *entry, bool send_imm_support)
{
	if (!send_imm_support) {
		entry->opcode = cur_cqe->imm_valid ? IBV_WC_RECV_RDMA_WITH_IMM :
				IBV_WC_RECV;
		return;
	}
	switch (cur_cqe->op_type) {
	case IBV_OPCODE_RDMA_WRITE_ONLY_WITH_IMMEDIATE:
	case IBV_OPCODE_RDMA_WRITE_LAST_WITH_IMMEDIATE:
		entry->opcode = IBV_WC_RECV_RDMA_WITH_IMM;
		break;
	default:
		entry->opcode = IBV_WC_RECV;
	}
}

/**
 * irdma_process_cqe_ext - process current cqe for extended CQ
 * @cur_cqe - current cqe info
 */
static void irdma_process_cqe_ext(struct irdma_cq_poll_info *cur_cqe)
{
	struct irdma_ucq *iwucq = container_of(cur_cqe, struct irdma_ucq, cur_cqe);
	struct ibv_cq_ex *ibvcq_ex = &iwucq->verbs_cq.cq_ex;

	ibvcq_ex->wr_id = cur_cqe->wr_id;
	if (cur_cqe->error)
		ibvcq_ex->status = (cur_cqe->comp_status == IRDMA_COMPL_STATUS_FLUSHED) ?
				   irdma_flush_err_to_ib_wc_status(cur_cqe->minor_err) : IBV_WC_GENERAL_ERR;
	else
		ibvcq_ex->status = IBV_WC_SUCCESS;
}

/**
 * irdma_process_cqe - process current cqe info
 * @entry - ibv_wc object to fill in for non-extended CQ
 * @cur_cqe - current cqe info
 */
static void irdma_process_cqe(struct ibv_wc *entry, struct irdma_cq_poll_info *cur_cqe)
{
	struct irdma_qp_uk *qp;
	struct ibv_qp *ib_qp;

	entry->wc_flags = 0;
	entry->wr_id = cur_cqe->wr_id;
	entry->qp_num = cur_cqe->qp_id;
	qp = cur_cqe->qp_handle;
	ib_qp = qp->back_qp;

	if (cur_cqe->error) {
		entry->status = (cur_cqe->comp_status == IRDMA_COMPL_STATUS_FLUSHED) ?
				irdma_flush_err_to_ib_wc_status(cur_cqe->minor_err) : IBV_WC_GENERAL_ERR;
		entry->vendor_err = cur_cqe->major_err << 16 |
				    cur_cqe->minor_err;
	} else {
		entry->status = IBV_WC_SUCCESS;
	}

	entry->vendor_err = cur_cqe->major_err << 16 | cur_cqe->minor_err;

	if (cur_cqe->imm_valid) {
		entry->imm_data = htonl(cur_cqe->imm_data);
		entry->wc_flags |= IBV_WC_WITH_IMM;
	}

	if (cur_cqe->q_type == IRDMA_CQE_QTYPE_SQ) {
		set_ib_wc_op_sq(cur_cqe, entry);
	} else {
		if (qp->uk_attrs->hw_rev <= IRDMA_GEN_2)
			set_ib_wc_op_rq(cur_cqe, entry,
					qp->qp_caps & IRDMA_SEND_WITH_IMM ?
					true : false);
		else
			set_ib_wc_op_rq_gen_3(cur_cqe, entry);
		if (ib_qp->qp_type != IBV_QPT_UD &&
		    cur_cqe->stag_invalid_set) {
			entry->invalidated_rkey = cur_cqe->inv_stag;
			entry->wc_flags |= IBV_WC_WITH_INV;
		}
	}

	if (ib_qp->qp_type == IBV_QPT_UD) {
		entry->src_qp = cur_cqe->ud_src_qpn;
#define IRDMA_PKT_TYPE_ROCE_V2_IPV4 1
#define IRDMA_PKT_TYPE_ROCE_V2_IPV6 2
		entry->sl = cur_cqe->ipv4 ? IRDMA_PKT_TYPE_ROCE_V2_IPV4 :
			IRDMA_PKT_TYPE_ROCE_V2_IPV6;
		entry->wc_flags |= IBV_WC_GRH;
	} else {
		entry->src_qp = cur_cqe->qp_id;
	}
	entry->byte_len = cur_cqe->bytes_xfered;
}

static int __irdma_upost_send(struct ibv_qp *ib_qp, struct ibv_send_wr *ib_wr,
			      struct ibv_send_wr **bad_wr, bool shared_credit,
			      uint32_t credit_cookie, int credit_index);
static void irdma_try_drain_sq_backlog(struct irdma_uqp *qp);

/**
 * irdma_handle_deferred_ud - Check for and handle the UD deferred SQ.
 * @qp: irdma qp
 *
 * Context: QP lock must be held.
 */
static void irdma_handle_deferred_ud(struct irdma_uqp *qp)
{
	struct ibv_send_wr *bad_wr;
	struct ibv_send_wr *wr;
	const uint32_t masked_cons = qp->ud_ring.cons &
		(qp->ud_ring.ring_size - 1u);

	if (qp->ud_ring.cons == qp->ud_ring.prod)
		return;

	wr = &qp->ud_ring.entry[masked_cons].wqe;
	/* SG len is carried over from WQE, just fix up to point to local SG. */
	wr->sg_list = qp->ud_ring.entry[masked_cons].sgl;
	qp->ud_ring.cons++;

	__irdma_upost_send(&qp->ibv_qp, wr, &bad_wr, false, 0, 0);
}

/**
 * irdma_poll_one - poll one entry of the CQ
 * @ukcq: ukcq to poll
 * @cur_cqe: current CQE info to be filled in
 * @entry: ibv_wc object to be filled for non-extended CQ or NULL for extended CQ
 *
 * Returns the internal irdma device error code or 0 on success
 */
static int irdma_poll_one(struct irdma_cq_uk *ukcq, struct irdma_cq_poll_info *cur_cqe,
			  struct ibv_wc *entry)
{
	int ret = irdma_uk_cq_poll_cmpl(ukcq, cur_cqe);

	if (ret)
		return ret;

	/* Handle UD queue depth limit/deferred submission. */
	if (env.transparent_ud_qd_override) {
		struct irdma_qp_uk *qp = cur_cqe->qp_handle;
		struct ibv_qp *ib_qp = qp->back_qp;
		struct irdma_uqp *iwuqp = container_of(ib_qp, struct irdma_uqp,
						       ibv_qp);

		if (ib_qp->qp_type == IBV_QPT_UD &&
		    cur_cqe->q_type == IRDMA_CQE_QTYPE_SQ) {
			irdma_spin_lock(&iwuqp->lock);
			if (!iwuqp->ud_ring.delete_in_progress) {
				if (cur_cqe->shared_credit) {
					release_credit(cur_cqe->credit_index,
						       cur_cqe->credit_cookie);
				} else {
					iwuqp->ud_ring.active--;
					irdma_handle_deferred_ud(iwuqp);
				}
			}
			irdma_try_drain_sq_backlog(iwuqp);
			irdma_spin_unlock(&iwuqp->lock);
		}

		/* Was this WQE originally signaled? Just consume it if not. */
		if (cur_cqe->signaled_override)
			return ENOENT;
	}

	if (!entry)
		irdma_process_cqe_ext(cur_cqe);
	else
		irdma_process_cqe(entry, cur_cqe);

	return 0;
}

/**
 * __irdma_upoll_cq - irdma util function to poll device CQ
 * @iwucq: irdma cq to poll
 * @num_entries: max cq entries to poll
 * @entry: pointer to array of ibv_wc objects to be filled in for each completion or NULL if ext CQ
 *
 * Returns non-negative value equal to the number of completions
 * found. On failure, EINVAL
 */
static int __irdma_upoll_cq(struct irdma_ucq *iwucq, int num_entries,
			    struct ibv_wc *entry)
{
	struct irdma_cq_buf *cq_buf, *next;
	struct irdma_cq_buf *last_buf = NULL;
	struct irdma_cq_poll_info *cur_cqe = &iwucq->cur_cqe;
	bool cq_new_cqe = false;
	int resized_bufs = 0;
	int npolled = 0;
	int ret;

	/* go through the list of previously resized CQ buffers */
	list_for_each_safe(&iwucq->resize_list, cq_buf, next, list) {
		while (npolled < num_entries) {
			ret = irdma_poll_one(&cq_buf->cq, cur_cqe,
					     entry ? entry + npolled : NULL);
			if (!ret) {
				++npolled;
				cq_new_cqe = true;
				continue;
			}
			if (ret == ENOENT)
				break;
			 /* QP using the CQ is destroyed. Skip reporting this CQE */
			if (ret == EFAULT) {
				cq_new_cqe = true;
				continue;
			}
			goto error;
		}

		/* save the resized CQ buffer which received the last cqe */
		if (cq_new_cqe)
			last_buf = cq_buf;
		cq_new_cqe = false;
	}

	/* check the current CQ for new cqes */
	while (npolled < num_entries) {
		ret = irdma_poll_one(&iwucq->cq, cur_cqe,
				     entry ? entry + npolled : NULL);
		if (!ret) {
			++npolled;
			cq_new_cqe = true;
			continue;
		}
		if (ret == ENOENT)
			break;
		/* QP using the CQ is destroyed. Skip reporting this CQE */
		if (ret == EFAULT) {
			cq_new_cqe = true;
			continue;
		}
		goto error;
	}

	if (cq_new_cqe)
		/* all previous CQ resizes are complete */
		resized_bufs = irdma_process_resize_list(iwucq, NULL);
	else if (last_buf)
		/* only CQ resizes up to the last_buf are complete */
		resized_bufs = irdma_process_resize_list(iwucq, last_buf);
	if (resized_bufs)
		/* report to the HW the number of complete CQ resizes */
		irdma_uk_cq_set_resized_cnt(&iwucq->cq, resized_bufs);

	return npolled;

error:
	fprintf(stderr, PFX "%s: Error polling CQ, irdma_err: %d\n", __func__, ret);

	return EINVAL;
}

/**
 * irdma_upoll_cq - verb API callback to poll device CQ
 * @cq: ibv_cq to poll
 * @num_entries: max cq entries to poll
 * @entry: pointer to array of ibv_wc objects to be filled in for each completion
 *
 * Returns non-negative value equal to the number of completions
 * found and a negative error code on failure
 */
int irdma_upoll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *entry)
{
	struct irdma_ucq *iwucq;
	int ret;

	iwucq = container_of(cq, struct irdma_ucq, verbs_cq.cq);
	ret = irdma_spin_lock(&iwucq->lock);
	if (ret)
		return -ret;

	ret = __irdma_upoll_cq(iwucq, num_entries, entry);

	irdma_spin_unlock(&iwucq->lock);

	return ret;
}

/**
 * irdma_start_poll - verb_ex API callback to poll batch of WC's
 * @ibvcq_ex: ibv extended CQ
 * @attr: attributes (not used)
 *
 * Start polling batch of work completions. Return 0 on success, ENONENT when
 * no completions are available on CQ. And an error code on errors
 */
static int irdma_start_poll(struct ibv_cq_ex *ibvcq_ex, struct ibv_poll_cq_attr *attr)
{
	struct irdma_ucq *iwucq;
	int ret;

	iwucq = container_of(ibvcq_ex, struct irdma_ucq, verbs_cq.cq_ex);
	ret = irdma_spin_lock(&iwucq->lock);
	if (ret)
		return ret;

	ret = __irdma_upoll_cq(iwucq, 1, NULL);
	if (ret == 1)
		return 0;

	/* No Completions on CQ */
	if (!ret)
		ret = ENOENT;

	irdma_spin_unlock(&iwucq->lock);

	return ret;
}

/**
 * irdma_next_poll - verb_ex API callback to get next WC
 * @ibvcq_ex: ibv extended CQ
 *
 * Return 0 on success, ENONENT when no completions are available on CQ.
 * And an error code on errors
 */
static int irdma_next_poll(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_ucq *iwucq;
	int ret;

	iwucq = container_of(ibvcq_ex, struct irdma_ucq, verbs_cq.cq_ex);
	ret = __irdma_upoll_cq(iwucq, 1, NULL);
	if (ret == 1)
		return 0;

	/* No Completions on CQ */
	if (!ret)
		ret = ENOENT;

	return ret;
}

/**
 * irdma_end_poll - verb_ex API callback to end polling of WC's
 * @ibvcq_ex: ibv extended CQ
 */
static void irdma_end_poll(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_ucq *iwucq = container_of(ibvcq_ex, struct irdma_ucq,
					       verbs_cq.cq_ex);

	irdma_spin_unlock(&iwucq->lock);
}

/**
 * irdma_wc_read_completion_ts - Get completion timestamp
 * @ibvcq_ex: ibv extended CQ
 *
 * Get completion timestamp in HCA clock units
 */
static uint64_t irdma_wc_read_completion_ts(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_ucq *iwucq = container_of(ibvcq_ex, struct irdma_ucq,
					       verbs_cq.cq_ex);

	return iwucq->cur_cqe.stat.timestamp;
}

static enum ibv_wc_opcode irdma_wc_read_opcode(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_ucq *iwucq = container_of(ibvcq_ex, struct irdma_ucq,
					       verbs_cq.cq_ex);

	switch (iwucq->cur_cqe.op_type) {
	case IRDMA_OP_TYPE_RDMA_WRITE:
	case IRDMA_OP_TYPE_RDMA_WRITE_SOL:
		return IBV_WC_RDMA_WRITE;
	case IRDMA_OP_TYPE_RDMA_READ:
		return IBV_WC_RDMA_READ;
	case IRDMA_OP_TYPE_SEND_SOL:
	case IRDMA_OP_TYPE_SEND_SOL_INV:
	case IRDMA_OP_TYPE_SEND_INV:
	case IRDMA_OP_TYPE_SEND:
		return IBV_WC_SEND;
	case IRDMA_OP_TYPE_BIND_MW:
		return IBV_WC_BIND_MW;
	case IRDMA_OP_TYPE_ATOMIC_COMPARE_AND_SWAP:
		return IBV_WC_COMP_SWAP;
	case IRDMA_OP_TYPE_ATOMIC_FETCH_AND_ADD:
		return IBV_WC_FETCH_ADD;
	case IRDMA_OP_TYPE_REC:
		return IBV_WC_RECV;
	case IRDMA_OP_TYPE_REC_IMM:
		return IBV_WC_RECV_RDMA_WITH_IMM;
	case IRDMA_OP_TYPE_INV_STAG:
		return IBV_WC_LOCAL_INV;
	}

	fprintf(stderr, PFX "%s: Invalid opcode = %d in CQE\n", __func__,
		iwucq->cur_cqe.op_type);

	return 0;
}

static uint32_t irdma_wc_read_vendor_err(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_cq_poll_info *cur_cqe;
	struct irdma_ucq *iwucq;

	iwucq = container_of(ibvcq_ex, struct irdma_ucq, verbs_cq.cq_ex);
	cur_cqe = &iwucq->cur_cqe;

	return cur_cqe->major_err << 16 | cur_cqe->minor_err;
}

static unsigned int irdma_wc_read_wc_flags(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_cq_poll_info *cur_cqe;
	struct irdma_ucq *iwucq;
	struct irdma_qp_uk *qp;
	struct ibv_qp *ib_qp;
	unsigned int wc_flags = 0;

	iwucq = container_of(ibvcq_ex, struct irdma_ucq, verbs_cq.cq_ex);
	cur_cqe = &iwucq->cur_cqe;
	qp = cur_cqe->qp_handle;
	ib_qp = qp->back_qp;

	if (cur_cqe->imm_valid)
		wc_flags |= IBV_WC_WITH_IMM;

	if (ib_qp->qp_type == IBV_QPT_UD) {
		wc_flags |= IBV_WC_GRH;
	} else {
		if (cur_cqe->stag_invalid_set) {
			switch (cur_cqe->op_type) {
			case IRDMA_OP_TYPE_REC:
				wc_flags |= IBV_WC_WITH_INV;
				break;
			case IRDMA_OP_TYPE_REC_IMM:
				wc_flags |= IBV_WC_WITH_INV;
				break;
			}
		}
	}

	return wc_flags;
}

static uint32_t irdma_wc_read_byte_len(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_ucq *iwucq = container_of(ibvcq_ex, struct irdma_ucq,
					       verbs_cq.cq_ex);

	return iwucq->cur_cqe.bytes_xfered;
}

static __be32 irdma_wc_read_imm_data(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_cq_poll_info *cur_cqe;
	struct irdma_ucq *iwucq;

	iwucq = container_of(ibvcq_ex, struct irdma_ucq, verbs_cq.cq_ex);
	cur_cqe = &iwucq->cur_cqe;

	return cur_cqe->imm_valid ? htonl(cur_cqe->imm_data) : 0;
}

static uint32_t irdma_wc_read_qp_num(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_ucq *iwucq = container_of(ibvcq_ex, struct irdma_ucq,
					       verbs_cq.cq_ex);

	return iwucq->cur_cqe.qp_id;
}

static uint32_t irdma_wc_read_src_qp(struct ibv_cq_ex *ibvcq_ex)
{
	struct irdma_cq_poll_info *cur_cqe;
	struct irdma_ucq *iwucq;
	struct irdma_qp_uk *qp;
	struct ibv_qp *ib_qp;

	iwucq = container_of(ibvcq_ex, struct irdma_ucq, verbs_cq.cq_ex);
	cur_cqe = &iwucq->cur_cqe;
	qp = cur_cqe->qp_handle;
	ib_qp = qp->back_qp;

	return ib_qp->qp_type == IBV_QPT_UD ? cur_cqe->ud_src_qpn : cur_cqe->qp_id;
}

static uint8_t irdma_wc_read_sl(struct ibv_cq_ex *ibvcq_ex)
{
	return 0;
}

void irdma_ibvcq_ex_fill_priv_funcs(struct irdma_ucq *iwucq,
				    struct ibv_cq_init_attr_ex *attr_ex)
{
	struct ibv_cq_ex *ibvcq_ex = &iwucq->verbs_cq.cq_ex;

	ibvcq_ex->start_poll = irdma_start_poll;
	ibvcq_ex->end_poll = irdma_end_poll;
	ibvcq_ex->next_poll = irdma_next_poll;

	if (attr_ex->wc_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP)
		ibvcq_ex->read_completion_ts = irdma_wc_read_completion_ts;
	ibvcq_ex->read_opcode = irdma_wc_read_opcode;
	ibvcq_ex->read_vendor_err = irdma_wc_read_vendor_err;
	ibvcq_ex->read_wc_flags = irdma_wc_read_wc_flags;

	if (attr_ex->wc_flags & IBV_WC_EX_WITH_BYTE_LEN)
		ibvcq_ex->read_byte_len = irdma_wc_read_byte_len;
	if (attr_ex->wc_flags & IBV_WC_EX_WITH_IMM)
		ibvcq_ex->read_imm_data = irdma_wc_read_imm_data;
	if (attr_ex->wc_flags & IBV_WC_EX_WITH_QP_NUM)
		ibvcq_ex->read_qp_num = irdma_wc_read_qp_num;
	if (attr_ex->wc_flags & IBV_WC_EX_WITH_SRC_QP)
		ibvcq_ex->read_src_qp = irdma_wc_read_src_qp;
	if (attr_ex->wc_flags & IBV_WC_EX_WITH_SL)
		ibvcq_ex->read_sl = irdma_wc_read_sl;
}

/**
 * irdma_arm_cq - arm of cq
 * @iwucq: cq to which arm
 * @cq_notify: notification params
 */
static void irdma_arm_cq(struct irdma_ucq *iwucq,
			 enum irdma_cmpl_notify cq_notify)
{
	iwucq->is_armed = true;
	iwucq->arm_sol = true;
	iwucq->skip_arm = false;
	iwucq->skip_sol = true;
	irdma_uk_cq_request_notification(&iwucq->cq, cq_notify);
}

/**
 * irdma_uarm_cq - callback for arm of cq
 * @cq: cq to arm
 * @solicited: to get notify params
 */
int irdma_uarm_cq(struct ibv_cq *cq, int solicited)
{
	struct irdma_ucq *iwucq;
	enum irdma_cmpl_notify cq_notify = IRDMA_CQ_COMPL_EVENT;
	int ret;

	iwucq = container_of(cq, struct irdma_ucq, verbs_cq.cq);
	if (solicited)
		cq_notify = IRDMA_CQ_COMPL_SOLICITED;

	ret = irdma_spin_lock(&iwucq->lock);
	if (ret)
		return ret;

	if (iwucq->is_armed) {
		if (iwucq->arm_sol && !solicited) {
			irdma_arm_cq(iwucq, cq_notify);
		} else {
			iwucq->skip_arm = true;
			iwucq->skip_sol = solicited ? true : false;
		}
	} else {
		irdma_arm_cq(iwucq, cq_notify);
	}

	irdma_spin_unlock(&iwucq->lock);

	return 0;
}

/**
 * irdma_cq_event - cq to do completion event
 * @cq: cq to arm
 */
void irdma_cq_event(struct ibv_cq *cq)
{
	struct irdma_ucq *iwucq;

	iwucq = container_of(cq, struct irdma_ucq, verbs_cq.cq);
	if (irdma_spin_lock(&iwucq->lock))
		return;

	if (iwucq->skip_arm)
		irdma_arm_cq(iwucq, IRDMA_CQ_COMPL_EVENT);
	else
		iwucq->is_armed = false;

	irdma_spin_unlock(&iwucq->lock);
}

void *irdma_mmap(int fd, size_t length, off_t offset)
{
	void *map;

	map = mmap(NULL, length, PROT_WRITE | PROT_READ, MAP_SHARED,
		   fd, offset);
	if (map == MAP_FAILED)
		return map;

	if (ibv_dontfork_range(map, length)) {
		munmap(map, length);
		return MAP_FAILED;
	}

	return map;
}

void irdma_munmap(void *map, size_t length)
{
	ibv_dofork_range(map, length);
	munmap(map, length);
}

/**
 * irdma_destroy_vmapped_qp - destroy resources for qp
 * @iwuqp: qp struct for resources
 */
static int irdma_destroy_vmapped_qp(struct irdma_uqp *iwuqp)
{
	int ret;

	ret = ibv_cmd_destroy_qp(&iwuqp->ibv_qp);
	if (ret)
		return ret;

	if (iwuqp->qp.push_db)
		irdma_munmap(iwuqp->qp.push_db_map, IRDMA_HW_PAGE_SIZE);
	if (iwuqp->qp.push_wqe)
		irdma_munmap(iwuqp->qp.push_wqe_map, IRDMA_HW_PAGE_SIZE);

	ibv_cmd_dereg_mr(&iwuqp->vmr);

	return 0;
}

/**
 * irdma_vmapped_qp - create resources for qp
 * @iwuqp: qp struct for resources
 * @pd: pd for the qp
 * @attr: attributes of qp passed
 * @resp: response back from create qp
 * @info: uk info for initializing user level qp
 * @abi_ver: abi version of the create qp command
 */
static int irdma_vmapped_qp(struct irdma_uqp *iwuqp, struct ibv_pd *pd,
			    struct ibv_qp_init_attr *attr,
			    struct irdma_qp_uk_init_info *info,
			    bool legacy_mode)
{
	struct irdma_ucreate_qp cmd = {};
	size_t sqsize, rqsize, totalqpsize;
	struct irdma_ucreate_qp_resp resp = {};
	struct irdma_ureg_mr reg_mr_cmd = {};
	struct ib_uverbs_reg_mr_resp reg_mr_resp = {};
	struct irdma_uvcontext *iwvctx;
	int ret;
	long os_pgsz = IRDMA_HW_PAGE_SIZE;
	enum irdma_alloc_type alloc_type;

	sqsize = roundup(info->sq_depth * IRDMA_QP_WQE_MIN_SIZE, IRDMA_HW_PAGE_SIZE);
	rqsize = roundup(info->rq_depth * IRDMA_QP_WQE_MIN_SIZE, IRDMA_HW_PAGE_SIZE);
	totalqpsize = rqsize + sqsize + IRDMA_DB_SHADOW_AREA_SIZE;

	iwvctx = container_of(pd->context, struct irdma_uvcontext,
			      ibv_ctx.context);
	/* adjust alignment for iwarp */
	if (iwvctx->ibv_ctx.context.device->transport_type ==
			IBV_TRANSPORT_IWARP) {
		long pgsz = sysconf(_SC_PAGESIZE);

		if (pgsz > 0)
			os_pgsz = pgsz;
	}
	info->sq = irdma_alloc_buf_sz(IRDMA_RESOURCE_TYPE_QP, pd, totalqpsize, pd->context->cmd_fd,
				      &alloc_type, os_pgsz);
	if (!info->sq)
		return ENOMEM;

	iwuqp->alloc_type = alloc_type;
	iwuqp->buf_size = totalqpsize;
	info->rq = &info->sq[sqsize / IRDMA_QP_WQE_MIN_SIZE];
	info->shadow_area = info->rq[rqsize / IRDMA_QP_WQE_MIN_SIZE].elem;

	reg_mr_cmd.reg_type = IRDMA_MEMREG_TYPE_QP;
	reg_mr_cmd.sq_pages = sqsize >> IRDMA_HW_PAGE_SHIFT;
	reg_mr_cmd.rq_pages = rqsize >> IRDMA_HW_PAGE_SHIFT;

	ret = ibv_cmd_reg_mr(pd, info->sq, totalqpsize,
			     (uintptr_t)info->sq, IBV_ACCESS_LOCAL_WRITE,
			     &iwuqp->vmr, &reg_mr_cmd.ibv_cmd,
			     sizeof(reg_mr_cmd), &reg_mr_resp,
			     sizeof(reg_mr_resp));
	if (ret)
		goto err_dereg_mr;

	cmd.user_wqe_bufs = (__u64)((uintptr_t)info->sq);
	cmd.user_compl_ctx = (__u64)(uintptr_t)&iwuqp->qp;
	cmd.comp_mask |= IRDMA_CREATE_QP_USE_START_WQE_IDX;

	ret = ibv_cmd_create_qp(pd, &iwuqp->ibv_qp, attr, &cmd.ibv_cmd,
				sizeof(cmd), &resp.ibv_resp,
				sizeof(struct irdma_ucreate_qp_resp));
	if (ret)
		goto err_qp;

	info->sq_size = resp.actual_sq_size;
	info->rq_size = resp.actual_rq_size;
	info->first_sq_wq = legacy_mode ? 1 : resp.lsmm;
	if (resp.comp_mask & IRDMA_CREATE_QP_USE_START_WQE_IDX)
		info->start_wqe_idx = resp.start_wqe_idx;
	info->qp_caps = resp.qp_caps;
	info->qp_id = resp.qp_id;
	iwuqp->irdma_drv_opt = resp.irdma_drv_opt;
	iwuqp->ibv_qp.qp_num = resp.qp_id;

	iwuqp->send_cq = container_of(attr->send_cq, struct irdma_ucq,
				      verbs_cq.cq);
	iwuqp->recv_cq = container_of(attr->recv_cq, struct irdma_ucq,
				      verbs_cq.cq);
	iwuqp->send_cq->uqp = iwuqp;
	iwuqp->recv_cq->uqp = iwuqp;

	return 0;
err_qp:
	ibv_cmd_dereg_mr(&iwuqp->vmr);
err_dereg_mr:
	fprintf(stderr, PFX "%s: failed to create QP, status %d\n", __func__, ret);
	irdma_free_buf(info->sq, iwuqp->buf_size, alloc_type);
	return ret;
}

/**
 * irdma_ucreate_qp - create qp on user app
 * @pd: pd for the qp
 * @attr: attributes of the qp to be created (sizes, sge, cq)
 */
struct ibv_qp *irdma_ucreate_qp(struct ibv_pd *pd,
				struct ibv_qp_init_attr *attr)
{
	struct irdma_qp_uk_init_info info = {};
	struct irdma_uk_attrs *uk_attrs;
	struct irdma_uvcontext *iwvctx;
	struct irdma_uqp *iwuqp;
	int status;

	status = irdma_validate_pd(pd);
	if (status) {
		errno = status;
		return NULL;
	}

	if (attr->qp_type != IBV_QPT_RC && attr->qp_type != IBV_QPT_UD) {
		fprintf(stderr, PFX "%s: failed to create QP, unsupported QP type: 0x%x\n",
			__func__, attr->qp_type);
		errno = EOPNOTSUPP;
		return NULL;
	}

	iwvctx = container_of(pd->context, struct irdma_uvcontext,
			      ibv_ctx.context);
	uk_attrs = &iwvctx->uk_attrs;

	if (attr->srq) {
		struct irdma_usrq *iwusrq;
		struct verbs_srq *vsrq;

		if (!(uk_attrs->feature_flags & IRDMA_FEATURE_SRQ)) {
			errno = EOPNOTSUPP;
			return NULL;
		}

		vsrq = container_of(attr->srq, struct verbs_srq, srq);
		iwusrq = container_of(vsrq, struct irdma_usrq, v_srq);
		attr->cap.max_recv_sge = uk_attrs->max_hw_wq_frags;
		attr->cap.max_recv_wr = 1;
		info.srq_uk = &iwusrq->srq;
	}

	if (attr->cap.max_send_sge > uk_attrs->max_hw_wq_frags ||
	    attr->cap.max_recv_sge > uk_attrs->max_hw_wq_frags ||
	    attr->cap.max_send_wr > uk_attrs->max_hw_wq_quanta ||
	    attr->cap.max_recv_wr > uk_attrs->max_hw_rq_quanta ||
	    attr->cap.max_inline_data > uk_attrs->max_hw_inline) {
		errno = EINVAL;
		return NULL;
	}

	info.uk_attrs = uk_attrs;
	info.sq_size = attr->cap.max_send_wr;
	info.rq_size = attr->cap.max_recv_wr;
	info.max_sq_frag_cnt = attr->cap.max_send_sge;
	info.max_rq_frag_cnt = attr->cap.max_recv_sge;
	info.max_inline_data = attr->cap.max_inline_data;
	info.abi_ver = iwvctx->abi_ver;
	if (uk_attrs->hw_rev >= IRDMA_GEN_3) {
		if (attr->qp_type == IBV_QPT_UD)
			info.type = IRDMA_QP_TYPE_ROCE_UD;
		else
			info.type = IRDMA_QP_TYPE_ROCE_RC;
	}

	status = irdma_uk_calc_depth_shift_sq(&info, &info.sq_depth, &info.sq_shift);
	if (status) {
		fprintf(stderr, PFX "%s: invalid SQ attributes, max_send_wr=%d max_send_sge=%d max_inline=%d\n",
			__func__, attr->cap.max_send_wr, attr->cap.max_send_sge,
			attr->cap.max_inline_data);
		errno = status;
		return NULL;
	}

	status = irdma_uk_calc_depth_shift_rq(&info, &info.rq_depth, &info.rq_shift);
	if (status) {
		fprintf(stderr, PFX "%s: invalid RQ attributes, recv_wr=%d recv_sge=%d\n",
			__func__, attr->cap.max_recv_wr, attr->cap.max_recv_sge);
		errno = status;
		return NULL;
	}

	iwuqp = memalign(1024, sizeof(*iwuqp));
	if (!iwuqp)
		return NULL;

	memset(iwuqp, 0, sizeof(*iwuqp));

	status = irdma_spin_init_pd(&iwuqp->lock, pd);
	if (status)
		goto err_free_qp;

	info.sq_size = info.sq_depth >> info.sq_shift;
	info.rq_size = info.rq_depth >> info.rq_shift;
	/**
	 * Maintain backward compatibility with older ABI which pass sq
	 * and rq depth (in quanta) in cap.max_send_wr a cap.max_recv_wr
	 */
	if (!iwvctx->use_raw_attrs) {
		attr->cap.max_send_wr = info.sq_size;
		attr->cap.max_recv_wr = info.rq_size;
	}

	info.wqe_alloc_db = (__u32 *)iwvctx->db;
	info.sq_sigwrtrk_array = calloc(info.sq_depth, sizeof(struct irdma_sig_wr_trk_info));
	if (!info.sq_sigwrtrk_array) {
		status = errno; /* preserve errno */
		goto err_destroy_lock;
	}

	info.legacy_mode = iwvctx->legacy_mode;
	info.sq_wrtrk_array = calloc(info.sq_depth, sizeof(*info.sq_wrtrk_array));
	if (!info.sq_wrtrk_array) {
		status = errno; /* preserve errno */
		goto err_free_sq_sigwrtrk;
	}

	info.rq_wrid_array = calloc(info.rq_depth, sizeof(*info.rq_wrid_array));
	if (!info.rq_wrid_array) {
		status = errno; /* preserve errno */
		goto err_free_sq_wrtrk;
	}

	iwuqp->sq_sig_all = attr->sq_sig_all;
	iwuqp->qp_type = attr->qp_type;
	status = irdma_vmapped_qp(iwuqp, pd, attr, &info, iwvctx->legacy_mode);
	if (status)
		goto err_free_rq_wrid;

	iwuqp->qp.back_qp = iwuqp;
	iwuqp->qp.lock = &iwuqp->lock;

	status = irdma_uk_qp_init(&iwuqp->qp, &info);
	if (status)
		goto err_free_vmap_qp;

	attr->cap.max_send_wr = (info.sq_depth - IRDMA_SQ_RSVD) >> info.sq_shift;
	attr->cap.max_recv_wr = (info.rq_depth - IRDMA_RQ_RSVD) >> info.rq_shift;

	iwuqp->qp.sq_ring.user_size = attr->cap.max_send_wr;

	if (env.ud_qd_override && attr->qp_type == IBV_QPT_UD)
		attr->cap.max_send_wr = env.ud_qd_override;

	if (env.transparent_ud_qd_override && attr->qp_type == IBV_QPT_UD) {
		uint32_t ring_size = attr->cap.max_send_wr;

		if ((ring_size - 1u) & ring_size) {
			/* Not a power of 2, so round up. */
			ring_size = 1u << ((CHAR_BIT * sizeof(unsigned int)) -
					   __builtin_clz(ring_size));
		}

		iwuqp->ud_ring.entry = calloc(ring_size,
					      sizeof(struct ud_ring_entry));
		if (!iwuqp->ud_ring.entry) {
			status = errno; /* preserve errno */
			goto err_free_vmap_qp;
		}
		iwuqp->ud_ring.ring_size = ring_size;
	}

	pthread_mutex_lock(&sigusr1_wait_mutex);
	list_add(&dbg_uqp_list, &iwuqp->dbg_entry);
	pthread_mutex_unlock(&sigusr1_wait_mutex);

	return &iwuqp->ibv_qp;

err_free_vmap_qp:
	irdma_destroy_vmapped_qp(iwuqp);
	irdma_free_buf(info.sq, iwuqp->buf_size, iwuqp->alloc_type);
err_free_rq_wrid:
	free(info.rq_wrid_array);
err_free_sq_wrtrk:
	free(info.sq_wrtrk_array);
err_free_sq_sigwrtrk:
	free(info.sq_sigwrtrk_array);
err_destroy_lock:
	irdma_spin_destroy(&iwuqp->lock);
err_free_qp:
	fprintf(stderr, PFX "%s: failed to create QP\n", __func__);
	free(iwuqp);

	errno = status;
	return NULL;
}

/**
 * irdma_uquery_qp - query qp for some attribute
 * @qp: qp for the attributes query
 * @attr: to return the attributes
 * @attr_mask: mask of what is query for
 * @init_attr: initial attributes during create_qp
 */
int irdma_uquery_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask,
		    struct ibv_qp_init_attr *init_attr)
{
	struct ibv_query_qp cmd;

	return ibv_cmd_query_qp(qp, attr, attr_mask, init_attr, &cmd,
				sizeof(cmd));
}

/**
 * irdma_umodify_qp - send qp modify to driver
 * @qp: qp to modify
 * @attr: attribute to modify
 * @attr_mask: mask of the attribute
 */
int irdma_umodify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask)
{
	struct irdma_umodify_qp_resp resp = {};
	struct ibv_modify_qp cmd = {};
	struct irdma_modify_qp_cmd cmd_ex = {};
	struct irdma_uvcontext *iwvctx;
	struct irdma_uqp *iwuqp;

	iwuqp = container_of(qp, struct irdma_uqp, ibv_qp);
	iwvctx = container_of(qp->context, struct irdma_uvcontext,
			      ibv_ctx.context);

	if (iwuqp->qp.qp_caps & IRDMA_PUSH_MODE && attr_mask & IBV_QP_STATE &&
	    iwvctx->uk_attrs.hw_rev > IRDMA_GEN_1) {
		__u64 offset;
		int ret;

		ret = ibv_cmd_modify_qp_ex(qp, attr, attr_mask, &cmd_ex.ibv_cmd,
					   sizeof(cmd_ex), &resp.ibv_resp, sizeof(resp));
		if (!ret)
			iwuqp->qp.rd_fence_rate = resp.rd_fence_rate;
		if (ret || !resp.push_valid)
			return ret;

		if (iwuqp->qp.push_wqe)
			return ret;

		offset = resp.push_wqe_mmap_key;
		iwuqp->qp.push_wqe_map = irdma_mmap(qp->context->cmd_fd, IRDMA_HW_PAGE_SIZE,
						    offset);
		if (iwuqp->qp.push_wqe_map == MAP_FAILED)
			return ret;

		offset = resp.push_db_mmap_key;
		iwuqp->qp.push_db_map = irdma_mmap(qp->context->cmd_fd, IRDMA_HW_PAGE_SIZE,
						   offset);
		if (iwuqp->qp.push_db_map == MAP_FAILED) {
			irdma_munmap(iwuqp->qp.push_wqe_map, IRDMA_HW_PAGE_SIZE);
			fprintf(stderr, PFX "failed to map push page, errno %d\n", errno);
			return ret;
		}
		iwuqp->qp.push_wqe = iwuqp->qp.push_wqe_map + resp.push_offset;
		iwuqp->qp.push_db = iwuqp->qp.push_db_map + resp.push_offset;

		return ret;
	} else {
		return ibv_cmd_modify_qp(qp, attr, attr_mask, &cmd, sizeof(cmd));
	}
}

static void irdma_issue_flush(struct ibv_qp *qp, bool sq_flush, bool rq_flush)
{
	struct ib_uverbs_ex_modify_qp_resp resp = {};
	struct irdma_modify_qp_cmd cmd_ex = {};
	struct ibv_qp_attr attr = {};

	attr.qp_state = IBV_QPS_ERR;
	cmd_ex.sq_flush = sq_flush;
	cmd_ex.rq_flush = rq_flush;

	ibv_cmd_modify_qp_ex(qp, &attr, IBV_QP_STATE, &cmd_ex.ibv_cmd,
			     sizeof(cmd_ex), &resp, sizeof(resp));
}

/**
 * irdma_clean_cqes - clean cq entries for qp
 * @qp: qp for which completions are cleaned
 * @iwcq: cq to be cleaned
 */
static void irdma_clean_cqes(struct irdma_qp_uk *qp, struct irdma_ucq *iwucq)
{
	struct irdma_cq_uk *ukcq = &iwucq->cq;
	int ret;

	ret = irdma_spin_lock(&iwucq->lock);
	if (ret)
		return;

	irdma_uk_clean_cq(qp, ukcq);
	irdma_spin_unlock(&iwucq->lock);
}

/**
 * irdma_udestroy_qp - destroy qp
 * @qp: qp to destroy
 */
int irdma_udestroy_qp(struct ibv_qp *qp)
{
	struct irdma_uqp *iwuqp;
	int ret;

	iwuqp = container_of(qp, struct irdma_uqp, ibv_qp);

	if (env.transparent_ud_qd_override &&
	    iwuqp->ibv_qp.qp_type == IBV_QPT_UD) {
		/* If transparent UD QD limit is enabled, mark the QP as going
		 * down so that a concurrent CQ poll doesn't attempt to post a
		 * WQE during the deletion. The user is supposed to ensure that
		 * they don't post to a QP being deleted, but they do not know
		 * that polling a CQ may indirectly result in a QP post.
		 */
		irdma_spin_lock(&iwuqp->lock);
		iwuqp->ud_ring.delete_in_progress = true;
		irdma_spin_unlock(&iwuqp->lock);
	}

	pthread_mutex_lock(&sigusr1_wait_mutex);
	list_del(&iwuqp->dbg_entry);
	pthread_mutex_unlock(&sigusr1_wait_mutex);

	ret = irdma_destroy_vmapped_qp(iwuqp);
	if (ret)
		goto err;

	/* Clean any pending completions from the cq(s) */
	if (iwuqp->send_cq)
		irdma_clean_cqes(&iwuqp->qp, iwuqp->send_cq);

	if (iwuqp->recv_cq && iwuqp->recv_cq != iwuqp->send_cq)
		irdma_clean_cqes(&iwuqp->qp, iwuqp->recv_cq);

	if (iwuqp->qp.sq_sigwrtrk_array)
		free(iwuqp->qp.sq_sigwrtrk_array);
	if (iwuqp->qp.sq_wrtrk_array)
		free(iwuqp->qp.sq_wrtrk_array);
	if (iwuqp->qp.rq_wrid_array)
		free(iwuqp->qp.rq_wrid_array);
	if (env.transparent_ud_qd_override && iwuqp->ud_ring.entry)
		free(iwuqp->ud_ring.entry);

	irdma_free_buf(iwuqp->qp.sq_base, iwuqp->buf_size, iwuqp->alloc_type);
	irdma_spin_destroy(&iwuqp->lock);
	free(iwuqp);
	return 0;

err:
	fprintf(stderr, PFX "%s: failed to destroy QP, status %d\n",
		__func__, ret);
	return ret;
}

/**
 * calc_type2_mw_stag - calculate type 2 MW stag
 * @rkey: desired rkey of the MW
 * @mw_rkey: type2 memory window rkey
 *
 * compute type2 memory window stag by taking lower 8 bits
 * of the desired rkey and leaving 24 bits if mw->rkey unchanged
 */
static inline __u32 calc_type2_mw_stag(__u32 rkey, __u32 mw_rkey)
{
	const __u32 mask = 0xff;

	return (rkey & mask) | (mw_rkey & ~mask);
}

/**
 * irdma_enqueue_deferred_ud - Add a WQE to the UD backlog.
 * @iwuqp: irdma qp.
 * @wr: ib wr
 *
 * Context: Assumes QP lock is held.
 */
static void irdma_enqueue_deferred_ud(struct irdma_uqp *iwuqp,
				      struct ibv_send_wr *wr)
{
	size_t i;
	uint64_t inl_len = 0;
	const uint32_t masked_prod = iwuqp->ud_ring.prod &
		(iwuqp->ud_ring.ring_size - 1u);

	iwuqp->ud_ring.entry[masked_prod].wqe = *wr;
	iwuqp->ud_ring.entry[masked_prod].wqe.next = NULL;

	if (wr->send_flags & IBV_SEND_INLINE) {
		/* Inline data needs to be copied now. */
		for (i = 0; i < wr->num_sge; i++) {
			if ((inl_len + wr->sg_list[i].length) > UD_MAX_INLINE)
				break;

			memcpy(&iwuqp->ud_ring.entry[masked_prod].
			       inline_data[inl_len],
			       (void *)wr->sg_list[i].addr,
			       wr->sg_list[i].length);

			inl_len += wr->sg_list[i].length;
		}

		/* Override sg info to point to the inline data buffer. */
		iwuqp->ud_ring.entry[masked_prod].wqe.num_sge = 1;
		iwuqp->ud_ring.entry[masked_prod].sgl[0].addr =
			(uint64_t)iwuqp->ud_ring.entry[masked_prod].inline_data;
		iwuqp->ud_ring.entry[masked_prod].sgl[0].length = inl_len;
	} else {
		for (i = 0; i < wr->num_sge; i++)
			iwuqp->ud_ring.entry[masked_prod].sgl[i] =
			wr->sg_list[i];
	}

	iwuqp->ud_ring.prod++;
}

/**
 * __irdma_post_send -  post send wr for user application
 * @ib_qp: qp to post wr
 * @ib_wr: work request ptr
 * @bad_wr: return of bad wr if err
 * @shared_credit: Flag to indicate whether a shared credit was used. UD only.
 * @credit_cookie: Shared credit cookie. Only used if shared_credit==true.
 * @credit_index: Shared credit index. Only used if shared_credit==true.
 *
 * Context: Assumes QP lock is held.
 */
static int __irdma_upost_send(struct ibv_qp *ib_qp, struct ibv_send_wr *ib_wr,
			      struct ibv_send_wr **bad_wr, bool shared_credit,
			      uint32_t credit_cookie, int credit_index)
{
	struct irdma_post_sq_info info;
	struct irdma_uvcontext *iwvctx;
	struct irdma_uk_attrs *uk_attrs;
	struct irdma_uqp *iwuqp;
	bool reflush = false;
	int err = 0;

	iwuqp = container_of(ib_qp, struct irdma_uqp, ibv_qp);
	iwvctx = container_of(ib_qp->context, struct irdma_uvcontext,
			      ibv_ctx.context);
	uk_attrs = &iwvctx->uk_attrs;

	if (!IRDMA_RING_MORE_WORK(iwuqp->qp.sq_ring) &&
	    ib_qp->state == IBV_QPS_ERR)
		reflush = true;

	while (ib_wr) {
		if (env.transparent_ud_qd_override &&
		    ib_qp->qp_type == IBV_QPT_UD && !shared_credit) {
			if (iwuqp->ud_ring.active >=
			    env.transparent_ud_qd_override) {
				/* HW limit is reached, queue it for later. */
				irdma_enqueue_deferred_ud(iwuqp, ib_wr);
				ib_wr = ib_wr->next;
				continue;
			}
		}

		memset(&info, 0, sizeof(info));
		info.wr_id = (__u64)(ib_wr->wr_id);
		if ((ib_wr->send_flags & IBV_SEND_SIGNALED) ||
		    iwuqp->sq_sig_all)
			info.signaled = true;
		if (ib_wr->send_flags & IBV_SEND_FENCE)
			info.read_fence = true;

		switch (ib_wr->opcode) {
		case IBV_WR_ATOMIC_CMP_AND_SWP:
			if (unlikely(!(uk_attrs->feature_flags & IRDMA_FEATURE_ATOMIC_OPS))) {
				err = EINVAL;
				break;
			}
			info.op_type = IRDMA_OP_TYPE_ATOMIC_COMPARE_AND_SWAP;
			info.op.atomic_compare_swap.tagged_offset = ib_wr->sg_list[0].addr;
			info.op.atomic_compare_swap.remote_tagged_offset =
							ib_wr->wr.atomic.remote_addr;
			info.op.atomic_compare_swap.swap_data_bytes =
							ib_wr->wr.atomic.swap;
			info.op.atomic_compare_swap.compare_data_bytes =
							ib_wr->wr.atomic.compare_add;
			info.op.atomic_compare_swap.stag = ib_wr->sg_list[0].lkey;
			info.op.atomic_compare_swap.remote_stag = ib_wr->wr.atomic.rkey;
			err = irdma_uk_atomic_compare_swap(&iwuqp->qp, &info, false);
			break;
		case IBV_WR_ATOMIC_FETCH_AND_ADD:
			if (unlikely(!(uk_attrs->feature_flags & IRDMA_FEATURE_ATOMIC_OPS))) {
				err = EINVAL;
				break;
			}
			info.op_type = IRDMA_OP_TYPE_ATOMIC_FETCH_AND_ADD;
			info.op.atomic_fetch_add.tagged_offset = ib_wr->sg_list[0].addr;
			info.op.atomic_fetch_add.remote_tagged_offset =
							ib_wr->wr.atomic.remote_addr;
			info.op.atomic_fetch_add.fetch_add_data_bytes =
							ib_wr->wr.atomic.compare_add;
			info.op.atomic_fetch_add.stag = ib_wr->sg_list[0].lkey;
			info.op.atomic_fetch_add.remote_stag = ib_wr->wr.atomic.rkey;
			err = irdma_uk_atomic_fetch_add(&iwuqp->qp, &info, false);
			break;
		case IBV_WR_SEND_WITH_IMM:
			if (iwuqp->qp.qp_caps & IRDMA_SEND_WITH_IMM) {
				info.imm_data_valid = true;
				info.imm_data = ntohl(ib_wr->imm_data);
			} else {
				err = EINVAL;
				break;
			}
			SWITCH_FALLTHROUGH;
		case IBV_WR_SEND:
		case IBV_WR_SEND_WITH_INV:
			if (ib_wr->opcode == IBV_WR_SEND ||
			    ib_wr->opcode == IBV_WR_SEND_WITH_IMM) {
				if (ib_wr->send_flags & IBV_SEND_SOLICITED)
					info.op_type = IRDMA_OP_TYPE_SEND_SOL;
				else
					info.op_type = IRDMA_OP_TYPE_SEND;
			} else {
				if (ib_wr->send_flags & IBV_SEND_SOLICITED)
					info.op_type = IRDMA_OP_TYPE_SEND_SOL_INV;
				else
					info.op_type = IRDMA_OP_TYPE_SEND_INV;
				info.stag_to_inv = ib_wr->invalidate_rkey;
			}
			info.op.send.num_sges = ib_wr->num_sge;
			info.op.send.sg_list = (struct ibv_sge *)ib_wr->sg_list;
			if (ib_qp->qp_type == IBV_QPT_UD) {
				struct irdma_uah *ah  = container_of(ib_wr->wr.ud.ah,
								     struct irdma_uah, ibv_ah);

				info.op.send.ah_id = ah->ah_id;
				info.op.send.qkey = ib_wr->wr.ud.remote_qkey;
				info.op.send.dest_qp = ib_wr->wr.ud.remote_qpn;

				if (env.transparent_ud_qd_override) {
					info.shared_credit = shared_credit;
					info.credit_cookie = credit_cookie;
					info.credit_index = credit_index;
					if (!shared_credit)
						iwuqp->ud_ring.active++;
					if (!info.signaled) {
						info.signaled = true;
						info.signaled_override = true;
					}
				}
			}

			if (ib_wr->send_flags & IBV_SEND_INLINE)
				err = irdma_uk_inline_send(&iwuqp->qp, &info, false);
			else
				err = irdma_uk_send(&iwuqp->qp, &info, false);
			break;
		case IBV_WR_RDMA_WRITE_WITH_IMM:
			if (iwuqp->qp.qp_caps & IRDMA_WRITE_WITH_IMM) {
				info.imm_data_valid = true;
				info.imm_data = ntohl(ib_wr->imm_data);
			} else {
				err = EINVAL;
				break;
			}
			SWITCH_FALLTHROUGH;
		case IBV_WR_RDMA_WRITE:
			if (ib_wr->send_flags & IBV_SEND_SOLICITED)
				info.op_type = IRDMA_OP_TYPE_RDMA_WRITE_SOL;
			else
				info.op_type = IRDMA_OP_TYPE_RDMA_WRITE;

			info.op.rdma_write.num_lo_sges = ib_wr->num_sge;
			info.op.rdma_write.lo_sg_list = ib_wr->sg_list;
			info.op.rdma_write.rem_addr.addr = ib_wr->wr.rdma.remote_addr;
			info.op.rdma_write.rem_addr.lkey = ib_wr->wr.rdma.rkey;
			if (ib_wr->send_flags & IBV_SEND_INLINE)
				err = irdma_uk_inline_rdma_write(&iwuqp->qp, &info, false);
			else
				err = irdma_uk_rdma_write(&iwuqp->qp, &info, false);
			break;
		case IBV_WR_RDMA_READ:
			if (ib_wr->num_sge > uk_attrs->max_hw_read_sges) {
				err = EINVAL;
				break;
			}
			info.op_type = IRDMA_OP_TYPE_RDMA_READ;
			info.op.rdma_read.rem_addr.addr = ib_wr->wr.rdma.remote_addr;
			info.op.rdma_read.rem_addr.lkey = ib_wr->wr.rdma.rkey;

			info.op.rdma_read.lo_sg_list = ib_wr->sg_list;
			info.op.rdma_read.num_lo_sges = ib_wr->num_sge;
			err = irdma_uk_rdma_read(&iwuqp->qp, &info, false, false);
			break;
		case IBV_WR_BIND_MW:
			if (ib_qp->qp_type != IBV_QPT_RC ||
			    (ib_wr->bind_mw.mw->type == IBV_MW_TYPE_1 &&
			     ib_wr->bind_mw.bind_info.mw_access_flags &
			     IBV_ACCESS_ZERO_BASED)) {
				err = EINVAL;
				break;
			}
			info.op_type = IRDMA_OP_TYPE_BIND_MW;
			info.op.bind_window.mr_stag = ib_wr->bind_mw.bind_info.mr->rkey;
			if (ib_wr->bind_mw.mw->type == IBV_MW_TYPE_1) {
				info.op.bind_window.mem_window_type_1 = true;
				info.op.bind_window.mw_stag = ib_wr->bind_mw.rkey;
			} else {
				struct verbs_mr *vmr = verbs_get_mr(ib_wr->bind_mw.bind_info.mr);

				if (vmr->access & IBV_ACCESS_ZERO_BASED) {
					err = EINVAL;
					break;
				}
				info.op.bind_window.mw_stag =
					calc_type2_mw_stag(ib_wr->bind_mw.rkey, ib_wr->bind_mw.mw->rkey);
				ib_wr->bind_mw.mw->rkey = info.op.bind_window.mw_stag;
			}

			if (ib_wr->bind_mw.bind_info.mw_access_flags & IBV_ACCESS_ZERO_BASED)
				info.op.bind_window.addressing_type = IRDMA_ADDR_TYPE_ZERO_BASED;
			else
				info.op.bind_window.addressing_type = IRDMA_ADDR_TYPE_VA_BASED;

			info.op.bind_window.va =  (void *)(uintptr_t)ib_wr->bind_mw.bind_info.addr;
			info.op.bind_window.bind_len = ib_wr->bind_mw.bind_info.length;
			info.op.bind_window.ena_reads =
				(ib_wr->bind_mw.bind_info.mw_access_flags & IBV_ACCESS_REMOTE_READ) ? 1 : 0;
			info.op.bind_window.ena_writes =
				(ib_wr->bind_mw.bind_info.mw_access_flags & IBV_ACCESS_REMOTE_WRITE) ? 1 : 0;
			info.op.bind_window.remote_atomics_en =
				(ib_wr->bind_mw.bind_info.mw_access_flags & IBV_ACCESS_REMOTE_ATOMIC) ? 1 : 0;

			err = irdma_uk_mw_bind(&iwuqp->qp, &info, false);
			break;
		case IBV_WR_LOCAL_INV:
			info.op_type = IRDMA_OP_TYPE_INV_STAG;
			info.op.inv_local_stag.target_stag = ib_wr->invalidate_rkey;
			err = irdma_uk_stag_local_invalidate(&iwuqp->qp, &info, true);
			break;
		default:
			/* error */
			err = EINVAL;
			fprintf(stderr, PFX "%s: post work request failed, invalid opcode: 0x%x\n",
				__func__, ib_wr->opcode);
			break;
		}
		if (err)
			break;

		ib_wr = ib_wr->next;
	}

	if (err)
		*bad_wr = ib_wr;

	if (!iwuqp->qp.push_db && !shared_credit)
		irdma_uk_qp_post_wr(&iwuqp->qp);
	if (reflush)
		irdma_issue_flush(ib_qp, 1, 0);

	return err;
}

/**
 * irdma_try_drain_sq_backlog - Try to drain the backlog using shared credits.
 * @qp: qp
 *
 * Context: QP lock must be held.
 */
static void irdma_try_drain_sq_backlog(struct irdma_uqp *qp)
{
	struct ibv_send_wr *bad_wr;
	struct ibv_send_wr *wr;
	bool posted = false;
	uint32_t cookie;
	uint32_t cons;
	int ret;

	while (true) {
		if (qp->ud_ring.cons == qp->ud_ring.prod)
			break;

		ret = try_acquire_credit(&cookie);
		if (ret < 0)
			break;

		cons = qp->ud_ring.cons & (qp->ud_ring.ring_size - 1u);
		wr = &qp->ud_ring.entry[cons].wqe;
		wr->sg_list = qp->ud_ring.entry[cons].sgl;

		qp->ud_ring.cons++;

		__irdma_upost_send(&qp->ibv_qp, wr, &bad_wr, true, cookie, ret);
		posted = true;
	}

	if (posted)
		irdma_uk_qp_post_wr(&qp->qp);
}

/**
 * irdma_post_send -  post send wr for user application
 * @ib_qp: qp to post wr
 * @ib_wr: work request ptr
 * @bad_wr: return of bad wr if err
 */
int irdma_upost_send(struct ibv_qp *ib_qp, struct ibv_send_wr *ib_wr,
		     struct ibv_send_wr **bad_wr)
{
	struct irdma_uqp *iwuqp;
	int err;

	iwuqp = container_of(ib_qp, struct irdma_uqp, ibv_qp);

	err = irdma_spin_lock(&iwuqp->lock);
	if (err)
		return err;

	err = __irdma_upost_send(ib_qp, ib_wr, bad_wr, false, 0, 0);

	if (env.transparent_ud_qd_override && ib_qp->qp_type == IBV_QPT_UD)
		irdma_try_drain_sq_backlog(iwuqp);

	irdma_spin_unlock(&iwuqp->lock);

	return err;
}

/**
 * irdma_upost_srq - post receive wr for user application
 * @ib_wr: work request for receive
 * @bad_wr: bad wr caused an error
 */
int irdma_upost_srq(struct ibv_srq *ibsrq, struct ibv_recv_wr *ib_wr,
		    struct ibv_recv_wr **bad_wr)
{
	struct irdma_post_rq_info post_recv = {};
	struct irdma_usrq *iwusrq;
	struct irdma_srq_uk *srq;
	struct verbs_srq *vsrq;
	int err;

	vsrq = container_of(ibsrq, struct verbs_srq, srq);
	iwusrq = container_of(vsrq, struct irdma_usrq, v_srq);
	srq = &iwusrq->srq;

	err = irdma_spin_lock(&iwusrq->lock);
	if (err)
		return err;

	while (ib_wr) {
		if (ib_wr->num_sge > srq->max_srq_frag_cnt) {
			*bad_wr = ib_wr;
			err = EINVAL;
			goto error;
		}
		post_recv.num_sges = ib_wr->num_sge;
		post_recv.wr_id = ib_wr->wr_id;
		post_recv.sg_list = ib_wr->sg_list;
		err = irdma_uk_srq_post_receive(srq, &post_recv);
		if (err) {
			*bad_wr = ib_wr;
			goto error;
		}

		ib_wr = ib_wr->next;
	}
error:
	irdma_spin_unlock(&iwusrq->lock);

	return err;
}

/**
 * irdma_post_recv - post receive wr for user application
 * @ib_wr: work request for receive
 * @bad_wr: bad wr caused an error
 */
int irdma_upost_recv(struct ibv_qp *ib_qp, struct ibv_recv_wr *ib_wr,
		     struct ibv_recv_wr **bad_wr)
{
	struct irdma_post_rq_info post_recv = {};
	struct irdma_uqp *iwuqp;
	bool reflush = false;
	int err;

	iwuqp = container_of(ib_qp, struct irdma_uqp, ibv_qp);
	if (iwuqp->qp.srq_uk) {
		*bad_wr = ib_wr;
		return EINVAL;
	}

	err = irdma_spin_lock(&iwuqp->lock);
	if (err)
		return err;

	if (!IRDMA_RING_MORE_WORK(iwuqp->qp.rq_ring) &&
	    ib_qp->state == IBV_QPS_ERR)
		reflush = true;

	while (ib_wr) {
		if (ib_wr->num_sge > iwuqp->qp.max_rq_frag_cnt) {
			*bad_wr = ib_wr;
			err = EINVAL;
			goto error;
		}
		post_recv.num_sges = ib_wr->num_sge;
		post_recv.wr_id = ib_wr->wr_id;
		post_recv.sg_list = ib_wr->sg_list;
		err = irdma_uk_post_receive(&iwuqp->qp, &post_recv);
		if (err) {
			*bad_wr = ib_wr;
			goto error;
		}

		if (reflush)
			irdma_issue_flush(ib_qp, 0, 1);

		ib_wr = ib_wr->next;
	}
error:
	irdma_spin_unlock(&iwuqp->lock);

	return err;
}

/**
 * irdma_ucreate_ah - create address handle associated with a pd
 * @ibpd: pd for the address handle
 * @attr: attributes of address handle
 */
struct ibv_ah *irdma_ucreate_ah(struct ibv_pd *ibpd, struct ibv_ah_attr *attr)
{
	struct irdma_uah *ah;
	union ibv_gid sgid;
	struct irdma_ucreate_ah_resp resp = {};
	int err;

	if (ibv_query_gid(ibpd->context, attr->port_num, attr->grh.sgid_index,
			  &sgid)) {
		fprintf(stderr, "irdma: Error from ibv_query_gid.\n");
		errno = ENOENT;
		return NULL;
	}

	ah = calloc(1, sizeof(*ah));
	if (!ah)
		return NULL;

	err = ibv_cmd_create_ah(ibpd, &ah->ibv_ah, attr, &resp.ibv_resp,
				sizeof(resp));
	if (err) {
		free(ah);
		errno = err;
		return NULL;
	}

	ah->ah_id = resp.ah_id;

	return &ah->ibv_ah;
}

/**
 * irdma_udestroy_ah - destroy the address handle
 * @ibah: address handle
 */
int irdma_udestroy_ah(struct ibv_ah *ibah)
{
	struct irdma_uah *ah;
	int ret;

	ah = container_of(ibah, struct irdma_uah, ibv_ah);

	ret = ibv_cmd_destroy_ah(ibah);
	if (ret)
		return ret;

	free(ah);

	return 0;
}

/**
 * irdma_uattach_mcast - Attach qp to multicast group implemented
 * @qp: The queue pair
 * @gid:The Global ID for multicast group
 * @lid: The Local ID
 */
int irdma_uattach_mcast(struct ibv_qp *qp, const union ibv_gid *gid,
			uint16_t lid)
{
	return ibv_cmd_attach_mcast(qp, gid, lid);
}

/**
 * irdma_udetach_mcast - Detach qp from multicast group
 * @qp: The queue pair
 * @gid:The Global ID for multicast group
 * @lid: The Local ID
 */
int irdma_udetach_mcast(struct ibv_qp *qp, const union ibv_gid *gid,
			uint16_t lid)
{
	return ibv_cmd_detach_mcast(qp, gid, lid);
}

/**
 * irdma_uresize_cq - resizes a cq
 * @cq: cq to resize
 * @cqe: the number of cqes of the new cq
 */
int irdma_uresize_cq(struct ibv_cq *cq, int cqe)
{
	struct irdma_uvcontext *iwvctx;
	struct irdma_uk_attrs *uk_attrs;
	struct irdma_uresize_cq cmd = {};
	struct ib_uverbs_resize_cq_resp resp = {};
	struct irdma_ureg_mr reg_mr_cmd = {};
	struct ib_uverbs_reg_mr_resp reg_mr_resp = {};
	struct irdma_cq_buf *cq_buf = NULL;
	struct irdma_cqe *cq_base = NULL;
	struct verbs_mr new_mr = {};
	struct irdma_ucq *iwucq;
	bool cqe_64byte_ena;
	size_t cq_size;
	__u32 cq_pages;
	int cqe_needed;
	int ret = 0;
	struct ibv_pd *pd = NULL;

	iwucq = container_of(cq, struct irdma_ucq, verbs_cq.cq);
	pd = iwucq->parent_domain;
	iwvctx = container_of(cq->context, struct irdma_uvcontext,
			      ibv_ctx.context);
	uk_attrs = &iwvctx->uk_attrs;

	if (!(uk_attrs->feature_flags & IRDMA_FEATURE_CQ_RESIZE))
		return EOPNOTSUPP;

	if (cqe < uk_attrs->min_hw_cq_size || cqe > uk_attrs->max_hw_cq_size - 1)
		return EINVAL;

	cqe_64byte_ena = uk_attrs->feature_flags & IRDMA_FEATURE_64_BYTE_CQE ? true : false;

	cqe_needed = get_cq_size(cqe, uk_attrs->hw_rev, cqe_64byte_ena);
	if (cqe_needed == iwucq->cq.cq_size)
		return 0;

	cq_size = get_cq_total_bytes(cqe_needed, cqe_64byte_ena);
	cq_pages = cq_size >> IRDMA_HW_PAGE_SHIFT;
	cq_base = irdma_alloc_buf(IRDMA_RESOURCE_TYPE_CQ, pd, cq_size, cq->context->cmd_fd, NULL);
	if (!cq_base)
		return ENOMEM;

	cq_buf = malloc(sizeof(*cq_buf));
	if (!cq_buf) {
		ret = ENOMEM;
		goto err_buf;
	}

	new_mr.ibv_mr.pd = iwucq->vmr.ibv_mr.pd;
	reg_mr_cmd.reg_type = IRDMA_MEMREG_TYPE_CQ;
	reg_mr_cmd.cq_pages = cq_pages;

	ret = ibv_cmd_reg_mr(new_mr.ibv_mr.pd, cq_base, cq_size,
			     (uintptr_t)cq_base, IBV_ACCESS_LOCAL_WRITE,
			     &new_mr, &reg_mr_cmd.ibv_cmd, sizeof(reg_mr_cmd),
			     &reg_mr_resp, sizeof(reg_mr_resp));
	if (ret)
		goto err_dereg_mr;

	ret = irdma_spin_lock(&iwucq->lock);
	if (ret)
		goto err_lock;

	cmd.user_cq_buffer = (__u64)((uintptr_t)cq_base);
	ret = ibv_cmd_resize_cq(&iwucq->verbs_cq.cq, cqe_needed, &cmd.ibv_cmd,
				sizeof(cmd), &resp, sizeof(resp));
	if (ret)
		goto err_resize;

	memcpy(&cq_buf->cq, &iwucq->cq, sizeof(cq_buf->cq));
	cq_buf->buf_size = iwucq->buf_size;
	iwucq->buf_size = cq_size;
	cq_buf->alloc_type = iwucq->alloc_type;
	cq_buf->vmr = iwucq->vmr;
	iwucq->vmr = new_mr;
	irdma_uk_cq_resize(&iwucq->cq, cq_base, cqe_needed);
	iwucq->verbs_cq.cq.cqe = cqe;
	list_add_tail(&iwucq->resize_list, &cq_buf->list);

	irdma_spin_unlock(&iwucq->lock);

	return ret;

err_resize:
	irdma_spin_unlock(&iwucq->lock);
err_lock:
	ibv_cmd_dereg_mr(&new_mr);
err_dereg_mr:
	free(cq_buf);
err_buf:
	fprintf(stderr, "failed to resize CQ cq_id=%d ret=%d\n", iwucq->cq.cq_id, ret);
	irdma_free_buf(cq_base, cq_size, iwucq->alloc_type);
	return ret;
}

struct ibv_td *irdma_ualloc_td(struct ibv_context *context, struct ibv_td_init_attr *init_attr)
{
	struct irdma_utd *iwutd;

	if (init_attr->comp_mask) {
		errno = EINVAL;
		return NULL;
	}

	iwutd = calloc(1, sizeof(*iwutd));
	if (!iwutd)
		return NULL;

	iwutd->ibv_td.context = context;
	atomic_init(&iwutd->refcount, 1);

	return &iwutd->ibv_td;
}

int irdma_udealloc_td(struct ibv_td *ibv_td)
{
	struct irdma_utd *iwutd;

	iwutd = container_of(ibv_td, struct irdma_utd, ibv_td);

	if (atomic_load(&iwutd->refcount) > 1)
		return EBUSY;

	free(iwutd);

	return 0;
}
