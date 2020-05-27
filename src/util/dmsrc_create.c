/*
 * Copyright (c) 2010, Facebook, Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * Neither the name Facebook nor the names of its contributors may be used to
 * endorse or promote products derived from this software without specific
 * prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <ctype.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <sys/time.h>
//#include <glib.h>
#include "header.h"

//#include <flashcache.h>
//typedef long unsigned int sector_t;
#define FLASHCACHE_WRITE_BACK		0
#define FLASHCACHE_WRITE_THROUGH	1
#define FLASHCACHE_WRITE_AROUND		2
#define	CACHE_MD_STATE_DIRTY		0
#define CACHE_MD_STATE_CLEAN 		1
#define CACHE_MD_STATE_FASTCLEAN	2
#define CACHE_MD_STATE_UNSTABLE		3

#define MAX_SSD	16

struct flash_superblock{
	int cache_sb_state;
};

#undef COMMIT_REV


char *pname;
//char buf[512];
char dmsetup_cmd[8192];
int verbose = 0;

static sector_t
get_chunk_size(char *s)
{
	sector_t size;
	char *c;
	
	size = strtoll(s, NULL, 0);
	for (c = s; isdigit(*c); c++)
		;
	switch (*c) {
		case '\0': 
			break;
		case 'k':
		case 'K':
			size = (size * 1024) / 512;
			break;
		default:
			fprintf (stderr, "%s: Unknown block size type %c\n", pname, *c);
			exit (1);
	}
	//if (size & (size - 1)) {
	//	fprintf(stderr, "%s: Block size must be a power of 2\n", pname);
	//	exit(1);
	//}
	return size;
}

static sector_t
get_block_size(char *s)
{
	sector_t size;
	char *c;
	
	size = strtoll(s, NULL, 0);
	for (c = s; isdigit(*c); c++)
		;
	switch (*c) {
		case '\0': 
			break;
		case 'k':
		case 'K':
			size = (size * 1024) / 512;
			break;
		default:
			fprintf (stderr, "%s: Unknown block size type %c\n", pname, *c);
			exit (1);
	}
	if (size & (size - 1)) {
		fprintf(stderr, "%s: Block size must be a power of 2\n", pname);
		exit(1);
	}
	return size;
}

static sector_t
get_cache_size(char *s)
{
	sector_t size;
	char *c;
	
	size = strtoll(s, NULL, 0);
	for (c = s; isdigit (*c); c++)
		;
	switch (*c) {
		case '\0': 
			break;
		case 'k':
			size = (size * 1024) / 512;
			break;
		case 'm':
		case 'M':
			size = (size * 1024 * 1024) / 512;
			break;
		case 'g': 
		case 'G': 
			size = (size * 1024 * 1024 * 1024) / 512;
			break;
		case 't': 
		case 'T': 
			/* Cache size in terabytes?  You lucky people! */
			size = (size * 1024 * 1024 * 1024 * 1024) / 512;
			break;
		default:
			fprintf (stderr, "%s: Unknown cache size type %c\n", pname, *c);
			exit (1);
	}
	return size;
}

static int 
module_loaded(void)
{
	FILE *fp;
	char line[8192];
	int found = 0;
	
	fp = fopen("/proc/modules", "ro");
	while (fgets(line, 8190, fp)) {
		char *s;
		
		s = strtok(line, " ");
		if (!strcmp(s, "flashcache")) {
			found = 1;
			break;
		}
	}
	fclose(fp);
	return found;
}

static void
load_module(void)
{
	FILE *fp;
	char line[8192];

	if (!module_loaded()) {
		if (verbose)
			fprintf(stderr, "Loading Flashcache Module\n");
		system("modprobe flashcache");
		if (!module_loaded()) {
			fprintf(stderr, "Could not load Flashcache Module\n");
			exit(1);
		}
	} else if (verbose)
			fprintf(stderr, "Flashcache Module already loaded\n");
	fp = fopen("/proc/flashcache/flashcache_version", "ro");
	fgets(line, 8190, fp);
	if (fgets(line, 8190, fp)) {
		if (verbose)
			fprintf(stderr, "version string \"%s\"\n", line);
#ifdef COMMIT_REV
		if (!strstr(line, COMMIT_REV)) {
			fprintf(stderr, "Flashcache revision doesn't match tool revision.\n");
			exit(1);
		}
#endif
	}
	fclose(fp);
}

static void 
check_sure(void)
{
	char input;

	fprintf(stderr, "Are you sure you want to proceed ? (y/n): ");
	scanf("%c", &input);
	printf("\n");
	if (input != 'y') {
		fprintf(stderr, "Exiting FlashCache creation\n");
		exit(1);
	}
}

unsigned int 
gen_uuid(){
	struct timeval tv;
	gettimeofday(&tv, NULL);
	//srand(tv.tv_usec + tv.tv_sec * 1000000);
	return tv.tv_sec;
}

void
usage(char *pname)
{
	//fprintf(stderr, "Usage: %s [-v] [-p back|thru|around] [-b block size] [-m md block size] [-s cache size] [-n num SSDs] cachedev ssd_devname disk_devname\n", pname);
	fprintf(stderr, "\nUsage: %s [-v] [-n # of SSDs] ssd_devname disk_devname\n", pname);
	fprintf(stderr, "(Example : ./dmsrc_create -n 4 /dev/sdb1 /dev/sdd1 /dev/sde1 /dev/sdf1 /dev/sdc1)\n");
	fprintf(stderr, "\nOptions:\n");

	fprintf(stderr, "\n[-f Flush Command] \n");
	fprintf(stderr, "    none: no flush command\n");
	fprintf(stderr, "    fine: flush command when segment write \n");
	fprintf(stderr, "    coarse: flush command when segment group write \n");

	fprintf(stderr, "\n[-k Per SSD Cache Size in GB or MB]\n");
	fprintf(stderr, "    16GB\n");

	fprintf(stderr, "\n[-g Chunk Group Size in MB]\n");
	fprintf(stderr, "    256M\n");

	fprintf(stderr, "\n[-c Chunk Size in KB]\n");
	fprintf(stderr, "    1024k\n");

	fprintf(stderr, "\n[-a RAM Buffer Size in MB]\n");
	fprintf(stderr, "    128M\n");

	fprintf(stderr, "\n[-h hot data identification]\n");
	fprintf(stderr, "    on\n");
	fprintf(stderr, "    off\n");

	fprintf(stderr, "\n[-d data allocation]\n");
	fprintf(stderr, "    horizontal\n");
	fprintf(stderr, "    vertical\n");
	fprintf(stderr, "    flex_horizontal\n");
	fprintf(stderr, "    flex_vertical\n");

	fprintf(stderr, "\n[-p parity allocation]\n");
	fprintf(stderr, "    fixed (e.g., RAID4)\n");
	fprintf(stderr, "    rotated (e.g., RAID5)\n");

	fprintf(stderr, "\n[-e erasure coding]\n");
	fprintf(stderr, "    none (e.g., no redundancy)\n");
	fprintf(stderr, "    parity (e.g., single parity)\n");
	fprintf(stderr, "    raid6 (e.g., RAID6, dual parity)\n");

	fprintf(stderr, "\n[-s separated striping]\n");
	fprintf(stderr, "    separated (e.g., clean and dirty data are split into different stripes)\n");
	fprintf(stderr, "              (dirty stripes maintain data redundancy to protect from SSD failure)\n");
	fprintf(stderr, "              (while clean stripes do not maintain data redundancy)\n");
	fprintf(stderr, "    mixed (e.g., clean and dirty data are put together)\n");

	fprintf(stderr, "\n[-r reclaim policy to generate free space]\n");
	fprintf(stderr, "    selective (e.g., cold data are destaged to HDDs, while hot data are garbage collected to SSDs.)\n");
	fprintf(stderr, "    destage (e.g., cold data are destaged to HDDs.)\n");
	fprintf(stderr, "    gc (e.g., victim data are garbage collected to SSDs.)\n");

	
	//fprintf(stderr, "\nUsage : %s Default units for -b, -m, -s are sectors, or specify in k/M/G.\n",
		//pname);
	fprintf(stderr, "\n");

#ifdef COMMIT_REV
	fprintf(stderr, "git commit: %s\n", COMMIT_REV);
#endif
	exit(1);
}

int
main(int argc, char **argv)
{
	printf("[main] Hello kwang hyun!\n");
	int cache_fd[MAX_SSD][MAX_SSD], disk_fd, c;
	char *disk_devname, *ssd_devname_arr[MAX_SSD][MAX_SSD];
	//char *ssd_devname;


	sector_t cache_devsize_arr[MAX_SSD][MAX_SSD], cache_devsize = 0, disk_devsize;
	sector_t cache_devsize_min = ~0;
	sector_t block_size = 0, cache_size_user_dev = 0;
	sector_t chunk_size = 1024*2; // sectors
	sector_t ram_needed;
	sector_t chunk_group_size = 128 * 1024 * 2; // sector
	sector_t chunks_per_group; // sector

	struct sysinfo info;
	int cache_sectorsize_arr[MAX_SSD][MAX_SSD], cache_sectorsize;
	int ret;
	int per_ssd_arr_num = 0; 
	int ssd_arr_group_num = 0;
	int i, I;
	int force = 0;
	int parity_fixed = PARITY_ALLOC_FIXED;
	int erasure_code = ERASURE_CODE_PARITY;
	int separated_striping = SEPAR_STRIPING;
	int hot_identification = 1;
	int reclaim_policy = RECLAIM_GC;
	int data_allocation = DATA_ALLOC_HORI;
	int victim_policy = VICTIM_GREEDY;
	int rambuf_size = 128*1024/4;// 4KB unit
	struct superblock_device *sb;
	char *buf;
	int flush_command = FLUSH_NONE;
	
	pname = argv[0];
	while ((c = getopt(argc, argv, "N:k:g:a:f:s:r:b:d:m:n:v:a:p:e:h:c")) != -1) {
		switch (c) {
		case 'k':
			cache_size_user_dev = get_cache_size(optarg);
			break;
		case 'b':
			block_size = get_block_size(optarg);
			/* Block size should be a power of 2 */
			break;
		case 'c':
			chunk_size = get_chunk_size(optarg);
			/* Block size should be a power of 2 */
			break;
		case 'a':
			rambuf_size = get_cache_size(optarg)/8;
			/* Block size should be a power of 2 */
			break;
		case 'g':
			chunk_group_size = get_cache_size(optarg);
			/* Block size should be a power of 2 */
			break;
		case 'n':
			per_ssd_arr_num = atoi(optarg);
			break;
		case 'N':
			ssd_arr_group_num = atoi(optarg);
			break;

		// Victim Selection for GC
		case 'v':
			if (strcmp(optarg, "clock") == 0){
				victim_policy = VICTIM_CLOCK;
			}else if (strcmp(optarg, "lru") == 0) {
				victim_policy = VICTIM_LRU;
			}else if (strcmp(optarg, "greedy") == 0) {
				victim_policy = VICTIM_GREEDY;
			}
			break;			

		// Hot Data Identification
		case 'f':
			if (strcmp(optarg, "none") == 0){
				flush_command = FLUSH_NONE;
			}else if (strcmp(optarg, "fine") == 0) {
				flush_command = FLUSH_FINE;
			}else if (strcmp(optarg, "coarse") == 0) {
				flush_command = FLUSH_COARSE;
			}
			break;


		// Hot Data Identification
		case 'h':
			if (strcmp(optarg, "on") == 0){
				hot_identification = 1;
			}else if (strcmp(optarg, "off") == 0) {
				hot_identification = 0;
			}
			break;

		// Data allocation policy in a stripe
		case 'd':
			if (strcmp(optarg, "horizontal") == 0){
				data_allocation = DATA_ALLOC_HORI;
			}else if (strcmp(optarg, "vertical") == 0) {
				data_allocation = DATA_ALLOC_VERT;
			}else if (strcmp(optarg, "flex_horizontal") == 0) {
				data_allocation = DATA_ALLOC_FLEX_HORI;
			}else if (strcmp(optarg, "flex_vertical") == 0) {
				data_allocation = DATA_ALLOC_FLEX_VERT;
			}else{
				usage(pname);
			}
			break;

		// Parity Allocation
		case 'p':
			if (strcmp(optarg, "fixed") == 0){
				parity_fixed = PARITY_ALLOC_FIXED;
			}else if (strcmp(optarg, "rotated") == 0) {
				parity_fixed = PARITY_ALLOC_ROTAT;
			}else{
				usage(pname);
			}

			break;

		// Erasure Coding Type
		case 'e':
			if (strcmp(optarg, "none") == 0){
				erasure_code = ERASURE_CODE_NONE;
			}else if (strcmp(optarg, "parity") == 0) {
				erasure_code = ERASURE_CODE_PARITY;
			}else if (strcmp(optarg, "raid6") == 0) {
				erasure_code = ERASURE_CODE_RAID6;
				printf(" RAID6 coding will be supported\n");
				usage(pname);
			}else{
				usage(pname);
			}
			break;

		// Clean Dirty Separated Striping
		case 's':
			if (strcmp(optarg, "separated") == 0){
				separated_striping = SEPAR_STRIPING;;
			}else if (strcmp(optarg, "mixed") == 0) {
				separated_striping = MIXED_STRIPING;;
			}else{
				usage(pname);
			}

			break;

		// Free Space Reclaim Policy
		case 'r':
			if (strcmp(optarg, "selective") == 0){
				reclaim_policy = RECLAIM_SELECTIVE;
			}else if (strcmp(optarg, "destage") == 0) {
				reclaim_policy = RECLAIM_DESTAGE;
			}else if (strcmp(optarg, "gc") == 0) {
				reclaim_policy = RECLAIM_GC;
			}else{
				usage(pname);
			}
			break;
		case '?':
			usage(pname);
		}
	}


	if(per_ssd_arr_num <= 0 || ssd_arr_group_num <= 0)
		usage(pname);

	if (optind == argc)
		usage(pname);

	if (block_size == 0)
		block_size = 8;		/* 4KB default blocksize */

	for (I = 0; I < ssd_arr_group_num; I++) {
		for(i = 0;i < per_ssd_arr_num;i++){
			ssd_devname_arr[I][i] = argv[optind++];
			printf(" SSD[%d][%d] = %s \n", I, i, ssd_devname_arr[I][i]);
		}
	}

	//ssd_devname = ssd_devname_arr[0];

	if (optind == argc)
		usage(pname);

	disk_devname = argv[optind];

	buf = malloc(chunk_size * SECTOR_SIZE);
	sb = (struct superblock_device *)buf;
	
	for (I = 0; I < ssd_arr_group_num; I++) {
		for(i = 0;i < per_ssd_arr_num;i++){
			cache_fd[I][i] = open(ssd_devname_arr[I][i], O_RDWR);
			if (cache_fd[I][i] < 0) {
				fprintf(stderr, "Failed to open %s\n", ssd_devname_arr[I][i]);
				exit(1);
			}
			if (ioctl(cache_fd[I][i], BLKGETSIZE, &cache_devsize_arr[I][i]) < 0) {
				fprintf(stderr, "%s: Cannot get cache size %s\n", 
					pname, ssd_devname_arr[I][i]);
				exit(1);		
			}
			if(cache_devsize_arr[I][i] < cache_devsize_min)
				cache_devsize_min = cache_devsize_arr[I][i]/block_size*block_size;

			if (ioctl(cache_fd[I][i], BLKSSZGET, &cache_sectorsize_arr[I][i]) < 0) {
				fprintf(stderr, "%s: Cannot get cache size %s\n", 
					pname, ssd_devname_arr[I][i]);
				exit(1);		
			}	
		}
	}

	if(cache_size_user_dev && cache_size_user_dev<cache_devsize_min)
		cache_devsize_min = cache_size_user_dev;

	for (I = 0; I < ssd_arr_group_num; I++) {
		for(i = 0;i < per_ssd_arr_num;i++){
			cache_devsize_arr[I][i] = cache_devsize_min;
			cache_devsize += cache_devsize_min;
		}
	}

	//if (cache_size && cache_size > cache_devsize) {
	//	fprintf(stderr, "%s: Cache size is larger than ssd size %lu/%lu\n", 
	//			pname, cache_size, cache_devsize);
	//	exit(1);		
	//}

	disk_fd = open(disk_devname, O_RDONLY);
	if (disk_fd < 0) {
		fprintf(stderr, "%s: Failed to open %s\n", 
			pname, disk_devname);
		exit(1);
	}
	if (ioctl(disk_fd, BLKGETSIZE, &disk_devsize) < 0) {
		fprintf(stderr, "%s: Cannot get disk size %s\n", 
			pname, disk_devname);
		exit(1);				
	}

	/* Remind users how much core memory it will take - not always insignificant.
 	 * If it's > 25% of RAM, warn.
         */
	ram_needed = (cache_devsize / block_size) * sizeof(struct metablock);	/* Whole device */

	sysinfo(&info);
	printf(" DM-SRC metadata will use %luMB of your %luMB main memory\n",
		ram_needed >> 20, info.totalram >> 20);
	if (!force && ram_needed > (info.totalram * 25 / 100)) {
		fprintf(stderr, "Proportion of main memory needed for flashcache metadata is high.\n");
		fprintf(stderr, "You can reduce this with a smaller cache or a larger blocksize.\n");
		check_sure();
	}

	//if (!force && cache_size > disk_devsize) {
	//	fprintf(stderr, "Size of cache volume (%s) is larger than disk volume (%s)\n",
	//		ssd_devname, disk_devname);
	//	check_sure();
	//}

	sb->magic = SRC_MAGIC;
	sb->uuid = gen_uuid();
	sb->create_time = gen_uuid();

	sb->block_size = block_size;
	sb->chunk_size = chunk_size;
	sb->chunk_group_size = chunk_group_size;

	sb->ssd_devsize = cache_devsize_min / sb->chunk_size * sb->chunk_size;
	sb->hdd_devsize = disk_devsize / 8 * 8;

	sb->num_blocks_per_chunk = sb->chunk_size / sb->block_size;
	sb->num_blocks_per_ssd = sb->ssd_devsize / sb->block_size;

	//printf(" num blocks per chunk = %d \n", sb->num_blocks_per_chunk);
	chunks_per_group = chunk_group_size / chunk_size;
	sb->chunks_per_group = chunks_per_group;
	printf(" chunk group size = %d \n", (int)chunk_group_size);
	printf(" chunks per group = %d \n", (int)chunks_per_group);

	sb->num_chunks = sb->ssd_devsize / sb->chunk_size - chunks_per_group; // super block reserved
	sb->num_chunks = sb->num_chunks / chunks_per_group * chunks_per_group;
	sb->num_groups = sb->num_chunks / chunks_per_group;

	{
		__u32 temp;
		__u32 size;
		__u32 num_entry;

		num_entry = SECTOR_SIZE * chunk_size / 4096;
		size = (SEGMENT_HEADER_SIZE + sizeof(struct metablock_device) * num_entry);

		sb->num_summary_per_chunk = size / 4096;
		if(size%4096)
			sb->num_summary_per_chunk++;

		sb->num_entry_per_page = 4096/sizeof(struct metablock_device);
		if(4096%sizeof(struct metablock_device)){
			printf(" invalid struct metablock ... \n\n");
			usage(pname);
		}
	}

	sb->parity_allocation = parity_fixed;
	sb->striping_policy = separated_striping;

	sb->data_allocation = data_allocation;
	sb->victim_policy = victim_policy;
	sb->reclaim_policy = reclaim_policy;

	sb->erasure_code = erasure_code;
	sb->hot_identification = hot_identification;
	sb->rambuf_pool_amount = rambuf_size;

	sb->flush_command = flush_command;
	sb->ssd_row_num = per_ssd_arr_num;
	sb->ssd_col_num = ssd_arr_group_num;

	if(sb->rambuf_pool_amount < sb->chunk_size/sb->block_size*2*per_ssd_arr_num*ssd_arr_group_num){
		sb->rambuf_pool_amount = sb->chunk_size/sb->block_size*2*per_ssd_arr_num*ssd_arr_group_num;
	}

	for (I = 0; I < ssd_arr_group_num; I++) {
		for(i = 0;i < per_ssd_arr_num; i++){
			int j;
			lseek(cache_fd[I][i], 0, SEEK_SET);
			for(j = 0;j < chunks_per_group;j++){
				if (write(cache_fd[I][i], buf, chunk_size * 512) < 0) {
					fprintf(stderr, "Cannot write Flashcache superblock %s\n", 
						ssd_devname_arr[I][i]);
					exit(1);		
				}
			}
			printf(" Writing superblock on %s \n", ssd_devname_arr[I][i]);
			fsync(cache_fd[I][i]);
			close(cache_fd[I][i]);
		}
	}

	printf(" RAM buffer size = %.3fMB\n", (double)sb->rambuf_pool_amount*sb->block_size*512/(1024*1024));
	//printf(" Block_size %lu, cache_size %lu\n", 
	//	       block_size, cache_size);
	printf(" Total cache size = %.2fMB\n", (double)cache_devsize*512/1024/1024);
	printf(" Num chunks per SSD = %d\n", sb->num_chunks);
	printf(" Num groups per SSD = %d\n", sb->num_groups);
	printf(" Chunk size = %dKB, stripe size = %dKB\n", sb->chunk_size/2, sb->chunk_size/2 * per_ssd_arr_num*ssd_arr_group_num);
	printf(" Chunk group size = %dMB, stripe group size = %dMB\n", sb->chunk_group_size/2/1024, sb->chunk_group_size/2/1024 * per_ssd_arr_num*ssd_arr_group_num);
	printf(" Summary Per Chunk = %d \n", sb->num_summary_per_chunk);
	printf(" Meta Per Page = %d \n", sb->num_entry_per_page);

	if(sb->erasure_code==ERASURE_CODE_NONE)
		printf(" Erasure Code: None\n");
	else if(sb->erasure_code==ERASURE_CODE_PARITY)
		printf(" Erasure Code: Parity\n");
	else 
		printf(" Erasure Code: RAID-6\n");

	if(sb->parity_allocation==PARITY_ALLOC_FIXED)
		printf(" Parity Allocation: Fixed\n");
	else
		printf(" Parity Allocation: Rotated\n");

	if(sb->data_allocation==DATA_ALLOC_HORI)
		printf(" Data Allocation: Horizontal\n");
	else if(sb->data_allocation==DATA_ALLOC_VERT)
		printf(" Data Allocation: Vertical\n");
	else if(sb->data_allocation==DATA_ALLOC_FLEX_VERT)
		printf(" Data Allocation: Flexible Vertical \n");
	else
		printf(" Data Allocation: Flexible Horizontal\n");

	if(sb->striping_policy==SEPAR_STRIPING)
		printf(" Striping Policy: Separated\n");
	else
		printf(" Striping Policy: Mixed\n");

	if(sb->hot_identification)
		printf(" Hot Data Identification: On\n");
	else
		printf(" Hot Data Identification: Off\n");

	if(sb->reclaim_policy==RECLAIM_GC)
		printf(" Reclaim Policy: GC\n");
	else if(sb->reclaim_policy==RECLAIM_SELECTIVE)
		printf(" Reclaim Policy: Selective\n");
	else
		printf(" Reclaim Policy: Destage\n");

	if(sb->victim_policy==VICTIM_GREEDY)
		printf(" Victim Policy: Greedy\n");
	else if(sb->victim_policy==VICTIM_CLOCK)
		printf(" Victim Policy: Clock\n");
	else
		printf(" Victim Policy: LRU\n");

	if(sb->flush_command==FLUSH_NONE)
		printf(" None Flush Command\n");
	else if(sb->flush_command==FLUSH_FINE)
		printf(" Fine-grain Flush Command\n");
	else
		printf(" Coarse-grain Flush Command\n");


	close(disk_fd);
	return 0;
}
