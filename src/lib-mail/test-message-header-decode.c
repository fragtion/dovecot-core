/* Copyright (c) 2009-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "str.h"
#include "charset-utf8.h"
#include "message-header-encode.h"
#include "message-header-decode.h"
#include "test-common.h"


bool charset_is_utf8(const char *charset ATTR_UNUSED) { return TRUE; }

int charset_to_utf8_begin(const char *charset ATTR_UNUSED,
			  normalizer_func_t *normalizer ATTR_UNUSED,
			  struct charset_translation **t_r ATTR_UNUSED) { return 0; }
void charset_to_utf8_end(struct charset_translation **t ATTR_UNUSED) {}

enum charset_result
charset_to_utf8(struct charset_translation *t ATTR_UNUSED,
		const unsigned char *src, size_t *src_size, buffer_t *dest)
{
	buffer_append(dest, src, *src_size);
	return CHARSET_RET_OK;
}

static void test_message_header_decode(void)
{
	static const char *data[] = {
		" \t=?utf-8?q?=c3=a4?=  =?utf-8?q?=c3=a4?=  b  \t\r\n ", "\xC3\xA4\xC3\xA4  b  \t\r\n ",
		"a =?utf-8?q?=c3=a4?= b", "a \xC3\xA4 b",
		"a =?utf-8?q?=c3=a4?= b", "a \xC3\xA4 b",
		"a =?utf-8?q?=c3=a4?=\t\t\r\n =?utf-8?q?=c3=a4?= b", "a \xC3\xA4\xC3\xA4 b",
		"a =?utf-8?q?=c3=a4?=  x  =?utf-8?q?=c3=a4?= b", "a \xC3\xA4  x  \xC3\xA4 b",
		"a =?utf-8?b?w6TDpCDDpA==?= b", "a \xC3\xA4\xC3\xA4 \xC3\xA4 b",
		"=?utf-8?b?w6Qgw6Q=?=", "\xC3\xA4 \xC3\xA4",
	};
	string_t *dest;
	unsigned int i;

	test_begin("message header decode");

	dest = t_str_new(256);
	for (i = 0; i < N_ELEMENTS(data); i += 2) {
		str_truncate(dest, 0);
		message_header_decode_utf8((const unsigned char *)data[i],
					   strlen(data[i]), dest, NULL);
		test_assert_strcmp_idx(str_c(dest), data[i+1], i / 2);
	}
	test_end();
}

static void test_message_header_decode_read_overflow(void)
{
	const unsigned char input[] = "=?utf-8?Q?=EF?=";
	string_t *dest = t_str_new(32);

	test_begin("message header decode read overflow");
	message_header_decode_utf8(input, sizeof(input)-2, dest, NULL);
	test_end();
}

static void check_encoded(string_t *encoded, unsigned int test_idx)
{
	const unsigned char *enc = str_data(encoded), *p, *pend;
	size_t enc_len = str_len(encoded), cur_line_len = 0;

	p = enc;
	pend = enc + enc_len;
	while (p < pend) {
		if (*p == '\r') {
			p++;
			continue;
		}
		if (*p == '\n') {
			test_assert_idx(cur_line_len <= 76, test_idx);
			cur_line_len = 0;
			p++;
			continue;
		}
		cur_line_len++;
		test_assert_idx((*p >= 0x20 && *p <= 0x7e) || *p == '\t',
				test_idx);
		p++;
	}

	test_assert_idx(cur_line_len <= 76, test_idx);
}

static void test_message_header_decode_encode_random(void)
{
	string_t *encoded, *decoded;
	unsigned char buf[1024];
	unsigned int i, j, buflen;

	test_begin("message header encode & decode randomly");

	encoded = t_str_new(256);
	decoded = t_str_new(256);
	for (i = 0; i < 1000; i++) {
		/* fill only with 7bit data so we don't have to worry about
		   the data being valid UTF-8 */
		buflen = i_rand_limit(sizeof(buf));
		for (j = 0; j < buflen; j++)
			buf[j] = i_rand_limit(128);

		str_truncate(encoded, 0);
		str_truncate(decoded, 0);

		/* test Q */
		message_header_encode_q(buf, buflen, encoded, 0);
		check_encoded(encoded, i);
		message_header_decode_utf8(encoded->data, encoded->used,
					   decoded, NULL);
		test_assert_idx(decoded->used == buflen &&
				memcmp(decoded->data, buf, buflen) == 0, i);

		/* test B */
		str_truncate(encoded, 0);
		str_truncate(decoded, 0);

		message_header_encode_b(buf, buflen, encoded, 0);
		check_encoded(encoded, i);
		message_header_decode_utf8(encoded->data, encoded->used,
					   decoded, NULL);
		test_assert_idx(decoded->used == buflen &&
				memcmp(decoded->data, buf, buflen) == 0, i);
	}
	test_end();
}

int main(void)
{
	static void (*const test_functions[])(void) = {
		test_message_header_decode,
		test_message_header_decode_read_overflow,
		test_message_header_decode_encode_random,
		NULL
	};
	return test_run(test_functions);
}
