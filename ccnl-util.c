/*
 * @f ccnl-util.c
 * @b CCN lite, common utility procedures for applications (not the relays)
 *
 * Copyright (C) 2011-13, Christian Tschudin, University of Basel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * File history:
 * 2014-06-18 created
 */


int
hex2int(char c)
{
    if (c >= '0' && c <= '9')
	return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f')
	return c - 'a' + 0x0a;
    return 0;
}

int
unescape_component(unsigned char *comp) // inplace, returns len after shrinking
{
    unsigned char *in = comp, *out = comp;
    int len;

    for (len = 0; *in; len++) {
	if (in[0] != '%' || !in[1] || !in[2]) {
	    *out++ = *in++;
	    continue;
	}
	*out++ = hex2int(in[1])*16 + hex2int(in[2]);
	in += 3;
    }
    return len;
}

// ----------------------------------------------------------------------

int
ccnl_pkt2suite(unsigned char *data, int len)
{
    if (len <= 0)
	return -1;

#ifdef USE_SUITE_CCNB
    if (*data == 0x01 || *data == 0x04)
	return CCNL_SUITE_CCNB;
#endif

#ifdef USE_SUITE_CCNTLV
    if (data[0] == 0 && len > 1) {
	if (data[1] == CCNX_TLV_TL_Interest ||
	    data[1] == CCNX_TLV_TL_Object)
	    return CCNL_SUITE_CCNTLV;
    }
#endif

#ifdef USE_SUITE_NDNTLV
    if (*data == 0x05 || *data == 0x06)
	return CCNL_SUITE_NDNTLV;
#endif

#ifdef USE_SUITE_LOCALRPC
    if (*data == 0x80)
	return CCNL_SUITE_LOCALRPC;
#endif

    return -1;
}

// ----------------------------------------------------------------------

