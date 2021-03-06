/*
 * @f fwd-ccnb.c
 * @b CCN lite, core CCNx protocol logic (pre 2014)
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
 * 2011-04-09 created
 * 2013-10-12 add crypto support <christopher.scherb@unibas.ch>
 * 2014-03-20 extracted forwarding code from ccnl-core.c
 */

#include "pkt-ccnb.h"
#include "pkt-ccnb-dec.c"


struct ccnl_buf_s*
ccnl_ccnb_extract(unsigned char **data, int *datalen,
		  int *scope, int *aok, int *min, int *max,
		  struct ccnl_prefix_s **prefix,
		  struct ccnl_buf_s **nonce,
		  struct ccnl_buf_s **ppkd,
		  unsigned char **content, int *contlen)
{
    unsigned char *start = *data - 2 /* account for outer TAG hdr */, *cp;
    int num, typ, len;
    struct ccnl_prefix_s *p;
    struct ccnl_buf_s *buf, *n = 0, *pub = 0;
    DEBUGMSG(99, "ccnl_ccnb_extract\n");

    p = (struct ccnl_prefix_s *) ccnl_calloc(1, sizeof(struct ccnl_prefix_s));
    if (!p) return NULL;
    p->comp = (unsigned char**) ccnl_malloc(CCNL_MAX_NAME_COMP *
					   sizeof(unsigned char**));
    p->complen = (int*) ccnl_malloc(CCNL_MAX_NAME_COMP * sizeof(int));
    if (!p->comp || !p->complen) goto Bail;

    while (ccnl_ccnb_dehead(data, datalen, &num, &typ) == 0) {
	if (num==0 && typ==0) break; // end
	if (typ == CCN_TT_DTAG) {
	    if (num == CCN_DTAG_NAME) {
		for (;;) {
		    if (ccnl_ccnb_dehead(data, datalen, &num, &typ) != 0)
			goto Bail;
		    if (num==0 && typ==0)
			break;
		    if (typ == CCN_TT_DTAG && num == CCN_DTAG_COMPONENT &&
			p->compcnt < CCNL_MAX_NAME_COMP) {
			if (ccnl_ccnb_hunt_for_end(data, datalen, p->comp + p->compcnt,
				p->complen + p->compcnt) < 0) goto Bail;
			p->compcnt++;
		    } else {
			if (ccnl_ccnb_consume(typ, num, data, datalen, 0, 0) < 0)
			    goto Bail;
		    }
		}
		continue;
	    }
	    if (num == CCN_DTAG_SCOPE || num == CCN_DTAG_NONCE ||
		num == CCN_DTAG_MINSUFFCOMP || num == CCN_DTAG_MAXSUFFCOMP ||
					 num == CCN_DTAG_PUBPUBKDIGEST) {
		if (ccnl_ccnb_hunt_for_end(data, datalen, &cp, &len) < 0) goto Bail;
		if (num == CCN_DTAG_SCOPE && len == 1 && scope)
		    *scope = isdigit(*cp) && (*cp < '3') ? *cp - '0' : -1;
		if (num == CCN_DTAG_ANSWERORIGKIND && aok)
		    *aok = ccnl_ccnb_data2uint(cp, len);
		if (num == CCN_DTAG_MINSUFFCOMP && min)
		    *min = ccnl_ccnb_data2uint(cp, len);
		if (num == CCN_DTAG_MAXSUFFCOMP && max)
		    *max = ccnl_ccnb_data2uint(cp, len);
		if (num == CCN_DTAG_NONCE && !n)
		    n = ccnl_buf_new(cp, len);
		if (num == CCN_DTAG_PUBPUBKDIGEST && !pub)
		    pub = ccnl_buf_new(cp, len);
		if (num == CCN_DTAG_EXCLUDE) {
		    DEBUGMSG(49, "warning: 'exclude' field ignored\n");
		} else
		    continue;
	    }
	    if (num == CCN_DTAG_CONTENT) {
		if (ccnl_ccnb_consume(typ, num, data, datalen, content, contlen) < 0)
		    goto Bail;
		continue;
	    }
	}
	if (ccnl_ccnb_consume(typ, num, data, datalen, 0, 0) < 0) goto Bail;
    }
    if (prefix)    *prefix = p;    else free_prefix(p);
    if (nonce)     *nonce = n;     else ccnl_free(n);
    if (ppkd)      *ppkd = pub;    else ccnl_free(pub);

    buf = ccnl_buf_new(start, *data - start);
    // carefully rebase ptrs to new buf because of 64bit pointers:
    if (content)
	*content = buf->data + (*content - start);
    for (num = 0; num < p->compcnt; num++)
	    p->comp[num] = buf->data + (p->comp[num] - start);
    return buf;
Bail:
    free_prefix(p);
    free_2ptr_list(n, pub);
    return NULL;
}


int
ccnl_ccnb_forwarder(struct ccnl_relay_s *relay, struct ccnl_face_s *from,
		    unsigned char **data, int *datalen)
{
    int rc= -1, scope=3, aok=3, minsfx=0, maxsfx=CCNL_MAX_NAME_COMP, contlen;
    struct ccnl_buf_s *buf = 0, *nonce=0, *ppkd=0;
    struct ccnl_interest_s *i = 0;
    struct ccnl_content_s *c = 0;
    struct ccnl_prefix_s *p = 0;
    unsigned char *content = 0;
    DEBUGMSG(99, "ccnl/ccnb forwarder (%d bytes left)\n", *datalen);

    buf = ccnl_ccnb_extract(data, datalen, &scope, &aok, &minsfx,
			    &maxsfx, &p, &nonce, &ppkd, &content, &contlen);
    if (!buf) {
	    DEBUGMSG(6, "  parsing error or no prefix\n"); goto Done;
    }
    if (nonce && ccnl_nonce_find_or_append(relay, nonce)) {
	DEBUGMSG(6, "  dropped because of duplicate nonce\n"); goto Skip;
    }
    if (buf->data[0] == 0x01 && buf->data[1] == 0xd2) { // interest
	DEBUGMSG(6, "  interest=<%s>\n", ccnl_prefix_to_path(p));
	ccnl_print_stats(relay, STAT_RCV_I); //log count recv_interest
	if (p->compcnt > 0 && p->comp[0][0] == (unsigned char) 0xc1) goto Skip;
	if (p->compcnt == 4 && !memcmp(p->comp[0], "ccnx", 4)) {
	    rc = ccnl_mgmt(relay, buf, p, from); goto Done;
	}
	// CONFORM: Step 1:
	if ( aok & 0x01 ) { // honor "answer-from-existing-content-store" flag
	    for (c = relay->contents; c; c = c->next) {
		if (c->suite != CCNL_SUITE_CCNB) continue;
		if (!ccnl_i_prefixof_c(p, minsfx, maxsfx, c)) continue;
		if (ppkd && !buf_equal(ppkd, c->details.ccnb.ppkd)) continue;
		// FIXME: should check stale bit in aok here
		DEBUGMSG(7, "  matching content for interest, content %p\n", (void *) c);
		ccnl_print_stats(relay, STAT_SND_C); //log sent_c
		if (from->ifndx >= 0)
		    ccnl_face_enqueue(relay, from, buf_dup(c->pkt));
		else
		    ccnl_app_RX(relay, c);
		goto Skip;
	    }
	}
	// CONFORM: Step 2: check whether interest is already known
	for (i = relay->pit; i; i = i->next) {
	    if (i->suite == CCNL_SUITE_CCNB &&
		!ccnl_prefix_cmp(i->prefix, NULL, p, CMP_EXACT) &&
		i->details.ccnb.minsuffix == minsfx &&
		i->details.ccnb.maxsuffix == maxsfx && 
		((!ppkd && !i->details.ccnb.ppkd) ||
		   buf_equal(ppkd, i->details.ccnb.ppkd)) )
		break;
	}
	if (!i) { // this is a new/unknown I request: create and propagate
	    i = ccnl_interest_new(relay, from, CCNL_SUITE_CCNB,
				  &buf, &p, minsfx, maxsfx);
	    if (ppkd)
		i->details.ccnb.ppkd = ppkd, ppkd = NULL;
	    if (i) { // CONFORM: Step 3 (and 4)
		DEBUGMSG(7, "  created new interest entry %p\n", (void *) i);
		if (scope > 2)
		    ccnl_interest_propagate(relay, i);
	    }
	} else if (scope > 2 && (from->flags & CCNL_FACE_FLAGS_FWDALLI)) {
	    DEBUGMSG(7, "  old interest, nevertheless propagated %p\n", (void *) i);
	    ccnl_interest_propagate(relay, i);
	}
	if (i) { // store the I request, for the incoming face (Step 3)
	    DEBUGMSG(7, "  appending interest entry %p\n", (void *) i);
	    ccnl_interest_append_pending(i, from);
	}
    } else { // content
	DEBUGMSG(6, "  content=<%s>\n", ccnl_prefix_to_path(p));
	ccnl_print_stats(relay, STAT_RCV_C); //log count recv_content
        
#ifdef USE_SIGNATURES
        if (p->compcnt == 2 && !memcmp(p->comp[0], "ccnx", 4)
                && !memcmp(p->comp[1], "crypto", 6) &&
                from == relay->crypto_face) {
	    rc = ccnl_crypto(relay, buf, p, from); goto Done;
	}
#endif /*USE_SIGNATURES*/
        
        // CONFORM: Step 1:
	for (c = relay->contents; c; c = c->next)
	    if (buf_equal(c->pkt, buf)) goto Skip; // content is dup
	c = ccnl_content_new(relay, CCNL_SUITE_CCNB,
			     &buf, &p, &ppkd, content, contlen);
	if (c) { // CONFORM: Step 2 (and 3)
	    if (!ccnl_content_serve_pending(relay, c)) { // unsolicited content
		// CONFORM: "A node MUST NOT forward unsolicited data [...]"
		DEBUGMSG(7, "  removed because no matching interest\n");
		free_content(c);
		goto Skip;
	    }
	    if (relay->max_cache_entries != 0) { // it's set to -1 or a limit
		DEBUGMSG(7, "  adding content to cache\n");
		ccnl_content_add2cache(relay, c);
	    } else {
		DEBUGMSG(7, "  content not added to cache\n");
		free_content(c);
	    }
	}
    }
Skip:
    rc = 0;
Done:
    free_prefix(p);
    free_3ptr_list(buf, nonce, ppkd);
    return rc;
}

int
ccnl_RX_ccnb(struct ccnl_relay_s *relay, struct ccnl_face_s *from,
	      unsigned char **data, int *datalen)
{
    int rc = 0, num, typ;
    DEBUGMSG(6, "ccnl_RX_ccnb: %d bytes from face=%p (id=%d.%d)\n",
	     *datalen, (void*)from, relay->id, from ? from->faceid : -1);

    while (rc >= 0 && *datalen > 0) {
	if (ccnl_ccnb_dehead(data, datalen, &num, &typ) || typ != CCN_TT_DTAG)
	    return -1;
	switch (num) {
	case CCN_DTAG_INTEREST:
	case CCN_DTAG_CONTENTOBJ:
	    rc = ccnl_ccnb_forwarder(relay, from, data, datalen);
	    continue;
#ifdef USE_FRAG
	case CCNL_DTAG_FRAGMENT2012:
	    rc = ccnl_frag_RX_frag2012(ccnl_RX_ccnb, relay, from, data, datalen);
	    continue;
	case CCNL_DTAG_FRAGMENT2013:
	    rc = ccnl_frag_RX_CCNx2013(ccnl_RX_ccnb, relay, from, data, datalen);
	    continue;
#endif
	default:
	    DEBUGMSG(15, "  unknown datagram type %d\n", num);
	    return -1;
	}
    }
    return rc;
}

// eof
