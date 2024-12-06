// #define _GNU_SOURCE
// #define _BSD_SOURCE
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <inttypes.h>
// #include <netinet/in.h>
// #include <infiniband/arch.h>
#include <rdma/rdma_cma.h>
#include "global.h"
#include "df_rdma.h"
#include "log.h"

/* In this module, a server pulls data from a client by RDMA read. */

#define RDMA_SQ_DEPTH 16
#define RDMA_RQ_DEPTH 16

static void deregister_mrs(struct rdma_ch_cb *cb);
static void free_buffers(struct rdma_ch_cb *cb);
static int init_buf_ctxs(struct rdma_ch_cb *cb);
static void free_buf_ctxs(struct rdma_ch_cb *cb);

struct rdma_event_channel *create_first_event_channel(void)
{
	struct rdma_event_channel *channel;

	channel = rdma_create_event_channel();
	if (!channel) {
		if (errno == ENODEV)
			fprintf(stderr, "No RDMA devices were detected\n");
		else
			printf("Failed to create RDMA CM event channel\n");
	}
	return channel;
}

/**
 * @brief Set the remote mr info.
 * TODO: multi-client support. It should be copied to the per-client buf_ctxs.
 * Currently, we store it to listening_cb's buf_ctxs and copy it to connect cb
 * in `clone_cb()`.
 * 
 * @param cb 
 * @param rm_info pointer to the array of rm_info.
 */
static void set_remote_mr_info(struct rdma_ch_cb *cb,
			       struct remote_mr_info *rm_info)
{
	int i;
	struct databuf_ctx *db_ctx;
	uint32_t r_rkey;
	uint64_t r_addr;

	for (i = 0; i < cb->databuf_cnt; i++) {
		db_ctx = &cb->buf_ctxs[i];
#ifdef PER_BUF_MR
		r_rkey = be32toh(rm_info[i].remote_rkey);
		r_addr = be64toh(rm_info[i].remote_addr) + cb->databuf_size * i;

		// db_ctx->remote_mr_info.remote_rkey =
		// 	be32toh(rm_info[i].remote_rkey);
		// db_ctx->remote_mr_info.remote_addr =
		// 	be64toh(rm_info[i].remote_addr) + cb->databuf_size * i;
#else
		r_rkey = be32toh(rm_info->remote_rkey);
		r_addr = be64toh(rm_info->remote_addr) + cb->databuf_size * i;

		// db_ctx->remote_mr_info.remote_rkey =
		// 	be32toh(rm_info->remote_rkey);
		// db_ctx->remote_mr_info.remote_addr =
		// 	be64toh(rm_info->remote_addr) + cb->databuf_size * i;
#endif
		db_ctx->remote_mr_info.remote_rkey = r_rkey;
		db_ctx->remote_mr_info.remote_addr = r_addr;

		db_ctx->rdma_sq_wr.wr.rdma.rkey = r_rkey;
		db_ctx->rdma_sq_wr.wr.rdma.remote_addr = r_addr;

		log_debug("buf_ctxs[%d] remote_rkey=0x%x remote_addr=0x%lx", i,
			  r_rkey, r_addr);
	}
}

static int cma_event_handler(struct rdma_cm_id *cma_id,
			     struct rdma_cm_event *event)
{
	int ret = 0;
	struct rdma_ch_cb *cb = cma_id->context;

	log_debug("cma_event type %s cma_id %p (%s)",
		  rdma_event_str(event->event), cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			cb->state = ERROR;
			fprintf(stderr, "rdma_resolve_route\n");
			sem_post(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;

		set_remote_mr_info(cb, (struct remote_mr_info *)
					       event->param.conn.private_data);

		log_debug("child cma %p", cb->child_cm_id);
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		log_debug("ESTABLISHED");

		/*
		 * Server will wake up when first RECV completes.
		 */
		if (!cb->server) {
			cb->state = CONNECTED;
		}
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		log_error("cma event %s, error %d\n",
				rdma_event_str(event->event), event->status);
		sem_post(&cb->sem);
		ret = -1;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		log_info("Data Fetcher %s got CLIENT DISCONNECTED event.",
				cb->server ? "server" : "client", cb);
		cb->state = DISCONNECTED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		log_error("cma detected device removal!!!!\n");
		ret = -1;
		break;

	default:
		log_warn("unhandled event: %s, ignoring\n",
				rdma_event_str(event->event));
		break;
	}

	return ret;
}

void *df_cm_thread(void *arg)
{
	struct rdma_ch_cb *cb = (struct rdma_ch_cb *)arg;
	struct rdma_cm_event *event;
	int ret;

	while (1) {
		ret = rdma_get_cm_event(cb->cm_channel, &event);
		if (ret) {
			fprintf(stderr, "rdma_get_cm_event");
			exit(ret);
		}
		ret = cma_event_handler(event->id, event);
		rdma_ack_cm_event(event);
		if (ret)
			exit(ret);
	}
}

static int bind_server(struct rdma_ch_cb *cb)
{
	int ret;

	if (cb->sin.ss_family == AF_INET)
		((struct sockaddr_in *)&cb->sin)->sin_port = cb->port;
	else
		((struct sockaddr_in6 *)&cb->sin)->sin6_port = cb->port;

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&cb->sin);
	if (ret) {
		fprintf(stderr, "rdma_bind_addr failed.");
		return ret;
	}
	log_info("rdma_bind_addr successful");

	log_info("rdma_listen");
	ret = rdma_listen(cb->cm_id, 3); // FIXME: Proper backlog value?
	if (ret) {
		fprintf(stderr, "rdma_listen failed.");
		return ret;
	}

	return 0;
}

static struct rdma_ch_cb *clone_cb(struct rdma_ch_cb *listening_cb)
{
	struct rdma_ch_cb *cb;
	int ret, i;

	cb = calloc(1, sizeof *cb);
	if (!cb)
		return NULL;

	*cb = *listening_cb; // shallow copy.
	cb->child_cm_id->context = cb;

	// Alloc and init buf_ctxs.
	ret = init_buf_ctxs(cb);
	if (ret < 0) {
		free(cb);
		return NULL;
	}

	// To pass remote_mr_info.
	// TODO: We need to change it for multi-client support.
	for (i = 0; i < cb->databuf_cnt; i++) {
		cb->buf_ctxs[i] = listening_cb->buf_ctxs[i]; //shallow copy.
	}

	log_debug("Cloning CB:");
	log_debug("parent_cb=%lx", listening_cb);
	log_debug("parent_cb->child_cm_id=%lx", listening_cb->child_cm_id);
	log_debug("parent_cb->child_cm_id->context=%lx",
		  listening_cb->child_cm_id->context);
	log_debug("parent cb->buf_ctxs=%lx", listening_cb->buf_ctxs);
	log_debug("cloned_cb=%lx", cb);
	log_debug("cloned_cb->child_cm_id=%lx", cb->child_cm_id);
	log_debug("cloned_cb->child_cm_id->context=%lx",
		  cb->child_cm_id->context);
	log_debug("cloned cb->buf_ctxs=%lx", cb->buf_ctxs);

	return cb;
}

static int create_qp(struct rdma_ch_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));

	// TODO: Check configuration.
	init_attr.cap.max_send_wr = RDMA_SQ_DEPTH;
	init_attr.cap.max_recv_wr = RDMA_RQ_DEPTH;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static int setup_qp(struct rdma_ch_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;

	cb->pd = ibv_alloc_pd(cm_id->verbs);
	if (!cb->pd) {
		fprintf(stderr, "ibv_alloc_pd failed\n");
		return errno;
	}
	log_debug("created pd %p", cb->pd);

	cb->channel = ibv_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		fprintf(stderr, "ibv_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	log_debug("created channel %p", cb->channel);

	cb->cq = ibv_create_cq(cm_id->verbs, RDMA_SQ_DEPTH * 2, cb, cb->channel,
			       0);
	if (!cb->cq) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto err2;
	}
	log_debug("created cq %p", cb->cq);

	ret = ibv_req_notify_cq(cb->cq, 0);
	if (ret) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto err3;
	}

	ret = create_qp(cb);
	if (ret) {
		fprintf(stderr, "rdma_create_qp");
		goto err3;
	}
	log_debug("created qp %p", cb->qp);
	return 0;

err3:
	ibv_destroy_cq(cb->cq);
err2:
	ibv_destroy_comp_channel(cb->channel);
err1:
	ibv_dealloc_pd(cb->pd);
	return ret;
}

static void setup_wr(struct rdma_ch_cb *cb)
{
	struct databuf_ctx *db_ctx;
	int i;

	for (i = 0; i < cb->databuf_cnt; i++) {
		db_ctx = &cb->buf_ctxs[i];

		db_ctx->rdma_sgl.addr =
			(uint64_t)(unsigned long)db_ctx->rdma_buf;
		db_ctx->rdma_sgl.lkey = db_ctx->rdma_mr->lkey;

		db_ctx->rdma_sq_wr.wr_id = i;
		db_ctx->rdma_sq_wr.opcode = IBV_WR_RDMA_READ;
		db_ctx->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
		db_ctx->rdma_sq_wr.sg_list = &db_ctx->rdma_sgl;
		db_ctx->rdma_sq_wr.num_sge = 1;
	}
}

static int alloc_rdma_buffers(struct rdma_ch_cb *cb)
{
	struct databuf_ctx *db_ctx;
	int i, ret;
	char *buf;

	// Alloc one big region.
	ret = posix_memalign((void **)&buf, sysconf(_SC_PAGESIZE),
			     cb->databuf_size * cb->databuf_cnt);

	if (ret != 0) {
		fprintf(stderr,
			"Allocating message buffer (rdma) failed. Error code=%d\n",
			ret);
		return -ENOMEM;
	}

	// Set rdma buffer pointers.
	for (i = 0; i < cb->databuf_cnt; i++) {
		db_ctx = &cb->buf_ctxs[i];
		db_ctx->rdma_buf = buf + cb->databuf_size * i;

		log_debug("Allocated buf_ctxs[%d]->rdma_buf=0x%lx", i,
			  db_ctx->rdma_buf);
	}

	return 0;
}

static int setup_buffers(struct rdma_ch_cb *cb)
{
	int ret, i;
	struct databuf_ctx *db_ctx;

	log_debug("setup_buffers called on cb %p", cb);

	// Malloc buffers.
	ret = alloc_rdma_buffers(cb);
	if (ret) {
		log_error("Failed to alloc msg buffers.");
		goto err2;
	}

#ifdef PER_BUF_MR
	// Register buffers as separate MRs.
	for (i = 0; i < cb->databuf_cnt; i++) {
		db_ctx = &cb->buf_ctxs[i];

		db_ctx->rdma_mr =
			ibv_reg_mr(cb->pd, db_ctx->rdma_buf, cb->databuf_size,
				   IBV_ACCESS_LOCAL_WRITE |
					   IBV_ACCESS_REMOTE_READ |
					   IBV_ACCESS_REMOTE_WRITE);
		if (!db_ctx->rdma_mr) {
			log_error("rdma_buf reg_mr failed");
			ret = errno;
			goto err1;
		}
	}
#else
	// Register as a one MR.
	db_ctx = &cb->buf_ctxs[0];

	db_ctx->rdma_mr =
		ibv_reg_mr(cb->pd, db_ctx->rdma_buf,
			   cb->databuf_size * cb->databuf_cnt,
			   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
				   IBV_ACCESS_REMOTE_WRITE);
	if (!db_ctx->rdma_mr) {
		log_error("rdma_buf reg_mr failed");
		ret = errno;
		goto err1;
	}

	// Set all the rdma_mr of buffers to point the same MR.
	for (i = 1; i < cb->databuf_cnt; i++) {
		db_ctx = &cb->buf_ctxs[i];
		db_ctx->rdma_mr = cb->buf_ctxs[0].rdma_mr;
	}
#endif

	// Setup Work Request.
	setup_wr(cb);
	return 0;

err1:
	deregister_mrs(cb);
err2:
	free_buffers(cb);
	return ret;
}

static int cq_event_handler(struct rdma_ch_cb *cb)
{
	struct ibv_wc wc;
	// struct ibv_recv_wr *bad_wr;
	int ret;
	int flushed = 0;

	while ((ret = ibv_poll_cq(cb->cq, 1, &wc)) == 1) {
		ret = 0;

		if (wc.status) {
			if (wc.status == IBV_WC_WR_FLUSH_ERR) {
				flushed = 1;
				continue;
			}
			log_error("cq completion failed wc.status=%d",
				  wc.status);
			ret = -1;
			goto error;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
			log_debug("send completion");
			break;

		case IBV_WC_RDMA_WRITE:
			log_debug("rdma write completion");
			// cb->state = RDMA_WRITE_COMPLETE;
			// sem_post(&cb->sem);
			break;

		case IBV_WC_RDMA_READ:
			log_debug("rdma read completion");

			// Wake up waiting thread.
			notify_post_completion(&cb->buf_ctxs[wc.wr_id]);
			// cb->state = RDMA_READ_COMPLETE;
			// sem_post(&cb->sem);
			break;

		case IBV_WC_RECV:
			log_debug("recv completion\n");
			// ret = receive_msg(cb, &wc);
			// if (ret) {
			// 	log_error("recv wc error: ret=%d", ret);
			// 	goto error;
			// }

			// ret = ibv_post_recv(
			// 	cb->qp, &cb->buf_ctxs[wc.wr_id].rq_wr,
			// 	&bad_wr); // wc.wr_id stores msgbuf_id.
			// if (ret) {
			// 	log_error("post recv error: ret=%d", ret);
			// 	goto error;
			// }
			// sem_post(&cb->sem);
			break;

		default:
			log_error("unknown!!!!! completion");
			ret = -1;
			goto error;
		}
	}
	if (ret) {
		log_error("poll error %d", ret);
		goto error;
	}
	return flushed;

error:
	cb->state = ERROR;
	sem_post(&cb->sem);
	return ret;
}

static void *cq_thread(void *arg)
{
	struct rdma_ch_cb *cb = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret;

	log_debug("cq_thread started.");

	while (1) {
		pthread_testcancel();

		ret = ibv_get_cq_event(cb->channel, &ev_cq, &ev_ctx);
		if (ret) {
			fprintf(stderr, "Failed to get cq event!\n");
			pthread_exit(NULL);
		}
		if (ev_cq != cb->cq) {
			fprintf(stderr, "Unknown CQ!\n");
			pthread_exit(NULL);
		}
		ret = ibv_req_notify_cq(cb->cq, 0);
		if (ret) {
			fprintf(stderr, "Failed to set notify!\n");
			pthread_exit(NULL);
		}
		ret = cq_event_handler(cb);
		ibv_ack_cq_events(cb->cq, 1);
		if (ret)
			pthread_exit(NULL);
	}
}

static int do_accept(struct rdma_ch_cb *cb)
{
	int ret;

	log_debug("accepting client connection request");

	ret = rdma_accept(cb->child_cm_id, NULL);
	if (ret) {
		fprintf(stderr, "rdma_accept\n");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state == ERROR) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}
	return 0;
}

static void deregister_mrs(struct rdma_ch_cb *cb)
{
	int i;
	struct databuf_ctx *db_ctx;

#ifdef PER_BUF_MR
	for (i = 0; i < cb->databuf_cnt; i++) {
		db_ctx = &cb->buf_ctxs[i];
		if (db_ctx->rdma_mr)
			ibv_dereg_mr(db_ctx->rdma_mr);
	}
#else
	if (cb->buf_ctxs[0].rdma_mr)
		ibv_dereg_mr(cb->buf_ctxs[0].rdma_mr);

	for (i = 0; i < cb->databuf_cnt; i++) {
		db_ctx = &cb->buf_ctxs[i];
		db_ctx->rdma_mr = NULL;
	}
#endif
}

static void free_buffers(struct rdma_ch_cb *cb)
{
	// Free the big region.
	log_debug("free buf_ctxs[0].rdma_buf=%lx", cb->buf_ctxs[0].rdma_buf);
	free(cb->buf_ctxs[0].rdma_buf);
}

static void free_qp(struct rdma_ch_cb *cb)
{
	ibv_destroy_qp(cb->qp);
	ibv_destroy_cq(cb->cq);
	ibv_destroy_comp_channel(cb->channel);
	ibv_dealloc_pd(cb->pd);
}

static void free_cb(struct rdma_ch_cb *cb)
{
	log_debug("free cb->buf_ctxs=%lx", cb->buf_ctxs);
	free_buf_ctxs(cb);
	log_debug("free cb=%lx", cb);
	free(cb);
}

// Per client connetion thread.
static void *server_thread(void *arg)
{
	struct rdma_ch_cb *cb = arg;
	// struct ibv_recv_wr *bad_wr;
	// struct databuf_ctx *mb_ctx;
	// int i;
	int ret;

	ret = setup_qp(cb, cb->child_cm_id);
	if (ret) {
		fprintf(stderr, "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = setup_buffers(cb);
	if (ret) {
		fprintf(stderr, "setup_buffers failed: %d\n", ret);
		goto err1;
	}

	// Post recv WR for all the data buffers.
	// for (i = 0; i < cb->databuf_cnt; i++) {
	// 	mb_ctx = &cb->buf_ctxs[i];

	// 	ret = ibv_post_recv(cb->qp, &mb_ctx->rq_wr, &bad_wr);
	// 	if (ret) {
	// 		fprintf(stderr, "ibv_post_recv failed: %d\n", ret);
	// 		goto err2;
	// 	}
	// }

	ret = pthread_create(&cb->cqthread, NULL, cq_thread, cb);
	if (ret) {
		fprintf(stderr, "pthread_create\n");
		goto err2;
	}

	ret = do_accept(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		goto err3;
	}

	log_info("Client is connected.");

	while (cb->state != DISCONNECTED) {
		sleep(1);
	}

	log_info("Freeing resources allocated for the client.");

	rdma_disconnect(cb->child_cm_id);

	pthread_cancel(cb->cqthread);
	pthread_join(cb->cqthread, NULL);

	deregister_mrs(cb);
	free_buffers(cb);
	free_qp(cb);

	rdma_destroy_id(cb->child_cm_id);
	free_cb(cb);
	return NULL;
err3:
	pthread_cancel(cb->cqthread);
	pthread_join(cb->cqthread, NULL);
err2:
	deregister_mrs(cb);
	free_buffers(cb);
err1:
	free_qp(cb);
err0:
	free_cb(cb);
	return NULL;
}

// Per server thread.
void *run_df_server(void *arg)
{
	int ret;
	char res[50];
	struct rdma_ch_cb *child_cb;
	pthread_attr_t attr;
	struct rdma_ch_cb *listening_cb = (struct rdma_ch_cb *)arg;

	ret = bind_server(listening_cb);
	if (ret) {
		goto err;
	}

	/*
	 * Set persistent server threads to DEATCHED state so
	 * they release all their resources when they exit.
	 */
	ret = pthread_attr_init(&attr);
	if (ret) {
		fprintf(stderr, "pthread_attr_init failed.\n");
		goto err;
	}
	ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (ret) {
		fprintf(stderr, "pthread_attr_setdetachstate failed.\n");
		goto err;
	}

	log_info("Waiting a client...");

	while (1) {
		sem_wait(&listening_cb->sem);
		if (listening_cb->state != CONNECT_REQUEST) {
			fprintf(stderr,
				"Wait for CONNECT_REQUEST state but state is %d\n",
				listening_cb->state);
			ret = -1;
			goto err;
		}

		// CB for a new client. TODO: support multi-client.
		child_cb = clone_cb(listening_cb);
		if (!child_cb) {
			ret = -1;
			goto err;
		}

		log_debug("Cloning cb: listening_cb=0x%lx child_cb=0x%lx",
				listening_cb, child_cb);

		// A handler thread is created when there is a new connect request.
		ret = pthread_create(&child_cb->server_thread, &attr, server_thread,
				     child_cb);
		if (ret) {
			fprintf(stderr, "pthread_create failed.");
			goto err;
		}
	}

err:
	log_error("Failure in run_server(). ret=%d", ret);

	rdma_destroy_id(listening_cb->cm_id);
	rdma_destroy_event_channel(listening_cb->cm_channel);

	// return -1;

	sprintf(res, "Failure in run_server(). ret=%d", ret);
	pthread_exit(res);
}

static int bind_client(struct rdma_ch_cb *cb)
{
	int ret;

	if (cb->sin.ss_family == AF_INET)
		((struct sockaddr_in *)&cb->sin)->sin_port = cb->port;
	else
		((struct sockaddr_in6 *)&cb->sin)->sin6_port = cb->port;

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&cb->sin,
				2000);
	if (ret) {
		fprintf(stderr, "rdma_resolve_addr\n");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != ROUTE_RESOLVED) {
		fprintf(stderr, "waiting for addr/route resolution state %d\n",
			cb->state);
		return -1;
	}

	log_debug("rdma_resolve_addr - rdma_resolve_route successful");
	return 0;
}

static int connect_client(struct rdma_ch_cb *cb)
{
	struct rdma_conn_param conn_param;
	struct remote_mr_info *rm_info;
	int ret;
	int mr_num; // The number of MR info.

#ifdef PER_BUF_MR
	int i;

	mr_num = cb->databuf_cnt;
	rm_info = calloc(mr_num, sizeof(struct remote_mr_info));

	// Set local mr info that is passed to the server.
	for (i = 0; i < mr_num; i++) {
		rm_info[i].remote_rkey = htobe32(cb->buf_ctxs[i].rdma_mr->rkey);
		// TODO: We can send only the first buffer's base address.
		// Server can calculate the other buffer addresses with it.
		rm_info[i].remote_addr = htobe64(
			(uint64_t)(unsigned long)cb->buf_ctxs[i].rdma_buf);
	}
#else
	mr_num = 1;
	rm_info = calloc(1, sizeof(struct remote_mr_info));

	// Set local mr info that is passed to the server.
	rm_info->remote_rkey = htobe32(
		cb->buf_ctxs[0].rdma_mr->rkey); // All MRs are identical.

	// Send base address of buffer.
	// Server calculates the other buffer addresses with it.
	rm_info->remote_addr =
		htobe64((uint64_t)(unsigned long)cb->buf_ctxs[0].rdma_buf);

	log_debug("Client rkey=0x%x addr=0x%lx", cb->buf_ctxs[0].rdma_mr->rkey,
		  (uint64_t)(unsigned long)cb->buf_ctxs[0].rdma_buf);
#endif

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 7;
	conn_param.private_data = rm_info; // Delivered to server.
	conn_param.private_data_len = sizeof(struct remote_mr_info) * mr_num;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		fprintf(stderr, "rdma_connect failed.\n");
		goto err1;
	}

	sem_wait(&cb->sem);
	if (cb->state != CONNECTED) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		ret = -1;
		goto err1;
	}

	log_debug("rdma_connect successful");
	free(rm_info);
	return 0;

err1:
	free(rm_info);
	return ret;
}

static inline uint64_t alloc_seqn(struct databuf_ctx *db_ctx)
{
	uint64_t ret;

	ret = atomic_fetch_add(&db_ctx->seqn, 1);
	return ret;
}

// TODO: add client_id for multi-client support.
/**
 * @brief Post RDMA read request. (This function is used by server.)
 * 
 * @param server_cb  a.k.a. listening_cb
 * @param databuf_id 
 * @param length The length of data to read.
 * @return int 
 */
int df_post_rdma_read(struct rdma_ch_cb *server_cb, int databuf_id,
		      uint32_t length)
{
	struct rdma_ch_cb *cb;
	struct ibv_send_wr *bad_wr;
	struct ibv_send_wr *send_wr;
	int ret;

	// Get connection cb. TODO: multi-client support.
	cb = server_cb->child_cm_id->context;

	// FYI, send_wr == cb->buf_ctxs[databuf_id].rdma_sgl;
	send_wr = &cb->buf_ctxs[databuf_id].rdma_sq_wr;
	send_wr->sg_list[0].length = length;

	ret = ibv_post_send(cb->qp, send_wr, &bad_wr);
	if (ret) {
		log_error("Post READ error %d", ret);
		return -1;
	}

	// Wait for the post completion.
	// TODO: [OPTIMIZE] We can reduce scheduling overhead by implementing
	// another latency-critical channel.
	wait_post_completion(&cb->buf_ctxs[databuf_id]);
	log_debug("Post completed. Resume thread. databuf_id=%d", databuf_id);

	return 0;
}

static int run_df_client(struct rdma_ch_cb *cb)
{
	int ret;

	ret = bind_client(cb);
	if (ret)
		return ret;

	ret = setup_qp(cb, cb->cm_id);
	if (ret) {
		fprintf(stderr, "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = setup_buffers(cb);
	if (ret) {
		fprintf(stderr, "setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = pthread_create(&cb->cqthread, NULL, cq_thread, cb);
	if (ret) {
		fprintf(stderr, "pthread_create\n");
		goto err2;
	}

	ret = connect_client(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		goto err3;
	}

	return 0;

err3:
	pthread_join(cb->cqthread, NULL);
err2:
	deregister_mrs(cb);
	free_buffers(cb);
err1:
	free_qp(cb);

	return ret;
}

static int get_addr(char *dst, struct sockaddr *addr)
{
	struct addrinfo *res;
	int ret;

	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		printf("getaddrinfo failed (%s) - invalid hostname or IP address\n",
		       gai_strerror(ret));
		return ret;
	}

	if (res->ai_family == PF_INET)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	else if (res->ai_family == PF_INET6)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
	else
		ret = -1;

	freeaddrinfo(res);
	return ret;
}

// Allocate and initialize buffer contexts.
static int init_buf_ctxs(struct rdma_ch_cb *cb)
{
	struct databuf_ctx *db_ctx;
	int i, ret;

	// Alloc.
	cb->buf_ctxs = calloc(cb->databuf_cnt, sizeof(struct databuf_ctx));
	if (!cb->buf_ctxs) {
		ret = -ENOMEM;
		log_error("calloc failed.");
		return -ret;
	}

	// Some initialization.
	for (i = 0; i < cb->databuf_cnt; i++) {
		db_ctx = &cb->buf_ctxs[i];
		db_ctx->id = i; // Set ID.
		atomic_init(&db_ctx->seqn, 1); // Start from 1.
#ifdef SYNC_WITHOUT_SLEEP
		pthread_spin_init(&db_ctx->db_lock, PTHREAD_PROCESS_PRIVATE);
#else
		sem_init(&db_ctx->db_sem, 0, 0);
#endif
	}

	return 0;
}

static void free_buf_ctxs(struct rdma_ch_cb *cb)
{
	free(cb->buf_ctxs);
}

/**
 * @brief Init rdma channel. Client and Server should pass the same
 * `databuf_cnt` and `databuf_size`.
 * 
 * @param attr Attributes for an RDMA connection.
 * @return struct rdma_ch_cb* Returns rdma_ch_cb pointer. Return NULL on failure.
 */
struct rdma_ch_cb *df_init_rdma_ch(struct rdma_ch_attr *attr)
{
	struct rdma_ch_cb *cb;
	int ret;

	cb = calloc(1, sizeof(struct rdma_ch_cb));
	if (!cb) {
		ret = -ENOMEM;
		goto out4;
	}

	cb->databuf_cnt = attr->databuf_cnt;
	cb->databuf_size = attr->databuf_size;
	cb->server = attr->server;
	cb->state = IDLE;
	cb->sin.ss_family = AF_INET;
	cb->port = htobe16(attr->port);
	sem_init(&cb->sem, 0, 0); // Used in CM.

	// Server's listening cb also allocates buf_ctxs to store remote_mr_info temporarily.
	ret = init_buf_ctxs(cb);
	if (ret < 0)
		goto out3;

	if (!cb->server) {
		ret = get_addr(attr->ip_addr, (struct sockaddr *)&cb->sin);
		if (ret)
			goto out3;
	}

	cb->cm_channel = create_first_event_channel();
	if (!cb->cm_channel) {
		ret = errno;
		goto out3;
	}

	ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);

	if (ret) {
		printf("rdma_create_id failed.\n");
		goto out2;
	}

	ret = pthread_create(&cb->cmthread, NULL, df_cm_thread, cb);
	if (ret) {
		printf("Creating cm_thread failed.\n");
		goto out1;
	}

	if (cb->server) {
		ret = pthread_create(&cb->server_daemon, NULL, run_df_server,
				     cb);
		if (ret) {
			printf("Creating server daemon failed.\n");
			goto out1;
		}
	} else
		run_df_client(cb);

	return cb;

out1:
	rdma_destroy_id(cb->cm_id);
out2:
	rdma_destroy_event_channel(cb->cm_channel);
out3:
	free_cb(cb);
out4:
	printf("init_rdma_ch failed. ret=%d\n", ret);
	return NULL;
}

/**
 * @brief Called by client.
 * 
 * @param cb 
 */
void df_destroy_rdma_client(struct rdma_ch_cb *cb)
{
	if (cb->server) {
		log_warn("destroy_rdma_ch() is for client.");
		return;
	}

	rdma_disconnect(cb->cm_id);
	pthread_join(cb->cqthread, NULL);
	deregister_mrs(cb);
	free_buffers(cb);
	free_qp(cb);

	rdma_destroy_id(cb->cm_id);
	rdma_destroy_event_channel(cb->cm_channel);
	free_cb(cb);
}

void df_destroy_rdma_server(struct rdma_ch_cb *listening_cb)
{
	struct rdma_ch_cb *cb;

	// destroy per client.
	cb = listening_cb->child_cm_id->context;
	rdma_disconnect(cb->child_cm_id);
	pthread_join(cb->cqthread, NULL);
	pthread_join(cb->server_thread, NULL);
	free_buffers(cb);
	deregister_mrs(cb);
	free_qp(cb);

	// destroy per server.
	rdma_disconnect(listening_cb->cm_id);
	pthread_join(listening_cb->cmthread, NULL);
	pthread_join(listening_cb->server_daemon, NULL);

	// Free cb.
	free_cb(cb); // per client.
	free_cb(listening_cb); // per server.
}
