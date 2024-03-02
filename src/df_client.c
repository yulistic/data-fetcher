#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "log.h"
#include "df_rdma.h"
#include "data_fetcher.h"
#include "bit_array.h"

static int init_databuf_bitmap(struct data_fetcher_ctx *df_ctx, int databuf_cnt)
{
	df_ctx->buf_bitmap.map = bit_array_create(databuf_cnt);
	if (!df_ctx->buf_bitmap.map) {
		log_error("Failed to allocate a bitmap.");
		return -1;
	}

	pthread_spin_init(&df_ctx->buf_bitmap.lock, PTHREAD_PROCESS_PRIVATE);

	// Print for test.
	printf("Message buffer bitmaps: ");
	bit_array_print(df_ctx->buf_bitmap.map, stdout);
	fputc('\n', stdout);

	return 0;
}

/**
 * @brief Initialize Data Fetcher client. It registers RDMA MRs.
 * 
 * @param target Server ip_addr for RDMA connection.
 * @param port 
 * @param databuf_size The maximum size of a msg data in byte.
 * @param databuf_cnt 
 * @param df_ctx_p Data Fetcher context is returned.
 * @return int 0 on success, -1 on error.
 */
int init_df_client(char *target, int port, uint64_t databuf_size,
		   int databuf_cnt, struct data_fetcher_ctx **df_ctx_p)
{
	struct rdma_ch_attr rdma_attr;
	struct data_fetcher_ctx *df_ctx;
	int ret;

	rdma_attr = (struct rdma_ch_attr){
		.server = 0,
		.databuf_size = databuf_size,
		.databuf_cnt = databuf_cnt,
		.port = port,
	};

	strcpy(rdma_attr.ip_addr, target);

	df_ctx = calloc(1, sizeof(struct data_fetcher_ctx));
	if (!df_ctx) {
		log_error("Memory allocation failed.");
		return -1;
	}

	ret = init_databuf_bitmap(df_ctx, databuf_cnt);
	if (ret < 0) {
		log_error("Failed to init databuf bitmap.");
		goto err1;
	}

	df_ctx->ch_cb = df_init_rdma_ch(&rdma_attr);
	if (!df_ctx->ch_cb) {
		log_error("Failed to initialize RDMA channel.");
		goto err1;
	}

	log_info("Data fetcher client initialized.");

	// Return df context.
	*df_ctx_p = df_ctx;

	return 0;

err1:
	free(df_ctx);
	return -1;
}

void destroy_df_client(struct data_fetcher_ctx *df_ctx)
{
	df_destroy_rdma_client(df_ctx->ch_cb);
	free(df_ctx);
}

static void lock_databuf(struct data_fetcher_ctx *df_ctx)
{
	pthread_spin_lock(&df_ctx->buf_bitmap.lock);
}

static void unlock_databuf(struct data_fetcher_ctx *df_ctx)
{
	pthread_spin_unlock(&df_ctx->buf_bitmap.lock);
}

// TODO: Need to profile this lock contention.
static uint64_t alloc_databuf_id(struct data_fetcher_ctx *df_ctx)
{
	uint64_t bit_id;
	int ret;

	ret = 0;
	while (1) {
		lock_databuf(df_ctx);
		ret = bit_array_find_first_clear_bit(
			(const BIT_ARRAY *)df_ctx->buf_bitmap.map, &bit_id);
		if (ret)
			bit_array_set_bit((BIT_ARRAY *)df_ctx->buf_bitmap.map,
					  bit_id);
		unlock_databuf(df_ctx);

		if (ret)
			break;
		else
			log_info("Failed to alloc a databuf id.\n");
	}

	return bit_id;
}

/**
 * @brief Set the RDMA buffer. Server will fetch it with an RDMA read operation.
 * 
 * @param df_ctx 
 * @param data Data to be copied to data buffer.
 * @param length The length of copied data. It should be less than `databuf_size`.
 * @return int Allocated buf_id. It needs to be delivered to Server.
 */
int df_set_buffer(struct data_fetcher_ctx *df_ctx, char *data, uint64_t length)
{
	char *rdma_buf;
	int buf_id;
	struct rdma_ch_cb *ch_cb;

	ch_cb = (struct rdma_ch_cb *)df_ctx->ch_cb;

	buf_id = alloc_databuf_id(df_ctx);
	rdma_buf = get_buffer(df_ctx, buf_id);

	assert(length <= ch_cb->databuf_size);

	memcpy(rdma_buf, data, length);

	return buf_id;
}

/**
  * @brief Allocated buf_id and get the Data Fetcher's RDMA buffer.
  * Caller should check the size of data when filling this buffer. Use
  * df_buf_size() function.
  * 
  * @param df_ctx 
  * @param buf_p Data buffer pointer is passed.
  * @return int buf_id
  */
int df_alloc_buffer(struct data_fetcher_ctx *df_ctx, char **buf_p)
{
	int buf_id;

	buf_id = alloc_databuf_id(df_ctx);
	*buf_p = get_buffer(df_ctx, buf_id);

	return buf_id;
}

/**
 * @brief Returns the RDMA buffer size.
 * 
 * @param df_ctx 
 * @return uint64_t 
 */
uint64_t df_buf_size(struct data_fetcher_ctx *df_ctx)
{
	struct rdma_ch_cb *ch_cb;

	ch_cb = (struct rdma_ch_cb *)df_ctx->ch_cb;
	return ch_cb->databuf_size;
}

static void free_databuf_id(struct data_fetcher_ctx *df_ctx, uint64_t bit_id)
{
	lock_databuf(df_ctx);
	bit_array_clear_bit(df_ctx->buf_bitmap.map, bit_id);
	unlock_databuf(df_ctx);
}

/**
 * @brief Free allocated data buffer.
 *
 * @param df_ctx
 * @param buf_id Data buffer id.
 */
void df_free_buffer(struct data_fetcher_ctx *df_ctx, int buf_id)
{
	// Free buffer.
	free_databuf_id(df_ctx, buf_id);
}