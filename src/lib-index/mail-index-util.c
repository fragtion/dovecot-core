/* Copyright (c) 2003-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "bsearch-insert-pos.h"
#include "mail-index-private.h"

uint32_t mail_index_uint32_to_offset(uint32_t offset)
{
	i_assert(offset < 0x40000000);
	i_assert((offset & 3) == 0);

	offset >>= 2;
	offset = 0x00000080 | ((offset & 0x0000007f)) |
		 0x00008000 | ((offset & 0x00003f80) >> 7 << 8) |
		 0x00800000 | ((offset & 0x001fc000) >> 14 << 16) |
		 0x80000000 | ((offset & 0x0fe00000) >> 21 << 24);

	return cpu32_to_be(offset);
}

uint32_t mail_index_offset_to_uint32(uint32_t offset)
{
	offset = be32_to_cpu(offset);

	if ((offset & 0x80808080) != 0x80808080)
		return 0;

	return  (((offset & 0x0000007f)) |
		 ((offset & 0x00007f00) >> 8 << 7) |
		 ((offset & 0x007f0000) >> 16 << 14) |
		 ((offset & 0x7f000000) >> 24 << 21)) << 2;
}

void mail_index_pack_num(uint8_t **p, uint32_t num)
{
	/* number continues as long as the highest bit is set */
	while (num >= 0x80) {
		**p = (num & 0x7f) | 0x80;
		*p += 1;
		num >>= 7;
	}

	**p = num;
	*p += 1;
}

int mail_index_unpack_num(const uint8_t **p, const uint8_t *end,
			  uint32_t *num_r)
{
	const uint8_t *c = *p;
	uint32_t value = 0;
	unsigned int bits = 0;

	for (;;) {
		if (unlikely(c == end)) {
			/* we should never see EOF */
			*num_r = 0;
			return -1;
		}

		value |= (*c & 0x7f) << bits;
		if (*c < 0x80)
			break;

		bits += 7;
		c++;
	}

	if (unlikely(bits >= 32)) {
		/* broken input */
		*p = end;
		*num_r = 0;
		return -1;
	}

	*p = c + 1;
	*num_r = value;
	return 0;
}

static int mail_index_seq_record_cmp(const uint32_t *key_seq,
				     const uint32_t *data_seq)
{
	return (int)*key_seq - (int)*data_seq;
}

bool mail_index_seq_array_lookup(const ARRAY_TYPE(seq_array) *array,
				 uint32_t seq, unsigned int *idx_r)
{
	/* fast path: expect array to be usually appended to */
	unsigned int count = array_count(array);
	if (count > 0) {
		const uint32_t *elem = array_back(array);
		if (seq > *elem) {
			*idx_r = count;
			return FALSE;
		}
	}
	return array_bsearch_insert_pos(array, &seq,
					mail_index_seq_record_cmp, idx_r);
}

void mail_index_seq_array_alloc(ARRAY_TYPE(seq_array) *array,
				size_t record_size)
{
	size_t aligned_record_size = (record_size + 3) & ~3U;

	i_assert(!array_is_created(array));

	array_create(array, default_pool,
		     sizeof(uint32_t) + aligned_record_size,
		     1024 / (sizeof(uint32_t) + aligned_record_size));
}

bool mail_index_seq_array_add(ARRAY_TYPE(seq_array) *array, uint32_t seq,
			      const void *record, size_t record_size,
			      void *old_record)
{
	void *p;
	unsigned int idx, aligned_record_size;

	/* records need to be 32bit aligned */
	aligned_record_size = (record_size + 3) & ~3U;

	if (!array_is_created(array))
		mail_index_seq_array_alloc(array, record_size);
	i_assert(array->arr.element_size == sizeof(seq) + aligned_record_size);

	if (mail_index_seq_array_lookup(array, seq, &idx)) {
		/* already there, update */
		p = array_idx_modifiable(array, idx);
		if (old_record != NULL) {
			/* save the old record before overwriting it */
			memcpy(old_record, PTR_OFFSET(p, sizeof(seq)),
			       record_size);
		}
		memcpy(PTR_OFFSET(p, sizeof(seq)), record, record_size);
		return TRUE;
	} else {
		/* insert */
		p = array_insert_space(array, idx);
		memcpy(p, &seq, sizeof(seq));
		memcpy(PTR_OFFSET(p, sizeof(seq)), record, record_size);
		return FALSE;
	}
}
