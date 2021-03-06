/****************************************************************************
 * SWAN (Serial management With an Array of solid state drives on Network): 
 * Serial Management of All Flash Array for Sustained Garbage Collection Free High Performance 
 * Jaeho Kim (kjhnet@gmail.com), K. Hyun Lim (limkh4343@gmail.com) 2016 - 2018
 * filename: daemon.c
 * 
 * Based on DM-Writeboost:
 *   1) SRC (SSD RAID Cache): Device mapper target for block-level disk caching
 *      Copyright (C) 2013-2014 Yongseok Oh (ysoh@uos.ac.kr)
 *   2) Log-structured Caching for Linux
 *      Copyright (C) 2012-2013 Akira Hayakawa <ruby.wktk@gmail.com>
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

#include <linux/dm-kcopyd.h>
#include <linux/raid/xor.h>
#include <linux/list_sort.h>
#include <linux/crc32.h>
#include "target.h"
#include "metadata.h"
#include "daemon.h"
#include "alloc.h"
#include "lru.h"

void update_plug_deadline(struct dmsrc_super *super)
{
	mod_timer(&super->plugger.timer,
	  jiffies + usecs_to_jiffies(ACCESS_ONCE(super->plugger.deadline_us)));
}

#if 0
void check_plug_proc(struct dmsrc_super *super, 
		struct segment_header *seg, 
		struct rambuffer *rambuf, 
		int force, 
		int devno){

	//struct blk_plug plug;
	int count = 0;
	int i;
	int full = 0;

	if(atomic_read(&rambuf->bios_count[devno])) {
		if(force){
			count = 1;
			full = 1;
		}else{
			if(atomic_read(&rambuf->bios_count[devno]) >= CHUNK_SZ){
				count = 1;
				full = 1;
			}
		}
	}

	if(1){
		int count = 0;
		unsigned long f;

		spin_lock_irqsave(&rambuf->lock, f);
		for(i = (int)atomic_read(&rambuf->bios_start[devno]);
			i < (int)atomic_read(&rambuf->bios_count[devno]);
			i++)
		{
			count++;
		}
		spin_unlock_irqrestore(&rambuf->lock, f);
		printk(" dev = %d, count = %d \n", devno, count);
	}
}
#endif

int flush_plug_proc(struct dmsrc_super *super, 
		struct segment_header *seg, 
		struct rambuffer *rambuf, 
		atomic_t *bios_start,
		atomic_t *bios_count,
		int force, 
		int devno, 
		int flush_command){

	struct wb_job *job;
	int count = 0;
	int i;
	u32 start_idx;


	for(i = (int)atomic_read(&bios_start[devno]);
			i < (int)atomic_read(&bios_count[devno]);
			i++)
	{
		u32 idx = (u32)seg->seg_id * STRIPE_SZ + devno * CHUNK_SZ + i;
		struct bio *bio = rambuf->bios[idx % STRIPE_SZ];
		struct metablock *mb = mb_at(super, idx);

		if(bio){
			atomic_inc(&super->cache_stat.total_bios2);
			atomic_inc(&seg->bios_count);
		}else{
			if(!test_bit(MB_SKIP, &mb->mb_flags)){
	//			job = writeback_make_job(super, seg, idx, 
	//					NULL, rambuf, 1);
			}else{
				printk(" mb skip not support ...\n");
				BUG_ON(1);
				if(atomic_dec_and_test(&rambuf->ref_count)){
					printk(" >>> partial writeback end seg id %d \n", (int)seg->seg_id);
					release_rambuffer(super, rambuf, seg->seg_type);
				}
			}
		}
	}

	start_idx = (u32)seg->seg_id * STRIPE_SZ + devno * CHUNK_SZ + atomic_read(&bios_start[devno]);
	count = atomic_read(&bios_count[devno]) - atomic_read(&bios_start[devno]);


#if (USE_SEG_WRITER == 0)
	if(count){
		job = writeback_make_job_extent(super, seg, rambuf, start_idx, count);
		job->flush_command = flush_command;
		writeback_issue_job_extent(super, job, flush_command);
	}
#else
	if(count){
		struct seg_write_manager *seg_write_mgr = &super->seg_write_mgr[devno];
		unsigned long flags;

		job = writeback_make_job_extent(super, seg, rambuf, start_idx, count);
		job->flush_command = flush_command;

		spin_lock_irqsave(&seg_write_mgr->spinlock, flags);
		list_add_tail(&job->list, &seg_write_mgr->head);
		atomic_inc(&seg_write_mgr->count);
		spin_unlock_irqrestore(&seg_write_mgr->spinlock, flags);

		queue_work(seg_write_mgr->wq, &seg_write_mgr->work);

	}
#endif 

	return count;
}

#if 0
void plug_proc(struct work_struct *work)
{
	struct plugging_manager *plugger = container_of(work, struct plugging_manager, work);
	//struct dmsrc_super *super = plugger->super;

#if 0
	printk(" plug proc ... %d\n", atomic_read(&plugger->total_length));

	for(i = 0;i < NUM_SSD;i++){
		printk(" plug proc ssd = %d \n", i);
		flush_plug_proc(super, seg, rambuf, 0, 1, i);
	}	
#endif

	printk(" end plug proc ... %d \n", atomic_read(&plugger->total_length));
}
#endif

void plug_deadline_proc(unsigned long data)
{
	//struct dmsrc_super *super = (struct dmsrc_super *) data;
	//schedule_work(&super->plugger.work);
}

void queue_barrier_io(struct dmsrc_super *super, struct bio *bio)
{
	unsigned long f;
	
	spin_lock_irqsave(&super->pending_mgr.barrier_lock, f);
	bio_list_add(&super->pending_mgr.barrier_ios, bio);
	atomic_inc(&super->pending_mgr.barrier_count);
	spin_unlock_irqrestore(&super->pending_mgr.barrier_lock, f);
	//printk(" flush command ...%d \n", atomic_read(&super->pending_mgr.barrier_count));
}

void read_callback(unsigned long error, void *context)
{
	struct read_caching_job *gn = context;
	struct dmsrc_super *super = gn->gn_super;
	struct read_miss_manager *read_miss_mgr = &super->read_miss_mgr;
	struct read_caching_job_list *r_list = &read_miss_mgr->queue;
	struct list_head *copy_head = &r_list->rm_copy_head;
	unsigned long flags;

	gn->gn_bio_error = error;
	super = gn->gn_super;

	spin_lock_irqsave(&r_list->rm_spinlock, flags);
	list_add_tail(&gn->gn_list, copy_head);
	atomic_inc(&r_list->rm_copy_count);
	spin_unlock_irqrestore(&r_list->rm_spinlock, flags);

	queue_work(read_miss_mgr->wq, &read_miss_mgr->work);
}

static void map_region(struct dm_io_region *io, struct bio *bio)
{
	io->bdev =bio->bi_bdev;
	io->sector = bio->bi_sector;
	io->count = bio_sectors(bio);
}

inline struct read_caching_job *alloc_read_caching_job(struct dmsrc_super *super){
	struct read_caching_job *gn;

	gn = mempool_alloc(super->read_miss_mgr.job_pool, GFP_NOIO);
	if (!gn) {
		WBERR();
		BUG_ON(1);
		return NULL;;
	}

	return gn;
}

void read_caching_make_job(struct dmsrc_super *super, struct segment_header *seg, struct metablock *mb, struct bio *bio, struct rambuffer *rambuf)
{
	struct dm_io_region io;
	struct dm_io_request io_req;
	struct read_caching_job *gn;

	gn = alloc_read_caching_job(super);
	gn->gn_sector = bio->bi_sector;
	gn->gn_bio = bio;
	gn->gn_seg = seg;
	gn->gn_mb = mb;
	gn->gn_rambuf = rambuf;
	gn->gn_super = super;

	io_req.bi_rw = READ,
	io_req.mem.type = DM_IO_BVEC,
	io_req.mem.ptr.bvec = bio->bi_io_vec + bio->bi_idx,
	io_req.notify.fn = read_callback,
	io_req.notify.context = gn,
	io_req.client = super->io_client,
	map_region(&io, bio);

	BUG_ON(bio->bi_sector>=super->dev_info.origin_sectors);

	dmsrc_io(&io_req, 1, &io, NULL);
}

static int more_work(struct dmsrc_super *super){
	struct read_caching_job_list *r_list = &super->read_miss_mgr.queue;

	return  atomic_read(&r_list->rm_copy_count);
}

static void copy_read_caching_data(struct dmsrc_super *super){
	struct read_miss_manager *read_miss_mgr = &super->read_miss_mgr;
	struct read_caching_job_list *r_list = &read_miss_mgr->queue;
	struct list_head *copy_head = &r_list->rm_copy_head;
	struct read_caching_job *gn, *temp;
	struct list_head local_head;

	unsigned long flags;
	//unsigned long f;
	int count;

	count = atomic_read(&r_list->rm_copy_count);
	if(!count)
		return;

	INIT_LIST_HEAD(&local_head);

	spin_lock_irqsave(&r_list->rm_spinlock, flags);
	list_for_each_entry_safe(gn, temp, copy_head, gn_list){
		list_del(&gn->gn_list);
		list_add(&gn->gn_list, &local_head);
		atomic_dec(&r_list->rm_copy_count);
	}
	spin_unlock_irqrestore(&r_list->rm_spinlock, flags);

	list_for_each_entry_safe(gn, temp, &local_head, gn_list){
		struct cache_manager *clean_cache = super->clean_dram_cache_manager;
		struct lru_node *ln = NULL;
		unsigned long lru_flags; 
		sector_t key;
		u32 crc32;
		void *src_ptr, *dst_ptr;
		
		//ptr = kmap_atomic(bio_page(gn->gn_bio));
		src_ptr = page_address(bio_page(gn->gn_bio));
		crc32 = crc32(17, src_ptr, PAGE_SIZE);
		//kunmap_atomic(ptr);

		key  = calc_cache_alignment(super, gn->gn_bio->bi_sector);

		//LOCK(super, f);

		spin_lock_irqsave(&clean_cache->lock, lru_flags);
		ln = CACHE_SEARCH(clean_cache, key);
		if(ln){
			atomic_set(&ln->locked, 1);
			spin_unlock_irqrestore(&clean_cache->lock, lru_flags);

			dst_ptr = page_address(ln->cn_page);
			memcpy(dst_ptr, src_ptr, SRC_PAGE_SIZE);

			spin_lock_irqsave(&clean_cache->lock, lru_flags);
			atomic_set(&ln->locked, 0);
			ln->crc32 = crc32;
			atomic_set(&ln->sealed, 1);
			atomic_inc(&clean_cache->cm_sealed_count);

		}
		//else{
			//printk(" rcaching: lru cache has been already removed %d \n", (int)key);
		//}
		spin_unlock_irqrestore(&clean_cache->lock, lru_flags);

		bio_endio(gn->gn_bio, 0);
		mempool_free(gn, read_miss_mgr->job_pool);
		gn = NULL;
	}
}


void seg_write_worker(struct work_struct *work){
	struct seg_write_manager *seg_write_mgr = 
						container_of(work, struct seg_write_manager,
					    work); 
	struct dmsrc_super *super = seg_write_mgr->super;
	unsigned long flags;
	struct wb_job *job;

	 while (atomic_read(&seg_write_mgr->count)){
		 spin_lock_irqsave(&seg_write_mgr->spinlock, flags);
		 job	 = (struct wb_job *)
			 list_entry(seg_write_mgr->head.next, struct wb_job, list);
		 list_del(&job->list);
		 atomic_dec(&seg_write_mgr->count);
		 spin_unlock_irqrestore(&seg_write_mgr->spinlock, flags);
		 writeback_issue_job_extent(super, job, job->flush_command);
	 }
}

void do_read_caching_worker(struct work_struct *work){
	struct read_miss_manager *read_miss_mgr = 
						container_of(work, struct read_miss_manager,
					    work); 
	struct dmsrc_super *super = read_miss_mgr->super;

	 while (more_work(super)){
		copy_read_caching_data(super);
		if(need_clean_seg_write(super)) {
			//printk(" need clean seg write in read caching worker %d pages \n", 
			//		atomic_read(&super->clean_dram_cache_manager.cm_sealed_count));
			pending_worker_schedule(super);
		}
	 }
}

static void dmsrc_bio_end_flush(struct bio *bio, int err)
{
	if (err)
		clear_bit(BIO_UPTODATE, &bio->bi_flags);
	if (bio->bi_private)
		complete(bio->bi_private);
	bio_put(bio);
}

/**
 * blkdev_issue_flush - queue a flush
 * @bdev:	blockdev to issue flush for
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @error_sector:	error sector
 *
 * Description:
 *    Issue a flush for the block device in question. Caller can supply
 *    room for storing the error offset in case of a flush error, if they
 *    wish to. If WAIT flag is not passed then caller may check only what
 *    request was pushed in some internal queue for later handling.
 */
static struct bio *dmsrc_blkdev_issue_flush(struct block_device *bdev, gfp_t gfp_mask,
		sector_t *error_sector, struct completion *wait)
{
	struct request_queue *q;
	struct bio *bio;

	if (bdev->bd_disk == NULL)
		return NULL;

	q = bdev_get_queue(bdev);
	if (!q)
		return NULL;

	/*
	 * some block devices may not have their queue correctly set up here
	 * (e.g. loop device without a backing file) and so issuing a flush
	 * here will panic. Ensure there is a request function before issuing
	 * the flush.
	 */
	if (!q->make_request_fn)
		return NULL;

	bio = bio_alloc(gfp_mask, 0);
	bio->bi_end_io = dmsrc_bio_end_flush;
	bio->bi_bdev = bdev;
	bio->bi_private = wait;

	bio_get(bio);
	submit_bio(WRITE_FLUSH, bio);
	return bio;

}

static int dmsrc_blkdev_wait_flush(struct bio *bio)
{
	struct completion *wait = bio->bi_private;
	int ret;

	wait_for_completion_io(wait);

	/*
	 * The driver must store the error location in ->bi_sector, if
	 * it supports it. For non-stacked drivers, this should be
	 * copied from blk_rq_pos(rq).
	 */
	//if (error_sector)
		//*error_sector = bio->bi_sector;

	if (!bio_flagged(bio, BIO_UPTODATE))
		ret = -EIO;

	bio_put(bio);
	return ret;
}


/*----------------------------------------------------------------*/
void issue_deferred_bio(struct dmsrc_super *super, struct bio_list *barrier_ios){
	struct device_info *dev_info = &super->dev_info;
	struct bio *bio;
	struct bio *bios[MAX_CACHE_DEVS];
	struct completion wait[MAX_CACHE_DEVS];
	int i;


	if (bio_list_empty(barrier_ios)) {
		return;
	}

	while ((bio = bio_list_pop(barrier_ios))) {
		for(i = 0;i < dev_info->num_cache_devs;i++){
			init_completion(&wait[i]);
			bios[i] = dmsrc_blkdev_issue_flush(dev_info->cache_dev[i]->bdev, GFP_KERNEL, NULL, &wait[i]);
			if(!bios[i]){
				//printk(" blkdev issue flush error, ssd %d \n", i);
			}
			//printk(" flush command to SSD %d \n", i);
		}
		for(i = 0;i < dev_info->num_cache_devs;i++){
			if(bios[i]){
				dmsrc_blkdev_wait_flush(bios[i]);
				//printk(" Complete: flush command to SSD %d \n", i);
			}
		}

		bio_endio(bio, 0);
		//printk(" flush barrier bios .. \n");
	}
}


#if 0 
void gen_partial_summary_io(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuffer *rambuf,
		struct summary_io_job *context){

	struct dm_io_request io_req;
	struct dm_io_region region;
	u32 mem_offset = 0;
	u32 tmp32;
	u32 idx;
	//int r;

	idx = cursor_summary_offset(super, seg->seg_id, seg->seg_type);

	atomic_inc(&(context->count));

	div_u64_rem(idx, STRIPE_SZ, &tmp32);
	mem_offset = tmp32;

	region.bdev = get_bdev(super, idx);
	region.sector = get_sector(super, seg->seg_id, idx);
	region.count = SRC_SECTORS_PER_PAGE;

	io_req.client = super->io_client;
	io_req.bi_rw = WRITE;
	io_req.notify.fn = flush_segmd_endio;
	io_req.notify.context = context;
	io_req.mem.type = DM_IO_KMEM;
	io_req.mem.ptr.addr = rambuf->pages[mem_offset]->data;

	dmsrc_io(&io_req, 1, &region, NULL);

}
#endif




void wait_bufcopy_ios(struct segment_header *seg){
	u32 count = 0;
	while(atomic_read(&seg->num_bufcopy)){
		schedule_timeout_interruptible(usecs_to_jiffies(TIMEOUT_US));
		//printk("[wait_bufcopy_ios] @@@@@ interrupt @@@@@\n");
		count++;
		if(count==100000){
			printk(" WARN: wait bufcopy ios ... \n");
			count = 0;
		}
	}
}

void wait_filling_ios(struct dmsrc_super *super, struct segment_header *seg){
	u32 count = 0;
	while(atomic_read(&seg->num_filling)){
		schedule_timeout_interruptible(usecs_to_jiffies(TIMEOUT_US));
		//printk("[wait_filling_ios] @@@@@ interrupt @@@@@\n");
		count++;
		if(count==100000){
			printk(" WARN: wait filling ios ... \n");
			count = 0;
		}
	}
}

#if SWAN

// flush segment metadata and parity date 
int flush_meta_proc2(void *data)
{
	printk("[flush_meta_proc2] SWAN_GC_CACHE_COLD\n");
	struct dmsrc_super *super = data;
	struct flush_manager *flush_mgr = &super->flush_mgr2;
	struct flush_invoke_job *job;
	unsigned long flags;

	#if SWAN_READ_BLK_GC
	struct group_header *grp;
        u32 phy_col_id;
	#endif



	while (true) {

		spin_lock_irqsave(&flush_mgr->lock, flags);
		while (list_empty(&flush_mgr->queue)) {
			spin_unlock_irqrestore(&flush_mgr->lock, flags);
			schedule_timeout_interruptible(usecs_to_jiffies(10));
			//printk("[flush_meta_proc] @@@@@ interrupt @@@@@\n");

			if (kthread_should_stop())
				return 0;
			else
				spin_lock_irqsave(&flush_mgr->lock, flags);
		}

		job = list_first_entry(&flush_mgr->queue, struct flush_invoke_job, list);
		list_del(&job->list);
		spin_unlock_irqrestore(&flush_mgr->lock, flags);


		//printk(" wait filling ... \n");
		wait_filling_ios(super, job->seg);
		//printk(" buf copy... \n");
		wait_bufcopy_ios(job->seg);


		#if SWAN_READ_BLK_GC

		grp = job->seg->group;
                phy_col_id = grp->phy_col_id;

                while(atomic64_read(&super->num_read_inflight_in_col[phy_col_id])){
                        schedule_timeout_interruptible(msecs_to_jiffies(1000));
                        printk("[flush_meta_proc2] @@@@ GC is interrupted by read! @@@@\n");
                }

		#endif


		build_metadata(super, 
				job->seg, 
				job->seg_length,
				job->rambuf,
				job->bios_start,
				job->bios_count,
				job->force_seal,
				job->build_summary,
				job->flush_data);

		issue_deferred_bio(super, &job->barrier_ios);
		atomic_dec(&flush_mgr->invoke_count);
		mempool_free(job, flush_mgr->invoke_pool);

		pending_worker_schedule(super); //[kh3]
	}

	printk(" finish meta flush thread ... \n");
	return 0;
}


// flush segment metadata and parity date 
int flush_meta_proc1(void *data)
{
	printk("[flush_meta_proc1] SWAN_GC_CACHE_HOT\n");
	struct dmsrc_super *super = data;
	struct flush_manager *flush_mgr = &super->flush_mgr1;
	struct flush_invoke_job *job;
	unsigned long flags;


	#if SWAN_READ_BLK_GC
	struct group_header *grp;
        u32 phy_col_id;
	#endif



	while (true) {

		spin_lock_irqsave(&flush_mgr->lock, flags);
		while (list_empty(&flush_mgr->queue)) {
			spin_unlock_irqrestore(&flush_mgr->lock, flags);
			schedule_timeout_interruptible(usecs_to_jiffies(10));
			//printk("[flush_meta_proc] @@@@@ interrupt @@@@@\n");

			if (kthread_should_stop())
				return 0;
			else
				spin_lock_irqsave(&flush_mgr->lock, flags);
		}

		job = list_first_entry(&flush_mgr->queue, struct flush_invoke_job, list);
		list_del(&job->list);
		spin_unlock_irqrestore(&flush_mgr->lock, flags);


		//printk(" wait filling ... \n");
		wait_filling_ios(super, job->seg);
		//printk(" buf copy... \n");
		wait_bufcopy_ios(job->seg);


		#if SWAN_READ_BLK_GC

		grp = job->seg->group;
                phy_col_id = grp->phy_col_id;

                while(atomic64_read(&super->num_read_inflight_in_col[phy_col_id])){
                        schedule_timeout_interruptible(msecs_to_jiffies(1000));
                        printk("[flush_meta_proc1] @@@@ GC is interrupted by read! @@@@\n");
                }

		#endif


		build_metadata(super, 
				job->seg, 
				job->seg_length,
				job->rambuf,
				job->bios_start,
				job->bios_count,
				job->force_seal,
				job->build_summary,
				job->flush_data);

		issue_deferred_bio(super, &job->barrier_ios);
		atomic_dec(&flush_mgr->invoke_count);
		mempool_free(job, flush_mgr->invoke_pool);

		pending_worker_schedule(super); //[kh3]
	}

	printk(" finish meta flush thread ... \n");
	return 0;
}

#endif



// flush segment metadata and parity date 
int flush_meta_proc(void *data)
{
	struct dmsrc_super *super = data;
	struct flush_manager *flush_mgr = &super->flush_mgr;
	struct flush_invoke_job *job;
	unsigned long flags;

	while (true) {

		spin_lock_irqsave(&flush_mgr->lock, flags);
		while (list_empty(&flush_mgr->queue)) {
			spin_unlock_irqrestore(&flush_mgr->lock, flags);
			schedule_timeout_interruptible(usecs_to_jiffies(10));
			//printk("[flush_meta_proc] @@@@@ interrupt @@@@@\n");

			if (kthread_should_stop())
				return 0;
			else
				spin_lock_irqsave(&flush_mgr->lock, flags);
		}

		job = list_first_entry(&flush_mgr->queue, struct flush_invoke_job, list);
		list_del(&job->list);
		spin_unlock_irqrestore(&flush_mgr->lock, flags);


		//printk(" wait filling ... \n");
		wait_filling_ios(super, job->seg);
		//printk(" buf copy... \n");
		wait_bufcopy_ios(job->seg);

		build_metadata(super, 
				job->seg, 
				job->seg_length,
				job->rambuf,
				job->bios_start,
				job->bios_count,
				job->force_seal,
				job->build_summary,
				job->flush_data);

		issue_deferred_bio(super, &job->barrier_ios);
		atomic_dec(&flush_mgr->invoke_count);
		mempool_free(job, flush_mgr->invoke_pool);

		pending_worker_schedule(super); //[kh3]
	}

	printk(" finish meta flush thread ... \n");
	return 0;
}


static void cleanup_segment(struct dmsrc_super *super, struct segment_header *seg)
{
	u32 i;

	for (i = 0; i < STRIPE_SZ; i++) {
		struct metablock *mb = get_mb(super, seg->seg_id, i);
		cleanup_mb_if_dirty(super, seg, mb);
	}
}



#if 0
void finish_mig_work(struct dmsrc_super *super, struct mig_job *job, int cleanup){

#if 0
	if(is_write_stripe(seg->seg_type)){
		if(CHUNK_SZ+NUM_DATA_SSD!=atomic_read(&seg->valid_count)){
			printk(" Finish: migrating write seg = %d, valid count = %d cleanup = %d  \n", 
					(int)seg->seg_id, (int)atomic_read(&seg->valid_count), cleanup);
			printk(" valid count = %d, dirty count = %d \n", 
					check_valid_count(super, seg), 
					check_dirty_count(super, seg));
			printk(" use gc = %d \n", job->use_gc);
		}
	}else{
		if(NUM_SSD!= atomic_read(&seg->valid_count)){
			printk(" Finish: migrating read seg = %d, valid count = %d cleanup = %d  \n", 
					(int)seg->seg_id, (int)atomic_read(&seg->valid_count), cleanup);
			printk(" valid count = %d, dirty count = %d \n", 
					check_valid_count(super, seg), 
					check_dirty_count(super, seg));
			printk(" use gc = %d \n", job->use_gc);
		}
	}
#endif

	//finalize_clean_seg(job->super, seg, cleanup);


	//printk(" Finish Mig: alloc count = %d, reserve count = %d \n", 
	//		atomic_read(&super->alloc_count), 
	//		atomic_read(&super->reserve_count));

	//if(atomic64_read(&job->count)){
	//	printk(" Finish: migrating seg = %d, count = %d cleanup = %d  \n", 
	//			(int)seg->seg_id, (int)atomic64_read(&job->count), cleanup);
	//}

	BUG_ON(atomic64_read(&job->count));
	mempool_free(job, super->migrate_mgr.mig_job_pool);
	atomic_inc(&super->migrate_mgr.mig_completes);

	pending_worker_schedule(super);
}
#endif

int get_metadata_count(struct dmsrc_super *super, int seg_type){
	int valid_count = 0;

	if(is_write_stripe(seg_type)){
		valid_count += NUM_SUMMARY*NUM_DATA_SSD;
		if(USE_ERASURE_PARITY(&super->param))
			valid_count += CHUNK_SZ; 
	}else{
		valid_count += (NUM_SUMMARY*NUM_SSD);
	}

	return valid_count;
}

int get_data_max_count(struct dmsrc_super *super, int seg_type){

	return STRIPE_SZ - get_metadata_count(super, seg_type);
}

int get_data_valid_count(struct dmsrc_super *super, struct segment_header *seg){
	int valid_count = 0;
	struct group_header *group = &super->group_header_array[seg->seg_id/SEGMENT_GROUP_SIZE];

	if(is_write_stripe(seg->seg_type)){
		u32 num_data_ssd = atomic_read(&group->num_used_ssd)-1;
		valid_count += NUM_SUMMARY*num_data_ssd;
		if(USE_ERASURE_PARITY(&super->param))
			valid_count += CHUNK_SZ; 
	}else{
		valid_count += (NUM_SUMMARY*NUM_SSD);
	}

	return atomic_read(&seg->valid_count)-valid_count;
}

int is_empty_seg(struct dmsrc_super *super, struct segment_header *seg, int use_gc){
#if 0
	int valid_count = 0;
	struct group_header *group = &super->group_header_array[seg->seg_id/SEGMENT_GROUP_SIZE];
	if(use_gc){
		if(is_write_stripe(seg->seg_type)){
			u32 num_data_ssd = atomic_read(&group->num_used_ssd)-1;
			valid_count += NUM_SUMMARY*num_data_ssd;
			if(USE_ERASURE_PARITY(&super->param))
				valid_count += CHUNK_SZ; 
		}else{
			valid_count += (NUM_SUMMARY*NUM_SSD);
		}

		if(atomic_read(&seg->valid_count)<valid_count || atomic_read(&seg->valid_count)!=valid_count){

			printk(" seg id = %d type = %d valid = %d %d %d\n", (int)seg->seg_id, seg->seg_type, atomic_read(&seg->valid_count), valid_count, calc_meta_count(super, seg));
			BUG_ON(atomic_read(&seg->valid_count)<valid_count);
		}

		if(atomic_read(&seg->valid_count)==valid_count)
			return 1;
	}else{
		if(atomic_read(&seg->valid_dirty_count)==0)
			return 1;
	}
#else
	if(use_gc){
#ifdef NO_CLEAN_COPY
		if(!atomic_read(&seg->valid_dirty_count))
			return 1;
#else
#	ifdef HOT_DATA_COPY
		if(!atomic_read(&seg->valid_dirty_count)&&!atomic_read(&seg->hot_clean_count))
			return 1;
#	else
		if(!atomic_read(&seg->valid_dirty_count)&&!atomic_read(&seg->valid_clean_count))
			return 1;
#	endif 
#endif 
	}else{
		if(!atomic_read(&seg->valid_dirty_count))
			return 1;

	}
#endif 

	return 0;
}

#if 0 
void finish_kcopy_job(struct dmsrc_super *super, struct copy_job_group *cp_job_group){
	//struct migration_manager *migrate_mgr = &super->migrate_mgr;
	struct list_head *kcopy_list = &cp_job_group->cp_head;
	struct rambuffer *dst_rambuf = cp_job_group->dst_rambuf;
	struct copy_job *cp_job, *tmp;
	//unsigned long flags;
	//unsigned long f;
	int summary_count = 0;

	// summary
	list_for_each_entry_safe(cp_job, tmp, kcopy_list, cp_list){
		if(cp_job->src_mb==NULL&&cp_job_group->rw==WRITE){
			if(cp_job_group->gc){
				list_del(&cp_job->cp_list);
				set_bit(MB_SEAL, &cp_job->dst_mb->mb_flags);
				if(!cp_job->src_mb && atomic_dec_and_test(&dst_rambuf->ref_count)){
					//printk(" rambuf ref count %d \n", atomic_read(&dst_rambuf->ref_count));
					//printk(" finish kcopy job, release ram buffer \n");
					release_rambuffer(super, cp_job_group->dst_rambuf, cp_job_group->cache_type);
				}
				BUG_ON(atomic_read(&dst_rambuf->ref_count)<0);
				mempool_free(cp_job, super->migrate_mgr.copy_job_pool);
				summary_count++;
			}else{
				printk(" invalid ... cp_job->gc\n");
				BUG_ON(1);
			}
		}
	}

	// data 
	list_for_each_entry_safe(cp_job, tmp, kcopy_list, cp_list){
		struct segment_header *dst_seg = NULL, *src_seg = NULL;

		list_del(&cp_job->cp_list);
		if(!cp_job->src_mb){
			printk(" summary data ... \n");
			BUG_ON(1);
		}

		src_seg = get_seg_by_mb(super, cp_job->src_mb);
		dst_seg = cp_job_group->dst_seg;

		if(src_seg && atomic_dec_and_test(&src_seg->num_migios)){
			atomic_dec(&super->migrate_mgr.mig_inflights);

			if(is_empty_seg(super, src_seg)){
				//printk(" cleaned seg = %d %d \n", (int)src_seg->seg_id, atomic_read(&src_seg->num_migios));
				finalize_clean_seg(super, src_seg, 1);
			}else{
				schedule_timeout_interruptible(msecs_to_jiffies(10));
				printk(" need change state ... not empty seg ... seg %d %d \n", (int)src_seg->seg_id,
						atomic_read(&src_seg->valid_count));
			}
		}

		if(cp_job_group->gc){
			if(cp_job->src_mb){
				update_data_in_mb(super, cp_job->dst_mb->idx, dst_seg, dst_rambuf, dst_seg->seg_type, NULL);
				atomic_dec(&dst_seg->num_filling);
			}
			set_bit(MB_SEAL, &cp_job->dst_mb->mb_flags);
		}

		if(!cp_job_group->gc){
			free_single_page(super, cp_job->page);
			//printk(" free single page ... \n");
		}

		mempool_free(cp_job, super->migrate_mgr.copy_job_pool);
	}

	if(cp_job_group->gc)
		make_flush_invoke_job(super, cp_job_group->dst_seg, cp_job_group->dst_rambuf, cp_job_group->cache_type, 0, 0, 1);

	printk(" ### finish kcopy job ... \n");
	//schedule_timeout_interruptible(usecs_to_jiffies(500000));
	//list_for_each_entry_safe(mg_job, mg_tmp, &mig_local_head, mig_list) {
		//list_del(&mg_job->mig_list);
		//finish_mig_work(super, mg_job, 1);
	//	printk("finish kcopyd job inflight = %d id %d \n", atomic_read(&migrate_mgr->mig_inflights));
//	}
}
#endif

#if 0
void update_summary_block(struct dmsrc_super *super, struct copy_job_group *cp_job_group){
	//struct migration_manager *migrate_mgr = &super->migrate_mgr;
	struct list_head *kcopy_list = &cp_job_group->cp_head;
	struct copy_job *cp_job, *tmp;
	struct segment_header *src_seg = NULL;
	unsigned long f;
	int i;

	list_for_each_entry_safe(cp_job, tmp, kcopy_list, cp_list){
		if(cp_job->src_mb==NULL&&cp_job_group->rw==WRITE)
			continue;

		LOCK(super, f);
		src_seg = get_seg_by_mb(super, cp_job->src_mb);
		invalidate_previous_cache(super, src_seg, cp_job->src_mb);
		if(cp_job_group->gc){
			sector_t key = calc_cache_alignment(super, cp_job->src_mb->sector);
			if(key==~0){
				printk(" invalid sector = %d \n", (int)key);
				BUG_ON(1);
			}
			ht_register(super, key, cp_job->dst_mb);
			BUG_ON(cp_job->dst_mb->sector!=cp_job->src_mb->sector);
		}
		UNLOCK(super, f);
	}

	if(cp_job_group->gc){
		for(i = 0;i < NUM_SSD;i++){
			if(USE_ERASURE_PARITY(&super->param) && 
				i == get_parity_ssd(super, cp_job_group->dst_seg->seg_id) &&
				is_write_stripe(cp_job_group->dst_seg->seg_type))
				continue;

			prepare_chunk_summary(super, cp_job_group->dst_seg, cp_job_group->dst_rambuf->pages, i, 
					cp_job_group->dst_seg->seg_type);
		}
	}
}
#endif 

void generate_gc_write(struct dmsrc_super *super, struct copy_job_group *cp_job_group){
	struct list_head *kcopy_list = &cp_job_group->cp_head;
	struct copy_job *cp_job, *tmp;
	struct segment_header *src_seg = NULL;
	struct metablock *src_mb;
	unsigned long f;
	int cache_type;

	#if SWAN
		struct segment_allocator *seg_allocator = &super->seg_allocator;
		unsigned long flags;
	#endif
	
	//printk("[generate_gc_write]\n");

	list_for_each_entry_safe(cp_job, tmp, kcopy_list, cp_list){

		#if SWAN
		//do nothing yet
		#else
		cache_type = WCBUF;
		#endif

		src_mb = cp_job->src_mb;

		LOCK(super, f);

		src_seg = get_seg_by_mb(super, src_mb);


		#if SWAN 
		if(test_bit(MB_DIRTY, &src_mb->mb_flags) && test_bit(MB_VALID, &src_mb->mb_flags)){
		#else
		if(test_bit(MB_DIRTY, &src_mb->mb_flags)){
		#endif

			#if SWAN_SEG_GRANULARITY
			cache_type = cp_job->mb_gc_cache_type;
			#endif

			#if SWAN_MB_GRANULARITY
				
			// [KH] must be SWAN_MB_GRANULARITY 1 and SWAN_GRP_GRANULARITY 1
			unsigned int pageno = calc_cache_alignment(super, src_mb->sector);
			spin_lock_irqsave(&seg_allocator->alloc_lock, flags);

			if ( seg_allocator->gc_cold_write_col != (NUM_PHY_COL - 1) ) {
				spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);
				if ( !hot_filter_check(super, pageno) ) {
					//printk("[generate_gc_write] COLD pageno %u \n", pageno);   
					cache_type = SWAN_GC_CACHE_COLD;
				}else {	
					//printk("[generate_gc_write] HOT pageno %u \n", pageno);   
					cache_type = SWAN_GC_CACHE_HOT;
				} 
			} else {
				spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);
				#if SWAN_GRP_GRANULARITY
				//printk("[generate_gc_write] NO HOT/COLD SEGREGATION pageno %u \n", pageno);   
				cache_type = cp_job->mb_gc_cache_type;
				#endif
			}

			#endif

			#if SWAN_GRP_GRANULARITY
				//printk("[generate_gc_write] NO HOT/COLD SEGREGATION pageno %u \n", pageno);   
			cache_type = cp_job->mb_gc_cache_type;
			#endif

			if(should_need_refresh_seg(super, cache_type)){
RETRY:
				if(can_get_free_segment(super, cache_type, 1)){
					alloc_new_segment(super, cache_type, false);
				}else{
					UNLOCK(super, f);
					while(1){
						printk("[generate_gc_write] gc_write: no more free segs %d , ram buf ...%d \n", atomic_read(&super->seg_allocator.seg_alloc_count), 
							atomic_read(&super->segbuf_mgr.inactive_page_count));
						schedule_timeout_interruptible(usecs_to_jiffies(100));
						printk("[generate_gc_write] @@@@@ interrupt #1 @@@@@\n");
						LOCK(super, f);
						goto RETRY;
					}
				}
			}

			invalidate_previous_cache(super, src_seg, src_mb);

			wp_update(super, WRITE, REQ_CATEGORY_GC);
		#ifdef TRACE_GC
			atomic64_inc(&super->wstat.gc_io_cnt[WRITE]);
		#endif
			if(!test_bit(MB_DIRTY, &src_mb->mb_flags)){
				printk(" WARN: No clean data copy ... \n");
			}

			process_write_request(super, NULL, cp_job->page->pl->page, src_mb->sector, 
					test_bit(MB_DIRTY, &src_mb->mb_flags), f, cache_type, src_mb->checksum);
		
		#if SWAN
		}else if (test_bit(MB_DIRTY, &src_mb->mb_flags) && !test_bit(MB_VALID, &src_mb->mb_flags)){
	
			//printk("[generate_gc_write] This src_mb is invalided by write_req \n");
			UNLOCK(super, f);
		
		#endif

		}else{
			printk("[generate_gc_write] @@ WARN @@ #2 CHECK CODE HERE !\n");
			struct cache_manager *clean_cache = super->clean_dram_cache_manager;
			struct lru_node *ln = NULL;
			unsigned long lru_flags; 

			spin_lock_irqsave(&clean_cache->lock, lru_flags);
			ln = CACHE_SEARCH(clean_cache, src_mb->sector);
			if(ln){
				atomic_set(&ln->sealed, 1);
				atomic_set(&ln->locked, 0);
				atomic_inc(&clean_cache->cm_sealed_count);
			}else{
				printk(" WARN: No Data ... %s \n", __FUNCTION__);
			}
			spin_unlock_irqrestore(&clean_cache->lock, lru_flags);

			UNLOCK(super, f);
		}

		if(atomic_dec_and_test(&src_seg->num_migios)){
			if(is_empty_seg(super, src_seg, 1)){
				atomic_dec(&super->migrate_mgr.mig_inflights);
				finalize_clean_seg(super, src_seg, 1);
			}else{
				printk(" need change state ... not empty seg ... seg %d (valid: total %d, clean %d, dirty %d, seg_migrating = %d \n", (int)src_seg->seg_id,
						atomic_read(&src_seg->valid_count), 
						atomic_read(&src_seg->valid_clean_count), 
						atomic_read(&src_seg->valid_dirty_count), 
						test_bit(SEG_MIGRATING, &src_seg->flags));
				schedule_timeout_interruptible(msecs_to_jiffies(1000));
				printk("[generate_gc_write] @@@@@ interrupt #2 @@@@@\n");
			}
		}

		list_del(&cp_job->cp_list);
		free_single_page(super, cp_job->page);
		atomic_dec(&super->migrate_mgr.copy_job_count);
		mempool_free(cp_job, super->migrate_mgr.copy_job_pool);
	}
}

void generate_destage_write(struct dmsrc_super *super, struct copy_job_group *cp_job_group){
	struct list_head *kcopy_list = &cp_job_group->cp_head;
	struct copy_job *cp_job, *tmp;
	struct segment_header *src_seg = NULL;
	unsigned long f;

	list_for_each_entry_safe(cp_job, tmp, kcopy_list, cp_list){

		LOCK(super, f);
		src_seg = get_seg_by_mb(super, cp_job->src_mb);
		invalidate_previous_cache(super, src_seg, cp_job->src_mb);
		UNLOCK(super, f);

		if(atomic_dec_and_test(&src_seg->num_migios)){
			if(is_empty_seg(super, src_seg, 0)){
				atomic_dec(&super->migrate_mgr.mig_inflights);
				finalize_clean_seg(super, src_seg, 1);
				//printk(" finalize SSD to Disk cleaning seg = %d \n", (int)src_seg->seg_id);
			}else{
				printk(" WARN: generate_desage_write: need change state ... not empty seg ... seg %d %d, seg_migrating = %d \n", (int)src_seg->seg_id,
						atomic_read(&src_seg->valid_count), test_bit(SEG_MIGRATING, &src_seg->flags));
				schedule_timeout_interruptible(msecs_to_jiffies(1000));
			}
		}

		list_del(&cp_job->cp_list);
		free_single_page(super, cp_job->page);
		atomic_dec(&super->migrate_mgr.copy_job_count);
		mempool_free(cp_job, super->migrate_mgr.copy_job_pool);
	}
}

void do_mig_worker(struct work_struct *work){
	struct migration_manager *migrate_mgr = container_of(work, struct migration_manager, mig_work);
	struct dmsrc_super *super = migrate_mgr->super;//	unsigned long flags;
	struct copy_job_group *cp_job_grp, *cp_tmp;
	struct list_head local_head;
	//struct segment_header *src_seg;
	unsigned long flags;

	INIT_LIST_HEAD(&local_head);

	spin_lock_irqsave(&migrate_mgr->group_queue_lock, flags);
	list_for_each_entry_safe(cp_job_grp, cp_tmp, &migrate_mgr->group_queue, group_list) {
		list_move_tail(&cp_job_grp->group_list, &local_head);
	}
	spin_unlock_irqrestore(&migrate_mgr->group_queue_lock, flags);

	list_for_each_entry_safe(cp_job_grp, cp_tmp, &local_head, group_list) {
		list_del(&cp_job_grp->group_list);

		if(cp_job_grp->gc){
			generate_gc_write(super, cp_job_grp);
			mempool_free(cp_job_grp, migrate_mgr->group_job_pool);
			atomic_dec(&migrate_mgr->group_job_count);
		}else{
			if(cp_job_grp->rw==READ){
				cp_job_grp->rw = WRITE;
				flush_kcopy_job(super, cp_job_grp);
			}else{
				generate_destage_write(super, cp_job_grp);
				mempool_free(cp_job_grp, migrate_mgr->group_job_pool);
				atomic_dec(&migrate_mgr->group_job_count);
			}
		}

#if 0
		if(cp_job_grp->rw==READ){
			cp_job_grp->rw = WRITE;
			update_summary_block(super, cp_job_grp);
			flush_kcopy_job(super, cp_job_grp, cp_job_grp->cache_type, cp_job_grp->cur_kcopyd);
		}else{
			finish_kcopy_job(super, cp_job_grp);
			mempool_free(cp_job_grp, migrate_mgr->group_job_pool);
		}
#endif
	}

}

#if 0 
static void copy_complete(int read_err, unsigned long write_err, void *__context)
{
	struct copy_job *cp_job = __context;
	struct mig_job *mg_job = cp_job->mg_job;
	struct dmsrc_super *super = mg_job->wb;
	unsigned long flags;

	if (read_err || write_err)
		mg_job->error = 1;

	spin_lock_irqsave(&super->mig_queue_lock, flags);
	list_add_tail(&cp_job->cp_list, &super->copy_queue);
	spin_unlock_irqrestore(&super->mig_queue_lock, flags);

	queue_work(super->mig_wq, &wb->mig_work);
	//printk(" copy complete ... \n");
}
#endif 

#if 0
static void refresh_gc_buf(struct dmsrc_super *super, int cache_type, unsigned long *f){
	struct segment_header *dst_seg;
	struct rambuffer *rambuf;

	//tmp = super->current_seg[cache_type];
	dst_seg = super->current_seg[cache_type];
	rambuf = super->segbuf_mgr.current_rambuf[cache_type];

	UNLOCK(super, *f);

	printk(" refresh gc invoke job seg id = %d, length = %d \n", 
			(int)dst_seg->seg_id, atomic_read(&dst_seg->length)); 
#if 0 
	make_flush_invoke_job(super, dst_seg, rambuf, cache_type, 0, 0);
#endif
	//_build_summary_job(super, dst_seg, rambuf, 1);

	wait_rambuf_event(super, cache_type);

	printk(" rambuf gc buf exits.. \n");

	BUG_ON(!atomic_read(&super->segbuf_mgr.inactive_count));
	BUG_ON(!atomic_read(&super->seg_allocator.reserve_count));

	LOCK(super, *f);

	while(atomic_read(&super->segbuf_mgr.inactive_page_count)<STRIPE_SZ){
		UNLOCK(super, *f);
		schedule_timeout_interruptible(usecs_to_jiffies(10));
		//printk(" no rambuf gc buf refresh .. \n");
		LOCK(super, *f);
	}

	alloc_new_segment(super, cache_type, true);
	rambuf = super->segbuf_mgr.current_rambuf[cache_type];
	printk(" refresh GC ref count = %d \n", atomic_read(&rambuf->ref_count));
}
#endif


void gc_job_done(unsigned long error, void *context)
{
	struct copy_job_group *cp_job_group = context;
	struct dmsrc_super *super = cp_job_group->super;
	struct migration_manager *migrate_mgr = &super->migrate_mgr;
	unsigned long flags;

	if(atomic_dec_and_test(&cp_job_group->cp_job_count)){
		spin_lock_irqsave(&migrate_mgr->group_queue_lock, flags);
		list_add_tail(&cp_job_group->group_list, &migrate_mgr->group_queue);
		spin_unlock_irqrestore(&migrate_mgr->group_queue_lock, flags);
		queue_work(migrate_mgr->mig_wq, &migrate_mgr->mig_work);
	}
}


void _flush_kcopy_job(struct dmsrc_super *super, struct copy_job_group *cp_job_group, struct copy_job *cp_job){
	struct dm_io_request io_req;

	io_req.mem.type = DM_IO_PAGE_LIST;
	io_req.mem.ptr.pl= cp_job->page->pl;
	io_req.mem.offset = 0;
	io_req.notify.fn = gc_job_done;
	io_req.notify.context = cp_job_group;
	io_req.client = super->io_client;

	if(cp_job_group->rw==READ){
		wp_update(super, READ, REQ_CATEGORY_GC);
		io_req.bi_rw = READ;
		dmsrc_io(&io_req, 1, &cp_job->src_region, NULL);
#ifdef TRACE_GC
		atomic64_inc(&super->wstat.gc_io_cnt[READ]);
#endif
	}else{
		io_req.bi_rw = WRITE;
		atomic_inc(&super->wstat.total_migration);
		dmsrc_io(&io_req, cp_job->dst_count, cp_job->dst_region, NULL);
	}
}


void flush_kcopy_job(struct dmsrc_super *super, struct copy_job_group *cp_job_group){
	struct list_head *kcopy_list = &cp_job_group->cp_head;
	struct copy_job *cp_job, *tmp;
	struct blk_plug plug;
	int count = 0;

	list_for_each_entry_safe(cp_job, tmp, kcopy_list, cp_list){
		atomic_inc(&cp_job_group->cp_job_count);
		count++;
	}

	if(!atomic_read(&cp_job_group->cp_job_count)){
		BUG_ON(1);
	}

	blk_start_plug(&plug);
	list_for_each_entry_safe(cp_job, tmp, kcopy_list, cp_list){
		_flush_kcopy_job(super, cp_job_group, cp_job);
	}
	blk_finish_plug(&plug);
}

void *alloc_copy_job_group(struct dmsrc_super *super, int cache_type){
	struct copy_job_group *cp_job_group;
	struct migration_manager *migrate_mgr = &super->migrate_mgr;

	cp_job_group = mempool_alloc(migrate_mgr->group_job_pool,GFP_KERNEL);
	if(!cp_job_group){
		printk(" cannot alloc memory via kmalloc \n");
		BUG_ON(!cp_job_group);
	}

	atomic_inc(&migrate_mgr->group_job_count);

	INIT_LIST_HEAD(&cp_job_group->cp_head);
	atomic_set(&cp_job_group->cp_job_count, 0);
	cp_job_group->rw = READ;
	cp_job_group->cache_type = cache_type;
	cp_job_group->super = super;
	cp_job_group->seq = atomic_read(&migrate_mgr->group_job_seq);
	atomic_inc(&migrate_mgr->group_job_seq);

	return cp_job_group;
}

int check_validness(struct dmsrc_super *super, struct segment_header *seg, struct metablock *victim_mb, int use_gc){
	u8 dirty_bits = 0;

	if(use_gc){
#ifdef NO_CLEAN_COPY
		if(atomic_read_mb_validness(seg, victim_mb) && atomic_read_mb_dirtiness(seg, victim_mb))
			dirty_bits = 1;
#else
#	ifdef HOT_DATA_COPY
		if(atomic_read_mb_dirtiness(seg, victim_mb)){
			if(atomic_read_mb_validness(seg, victim_mb))
				dirty_bits = 1;
		}else{ // clean  
			if(atomic_read_mb_validness(seg, victim_mb)&&test_bit(MB_HIT, &victim_mb->mb_flags))
				dirty_bits = 1;
		}
#	else
		dirty_bits = atomic_read_mb_validness(seg, victim_mb);
#	endif 
#endif 
	}else{
		if(atomic_read_mb_validness(seg, victim_mb) && atomic_read_mb_dirtiness(seg, victim_mb))
			dirty_bits = 1;
	}

	return dirty_bits;
}

int calc_meta_count(struct dmsrc_super *super, struct segment_header *seg){
	struct metablock *mb;
	int valid_count = 0;
	int summary_count = 0;
	int parity_count = 0;
	int i;

	for (i = 0; i < STRIPE_SZ; i++) {
		mb = get_mb(super, seg->seg_id, i);
		if(test_bit(MB_PARITY, &mb->mb_flags)){
			valid_count++;
			parity_count++;
		}
		if(test_bit(MB_SUMMARY, &mb->mb_flags)){
			valid_count++;
			summary_count++;
		}
	}

	if(valid_count!=get_metadata_count(super, seg->seg_type)){
		printk(" invalid meta count: seg = %d total = %d, parity = %d, summary = %d \n", 
				(int)seg->seg_id, valid_count, parity_count, summary_count);
	}

	return valid_count;
}

int calc_valid_count(struct dmsrc_super *super, struct segment_header *seg, int use_gc){
	struct metablock *mb;
	int valid_count = 0;
	int i;
	int hot_gc;
	u8 dirty_bits;

	for (i = 0; i < STRIPE_SZ; i++) {

		hot_gc = 0;

		mb = get_mb(super, seg->seg_id, i);

		#if 0

		if(mb->sector==~0)
			continue;

		if (test_bit(MB_VALID, &mb->mb_flags)) {
			valid_count++;	
		}

		#else
		dirty_bits = check_validness(super, seg, mb, use_gc) ; 
		if(mb->sector==~0)
			continue;

		if(dirty_bits) {
			#if SWAN_GRP_GRANULARITY
			
			#endif
			
			#if SWAN_SEG_GRANULARITY
			
			#endif
			
			valid_count++;
		}
		#endif
	}

	if(use_gc){
#ifdef NO_CLEAN_COPY
		if(valid_count!= atomic_read(&seg->valid_dirty_count)){
			printk(" calc valid count: gc Invalid valid count .. %d %d \n",
			valid_count, atomic_read(&seg->valid_dirty_count));
		}
#else
#	ifdef HOT_DATA_COPY
		if(valid_count!= atomic_read(&seg->valid_dirty_count)+atomic_read(&seg->hot_clean_count)){
			printk(" calc valid count: gc Invalid valid count .. %d %d \n",
			valid_count, atomic_read(&seg->valid_dirty_count)+atomic_read(&seg->hot_clean_count));
		}
#	else
		if(valid_count!= atomic_read(&seg->valid_dirty_count)+atomic_read(&seg->valid_clean_count)){
			printk(" calc valid count: gc Invalid valid count .. %d %d \n",
			valid_count, atomic_read(&seg->valid_dirty_count)+atomic_read(&seg->valid_clean_count));
		}
#	endif 
#endif 
	}else{
		if(valid_count!=atomic_read(&seg->valid_dirty_count)){
			printk(" calc valid count: destage Invalid valid count .. %d %d \n",
			valid_count, atomic_read(&seg->valid_dirty_count));
		}
	}
		
	return valid_count;
}

#if 0
void make_destage_job(struct dmsrc_super *super, struct copy_job *cp_job, struct metablock *victim_mb, int dst_count){

	cp_job->dst_mb = NULL;
	cp_job->dst_region[dst_count].bdev = super->dev_info.origin_dev->bdev;
	cp_job->dst_region[dst_count].sector = victim_mb->sector;
	cp_job->dst_region[dst_count].count = SRC_SECTORS_PER_PAGE;
	cp_job->page = alloc_single_page(super);

	cp_job->dst_count = dst_count;
	dst_count++;

	atomic64_inc(&super->wstat.destage_io_count);

	BUG_ON(victim_mb->sector>=super->dev_info.origin_sectors);
}
#endif 

#if 0
struct metablock *alloc_mb_gc(struct dmsrc_super *super, struct metablock *victim_mb, int cache_type){
	struct segment_header *dst_seg;
	struct metablock *dst_mb;
	unsigned long f;
	sector_t key;
	u32 update_mb_idx;
	u32 count;
	u32 total_count = 0;
	bool chunk_summary = false;

	key = victim_mb->sector;

	count = ma_get_count(super, cache_type);
	if(need_refresh_segment(super, cache_type, count)){
		printk(" need refresh seg gc .. \n");
		BUG_ON(1);
#if 0
		if(!list_empty(&cp_job_group->cp_head)){
			flush_kcopy_job(super, cp_job_group, cache_type, cur_kcopyd);
			cp_job_group = alloc_copy_job_group(super, cache_type, cur_kcopyd );
	//		BUG_ON(1);
		}
#endif
	}

	LOCK(super, f);

	count = ma_get_count(super, cache_type);
	if (need_refresh_segment(super, cache_type, count)){
		printk(" need refresh seg gc .. \n");
		BUG_ON(1);
		//printk(" refresh gc buf ... type = %d \n", cache_type);
		refresh_gc_buf(super, cache_type, &f);
	}

	if(super->param.gc_with_dirtysync)
		dst_mb = alloc_mb_data(super, key, cache_type, true, &total_count);
	else
		dst_mb = alloc_mb_data(super, key, cache_type, !test_bit(MB_DIRTY, &victim_mb->mb_flags), &total_count);

	dst_seg = super->current_seg[cache_type];
	dst_seg->seg_type = cache_type;

	update_mb_idx = dst_mb->idx;

	if(need_chunk_summary(super, update_mb_idx+NUM_SUMMARY)){
		u32 summary_idx = data_to_summary_idx(super, update_mb_idx);
		alloc_mb_summary(super, dst_seg, cache_type, summary_idx, &total_count);
		initialize_mb_summary(super, dst_seg, cache_type, summary_idx);
		chunk_summary = true;
	}

	UNLOCK(super, f);

	clear_bit(MB_SEAL, &dst_mb->mb_flags);

	return dst_mb;
}
#endif

#if 0 
struct copy_job_group *make_gc_job(struct dmsrc_super *super,
									struct copy_job_group *cp_job_group,
									struct copy_job *cp_job,
									struct metablock *victim_mb, 
									struct metablock *dst_mb, 
									int *dst_count,
									int cache_type)
{
	struct segment_header *dst_seg = get_seg_by_mb(super, dst_mb);
	u32 update_mb_idx = dst_mb->idx;

	cp_job->dst_mb = dst_mb;
	//cp_job->dst_rambuf = super->segbuf_mgr.current_rambuf[cache_type];
	cp_job->dst_region[*dst_count].bdev = get_bdev(super, update_mb_idx);
	cp_job->dst_region[*dst_count].sector = get_sector(super, dst_seg->seg_id, update_mb_idx); 
	cp_job->dst_region[*dst_count].count = SRC_SECTORS_PER_PAGE;
	//cp_job->dst_seg = dst_seg;
	cp_job->page = cp_job_group->dst_rambuf->pages[(update_mb_idx%STRIPE_SZ)];
	(*dst_count)++;

	if(victim_mb && super->param.gc_with_dirtysync && test_bit(MB_DIRTY, &victim_mb->mb_flags)){
		cp_job->dst_region[*dst_count].bdev = super->dev_info.origin_dev->bdev;
		cp_job->dst_region[*dst_count].sector = victim_mb->sector;
		cp_job->dst_region[*dst_count].count = SRC_SECTORS_PER_PAGE; 
		(*dst_count)++;
	}

	cp_job->dst_count = *dst_count;

	atomic64_inc(&super->wstat.gc_io_count);

	//return cp_job_group;
	return NULL;
}
#endif 

#if 0
int make_gc_summary_job(struct dmsrc_super *super,
									struct copy_job_group *cp_job_group,
									struct metablock *dst_mb, 
									int cache_type)
{
	struct migration_manager *migrate_mgr = &super->migrate_mgr;
	int dst_count = 0;
	int i = 0;

	if(need_chunk_summary(super, dst_mb->idx+NUM_SUMMARY)){
		//printk(" GC: prepare and write summary data \n");
		for(i = 0;i < NUM_SUMMARY;i++){
			struct metablock *summary_mb;
			struct copy_job *summary_job;
			u32 idx = dst_mb->idx + 1 + i;

			summary_mb = mb_at(super, idx);
			//printk(" summary idx = %d \n", (int)summary_mb->idx);
			summary_job = mempool_alloc(migrate_mgr->copy_job_pool, GFP_NOIO);
			BUG_ON(!summary_job);
			summary_job->src_mb = NULL;
			dst_count = 0;
			make_gc_job(super, cp_job_group, summary_job, NULL, summary_mb, &dst_count, cache_type);

			summary_job->dst_count = dst_count;

			list_add_tail(&summary_job->cp_list, &cp_job_group->cp_head);
		}
	}

	return i;
}
#endif


void release_victim_seg(struct dmsrc_super *super, struct migration_manager *migrate_mgr){
	struct segment_header *seg, *tmp;
	unsigned long f;

	#if SWAN_Q2
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct col_queue *col_Q;
	u32 phy_col_id;
	#endif


	if(atomic_read(&super->migrate_mgr.migrate_queue_count))
		printk(" Release victim seg = %d \n", atomic_read(&super->migrate_mgr.migrate_queue_count));

	list_for_each_entry_safe(seg, tmp, &migrate_mgr->migrate_queue, migrate_list) {
		if(migrate_mgr->gc_cur_seg != seg){
			list_del(&seg->migrate_list);
			atomic_dec(&super->migrate_mgr.migrate_queue_count);
			atomic_dec(&migrate_mgr->mig_inflights);

			printk(" release victim seg, mig inflight = %d\n", atomic_read(&migrate_mgr->mig_inflights));
			
			#if SWAN_Q2
			phy_col_id = seg->group->phy_col_id;
			col_Q = &seg_allocator->col_queue_array[phy_col_id];
			move_seg_migrate_to_sealed_queue(super, seg, col_Q);
			#else
			move_seg_migrate_to_sealed_queue(super, seg);
			#endif
			LOCK(super, f);
			clear_bit(SEG_MIGRATING, &seg->flags);
			UNLOCK(super, f);
		}else{
			printk(" same .. \n");
			BUG_ON(1);
		}
	}

	if(migrate_mgr->gc_cur_seg){
		seg = migrate_mgr->gc_cur_seg;

		while(atomic_read(&seg->num_migios)){
			schedule_timeout_interruptible(msecs_to_jiffies(2000));
			printk(" wait num migios ... \n");
			printk("[release_victim_seg] @@@@@ interrupt #2 @@@@@\n");
		}

		//if(is_empty_seg(super, seg)){
		//	printk(" current gc seg is empty ... \n");
		//	BUG_ON(1);
		//}

		if(!test_bit(SEG_MIGRATING, &seg->flags)){
			printk(" invalid state .. \n");
			BUG_ON(1);
		}

		printk(" relase gc cur seg = %d \n", (int)seg->seg_id);
		BUG_ON(1);

		#if SWAN_Q2
		phy_col_id = seg->group->phy_col_id;
		col_Q = &seg_allocator->col_queue_array[phy_col_id];
		move_seg_migrate_to_sealed_queue(super, seg, col_Q);
		#else
		move_seg_migrate_to_sealed_queue(super, seg);
		#endif

		LOCK(super, f);
		clear_bit(SEG_MIGRATING, &seg->flags);
		UNLOCK(super, f);

		migrate_mgr->gc_cur_seg = NULL;
		migrate_mgr->gc_cur_offset = 0;
	}

	//printk(" End Release victim seg = %d \n", atomic_read(&super->migrate_mgr.migrate_queue_count));
}

void get_next_victim_seg(struct dmsrc_super *super, struct migration_manager *migrate_mgr){
	if(atomic_read(&super->migrate_mgr.migrate_queue_count))
		migrate_mgr->gc_cur_seg = list_first_entry(&migrate_mgr->migrate_queue, struct segment_header, migrate_list); 
	else
		migrate_mgr->gc_cur_seg = NULL;

	migrate_mgr->gc_cur_offset = 0;
	migrate_mgr->gc_alloc_count = 0;
	if(migrate_mgr->gc_cur_seg){
		u32 valid_count;
		list_del(&migrate_mgr->gc_cur_seg->migrate_list);
		atomic_dec(&super->migrate_mgr.migrate_queue_count);
		atomic_set(&migrate_mgr->gc_cur_seg->num_migios, 0);

		valid_count = calc_valid_count(super, migrate_mgr->gc_cur_seg, 1);
		atomic64_add(valid_count*100/STRIPE_SZ, &super->wstat.victim_util);
		atomic64_inc(&super->wstat.victim_count);
		///printk(" get next victim seg = %d \n", (int)migrate_mgr->gc_cur_seg->seg_id);
	}else{
		//printk(" get next victim seg = Unavailable \n");
	}
}

struct metablock *get_next_victim_mb(struct dmsrc_super *super, 
									struct migration_manager *migrate_mgr, 
									int use_gc){
	struct segment_header *seg = migrate_mgr->gc_cur_seg;
	struct metablock *victim_mb = NULL;
	int i;
	
	if(migrate_mgr->gc_cur_seg==NULL)
		return NULL;

	if(migrate_mgr->gc_cur_offset >= STRIPE_SZ){
		migrate_mgr->gc_cur_seg = NULL;
		if(atomic_read(&seg->num_migios)==0){
			if(!atomic_read(&seg->valid_dirty_count)){
				finalize_clean_seg(super, seg, 1);
			}else{
				printk(" WARN: clean empty seg .. \n"); 
			}
		}
		return NULL;
	}

	for(i = migrate_mgr->gc_cur_offset;i < STRIPE_SZ;i++){

		 victim_mb = get_mb(super, seg->seg_id, i);

		if(!check_validness(super, seg, victim_mb, use_gc))
			continue;

		if(victim_mb->sector==~0)
			continue;

		migrate_mgr->gc_cur_offset = i + 1;
		migrate_mgr->gc_alloc_count++;
		return victim_mb;
	}
	if(atomic_read(&seg->num_migios)==0){
		if(!atomic_read(&seg->valid_dirty_count)){
			finalize_clean_seg(super, seg, 1);
		}else{
			printk(" WARN: clean empty seg .. \n"); 
		}
	}

	return NULL;

}

#if 0 
int make_kcopyd_job(struct dmsrc_super *super, struct segment_header *seg, struct mig_job *mg_job, int use_gc){
	struct copy_job_group *cp_job_group;
	struct copy_job *cp_job;
	int i, issue_count = 0;
	int cache_type;
	struct migration_manager *migrate_mgr = &super->migrate_mgr;

	if(seg->seg_type==WCBUF || seg->seg_type==WHBUF)
		cache_type = GWBUF;
	else
		cache_type = GRBUF;

	if(super->param.gc_with_dirtysync)
		cache_type = GRBUF;

	issue_count = calc_valid_count(super, seg, use_gc);
	if(!issue_count){
		printk(" Invalid issue count = %d \n", issue_count);
		BUG_ON(1);
		return issue_count;
	}

	atomic64_add(issue_count, &(mg_job->count));

	cp_job_group = mg_job->cp_job_group;

	for (i = 0; i < STRIPE_SZ; i++) {
		struct metablock *victim_mb = get_mb(super, seg->seg_id, i);
		int dst_count = 0;
		int hot_gc = 0;

		if(!check_validness(super, seg, victim_mb, use_gc, &hot_gc))
			continue;

		if(victim_mb->sector==~0)
			continue;

		count = ma_get_count(super, cache_type);
		if(need_refresh_segment(super, cache_type, count)){
			printk(" no block in gc buf .. \n");
			BUG_ON(0);
		}
		atomic_inc(&seg->num_migios);

		cp_job = mempool_alloc(migrate_mgr->copy_job_pool, GFP_NOIO);
		while(!cp_job){
			printk(" copy job pool error \n");
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			cp_job = mempool_alloc(migrate_mgr->copy_job_pool, GFP_NOIO);
		}

		cp_job->mg_job = mg_job;
		cp_job->gc = hot_gc;
		cp_job->src_mb = victim_mb;
		cp_job->src_region.bdev = get_bdev(super, i);
		cp_job->src_region.sector = get_sector(super, seg->seg_id, i);
		cp_job->src_region.count = SRC_SECTORS_PER_PAGE;

		dst_count = 0;
		if(!hot_gc){
			make_destage_job(super, cp_job, victim_mb, dst_count);
			dst_count++;
		}else{
			struct metablock *dst_mb = alloc_mb_gc(super, victim_mb, cache_type);
			make_gc_job(super, cp_job, victim_mb, dst_mb, &dst_count, cache_type);
		}

		cp_job->dst_count = dst_count;
		list_add_tail(&cp_job->cp_list, &cp_job_group->cp_head);
	}


//	printk(" 2 make kcopyd job seg_id = %d type = %d, valid count = %d \n", (int)seg->seg_id, seg->seg_type, issue_count);

	return issue_count;
}
#endif

#if 0
void try_reserve_segment_for_gc(struct dmsrc_super *super){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	release_reserve_segs(super);
	while(1){
		int reserve_done;
		while(atomic_read(&seg_allocator->alloc_count)<MIN_FREE)
			schedule_timeout_interruptible(usecs_to_jiffies(10));

		BUG_ON(atomic_read(&seg_allocator->alloc_count)<MIN_FREE);

		if(atomic_read(&seg_allocator->alloc_count)<5)
			reserve_done = reserve_reserve_segs(super, atomic_read(&seg_allocator->alloc_count));
		else
			reserve_done = reserve_reserve_segs(super, 5);

		if(reserve_done){
			break;
		}
		schedule_timeout_interruptible(usecs_to_jiffies(10));
		printk(" It cannot reserve segments for GC free = %d \n", (int)atomic_read(&super->seg_allocator.alloc_count));
	}
}
#endif

#if 0 
int select_empty_segs(struct dmsrc_super *super){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct list_head *sealed_queue = &seg_allocator->seg_sealed_queue;
	struct segment_header *seg;
	unsigned long flags;
	int recount = 0;
	int loop = 0;

	spin_lock_irqsave(&seg_allocator->alloc_lock, flags);

	while(!list_empty(sealed_queue)){
		seg	 = (struct segment_header *)
			list_entry(sealed_queue->prev, struct segment_header, alloc_list);

		if(is_empty_seg(super, seg)){
			list_add_tail(&seg->migrate_list, &super->migrate_mgr.migrate_queue);
			atomic_inc(&super->migrate_mgr.migrate_queue_count);
			move_seg_sealed_to_migrate_queue(super, seg, 0);
			recount++;
		}

		if(recount>=MIGRATE_HIGHWATER)
			break;

		if(++loop>=seg_allocator->seg_sealed_count)
			break;
	}

	spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);

	//printk(" empty segs = %d \n", recount);

	return recount;
}

void clean_empty_seg(struct dmsrc_super *super, int use_gc){
	struct migration_manager *migrate_mgr = &super->migrate_mgr;
	struct segment_header *seg, *tmp;
	int issue_count = 0;
	int bug_count = 0;

	if(!select_empty_segs(super))
		return;

	list_for_each_entry_safe(seg, tmp, &migrate_mgr->migrate_queue, migrate_list) {
		unsigned long f;

		if(test_bit(SEG_MIGRATING, &seg->flags)){
			printk(" seg is migrating ... seg %d \n", (int)seg->seg_id);
			BUG_ON(1);
		}
		LOCK(super, f);
		set_bit(SEG_MIGRATING, &seg->flags);
		atomic_set(&seg->num_migios, 0);
		UNLOCK(super, f);

		while(atomic_read(&seg->num_read_inflight) || 
			 atomic_read(&seg->num_filling) ) 
		{
			schedule_timeout_interruptible(usecs_to_jiffies(500000));
			printk(" seg = %d  inflight %d filling = %d\n", 
					(int)seg->seg_id,
					atomic_read(&seg->num_read_inflight)
					,atomic_read(&seg->num_filling)); 

			schedule_timeout_interruptible(usecs_to_jiffies(100));
		}

		issue_count = calc_valid_count(super, seg, use_gc);
		if(!issue_count){
			if(is_empty_seg(super, seg)){
				atomic_inc(&super->wstat.gc_empty_count);
				//printk(" cleaning empty segments = %d \n", (int)seg->seg_id);
				finalize_clean_seg(super, seg, 1);
				list_del(&seg->migrate_list);
				atomic_dec(&migrate_mgr->migrate_queue_count);
			}else{
				printk(" no issue count but not empty seg ... \n");
				BUG_ON(1);
			}
		}else{
			printk(" seg is not empty \n");
			BUG_ON(1);
			atomic_inc(&migrate_mgr->mig_inflights);
		}

		//atomic64_inc(&super->wstat.victim_count);
		//atomic64_add(issue_count*100/STRIPE_SZ, &super->wstat.victim_util);

		BUG_ON(++bug_count==100000000);
	}

}
#endif

#if SWAN
int __issue_mig_kcopyd(struct dmsrc_super *super, struct copy_job_group *cp_job_group, int cache_type, int use_gc, int num_copy_block, int *total_cnt){
#else
int __issue_mig_kcopyd(struct dmsrc_super *super, struct copy_job_group *cp_job_group, int cache_type, int use_gc, int num_copy_block){
#endif


	struct migration_manager *migrate_mgr = &super->migrate_mgr;
	int total_count = 0;
	//int no_rambuffer_count = 0;

	while(1){
		struct metablock *victim_mb = NULL;
		struct segment_header *victim_seg = NULL;
		struct copy_job *cp_job;

		while(!victim_mb){
			victim_mb = get_next_victim_mb(super, &super->migrate_mgr, use_gc);
			if(!victim_mb){
				get_next_victim_seg(super, &super->migrate_mgr);
				if(!migrate_mgr->gc_cur_seg){
					//printk(" no victim segs 2... \n");
					goto NO_VICTIM;
				}
			}
		}

		victim_seg = get_seg_by_mb(super, victim_mb);
		
		#if SWAN
		if(!check_validness(super, victim_seg, victim_mb, use_gc) || victim_mb->sector==~0){
			*total_cnt = *total_cnt - 1;
			continue;
		}
		#else

		if(!check_validness(super, victim_seg, victim_mb, use_gc) || victim_mb->sector==~0){
			printk(" invalid victim ... \n");
			BUG_ON(1);
			continue;
		}
		#endif

		if(!test_bit(MB_DIRTY, &victim_mb->mb_flags)){ // valid clean case
		//if(!test_bit(MB_DIRTY, &victim_mb->mb_flags) && 
		//	test_bit(MB_HIT, &victim_mb->mb_flags)){ 
			struct cache_manager *clean_cache = super->clean_dram_cache_manager;
			struct lru_node *ln = NULL;
			unsigned long lru_flags; 
			unsigned long f;
			int sealed = 0, locked = 0, cn_alloc_by_gc = 0;
			int alloc = 0;
			//int free = 0;

			LOCK(super, f);
			spin_lock_irqsave(&clean_cache->lock, lru_flags);
			ln = CACHE_SEARCH(clean_cache, victim_mb->sector);
			//free = clean_cache->cm_free;
			if(ln){
				printk(" WARN: clean data hit n clean ram during GC \n");
			}

#ifdef HOT_DATA_COPY
			if(!ln && test_bit(MB_HIT, &victim_mb->mb_flags)){
#else
			if(!ln){
#endif
				if(clean_cache->cm_free){
					ln = CACHE_ALLOC(clean_cache, ln, victim_mb->sector);
					CACHE_INSERT(clean_cache, ln);
					atomic_set(&ln->sealed, 0);
					atomic_set(&ln->locked, 0);
					ln->cn_alloc_by_gc = 1;
					alloc = 1;
				}
			}

			invalidate_previous_cache(super, victim_seg, victim_mb);

			if(ln){
				sealed = atomic_read(&ln->sealed);
				locked = atomic_read(&ln->locked);
				cn_alloc_by_gc = ln->cn_alloc_by_gc;
			}

			spin_unlock_irqrestore(&clean_cache->lock, lru_flags);
			UNLOCK(super, f);

			if(!ln){
			//	printk(" WARN: No RAM buffer (free = %d) for migrating clean data ... %s \n", 
			//			free, __FUNCTION__);
				//no_rambuffer_count++;
				continue;
			}else{
				if(!alloc){
					printk(" WARN: dram hit ... during GC  sector = %d sealed = %d locked = %d, alloc by gc = %d \n",
							(int)victim_mb->sector, sealed, locked, cn_alloc_by_gc);
				}
			}
		}

		atomic_inc(&victim_seg->num_migios);

		cp_job = mempool_alloc(migrate_mgr->copy_job_pool, GFP_NOIO);
		while(!cp_job){
			printk(" copy job pool error \n");
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			printk("[__issue_mig_kcopyd] @@@@ interrupt @@@@\n");
			cp_job = mempool_alloc(migrate_mgr->copy_job_pool, GFP_NOIO);
		}
		atomic_inc(&migrate_mgr->copy_job_count);

		cp_job->src_mb = victim_mb;
		cp_job->src_region.bdev = get_bdev(super, victim_mb->idx);
		cp_job->src_region.sector = get_sector(super, victim_seg->seg_id, victim_mb->idx);
		cp_job->src_region.count = SRC_SECTORS_PER_PAGE;

		#if SWAN_GRP_GRANULARITY

		cp_job->mb_gc_cache_type = victim_seg->seg_gc_cache_type;

			#if 0 
			if (victim_seg->seg_gc_cache_type == SWAN_GC_CACHE_HOT) {
				printk("[__issue_mig_kcopyd] cp_job->mb_gc_cache ==  victim_seg->seg_gc_cache == SWAN_GC_CACHE_HOT\n");
			} else if (victim_seg->seg_gc_cache_type == SWAN_GC_CACHE_COLD) {
				printk("[__issue_mig_kcopyd] cp_job->mb_gc_cache ==  victim_seg->seg_gc_cache == SWAN_GC_CACHE_COLD\n");
			} else {
				printk("[__issue_mig_kcopyd] @@ERROR@@ victim_seg->seg_gc_cache %d\n", victim_seg->seg_gc_cache_type);
			}
			#endif	

		#endif

		#if SWAN_SEG_GRANULARITY

		victim_seg->seg_gc_cache_type = select_gc_cache_type_seg(super, victim_seg);
		//victim_seg->seg_gc_cache_type = SWAN_GC_CACHE_HOT;
		cp_job->mb_gc_cache_type = victim_seg->seg_gc_cache_type;
			
			#if 0  
			if (victim_seg->seg_gc_cache_type == SWAN_GC_CACHE_HOT) {
				printk("[__issue_mig_kcopyd] cp_job->mb_gc_cache ==  victim_seg->seg_gc_cache == SWAN_GC_CACHE_HOT\n");
			} else if (victim_seg->seg_gc_cache_type == SWAN_GC_CACHE_COLD) {
				printk("[__issue_mig_kcopyd] cp_job->mb_gc_cache ==  victim_seg->seg_gc_cache == SWAN_GC_CACHE_COLD\n");
			} else {
				printk("[__issue_mig_kcopyd] @@ERROR@@ victim_seg->seg_gc_cache %d\n", victim_seg->seg_gc_cache_type);
			}
			#endif	

		#endif

		list_add_tail(&cp_job->cp_list, &cp_job_group->cp_head);

		cp_job->page = alloc_single_page(super);
		if(cp_job->page==NULL){
			printk(" WARN: no rambuffer is allocated ... \n");
			BUG_ON(1);
		}



		if(use_gc){
			cp_job->dst_mb = NULL;
			cp_job->dst_region[0].bdev = NULL;
			cp_job->dst_count = 0;
		}else{
			cp_job->dst_region[0].bdev = super->dev_info.origin_dev->bdev;
			cp_job->dst_region[0].sector = victim_mb->sector;
			cp_job->dst_region[0].count = SRC_SECTORS_PER_PAGE;
			cp_job->dst_count = 1;
		}

		total_count++;
		if(total_count>=num_copy_block)
			break;
	}

NO_VICTIM:
	//if(no_rambuffer_count){
	//	printk(" WARN: No RAM buffer (retry = %d) for migrating clean data ... %s \n", 
	//				no_rambuffer_count, __FUNCTION__);
	//}

	return total_count;
}


void issue_mig_kcopyd(struct dmsrc_super *super, int use_gc){
	struct migration_manager *migrate_mgr = &super->migrate_mgr;
	struct segment_header *seg, *tmp;
	struct copy_job_group *cp_job_group;
	unsigned long f;
	int issue_count = 0;
	int total_count = 0;
	int total_count2 = 0;
	int bug_count = 0;
	int cache_type;

	#if SWAN
		static unsigned int gc_delay_threshold = 100;
		int free_seg_cnt1;
		int free_seg_cnt2;
	#endif

	total_count = 0;

	#if 0
	while(!atomic_read(&migrate_mgr->migrate_queue_count)){
		printk("[issue_mig_kcopyd] invalid victim count ... \n");
		printk("[issue_mig_kcopyd] Actually should have done BUG_ON(1) ... \n");
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
		//BUG_ON(1);
		//return;
	}
	#else
	if(!atomic_read(&migrate_mgr->migrate_queue_count)){
		printk("[issue_mig_kcopyd] invalid victim count ... \n");
		BUG_ON(1);
		return;
	}
	#endif

	list_for_each_entry_safe(seg, tmp, &migrate_mgr->migrate_queue, migrate_list) {
		unsigned long f;

		//printk("issue mig kcopyd inflight  = %d seg = %d \n", atomic_read(&migrate_mgr->mig_inflights), (int)seg->seg_id);
		//if(is_empty_seg(super, seg)){
		//	printk(" empty seg ... %d, stat = %d \n", (int)seg->seg_id, test_bit(SEG_MIGRATING, &seg->flags));
		//}

		BUG_ON(!test_bit(SEG_MIGRATING, &seg->flags));

		LOCK(super, f);
		set_bit(SEG_MIGRATING, &seg->flags);
		atomic_set(&seg->num_migios, 0);






		#if SWAN_READ_BLK_GC

                struct group_header *grp;
                u32 phy_col_id;
                grp = seg->group;
                phy_col_id = grp->phy_col_id;

                while(atomic_read(&seg->num_filling)){
                        UNLOCK(super, f);
                        schedule_timeout_interruptible(msecs_to_jiffies(1000));
                        printk("[issue_mig_kcopyd] @@@@ interrupt @@@@\n");
                        printk(" seg = %d  inflight %d filling = %d \n",
                                        (int)seg->seg_id, atomic_read(&seg->num_read_inflight)
                                        ,atomic_read(&seg->num_filling));
                        LOCK(super, f);
                }

                while(atomic64_read(&super->num_read_inflight_in_col[phy_col_id])){
                        UNLOCK(super, f);
                        schedule_timeout_interruptible(msecs_to_jiffies(1000));
                        printk("[issue_mig_kcopyd] @@@@ GC is interrupted by read! @@@@\n");
                        LOCK(super, f);
                }

		#else
		while(atomic_read(&seg->num_read_inflight) || atomic_read(&seg->num_filling)){
			UNLOCK(super, f);
			schedule_timeout_interruptible(msecs_to_jiffies(1000));
			printk("[issue_mig_kcopyd] @@@@ interrupt @@@@\n");
			printk(" seg = %d  inflight %d filling = %d \n", 
					(int)seg->seg_id, atomic_read(&seg->num_read_inflight)
					,atomic_read(&seg->num_filling)); 
			LOCK(super, f);
		}
		#endif
		UNLOCK(super, f);


		issue_count = calc_valid_count(super, seg, use_gc);
		//printk(" issue gc group = %d seg = %d, valid = %d, %d  \n", (int)seg->group_id,
		//		(int)seg->seg_id, 
		//		atomic_read(&seg->valid_count),
		//		issue_count);
		if(!issue_count){
			if(is_empty_seg(super, seg, use_gc)){
				//printk(" finalize clean free = %d, seg = %d, type = %d  \n", get_alloc_count(super), (int)seg->seg_id, seg->seg_type);
				finalize_clean_seg(super, seg, 1);
				list_del(&seg->migrate_list);
				atomic_dec(&migrate_mgr->migrate_queue_count);
			}else{
				printk(" Valid = %d\n", atomic_read(&seg->valid_count));
				printk(" Valid dirty = %d\n", atomic_read(&seg->valid_dirty_count));
				printk(" Valid clean = %d\n", atomic_read(&seg->valid_clean_count));
				printk(" no issue count but not empty seg ... \n");
				BUG_ON(1);
			}
		}else{
			atomic_inc(&migrate_mgr->mig_inflights);
		}
		total_count += issue_count;

		//atomic64_inc(&super->wstat.victim_count);
		//atomic64_add(issue_count*100/STRIPE_SZ, &super->wstat.victim_util);

		BUG_ON(++bug_count==100000000);
	}


	#if SWAN
	//cache_type = SWAN_GC_CACHE;
	cache_type = WCBUF;
	#else
	cache_type = WCBUF;
	#endif

//	if(use_gc){
		if(!total_count || 
			(total_count < STRIPE_SZ-get_metadata_count(super, cache_type)
			&& get_alloc_count(super) > MIGRATE_LOWWATER) ){
			//printk(" invalid victim count ... \n");
		//	release_victim_seg(super, migrate_mgr);
			//printk(" total count = %d \n", total_count);
			//printk(" free count = %d \n", get_alloc_count(super));
			//BUG_ON(1);
			//return;
		}

		if(!total_count){
			return;
		}
//	}else{
//		if(!total_count){
//			printk(" invalid victim count 2... \n");
//			release_victim_seg(super, migrate_mgr);
//			BUG_ON(1);
		//	return;
//			return;
//		}
//	}

	if(!atomic_read(&migrate_mgr->migrate_queue_count)){
		release_victim_seg(super, migrate_mgr);
		printk("[issue_mig_kcopyd] invalid victim count 3... \n");
		return;
	}


	#if 0
	free_seg_cnt1 = get_alloc_count(super);
	printk("     ###GC Start: total count = %d, free segs = %d ### \n", total_count, free_seg_cnt1);

	//total_count = 0;

	//if(!use_gc){
	//	printk(" destage mode \n");
	//	printk(" destage mode \n");
	//}
	//
	struct timeval start, latency;
	s64 us;
	do_gettimeofday(&start);
	#endif

	{
		
			while(total_count){
			cp_job_group = alloc_copy_job_group(super, cache_type);
			cp_job_group->gc = use_gc;

			LOCK(super, f);
			cp_job_group->dst_seg = super->current_seg[cache_type];
			cp_job_group->dst_rambuf = super->segbuf_mgr.current_rambuf[cache_type];
			cp_job_group->rw = READ;
			UNLOCK(super, f);

			#if SWAN
			total_count2 = __issue_mig_kcopyd(super, cp_job_group, cache_type, use_gc, 
					get_data_max_count(super, cache_type), &total_count);
			total_count -= total_count2;
			#else
			total_count2 = __issue_mig_kcopyd(super, cp_job_group, cache_type, use_gc, 
					get_data_max_count(super, cache_type));
			total_count -= total_count2;
			#endif

			if(total_count2){
				flush_kcopy_job(super, cp_job_group);
			}else{
				//printk(" no victim count = %d, total = %d  \n",total_count2, total_count);
				mempool_free(cp_job_group, migrate_mgr->group_job_pool);
				atomic_dec(&migrate_mgr->group_job_count);
			}
			if(total_count2==0){
				//printk(" total count = %d \n",total_count);
				break;
			}
		}

		
	}

	#if 0

	do_gettimeofday(&latency);
	latency.tv_sec -= start.tv_sec;
	latency.tv_usec -= start.tv_usec;	
	us = latency.tv_sec * USEC_PER_SEC + latency.tv_usec;

	#if SWAN
	free_seg_cnt2 = get_alloc_count(super);
	printk("     ###GC End: free segs = %d GC read latency = %lld ms ###\n", free_seg_cnt2, us/1000);
	#endif
	
	#if SWAN
	if (free_seg_cnt1 > free_seg_cnt2) { //GC rate < write rate
		printk("      $$$ BAD $$$ (GC rate < write rate) \n");

	} else {	//GC rate >= write rate
		printk("      $$$ GOOD $$$ (GC rate >= write rate) \n");
	}
	#endif

	#endif

	get_next_victim_seg(super, &super->migrate_mgr);
	if(migrate_mgr->gc_cur_seg){
		printk("[issue_mig_kcopyd] no victim segs 2... \n");
		BUG_ON(1);
	}
	release_victim_seg(super, migrate_mgr);

}

void finalize_clean_seg(struct dmsrc_super *super, struct segment_header *seg, int cleanup){
	size_t n1 = 1;
	unsigned long f;
	u32 free_seg_count;
	
	#if SWAN_TRIM
		sector_t sects_begin, sects_cnt;
		struct segment_allocator *seg_allocator = &super->seg_allocator;
		struct segment_header *trim_begin_seg;
		static int trim_cnt;
	#endif

	#if SWAN_Q2
		u32 phy_col_id;
		struct col_queue *col_Q;
	#endif

	while (atomic_read(&seg->num_read_inflight)) {
		WBWARN("Finalize clean seg: inflight ios remained for current seg");
		#if SWAN
	
		#else
		BUG_ON(1);
		#endif
		n1++;
		if (n1%10000==0){
			WBWARN("Finalize clean seg: inflight ios remained for current seg");
			n1 = 0;
		}
		schedule_timeout_interruptible(usecs_to_jiffies(1));
		#if KH_INTERRUPT_PRINT
		printk("[finalize_clean_seg] @@@@ interrupt @@@@\n");
		#endif
	}

	LOCK(super, f);

	//if(seg->seg_type==WBUF){
	//	atomic_sub( get_parity_size(super, seg->seg_type), &wb->num_used_caches);
	//}

	if(cleanup)
		cleanup_segment(super, seg);
	discard_caches_inseg(super, seg);

	UNLOCK(super, f);

	set_bit(SEG_CLEAN, &seg->flags);
	clear_bit(SEG_MIGRATING, &seg->flags);
	clear_bit(SEG_SEALED, &seg->flags);


	//if(use_gc){
	//	release_reserve_segs(super);
	//}

	#if SWAN_Q2
	phy_col_id = seg->group->phy_col_id;
	col_Q = &seg_allocator->col_queue_array[phy_col_id];
	move_seg_migrate_to_alloc_queue(super, seg, col_Q);
	#else
	move_seg_migrate_to_alloc_queue(super, seg);
	#endif

	free_seg_count = segment_group_inc_free_segs(super, seg);
	if(free_seg_count == SEGMENT_GROUP_SIZE){
		#if SWAN_DBG
		
		//rintk("[finalize_clean_seg] #1 group_id %d free_seg_count %d\n", seg->group->group_id, free_seg_count);
		//printk("[finalize_clean_seg] #2 CHUNK_SZ %u SEGMENT_GROUP_SIZE %u GROUP_SZ %u GROUP_SZ_SECTOR %u STRIPE_SZ %u\n", CHUNK_SZ, SEGMENT_GROUP_SIZE, GROUP_SZ, GROUP_SZ_SECTOR, STRIPE_SZ);

		#endif
		
		#if SWAN_TRIM
		
		//LOCK(super, f);

		int dev_col_no = seg_allocator->gc_cold_write_col;
		int dev_no;
		int i;
		
		trim_begin_seg = get_segment_header_by_id(super, seg->group->group_id * SEGMENT_GROUP_SIZE);
		sects_begin = get_sector( super, trim_begin_seg->seg_id, SEG_START_IDX(trim_begin_seg) );
		sects_cnt = CHUNK_SZ_SECTOR * SEGMENT_GROUP_SIZE;

		for ( i = 0; i < NUM_SSD; i++ ) {
			dev_no = i + NUM_SSD * dev_col_no;
			
			++trim_cnt;
			#if SWAN_ONLY_CHECKER_PROC

			#else

		#if SWAN_DBG
			printk("[finalize_clean_seg] #3 @@@@TRIM[%llu]@@@@ [i %u] seg->group_id %d seg->seg_id %d dev_no %d sects_begin %llu sects_cnt %llu\n",
			trim_cnt,
			i,
			(int)seg->group->group_id,
			(int)seg->seg_id,
			dev_no,
			sects_begin,
			sects_cnt);
		#endif
			#endif

	
			blkdev_issue_discard(super->dev_info.cache_dev[dev_no]->bdev, sects_begin, sects_cnt, GFP_KERNEL, 0);
		}
	
		//UNLOCK(super, f);	

		#endif

		#if SWAN_Q2
		move_group_migrate_to_alloc_queue(super, seg->group, col_Q);
		#else
		move_group_migrate_to_alloc_queue(super, seg->group);
		#endif
	}



	#if SWAN

	//Do nothing
	
	#else

	//atomic64_set(&super->last_migrated_segment_id, seg->seg_id);
	if(!atomic_read(&super->migrate_mgr.background_gc_on)){
		if(get_alloc_count(super) >= MIGRATE_HIGHWATER &&
			atomic_read(&super->seg_allocator.group_alloc_count)>=1){
			//printk(" >>>> Fore GC: free seg count = %d \n", get_alloc_count(super));
			if(atomic_read(&super->migrate_mgr.migrate_triggered)){
				atomic_set(&super->migrate_mgr.background_gc_on, 0);
				atomic_set(&super->migrate_mgr.migrate_triggered, 0);
			}
		}
	}else{
		if(get_alloc_count(super) >= MIGRATE_HIGHWATER*4){
			printk(" >>>> Back GC: free seg count = %d \n", get_alloc_count(super));
			if(atomic_read(&super->migrate_mgr.migrate_triggered)){
				atomic_set(&super->migrate_mgr.background_gc_on, 0);
				atomic_set(&super->migrate_mgr.migrate_triggered, 0);
			}
		}

	}
	#endif
	
	#if SWAN
	//pending_worker_schedule(super); [kh3]
	#else 
	pending_worker_schedule(super); 
	#endif
}

void seg_clear_hit_bits(struct dmsrc_super *super, struct segment_header *seg){
	struct metablock *mb;
	int i;

	for(i = 0;i < STRIPE_SZ;i++){
		mb = get_mb(super, seg->seg_id, i);
		if(test_bit(MB_HIT, &mb->mb_flags)){
			clear_bit(MB_HIT, &mb->mb_flags);
			atomic_dec(&seg->hot_count);
		}
	}
}

#if 0
int get_data_size_in_stripe(struct dmsrc_super *super, int cache_type){
	int min;

	if(SUMMARY_SCHEME==SUMMARY_PER_CHUNK){
		if(cache_type==WCBUF || cache_type==WHBUF)
			min = STRIPE_SZ;
		else
			min = STRIPE_SZ;
	}else{
		if(cache_type==WCBUF || cache_type==WHBUF)
			min = STRIPE_SZ;
		else
			min = STRIPE_SZ;
	}

	return min;

}
#endif 

#if 0
struct segment_header *select_victim_greedy_cold(struct dmsrc_super *super, int use_gc){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct segment_header *seg, *min_seg = NULL;
	u32 min = 0;
		;

	list_for_each_entry(seg, &seg_allocator->sealed_queue, alloc_list){
		if(use_gc && atomic_read(&seg->valid_count)>=get_data_size_in_stripe(super, seg->seg_type))
			continue;

		if(!is_cold_stripe(seg->seg_type))
			continue;

		if(min_seg){
			if(atomic_read(&seg->valid_count)<min &&
			   !test_bit(SEG_MIGRATING, &seg->flags))
			{
				min_seg = seg;
				min = atomic_read(&seg->valid_count);
			}
		}else{
			min_seg = seg;
			min = atomic_read(&seg->valid_count);
		}
	}

	//printk(" victim count = %d \n", atomic_read(&min_seg->valid_count));

	return min_seg;
}
#endif

void print_valid_count(struct dmsrc_super *super){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct segment_header *seg;
	//u32 min = 0;
	/*
	list_for_each_entry(seg, &seg_allocator->seg_sealed_queue, alloc_list){
		if(!test_bit(SEG_MIGRATING, &seg->flags))
			printk(" normal seg = %d vc = %d \n", (int)seg->seg_id, atomic_read(&seg->valid_count));
		else
			printk(" migrating seg = %d vc = %d \n", (int)seg->seg_id, atomic_read(&seg->valid_count));
	}
	*/

}
	
#if 0
struct segment_header *select_victim_greedy(struct dmsrc_super *super, int use_gc){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct segment_header *seg, *min_seg = NULL;
	u32 min = 0;

	list_for_each_entry(seg, &seg_allocator->sealed_queue, alloc_list){
		if(use_gc && atomic_read(&seg->valid_count)>=get_data_size_in_stripe(super, seg->seg_type))
			continue;

		if(test_bit(SEG_MIGRATING, &seg->flags))
			continue;

		if(min_seg){
			if(atomic_read(&seg->valid_count)<min &&
			   !test_bit(SEG_MIGRATING, &seg->flags))
			{
				min_seg = seg;
				min = atomic_read(&seg->valid_count);
			}
		}else{
			min_seg = seg;
			min = atomic_read(&seg->valid_count);
		}
	}

	//printk(" victim count = %d \n", atomic_read(&min_seg->valid_count));

	return min_seg;
}
#endif

struct group_header *select_victim_fifo(struct dmsrc_super *super, int use_gc){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct group_header *group, *min_group = NULL;
	u32 min = 0;
	
	/*	not used -KH
	list_for_each_entry(group, &seg_allocator->group_sealed_queue, alloc_list){
		//if(use_gc && atomic_read(&group->valid_count)>=STRIPE_SZ * SEGMENT_GROUP_SIZE)
		//	continue;

		min_group = group;
		min = atomic_read(&group->valid_count);
		break;
	}

	//printk(" ** victim group = %d count = %d \n", min_group->group_id, atomic_read(&min_group->valid_count));
	//BUG_ON(min_group==NULL);
	*/
	return min_group;
}


#if 0
struct group_header *select_victim_greedy2(struct dmsrc_super *super, int use_gc){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct group_header *group, *min_group = NULL;
	u32 min = 0;

	list_for_each_entry(group, &seg_allocator->group_sealed_queue, alloc_list){
		#if SWAN
		if (group->phy_col_id == seg_allocator->gc_hot_write_col) {
		#endif		
			if(use_gc && atomic_read(&group->valid_count)>=STRIPE_SZ * SEGMENT_GROUP_SIZE)
				continue;

		//printk(" victim group = %d count = %d \n", group->group_id, atomic_read(&group->valid_count));
			if(min_group){
				if(atomic_read(&group->valid_count)<min){
					min_group = group;
					min = atomic_read(&group->valid_count);
				}
			}else{
				min_group = group;
				min = atomic_read(&group->valid_count);
			}
		#if SWAN
		}
		#endif
	}

	//printk(" ** victim group = %d count = %d \n", min_group->group_id, atomic_read(&min_group->valid_count));
	//BUG_ON(min_group==NULL);
	#if SWAN_DBG
		if (min_group != NULL) {
			#if SWAN_ONLY_CHECKER_PROC
			
			#else
			printk("[select_victim_greedy2] min_group->group_id %u seg_allocator->gc_hot_write_col %u\n", min_group->group_id, seg_allocator->gc_hot_write_col);
			#endif
		} else {
			#if SWAN_ONLY_CHECKER_PROC
			
			#else
			printk("[select_victim_greedy2] min_group->group_id NULL!! seg_allocator->gc_hot_write_col %u\n", seg_allocator->gc_hot_write_col);
			#endif
		}
		
	#endif

	return min_group;
}
#endif


#if SWAN_Q2
struct group_header *select_victim_greedy(struct dmsrc_super *super, int use_gc, struct col_queue *col_Q){
#else
struct group_header *select_victim_greedy(struct dmsrc_super *super, int use_gc){
#endif
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct group_header *group, *min_group = NULL;
	u32 min = 0;

	#if SWAN_Q2
	list_for_each_entry(group, &col_Q->group_sealed_queue, alloc_list){
	#else
	list_for_each_entry(group, &seg_allocator->group_sealed_queue, alloc_list){
	#endif
		
		#if SWAN_Q2	
		//do nothing
		#else
			#if SWAN
			if (group->phy_col_id == seg_allocator->gc_cold_write_col) {
			#endif		
		#endif
				if(use_gc && atomic_read(&group->valid_count)>=STRIPE_SZ * SEGMENT_GROUP_SIZE)
					continue;

			//printk(" victim group = %d count = %d \n", group->group_id, atomic_read(&group->valid_count));
				if(min_group){
					if(atomic_read(&group->valid_count)<min){
						min_group = group;
						min = atomic_read(&group->valid_count);
					}
				}else{
					min_group = group;
					min = atomic_read(&group->valid_count);
				}
		#if SWAN_Q2
		//do nothing
		#else
			#if SWAN
			}
			#endif
		#endif
	}

	//printk(" ** victim group = %d count = %d \n", min_group->group_id, atomic_read(&min_group->valid_count));
	//BUG_ON(min_group==NULL);
	#if SWAN_DBG
		if (min_group != NULL) {
			#if SWAN_ONLY_CHECKER_PROC
			
			#else
		#if SWAN_DBG
			printk("[select_victim_greedy] min_group->group_id %u seg_allocator->gc_cold_write_col %u\n", min_group->group_id, seg_allocator->gc_cold_write_col);
		#endif
			#endif
		} else {
			#if SWAN_ONLY_CHECKER_PROC
			
			#else
		#if SWAN_DBG
			printk("[select_victim_greedy] min_group->group_id NULL!! seg_allocator->gc_cold_write_col %u\n", seg_allocator->gc_cold_write_col);
		#endif
			#endif
		}
	#endif


	return min_group;
}


//int _select_victim_for_simple_gc(struct dmsrc_super *super, int num_mig, int *use_gc){
//}

#if 0 
int _select_victim_for_selective_gc(struct dmsrc_super *super, int num_mig, int *use_gc){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct list_head *sealed_queue = &seg_allocator->sealed_queue;
	struct segment_header *seg;
	unsigned long flags;
	int recount = 0;
	int loop = 0;

	spin_lock_irqsave(&seg_allocator->alloc_lock, flags);

	while(!list_empty(sealed_queue)){
		u32 min;
		bool hit = false;
		bool condition = false;

		seg	 = (struct segment_header *)
			list_entry(sealed_queue->prev, struct segment_header, alloc_list);

		min = get_data_size_in_stripe(super, seg->seg_type);

		hit = test_bit(SEG_HIT, &seg->flags);
		if(super->param.hit_bitmap_type==HIT_BITMAP_PER_STRIPE){
			if(hit && atomic_read(&seg->valid_count) < min){
				condition = true;
			}
		}else{
			if(hit && atomic_read(&seg->valid_count) < min &&
				atomic_read(&seg->hot_count) < STRIPE_SZ/2){
				condition = true;
			}
		}

		if(condition){ // selective gc 
			clear_bit(SEG_HIT, &seg->flags);
			list_add_tail(&seg->migrate_list, &super->migrate_mgr.migrate_queue);
			atomic_inc(&super->migrate_mgr.migrate_queue_count);

			move_seg_sealed_to_migrate_queue(super, seg, 0);
			recount++;

			if(recount>=num_mig)
				break;
		}else{
			if(hit){ // skip
				list_move(&seg->alloc_list, sealed_queue);
				clear_bit(SEG_HIT, &seg->flags);
				seg_clear_hit_bits(super, seg);
			}else{ // go to destage
				break;
			}
		}

		loop++;
		if(loop>=seg_allocator->sealed_count)
			break;
	}

	spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);

	//if(!recount){
	//	release_reserve_segs(super);
	//	*use_gc = 0;
	//}

	return recount;
}
#endif

#if SWAN_Q2
int _select_victim_for_gc(struct dmsrc_super *super, int num_mig, struct segment_header *seg, struct col_queue *col_Q){
#else
int _select_victim_for_gc(struct dmsrc_super *super, int num_mig, struct segment_header *seg){
#endif
	int total_valid_count = 0;

	set_bit(SEG_MIGRATING, &seg->flags);

	list_add_tail(&seg->migrate_list, &super->migrate_mgr.migrate_queue);
	atomic_inc(&super->migrate_mgr.migrate_queue_count);

	#if SWAN_Q2
	move_seg_sealed_to_migrate_queue(super, seg, 0, col_Q);
	#else
	move_seg_sealed_to_migrate_queue(super, seg, 0);
	#endif

	total_valid_count += get_data_valid_count(super, seg);

	return total_valid_count;
}


#if 0
int select_victim_for_gc2(struct dmsrc_super *super, int num_mig){
	//printk("[select_victim_for_gc2]\n");
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct list_head *sealed_queue = &seg_allocator->seg_sealed_queue;
	struct group_header *group = NULL;
	struct segment_header *seg = NULL;
	unsigned long flags;
	int loop = 0;
	int recount = 0;
	int total_valid_count = 0;
	u32 seg_offset = 0;

	spin_lock_irqsave(&seg_allocator->alloc_lock, flags);

	while(!list_empty(sealed_queue)){

		if(super->param.victim_policy==VICTIM_GREEDY) {
			group = select_victim_greedy2(super, 1);

			#if SWAN_GRP_GRANULARITY

			if(group==NULL){
				//print_valid_count(super);
				printk(" Greedy: No victim, selected victims = %d \n", total_valid_count);
				goto NO_VICTIM;
			} else {
				//group->grp_gc_cache_type = select_gc_cache_type_grp(super, group);  //Core fuction on hot/cold classification (SWAN_GRP_GRANULARITY)
				group->grp_gc_cache_type = SWAN_GC_CACHE_HOT;
			}	

			#endif
		}
		else {
			group = select_victim_fifo(super, 1);
		}

		if(group==NULL){
			//print_valid_count(super);
			printk(" Greedy: No victim, selected victims = %d \n", total_valid_count);
			goto NO_VICTIM;
		}
		//not yet.. KH will fix when GC2 is required..
		//move_group_sealed_to_migrate_queue(super, group, 0);

		for(seg_offset = 0;seg_offset < SEGMENT_GROUP_SIZE;seg_offset++){
			seg = get_segment_header_by_id(super, group->group_id * SEGMENT_GROUP_SIZE + seg_offset);

			#if SWAN_GRP_GRANULARITY
			seg->seg_gc_cache_type = group->grp_gc_cache_type;
			#endif

			total_valid_count += _select_victim_for_gc(super, num_mig, seg);
			BUG_ON(seg->group!=group);
		}

		recount += SEGMENT_GROUP_SIZE;

		//printk(" select victim util %d \n", total_valid_count * 100 / (recount*STRIPE_SZ));

		if(total_valid_count >= num_mig * get_data_max_count(super, WCBUF))
			break;

		loop++;
		if(loop>=seg_allocator->seg_sealed_count)
			break;

		if(recount >= num_mig*2)
			break;
	}

NO_VICTIM:

	spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);


	//printk(" GC: num victim segs = %d \n", recount);

	return recount;
}
#endif




#if 1
int select_victim_for_gc(struct dmsrc_super *super, int num_mig){
//printk("[select_victim_for_gc]\n");
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	#if SWAN_Q2

	#else
	struct list_head *sealed_queue = &seg_allocator->seg_sealed_queue;
	#endif

	struct group_header *group = NULL;
	struct segment_header *seg = NULL;
	unsigned long flags;
	int loop = 0;
	int recount = 0;
	int total_valid_count = 0;
	u32 seg_offset = 0;

	#if SWAN_Q2

	spin_lock_irqsave(&seg_allocator->alloc_lock, flags);
	u32 gc_cold_write_col = seg_allocator->gc_cold_write_col;
	spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);
	struct col_queue *col_Q = &seg_allocator->col_queue_array[gc_cold_write_col];
	struct list_head *sealed_queue = &col_Q->seg_sealed_queue;
	spin_lock_irqsave(&col_Q->col_queue_lock, flags);

	#else
	spin_lock_irqsave(&seg_allocator->alloc_lock, flags);
	#endif

	while(!list_empty(sealed_queue)){

		if(super->param.victim_policy==VICTIM_GREEDY) {
			#if SWAN_Q2
			group = select_victim_greedy(super, 1, col_Q);
			#else
			group = select_victim_greedy(super, 1);
			#endif

			#if SWAN_GRP_GRANULARITY

			if(group==NULL){
				//print_valid_count(super);
				printk(" Greedy: No victim, selected victims = %d \n", total_valid_count);
				goto NO_VICTIM;
			} else {
				//group->grp_gc_cache_type = select_gc_cache_type_grp(super, group);  //Core fuction on hot/cold classification (SWAN_GRP_GRANULARITY)
				group->grp_gc_cache_type = SWAN_GC_CACHE_COLD;
			}	

			#endif
		}
		else {
			group = select_victim_fifo(super, 1);
		}

		if(group==NULL){
			//print_valid_count(super);
			printk(" Greedy: No victim, selected victims = %d \n", total_valid_count);
			goto NO_VICTIM;
		}
	
		#if SWAN_Q2
		move_group_sealed_to_migrate_queue(super, group, 0, col_Q);
		#else
		move_group_sealed_to_migrate_queue(super, group, 0);
		#endif

		for(seg_offset = 0;seg_offset < SEGMENT_GROUP_SIZE;seg_offset++){
			seg = get_segment_header_by_id(super, group->group_id * SEGMENT_GROUP_SIZE + seg_offset);

			#if SWAN_GRP_GRANULARITY
			seg->seg_gc_cache_type = group->grp_gc_cache_type;
			#endif

			#if SWAN_Q2
			total_valid_count += _select_victim_for_gc(super, num_mig, seg, col_Q);
			#else
			total_valid_count += _select_victim_for_gc(super, num_mig, seg);
			#endif

			BUG_ON(seg->group!=group);
		}

		recount += SEGMENT_GROUP_SIZE;

		//printk(" select victim util %d \n", total_valid_count * 100 / (recount*STRIPE_SZ));

		if(total_valid_count >= num_mig * get_data_max_count(super, WCBUF))
			break;

		loop++;
		#if SWAN_Q2
		if(loop>=col_Q->seg_sealed_count)
		#else
		if(loop>=seg_allocator->seg_sealed_count)
		#endif
			break;

		if(recount >= num_mig*2)
			break;
	}

NO_VICTIM:

	#if SWAN_Q2
	spin_unlock_irqrestore(&col_Q->col_queue_lock, flags);
	#else
	spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);
	#endif

	//printk(" GC: num victim segs = %d \n", recount);

	return recount;
}
#else
int select_victim_for_gc(struct dmsrc_super *super, int num_mig, int *use_gc){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct list_head *sealed_queue = &seg_allocator->sealed_queue;
	struct segment_header *seg = NULL;
	unsigned long flags;
	int loop = 0;
	int total_valid_count = 0;
	int recount = 0;

	spin_lock_irqsave(&seg_allocator->alloc_lock, flags);

	while(!list_empty(sealed_queue)){
		unsigned long debug_count = 0;

		seg = NULL;
		if(super->param.victim_policy==VICTIM_GREEDY){
			seg = select_victim_greedy(super, 1);
			if(seg==NULL){
				print_valid_count(super);
				printk(" Greedy: No victim, selected victims = %d \n", recount);
				goto NO_VICTIM;
			}
		}

		if(seg==NULL)
			seg	 = (struct segment_header *)
				list_entry(sealed_queue->prev, struct segment_header, alloc_list);

		set_bit(SEG_MIGRATING, &seg->flags);

		//printk(" + wait + \n");
		while(atomic_read(&seg->num_read_inflight) || atomic_read(&seg->num_filling)) {
			schedule_timeout_interruptible(usecs_to_jiffies(100));
			debug_count++;
			if(debug_count>10000){
				printk(" wait ... \n");
				debug_count = 0;
			}
		}
		//printk(" - wait - \n");

		if(atomic_read(&seg->valid_count) < get_data_size_in_stripe(super, seg->seg_type)){

			list_add_tail(&seg->migrate_list, &super->migrate_mgr.migrate_queue);
			atomic_inc(&super->migrate_mgr.migrate_queue_count);

			move_seg_sealed_to_migrate_queue(super, seg, 0);

			total_valid_count += get_data_valid_count(super, seg);
			//printk(" total valid count = %d \n", total_valid_count+get_metadata_count(super, seg));
			printk(" GC %d select victim seg = %d length = %d, valid count = %d \n", 
					recount,
					(int)seg->seg_id,
					(int)atomic_read(&seg->length),
					(int)atomic_read(&seg->valid_count));

			BUG_ON(!test_bit(SEG_MIGRATING, &seg->flags));

			recount++;
			//if((total_valid_count+get_metadata_count(super, seg->seg_type)) > STRIPE_SZ){
			if(total_valid_count > get_data_max_count(super, WCBUF) * num_mig){
				break;
			}
		}else{
			//LOCK(super, f);
			clear_bit(SEG_MIGRATING, &seg->flags);
			//UNLOCK(super, f);

			list_move(&seg->alloc_list, sealed_queue);
		}

		loop++;
		if(loop>=seg_allocator->seg_sealed_count)
			break;
	}

NO_VICTIM:

	spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);

	//if(!total_valid_count){
	//	release_reserve_segs(super);
	//	*use_gc = 0;
	//}

	printk(" GC: total valid count = %d, num segs = %d \n", total_valid_count, recount);

	return recount;
}
#endif

#if 0
int select_victim_for_destage(struct dmsrc_super *super, int num_mig){
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	struct list_head *sealed_queue = &seg_allocator->sealed_queue;
	struct segment_header *seg;
	unsigned long flags;
	int recount = 0;
	int loop = 0;
	int total_valid_count = 0;

	spin_lock_irqsave(&seg_allocator->alloc_lock, flags);

	while(!list_empty(sealed_queue)){

		seg = select_victim_greedy_cold(super, 0);
		if(!seg)
			seg = select_victim_greedy(super, 0);

		if(!seg)
			seg	 = (struct segment_header *)
				list_entry(sealed_queue->prev, struct segment_header, alloc_list);

		clear_bit(SEG_HIT, &seg->flags);
		seg_clear_hit_bits(super, seg);

		//LOCK(super, f);
		set_bit(SEG_MIGRATING, &seg->flags);
	//	UNLOCK(super, f);

		while(atomic_read(&seg->num_read_inflight) || atomic_read(&seg->num_filling)) {
			schedule_timeout_interruptible(usecs_to_jiffies(100));
		}

		printk(" >>>***  Destage seg = %d valid count = %d \n",
				(int)seg->seg_id, (int)atomic_read(&seg->valid_count));

		move_seg_sealed_to_migrate_queue(super, seg, 0);
		list_add_tail(&seg->migrate_list, &super->migrate_mgr.migrate_queue);
		atomic_inc(&super->migrate_mgr.migrate_queue_count);

		total_valid_count += get_data_valid_count(super, seg);

		recount++;
#if 0 
		if((total_valid_count+get_metadata_count(super, seg->seg_type))*2 > STRIPE_SZ*2){
			break;
		}
		//else{
		//	list_move(&seg->alloc_list, sealed_queue);
		//}

#else
		if(recount>=num_mig)
			break;
#endif


		if(++loop>=seg_allocator->sealed_count)
			break;
	}

	spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);

//	printk(" Destage: total valid count = %d \n", total_valid_count);

	return recount;
}
#endif

#if 0
int select_victim_segs2(struct dmsrc_super *super, int num_mig, int *use_gc){
//printk("[select_victim_segs2]\n");
	int recount = 0;

	if(super->param.reclaim_policy==RECLAIM_SELECTIVE || super->param.reclaim_policy==RECLAIM_GC){
//#ifdef FORCE_UMAX
		if(get_curr_util(super) >= super->param.u_max) {
			#if SWAN
			*use_gc = 1;
			#else	
			*use_gc = 0; // to disks
			#endif
		}
		else { 
			*use_gc = 1;
		}
//#else
//		if(get_curr_util(super) >= super->param.u_max &&  atomic_read(&super->seg_allocator.seg_alloc_count) < 2){ 
//			*use_gc = 0; // to disks
//		}else{ // to SSDs
//			*use_gc = 1;
//		}
//#endif 
	}else if(super->param.reclaim_policy==RECLAIM_DESTAGE){
		#if SWAN
		*use_gc = 1;
		#else	
		*use_gc = 0;
		#endif
	}

	recount = select_victim_for_gc2(super, num_mig);

	if(recount){
	   if(*use_gc){
		   atomic_add(num_mig, &super->wstat.gc_count);
	   }else{
	  	   atomic_add(num_mig, &super->wstat.destage_count);
	   }
	   #if SWAN_DBG
		//printk("[selec_victim_segs] gc_count %u destage_count %u\n", atomic_read(&super->wstat.gc_count), atomic_read(&super->wstat.destage_count));
	   #endif

	}
	return recount;
}
#endif




int select_victim_segs(struct dmsrc_super *super, int num_mig, int *use_gc){
	//printk("[select_victim_segs]\n");
	int recount = 0;

	if(super->param.reclaim_policy==RECLAIM_SELECTIVE || super->param.reclaim_policy==RECLAIM_GC){
//#ifdef FORCE_UMAX
		if(get_curr_util(super) >= super->param.u_max) {
			#if SWAN
			*use_gc = 1;
			#else	
			*use_gc = 0; // to disks
			#endif
		}
		else { 
			*use_gc = 1;
		}
//#else
//		if(get_curr_util(super) >= super->param.u_max &&  atomic_read(&super->seg_allocator.seg_alloc_count) < 2){ 
//			*use_gc = 0; // to disks
//		}else{ // to SSDs
//			*use_gc = 1;
//		}
//#endif 
	}else if(super->param.reclaim_policy==RECLAIM_DESTAGE){
		#if SWAN
		*use_gc = 1;
		#else	
		*use_gc = 0;
		#endif
	}

	recount = select_victim_for_gc(super, num_mig);

	if(recount){
	   if(*use_gc){
		   atomic_add(num_mig, &super->wstat.gc_count);
	   }else{
	  	   atomic_add(num_mig, &super->wstat.destage_count);
	   }
	   #if SWAN_DBG
		//printk("[selec_victim_segs] gc_count %u destage_count %u\n", atomic_read(&super->wstat.gc_count), atomic_read(&super->wstat.destage_count));
	   #endif

	}
	return recount;
}

int migrate_proc(void *data)
{
	struct dmsrc_super *super = data;
	struct migration_manager *migrate_mgr = &super->migrate_mgr;
	u32 num_mig = 0;

	#if SWAN	
	int use_gc = 1;
	u32 num_mig2 = 0;
	struct segment_allocator *seg_allocator = &super->seg_allocator;
	unsigned long flags;
	#else
	int use_gc = 0;
	#endif
	u32 high_water;
	u32 gc_buf_max_count = atomic_read(&super->segbuf_mgr.total_page_count)/10*5;

	printk(" Migrate daemon has been started .. \n");

	while (1) {

		if(kthread_should_stop())
			break;

		if (!atomic_read(&migrate_mgr->migrate_triggered)||
			atomic_read(&super->degraded_mode) ||
			atomic_read(&super->resize_mode))
		{
			schedule_timeout_interruptible(usecs_to_jiffies(1000));
			continue;
		}
		
		#if SWAN

		#else
		if(!atomic_read(&migrate_mgr->background_gc_on))
			high_water = MIGRATE_HIGHWATER;
		else
			high_water = MIGRATE_HIGHWATER * 10;
		#endif
		//printk(" Start: num mig = %d, job count = %d, active = %d, total = %d (%d%%) free = %d\n", num_mig, atomic_read(&migrate_mgr->copy_job_count),
		//	atomic_read(&super->segbuf_mgr.active_page_count), atomic_read(&super->segbuf_mgr.total_page_count),
		//	atomic_read(&super->segbuf_mgr.active_page_count)*100/atomic_read(&super->segbuf_mgr.total_page_count),
		//	get_alloc_count(super) 
		//	);
		//printk(" free %d high = %d, cur util = %d, umax = %d, group = %d \n", (int)get_alloc_count(super) , (int)high_water ,
		//	(int)get_curr_util(super) ,  (int)super->param.u_max, 
		//	atomic_read(&super->seg_allocator.group_alloc_count));


#ifdef FORCE_UMAX
		if((get_alloc_count(super) >= high_water && 
			get_curr_util(super) <=  super->param.u_max) &&
			atomic_read(&super->seg_allocator.group_alloc_count)>1
		  ){
#endif

#if SWAN
		//if(get_grp_alloc_count_of_gc_read_col(super) >= SWAN_NUM_VICTIM_GROUP) {
		if (0) {	
#else
		if(get_alloc_count(super) >= high_water && 
			atomic_read(&super->seg_allocator.group_alloc_count)>1
		  ){
#endif 
			atomic_set(&migrate_mgr->background_gc_on, 0);
			atomic_set(&migrate_mgr->migrate_triggered, 0);
			schedule_timeout_interruptible(usecs_to_jiffies(50));
			printk("[migrate_proc] GC Finished ... 1\n");
			pending_worker_schedule(super); 
			continue;
		}

		//if(super->param.victim_policy==VICTIM_GREEDY)
		//clean_empty_seg(super, use_gc);

		//if((get_alloc_count(super) >= high_water && 
		//	get_curr_util(super) <=  super->param.u_max)){
		//	atomic_set(&migrate_mgr->background_gc_on, 0);
		//	atomic_set(&migrate_mgr->migrate_triggered, 0);
		//	schedule_timeout_interruptible(usecs_to_jiffies(50));
		//	pending_worker_schedule(super);
		//	printk(" GC Finished ... 2\n");
		//	continue;
		//}

		num_mig = 0;
		while(!num_mig){
	
			num_mig = 
				(gc_buf_max_count  -
				atomic_read(&super->segbuf_mgr.gc_active_page_count))
					/ get_data_max_count(super, WCBUF);
			//printk(" num mig = %d, max = %d \n", num_mig, gc_buf_max_count/get_data_max_count(super, WCBUF));
			if(!num_mig)
				schedule_timeout_interruptible(usecs_to_jiffies(50));
		}

		if(num_mig>get_alloc_count(super))
			num_mig = get_alloc_count(super)-1;

		if(!num_mig){
			printk(" num mig = %d \n", num_mig);
			num_mig = 1;
		}
		/*
		printk(" Start: num mig = %d, job count = %d, active = %d, total = %d (%d%%) free = %d\n", num_mig, atomic_read(&migrate_mgr->copy_job_count),
			atomic_read(&super->segbuf_mgr.active_page_count), atomic_read(&super->segbuf_mgr.total_page_count),
			atomic_read(&super->segbuf_mgr.active_page_count)*100/atomic_read(&super->segbuf_mgr.total_page_count),
			get_alloc_count(super) 
			);
		*/
		#if SWAN

		#if SWAN_GC_1

		num_mig = select_victim_segs(super, num_mig, &use_gc);

		if(num_mig == 0){ //[TO FIX]
			
			printk("[migrate_proc] @@@@@@@@ no victim block ... use gc = %d, free = %d, num victim segs = %d\n", use_gc,
					get_alloc_count(super), num_mig);
			
			atomic_set(&migrate_mgr->background_gc_on, 0);
			atomic_set(&migrate_mgr->migrate_triggered, 0);
			schedule_timeout_interruptible(usecs_to_jiffies(50));
			printk("[migrate_proc] GC Finished ... 1\n");
			//pending_worker_schedule(super); //[kh3]

			//spin_lock_irqsave(&seg_allocator->alloc_lock, flags);
			//seg_allocator->req_write_col = (seg_allocator->req_write_col + 1) % NUM_PHY_COL;
			//seg_allocator->gc_cold_write_col = (seg_allocator->gc_cold_write_col + 1) % NUM_PHY_COL;
			//seg_allocator->gc_hot_write_col = (seg_allocator->gc_hot_write_col + 1) % NUM_PHY_COL;
			#if SWAN_DBG
			//	printk("[migrate_proc] REQ_WRITE_COL MOVED to %u\n", seg_allocator->req_write_col);
			//	printk("[migrate_proc] req_write_col %u gc_cold_write_col %u gc_hot_write_col %u\n", seg_allocator->req_write_col, seg_allocator->gc_cold_write_col, seg_allocator->gc_hot_write_col);
			#endif

		//	spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);
		 	continue;
		}

		#endif
		
		#if SWAN_GC_2
		//[kh] Don't change the order of select_victim_segs2() and select_victim_seg()..
		num_mig2 = select_victim_segs2(super, num_mig, &use_gc); // closer to req_col (1)
		num_mig = select_victim_segs(super, num_mig, &use_gc); // farther from req_col (0)

		if(num_mig == 0) {
			printk("[migrate_proc] @@@ no victim in GC1 column\n");
		}

		if(num_mig2 == 0) {
			printk("[migrate_proc] @@@ no victim in GC2 column\n");
		}

		if(num_mig == 0 && num_mig2 == 0) {
			printk("[migrate_proc] @@@@@@@@ no victim block ... use gc = %d, free = %d, num victim segs = %d\n", use_gc,
					get_alloc_count(super), num_mig);
			//printk("[migrate_proc] Let's call print_valid_count() here!!\n");
			atomic_set(&migrate_mgr->background_gc_on, 0);
			atomic_set(&migrate_mgr->migrate_triggered, 0);
			schedule_timeout_interruptible(usecs_to_jiffies(50));
			printk("[migrate_proc] GC Finished ... 1\n");
		 	continue;
		}

		/*
		if(num_mig == 0 || num_mig2 == 0){ //[TO FIX]
			
			spin_lock_irqsave(&seg_allocator->alloc_lock, flags);

			seg_allocator->req_write_col = (seg_allocator->req_write_col + 1) % NUM_PHY_COL;
			seg_allocator->gc_cold_write_col = (seg_allocator->gc_cold_write_col + 1) % NUM_PHY_COL;
			seg_allocator->gc_hot_write_col = (seg_allocator->gc_hot_write_col + 1) % NUM_PHY_COL;
			#if SWAN_DBG
				printk("[migrate_proc] REQ_WRITE_COL MOVED to %u\n", seg_allocator->req_write_col);
				printk("[migrate_proc] req_write_col %u gc_cold_write_col %u gc_hot_write_col %u\n", seg_allocator->req_write_col, seg_allocator->gc_cold_write_col, seg_allocator->gc_hot_write_col);
			#endif

			spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);
			continue;
		}
		*/
		#endif

		
		/*
		printk(" selected victim block ... use gc = %d, free = %d, num victim segs = %d\n", use_gc,
				get_alloc_count(super), num_mig);
		*/
		
		issue_mig_kcopyd(super, use_gc);

		#else
		num_mig = select_victim_segs(super, num_mig, &use_gc);
		
		if(num_mig==0){
			printk("@@@@@@@@ no victim block ... use gc = %d, free = %d, num victim segs = %d\n", use_gc,
					get_alloc_count(super), num_mig);
			

			atomic_set(&migrate_mgr->background_gc_on, 0);
			atomic_set(&migrate_mgr->migrate_triggered, 0);
			schedule_timeout_interruptible(usecs_to_jiffies(1000));
			continue;
		}
		/*
		printk(" selected victim block ... use gc = %d, free = %d, num victim segs = %d\n", use_gc,
				get_alloc_count(super), num_mig);
		*/

		
		issue_mig_kcopyd(super, use_gc);
		#endif
		//if(use_gc)
		//	release_reserve_segs(super);

		//printk(" bug on ... \n");
		//BUG_ON(1);

		//printk(" End: num mig = %d, job count = %d, active = %d, total = %d (%d%%) free = %d\n", num_mig, atomic_read(&migrate_mgr->copy_job_count),
		//	atomic_read(&super->segbuf_mgr.active_page_count), atomic_read(&super->segbuf_mgr.total_page_count),
		//	atomic_read(&super->segbuf_mgr.active_page_count)*100/atomic_read(&super->segbuf_mgr.total_page_count),
		//	get_alloc_count(super) 
		//	);

		#if SWAN_Q
		while(atomic_read(&migrate_mgr->copy_job_count) && 
				(gc_buf_max_count - get_data_max_count(super, WCBUF)) < 
				atomic_read(&super->segbuf_mgr1.gc_active_page_count)){
		#else
		while(atomic_read(&migrate_mgr->copy_job_count) && 
				(gc_buf_max_count - get_data_max_count(super, WCBUF)) < 
				atomic_read(&super->segbuf_mgr.gc_active_page_count)){
		#endif
			//printk(" free segs = %d, copy job count = %d \n", get_alloc_count(super), 
			//		atomic_read(&migrate_mgr->copy_job_count));
			schedule_timeout_interruptible(usecs_to_jiffies(10));
		}

		while(get_alloc_count(super)<1){
			printk(" no free segs for GC free = %d \n", get_alloc_count(super));
			schedule_timeout_interruptible(msecs_to_jiffies(5000));
		}

		//printk(" Next GC ... inflight num = %d \n", atomic_read(&migrate_mgr->group_job_count));
	}

	//printk(" Finalizing migrate daemon ... \n");
	return 0;
}

#if KH_DAEMON
int migrate_proc2(void *data) {
	printk("Hello, kwang hyun! I am [migrate_proc2]\n");
}
#endif


static void chunk_io_complete(unsigned long error, void *__context)
{
	struct recovery_job *job = __context;
	struct dmsrc_super *super = job->super;
	struct recovery_manager *recovery_mgr = &super->recovery_mgr;
	unsigned long flags;

	if (error)
		job->error = 1;

	if(!atomic_dec_and_test(&job->num_remaining_ios)){
		return;
	}

	spin_lock_irqsave(&recovery_mgr->lock, flags);
	list_add_tail(&job->recovery_list, &recovery_mgr->queue);
	spin_unlock_irqrestore(&recovery_mgr->lock, flags);
	queue_work(recovery_mgr->wq, &recovery_mgr->work);
}

void mark_recover(struct dmsrc_super *super, struct segment_header *seg)
{
	struct metablock *mb;
	int i;

	for(i = 0;i < CHUNK_SZ;i++){
		mb = get_mb(super, seg->seg_id, super->recovery_mgr.failure_ssd*CHUNK_SZ + i);
		if(test_bit(MB_BROKEN, &mb->mb_flags)){
			clear_bit(MB_BROKEN, &mb->mb_flags);
			atomic_dec(&super->recovery_mgr.broken_block_count);
		}
	}

	set_bit(SEG_SEALED, &seg->flags);
	clear_bit(SEG_RECOVERY, &seg->flags);
}

void _chunk_io_async(struct dmsrc_super *super, struct segment_header *seg, struct metablock *mb, 
					 struct recovery_job *context, void *ptr, int rw){
	struct dm_io_region io;
	struct dm_io_request io_req;

	io_req.mem.type = DM_IO_KMEM;
	io_req.mem.ptr.addr = ptr;

	io_req.bi_rw = rw;
	io_req.notify.fn = chunk_io_complete;
	io_req.notify.context = context;
	io_req.client = super->io_client;

	io.bdev = get_bdev(super, mb->idx);
	io.sector = get_sector(super, seg->seg_id, mb->idx);
	io.count = SRC_SECTORS_PER_PAGE;
//	printk(" Read: seg = %d, devno = %d, sector = %d \n", (int)seg->seg_id, (int)get_devno(super, calc_mb_start_sector(wb, seg, mb->idx)), (int)io.sector); 

	dmsrc_io(&io_req, 1, &io, NULL);
}

int row_is_broken(struct dmsrc_super *super, struct segment_header *seg, int failure_ssd, int row){
	struct metablock *mb;
	mb = get_mb(super, seg->seg_id, failure_ssd*CHUNK_SZ + row);
	return test_bit(MB_BROKEN, &mb->mb_flags);
	//return atomic_read(&mb->broken);
}

void chunk_io_async(struct dmsrc_super *super, struct segment_header *seg, struct recovery_job *context,
		struct rambuffer *rambuf)
{
	struct metablock *mb;
	u32 offset;
	u32 mem_offset;
	int i, j;
	int count=0, count2=0;
	struct recovery_manager *recovery_mgr = &super->recovery_mgr;

	atomic_set(&context->num_remaining_ios, 0); // parity write

	for(i = 0;i < NUM_SSD;i++){
		if(context->is_read){
			if(i==recovery_mgr->failure_ssd)
				continue;
		}else{
			if(i!=recovery_mgr->failure_ssd)
				continue;
		}

		for(j = 0;j < CHUNK_SZ;j++){
			if(row_is_broken(super, seg, recovery_mgr->failure_ssd, j)){
				atomic_inc(&context->num_remaining_ios);
				count++;
			}
		}
	}

	for(i = 0;i < NUM_SSD;i++){
		if(context->is_read){
			if(i==recovery_mgr->failure_ssd)
				continue;
		}else{
			if(i!=recovery_mgr->failure_ssd)
				continue;
		}

		for(j = 0;j < CHUNK_SZ;j++){

			if(!row_is_broken(super, seg, recovery_mgr->failure_ssd, j))
				continue;

			count2++;
			offset = i*CHUNK_SZ+j;
			mb = get_mb(super, seg->seg_id, offset);
			mem_offset = offset;
			if(context->is_read){
				_chunk_io_async( super, seg, mb, context, page_address(rambuf->pages[mem_offset]->pl->page), READ);
			}else{
				_chunk_io_async( super, seg, mb, context, page_address(rambuf->pages[mem_offset]->pl->page), WRITE);
			}
		}
	}
	BUG_ON(count!=count2);

	if(count==0){
		//printk(" count = 0 case \n");
		mark_recover(super, context->seg);
		release_rambuffer(super, context->rambuf, seg->seg_type);
		mempool_free(context, recovery_mgr->job_pool);
	}
}


void recovery_segment(struct dmsrc_super *super, struct segment_header *seg, 
		struct recovery_job *context, int is_read){
	struct rambuffer *rambuf;

	if(is_read){
		rambuf = alloc_rambuffer(super, RCVBUF, STRIPE_SZ);
		context->rambuf = rambuf;
		context->super = super;
		context->seg = seg;
	}else{
		rambuf = context->rambuf;
	}

	context->is_read = is_read;

	chunk_io_async(super, seg, context, rambuf);
}

void do_xoring(struct dmsrc_super *super, struct segment_header *seg, 
		struct rambuffer *rambuf){
	int i, j;
	int src_count = 0;
	void *dst=NULL;
	void *srcs[MAX_CACHE_DEVS];

	for(i = 0;i <CHUNK_SZ;i++){
		if(!row_is_broken(super, seg, super->recovery_mgr.failure_ssd, i))
			continue;

		src_count = 0;
		for(j = 0;j < NUM_SSD;j++){
			if(j==super->recovery_mgr.failure_ssd)
				dst = page_address(rambuf->pages[i + (j * CHUNK_SZ)]->pl->page);
			else
				srcs[src_count++] = page_address(rambuf->pages[i + (j * CHUNK_SZ)]->pl->page);
		}
		memset(dst, 0xFF, SRC_PAGE_SIZE);
		//xor_blocks(src_count, PAGE_SIZE, dst, srcs);
		run_xor(srcs, dst, src_count, SRC_PAGE_SIZE);
	}
}

void do_recovery_worker(struct work_struct *work){
	struct recovery_manager *recovery_mgr = container_of(work, struct recovery_manager, work);
	struct dmsrc_super *super = recovery_mgr->super;
	struct list_head recovery_local_head;
	struct recovery_job *job, *job_tmp;
	unsigned long flags;

	INIT_LIST_HEAD(&recovery_local_head);

	spin_lock_irqsave(&recovery_mgr->lock, flags);
	list_for_each_entry_safe(job, job_tmp, &recovery_mgr->queue, recovery_list) {
		list_move_tail(&job->recovery_list, &recovery_local_head);
	}
	spin_unlock_irqrestore(&recovery_mgr->lock, flags);

	list_for_each_entry_safe(job, job_tmp, &recovery_local_head, recovery_list) {
		list_del(&job->recovery_list);

		if(job->is_read){
			//printk(" Xored: seg id = %d, rambuf cnt = %d \n", (int)job->seg->seg_id, atomic_read(&super->rambuf_inactive_count[RCBUF]));
			do_xoring(super, job->seg, job->rambuf);
			recovery_segment(super, job->seg, job, 0);
		}else{
			//printk(" recovered: seg id = %d, rambuf cnt = %d \n", (int)job->seg->seg_id, atomic_read(&super->rambuf_inactive_count[RCBUF]));
			mark_recover(super, job->seg);
			release_rambuffer(super, job->rambuf, RCVBUF);
			mempool_free(job, recovery_mgr->job_pool);
		}
	}

	//printk(" do recovery worker \n");

}


int recovery_proc(void *data)
{
	struct dmsrc_super *super = data;
	struct recovery_manager *recovery_mgr = &super->recovery_mgr;
	struct segment_header *seg;
	struct recovery_job *context;
	int i;
	int percent = -10;
	int temp = 0;
	int total = 0;

	while (!kthread_should_stop()) {
		if(!atomic_read(&super->degraded_mode)){
			schedule_timeout_interruptible(msecs_to_jiffies(1000));
			continue;
		}

		recovery_mgr->start_jiffies = jiffies;
		total = atomic_read(&recovery_mgr->broken_block_count);
		percent = -10;

		for(i = 0;i < super->cache_stat.num_segments && atomic_read(&recovery_mgr->broken_block_count);i++){
			seg = get_segment_header_by_id(super, (u64)i);

			if(!test_bit(SEG_RECOVERY, &seg->flags)){
				continue;
			}

			wait_rambuf_event(super, RCVBUF);

retry:;
			context = mempool_alloc(recovery_mgr->job_pool, GFP_NOIO);
			if(!context){
				printk(" mempool error \n");
				goto retry;

			}

			//printk(" Selected: seg id = %d, rambuf cnt = %d \n", (int)seg->seg_id, atomic_read(&super->rambuf_inactive_count[RCBUF]));

			if(!test_bit(SEG_SEALED, &seg->flags)){
				printk(" segment is not sealed ... \n");
				BUG_ON(1);
			}

			//LOCK(super, f);
			//atomic_set(&seg->in_use, SEG_RECOVERY);
			//set_bit(SEG_RECOVERY, &seg->flags);
			//UNLOCK(super, f);
			recovery_segment(super, seg, context, 1);

			temp = atomic_read(&recovery_mgr->broken_block_count) * 100 / (total+1);
			if(temp%10 == 0 && temp != percent){
				printk(" %d percent blocks are recovered \n", 100-temp);
				percent = temp;
			}
		}

		while( atomic_read(&recovery_mgr->broken_block_count)){
			printk(" Wating recovery proc... broken block = %d\n", 
					(int)atomic_read(&recovery_mgr->broken_block_count));
			schedule_timeout_interruptible(msecs_to_jiffies(1000));
		}

		temp = atomic_read(&recovery_mgr->broken_block_count) * 100 / (total+1);
		if(temp%10 == 0 && temp != percent){
			printk(" %d percent blocks are recovered \n", 100-temp);
			percent = temp;
		}

		recovery_mgr->end_jiffies = jiffies;
		printk(" Recovery Time  = %d ms \n", jiffies_to_msecs(recovery_mgr->end_jiffies
					- recovery_mgr->start_jiffies));
		printk(" END recovery proc... broken block = %d\n", 
				(int)atomic_read(&recovery_mgr->broken_block_count));

		atomic_set(&super->degraded_mode, 0);
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
	}
	return 0;
}


#if 0 
static void update_superblock_record(struct dmsrc_super *super)
{
	int r;
	struct superblock_record_device o;
	void *buf;
	struct dm_io_request io_req;
	struct dm_io_region region;

	o.last_migrated_segment_id =
		cpu_to_le64(atomic64_read(&super->last_migrated_segment_id));

	buf = mempool_alloc(super->buf_1_pool, GFP_NOIO | __GFP_ZERO);
	memcpy(buf, &o, sizeof(o));

	io_req = (struct dm_io_request) {
		.client = super_io_client,
		.bi_rw = WRITE_FUA,
		.notify.fn = NULL,
		.mem.type = DM_IO_KMEM,
		.mem.ptr.addr = buf,
	};
	region = (struct dm_io_region) {
		.bdev = super->cache_dev[0]->bdev,
		.sector = (1 << 11) - 1,
		.count = 1,
	};

#ifdef USE_RAID_FTL
	//memset(buf, 0x00, 8);
#endif 
	IO(dmsrc_io(&io_req, 1, &region, NULL, 0));
	mempool_free(buf, super->buf_1_pool);
}
#endif 

#if 0 
int recorder_proc(void *data)
{
	struct dmsrc_super *super = data;

	unsigned long intvl;

	while (!kthread_should_stop()) {
		//stop_on_dead();

		/* sec -> ms */
		intvl = ACCESS_ONCE(super->param.update_record_interval) * 1000;

		if (!intvl) {
			schedule_timeout_interruptible(msecs_to_jiffies(1000));
			continue;
		}

		//update_superblock_record(super);

		schedule_timeout_interruptible(msecs_to_jiffies(intvl));

		if(atomic64_read(&super->pending_io_count))
			queue_work(super->pending_wq, &super->pending_work);

		//printk(" update super .. \n");
		//queue_work(super->read_caching_wq, &wb->read_caching_work);
	}
	return 0;
}
#endif

int checker_proc(void *data)
{
	struct dmsrc_super *super = data;
	unsigned long intvl;
	//int i;
	int j;
	unsigned long f;
	unsigned long flags;
	//struct segment_header *seg;
	struct segment_allocator *seg_allocator = &super->seg_allocator;

	#if SWAN_READ_BLK_GC
	int idx;
	#endif


	while (!kthread_should_stop()) {

		/* sec -> ms */
		intvl = ACCESS_ONCE(super->param.checker_interval) * 1000;
		if (!intvl) {
			schedule_timeout_interruptible(msecs_to_jiffies(1000));
			continue;
		}
		schedule_timeout_interruptible(msecs_to_jiffies(intvl));


		printk("---------------------------------\n");

		printk(" Current Total Bandwidth = %d MB/s\n", wp_get_iops(super, NULL, REQ_CATEGORY_TOTAL, REQ_TYPE_TOTAL)/256);
		printk(" Current Write Bandwidth = %d MB/s\n", wp_get_iops(super, NULL, REQ_CATEGORY_NORMAL, REQ_TYPE_WRITE)/256);
		printk(" Current GC Bandwidth = %d MB/s\n", wp_get_iops(super, NULL, REQ_CATEGORY_GC, REQ_TYPE_WRITE)/256);

		printk(" Checker live inflight ios = %d \n", atomic_read(&super->cache_stat.inflight_ios));
		printk(" Checker live inflight bios = %d \n", atomic_read(&super->cache_stat.inflight_bios));

		printk("free_seg : %d \n", (int)atomic_read(&seg_allocator->seg_alloc_count) );
                printk("free_grp : %d \n", (int)atomic_read(&seg_allocator->group_alloc_count) );

		/*
		printk(" Checker live total ios = %d \n", atomic_read(&super->cache_stat.total_ios));
		printk(" Checker live total bios = %d \n", atomic_read(&super->cache_stat.total_bios));
		printk(" Checker live total bios2 = %d \n", atomic_read(&super->cache_stat.total_bios2));*/
		//unsigned long f;

		//stop_on_dead();

		//printk(" Log %d ...  \n", i);
		#if 0


		#if SWAN_READ_BLK_GC

		printk("num_read_inflight_in_col: \n");
                for (idx = 0; idx < NUM_PHY_COL; idx++) {
                        printk("[col %u] %llu\n", idx, atomic64_read(&super->num_read_inflight_in_col[idx]));
                }

		#endif

		spin_lock_irqsave(&seg_allocator->alloc_lock, flags);

		//printk("free_seg : %d \n", (int)atomic_read(&seg_allocator->seg_alloc_count) );

		if (((int)atomic_read(&seg_allocator->seg_alloc_count) + 
			(int)seg_allocator->seg_used_count +
			(int)seg_allocator->seg_sealed_count +
			(int)atomic_read(&seg_allocator->seg_migrate_count)) != 0) {
	

		printk("free_seg : %d%% (%d/%d) \n", 
		(int)atomic_read(&seg_allocator->seg_alloc_count)*100 / 
			((int)atomic_read(&seg_allocator->seg_alloc_count) + 
			(int)seg_allocator->seg_used_count +
			(int)seg_allocator->seg_sealed_count +
			(int)atomic_read(&seg_allocator->seg_migrate_count)),
			(int)atomic_read(&seg_allocator->seg_alloc_count),
			((int)atomic_read(&seg_allocator->seg_alloc_count) + 
			(int)seg_allocator->seg_used_count +
			(int)seg_allocator->seg_sealed_count +
			(int)atomic_read(&seg_allocator->seg_migrate_count))
		);

		}


		spin_unlock_irqrestore(&seg_allocator->alloc_lock, flags);


		if(atomic64_read(&super->wstat.victim_count)){
			printk("average segment victim util : %d%%\n", 
					(int)(atomic64_read(&super->wstat.victim_util)/
						atomic64_read(&super->wstat.victim_count)));

		}


		#else
		printk(" current utilization = %d%%(%d/%d) \n", get_curr_util(super),
				(int)NUM_USED_BLOCKS, (int)NUM_BLOCKS);
		#endif
		//printk(" read seg inflight = %d \n", atomic_read(&super->current_seg[RBUF]->num_read_inflight));
		//printk(" write  seg inflight = %d \n", atomic_read(&super->current_seg[WBUF]->num_read_inflight));
		//printk(" gread seg inflight = %d \n", atomic_read(&super->current_seg[GRBUF]->num_read_inflight));
		//printk(" gwrite  seg inflight = %d \n", atomic_read(&super->current_seg[GWBUF]->num_read_inflight));

#if 1
		print_alloc_queue(super);

		LOCK(super, f);
		
		if(super->segbuf_mgr.current_rambuf[WCBUF]) {
			printk(" bios total start = %d, count = %d, refcount = %d \n", 
			atomic_read(&super->segbuf_mgr.current_rambuf[WCBUF]->bios_total_start),
			atomic_read(&super->segbuf_mgr.current_rambuf[WCBUF]->bios_total_count),
			atomic_read(&super->segbuf_mgr.current_rambuf[WCBUF]->ref_count));
		#if 0
			{
				int i;
				for(i = 0;i < NUM_SSD;i++)
					check_plug_proc(super, 
							super->current_seg[WCBUF], 
							super->segbuf_mgr.current_rambuf[WCBUF], 
							1, i);

			}
		#endif
		}
		
		
		UNLOCK(super, f);
#endif

		if(super->wstat.read_count && super->wstat.write_count){
			printk(" hit ratio = %d  rhit = %d, whit = %d\n", 
					super->wstat.hit * 100 / super->wstat.count,
					super->wstat.read_hit * 100 / super->wstat.read_count,
					super->wstat.write_hit * 100 / super->wstat.write_count
				  );
			printk(" cold bypass = %dMB, seq bypass = %dMB \n", 
					atomic_read(&super->wstat.cold_bypass_count)/256, 
					atomic_read(&super->wstat.seq_bypass_count)/256);
		}
		printk(" used = %u, total = %u \n", atomic_read(&super->cache_stat.num_used_blocks), NUM_BLOCKS);
		printk(" gc empty count = %u\n", atomic_read(&super->wstat.gc_empty_count));
		printk(" gc job count = %u, destage job count = %d \n", atomic_read(&super->wstat.gc_count), atomic_read(&super->wstat.destage_count));
		printk(" gc io count = %lu, destage io count = %lu \n", atomic64_read(&super->wstat.gc_io_count), atomic64_read(&super->wstat.destage_io_count));
		printk(" pending io count = %d\n", (int)atomic64_read(&super->pending_mgr.io_count));


		if(atomic64_read(&super->wstat.victim_count)){
			printk(" Average Victim Util = %d\n", 
					(int)(atomic64_read(&super->wstat.victim_util)/
						atomic64_read(&super->wstat.victim_count)));

		}

		printk(" partial seg writes = %d \n", atomic_read(&super->wstat.partial_write_count));
		printk(" bypass writes = %dMB/s \n", atomic_read(&super->wstat.bypass_write_count)/256);
		if(atomic64_read(&super->wstat.average_arrival_time))
			printk(" Average Arrival Time = %luus \n", 
				atomic64_read(&super->wstat.average_arrival_time)/
				atomic64_read(&super->wstat.average_arrival_time));

        #ifdef TRACE_GC
                printk("### normal io cnt[0]: %lu  normal io cnt[1]: %lu\n",
                        atomic64_read(&super->wstat.normal_io_cnt[0]),
                        atomic64_read(&super->wstat.normal_io_cnt[1]));
                printk("### gc read cnt: %lu  gc write cnt: %lu\n",
                        atomic64_read(&super->wstat.gc_io_cnt[READ]),
                        atomic64_read(&super->wstat.gc_io_cnt[WRITE]));
        #endif

		//printk(" seg count: WCBUF %d, WHBUF %d, RCBUF %d, RHBUF %d, GWBUF %d, GRBUF %d\n", 
		//		atomic_read(&super->wstat.seg_count[WCBUF]),
		//		atomic_read(&super->wstat.seg_count[WHBUF]),
		//		atomic_read(&super->wstat.seg_count[RCBUF]),
		//		atomic_read(&super->wstat.seg_count[RHBUF]),
		//		atomic_read(&super->wstat.seg_count[GWBUF]),
		//		atomic_read(&super->wstat.seg_count[GRBUF]));

		//if(atomic64_read(&super->pending_mgr.io_count))
		//	queue_work(super->pending_mgr.pending_wq, &super->pending_mgr.pending_work);

		queue_work(super->migrate_mgr.mig_wq, &super->migrate_mgr.mig_work);

		pending_worker_schedule(super); //[kh3]

		//	printk(" Wating recovery proc... broken block = %d\n", 
		//				(int)atomic_read(&super->broken_block_count));
		//	printk(" recovery rambuf = %d \n", 
		//			atomic_read(&super->rambuf_inactive_count));
		printk(" mig seg inflights = %d, complete = %d \n", 
				atomic_read(&super->migrate_mgr.mig_inflights),
				atomic_read(&super->migrate_mgr.mig_completes));

		//	printk(" Rambuf inactive count = %d \n", (int)atomic_read(&super->rambuf_inactive_count));
		printk(" rambuf page Active count = %d (%d MB) \n", (int)atomic_read(&super->segbuf_mgr.active_page_count), 
				(int)atomic_read(&super->segbuf_mgr.active_page_count)/256);
		printk(" rambuf page Inactive count = %d (%d MB) \n", (int)atomic_read(&super->segbuf_mgr.inactive_page_count), 
				(int)atomic_read(&super->segbuf_mgr.inactive_page_count)/256);
		printk(" plug queue = %d \n", (int)atomic_read(&super->plugger.total_length));

		for(j = 0;j < NBUF;j++){
			printk(" [%d]%d ", j, atomic_read(&super->segbuf_mgr.active_count[j]));
		}
		printk("\n");
		//check_rambuf_pool(super);
		//segment_group_print_stat(super);
		if(super->param.enable_read_cache){
			printk(" clean dram buffer: free = %d, count = %d, sealed = %dMB\n", 
				super->clean_dram_cache_manager->cm_free/256, 
				super->clean_dram_cache_manager->cm_count/256, 
				atomic_read(&super->clean_dram_cache_manager->cm_sealed_count)/256);
		}

		printk("\n");
		printk("---------------------------------\n");
	}
	return 0;
}


#if 0 
void schedule_sync_proc(unsigned long data)
{
	struct dmsrc_super *super = (struct dmsrc_super *) data;
#if 0
	schedule_work(&super->sync_mgr.work);
#else
//	unsigned long flags;
//	unsigned long target_jiffies;

//	spin_lock_irqsave(&sync_mgr->lock, flags);
//	target_jiffies = sync_mgr->target_jiffies; 
//	spin_unlock_irqrestore(&sync_mgr->lock, flags);

//	printk(" sync proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
//		 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));
	//printk(" Checker live inflight ios = %d \n", atomic_read(&super->cache_stat.inflight_ios));
	///printk(" Checker live inflight bios = %d \n", atomic_read(&super->cache_stat.inflight_bios));
	//printk(" Checker live total bios = %d \n", atomic_read(&super->cache_stat.total_bios));
	//printk(" Checker live total bios2 = %d \n", atomic_read(&super->cache_stat.total_bios2));
	//check_rambuf_pool(super);
	//printk(" sync proc in timer ... \n");

	if(!atomic_read(&super->migrate_mgr.migrate_triggered)){
		flush_partial_meta(super, WHBUF);
		flush_partial_meta(super, WCBUF);
	}

#endif 
}
#endif

#if SWAN
void sync_proc2(struct work_struct *work)
{
	struct sync_manager *sync_mgr = container_of(work, struct sync_manager, work);
	struct dmsrc_super *super = sync_mgr->super;
//	unsigned long flags;
//	unsigned long target_jiffies;

//	spin_lock_irqsave(&sync_mgr->lock, flags);
//	target_jiffies = sync_mgr->target_jiffies; 
//	spin_unlock_irqrestore(&sync_mgr->lock, flags);

	//printk(" sync proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
	//	 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));
	//printk(" Checker live inflight ios = %d \n", atomic_read(&super->cache_stat.inflight_ios));
	///printk(" Checker live inflight bios = %d \n", atomic_read(&super->cache_stat.inflight_bios));
	//printk(" Checker live total bios = %d \n", atomic_read(&super->cache_stat.total_bios));
	//printk(" Checker live total bios2 = %d \n", atomic_read(&super->cache_stat.total_bios2));
	//check_rambuf_pool(super);


	#if SWAN
	flush_partial_meta(super, SWAN_GC_CACHE_HOT);
	
	#else
	if(!atomic_read(&super->migrate_mgr.migrate_triggered)){
//		flush_partial_meta(super, WHBUF);
		flush_partial_meta(super, WCBUF);
	}
	#endif

	//printk(" end proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
	//	 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));

	return;

	//spin_lock_irqsave(&sync_mgr->lock, flags);
	//sync_mgr->last_jiffies= jiffies;
	//spin_unlock_irqrestore(&sync_mgr->lock, flags);
}

void sync_proc1(struct work_struct *work)
{
	struct sync_manager *sync_mgr = container_of(work, struct sync_manager, work);
	struct dmsrc_super *super = sync_mgr->super;
//	unsigned long flags;
//	unsigned long target_jiffies;

//	spin_lock_irqsave(&sync_mgr->lock, flags);
//	target_jiffies = sync_mgr->target_jiffies; 
//	spin_unlock_irqrestore(&sync_mgr->lock, flags);

	//printk(" sync proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
	//	 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));
	//printk(" Checker live inflight ios = %d \n", atomic_read(&super->cache_stat.inflight_ios));
	///printk(" Checker live inflight bios = %d \n", atomic_read(&super->cache_stat.inflight_bios));
	//printk(" Checker live total bios = %d \n", atomic_read(&super->cache_stat.total_bios));
	//printk(" Checker live total bios2 = %d \n", atomic_read(&super->cache_stat.total_bios2));
	//check_rambuf_pool(super);


	#if SWAN
	flush_partial_meta(super, SWAN_GC_CACHE_COLD);
	
	#else
	if(!atomic_read(&super->migrate_mgr.migrate_triggered)){
//		flush_partial_meta(super, WHBUF);
		flush_partial_meta(super, WCBUF);
	}
	#endif

	//printk(" end proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
	//	 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));

	return;

	//spin_lock_irqsave(&sync_mgr->lock, flags);
	//sync_mgr->last_jiffies= jiffies;
	//spin_unlock_irqrestore(&sync_mgr->lock, flags);
}



void sync_proc(struct work_struct *work)
{
	//printk("[sync_proc1]\n");
	struct sync_manager *sync_mgr = container_of(work, struct sync_manager, work);
	struct dmsrc_super *super = sync_mgr->super;
//	unsigned long flags;
//	unsigned long target_jiffies;

//	spin_lock_irqsave(&sync_mgr->lock, flags);
//	target_jiffies = sync_mgr->target_jiffies; 
//	spin_unlock_irqrestore(&sync_mgr->lock, flags);

	//printk(" sync proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
	//	 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));
	//printk(" Checker live inflight ios = %d \n", atomic_read(&super->cache_stat.inflight_ios));
	///printk(" Checker live inflight bios = %d \n", atomic_read(&super->cache_stat.inflight_bios));
	//printk(" Checker live total bios = %d \n", atomic_read(&super->cache_stat.total_bios));
	//printk(" Checker live total bios2 = %d \n", atomic_read(&super->cache_stat.total_bios2));
	//check_rambuf_pool(super);


	#if SWAN
	flush_partial_meta(super, WCBUF);
	
	#else
	if(!atomic_read(&super->migrate_mgr.migrate_triggered)){
//		flush_partial_meta(super, WHBUF);
		flush_partial_meta(super, WCBUF);
	}
	#endif

	//printk(" end proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
	//	 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));

	return;

	//spin_lock_irqsave(&sync_mgr->lock, flags);
	//sync_mgr->last_jiffies= jiffies;
	//spin_unlock_irqrestore(&sync_mgr->lock, flags);
}

#else

void sync_proc(struct work_struct *work)
{
	struct sync_manager *sync_mgr = container_of(work, struct sync_manager, work);
	struct dmsrc_super *super = sync_mgr->super;
//	unsigned long flags;
//	unsigned long target_jiffies;

//	spin_lock_irqsave(&sync_mgr->lock, flags);
//	target_jiffies = sync_mgr->target_jiffies; 
//	spin_unlock_irqrestore(&sync_mgr->lock, flags);

	//printk(" sync proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
	//	 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));
	//printk(" Checker live inflight ios = %d \n", atomic_read(&super->cache_stat.inflight_ios));
	///printk(" Checker live inflight bios = %d \n", atomic_read(&super->cache_stat.inflight_bios));
	//printk(" Checker live total bios = %d \n", atomic_read(&super->cache_stat.total_bios));
	//printk(" Checker live total bios2 = %d \n", atomic_read(&super->cache_stat.total_bios2));
	//check_rambuf_pool(super);


	#if 0
	flush_partial_meta(super, WCBUF);
	flush_partial_meta(super, SWAN_GC_CACHE_HOT);
	flush_partial_meta(super, SWAN_GC_CACHE_COLD);
	
	#else
	if(!atomic_read(&super->migrate_mgr.migrate_triggered)){
//		flush_partial_meta(super, WHBUF);
		flush_partial_meta(super, WCBUF);
	}
	#endif

	//printk(" end proc ... %d  %d  %d\n", jiffies_to_msecs(target_jiffies), jiffies_to_msecs(jiffies),
	//	 jiffies_to_msecs(target_jiffies) - jiffies_to_msecs(jiffies));

	return;

	//spin_lock_irqsave(&sync_mgr->lock, flags);
	//sync_mgr->last_jiffies= jiffies;
	//spin_unlock_irqrestore(&sync_mgr->lock, flags);
}

#endif

#if 0 
void readdone_noirq(int *dst_count, struct dm_io_region *dst,
						void *context, struct page_list *pages) 
{
	struct copy_job *cp_job = (struct copy_job *) context;
	struct mig_job *mg_job = cp_job->mg_job;
	struct dmsrc_super *super = mg_job->wb;
	struct segment_header *seg;
	struct rambuffer *rambuf;
	unsigned int mb_idx;

	struct page *page = pages->page;
	void *src = kmap_atomic(page);

	seg = get_seg_by_mb(super, cp_job->dst_mb);
	mb_idx = cp_job->dst_mb->idx;
	rambuf = cp_job->dst_rambuf;

	update_data_in_mb(super, mb_idx, seg, rambuf, NULL, seg->seg_type, src);

	kunmap_atomic(src);
}
#endif 
