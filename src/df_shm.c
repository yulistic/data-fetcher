#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "df_shm.h"
#include "log.h"

void *df_init_shm_ch(const char *shm_name, uint64_t databuf_size, int databuf_cnt, int is_server)
{
    struct shm_ch_cb *cb;
    size_t total_size;

    cb = calloc(1, sizeof(*cb));
    if (!cb) {
        log_error("Failed to allocate shm control block");
        return NULL;
    }

    cb->databuf_size = databuf_size;
    cb->databuf_cnt = databuf_cnt;
    total_size = databuf_size * databuf_cnt;
    cb->shm_size = total_size;
    
    strncpy(cb->shm_name, shm_name, sizeof(cb->shm_name) - 1);

    if (is_server) {
        // Server creates and initializes shared memory
        shm_unlink(cb->shm_name); // Remove any existing shm
        cb->shm_fd = shm_open(cb->shm_name, O_CREAT | O_RDWR, 0666);
    } else {
        // Client opens existing shared memory
        cb->shm_fd = shm_open(cb->shm_name, O_RDWR, 0666);
    }

    if (cb->shm_fd < 0) {
        log_error("Failed to open shared memory");
        goto err_free;
    }

    if (is_server) {
        if (ftruncate(cb->shm_fd, total_size) < 0) {
            log_error("Failed to set shared memory size");
            goto err_close;
        }
    }

    cb->shm_base = mmap(NULL, total_size, PROT_READ | PROT_WRITE, 
                        MAP_SHARED, cb->shm_fd, 0);
    if (cb->shm_base == MAP_FAILED) {
        log_error("Failed to map shared memory");
        goto err_close;
    }

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
