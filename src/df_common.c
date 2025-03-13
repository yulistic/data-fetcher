#include <stdio.h>
#include "global.h"
#include "bit_array.h"
#include "data_fetcher.h"
#include "df_rdma.h"
#include "log.h"
#include "df_shm.h"

/**
 * @brief Get the pointer of RDMA buffer. You can access this buffer until you
 * free it by calling `free_buffer()`.
 * 
 * @param df_ctx 
 * @param buf_id 
 * @return void* Buffer pointer.
 */
void *get_buffer(struct data_fetcher_ctx *df_ctx, int buf_id)
{
	if (df_ctx->transport == DF_TRANSPORT_RDMA) {
		struct rdma_ch_cb *conn_cb, *cb;
		
		cb = df_ctx->ch_cb;
		if (cb->server) {
			conn_cb = cb->child_cm_id->context;
		} else {
			conn_cb = cb;
		}
		return conn_cb->buf_ctxs[buf_id].rdma_buf;
	} else { // DF_TRANSPORT_SHM
		return get_shm_buffer(df_ctx->shm_cb, buf_id);
	}
}
