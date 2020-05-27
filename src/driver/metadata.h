/****************************************************************************
 * SWAN (Serial management With an Array of solid state drives on Network): 
 * Serial Management of All Flash Array for Sustained Garbage Collection Free High Performance 
 * Jaeho Kim (kjhnet@gmail.com), K. Hyun Lim (limkh4343@gmail.com) 2016 - 2018
 * filename: metadata.h 
 * 
 * Based on DM-Writeboost:
 *   1) SRC (SSD RAID Cache): Device mapper target for block-level disk caching
 *      Copyright (C) 2013-2014 Yongseok Oh (ysoh@uos.ac.kr)
 *   2) Log-structured Caching for Linux
 *      Copyright (C) 2012-2013 Akira Hayakawa <ruby.wktk@gmail.com>
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

#ifndef DM_SRC_METADATA_H
#define DM_SRC_METADATA_H

#define VMALLOC
//#define KMALLOC

struct part {
	void *memory;
};

struct large_array {
	struct part *parts;
	u64 num_elems;
	u32 elemsize;
};
#define ALLOC_SIZE (1 << 16)


/*----------------------------------------------------------------*/

//struct metablock *mb_at(struct dmsrc_super *super, u32 idx);
struct metablock *mb_at(struct dmsrc_super *super, u64 idx);	// Large SSD
struct segment_header *get_segment_header_by_id(struct dmsrc_super *,
						u64 segment_id);
struct segment_header *get_segment_header_by_mb_idx(struct dmsrc_super *super,
							   u32 mb_idx);
sector_t calc_mb_start_sector(struct dmsrc_super *,
			      struct segment_header *, u32 mb_idx);
bool is_on_buffer(struct dmsrc_super *, u32 mb_idx);

u32 get_curr_util(struct dmsrc_super *);

/*----------------------------------------------------------------*/

struct ht_head *ht_get_head(struct dmsrc_super *, sector_t);
struct metablock *ht_lookup(struct dmsrc_super *, sector_t);
void ht_register(struct dmsrc_super *, sector_t, struct metablock *);
void ht_del(struct dmsrc_super *, struct metablock *);
void discard_caches_inseg(struct dmsrc_super *, struct segment_header *);

/*----------------------------------------------------------------*/

int __must_check scan_superblock(struct dmsrc_super *);
int __must_check format_cache_device(struct dmsrc_super *);

/*----------------------------------------------------------------*/

void prepare_segment_header_device(struct segment_header_device *dest,
				   struct dmsrc_super *,
				   struct segment_header *src, int cache_type, struct rambuf_page **rambuf);

u8 calc_checksum(u8 *ptr, int size);
/*----------------------------------------------------------------*/

int alloc_migration_buffer(struct dmsrc_super *, size_t num_batch);
void free_migration_buffer(struct dmsrc_super *);

/*----------------------------------------------------------------*/

int __must_check resume_cache(struct dmsrc_super *);
void free_cache(struct dmsrc_super *);

/*----------------------------------------------------------------*/


#if SWAN_Q2

void insert_seg_to_alloc_queue(struct dmsrc_super *super, struct segment_header *seg, struct col_queue *col_Q); //construct_metadata_in_ram
void insert_seg_to_used_queue(struct dmsrc_super *super, struct segment_header *seg, struct col_queue *col_Q); //alloc_new_segment, construct_metadata_in_ram

void insert_group_to_used_queue(struct dmsrc_super *super, struct group_header *group, struct col_queue *col_Q); //_alloc_new_seg, construct_metadata_in_ram
struct segment_header *remove_alloc_queue(struct dmsrc_super *super, struct segment_header *seg, struct col_queue *col_Q); //_alloc_new_seg 
void move_seg_used_to_sealed_queue(struct dmsrc_super *super, struct segment_header *seg, struct col_queue *col_Q); //change_seg_status, construct_metadata_in_ram
void move_seg_sealed_to_migrate_queue(struct dmsrc_super *super, struct segment_header *seg, int lock, struct col_queue *col_Q);//_select_victim_for_gc

void move_seg_migrate_to_alloc_queue(struct dmsrc_super *super, struct segment_header *seg, struct col_queue *col_Q);//finalize_clean_seg
void move_seg_migrate_to_sealed_queue(struct dmsrc_super *super, struct segment_header *seg, struct col_queue *col_Q);//release_victim_seg

struct group_header *remove_alloc_group_queue(struct dmsrc_super *super, struct col_queue *col_Q, int cache_type);//_alloc_new_seg

void move_group_used_to_sealed_queue(struct dmsrc_super *super, struct group_header *group, struct col_queue *col_Q); //change_seg_status, construct_metadata_in_ram

void move_group_sealed_to_migrate_queue(struct dmsrc_super *super, struct group_header *group, int lock, struct col_queue *col_Q);//select_victim_for_gc, select_victim_for_gc2
void move_group_migrate_to_alloc_queue(struct dmsrc_super *super, struct group_header *group, struct col_queue *col_Q);//finalize_clean_seg

int empty_alloc_queue(struct dmsrc_super *super); //not used
int empty_sealed_queue(struct dmsrc_super *super);//not used

//void move_seg_mru_sealed(struct dmsrc_super *super, struct segment_header *seg, struct col_queue *col_Q);//not used

void print_alloc_queue(struct dmsrc_super *super);
//void print_alloc_queue(struct dmsrc_super *super, struct col_queue *col_Q);
//int get_alloc_count(struct dmsrc_super *super, struct col_queue *col_Q);
int get_alloc_count(struct dmsrc_super *super);
int get_group_alloc_count(struct dmsrc_super *super);
//int get_group_alloc_count(struct dmsrc_super *super, struct col_queue *col_Q);

//Please revise functions in _alloc_new_seg().!! -KH
//Please make a print function by refering print_alloc_queue in check_proc! -KH
//In the function, _alloc_new_seg(), please cope with the case that the selected group is NULL -KH

#else
void insert_seg_to_alloc_queue(struct dmsrc_super *super, struct segment_header *seg);
void insert_seg_to_used_queue(struct dmsrc_super *super, struct segment_header *seg);
void move_seg_used_to_sealed_queue(struct dmsrc_super *super, struct segment_header *seg);
void move_seg_sealed_to_migrate_queue(struct dmsrc_super *super, struct segment_header *seg, int lock);
void move_seg_migrate_to_alloc_queue(struct dmsrc_super *super, struct segment_header *seg);
struct segment_header *remove_alloc_queue(struct dmsrc_super *super, struct segment_header *seg);
int empty_sealed_queue(struct dmsrc_super *super);
int empty_alloc_queue(struct dmsrc_super *super);
int get_alloc_count(struct dmsrc_super *super);
#endif

#if SWAN 
int get_grp_alloc_count_of_gc_read_col(struct dmsrc_super *super);
int get_grp_alloc_count_of_gc_cold_write_col(struct dmsrc_super *super);
int get_grp_alloc_count_of_gc_hot_write_col(struct dmsrc_super *super);
int get_grp_alloc_count_of_req_write_col(struct dmsrc_super *super);
#endif
#if SWAN_GRP_GRANULARITY
int get_grp_sealed_count_of_gc_read_col(struct dmsrc_super *super);
#endif

void move_seg_mru_sealed(struct dmsrc_super *super, struct segment_header *seg);
void release_reserve_segs(struct dmsrc_super *super);
int reserve_reserve_segs(struct dmsrc_super *super, int need_segs);
struct segment_header *remove_reserve_queue(struct dmsrc_super *super);


//Rambuf
#if SWAN_Q

struct rambuffer *alloc_rambuffer(struct dmsrc_super *super, int cache_type, int stripe_size); //alloc_new_segment
void release_rambuffer(struct dmsrc_super *super, struct rambuffer *, int cache_type);//writeback_endio_extent
//-------------
void alloc_rambuf_page(struct dmsrc_super *super, struct rambuffer *rambuf, int cache_type, struct segbuf_manager *segbuf_mgr); //alloc_rambuffer
void free_rambuf_page(struct dmsrc_super *super, struct rambuffer *rambuf, struct segbuf_manager *segbuf_mgr); //// release_rambuffer
////-------------
struct rambuf_page *_alloc_rambuf_page(struct dmsrc_super *super, struct segbuf_manager *segbuf_mgr); //alloc a page in rambuf,,: alloc_summary_rambuf, alloc_rambuf_page
void alloc_summary_rambuf(struct dmsrc_super *super, struct rambuffer *rambuf, struct segbuf_manager *segbuf_mgr);////alloc_rambuf_page 
struct rambuf_page *_free_rambuf_page(struct dmsrc_super *super, struct rambuf_page *page, struct segbuf_manager *segbuf_mgr); //free_rambuf_page
////-------------
////
////--------------
struct rambuf_page *alloc_single_page(struct dmsrc_super *super);
//, struct segbuf_manager *segbuf_mgr); //__issue_mig_kcopyd
void free_single_page(struct dmsrc_super *super, struct rambuf_page *page);
//, struct segbuf_manager *segbuf_mgr); //generate_gc_write, generate_destage_write
////--------------
struct rambuf_page *_alloc_rambuf_page_for_gc_read(struct dmsrc_super *super, struct segbuf_manager *segbuf_mgr); //alloc_single_page
struct rambuf_page *_free_rambuf_page_for_gc_read(struct dmsrc_super *super, struct rambuf_page *page, struct segbuf_manager *segbuf_mgr); //free_single_page

#else

struct rambuffer *alloc_rambuffer(struct dmsrc_super *super, int cache_type, int stripe_size);
void release_rambuffer(struct dmsrc_super *super, struct rambuffer *, int cache_type);
void alloc_rambuf_page(struct dmsrc_super *super, struct rambuffer *rambuf, int cache_type);
struct rambuf_page *_alloc_rambuf_page(struct dmsrc_super *super);
struct rambuf_page *_free_rambuf_page(struct dmsrc_super *super, struct rambuf_page *page);
struct rambuf_page *alloc_single_page(struct dmsrc_super *super);
void free_single_page(struct dmsrc_super *super, struct rambuf_page *page);

#endif

int seg_stat(struct segment_header *seg);
//struct metablock *get_mb(struct dmsrc_super *super, u32 seg_id, u32 idx);
struct metablock *get_mb(struct dmsrc_super *super, u32 seg_id, u64 idx);	// Large SSD
struct large_array *large_array_alloc(u32 elemsize, u64 num_elems);

//void *large_array_at(struct large_array *arr, u32 i);
void *large_array_at(struct large_array *arr, u64 i);	// Large SSD
void large_array_free(struct large_array *arr);
struct large_array *large_array_alloc(u32 elemsize, u64 num_elems);
int create_daemon(struct dmsrc_super *super, struct task_struct **taskp, int (*threadfn)(void *data), char *name);
u32 calc_num_chunks(struct dm_dev *dev, struct dmsrc_super *super);
int __must_check scan_metadata(struct dmsrc_super *super);
int __must_check resume_managers(struct dmsrc_super *super);
int __must_check init_rambuf_pool(struct dmsrc_super *super);
int __must_check init_segment_header_array(struct dmsrc_super *super);
int __must_check ht_empty_init(struct dmsrc_super *super);
void free_ht(struct dmsrc_super *super);
void free_segment_header_array(struct dmsrc_super *super);
void free_rambuf_pool(struct dmsrc_super *super);
int check_dirty_count(struct dmsrc_super *super, struct segment_header *cur_seg);
int check_valid_count(struct dmsrc_super *super, struct segment_header *cur_seg);
int check_rambuf_pool(struct dmsrc_super *super);

u32 segment_group_inc_free_segs(struct dmsrc_super *super, struct segment_header *seg);
void segment_group_dec_free_segs(struct dmsrc_super *super, struct segment_header *seg);
void segment_group_print_stat(struct dmsrc_super *super);
#if SWAN
struct group_header *remove_alloc_group_queue_from_gc_hot_write_col(struct dmsrc_super *super);
struct group_header *remove_alloc_group_queue_from_gc_cold_write_col(struct dmsrc_super *super);
#endif
#if SWAN_GRP_GRANULARITY
int select_gc_cache_type_grp(struct dmsrc_super *super, struct group_header *group);
#endif
#if SWAN_SEG_GRANULARITY
int select_gc_cache_type_seg(struct dmsrc_super *super, struct segment_header *seg);
#endif
int assign_new_segment(struct dmsrc_super *super);


#endif
