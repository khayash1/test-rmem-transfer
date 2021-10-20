// SPDX-License-Identifier: GPL-2.0
/*
 * CPU/DMA Transfer Test with reserved memory
 * Author: Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 */

#include <linux/crc32.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/version.h>

static unsigned int test_buf_size = 16384;
module_param(test_buf_size, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(test_buf_size, "Size of the memcpy test buffer");

static unsigned int test_type = 3;
module_param(test_type, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(test_type, "Type of test (1=dma only, 2=cpu only, 3=both");

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#define dmaengine_get_dma_device(c) ((c)->device->dev)
#endif

static void test_rmem_release_fix_memory(struct device *child)
{
	of_reserved_mem_device_release(child);
}

static struct device *test_rmem_alloc_fix_memory(struct device *dev,
						 struct device *chan_dev, const char *name,
						 unsigned int idx)
{
	struct device *child;
	int ret;

	child = devm_kzalloc(dev, sizeof(*child), GFP_KERNEL);
	if (!child)
		return NULL;

	device_initialize(child);
	dev_set_name(child, "%s:%s", dev_name(chan_dev), name);
	child->parent = chan_dev;
	child->coherent_dma_mask = chan_dev->coherent_dma_mask;
	child->dma_mask = chan_dev->dma_mask;
	child->release = test_rmem_release_fix_memory;

	child->dma_parms = devm_kzalloc(dev, sizeof(*child->dma_parms),
					GFP_KERNEL);
	if (!child->dma_parms)
		goto out;

	of_dma_configure(child, dev->of_node, true);

	if (!device_add(child)) {
		ret = of_reserved_mem_device_init_by_idx(child, dev->of_node, idx);
		if (!ret)
			return child;
		device_del(child);
	}
out:
	put_device(child);

	return NULL;
}

static void test_rmem_free_fix_memory(struct device *dev)
{
	of_reserved_mem_device_release(dev);
	device_unregister(dev);
}

static int test_memcpy_dma(struct dma_chan *chan,
			   dma_addr_t dst, dma_addr_t src, size_t len)
{
	struct device *dev = dmaengine_get_dma_device(chan);
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	enum dma_status status;
	enum dma_ctrl_flags flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;

	tx = dmaengine_prep_dma_memcpy(chan, dst, src, len, flags);
	if (!tx) {
		dev_err(dev, "Failed to prepare dma\n");
		return -ENODEV;
	}

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie)) {
		dev_err(dev, "Failed to submit dma\n");
		return -EINVAL;
	}

	status = dma_sync_wait(chan, cookie);
	dmaengine_terminate_sync(chan);

	if (status != DMA_COMPLETE) {
		dev_err(dev, "Failed to transfer dma\n");
		return -EIO;
	}

	return 0;
}

static void test_memory_init(u32 *src, u32 *fix, u32 *dst, int len)
{
	int i;

	for (i = 0; i < len; i += 4) {
		*src++ = get_random_u32();
		*fix++ = get_random_u32();
		*dst++ = get_random_u32();
	}
}

static int test_rmem_trasnfer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *chan_dev, *rsvmem_dev, *fixmem_dev;
	struct dma_chan *chan;
	void *src_addr, *fixmem_addr, *dst_addr;
	dma_addr_t src_paddr, fixmem_paddr, dst_paddr;
	dma_cap_mask_t mask;
	size_t len = test_buf_size;
	u32 crc1, crc2;
	int ret = 0;

	dev_info(dev, "transfer test for reserved-memory\n");

	/* Request DMA channel */
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_channel(mask, NULL, NULL);
	if (!chan) {
		dev_err(dev, "Failed to request dma channel\n");
		return -EPROBE_DEFER;
	}
	chan_dev = dmaengine_get_dma_device(chan);

	/* Reserved DRAM */
	rsvmem_dev = test_rmem_alloc_fix_memory(dev, chan_dev, "test-rmem-resv", 0);
	if (!rsvmem_dev) {
		dev_err(dev, "No memory-region found for index 0\n");
		ret = -ENODEV;
		goto out_release_chan;
	}

	/* Fixed memory */
	fixmem_dev = test_rmem_alloc_fix_memory(dev, chan_dev, "test-rmem-fixed", 1);
	if (!fixmem_dev) {
		dev_err(dev, "No memory-region found for index 1\n");
		ret = -ENODEV;
		goto out_unreg_rsvmem;
	}

	src_addr = dma_alloc_coherent(rsvmem_dev, len, &src_paddr, GFP_KERNEL);
	if (!src_addr) {
		ret = -ENOMEM;
		goto out_unreg_fixmem;
	}

	dst_addr = dma_alloc_coherent(rsvmem_dev, len, &dst_paddr, GFP_KERNEL);
	if (!dst_addr) {
		ret = -ENOMEM;
		goto out_free_src;
	}

	fixmem_addr = dma_alloc_coherent(fixmem_dev, len, &fixmem_paddr, GFP_KERNEL);
	if (!fixmem_addr) {
		ret = -ENOMEM;
		goto out_free_dst;
	}

	if (!(test_type & 1))
		goto test_dma_exit;

	/* init for test DMA */
	test_memory_init(src_addr, fixmem_addr, dst_addr, len);

	/* test DMA src->fix */
	ret = test_memcpy_dma(chan, fixmem_paddr, src_paddr, len);
	if (ret) {
		dev_err(dev, "Failed to transfer src->fix\n");
		goto test_dma_exit;
	}
	crc1 = crc32_le(0, src_addr, len);
	crc2 = crc32_le(0, fixmem_addr, len);
	dev_info(dev, "DMA: src:%llx -> fix:%llx %s\n", src_paddr, fixmem_paddr,
		 (crc1 == crc2) ? "OK" : "NG");

	/* test DMA fix->dst */
	ret = test_memcpy_dma(chan, dst_paddr, fixmem_paddr, len);
	if (ret) {
		dev_err(dev, "Failed to transfer fix->dst\n");
		goto test_dma_exit;
	}
	crc1 = crc32_le(0, fixmem_addr, len);
	crc2 = crc32_le(0, dst_addr, len);
	dev_info(dev, "DMA: fix:%llx -> dst:%llx %s\n", fixmem_paddr, dst_paddr,
		 (crc1 == crc2) ? "OK" : "NG");

 test_dma_exit:

	if (!(test_type & 2))
		goto test_cpu_exit;

	/* init for test CPU */
	test_memory_init(src_addr, fixmem_addr, dst_addr, len);

	/* test CPU src->fix */
	memcpy(fixmem_addr, src_addr, len);
	crc1 = crc32_le(0, src_addr, len);
	crc2 = crc32_le(0, fixmem_addr, len);
	dev_info(dev, "CPU: src:%llx -> fix:%llx %s\n", src_paddr, fixmem_paddr,
		 (crc1 == crc2) ? "OK" : "NG");

	/* test CPU fix->dst */
	memcpy(dst_addr, fixmem_addr, len);
	crc1 = crc32_le(0, fixmem_addr, len);
	crc2 = crc32_le(0, dst_addr, len);
	dev_info(dev, "CPU: fix:%llx -> dst:%llx %s\n", fixmem_paddr, dst_paddr,
		 (crc1 == crc2) ? "OK" : "NG");

test_cpu_exit:
	dma_free_coherent(fixmem_dev, len, fixmem_addr, fixmem_paddr);
out_free_dst:
	dma_free_coherent(rsvmem_dev, len, dst_addr, dst_paddr);
out_free_src:
	dma_free_coherent(rsvmem_dev, len, src_addr, src_paddr);
out_unreg_fixmem:
	test_rmem_free_fix_memory(fixmem_dev);
out_unreg_rsvmem:
	test_rmem_free_fix_memory(rsvmem_dev);
out_release_chan:
	dma_release_channel(chan);

	return ret;
}

static const struct of_device_id test_rmem_trasnfer_of_match[] = {
	{ .compatible = "test-rmem-transfer", },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, test_rmem_trasnfer_of_match);

static struct platform_driver test_rmem_trasnfer_driver = {
	.probe = test_rmem_trasnfer_probe,
	.driver	= {
		.name = "test-rmem-trasnfer",
		.of_match_table	= test_rmem_trasnfer_of_match,
	},
};

module_platform_driver(test_rmem_trasnfer_driver);

MODULE_AUTHOR("Kunihiko Hayashi <hayashi.kunihiko@socionext.com>");
MODULE_DESCRIPTION("Trasnfer test module with reserved memory");
MODULE_LICENSE("GPL v2");
