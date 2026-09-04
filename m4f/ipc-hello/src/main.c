/*
 * Copyright (c) 2026 Chrispine Tinega <dev@chrispinetinega.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * BL350 IPC demo — the Cortex-M4F half.
 *
 * Zephyr runs on the AM62x Cortex-M4F. Linux runs on the Cortex-A53 cores of
 * the same chip. They exchange plain text over RPMsg, carried by OpenAMP and
 * kicked by the MCU-domain mailbox.
 *
 * Linux is the OpenAMP *master*: it reads this firmware's resource table (the
 * .resource_table ELF section that CONFIG_OPENAMP_RSC_TABLE emits), allocates
 * the vrings out of the m4f-dma-memory carveout, writes their addresses back
 * into the resource table, and sets VIRTIO DRIVER_OK. This firmware waits for
 * DRIVER_OK, reads those addresses back out, brings up rpmsg and announces a
 * name-service endpoint called "bl350-demo".
 *
 * This is the samples/subsys/ipc/openamp_rsc_table pattern. Do NOT reach for
 * the ipc_service static-vrings backend instead: it computes vring addresses
 * from a fixed devicetree region and never reads the resource table, so it
 * only works Zephyr-to-Zephyr and cannot talk to a Linux master that allocates
 * the vrings dynamically.
 *
 * LINUX MUST SPEAK FIRST. rpmsg learns the A53's endpoint address from the
 * first inbound message; until one arrives this endpoint has no destination
 * and rpmsg_send() has nowhere to go. So the M4F only ever replies. Sending an
 * unsolicited greeting before that point does not fail loudly -- it is simply
 * dropped, which looks exactly like a broken link.
 *
 * There is no serial console on this board's M4F. Output goes to a RAM console
 * that Linux publishes at /sys/kernel/debug/remoteproc/<M4F>/trace0.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/ipm.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(ipc_hello, CONFIG_LOG_DEFAULT_LEVEL);

/* The name-service channel this firmware announces. Linux binds to it by name
 * (see udev/99-bl350-demo.rules), so the two sides must agree on this string. */
#define EPT_NAME "bl350-demo"

/* Shared memory for the vrings and rpmsg buffers: the Linux
 * m4f-dma-memory@9cb00000 carveout, which the board devicetree exposes as
 * chosen zephyr,ipc_shm. Identity-mapped from this core -- DDR needs no
 * address translation here, unlike peripheral config space. */
#define SHM_NODE       DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE       DT_REG_SIZE(SHM_NODE)

/* Mailbox used to kick the other core. chosen zephyr,ipc is the
 * zephyr,mbox-ipm node wrapping the AM62x MCU mailbox. */
static const struct device *const ipm_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_physmap;
static struct metal_io_region shm_io_data;
static struct metal_io_region rsc_io_data;
static struct metal_io_region *shm_io = &shm_io_data;
static struct metal_io_region *rsc_io = &rsc_io_data;

static struct rpmsg_virtio_device rvdev;
static struct rpmsg_device *rpdev;
static struct rpmsg_endpoint ept;
static void *rsc_table;

/* Raised by the mailbox RX interrupt so the management thread can drain the
 * virtqueues in thread context rather than in the ISR. */
static K_SEM_DEFINE(mbox_rx_sem, 0, 1);

static uint32_t messages_received;

/* --------------------------------------------------------------------------
 * rpmsg
 * -------------------------------------------------------------------------- */

static int rpmsg_recv_cb(struct rpmsg_endpoint *cb_ept, void *data, size_t len,
			 uint32_t src, void *priv)
{
	ARG_UNUSED(cb_ept);
	ARG_UNUSED(src);
	ARG_UNUSED(priv);

	char in[128];
	char reply[160];
	size_t copy = MIN(len, sizeof(in) - 1);
	int n;

	memcpy(in, data, copy);
	in[copy] = '\0';

	messages_received++;
	LOG_INF("A53 -> M4F: \"%s\"", in);

	/* "since boot" on purpose. This counter belongs to the firmware, which
	 * has been running since the board powered up and does not restart when
	 * the Linux program does. Calling it "message N" would make a second run
	 * of the Linux side look like a mismatched conversation. */
	n = snprintf(reply, sizeof(reply),
		     "Hello from Zephyr on the Cortex-M4F "
		     "(reply %u since boot, uptime %lld ms)",
		     messages_received, k_uptime_get());

	/* Safe to send from here: this callback runs on the management thread
	 * via rproc_virtio_notified(), not in interrupt context. By now the
	 * endpoint has a destination, because an inbound message is what gave
	 * it one. */
	if (rpmsg_send(&ept, reply, n) < 0) {
		LOG_ERR("rpmsg_send failed");
	} else {
		LOG_INF("M4F -> A53: \"%s\"", reply);
	}

	return RPMSG_SUCCESS;
}

static void ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t src)
{
	ARG_UNUSED(rdev);
	ARG_UNUSED(src);
	LOG_WRN("Unexpected name-service announcement from the A53: %s", name);
}

/* --------------------------------------------------------------------------
 * Mailbox
 * -------------------------------------------------------------------------- */

/** OpenAMP calls this to kick a vring on the A53 (id = vring index). */
static int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);

	if (ipm_send(ipm_dev, 0, id, &id, sizeof(id)) < 0) {
		LOG_DBG("ipm_send (id=%u) failed", id);
	}
	return 0;
}

/** The A53 kicked us. Wake the management thread to drain the vrings. */
static void mailbox_rx_cb(const struct device *dev, void *user_data,
			  uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(id);
	ARG_UNUSED(data);

	k_sem_give(&mbox_rx_sem);
}

/* --------------------------------------------------------------------------
 * OpenAMP bring-up
 * -------------------------------------------------------------------------- */

static int platform_init(void)
{
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	int rsc_size;
	int ret;

	ret = metal_init(&metal_params);
	if (ret != 0) {
		LOG_ERR("metal_init failed: %d", ret);
		return ret;
	}

	metal_io_init(shm_io, (void *)SHM_START_ADDR, &shm_physmap, SHM_SIZE,
		      -1, 0, NULL);

	rsc_table_get(&rsc_table, &rsc_size);
	rsc_physmap = (metal_phys_addr_t)rsc_table;
	metal_io_init(rsc_io, rsc_table, &rsc_physmap, rsc_size, -1, 0, NULL);

	if (!device_is_ready(ipm_dev)) {
		LOG_ERR("mailbox device not ready");
		return -ENODEV;
	}

	ipm_register_callback(ipm_dev, mailbox_rx_cb, NULL);

	ret = ipm_set_enabled(ipm_dev, 1);
	if (ret != 0) {
		LOG_ERR("ipm_set_enabled failed: %d", ret);
		return ret;
	}

	return 0;
}

static int create_rpmsg_device(void)
{
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device *vdev;
	int ret;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID,
					rsc_table_to_vdev(rsc_table), rsc_io,
					NULL, mailbox_notify, NULL);
	if (vdev == NULL) {
		LOG_ERR("rproc_virtio_create_vdev failed");
		return -ENODEV;
	}

	/* Blocks until Linux sets DRIVER_OK. Nothing else in this firmware
	 * depends on the link, so blocking the management thread here is fine. */
	LOG_INF("waiting for Linux to set DRIVER_OK...");
	rproc_virtio_wait_remote_ready(vdev);
	LOG_INF("DRIVER_OK seen — reading vring addresses Linux wrote back");

	vring_rsc = rsc_table_get_vring0(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 0, vring_rsc->notifyid,
				      (void *)(uintptr_t)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret != 0) {
		LOG_ERR("init vring0 failed: %d", ret);
		return ret;
	}

	vring_rsc = rsc_table_get_vring1(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 1, vring_rsc->notifyid,
				      (void *)(uintptr_t)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret != 0) {
		LOG_ERR("init vring1 failed: %d", ret);
		return ret;
	}

	ret = rpmsg_init_vdev(&rvdev, vdev, ns_bind_cb, shm_io, NULL);
	if (ret != 0) {
		LOG_ERR("rpmsg_init_vdev failed: %d", ret);
		return ret;
	}

	rpdev = rpmsg_virtio_get_rpmsg_device(&rvdev);

	ret = rpmsg_create_ept(&ept, rpdev, EPT_NAME, RPMSG_ADDR_ANY,
			       RPMSG_ADDR_ANY, rpmsg_recv_cb, NULL);
	if (ret != 0) {
		LOG_ERR("rpmsg_create_ept failed: %d", ret);
		return ret;
	}

	return 0;
}

/* --------------------------------------------------------------------------
 * Management thread
 * -------------------------------------------------------------------------- */

#define MNG_STACK_SIZE 2048
#define MNG_PRIORITY   4

static K_THREAD_STACK_DEFINE(mng_stack, MNG_STACK_SIZE);
static struct k_thread mng_thread;

static void mng_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (create_rpmsg_device() != 0) {
		LOG_ERR("OpenAMP bring-up failed — no IPC");
		return;
	}

	LOG_INF("endpoint \"%s\" announced — waiting for the A53 to speak first",
		EPT_NAME);

	/* Service BOTH vrings on every kick: host-to-remote (our RX) and
	 * remote-to-host (TX completion). The mailbox message does not
	 * reliably say which vring was kicked, so checking only one loses
	 * buffers. */
	while (1) {
		k_sem_take(&mbox_rx_sem, K_FOREVER);
		rproc_virtio_notified(rvdev.vdev, VRING0_ID);
		rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	}
}

int main(void)
{
	LOG_INF("=========================================");
	LOG_INF(" BL350 IPC demo — Zephyr on the Cortex-M4F");
	LOG_INF(" board: " CONFIG_BOARD_TARGET);
	LOG_INF("=========================================");

	if (platform_init() != 0) {
		LOG_ERR("platform init failed");
		return 0;
	}

	k_thread_create(&mng_thread, mng_stack, MNG_STACK_SIZE, mng_thread_fn,
			NULL, NULL, NULL, MNG_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&mng_thread, "openamp_mng");

	return 0;
}
