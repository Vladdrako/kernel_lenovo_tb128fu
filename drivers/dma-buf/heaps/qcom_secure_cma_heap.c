// SPDX-License-Identifier: GPL-2.0
/*
 * Secure CMA DMA-buf heap for Qualcomm platforms
 *
 * This driver provides secure CMA memory allocation support for
 * GPU firmware and other secure memory needs on Qualcomm platforms.
 *
 * Copyright (C) 2024
 */

#include <linux/cma.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

struct secure_cma_heap {
	struct cma *cma;
	phys_addr_t base;
	size_t size;
};

struct secure_cma_buffer {
	struct secure_cma_heap *s_heap;
	struct dma_buf *dmabuf;
	size_t len;
	struct page *cma_pages;
};

static void secure_cma_heap_dma_buf_release(struct dma_buf *dmabuf)
{
	struct secure_cma_buffer *buffer = dmabuf->priv;
	struct secure_cma_heap *s_heap = buffer->s_heap;

	if (buffer->cma_pages) {
		cma_release(s_heap->cma, buffer->cma_pages, buffer->len >> PAGE_SHIFT);
	}

	kfree(buffer);
}

static int secure_cma_heap_dma_buf_mmap(struct dma_buf *dmabuf,
					 struct vm_area_struct *vma)
{
	struct secure_cma_buffer *buffer = dmabuf->priv;
	unsigned long pfn = page_to_pfn(buffer->cma_pages);
	unsigned long size = buffer->len;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (offset >= size)
		return -EINVAL;

	size -= offset;

	if (vma->vm_end - vma->vm_start > size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return remap_pfn_range(vma, vma->vm_start, pfn + (offset >> PAGE_SHIFT),
			     vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static const struct dma_buf_ops secure_cma_heap_buf_ops = {
	.release = secure_cma_heap_dma_buf_release,
	.mmap = secure_cma_heap_dma_buf_mmap,
};

static struct dma_buf *secure_cma_heap_allocate(struct dma_heap *heap,
					       unsigned long len,
					       u32 fd_flags,
					       u64 heap_flags)
{
	struct secure_cma_heap *s_heap = dma_heap_get_drvdata(heap);
	struct secure_cma_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	struct page *cma_pages;
	unsigned long pagecount;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	buffer->s_heap = s_heap;
	buffer->len = len;

	/* Align to page boundary */
	len = PAGE_ALIGN(len);
	pagecount = len >> PAGE_SHIFT;

	/* Allocate from CMA */
	cma_pages = cma_alloc(s_heap->cma, pagecount, 0, GFP_KERNEL);
	if (!cma_pages) {
		kfree(buffer);
		return ERR_PTR(-ENOMEM);
	}

	buffer->cma_pages = cma_pages;

	/* Create the dmabuf */
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.ops = &secure_cma_heap_buf_ops;
	exp_info.size = buffer->len;
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		cma_release(s_heap->cma, cma_pages, pagecount);
		kfree(buffer);
		return dmabuf;
	}

	return dmabuf;
}

static const struct dma_heap_ops secure_cma_heap_ops = {
	.allocate = secure_cma_heap_allocate,
};

static int secure_cma_heap_probe(struct platform_device *pdev)
{
	struct dma_heap_export_info exp_info;
	struct secure_cma_heap *s_heap;
	struct dma_heap *heap;
	extern struct cma *dma_contiguous_default_area;
	struct cma *cma = dma_contiguous_default_area;

	pr_info("secure_cma_heap_probe: Called for %s\n", pdev->name);

	s_heap = devm_kzalloc(&pdev->dev, sizeof(*s_heap), GFP_KERNEL);
	if (!s_heap)
		return -ENOMEM;

	if (!cma) {
		pr_err("No default CMA region found for %s\n", pdev->name);
		return -ENODEV;
	}

	s_heap->cma = cma;
	s_heap->base = 0; /* CMA base is managed by CMA subsystem */
	s_heap->size = 0; /* CMA size is managed by CMA subsystem */

	/* Register secure CMA heap */
	exp_info.name = pdev->name;
	exp_info.ops = &secure_cma_heap_ops;
	exp_info.priv = s_heap;

	heap = dma_heap_add(&exp_info);
	if (IS_ERR(heap))
		return PTR_ERR(heap);

	dma_coerce_mask_and_coherent(dma_heap_get_dev(heap), ~0ULL);

	dev_set_drvdata(&pdev->dev, heap);

	dev_info(&pdev->dev, "Secure CMA DMA-buf heap initialized\n");

	return 0;
}

static int secure_cma_heap_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver secure_cma_heap_driver = {
	.probe = secure_cma_heap_probe,
	.remove = secure_cma_heap_remove,
	.driver = {
		.name = "qcom-secure-cma-heap",
	},
};

/* Platform device creation helper */
static struct platform_device *secure_cma_heap_create_device(const char *name,
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

static struct platform_device *secure_cma_dev;

static int __init secure_cma_heap_init(void)
{
	int ret;

	pr_info("secure_cma_heap_init: Starting\n");

	ret = platform_driver_register(&secure_cma_heap_driver);
	if (ret) {
		pr_err("secure_cma_heap_init: Failed to register platform driver: %d\n", ret);
		return ret;
	}

	pr_info("secure_cma_heap_init: Platform driver registered\n");

	/* Create platform device for secure CMA heap without DTS */
	/* Secure CMA heap - heap id 10 (GPU firmware) */
	secure_cma_dev = secure_cma_heap_create_device("qcom,secure-cma",
							"qcom,secure-cma-heap",
							0, 0);
	if (IS_ERR(secure_cma_dev))
		pr_warn("Failed to create secure-cma platform device\n");
	else {
		pr_info("secure_cma_heap_init: Created secure-cma platform device\n");
		/* Manually call probe since name matching won't work */
		secure_cma_heap_probe(secure_cma_dev);
	}

	return 0;
}

static void __exit secure_cma_heap_exit(void)
{
	if (secure_cma_dev)
		platform_device_unregister(secure_cma_dev);
	platform_driver_unregister(&secure_cma_heap_driver);
}

late_initcall(secure_cma_heap_init);
module_exit(secure_cma_heap_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm Secure CMA DMA-buf heap");
