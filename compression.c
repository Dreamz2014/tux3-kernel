#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/lzo.h>
#include <linux/highmem.h>
//#include "compression.h"

#define COMPRESSION_STRIDE_LEN 16

struct workspace
{
	void *mem;//memory required for compression
	void *c_buf;//memory where compressed buffer goes
	void *d_buf;//memory where decompressed buffer goes
};

static struct workspace *init_workspace(unsigned stride_len)
{
	if(DEBUG_MODE_K==1)
	{
		printk(KERN_INFO"%25s  %25s  %4d  #in\n",__FILE__,__func__,__LINE__);
	}
	
	struct workspace *workspace;

	workspace = kzalloc(sizeof(*workspace), GFP_NOFS);
	if(!workspace)
		return ERR_PTR(-ENOMEM);

	workspace->mem = vmalloc(LZO1X_MEM_COMPRESS);
	workspace->c_buf = vmalloc(lzo1x_worst_compress(PAGE_CACHE_SIZE*stride_len));
	workspace->d_buf = vmalloc(lzo1x_worst_compress(PAGE_CACHE_SIZE*stride_len));

	if (!workspace->mem || !workspace->d_buf || !workspace->c_buf)
		goto fail;

	return workspace;
	
	fail:
	return ERR_PTR(-ENOMEM);
}

static void free_workspace(struct workspace *workspace)
{
	if(DEBUG_MODE_K==1)
	{
		printk(KERN_INFO"%25s  %25s  %4d  #in\n",__FILE__,__func__,__LINE__);
	}
	
	vfree(workspace->mem);
	vfree(workspace->c_buf);
	vfree(workspace->d_buf);
	kfree(workspace);
}

//Regular files ->
//if (S_ISREG(inode->i_mode))...compress

int compress_stride(struct bufvec *bufvec)
{
	if(DEBUG_MODE_K==1)
	{
		printk(KERN_INFO"%25s  %25s  %4d  #in\n",__FILE__,__func__,__LINE__);
	}
	struct inode *inode = bufvec_inode(bufvec);
	struct buffer_head *buffer;
	struct workspace *workspace;
	struct list_head *list;
	struct page *page;
	unsigned nr_pages, page_idx, offset, length;
	unsigned len = bufvec_contig_count(bufvec);
	size_t in_len, out_len, tail;
	char *data;
	int ret = 0;
	
	workspace = init_workspace(len);
//	buffer = list_entry(bufvec->contig.next, struct buffer_head, b_assoc_buffers);
	printk(KERN_INFO"\n[C]inode : %lu", inode->i_ino);
	
	in_len = bufvec_contig_count(bufvec)*PAGE_CACHE_SIZE;
	out_len = 0;

	offset = 0;
	bufvec_buffer_for_each_contig(buffer,bufvec){
		
		//printk("[C]Index : %Lu\n",bufindex(buffer));
		data = kmap(buffer->b_page);
		memcpy((char *)workspace->d_buf + offset, data, PAGE_CACHE_SIZE);
		offset += PAGE_CACHE_SIZE;
		kunmap(buffer->b_page);
	}
	
	length = len;
	while(length)
	{
		list = bufvec->contig.next;
		buffer = list_entry(list, struct buffer_head, b_assoc_buffers);
		//printk(KERN_INFO "Move_to_compress : %Lu",bufindex(buffer));
		
		bufvec_contig_move_to_compress(bufvec, buffer);
		length--;
	}
	printk(KERN_INFO "MEMCPY DONE !");

	ret = lzo1x_1_compress(workspace->d_buf, in_len, workspace->c_buf,
			       &out_len, workspace->mem);

	nr_pages = out_len / PAGE_CACHE_SIZE + 1;
	if (ret != LZO_E_OK) 
	{
		printk(KERN_DEBUG "tux3_compr error :  %d\n",ret);
		ret = -1;
	}
	else
	{
		tail = PAGE_CACHE_SIZE - (out_len % PAGE_CACHE_SIZE);
		printk(KERN_INFO"COMPRESSED FROM %zu to %zu | Compressed_blocks : %zu, tail : %zu\n", in_len, out_len, nr_pages, tail);
		memset((char *)workspace->c_buf + out_len, 0, tail);
	}
	
	bufvec->compressed_pages = kzalloc(sizeof(struct page *) * nr_pages, GFP_NOFS);
	if(!bufvec->compressed_pages){
		/* ERROR... EXIT */
		ret = -1;
		goto out;
	}

	offset = 0;
	for(page_idx = 0; page_idx < nr_pages; page_idx++)
	{
		page = alloc_page(GFP_NOFS |__GFP_HIGHMEM);
		if(!page){
			/* ERROR */
			ret = -1;
			goto out;
		}
		data = kmap(page);
		memcpy(data, (char *)workspace->c_buf + offset, PAGE_CACHE_SIZE);
		offset += PAGE_CACHE_SIZE;
		kunmap(page);
		bufvec->compressed_pages[page_idx] = page;
	}
	
	length = nr_pages;
	while(length)
	{
		list = bufvec->compress.next;
		buffer = list_entry(list, struct buffer_head, b_assoc_buffers);
		//printk(KERN_INFO "Move_to_contig : %Lu",bufindex(buffer));
		
		bufvec_compress_move_to_contig(bufvec, buffer);
		length--;
	}
/*	
	//last stride
	if(in_len < COMPRESSION_STRIDE_LEN * PAGE_CACHE_SIZE){

		bufvec_buffer_for_each_compress(buffer,bufvec)
		{
			//release buffers!
			printk("[Discard_C]Index : %Lu\n",bufindex(buffer));
			discard_buffer(buffer);
			//__list_del_entry(&buffer->b_assoc_buffers);//or list_del(list);
			//bufvec->compress_count--;
		}
		//remove_inode_buffers(inode);
	}
*/
out:
	free_workspace(workspace);
	return 0;
}
