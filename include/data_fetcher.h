#ifndef _DATA_FETCHER_H_
#define _DATA_FETCHER_H_

#include <stdint.h>
#include <pthread.h>

// Add after struct data_fetcher_ctx definition
enum df_transport {
    DF_TRANSPORT_RDMA,
    DF_TRANSPORT_SHM
};

struct databuf_bitmap {
	void *map; // BIT ARRAY;
	pthread_spinlock_t lock; // databuf bitmap lock.
	pthread_mutex_t cond_mutex;  // mutex for condition variable
	pthread_cond_t cond;         // condition variable for databuf availability
};

struct data_fetcher_ctx {
	void *ch_cb; // RDMA Channel control block. (server: listening cb, client: connect cb)
	void *shm_cb; // Shared memory control block 
	enum df_transport transport;
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
int df_set_buffer(struct data_fetcher_ctx *df_ctx, char *data, uint64_t length);
int df_alloc_buffer(struct data_fetcher_ctx *df_ctx, char **buf_p);
uint64_t df_buf_size(struct data_fetcher_ctx *df_ctx);
void df_free_buffer(struct data_fetcher_ctx *df_ctx, int buf_id);

/* Library internal use */
void *get_buffer(struct data_fetcher_ctx *df_ctx, int buf_id);

// Add new API for shared memory initialization
int init_df_server_shm(const char *shm_name, uint64_t databuf_size, int databuf_cnt,
                      struct data_fetcher_ctx **df_ctx_p);
int init_df_client_shm(const char *shm_name, uint64_t databuf_size, int databuf_cnt, 
                      struct data_fetcher_ctx **df_ctx_p);

#endif
