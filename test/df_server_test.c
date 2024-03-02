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

int main()
{
	int ret;
	int buf_id;
	struct data_fetcher_ctx *df_ctx;

	// run server.
	ret = init_df_server(7175, DATABUF_SIZE, DATABUF_CNT, &df_ctx);

	if (ret) {
		log_error("init df server failed. ret=%d", ret);
		return -1;
	}

	while (1) {
		buf_id = get_user_input();
		if (buf_id < 0 || buf_id >= (int)DATABUF_CNT) {
			printf("Please enter a number N where 0 < N < %d.\n",
			       (int)DATABUF_CNT);
			continue;
		}
		fetch_and_print_data(df_ctx, buf_id, DATABUF_SIZE);
	}

	log_info("Exiting server.");
	return 0;
}
