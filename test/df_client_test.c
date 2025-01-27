#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "test_global.h"
#include "log.h"
#include "data_fetcher.h"

void test_rdma_client(void)
{
	char *ip_addr = "192.168.14.113";
	char *str = "Hello world. I'm a Data Fetcher client.";
	char data[DATABUF_CNT][255];
	struct data_fetcher_ctx *df_ctx = NULL;
	int buf_id, i, ret;
	int len;

	ret = init_df_client(ip_addr, 7175, DATABUF_SIZE, DATABUF_CNT, &df_ctx);
	if (ret < 0) {
		log_error("Failed to init client. ret=%d", ret);
		return;
	}
	log_info("Client is connected to server.");

	if (df_ctx == NULL) {
		printf("init_df_client failed.\n");
		return;
	}

	for (i = 0; i < DATABUF_CNT; i++) {
		len = sprintf(&data[i][0], "%s It is buffer %d.", str, i);
		len++; // Including null character.

		// Write to buffer.
		buf_id = df_set_buffer(df_ctx, &data[i][0], len);
		printf("Buffer is ready. buf_id=%d len=%d\n", buf_id, len);
		sleep(1);
	}

	log_info("All buffers are ready.\n");

	// Wait user input.
	printf("Press enter to free buffers.\n");
	getchar();

	// Free buffers.
	for (i = 0; i < DATABUF_CNT; i++) {
		df_free_buffer(df_ctx, i);
	}

	destroy_df_client(df_ctx);
}

void test_shm_client(void)
{
	char *str = "Hello world from shared memory client.";
	char data[DATABUF_CNT][255];
	struct data_fetcher_ctx *df_ctx = NULL;
	int buf_id, i, ret;
	int len;

	ret = init_df_client_shm("/df_test_shm", DATABUF_SIZE, DATABUF_CNT, &df_ctx);
	if (ret < 0) {
		log_error("Failed to init shm client. ret=%d", ret);
		return;
	}
	log_info("Shared memory client initialized.");

	if (df_ctx == NULL) {
		printf("init_df_client_shm failed.\n");
		return;
	}

	for (i = 0; i < DATABUF_CNT; i++) {
		len = sprintf(&data[i][0], "%s It is buffer %d.", str, i);
		len++; // Including null character.

		// Write to buffer.
		buf_id = df_set_buffer(df_ctx, &data[i][0], len);
		printf("Buffer is ready. buf_id=%d len=%d\n", buf_id, len);
		sleep(1);
	}

	log_info("All buffers are ready.\n");

	// Wait user input.
	printf("Press enter to free buffers.\n");
	getchar();

	// Free buffers.
	for (i = 0; i < DATABUF_CNT; i++) {
		df_free_buffer(df_ctx, i);
	}

	destroy_df_client(df_ctx);
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		printf("Usage: %s <rdma|shm>\n", argv[0]);
		return -1;
	}

	if (strcmp(argv[1], "rdma") == 0) {
		test_rdma_client();
	} else if (strcmp(argv[1], "shm") == 0) {
		test_shm_client();
	} else {
		printf("Invalid transport type. Use 'rdma' or 'shm'\n");
		return -1;
	}

	return 0;
}