/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "mail-index.h"

int mail_index_fsck(MailIndex *index)
{
	/* we verify only the fields in the header. other problems will be
	   noticed and fixed while reading the messages. */
	MailIndexHeader *hdr;
	MailIndexRecord *rec, *end_rec;
	unsigned int max_uid;
	off_t pos;

	i_assert(index->lock_type != MAIL_LOCK_SHARED);

	if (!mail_index_set_lock(index, MAIL_LOCK_EXCLUSIVE))
		return FALSE;

	hdr = index->header;

	hdr->first_hole_position = 0;
	hdr->first_hole_records = 0;

	hdr->messages_count = 0;
	hdr->seen_messages_count = 0;
	hdr->deleted_messages_count = 0;

	hdr->first_unseen_uid_lowwater = 0;
	hdr->first_deleted_uid_lowwater = 0;

	rec = (MailIndexRecord *) ((char *) index->mmap_base +
				   sizeof(MailIndexHeader));
	end_rec = (MailIndexRecord *) ((char *) index->mmap_base +
				       index->mmap_length);

	max_uid = 0;
	for (; rec < end_rec; rec++) {
		if (rec->uid == 0) {
			/* expunged message */
			pos = INDEX_FILE_POSITION(index, rec);
			if (hdr->first_hole_position == 0) {
				hdr->first_hole_position = pos;
				hdr->first_hole_records = 1;
			} else if ((off_t) (hdr->first_hole_position +
					    (hdr->first_hole_records *
					     sizeof(MailIndexRecord))) == pos) {
				/* hole continues */
				hdr->first_hole_records++;
			}
			continue;
		}

		if (rec->uid < max_uid) {
			i_error("fsck %s: UIDs are not ordered (%u < %u)",
				index->filepath, rec->uid, max_uid);
			return mail_index_rebuild_all(index);
		}
		max_uid = rec->uid;

		if (rec->msg_flags & MAIL_SEEN)
			hdr->seen_messages_count++;
		else if (hdr->first_unseen_uid_lowwater)
			hdr->first_unseen_uid_lowwater = rec->uid;

		if (rec->msg_flags & MAIL_DELETED) {
			if (hdr->first_deleted_uid_lowwater == 0)
                                hdr->first_deleted_uid_lowwater = rec->uid;
			hdr->deleted_messages_count++;
		}
		hdr->messages_count++;
	}

	if (hdr->next_uid <= max_uid)
		hdr->next_uid = max_uid+1;
	if (hdr->last_nonrecent_uid >= hdr->next_uid)
		hdr->last_nonrecent_uid = hdr->next_uid-1;

	/* FSCK flag is removed automatically by set_lock() */
	return TRUE;
}
