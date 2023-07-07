#ifndef _DATA_FETCHER_H_
#define _DATA_FETCHER_H_

#include <stdint.h>
#include <pthread.h>

struct databuf_bitmap {
	void *map; // BIT ARRAY;
	pthread_spinlock_t lock; // databuf bitmap lock.
};

struct data_fetcher_ctx {
	void *ch_cb; // RDMA Channel control block. (server: listening cb, client: connect cb)
	struct databuf_bitmap buf_bitmap;
};

struct data_fetch_param {
	struct data_fetcher_ctx *df_ctx;
	int databuf_id; // Allocated data buffer id.
};

/* Server-side API */
int init_df_server(int port, uint64_t databuf_size, int databuf_cnt,
		   struct data_fetcher_ctx **df_ctx_p);
void destroy_df_server(struct data_fetcher_ctx *df_ctx);
char *fetch_data(struct data_fetcher_ctx *df_ctx, int buf_id, uint32_t length);

/* Client-side API */
int init_df_client(char *target, int port, uint64_t databuf_size,
		   int databuf_cnt, struct data_fetcher_ctx **df_ctx_p);
void destroy_df_client(struct data_fetcher_ctx *df_ctx);
int set_buffer(struct data_fetcher_ctx *df_ctx, char *data, uint64_t length);
void free_buffer(struct data_fetcher_ctx *df_ctx, int buf_id);

/* Library internal use */
void *get_buffer(struct data_fetcher_ctx *df_ctx, int buf_id);
#endif