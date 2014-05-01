#ifndef __TUX3_COMPRESSION_
#define __TUX3_COMPRESSION_

#define COMPRESSION_STRIDE_LEN 16

int compressed_bio_init(struct compressed_bio *cb, struct inode *inode, block_t start,
			unsigned nr_pages, unsigned len, unsigned compressed_len);
int compress_stride(struct bufvec *bufvec);
int decompress_stride(struct compressed_bio *cb);

#endif
