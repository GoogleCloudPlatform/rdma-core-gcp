// SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB
/* Copyright (C) 2019 - 2023 Intel Corporation */
#if HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

#include "ice_devids.h"
#include "i40e_devids.h"
#include "idpf_devids.h"
#include "umain.h"
#include "abi.h"

/* Shm ABI max. */
#define SHARED_UD_MAX_CREDITS            1024u

/* If a credit is held for >= 8 seconds, assume owner dead and reclaim it. */
#define SHARED_UD_CREDIT_RECLAIM_S       8u

#define SHARED_UD_SHM_NAME               "/irdma_ud_shm"
#define SHARED_UD_SHM_SIZE               sizeof(struct shared_ud_shm)
#define SHARED_UD_SHM_ABI_VER            1

struct shared_ud_shm {
	/* Shm region is initialized to 0 by default. This gets set to true by
	 * the process that wins the race to create the region.
	 */
	_Atomic(bool) init_done;
	uint32_t abi_version;
	/* Credit array. */
	_Atomic(uint32_t) acquire_time[SHARED_UD_MAX_CREDITS];
};

/* Mutual exclusion during init isn't strictly necessary... there can already be
 * multiple independent processes racing for init and the init routine is robust
 * in that scenario...
 */
static pthread_mutex_t shared_ud_init_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(bool) shared_ud_init_done;
static struct shared_ud_shm *g_shared_ud_shm;

struct irdma_env_params env = {
	.pass_cie_vendor_err = false,
	.ud_qd_override = 0,
	.transparent_ud_qd_override = 0,
	.cq_size_override = 0,
	.shared_ud_credits = 64,
};

unsigned int irdma_dbg;
static pthread_t dbg_thread;
static pthread_cond_t cond_sigusr1_rcvd;
static _Atomic(int) dbg_thread_exit;
static _Atomic(int) dev_allocated_refcount;
pthread_mutex_t sigusr1_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(dbg_ucq_list);	/* list of alive cqs */
LIST_HEAD(dbg_uqp_list);	/* list of alive qps */

#define INTEL_HCA(v, d) VERBS_PCI_MATCH(v, d, NULL)
static const struct verbs_match_ent hca_table[] = {
	VERBS_DRIVER_ID(RDMA_DRIVER_IRDMA),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823L_BACKPLANE),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823L_SFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823L_10G_BASE_T),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823L_1GBE),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823L_QSFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E810C_BACKPLANE),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E810C_QSFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E810C_SFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E810_XXV_BACKPLANE),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E810_XXV_QSFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E810_XXV_SFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823C_BACKPLANE),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823C_QSFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823C_SFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823C_10G_BASE_T),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E823C_SGMII),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_C822N_BACKPLANE),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_C822N_QSFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_C822N_SFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E822C_10G_BASE_T),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E822C_SGMII),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E822L_BACKPLANE),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E822L_SFP),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E822L_10G_BASE_T),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, ICE_DEV_ID_E822L_SGMII),

	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_X722_A0),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_X722_A0_VF),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_KX_X722),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_QSFP_X722),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_SFP_X722),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_1G_BASE_T_X722),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_10G_BASE_T_X722),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_SFP_I_X722),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_X722_VF),
	INTEL_HCA(I40E_INTEL_VENDOR_ID, I40E_DEV_ID_X722_VF_HV),

	INTEL_HCA(PCI_VENDOR_ID_INTEL, IDPF_DEV_ID_PF),
	INTEL_HCA(PCI_VENDOR_ID_INTEL, IAVF_DEV_ID_ADAPTIVE_VF),
	{}
};

/**
 * irdma_ufree_context - free context that was allocated
 * @ibctx: context allocated ptr
 */
static void irdma_ufree_context(struct ibv_context *ibctx)
{
	struct irdma_uvcontext *iwvctx;

	iwvctx = container_of(ibctx, struct irdma_uvcontext,
			      ibv_ctx.context);
	irdma_ufree_pd(&iwvctx->iwupd->ibv_pd);
	irdma_munmap(iwvctx->db, IRDMA_HW_PAGE_SIZE);
	verbs_uninit_context(&iwvctx->ibv_ctx);
	irdma_spin_destroy(&iwvctx->pd_lock);

	free(iwvctx);
}

static const struct verbs_context_ops irdma_uctx_mcast_ops = {
	.attach_mcast = irdma_uattach_mcast,
	.detach_mcast = irdma_udetach_mcast,
};

static const struct verbs_context_ops irdma_uctx_srq_ops = {
	.create_srq = irdma_ucreate_srq,
	.destroy_srq = irdma_udestroy_srq,
	.modify_srq = irdma_umodify_srq,
	.post_srq_recv = irdma_upost_srq,
	.query_srq = irdma_uquery_srq,
};

static const struct verbs_context_ops irdma_uctx_ops = {
	.alloc_mw = irdma_ualloc_mw,
	.alloc_pd = irdma_ualloc_pd,
	.alloc_parent_domain = irdma_ualloc_parent_domain,
	.alloc_td = irdma_ualloc_td,
	.bind_mw = irdma_ubind_mw,
	.cq_event = irdma_cq_event,
	.create_ah = irdma_ucreate_ah,
	.create_cq = irdma_ucreate_cq,
	.create_cq_ex = irdma_ucreate_cq_ex,
	.create_qp = irdma_ucreate_qp,
	.dealloc_mw = irdma_udealloc_mw,
	.dealloc_pd = irdma_ufree_pd,
	.dealloc_td = irdma_udealloc_td,
	.dereg_mr = irdma_udereg_mr,
	.destroy_ah = irdma_udestroy_ah,
	.destroy_cq = irdma_udestroy_cq,
	.destroy_qp = irdma_udestroy_qp,
	.modify_qp = irdma_umodify_qp,
	.poll_cq = irdma_upoll_cq,
	.post_recv = irdma_upost_recv,
	.post_send = irdma_upost_send,
	.query_device_ex = irdma_uquery_device_ex,
	.query_port = irdma_uquery_port,
	.query_qp = irdma_uquery_qp,
	.reg_dmabuf_mr = irdma_ureg_mr_dmabuf,
	.reg_mr = irdma_ureg_mr,
	.rereg_mr = irdma_urereg_mr,
	.req_notify_cq = irdma_uarm_cq,
	.resize_cq = irdma_uresize_cq,
	.free_context = irdma_ufree_context,
};

/**
 * i40iw_set_hw_attrs - set the hw attributes
 * @attrs: pointer to hw attributes
 *
 * Set the device attibutes to allow user mode to work with
 * driver on older ABI version.
 */
static void i40iw_set_hw_attrs(struct irdma_uk_attrs *attrs)
{
	attrs->hw_rev = IRDMA_GEN_1;
	attrs->max_hw_wq_frags = I40IW_MAX_WQ_FRAGMENT_COUNT;
	attrs->max_hw_read_sges = I40IW_MAX_SGE_RD;
	attrs->max_hw_inline = I40IW_MAX_INLINE_DATA_SIZE;
	attrs->max_hw_rq_quanta = I40IW_QP_SW_MAX_RQ_QUANTA;
	attrs->max_hw_wq_quanta = I40IW_QP_SW_MAX_WQ_QUANTA;
	attrs->max_hw_sq_chunk = I40IW_MAX_QUANTA_PER_WR;
	attrs->max_hw_cq_size = I40IW_MAX_CQ_SIZE;
	attrs->min_hw_cq_size = IRDMA_MIN_CQ_SIZE;
	attrs->min_hw_wq_size = I40IW_MIN_WQ_SIZE;
}

static uint32_t seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

/* Try to obtain a UD credit. May fail if racing with another thread. Returns
 * true on success.
 */
static bool try_ud_credit(_Atomic(uint32_t) *credit_ts, uint32_t *old_credit_ts,
			  uint32_t now)
{
	return atomic_compare_exchange_weak_explicit(credit_ts,
						     old_credit_ts,
						     now,
						     memory_order_relaxed,
						     memory_order_relaxed);
}

/* Returns -1 on failure, or the credit index otherwise. */
int try_acquire_credit(uint32_t *cookie)
{
	struct shared_ud_shm *inst = g_shared_ud_shm;
	uint32_t acquisition_ts;
	uint32_t delta;
	uint32_t now;
	size_t i;

	if (!atomic_load_explicit(&shared_ud_init_done, memory_order_acquire))
		return -1;

	now = seconds();

	for (i = 0; i < env.shared_ud_credits; i++) {
		acquisition_ts =
			atomic_load_explicit(&inst->acquire_time[i],
					     memory_order_relaxed);
		delta = now - acquisition_ts;
		if (!acquisition_ts || delta > SHARED_UD_CREDIT_RECLAIM_S) {
			if (try_ud_credit(&inst->acquire_time[i],
					  &acquisition_ts, now)) {
				*cookie = now;
				return i;
			}
		}
	}

	return -1;
}

/* Return a credit to the table. */
void release_credit(int index, uint32_t cookie)
{
	struct shared_ud_shm *inst = g_shared_ud_shm;
	atomic_store_explicit(&inst->acquire_time[index], 0,
			      memory_order_relaxed);

}

/* Initialize the backpressure shared memory region for the first time. */
static void init_backpressure_region(struct shared_ud_shm *inst)
{
	size_t i;

	for (i = 0; i < SHARED_UD_MAX_CREDITS; i++)
		atomic_store(&inst->acquire_time[i], 0);

	inst->abi_version = SHARED_UD_SHM_ABI_VER;

	atomic_store(&inst->init_done, true);
}

static void init_ud_credits(void)
{
	int shm_fd;
	bool created = false;
	struct shared_ud_shm *inst;

	if (atomic_load_explicit(&shared_ud_init_done, memory_order_relaxed))
		return;

	shm_fd = shm_open(SHARED_UD_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
	if (shm_fd >= 0) {
		/* We won the race to create the region. */
		/* Force 0666 permissions to override any restrictive umask */
		if (fchmod(shm_fd, 0666) == -1)
			perror("Failed to force 0666 permissions on shm");
		created = true;
	} else if (errno == EEXIST) {
		/* We lost the race. */
		shm_fd = shm_open(SHARED_UD_SHM_NAME, O_RDWR, 0);
		if (shm_fd == -1) {
			perror("Failed to open existing backpressure region");
			return;
		}
	} else {
		perror("Failed to open backpressure region");
		return;
	}

	/* ftruncate is supposed to be atomic... */
	if (ftruncate(shm_fd, SHARED_UD_SHM_SIZE) == -1) {
		perror("Failed to set backpressure region size");
		close(shm_fd);
		if (created)
			shm_unlink(SHARED_UD_SHM_NAME);
		return;
	}

	inst = mmap(NULL, SHARED_UD_SHM_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED, shm_fd, 0);
	if (inst == MAP_FAILED) {
		perror("Failed to map backpressure region");
		close(shm_fd);
		if (created)
			shm_unlink(SHARED_UD_SHM_NAME);
		exit(EXIT_FAILURE);
	}

	if (created) {
		printf("Created shared UD credit table\n");
		init_backpressure_region(inst);
	} else {
		while (!atomic_load(&inst->init_done))
			/* Wait for the creator to finish initializing. */
			usleep(1000);
		if (inst->abi_version != SHARED_UD_SHM_ABI_VER) {
			fprintf(stderr, "Backpressure region ABI mismatch!\n"
					"This is expected if an update was "
					"performed without rebooting.\n"
					"Cleaning up then exiting. "
					"Please restart the application.\n");
			/* Delete the region. */
			shm_unlink(SHARED_UD_SHM_NAME);
			exit(EXIT_FAILURE);
		}
	}

	close(shm_fd);

	g_shared_ud_shm = inst;
	atomic_store_explicit(&shared_ud_init_done, true, memory_order_release);
}

/**
 * irdma_ualloc_context - allocate context for user app
 * @ibdev: ib device created during irdma_driver_init
 * @cmd_fd: save fd for the device
 * @private_data: device private data
 *
 * Returns callback routine table and calls driver for allocating
 * context and getting back resource information to return as ibv_context.
 */
static struct verbs_context *irdma_ualloc_context(struct ibv_device *ibdev,
						  int cmd_fd, void *private_data)
{
	struct ibv_pd *ibv_pd;
	struct irdma_uvcontext *iwvctx;
	struct irdma_get_context cmd = {};
	struct irdma_get_context_resp resp = {};
	__u64 mmap_key;
	__u8 user_ver = IRDMA_ABI_VER;
	int ret;

	iwvctx = verbs_init_and_alloc_context(ibdev, cmd_fd, iwvctx, ibv_ctx,
					      RDMA_DRIVER_IRDMA);
	if (!iwvctx)
		return NULL;

	if (irdma_spin_init(&iwvctx->pd_lock, false)) {
		free(iwvctx);
		return NULL;
	}

	cmd.comp_mask |= IRDMA_ALLOC_UCTX_USE_RAW_ATTR;
	cmd.comp_mask |= IRDMA_SUPPORT_WQE_FORMAT_V2;
retry:
	cmd.userspace_ver = user_ver;
#if 1
	ret = ibv_cmd_get_context(&iwvctx->ibv_ctx, (struct ibv_get_context *)&cmd,
				  sizeof(cmd), NULL, &resp.ibv_resp, sizeof(resp));
#else
	ret = ibv_cmd_get_context(&iwvctx->ibv_ctx, (struct ibv_get_context *)&cmd,
				  sizeof(cmd), &resp.ibv_resp, sizeof(resp));
#endif
	if (ret) {
		if (--user_ver >= 4)
			goto retry;

		goto err_free;
	}

	verbs_set_ops(&iwvctx->ibv_ctx, &irdma_uctx_ops);
	if (resp.hw_rev == IRDMA_GEN_2 && ibdev->transport_type != IBV_TRANSPORT_IWARP)
		verbs_set_ops(&iwvctx->ibv_ctx, &irdma_uctx_mcast_ops);
	if (resp.feature_flags & IRDMA_FEATURE_SRQ)
		verbs_set_ops(&iwvctx->ibv_ctx, &irdma_uctx_srq_ops);

	/* Legacy i40iw does not populate hw_rev. The irdma driver always sets it */
	if (!resp.hw_rev) {
		i40iw_set_hw_attrs(&iwvctx->uk_attrs);
		iwvctx->abi_ver = resp.kernel_ver;
		iwvctx->legacy_mode = true;
		mmap_key = 0;
	} else {
		iwvctx->uk_attrs.feature_flags = resp.feature_flags;
		iwvctx->uk_attrs.hw_rev = resp.hw_rev;
		iwvctx->uk_attrs.max_hw_wq_frags = resp.max_hw_wq_frags;
		iwvctx->uk_attrs.max_hw_read_sges = resp.max_hw_read_sges;
		iwvctx->uk_attrs.max_hw_inline = resp.max_hw_inline;
		iwvctx->uk_attrs.max_hw_rq_quanta = resp.max_hw_rq_quanta;
		iwvctx->uk_attrs.max_hw_wq_quanta = resp.max_hw_wq_quanta;
		iwvctx->uk_attrs.max_hw_sq_chunk = resp.max_hw_sq_chunk;
		iwvctx->uk_attrs.max_hw_cq_size = resp.max_hw_cq_size;
		iwvctx->uk_attrs.min_hw_cq_size = resp.min_hw_cq_size;
		iwvctx->abi_ver = user_ver;
		if (resp.comp_mask & IRDMA_ALLOC_UCTX_USE_RAW_ATTR)
			iwvctx->use_raw_attrs = true;
		if (resp.comp_mask & IRDMA_ALLOC_UCTX_MIN_HW_WQ_SIZE)
			iwvctx->uk_attrs.min_hw_wq_size = resp.min_hw_wq_size;
		else
			iwvctx->uk_attrs.min_hw_wq_size = IRDMA_QP_SW_MIN_WQSIZE;
		iwvctx->uk_attrs.max_hw_srq_quanta = resp.max_hw_srq_quanta;
		if (resp.comp_mask & IRDMA_SUPPORT_MAX_HW_PUSH_LEN)
			iwvctx->uk_attrs.max_hw_push_len = resp.max_hw_push_len;
		else
			iwvctx->uk_attrs.max_hw_push_len = IRDMA_DEFAULT_MAX_PUSH_LEN;
		mmap_key = resp.db_mmap_key;
	}

	iwvctx->db = irdma_mmap(cmd_fd, IRDMA_HW_PAGE_SIZE, mmap_key);
	if (iwvctx->db == MAP_FAILED)
		goto err_free;

	list_head_init(&iwvctx->pd_list);
	ibv_pd = irdma_ualloc_pd(&iwvctx->ibv_ctx.context);
	if (!ibv_pd) {
		irdma_munmap(iwvctx->db, IRDMA_HW_PAGE_SIZE);
		goto err_free;
	}

	ibv_pd->context = &iwvctx->ibv_ctx.context;
	iwvctx->iwupd = container_of(ibv_pd, struct irdma_upd, ibv_pd);
	return &iwvctx->ibv_ctx;

err_free:
	fprintf(stderr, PFX "%s: failed to allocate context for device, kernel ver:%d, user ver:%d hw_rev=%d\n",
		__func__, resp.kernel_ver, IRDMA_ABI_VER, resp.hw_rev);
	irdma_spin_destroy(&iwvctx->pd_lock);
	free(iwvctx);

	return NULL;
}

static void irdma_uninit_device(struct verbs_device *verbs_device)
{
	struct irdma_udevice *dev;

	if (!verbs_device) {
		fprintf(stderr, PFX "%s: called with NULL ptr.\n", __func__);
		return;
	}
	/* Destroy debug_thread only upon last call to irdma_uninit_device.
	 * dev_allocated_refcount tracks number of devices created
	 */
	if (irdma_dbg && atomic_fetch_sub(&dev_allocated_refcount, 1) == 1) {
		atomic_store(&dbg_thread_exit, 1);
		if (!pthread_cond_signal(&cond_sigusr1_rcvd)) {
			int ret;

			ret = pthread_join(dbg_thread, NULL);
			if (ret)
				fprintf(stderr, PFX "%s: failed to pthread join the dbg thread error:%d\n",
					__func__, ret);
			pthread_mutex_destroy(&sigusr1_wait_mutex);
			pthread_cond_destroy(&cond_sigusr1_rcvd);
		}
	}
	dev = container_of(&verbs_device->device, struct irdma_udevice,
			   ibv_dev.device);
	free(dev);
}

static void *dump_data_handler(void *unused)
{
	struct irdma_ucq *dbg_ucq, *next;
	struct irdma_uqp *dbg_uqp, *next_qp;
	int ret = 0;

	pthread_mutex_lock(&sigusr1_wait_mutex);
	while (1) {
		ret = pthread_cond_wait(&cond_sigusr1_rcvd, &sigusr1_wait_mutex);

		if (ret || atomic_load(&dbg_thread_exit)) {
			pthread_mutex_unlock(&sigusr1_wait_mutex);
			return NULL;
		}

		list_for_each_safe(&dbg_ucq_list, dbg_ucq, next, dbg_entry) {
			ret = irdma_spin_lock(&dbg_ucq->lock);
			if (ret) {
				pthread_mutex_unlock(&sigusr1_wait_mutex);
				return NULL;
			}
			irdma_print_cqes(&dbg_ucq->cq);
			irdma_spin_unlock(&dbg_ucq->lock);
		}

		list_for_each_safe(&dbg_uqp_list, dbg_uqp, next_qp, dbg_entry) {
			ret = irdma_spin_lock(&dbg_uqp->lock);
			if (ret) {
				pthread_mutex_unlock(&sigusr1_wait_mutex);
				return NULL;
			}
			irdma_print_sq_wqes(&dbg_uqp->qp);
			irdma_spin_unlock(&dbg_uqp->lock);
		}
	}
	pthread_mutex_unlock(&sigusr1_wait_mutex);
}

static void irdma_signal_handler(int signum)
{
	switch (signum) {
	case SIGUSR1:
		fprintf(stdout, "%s: Received SIGUSR1 signal\n", __func__);
		pthread_cond_signal(&cond_sigusr1_rcvd);
		break;
	default:
		fprintf(stdout, "%s: Unhandled signal %d\n", __func__, signum);
		break;
	}
}

static struct verbs_device *irdma_device_alloc(struct verbs_sysfs_dev *sysfs_dev)
{
	struct irdma_udevice *dev;
	char *env_val;
	int tmp;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	env_val = getenv("IRDMA_PASS_CIE_VENDOR_ERR");
	if (env_val) {
		tmp = atoi(env_val);
		if (tmp == 0 || tmp == 1)
			env.pass_cie_vendor_err = tmp;
		else
			fprintf(stderr,
				"%s: Invalid value (%d) for "
				"IRDMA_PASS_CIE_VENDOR_ERR\n", __func__, tmp);
	}

	env_val = getenv("IRDMA_UD_QD_OVERRIDE");
	if (env_val) {
		tmp = atoi(env_val);
		if (tmp < 0)
			fprintf(stderr, "Ignoring invalid IRDMA_UD_QD_OVERRIDE "
				"value of %d\n", tmp);
		else
			env.ud_qd_override = tmp;
	}


	env_val = getenv("IRDMA_TRANSPARENT_UD_QD_OVERRIDE");
	if (env_val) {
		tmp = atoi(env_val);
		if (tmp < 0)
			fprintf(stderr, "Ignoring invalid "
				"IRDMA_TRANSPARENT_UD_QD_OVERRIDE value of "
				"%d\n", tmp);
		else
			env.transparent_ud_qd_override = tmp;
	}

	env_val = getenv("IRDMA_CQ_SIZE_OVERRIDE");
	if (env_val) {
		tmp = atoi(env_val);
		if (tmp < 0)
			fprintf(stderr, "Ignoring invalid "
				"IRDMA_CQ_SIZE_OVERRIDE value of %d\n", tmp);
		else
			env.cq_size_override = tmp;
	}

	env_val = getenv("IRDMA_SHARED_UD_CREDITS");
	if (env_val) {
		tmp = atoi(env_val);
		if (tmp < 0) {
			fprintf(stderr, "Ignoring invalid "
				"IRDMA_SHARED_UD_CREDITS value of %d\n", tmp);
		} else if (tmp > SHARED_UD_MAX_CREDITS) {
			fprintf(stderr, "Clamping IRDMA_SHARED_UD_CREDITS to "
				"%d\n", SHARED_UD_MAX_CREDITS);
			tmp = SHARED_UD_MAX_CREDITS;
		}
		env.shared_ud_credits = tmp;
	}

	env_val = getenv("IRDMA_DEBUG");
	if (env_val)
		irdma_dbg = atoi(env_val);

	if (irdma_dbg) {
		printf("IRDMA provider running in debug mode.\n"
		       "Params:\n"
		       "\tIRDMA_PASS_CIE_VENDOR_ERR = %d\n"
		       "\tIRDMA_UD_QD_OVERRIDE = %d\n"
		       "\tIRDMA_TRANSPARENT_UD_QD_OVERRIDE = %d\n"
		       "\tIRDMA_CQ_SIZE_OVERRIDE = %d\n"
		       "\tIRDMA_SHARED_UD_CREDITS = %d\n",
		       env.pass_cie_vendor_err, env.ud_qd_override,
		       env.transparent_ud_qd_override, env.cq_size_override,
		       env.shared_ud_credits);
	}

	/* Create debug_thread only once upon first call to irdma_device_alloc
	 * dev_allocated_refcount tracks number of devices created
	 */
	if (irdma_dbg && !atomic_fetch_add(&dev_allocated_refcount, 1)) {
		int ret;

		signal(SIGUSR1, irdma_signal_handler);
		pthread_cond_init(&cond_sigusr1_rcvd, NULL);

		ret = pthread_create(&dbg_thread, NULL, dump_data_handler, NULL);
		if (ret) {
			free(dev);
			pthread_cond_destroy(&cond_sigusr1_rcvd);
			return NULL;
		}
	}

	pthread_mutex_lock(&shared_ud_init_lock);
	if (env.transparent_ud_qd_override && env.shared_ud_credits)
		init_ud_credits();
	pthread_mutex_unlock(&shared_ud_init_lock);

	return &dev->ibv_dev;
}

static const struct verbs_device_ops irdma_udev_ops = {
	.alloc_context = irdma_ualloc_context,
	.alloc_device = irdma_device_alloc,
	.match_max_abi_version = IRDMA_MAX_ABI_VERSION,
	.match_min_abi_version = IRDMA_MIN_ABI_VERSION,
	.match_table = hca_table,
	.name = "irdma",
	.uninit_device = irdma_uninit_device,
};

PROVIDER_DRIVER(irdma, irdma_udev_ops);
