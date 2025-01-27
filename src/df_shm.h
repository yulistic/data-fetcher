#ifndef _DF_SHM_H_
#define _DF_SHM_H_

#include <stdint.h>
#include <sys/types.h>

struct shm_ch_cb {
    int shm_fd;           // Shared memory file descriptor
    void *shm_base;       // Base address of shared memory region
    size_t shm_size;      // Total size of shared memory
    uint64_t databuf_size; // Size of each data buffer
    int databuf_cnt;      // Number of data buffers
    char shm_name[256];   // Name of shared memory object
};

void *df_init_shm_ch(const char *shm_name, uint64_t databuf_size, int databuf_cnt, int is_server);
void df_destroy_shm_ch(void *cb);
void *get_shm_buffer(void *cb, int buf_id);

#endif 