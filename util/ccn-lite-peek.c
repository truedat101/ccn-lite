/*
 * @f util/ccn-lite-peek.c
 * @b request content: send an interest, wait for reply, output to stdout
 *
 * Copyright (C) 2013-14, Christian Tschudin, University of Basel
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
 * 2013-04-06  created
 * 2014-06-18  added NDNTLV support
 */

#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <arpa/inet.h>


#define USE_SUITE_CCNB
#define USE_SUITE_NDNTLV

#include "../ccnl.h"

#include "ccnl-common.c"

#ifdef USE_SUITE_CCNB
# include "../pkt-ccnb.h"
#endif

#ifdef USE_SUITE_NDNTLV
# include "../pkt-ndntlv.h"
#endif

#include "../ccnl-util.c"

#ifdef USE_SUITE_CCNB
# include "../pkt-ccnb-dec.c"
# include "../pkt-ccnb-enc.c"
#endif

#ifdef USE_SUITE_NDNTLV
# include "../pkt-ndntlv-dec.c"
# include "../pkt-ndntlv-enc.c"
#endif


// ----------------------------------------------------------------------

char *unix_path;

void
myexit(int rc)
{
    if (unix_path)
	unlink(unix_path);
    exit(rc);
}

// ----------------------------------------------------------------------

int
ndntlv_mkInterest(char **namecomp, int *nonce,
		  unsigned char *out, int outlen)
{
    int len, offset;

    offset = outlen;
    len = ccnl_ndntlv_mkInterest(namecomp, -1, nonce, &offset, out);
    memmove(out, out + offset, len);

    return len;
}



// ----------------------------------------------------------------------

int
udp_open()
{
    int s;
    struct sockaddr_in si;

    s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
	perror("udp socket");
	exit(1);
    }
    si.sin_addr.s_addr = INADDR_ANY;
    si.sin_port = htons(0);
    si.sin_family = PF_INET;
    if (bind(s, (struct sockaddr *)&si, sizeof(si)) < 0) {
        perror("udp sock bind");
	exit(1);
    }

    return s;
}

int
ux_open()
{
static char mysockname[200];
 int sock, bufsize;
    struct sockaddr_un name;

    sprintf(mysockname, "/tmp/.ccn-lite-peek-%d.sock", getpid());
    unlink(mysockname);

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
	perror("opening datagram socket");
	exit(1);
    }
    name.sun_family = AF_UNIX;
    strcpy(name.sun_path, mysockname);
    if (bind(sock, (struct sockaddr *) &name,
	     sizeof(struct sockaddr_un))) {
	perror("binding path name to datagram socket");
	exit(1);
    }

    bufsize = 4 * CCNL_MAX_PACKET_SIZE;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    unix_path = mysockname;
    return sock;
}

// ----------------------------------------------------------------------

int
block_on_read(int sock, float wait)
{
    fd_set readfs;
    struct timeval timeout;
    int rc;

    FD_ZERO(&readfs);
    FD_SET(sock, &readfs);
    timeout.tv_sec = wait;
    timeout.tv_usec = 1000000.0 * (wait - timeout.tv_sec);
    rc = select(sock+1, &readfs, NULL, NULL, &timeout);
    if (rc < 0)
	perror("select()");
    return rc;
}

#ifdef USE_SUITE_CCNB
int ccnb_isContent(unsigned char *buf, int len)
{
    int num, typ;
    if (len < 0 || ccnl_ccnb_dehead(&buf, &len, &num, &typ))
	return -1;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ)
	return 0;
    return 1;
}
#endif

#ifdef USE_SUITE_NDNTLV
int ndntlv_isData(unsigned char *buf, int len)
{
    int typ, vallen;
    if (len < 0 || ccnl_ndntlv_dehead(&buf, &len, &typ, &vallen))
	return -1;
    if (typ != NDN_TLV_Data)
	return 0;
    return 1;
}
#endif


// ----------------------------------------------------------------------

int
main(int argc, char *argv[])
{
    unsigned char out[64*1024];
    int cnt, i, len, opt, sock = 0, suite = CCNL_SUITE_NDNTLV;
    char *prefix[CCNL_MAX_NAME_COMP], *udp = "127.0.0.1/6363", *ux = NULL;
    struct sockaddr sa;
    float wait = 3.0;
    int (*mkInterest)(char**,int*,unsigned char*,int);
    int (*isContent)(unsigned char*,int);

    while ((opt = getopt(argc, argv, "hs:u:w:x:")) != -1) {
        switch (opt) {
        case 's':
	    suite = atoi(optarg);
	    break;
        case 'u':
	    udp = optarg;
	    break;
        case 'w':
	    wait = atof(optarg);
	    break;
        case 'x':
	    ux = optarg;
	    break;
        case 'h':
        default:
Usage:
	    fprintf(stderr, "usage: %s "
	    "[-u host/port] [-x ux_path_name] [-w timeout] URI\n"
	    "  -s SUITE         0=ccnb, 1=ccntlv, 2=ndntlv (default)\n"
	    "  -u a.b.c.d/port  UDP destination (default is 127.0.0.1/6363)\n"
	    "  -x ux_path_name  UNIX IPC: use this instead of UDP\n"
	    "  -w timeout       in sec (float)\n"
	    "Example URI: /ndn/edu/wustl/ping\n",
	    argv[0]);
	    exit(1);
        }
    }

    if (!argv[optind]) 
	goto Usage;

    srandom(time(NULL));

    switch(suite) {
#ifdef USE_SUITE_CCNB
    case CCNL_SUITE_CCNB:
	mkInterest = ccnl_ccnb_mkInterest;
	isContent = ccnb_isContent;
	break;
#endif
#ifdef USE_SUITE_NDNTLV
    case CCNL_SUITE_NDNTLV:
	mkInterest = ndntlv_mkInterest;
	isContent = ndntlv_isData;
	break;
#endif
    default:
	printf("CCNx-TLV not supported at this time, aborting\n");
	exit(-1);
    }

    if (ux) { // use UNIX socket
	struct sockaddr_un *su = (struct sockaddr_un*) &sa;
	su->sun_family = AF_UNIX;
	strcpy(su->sun_path, ux);
	sock = ux_open();
    } else { // UDP
	struct sockaddr_in *si = (struct sockaddr_in*) &sa;
	si->sin_family = PF_INET;
	si->sin_addr.s_addr = inet_addr(strtok(udp, "/"));
	si->sin_port = htons(atoi(strtok(NULL, "/")));
	sock = udp_open();
    }

    for (cnt = 0; cnt < 3; cnt++) {
	char *uri = strdup(argv[optind]), *cp;
	int nonce = random();

	cp = strtok(argv[optind], "/");
	while (i < (CCNL_MAX_NAME_COMP - 1) && cp) {
	    prefix[i++] = cp;
	    cp = strtok(NULL, "/");
	}
	prefix[i] = NULL;
	len = mkInterest(prefix, &nonce, out, sizeof(out));
	free(uri);

	if (sendto(sock, out, len, 0, &sa, sizeof(sa)) < 0) {
	    perror("sendto");
	    myexit(1);
	}

	for (;;) { // wait for a content pkt (ignore interests)
	    int rc;

	    if (block_on_read(sock, wait) <= 0) // timeout
		break;
	    len = recv(sock, out, sizeof(out), 0);
/*
	    fprintf(stderr, "received %d bytes\n", len);
	    if (len > 0)
		fprintf(stderr, "  suite=%d\n", ccnl_pkt2suite(out, len));
*/
	    rc = isContent(out, len);
	    if (rc < 0)
		goto done;
	    if (rc == 0) { // it's an interest, ignore it
		fprintf(stderr, "skipping non-data packet\n");
		continue;
	    }
	    write(1, out, len);
	    myexit(0);
	}
	if (cnt < 2)
	    fprintf(stderr, "re-sending interest\n");
    }
    fprintf(stderr, "timeout\n");

done:
    close(sock);
    myexit(-1);
    return 0; // avoid a compiler warning
}

// eof
