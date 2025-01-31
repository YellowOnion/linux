// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Squashfs - a compressed read only filesystem for Linux
 *
 * Copyright (c) 2010 LG Electronics
 * Chan Jeong <chan.jeong@lge.com>
 *
 * lzo_wrapper.c
 */

#include <linux/mutex.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/lzo.h>

#include "squashfs_fs.h"
#include "squashfs_fs_sb.h"
#include "squashfs.h"
#include "decompressor.h"
#include "page_actor.h"

struct squashfs_lzo {
	void	*input;
	void	*output;
};

static void *lzo_init(struct squashfs_sb_info *msblk, void *buff)
{
	int block_size = max_t(int, msblk->block_size, SQUASHFS_METADATA_SIZE);

	struct squashfs_lzo *stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (stream == NULL)
		goto failed;
	stream->input = vmalloc(block_size);
	if (stream->input == NULL)
		goto failed;
	stream->output = vmalloc(block_size);
	if (stream->output == NULL)
		goto failed2;

	return stream;

failed2:
	vfree(stream->input);
failed:
	ERROR("Failed to allocate lzo workspace\n");
	kfree(stream);
	return ERR_PTR(-ENOMEM);
}


static void lzo_free(void *strm)
{
	struct squashfs_lzo *stream = strm;

	if (stream) {
		vfree(stream->input);
		vfree(stream->output);
	}
	kfree(stream);
}


static int lzo_uncompress(struct squashfs_sb_info *msblk, void *strm,
	struct bio *bio, int offset, int length,
	struct squashfs_page_actor *output)
{
	struct bvec_iter_all iter;
	struct bio_vec bvec;
	struct squashfs_lzo *stream = strm;
	void *buff = stream->input, *data;
	int bytes = length, res;
	size_t out_len = output->length;

	bvec_iter_all_init(&iter);
	bio_iter_all_advance(bio, &iter, offset);

	bio_for_each_segment_all_continue(bvec, bio, iter) {
		unsigned avail = min_t(unsigned, bytes, bvec.bv_len);

		memcpy(buff, bvec_virt(&bvec), avail);
		buff += avail;
		bytes -= avail;
		if (!bytes)
			break;
	}

	res = lzo1x_decompress_safe(stream->input, (size_t)length,
					stream->output, &out_len);
	if (res != LZO_E_OK)
		goto failed;

	res = bytes = (int)out_len;
	data = squashfs_first_page(output);
	buff = stream->output;
	while (data) {
		if (bytes <= PAGE_SIZE) {
			if (!IS_ERR(data))
				memcpy(data, buff, bytes);
			break;
		} else {
			if (!IS_ERR(data))
				memcpy(data, buff, PAGE_SIZE);
			buff += PAGE_SIZE;
			bytes -= PAGE_SIZE;
			data = squashfs_next_page(output);
		}
	}
	squashfs_finish_page(output);

	return res;

failed:
	return -EIO;
}

const struct squashfs_decompressor squashfs_lzo_comp_ops = {
	.init = lzo_init,
	.free = lzo_free,
	.decompress = lzo_uncompress,
	.id = LZO_COMPRESSION,
	.name = "lzo",
	.alloc_buffer = 0,
	.supported = 1
};
