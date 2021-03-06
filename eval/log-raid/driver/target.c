/****************************************************************************
 * SRC (SSD RAID Cache): Device mapper target for block-level disk caching
 * Yongseok Oh (ysoh@uos.ac.kr) 2013 - 2014
 * filename: target.c 
 * 
 * Based on DM-Writeboost:
 *   Log-structured Caching for Linux
 *   Copyright (C) 2012-2013 Akira Hayakawa <ruby.wktk@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#include <linux/buffer_head.h>
#include <linux/dmaengine.h>
#include <linux/raid/xor.h>
#include <linux/raid/pq.h>
#include <linux/dm-kcopyd.h>
#include <linux/blkdev.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/crc32.h>
#include <linux/gfp.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/jhash.h>
#include "target.h"
#include "metadata.h"
#include "daemon.h"
#include "lru.h"
#include "alloc.h"

/*----------------------------------------------------------------*/


int dmsrc_io(struct dm_io_request *io_req, unsigned num_regions,
	  struct dm_io_region *where, unsigned long *sync_error_bits)
{
	return dm_io(io_req, num_regions, where, sync_error_bits);
}


sector_t dmsrc_devsize_sectors(struct dm_dev *dev)
{
	return i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT;
}


void prepare_chunk_summary(
					struct dmsrc_super *super,
					struct segment_header *seg,
					struct rambuf_page **pages,
					int ssd_id,
					 int cache_type
					 )
{
	struct segment_header_device *dest;
	u32 chunk_start;
	u32 i;
	u64 sequence;
	u32 summary_offset;

	summary_offset = get_summary_offset(super, ssd_id);
	chunk_start = ssd_id * CHUNK_SZ;

	sequence = atomic64_inc_return(&super->cache_stat.alloc_sequence);
	seg->sequence = sequence;

	//dest = (struct segment_header_device *)pages[summary_offset]->data;
	dest = (struct segment_header_device *)page_address(pages[summary_offset]->pl->page);
	dest->sequence = cpu_to_le64(seg->sequence);
	dest->uuid = super->cache_stat.uuid;
	dest->magic = SRC_MAGIC;
	dest->type = cache_type;

	if(cache_type==GWBUF)
		dest->type = WCBUF;
	else if(cache_type==GRBUF)
		dest->type = RCBUF;

	for (i = 0; i < CHUNK_SZ; i++) {
		struct metablock *mb = get_mb(super, seg->seg_id, chunk_start + i);
		struct metablock_device *mbdev; 
		u32 meta_page = summary_offset + 1 + i/NUM_ENTRY_PER_PAGE;
		u32 meta_offset = i % NUM_ENTRY_PER_PAGE;
	 
		mbdev = (struct metablock_device *)page_address(pages[meta_page]->pl->page);

		if(!chunk_summary_range(super, i)){
			if(cache_type==GWBUF){
				if(mb->sector==~0){
					printk(" seg = %d, ssd = %d, offset = %d sector = %d \n", (int)seg->seg_id, ssd_id, i, (int)mb->sector);
					printk(" check .. \n");
					//BUG_ON(mb->sector==~0);
				}
			}
			mbdev[meta_offset].sector = mb->sector;
			mbdev[meta_offset].dirty_bits = test_bit(MB_DIRTY, &mb->mb_flags);
		}else{ // summary 
			mbdev[meta_offset].sector = ~0;
			mbdev[meta_offset].dirty_bits = 0;
		}
	}

#ifdef USE_CHECKSUM
	for (i = 0; i < CHUNK_SZ; i++) {
		struct metablock *mb = get_mb(super, seg->seg_id, chunk_start + i);
		struct metablock_device *mbdev = &dest->mbarr[i];
		mbdev->checksum = mb->checksum;
	}
#endif 
}



#if 0
void generate_full_summary(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuffer *rambuf, int ssd_id, int full){
	struct blk_plug plug;
	int i;

	prepare_chunk_summary(super, seg, rambuf->pages, ssd_id, seg->seg_type);

	blk_start_plug(&plug);
	for(i = 0;i < NUM_SUMMARY;i++){
		struct wb_job *job;
		//u32 summary_mb_idx = (ssd_id + 1) * CHUNK_SZ - NUM_SUMMARY + i;
		u32 summary_mb_idx = get_summary_offset(super, ssd_id) + i;
		job = writeback_make_job(super, seg, summary_mb_idx, NULL, rambuf, full);
		writeback_issue_job(super, job);
	}
	blk_finish_plug(&plug);
}
#endif 


int change_seg_status(struct dmsrc_super *super, struct segment_header *seg, int seg_length, int force_seal){
	struct group_header *group;
	unsigned long flags;
	int group_sealed = 0;
	//unsigned long f;

#if  0
	LOCK(super, f);
	atomic_add(atomic_read(&seg->part_length), &seg->part_start);
	atomic_set(&seg->part_length, 0);
	UNLOCK(super, f);
#endif 

	if(force_seal || seg_length>=STRIPE_SZ){

		if(test_bit(SEG_PARTIAL, &seg->flags)){
			printk(" force seal ... seg .. = %d \n", (int)seg->seg_id);
		}

#if 0
		if(calc_meta_count(super, seg)!=get_metadata_count(super, seg->seg_type)){
			printk(" *sealed: seg = %d valid = %d, metacount = %d  \n", (int)seg->seg_id, atomic_read(&seg->valid_count),
					calc_meta_count(super, seg));
		}

		LOCK(super, f);
		if(atomic_read(&seg->valid_count)-get_metadata_count(super, seg->seg_type)!=
				calc_valid_count(super, seg, 1)){
			printk(" invalid valid count = %d %d, dummy = %d \n",  
					atomic_read(&seg->valid_count)-get_metadata_count(super, seg->seg_type), 
					calc_valid_count(super, seg, 1), 
					atomic_read(&seg->dummy_count));
		}
		UNLOCK(super, f);
#endif 

		//if(seg->seg_type==RCBUF){
		//	printk(" Sealed seg  valid count = %d %d, dummy = %d \n",  
		//			atomic_read(&seg->valid_count)-get_metadata_count(super, seg->seg_type), 
		//			calc_valid_count(super, seg, 1), 
		//			atomic_read(&seg->dummy_count));
		//}

		lockseg(seg, flags);
		if(seg->seg_type==GWBUF){
			atomic_dec(&super->wstat.seg_count[GWBUF]);
			atomic_inc(&super->wstat.seg_count[WCBUF]);
			seg->seg_type = WCBUF;
		}else if(seg->seg_type==GRBUF){
			atomic_dec(&super->wstat.seg_count[GRBUF]);
			atomic_inc(&super->wstat.seg_count[RCBUF]);
			seg->seg_type = RCBUF;
		}
		unlockseg(seg, flags);

		if(test_bit(SEG_USED, &seg->flags)||test_bit(SEG_PARTIAL, &seg->flags)){
			set_bit(SEG_SEALED, &seg->flags);
			clear_bit(SEG_USED, &seg->flags);
			clear_bit(SEG_PARTIAL, &seg->flags);
			move_seg_used_to_sealed_queue(super, seg);

			group = &super->group_header_array[seg->seg_id/SEGMENT_GROUP_SIZE];
			if(atomic_inc_return(&group->sealed_seg_count)==SEGMENT_GROUP_SIZE){
				//printk(" insert group = %d to sealed queue valid count = %d\n", group->group_id, atomic_read(&group->valid_count));
				move_group_used_to_sealed_queue(super, group);
				group_sealed = 1;
			}
		}
	}else{
		if(test_bit(SEG_PARTIAL, &seg->flags)){
			set_bit(SEG_USED, &seg->flags);
			clear_bit(SEG_PARTIAL, &seg->flags);
		}
	}

	BUG_ON(atomic_read(&seg->length)>STRIPE_SZ);
	return group_sealed;

}

#if 0
void build_summary_job(struct dmsrc_super *super, 
		struct segment_header *seg, 
		struct rambuffer *rambuf, 
		int force_seal){
	int i;

	if(atomic_read(&seg->part_length) || 
		(force_seal && atomic_read(&seg->length)))
	{
		int full = 0;

		if(force_seal || need_refresh_segment(super, seg->seg_type, atomic_read(&seg->length)))
			full = 1;
		else
			full = 0;

		for(i = 0;i < NUM_SSD;i++){
			if(USE_ERASURE_PARITY(&super->param) && is_write_stripe(seg->seg_type)){
				if(i==get_parity_ssd(super, seg->seg_id))
					continue;
			}

			generate_full_summary(super, seg, rambuf, i, full);	
		}
		BUG_ON(atomic_read(&seg->length)>STRIPE_SZ);
	}
}
#endif 



#if 0
void generate_parity_io(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuffer  *rambuf,
		int full){

	struct dm_io_request io_req;
	struct dm_io_region region;
	u32 mem_offset = 0;
	u32 tmp32;
	int parity_size = CHUNK_SZ;
	int parity_start;
	int i;

	if(NO_USE_ERASURE_CODE(&super->param))
		return;

	parity_start = cursor_parity_start(super, seg->seg_id, seg->seg_type);
	for(i = 0;i <parity_size;i++){
		u32 offset = parity_start + i;
		struct metablock *mb = get_mb(super, seg->seg_id, offset); 
		struct wb_job *job;

		if(!test_bit(MB_PARITY_NEED, &mb->mb_flags)){
			if(i < 5)
			printk(" partial parity (unsupported) data seg = %d, idx = %d, refcount = %d, %d\n", 
				(int)seg->seg_id, (int)mb->idx%STRIPE_SZ, (int)atomic_read(&rambuf->ref_count), full);
		//	continue;
		}

		//printk(" parity data seg = %d, idx = %d, refcount = %d, %d\n", 
		//		(int)seg->seg_id, (int)mb->idx%STRIPE_SZ, (int)atomic_read(&rambuf->ref_count), full);

		job = writeback_make_job(super, seg, mb->idx, NULL, rambuf, full);
		writeback_issue_job(super, job);
	}

	if(USE_ERASURE_PARITY(&super->param))
		return;

	for(i = 0;i <parity_size;i++){
		u32 offset = (parity_start + CHUNK_SZ + i) % STRIPE_SZ;
		struct metablock *mb = get_mb(super, seg->seg_id, offset); 

		printk(" RAID6 is not supported. \n");
		BUG_ON(1);
		if(!test_bit(MB_PARITY_NEED, &mb->mb_flags))
			continue;

		//atomic_inc(&(job->count));

		div_u64_rem(mb->idx, STRIPE_SZ, &tmp32);
		mem_offset = tmp32;

		region.bdev = get_bdev(super, offset);
		region.sector = get_sector(super, seg->seg_id, offset);
		region.count = SRC_SECTORS_PER_PAGE; 

		io_req.client = super->io_client;
		io_req.bi_rw = WRITE;
		io_req.notify.fn = flush_segmd_endio;
		///io_req.notify.context = job;
		io_req.mem.type = DM_IO_KMEM;
		io_req.mem.ptr.addr = rambuf->pages[mem_offset]->data;

		dmsrc_io(&io_req, 1, &region, NULL);
	}
}
#endif 

#if 0
void build_parity_job(struct dmsrc_super *super, 
		struct segment_header *seg, 
		struct rambuffer *rambuf, 
		u32 seg_length,
		int cache_type,
		int force_seal)
{
	int full_seg = 0;

	if(is_read_stripe(seg->seg_type))
		return;

	if(force_seal || need_refresh_segment(super, cache_type, seg_length))
		full_seg = 1;

	generate_parity_data(super, seg, rambuf->pages, full_seg, cache_type);
	generate_parity_io(super, seg, rambuf, full_seg);

	if(force_seal || seg_length==STRIPE_SZ){
		reset_parity(super, seg, seg->seg_type, 1); // full stripe 
	}else{
		reset_parity(super, seg, seg->seg_type, 0); // partial stripe 
	}
}
#endif 

void alloc_summary(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuffer *rambuf, u32 summary_idx, int cache_type, int full){
	u32 j;

	alloc_mb_summary(super, seg, cache_type, summary_idx, NULL);
	initialize_mb_summary(super, seg, cache_type, summary_idx, full);
	for(j = 0;j < NUM_SUMMARY;j++)
		bio_plugging(super, seg, rambuf, NULL, summary_idx + j);
}

u32 alloc_skip(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuffer *rambuf, int cache_type){
	u32 dummy_idx;

//	tmp_idx = ma_alloc(super, seg, seg->seg_id, cache_type, (dummy_idx/CHUNK_SZ)%NUM_SSD, NULL);
//	if(tmp_idx!=dummy_idx){
//		printk(" tmp_idx %d, dummy_idx = %d \n", tmp_idx, dummy_idx);
//		BUG_ON(1);
//	}
	dummy_idx = alloc_next_mb(super, seg, 0, cache_type, NULL);
	_alloc_mb_summary(super, seg, cache_type, dummy_idx, 0, 1);
	bio_plugging(super, seg, rambuf, NULL, dummy_idx);
	return dummy_idx;
}


u32 alloc_dummy(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuffer *rambuf, int cache_type){
	u32 dummy_idx;
	dummy_idx = alloc_next_mb(super, seg, 0, cache_type, NULL);
	_alloc_mb_summary(super, seg, cache_type, dummy_idx, 0, 1);
	bio_plugging(super, seg, rambuf, NULL, dummy_idx);
	return dummy_idx;
}

int alloc_partial_summary(struct dmsrc_super *super, 
		struct segment_header *seg, 
		struct rambuffer *rambuf, int cache_type){
	struct group_header *group = seg->group;
	int i;
	int need_full_summary = 0;
	s32 free_blocks = 0;
	s32 summary_blocks = 0;

	s32 num_partial_summary;
	s32 num_updated_blocks;
	s32 summary_bytes;
	u32 row_count = 0, last_row_free_count = 0;
	int num_active_ssd;

	for(i = 0;i < CHUNK_SZ-NUM_SUMMARY;i++){
		row_count = ma_get_row_count(super, seg->seg_type, i);
		free_blocks += (NUM_SSD - row_count);
		if(i==CHUNK_SZ-NUM_SUMMARY-1)
			last_row_free_count = (NUM_SSD - row_count);
	}

	num_updated_blocks = atomic_read(&rambuf->bios_total_count) - atomic_read(&rambuf->bios_total_start);
	summary_bytes = sizeof(struct metablock_device)*num_updated_blocks+1;
	num_partial_summary = summary_bytes/SRC_PAGE_SIZE;
	if(summary_bytes%SRC_PAGE_SIZE)
		num_partial_summary++;

	//printk(" free blocks = %d, last row = %d  num partial summary = %d \n", free_blocks, last_row_free_count, num_partial_summary);

#if 0
	if(free_blocks==0){
		summary_blocks = 0;
		need_full_summary = 1;
	}
	else if(free_blocks-num_partial_summary<last_row_free_count){
		// fill remaining blocks
		//printk(" *** fill remaining blocks with dummy %d \n", free_blocks);
		summary_blocks = free_blocks;
		need_full_summary = 1;
	}else if(free_blocks-num_partial_summary>=last_row_free_count){
		//printk(" ### fill summary blocks %d \n", free_blocks);
		summary_blocks = num_partial_summary;
		need_full_summary = 0;
	}
#else

	if(USE_ERASURE_PARITY(&super->param))
		num_active_ssd = atomic_read(&group->num_used_ssd)-1;
	else
		num_active_ssd = atomic_read(&group->num_used_ssd);

	if(free_blocks<=last_row_free_count){ 
		//summary_blocks = free_blocks;
		summary_blocks = last_row_free_count;
		need_full_summary = 1;
	}else{ // free_blocks > last_row_free_count
		u32 remaining_free_blocks = free_blocks - num_partial_summary;

		if(free_blocks<num_partial_summary){
			summary_blocks = free_blocks;
			need_full_summary = 1;
		}else if(remaining_free_blocks>=last_row_free_count && 
				last_row_free_count == num_active_ssd 
				){
			//printk(" ### fill summary blocks %d \n", free_blocks);
			summary_blocks = num_partial_summary;
			need_full_summary = 0;
		}else if(remaining_free_blocks<last_row_free_count){
			// fill remaining blocks
			//printk(" *** fill remaining blocks with dummy %d \n", free_blocks);
			summary_blocks = free_blocks;
			need_full_summary = 1;
		}else{
			summary_blocks = free_blocks;
			need_full_summary = 1;
			printk(" partial summary special case = %d last free = %d num active ssd %d   \n", free_blocks, last_row_free_count, num_active_ssd);
		}
	}

#endif 

	for(i = 0;i < summary_blocks;i++){
		struct metablock *mb;
		u32 dummy_idx;

#if 0
		dummy_idx = alloc_next_mb(super, seg, 0, seg->seg_type, NULL);
		_alloc_mb_summary(super, seg, cache_type, dummy_idx, 0, 1);
		bio_plugging(super, seg, rambuf, NULL, dummy_idx);
#else
		dummy_idx = alloc_dummy(super, seg, rambuf, seg->seg_type);
#endif 
		mb = get_mb(super, seg->seg_id, dummy_idx%STRIPE_SZ);
		set_bit(MB_DUMMY, &mb->mb_flags);

	}


//	printk(" written summary matotal = %d, valid  %d \n", ma_get_count(super, seg->seg_type), STRIPE_SZ);

	if(!need_full_summary)
		return 0;

	for(i = 0;i < NUM_SSD;i++){
		u32 summary_idx = i * CHUNK_SZ;
		u32 dev_count;

		if(USE_ERASURE_PARITY(&super->param) && is_write_stripe(seg->seg_type)){
			if(i==get_parity_ssd(super, seg->seg_id))
				continue;
		}

		dev_count = ma_get_count_per_dev(super, cache_type, i);
		if(dev_count == CHUNK_SZ)
			continue;

		summary_idx = i * CHUNK_SZ + dev_count;
		//printk(" alloc last summary ... %d \n", summary_idx % CHUNK_SZ);
		alloc_summary(super, seg, rambuf, summary_idx, cache_type, 1);
		atomic_set(&seg->summary_start[i], dev_count+NUM_SUMMARY);
	}

	if(ma_get_count(super, seg->seg_type)!= STRIPE_SZ)
		printk(" Need **full summary  written summary matotal = %d, valid  %d \n", ma_get_count(super, seg->seg_type), STRIPE_SZ);

	return 0;
}

void build_metadata(struct dmsrc_super *super, 
		struct segment_header *seg, 
		int seg_length,
		struct rambuffer *rambuf,
		atomic_t *bios_start,
		atomic_t *bios_count,
		int force_seal,
		int build_summary,
		int flush_data)
{
	int group_sealed;
	int flush_command = 0;

#if SUMMARY_LOCATION == USE_FIRST_SUMMARY 
	if(test_bit(SEG_PARTIAL, &seg->flags))
		alloc_partial_summary(super, seg, rambuf, cache_type);
#endif
	
	group_sealed = change_seg_status(super, seg, seg_length, force_seal);
	if(build_summary){
		printk(" build summary ... \n");
		BUG_ON(1);
		//build_summary_job(super, seg, rambuf, 0);
	}

	if(super->param.flush_command==FLUSH_NONE){
		flush_command = 0;
	}else if(super->param.flush_command==FLUSH_FINE){
		flush_command = 1;
	}else if(super->param.flush_command==FLUSH_COARSE){
		if(group_sealed)
			flush_command = 1;
		else 
			flush_command = 0;
	}

	//if(!group_sealed && seg->seg_type == RCBUF)
	//	flush_command = 0;

	if(flush_data){
		int count;
		int i;

		if(test_bit(SEG_PARTIAL, &seg->flags))
			printk(" Partial flush data ...seg = %d length = %d(bio length = %d) \n", (int)seg->seg_id, 
					(int)atomic_read(&seg->length), 
					(int)atomic_read(&rambuf->bios_total_count));

		for(i = 0;i < NUM_SSD;i++){
			if(USE_ERASURE_PARITY(&super->param) && is_write_stripe(seg->seg_type)){
				if(i==get_parity_ssd(super, seg->seg_id))
					continue;
			}
			prepare_chunk_summary(super, seg, rambuf->pages, i, seg->seg_type);
		}

		count = 0;
		for(i = 0;i < NUM_SSD;i++){
			count += flush_plug_proc(super, seg, rambuf, bios_start, bios_count, 1, i, flush_command);
		}
	}
}


void init_new_segment(struct dmsrc_super *super, u64 next_id, int cache_type){
	struct segment_header *new_seg;
	int i;

	new_seg = get_segment_header_by_id(super, next_id);
	new_seg->seg_type = cache_type;
	new_seg->flags = 0;

	atomic_set(&new_seg->length, 0);
	atomic_set(&new_seg->valid_count, 0);
	atomic_set(&new_seg->valid_clean_count, 0);
	atomic_set(&new_seg->valid_dirty_count, 0);
	atomic_set(&new_seg->dummy_count, 0);
	atomic_set(&new_seg->dirty_count, 0);
	atomic_set(&new_seg->hot_count, 0);
	atomic_set(&new_seg->hot_clean_count, 0);
	atomic_set(&new_seg->part_start, 0);
	atomic_set(&new_seg->part_length, 0);
	atomic_set(&new_seg->bios_count, 0);
	atomic_set(&new_seg->num_bufcopy, 0);
	atomic_set(&new_seg->num_filling, 0);
	atomic_set(&new_seg->num_read_inflight, 0);
	atomic_set(&new_seg->num_migios, 0);
	for(i = 0;i < MAX_CACHE_DEVS;i++)
		atomic_set(&new_seg->summary_start[i], 0);

	clear_bit(SEG_CLEAN, &new_seg->flags);
	clear_bit(SEG_USED, &new_seg->flags);
	clear_bit(SEG_SEALED, &new_seg->flags);
	clear_bit(SEG_MIGRATING, &new_seg->flags);
	clear_bit(SEG_PARTIAL, &new_seg->flags);
	clear_bit(SEG_RECOVERY, &new_seg->flags);
	clear_bit(SEG_HIT, &new_seg->flags);
	clear_bit(SEG_LOCK, &new_seg->flags);

}

inline int need_rambuf_size(struct dmsrc_super *super, int cache_type){
	int need_size;

	if(is_gc_stripe(cache_type)){
		need_size = STRIPE_SZ;
	}else if(is_read_stripe(cache_type) || NO_USE_ERASURE_CODE(&super->param)){
		need_size = STRIPE_SZ;
	}else{
		need_size = STRIPE_SZ;
	}

	return need_size;
}

inline int can_get_free_segment(struct dmsrc_super *super, int cache_type, int gc){
	int need_size;
	struct segment_allocator *seg_allocator = &super->seg_allocator;

	need_size = need_rambuf_size(super, cache_type);

	if(!gc){
		if(atomic_read(&seg_allocator->seg_alloc_count)>=MIGRATE_LOWWATER && 
		   atomic_read(&super->segbuf_mgr.inactive_count) && 
		   atomic_read(&super->segbuf_mgr.inactive_page_count)>=need_size)
		{
			return 1;
		}
	}else{
		if(atomic_read(&seg_allocator->seg_alloc_count)>=2&& 
		   atomic_read(&super->segbuf_mgr.inactive_count) && 
		   atomic_read(&super->segbuf_mgr.inactive_page_count)>=need_size)
		{
			return 1;
		}
	}

	if(atomic_read(&super->segbuf_mgr.inactive_page_count)<need_size)
		printk(" Rambuffer is not enough \n");

	return 0;
}

struct segment_header *_alloc_new_seg(struct dmsrc_super *super, int cache_type){
	struct group_header *group;
	struct segment_header *new_seg = NULL;
	// group allocator ...
	//super->current_group = &super->group_header_array[new_seg->seg_id/SEGMENT_GROUP_SIZE];
	//if(new_seg->seg_id%SEGMENT_GROUP_SIZE==0){
	//	printk(" warning: group free seg = %d, %d\n", 
	//			atomic_read(&super->current_group->free_seg_count), 
	//			SEGMENT_GROUP_SIZE);
		//BUG_ON(atomic_read(&super->current_group->free_seg_count)!=SEGMENT_GROUP_SIZE);
	//}
	//printk("\n alloc new segment = %d, group_free = %d \n", (int)new_seg->seg_id,
	//	(int)atomic_read(&super->current_group->free_seg_count)	
	//		);
	group = super->current_group;
	if(super->current_group==NULL || (super->current_group && atomic_read(&super->current_group->free_seg_count)==0)){

		group = remove_alloc_group_queue(super);
		if(atomic_read(&super->seg_allocator.group_alloc_count)<=1){
			printk("WARN: Alloc seg free = %d \n", (int)get_alloc_count(super));
			printk("WARN: Alloc group free = %d \n", (int)get_group_alloc_count(super));

		}
		if(group==NULL){
			printk("Alloc cur group free = %d \n", (int)atomic_read(&super->seg_allocator.group_alloc_count));
			printk("Alloc seg free = %d \n", (int)get_alloc_count(super));
			printk("Alloc group free = %d \n", (int)get_group_alloc_count(super));
			BUG_ON(1);
		}
		insert_group_to_used_queue(super, group);
		super->current_group = group;
		group->current_seg_id = group->group_id * SEGMENT_GROUP_SIZE-1;
		atomic_set(&group->sealed_seg_count, 0);
		atomic_set(&group->valid_count, 0);

		if((super->param.data_allocation==DATA_ALLOC_FLEX_VERT || 
			 super->param.data_allocation==DATA_ALLOC_FLEX_HORI)){
			group_reserve_empty_chunk(super, group->group_id, cache_type);
		}

		if(super->param.data_allocation==DATA_ALLOC_FLEX_VERT || 
				 super->param.data_allocation==DATA_ALLOC_FLEX_HORI){
		//	atomic_set(&group->num_used_ssd,  NUM_SSD - reserved_chunks);
		//	atomic_set(&group->skewed_segment, 1);
		}else{
			atomic_set(&group->num_used_ssd,  NUM_SSD);
			atomic_set(&group->skewed_segment, 1);
		}
	}
	group->current_seg_id++;
	//atomic_dec(group->free_seg_count);

	//printk(" alloc group = %d seg = %d \n", (int)group->group_id, (int)group->current_seg_id);
	new_seg = get_segment_header_by_id(super, group->current_seg_id);
	remove_alloc_queue(super, new_seg);

	segment_group_dec_free_segs(super, new_seg);

	return new_seg;
}

u64 alloc_new_segment(struct dmsrc_super *super, int cache_type, bool use_migrate)
{
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct segment_header *new_seg;
	struct rambuffer *rambuf;
	struct metablock *mb;
	int reserved_empty_blocks;
	int i;
	int stripe_size;

	BUG_ON(atomic_read(&seg_allocator->seg_alloc_count)<1);

	new_seg = _alloc_new_seg(super, cache_type);
	insert_seg_to_used_queue(super, new_seg);

	init_new_segment(super, new_seg->seg_id, cache_type);

	set_bit(SEG_USED, &new_seg->flags);
	super->current_seg[cache_type] = new_seg;

	stripe_size = ma_reset(super, new_seg->seg_id, cache_type);

	//printk(" stripe size = %d %d \n", stripe_size, STRIPE_SZ);
	rambuf = alloc_rambuffer(super, cache_type, stripe_size);
	super->segbuf_mgr.current_rambuf[cache_type] = rambuf;

	reserved_empty_blocks = get_num_empty_chunks(super, cache_type) * CHUNK_SZ;
	atomic_set(&new_seg->length, reserved_empty_blocks);
	atomic_set(&new_seg->part_length, reserved_empty_blocks);

	for(i = 0;i < STRIPE_SZ;i++){
		mb = get_mb(super, new_seg->seg_id, i);
		mb->sector = ~0;
		mb->mb_flags = 0;
	}

	atomic_inc(&super->wstat.seg_count[cache_type]);

#if SUMMARY_LOCATION == USE_FIRST_SUMMARY 
	for(i = 0;i < NUM_SSD;i++){
		u32 summary_idx = i * CHUNK_SZ;

		if(USE_ERASURE_PARITY(&super->param) && is_write_stripe(new_seg->seg_type)){
			if(i==get_parity_ssd(super, new_seg->seg_id))
				continue;
		}
		alloc_summary(super, new_seg, rambuf, summary_idx, cache_type, 1);
	}
	//printk(" part start = %d, length = %d \n", atomic_read(&new_seg->part_start), 
	//		atomic_read(&new_seg->part_length));
#endif

	atomic_add(atomic_read(&new_seg->part_length), &new_seg->part_start);
	atomic_set(&new_seg->part_length, 0);

	//printk(" alloc new seg = %d valid = %d, length = %d  type = %d \n", (int)new_seg->seg_id, 
	//		atomic_read(&new_seg->valid_count), 
	//		atomic_read(&new_seg->length), cache_type);

	return new_seg->seg_id;
}

#if 0 
void wait_alloc_event(struct dmsrc_super *super, int min){
	while(atomic_read(&super->alloc_count)<min){
		wait_event_interruptible_timeout(super->alloc_wait_queue,
			atomic_read(&super->alloc_count)>=min, usecs_to_jiffies(TIMEOUT_US));
	}
}
#endif

void wait_rambuf_event(struct dmsrc_super *super, int cache_type){
	int need_size = need_rambuf_size(super, cache_type);

	while( !atomic_read(&super->segbuf_mgr.inactive_count) || 
			atomic_read(&super->segbuf_mgr.inactive_page_count)<need_size){

		  wait_event_interruptible_timeout(super->segbuf_mgr.wait_queue,
					atomic_read(&super->segbuf_mgr.inactive_count) && 
					atomic_read(&super->segbuf_mgr.inactive_page_count)>=need_size,
					usecs_to_jiffies(TIMEOUT_US));
	}
}

void make_bios_parity(struct dmsrc_super *super, struct segment_header *seg, struct rambuffer *rambuf, int partial){
	u32 chunk_offset;
	struct metablock *parity_mb;
	struct metablock *dummy_mb;
	unsigned long flags;
	//int parity_ssd = get_parity_ssd(super, seg->seg_id);

	if(!(is_write_stripe(seg->seg_type) && USE_ERASURE_CODE(&super->param)))
		return;

//	printk(" make bios parity .. \n");

	for(chunk_offset = 0;chunk_offset < CHUNK_SZ;chunk_offset++){
		u32 parity_idx = cursor_parity_start(super, seg->seg_id, seg->seg_type) + chunk_offset;
		// including full stripe parity 
		u32 row_count = ma_get_row_count(super, seg->seg_type, parity_idx % CHUNK_SZ);

		parity_mb = get_mb(super, seg->seg_id, parity_idx);

		if(!test_bit(MB_PARITY_NEED, &parity_mb->mb_flags))
			continue;
		if(test_bit(MB_PARITY_WRITTEN, &parity_mb->mb_flags))
			continue;
		if(row_count==1)
			continue;


		if(row_count <  NUM_SSD){
			u32 dummy_idx;// = seg->seg_id * STRIPE_SZ + dummy_ssd * CHUNK_SZ + chunk_offset;

			if(chunk_offset >= CHUNK_SZ-NUM_SUMMARY)
				printk(" ** Partial segment, Invalid ... seg = %d  parity offset = %d....row count = %d, partial = %d  \n", 
						(int)seg->seg_id,
						chunk_offset, row_count, partial);

			if(row_count + 1 >= NUM_SSD){ // [Data][Data][Dummy][]
				if(super->param.aligned_io_dummy==ALIGNED_IO_DUMMY){
					//printk(" write dummy data: parity ... \n");
					dummy_idx = alloc_dummy(super, seg, rambuf, seg->seg_type);
					dummy_mb = get_mb(super, seg->seg_id, dummy_idx%STRIPE_SZ);
					set_bit(MB_DUMMY, &dummy_mb->mb_flags);
				}else{
			//		printk(" skip writint dummy data: parity  ... \n");
					dummy_idx = alloc_skip(super, seg, rambuf, seg->seg_type);
					dummy_mb = get_mb(super, seg->seg_id, dummy_idx%STRIPE_SZ);
					set_bit(MB_SKIP, &dummy_mb->mb_flags);
				}

			}else{ // [Data][Partial parity][ ][ ]
				dummy_idx = alloc_dummy(super, seg, rambuf, seg->seg_type);
				clear_bit(MB_PARITY_NEED, &parity_mb->mb_flags);
				dummy_mb = get_mb(super, seg->seg_id, dummy_idx%STRIPE_SZ);
				set_bit(MB_DUMMY, &dummy_mb->mb_flags);
			}
			row_count = ma_get_row_count(super, seg->seg_type, parity_idx % CHUNK_SZ);
			//printk(" after row count = %d \n", row_count);
		}

		// full stripe parity 
		if(row_count == NUM_SSD){

			bio_plugging(super, seg, rambuf, NULL, parity_idx);

			lockseg(seg, flags);
			clear_bit(MB_DIRTY, &parity_mb->mb_flags);
			clear_bit(MB_VALID, &parity_mb->mb_flags);
			set_bit(MB_PARITY, &parity_mb->mb_flags);
			// FIX ME
			if(test_bit(MB_PARITY_NEED, &parity_mb->mb_flags)){
				set_bit(MB_PARITY_WRITTEN, &parity_mb->mb_flags);
			}
			unlockseg(seg, flags);

			if(test_bit(MB_PARITY_WRITTEN, &parity_mb->mb_flags)){
				atomic_inc(&seg->valid_count);
				atomic_inc(&seg->group->valid_count);
				atomic_inc(&seg->dirty_count);
				atomic_inc(&super->cache_stat.num_used_blocks);
				if(atomic_read(&seg->valid_count) > STRIPE_SZ){
					printk("WARN: segid = %d, parity xor data valid count = %d \n", (int)seg->seg_id,  atomic_read(&seg->valid_count));
				}
			}


		}


		{
			int src_count = 0;
			void *srcs[MAX_CACHE_DEVS];
			void *dst = NULL;
			int cur_ssd = cursor_data_start_ssd(super, seg->seg_id, WCBUF);
			u32 ram_offset;

			for(src_count = 0;src_count < row_count-1; src_count++){
				ram_offset = chunk_offset + (cur_ssd * CHUNK_SZ);
				srcs[src_count] = page_address(rambuf->pages[ram_offset]->pl->page);
				cur_ssd = (cur_ssd + 1) % NUM_SSD;
			}
			ram_offset = chunk_offset + (cur_ssd * CHUNK_SZ);
			dst = page_address(rambuf->pages[ram_offset]->pl->page);

			//printk(" xor blocks ...src count = %d  \n", src_count);
			memset(dst, 0xFF, SRC_PAGE_SIZE);
			run_xor(srcs, dst, src_count, SRC_PAGE_SIZE);
		}
	}
}

void make_bios_vector(struct dmsrc_super *super, struct segment_header *seg, struct rambuffer *rambuf, 
		atomic_t *bios_start, atomic_t *bios_count){
	u32 devno;

	for(devno = 0;devno < NUM_SSD;devno++){
		u32 count = atomic_read(&rambuf->bios_count[devno])
					- atomic_read(&rambuf->bios_start[devno]);

		//printk(" make bios: seg id = %d, devno = %d, count = %d \n", (u32)seg->seg_id, devno, count);
		atomic_set(&bios_count[devno], atomic_read(&rambuf->bios_count[devno]));
		atomic_set(&bios_start[devno], atomic_read(&rambuf->bios_start[devno]));

		atomic_set(&rambuf->bios_start[devno], atomic_read(&rambuf->bios_count[devno]));
		atomic_add(count, &rambuf->bios_total_start);
	}
}

void flush_partial_meta(struct dmsrc_super *super, int cache_type)
{
	struct segment_header *seg; 
	struct rambuffer *rambuf;
	bool need_flush = false;
	unsigned long f;
	u32 length;
	u32 seg_length;
	atomic_t bios_start[MAX_CACHE_DEVS];
	atomic_t bios_count[MAX_CACHE_DEVS];
	u64 global_seg_sequence;
	//int i;
	int ret = 0;

	LOCK(super, f);

	seg = super->current_seg[cache_type];

	rambuf = super->segbuf_mgr.current_rambuf[cache_type];
	if(!rambuf)
		goto No_Rambuf;

	//printk(" Partial: flush current buffer id = %d length = %d(part %d + %d), ram ref = %d \n", 
	//			(int) seg->seg_id, (int)atomic_read(&seg->length),
	//			(int)atomic_read(&seg->part_start),
	//			(int)atomic_read(&seg->part_length),
	//			atomic_read(&rambuf->ref_count));
	//printk(" Partial: seg = %d start = %d, count = %d \n", (int)seg->seg_id, 
	//		(int)atomic_read(&rambuf->bios_total_start),
	//		(int)atomic_read(&rambuf->bios_total_count));

	length = atomic_read(&rambuf->bios_total_count) - atomic_read(&rambuf->bios_total_start);

	if(length || ((atomic_read(&seg->part_length)) && 
		atomic_read(&seg->length)<STRIPE_SZ && 
		!test_bit(SEG_SEALED, &seg->flags)))
	{
		
	//	printk(" flush partial seg length = %d \n", length);

		need_flush = true;
		global_seg_sequence = atomic64_inc_return(&super->flush_mgr.global_seg_sequence);

		ret = alloc_partial_summary(super, seg, rambuf, cache_type);
		if(ret < 0)
			goto No_Rambuf;

		make_bios_parity(super, seg, rambuf, 1);

		//for(i = 0;i < NUM_SSD;i++){
		//	u32 dev_count;
		//	dev_count = ma_get_count_per_dev(super, cache_type, i);
		//}
		atomic_inc(&super->wstat.partial_write_count);
	}

	make_bios_vector(super, seg, rambuf, bios_start, bios_count);
	seg_length = atomic_read(&seg->length);

No_Rambuf:
	UNLOCK(super, f);

	if(ret<0){
		printk(" alloc partial summary error = %d \n", ret);
		BUG_ON(1);
	}

	if(need_flush){
//		printk(" make flush invoke job ... \n");
		if(super->param.bio_plugging){
			make_flush_invoke_job(super, seg, rambuf, seg_length, bios_start, bios_count,
					cache_type, 0, 0, 1, global_seg_sequence);
		}else{
			make_flush_invoke_job(super, seg, rambuf, seg_length, bios_start, bios_count,
					cache_type, 0, 1, 0, global_seg_sequence);
		}
	}else{
		struct pending_manager *pending_mgr = &super->pending_mgr;
		struct bio_list local_list;
		bio_list_init(&local_list);
		spin_lock_irqsave(&pending_mgr->barrier_lock, f);
		bio_list_merge(&local_list, &pending_mgr->barrier_ios);
		bio_list_init(&pending_mgr->barrier_ios);
		atomic_set(&pending_mgr->barrier_count, 0);
		spin_unlock_irqrestore(&pending_mgr->barrier_lock, f);

		issue_deferred_bio(super, &local_list);
	}
}

/*----------------------------------------------------------------*/

inline void inc_num_dirty_blocks(struct dmsrc_super *super)
{
	atomic64_inc(&super->cache_stat.num_dirty_blocks);
}

inline static void dec_num_dirty_blocks(struct dmsrc_super *super)
{
	atomic64_dec(&super->cache_stat.num_dirty_blocks);
}

void cleanup_mb_if_dirty(struct dmsrc_super *super,
			 struct segment_header *seg,
			 struct metablock *mb)
{
	unsigned long flags;
	bool b = false;

	lockseg(seg, flags);
	if (test_bit(MB_DIRTY, &mb->mb_flags)) {
		//clear_bit(MB_DIRTY, &mb->mb_flags);
		b = true;
	}

	if(is_read_stripe(seg->seg_type))
		b = true;

	unlockseg(seg, flags);

	if (b)
		dec_num_dirty_blocks(super);
}

u8 atomic_read_mb_validness(struct segment_header *seg, struct metablock *mb)
{
	return test_bit(MB_VALID, &mb->mb_flags);
}

u8 atomic_read_mb_dirtiness(struct segment_header *seg, struct metablock *mb)
{
	return test_bit(MB_DIRTY, &mb->mb_flags);
}


inline static void bio_remap(struct bio *bio, struct dm_dev *dev, sector_t sector)
{
	bio->bi_bdev = dev->bdev;
	bio->bi_sector = sector;
}

inline sector_t calc_cache_alignment(struct dmsrc_super *super,
				     sector_t bio_sector)
{
	return div_u64(bio_sector, SRC_SECTORS_PER_PAGE) * (SRC_SECTORS_PER_PAGE);
}

void invalidate_previous_cache(struct dmsrc_super *super,
			       struct segment_header *seg,
			       struct metablock *old_mb)
{

	cleanup_mb_if_dirty(super, seg, old_mb);

	ht_del(super, old_mb);

	atomic_dec(&seg->valid_count);
	atomic_dec(&seg->group->valid_count);

	if(test_bit(MB_DIRTY, &old_mb->mb_flags))
		atomic_dec(&seg->valid_dirty_count);
	else
		atomic_dec(&seg->valid_clean_count);

	atomic_dec(&super->cache_stat.num_used_blocks);
	clear_bit(MB_VALID, &old_mb->mb_flags);
	//clear_bit(MB_DIRTY, &old_mb->mb_flags);

	if(test_bit(MB_BROKEN, &old_mb->mb_flags)){
		clear_bit(MB_BROKEN, &old_mb->mb_flags);
		atomic_dec(&super->recovery_mgr.broken_block_count);
	}

	if(test_bit(MB_HIT, &old_mb->mb_flags)){
		clear_bit(MB_HIT, &old_mb->mb_flags);
		atomic_dec(&seg->hot_count);

		if(!test_bit(MB_DIRTY, &old_mb->mb_flags))
			atomic_dec(&seg->hot_clean_count);
	}

#if 0
	if(test_bit(SEG_SEALED, &seg->flags)){
		if(get_data_valid_count(super, seg)<0){
		printk(" ** Invalid valid count seg = %d valid = %d \n", (int)seg->seg_id, 
				atomic_read(&seg->valid_count));
		}

		if(atomic_read(&seg->valid_count)-get_metadata_count(super, seg->seg_type)!=
				calc_valid_count(super, seg, 1)){
			printk(" invalid valid count = %d %d, dummy = %d \n",  
					atomic_read(&seg->valid_count)-get_metadata_count(super, seg->seg_type), 
					calc_valid_count(super, seg, 1), 
					atomic_read(&seg->dummy_count));
		}
	}
#endif 
}


inline void wake_read_caching_worker(struct dmsrc_super *super){
	queue_work(super->read_miss_mgr.wq, &super->read_miss_mgr.work);
}

static void init_read_caching_job_list(struct dmsrc_super *super){
	struct read_caching_job_list *list;
	
	list = &super->read_miss_mgr.queue;
	atomic_set(&list->rm_copy_count, 0);
	spin_lock_init(&list->rm_spinlock);
	INIT_LIST_HEAD(&list->rm_copy_head);
}


void mark_parity_dirty(struct dmsrc_super *super, int cache_type, 
		struct segment_header *seg, u32 idx){
	
	struct metablock *mb;
	int parity_start;
	int parity_offset;
	u32 parity_idx;
	
	if(is_read_stripe(cache_type))
		return;

	if(NO_USE_ERASURE_CODE(&super->param))
		return;

	parity_start = cursor_parity_start(super, seg->seg_id, cache_type);
	parity_offset = idx%(CHUNK_SZ);

	parity_idx = parity_start + parity_offset;
	mb = get_mb(super, seg->seg_id, parity_idx);
	set_bit(MB_PARITY_NEED, &mb->mb_flags);
	clear_bit(MB_VALID, &mb->mb_flags);
	clear_bit(MB_DIRTY, &mb->mb_flags);

	//if(super->param->erasure_code==ERASURE_CODE_RAID6){
	if(USE_ERASURE_RAID6(&super->param)){
		parity_idx += CHUNK_SZ;
		parity_idx %= STRIPE_SZ;
		mb = get_mb(super, seg->seg_id, parity_idx);
		set_bit(MB_PARITY_NEED, &mb->mb_flags);
	}
}

void reset_parity(struct dmsrc_super *super, struct segment_header *seg, int cache_type, int clear_written){
	unsigned long flags;
	int parity_size; 
	int parity_start;
	int i;

	if(is_read_stripe(cache_type))
		return;

	if(NO_USE_ERASURE_CODE(&super->param))
		return;

	parity_size = CHUNK_SZ;
	parity_start = cursor_parity_start(super, seg->seg_id, cache_type);

	lockseg(seg, flags);

	for(i = 0;i <parity_size;i++){
		u32 offset = parity_start + i;
		struct metablock *mb = get_mb(super, seg->seg_id, offset);

		clear_bit(MB_PARITY_NEED, &mb->mb_flags);
		
		if(clear_written && test_bit(MB_PARITY_WRITTEN, &mb->mb_flags)){
			clear_bit(MB_PARITY_WRITTEN, &mb->mb_flags);
		}
	}

	if(USE_ERASURE_RAID6(&super->param)){
		for(i = 0;i <parity_size;i++){
			u32 offset = (parity_start + CHUNK_SZ +  i) % STRIPE_SZ;
			struct metablock *mb = get_mb(super, seg->seg_id, offset);

			clear_bit(MB_PARITY_NEED, &mb->mb_flags);
			
			if(clear_written && test_bit(MB_PARITY_WRITTEN, &mb->mb_flags)){
				clear_bit(MB_PARITY_WRITTEN, &mb->mb_flags);
			}
		}
	}

	unlockseg(seg, flags);

}

/* ported from linxu/fs/btrfs/raid56.c
 *
 * helper function to run the xor_blocks api.  It is only
 * able to do MAX_XOR_BLOCKS at a time, so we need to
 * loop through.
 */
void run_xor(void **pages, void *dest, int src_cnt, ssize_t len)
{
	int src_off = 0;
	int xor_src_cnt = 0;

	while(src_cnt > 0) {
		xor_src_cnt = min(src_cnt, MAX_XOR_BLOCKS);
		xor_blocks(xor_src_cnt, len, dest, pages + src_off);

		src_cnt -= xor_src_cnt;
		src_off += xor_src_cnt;
	}
}

#if 0
void verify_xor(struct dmsrc_super *super, struct rambuf_page **pages, int i, void *temp){
	void *srcs[MAX_CACHE_DEVS];
	void *dst = NULL;
	int src_count;
	int parity_size;
	int j;

	parity_size = CHUNK_SZ;

	src_count = 0;
	for(j = 0;j < NUM_SSD;j++){
		if(j==0)
			dst = pages[i + (j * parity_size)]->data;
		else
			srcs[src_count++] = pages[i + (j * parity_size)]->data;
	}

	memset(temp, 0xFF, PAGE_SIZE);

	run_xor(srcs, temp, src_count, PAGE_SIZE);
	if(memcmp(temp, dst, PAGE_SIZE)){
		printk(" %d parity mismatch recovered %u original %u\n", 
				i,
				crc32_le(17, temp, PAGE_SIZE),
				crc32_le(17, dst, PAGE_SIZE)
			  );
	}
}
#endif

#if 0 
void generate_parity_xor(struct dmsrc_super *super, struct segment_header *seg, struct rambuf_page **pages, int full_seg, int cache_type){
	struct metablock *mb; 
	void *srcs[MAX_CACHE_DEVS];

	unsigned long flags;
	void *temp;
	void *dst = NULL;

	int parity_size;
	int parity_start;
	//int count = 0;
	int i, j;

	int src_count = 0;
	int parity_ssd; 

	parity_size = CHUNK_SZ;
	parity_start = cursor_parity_start(super, seg->seg_id, cache_type);
	temp = (void *)get_zeroed_page(GFP_KERNEL);

	//printk(" parity ssd = %d \n", (int)get_parity_ssd(super, seg->seg_id));

	for(i = 0;i <parity_size;i++){
		parity_ssd = get_parity_ssd(super, seg->seg_id);
		src_count = 0;
		dst = NULL;

		mb = get_mb(super, seg->seg_id, parity_start + i);

		lockseg(seg, flags);
		clear_bit(MB_DIRTY, &mb->mb_flags);
		clear_bit(MB_VALID, &mb->mb_flags);
		set_bit(MB_PARITY, &mb->mb_flags);

		// FIX ME
		if(test_bit(MB_PARITY_NEED, &mb->mb_flags)){
			set_bit(MB_PARITY_WRITTEN, &mb->mb_flags);
		}
		unlockseg(seg, flags);

		if(test_bit(MB_PARITY_NEED, &mb->mb_flags)){
			for(j = 0;j < NUM_SSD;j++){
				if(j != parity_ssd){
					srcs[src_count++] = pages[i + (j * parity_size)]->data;
				}else{
					dst = pages[i + (j * parity_size)]->data;
				}
			}

			memset(dst, 0xFF, SRC_PAGE_SIZE);
			run_xor(srcs, dst, src_count, SRC_PAGE_SIZE);
#if 0  
			verify_xor(super, pages, i dst);
#endif 
		}

#if 0
		if(full_seg && test_bit(MB_PARITY_WRITTEN, &mb->mb_flags)){
			atomic_inc(&seg->valid_count);
			atomic_inc(&seg->group->valid_count);
			atomic_inc(&seg->dirty_count);
			atomic_inc(&super->cache_stat.num_used_blocks);
			count ++;
			if(atomic_read(&seg->valid_count) > STRIPE_SZ){
				printk(" segid = %d, parity xor data valid count = %d \n", (int)seg->seg_id,  atomic_read(&seg->valid_count));
			}
		}
#endif 
	}

	free_page((unsigned long)temp);

	//if(full_seg && atomic_read(&seg->length)!=NR_CACHES_INSEG){
		//printk(" seg length = %d, count = %d  \n", (int)atomic_read(&seg->length), count);
	//	printk(" seg id = %d length = %d, count = %d  \n", (int)seg->seg_id, (int)atomic_read(&seg->length), count);
	//}

	BUG_ON(atomic_read(&seg->length)>STRIPE_SZ);
}

void generate_parity_pq(struct dmsrc_super *super, struct segment_header *seg, struct rambuf_page **pages, int full_seg, int cache_type){
	int parity_size; 
	int parity_start; 
	unsigned long flags;
	int count = 0;
	int i;

	parity_size = CHUNK_SZ;
	parity_start = cursor_parity_start(super, seg->seg_id, cache_type);

	for(i = 0;i <parity_size;i++){
		u32 parity_idx = parity_start + i;
		struct metablock *mb[2];
		void *srcs[MAX_CACHE_DEVS];
		int src_count = 0;
		int parity_ssd = get_parity_ssd(super, seg->seg_id);
		int j;

		mb[0] = get_mb(super, seg->seg_id, parity_idx);
		mb[1] = get_mb(super, seg->seg_id, (parity_idx + CHUNK_SZ)%STRIPE_SZ);

		lockseg(seg, flags);
		for(j =0;j < 2;j++){
			clear_bit(MB_DIRTY, &mb[j]->mb_flags);
			clear_bit(MB_VALID, &mb[j]->mb_flags);
			set_bit(MB_PARITY, &mb[j]->mb_flags);

			if(test_bit(MB_PARITY_NEED, &mb[j]->mb_flags)){
				set_bit(MB_PARITY_WRITTEN, &mb[j]->mb_flags);
			}
		}
		unlockseg(seg, flags);

		if(test_bit(MB_PARITY_NEED, &mb[0]->mb_flags)){
			for(j = 0;j < NUM_SSD;j++){
				u32 offset = i + (j * parity_size);
				if(!pages[offset]){
					printk(" pages is null ... %d \n", offset);
					BUG_ON(1);
				}

				if(j==parity_ssd){
					srcs[NUM_SSD-2] = pages[offset]->data;
					memset(srcs[NUM_SSD-2], 0xEE, SRC_PAGE_SIZE);
				}else if(j==(parity_ssd+1)%NUM_SSD){
					srcs[NUM_SSD-1] = pages[offset]->data;
					memset(srcs[NUM_SSD-1], 0xEE, SRC_PAGE_SIZE);
				}else{
					srcs[src_count++] = pages[offset]->data;
				}
			}

			raid6_call.gen_syndrome(NUM_SSD, SRC_PAGE_SIZE, srcs);
		}

		if(full_seg && test_bit(MB_PARITY_WRITTEN, &mb[0]->mb_flags)){
			for(j =0;j < 2;j++){
				atomic_inc(&seg->length);
				atomic_inc(&seg->part_length);
				atomic_inc(&super->cache_stat.num_used_blocks);
				count ++;
			}
		}
	}

	BUG_ON(atomic_read(&seg->length)>STRIPE_SZ);

//	if(full_seg && atomic_read(&seg->length)!=NR_CACHES_INSEG){
//		printk(" seg id = %d length = %d, count = %d  \n", (int)seg->seg_id, (int)atomic_read(&seg->length), count);
//	}
}
#endif 

#if 0
void generate_parity_data(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuf_page **pages, int full_seg, int cache_type){

	if(is_read_stripe(cache_type))
		return;

	if(NO_USE_ERASURE_CODE(&super->param))
		return;

	if(USE_ERASURE_PARITY(&super->param)){
		generate_parity_xor(super, seg, pages, full_seg, cache_type);
	}else{
		generate_parity_pq(super, seg, pages, full_seg, cache_type);
	}
}
#endif 


#if SUMMARY_LOCATION == USE_FIRST_SUMMARY 
inline u32 get_summary_offset(struct dmsrc_super *super, u32 ssd_id){
	return (ssd_id) * CHUNK_SZ;
}

inline bool chunk_summary_range(struct dmsrc_super *super, u32 idx){
	u32 offset = (idx) % CHUNK_SZ;
	if(offset >= 0  && (offset) < NUM_SUMMARY){
		return true;
	}
	return false;
}

inline bool need_chunk_summary(struct dmsrc_super *super, u32 idx){
	u32 offset = (idx) % CHUNK_SZ;
	if((offset) == (NUM_SUMMARY)){
		return true;
	}
	return false;
}

inline u32 data_to_summary_idx(struct dmsrc_super *super, u32 idx){
	return (idx%STRIPE_SZ)/CHUNK_SZ*CHUNK_SZ;
}

#endif 

#if SUMMARY_LOCATION == USE_LAST_SUMMARY //summary information stored in last of each chunk 
inline u32 get_summary_offset(struct dmsrc_super *super, u32 ssd_id){
	return (ssd_id + 1) * CHUNK_SZ - NUM_SUMMARY;
}

inline bool chunk_summary_range(struct dmsrc_super *super, u32 idx){
	u32 offset = (idx) % CHUNK_SZ;
	if((offset+1) >= CHUNK_SZ-(NUM_SUMMARY-1)  && (offset+1) <= CHUNK_SZ){
		return true;
	}
	return false;
}

inline bool need_chunk_summary(struct dmsrc_super *super, u32 idx){
	u32 offset = (idx) % CHUNK_SZ;
	if((offset+1) == (CHUNK_SZ)){
		return true;
	}
	return false;
}
inline u32 data_to_summary_idx(struct dmsrc_super *super, u32 idx){
	return (idx%STRIPE_SZ)  - (idx%CHUNK_SZ) + (CHUNK_SZ-NUM_SUMMARY);
}
#endif



inline int get_parity_size(struct dmsrc_super *super, int cache_type){

	if(NO_USE_ERASURE_CODE(&super->param))
		return 0;
	else if(USE_ERASURE_PARITY(&super->param))
		return CHUNK_SZ;
	else
		return CHUNK_SZ * 2;
}

#if 0
inline void set_max_length(struct dmsrc_super *super, int cache_type){
	u32 threshold;
	//u32 num_parity = get_parity_size(super, cache_type);
	struct segment_header *seg = super->current_seg[cache_type];

	if(SUMMARY_SCHEME==SUMMARY_PER_CHUNK)
		threshold = STRIPE_SZ; // summary
	else
		threshold = STRIPE_SZ - NUM_SUMMARY;

	if(is_read_stripe(cache_type)){
		atomic_set(&seg->length, threshold);
		atomic_set(&seg->part_length, threshold);
	}else{
		//atomic_set(&seg->length, threshold-num_parity);
		//atomic_set(&seg->part_length, threshold-num_parity);
		atomic_set(&seg->length, threshold);
		atomic_set(&seg->part_length, threshold);
	}
}

inline void set_max_count(struct dmsrc_super *super, int cache_type){
	u32 threshold;
	///u32 num_parity = get_parity_size(super, cache_type);

	if(SUMMARY_SCHEME==SUMMARY_PER_CHUNK)
		threshold = STRIPE_SZ; // summary
	else
		threshold = STRIPE_SZ-NUM_SUMMARY;

	if(is_read_stripe(cache_type)){
		ma_set_count(super, cache_type, threshold);
	}else{
		ma_set_count(super, cache_type, threshold);
	}

}
#endif

inline bool need_refresh_segment(struct dmsrc_super *super, int cache_type, int count){
	bool refresh = false;
	u32 threshold = STRIPE_SZ; // summary

	if(is_read_stripe(cache_type)){
		if(count>=threshold)
			refresh = true;
	}else{
		if(count>=threshold) // last parity 
			refresh = true;
	}

	return refresh;
}

#if 0 
inline int get_start_ssd(struct dmsrc_super *super, u64 seg_id){
	int ssd;
	if(super->param->parity_allocation==PARITY_ALLOC_FIXED){
		ssd =  0;
	}else{
		ssd = seg_id % NR_SSD;
	}

	return ssd;
}
#endif 


inline int get_parity_ssd(struct dmsrc_super *super, u64 seg_id){
	sector_t sector;
	int parity_ssd;
	int temp;

	if(NO_USE_ERASURE_CODE(&super->param))
		return -1;

	if(super->param.parity_allocation==PARITY_ALLOC_FIXED)
		sector = (seg_id * STRIPE_SZ - CHUNK_SZ) << SRC_SECTORS_PER_PAGE_SHIFT;
	else
		sector = (seg_id * STRIPE_SZ) << SRC_SECTORS_PER_PAGE_SHIFT;

	raid5_calc_sector(&super->raid_conf, sector, 0, &temp, &parity_ssd, NULL);

	return parity_ssd % NUM_SSD;
}

inline int get_parityq_ssd(struct dmsrc_super *super, u64 seg_id){
	sector_t sector;
	int parity_ssd;
	int temp;

	if(!USE_ERASURE_RAID6(&super->param))
		return -1;

	sector = (seg_id * STRIPE_SZ) << SRC_SECTORS_PER_PAGE_SHIFT;
	raid5_calc_sector(&super->raid_conf, sector, 0, &temp, NULL, &parity_ssd);

	return parity_ssd % NUM_SSD;
}

inline int cursor_parity_start(struct dmsrc_super *super, u64 seg_id, int cache_type){
	int parity_ssd = get_parity_ssd(super, seg_id);
	return (parity_ssd * CHUNK_SZ) % STRIPE_SZ;
}

inline int cursor_data_start_ssd(struct dmsrc_super *super, u64 seg_id, int cache_type){
	int parity_ssd = get_parity_ssd(super, seg_id);
	return (parity_ssd + 1 ) % NUM_SSD;
}

#if 0 
inline int cursor_summary_offset(struct dmsrc_super *super, u64 seg_id, int cache_type){
	int data_start = cursor_start(super, seg_id);

	if(is_read_stripe(cache_type) || NO_USE_ERASURE_CODE(&super->param)){
		return (data_start + CHUNK_SZ * NUM_SSD - NUM_SUMMARY) % STRIPE_SZ;
	}else{
		return (data_start + CHUNK_SZ * NUM_DATA_SSD - NUM_SUMMARY) % STRIPE_SZ;
	}
}
#endif

#if 0 
inline int cursor_init(struct dmsrc_super *super, u64 seg_id, int cache_type){
	if(super->param->data_allocation==DATA_ALLOC_VERT){
		return cursor_start(super, seg_id) - 1;
	}else{

		if(super->param->parity_allocation==PARITY_ALLOC_FIXED)
			super->col[cache_type] = NR_SSD-1;
		else
			super->col[cache_type] = ((seg_id%NR_SSD) + (NR_SSD-1)) % NR_SSD;

		super->row[cache_type] = -1;

		return cursor_start(super, seg_id) - 1;
	}
}
#endif 

void _alloc_mb_summary(struct dmsrc_super *super,struct segment_header *seg, int cache_type, u32 mb_idx, int full, int dummy){
	struct metablock *new_mb;
	u32 tmp32;

	tmp32 = mb_idx%STRIPE_SZ;
	new_mb = get_mb(super, seg->seg_id, tmp32);
	new_mb->sector = ~0;

	if(test_bit(MB_VALID, &new_mb->mb_flags))
		printk(" summary block is data block .. type = %d\n", seg->seg_type);

	atomic_inc(&super->cache_stat.num_used_blocks);

	clear_bit(MB_SEAL, &new_mb->mb_flags);
	clear_bit(MB_DIRTY, &new_mb->mb_flags);
	clear_bit(MB_VALID, &new_mb->mb_flags);
	clear_bit(MB_SKIP, &new_mb->mb_flags);

	if(!full){
		clear_bit(MB_SUMMARY, &new_mb->mb_flags);
		if(dummy)
			set_bit(MB_DUMMY, &new_mb->mb_flags);
		else
			clear_bit(MB_DUMMY, &new_mb->mb_flags);
		atomic_inc(&seg->dummy_count);
	}
	

	if(full){
		set_bit(MB_SUMMARY, &new_mb->mb_flags);
		atomic_inc(&seg->valid_count);
		atomic_inc(&seg->group->valid_count);
	}

	atomic_inc(&seg->dirty_count);
	atomic_inc(&seg->length);
	atomic_inc(&seg->part_length);
	mark_parity_dirty(super, cache_type, seg, tmp32);

	BUG_ON(atomic_read(&seg->valid_count) > STRIPE_SZ);
	BUG_ON(atomic_read(&seg->length) > STRIPE_SZ);
}

#if 0 
void _alloc_mb_summary(struct dmsrc_super *super, int cache_type, u32 idx){
	struct segment_header *seg;
	struct metablock *new_mb;
	u32 update_mb_idx;
	u32 tmp32;

	seg = super->current_seg[cache_type];

	atomic_inc(&super->count[cache_type]);

	update_mb_idx = SEG_START_IDX(seg) + super->cursor[cache_type];
	update_mb_idx = idx;

	div_u64_rem(update_mb_idx, NR_CACHES_INSEG, &tmp32);
	new_mb = get_mb(super, seg->seg_id, tmp32);

	if(test_bit(MB_VALID, &new_mb->mb_flags))
		printk(" block is already allocated as a data block .. type = %d\n", seg->seg_type);


	atomic_inc(&super->nr_used_caches);

	clear_bit(MB_SEAL, &new_mb->mb_flags);
	clear_bit(MB_DIRTY, &new_mb->mb_flags);
	clear_bit(MB_VALID, &new_mb->mb_flags);
	set_bit(MB_SUMMARY, &new_mb->mb_flags);

	atomic_inc(&seg->valid_count);
	atomic_inc(&seg->length);
	atomic_inc(&seg->part_length);

	mark_parity_dirty(super, cache_type, seg, tmp32);

	//printk(" write chunk summary %d, %d %d\n", (int)tmp32, (int)atomic_read(&super->count[cache_type]),
	//		(int)atomic_read(&seg->length));

	BUG_ON(atomic_read(&seg->length) > NR_CACHES_INSEG);

}
#endif 

void initialize_mb_summary(struct dmsrc_super *super, struct segment_header *seg,
		int cache_type, u32 idx, int full)
{
	int i;

	for(i = 0;i < NUM_SUMMARY;i++){
		_alloc_mb_summary(super, seg, cache_type, idx+i, full, 0);
	}
}

#if 1 
void alloc_mb_summary(struct dmsrc_super *super, struct segment_header *seg,
		int cache_type, u32 idx, u32 *total_count ){
	int i;

	for(i = 0;i < NUM_SUMMARY;i++){
		ma_alloc(super, seg, seg->seg_id, cache_type, (idx+i)/CHUNK_SZ, total_count);
	}
}
#else

void alloc_mb_summary(struct dmsrc_super *super, struct segment_header *seg,
		int cache_type, u32 idx, u32 *total_count )
{
	int i;

	if(super->param->data_allocation==DATA_ALLOC_VERT){
		for(i = 0;i < NR_SUMMARY;i++){
			u32 alloc_idx = sa_alloc(super, seg, seg->seg_id, cache_type, total_count);
			BUG_ON(idx+i!=alloc_idx%STRIPE_SZ);
		}
	}else if(super->param->data_allocation==DATA_ALLOC_HORI){
		for(i = 0;i < NR_SUMMARY;i++){
#if 0 
			u32 alloc_idx = sa_alloc(super, seg, seg->seg_id, cache_type, total_count);
#else
			//u32 alloc_idx = idx + i;
			atomic_inc(&super->sa[cache_type].count);
			if(total_count)
				*total_count = atomic_read(&super->sa[cache_type].count);
#endif 
			//printk(" alloc summary = %d, count = %d \n", (int)alloc_idx%STRIPE_SZ, (int)*total_count);
			//BUG_ON(idx+i!=alloc_idx%STRIPE_SZ);
		}
	}else{ // Multi Allocator
		for(i = 0;i < NR_SUMMARY;i++){
			//u32 alloc_idx = ma_alloc(super, seg->seg_id, cache_type, (idx+1)/CHUNK_SZ);
			ma_alloc(super, seg, seg->seg_id, cache_type, (idx+i)/CHUNK_SZ, total_count);
			//BUG_ON(idx+i!=alloc_idx%STRIPE_SZ);
		}
	}
}
#endif 

#if 0
void alloc_mb_summary(struct dmsrc_super *super, struct segment_header *seg,
		int cache_type, u32 idx )
{
	int i;

	if(super->param->data_allocation==DATA_ALLOC_VERT){
		for(i = 0;i < NR_SUMMARY;i++){
			cursor_inc(super, seg->seg_id, cache_type);
			_alloc_mb_summary(super, cache_type, idx+i);
		}
	}else{
		for(i = 0;i < NR_SUMMARY;i++){
	//		printk(" alloc summary seg id %d idx = %d count = %d \n", (int)seg->seg_id,
	//				(int)idx+i, (int)atomic_read(&super->count[cache_type]));
			_alloc_mb_summary(super, cache_type, idx+i);
		}
	}
}
#endif 

void seg_length_inc(struct dmsrc_super *super, struct segment_header *seg, struct metablock *mb, bool inflight){

	atomic_inc(&seg->valid_count);
	atomic_inc(&seg->group->valid_count);

	if(test_bit(MB_DIRTY, &mb->mb_flags)){
		atomic_inc(&seg->dirty_count);
		atomic_inc(&seg->valid_dirty_count);
	}else{
		atomic_inc(&seg->valid_clean_count);

	}

	if(inflight){
		atomic_inc(&seg->length);
		atomic_inc(&seg->part_length);
		atomic_inc(&seg->num_filling);
		atomic_inc(&seg->num_bufcopy);
	}
	BUG_ON(atomic_read(&seg->valid_count) > STRIPE_SZ);
	BUG_ON(atomic_read(&seg->length) > STRIPE_SZ);
}

void initialize_mb(struct dmsrc_super *super, struct metablock *new_mb, int cache_type, bool clean){

	clear_bit(MB_SEAL, &new_mb->mb_flags);
	set_bit(MB_VALID, &new_mb->mb_flags);
	clear_bit(MB_PARITY, &new_mb->mb_flags);
	clear_bit(MB_SUMMARY, &new_mb->mb_flags);
	clear_bit(MB_HIT, &new_mb->mb_flags);
	clear_bit(MB_SKIP, &new_mb->mb_flags);
	clear_bit(MB_DUMMY, &new_mb->mb_flags);

	if(is_read_stripe(cache_type)){
		clear_bit(MB_DIRTY, &new_mb->mb_flags);
	}else{
		set_bit(MB_DIRTY, &new_mb->mb_flags);
	}

	if(clean){
		clear_bit(MB_DIRTY, &new_mb->mb_flags);
	}
}

u32 alloc_next_mb(struct dmsrc_super *super, struct segment_header *seg, sector_t key, 
		int cache_type, u32 *total_count){
	s32 devno; 
	u32 mb_idx;

	devno = ma_select_dev(super, key, cache_type);
	BUG_ON(!seg);
	mb_idx = ma_alloc(super, seg, seg->seg_id, cache_type, devno, total_count);
	return mb_idx;
}

struct metablock *alloc_mb_data(struct dmsrc_super *super, 
		sector_t key,
		int cache_type, 
		bool clean, 
		u32 *total_count)
{
	struct segment_header *seg;
	struct metablock *new_mb;
	u32 update_mb_idx;

	seg = super->current_seg[cache_type];
	update_mb_idx = alloc_next_mb(super, seg, key, cache_type, total_count);
	new_mb = get_mb(super, seg->seg_id, update_mb_idx%STRIPE_SZ);

	atomic_inc(&super->cache_stat.num_used_blocks);

	if(is_normal_stripe(cache_type)){
		ht_register(super, key, new_mb);
	}

	initialize_mb(super, new_mb, cache_type, clean);
	seg_length_inc(super, seg, new_mb, true);

	mark_parity_dirty(super, seg->seg_type, seg, new_mb->idx%STRIPE_SZ);

	return new_mb;
}

void update_data_in_mb(struct dmsrc_super *super,
						u32 update_mb_idx,
						struct segment_header *seg,
						struct rambuffer *rambuf,
						int cache_type,
						void *data, 
						u32 crc32)
{
	struct metablock *mb;
	u32 tmp32;

	div_u64_rem(update_mb_idx, STRIPE_SZ, &tmp32);
	mb = get_mb(super, seg->seg_id, tmp32);;

	inc_num_dirty_blocks(super);

	if(rambuf && data && is_write_stripe(cache_type) &&
		USE_ERASURE_CODE(&super->param)){
#if 0
		if(rambuf->pages[tmp32]){
			BUG_ON(!rambuf->pages[tmp32]);
			memcpy(rambuf->pages[tmp32]->data, data, SRC_PAGE_SIZE);
		}
#else
		void *ptr;
		ptr = page_address(rambuf->pages[tmp32]->pl->page);
		memcpy(ptr, data, SRC_PAGE_SIZE);
		//kunmap_atomic(ptr);
#endif 
	}

#ifdef USE_CHECKSUM
//	if(data)
//		mb->checksum = crc32(CRC_SEED, data, SRC_PAGE_SIZE);
	mb->checksum = crc32;
#endif 

	if(atomic_dec_return(&seg->num_bufcopy)<0){
		printk(" invalid num bufcopy ... \n");
		BUG_ON(1);
	}
}

static void writeback_endio_extent(unsigned long error, void *context)
{
	struct wb_job *job = context;
	struct dmsrc_super *super = job->super;
	struct segment_header *seg = job->seg;
	struct rambuffer *rambuf = job->rambuf;
	u32 idx;

	if(error){
		printk(" Writeback: end io error \n");
	}

	//printk(" writeback endio seg = %d, start = %d, count = %d \n", 
	//		(int)seg->seg_id, job->start_idx, job->count);

	for(idx = job->start_idx;idx < job->start_idx + job->count;idx++){
		struct bio *bio = rambuf->bios[idx % STRIPE_SZ];
		struct metablock *mb = mb_at(super, idx);

		if(bio){
			atomic_dec(&super->cache_stat.inflight_bios);
			bio_endio(bio, 0);
		}

		if(job->rambuf && atomic_read(&job->rambuf_release)){
	//		printk(" writeback seg = %d ram refcount = %d \n", (int)seg->seg_id, atomic_read(&job->rambuf->ref_count));
			if(atomic_dec_and_test(&job->rambuf->ref_count)){
				//printk(" >>>writeback end seg id %d ram refcount = %d \n", (int)seg->seg_id, atomic_read(&job->rambuf->ref_count));
				release_rambuffer(super, job->rambuf, seg->seg_type);
			}
			if(atomic_read(&job->rambuf->ref_count)<0){
				printk(" Invalid!! parity end io ram refcount = %d, id = %d \n", 
						atomic_read(&job->rambuf->ref_count), 
						job->rambuf->rambuf_id);
			}
		}
		set_bit(MB_SEAL, &mb->mb_flags);
	}


	mempool_free(job, super->pending_mgr.wb_job_pool);

	//pending_worker_schedule(super);
}

#if 0
static void writeback_endio(unsigned long error, void *context)
{
	struct wb_job *job = context;
	struct dmsrc_super *super = job->super;
	struct segment_header *seg = job->seg;
	struct metablock *mb = job->mb;
	struct bio *bio = job->bio;

	if(error){
		printk(" Writeback: end io error \n");
	}

	if(job->bio){
		atomic_dec(&super->cache_stat.inflight_bios);
		bio_endio(bio, 0);
		job->bio = NULL;
	}

	if(job->rambuf && atomic_read(&job->rambuf_release)){
		//printk(" writeback seg = %d ram refcount = %d \n", (int)seg->seg_id, atomic_read(&job->rambuf->ref_count));
		if(atomic_dec_and_test(&job->rambuf->ref_count)){
		//	printk(" >>>writeback end seg id %d ram refcount = %d \n", (int)seg->seg_id, atomic_read(&job->rambuf->ref_count));
			release_rambuffer(super, job->rambuf, seg->seg_type);
		}
		if(atomic_read(&job->rambuf->ref_count)<0){
			printk(" Invalid!! parity end io ram refcount = %d, id = %d \n", 
					atomic_read(&job->rambuf->ref_count), 
					job->rambuf->rambuf_id);
		}
	}

	set_bit(MB_SEAL, &mb->mb_flags);
	mempool_free(job, super->pending_mgr.wb_job_pool);

	//pending_worker_schedule(super);
}
#endif 

inline struct dm_dev *get_dmdev(struct dmsrc_super *super, u32 idx){
	int dev_no;
	dev_no = idx / CHUNK_SZ % NUM_SSD;
	return super->dev_info.cache_dev[dev_no];
}

inline int get_devno(struct dmsrc_super *super, u32 idx){
	return idx / CHUNK_SZ % NUM_SSD;
}

inline struct block_device *get_bdev(struct dmsrc_super *super, u32 idx){
	int dev_no;
	dev_no = idx / CHUNK_SZ % NUM_SSD;
	return super->dev_info.cache_dev[dev_no]->bdev;
}


inline sector_t get_sector(struct dmsrc_super *super, u32 seg_id, u32 idx){
	u32 base;
	u32 offset;

	base = seg_id * CHUNK_SZ;
	offset = idx % CHUNK_SZ;

	BUG_ON((base+offset)*SRC_SECTORS_PER_PAGE>=super->dev_info.per_ssd_sectors);
	return (RESERVED_START+RESERVED_LENGTH) + 
			(base + offset) * SRC_SECTORS_PER_PAGE;
}

struct wb_job *writeback_make_job_extent(struct dmsrc_super *super, 
		struct segment_header *seg, 
		struct rambuffer *rambuf, 
		u32 start_idx, 
		u32 count)
{
	struct wb_job *job;

	job = mempool_alloc(super->pending_mgr.wb_job_pool, GFP_NOIO);
	if(!job){
		printk(" mempool error \n");
		BUG_ON(1);
	}

	job->super = super;
	job->seg = seg;
	job->rambuf = rambuf;
	job->start_idx = start_idx;
	job->count = count;
	atomic_set(&job->rambuf_release, 1);

	return job;
}

void writeback_issue_job_extent(struct dmsrc_super *super, struct wb_job *job, int flush_command)
{
	struct dm_io_region io;
	struct dm_io_request io_req;
	struct rambuffer *rambuf = job->rambuf;
	struct segment_header *seg = job->seg;
	u32 start_idx = job->start_idx;
	u32 rambuf_idx = job->start_idx % STRIPE_SZ;
	u32 cur_idx;
	struct page_list *pls;
	u32 count = 0;

	pls = NULL;
	for(cur_idx = rambuf_idx + job->count - 1;cur_idx >= rambuf_idx && cur_idx < STRIPE_SZ;cur_idx--){
		struct page_list *pl;
		pl = rambuf->pages[cur_idx]->pl;
		BUG_ON(!pl);
		pl->next = pls;
		pls = pl;
		count++;
	}

	if(count==0 || count > CHUNK_SZ){
		BUG_ON(count==0);
	}

	io_req.mem.offset = 0;
	io_req.mem.type = DM_IO_PAGE_LIST;
	io_req.mem.ptr.pl = pls;
	
	if(flush_command){
		io_req.bi_rw = WRITE_FLUSH;
	//io_req.bi_rw = WRITE_FUA;
	}else{
		io_req.bi_rw = WRITE_SYNC;
		//io_req.bi_rw = WRITE;
	}

	io_req.notify.fn = writeback_endio_extent;
	io_req.notify.context = job;
	io_req.client = super->io_client;

	io.bdev = get_bdev(super, start_idx);
	io.sector = get_sector(super, seg->seg_id, start_idx);
	io.count = SRC_SECTORS_PER_PAGE * job->count;

	dmsrc_io(&io_req, 1, &io, NULL);
}

#if 0 
struct wb_job *writeback_make_job(struct dmsrc_super *super, 
		struct segment_header *seg, 
		u32 idx, 
		struct bio *bio, 
		struct rambuffer *rambuf, 
		int rambuf_release)
{
	struct wb_job *job;
	u32 rambuf_idx = idx % STRIPE_SZ;

	job = mempool_alloc(super->pending_mgr.wb_job_pool, GFP_NOIO);
	if(!job){
		printk(" mempool error \n");
		BUG_ON(1);
	}

	job->super = super;
	job->seg = seg;
	job->mb = get_mb(super, seg->seg_id, rambuf_idx);
	job->bio = bio;
	job->rambuf = rambuf;
	atomic_set(&job->rambuf_release, rambuf_release);

	return job;
}

void writeback_issue_job(struct dmsrc_super *super, struct wb_job *job)
{
	u32 idx = job->mb->idx;
	u32 rambuf_idx = job->mb->idx % STRIPE_SZ;
	struct dm_io_region io;
	struct dm_io_request io_req;
	struct bio *bio = job->bio;
	struct rambuffer *rambuf = job->rambuf;
	struct segment_header *seg = job->seg;

#if 0
	if(job->bio){ // incoming write
		io_req.mem.type = DM_IO_BVEC;
		io_req.mem.ptr.bvec = bio->bi_io_vec + bio->bi_idx;
	}else{ // parity, summary, gc data, read data
		io_req.mem.type = DM_IO_KMEM;
		if(rambuf->pages[rambuf_idx]==NULL){
			printk(" invalid rambuf idx %d \n", rambuf_idx);
			BUG_ON(1);
		}
		io_req.mem.ptr.addr = rambuf->pages[rambuf_idx]->data;
	}
#else
	io_req.mem.type = DM_IO_KMEM;
	if(rambuf->pages[rambuf_idx]==NULL){
		printk(" invalid rambuf idx %d \n", rambuf_idx);
		BUG_ON(1);
	}
	io_req.mem.ptr.addr = rambuf->pages[rambuf_idx]->data;
#endif
	
	io_req.bi_rw = WRITE;
	//WRITE_SYNC
	io_req.notify.fn = writeback_endio;
	io_req.notify.context = job;
	io_req.client = super->io_client;

	io.bdev = get_bdev(super, idx);
	io.sector = get_sector(super, seg->seg_id, idx);
	if(bio)
		io.count = bio_sectors(bio);
	else
		io.count = SRC_SECTORS_PER_PAGE;

	dmsrc_io(&io_req, 1, &io, NULL);
}

#endif 

#if 0
void write_async_bio(struct dmsrc_super *super, 
		struct segment_header *seg, 
		u32 idx, 
		struct bio *bio, 
		struct rambuffer *rambuf, 
		int rambuf_release)
{
	u32 rambuf_idx = idx % STRIPE_SZ;
	struct dm_io_region io;
	struct dm_io_request io_req;
	struct wb_job *job;

	job = mempool_alloc(super->pending_mgr.wb_job_pool, GFP_NOIO);
	if(!job){
		printk(" mempool error \n");
		BUG_ON(1);
	}

	job->super = super;
	job->seg = seg;
	job->mb = get_mb(super, seg->seg_id, rambuf_idx);

	if(bio){ // incoming write
		job->bio = bio;
		job->rambuf = rambuf;
		atomic_set(&job->rambuf_release, rambuf_release);
		io_req.mem.type = DM_IO_BVEC;
		io_req.mem.ptr.bvec = bio->bi_io_vec + bio->bi_idx;
	}else{ // parity, summary, gc data, read data
		job->bio = NULL;
		job->rambuf = rambuf;
		atomic_set(&job->rambuf_release, rambuf_release);

		io_req.mem.type = DM_IO_KMEM;

		if(rambuf->pages[rambuf_idx]==NULL){
			printk(" invalid rambuf idx %d \n", rambuf_idx);
			BUG_ON(1);
		}
		io_req.mem.ptr.addr = rambuf->pages[rambuf_idx]->data;
	}
	
	io_req.bi_rw = WRITE;
	io_req.notify.fn = writeback_endio;
	io_req.notify.context = job;
	io_req.client = super->io_client;

	io.bdev = get_bdev(super, idx);
	io.sector = get_sector(super, seg->seg_id, idx);
	if(bio)
		io.count = bio_sectors(bio);
	else
		io.count = SRC_SECTORS_PER_PAGE;

	dmsrc_io(&io_req, 1, &io, NULL);
}
#endif

void make_flush_invoke_job(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuffer *rambuf, int seg_length, 
		atomic_t *bios_start, atomic_t *bios_count,
		int cache_type, int force_seal, int summary, int flush_data, u64 global_seg_sequence){
	struct flush_manager *flush_mgr = &super->flush_mgr;
	struct flush_invoke_job *job, *prev_job;
	unsigned long flags;
	bool found = 0;
	u32 devno;

	job = mempool_alloc(flush_mgr->invoke_pool, GFP_NOIO);

	bio_list_init(&job->barrier_ios);
	if(is_normal_stripe(cache_type)){
		unsigned long f;
		spin_lock_irqsave(&super->pending_mgr.barrier_lock, f);
		//if (!bio_list_empty(&super->pending_mgr.barrier_ios)) {
		//	printk(" Merge barrier bios ... \n");
		//}

		bio_list_merge(&job->barrier_ios, &super->pending_mgr.barrier_ios);
		bio_list_init(&super->pending_mgr.barrier_ios);
		atomic_set(&super->pending_mgr.barrier_count, 0);
		spin_unlock_irqrestore(&super->pending_mgr.barrier_lock, f);
	}

	job->cache_type = cache_type;
	job->seg = seg;
	job->rambuf = rambuf;
	job->force_seal = force_seal;
	job->build_summary = summary;
	job->flush_data = flush_data;
	job->seg_length =seg_length;
	job->global_seg_sequence = global_seg_sequence;

	for(devno = 0;devno < NUM_SSD;devno++){
		atomic_set(&job->bios_start[devno], atomic_read(&bios_start[devno]));
		atomic_set(&job->bios_count[devno], atomic_read(&bios_count[devno]));
		//if(atomic_read(&bios_count[devno])==0){
		//	printk("WARN: dev = %d bios count = %d \n", devno, atomic_read(&bios_count[devno]));
		//}
	}

	spin_lock_irqsave(&flush_mgr->lock, flags);
	list_for_each_entry_reverse(prev_job, &flush_mgr->queue, list){
		if(prev_job->global_seg_sequence <  global_seg_sequence){
		//	printk(" >>>> seg found.. and then seg = %d, sequence = %d %d\n", (int)seg->seg_id, 
		//			(int)prev_job->global_seg_sequence, (int)global_seg_sequence);
			found = true;
			break;
		}
	}

	//printk(" make seg write seg = %d, length = %d, count = %d \n", (int)seg->seg_id, seg_length, 
	//		atomic_read(&flush_mgr->invoke_count));
	if(!found){
		list_add_tail(&job->list, &flush_mgr->queue);
	}else{
		list_add(&job->list, &prev_job->list);

	}

	atomic_inc(&flush_mgr->invoke_count);
	spin_unlock_irqrestore(&flush_mgr->lock, flags);

	wake_up_process(flush_mgr->daemon);
}


void bio_plugging(struct dmsrc_super *super, struct segment_header *seg, struct rambuffer *rambuf, 
				struct bio *bio, u32 update_mb_idx){
	int devno = get_devno(super, update_mb_idx);
	unsigned long flags, f;

	spin_lock_irqsave(&super->segbuf_mgr.lock, flags);
	spin_lock_irqsave(&rambuf->lock, f);
	rambuf->bios[update_mb_idx%STRIPE_SZ] = bio;
	BUG_ON(atomic_inc_return(&rambuf->bios_count[devno])>CHUNK_SZ);
	BUG_ON(atomic_inc_return(&rambuf->bios_total_count)>STRIPE_SZ);

//	if(bio){
//		_free_rambuf_page(super, rambuf->pages[update_mb_idx%STRIPE_SZ]); 
//		rambuf->pages[update_mb_idx%STRIPE_SZ] = NULL;
//		rambuf->alloc_count--;
//		atomic_dec(&rambuf->ref_count);
//	}

	spin_unlock_irqrestore(&rambuf->lock, f);
	spin_unlock_irqrestore(&super->segbuf_mgr.lock, flags);

	update_plug_deadline(super);
}

int get_desired_size(struct dmsrc_super *super, int seg_id){
	int size =  CHUNK_SZ * ((seg_id % (NUM_SSD-1)) + 1);
	//printk(" desired size = %d \n", size);
	size = CHUNK_SZ * 1;
	return size;
}

int process_write_request(struct dmsrc_super *super, 
							struct bio *bio,
							struct page *page,
							sector_t key,
							int is_dirty,
							unsigned long f, 
							int cache_type, 
							u32 crc32
							)
{
	struct segment_header *seg;
	struct metablock *new_mb;
	struct rambuffer *rambuf;
	bool need_refresh = false;
	bool need_summary = false;
	void *ptr;
	u32 total_count = 0;
	u32 mb_idx;
	u32 seg_length;
	atomic_t bios_start[MAX_CACHE_DEVS];
	atomic_t bios_count[MAX_CACHE_DEVS];
	u32 partial_test = 0;
	u64 global_seg_sequence;

	new_mb = alloc_mb_data(super, key, cache_type, !is_dirty, &total_count);
	if(test_bit(MB_DIRTY, &new_mb->mb_flags)!=is_dirty){
		printk(" invalid dirty flag = %d %d total_count = %d \n", test_bit(MB_DIRTY, &new_mb->mb_flags), is_dirty, total_count);
		printk(" cache type = %d \n", cache_type);
	}

	mb_idx = new_mb->idx;
	seg = super->current_seg[cache_type];
	rambuf = super->segbuf_mgr.current_rambuf[cache_type];

#if SUMMARY_LOCATION == USE_LAST_SUMMARY 
	if(need_chunk_summary(super, mb_idx+NUM_SUMMARY)){
		u32 summary_idx = data_to_summary_idx(super, mb_idx);
		alloc_mb_summary(super, seg, cache_type, summary_idx, &total_count);
		initialize_mb_summary(super, seg, cache_type, summary_idx, 1);
		need_summary = true;
		//printk(" Need summary in process_write_request ... \n");
	}
#endif

	if(super->param.bio_plugging){
		bio_plugging(super, seg, rambuf, bio, mb_idx);
#if SUMMARY_LOCATION == USE_LAST_SUMMARY 
		if(need_summary){
			int j;
			for(j = 0;j < NUM_SUMMARY;j++)
				bio_plugging(super, seg, rambuf, NULL, mb_idx + 1 + j);
		}
#endif
	}

	//if(total_count == STRIPE_SZ/2){
	//	partial_test = 1;
	//}

	if(partial_test || need_refresh_segment(super, cache_type, total_count)){
		if(!partial_test)
			super->segbuf_mgr.current_rambuf[cache_type] = NULL;

		need_refresh = true;
		global_seg_sequence = atomic64_inc_return(&super->flush_mgr.global_seg_sequence);

		if(partial_test){
			//printk(" Partial write seg = %d, count = %d \n", (int)seg->seg_id, total_count);
			alloc_partial_summary(super, seg, rambuf, cache_type);
		}

		make_bios_parity(super, seg, rambuf, 0);
		make_bios_vector(super, seg, rambuf, bios_start, bios_count);
		seg_length = atomic_read(&seg->length);
#if 0
		printk(" seg id = %d, seg length = %d valid count = %d(dirty = %d, clean = %d), dummy = %d  \n", (int)seg->seg_id, seg_length, 
			atomic_read(&seg->valid_count), 
			atomic_read(&seg->valid_dirty_count), 
			atomic_read(&seg->valid_clean_count), 
				atomic_read(&seg->dummy_count));
#endif 
	}
	UNLOCK(super, f);

	//if(bio){
	//	ptr = kmap_atomic(bio_page(bio));
	//	update_data_in_mb(super, mb_idx, seg, rambuf, cache_type, ptr, crc32);
	//	kunmap_atomic(ptr);
	//}else 
	if(page){
		ptr = page_address(page);  
		update_data_in_mb(super, mb_idx, seg, rambuf, cache_type, ptr, crc32);
	}else{
		ptr = NULL;
		update_data_in_mb(super, mb_idx, seg, rambuf, cache_type, ptr, 0);
	}

	BUG_ON(cache_type!=seg->seg_type);
	if(need_refresh){
		BUG_ON(test_bit(SEG_SEALED, &seg->flags));
	}

	if(super->param.bio_plugging){
		atomic_dec(&seg->num_filling);
		if(bio)
			atomic_inc(&super->cache_stat.inflight_bios);
		atomic_inc(&super->cache_stat.total_bios);

		if(need_refresh && !test_bit(SEG_SEALED, &seg->flags)){
			//if(partial_test)
			//	printk(" Flush partial seg write \n");
			make_flush_invoke_job(super, seg, rambuf, seg_length, bios_start, bios_count,
					cache_type, 0, 0, 1, global_seg_sequence);
		}
	}else{
		//struct wb_job *job;
		//atomic_dec(&seg->num_filling);
		//job = writeback_make_job(super, seg, mb_idx, bio, rambuf, 1);
		//writeback_issue_job(super, job);
		//if(need_summary){
		//	generate_full_summary(super, seg, rambuf, get_devno(super, mb_idx), 1);
		//}
		printk(" no queue not supported ... \n");
		BUG_ON(1);
	}

	return DM_MAPIO_SUBMITTED;
}

void do_degraded_worker(struct work_struct *work){
	struct degraded_manager *degraded_mgr = container_of(work, struct degraded_manager, work);
	struct dmsrc_super *super = degraded_mgr->super;
	struct list_head degraded_local_head;
	struct degraded_job *job, *job_tmp;
	unsigned long flags;
	struct bio *bio;
	void *ptr;
	void *srcs[MAX_CACHE_DEVS];
	void *dst;
	int i;


	INIT_LIST_HEAD(&degraded_local_head);

//	printk(" do degrade worker .... \n");

	spin_lock_irqsave(&degraded_mgr->lock, flags);
	list_for_each_entry_safe(job, job_tmp, &degraded_mgr->queue, degraded_list) {
		list_move_tail(&job->degraded_list, &degraded_local_head);
	}
	spin_unlock_irqrestore(&degraded_mgr->lock, flags);


	list_for_each_entry_safe(job, job_tmp, &degraded_local_head, degraded_list) {

		list_del(&job->degraded_list);
		bio = job->org_bio;
		BUG_ON(bio_sectors(bio)!=SRC_SECTORS_PER_PAGE);
		for(i = 0;i < NUM_DATA_SSD;i++){

			srcs[i] = job->buf + SRC_PAGE_SIZE * i;
#if 0 
			u32 checksum;
			struct metablock *read_mb;
			u32 offset;
			int j;

			j = job->mb->idx % CHUNK_SZ;
			offset = i*CHUNK_SZ+j;
			read_mb = get_mb(super, job->seg->seg_id, offset);
			if(i<2){
				checksum = crc32_le(17, srcs[i], PAGE_SIZE);
				if(checksum!=read_mb->checksum){
					printk(" checksum mismatch 0\n");
				}
			}
#endif 
		}

		dst = job->buf + NUM_DATA_SSD * SRC_PAGE_SIZE;
		memset(dst, 0xFF, SRC_PAGE_SIZE);
		//xor_blocks(NR_DATA_SSD, PAGE_SIZE, dst, srcs);
		run_xor(srcs, dst, NUM_DATA_SSD, SRC_PAGE_SIZE);

		ptr = bio_data(bio);
		if(memcmp(ptr, dst, SRC_PAGE_SIZE)){
			u32 checksum = crc32_le(CRC_SEED, ptr, SRC_PAGE_SIZE);

			printk(" parity inconsistency \n");

			if(checksum!=job->mb->checksum){
				printk(" checksum mismatch 1\n");
			}
			checksum = crc32_le(CRC_SEED, dst, SRC_PAGE_SIZE);
			if(checksum!=job->mb->checksum){
				printk(" checksum mismatch 2 %u %u\n", checksum, job->mb->checksum);
			}
		}

		BUG_ON(bio->bi_size!=SRC_PAGE_SIZE);
		memcpy(ptr, dst, SRC_PAGE_SIZE);
		
		mempool_free(job->buf, degraded_mgr->buf_pool);
		mempool_free(job, degraded_mgr->job_pool);

		bio_endio(bio, 0);
	}
}


static void degraded_io_complete(unsigned long error, void *__context)
{
	struct degraded_job *job = __context;
	struct dmsrc_super *super = job->super;
	struct degraded_manager *degraded_mgr = &super->degraded_mgr;
	unsigned long flags;

	if (error)
		job->error = 1;

	atomic_dec(&job->num_remaining_ios);

	if(atomic_read(&job->num_remaining_ios)){
		return;
	}

	spin_lock_irqsave(&degraded_mgr->lock, flags);
	list_add_tail(&job->degraded_list, &degraded_mgr->queue);
	spin_unlock_irqrestore(&degraded_mgr->lock, flags);

	queue_work(degraded_mgr->wq, &degraded_mgr->work);

	//printk(" degrade io complete \n");
}

void make_degraded_read(struct dmsrc_super *super, struct bio *bio, 
				struct segment_header *seg, struct metablock *mb){
	struct dm_io_region io;
	struct dm_io_request io_req;
	struct metablock *read_mb;
	struct degraded_job *job;
	u32 offset;
	u32 mem_offset = 0;
	int i;
	void *buf;
	struct degraded_manager *degraded_mgr = &super->degraded_mgr;

	//printk(" make degraded read idx = %d \n", (int)mb->idx);

	job = mempool_alloc(degraded_mgr->job_pool, GFP_NOIO);
	buf = mempool_alloc(degraded_mgr->buf_pool, GFP_NOIO);
	BUG_ON(!job);
	BUG_ON(!buf);

	job->super = super;
	job->mb = mb;
	job->seg = seg;
	job->org_bio = bio;
	atomic_set(&job->num_remaining_ios, NUM_SSD);
	job->buf = buf;
	job->error = 0;

	for(i = 0;i < NUM_SSD;i++){
		// for verify 
		if(i==super->recovery_mgr.failure_ssd)
			continue;
	
		offset = i*CHUNK_SZ+(mb->idx % CHUNK_SZ);
	//	printk(" offset = %d \n", offset);
		read_mb = get_mb(super, seg->seg_id, offset);

		io_req.mem.type = DM_IO_KMEM;
		io_req.mem.ptr.addr = buf + mem_offset;
		mem_offset += SRC_PAGE_SIZE;

		io_req.bi_rw = READ;
		io_req.notify.fn = degraded_io_complete;
		io_req.notify.context = job;
		io_req.client = super->io_client;

		io.bdev = get_bdev(super, offset);
		io.sector = get_sector(super, seg->seg_id, offset);
		io.count = SRC_SECTORS_PER_PAGE;
	//	printk(" Read: seg = %d, devno = %d, sector = %d \n", (int)seg->seg_id, (int)get_devno(super, calc_mb_start_sector(super, seg, read_mb->idx)), (int)io.sector); 

		dmsrc_io(&io_req, 1, &io, NULL);
	}

	io_req.mem.type = DM_IO_BVEC;
	io_req.mem.ptr.bvec = bio->bi_io_vec + bio->bi_idx;
	io_req.bi_rw = READ;
	io_req.notify.fn = degraded_io_complete;
	io_req.notify.context = job;
	io_req.client = super->io_client;

	io.bdev = get_bdev(super, mb->idx);
	io.sector = get_sector(super, seg->seg_id, mb->idx);
	io.count = SRC_SECTORS_PER_PAGE;
	dmsrc_io(&io_req, 1, &io, NULL);
}

#if 0 
void print_inode(struct dmsrc_super *super, struct bio *bio){
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_idx;
	struct page *page = bvec->bv_page;
	struct dm_dev *origin_dev = super->dev_info.origin_dev;
	//struct block_device *bdev = dev->bdev;


#if 0 
	if(page && page_has_buffers(page)){
		struct address_space *mapping = page->mapping;
		struct inode *host = mapping->host;
		struct inode *inode = bdev->bd_inode;

		printk(" offset = %d \n", (int) page->index);
		if(host)
			printk(" inode ino = %lu, %lu %lu\n", host->i_ino, inode->i_ino, inode->i_size);
	//	find_get_page

	}
#endif

	printk(" pid = %d %s\n", current->pid, current->comm);
}
#endif 

static int process_read_miss_request(struct dmsrc_super *super, struct bio *bio, unsigned long f){
	struct dm_dev *origin_dev = super->dev_info.origin_dev;

	UNLOCK(super, f);

	bio_remap(bio, origin_dev, bio->bi_sector);
	read_caching_make_job(super, NULL, NULL, bio, NULL);

	return DM_MAPIO_SUBMITTED;

}

static int process_read_hit_request(struct dmsrc_super *super, struct bio *bio,
		struct segment_header *seg,
		struct metablock *mb,
		unsigned long f){

	struct bio_ctx *map_context;
	struct dm_target *ti = super->ti;
	u32 tmp32;

	map_context = dm_per_bio_data(bio, ti->per_bio_data_size);

	if(test_bit(SEG_SEALED, &seg->flags)){
		set_bit(SEG_HIT, &seg->flags);

		if(!test_bit(MB_HIT, &mb->mb_flags)){
			set_bit(MB_HIT, &mb->mb_flags);
			atomic_inc(&seg->hot_count);
			if(!test_bit(MB_DIRTY, &mb->mb_flags))
				atomic_inc(&seg->hot_clean_count);
		}
	}

	atomic_inc(&seg->num_read_inflight);
	UNLOCK(super, f);

	map_context->ptr = seg;

	div_u64_rem(bio->bi_sector, 1 << 3, &tmp32);
	if(tmp32){
		printk(" read hit: partial read: sector %d offset = %d, size = %d \n", (int)bio->bi_sector, tmp32,
				bio->bi_size);
		printk(" read hit: partial read: mb->idx %d \n", mb->idx);
	//	BUG_ON(1);
	}

	bio_remap(bio,
			  get_dmdev(super, mb->idx),
			  get_sector(super, seg->seg_id, mb->idx)
			  + tmp32);

	if(atomic_read(&super->degraded_mode) && test_bit(MB_BROKEN, &mb->mb_flags)){
		BUG_ON(!test_bit(MB_DIRTY, &mb->mb_flags));
		BUG_ON(bio->bi_size!=SRC_PAGE_SIZE);
		//printk(" require reconstruct request \n");

		if(test_bit(SEG_SEALED, &seg->flags)){
			make_degraded_read(super, bio, seg, mb);
		}else{
			printk(" seg is not sealed ... \n");
			generic_make_request(bio);
		}
	}else{
		generic_make_request(bio);
	}


	return DM_MAPIO_SUBMITTED;
}


#if defined(CONFIG_BCACHE) || defined(CONFIG_BCACHE_MODULE)
#define ewma_add(ewma, val, weight, factor)				\
({									\
	(ewma) *= (weight) - 1;						\
	(ewma) += (val) << factor;					\
	(ewma) /= (weight);						\
	(ewma) >> factor;						\
})


static struct hlist_head *iohash(struct seq_io_detector *dc, uint64_t k)
{
	return &dc->io_hash[hash_64(k, RECENT_IO_BITS)];
}

static void add_sequential(struct task_struct *t)
{
	ewma_add(t->sequential_io_avg,
		 t->sequential_io, 8, 0);

	t->sequential_io = 0;
}

/*
This code is based on bcache (drivers/md/bcache/request.c) 
 */

bool detect_sequential_io(struct dmsrc_super *super, struct bio *bio){
	struct io *i;
	unsigned sectors;
	int use_merge = 1;
#if defined(CONFIG_BCACHE) || defined(CONFIG_BCACHE_MODULE)
	unsigned long flags;
#endif 
	struct seq_io_detector *seq_detector = &super->seq_detector;
	bool r = false;

	if(!super->param.sequential_enable){
		return false;
	}
	if (bio->bi_rw & REQ_DISCARD) {
		return false;
	}

	spin_lock_irqsave(&seq_detector->seq_lock, flags);
	if (use_merge) {

		hlist_for_each_entry(i, iohash(seq_detector, bio->bi_sector), hash)
			if (i->last == bio->bi_sector &&
			    time_before(jiffies, i->jiffies))
				goto found;

		i = list_first_entry(&seq_detector->io_lru, struct io, lru);

		add_sequential(current);
		i->sequential = 0;
found:
		if (i->sequential + bio->bi_size > i->sequential)
			i->sequential	+= bio->bi_size;

		i->last			 = bio_end_sector(bio);
		i->jiffies		 = jiffies + msecs_to_jiffies(5000);
		current->sequential_io	 = i->sequential;

		hlist_del(&i->hash);
		hlist_add_head(&i->hash, iohash(seq_detector, i->last));
		list_move_tail(&i->lru, &seq_detector->io_lru);

	} else {
		current->sequential_io = bio->bi_size;
		add_sequential(current);
	}
	spin_unlock_irqrestore(&seq_detector->seq_lock, flags);

	sectors = max(current->sequential_io,
		      current->sequential_io_avg) >> 9;

	if (super->param.sequential_cutoff &&
	    sectors >= super->param.sequential_cutoff >> 9) {
		r = true;
	}


	return r;
}
#endif 

inline bool should_need_refresh_seg(struct dmsrc_super *super, int cache_type){
	bool need_refresh = false;
	int count;

	count = ma_get_count(super, cache_type);
	if(need_refresh_segment(super, cache_type, count))
		need_refresh = true;
	return need_refresh;
}


int try_wait_free_seg(struct dmsrc_super *super, int cache_type){

	if((atomic_read(&super->seg_allocator.seg_alloc_count)<=
		MIGRATE_HIGHWATER - (MIGRATE_HIGHWATER - MIGRATE_LOWWATER)/2
		||atomic_read(&super->seg_allocator.group_alloc_count)<=1)	
		&& atomic_read(&super->migrate_mgr.migrate_triggered)){
			return 1;
	}
	
	if(should_need_refresh_seg(super, cache_type)){
#ifdef FORCE_UMAX
		if(atomic_read(&super->seg_allocator.seg_alloc_count)<MIGRATE_LOWWATER || 
			get_curr_util(super) > super->param.u_max || 
			atomic_read(&super->seg_allocator.group_alloc_count)<=1){
#else
		if(atomic_read(&super->seg_allocator.seg_alloc_count)<MIGRATE_LOWWATER || 
			atomic_read(&super->seg_allocator.group_alloc_count)<=1){
#endif 
			if(!atomic_read(&super->migrate_mgr.migrate_triggered)){
				atomic_set(&super->migrate_mgr.migrate_triggered, 1);
				super->migrate_mgr.allow_migrate = true;
				wake_up_process(super->migrate_mgr.daemon);
				//printk(" migration daemon is invoked ... \n");
			}
			return 1;
		}

		if(can_get_free_segment(super, cache_type, 0)){
			alloc_new_segment(super, cache_type, false);
		}else{
			return 1;
		}
	}

	return 0;
}


int should_bypass_bio(struct dmsrc_super *super, 
		struct segment_header *seg, 
		struct metablock *mb, 
		struct bio *bio,
		int is_write){

	struct bio_ctx *map_context =
		dm_per_bio_data(bio, super->ti->per_bio_data_size);
	bool can_bypass = false;
	bool seq_io;
	bool cold_io = false;

	if(is_write){
		if(!map_context->hot_io && get_curr_util(super) > super->param.u_max){
			cold_io = true;
		}
	}else{
		if(!map_context->hot_io && get_curr_util(super) > super->param.u_max){
			cold_io = true;
		}
	}

	seq_io = (bool)map_context->seq_io;
	if(seq_io || cold_io){
		bool remapped = false;

		if(!mb){
			remapped = true;
		}else{
			if(is_write){ // write 
				BUG_ON( !test_bit(MB_SEAL, &mb->mb_flags) && is_gc_stripe(seg->seg_type));
				invalidate_previous_cache(super, seg, mb);
				remapped = true;
			}
		}

		if(remapped){
			if(cold_io)
				atomic_inc(&super->wstat.cold_bypass_count);
			else if(seq_io)
				atomic_inc(&super->wstat.seq_bypass_count);

			can_bypass = true;
			goto do_bypass;
		}
	} 

	if(atomic_read(&super->resize_mode)){
		if(!mb){
			can_bypass = true;
		}else{
			printk(" data is found\n");
			BUG_ON(1);
		}
	}

	if(atomic_read(&super->degraded_mode)){
		if(!mb){
			can_bypass = true;
		}else{
			if(is_write){ // write case 
				invalidate_previous_cache(super, seg, mb);
				can_bypass = true;
			}else{ // read case 
				if(test_bit(MB_BROKEN, &mb->mb_flags)){
					printk(" need reconstruct read \n");
				}
			}
		}
	}

do_bypass:

	if(can_bypass){
		return 1;
	}

	return 0;
}

#if 0 
int try_get_free_segment(struct dmsrc_super *super, bool found, int rw){
	int cache_type;

	if(super->param->striping_policy==MIXED_STRIPING)
		rw = 1;

	if(rw){ // write cache
		cache_type = superUF;
	}else{ // read cache 
		cache_type =RBUF;
	}

	if(should_need_refresh_seg(super, rw)){
		if(can_get_free_segment(super, cache_type)){
			BUG_ON(atomic_read(&super->rambuf_inactive_count)==0);
			alloc_new_segment(super, cache_type, false);
		}else{
			return -1;
		}
	}

	return 0;
}
#endif 


#if 0
int update_ghost_buffer(struct dmsrc_super *super, sector_t sector, int is_write, int *is_hot){
	struct cache_manager *lru_manager = super->lru_manager;
	struct lru_node *ln = NULL;

	ln = CACHE_SEARCH(lru_manager, sector);
	if(!ln){
		ln = CACHE_REPLACE(lru_manager, 0);
		ln = CACHE_ALLOC(lru_manager, ln, sector);
		CACHE_INSERT(lru_manager, ln);
		*is_hot = 0;
	}else{
		*is_hot = 1;
		ln = CACHE_REMOVE(lru_manager, ln);
		CACHE_INSERT(lru_manager, ln);
		if(is_write)
			ln->cn_write_hit++;
		else
			ln->cn_read_hit++;
	}
	if(is_write)
		ln->cn_write_ref++;
	else
		ln->cn_read_ref++;

	if(is_write){
		if(*is_hot)
			return 1;
	}else{
		if(ln->cn_write_ref==0&&ln->cn_read_ref>1)
			return 1;
	}

	return 0;
}
#endif 

void update_stat(struct dmsrc_super *super, struct metablock *mb, int is_write){
	super->wstat.count++;
	if(is_write)
		super->wstat.write_count++;
	else
		super->wstat.read_count++;

	if (mb) {
		super->wstat.hit++;
		if(is_write)
			super->wstat.write_hit++;
		else
			super->wstat.read_hit++;
	}
}

static int preprocess_pending_bio(struct dmsrc_super *super, 
		struct segment_header *seg, 
		struct bio *bio, 
		struct metablock *mb, 
		int is_write)
{
	int res = 0;

	if(mb){

		if(unlikely(test_bit(SEG_MIGRATING, &seg->flags))){
			res = RES_MIG;
			goto pending_process;
		}

		if(unlikely(test_bit(SEG_PARTIAL, &seg->flags) ||
					test_bit(SEG_RECOVERY, &seg->flags) )) 
		{
			res = RES_PARTIAL;
			goto pending_process;
		}

		if(unlikely(!test_bit(MB_SEAL, &mb->mb_flags))){
			if(!is_write){
				res = RES_SEAL;
				goto pending_process;
			}//else{
			//	printk(" mb write hit .... \n");
			//}
		}

		if(unlikely(atomic_read(&super->resize_mode))){
			goto pending_process;
		}
	}

#if 1
	/* Don't process bypass to HDD */
	// KH_SWAN_CUT_OFF_HDD_PATH
#else
	if(unlikely(should_bypass_bio(super, seg, mb, bio, is_write) && 
		!(bio->bi_rw & REQ_DISCARD))){
		res = RES_BYPASS;
		goto bypass_process;
	}
#endif

	//if(is_write || (!is_write && !mb)){
	if(is_write){
		if(super->current_group && atomic_read(&super->current_group->free_seg_count)==0){
			if(should_need_refresh_seg(super, WCBUF) && 
				!should_need_refresh_seg(super, RCBUF)){
				printk(" * dirty seg write: need fill RCBUF with dummy ... \n");
			}
		}

		if(try_wait_free_seg(super, WCBUF)){
			res = RES_NOFREE1;
			goto pending_process;
		}
	}

	return 0;

pending_process:
	return res;

bypass_process:
	return res;

}

#if 0
int lookup_dram_cache(struct dmsrc_super *super, sector_t sector){
	struct cache_manager *lru_manager = super->clean_dram_cache_manager;
	struct lru_node *ln = NULL;

	ln = CACHE_SEARCH(lru_manager, sector);
	if(!ln){
		ln = CACHE_REPLACE(lru_manager, 0);
		ln = CACHE_ALLOC(lru_manager, ln, sector);
		CACHE_INSERT(lru_manager, ln);
		atomic_set(&ln->sealed, 0);
	}else{
		ln = CACHE_REMOVE(lru_manager, ln);
		CACHE_INSERT(lru_manager, ln);
		ln->cn_read_hit++;
	}

	ln->cn_read_ref++;

	return 0;
}
#endif

static int process_read_request(struct dmsrc_super *super, struct segment_header *seg, 
		struct metablock *mb, struct bio *bio, sector_t key, unsigned long f){
	struct cache_manager *clean_cache = super->clean_dram_cache_manager;
	struct lru_node *ln = NULL;
	unsigned long lru_flags; 
	int dram_hit = 0;
	int sealed = 0;
	int res;

	if(!mb && super->param.enable_read_cache){ // not in SSDs 
		spin_lock_irqsave(&clean_cache->lock, lru_flags);
		ln = CACHE_SEARCH(clean_cache, key);
		if(!ln){
			dram_hit = 0;

			if(clean_cache->cm_free){
				ln = CACHE_ALLOC(clean_cache, ln, key);
				CACHE_INSERT(clean_cache, ln);
				atomic_set(&ln->sealed, 0);
				atomic_set(&ln->locked, 0);
			}
			
		}else{
			printk(" hit in dram ... \n");
			dram_hit = 1;
			ln = CACHE_REMOVE(clean_cache, ln);
			CACHE_INSERT(clean_cache, ln);
			ln->cn_read_hit++;
		}

		if(ln){
			if(atomic_read(&ln->sealed))
				sealed = 1;
			else
				sealed = 0;
		}
		spin_unlock_irqrestore(&clean_cache->lock, lru_flags);

		if(!ln){
			UNLOCK(super, f);
			//printk(" no lru buffers in dram \n");
			bio_remap(bio, super->dev_info.origin_dev, bio->bi_sector);
			generic_make_request(bio);
			return DM_MAPIO_SUBMITTED;
		}
	}

	if(dram_hit){
		BUG_ON(mb);
		if(sealed){
			UNLOCK(super, f);
			BUG_ON(1);
			printk(" dram hit sealed %d .. \n", (int)key);
			bio_endio(bio, 0);
			return DM_MAPIO_SUBMITTED;
		}else{
			UNLOCK(super, f);
			printk(" dram hit but not sealed..pending queue = %d \n", (int)key);
			BUG_ON(1);
			res = RES_DRAM;
			//goto pending_process;
		}
	}else{
		if (!mb) { // not found
			//printk(" read miss in ssd cache .. %d \n", (int)key);
			return process_read_miss_request(super, bio, f);
		}else{
			//printk(" read hit in ssd cache .. \n");
			return process_read_hit_request(super, bio, seg, mb, f);
		}
	}

}

static int map_pending_bio(struct dmsrc_super *super, struct bio *bio)
{
	struct dm_target *ti = super->ti;
	struct segment_header *seg = NULL;
	struct metablock *mb = NULL;
	struct bio_ctx *map_context = NULL;

	struct cache_manager *clean_cache = super->clean_dram_cache_manager;
	struct lru_node *ln = NULL;
	unsigned long lru_flags; 

	sector_t key;
	int res = 0;
	int is_write;
	unsigned long f;

	map_context = dm_per_bio_data(bio, ti->per_bio_data_size);

	if(atomic_read(&super->migrate_mgr.background_gc_on)){
		atomic_set(&super->migrate_mgr.background_gc_on, 0);
	}

	is_write = bio_data_dir(bio);
	key  = calc_cache_alignment(super, bio->bi_sector);

#if 0
	if(bio){
		if(is_write){
			printk(" write: map bio sector = %d count = %d \n", (int)bio->bi_sector, (int)bio->bi_size);
		}else{
			printk(" read: map bio sector = %d count = %d \n", (int)bio->bi_sector, (int)bio->bi_size);
		}
	}
#endif 

	LOCK(super, f);
	mb = ht_lookup(super, key);
	if (mb) {
		seg = get_segment_header_by_mb_idx(super, mb->idx);
	}

	update_stat(super, mb, is_write);
///		if(!is_write && !super->param.enable_read_cache)
//			admit = 0;

	res = preprocess_pending_bio(super, seg, bio, mb, is_write);
	if(res && res < RES_BYPASS)
		goto pending_process;
	else if(res == RES_BYPASS)
		goto bypass_process;

	if(super->param.enable_read_cache){
		spin_lock_irqsave(&clean_cache->lock, lru_flags);
		ln = CACHE_SEARCH(clean_cache, key);
		if(ln){
			if(mb)
				printk(" WARN: hit in both SSD and DRAM \n");
			
			if(is_write){
				if(atomic_read(&ln->locked)){
				//if(!atomic_read(&ln->sealed)||atomic_read(&ln->locked)){
					res = RES_DRAM;
					//printk(" Write hit in DRAM ... \n");
					spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
					goto pending_process;
				}
				// write hit in DRAM
				//BUG_ON(!atomic_read(&ln->sealed));
				ln = CACHE_REMOVE(clean_cache, ln);
				if(atomic_read(&ln->sealed)){
					atomic_dec(&clean_cache->cm_sealed_count);
				}
				list_add(&ln->cn_list, &clean_cache->cm_free_head);
			}else{
				if(!atomic_read(&ln->sealed)||atomic_read(&ln->locked)){
					res = RES_DRAM;
					spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
					goto pending_process;
				}else{
					// clean hit in DRAM
					ln = CACHE_REMOVE(clean_cache, ln);
					CACHE_INSERT(clean_cache, ln);
					spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
					UNLOCK(super, f);
				//	printk(" clean hit in dram ...key = %d \n", (int)key);
					bio_endio(bio, 0);
					return DM_MAPIO_SUBMITTED;
				}
			}
		}
		spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
	}

	if (!is_write) { // read cache 
	//	if(admit){
			return process_read_request(super, seg, mb, bio, key, f);
	//	}else{ // no cache on read miss
	//		if(mb){
	//			return process_read_hit_request(super, bio, seg, mb, f);
	//		}else{
	//			UNLOCK(super, f);
	//			bio_remap(bio, super->dev_info.origin_dev, bio->bi_sector);
	//			generic_make_request(bio);
	//			return DM_MAPIO_SUBMITTED;
	//		}
	//	}
	}

	map_context->inflight = 1;
	atomic_inc(&super->cache_stat.inflight_ios);
	atomic_inc(&super->cache_stat.total_ios);
	// write cache 

	if (mb) { // found in the cache 
		BUG_ON(seg->seg_type==GRBUF&&!test_bit(MB_SEAL, &mb->mb_flags));
		invalidate_previous_cache(super, seg, mb);
	}

	//if(!mb && !admit){ // no hit && no admission
	//	BUG_ON(1);
	//	UNLOCK(super, f);
	//	bio_remap(bio, super->dev_info.origin_dev, bio->bi_sector);
	//	generic_make_request(bio);
	//	return DM_MAPIO_SUBMITTED;
	//}else{
	//	BUG_ON(atomic_read(&super->degraded_mode));
#if 1
		return process_write_request(super, bio, bio_page(bio), key, 1, f, WCBUF, map_context->crc32);
#else
		bio_endio(bio, 0);
#endif 
	//}

pending_process:
	UNLOCK(super, f);
	return -res;

bypass_process:
	UNLOCK(super, f);
	bio_remap(bio, super->dev_info.origin_dev, bio->bi_sector);
	generic_make_request(bio);
	return DM_MAPIO_SUBMITTED;
}

void pending_bio_add(struct dmsrc_super *super, struct bio *bio){
	struct pending_manager *pending_mgr = &super->pending_mgr;
	unsigned long f;
	//struct timeval tv;
	struct timespec ts;


	spin_lock_irqsave(&pending_mgr->lock, f);

	bio_list_add(&pending_mgr->bios, bio);
	atomic64_inc(&pending_mgr->io_count);

	if(bio_data_dir(bio)){
		///do_gettimeofday(&tv);
		getnstimeofday(&ts);
		pending_mgr->arrival_times[atomic_read(&pending_mgr->arrival_cur)].tv_sec = ts.tv_sec;
		pending_mgr->arrival_times[atomic_read(&pending_mgr->arrival_cur)].tv_nsec = ts.tv_nsec;

		atomic_inc(&pending_mgr->arrival_count);

		if(atomic_inc_return(&pending_mgr->arrival_cur)==STRIPE_SZ){
			atomic_set(&pending_mgr->arrival_cur, 0);
		}

		if(atomic_read(&pending_mgr->arrival_cur)==atomic_read(&pending_mgr->arrival_start)){
			if(atomic_inc_return(&pending_mgr->arrival_start)==STRIPE_SZ){
				atomic_set(&pending_mgr->arrival_start, 0);
			}
		}

		//printk(" arrival start = %d, cur = %d \n", 
		//		atomic_read(&pending_mgr->arrival_start), atomic_read(&pending_mgr->arrival_cur));
	}

	spin_unlock_irqrestore(&pending_mgr->lock, f);
}

bool pending_bio_empty(struct dmsrc_super *super){
	struct pending_manager *pending_mgr = &super->pending_mgr;
	int empty = 0;
	unsigned long f;

	spin_lock_irqsave(&pending_mgr->lock, f);
	if(bio_list_empty(&pending_mgr->bios))
		empty = 1;
	spin_unlock_irqrestore(&pending_mgr->lock, f);
	
	spin_lock_irqsave(&pending_mgr->barrier_lock, f);
	if(bio_list_empty(&pending_mgr->barrier_ios))
		empty += 1;
	
	spin_unlock_irqrestore(&pending_mgr->barrier_lock, f);


	if(empty==2)
		return 1;

	return 0;
}

void pending_worker_schedule(struct dmsrc_super *super){
	if(atomic64_read(&super->pending_mgr.io_count) || 
		atomic_read(&super->clean_dram_cache_manager->cm_sealed_count)){
		queue_work(super->pending_mgr.wq, &super->pending_mgr.work);
	}
}

int need_clean_seg_write(struct dmsrc_super *super){
	struct cache_manager *clean_cache = super->clean_dram_cache_manager;

	if(!super->param.enable_read_cache)
		return 0;

	if((atomic_read(&super->seg_allocator.seg_alloc_count)<=
		MIGRATE_HIGHWATER - (MIGRATE_HIGHWATER - MIGRATE_LOWWATER)/2
		||atomic_read(&super->seg_allocator.group_alloc_count)<=2)	
		&& atomic_read(&super->migrate_mgr.migrate_triggered)){
			return 0;
	}

	//if(atomic_read(&clean_cache->cm_sealed_count)>=clean_cache->cm_size/2){
	//if(atomic_read(&clean_cache->cm_sealed_count)>=STRIPE_SZ*128){
	if(atomic_read(&clean_cache->cm_sealed_count)>=STRIPE_SZ){
		return 1;
	}

	return 0;
}

int clean_seg_write(struct dmsrc_super *super, int need_count, int cache_type){
	struct cache_manager *clean_cache = super->clean_dram_cache_manager;
	struct lru_node *ln, *temp;
	struct list_head clean_seg_head;
	struct segment_header *seg;
	struct metablock *mb;
	unsigned long flags;
	unsigned long lru_flags;
	int count = 0;

#if 0
	if(STRIPING_POLICY==MIXED_STRIPING){
		cache_type = WCBUF;
		need_count = get_data_max_count(super, WCBUF);
	}else{
		cache_type = RCBUF;
		need_count = get_data_max_count(super, RCBUF);

		if(super->current_group && atomic_read(&super->current_group->free_seg_count)==0){
			if(!should_need_refresh_seg(super, WCBUF) && 
				should_need_refresh_seg(super, RCBUF)){
				cache_type = WCBUF;
				need_count = ma_get_free(super, WCBUF);
				printk(" * clean seg write: need fill WCBUF with dummy ... count = %d \n", need_count);
			}
		}
	}
#endif 

	INIT_LIST_HEAD(&clean_seg_head);

	LOCK(super, flags);
	spin_lock_irqsave(&clean_cache->lock, lru_flags);
	list_for_each_entry_safe(ln, temp, &clean_cache->cm_head, cn_list){
		if(atomic_read(&ln->sealed)){
			//CACHE_REMOVE(clean_cache, ln);
			atomic_set(&ln->locked, 1);
			list_add_tail(&ln->cn_write_list, &clean_seg_head);
#if 1
			if(++count==need_count)
				break;
#else
			count++;
#endif 
		}
	}

	spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
	UNLOCK(super, flags);

	if(count!=need_count)
		printk("clean data count = %d, need count = %d  \n", count, need_count );

	//printk("clean seg write:  data count = %d, need count = %d  \n", count, need_count );

	list_for_each_entry_safe(ln, temp, &clean_seg_head, cn_write_list){
		LOCK(super, flags);
		if(should_need_refresh_seg(super, cache_type)){
RETRY:
			//if(try_wait_free_seg(super, RCBUF, 0)){
			if(try_wait_free_seg(super, cache_type)){
				UNLOCK(super, flags);
				while(1){
					printk(" clean_seg_write: no more free group = %d segs %d, migrate = %d ram buf ...%d \n", 
						atomic_read(&super->seg_allocator.group_alloc_count),
						atomic_read(&super->seg_allocator.seg_alloc_count), 
						atomic_read(&super->migrate_mgr.migrate_triggered),
						atomic_read(&super->segbuf_mgr.inactive_page_count));
					schedule_timeout_interruptible(msecs_to_jiffies(1000));
					LOCK(super, flags);
					goto RETRY;
				}
			}
		}
	
		mb = ht_lookup(super, ln->cn_blkno);
		if (mb) {
			seg = get_segment_header_by_mb_idx(super, mb->idx);

			printk(" WARN: clean write data hit in seg = %d idx = %d(dirty=%d, valid= %d, type = %d  \n", 
					(int)seg->seg_id, mb->idx, 
					test_bit(MB_DIRTY, &mb->mb_flags),
					test_bit(MB_VALID, &mb->mb_flags),
					seg->seg_type);

			invalidate_previous_cache(super, seg, mb);
		}

		process_write_request(super, NULL, ln->cn_page, ln->cn_blkno, 0, flags, cache_type, ln->crc32);

//		printk(" write clean cached block = %d %d \n", ln->cn_blkno,
//				ma_get_count(super, cache_type));
	}

	if(STRIPING_POLICY!=MIXED_STRIPING && !should_need_refresh_seg(super, cache_type)){
		printk(" WARN: clean stripe is not fully filled with clean data %d \n", ma_get_count(super, cache_type));
	}


	LOCK(super, flags);
	spin_lock_irqsave(&clean_cache->lock, lru_flags);
	list_for_each_entry_safe(ln, temp, &clean_seg_head, cn_write_list){
		CACHE_REMOVE(clean_cache, ln);
		atomic_dec(&clean_cache->cm_sealed_count);
		list_add(&ln->cn_list, &clean_cache->cm_free_head);
		list_del(&ln->cn_write_list);
		atomic_set(&ln->locked, 0);
	}
	//printk(" clean seg write = %d, free = %d, count = %d  \n", count, 
	//		clean_cache->cm_free, clean_cache->cm_count);
	spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
	UNLOCK(super, flags);

	return count;
}

int try_clean_seg_write(struct dmsrc_super *super){
	//struct cache_manager *clean_cache = super->clean_dram_cache_manager;
	int clean_write_count;
	int total_write_count = 0; 
	int cache_type;

	if(need_clean_seg_write(super) && super->param.enable_read_cache){
		int process_count;

		//if(atomic_read(&clean_cache->cm_sealed_count)>=STRIPE_SZ){

		if(STRIPING_POLICY==MIXED_STRIPING)
			cache_type = WCBUF;
		else
			cache_type = RCBUF;

		clean_write_count = get_data_max_count(super, cache_type);
		
		if(super->current_group && atomic_read(&super->current_group->free_seg_count)==0){
			if(!should_need_refresh_seg(super, WCBUF) && 
				should_need_refresh_seg(super, RCBUF)){
				cache_type = WCBUF;
				clean_write_count = ma_get_free(super, WCBUF);
				//printk(" * clean seg write: need fill WCBUF with dummy ... count = %d \n", clean_write_count);
			}
		}

		process_count = clean_seg_write(super, clean_write_count, cache_type);
		if(process_count != clean_write_count){
			//printk(" WARN: ramaining clean write count = %d \n", clean_write_count);
		}else{
			total_write_count += clean_write_count;
		}
	}

	//if(total_write_count)
	//	printk(" total clean write count = %d \n", total_write_count);

	return total_write_count;
}

int get_partial_seg_length(struct dmsrc_super *super, int cache_type){
	unsigned long f;
	struct rambuffer *rambuf;
	int length;

	LOCK(super, f);
	rambuf = super->segbuf_mgr.current_rambuf[cache_type];
	if(!rambuf)
		length = 0;
	else
		length = atomic_read(&rambuf->bios_total_count) - atomic_read(&rambuf->bios_total_start);
	UNLOCK(super, f);

	return length;
}

void pending_worker(struct work_struct *work){
	struct pending_manager *pending_mgr = 
						container_of(work, struct pending_manager,
					    work);
	struct dmsrc_super *super = pending_mgr->super;
	struct bio_list local_list;
	struct bio *bio;
	unsigned long f;
	int retry_reasons[RES_COUNT];
	int local_count = 0;
	int i;
	int is_write = 0;
	int clean_write_count = 0;

	for(i=0;i<RES_COUNT;i++){
		retry_reasons[i] = 0;
	}

	bio_list_init(&local_list);

	while (1) {
		int reason;

		spin_lock_irqsave(&pending_mgr->lock, f);
		bio = bio_list_pop(&pending_mgr->bios);
		if(bio)
			atomic64_dec(&pending_mgr->io_count);
		spin_unlock_irqrestore(&pending_mgr->lock, f);

		if(!bio)
			break;

		if(bio_data_dir(bio))
			is_write = 1;
#if 1
		reason = map_pending_bio(super, bio);
		if(reason<0){
			bio_list_add(&local_list, bio);
			local_count++;
			retry_reasons[reason*-1]++;
		}
#else // test for null device performance 
		bio_endio(bio, 0);
#endif 

		clean_write_count += try_clean_seg_write(super);
	}

	clean_write_count += try_clean_seg_write(super);

	if(local_count){
		//debug 
		//for(i=0;i<RES_COUNT;i++){
		//	if(retry_reasons[i])
		//		printk(" pending i/o reason  = %d %d \n", i, retry_reasons[i]);
		//}
		spin_lock_irqsave(&pending_mgr->lock, f);
		bio_list_merge(&pending_mgr->bios, &local_list);
		for(i=0;i<local_count;i++){
			atomic64_inc(&pending_mgr->io_count);
		}
		spin_unlock_irqrestore(&pending_mgr->lock, f);
	}

#if 0
	bio_list_init(&local_list);
	spin_lock_irqsave(&pending_mgr->barrier_lock, f);
	bio_list_merge(&local_list, &pending_mgr->barrier_ios);
	bio_list_init(&pending_mgr->barrier_ios);
	spin_unlock_irqrestore(&pending_mgr->barrier_lock, f);

	issue_deferred_bio(super, &local_list);
#endif 

	if(clean_write_count || (!local_count && is_write) || 
			(retry_reasons[RES_SEAL] && get_partial_seg_length(super, WCBUF))){

		update_sync_deadline(super);
	//	printk(" clean_write count = %d, local_count = %d, is write = %d \n", 
	//			clean_write_count, local_count, is_write);
	}else{
		//printk(" partial seg length = %d \n", get_partial_seg_length(super, WCBUF));

	}
	//printk(" pending worker sleep ... queue = %d \n", (int)atomic64_read(&super->pending_mgr.io_count));
//	flush_pending_bios(super);

}
#if 0 
void map_debugging(){
#if 0 // Debugging
	 if(bio->bi_rw & REQ_FUA) 
		 printk(" req fua \n");

//	print_inode(super, bio);

	while(bio->bi_size>SRC_PAGE_SIZE){
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
		printk(" size = %d \n", bio->bi_size);
	}
	//if(SRC_SECTORS_PER_PAGE!=bio_count && (strcmp(current->comm, "blkid")&&strcmp(current->comm, "udisks-part-id"))){
	//if(SRC_SECTORS_PER_PAGE!=bio_count){
	//	printk(" rw = %d\n", (int)bio_data_dir(bio));
	//	printk(" bio count = %d, process = %s \n",(int)bio_count, current->comm);
	//}

	if(bio->bi_sector%SRC_SECTORS_PER_PAGE && bio_data_dir(bio)){
		printk(" unaligned sector number = %u %u\n", (u32)bio->bi_sector, (u32)bio->bi_sector % SRC_SECTORS_PER_PAGE);
	}
#endif 
}
#endif 

static int dmsrc_map(struct dm_target *ti, struct bio *bio)
{
	struct segment_header *uninitialized_var(seg);
	struct bio_ctx *map_context;
	sector_t bio_count;
	bool bio_fullsize;
	struct dmsrc_super *super = ti->private;
	struct pending_manager *pending_mgr = &super->pending_mgr;
#ifndef USE_PENDING_WORKER
	int ret = 0;
#endif

	map_context = dm_per_bio_data(bio, ti->per_bio_data_size);
	map_context->ptr = NULL;
	map_context->inflight = 0;

	if (bio->bi_rw & REQ_DISCARD) {
		struct dm_dev *origin_dev = super->dev_info.origin_dev;
		bio_remap(bio, origin_dev, bio->bi_sector);
		printk(" WARN: discard command ... \n");
		return DM_MAPIO_REMAPPED;
	}

	/* It doesn't support REQ_FLUSH */
	if (bio->bi_rw & REQ_FLUSH) {
#if 0
		BUG_ON(bio->bi_size);
		bio_endio(bio, 0);
		return DM_MAPIO_SUBMITTED;
#else
		queue_barrier_io(super, bio);
		update_sync_deadline(super);
		return DM_MAPIO_SUBMITTED;
		//return DM_MAPIO_REMAPPED;
#endif
	}

#if 0 // test for dm interface 
	bio_endio(bio, 0);
	return DM_MAPIO_SUBMITTED;
#endif 

	bio->bi_rw &= ~REQ_SYNC;
	bio_count = bio->bi_size >> SECTOR_SHIFT;
	bio_fullsize = (bio_count == (1 << 3));

	if(!bio_fullsize)
		printk(" bio count = %d sectors \n", (int)bio_count);
	
	wp_update(super, bio_data_dir(bio), REQ_CATEGORY_NORMAL);
#ifdef TRACE_GC
	atomic64_inc(&super->wstat.normal_io_cnt[bio_data_dir(bio)]); 
#endif


	map_context->seq_io = false;
#if defined(CONFIG_BCACHE) || defined(CONFIG_BCACHE_MODULE)
	map_context->seq_io = detect_sequential_io(super, bio);
#endif 


	if((super->param.data_allocation==DATA_ALLOC_FLEX_VERT || 
		 super->param.data_allocation==DATA_ALLOC_FLEX_HORI)){
		if(bio_data_dir(bio)){
			//if(calc_need_num_ssds(super)<=1){
			//	map_context->seq_io = 1;
			//}
		}
	}

	if(map_context->seq_io){
		atomic_inc(&super->wstat.bypass_write_count);
	}

	if(bio_data_dir(bio)){
		void *ptr = kmap_atomic(bio_page(bio));
		map_context->crc32 = crc32(17, ptr, PAGE_SIZE);
		kunmap_atomic(ptr);
	}

	map_context->hot_io = 0;
#if 1
	if(super->param.hot_identification){
		if(!map_context->seq_io){
			unsigned int pageno = calc_cache_alignment(super, bio->bi_sector);
			hot_filter_update(super, bio->bi_sector);
			map_context->hot_io = hot_filter_check(super, pageno);
		}
	}else{
		map_context->hot_io = 1;
	}
#endif

	if(!bio_data_dir(bio)){
		struct metablock *mb = NULL;
		struct segment_header *seg = NULL;
		unsigned long f;
		int ret;

		sector_t key = calc_cache_alignment(super, bio->bi_sector);

		LOCK(super, f);
		mb = ht_lookup(super, key);
		if (!mb) { // no ssd hit 
			if(super->param.enable_read_cache){
				struct cache_manager *clean_cache = super->clean_dram_cache_manager;
				unsigned long lru_flags; 
				struct lru_node *ln = NULL;

				spin_lock_irqsave(&clean_cache->lock, lru_flags);
				ln = CACHE_SEARCH(clean_cache, key);
				if(ln && atomic_read(&ln->sealed) && !atomic_read(&ln->locked)){
					spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
					UNLOCK(super, f);
					bio_endio(bio, 0);
					return DM_MAPIO_SUBMITTED;
				}else if(!ln){
					if(clean_cache->cm_free){
						ln = CACHE_ALLOC(clean_cache, ln, key);
						CACHE_INSERT(clean_cache, ln);
						atomic_set(&ln->sealed, 0);
						atomic_set(&ln->locked, 0);
						spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
						return process_read_miss_request(super, bio, f);
					}
				}
				spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
			}
			// goto pending manager 
		}else{
			seg = get_segment_header_by_mb_idx(super, mb->idx);
			ret = preprocess_pending_bio(super, seg, bio, mb, 0);
			if(ret==0){ //  normal case 
				update_stat(super, mb, 0);
				process_read_hit_request(super, bio, seg, mb, f);
				return DM_MAPIO_SUBMITTED;
			}
			// goto pending manager 
		}
		UNLOCK(super, f);
	}


#ifdef USE_PENDING_WORKER
	//hrtimer_try_to_cancel(&super->sync_mgr.hr_timer);
	pending_bio_add(super, bio);
	queue_work(pending_mgr->wq, &pending_mgr->work);
	//printk(" queue: pending queue ... \n");
#else
	if(!atomic64_read(&pending_mgr->io_count)){
		ret = map_pending_bio(super, bio);
		if(ret<0){
			pending_bio_add(super, bio);
			queue_work(pending_mgr->wq, &pending_mgr->work);
		}
	}else{
		pending_bio_add(super, bio);
		queue_work(pending_mgr->wq, &pending_mgr->work);
	}
#endif 

	return DM_MAPIO_SUBMITTED;
}

#if 0 
static void callback_func(int read_err, unsigned long write_err, void *__context)
{
	atomic_t *count = (atomic_t *)__context;
	atomic_dec(count);

//	printk(" copy complete ... \n");
}
#endif 

void do_background_gc(struct dmsrc_super *super){
	struct migration_manager *migrate_mgr = &super->migrate_mgr;

	migrate_mgr->allow_migrate = 1;
	atomic_set(&migrate_mgr->background_gc_on, 1);
	if(!atomic_read(&migrate_mgr->migrate_triggered)){
		atomic_set(&migrate_mgr->migrate_triggered, 1);
		wake_up_process(migrate_mgr->daemon);
	}
}

void do_grow(struct dmsrc_super *super){

	int i;
	int org_caches = NUM_SSD;
	int org_stripe_sz = STRIPE_SZ;
	struct rambuffer *rambuf[NBUF];
	struct segment_header *segs[NBUF];
	struct segment_header *seg;
	struct group_header *group;
	unsigned long f;
	struct large_array *metablock_array;

	if(!super->dev_info.num_spare_cache_devs){
		printk(" No more drive for growing \n");
		return;
	}
	atomic_set(&super->resize_mode, 1);

	while(atomic_read(&super->cache_stat.inflight_ios)){
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}

	super->migrate_mgr.allow_migrate = 0;

	while(atomic_read(&super->migrate_mgr.mig_inflights)){
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}

	while(atomic_read(&super->flush_mgr.invoke_count)){
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}

	while(atomic_read(&super->migrate_mgr.migrate_triggered))
		schedule_timeout_interruptible(msecs_to_jiffies(1000));

	while(atomic64_read(&super->pending_mgr.io_count))
		schedule_timeout_interruptible(msecs_to_jiffies(1000));

	LOCK(super, f);
	for(i = 0;i < NBUF-1;i++){
		if(super->current_seg[i] && atomic_read(&super->current_seg[i]->length)){
			segs[i] = super->current_seg[i];
			rambuf[i] = super->segbuf_mgr.current_rambuf[i];
		}else{
			segs[i] = NULL;
			rambuf[i] = NULL;
		}
	}
	UNLOCK(super, f);

	for(i = 0;i < NBUF-1;i++){
		if(segs[i]){
			printk(" make flush job seg type = %d, seg id %d length = %d \n", i, (int)segs[i]->seg_id, (int)atomic_read(&segs[i]->length));
			//make_flush_invoke_job(super, segs[i], rambuf[i], i, 1, 1, 1);
		}
	}

	while(atomic_read(&super->flush_mgr.invoke_count)){
		struct flush_invoke_job *job;
		struct segment_header *seg;// = job->seg;
		unsigned long flags;
		spin_lock_irqsave(&super->flush_mgr.lock, flags);
		list_for_each_entry(job,  &super->flush_mgr.queue, list){
			seg = job->seg;
			printk(" make flush job seg type = %d, seg id %d length = %d \n", seg->seg_type, (int)seg->seg_id, (int)atomic_read(&seg->length));
		}
		spin_unlock_irqrestore(&super->flush_mgr.lock, flags);

		printk(" flush queue count = %d \n", (int)atomic_read(&super->flush_mgr.invoke_count));
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}

	printk(" Resizing metadata ... \n");

	for(i = 0;i < NBUF-1;i++){
		if(segs[i]){
			printk(" change status seg type = %d, seg id %d length = %d \n", i, (int)segs[i]->seg_id, (int)atomic_read(&segs[i]->length));
			//make_flush_invoke_job(super, segs[i], rambuf[i], i, 1, 1, 1);
			seg = segs[i];
			change_seg_status(super, seg, atomic_read(&seg->length), 1);
		}
	}

	printk(" metablock alloc \n");
	metablock_array =
		large_array_alloc(sizeof(struct metablock), NUM_BLOCKS_PER_SSD);
	if (!metablock_array) {
		WBERR();
		return;
	}

	LOCK(super, f);

	super->dev_info.cache_dev[NUM_SSD] = super->dev_info.spare_cache_dev[0];
	NUM_SSD++;

	group = super->current_group;
	if(group){
		atomic_set(&group->num_used_ssd,  NUM_SSD);
	}

	for(i = 0;i < super->dev_info.num_spare_cache_devs-1;i++){
		super->dev_info.spare_cache_dev[i] = super->dev_info.spare_cache_dev[i+1];
	}
	super->dev_info.spare_cache_dev[i] = NULL;
	super->dev_info.num_spare_cache_devs--;
	printk(" Num Spare SSDs %d \n", super->dev_info.num_spare_cache_devs);

#if 0 
	super->metablock_array[NUM_SSD-1] =
		large_array_alloc(sizeof(struct metablock), NUM_BLOCKS_PER_SSD);
	if (!super->metablock_array[NUM_SSD-1]) {
		WBERR();
		return;
	}
#else
	super->metablock_array[NUM_SSD-1] = metablock_array;
#endif 

	printk(" init meta block ... \n");
	for (i = 0; i < NUM_BLOCKS; i++) {
		struct metablock *mb = mb_at(super, i);
		int ssdno = get_devno(super, i);

		mb->idx = i;

		if(ssdno == NUM_SSD-1){
			INIT_HLIST_NODE(&mb->ht_list);
			clear_bit(MB_DIRTY, &mb->mb_flags);
			clear_bit(MB_VALID, &mb->mb_flags);
			clear_bit(MB_SEAL, &mb->mb_flags);
			clear_bit(MB_PARITY, &mb->mb_flags);
			clear_bit(MB_BROKEN, &mb->mb_flags);
			clear_bit(MB_SUMMARY, &mb->mb_flags);
			clear_bit(MB_PARITY_NEED, &mb->mb_flags);
			clear_bit(MB_PARITY_WRITTEN, &mb->mb_flags);
			clear_bit(MB_HIT, &mb->mb_flags);
			clear_bit(MB_SKIP, &mb->mb_flags);
			clear_bit(MB_DUMMY, &mb->mb_flags);
			hlist_add_head(&mb->ht_list, &super->null_head->ht_list);
		}
	}

	if(USE_ERASURE_CODE(&super->param))
		raid_conf_init(super, &super->raid_conf);

	for(i = 0;i < NBUF;i++){
		alloc_rambuf_page(super, super->segbuf_mgr.current_rambuf[i], i);
	}

#if 0
	for(i = 0;i < NBUF-1;i++){
		if(segs[i]){
			printk(" alloc new segment ... %d \n", i);
			alloc_new_segment(super, i, false);
		}else{
			printk(" ma init ... %d \n", i);
			if(super->current_seg[i]) {
				printk(" ma reset .. \n");
				ma_reset(super, super->current_seg[i]->seg_id, i);
			}
			printk(" ma init ... %d \n", i);
		}
	}
#else
	printk(" assigne new segment .. \n");
	assign_new_segment(super);
#endif

	UNLOCK(super, f);

	atomic_set(&super->resize_mode, 0);
	printk(" Growing drives is complete\n");
	printk(" No of caches: (%d->%d)\n", org_caches, NUM_SSD);
	printk(" Stripe siz : (%d->%d)\n", org_stripe_sz, STRIPE_SZ);
	print_alloc_queue(super);

}


void do_recovery(struct dmsrc_super *super){
	struct segment_header *seg;
	struct metablock *mb;
	unsigned long flags;
	int failure_ssd = 0;
	int segno;
	int offset;
	int broken_count = 0;
	u8 dirty_bits;
	u8 valid_bits;
	u8 meta_bits;
	unsigned long f;

	LOCK(super, f);
	super->recovery_mgr.failure_ssd = failure_ssd;
	atomic_set(&super->recovery_mgr.broken_block_count, 0);

	for(segno = 0;segno < super->cache_stat.num_segments;segno++){
		seg = get_segment_header_by_id(super, (u64)segno);

		if(!test_bit(SEG_SEALED, &seg->flags))
			continue;

		// mixed segment
		for(offset=0;offset<CHUNK_SZ;offset++){
			u32 idx = failure_ssd * CHUNK_SZ + offset;
			mb = get_mb(super, seg->seg_id, idx);

			lockseg(seg, flags);
			valid_bits = test_bit(MB_VALID, &mb->mb_flags);
			dirty_bits = test_bit(MB_DIRTY, &mb->mb_flags);
			meta_bits = test_bit(MB_PARITY, &mb->mb_flags);
			meta_bits = test_bit(MB_SUMMARY, &mb->mb_flags);
			unlockseg(seg, flags);

			if(seg->seg_type==WCBUF){
				set_bit(SEG_RECOVERY, &seg->flags);
				BUG_ON(!test_bit(SEG_SEALED, &seg->flags));
				BUG_ON(test_bit(MB_BROKEN, &mb->mb_flags));
				set_bit(MB_BROKEN, &mb->mb_flags);
				broken_count++;
				atomic_inc(&super->recovery_mgr.broken_block_count);
			}else{
				if(offset>=CHUNK_SZ-NUM_SUMMARY)
					continue;

				if(test_bit(MB_SUMMARY, &mb->mb_flags))
					continue;

				invalidate_previous_cache(super, seg, mb);
			}
		}
#if 0
		for(j = 0;j < STRIPE_SZ;j++){
			mb = get_mb(super, seg->seg_id, j);

			//devno = get_devno(super, seg, mb);
			devno = j / CHUNK_SZ;

			if(devno!=failure_ssd)
				continue;

			lockseg(seg, flags);
			valid_bits = test_bit(MB_VALID, &mb->mb_flags);
			dirty_bits = test_bit(MB_DIRTY, &mb->mb_flags);
			meta_bits = test_bit(MB_PARITY, &mb->mb_flags);
			meta_bits = test_bit(MB_SUMMARY, &mb->mb_flags);
			unlockseg(seg, flags);

			//if(meta_bits){
			//	printk(" meta bits seg id = %d type = %d \n", (int)seg->seg_id,(int)seg->seg_type);
			//}
			
			if(valid_bits || meta_bits){
				if(dirty_bits || meta_bits){
					//lockseg(seg, flags);
					//seg->need_recovery = 1;
					set_bit(SEG_RECOVERY, &seg->flags);
					BUG_ON(!test_bit(SEG_SEALED, &seg->flags));

					//unlockseg(seg, flags);
					BUG_ON(test_bit(MB_BROKEN, &mb->mb_flags));
					set_bit(MB_BROKEN, &mb->mb_flags);
					broken_count++;
					atomic_inc(&super->recovery_mgr.broken_block_count);
				}else{
					// invalidate broken clean data
					invalidate_previous_cache(super, seg, mb);
				}
			}
		}
#endif 
	}

	UNLOCK(super, f);

#if 1 
	if(broken_count){
		atomic_set(&super->degraded_mode, 1);
		wake_up_process(super->recovery_mgr.daemon);
	}
#endif

	printk(" broken pages = %d \n", broken_count);
}

static int dmsrc_end_io(struct dm_target *ti, struct bio *bio, int error)
{
	struct dmsrc_super *super = ti->private;
	struct segment_header *seg;
	struct bio_ctx *map_context =
		dm_per_bio_data(bio, ti->per_bio_data_size);

	if(map_context->inflight){
		atomic_dec(&super->cache_stat.inflight_ios);
	}	

	if (!map_context->ptr)
		return 0;

	seg = map_context->ptr;
	atomic_dec(&seg->num_read_inflight);
	//printk(" read inflight ios = %d \n", (int)atomic_read(&seg->nr_inflight_ios));

	return 0;
}

static int consume_essential_argv(struct dmsrc_super *super, unsigned int argc, char **argv)
{
	static struct dm_arg _args[] = {
		{0, 0, "invalid buffer type"},
		{1, MAX_CACHE_DEVS, "invalid num caches devs"},
	};

	struct dm_target *ti = super->ti;
	struct dm_arg_set as;
	const char *str;
	unsigned tmp;
	int r = 0, i = 0;

	as.argc = argc;
	as.argv = argv;

	r = dm_read_arg(_args, &as, &tmp, &ti->error);
	if (r)
		return r;

	str = dm_shift_arg(&as);
	strcpy(super->dev_info.origin_name, str);

	r = dm_read_arg(&_args[1], &as, &tmp, &ti->error);
	if (r)
		return r;

	super->dev_info.num_cache_devs = tmp;
	for(i = 0;i < tmp;i++){
		strcpy(super->dev_info.cache_name[i], dm_shift_arg(&as));
	}

	return r;
}

#define consume_kv(name, nr) \
	if (!strcasecmp(key, #name)) { \
		if (!argc) \
			break; \
		r = dm_read_arg(_args + (nr), as, &tmp, &ti->error); \
		if (r) \
			break; \
		super->param.name = tmp; \
		printk(" %s = %u \n", #name, tmp);\
	 }

#define consume_kv_str(name, nr) \
	if (!strcasecmp(key, #name)) { \
		char *str; \
		if (!argc) \
			break; \
		str = dm_shift_arg(as);\
		printk(" %s = %s \n", #name, str);\
	 }

#if 0 
static int consume_optional_argv(struct dmsrc_super *super, struct dm_arg_set *as)
{
	static struct dm_arg _args[] = {
		{0, 14, "invalid optional argc"},
		{4, 12, "invalid chunk_size_order"},
		{512, UINT_MAX, "invalid rambuf_pool_amount"},
		{0, 1, "invalid parity_allocation"},
		{0, 1, "invalid striping_policy"},
		{0, 3, "invalid data_allocation"},
		{ERASURE_CODE_NONE, ERASURE_CODE_RAID6, "invalid type of erasure code"},
		{0, 1, "invalid per_disk_logging"},
	};

	struct dm_target *ti = super->ti;
	int r = 0;
	unsigned argc = 0, tmp;
	if (as->argc) {
		r = dm_read_arg_group(_args, as, &argc, &ti->error);
		if (r)
			return r;
	}

	while (argc) {
		const char *key = dm_shift_arg(as);
		argc--;

		r = -EINVAL;

		consume_kv(chunk_size_order, 1);
		consume_kv(rambuf_pool_amount, 2);
		consume_kv(parity_allocation, 3);
		consume_kv(striping_policy, 4);
		consume_kv(data_allocation, 5);
		consume_kv(erasure_code, 6);
		//consume_kv(per_disk_logging, 7);

		if (!r) {
			argc--;
		} else {
			ti->error = "invalid optional key";
			break;
		}
	}

	return r;
}
#endif

int do_add_spare(struct dmsrc_super *super, struct dm_arg_set *as, unsigned argc){
	struct dm_target *ti = super->ti;
	struct dm_dev *dev;
	char *str; 
	int r = 0;

	if (!argc) 
		return 1;

	str = (char *)dm_shift_arg(as);
	printk(" add_spare = %s [%d] \n",  str, (int)super->dev_info.num_spare_cache_devs);
	r = dm_get_device(ti, str, dm_table_get_mode(ti->table),
			  &super->dev_info.spare_cache_dev[super->dev_info.num_spare_cache_devs]);
	if (r) {
		ti->error = "couldn't get spare dev";
		printk(" couldn't get spare dev %s\n", str);
		return r;
	}

	dev = super->dev_info.spare_cache_dev[super->dev_info.num_spare_cache_devs];
	if(dmsrc_devsize_sectors(dev)<super->dev_info.per_ssd_sectors){
		dm_put_device(ti, dev);
		ti->error = "spare drv is smaller than current drv!";
		printk(" spare drive is smaller than current drive %s\n", str);
		return r;

	}

	super->dev_info.num_spare_cache_devs++;

	return 0;
}

static int do_consume_tunable_argv(struct dm_target *ti,
				   struct dm_arg_set *as, unsigned argc)
{
	static struct dm_arg _args[] = {
		{0, 1, "invalid allow_migrate"},
		{0, 1, "invalid enable_migration_modulator"},
		{1, 1000, "invalid barrier_deadline_ms"},
		{1, 1000, "invalid nr_max_batched_migration"},
		{0, 100, "invalid migrate_threshold"},
		{0, 3600, "invalid update_record_interval"},
		{0, 10000000, "invalid sync_interval"},
		{0, 1, "invalid sequential_enable"},
		{SEQ_CUTOFF_MIN, SEQ_CUTOFF_MAX, "invalid sequential_cutoff size (bytes)"},
		{0, 2, "invalid victim_policy"},
		{0, 2, "invalid reclaim_policy"},
		{5, 99, "invalid u_max"},
		{0, 1, "invalid gc_with_dirtysync"},
		{1, 50, "invalid max_migrate_inflights"},
		{1, 1000, "invalid migrate_lowwater"},
		{2, 1000, "invalid migrate_highwater"},
		{0, 1, "invalid hit_bitmap_type"},
		{0, 1, "invalid bio_plugging"},
		{0, 1, "invalid enable_read_cache"},
		{0, 1, "invalid hot_identification"},
	};

	struct dmsrc_super *super = ti->private;
	int r = 0;
	unsigned tmp;

	while (argc) {
		const char *key = dm_shift_arg(as);
		argc--;

		r = -EINVAL;

		//consume_kv(update_record_interval, 5);
		consume_kv(sync_interval, 6);
		consume_kv(sequential_enable, 7);
		consume_kv(sequential_cutoff, 8);
		consume_kv(victim_policy, 9);
		consume_kv(reclaim_policy, 10);
		consume_kv(u_max, 11);
		consume_kv(gc_with_dirtysync, 12);
		consume_kv(max_migrate_inflights, 13);
		consume_kv(migrate_lowwater, 14);
		consume_kv(migrate_highwater, 15);
		consume_kv(hit_bitmap_type, 16);
		consume_kv(bio_plugging, 17);
		consume_kv(enable_read_cache, 18);
		consume_kv(hot_identification, 19);

		if(super->param.migrate_lowwater>=super->param.migrate_highwater){
			super->param.migrate_lowwater = super->param.migrate_highwater;
			super->param.migrate_highwater++;
		}

		if (!strcasecmp(key, "add_spare")) { 
			r = do_add_spare(super, as, argc);
			if(r)
				return r;
		 }

		if (!strcasecmp(key, "recovery")) { 
			r = 0;
			printk(" do recovery \n");
			do_recovery(super);
		 }

		if (!strcasecmp(key, "grow")) { 
			r = 0;
			printk(" growing # of drives \n");
			do_grow(super);
		 }

		if (!strcasecmp(key, "gc")) { 
			r = 0;
			printk(" background GC \n");
			do_background_gc(super);
		 }

		if (!r) {
			argc--;
		} else {
			ti->error = "invalid optional key";
			break;
		}
	}

	return r;
}

#if 0 
static int consume_tunable_argv(struct dm_target *ti, struct dm_arg_set *as)
{
	static struct dm_arg _args[] = {
		{0, 13, "invalid tunable argc"},
	};

	int r = 0;
	unsigned argc = 0;
	if (as->argc) {
		r = dm_read_arg_group(_args, as, &argc, &ti->error);
		if (r)
			return r;
		/*
		 * tunables are emitted only if
		 * they were origianlly passed.
		 */
	//	super->should_emit_tunables = true;
	}

	return do_consume_tunable_argv(ti, as, argc);
}
#endif

static void
xor_8regs_4_2(unsigned long bytes, unsigned long *p1, unsigned long *p2,
	    unsigned long *p3, unsigned long *p4)
{
	long lines = bytes / (sizeof (long)) / 8;

	do {
		p1[0] ^= p2[0] ^ p3[0] ^ p4[0];
		p1[1] ^= p2[1] ^ p3[1] ^ p4[1];
		p1[2] ^= p2[2] ^ p3[2] ^ p4[2];
		p1[3] ^= p2[3] ^ p3[3] ^ p4[3];
		p1[4] ^= p2[4] ^ p3[4] ^ p4[4];
		p1[5] ^= p2[5] ^ p3[5] ^ p4[5];
		p1[6] ^= p2[6] ^ p3[6] ^ p4[6];
		p1[7] ^= p2[7] ^ p3[7] ^ p4[7];

		p1 += 8;
		p2 += 8;
		p3 += 8;
		p4 += 8;
	} while (--lines > 0);
}

void
xor_blocks2(unsigned int src_count, unsigned int bytes, void *dest, void **srcs)
{
	unsigned long *p1, *p2, *p3;
	p1 = (unsigned long *) srcs[0];
	p2 = (unsigned long *) srcs[1];
	p3 = (unsigned long *) srcs[2];
	xor_8regs_4_2(bytes, dest, p1, p2, p3);
}

void raid6_dual_recov(int disks, size_t bytes, int faila, int failb, void **ptrs)
{
	if ( faila > failb ) {
		int tmp = faila;
		faila = failb;
		failb = tmp;
	}

	if ( failb == disks-1 ) {
		if ( faila == disks-2 ) {
			/* P+Q failure.  Just rebuild the syndrome. */
			raid6_call.gen_syndrome(disks, bytes, ptrs);
		} else {
			/* data+Q failure.  Reconstruct data from P,
			   then rebuild syndrome. */
			/* NOT IMPLEMENTED - equivalent to RAID-5 */
		}
	} else {
		if ( failb == disks-2 ) {
			/* data+P failure. */
			raid6_datap_recov(disks, bytes, faila, ptrs);
		} else {
			/* data+data failure. */
			raid6_2data_recov(disks, bytes, faila, failb, ptrs);
		}
	}
}

void pq_test(struct dmsrc_super *super){
#define NUM_DISKS 5
	void *src[NUM_DISKS];
	void *dst;
	int i;
	int *intp;

	printk(" PQ test ... \n");

	for(i = 0;i < NUM_DISKS;i++){
		src[i] = (void *) get_zeroed_page(GFP_KERNEL | __GFP_NOTRACK);
		intp = (int *)src[i];
		*intp = i*80 + 20;
	}

	for(i = 0;i < NUM_DISKS-2;i++){
		intp = (int *)src[i];
		printk(" %d: %d \n", i, *intp);
	}

	dst = src[NUM_DISKS-1];
	memset(dst, 0xee, SRC_PAGE_SIZE);

	dst = src[NUM_DISKS-2];
	memset(dst, 0xee, SRC_PAGE_SIZE);

	raid6_call.gen_syndrome(NUM_DISKS, SRC_PAGE_SIZE, src);

	intp = (int *)src[0];
	*intp = 0;
	intp = (int *)src[1];
	*intp = 0;

	for(i = 0;i < NUM_DISKS-2;i++){
		intp = (int *)src[i];
		printk(" %d: %d \n", i, *intp);
	}
	
	raid6_dual_recov(NUM_DISKS, SRC_PAGE_SIZE, 0, 1, (void **) &src);

	for(i = 0;i < NUM_DISKS-2;i++){
		intp = (int *)src[i];
		printk(" %d: %d \n", i, *intp);
	}

	for(i = 0;i < NUM_DISKS;i++)
		free_page((unsigned long)src[i]);

}

void xor_test(struct dmsrc_super *super){
#define SRC_LEN 3
	void *src[SRC_LEN+1];
	void *src2[SRC_LEN+1];
	void *dst;
	void *temp;
	int i;
	int *intp;


	for(i = 0;i < SRC_LEN+1;i++){
		src[i] = (void *) get_zeroed_page(GFP_KERNEL | __GFP_NOTRACK);
		intp = (int *)src[i];
		*intp = i*80 + 20;
	}

	dst = src[SRC_LEN];
	intp = (int *)dst;
	*intp = 0;
	memset(dst, 0xFF, SRC_PAGE_SIZE);

	temp = (void *) get_zeroed_page(GFP_KERNEL | __GFP_NOTRACK);
	intp = (int *)temp;
	*intp = 0;
	memset(temp, 0xFF, SRC_PAGE_SIZE);


	xor_blocks(SRC_LEN, SRC_PAGE_SIZE, dst, src);
	xor_blocks(SRC_LEN, SRC_PAGE_SIZE, temp, src);

	for(i = 0;i < SRC_LEN+1;i++){
		intp = (int *)src[i];
		printk(" %d data %u %d \n", i, 
				crc32_le(CRC_SEED, src[i], SRC_PAGE_SIZE), *intp);
	}

	intp = (int *)dst;
	printk(" dst data %u %d \n",  
				crc32_le(CRC_SEED, dst, SRC_PAGE_SIZE), *intp);
	intp = (int *)temp;
	printk(" temp data %u %d \n",  
				crc32_le(CRC_SEED, temp, SRC_PAGE_SIZE), *intp);

	for(i = 0;i < SRC_LEN;i++){
		src2[i] = src[i+1];
	}

	memset(temp, 0xFF, SRC_PAGE_SIZE);
	xor_blocks(SRC_LEN, SRC_PAGE_SIZE, temp, src2);

	intp = (int *)temp;
	printk(" recovered data %u %d \n",  
				crc32_le(CRC_SEED, temp, SRC_PAGE_SIZE), *intp);

	if(memcmp(temp, src[0], SRC_PAGE_SIZE)){
		printk(" *** parity mismatch ******\n");
	}

	for(i = 0;i < SRC_LEN+1;i++)
		free_page((unsigned long)src[i]);

	free_page((unsigned long)temp);
}

static void set_default_param(struct dmsrc_super *super, struct dmsrc_param *param){

	param->enable_read_cache = 1;

	param->erasure_code = ERASURE_CODE_NONE;
	//super->erasure_code = ERASURE_CODE_PARITY;
	//super->erasure_code = ERASURE_CODE_RAID6;
	param->parity_allocation = PARITY_ALLOC_FIXED;
	//super->parity_allocation = PARITY_ALLOC_ROTAT;

	//param->data_allocation = DATA_ALLOC_VERT;
	param->data_allocation = DATA_ALLOC_HORI;
	param->aligned_io_dummy = ALIGNED_IO_DUMMY;
	//param->aligned_io_dummy = ALIGNED_IO_SKIP;
	//super->data_allocation = DATA_ALLOC_FLEX_VERT;
	//super->data_allocation = DATA_ALLOC_FLEX_HORI;

	param->striping_policy = SEPAR_STRIPING;
	//super->striping_policy = MIXED_STRIPING;
	//super->hit_bitmap_type = HIT_BITMAP_PER_BLOCK;
	param->hit_bitmap_type = HIT_BITMAP_PER_STRIPE;

	param->gc_with_dirtysync = GC_WITHOUT_DIRTY_SYNC;
	param->sequential_enable = 1;
	param->u_max = DEFAULT_U_MAX;
	//param->chunk_size_order = 5;
	param->chunk_size = 2048;
	param->rambuf_pool_amount = 2048;
	param->reclaim_policy = RECLAIM_DESTAGE;
	param->bio_plugging = 0;
	param->hot_identification = 1;
	param->sequential_cutoff= SEQ_CUTOFF;

	param->max_migrate_inflights = MAX_MIGRATE_INFLIGHT_NUM;
	//param->migrate_lowwater = DEFAULT_MIGRATE_LOWWATER;
	//param->migrate_highwater = DEFAULT_MIGRATE_HIGHWATER;

	super->param.checker_interval = 20;
	//printk(" Origin num sectors = %lu\n", super->origin_num_sectors);
	//param->origin_num_sectors = param->origin_num_sectors / tmp * tmp;
	//printk(" Origin num sectors = %lu\n", super->origin_num_sectors);

}

void raid_conf_init(struct dmsrc_super *super, struct r5conf *conf){

#if 0
	int i;
	int d_disk, p_disk;
#endif

	memset(conf, 0x00, sizeof(struct r5conf));

	conf->algorithm = ALGORITHM_LEFT_SYMMETRIC;
	conf->chunk_sectors = CHUNK_SZ_SECTOR;

	if(USE_ERASURE_PARITY(&super->param)){
		if(super->param.parity_allocation==PARITY_ALLOC_FIXED){
			conf->level = 4;
			conf->raid_disks =  NUM_SSD-1;
		}else{
			conf->level = 5;
			conf->raid_disks =  NUM_SSD;
		}
	}

	if(USE_ERASURE_RAID6(&super->param)){
		conf->level = 6;
		conf->raid_disks =  NUM_SSD;
	}
#if 0 
#if 0  // RAID5
	for(i = 0;i < 200;i++){
		raid5_calc_sector(conf, i * 8 * (STRIPE_SZ-CHUNK_SZ), 0,&d_disk, &p_disk, NULL);
		printk(" %d ddisk = %d, pdisk = %d \n", i, d_disk, p_disk);
	}
#else
	for(i = 0;i < 200;i++){
		raid5_calc_sector(conf, i * 8 * STRIPE_SZ, 0,&d_disk, &p_disk, NULL);
		printk(" %d ddisk = %d, pdisk = %d \n", i, d_disk, p_disk);

	}
#endif
#endif
}

void reset_arrival_time(struct dmsrc_super *super){
	struct pending_manager *pending_mgr = &super->pending_mgr;
	int i;

	for(i = 0;i < MAX_ARRIVAL_TIME;i++){
		pending_mgr->arrival_times[i].tv_sec = 0;
		pending_mgr->arrival_times[i].tv_nsec = 0;
	}

	atomic_set(&pending_mgr->arrival_start, 0);
	atomic_set(&pending_mgr->arrival_count, 0);
	atomic_set(&pending_mgr->arrival_cur, 0);
}

static int pending_mgr_init(struct dmsrc_super *super){
	struct pending_manager *pending_mgr = &super->pending_mgr;
	int r = 0;

	pending_mgr->super = super;
	spin_lock_init(&pending_mgr->barrier_lock);
	bio_list_init(&pending_mgr->barrier_ios);
	atomic_set(&pending_mgr->barrier_count, 0);

	spin_lock_init(&pending_mgr->lock);
	atomic64_set(&pending_mgr->io_count, 0);
	bio_list_init(&pending_mgr->bios);

	reset_arrival_time(super);

	//pending_mgr->wq = alloc_workqueue("pending_wq",
	//			     WQ_NON_REENTRANT | WQ_MEM_RECLAIM, 1);
	pending_mgr->wq = create_singlethread_workqueue("pending_wq");
	if (!pending_mgr->wq) {
		WBERR("failed to alloc read caching wq");
		r = -1;
		goto bad_init;
	}

	INIT_WORK(&pending_mgr->work, pending_worker);

	pending_mgr->wb_job_pool = mempool_create_kmalloc_pool(STRIPE_SZ,
							    sizeof(struct wb_job));
	if (!pending_mgr->wb_job_pool) {
		r = -ENOMEM;
		WBERR("couldn't alloc flush job pool");
		goto bad_init;
	}

	pending_mgr->initialized = 1;
bad_init:
	return r;
}

static void pending_mgr_exit(struct dmsrc_super *super){
	if(!super->pending_mgr.initialized)
		return;

	destroy_workqueue(super->pending_mgr.wq);
	mempool_destroy(super->pending_mgr.wb_job_pool);
}

static void seq_detector_init(struct dmsrc_super *super){
	struct seq_io_detector *seq_detector = &super->seq_detector;
	struct io *io;

	INIT_LIST_HEAD(&seq_detector->io_lru);

	spin_lock_init(&seq_detector->seq_lock);
	for (io = seq_detector->io; io < seq_detector->io + RECENT_IO; io++) {
		list_add(&io->lru, &seq_detector->io_lru);
		hlist_add_head(&io->hash, seq_detector->io_hash + RECENT_IO);
	}
}

static void stat_init(struct dmsrc_super *super){
	super->wstat.count=0;
	super->wstat.hit=0;
	super->wstat.read_count=0;
	super->wstat.read_hit=0;
	super->wstat.write_count=0;
	super->wstat.write_hit=0;
	atomic_set(&super->wstat.destage_count, 0);
	atomic_set(&super->wstat.gc_count, 0);
	atomic_set(&super->wstat.total_migration, 0);
	atomic_set(&super->wstat.cold_bypass_count, 0);
	atomic_set(&super->wstat.seq_bypass_count, 0);

	atomic64_set(&super->wstat.average_arrival_time, 0);
	atomic64_set(&super->wstat.average_arrival_count, 0);
}

void seg_allocator_init(struct dmsrc_super *super){
	struct segment_allocator *seg_allocator = &super->seg_allocator;

	init_waitqueue_head(&seg_allocator->alloc_wait_queue);

	INIT_LIST_HEAD(&seg_allocator->group_alloc_queue);
	INIT_LIST_HEAD(&seg_allocator->group_migrate_queue);
	INIT_LIST_HEAD(&seg_allocator->group_used_queue);
	INIT_LIST_HEAD(&seg_allocator->group_sealed_queue);

	atomic_set(&seg_allocator->group_alloc_count, 0); 
	atomic_set(&seg_allocator->group_migrate_count, 0); 
	seg_allocator->group_used_count = 0;
	seg_allocator->group_sealed_count = 0;

	INIT_LIST_HEAD(&seg_allocator->seg_alloc_queue);
	INIT_LIST_HEAD(&seg_allocator->seg_migrate_queue);
	INIT_LIST_HEAD(&seg_allocator->seg_used_queue);
	INIT_LIST_HEAD(&seg_allocator->seg_sealed_queue);

	atomic_set(&seg_allocator->seg_alloc_count, 0); 
	atomic_set(&seg_allocator->seg_migrate_count, 0); 
	seg_allocator->seg_used_count = 0;
	seg_allocator->seg_sealed_count = 0;
}

int degraded_mgr_init(struct dmsrc_super *super){
	struct degraded_manager *degraded_mgr = &super->degraded_mgr;
	int r = 0;

	degraded_mgr->buf_pool = mempool_create_kmalloc_pool(16, 
			NUM_SSD * (1 << PAGE_SHIFT));
	if (!degraded_mgr->buf_pool) {
		r = -ENOMEM;
		WBERR("couldn't alloc 8 sector pool");
		goto bad_init;;
	}

	degraded_mgr->job_pool = mempool_create_kmalloc_pool(STRIPE_SZ,
							    sizeof(struct degraded_job));
	if (!degraded_mgr->job_pool) {
		r = -ENOMEM;
		WBERR("couldn't alloc flush job pool");
		goto bad_init;;
	}

	spin_lock_init(&degraded_mgr->lock);
	INIT_LIST_HEAD(&degraded_mgr->queue);
	degraded_mgr->wq = alloc_workqueue("degraded_wq",
				     WQ_NON_REENTRANT | WQ_MEM_RECLAIM, 1);
	if (!degraded_mgr->wq) {
		WBERR("failed to alloc recovery wq");
		r = -1;
		goto bad_init;;
	}

	INIT_WORK(&degraded_mgr->work, do_degraded_worker);
	
	degraded_mgr->initialized = 1;

	return r;

bad_init:

	return r;
}

void degraded_mgr_deinit(struct dmsrc_super *super){
	struct degraded_manager *degraded_mgr = &super->degraded_mgr;

	if(!degraded_mgr->initialized)
		return;

	flush_work(&degraded_mgr->work);
	cancel_work_sync(&degraded_mgr->work);
	mempool_destroy(degraded_mgr->buf_pool);
	mempool_destroy(degraded_mgr->job_pool);
}

int recovery_mgr_init(struct dmsrc_super *super){
	int r = 0;
	struct recovery_manager *recovery_mgr = &super->recovery_mgr;

	recovery_mgr->super = super;

	spin_lock_init(&recovery_mgr->lock);
	INIT_LIST_HEAD(&recovery_mgr->queue);
	recovery_mgr->wq = alloc_workqueue("recovery_wq",
				     WQ_NON_REENTRANT | WQ_MEM_RECLAIM, 1);
	if (!recovery_mgr->wq) {
		WBERR("failed to alloc recovery wq");
		r = -1;
		goto error_finish;
	}
	INIT_WORK(&recovery_mgr->work, do_recovery_worker);

	recovery_mgr->start_jiffies = jiffies;
	recovery_mgr->end_jiffies = jiffies;

	recovery_mgr->job_pool = mempool_create_kmalloc_pool(STRIPE_SZ,
							    sizeof(struct recovery_job));
	if (!recovery_mgr->job_pool) {
		r = -ENOMEM;
		WBERR("couldn't alloc flush job pool");
		goto error_finish;
	}

	r = create_daemon(super, &recovery_mgr->daemon, recovery_proc, "recovery_daemon");
	if(r) 
		goto error_finish;

	recovery_mgr->initialized = 1;

error_finish:

	return r;
}

void recovery_mgr_deinit(struct dmsrc_super *super){
	struct recovery_manager *recovery_mgr = &super->recovery_mgr;

	if(!recovery_mgr->initialized)
		return;

	while( atomic_read(&super->recovery_mgr.broken_block_count)){
	//	printk(" Wating recovery proc... broken block = %d\n", 
	//			(int)atomic_read(&super->recovery_mgr.broken_block_count));
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}

	flush_work(&recovery_mgr->work);
	cancel_work_sync(&recovery_mgr->work);
	destroy_workqueue(recovery_mgr->wq);
	kthread_stop(recovery_mgr->daemon);
	mempool_destroy(recovery_mgr->job_pool);
}

int flush_mgr_init(struct dmsrc_super *super){
	struct flush_manager *flush_mgr = &super->flush_mgr;
	int r = 0;

	spin_lock_init(&flush_mgr->lock);
	atomic_set(&flush_mgr->invoke_count, 0);
	atomic_set(&flush_mgr->io_count, 0);
	atomic64_set(&flush_mgr->global_seg_sequence, 0);
	INIT_LIST_HEAD(&flush_mgr->queue);

	r = create_daemon(super, &flush_mgr->daemon, flush_meta_proc, "flush_meta_daemon"); 
	if(r)
		goto bad_init;

	flush_mgr->invoke_pool = mempool_create_kmalloc_pool(100,
							    sizeof(struct flush_invoke_job));
	if (!flush_mgr->invoke_pool) {
		r = -ENOMEM;
		WBERR("couldn't alloc flush job pool");
		goto bad_init;
	}

	//flush_mgr->io_pool = mempool_create_kmalloc_pool(STRIPE_SZ,
	//						    sizeof(struct summary_io_job));
	//if (!flush_mgr->io_pool) {
	//	r = -ENOMEM;
	//	WBERR("couldn't alloc contex job pool");
	//	goto bad_init;
	//}

	flush_mgr->initialized = 1;

	return r;

bad_init:
	return r;

}

void flush_mgr_deinit(struct dmsrc_super *super){
	if(!super->flush_mgr.initialized)
		return;

	printk(" flush partial meta .. \n");
	flush_partial_meta(super, WHBUF);
	printk(" flush partial meta .. \n");
	flush_partial_meta(super, WCBUF);
	printk(" end flush partial meta .. \n");

	while(atomic_read(&super->flush_mgr.invoke_count)){
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}

	while(atomic_read(&super->flush_mgr.io_count)){
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}

	kthread_stop(super->flush_mgr.daemon);
	mempool_destroy(super->flush_mgr.invoke_pool);
	//mempool_destroy(super->flush_mgr.io_pool);
}

int migrate_mgr_init(struct dmsrc_super *super){
	struct migration_manager *migrate_mgr = &super->migrate_mgr;
	int r;

	migrate_mgr->super = super;
	migrate_mgr->allow_migrate = false;

	spin_lock_init(&migrate_mgr->mig_queue_lock);
	spin_lock_init(&migrate_mgr->group_queue_lock);

	INIT_LIST_HEAD(&migrate_mgr->migrate_queue);
	INIT_LIST_HEAD(&migrate_mgr->mig_queue);
	INIT_LIST_HEAD(&migrate_mgr->copy_queue);
	INIT_LIST_HEAD(&migrate_mgr->group_queue);

	create_daemon(super, &migrate_mgr->daemon, migrate_proc, "migrate_daemon"); 

	migrate_mgr->mig_wq = alloc_workqueue("mig_wq",
				     WQ_NON_REENTRANT | WQ_MEM_RECLAIM, 1);
	if (!migrate_mgr->mig_wq) {
		WBERR("failed to alloc mig wq");
		r = -1;
		goto bad_wq;
	}
	INIT_WORK(&migrate_mgr->mig_work, do_mig_worker);

	atomic_set(&migrate_mgr->migrate_queue_count, 0);
	atomic_set(&migrate_mgr->mig_inflights, 0);
	//atomic_set(&migrate_mgr->mig_cur_kcopyd, 0);
	atomic_set(&migrate_mgr->migrate_triggered, 0);
	atomic_set(&migrate_mgr->background_gc_on, 0);

	migrate_mgr->gc_cur_seg = NULL;
	migrate_mgr->gc_cur_offset = 0;
	migrate_mgr->gc_alloc_count= 0;

	migrate_mgr->copy_job_pool = mempool_create_kmalloc_pool(100,
							    sizeof(struct copy_job));
	if (!migrate_mgr->copy_job_pool) {
		r = -ENOMEM;
		WBERR("couldn't alloc copy job pool");
		goto bad_copy_job_pool;
	}

	migrate_mgr->group_job_pool = mempool_create_kmalloc_pool(1024,
							    sizeof(struct copy_job_group));
	if (!migrate_mgr->group_job_pool) {
		r = -ENOMEM;
		WBERR("couldn't alloc copy job pool");
		goto bad_group_job_pool;
	}

	//migrate_mgr->mig_job_pool = mempool_create_kmalloc_pool(100,
	//						    sizeof(struct mig_job));
	//if (!migrate_mgr->mig_job_pool) {
	//	r = -ENOMEM;
	//	WBERR("couldn't alloc mig job pool");
	//	goto bad_mig_job_pool;
	//}

	atomic_set(&migrate_mgr->copy_job_count, 0);
	atomic_set(&migrate_mgr->group_job_count, 0);
	atomic_set(&migrate_mgr->group_job_seq, 0);

	super->param.migrate_lowwater = atomic_read(&super->segbuf_mgr.total_page_count)/STRIPE_SZ;
	if(super->param.migrate_lowwater<2)
		super->param.migrate_lowwater = 2;
	super->param.migrate_highwater = atomic_read(&super->segbuf_mgr.total_page_count)/STRIPE_SZ/4*6;

	migrate_mgr->initialized = 1;

	return 0;

//bad_mig_job_pool:
//	mempool_destroy(migrate_mgr->group_job_pool);
bad_group_job_pool:
	mempool_destroy(migrate_mgr->copy_job_pool);
bad_copy_job_pool:
	destroy_workqueue(migrate_mgr->mig_wq);
bad_wq:

	return -1;
}

void migrate_mgr_deinit(struct dmsrc_super *super){
	struct migration_manager *migrate_mgr = &super->migrate_mgr;

	if(!migrate_mgr->initialized)
		return;

	wake_up_process(migrate_mgr->daemon);

	printk(" wait for triggered off\n");
	while(atomic_read(&migrate_mgr->migrate_triggered))
		schedule_timeout_interruptible(usecs_to_jiffies(1000));

	printk(" wait for inflights \n");
	//while(atomic_read(&migrate_mgr->mig_inflights))
		//schedule_timeout_interruptible(usecs_to_jiffies(1000));

	migrate_mgr->allow_migrate = false;
	atomic_set(&migrate_mgr->migrate_triggered, 0);
	atomic_set(&super->migrate_mgr.background_gc_on, 0);

	printk(" wait for inflights \n");
//	while(atomic_read(&migrate_mgr->mig_inflights))
//		schedule_timeout_interruptible(usecs_to_jiffies(1000));

	kthread_stop(migrate_mgr->daemon);
	flush_work(&migrate_mgr->mig_work);
	cancel_work_sync(&migrate_mgr->mig_work);
	mempool_destroy(migrate_mgr->copy_job_pool);
	//mempool_destroy(migrate_mgr->mig_job_pool);

	migrate_mgr->initialized = 0;
}

int plugger_init(struct dmsrc_super *super){
	struct plugging_manager *plugger = &super->plugger;
	int i;
	int r = 0;

	plugger->super = super;
	plugger->deadline_us = 1000000;

	setup_timer(&plugger->timer,
		    plug_deadline_proc, (unsigned long) super);

	for(i = 0;i < MAX_CACHE_DEVS;i++){
		INIT_LIST_HEAD(&plugger->queue[i]);
		atomic_set(&plugger->queue_length[i], 0);
		spin_lock_init(&plugger->lock[i]);
	}
	atomic_set(&plugger->total_length, 0);

#if 0
	plugger->mem_pool = mempool_create_kmalloc_pool(STRIPE_SZ*16,
							    sizeof(struct plug_job));
	if(!plugger->mem_pool){
		goto bad_init;
	}
#endif

	//INIT_WORK(&plugger->work, plug_proc);

	//update_plug_deadline(super);

	plugger->initialized = 1;

	return r;

//bad_init:
//	return r;
}

void plugger_deinit(struct dmsrc_super *super){
	struct plugging_manager *plugger = &super->plugger;
	//int i;

	if(!plugger->initialized)
		return;

	//for(i = 0;i < NUM_SSD;i++)
	//	flush_plug_proc(super, NULL, 0, 1, i);

	del_timer(&plugger->timer);
	//flush_work(&plugger->work);
	//cancel_work_sync(&plugger->work);

	//mempool_destroy(plugger->mem_pool);
}

#if (USE_SEG_WRITER == 1)
int seg_write_mgr_init(struct dmsrc_super *super){
	struct seg_write_manager *seg_write_mgr; // = &super->seg_write_mgr;
	int r = 0;
	int i = 0;

	for(i = 0;i < MAX_CACHE_DEVS;i++){
		seg_write_mgr = &super->seg_write_mgr[i];
		seg_write_mgr->super = super;

		atomic_set(&seg_write_mgr->count, 0);
		spin_lock_init(&seg_write_mgr->spinlock);
		INIT_LIST_HEAD(&seg_write_mgr->head);

		seg_write_mgr->wq= alloc_workqueue("reacaching_wq",
						 WQ_NON_REENTRANT | WQ_MEM_RECLAIM, 1);
		if (!seg_write_mgr->wq) {
			WBERR("failed to alloc read caching wq");
			r = -1;
			goto bad_init;
		}
		INIT_WORK(&seg_write_mgr->work, seg_write_worker);

		seg_write_mgr->initialized = 1;

	}
	return r;
	
bad_init:
	return r;
}

void seg_write_mgr_deinit(struct dmsrc_super *super){
	struct seg_write_manager *seg_write_mgr;// = &super->seg_write_mgr;
	int i;

	for(i = 0;i < MAX_CACHE_DEVS;i++){
		seg_write_mgr = &super->seg_write_mgr[i];
		if(!seg_write_mgr->initialized)
			return;

		destroy_workqueue(seg_write_mgr->wq);
	}
}
#endif 

int read_miss_mgr_init(struct dmsrc_super *super){
	struct read_miss_manager *read_miss_mgr = &super->read_miss_mgr;
	int r = 0;

	read_miss_mgr->super = super;

	init_read_caching_job_list(super);
	read_miss_mgr->wq= alloc_workqueue("reacaching_wq",
				     WQ_NON_REENTRANT | WQ_MEM_RECLAIM, 1);
	if (!read_miss_mgr->wq) {
		WBERR("failed to alloc read caching wq");
		r = -1;
		goto bad_init;
	}
	INIT_WORK(&read_miss_mgr->work, do_read_caching_worker);

	read_miss_mgr->job_pool = mempool_create_kmalloc_pool(STRIPE_SZ,
							    sizeof(struct read_caching_job));
	if (!read_miss_mgr->job_pool) {
		r = -ENOMEM;
		WBERR("couldn't alloc flush job pool");
		goto bad_init;
	}

	read_miss_mgr->initialized = 1;
	return r;
	
bad_init:
	return r;
}

void read_miss_mgr_deinit(struct dmsrc_super *super){
	struct read_miss_manager *read_miss_mgr = &super->read_miss_mgr;

	if(!read_miss_mgr->initialized)
		return;

	//flush_work(&read_miss_mgr->work);
	//cancel_work_sync(&read_miss_mgr->work);
	destroy_workqueue(read_miss_mgr->wq);

	mempool_destroy(read_miss_mgr->job_pool);
}

enum hrtimer_restart sync_timer_callback( struct hrtimer *timer )
{
	struct sync_manager *sync_mgr = container_of(timer, struct sync_manager, hr_timer);
	struct dmsrc_super *super = sync_mgr->super;

	//printk(" hrtimer: timeout .... pending = %d \n", (int)atomic64_read(&super->pending_mgr.io_count));
#if 0
	if(!atomic_read(&super->migrate_mgr.migrate_triggered)){
		if(!atomic64_read(&super->pending_mgr.io_count)){
			flush_partial_meta(super, WHBUF);
			flush_partial_meta(super, WCBUF);
		}
	}
#else
	 schedule_work(&super->sync_mgr.work);
#endif 
	
	return HRTIMER_NORESTART;
}

int sync_mgr_init(struct dmsrc_super *super){
	struct sync_manager *sync_mgr = &super->sync_mgr;

	spin_lock_init(&sync_mgr->lock);
	sync_mgr->super = super;
	super->param.sync_interval = DEFAULT_ARRIVAL_US;

	hrtimer_init( &sync_mgr->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	sync_mgr->hr_timer.function = &sync_timer_callback;

	//setup_timer(&sync_mgr->timer,
	//	    schedule_sync_proc, (unsigned long) super);
	INIT_WORK(&sync_mgr->work, sync_proc);

	sync_mgr->initialized = 1;

	return 0;
}

void sync_mgr_deinit(struct dmsrc_super *super){
	struct sync_manager *sync_mgr = &super->sync_mgr;
	
	if(!sync_mgr->initialized)
		return;

	//del_timer(&sync_mgr->timer);
	flush_work(&sync_mgr->work);
	cancel_work_sync(&sync_mgr->work);
}

unsigned long get_avg_arrival_time(struct dmsrc_super *super){
	struct pending_manager *pending_mgr = &super->pending_mgr;
	unsigned long f;
	int count;
	int i;
	int start;
	int next;
	//struct timeval *start_tv, *next_tv, diff_tv;
	struct timespec *start_ts, *next_ts, diff_ts;
	unsigned long ns = 0;
	unsigned long avg_us = DEFAULT_ARRIVAL_US;

	spin_lock_irqsave(&pending_mgr->lock, f);
	count = atomic_read(&pending_mgr->arrival_count);
	if(count<=1){
//		avg_us = DEFAULT_ARRIVAL_US;
		avg_us = super->param.sync_interval;
		goto RESET;
	}

	avg_us = super->param.sync_interval;
	goto RESET;

	start = atomic_read(&pending_mgr->arrival_start);

	for(i = 0;i < count-1;i++){
		next = (start + 1) % STRIPE_SZ;
		start_ts = &pending_mgr->arrival_times[start];
		next_ts  = &pending_mgr->arrival_times[next];

		diff_ts.tv_sec = next_ts->tv_sec - start_ts->tv_sec;
		diff_ts.tv_nsec = next_ts->tv_nsec - start_ts->tv_nsec;	
		ns += diff_ts.tv_sec * NSEC_PER_SEC + diff_ts.tv_nsec;

		start = (start + 1)% STRIPE_SZ;
	}

	avg_us = ns/(count-1)/1000;
RESET:
	reset_arrival_time(super);
	spin_unlock_irqrestore(&pending_mgr->lock, f);

	atomic64_add(avg_us, &super->wstat.average_arrival_time);
	atomic64_inc( &super->wstat.average_arrival_count);
	//printk(" avarage arrival time = %dus, count = %d \n", (int)avg_us, count);

	return avg_us;
}

void update_sync_deadline(struct dmsrc_super *super)
{
	struct sync_manager *sync_mgr = &super->sync_mgr;
	unsigned long usec;


	usec = get_avg_arrival_time(super);
	//if(usec>10000)
		//printk(" queue timer: usec = %d (%d sec)\n", (int)usec, (int)(usec/USEC_PER_SEC));
	
	//printk(" update sync ... timer ... \n");
	sync_mgr->period = ktime_set(0, usec*1000);
	hrtimer_start( &sync_mgr->hr_timer, sync_mgr->period, HRTIMER_MODE_REL );
}

void wp_print_iops(struct dmsrc_super *super){
	struct workload_predictor *wp = &super->wp;
	int i;

	for(i = 0;i < wp->num_window;i++){
		struct io_stat_t *iostat = &wp->iostat[i];
		//if(i==atomic_read(&wp->cur_window))
			printk(" [*%d:%d:%dMB/s]\n", i, (int)atomic64_read(&iostat->iops[REQ_CATEGORY_TOTAL][REQ_TYPE_TOTAL]),
					(int)atomic64_read(&iostat->iops[REQ_CATEGORY_TOTAL][REQ_TYPE_TOTAL])/256);
		//else
		//	printk(" [%d:%d:%dMB/s]", i, (int)atomic64_read(&iostat->iops[REQ_CATEGORY_TOTAL][REQ_TYPE_TOTAL]), 
		//			(int)atomic64_read(&iostat->iops[REQ_CATEGORY_TOTAL][REQ_TYPE_TOTAL])/256);
	}
	printk("\n");
		
}

u32 wp_get_iops(struct dmsrc_super *super, u32 *bw_mb, int category, int type){
	struct workload_predictor *wp = &super->wp;
	u32 start_window;
	u32 cur_window;
	u32 cur_iops;
	struct io_stat_t *iostat; 
	unsigned long flags;
	int i;

	cur_iops = 0;

	spin_lock_irqsave(&wp->lock, flags);

	cur_window = atomic_read(&wp->cur_window);
	start_window = atomic_read(&wp->start_window);

	for(i = 0;i < wp->num_window;i++){
		if(start_window==cur_window)
			break;
		iostat = &wp->iostat[start_window];
		cur_iops += (u32)atomic64_read(&iostat->iops[category][type]);
		start_window = (start_window + 1) % wp->num_window;
	}

	spin_unlock_irqrestore(&wp->lock, flags);
	cur_iops /= (wp->num_window/wp->window_per_sec);

	if(bw_mb)
		*bw_mb = cur_iops/256;

	return cur_iops;
}

enum hrtimer_restart wp_display( struct hrtimer *timer )
{
	struct workload_predictor *wp = container_of(timer, struct workload_predictor, hr_timer_meter);

	if(wp->super->param.data_allocation==DATA_ALLOC_FLEX_HORI){
		printk(" CurBW: Total=%d, Write=%d, GCWrite=%d GCRead=%d MB/s\n", 
		wp_get_iops(wp->super, NULL, REQ_CATEGORY_TOTAL, REQ_TYPE_WRITE)/256,
		wp_get_iops(wp->super, NULL, REQ_CATEGORY_NORMAL, REQ_TYPE_WRITE)/256,
		wp_get_iops(wp->super, NULL, REQ_CATEGORY_GC, REQ_TYPE_WRITE)/256,
		wp_get_iops(wp->super, NULL, REQ_CATEGORY_GC, REQ_TYPE_READ)/256);
	}
	
	hrtimer_forward_now(timer, wp->display_period);

	return HRTIMER_RESTART;
}

enum hrtimer_restart wp_callback( struct hrtimer *timer )
{
	struct workload_predictor *wp = container_of(timer, struct workload_predictor, hr_timer_track);
	struct io_stat_t *iostat; 
	int i, j;
	unsigned long flags;

	//wp_print_iops(wp->super);

	spin_lock_irqsave(&wp->lock, flags);
	atomic_set(&wp->cur_window, ((atomic_read(&wp->cur_window) + 1) % wp->num_window));
	if(atomic_read(&wp->cur_window)==atomic_read(&wp->start_window)){
		atomic_set(&wp->start_window, ((atomic_read(&wp->start_window) + 1) % wp->num_window));
	}

	iostat = &wp->iostat[atomic_read(&wp->cur_window)];
	for(i = 0;i < REQ_CATEGORY_NUM;i++){
		for(j = 0;j <  REQ_TYPE_NUM;j++){
			atomic64_set(&iostat->iops[i][j], 0);
		}
	}
	spin_unlock_irqrestore(&wp->lock, flags);
	
	hrtimer_forward_now(timer, wp->track_period);

	return HRTIMER_RESTART;
}

void wp_update(struct dmsrc_super *super, int is_write, int category){
	struct workload_predictor *wp = &super->wp;
	struct io_stat_t *iostat = &wp->iostat[atomic_read(&wp->cur_window)];

	atomic64_inc(&iostat->iops[category][is_write]);
	atomic64_inc(&iostat->iops[category][REQ_TYPE_TOTAL]);

	atomic64_inc(&iostat->iops[REQ_CATEGORY_TOTAL][is_write]);
	atomic64_inc(&iostat->iops[REQ_CATEGORY_TOTAL][REQ_TYPE_TOTAL]);
}

int wp_init(struct dmsrc_super *super)
{
	struct workload_predictor *wp = &super->wp;
	int i, j;
	u32 track_ms;

	wp->super = super;
	track_ms = 100;
	wp->window_per_sec = (MSEC_PER_SEC/track_ms);
	wp->num_window = wp->window_per_sec*2;
	if(wp->num_window>MAX_WINDOW_NUM)
		wp->num_window = MAX_WINDOW_NUM;

	wp->track_period = ktime_set(0, track_ms*NSEC_PER_MSEC);
	printk(" Workload Perdictor: tracking period = %dms\n", track_ms);

	spin_lock_init(&wp->lock);

	hrtimer_init( &wp->hr_timer_track, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	wp->hr_timer_track.function = &wp_callback;
	hrtimer_start( &wp->hr_timer_track, wp->track_period, HRTIMER_MODE_REL );

	hrtimer_init( &wp->hr_timer_meter, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	wp->hr_timer_meter.function = &wp_display;
	wp->display_period = ktime_set(1, 0);
	hrtimer_start( &wp->hr_timer_meter, wp->display_period, HRTIMER_MODE_REL );

	for(i = 0;i < wp->num_window;i++){
		struct io_stat_t *iostat = &wp->iostat[i];
		for(j = 0;j <  REQ_TYPE_NUM;j++){
			atomic64_set(&iostat->iops[i][j], 0);
		}
	}

	atomic_set(&wp->cur_window,0);
	atomic_set(&wp->start_window,0);


	wp->initialized = 1;

	return 0;
}

void wp_cleanup(struct dmsrc_super *super)
{
	struct workload_predictor *wp = &super->wp;
	int ret;

	if(wp->initialized==0)
		return;

retry:
	ret = hrtimer_cancel( &wp->hr_timer_track );
	if (ret) {
		goto retry;
	}

	ret = hrtimer_cancel( &wp->hr_timer_meter );
	if (ret) {
		goto retry;
	}

	wp->initialized = 0;

	return;
}



void put_devices(struct dmsrc_super *super){
	struct dm_target *ti = super->ti;
	struct device_info *dev_info = &super->dev_info;
	struct dm_dev *dev;
	int i;

	dev = dev_info->origin_dev;
	if(dev){
		dm_put_device(ti, dev);
	}
	dev_info->origin_dev = NULL;;

	for(i = 0;i < dev_info->num_cache_devs;i++){
		dev = dev_info->cache_dev[i];
		if(dev)
			dm_put_device(ti, dev);
		dev_info->cache_dev[i] = NULL;
	}

	for(i = 0;i < super->dev_info.num_spare_cache_devs;i++){
		dev = dev_info->spare_cache_dev[i];
		if(dev)
			dm_put_device(ti, dev);
		dev_info->spare_cache_dev[i] = NULL;
	}
}

int get_devices(struct dmsrc_super *super){
	struct dm_target *ti = super->ti;
	struct device_info *dev_info = &super->dev_info;
	int i;
	int r;
	char *str;

	str = dev_info->origin_name;
	r = dm_get_device(ti, str, dm_table_get_mode(ti->table),
			  &super->dev_info.origin_dev);
	if(r){
		ti->error = "couldn't get origin dev";
		return r;
	}

	for(i = 0;i < dev_info->num_cache_devs;i++){
		str = dev_info->cache_name[i];
		r = dm_get_device(ti, str, dm_table_get_mode(ti->table),
				  &super->dev_info.cache_dev[i]);
		if (r) {
			ti->error = "couldn't get cache dev";
			printk( "couldn't get cache dev %s \n", str);
			goto bad;
		}
	}

	return 0;

bad:
	put_devices(super);

	return r;
}

int calc_need_num_ssds(struct dmsrc_super *super){
	u32 cur_bw_mb;
	u32 remain_bw;
	u32 need_ssds;

	wp_get_iops(super, &cur_bw_mb, REQ_CATEGORY_TOTAL, REQ_TYPE_WRITE);

	need_ssds = cur_bw_mb / super->dev_info.per_cache_bw + 1; 
	remain_bw = cur_bw_mb % super->dev_info.per_cache_bw;
	if(remain_bw > cur_bw_mb / super->dev_info.per_cache_bw)
		need_ssds++;
	//if(cur_bw_mb % super->dev_info.per_cache_bw)
	//	need_ssds++;

	if(need_ssds> NUM_SSD)
		need_ssds = NUM_SSD;
#if 0
	if(need_ssds<3)
		need_ssds = 3;
#else 
	if(need_ssds<2)
		need_ssds = 2;
#endif 

	//printk(" cur get iops = %d %dKB, need_ssds= %d \n", (int)wp_get_iops(super, NULL), (int)wp_get_iops(super, NULL)*4, need_ssds);
	printk(" bw_mb = %d, NUM SSDs %d \n", cur_bw_mb, need_ssds);
//need_ssds = NUM_SSD-1;
	//need_ssds = 2;

	return need_ssds;
}

int devinfo_init(struct dmsrc_super *super, int num_ssd){
	int i;
	int r = 0;

	super->dev_info.num_spare_cache_devs = 0;
	for(i = 0;i < MAX_CACHE_DEVS;i++){
		super->dev_info.spare_cache_dev[i] = NULL;
	}

	if(USE_ERASURE_RAID6(&super->param)){
		if(NUM_SSD<4){
			printk(" NUM SSD is too small %d \n", NUM_SSD);
			r = -1;
		}
	}else if(USE_ERASURE_PARITY(&super->param)){
		if(NUM_SSD<2){
			printk(" NUM SSD is too small %d \n", NUM_SSD);
			r = -1;
		}
	}

	if(NO_USE_ERASURE_CODE(&super->param))
		NUM_DATA_SSD = NUM_SSD;
	else if(USE_ERASURE_PARITY(&super->param))
		NUM_DATA_SSD = NUM_SSD-1;
	else 
		NUM_DATA_SSD = NUM_SSD-2;

	super->dev_info.per_cache_bw  = 200; // bandwidth (MB/s)
	return r;
}

int hot_filter_check(struct dmsrc_super *super, unsigned int pageno){
	struct hot_data_filter *hot_filter = &super->hot_filter;
	unsigned long flags;
	unsigned long *cur_bitmap_ptr;
	unsigned int offset[8];
	unsigned int hot_count = 0;
	unsigned int i;

	//printk(" check: pageno = %u \n",pageno);
	offset[0] = crc32(17, (unsigned char *)&pageno, sizeof(unsigned int)) % hot_filter->num_bits_per_table; 
	offset[1] = jhash_1word(pageno, 0xbeef) % hot_filter->num_bits_per_table;
	offset[2] = hash_32(pageno, 32) % hot_filter->num_bits_per_table;

	spin_lock_irqsave(&hot_filter->lock, flags);
	for(i = 0;i < hot_filter->bitmap_table_num;i++){
		cur_bitmap_ptr = hot_filter->bitmap[i];
		if(test_bit(offset[0], cur_bitmap_ptr) && 
			test_bit(offset[1], cur_bitmap_ptr) &&
			test_bit(offset[2], cur_bitmap_ptr))
		{
			hot_count++;
		}
	}
	spin_unlock_irqrestore(&hot_filter->lock, flags);

	if(hot_count>=hot_filter->hot_threshold)
		return 1;

	return 0;

}

void hot_filter_update(struct dmsrc_super *super, unsigned int pageno){
	struct hot_data_filter *hot_filter = &super->hot_filter;
	unsigned long flags;
	unsigned long *cur_bitmap;
	unsigned int offset[8];

	//printk(" update: pageno = %u \n",pageno);
	offset[0] = crc32(17, (unsigned char *)&pageno, sizeof(unsigned int)) % hot_filter->num_bits_per_table; 
	offset[1] = jhash_1word(pageno, 0xbeef) % hot_filter->num_bits_per_table;
	offset[2] = hash_32(pageno, 32) % hot_filter->num_bits_per_table;

	spin_lock_irqsave(&hot_filter->lock, flags);
	cur_bitmap = hot_filter->bitmap[hot_filter->cur_table];
	set_bit(offset[0], cur_bitmap);
	set_bit(offset[1], cur_bitmap);
	set_bit(offset[2], cur_bitmap);

	hot_filter->io_count++;
	if(hot_filter->io_count == hot_filter->decay_period){
		hot_filter->io_count = 0;
		hot_filter->cur_table = (hot_filter->cur_table + 1) % hot_filter->bitmap_table_num;
		if(hot_filter->cur_table == hot_filter->last_table){
			hot_filter->last_table = (hot_filter->last_table + 1) % hot_filter->bitmap_table_num;
		}
		memset(hot_filter->bitmap[hot_filter->cur_table], 0x00, hot_filter->num_bytes_per_table);
	}
	spin_unlock_irqrestore(&hot_filter->lock, flags);

}

#define ceil(n, d) (((n) < 0) ? (-((-(n))/(d))) : (n)/(d) + ((n)%(d) != 0))

int hot_filter_init(struct dmsrc_super *super){
	struct hot_data_filter *hot_filter = &super->hot_filter;
	unsigned int i;

	spin_lock_init(&hot_filter->lock);

	hot_filter->cur_table = 0;
	hot_filter->last_table = 0;
	hot_filter->hash_num = 3;
	hot_filter->hot_threshold = 2;
	hot_filter->decay_period = NUM_BLOCKS/2;
	//hot_filter->decay_period = 1024*256; //1GB
	hot_filter->bitmap_table_num = MAX_BITMAP_TABLE;
	hot_filter->num_bits_per_table = NUM_BLOCKS * hot_filter->hash_num;
	hot_filter->num_bytes_per_table  = ceil(hot_filter->num_bits_per_table, 8);
	printk(" num bits per table = %d \n", hot_filter->num_bits_per_table);

	for(i = 0;i <hot_filter->bitmap_table_num;i++){
		printk(" Hot Filter[%d]: bytes per table = %dKB \n", i, hot_filter->num_bytes_per_table/1024);
		hot_filter->bitmap[i] = vmalloc(hot_filter->num_bytes_per_table);
		if(!hot_filter->bitmap[i]){
			printk(" vmalloc error \n");
			return -1;
		}
		memset(hot_filter->bitmap[i], 0x00, hot_filter->num_bytes_per_table);
	}

	return 0;
}

void hot_filter_deinit(struct dmsrc_super *super){
	struct hot_data_filter *hot_filter = &super->hot_filter;
	unsigned int i;

	for(i = 0;i <hot_filter->bitmap_table_num;i++){
		if(hot_filter->bitmap[i])
			vfree(hot_filter->bitmap[i]); 
	}
}

#define CREATE_DAEMON(name) \
	do { \
		super->name##_daemon = kthread_create(name##_proc, super, \
						      #name "_daemon"); \
		if (IS_ERR(super->name##_daemon)) { \
			r = PTR_ERR(super->name##_daemon); \
			super->name##_daemon = NULL; \
			WBERR("couldn't spawn" #name "daemon"); \
			goto bad_##name##_daemon; \
		} \
		wake_up_process(super->name##_daemon); \
	} while (0)



#if 0 
static sector_t max_io_len_target_boundary(sector_t sector, struct dm_target *ti)
{
	sector_t target_offset = dm_target_offset(ti, sector);

	return ti->len - target_offset;
}
static sector_t max_io_len(sector_t sector, struct dm_target *ti)
{
	sector_t len = max_io_len_target_boundary(sector, ti);
	sector_t offset, max_len;

	/*
	 * Does the target need to split even further?
	 */
	if (ti->max_io_len) {
		offset = dm_target_offset(ti, sector);
		if (unlikely(ti->max_io_len & (ti->max_io_len - 1)))
			max_len = sector_div(offset, ti->max_io_len);
		else
			max_len = offset & (ti->max_io_len - 1);
		max_len = ti->max_io_len - max_len;

		if (len > max_len)
			len = max_len;
	}

	return len;
}
#endif

void print_param(struct dmsrc_super *super){

	printk(" SRC Page Size = %d \n", SRC_PAGE_SIZE);
	printk(" SRC Sectors per Page = %d \n", SRC_SECTORS_PER_PAGE);
	printk(" SRC Sectors per Page Shift = %d \n", SRC_SECTORS_PER_PAGE_SHIFT);

	if(SUMMARY_SCHEME==SUMMARY_PER_CHUNK)
		printk(" Summary Scheme: Per Chunk\n");
	else{
		printk(" Summary Scheme: Per Stripe (Unsupported)\n");
		BUG_ON(1);
	}

	if(NO_USE_ERASURE_CODE(&super->param))
		printk(" Erasure Code: None\n");
	else if(USE_ERASURE_PARITY(&super->param))
		printk(" Erasure Code: Parity\n");
	else 
		printk(" Erasure Code: RAID-6\n");


	if(super->param.parity_allocation==PARITY_ALLOC_FIXED)
		printk(" Parity Allocation: Fixed\n");
	else
		printk(" Parity Allocation: Rotated\n");

	if(super->param.data_allocation==DATA_ALLOC_HORI)
		printk(" Data Allocation: Horizontal\n");
	else if(super->param.data_allocation==DATA_ALLOC_VERT)
		printk(" Data Allocation: Vertical\n");
	else if(super->param.data_allocation==DATA_ALLOC_FLEX_VERT)
		printk(" Data Allocation: Flexible Vertical \n");
	else
		printk(" Data Allocation: Flexible Horizontal\n");

	if(super->param.striping_policy==SEPAR_STRIPING)
		printk(" Striping Policy: Separated\n");
	else
		printk(" Striping Policy: Mixed\n");

	if(super->param.hit_bitmap_type==HIT_BITMAP_PER_STRIPE)
		printk(" Hit Bitmap Type: Per Stripe\n");
	else
		printk(" Hit Bitmap Type: Per Block\n");

	if(super->param.reclaim_policy==RECLAIM_SELECTIVE)
		printk(" Reclaim Policy: Selective \n");
	else if(super->param.reclaim_policy==RECLAIM_GC)
		printk(" Reclaim Policy: GC\n");
	else
		printk(" Reclaim Policy: Destage\n");

	if(super->param.victim_policy==VICTIM_GREEDY)
		printk(" Victim Policy: Greedy\n");
	else if(super->param.victim_policy==VICTIM_CLOCK)
		printk(" Victim Policy: Clock\n");
	else
		printk(" Victim Policy: LRU\n");

	printk(" Chunk size = %d,(%dKB) \n", super->param.chunk_size, SECTOR_SIZE * super->param.chunk_size/1024);
	printk(" Num Summary  = %d\n", super->param.num_summary_per_chunk);
	printk(" Metablock size = %d \n", (int)sizeof(struct metablock));
	printk(" GC Low Water = %d Segments \n", MIGRATE_LOWWATER);
	printk(" GC HIGH Water = %d Segments \n", MIGRATE_HIGHWATER);
	printk(" Dirty Seg Buffer Timeout= %d us \n", super->param.sync_interval);
}

int __must_check resume_managers(struct dmsrc_super *super)
{
	int r = 0;

	r = init_rambuf_pool(super);
	if(r)
		return r;

	r = init_segment_header_array(super);
	if(r)
		return r;

	r = ht_empty_init(super);
	if(r)
		return r;

	r = degraded_mgr_init(super);
	if(r)
		return r;

	/* Migration Worker */
	r = migrate_mgr_init(super);
	if(r)
		return r;

	/* Flush Daemon */
	r = flush_mgr_init(super);
	if(r)
		return r;

	r = recovery_mgr_init(super);
	if(r)
		return r;

	r = plugger_init(super);
	if(r)
		return r;

	r = sync_mgr_init(super);
	if(r)
		return r;

	r = read_miss_mgr_init(super);
	if(r)
		return r;

	r = pending_mgr_init(super);
	if(r)
		return r;

#if (USE_SEG_WRITER == 1)
	r = seg_write_mgr_init(super);
	if(r)
		return r;
#endif 

	r = create_daemon(super, &super->checker_daemon, checker_proc, "checker_daemon");
	if(r)
		return r;

	r = wp_init(super);
	if(r)
		return r;

	if(super->param.enable_read_cache){
		printk(" clean dram buffer size = %dMB \n", super->param.rambuf_pool_amount/256);
		lru_init(&super->clean_dram_cache_manager, "LRU", super->param.rambuf_pool_amount, 1, 0);
	}

#ifdef USE_GHOST_BUFFER
	if(super->param.hot_identification){
		//lru_init(&super->lru_manager, "LRU", NUM_BLOCKS, 1, 0);
		r = hot_filter_init(super);
		if(r)
			return r;
	}
#endif

	seg_allocator_init(super);
	seq_detector_init(super);
	r = multi_allocator_init(super);

	return r;
}

void stop_managers(struct dmsrc_super *super){

	printk(" Stopping check mgr \n");
	if(super->checker_daemon){
		wake_up_process(super->checker_daemon);
		kthread_stop(super->checker_daemon);
	}

	wp_cleanup(super);

	printk(" Stopping sync damone \n");
	sync_mgr_deinit(super);

	printk(" Stopping migrate damone \n");
	migrate_mgr_deinit(super);

	printk(" Stopping flush mgr \n");
	flush_mgr_deinit(super);


	degraded_mgr_deinit(super);
	recovery_mgr_deinit(super);

	plugger_deinit(super);
	printk(" Stopping read miss damone \n");
	read_miss_mgr_deinit(super);
#if (USE_SEG_WRITER == 1)
	seg_write_mgr_deinit(super);
#endif 
	printk(" Stopping pending damone \n");
	pending_mgr_exit(super);

	free_rambuf_pool(super);
	free_segment_header_array(super);
	free_ht(super);

	multi_allocator_deinit(super);

	if(super->param.enable_read_cache){
		if(super->clean_dram_cache_manager)
			lru_deinit(super->clean_dram_cache_manager);
	}

#ifdef USE_GHOST_BUFFER
	if(super->param.hot_identification){
		//if(super->lru_manager)
		//	lru_deinit(super->lru_manager);
		hot_filter_deinit(super);
	}
#endif 
	printk(" Stopping all manages \n");
}


/*
 * Create a device
 * See Documentation for detail.
*/
static int dmsrc_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct dmsrc_super *super;
	int r = 0;

	r = dm_set_target_max_io_len(ti, (SRC_SECTORS_PER_PAGE));
	if (r) {
		ti->error = "failed to set max_io_len";
		return r;
	}

	super = kzalloc(sizeof(*super), GFP_KERNEL);
	if (!super) {
		ti->error = "couldn't allocate super";
		return -ENOMEM;
	}

	ti->flush_supported = true;
	ti->discards_supported = false;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->discard_zeroes_data_unsupported = true;
	ti->per_bio_data_size = sizeof(struct bio_ctx);
	ti->private = super;

	super->ti = ti;
	spin_lock_init(&super->io_lock);

	super->io_client = dm_io_client_create();
	if (IS_ERR(super->io_client)) {
		WBERR("failed to alloc io_client");
		r = PTR_ERR(super->io_client);
		goto bad_init;
	}

	set_default_param(super, &super->param);

	r = consume_essential_argv(super, argc, argv);
	if(r){
		goto bad_init;
	}
	
	r = get_devices(super);
	if(r){
		goto bad_init;
	}

	r = scan_superblock(super);
	if (r) {
		ti->error = "failed to audit cache device";
		goto bad_init;
	}

	r = devinfo_init(super, super->dev_info.num_cache_devs);
	if (r) {
		ti->error = "failed to init devinfo";
		goto bad_init;
	}

	if(USE_ERASURE_CODE(&super->param))
		raid_conf_init(super, &super->raid_conf);

	r = resume_managers(super);
	if (r) {
		ti->error = "failed to resume managers";
		goto bad_init;
	}

	r = scan_metadata(super);
	if (r) {
		ti->error = " failed to recovery metadata in SSDs";
		goto bad_init;
	}

	stat_init(super);
	print_param(super);

	printk(" DM-SRC has been successfuly started. \n");
	return r;

#if 0
	xor_test(super);
	pq_test(super);
#endif


bad_init:

	dmsrc_dtr(ti);

	return r;
}


void dmsrc_dtr(struct dm_target *ti)
{
	struct dmsrc_super *super = ti->private;

	stop_managers(super);
	put_devices(super);
	if(super->io_client)
		dm_io_client_destroy(super->io_client);
	kfree(super);
	ti->private = NULL;

	printk(" DM-SRC has been removed.\n");
}

#if 0 
int wait_schedule(void *unused)
{
	schedule();
	return 0;
}
#endif

#if 0 
/*
 * .postsuspend is called before .dtr
 * same code not needed in .dtr
 */
static void dmsrc_postsuspend(struct dm_target *ti)
{
	int r;
	int i;

	struct dmsrc_super *super = ti->private;
	struct dmsrc_super *super = super->super[0];


	//while(atomic_read(&super->corrupt_segments) && atomic_read(&super->mig_inflights)){
		//printk(" migrate wait .. inflight = %d free = %d \n", atomic_read(&super->mig_inflights), get_alloc_count(super) );
	//	schedule_timeout_interruptible(msecs_to_jiffies(10));
	//}

	//flush_current_buffer(super, superUF);

	for(i=0;i<NUM_SSD;i++)
		IO(blkdev_issue_flush(super->cache_dev[i]->bdev, GFP_NOIO, NULL));
}
#endif 

static void dmsrc_resume(struct dm_target *ti) {}

static int dmsrc_message(struct dm_target *ti, unsigned argc, char **argv)
{
	//struct dmsrc_super *super = ti->private;
	//struct dmsrc_super *super = super->super;

	struct dm_arg_set as;
	as.argc = argc;
	as.argv = argv;

#if 0
	if (!strcasecmp(argv[0], "clear_stat")) {
		clear_stat(super);
		return 0;
	}
#endif 

	return do_consume_tunable_argv(ti, &as, 2);
}

static int dmsrc_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
			    struct bio_vec *biovec, int max_size)
{
	struct dmsrc_super *super = ti->private;
	struct dm_dev *device = super->dev_info.origin_dev;
	struct request_queue *q = bdev_get_queue(device->bdev);

	if (!q->merge_bvec_fn)
		return max_size;

	bvm->bi_bdev = device->bdev;
	return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

static int dmsrc_iterate_devices(struct dm_target *ti,
				      iterate_devices_callout_fn fn, void *data)
{
	struct dmsrc_super *super;
	struct dm_dev *orig;
	sector_t start = 0;
	sector_t len;

	super = ti->private;
	orig = super->dev_info.origin_dev;
	if(orig){
		len = super->dev_info.origin_sectors;
		return fn(ti, orig, start, len, data);
	}
	return -1;
}

#if 0 
static void dmsrc_io_hints(struct dm_target *ti,
				struct queue_limits *limits)
{
#if 0 
	blk_queue_io_min(limits, 4096);
	blk_limits_io_min(limits, 4096);
#else
	//blk_queue_io_min(limits, 32768);
	//blk_limits_io_min(limits, 32768);
#endif 
	//blk_limits_io_opt(limits, 32768);
}
#endif 


#if 0
static void dmsrc_status(struct dm_target *ti, status_type_t type,
			      unsigned flags, char *result, unsigned maxlen)
{
#if 0 
	ssize_t sz = 0;
	char buf[BDEVNAME_SIZE];
	struct dmsrc_super *super = ti->private;
	struct dmsrc_super *super = super->super[0];
	size_t i;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%u %u %llu %llu %llu %llu %llu",
		       (unsigned int)
		       super->cursor[superUF],
		       (unsigned int)
		       nr_caches,
		       (long long unsigned int)
		       super->nr_segments,
		       (long long unsigned int)
		       super->current_seg[superUF]->sequence,
		       (long long unsigned int)
		       atomic64_read(&super->last_flushed_segment_id),
		       (long long unsigned int)
		       atomic64_read(&super->last_migrated_segment_id),
		       (long long unsigned int)
		       atomic64_read(&super->nr_dirty_caches));

		for (i = 0; i < STATLEN; i++) {
			atomic64_t *v = &super->stat[i];
			DMEMIT(" %llu", (unsigned long long) atomic64_read(v));
		}
		DMEMIT(" %llu", (unsigned long long) atomic64_read(&super->count_non_full_flushed));
		emit_tunables(super, result + sz, maxlen - sz);
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("0");
		format_dev_t(buf, super->origin_dev->bdev->bd_dev),
		DMEMIT(" %s", buf);
		//format_dev_t(buf, super->cache_dev->bdev->bd_dev),
		//DMEMIT(" %s", buf);
		DMEMIT(" 4 chunk_size_order %u rambuf_pool_amount %u",
		       super->param->chunk_size_order,
		       super->param->rambuf_pool_amount);
		//if (super->should_emit_tunables)
		//	emit_tunables(super, result + sz, maxlen - sz);
		break;
	}
#endif 
}
#endif 

#if 1 
static struct target_type dmsrc_target = {
	.name = "dmsrc",
	.version = {0, 1, 0},
	.module = THIS_MODULE,
	.map = dmsrc_map,
	.end_io = dmsrc_end_io,
	.ctr = dmsrc_ctr,
	.dtr = dmsrc_dtr,
	.resume = dmsrc_resume,
	.merge = dmsrc_merge,
	.message = dmsrc_message,
	.iterate_devices = dmsrc_iterate_devices,
};
#else
static struct target_type dmsrc_target = {
	.name = "dmsrc",
	.version = {0, 1, 0},
	.module = THIS_MODULE,
	.map = dmsrc_map,
	.end_io = dmsrc_end_io,
	.ctr = dmsrc_ctr,
	.dtr = dmsrc_dtr,
	//.postsuspend = dmsrc_postsuspend,
	.resume = dmsrc_resume,
	.merge = dmsrc_merge,
	.message = dmsrc_message,
	//.status = dmsrc_status,
	//.io_hints = dmsrc_io_hints,
	.iterate_devices = dmsrc_iterate_devices,
};

#endif 

static int __init dmsrc_module_init(void)
{
	int r = 0;

	r = dm_register_target(&dmsrc_target);
	if (r < 0) {
		WBERR("failed to register target err(%d)", r);
		return r;
	}

	return 0;
}

static void __exit dmsrc_module_exit(void)
{
	dm_unregister_target(&dmsrc_target);
}

module_init(dmsrc_module_init);
module_exit(dmsrc_module_exit);

MODULE_AUTHOR("Yongseok Oh <ysoh@uos.ac.kr>");
MODULE_DESCRIPTION(DM_NAME " dmsrc target");
MODULE_LICENSE("GPL");



#if 0 
void cursor_inc(struct dmsrc_super *super,int seg_id, int cache_type){
	u32 next;

	// vertical allocation 
	if(super->param->data_allocation==DATA_ALLOC_VERT){
		div_u64_rem(super->cursor[cache_type] + 1, NR_CACHES_INSEG, &next);
	}else{ // horizontal allocation 

		u32 col, row;
		u32 start_ssd;
		u32 parity_ssd;

		start_ssd = get_start_ssd(super, seg_id);

		col = super->col[cache_type];
		row = super->row[cache_type]; 
		col = (col + 1) % NR_SSD;

		if(is_write_stripe(cache_type) && 
			USE_ERASURE_CODE(super->param)){

			parity_ssd = get_parity_ssd(super, seg_id);

			if(col==parity_ssd ||
				(super->param->erasure_code==ERASURE_CODE_RAID6 && 
				 col == (parity_ssd+1)%NR_SSD)){
				while(col!=start_ssd){
					col = (col + 1) % NR_SSD;
				}
			}
		}

		if(col==start_ssd)
			row++;

		next = col * CHUNK_SZ + row;
		next %= STRIPE_SZ;

		super->col[cache_type] = col;
		super->row[cache_type] = row;
	}

	//printk(" next = %d \n", (int)next);

	super->cursor[cache_type] = next;
}
#endif


#if 0 
inline int cursor_start(struct dmsrc_super *super, u64 seg_id, int cache_type){
	if(super->param->parity_allocation==PARITY_ALLOC_FIXED){
		return 0;
	}else{
		int start = seg_id % NR_SSD;
		return start * CHUNK_SZ;
	}
}
#endif 
#if 0 
void flush_pending_bios(struct dmsrc_super *super){
#if 1
	struct bio_list local_list;
	struct bio *bio;
	unsigned long f;
	int retry_resons[RES_COUNT];
	int local_count = 0;
	int i;

	for(i=0;i<RES_COUNT;i++){
		retry_resons[i] = 0;
	}

	bio_list_init(&local_list);

	while (1) {
		int reason;

		spin_lock_irqsave(&super->pending_lock, f);
		bio = bio_list_pop(&super->pending_bios);
		if(bio)
			atomic64_dec(&super->pending_io_count);
		spin_unlock_irqrestore(&super->pending_lock, f);

		if(!bio)
			break;

		reason = map_pending_bio(super, bio);
		if(reason<0){
			bio_list_add(&local_list, bio);
			local_count++;
			//pending_bio_add(super, bio);
			retry_resons[reason*-1]++;
		}
	}

	if(local_count){
		//printk(" local count = %d %d\n", local_count, 
				//(int)atomic64_read(&super->pending_io_count) );
		spin_lock_irqsave(&super->pending_lock, f);
		bio_list_merge(&super->pending_bios, &local_list);
		///atomic64_add((long long)local_count, &super->pending_io_count);
		for(i=0;i<local_count;i++)
			atomic64_inc(&super->pending_io_count);
		spin_unlock_irqrestore(&super->pending_lock, f);
	}

#else
	struct bio *bio;
	struct bio_list local_list;
	//struct blk_plug plug;
	int retry_count = 0;
#if (IOLOCK == IOLOCK_SPIN) 
	unsigned long f;
#endif 
	int retry_resons[RES_COUNT];
	int i;

	for(i=0;i<RES_COUNT;i++){
		retry_resons[i] = 0;
	}


retry:;

	bio_list_init(&local_list);
	spin_lock_irqsave(&super->pending_lock, f);
	bio_list_merge(&local_list, &super->pending_bios);
	bio_list_init(&super->pending_bios);
	atomic64_set(&super->pending_io_count, 0);
	spin_unlock_irqrestore(&super->pending_lock, f);

	while ((bio = bio_list_pop(&local_list))) {
		int reason;
		reason = map_pending_bio(super, bio);
		if(reason<0)
			retry_resons[reason*-1]++;
	}

#endif 

	bio_list_init(&local_list);
	spin_lock_irqsave(&super->barrier_lock, f);
	bio_list_merge(&local_list, &super->barrier_ios);
	bio_list_init(&super->barrier_ios);
	spin_unlock_irqrestore(&super->barrier_lock, f);

	issue_deferred_bio(super, &local_list);

#if 0 
	if(++retry_count>=10){
		printk(" retry_count = %d \n", retry_count);
		for(i=0;i<RES_COUNT;i++){
			printk(" Reson %d = %d \n", i, retry_resons[i]);
			retry_resons[i] = 0;
		}
		schedule_timeout_interruptible(usecs_to_jiffies(10000));
		retry_count = 0;
	}


	if (!pending_bio_empty(super)) {
		schedule_timeout_interruptible(usecs_to_jiffies(10));
		retry_count++;
		goto retry;
	}
#endif 

}
#endif 

#if 0 
static void _dmsrc_dtr(struct dmsrc_super *super){
	int i;

	if(!super)
		return;

	if(atomic_read(&super->migrate_mgr.background_gc_on)){
		atomic_set(&super->migrate_mgr.background_gc_on, 0);
	}

	for(i = 0;i < NUM_SSD;i++){
		flush_plug_proc(super, 0, 1, i);
	}	

	while( atomic_read(&super->recovery_mgr.broken_block_count)){
		printk(" Wating recovery proc... broken block = %d\n", 
				(int)atomic_read(&super->recovery_mgr.broken_block_count));
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}

	while(atomic_read(&super->flush_mgr.count)){
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
		printk(" wait for flush queue\n");
	}

#if 0
	LOCK(super, f);
	seg = super->current_seg[superUF];
	length = atomic_read(&seg->part_length);
	UNLOCK(super, f);

	if(length){
		flush_current_buffer(super, superUF);
	}

	LOCK(super, f);
	seg = super->current_seg[RBUF];
	length = atomic_read(&seg->length);
	UNLOCK(super, f);

	if(length){
		flush_current_buffer(super, RBUF);
	}
#endif

#if 0 
	start_recovery(super);


	//while(atomic_read(&super->force_recovery)){
	//	wait_event_interruptible_timeout(super->recovery_wait_queue,
	//		!atomic_read(&super->force_recovery), msecs_to_jiffies(1000));
	//}

	while(atomic_read(&super->force_recovery)){
//		schedule_timeout_interruptible(usecs_to_jiffies(10000));
		msleep(1000);
		printk(" Recovery sleep \n");
	}
#endif 

	//super->urge_migrate = false;
	super->migrate_mgr.allow_migrate = false;
	while(atomic_read(&super->migrate_mgr.mig_inflights)){
		schedule_timeout_interruptible(usecs_to_jiffies(100));
	}
	migrate_mgr_deinit(super);

	pending_mgr_exit(super);

	free_cache(super);

#ifdef USE_GHOST_BUFFER
	CACHE_CLOSE(super->lru_manager);
#endif 

	kfree(super->lru_manager);
	//kfree(super);

}
#endif
#if 0 
static void prepare_meta_rambuffer(struct rambuf_page **pages,
				   struct dmsrc_super *super,
				   struct segment_header *seg, int cache_type, int full)
{
	struct metablock *new_mb;
	u32 summary_offset;

	summary_offset = cursor_summary_offset(super, seg->seg_id, seg->seg_type);
	
	new_mb = get_mb(super, seg->seg_id, summary_offset);
	clear_bit(MB_DIRTY, &new_mb->mb_flags);
	clear_bit(MB_VALID, &new_mb->mb_flags);
	set_bit(MB_SUMMARY, &new_mb->mb_flags);

	if(full){
		atomic_inc(&seg->length);
		atomic_inc(&seg->part_length);
		BUG_ON(atomic_read(&seg->length)>STRIPE_SZ);
	}

	prepare_segment_header_device((struct segment_header_device *)pages[mem_offset]->data,
			super, seg, cache_type, pages);
}
#endif 
