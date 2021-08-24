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

static unsigned int test_buf_size = 16384;
module_param(test_buf_size, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(test_buf_size, "Size of the memcpy test buffer");

static int test_memcpy_dma(struct device *dev,
			   dma_addr_t dst, dma_addr_t src, size_t len)
{
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	dma_cap_mask_t mask;
	enum dma_status status;
	enum dma_ctrl_flags flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
	int ret = 0;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_channel(mask, NULL, NULL);
	if (!chan) {
		dev_err(dev, "Failed to request dma channel\n");
		return -ENODEV;
	}

	tx = dmaengine_prep_dma_memcpy(chan, dst, src, len, flags);
	if (!tx) {
		dev_err(dev, "Failed to prepare dma\n");
		ret = -ENODEV;
		goto out;
	}

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie)) {
		dev_err(dev, "Failed to submit dma\n");
		ret = -EINVAL;
		goto out;
	}

	status = dma_sync_wait(chan, cookie);
	dmaengine_terminate_sync(chan);

	if (status != DMA_COMPLETE) {
		dev_err(dev, "Failed to transfer dma\n");
		ret = -EIO;
		goto out;
	}

out:
	dma_release_channel(chan);

	return ret;
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
	struct device_node *np = dev->of_node, *np_f;
	struct reserved_mem *rmem;
	void *src_addr, *fixmem_addr, *dst_addr;
	dma_addr_t src_paddr, fixmem_paddr, dst_paddr;
	size_t len = test_buf_size;
	u32 crc1, crc2;
	int ret;

	dev_info(dev, "transfer test for reserved-memory\n");

	/* Reserved DRAM */
	ret = of_reserved_mem_device_init_by_idx(dev, np, 0);
	if (ret) {
		dev_err(dev, "No memory-region found for index 0\n");
		return ret;
	}

	/* Fixed memory */
	np_f = of_parse_phandle(np, "memory-region", 1);
	if (!np_f) {
		dev_err(dev, "No memory-region found for index 1\n");
		return -ENODEV;
	}
	rmem = of_reserved_mem_lookup(np_f);
	if (!rmem) {
		dev_err(dev, "Failed to lookup memory-region 1\n");
		return -ENODEV;
	}
	if (rmem->size < len) {
		dev_err(dev, "The size of memory-region 1 not enough\n");
		return -ENOMEM;
	}
	fixmem_paddr = rmem->base;

	src_addr = dmam_alloc_coherent(dev, len, &src_paddr, GFP_KERNEL);
	if (!src_addr) {
		dev_err(dev, "Failed to alloc 'src' memory\n");
		return -ENOMEM;
	}

	dst_addr = dmam_alloc_coherent(dev, len, &dst_paddr, GFP_KERNEL);
	if (!dst_addr) {
		dev_err(dev, "Failed to alloc 'dst' memory\n");
		return -ENOMEM;
	}

	fixmem_addr = devm_memremap(dev, fixmem_paddr, len, MEMREMAP_WC);
	if (!fixmem_addr) {
		dev_err(dev, "Failed to map 'fix' memory\n");
		return -ENOMEM;
	}

	/* init for test DMA */
	test_memory_init(src_addr, fixmem_addr, dst_addr, len);

	/* test DMA src->fix */
	ret = test_memcpy_dma(dev, fixmem_paddr, src_paddr, len);
	if (ret) {
		dev_err(dev, "Failed to transfer src->fix\n");
		return ret;
	}
	crc1 = crc32_le(0, src_addr, len);
	crc2 = crc32_le(0, fixmem_addr, len);
	dev_info(dev, "DMA: src:%llx -> fix:%llx %s\n", src_paddr, fixmem_paddr,
		 (crc1 == crc2) ? "OK" : "NG");

	/* test DMA fix->dst */
	ret = test_memcpy_dma(dev, dst_paddr, fixmem_paddr, len);
	if (ret) {
		dev_err(dev, "Failed to transfer fix->dst\n");
		return ret;
	}
	crc1 = crc32_le(0, fixmem_addr, len);
	crc2 = crc32_le(0, dst_addr, len);
	dev_info(dev, "DMA: fix:%llx -> dst:%llx %s\n", fixmem_paddr, dst_paddr,
		 (crc1 == crc2) ? "OK" : "NG");

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

	return 0;
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
