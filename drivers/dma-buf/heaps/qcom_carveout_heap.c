// SPDX-License-Identifier: GPL-2.0
/*
 * DMA-buf carveout heap for Qualcomm platforms
 *
 * This driver provides carveout memory allocation support for
 * reserved memory regions on Qualcomm platforms.
 *
 * Copyright (C) 2024
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

struct carveout_heap {
	struct gen_pool *pool;
	phys_addr_t base;
	size_t size;
};

struct carveout_buffer {
	struct carveout_heap *c_heap;
	struct dma_buf *dmabuf;
	size_t len;
	phys_addr_t phys;
};

static void carveout_heap_dma_buf_release(struct dma_buf *dmabuf)
{
	struct carveout_buffer *buffer = dmabuf->priv;
	struct carveout_heap *c_heap = buffer->c_heap;

	if (buffer->phys) {
		gen_pool_free(c_heap->pool, buffer->phys, buffer->len);
	}

	kfree(buffer);
}

static int carveout_heap_dma_buf_mmap(struct dma_buf *dmabuf,
				       struct vm_area_struct *vma)
{
	struct carveout_buffer *buffer = dmabuf->priv;
	unsigned long pfn = __phys_to_pfn(buffer->phys);
	unsigned long size = buffer->len;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	int ret;

	if (offset >= size)
		return -EINVAL;

	size -= offset;

	if (vma->vm_end - vma->vm_start > size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	ret = remap_pfn_range(vma, vma->vm_start, pfn + (offset >> PAGE_SHIFT),
			     vma->vm_end - vma->vm_start, vma->vm_page_prot);
	if (ret)
		return ret;

	return 0;
}

static const struct dma_buf_ops carveout_heap_buf_ops = {
	.release = carveout_heap_dma_buf_release,
	.mmap = carveout_heap_dma_buf_mmap,
};

static struct dma_buf *carveout_heap_allocate(struct dma_heap *heap,
					      unsigned long len,
					      u32 fd_flags,
					      u64 heap_flags)
{
	struct carveout_heap *c_heap = dma_heap_get_drvdata(heap);
	struct carveout_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	phys_addr_t phys;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	buffer->c_heap = c_heap;
	buffer->len = len;

	/* Align to page boundary */
	len = PAGE_ALIGN(len);

	/* Allocate from genpool */
	phys = gen_pool_alloc(c_heap->pool, len);
	if (!phys) {
		kfree(buffer);
		return ERR_PTR(-ENOMEM);
	}

	buffer->phys = phys;

	/* Create the dmabuf */
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.ops = &carveout_heap_buf_ops;
	exp_info.size = buffer->len;
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		gen_pool_free(c_heap->pool, phys, len);
		kfree(buffer);
		return dmabuf;
	}

	return dmabuf;
}

static const struct dma_heap_ops carveout_heap_ops = {
	.allocate = carveout_heap_allocate,
};

static int carveout_heap_setup(struct carveout_heap *c_heap,
			      struct device *dev,
			      const char *name,
			      phys_addr_t base,
			      size_t size)
{
	int ret;

	/* Use provided base and size */
	c_heap->base = base;
	c_heap->size = size;

	/* Create genpool */
	c_heap->pool = gen_pool_create(PAGE_SHIFT, dev_to_node(dev));
	if (!c_heap->pool) {
		pr_err("Failed to create genpool for %s\n", name);
		return -ENOMEM;
	}

	ret = gen_pool_add(c_heap->pool, c_heap->base, c_heap->size,
			   dev_to_node(dev));
	if (ret) {
		pr_err("Failed to add memory to genpool for %s\n", name);
		gen_pool_destroy(c_heap->pool);
		return ret;
	}

	pr_info("Carveout heap %s: base=0x%pa size=0x%zx\n",
		name, &c_heap->base, &c_heap->size);

	return 0;
}

static int carveout_heap_probe(struct platform_device *pdev)
{
	struct dma_heap_export_info exp_info;
	struct carveout_heap *c_heap;
	struct dma_heap *heap;
	struct resource *res;
	int ret;

	pr_info("carveout_heap_probe: Called for %s\n", pdev->name);

	c_heap = devm_kzalloc(&pdev->dev, sizeof(*c_heap), GFP_KERNEL);
	if (!c_heap)
		return -ENOMEM;

	/* Try to get memory from platform device resources first */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res) {
		ret = carveout_heap_setup(c_heap, &pdev->dev, pdev->name,
					  res->start, resource_size(res));
	} else if (pdev->dev.of_node) {
		/* Fallback to device tree reserved memory */
		struct reserved_mem *rmem = of_reserved_mem_lookup(pdev->dev.of_node);
		if (rmem) {
			ret = carveout_heap_setup(c_heap, &pdev->dev, pdev->name,
						  rmem->base, rmem->size);
		} else {
			pr_warn("No memory resource found for %s\n", pdev->name);
			return -ENODEV;
		}
	} else {
		pr_warn("No memory resource or device tree for %s\n", pdev->name);
		return -ENODEV;
	}

	if (ret)
		return ret;

	/* Register carveout heap */
	exp_info.name = pdev->name;
	exp_info.ops = &carveout_heap_ops;
	exp_info.priv = c_heap;

	heap = dma_heap_add(&exp_info);
	if (IS_ERR(heap)) {
		gen_pool_destroy(c_heap->pool);
		return PTR_ERR(heap);
	}

	dma_coerce_mask_and_coherent(dma_heap_get_dev(heap), ~0ULL);

	dev_set_drvdata(&pdev->dev, heap);

	dev_info(&pdev->dev, "Carveout DMA-buf heap initialized\n");

	return 0;
}

static int carveout_heap_remove(struct platform_device *pdev)
{
	struct dma_heap *heap = dev_get_drvdata(&pdev->dev);
	struct carveout_heap *c_heap = dma_heap_get_drvdata(heap);

	if (c_heap && c_heap->pool)
		gen_pool_destroy(c_heap->pool);

	return 0;
}

static struct platform_driver carveout_heap_driver = {
	.probe = carveout_heap_probe,
	.remove = carveout_heap_remove,
	.driver = {
		.name = "qcom-carveout-heap",
	},
};

/* Platform device creation helper */
static struct platform_device *carveout_heap_create_device(const char *name,
							    const char *compatible,
							    phys_addr_t base,
							    size_t size)
{
	struct platform_device *pdev;
	struct resource res;
	int ret;

	pdev = platform_device_alloc(name, -1);
	if (!pdev)
		return ERR_PTR(-ENOMEM);

	/* Setup memory resource */
	memset(&res, 0, sizeof(res));
	res.start = base;
	res.end = base + size - 1;
	res.flags = IORESOURCE_MEM;

	ret = platform_device_add_resources(pdev, &res, 1);
	if (ret) {
		platform_device_put(pdev);
		return ERR_PTR(ret);
	}

	ret = platform_device_add(pdev);
	if (ret) {
		platform_device_put(pdev);
		return ERR_PTR(ret);
	}

	return pdev;
}

static struct platform_device *secure_display_dev;
static struct platform_device *secure_carveout_dev;

static int __init carveout_heap_init(void)
{
	int ret;

	pr_info("carveout_heap_init: Starting\n");

	ret = platform_driver_register(&carveout_heap_driver);
	if (ret) {
		pr_err("carveout_heap_init: Failed to register platform driver: %d\n", ret);
		return ret;
	}

	pr_info("carveout_heap_init: Platform driver registered\n");

	/* Create platform devices for carveout heaps without DTS */
	/* Secure display heap - heap id 14 */
	secure_display_dev = carveout_heap_create_device("qcom,secure-display",
							 "qcom,secure-display-heap",
							 0x46200000, 0x1e00000);
	if (IS_ERR(secure_display_dev))
		pr_warn("Failed to create secure-display platform device\n");
	else {
		pr_info("carveout_heap_init: Created secure-display platform device\n");
		/* Manually call probe since name matching won't work */
		carveout_heap_probe(secure_display_dev);
	}

	/* Secure carveout heap - heap id 14 (alternative name) */
	secure_carveout_dev = carveout_heap_create_device("qcom,secure-carveout",
							  "qcom,secure-carveout-heap",
							  0x46200000, 0x1e00000);
	if (IS_ERR(secure_carveout_dev))
		pr_warn("Failed to create secure-carveout platform device\n");
	else {
		pr_info("carveout_heap_init: Created secure-carveout platform device\n");
		/* Manually call probe since name matching won't work */
		carveout_heap_probe(secure_carveout_dev);
	}

	return 0;
}

static void __exit carveout_heap_exit(void)
{
	if (secure_display_dev)
		platform_device_unregister(secure_display_dev);
	if (secure_carveout_dev)
		platform_device_unregister(secure_carveout_dev);
	platform_driver_unregister(&carveout_heap_driver);
}

late_initcall(carveout_heap_init);
module_exit(carveout_heap_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm Carveout DMA-buf heap");
