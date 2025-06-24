#pragma once
#define SUPPORT_SPDK (0)
#if SUPPORT_SPDK
#include <spdk/stdinc.h>
#include <spdk/env.h>
#include <spdk/nvme.h>
#include <spdk/bdev.h>
#include <spdk/thread.h>
#include <spdk/event.h>
struct spdk_device_config {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *channel;
	void close (void) {
		if (channel) {
			spdk_put_io_channel(channel);
			channel = nullptr;
		}
		if (desc) {
			spdk_bdev_close(desc);
			desc = nullptr;
		}
		bdev = nullptr;
	}
};


void spdk_init(void) {
	struct spdk_env_opts opts;
	spdk_env_opts_init(&opts);
	if (spdk_env_init(&opts) < 0) {
		pr_info(g, "Failed to initialize SPDK env\n");
		return;
	}
	spdk_bdev_initialize(nullptr, nullptr, nullptr);
}

enum connect_rv spdk_bdev_connect(struct server_bdev *bdev) {
	struct bdev_info *info = &bdev->b.info;
	struct spdk_bdev *spdk_bdev = spdk_bdev_get_by_name(bdev->conf.conn.spdk_bdev_name);
	if (!spdk_bdev)
		return C_NO_DEVICE;
	int tc = spdk_bdev_open_ext(bdev->conf.conn.spdk_bdev_name, true, nullptr, nullptr, &bdev->spdk_dev.desc);
	if (rc)
		return C_NO_RESPONSE;
	bdev->spdk_dev.bdev = spdk_bdev;
	bdev->spdk_dev.channel = spdk_bdev_get_io_channel(bdev->spdk_dev.desc);
	info->bdev_descriptor = 1000000 + bdev->get_fd(); // Unique descriptor
	info->block_size = spdk_bdev_get_block_size(spdk_bdev);
	info->num_total_blocks = spdk_bdev_get_num_blocks(spdk_bdev);
	strncpy(info->name, spdk_bdev_get_name(spdk_bdev), sizeof(info->name)-1);
	return C_OK;
}
#else
void spdk_init(void) { }
enum connect_rv spdk_bdev_connect(struct server_bdev *bdev) { (void)bdev; return C_OK; }

#endif	// SUPPORT_SPDK
