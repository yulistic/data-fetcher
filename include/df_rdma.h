#ifndef _DF_RDMA_H_
#define _DF_RDMA_H_
#include <rdma/rdma_cma.h>
#include <sys/types.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <pthread.h>

/*
 * Synchronization between CQ thread and post thread.
 * Semaphore is used if this macro is undefined.
 */
// #define SYNC_WITHOUT_SLEEP

/*
 * Each buffer is registered as a separate MR. Not tested yet.
 * (For memory boundary check.)
 */
// #define PER_BUF_MR

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV, 
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum rdma_ch_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	WORKING,
	DISCONNECTED,
	ERROR
};

// Used when creating RDMA channel.
struct rdma_ch_attr {
	int port;
	int server; /* 0 if client */
	char ip_addr[16]; // Target server ip addr. (required by client)
	int databuf_cnt; // The number of data buffers.
	uint64_t databuf_size; // The size of a data buffer.
};

// struct __attribute__((__packed__)) remote_mr_info {
struct remote_mr_info {
	// For storing requestor's (client's) info.
	uint32_t remote_rkey; /* remote guys RKEY */
	uint64_t remote_addr; /* remote guys TO */
};

/** Per message buffer context. */
struct databuf_ctx {
	int id;

	struct ibv_send_wr rdma_sq_wr; /* rdma work request record */
	struct ibv_sge rdma_sgl; /* rdma single SGE */
	char *rdma_buf; /* used as rdma sink */
	struct ibv_mr *rdma_mr;

	struct remote_mr_info remote_mr_info;

	atomic_ulong
		seqn; // TODO: It doesn't need to be atomic. Only one thread accesses it.
#ifdef SYNC_WITHOUT_SLEEP
	pthread_spinlock_t db_lock; // Sync between cq thread and post thread.
#else
	sem_t db_sem; // Sync between cq thread and post thread.
#endif
};

#ifdef SYNC_WITHOUT_SLEEP
static inline void wait_post_completion(struct databuf_ctx *db_ctx)
{
	pthread_spin_lock(&db_ctx->db_lock);
}

static inline void notify_post_completion(struct databuf_ctx *db_ctx)
{
	pthread_spin_unlock(&db_ctx->db_lock);
}
#else
static inline void wait_post_completion(struct databuf_ctx *db_ctx)
{
	sem_wait(&db_ctx->db_sem);
}

static inline void notify_post_completion(struct databuf_ctx *db_ctx)
{
	sem_post(&db_ctx->db_sem);
}
#endif

/** RDMA channel control block (per connection) */
struct rdma_ch_cb {
	int server; /* 0 iff client */
	pthread_t cqthread; // per client.
	pthread_t server_thread; // per client.
	pthread_t server_daemon; // per server.
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	int databuf_cnt; // Number of msg buffers.
	uint64_t databuf_size; // A size of a data buffer.
	struct databuf_ctx
		*buf_ctxs; // Used by connection cb. (not used by listening cb)

	enum rdma_ch_state state; /* used for cond/signalling */
	sem_t sem;

	struct sockaddr_storage sin;
	struct sockaddr_storage ssource;
	__be16 port; /* dst port in NBO */

	/* CM stuff */
	pthread_t cmthread; // per server.
	struct rdma_event_channel *cm_channel;
	struct rdma_cm_id *
		cm_id; /* connection on client side, listener on service side. */

	// TODO: To support multi-client connection, cloned_cb should be mapped
	// to each client connection. Currently, only 1 client is support.
	struct rdma_cm_id
		*child_cm_id; /* connection on server side, per client */
	
	/* Thread control */
	int stop_cq_thread; /* Flag to signal cq_thread to exit */
	int stop_cm_thread; /* Flag to signal cm_thread to exit */
};

struct rdma_ch_cb *df_init_rdma_ch(struct rdma_ch_attr *attr);
void df_destroy_rdma_client(struct rdma_ch_cb *cb);
void df_destroy_rdma_server(struct rdma_ch_cb *cb);
int df_post_rdma_read(struct rdma_ch_cb *server_cb, int databuf_id,
		      uint32_t length);

#endif
