#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "global.h"
#include "log.h"
#include "df_rdma.h"
#include "bit_array.h"
#include "data_fetcher.h"
#include "df_shm.h"

/**
 * @brief Initialize Data Fetcher server.
 * 
 * @param port 
 * @param databuf_size The maximum size of a msg data in byte.
 * @param databuf_cnt 
 * @param df_ctx_p A Data Fetcher context is returned.
 * @return 0 on success.
 */
int init_df_server(int port, uint64_t databuf_size, int databuf_cnt,
		   struct data_fetcher_ctx **df_ctx_p)
{
	struct rdma_ch_attr rdma_attr;
	struct data_fetcher_ctx *df_ctx;
	// int ret;

	rdma_attr = (struct rdma_ch_attr){ .server = 1,
					   .databuf_size = databuf_size,
					   .databuf_cnt = databuf_cnt,
					   .port = port };

	df_ctx = calloc(1, sizeof(struct data_fetcher_ctx));
	if (!df_ctx) {
		log_error("Memory allocation failed.");
		return -1;
	}

	df_ctx->ch_cb = df_init_rdma_ch(&rdma_attr);
	if (!df_ctx->ch_cb) {
		log_error("Failed to initialize RDMA channel.");
		goto err1;
	}

	log_info("Data fetcher server initialized.");

	// Return df context.
	*df_ctx_p = df_ctx;

	return 0;

err1:
	free(df_ctx);
	return -1;
}

/**
 * @brief Fetch data from client via RDMA read or shared memory. 
 * 
 * @param df_ctx 
 * @param buf_id 
 * @param length The length of data to fetch.
 * @return char* The address of the buffer. NULL if fetch failed.
 */
char *fetch_data(struct data_fetcher_ctx *df_ctx, int buf_id, uint32_t length)
{
	// Check the length does not exceed the uint32_t.
	if (length > UINT32_MAX) {
		log_error("Length exceeds uint32_t.");
		return NULL;
	}

	if (df_ctx->transport == DF_TRANSPORT_RDMA) {
		int ret = df_post_rdma_read(df_ctx->ch_cb, buf_id, length);
		if (ret < 0) {
			log_error("RDMA read failed.");
			return NULL;
		}
	} else { // DF_TRANSPORT_SHM
		// For shared memory, we can directly access the buffer
		// No need to fetch since memory is already shared
		log_debug("Using shared memory - direct access");
	}

	return get_buffer(df_ctx, buf_id);
}

void destroy_df_server(struct data_fetcher_ctx *df_ctx)
{
	if (df_ctx->transport == DF_TRANSPORT_RDMA) {
		df_destroy_rdma_server(df_ctx->ch_cb);
	} else { // DF_TRANSPORT_SHM
		struct shm_ch_cb *shm_cb = df_ctx->shm_cb;
		df_destroy_shm_ch(df_ctx->shm_cb);
		shm_unlink(shm_cb->shm_name); // Server should remove the shared memory
	}

	free(df_ctx);
}

int init_df_server_shm(const char *shm_name, uint64_t databuf_size, int databuf_cnt,
                      struct data_fetcher_ctx **df_ctx_p)
{
    struct data_fetcher_ctx *df_ctx;

    df_ctx = calloc(1, sizeof(*df_ctx));
    if (!df_ctx) {
        log_error("Memory allocation failed.");
        return -1;
    }

    df_ctx->shm_cb = df_init_shm_ch(shm_name, databuf_size, databuf_cnt, 1);
    if (!df_ctx->shm_cb) {
        log_error("Failed to initialize shared memory channel");
        goto err1;
    }

    df_ctx->transport = DF_TRANSPORT_SHM;
    *df_ctx_p = df_ctx;
    return 0;

err1:
    free(df_ctx);
    return -1;
}
