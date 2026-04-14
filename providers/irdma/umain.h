/* SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB */
/* Copyright (C) 2019 - 2022 Intel Corporation */
#ifndef IRDMA_UMAIN_H
#define IRDMA_UMAIN_H

#include <inttypes.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <infiniband/driver.h>

#include "osdep.h"
#include "irdma.h"
#include "defs.h"
#include "i40iw_hw.h"
#include "user.h"

#ifndef likely
#define likely(x)	__builtin_expect((x), 1)
#endif
#ifndef unlikely
#define unlikely(x)	__builtin_expect((x), 0)
#endif
#define PFX	"libirdma-"

#define IRDMA_BASE_PUSH_PAGE		1
#define IRDMA_U_MINCQ_SIZE		4
#define IRDMA_DB_SHADOW_AREA_SIZE	64
#define IRDMA_DB_CQ_OFFSET		64

enum irdma_supported_wc_flags_ex {
	IRDMA_STANDARD_WC_FLAGS_EX = IBV_WC_EX_WITH_BYTE_LEN
				    | IBV_WC_EX_WITH_IMM
				    | IBV_WC_EX_WITH_QP_NUM
				    | IBV_WC_EX_WITH_SRC_QP
				    | IBV_WC_EX_WITH_SL,
	IRDMA_GEN3_WC_FLAGS_EX = IRDMA_STANDARD_WC_FLAGS_EX |
				 IBV_WC_EX_WITH_COMPLETION_TIMESTAMP,
};

enum irdma_alloc_type {
	IRDMA_ALLOC_TYPE_DEFAULT,
	IRDMA_ALLOC_TYPE_KMEM,
	IRDMA_ALLOC_TYPE_LAST,
};

enum irdma_resource_type {
	IRDMA_RESOURCE_TYPE_CQ,
	IRDMA_RESOURCE_TYPE_SRQ,
	IRDMA_RESOURCE_TYPE_QP,
	IRDMA_RESOURCE_TYPE_ANY,
};

struct irdma_resc_alloc_spec {
	enum irdma_resource_type resc_type;
	enum irdma_alloc_type alloc_type;
};

#define IRDMA_RESC_ALLOC_MAGIC 0xFA00FC00

struct irdma_resc_alloc_config {
	uint32_t magic;
	uint32_t total_size;
	uint32_t entry_size;
	uint32_t num_entries;
	struct irdma_resc_alloc_spec entries[];
};

struct irdma_udevice {
	struct verbs_device ibv_dev;
};

struct irdma_uah {
	struct ibv_ah ibv_ah;
	uint32_t ah_id;
	struct ibv_global_route grh;
};

struct irdma_upd {
	struct ibv_pd ibv_pd;
	void *arm_cq_page;
	void *arm_cq;
	uint32_t pd_id;
	struct list_node list;
	atomic_int refcount;
	struct irdma_upd *container_iwupd;
};

struct irdma_uvcontext {
	struct verbs_context ibv_ctx;
	struct irdma_upd *iwupd;
	struct irdma_uk_attrs uk_attrs;
	void *db;
	int abi_ver;
	bool legacy_mode:1;
	bool use_raw_attrs:1;
	struct list_head pd_list;
	struct irdma_spinlock pd_lock;
};

struct irdma_uqp;

struct irdma_cq_buf {
	struct list_node list;
	struct irdma_cq_uk cq;
	struct verbs_mr vmr;
	size_t buf_size;
	enum irdma_alloc_type alloc_type;
};

extern struct list_head dbg_ucq_list;
extern struct list_head dbg_uqp_list;
extern pthread_mutex_t sigusr1_wait_mutex;

struct irdma_usrq {
	struct verbs_srq v_srq;
	struct verbs_mr vmr;
	struct irdma_spinlock lock;
	struct irdma_srq_uk srq;
	size_t buf_size;
	enum irdma_alloc_type alloc_type;
};

struct irdma_ucq {
	struct verbs_cq verbs_cq;
	struct verbs_mr vmr;
	struct verbs_mr vmr_shadow_area;
	struct irdma_spinlock lock;
	size_t buf_size;
	enum irdma_alloc_type alloc_type;
	bool is_armed;
	bool skip_arm;
	bool arm_sol;
	bool skip_sol;
	int comp_vector;
	struct irdma_uqp *uqp;
	struct irdma_cq_uk cq;
	struct list_head resize_list;
	/* for extended CQ completion fields */
	struct irdma_cq_poll_info cur_cqe;
	struct list_node dbg_entry;
	struct ibv_pd *parent_domain;
};

struct irdma_uqp {
	struct ibv_qp ibv_qp;
	struct irdma_ucq *send_cq;
	struct irdma_ucq *recv_cq;
	struct verbs_mr vmr;
	size_t buf_size;
	enum irdma_alloc_type alloc_type;
	uint32_t irdma_drv_opt;
	struct irdma_spinlock lock;
	uint16_t sq_sig_all;
	uint16_t qperr;
	uint16_t rsvd;
	uint32_t pending_rcvs;
	uint32_t wq_size;
	struct ibv_recv_wr *pend_rx_wr;
	struct irdma_qp_uk qp;
	enum ibv_qp_type qp_type;
	struct list_node dbg_entry;
};

struct irdma_utd {
	struct ibv_td ibv_td;
	atomic_int refcount;
};

struct irdma_uparent_domain {
	struct irdma_upd iwupd;
	struct irdma_utd *iwutd;
	void *(*alloc)(struct ibv_pd *pd, void *pd_context, size_t size,
		       size_t alignment, uint64_t resource_type);
	void (*free)(struct ibv_pd *pd, void *pd_context, void *ptr,
		     uint64_t resource_type);
	void *pd_context;
};

static inline struct irdma_uparent_domain *to_iw_uparent_domain(struct ibv_pd *ibv_pd)
{
	struct irdma_uparent_domain *iw_parent_domain =
		container_of(ibv_pd, struct irdma_uparent_domain, iwupd.ibv_pd);

	if (iw_parent_domain && iw_parent_domain->iwupd.container_iwupd)
		return iw_parent_domain;

	return NULL;
}

static inline int irdma_spin_init(struct irdma_spinlock *lock, bool skip_lock)
{
	lock->skip_lock = skip_lock;

	if (lock->skip_lock)
		return 0;

	return pthread_spin_init(&lock->lock, PTHREAD_PROCESS_PRIVATE);
}

static inline int irdma_spin_init_pd(struct irdma_spinlock *lock, struct ibv_pd *pd)
{
	struct irdma_uparent_domain *iw_parent_domain = to_iw_uparent_domain(pd);
	bool skip_lock = false;

	if (iw_parent_domain && iw_parent_domain->iwutd)
		skip_lock = true;

	return irdma_spin_init(lock, skip_lock);
}

static inline int irdma_spin_destroy(struct irdma_spinlock *lock)
{
	if (lock->skip_lock)
		return 0;

	return pthread_spin_destroy(&lock->lock);
}

/* irdma_uverbs.c */
int irdma_uquery_device_ex(struct ibv_context *context,
			   const struct ibv_query_device_ex_input *input,
			   struct ibv_device_attr_ex *attr, size_t attr_size);
int irdma_uquery_port(struct ibv_context *context, uint8_t port,
		      struct ibv_port_attr *attr);
struct ibv_pd *irdma_ualloc_pd(struct ibv_context *context);
int irdma_ufree_pd(struct ibv_pd *pd);
struct ibv_mr *irdma_ureg_mr(struct ibv_pd *pd, void *addr, size_t length,
			     uint64_t hca_va, int access);
struct ibv_mr *irdma_ureg_mr_dmabuf(struct ibv_pd *pd, uint64_t offset,
				    size_t length, uint64_t iova, int fd,
				    int access);
int irdma_udereg_mr(struct verbs_mr *vmr);

int irdma_urereg_mr(struct verbs_mr *mr, int flags, struct ibv_pd *pd, void *addr,
		    size_t length, int access);

struct ibv_mw *irdma_ualloc_mw(struct ibv_pd *pd, enum ibv_mw_type type);
int irdma_ubind_mw(struct ibv_qp *qp, struct ibv_mw *mw,
		   struct ibv_mw_bind *mw_bind);
int irdma_udealloc_mw(struct ibv_mw *mw);
struct ibv_cq *irdma_ucreate_cq(struct ibv_context *context, int cqe,
				struct ibv_comp_channel *channel,
				int comp_vector);
struct ibv_cq_ex *irdma_ucreate_cq_ex(struct ibv_context *context,
				      struct ibv_cq_init_attr_ex *attr_ex);
void irdma_ibvcq_ex_fill_priv_funcs(struct irdma_ucq *iwucq,
				    struct ibv_cq_init_attr_ex *attr_ex);
int irdma_uresize_cq(struct ibv_cq *cq, int cqe);
int irdma_udestroy_cq(struct ibv_cq *cq);
int irdma_upoll_cq(struct ibv_cq *cq, int entries, struct ibv_wc *entry);
int irdma_uarm_cq(struct ibv_cq *cq, int solicited);
void irdma_cq_event(struct ibv_cq *cq);
struct ibv_qp *irdma_ucreate_qp(struct ibv_pd *pd,
				struct ibv_qp_init_attr *attr);
int irdma_uquery_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask,
		    struct ibv_qp_init_attr *init_attr);
int irdma_umodify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
		     int attr_mask);
int irdma_udestroy_qp(struct ibv_qp *qp);
int irdma_upost_send(struct ibv_qp *ib_qp, struct ibv_send_wr *ib_wr,
		     struct ibv_send_wr **bad_wr);
int irdma_upost_recv(struct ibv_qp *ib_qp, struct ibv_recv_wr *ib_wr,
		     struct ibv_recv_wr **bad_wr);
struct ibv_ah *irdma_ucreate_ah(struct ibv_pd *ibpd, struct ibv_ah_attr *attr);
int irdma_udestroy_ah(struct ibv_ah *ibah);
int irdma_uattach_mcast(struct ibv_qp *qp, const union ibv_gid *gid,
			uint16_t lid);
int irdma_udetach_mcast(struct ibv_qp *qp, const union ibv_gid *gid,
			uint16_t lid);
struct ibv_srq *irdma_ucreate_srq(struct ibv_pd *pd,
				  struct ibv_srq_init_attr *initattr);
int irdma_udestroy_srq(struct ibv_srq *ibsrq);
int irdma_uquery_srq(struct ibv_srq *ibsrq, struct ibv_srq_attr *attr);
int irdma_umodify_srq(struct ibv_srq *ibsrq, struct ibv_srq_attr *attr,
		      int attr_mask);
int irdma_upost_srq(struct ibv_srq *ib_srq, struct ibv_recv_wr *ib_wr,
		    struct ibv_recv_wr **bad_wr);
void irdma_async_event(struct ibv_context *context,
		       struct ibv_async_event *event);
void irdma_set_hw_attrs(struct irdma_hw_attrs *attrs);
void *irdma_mmap(int fd, size_t length, off_t offset);
void irdma_munmap(void *map, size_t length);
struct ibv_td *irdma_ualloc_td(struct ibv_context *context,
			       struct ibv_td_init_attr *init_attr);
int irdma_udealloc_td(struct ibv_td *td);
struct ibv_pd *irdma_ualloc_parent_domain(struct ibv_context *context,
					  struct ibv_parent_domain_init_attr *int_attr);
#endif /* IRDMA_UMAIN_H */
