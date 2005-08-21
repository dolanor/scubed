/* scubed - Simple Steganographic Store
 * Copyright (C) 2005 Rik Snel <rsnel@cube.dyndns.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* philosophy:
 * - do cautious error checking
 * - all errors are fatal
 * - OS will cleanup file descriptors and calloced stuff
 */
#define _FILE_OFFSET_BITS 64 /* off_t must be 64 bits */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#define __USE_LARGEFILE /* we need fseeko */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#undef NDEBUG /* we need sanity checking, always */
#include <assert.h>
#include <errno.h>
#include <string.h>

#include <libdevmapper.h>

#define SECTOR 512
#define BLOCK_SECTOR_OFFSET(a)	data_start512+((uint64_t)(a))*eff_block_size512

/* these macros are for textual user readable output, the first argument
 * to each macro must be a double quoted string, so: VERBOSE(msg); is wrong 
 * and VERBOSE("%s", msg); is correct (and also good practise). */
#define WHINE(a,...) fprintf(stderr, "%s:" a "\n", exec_name, ## __VA_ARGS__)
char *exec_name = NULL;
int verbose = 0;
int force = 0;
#define VERBOSE(a,...) if (verbose) WHINE(a, ## __VA_ARGS__)
#define FATAL(a,...) do { \
	WHINE("fatal:" a, ## __VA_ARGS__); exit(1); } while (0)

struct block_head {
	int64_t zero;
	int32_t zero_or_one;
	int32_t number;
	char unused[496];
};

struct dev_s {
	char *name;
	FILE *fp;
	int last_block;
	int no_blocks;
	int *blocks;
};

FILE *urand = NULL;

int block_size;
int hw_block_size = -1;

uint64_t data_start512;
uint64_t eff_block_size512;

/* contains the total amount of blocks */
int no_blocks;

/* device info with status of all blocks belonging to it sorted
 * in device order */
int no_devs = 0;
struct dev_s *devs = NULL;

/* list of empty blocks sorted in any order (i.e. unsorted) */
int no_empty_blocks = 0;
int *empty_blocks = NULL;

void usage(const char *msg) {
	if (msg && *msg != '\0') printf("%s\n", msg);
	printf("Usage: %s [-v] [-f] [-m|-M NAME|-r DEVICENO,BLOCKS] "
			"DEVICE1 [... DEVICEN]\n", 
			exec_name);
	exit(msg?1:0);
}

void *scalloc(size_t size) {
	void *ptr;
	if (!(ptr = calloc(size, 1))) FATAL("out of memory error");
	return ptr;
}

void alloc_check_open_devices(int no_names, char **names) {
	struct stat statbuf;
	struct dev_s *d;
	int i, size = -1;

	VERBOSE("number of devices %d", no_names);

	devs = scalloc(sizeof(*devs)*no_names);
	
	for (i = 0; i < no_names; i++) {
		d = &devs[i];
		d->name = names[i];
		int fd, thissize, thisblocksize;
		VERBOSE("opening device %s", d->name);
		if (stat(d->name, &statbuf)) 
			FATAL("stat %s: %s", d->name, strerror(errno));
		if (!S_ISBLK(statbuf.st_mode))
			FATAL("%s is not a block device", d->name);
		if (!(devs[i].fp = fopen(d->name, "r+"))) 
			FATAL("error opening %s: %s", d->name, strerror(errno));
		if ((fd = fileno(devs[i].fp)) == -1)
			FATAL("impossible: %s", strerror(errno));
		if (ioctl(fd, BLKGETSIZE, &thissize) == -1)
			FATAL("ioctl on %s: %s", d->name, strerror(errno));
		if (size == -1) size = thissize;
		else if (size != thissize) 
			FATAL("devices have different sizes");
		if (ioctl(fd, BLKBSZGET, &thisblocksize) == -1)
			FATAL("ioctl on %s: %s", d->name, strerror(errno));
		if (hw_block_size == -1) hw_block_size = thisblocksize;
		else if (hw_block_size != thisblocksize)
			FATAL("devices have different blocksizes");
	}
	no_devs = no_names;
	VERBOSE("all devices opened, hw_block_size=%d", hw_block_size);

	assert(hw_block_size%SECTOR == 0);

	/* make room for worst case padding */
	size = size - hw_block_size/SECTOR + 1; 

	i = 0;
	while ((no_blocks = size/(block_size=1+(hw_block_size<<i))) > 1024) i++;
	block_size *= SECTOR;

	eff_block_size512 = (block_size - SECTOR)/SECTOR;
	VERBOSE("using %d blocks of %d + %d bytes", 
			no_blocks, block_size - SECTOR, SECTOR);

	data_start512 = ((no_blocks + hw_block_size/SECTOR - 1)/
			(hw_block_size/SECTOR))*(hw_block_size/SECTOR);

	VERBOSE("data_start512=%llu, padded from %d", data_start512, no_blocks);

	for (i = 0; i < no_devs; i++) {
		int j;
		d = &devs[i];
		d->last_block = -1;
		d->blocks = scalloc(sizeof(struct block_s*)*no_blocks);
		for (j = 0; j < no_blocks; j++) d->blocks[j] = -1;
	}

	empty_blocks = scalloc(sizeof(int)*no_blocks);
	for (i = 0; i < no_blocks; i++) empty_blocks[i] = -1;

	VERBOSE("opening /dev/urandom");
	if (!(urand = fopen("/dev/urandom", "r"))) 
		FATAL("open /dev/urandom: %s", strerror(errno));
}

/* randomly select an unused block */
int rands(void) {
	uint32_t rd;
	assert(urand);
	if (fread(&rd, sizeof(rd), 1, urand) != 1) 
		FATAL("reading from /dev/urandom: %s", strerror(errno));

	return (int)(((double)no_empty_blocks)*rd/(UINT32_MAX+1.0));
}

void seek_head(int block, struct dev_s *dev) {
	assert(dev && dev->fp && dev->name);
	assert(block >= 0 && block < no_blocks);
	if (fseek(dev->fp, block*SECTOR, SEEK_SET) == -1) 
		FATAL("fseek to %d on %s: %s", block*SECTOR,
				dev->name, strerror(errno));
}

void write_head(struct block_head *buf, int block, struct dev_s *dev) {
	assert(buf);
	seek_head(block, dev);
	if (fwrite(buf, sizeof(*buf), 1, dev->fp) != 1)
		FATAL("fwrite %d bytes to %s: %s", sizeof(*buf), dev->name,
				strerror(errno));
}

void read_head(struct block_head *buf, int block, struct dev_s *dev) {
	assert(buf);
	seek_head(block, dev);
	if (fread(buf, sizeof(*buf), 1, dev->fp) != 1)
		FATAL("fread %d bytes from %s: %s", sizeof(*buf), dev->name,
				strerror(errno));
}

/* function must be called exacly ONCE for every block */
void indexify(int block) {
	int i;
	struct block_head head;
	for (i = 0; i < no_devs; i++) {
		struct dev_s *d = &devs[i];
		read_head(&head, block, d);
		if (head.zero == 0 && head.zero_or_one>>1 == 0) {
			/* add block to table */
			if (d->blocks[head.number] != -1) 
				FATAL("block already claimed?!?!?!");
			if (head.number >= no_blocks)
				FATAL("illegal block number");
			d->no_blocks++;

			/* this is impossible, not just caused by 
			 * corrupted data */
			assert(d->no_blocks <= no_blocks);

			d->blocks[head.number] = block;
			if (head.zero_or_one == 1) d->last_block = block;
			//VERBOSE("block %d claimed by %d", block, i);
			return;
		}
	}

	/* block unused */
	assert(no_empty_blocks < no_blocks);
	empty_blocks[no_empty_blocks++] = block;
	//VERBOSE("block %d empty", block);
}

/* check sanity, insanity is detected by assert since
 * it can only be caused by errors in this program; not
 * by errors in the data */
void check_sanity() {
	int i, j, blocks_seen = 0;
	int *bools = NULL;

	bools = scalloc(sizeof(int)*no_blocks);

	for (i = 0; i < no_devs; i++) {
		struct dev_s *d = &devs[i];
		int adder = 0;
		assert(d->no_blocks >= 0 && d->no_blocks <= no_blocks);
		blocks_seen += devs[i].no_blocks;
		for (j = 0; j < d->no_blocks; j++) {
			/* blocks[j] == -1 means integrity error, check later */
			while (d->blocks[j + adder] == -1) {
				adder++;
				assert(j + adder < d->no_blocks);
			}
			assert(d->blocks[j] >= -1 && d->blocks[j] < no_blocks);
			assert(bools[d->blocks[j+adder]] == 0);
			bools[d->blocks[j+adder]] = 1;
		}
	}

	blocks_seen += no_empty_blocks;
	for (j = 0; j < no_empty_blocks; j++) {
		assert(empty_blocks[j] >= 0 && empty_blocks[j] < no_blocks);
		assert(bools[empty_blocks[j]] == 0);
		bools[empty_blocks[j]] = 1;
	}
	assert(blocks_seen == no_blocks);

	for (i = 0; i < no_blocks; i++) {
		assert(bools[i] == 1);
	}
	
	VERBOSE("data seems sane");
}

/* check integrity */
void check_integrity(void) {
	int i, j;
	for (i = 0; i < no_devs; i++) {
		struct dev_s *d = &devs[i];
		if (d->last_block == -1 && d->no_blocks > 0)
			FATAL("there is no last block while "
					"%s nonempty", d->name);
		for (j = 0; j < d->no_blocks - 1; j++) {
			if (d->blocks[j] == -1) FATAL("block %d of %s missing",
					j, d->name);
			if (d->blocks[j] == d->last_block)
				FATAL("middle block %d of %s marked as "
						"last block", j, d->name);
		}
		if (d->blocks[j] != d->last_block) 
			FATAL("last block of %s not marked as last block "
					"or missing", d->name);
	}
	
	VERBOSE("data seems integer");
}

void print_info() {
	int i;
	printf("blocksize is %d bytes, overhead is %d bytes\n", 
			block_size, SECTOR);
	for (i = 0; i < no_devs; i++) 
		printf("%04d blocks in %s (index %d)\n", 
				devs[i].no_blocks, devs[i].name, i);
	printf("%04d blocks unclaimed\n", no_empty_blocks);
	printf("%04d blocks total\n", no_blocks);
}

enum command_e { INFO, RESIZE, MAP, MAP_AUTO };

int goal; /* resize goal */
int dev_no; /* resize device number */

void alloc_block(struct dev_s *d) {
	struct block_head old, new;
	int block = rands();
	VERBOSE("using empty block %d becomes block %d of %s", empty_blocks[block], d->no_blocks, d->name);

	new.zero = old.zero = 0;
	old.zero_or_one = 0;
	new.zero_or_one = 1;
	old.number = d->no_blocks - 1;
	new.number = d->no_blocks;

	/* adding block to device list */
	d->last_block = d->blocks[d->no_blocks++] = empty_blocks[block];
	/* removing block from empty list */
	empty_blocks[block] = empty_blocks[--no_empty_blocks];

	/* marking old 'last block' (if any) as 'non last block' */
	if (d->no_blocks > 1) write_head(&old, d->blocks[old.number], d);
	/* marking new block as 'last block' */
	write_head(&new, d->blocks[new.number], d);
}

int yesno() {
	char answer[4];
label:
	fgets(answer, sizeof(answer), stdin);
	if (answer[0] == '\n' || !strcmp("No\n", answer)) return 0; /* No */
	if (strcmp("Yes", answer)) {
		printf("Please answer Yes or No. [No] ");
		goto label;
	}
	return 1; /* Yes */
}

void warning(void) {
	printf("---WARNING---WARNING---WARNING---WARNING---WARNING---WARNING---WARNING---\n");
}

void enlarge(struct dev_s *d, int alloc) {
	assert(d && alloc > 0);
	/* device must be enlarged */

	if (!force) {

	warning();
	printf("allocating %d blocks for %s from the unclaimed pool\n", alloc,
			d->name);
	printf("this is only safe if you listed ALL your devices associated\n");
	printf("with the store on the commandline, continue? [No] ");

	if (!yesno()) return;

	}

	while (alloc--) alloc_block(d);
}	

/* block gets overwritten with random data, data is still there, but 
 * special equipment is needed to read it, if this is a concern you 
 * can up your chances of really detroying the data with using
 * wipe(1), shred(1) or srm(1) on the linearized device before
 * you start deleing blocks */
void destroy_block(struct dev_s *d) {
	struct block_head head;
	int i, block;

	block = d->blocks[d->no_blocks - 1];

	if (fread(&head, sizeof(head), 1, urand) != 1)
		FATAL("error reading /dev/urandom");

	write_head(&head, block, d);

	for (i = 0; i < eff_block_size512; i++) {
		if (fread(&head, sizeof(head), 1, urand) != 1)
			FATAL("error reading /dev/urandom: %s", 
					strerror(errno));
		if (fseeko(d->fp, (BLOCK_SECTOR_OFFSET(block) + i)*SECTOR, 
					SEEK_SET) == -1) 
			FATAL("fseeko to %llu on %s: %s", 
					(BLOCK_SECTOR_OFFSET(block) + i)*SECTOR,
					d->name, strerror(errno));

		if (fwrite(&head, sizeof(head), 1, d->fp) != 1)
			FATAL("error writing to %s: %s", 
					d->name, strerror(errno));

	}
}

void remove_block(struct dev_s *d) {
	struct block_head last;
	assert(d->no_blocks > 0);
	VERBOSE("removing block %d (%d) from %s", d->no_blocks - 1,
			d->blocks[d->no_blocks-1], d->name);

	
	destroy_block(d);
	last.zero = 0;
	last.zero_or_one = 1;
	last.number = --d->no_blocks - 1;
	empty_blocks[no_empty_blocks++] = d->blocks[d->no_blocks];
	d->blocks[d->no_blocks] = -1;
	if (d->no_blocks == 0) {
		d->last_block = -1;
		return;
	}
	d->last_block = d->blocks[d->no_blocks - 1];

	write_head(&last, d->last_block, d);
}

void shrink(struct dev_s *d, int alloc) {
	assert(d && alloc < 0);

	if (!force) {

	warning();
	printf("removing %d blocks from the end of %s\n", -alloc, d->name);
	printf("those blocks will be 'destroyed', continue? [No] ");

	if (!yesno()) return;

	}

	while (alloc++) remove_block(d);
}

void resize() {
	struct dev_s *d;
	int alloc;
	if (dev_no < 0 || dev_no > no_devs) FATAL("illegal device %d", dev_no);
	d = &devs[dev_no];
	alloc = goal - d->no_blocks;
	if (alloc > no_empty_blocks) FATAL("requested new size too large, nog enought unclaimed blocks");
	if (alloc == 0) return; /* nothing to do */
	if (alloc < 0) shrink(d, alloc);
	if (alloc > 0) enlarge(d, alloc);

	check_sanity();

	check_integrity();
}

void write_map() {
	struct dev_s *d;
	int i;
	assert(no_devs == 1);
	d = &devs[0];
	
	for (i = 0; i < d->no_blocks; i++) 
		printf("%llu %llu linear %s %llu\n", i*eff_block_size512, 
				eff_block_size512,
				d->name, BLOCK_SECTOR_OFFSET(d->blocks[i]));
}

const char *target = NULL;

void auto_map() {
	struct dev_s *d;
	int i;
	struct dm_task *task = dm_task_create(DM_DEVICE_CREATE);
	char buf[128]; /* long enough */
	assert(target);
	assert(no_devs == 1);
	d = &devs[0];

	if (!task) FATAL("unable to create dm_task for %s", target);
	if (!dm_task_set_name(task, target)) 
		FATAL("dm_task_set_name to %s failed", target);

	for (i = 0; i < d->no_blocks; i++) {
		snprintf(buf, 128, "%s %llu", d->name, 
				BLOCK_SECTOR_OFFSET(d->blocks[i]));
		if (!dm_task_add_target(task, i*eff_block_size512, 
					eff_block_size512, "linear", buf)) 
			FATAL("dm_task_add_target to %s failed", target);
	}

	if (!dm_task_run(task)) 
		FATAL("dm_task_run to create %s failed", target);

	dm_task_destroy(task);
	dm_lib_release();
	dm_lib_exit();
}

const char *version = "$Id$";

void print_version() {
	printf("scubed - %s\n", version);
	exit(0);
}

int main(int argc, char **argv) {
	int i, c;
	enum command_e command = INFO;
	char *tmp;
	assert(sizeof(struct block_head) == SECTOR); /* just checking... */
	assert(sizeof(off_t) == 8);

	/* stolen from wget */
	exec_name = strrchr(argv[0], '/');
	if (!exec_name++) exec_name = argv[0];

	while ((c = getopt(argc, argv, "Vvr:mfM:")) != -1) switch (c) {
		case 'V':
			print_version();
			break;
		case 'v':
			verbose = 1;
			break;
		case 'f':
			force = 1;
			break;
		case 'r':
			if (command != INFO) usage("only one command allowed at a time");
			command = RESIZE;
			dev_no = strtol(optarg, &tmp, 10);
			if (*tmp != ',') usage("wrong argument to -r (junk before comma or no comma)");
			optarg = tmp + 1;
			goal = strtol(optarg, &tmp, 10);
			if (*tmp != '\0') usage("wrong argument to -r (junk after comma)");
			break;
		case 'm':
			if (command != INFO) usage("only one command allowed at a time");
			command = MAP;
			break;
		case 'M':
			if (command != INFO) usage("only one command allowed at a time");
			target = optarg;
			command = MAP_AUTO;
			break;
		default:
			usage(NULL);
	}
	
	if (argc - optind == 0) usage("no devices specified");

	if ((command == MAP || command == MAP_AUTO) && argc - optind != 1) usage("only one device may be specifed wit option -m");

	VERBOSE("version: %s", version);

	alloc_check_open_devices(argc - optind, argv + optind);

	for (i = 0; i < no_blocks; i++) indexify(i);

	check_sanity();

	check_integrity();

	switch (command) {
		case INFO:
			print_info();
			break;
		case RESIZE:
			resize();
			break;
		case MAP:
			write_map();
			break;
		case MAP_AUTO:
			auto_map();
			break;
	}

	exit(0);
}

