/*
 * fs/mpage.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * Contains functions related to preparing and submitting BIOs which contain
 * multiple pagecache pages.
 *
 * 15May2002	Andrew Morton
 *		Initial version
 * 27Jun2002	axboe@suse.de
 *		use bio_add_page() to build bio's just the right size
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/kdev_t.h>
#include <linux/gfp.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/prefetch.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/cleancache.h>

/*
 * I/O completion handler for multipage BIOs.
 *
 * The mpage code never puts partial pages into a BIO (except for end-of-file).
 * If a page does not map to a contiguous run of blocks then it simply falls
 * back to block_read_full_page().
 *
 * Why is this?  If a page's completion depends on a number of different BIOs
 * which can complete in any order (or at the same time) then determining the
 * status of that page is hard.  See end_buffer_async_read() for the details.
 * There is no point in duplicating all that complexity.
 */
static void mpage_end_io(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;
	struct compressed_bio *cb = bio->bi_private;
	struct page *page;
	int page_idx, ret = 0;
	
	printk(KERN_INFO "\n==> IN MPAGE_END_IO");
	do {
		struct page *page = bvec->bv_page;		
		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);

		if (uptodate) {
			SetPageUptodate(page);
		} else {
			ClearPageUptodate(page);
			SetPageError(page);
		}
		unlock_page(page);
			
	} while (bvec >= bio->bi_io_vec);

	if (!atomic_dec_and_test(&cb->pending_bios))
		goto out;
	
	/* Last bio...start decompression */
	ret = decompress_stride(cb);
	
	for (page_idx = 0; page_idx < cb->nr_pages; page_idx++) {
		page = cb->compressed_pages[page_idx];
		page->mapping = NULL;
		page_cache_release(page);
	}
	
	kfree(cb->compressed_pages);
	kfree(cb);

out:
	bio_put(bio);
}

static struct bio *mpage_bio_submit(int rw, struct bio *bio)
{
	struct compressed_bio *cb = bio->bi_private;
	atomic_inc(&cb->pending_bios);
	bio->bi_end_io = mpage_end_io;
	submit_bio(rw, bio);
	return NULL;
}

static struct bio *
mpage_alloc(struct block_device *bdev,
		sector_t first_sector, int nr_vecs,
		gfp_t gfp_flags)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, nr_vecs);

	if (bio == NULL && (current->flags & PF_MEMALLOC)) {
		while (!bio && (nr_vecs /= 2))
			bio = bio_alloc(gfp_flags, nr_vecs);
	}
	assert(bio);
	
	if (bio) {
		bio->bi_bdev = bdev;
		bio->bi_sector = first_sector;
	}
	return bio;
}

/*
 * support function for mpage_readpages.  The fs supplied get_block might
 * return an up to date buffer.  This is used to map that buffer into
 * the page, which allows readpage to avoid triggering a duplicate call
 * to get_block.
 *
 * The idea is to avoid adding buffers to pages that don't already have
 * them.  So when the buffer is up to date and the page size == block size,
 * this marks the page up to date instead of adding new buffers.
 */
static void 
map_buffer_to_page(struct page *page, struct buffer_head *bh, int page_block) 
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *page_bh, *head;
	int block = 0;

	if (!page_has_buffers(page)) {
		/*
		 * don't make any buffers if there is only one buffer on
		 * the page and the page just needs to be set up to date
		 */
		if (inode->i_blkbits == PAGE_CACHE_SHIFT && 
		    buffer_uptodate(bh)) {
			SetPageUptodate(page);    
			return;
		}
		create_empty_buffers(page, 1 << inode->i_blkbits, 0);
	}
	head = page_buffers(page);
	page_bh = head;
	do {
		if (block == page_block) {
			page_bh->b_state = bh->b_state;
			page_bh->b_bdev = bh->b_bdev;
			page_bh->b_blocknr = bh->b_blocknr;
			break;
		}
		page_bh = page_bh->b_this_page;
		block++;
	} while (page_bh != head);
}

/*
 * This is the worker routine which does all the work of mapping the disk
 * blocks and constructs largest possible bios, submits them for IO if the
 * blocks are not contiguous on the disk.
 *
 * We pass a buffer_head back and forth and use its buffer_mapped() flag to
 * represent the validity of its disk mapping and to decide when to do the next
 * get_block() call.
 */
static struct bio *
do_mpage_readpage(struct bio *bio, struct page *page, unsigned nr_pages,
		sector_t *last_block_in_bio, struct buffer_head *map_bh,
		  unsigned long *first_logical_block, struct compressed_bio **cb, get_block_t get_block)
{
	struct inode *inode = page->mapping->host;
	const unsigned blkbits = inode->i_blkbits;
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits; //SET TO 1
	const unsigned blocksize = 1 << blkbits;
	sector_t block_in_file;
	sector_t last_block;
	sector_t last_block_in_file;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;                                         //Increments to 1
	unsigned first_hole = blocks_per_page;
	struct block_device *bdev = NULL;
	int fully_mapped = 1;
	unsigned nblocks;
	unsigned relative_block;
	int err;
	/* blkbits = 12 | MAX_BUF_PER_PAGE = 8 */
	
	if (page_has_buffers(page))
		goto confused;

	/* block_in_file : page->index
	 * last_block    : last page->index of requested nr_pages
 	 * last_block_in_file  : always index of last_page_of_file
	 */
	block_in_file = (sector_t)page->index << (PAGE_CACHE_SHIFT - blkbits);
	last_block = block_in_file + nr_pages * blocks_per_page;
	last_block_in_file = (i_size_read(inode) + blocksize - 1) >> blkbits;
	if (last_block > last_block_in_file)
		last_block = last_block_in_file;
	page_block = 0;

	/*
	 * Map blocks using the result from the previous get_blocks call first.
	 */
	
	/* nblocks : Initially 0 | Later mapped to 1 extent so is mostly 16 */
	nblocks = map_bh->b_size >> blkbits;
	printk(KERN_INFO "\ncurrent_page : %Lu | nblocks_initial : %u", block_in_file, nblocks);

	if (buffer_mapped(map_bh) && block_in_file > *first_logical_block &&
			block_in_file < (*first_logical_block + nblocks)) {
		unsigned map_offset = block_in_file - *first_logical_block;
		unsigned last = nblocks - map_offset;

		for (relative_block = 0; ; relative_block++) {
			if (relative_block == last) {
				clear_buffer_mapped(map_bh);
				break;
			}
			if (page_block == blocks_per_page)
				break;
			blocks[page_block] = map_bh->b_blocknr + map_offset +
						relative_block;

			page_block++;
			block_in_file++;
		}
		bdev = map_bh->b_bdev;
	}
	
	/*
	 * Then do more get_blocks calls until we are done with this page.
	 */
	map_bh->b_page = page;
	while (page_block < blocks_per_page) {
		map_bh->b_state = 0;
		map_bh->b_size = 0;

		if (block_in_file < last_block) {
			map_bh->b_size = (last_block - block_in_file) << blkbits;
			/* use of get_block => ***needs buffer_head map_bh 
			 * bdev     = map_bh->b_dev
			 * physical = map_bh->b_blocknr
			 * nblocks  = map_bh->b_size (no of logical blocks in extent)
			 * compress_count = map_bh->b_private
			 * first_logical_block
			 */
			if (get_block(inode, block_in_file, map_bh, 0)) //BLOCK_MAPPER
				goto confused;
			*first_logical_block = block_in_file;
		}
		/* generally is mapped.. so FALSE */
		if (!buffer_mapped(map_bh)) {
			fully_mapped = 0;
			if (first_hole == blocks_per_page)
				first_hole = page_block;
			page_block++;
			block_in_file++;
			continue;
		}

		/* some filesystems will copy data into the page during
		 * the get_block call, in which case we don't want to
		 * read it again.  map_buffer_to_page copies the data
		 * we just collected from get_block into the page's buffers
		 * so readpage doesn't have to repeat the get_block call
		 */
		
		//NEXT 3 => FALSE
		if (buffer_uptodate(map_bh)) {
			printk("\nIn map_buffer_to_page()");
			map_buffer_to_page(page, map_bh, page_block);
			goto confused;
		}
	
		if (first_hole != blocks_per_page)
			goto confused;		/* hole -> non-hole */

		/* Contiguous blocks? */
		if (page_block && blocks[page_block-1] != map_bh->b_blocknr-1)
			goto confused;

		nblocks = map_bh->b_size >> blkbits;
		printk(KERN_INFO "\nnblocks_mapped : %u", nblocks);
		//MAIN PART
		if (!*cb) {
			*cb = kzalloc(sizeof(struct compressed_bio), GFP_NOFS);
			if (!*cb) {
				/* ERROR */
				BUG_ON(1);
				err = -ENOMEM;
			}
			err = compressed_bio_init(*cb, inode, *first_logical_block, *(unsigned *)map_bh->b_private,
						  nblocks << PAGE_CACHE_SHIFT, 0);//compressed_len = 0
			if (err) {
				/* ERROR = -ENOMEM */
				err = -ENOMEM;
			}
			kfree((unsigned *)map_bh->b_private);
			map_bh->b_private = NULL;
		}
		
		for (relative_block = 0; ; relative_block++) {
			if (relative_block == nblocks) {
				clear_buffer_mapped(map_bh);
				break;
			} else if (page_block == blocks_per_page)
				break;

			blocks[page_block] = map_bh->b_blocknr+relative_block;
			page_block++;
			block_in_file++;
		}
		bdev = map_bh->b_bdev;
	}

	if (first_hole != blocks_per_page) {
		zero_user_segment(page, first_hole << blkbits, PAGE_CACHE_SIZE);
		if (first_hole == 0) {
			SetPageUptodate(page);
			unlock_page(page);
			goto out;
		}
	} else if (fully_mapped) {
                //TRUE...REQ?
		SetPageMappedToDisk(page);
	}
	
	if (fully_mapped && blocks_per_page == 1 && !PageUptodate(page) &&
	    cleancache_get_page(page) == 0) {
		//FALSE
		printk(KERN_INFO "\nSet_Page_Uptodate");
		SetPageUptodate(page);
		goto confused;
	}
	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
/* 	if (bio && (*last_block_in_bio != blocks[0] - 1)) */
/* 		bio = mpage_bio_submit(READ, bio); */

/* alloc_new: */
/* 	if (bio == NULL) { */
/* 		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9), */
/* 			  	min_t(int, nr_pages, bio_get_nr_vecs(bdev)), */
/* 				GFP_KERNEL); */
/* 		if (bio == NULL) */
/* 			goto confused; */
/* 	} */

/* 	length = first_hole << blkbits; */
/* 	if (bio_add_page(bio, page, length, 0) < length) { */
/* 		bio = mpage_bio_submit(READ, bio); */
/* 		goto alloc_new; */
/* 	} */

/* 	relative_block = block_in_file - *first_logical_block; */
/* 	nblocks = map_bh->b_size >> blkbits; */
/* 	if ((buffer_boundary(map_bh) && relative_block == nblocks) || */
/* 	    (first_hole != blocks_per_page)) */
/* 		bio = mpage_bio_submit(READ, bio); */
/* 	else */
/* 		*last_block_in_bio = blocks[blocks_per_page - 1]; */
	
out:
	return bio;

confused:
	printk(KERN_INFO "\nCONFUSED !");
	if (bio)
		bio = mpage_bio_submit(READ, bio);
	if (!PageUptodate(page))
	        block_read_full_page(page, get_block);
	else
		unlock_page(page);
	goto out;
}

unsigned readahead(struct address_space *mapping, struct list_head *pages,
		   unsigned nr_to_read, unsigned long start_index, unsigned nr_pages)
{
	struct page *page;
	unsigned page_idx;
	pgoff_t page_offset;
	loff_t isize = i_size_read(mapping->host);
	unsigned long end_index = ((isize - 1) >> PAGE_CACHE_SHIFT);

	printk(KERN_INFO "\nReadahead %u pages from INDEX : %lu", nr_to_read, start_index);
	for (page_idx = 0; page_idx < nr_to_read; page_idx++) {
		page_offset = start_index + page_idx;

		if (page_offset > end_index)
			break;

		rcu_read_lock();
		page = radix_tree_lookup(&mapping->page_tree, page_offset);
		rcu_read_unlock();
		if (page)
			continue;

		page = page_cache_alloc_readahead(mapping);
		if (!page) {
			printk(KERN_INFO "Page Readahead Failed");
			break;
		}
		page->index = page_offset;
		list_add(&page->lru, pages);
		if (page_idx == nr_to_read)
			SetPageReadahead(page);
		nr_pages++;
	}
	return nr_pages;
}

/**
 * mpage_readpages - populate an address space with some pages & start reads against them
 * @mapping: the address_space
 * @pages: The address of a list_head which contains the target pages.  These
 *   pages have their ->index populated and are otherwise uninitialised.
 *   The page at @pages->prev has the lowest file offset, and reads should be
 *   issued in @pages->prev to @pages->next order.
 * @nr_pages: The number of pages at *@pages
 * @get_block: The filesystem's block mapper function.
 *
 * This function walks the pages and the blocks within each page, building and
 * emitting large BIOs.
 *
 * If anything unusual happens, such as:
 *
 * - encountering a page which has buffers
 * - encountering a page which has a non-hole after a hole
 * - encountering a page with non-contiguous blocks
 *
 * then this code just gives up and calls the buffer_head-based read function.
 * It does handle a page which has holes at the end - that is a common case:
 * the end-of-file on blocksize < PAGE_CACHE_SIZE setups.
 *
 * BH_Boundary explanation:
 *
 * There is a problem.  The mpage read code assembles several pages, gets all
 * their disk mappings, and then submits them all.  That's fine, but obtaining
 * the disk mappings may require I/O.  Reads of indirect blocks, for example.
 *
 * So an mpage read of the first 16 blocks of an ext2 file will cause I/O to be
 * submitted in the following order:
 * 	12 0 1 2 3 4 5 6 7 8 9 10 11 13 14 15 16
 *
 * because the indirect block has to be read to get the mappings of blocks
 * 13,14,15,16.  Obviously, this impacts performance.
 *
 * So what we do it to allow the filesystem's get_block() function to set
 * BH_Boundary when it maps block 11.  BH_Boundary says: mapping of the block
 * after this one will require I/O against a block which is probably close to
 * this one.  So you should push what I/O you have currently accumulated.
 *
 * This all causes the disk requests to be issued in the correct order.
 */

int
mpage_readpages_compressed(struct address_space *mapping, struct list_head *pages,
				unsigned nr_pages, get_block_t get_block)
{
	struct bio *bio = NULL;
	struct inode *inode = mapping->host;
	const unsigned blkbits = inode->i_blkbits;
	unsigned page_idx, count, nr_to_read;
	sector_t last_block_in_bio = 0;
	struct buffer_head map_bh;
	unsigned long first_logical_block = 0;
	struct compressed_bio *cb;
	struct page *page;
	
	loff_t isize = i_size_read(inode);
	unsigned long prev_index = 0, end_index = ((isize - 1) >> PAGE_CACHE_SHIFT);
	struct list_head *list;

again:
	list = pages->prev;
	for (page_idx = 0; page_idx < nr_pages; page_idx++) {
		page = list_entry(list, struct page, lru);
		prev_index = page->index;
		list = list->prev;
	}
	if (prev_index == end_index || nr_pages >= COMPRESSION_STRIDE_LEN)
		goto start;
		
	/* Start Readahead : mm/readahead.c*/
	prev_index++;
	nr_to_read = COMPRESSION_STRIDE_LEN - nr_pages;
	printk(KERN_INFO "Start Readahead for %u pages", nr_to_read);
	for (page_idx = 0; page_idx < nr_to_read; page_idx++) {
		pgoff_t page_offset = prev_index + page_idx;

		if (page_offset > end_index)
			break;

		rcu_read_lock();
		page = radix_tree_lookup(&mapping->page_tree, page_offset);
		rcu_read_unlock();
		if (page)
			continue;

		page = page_cache_alloc_readahead(mapping);
		if (!page) {
			printk(KERN_INFO "Page Readahead Failed");
			break;
		}
		page->index = page_offset;
		list_add(&page->lru, pages);
		if (page_idx == nr_to_read)
			SetPageReadahead(page);
		nr_pages++;
	}
				
start:
	cb = NULL;
	map_bh.b_state = 0;
	map_bh.b_size = 0;
	printk(KERN_INFO "\n\n==> IN MPAGE_READPAGES | nr_pages : %u", nr_pages);
	
	/* for case when nr_pages > COMPRESSION_STRIDE_LEN...we use goto again */
	count = min_t(unsigned, nr_pages, COMPRESSION_STRIDE_LEN); 
	for (page_idx = 0; page_idx < count; page_idx++) {		
		if (list_empty(pages->prev))
			break;
		
		page = list_entry(pages->prev, struct page, lru);
		prefetchw(&page->flags);
		list_del(&page->lru);
		
		if (!add_to_page_cache_lru(page, mapping,
					page->index, GFP_KERNEL)) {
			
			/* first_logical   : first_logical_block_of_extent
			 * last_blk_in_bio : increments to last physical of bio
			 */
			printk(KERN_INFO "\n IN DO_MPAGE_READPAGE");
			bio = do_mpage_readpage(bio, page,
						count - page_idx,
						&last_block_in_bio, &map_bh,
						&first_logical_block, &cb,
						get_block);//nr_pages -> count
			
			/* restrict count to size of logical extent | nblocks */
			count = map_bh.b_size >> blkbits;		     		
			printk(KERN_INFO "\n OUT DO_MPAGE_READPAGE");
		}
		page_cache_release(page);
	}
	printk(KERN_INFO "\n\n==>OUT MPAGE_READPAGES | first_logical : %lu",first_logical_block);

	/* create and submit bio for compressed_read */
	for (page_idx = 0; page_idx < cb->nr_pages; page_idx++) {
		page = alloc_page(GFP_NOFS |__GFP_HIGHMEM);
		page->mapping = NULL;
		page->index = cb->start + page_idx;
		cb->compressed_pages[page_idx] = page;
		
		/* Try to add pages to exists bio */
		if (!bio || !bio_add_page(bio, page, PAGE_CACHE_SIZE, 0)) {
			/* Couldn't add. So submit old bio and allocate new bio */
			if (bio)
				bio = mpage_bio_submit(READ, bio);

			bio = mpage_alloc(map_bh.b_bdev, (map_bh.b_blocknr + page_idx) << (blkbits - 9),
					  min_t(int, cb->nr_pages - page_idx, bio_get_nr_vecs(map_bh.b_bdev)), 
					  GFP_NOFS); 
			bio->bi_private = cb;
			
			if (!bio_add_page(bio, page, PAGE_CACHE_SIZE, 0))
				assert(0);	/* why? */
		}		
	}
	
	if (bio)
		bio = mpage_bio_submit(READ, bio);

	nr_pages -= count;
	if(nr_pages > 0)
		goto again;
	
	BUG_ON(!list_empty(pages));
	return 0;
}

/*
 * This isn't called much at all
 */
int mpage_readpage(struct page *page, get_block_t get_block)
{
	struct bio *bio = NULL;
	sector_t last_block_in_bio = 0;
	struct buffer_head map_bh;
	struct compressed_bio *cb = NULL;
	unsigned long first_logical_block = 0;

	map_bh.b_state = 0;
	map_bh.b_size = 0;
	bio = do_mpage_readpage(bio, page, 1, &last_block_in_bio,
				&map_bh, &first_logical_block, &cb, get_block);
	if (bio)
		mpage_bio_submit(READ, bio);
	return 0;
}
