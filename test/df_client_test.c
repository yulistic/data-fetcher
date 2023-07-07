#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "test_global.h"
#include "log.h"
#include "data_fetcher.h"

int main()
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
		return -1;
	}
	log_info("Client is connected to server.");

	if (df_ctx == NULL) {
		printf("init_df_client failed.\n");
		return -1;
	}

	for (i = 0; i < DATABUF_CNT; i++) {
		len = sprintf(&data[i][0], "%s It is buffer %d.", str, i);
		len++; // Including null character.

		// Write to buffer.
		buf_id = set_buffer(df_ctx, &data[i][0], len);

		printf("Buffer is ready. buf_id=%d len=%d\n", buf_id, len);

		sleep(0.1);
	}

	log_info("All buffers are ready.\n");

	// Wait user input.
	printf("Press enter to free buffers.\n");
	getchar();

	// Free buffers.
	for (i = 0; i < DATABUF_CNT; i++) {
		free_buffer(df_ctx, i);
	}

	printf("Press Ctrl+c to exit.\n");
	while (1) {
		sleep(1);
	}

	destroy_df_client(df_ctx);

	printf("Bye.\n");
	return 0;
}