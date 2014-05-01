tux3-kernel
===========
Tux3 Filesystem with Transparent Compression

Refer Tux3 code from - https://github.com/OGAWAHirofumi/tux3

Files added :

newDefines.h - Enable/Disable Transparent Compression and Log 
mpage_compress.c - Modified mpage.c for compressed read 
compression.c
compression.h

Changes in Data Structures :

Added struct compressed_bio - store compressed data & metadata 
Modified struct bufvec & struct block_segment 
Current default COMPRESSION_STRIDE_LEN is 16, defined in compression.h

Compressed Write Path : compress_stride() bufvec_compressed_io() compressed_end_io()

Compressed Read Path : mpage_readpages_compressed() mpage_end_io() decompress_stride()

Test:

Set ENABLE_TRANSPARENT_COMPRESSION in newDefines.h make insmod tux3.ko

Download Tux3 code for mkfs 
$ ./tux3 mkfs <device>

Mount and test!
