#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "df_shm.h"
#include "log.h"

#define ALIGN_SIZE (4096UL * 512) // 2MB page size

/**
 * @brief 
 * 
 * @param shm_name Hugepages are mounted at this path by spdk scripts.
 * @param databuf_size 
 * @param databuf_cnt 
 * @param is_server 
 * @param offset Offset of the mapped region. (An offset in the hugepage file.)
 * @return void* 
 */
void *df_init_shm_ch(const char *shm_name, uint64_t databuf_size,
		     int databuf_cnt, int is_server, off_t offset)
{
	struct shm_ch_cb *cb;
	size_t total_size;
	size_t aligned_size;
	int ret;

	cb = calloc(1, sizeof(*cb));
	if (!cb) {
		log_error("Failed to allocate shm control block");
		return NULL;
	}

	cb->databuf_size = databuf_size;
	cb->databuf_cnt = databuf_cnt;
	total_size = databuf_size * databuf_cnt;
	// Align total_size to 2MB boundary
	aligned_size = (total_size + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);
	cb->shm_size = aligned_size;

	strncpy(cb->shm_name, shm_name, sizeof(cb->shm_name) - 1);

	cb->shm_fd = open(shm_name, O_RDWR | O_CREAT, 0666);

	if (cb->shm_fd < 0) {
		log_error("Failed to open shared memory");
		goto err_free;
	}

	ret = ftruncate(cb->shm_fd, aligned_size + offset);
	if (ret < 0) {
		log_error("Failed to set shared memory size");
		goto err_close;
	}

	cb->shm_base = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_HUGETLB, cb->shm_fd, offset);
	if (cb->shm_base == MAP_FAILED) {
		log_error("Failed to map shared memory");
		goto err_close;
	}

	// log_debug("Shared memory mapped at %p, size=%zu", cb->shm_base,
	// 	  cb->shm_size);

	return cb;

err_close:
	close(cb->shm_fd);
	if (is_server)
		shm_unlink(cb->shm_name);
err_free:
	free(cb);
	return NULL;
}

void df_destroy_shm_ch(void *cb)
{
	struct shm_ch_cb *shm_cb = cb;

	munmap(shm_cb->shm_base, shm_cb->shm_size);
	close(shm_cb->shm_fd);
	free(shm_cb);
}

void *get_shm_buffer(void *cb, int buf_id)
{
	struct shm_ch_cb *shm_cb = cb;
	return (char *)shm_cb->shm_base + (buf_id * shm_cb->databuf_size);
}

void *get_shm_buf_base(void *cb)
{
	struct shm_ch_cb *shm_cb = cb;
	return shm_cb->shm_base;
}

size_t get_shm_buf_size(void *cb)
{
	struct shm_ch_cb *shm_cb = cb;
	return shm_cb->shm_size;
}
