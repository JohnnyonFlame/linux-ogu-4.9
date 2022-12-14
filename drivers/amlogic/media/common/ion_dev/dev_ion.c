/*
 * drivers/amlogic/media/common/ion_dev/dev_ion.c
 *
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/err.h>
#include <ion/ion.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <ion/ion_priv.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/uaccess.h>
#include "meson_ion.h"
#include <linux/scatterlist.h>

MODULE_DESCRIPTION("AMLOGIC ION driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Amlogic SH");

#define ION_INFO(fmt, args...)     pr_info("ion-dev: " fmt "", ## args)
#define ION_DEBUG(fmt, args...)    pr_debug("ion-dev: debug: " fmt "", ## args)
#define ION_ERR(fmt, args...)    pr_err("ion-dev: error: " fmt "", ## args)

/*
 * TODO instead with enum ion_heap_type from ion.h
 */
#define AML_ION_TYPE_SYSTEM	 0
#define AML_ION_TYPE_CARVEOUT   1
#define MAX_HEAP 4

static struct ion_device *idev;
static int num_heaps;
static struct ion_heap **heaps;
static struct ion_platform_heap my_ion_heap[MAX_HEAP];

struct ion_client *meson_ion_client_create(unsigned int heap_mask,
		const char *name) {

	/*
	 * The assumption is that if there is a NULL device, the ion
	 * driver has not yet probed.
	 *
	 */
	if (idev == NULL) {
		ION_DEBUG("create error");
		return ERR_PTR(-EPROBE_DEFER);
	}

	if (IS_ERR(idev)) {
		ION_DEBUG("idev error");
		return (struct ion_client *)idev;
	}

	return ion_client_create(idev, name);
}
EXPORT_SYMBOL(meson_ion_client_create);

/*
 * ion_phys() is removed from ion.c in k4.4
 * this function is from pengcheng
 */

int ion_phys(struct ion_client *client, struct ion_handle *handle,
	     ion_phys_addr_t *addr, size_t *len)
{
	struct ion_buffer *buffer;
	struct sg_table *sg_table;
	struct page *page;
	int ret;

	buffer = handle->buffer;
	if (buffer) {
		sg_table = buffer->sg_table;
		page = sg_page(sg_table->sgl);
		*addr = PFN_PHYS(page_to_pfn(page));
		*len = buffer->size;
		ret = 0;
	}
	return 0;
}
EXPORT_SYMBOL(ion_phys);

int meson_ion_share_fd_to_phys(struct ion_client *client,
		int share_fd, ion_phys_addr_t *addr, size_t *len)
{
	struct ion_handle *handle = NULL;
	int ret;

	handle = ion_import_dma_buf_fd(client, share_fd);
	if (IS_ERR_OR_NULL(handle)) {
		/* pr_err("%s,EINVAL, client=%p, share_fd=%d\n",
		 *	 __func__, client, share_fd);
		 */
		return PTR_ERR(handle);
	}

	ret = ion_phys(client, handle, addr, (size_t *)len);
	ION_DEBUG("ion_phys ret=%d, phys=0x%lx\n", ret, *addr);
	ion_free(client, handle);
	if (ret < 0) {
		ION_ERR("ion_get_phys error, ret=%d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(meson_ion_share_fd_to_phys);

static int meson_ion_get_phys(
	struct ion_client *client,
	unsigned long arg)
{
	struct meson_phys_data data;
	struct ion_handle *handle;
	size_t len = 0;
	ion_phys_addr_t addr = 0;
	int ret;

	if (copy_from_user(&data, (void __user *)arg,
		sizeof(struct meson_phys_data))) {
		return -EFAULT;
	}
	handle = ion_import_dma_buf_fd(client, data.handle);
	if (IS_ERR_OR_NULL(handle)) {
		ION_DEBUG("EINVAL, client=%p, share_fd=%d\n",
			client, data.handle);
		return PTR_ERR(handle);
	}

	ret = ion_phys(client, handle, &addr, (size_t *)&len);
	ION_DEBUG("ret=%d, phys=0x%lX\n", ret, addr);
	ion_free(client, handle);
	if (ret < 0) {
		ION_DEBUG("meson_ion_get_phys error, ret=%d\n", ret);
		return ret;
	}

	data.phys_addr = (unsigned int)addr;
	data.size = (unsigned int)len;
	if (copy_to_user((void __user *)arg, &data,
		sizeof(struct meson_phys_data))) {
		return -EFAULT;
	}
	return 0;
}

static long meson_custom_ioctl(
	struct ion_client *client,
	unsigned int cmd,
	unsigned long arg)
{
	switch (cmd) {
	case ION_IOC_MESON_PHYS_ADDR:
		return meson_ion_get_phys(client, arg);
	default:
		return -ENOTTY;
	}
	return 0;
}

int dev_ion_probe(struct platform_device *pdev)
{
	int err = 0;
	int i;
#ifdef CONFIG_AMLOGIC_TEE
	unsigned int handle;
#endif

	my_ion_heap[num_heaps].type = ION_HEAP_TYPE_SYSTEM;
	my_ion_heap[num_heaps].id = ION_HEAP_TYPE_SYSTEM;
	my_ion_heap[num_heaps].name = "vmalloc_ion";
	num_heaps++;

	my_ion_heap[num_heaps].type = ION_HEAP_TYPE_CUSTOM;
	my_ion_heap[num_heaps].id = ION_HEAP_TYPE_CUSTOM;
	my_ion_heap[num_heaps].name = "codec_mm_ion";
	my_ion_heap[num_heaps].base = (ion_phys_addr_t) NULL;
	/* limit the maximum alloc total size 300M */
	my_ion_heap[num_heaps].size = 300 * 1024 * 1024;
	num_heaps++;

	/*add CMA ion heap*/
	my_ion_heap[num_heaps].type = ION_HEAP_TYPE_DMA;
	my_ion_heap[num_heaps].id = ION_HEAP_TYPE_DMA;
	my_ion_heap[num_heaps].name = "cma_ion";
	my_ion_heap[num_heaps].priv = &pdev->dev;
	num_heaps++;


	/* init reserved memory */
	err = of_reserved_mem_device_init(&pdev->dev);
	if (err != 0)
		ION_INFO("failed get reserved memory\n");
	err = of_reserved_mem_device_init_by_idx(&pdev->dev,
		pdev->dev.of_node, 1);
	if (err != 0)
		ION_INFO("failed get fb memory\n");
	err = of_reserved_mem_device_init_by_idx(&pdev->dev,
		pdev->dev.of_node, 2);
	if (err != 0)
		ION_INFO("failed get secure memory\n");
	heaps = kcalloc(num_heaps, sizeof(struct ion_heap *), GFP_KERNEL);
	if (!heaps)
		return -ENOMEM;
	/* idev = ion_device_create(NULL); */
	idev = ion_device_create(meson_custom_ioctl);
	if (IS_ERR_OR_NULL(idev)) {
		kfree(heaps);
		panic(0);
		return PTR_ERR(idev);
	}

	platform_set_drvdata(pdev, idev);

	/* create the heaps as specified in the board file */
	for (i = 0; i < num_heaps; i++) {
		heaps[i] = ion_heap_create(&my_ion_heap[i]);
		if (IS_ERR_OR_NULL(heaps[i])) {
			err = PTR_ERR(heaps[i]);
			goto failed;
		}
#ifdef CONFIG_AMLOGIC_TEE
		if (my_ion_heap[i].type == ION_HEAP_TYPE_CUSTOM &&
		    my_ion_heap[i].id == ION_HEAP_ID_SECURE &&
		    my_ion_heap[i].base &&
		    my_ion_heap[i].size) {
			tee_protect_mem_by_type(TEE_MEM_TYPE_GPU,
						(u32)my_ion_heap[i].base,
						(u32)my_ion_heap[i].size,
						&handle);
			ION_INFO("tee protect gpu mem done\n");
		}
#endif
		ion_device_add_heap(idev, heaps[i]);
		ION_INFO("add heap type:%d id:%d\n",
				my_ion_heap[i].type, my_ion_heap[i].id);
	}

	ION_INFO("%s, create %d heaps\n", __func__, num_heaps);
	return 0;
failed:
	ION_ERR("ion heap create failed\n");
	kfree(heaps);
	heaps = NULL;
	panic(0);
	return err;
}

int dev_ion_remove(struct platform_device *pdev)
{
	struct ion_device *idev = platform_get_drvdata(pdev);
	int i;
	ion_device_destroy(idev);
	for (i = 0; i < num_heaps; i++)
		ion_heap_destroy(heaps[i]);
	kfree(heaps);
	return 0;
}

static const struct of_device_id amlogic_ion_dev_dt_match[] = {
	{ .compatible = "amlogic, ion_dev", },
	{ },
};

static struct platform_driver ion_driver = {
	.probe = dev_ion_probe,
	.remove = dev_ion_remove,
	.driver = {
		.name = "ion_dev",
		.owner = THIS_MODULE,
		.of_match_table = amlogic_ion_dev_dt_match
	}
};

/*
 * reserved memory initialize begin
 */
static int ion_dev_mem_init(struct reserved_mem *rmem, struct device *dev)
{
	my_ion_heap[num_heaps].type = ION_HEAP_TYPE_CARVEOUT;
	my_ion_heap[num_heaps].id = ION_HEAP_TYPE_CARVEOUT;
	my_ion_heap[num_heaps].name = "carveout_ion";
	my_ion_heap[num_heaps].base = (ion_phys_addr_t) rmem->base;
	my_ion_heap[num_heaps].size = rmem->size;
	ION_INFO("ion_dev_mem_init size=%pa\n", &rmem->size);
	num_heaps++;

	return 0;
}

static const struct reserved_mem_ops rmem_ion_dev_ops = {
	.device_init = ion_dev_mem_init,
};

#ifdef AMLOGIC_RMEM_MULTI_USER
static struct rmem_multi_user rmem_ion_muser = {
	.of_match_table = amlogic_ion_dev_dt_match,
	.ops  = &rmem_ion_dev_ops,
};
#endif

static int __init ion_dev_mem_setup(struct reserved_mem *rmem)
{
#ifdef AMLOGIC_RMEM_MULTI_USER
	of_add_rmem_multi_user(rmem, &rmem_ion_muser);
#else
	rmem->ops = &rmem_ion_dev_ops;
#endif
	ION_DEBUG("ion_dev mem setup\n");

	return 0;
}

static int ion_secure_mem_init(struct reserved_mem *rmem, struct device *dev)
{
	my_ion_heap[num_heaps].type = ION_HEAP_TYPE_CUSTOM;
	my_ion_heap[num_heaps].id = ION_HEAP_ID_SECURE;
	my_ion_heap[num_heaps].name = "secure_ion";
	my_ion_heap[num_heaps].base = (ion_phys_addr_t)rmem->base;
	my_ion_heap[num_heaps].size = rmem->size;
	ION_INFO("ion secure_mem_init size=0x%pa, paddr=0x%pa\n",
		&rmem->size, &rmem->base);
	num_heaps++;
	return 0;
}

static const struct reserved_mem_ops rmem_ion_secure_ops = {
	.device_init = ion_secure_mem_init,
};

static int __init ion_secure_mem_setup(struct reserved_mem *rmem)
{
	rmem->ops = &rmem_ion_secure_ops;
	ION_DEBUG("ion secure mem setup\n");
	return 0;
}

static int ion_fb_mem_init(struct reserved_mem *rmem, struct device *dev)
{
	my_ion_heap[num_heaps].type = ION_HEAP_TYPE_CUSTOM;
	my_ion_heap[num_heaps].id = ION_HEAP_ID_FB;
	my_ion_heap[num_heaps].name = "fb_ion";
	my_ion_heap[num_heaps].base = (ion_phys_addr_t)rmem->base;
	my_ion_heap[num_heaps].size = rmem->size;
	ION_INFO("ion fb_mem_init size=0x%pa, paddr=0x%pa\n",
		 &rmem->size, &rmem->base);
	num_heaps++;
	return 0;
}

static const struct reserved_mem_ops rmem_ion_fb_ops = {
	.device_init = ion_fb_mem_init,
};

static int __init ion_fb_mem_setup(struct reserved_mem *rmem)
{
	rmem->ops = &rmem_ion_fb_ops;
	ION_DEBUG("ion fb mem setup\n");
	return 0;
}

RESERVEDMEM_OF_DECLARE(ion_fb_mem, "amlogic, ion-fb-mem",
		       ion_fb_mem_setup);

RESERVEDMEM_OF_DECLARE(ion_secure_mem, "amlogic, ion-secure-mem",
		       ion_secure_mem_setup);

RESERVEDMEM_OF_DECLARE(ion_dev_mem, "amlogic, idev-mem", ion_dev_mem_setup);
/*
 * reserved memory initialize end
 */

static int __init ion_init(void)
{
	return platform_driver_register(&ion_driver);
}

static void __exit ion_exit(void)
{
	platform_driver_unregister(&ion_driver);
}

module_init(ion_init);
module_exit(ion_exit);
