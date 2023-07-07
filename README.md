# Oxbow file system Data Fetcher module

## Source code format

Please enable auto formatting. Use `.clang-format` file.

## Load git submodules

```shell
git submodule update --init --recursive
```

## Install library

```shell
cd lib
./install.sh
```

## Build the project

```shell
./build.sh
```

## Limitations

- Only 1 client is supported. Currently, data fetch request is sent from the server. `rdma_ch_cb` is used to post an RDMA READ request and there is only one `rdma_ch_cb`, now. (cloned `rdma_ch_cb`) To support multiple clients, server needs to manage the list of clients and select which client's `rdma_ch_cb` is used.

> Q. Why does server do RDMA read?  
> A. The client will be ported as a kernel module and I want to make the kernel
> module be lightweight.
