# Simple Steganographic Store (scubed)


## Intro

With  [rubberhose] [1] [gone] [2], [stegfs] [3] [vast asleep] [4]  and [phonebook] [5]
having a (IMHO) weird and complex architecture, the GNU/Linux user wanting
plausible deniability is at a loss. Until now...

Let me introduce the Simple Steganographic Store:

- Simple, the program works on a need to know basis, it doesn't know
  encryption keys, modes etc, it is short and is easy to understand,
  maintain and use. It's also easy to lose your data if you make a slight
  mistake using this program.

- Steganographic, I think it gives the user plausible deniability (a 
  cryptologist/lawyer should probably speak up on this issue; I am neither).

- Store, you can put stuff in it and retrieve it.




## Prerequisites

You need:
- dmsetup and cryptsetup utilities
- dm support in the kernel with dm-linear and dm-crypt
- libdevmapper development files
- the latest version of scubed, get it with Subversion
        svn co http://cube.dyndns.org/svn/scubed/trunk scubed


## Compiling


Just type make in the directory with the Makefile and scubed.c.

# make

It should compile. 


## Tutorial

Suppose you want to hide two filesystems using scubed, one with a size of 512MB
and another with a size of 10MB. Make a file with a size of 1250MB and fill it
with pseudo random data.

        dd if=/dev/urandom of=store bs=1M count=1250

Attach it to a loopdevice

        losetup /dev/loop0 store

Then create two different cryptographic 'views' of this device using 
cryptsetup [6].

        cryptsetup create scubed1 /dev/loop0
Enter passphrase:
        cryptsetup create scubed2 /dev/loop0
Enter passphrase:

Be sure to use different passphrases and be sure to remember them.
Let's see wat scubed says.

        scubed /dev/mapper/scubed?
        blocksize is 2097664 bytes, overhead is 512 bytes
	0000 blocks in /dev/mapper/scubed1 (index 0)
	0000 blocks in /dev/mapper/scubed2 (index 1)
	0624 blocks unclaimed
	0624 blocks total

The blocksize is automatically selected. Note that scubed doesn't know how many
cryptographic views of /dev/loop0 exist (or if they are cryptographic at all);
it only knows about the ones you opened with cryptsetup and specified on the
commandline.

You may think of scubed as a partitioning tool. The amount of partitions
visible is equal to the amount of devices given on the command line, and the
size of each partition can be edited.

Now we will allocate space for our first filesystem, we need about 256 blocks
on /dev/mapper/scubed1 (note: the -r means resize).

        scubed -r 0,256 /dev/mapper/scubed?
	---WARNING---WARNING---WARNING---WARNING---WARNING---WARNING---WARNING---
	allocating 256 blocks for /dev/mapper/scubed1 from the unclaimed pool
	this is only safe if you listed ALL your devices associated
	with the store on the commandline, continue? [No] Yes

Check if it worked.

	scubed /dev/mapper/scubed?
	blocksize is 2097664 bytes, overhead is 512 bytes
	0256 blocks in /dev/mapper/scubed1 (index 0)
	0000 blocks in /dev/mapper/scubed2 (index 1)
	0368 blocks unclaimed
	0624 blocks total

Now allocate space for our second filesystem.

	scubed -r 1,5 /dev/mapper/scubed?
	---WARNING---WARNING---WARNING---WARNING---WARNING---WARNING---WARNING---
	allocating 5 blocks for /dev/mapper/scubed2 from the unclaimed pool
	this is only safe if you listed ALL your devices associated
	with the store on the commandline, continue? [No] Yes

Check again.

	scubed /dev/mapper/scubed?
	blocksize is 2097664 bytes, overhead is 512 bytes
	0256 blocks in /dev/mapper/scubed1 (index 0)
	0005 blocks in /dev/mapper/scubed2 (index 1)
	0363 blocks unclaimed
	0624 blocks total

How does plausible deniability come into play? Let's remove /dev/mapper/scubed2
from the system.

	cryptsetup remove scubed2
	scubed /dev/mapper/scubed?
	blocksize is 2097664 bytes, overhead is 512 bytes
	0256 blocks in /dev/mapper/scubed1 (index 0)
	0368 blocks unclaimed
	0624 blocks total

The blocks owned by /dev/mapper/scubed2 are listed as unclaimed, because
scubed doesn't have access to /dev/mapper/scubed2. 

This is nice and all, but how do we access our newly allocated spaces?
Do it with the following command.

	  scubed -M scubed1.linear /dev/mapper/scubed1
	  mke2fs -j -m 0 /dev/mapper/scubed1.linear

The former command sets up a device mapper table though which the kernel
(and filesystems) can access the blocks belonging to /dev/mapper/scubed1
in a normal order. You can examine the table sent to the device mapper
by doing

	scubed -m /dev/mapper/scubed1

To access the second filesystem; attach /dev/mapper/scubed2 (with
the right password, check for yourself what happens with the wrong password,
but first try to guess) and repeat the above steps.

Some scripts to make this more user-friendly (but less understandable)
are in the scripts/ directory. See the file scripts/SCRIPTS.


## How does it work?

The main idea is taken from stegfs, that is: have a block table where each
entry is encrypted with the key that belongs to the level to which the entry
belongs. If you don't have the key, then you can't decode the block table entry
and you don't know if the block is used.

For now: read the source. It's only approximately 600 lines, including (some)
comments.


[1]: http://www.rubberhose.org
[2]: http://www.mirrors.wiretapped.net/security/cryptography/filesystems/rubberhose/rubberhose-README.txt
[3]: http://www.mcdonald.org.uk/StegFS/
[4]: http://stegfs.sourceforge.net/
[5]: http://www.freenet.org.nz/phonebook/
[6]: http://www.saout.de/misc/dm-crypt/