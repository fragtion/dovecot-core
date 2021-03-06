/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "hex-dec.h"
#include "unichar.h"
#include "istream-private.h"
#include "istream-jsonstr.h"

#define MAX_UTF8_LEN 6

struct jsonstr_istream {
	struct istream_private istream;

	/* The end '"' was found */
	bool str_end:1;
};

static int
i_stream_jsonstr_read_parent(struct jsonstr_istream *jstream,
			     unsigned int min_bytes)
{
	struct istream_private *stream = &jstream->istream;
	size_t size, avail;
	ssize_t ret;

	size = i_stream_get_data_size(stream->parent);
	while (size < min_bytes) {
		ret = i_stream_read_memarea(stream->parent);
		if (ret <= 0) {
			if (ret == -2) {
				/* tiny parent buffer size - shouldn't happen */
				return -2;
			}
			stream->istream.stream_errno =
				stream->parent->stream_errno;
			stream->istream.eof = stream->parent->eof;
			if (ret == -1 && stream->istream.stream_errno == 0) {
				io_stream_set_error(&stream->iostream,
					"EOF before trailing <\"> was seen");
				stream->istream.stream_errno = EPIPE;
			}
			return ret;
		}
		size = i_stream_get_data_size(stream->parent);
	}

	if (!i_stream_try_alloc(stream, size, &avail))
		return -2;
	return 1;
}

static int
i_stream_json_unescape(const unsigned char *src, size_t len,
		       unsigned char *dest,
		       unsigned int *src_size_r, unsigned int *dest_size_r)
{
	switch (*src) {
	case '"':
	case '\\':
	case '/':
		*dest = *src;
		break;
	case 'b':
		*dest = '\b';
		break;
	case 'f':
		*dest = '\f';
		break;
	case 'n':
		*dest = '\n';
		break;
	case 'r':
		*dest = '\r';
		break;
	case 't':
		*dest = '\t';
		break;
	case 'u': {
		char chbuf[5] = {0};
		unichar_t chr,chr2 = 0;
		buffer_t buf;
		if (len < 5)
			return 5;
		buffer_create_from_data(&buf, dest, MAX_UTF8_LEN);
		memcpy(chbuf, src+1, 4);
		if (str_to_uint32_hex(chbuf, &chr)<0)
			return -1;
		if (UTF16_VALID_LOW_SURROGATE(chr))
			return -1;
		/* if we encounter surrogate, we need another \\uxxxx */
		if (UTF16_VALID_HIGH_SURROGATE(chr)) {
			if (len < 5+2+4)
				return 5+2+4;
			if (src[5] != '\\' && src[6] != 'u')
				return -1;
			memcpy(chbuf, src+7, 4);
			if (str_to_uint32_hex(chbuf, &chr2)<0)
				return -1;
			if (!UTF16_VALID_LOW_SURROGATE(chr2))
				return -1;
			chr = uni_join_surrogate(chr, chr2);
		}
		if (!uni_is_valid_ucs4(chr))
			return -1;
		uni_ucs4_to_utf8_c(chr, &buf);
		*src_size_r = 5 + (chr2>0?6:0);
		*dest_size_r = buf.used;
		return 0;
	}
	default:
		return -1;
	}
	*src_size_r = 1;
	*dest_size_r = 1;
	return 0;
}

static ssize_t i_stream_jsonstr_read(struct istream_private *stream)
{
	struct jsonstr_istream *jstream =
		container_of(stream, struct jsonstr_istream, istream);
	const unsigned char *data;
	unsigned int srcskip, destskip, extra;
	size_t i, dest, size;
	ssize_t ret, ret2;

	if (jstream->str_end) {
		stream->istream.eof = TRUE;
		return -1;
	}

	ret = i_stream_jsonstr_read_parent(jstream, 1);
	if (ret <= 0)
		return ret;

	/* @UNSAFE */
	dest = stream->pos;
	extra = 0;

	data = i_stream_get_data(stream->parent, &size);
	for (i = 0; i < size && dest < stream->buffer_size; ) {
		if (data[i] == '"') {
			jstream->str_end = TRUE;
			if (dest == stream->pos) {
				stream->istream.eof = TRUE;
				return -1;
			}
			break;
		} else if (data[i] == '\\') {
			if (i+1 == size) {
				/* not enough input for \x */
				extra = 1;
				break;
			}
			if (data[i+1] == 'u' && stream->buffer_size - dest < MAX_UTF8_LEN) {
				/* UTF8 output is max. 6 chars */
				if (dest == stream->pos)
					return -2;
				break;
			}
			i++;
			if ((ret2 = i_stream_json_unescape(data + i, size - i,
							   stream->w_buffer + dest,
							   &srcskip, &destskip)) < 0) {
				/* invalid string */
				io_stream_set_error(&stream->iostream,
						    "Invalid JSON string");
				stream->istream.stream_errno = EINVAL;
				return -1;
			} else if (ret2 > 0) {
				/* we need to get more bytes, do not consume
				   escape slash */
				i--;
				extra = ret2;
				break;
			}
			i += srcskip;
			i_assert(i <= size);
			dest += destskip;
			i_assert(dest <= stream->buffer_size);
		} else {
			stream->w_buffer[dest++] = data[i];
			i++;
		}
	}
	i_stream_skip(stream->parent, i);

	ret = dest - stream->pos;
	if (ret == 0) {
		/* not enough input */
		i_assert(i == 0);
		i_assert(extra > 0);
		ret = i_stream_jsonstr_read_parent(jstream, extra+1);
		if (ret <= 0)
			return ret;
		return i_stream_jsonstr_read(stream);
	}
	i_assert(ret > 0);
	stream->pos = dest;
	return ret;
}

struct istream *i_stream_create_jsonstr(struct istream *input)
{
	struct jsonstr_istream *dstream;

	dstream = i_new(struct jsonstr_istream, 1);
	dstream->istream.max_buffer_size = input->real_stream->max_buffer_size;
	dstream->istream.read = i_stream_jsonstr_read;

	dstream->istream.istream.readable_fd = FALSE;
	dstream->istream.istream.blocking = input->blocking;
	dstream->istream.istream.seekable = FALSE;
	return i_stream_create(&dstream->istream, input,
			       i_stream_get_fd(input), 0);
}
