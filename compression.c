#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/lzo.h>
#include <linux/highmem.h>
//#include "compression.h"

#define COMPRESSION_STRIDE_LEN 16
#define C_LEN sizeof(unsigned)

struct workspace
{
	void *memory;   /* memory required for compression */
	void *c_buffer; /* memory where compressed buffer goes */
	void *d_buffer; /* memory where decompressed buffer goes */
};

static inline void write_compress_length(char *buf, size_t len)
{
	__le32 dlen;

	dlen = cpu_to_le32(len);
	memcpy(buf, &dlen, C_LEN);
}

static inline size_t read_compress_length(char *buf)
{
	__le32 dlen;

	memcpy(&dlen, buf, C_LEN);
	return le32_to_cpu(dlen);
}

static struct workspace *init_workspace(unsigned stride_len)
{
	if(DEBUG_MODE_K==1)
	{
		printk(KERN_INFO"%25s  %25s  %4d  #in\n",__FILE__,__func__,__LINE__);
	}
	
	struct workspace *workspace;

	workspace = kmalloc(sizeof(struct workspace), GFP_NOFS);
	if(!workspace)
		return ERR_PTR(-ENOMEM);

	workspace->memory   = vmalloc(LZO1X_MEM_COMPRESS);
	workspace->c_buffer = vmalloc(lzo1x_worst_compress(PAGE_CACHE_SIZE*stride_len));
	workspace->d_buffer = vmalloc(lzo1x_worst_compress(PAGE_CACHE_SIZE*stride_len));

	if (!workspace->memory || !workspace->d_buffer || !workspace->c_buffer)
		goto fail;

	return workspace;
	
fail:

	BUG_ON(1);
	return ERR_PTR(-ENOMEM);
}

static void free_workspace(struct workspace *workspace)
{
	if(DEBUG_MODE_K==1)
	{
		printk(KERN_INFO"%25s  %25s  %4d  #in\n",__FILE__,__func__,__LINE__);
	}	
	vfree(workspace->memory);
	vfree(workspace->c_buffer);
	vfree(workspace->d_buffer);
	kfree(workspace);
}

static int compressed_bio_init(struct compressed_bio *cb, struct inode *inode, block_t start,
			       unsigned nr_pages, unsigned len, unsigned compressed_len)
{

	cb->compressed_pages = kzalloc(sizeof(struct page *) * nr_pages, GFP_NOFS);
	if (!cb->compressed_pages) {
		BUG_ON(1);
		return -ENOMEM;
	}
	cb->inode    = inode;
	cb->start    = start;
	cb->nr_pages = nr_pages;
	cb->len      = len;
	cb->compressed_len = compressed_len;
	cb->compress_type  = 1;
	cb->errors   = 0;
	cb->buffer   = NULL;
	
	atomic_set(&cb->pending_bios, 0);
	return 0;
}

int compress_stride(struct bufvec *bufvec)
{
	if(DEBUG_MODE_K==1)
	{
		printk(KERN_INFO"%25s  %25s  %4d  #in\n",__FILE__,__func__,__LINE__);
	}
	struct inode *inode = bufvec_inode(bufvec);
	struct buffer_head *buffer;
	struct workspace *workspace;
	struct page *page;
	unsigned nr_pages, page_idx, offset;
	unsigned len = bufvec_contig_count(bufvec);
	size_t in_len, out_len, tail;
	char *data;
	int ret = 0;
	
	workspace = init_workspace(len);
	printk(KERN_INFO"\n[C]inode : %lu\n", inode->i_ino);
	
	in_len  = len << PAGE_CACHE_SHIFT;
	out_len = 0;

	offset = 0;
	bufvec_buffer_for_each_contig(buffer,bufvec){
		data = kmap(buffer->b_page);
		memcpy((char *)workspace->d_buffer + offset, data, PAGE_CACHE_SIZE);
		offset += PAGE_CACHE_SIZE;
		kunmap(buffer->b_page);
	}
	printk(KERN_INFO "\nMemcpy Done...Start Compress!\n");

	ret = lzo1x_1_compress(workspace->d_buffer, in_len, workspace->c_buffer,
			       &out_len, workspace->memory);
	
	if (ret == LZO_E_OK) {
		
		if (out_len > in_len)/* Check this case */
			tail = lzo1x_worst_compress(PAGE_CACHE_SIZE * len) - out_len;
		else
			tail = PAGE_CACHE_SIZE - (out_len % PAGE_CACHE_SIZE);
		
		out_len += C_LEN;
		nr_pages = out_len / PAGE_CACHE_SIZE + 1;
		memset((char *)workspace->c_buffer + out_len, 0, tail);
		printk(KERN_INFO"Compressed from %zu to %zu | Compressed_blocks : %zu | Tail : %zu\n", in_len, out_len, nr_pages, tail);
	} else {
		/* Error in Compression! Bail out */
		printk(KERN_DEBUG "Tux3 Compression Error : %d\n", ret);
		BUG_ON(1);
		goto out;
	}
	
	bufvec->cb = kzalloc(sizeof(struct compressed_bio), GFP_NOFS);
	if (!bufvec->cb) {
		/* ERROR */
		BUG_ON(1);
		ret = -ENOMEM;
		goto out;
	}

	buffer = bufvec_contig_buf(bufvec);
	ret = compressed_bio_init(bufvec->cb, inode, bufindex(buffer), nr_pages, in_len, out_len);
	if (ret) {
		kfree(bufvec->cb);
		goto out;
	}

	offset = 0;
	for(page_idx = 0; page_idx < nr_pages; page_idx++)
	{
		page = alloc_page(GFP_NOFS |__GFP_HIGHMEM);
		if (!page) {
			/* ERROR */
			BUG_ON(1);	
			ret = -ENOMEM;
			goto out;
		}
		data = kmap(page);
		if (page_idx == 0) {
			/* Store compressed_len of stride in first page of stride */
			write_compress_length(data, out_len);
			memcpy((char *)data + C_LEN, (char *)workspace->c_buffer, PAGE_CACHE_SIZE - C_LEN);
			offset = PAGE_CACHE_SIZE - C_LEN;
		} else {
			memcpy(data, (char *)workspace->c_buffer + offset, PAGE_CACHE_SIZE);
			offset += PAGE_CACHE_SIZE;
		}
		kunmap(page);
		bufvec->cb->compressed_pages[page_idx] = page;
	}

out:
	free_workspace(workspace);
	return ret;
}
