/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "primes.h"
#include "mmap-util.h"
#include "mail-index.h"
#include "mail-index-util.h"
#include "mail-hash.h"

#include <stdio.h>
#include <fcntl.h>

/* Minimum record count for a hash file. By default, the hash file size is
   the number of messages * 3, and it's rebuilt after the file is 3/4 full.
   Use only primes as hash file sizes. */
#define MIN_HASH_SIZE 109

/* When rebuilding hash, make it 30% full */
#define MIN_PERCENTAGE 30

/* Try rebuilding hash sometimes soon after it's 60% full */
#define REBUILD_PERCENTAGE 60

/* Force a rebuild when hash is 80% full */
#define FORCED_REBUILD_PERCENTAGE 80

#define HASH_FUNC(uid) (uid * 2)

struct _MailHash {
	MailIndex *index;

	unsigned int size;

	int fd;
	char *filepath;

	void *mmap_base;
	size_t mmap_length;

	MailHashHeader *header;
	unsigned int dirty_mmap:1;
	unsigned int modified:1;
};

static int mmap_update(MailHash *hash)
{
	if (!hash->dirty_mmap)
		return TRUE;

	if (hash->mmap_base != NULL)
		(void)munmap(hash->mmap_base, hash->mmap_length);

	hash->mmap_base = mmap_rw_file(hash->fd, &hash->mmap_length);
	if (hash->mmap_base == MAP_FAILED) {
		hash->mmap_base = NULL;
		hash->header = NULL;
		index_set_error(hash->index,
				"hash: mmap() failed with file %s: %m",
				hash->filepath);
		return FALSE;
	}

	if (hash->mmap_length <= sizeof(MailHashHeader) ||
	    (hash->mmap_length - sizeof(MailHashHeader)) %
	    sizeof(MailHashRecord) != 0) {
		/* hash must be corrupted, rebuilding should have noticed
		   if it was only partially written. */
		hash->header = NULL;
		index_set_error(hash->index, "Corrupted hash file %s: %m",
				hash->filepath);
		return FALSE;
	}

	hash->dirty_mmap = FALSE;
	hash->header = hash->mmap_base;
	return TRUE;
}

static MailHash *mail_hash_new(MailIndex *index)
{
	MailHash *hash;

	hash = i_new(MailHash, 1);
	hash->fd = -1;
	hash->index = index;
	hash->filepath = i_strconcat(index->filepath, ".hash", NULL);
	hash->dirty_mmap = TRUE;

	index->hash = hash;
	return hash;
}

int mail_hash_create(MailIndex *index)
{
	i_assert(index->lock_type == MAIL_LOCK_EXCLUSIVE);

	return mail_hash_rebuild(mail_hash_new(index));
}

static int mail_hash_lock_and_rebuild(MailHash *hash)
{
	if (!hash->index->set_lock(hash->index, MAIL_LOCK_EXCLUSIVE))
		return FALSE;
	return mail_hash_rebuild(hash);
}

int mail_hash_open_or_create(MailIndex *index)
{
	MailHash *hash;

	hash = mail_hash_new(index);

	hash->fd = open(hash->filepath, O_RDWR);
	if (hash->fd == -1)
		return mail_hash_lock_and_rebuild(hash);

	if (!mmap_update(hash)) {
		/* mmap() failure is fatal */
		mail_hash_free(hash);
		return FALSE;
	}

	/* verify that this really is the hash file for wanted index */
	if (hash->header->indexid != index->indexid) {
		/* mismatch - just recreate it */
		(void)munmap(hash->mmap_base, hash->mmap_length);
		hash->mmap_base = NULL;
		hash->dirty_mmap = TRUE;

		return mail_hash_lock_and_rebuild(hash);
	}

	hash->size = (hash->mmap_length - sizeof(MailHashHeader)) /
		sizeof(MailHashRecord);
	return TRUE;
}

void mail_hash_free(MailHash *hash)
{
	hash->index->hash = NULL;

	if (hash->mmap_base != NULL) {
		(void)munmap(hash->mmap_base, hash->mmap_length);
		hash->mmap_base = NULL;
	}

	(void)close(hash->fd);
	i_free(hash->filepath);
	i_free(hash);
}

static int file_set_size(int fd, off_t size)
{
	char block[1024];
	unsigned int i, full_blocks;
	off_t pos;

	/* try truncating it to the size we want. if this succeeds, the written
	   area is full of zeros - exactly what we want. however, this may not
	   work at all, in which case we fallback to write()ing the zeros. */
	if (ftruncate(fd, size) != -1 && lseek(fd, 0, SEEK_END) == size)
		return lseek(fd, 0, SEEK_SET) == 0;

	/* skip the existing data in file */
	pos = lseek(fd, 0, SEEK_END);
	if (pos == -1)
		return FALSE;
	size -= pos;

	memset(block, 0, sizeof(block));

	/* write in 1kb blocks */
	full_blocks = size / sizeof(block);
	for (i = 0; i < full_blocks; i++) {
		if (write(fd, block, sizeof(block)) != sizeof(block))
			return FALSE;
	}

	/* write the remainder */
	i = size % sizeof(block);
	return i == 0 ? TRUE : (size_t) write(fd, block, i) == i;
}

static int hash_rebuild_to_file(MailIndex *index, int fd,
				unsigned int hash_size,
				unsigned int messages_count)
{
	MailHashHeader *hdr;
        MailHashRecord *rec;
	MailIndexRecord *idx_rec;
	void *mmap_base;
	unsigned int i, count;
	size_t mmap_length;
	size_t new_size;

	/* fill the file with zeros */
	new_size = sizeof(MailHashHeader) + hash_size * sizeof(MailHashRecord);
	if (!file_set_size(fd, (off_t) new_size)) {
		index_set_error(index,
				"Failed to fill temp hash to size %lu: %m",
				(unsigned long) new_size);
		return FALSE;
	}

	/* now, mmap() it */
	mmap_base = mmap_rw_file(fd, &mmap_length);
	if (mmap_base == MAP_FAILED) {
		index_set_error(index, "mmap()ing temp hash failed: %m");
		return FALSE;
	}

	i_assert(mmap_length == new_size);

	/* we have empty hash file mmap()ed now. fill it by reading the
	   messages from index. */
	rec = (MailHashRecord *) ((char *) mmap_base + sizeof(MailHashHeader));
        idx_rec = index->lookup(index, 1);
	for (count = 0; idx_rec != NULL; count++) {
		i = HASH_FUNC(idx_rec->uid) % hash_size;
		rec[i].uid = idx_rec->uid;
		rec[i].position = INDEX_FILE_POSITION(index, idx_rec);
		idx_rec = index->next(index, idx_rec);
	}

	if (count != messages_count) {
		/* mark this as an error but don't fail because of it. */
                INDEX_MARK_CORRUPTED(index);
		index_set_error(index, "Missing messages while rebuilding "
				"hash file %s - %u found, header says %u",
				index->filepath, count, messages_count);
	}

	/* setup header */
	hdr = mmap_base;
	hdr->indexid = index->indexid;
	hdr->used_records = count;

	return munmap(mmap_base, mmap_length) == 0;
}

int mail_hash_sync_file(MailHash *hash)
{
	if (!hash->modified)
		return TRUE;
	hash->modified = FALSE;

	if (msync(hash->mmap_base, hash->mmap_length, MS_SYNC) == 0)
		return TRUE;
	else {
		index_set_error(hash->index, "msync() failed for %s: %m",
				hash->filepath);
		return FALSE;
	}
}

int mail_hash_rebuild(MailHash *hash)
{
	MailIndexHeader *index_header;
	const char *path;
	unsigned int hash_size;
	int fd;

	i_assert(hash->index->lock_type == MAIL_LOCK_EXCLUSIVE);

	/* first get the number of messages in index */
	index_header = hash->index->get_header(hash->index);

	/* then figure out size for our hash */
	hash_size = primes_closest(index_header->messages_count * 100 / MIN_PERCENTAGE);
	if (hash_size < MIN_HASH_SIZE)
		hash_size = MIN_HASH_SIZE;

	/* create the hash in a new temp file */
	fd = mail_index_create_temp_file(hash->index, &path);
	if (fd == -1)
		return FALSE;

	if (!hash_rebuild_to_file(hash->index, fd, hash_size,
				  index_header->messages_count)) {
		(void)close(fd);
		(void)unlink(path);
		return FALSE;
	}

	if (fsync(fd) == -1) {
		index_set_error(hash->index,
				"fsync() failed with temp hash %s: %m", path);
		(void)close(fd);
		(void)unlink(path);
		return FALSE;
	}

	/* replace old hash file with this new one */
	if (rename(path, hash->filepath) == -1) {
		index_set_error(hash->index, "rename(%s, %s) failed: %m",
				path, hash->filepath);

		(void)close(fd);
		(void)unlink(path);
		return FALSE;
	}

	/* switch fds */
	if (hash->fd != -1)
		(void)close(hash->fd);
	hash->size = hash_size;
	hash->fd = fd;
	hash->dirty_mmap = TRUE;
	return TRUE;
}

off_t mail_hash_lookup_uid(MailHash *hash, unsigned int uid)
{
        MailHashRecord *rec;
	unsigned int hashidx, idx;

	i_assert(uid > 0);
	i_assert(hash->index->lock_type != MAIL_LOCK_UNLOCK);

	if (hash->fd == -1 || !mmap_update(hash))
		return 0;

	/* our hashing function is simple - UID*2. The *2 is there because
	   UIDs are normally contiguous, so once the UIDs wrap around, we
	   don't want to go through lots of records just to find an empty
	   spot */
	hashidx = HASH_FUNC(uid) % hash->size;
	rec = (MailHashRecord *) ((char *) hash->mmap_base +
				  sizeof(MailHashHeader));

	/* check from hash index to end of file */
	for (idx = hashidx; idx < hash->size; idx++) {
		if (rec[idx].uid == uid)
			return rec[idx].position;

		if (rec[idx].uid == 0) {
			/* empty hash record - not found. */
			return 0;
		}
	}

	/* check from beginning of file to hash index */
	for (idx = 0; idx < hashidx; idx++) {
		if (rec[idx].uid == uid)
			return rec[idx].position;

		if (rec[idx].uid == 0) {
			/* empty hash record - not found. */
			return 0;
		}
	}

	/* checked through the whole hash file. this really shouldn't happen,
	   we should have rebuilt it long time ago.. */
	return 0;
}

static MailHashRecord *hash_find_uid_or_free(MailHash *hash, unsigned int uid)
{
        MailHashRecord *rec;
	unsigned int hashidx, idx;

	hashidx = HASH_FUNC(uid) % hash->size;
	rec = (MailHashRecord *) ((char *) hash->mmap_base +
				  sizeof(MailHashHeader));

	/* check from hash index to end of file */
	for (idx = hashidx; idx < hash->size; idx++) {
		if (rec[idx].uid == 0 || rec[idx].uid == uid)
			return rec+idx;
	}

	/* check from beginning of file to hash index */
	for (idx = 0; idx < hashidx; idx++) {
		if (rec[idx].uid == 0 || rec[idx].uid == uid)
			return rec+idx;
	}

	/* hash file is full */
	return NULL;
}

void mail_hash_update(MailHash *hash, unsigned int uid, off_t pos)
{
	MailHashRecord *rec;
	unsigned int max_used;

	i_assert(uid > 0);
	i_assert(hash->index->lock_type == MAIL_LOCK_EXCLUSIVE);

	if (hash->fd == -1 || !mmap_update(hash))
		return;

	if (hash->header->used_records >
	    hash->size * FORCED_REBUILD_PERCENTAGE / 100) {
		/* we really need a rebuild. */
		mail_hash_rebuild(hash);
	}

	/* place the hash into first free record after wanted position */
	rec = hash_find_uid_or_free(hash, uid);

	if (rec == NULL) {
		/* this should never happen, we had already checked that
		   at least 1/5 of hash was empty. except, if the header
		   contained invalid record count for some reason. rebuild.. */
		mail_hash_rebuild(hash);

		rec = hash_find_uid_or_free(hash, uid);
		i_assert(rec != NULL);
	}

	if (pos != 0) {
		/* insert/update record */
		if (rec->uid == 0) {
			/* update records count, and see if hash is
			   getting full */
			max_used = hash->size / 100 * REBUILD_PERCENTAGE;
			if (++hash->header->used_records > max_used) {
				hash->index->set_flags |=
					MAIL_INDEX_FLAG_REBUILD_HASH;
			}
		}
		rec->uid = uid;
		rec->position = pos;
	} else {
		/* delete record */
		rec->uid = 0;
		rec->position = 0;
                hash->header->used_records--;
	}

	hash->modified = FALSE;
}
