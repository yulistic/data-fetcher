#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "test_global.h"
#include "log.h"
#include "data_fetcher.h"

void fetch_and_print_data(struct data_fetcher_ctx *df_ctx, int buf_id,
			  uint32_t length)
{
	char *buf;

	buf = fetch_data(df_ctx, buf_id, length);
	if (!buf) {
		log_error("Failed to fetch data.");
		return;
	}

	printf("Fetched from buffer(%d):", buf_id);
	printf("%s", buf);
	printf("\n");
}

int get_user_input(void)
{
	char *end = NULL;
	char buf[255];
	long n = 0;
	printf("Type the buf_id where client's data is stored:\n");
	while (fgets(buf, sizeof(buf), stdin)) {
		n = strtol(buf, &end, 10);
		if (end == buf || *end != '\n') {
			printf("Not an integer. Please enter an integer:\n");
		} else
			break;
	}
	printf("Entered buf_id: %ld\n", n);

	return n;
}

void test_rdma_server(void)
{
	int ret;
	int buf_id;
	struct data_fetcher_ctx *df_ctx;

	ret = init_df_server(7175, DATABUF_SIZE, DATABUF_CNT, &df_ctx);
	if (ret) {
		log_error("init df server failed. ret=%d", ret);
		return;
	}

	log_info("Server is ready. Waiting for client connection...");

	while (1) {
		buf_id = get_user_input();
		if (buf_id < 0 || buf_id >= DATABUF_CNT) {
			printf("Invalid buf_id. Enter 0-%d\n", DATABUF_CNT - 1);
			continue;
		}

		fetch_and_print_data(df_ctx, buf_id, DATABUF_SIZE);
	}

	destroy_df_server(df_ctx);
}

void test_shm_server(void)
{
	int ret;
	int buf_id;
	struct data_fetcher_ctx *df_ctx;

	ret = init_df_server_shm("/df_test_shm", DATABUF_SIZE, DATABUF_CNT, &df_ctx);
	if (ret) {
		log_error("init df server shm failed. ret=%d", ret);
		return;
	}

	log_info("Shared memory server is ready.");

	while (1) {
		buf_id = get_user_input();
		if (buf_id < 0 || buf_id >= DATABUF_CNT) {
			printf("Invalid buf_id. Enter 0-%d\n", DATABUF_CNT - 1);
			continue;
		}

		// For SHM, we can directly access the buffer without fetch_data
		char *buf = get_buffer(df_ctx, buf_id);
		printf("Read from buffer(%d):", buf_id);
		printf("%s", buf);
		printf("\n");
	}

	destroy_df_server(df_ctx);
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		printf("Usage: %s <rdma|shm>\n", argv[0]);
		return -1;
	}

	if (strcmp(argv[1], "rdma") == 0) {
		test_rdma_server();
	} else if (strcmp(argv[1], "shm") == 0) {
		test_shm_server();
	} else {
		printf("Invalid transport type. Use 'rdma' or 'shm'\n");
		return -1;
	}

	return 0;
}
