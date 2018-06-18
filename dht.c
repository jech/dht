/*
Copyright (c) 2009-2011 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/* Please, please, please.

   You are welcome to integrate this code in your favourite Bittorrent
   client.  Please remember, however, that it is meant to be usable by
   others, including myself.  This means no C++, no relicensing, and no
   gratuitious changes to the coding style.  And please send back any
   improvements to the author. */

/* For memmem. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>

#if !defined(_WIN32) || defined(__MINGW32__)
#include <sys/time.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501 /* Windows XP */
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "dht.h"

#ifndef HAVE_MEMMEM
#ifdef __GLIBC__
#define HAVE_MEMMEM
#endif
#endif

#ifndef MSG_CONFIRM
#define MSG_CONFIRM 0
#endif

#if !defined(_WIN32) || defined(__MINGW32__)
#define dht_gettimeofday(_ts, _tz) gettimeofday((_ts), (_tz))
#else
extern int dht_gettimeofday(struct timeval *tv, struct timezone *tz);
#endif

#ifdef _WIN32

#undef EAFNOSUPPORT
#define EAFNOSUPPORT WSAEAFNOSUPPORT

static int
random(void)
{
    return rand();
}

/* Windows Vista and later already provide the implementation. */
#if _WIN32_WINNT < 0x0600
extern const char *inet_ntop(int, const void *, char *, socklen_t);
#endif

#ifdef _MSC_VER
/* There is no snprintf in MSVCRT. */
#define snprintf _snprintf
#endif

#endif

/* We set sin_family to 0 to mark unused slots. */
#if AF_INET == 0 || AF_INET6 == 0
#error You lose
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
/* nothing */
#elif defined(__GNUC__)
#define inline __inline
#if  (__GNUC__ >= 3)
#define restrict __restrict
#else
#define restrict /**/
#endif
#else
#define inline /**/
#define restrict /**/
#endif

#define MAX(x, y) ((x) >= (y) ? (x) : (y))
#define MIN(x, y) ((x) <= (y) ? (x) : (y))

struct node {
    unsigned char id[20];
    struct sockaddr_storage ss;
    int sslen;
    time_t time;                    /* time of last message received */
    time_t reply_time;              /* time of last correct reply received */
    time_t pinged_time;             /* time of last request */
    int pinged;                     /* how many requests we sent since last reply */
    struct node *next;
};

struct bucket {
    int af;
    unsigned char first[20];
    int count;                      /* number of nodes */
    int max_count;                  /* max number of nodes for this bucket */
    time_t time;                    /* time of last reply in this bucket */
    struct node *nodes;
    struct sockaddr_storage cached; /* the address of a likely candidate */
    int cachedlen;
    struct bucket *next;
};

struct search_node {
    unsigned char id[20];
    struct sockaddr_storage ss;
    int sslen;
    time_t request_time;            /* the time of the last unanswered request */
    time_t reply_time;              /* the time of the last reply */
    int pinged;
    unsigned char token[40];
    int token_len;
    int replied;                    /* whether we have received a reply */
    int acked;                      /* whether they acked our announcement */
};

/* When performing a search, we search for up to SEARCH_NODES closest nodes
   to the destination, and use the additional ones to backtrack if any of
   the target 8 turn out to be dead. */
#define SEARCH_NODES 14

struct search {
    unsigned short tid;
    int af;
    time_t step_time;               /* the time of the last search_step */
    unsigned char id[20];
    unsigned short port;            /* 0 for pure searches */
    int done;
    struct search_node nodes[SEARCH_NODES];
    int numnodes;
    struct search *next;
};

struct peer {
    time_t time;
    unsigned char ip[16];
    unsigned short len;
    unsigned short port;
};

struct storage {
    unsigned char id[20];
    int numpeers, maxpeers;
    struct peer *peers;
    struct storage *next;
};

struct bootstrap_node {
    struct sockaddr_storage ss;     /* node address */
    int sslen;                      /* size/length of address ss */
    struct bootstrap_node *next;    /* pointer to next bootstrap node */
};

struct bootstrap {
    int state;                      /* bootstrap state */
    struct bootstrap_node *nodes;   /* list of bootstrap nodes */
    int numnodes;                   /* number of bootstrap nodes */
    time_t start_time;              /* time when bootstrap started */
    time_t end_time;                /* time when bootstrap ended */
    time_t next_time;               /* time of next bootstrap iteration */
};

/* The maximum number of peers we store for a given hash. */
#ifndef DHT_MAX_PEERS
#define DHT_MAX_PEERS 2048
#endif

/* The maximum number of hashes we're willing to track. */
#ifndef DHT_MAX_HASHES
#define DHT_MAX_HASHES 16384
#endif

/* The maximum number of searches we keep data about. */
#ifndef DHT_MAX_SEARCHES
#define DHT_MAX_SEARCHES 1024
#endif

/* The time after which we consider a search to be expirable. */
#ifndef DHT_SEARCH_EXPIRE_TIME
#define DHT_SEARCH_EXPIRE_TIME (62 * 60)
#endif

/* The maximum number of in-flight queries per search. */
#ifndef DHT_INFLIGHT_QUERIES
#define DHT_INFLIGHT_QUERIES 4
#endif

/* The retransmit timeout when performing searches. */
#ifndef DHT_SEARCH_RETRANSMIT
#define DHT_SEARCH_RETRANSMIT 10
#endif

/* Interval in seconds between bootstrap iterations. */
#ifndef DHT_BOOTSTRAP_INTERVAL
#define DHT_BOOTSTRAP_INTERVAL 3
#endif

/* Number of good nodes bootstrap should yield. */
#ifndef DHT_BOOTSTRAP_GOOD_TARGET
#define DHT_BOOTSTRAP_GOOD_TARGET 50
#endif

/* Maximum number of dubious nodes during bootstrap. */
#ifndef DHT_BOOTSTRAP_MAX_DUBIOUS
#define DHT_BOOTSTRAP_MAX_DUBIOUS 50
#endif

/* Maximum number of find_nodes sent per iteration. */
#ifndef DHT_BOOTSTRAP_MAX_FINDS
#define DHT_BOOTSTRAP_MAX_FINDS 5
#endif

/* Maximum number of pings sent per iteration. */
#ifndef DHT_BOOTSTRAP_MAX_PINGS
#define DHT_BOOTSTRAP_MAX_PINGS 10
#endif

/* Number of nodes we expect to receive for each find_node sent.*/
#ifndef DHT_BOOTSTRAP_EXPECTED_NODES
#define DHT_BOOTSTRAP_EXPECTED_NODES 8
#endif

static struct storage * find_storage(const unsigned char *id);
static void flush_search_node(struct search_node *n, struct search *sr);

static int send_ping(const struct sockaddr *sa, int salen,
                     const unsigned char *tid, int tid_len);
static int send_pong(const struct sockaddr *sa, int salen,
                     const unsigned char *tid, int tid_len);
static int send_find_node(const struct sockaddr *sa, int salen,
                          const unsigned char *tid, int tid_len,
                          const unsigned char *target, int want, int confirm);
static int send_nodes_peers(const struct sockaddr *sa, int salen,
                            const unsigned char *tid, int tid_len,
                            const unsigned char *nodes, int nodes_len,
                            const unsigned char *nodes6, int nodes6_len,
                            int af, struct storage *st,
                            const unsigned char *token, int token_len);
static int send_closest_nodes(const struct sockaddr *sa, int salen,
                              const unsigned char *tid, int tid_len,
                              const unsigned char *id, int want,
                              int af, struct storage *st,
                              const unsigned char *token, int token_len);
static int send_get_peers(const struct sockaddr *sa, int salen,
                          unsigned char *tid, int tid_len,
                          unsigned char *infohash, int want, int confirm);
static int send_announce_peer(const struct sockaddr *sa, int salen,
                              unsigned char *tid, int tid_len,
                              unsigned char *infohas, unsigned short port,
                              unsigned char *token, int token_len, int confirm);
static int send_peer_announced(const struct sockaddr *sa, int salen,
                               unsigned char *tid, int tid_len);
static int send_error(const struct sockaddr *sa, int salen,
                      unsigned char *tid, int tid_len,
                      int code, const char *message);

static void add_search_node(const unsigned char *id, const struct sockaddr *sa,
                            int salen);

static void bootstrap_update_timer();
static void bootstrap_switch_state(int af, int state, dht_main_callback_t *callback, void *closure);
static void bootstrap_periodic(int af, dht_main_callback_t *callback, void *closure);

#define ERROR 0
#define REPLY 1
#define PING 2
#define FIND_NODE 3
#define GET_PEERS 4
#define ANNOUNCE_PEER 5

#define WANT4 1
#define WANT6 2

#define PARSE_TID_LEN 16
#define PARSE_TOKEN_LEN 128
#define PARSE_NODES_LEN (26 * 16)
#define PARSE_NODES6_LEN (38 * 16)
#define PARSE_VALUES_LEN 2048
#define PARSE_VALUES6_LEN 2048

struct parsed_message {
    unsigned char tid[PARSE_TID_LEN];
    unsigned short tid_len;
    unsigned char id[20];
    unsigned char info_hash[20];
    unsigned char target[20];
    unsigned short port;
    unsigned short implied_port;
    unsigned char token[PARSE_TOKEN_LEN];
    unsigned short token_len;
    unsigned char nodes[PARSE_NODES_LEN];
    unsigned short nodes_len;
    unsigned char nodes6[PARSE_NODES6_LEN];
    unsigned short nodes6_len;
    unsigned char values[PARSE_VALUES_LEN];
    unsigned short values_len;
    unsigned char values6[PARSE_VALUES6_LEN];
    unsigned short values6_len;
    unsigned short want;
};

static int parse_message(const unsigned char *buf, int buflen,
                         struct parsed_message *m);

static const unsigned char zeroes[20] = {0};
static const unsigned char v4prefix[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0
};

static int dht_socket = -1;
static int dht_socket6 = -1;

static time_t search_time;
static time_t confirm_nodes_time;
static time_t rotate_secrets_time;
static time_t bootstrap_time = 0 ;      /* time of next bootstrap iteration */

static unsigned char myid[20];
static int have_v = 0;
static unsigned char my_v[9];
static unsigned char secret[8];
static unsigned char oldsecret[8];

static struct bucket *buckets = NULL;
static struct bucket *buckets6 = NULL;
static struct storage *storage;
static int numstorage;

static struct search *searches = NULL;
static int numsearches;
static unsigned short search_id;

static struct bootstrap bootstrap = {
    DHT_BOOTSTRAP_STATE_DISABLED, NULL, 0, 0, 0, 0
};
static struct bootstrap bootstrap6 = {
    DHT_BOOTSTRAP_STATE_DISABLED, NULL, 0, 0, 0, 0
};

/* The maximum number of nodes that we snub.  There is probably little
   reason to increase this value. */
#ifndef DHT_MAX_BLACKLISTED
#define DHT_MAX_BLACKLISTED 10
#endif
static struct sockaddr_storage blacklist[DHT_MAX_BLACKLISTED];
int next_blacklisted;

static struct timeval now;
static time_t mybucket_grow_time, mybucket6_grow_time;
static time_t expire_stuff_time;

#define MAX_TOKEN_BUCKET_TOKENS 400
static time_t token_bucket_time;
static int token_bucket_tokens;

/* Log incoming messages. */
#ifndef DHT_LOG_INCOMING_MESSAGES
#define DHT_LOG_INCOMING_MESSAGES 0
#endif

/* Log outgoing messages. */
#ifndef DHT_LOG_OUTGOING_MESSAGES
#define DHT_LOG_OUTGOING_MESSAGES 0
#endif

/* Mask socket addresses when logging (e.g. for screenshots). */
#ifndef DHT_LOG_MASK_ADDRESSES
#define DHT_LOG_MASK_ADDRESSES 0
#endif

/* Log callback. */
static dht_log_callback_t *dht_log_callback = NULL;

/* Set log callback. */
void
dht_set_log_callback(dht_log_callback_t* callback)
{
    dht_log_callback = callback;
}

/* Log macros. */
#define debugf(format, ...) printlf(DHT_LOG_TYPE_DEBUG, format, ##__VA_ARGS__)
#define  infof(format, ...) printlf(DHT_LOG_TYPE_INFO,  format, ##__VA_ARGS__)
#define  warnf(format, ...) printlf(DHT_LOG_TYPE_WARN,  format, ##__VA_ARGS__)
#define errorf(format, ...) printlf(DHT_LOG_TYPE_ERROR, format, ##__VA_ARGS__)

/* Log message (printlf = print log formatted). */
#ifdef __GNUC__
    __attribute__ ((format(printf, 2, 3)))
#endif
static void
printlf(int type, const char *format, ...)
{
    if(dht_log_callback) {
        va_list ap;
        va_start(ap, format);
        dht_gettimeofday(&now, NULL);
        (*dht_log_callback)(&now, type, format, ap);
        va_end(ap);
    }
}

/* Convert address family to string. */
const char*
af_to_str(int af)
{
    switch(af) {
        case AF_INET:
            return "AF_INET";
        case AF_INET6:
            return "AF_INET6";
        default:
            errno = EAFNOSUPPORT;
            return "";
    }
}

/* Convert address family to IP version string. */
const char*
af_to_ivs(int af)
{
    switch(af) {
        case AF_INET:
            return "IPv4";
        case AF_INET6:
            return "IPv6";
        default:
            errno = EAFNOSUPPORT;
            return "";
    }
}

/* Convert socket address to string. */
/* String length: '[' + length of IPv6 address string (46, includes
   '\0') + ']' + ':' + 5 characters for port -> 54 characters. */
char sa_str[1 + INET6_ADDRSTRLEN + 1 + 1 + 5];
const char*
sa_to_str(const struct sockaddr *sa)
{
    if(!sa) {
        errno = EINVAL;
        return "";
    }

    switch(sa->sa_family) {
        case AF_INET: {
#if (DHT_LOG_MASK_ADDRESSES == 0)
            struct sockaddr_in *sinp = (struct sockaddr_in*)sa;
            char ipstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sinp->sin_addr, ipstr, sizeof(ipstr));
            snprintf(sa_str, sizeof(sa_str), "%s:%u", ipstr, ntohs(sinp->sin_port));
            return sa_str;
#else
            return "xxx.xxx.xxx.xxx:xxxxx";
#endif
        }
        case AF_INET6: {
#if (DHT_LOG_MASK_ADDRESSES == 0)
            struct sockaddr_in6* sinp6 = (struct sockaddr_in6*)sa;
            char ipstr6[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &sinp6->sin6_addr, ipstr6, sizeof(ipstr6));
            snprintf(sa_str, sizeof(sa_str), "[%s]:%u", ipstr6, ntohs(sinp6->sin6_port));
            return sa_str;
#else
            return "[xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx]:xxxxx";
#endif
        }
        default:
            errno = EAFNOSUPPORT;
            return "";
    }
}

/* Convert byte array to printable string. */
/* String length: 2048 bytes + '\0' -> 2049 characters. */
char ba_prn_str[2049 + 1];
const char*
ba_to_str(const unsigned char *ba, int balen)
{
    if(!ba || balen < 0) {
        errno = EINVAL;
        return "";
    }

    int size = (balen < sizeof(ba_prn_str) ? balen : sizeof(ba_prn_str)-1);
    memcpy(ba_prn_str, ba, size);
    for(int i = 0; i < size; i++) {
        if(ba_prn_str[i] < 32 || ba_prn_str[i] > 126)
            ba_prn_str[i] = '.';
    }
    ba_prn_str[size] = '\0';
    return ba_prn_str;
}

/* Convert byte array to hex string. */
/* String length: 2048 bytes x 2 characters + '\0' -> 4097 characters. */
char ba_hex_str[2048*2 + 1];
const char*
ba_to_hex(const unsigned char *ba, int balen)
{
    if(!ba) {
        errno = EINVAL;
        return "";
    }

    //const char* hex_chr = "0123456789ABCDEF";
    const char* hex_chr = "0123456789abcdef";
    for(int i=0,j=0; i < balen && j < sizeof(ba_hex_str)-2; i++) {
        ba_hex_str[j++] = hex_chr[ (ba[i]>>4) & 0x0F ];
        ba_hex_str[j++] = hex_chr[  ba[i]     & 0x0F ];
    }
    ba_hex_str[balen*2] = '\0';
    return ba_hex_str;
}

/* Convert ID of bucket/node to hex string. */
static const char*
id_to_hex(const unsigned char *id)
{
    return ba_to_hex(id, 20);
}

/* Convert v of node to string. */
/* String length: 2 characters for client id + '-' + 3 characters for major
   version + '-' + 3 characters for minor version + '\0' -> 11 characters */
char v_str[11];
static const char*
v_to_str(const unsigned char *v)
{
    if(!v) {
        errno = EINVAL;
        return "";
    }

    snprintf(v_str, sizeof(v_str), "%c%c-%03u-%03u", v[0], v[1], v[2], v[3]);
    return v_str;
}

static int
is_martian(const struct sockaddr *sa)
{
    switch(sa->sa_family) {
    case AF_INET: {
        struct sockaddr_in *sin = (struct sockaddr_in*)sa;
        const unsigned char *address = (const unsigned char*)&sin->sin_addr;
        return sin->sin_port == 0 ||
            (address[0] == 0) ||
            (address[0] == 127) ||
            ((address[0] & 0xE0) == 0xE0);
    }
    case AF_INET6: {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)sa;
        const unsigned char *address = (const unsigned char*)&sin6->sin6_addr;
        return sin6->sin6_port == 0 ||
            (address[0] == 0xFF) ||
            (address[0] == 0xFE && (address[1] & 0xC0) == 0x80) ||
            (memcmp(address, zeroes, 15) == 0 &&
             (address[15] == 0 || address[15] == 1)) ||
            (memcmp(address, v4prefix, 12) == 0);
    }

    default:
        return 0;
    }
}

/* Forget about the ``XOR-metric''.  An id is just a path from the
   root of the tree, so bits are numbered from the start. */

static int
id_cmp(const unsigned char *restrict id1, const unsigned char *restrict id2)
{
    /* Memcmp is guaranteed to perform an unsigned comparison. */
    return memcmp(id1, id2, 20);
}

/* Find the lowest 1 bit in an id. */
static int
lowbit(const unsigned char *id)
{
    int i, j;
    for(i = 19; i >= 0; i--)
        if(id[i] != 0)
            break;

    if(i < 0)
        return -1;

    for(j = 7; j >= 0; j--)
        if((id[i] & (0x80 >> j)) != 0)
            break;

    return 8 * i + j;
}

/* Find how many bits two ids have in common. */
static int
common_bits(const unsigned char *id1, const unsigned char *id2)
{
    int i, j;
    unsigned char xor;
    for(i = 0; i < 20; i++) {
        if(id1[i] != id2[i])
            break;
    }

    if(i == 20)
        return 160;

    xor = id1[i] ^ id2[i];

    j = 0;
    while((xor & 0x80) == 0) {
        xor <<= 1;
        j++;
    }

    return 8 * i + j;
}

/* Determine distance between two ids. */
static int
distance(const unsigned char *id1, const unsigned char *id2)
{
    return 160 - common_bits(id1, id2);
}

/* Determine whether id1 or id2 is closer to ref */
static int
xorcmp(const unsigned char *id1, const unsigned char *id2,
       const unsigned char *ref)
{
    int i;
    for(i = 0; i < 20; i++) {
        unsigned char xor1, xor2;
        if(id1[i] == id2[i])
            continue;
        xor1 = id1[i] ^ ref[i];
        xor2 = id2[i] ^ ref[i];
        if(xor1 < xor2)
            return -1;
        else
            return 1;
    }
    return 0;
}

/* We keep buckets in a sorted linked list.  A bucket b ranges from
   b->first inclusive up to b->next->first exclusive. */
static int
in_bucket(const unsigned char *id, struct bucket *b)
{
    return id_cmp(b->first, id) <= 0 &&
        (b->next == NULL || id_cmp(id, b->next->first) < 0);
}

static struct bucket *
find_bucket(unsigned const char *id, int af)
{
    struct bucket *b = af == AF_INET ? buckets : buckets6;

    if(b == NULL)
        return NULL;

    while(1) {
        if(b->next == NULL)
            return b;
        if(id_cmp(id, b->next->first) < 0)
            return b;
        b = b->next;
    }
}

static struct bucket *
previous_bucket(struct bucket *b)
{
    struct bucket *p = b->af == AF_INET ? buckets : buckets6;

    if(b == p)
        return NULL;

    while(1) {
        if(p->next == NULL)
            return NULL;
        if(p->next == b)
            return p;
        p = p->next;
    }
}

/* Every bucket contains an unordered list of nodes. */
static struct node *
find_node(const unsigned char *id, int af)
{
    struct bucket *b = find_bucket(id, af);
    struct node *n;

    if(b == NULL)
        return NULL;

    n = b->nodes;
    while(n) {
        if(id_cmp(n->id, id) == 0)
            return n;
        n = n->next;
    }
    return NULL;
}

/* Return a random bucket. */
static struct bucket *
random_bucket(struct bucket *root_bucket, int numbuckets)
{
    struct bucket *b = root_bucket;
    int count = numbuckets;
    int bn;

    if(count == 0)
        return NULL;

    bn = random() % count;
    while(bn > 0 && b) {
        b = b->next;
        bn--;
    }
    return b;
}

/* Return a random node in a bucket. */
static struct node *
random_node(struct bucket *b)
{
    struct node *n;
    int nn;

    if(b->count == 0)
        return NULL;

    nn = random() % b->count;
    n = b->nodes;
    while(nn > 0 && n) {
        n = n->next;
        nn--;
    }
    return n;
}

/* Return the middle id of a bucket. */
static int
bucket_middle(struct bucket *b, unsigned char *id_return)
{
    int bit1 = lowbit(b->first);
    int bit2 = b->next ? lowbit(b->next->first) : -1;
    int bit = MAX(bit1, bit2) + 1;

    if(bit >= 160)
        return -1;

    memcpy(id_return, b->first, 20);
    id_return[bit / 8] |= (0x80 >> (bit % 8));
    return 1;
}

/* Return a random id within a bucket. */
static int
bucket_random(struct bucket *b, unsigned char *id_return)
{
    int bit1 = lowbit(b->first);
    int bit2 = b->next ? lowbit(b->next->first) : -1;
    int bit = MAX(bit1, bit2) + 1;
    int i;

    if(bit >= 160) {
        memcpy(id_return, b->first, 20);
        return 1;
    }

    memcpy(id_return, b->first, bit / 8);
    id_return[bit / 8] = b->first[bit / 8] & (0xFF00 >> (bit % 8));
    id_return[bit / 8] |= random() & 0xFF >> (bit % 8);
    for(i = bit / 8 + 1; i < 20; i++)
        id_return[i] = random() & 0xFF;
    return 1;
}

/* This is our definition of a known-good node. */
static int
node_good(struct node *node)
{
    return
        node->pinged <= 2 &&
        node->reply_time >= now.tv_sec - 7200 &&
        node->time >= now.tv_sec - 900;
}

/* Our transaction-ids are 4-bytes long, with the first two bytes identi-
   fying the kind of request, and the remaining two a sequence number in
   host order. */

static void
make_tid(unsigned char *tid_return, const char *prefix, unsigned short seqno)
{
    tid_return[0] = prefix[0] & 0xFF;
    tid_return[1] = prefix[1] & 0xFF;
    memcpy(tid_return + 2, &seqno, 2);
}

static int
tid_match(const unsigned char *tid, const char *prefix,
          unsigned short *seqno_return)
{
    if(tid[0] == (prefix[0] & 0xFF) && tid[1] == (prefix[1] & 0xFF)) {
        if(seqno_return)
            memcpy(seqno_return, tid + 2, 2);
        return 1;
    } else
        return 0;
}

/* Every bucket caches the address of a likely node.  Ping it. */
static int
send_cached_ping(struct bucket *b)
{
    unsigned char tid[4];
    int rc;
    /* We set family to 0 when there's no cached node. */
    if(b->cached.ss_family == 0)
        return 0;

    debugf("Sending ping to cached node %s", sa_to_str((struct sockaddr*)&b->cached));
    make_tid(tid, "pn", 0);
    rc = send_ping((struct sockaddr*)&b->cached, b->cachedlen, tid, 4);
    b->cached.ss_family = 0;
    b->cachedlen = 0;
    return rc;
}

/* Called whenever we send a request to a node, increases the ping count
   and, if that reaches 3, sends a ping to a new candidate. */
static void
pinged(struct node *n, struct bucket *b)
{
    n->pinged++;
    n->pinged_time = now.tv_sec;
    if(n->pinged >= 3)
        send_cached_ping(b ? b : find_bucket(n->id, n->ss.ss_family));
}

/* The internal blacklist is an LRU cache of nodes that have sent
   incorrect messages. */
static void
blacklist_node(const unsigned char *id, const struct sockaddr *sa, int salen)
{
    int i;

    debugf("Blacklisting node %s", sa_to_str(sa));

    if(id) {
        struct node *n;
        struct search *sr;
        /* Make the node easy to discard. */
        n = find_node(id, sa->sa_family);
        if(n) {
            n->pinged = 3;
            pinged(n, NULL);
        }
        /* Discard it from any searches in progress. */
        sr = searches;
        while(sr) {
            for(i = 0; i < sr->numnodes; i++)
                if(id_cmp(sr->nodes[i].id, id) == 0)
                    flush_search_node(&sr->nodes[i], sr);
            sr = sr->next;
        }
    }
    /* And make sure we don't hear from it again. */
    memcpy(&blacklist[next_blacklisted], sa, salen);
    next_blacklisted = (next_blacklisted + 1) % DHT_MAX_BLACKLISTED;
}

static int
node_blacklisted(const struct sockaddr *sa, int salen)
{
    int i;

    if((unsigned)salen > sizeof(struct sockaddr_storage))
        abort();

    if(dht_blacklisted(sa, salen))
        return 1;

    for(i = 0; i < DHT_MAX_BLACKLISTED; i++) {
        if(memcmp(&blacklist[i], sa, salen) == 0)
            return 1;
    }

    return 0;
}

static struct node *
append_nodes(struct node *n1, struct node *n2)
{
    struct node *n;

    if(n1 == NULL)
        return n2;

    if(n2 == NULL)
        return n1;

    n = n1;
    while(n->next != NULL)
        n = n->next;

    n->next = n2;
    return n1;
}

/* Insert a new node into a bucket, don't check for duplicates.
   Returns 1 if the node was inserted, 0 if a bucket must be split. */
static int
insert_node(struct node *node, struct bucket **split_return)
{
    struct bucket *b = find_bucket(node->id, node->ss.ss_family);

    if(b == NULL)
        return -1;

    if(b->count >= b->max_count) {
        *split_return = b;
        return 0;
    }
    node->next = b->nodes;
    b->nodes = node;
    b->count++;
    return 1;
}

/* Splits a bucket, and returns the list of nodes that must be reinserted
   into the routing table. */
static int
split_bucket_helper(struct bucket *b, struct node **nodes_return)
{
    struct bucket *new;
    int rc;
    unsigned char new_id[20];

    if(!in_bucket(myid, b)) {
        errorf("Attempted to split wrong bucket %s", id_to_hex(b->first));
        return -1;
    }

    rc = bucket_middle(b, new_id);
    if(rc < 0)
        return -1;

    new = calloc(1, sizeof(struct bucket));
    if(new == NULL)
        return -1;

    send_cached_ping(b);

    new->af = b->af;
    memcpy(new->first, new_id, 20);
    new->time = b->time;

    *nodes_return = b->nodes;
    b->nodes = NULL;
    b->count = 0;
    new->next = b->next;
    b->next = new;

    if(in_bucket(myid, b)) {
        new->max_count = b->max_count;
        b->max_count = MAX(b->max_count / 2, 8);
    } else {
        new->max_count = MAX(b->max_count / 2, 8);
    }

    return 1;
}

static int
split_bucket(struct bucket *b)
{
    int rc;
    struct node *nodes = NULL;
    struct node *n = NULL;

    debugf("Splitting bucket %s", id_to_hex(b->first));
    rc = split_bucket_helper(b, &nodes);
    if(rc < 0) {
        errorf("Failed to split bucket %s", id_to_hex(b->first));
        return -1;
    }

    while(n != NULL || nodes != NULL) {
        struct bucket *split = NULL;
        if(n == NULL) {
            n = nodes;
            nodes = nodes->next;
            n->next = NULL;
        }
        rc = insert_node(n, &split);
        if(rc < 0) {
            errorf("Failed to reinsert node %s into bucket %s after splitting bucket %s",
                   sa_to_str((struct sockaddr*)&n->ss), id_to_hex(split->first), id_to_hex(b->first));
            free(n);
            n = NULL;
        } else if(rc > 0) {
            n = NULL;
        } else if(!in_bucket(myid, split)) {
            free(n);
            n = NULL;
        } else {
            struct node *insert = NULL;
            debugf("Splitting bucket %s (recursive)", id_to_hex(split->first));
            rc = split_bucket_helper(split, &insert);
            if(rc < 0) {
                errorf("Failed to split bucket %s", id_to_hex(split->first));
                free(n);
                n = NULL;
            } else {
                nodes = append_nodes(nodes, insert);
            }
        }
    }
    return 1;
}

/* We just learnt about a node, not necessarily a new one.  Confirm is 1 if
   the node sent a message, 2 if it sent us a reply. */
static struct node *
new_node(const unsigned char *id, const struct sockaddr *sa, int salen,
         int confirm)
{
    struct bucket *b;
    struct node *n;
    int mybucket;

 again:

    b = find_bucket(id, sa->sa_family);
    if(b == NULL)
        return NULL;

    if(id_cmp(id, myid) == 0)
        return NULL;

    if(is_martian(sa) || node_blacklisted(sa, salen))
        return NULL;

    mybucket = in_bucket(myid, b);

    if(confirm == 2)
        b->time = now.tv_sec;

    n = b->nodes;
    while(n) {
        if(id_cmp(n->id, id) == 0) {
            if(confirm || n->time < now.tv_sec - 15 * 60) {
                /* Known node.  Update stuff. */
                memcpy((struct sockaddr*)&n->ss, sa, salen);
                if(confirm)
                    n->time = now.tv_sec;
                if(confirm >= 2) {
                    n->reply_time = now.tv_sec;
                    n->pinged = 0;
                    n->pinged_time = 0;
                }
            }
            if(confirm == 2)
                add_search_node(id, sa, salen);
            return n;
        }
        n = n->next;
    }

    /* New node. */

    if(mybucket) {
        if(sa->sa_family == AF_INET)
            mybucket_grow_time = now.tv_sec;
        else
            mybucket6_grow_time = now.tv_sec;
    }

    /* First, try to get rid of a known-bad node. */
    n = b->nodes;
    while(n) {
        if(n->pinged >= 3 && n->pinged_time < now.tv_sec - 15) {
            memcpy(n->id, id, 20);
            memcpy((struct sockaddr*)&n->ss, sa, salen);
            n->time = confirm ? now.tv_sec : 0;
            n->reply_time = confirm >= 2 ? now.tv_sec : 0;
            n->pinged_time = 0;
            n->pinged = 0;
            if(confirm == 2)
                add_search_node(id, sa, salen);
            return n;
        }
        n = n->next;
    }

    if(b->count >= b->max_count) {
        /* Bucket full.  Ping a dubious node, but only if not bootstrapping. */
        int dubious = 0;
        int bss = (sa->sa_family == AF_INET ? bootstrap.state : bootstrap6.state);
        if(bss < DHT_BOOTSTRAP_STATE_ENABLED) {
            n = b->nodes;
            while(n) {
                /* Pick the first dubious node that we haven't pinged in the
                last 15 seconds.  This gives nodes the time to reply, but
                tends to concentrate on the same nodes, so that we get rid
                of bad nodes fast. */
                if(!node_good(n)) {
                    dubious = 1;
                    if(n->pinged_time < now.tv_sec - 15) {
                        unsigned char tid[4];
                        debugf("Sending ping to dubious node %s", sa_to_str((struct sockaddr*)&n->ss));
                        make_tid(tid, "pn", 0);
                        send_ping((struct sockaddr*)&n->ss, n->sslen,
                                tid, 4);
                        n->pinged++;
                        n->pinged_time = now.tv_sec;
                        break;
                    }
                }
                n = n->next;
            }
        }

        if(mybucket && !dubious) {
            int rc;
            rc = split_bucket(b);
            if(rc > 0)
                goto again;
            return NULL;
        }

        /* No space for this node.  Cache it away for later. */
        if(confirm || b->cached.ss_family == 0) {
            memcpy(&b->cached, sa, salen);
            b->cachedlen = salen;
        }

        if(confirm == 2)
            add_search_node(id, sa, salen);
        return NULL;
    }

    /* Create a new node. */
    n = calloc(1, sizeof(struct node));
    if(n == NULL)
        return NULL;
    memcpy(n->id, id, 20);
    memcpy(&n->ss, sa, salen);
    n->sslen = salen;
    n->time = confirm ? now.tv_sec : 0;
    n->reply_time = confirm >= 2 ? now.tv_sec : 0;
    n->next = b->nodes;
    b->nodes = n;
    b->count++;
    if(confirm == 2)
        add_search_node(id, sa, salen);
    return n;
}

/* Called periodically to purge known-bad nodes.  Note that we're very
   conservative here: broken nodes in the table don't do much harm, we'll
   recover as soon as we find better ones. */
static int
expire_buckets(struct bucket *b)
{
    while(b) {
        struct node *n, *p;
        int changed = 0;

        while(b->nodes && b->nodes->pinged >= 4) {
            n = b->nodes;
            b->nodes = n->next;
            b->count--;
            changed = 1;
            free(n);
        }

        p = b->nodes;
        while(p) {
            while(p->next && p->next->pinged >= 4) {
                n = p->next;
                p->next = n->next;
                b->count--;
                changed = 1;
                free(n);
            }
            p = p->next;
        }

        if(changed)
            send_cached_ping(b);

        b = b->next;
    }
    expire_stuff_time = now.tv_sec + 120 + random() % 240;
    return 1;
}

/* While a search is in progress, we don't necessarily keep the nodes being
   walked in the main bucket table.  A search in progress is identified by
   a unique transaction id, a short (and hence small enough to fit in the
   transaction id of the protocol packets). */

static struct search *
find_search(unsigned short tid, int af)
{
    struct search *sr = searches;
    while(sr) {
        if(sr->tid == tid && sr->af == af)
            return sr;
        sr = sr->next;
    }
    return NULL;
}

/* A search contains a list of nodes, sorted by decreasing distance to the
   target.  We just got a new candidate, insert it at the right spot or
   discard it. */

static struct search_node*
insert_search_node(const unsigned char *id,
                   const struct sockaddr *sa, int salen,
                   struct search *sr, int replied,
                   unsigned char *token, int token_len)
{
    struct search_node *n;
    int i, j;

    if(sa->sa_family != sr->af) {
        errorf("Attempted to insert node %s in wrong address family (expected %s, got %s)",
               sa_to_str(sa), af_to_str(sr->af), af_to_str(sa->sa_family));
        return NULL;
    }

    for(i = 0; i < sr->numnodes; i++) {
        if(id_cmp(id, sr->nodes[i].id) == 0) {
            n = &sr->nodes[i];
            goto found;
        }
        if(xorcmp(id, sr->nodes[i].id, sr->id) < 0)
            break;
    }

    if(i == SEARCH_NODES)
        return NULL;

    if(sr->numnodes < SEARCH_NODES)
        sr->numnodes++;

    for(j = sr->numnodes - 1; j > i; j--) {
        sr->nodes[j] = sr->nodes[j - 1];
    }

    n = &sr->nodes[i];

    memset(n, 0, sizeof(struct search_node));
    memcpy(n->id, id, 20);

found:
    memcpy(&n->ss, sa, salen);
    n->sslen = salen;

    if(replied) {
        n->replied = 1;
        n->reply_time = now.tv_sec;
        n->request_time = 0;
        n->pinged = 0;
    }
    if(token) {
        if(token_len >= 40) {
            errorf("Overlong token (expected <%i, got %i)", 40, token_len);
        } else {
            memcpy(n->token, token, token_len);
            n->token_len = token_len;
        }
    }

    return n;
}

static void
flush_search_node(struct search_node *n, struct search *sr)
{
    int i = n - sr->nodes, j;
    for(j = i; j < sr->numnodes - 1; j++)
        sr->nodes[j] = sr->nodes[j + 1];
    sr->numnodes--;
}

static void
expire_searches(dht_main_callback_t *callback, void *closure)
{
    struct search *sr = searches, *previous = NULL;

    while(sr) {
        struct search *next = sr->next;
        if(sr->step_time < now.tv_sec - DHT_SEARCH_EXPIRE_TIME) {
            if(previous)
                previous->next = next;
            else
                searches = next;
            numsearches--;
            if(!sr->done) {
                infof("Search for id %s (%s) expired", id_to_hex(sr->id), af_to_ivs(sr->af));
                if(callback)
                    (*callback)(closure,
                                sr->af == AF_INET ?
                                DHT_EVENT_SEARCH_DONE : DHT_EVENT_SEARCH_DONE6,
                                sr->id, NULL, 0);
            }
            free(sr);
        } else {
            previous = sr;
        }
        sr = next;
    }
}

/* This must always return 0 or 1, never -1, not even on failure (see below). */
static int
search_send_get_peers(struct search *sr, struct search_node *n)
{
    struct node *node;
    unsigned char tid[4];

    if(n == NULL) {
        int i;
        for(i = 0; i < sr->numnodes; i++) {
            if(sr->nodes[i].pinged < 3 && !sr->nodes[i].replied &&
               sr->nodes[i].request_time < now.tv_sec - DHT_SEARCH_RETRANSMIT)
                n = &sr->nodes[i];
        }
    }

    if(!n || n->pinged >= 3 || n->replied ||
       n->request_time >= now.tv_sec - DHT_SEARCH_RETRANSMIT)
        return 0;

    debugf("Sending get_peers to node %s", sa_to_str((struct sockaddr*)&n->ss));
    make_tid(tid, "gp", sr->tid);
    send_get_peers((struct sockaddr*)&n->ss, n->sslen, tid, 4, sr->id, -1,
                   n->reply_time >= now.tv_sec - DHT_SEARCH_RETRANSMIT);
    n->pinged++;
    n->request_time = now.tv_sec;
    /* If the node happens to be in our main routing table, mark it
       as pinged. */
    node = find_node(n->id, n->ss.ss_family);
    if(node) pinged(node, NULL);
    return 1;
}

/* Insert a new node into any incomplete search. */
static void
add_search_node(const unsigned char *id, const struct sockaddr *sa, int salen)
{
    struct search *sr;
    for(sr = searches; sr; sr = sr->next) {
        if(sr->af == sa->sa_family && sr->numnodes < SEARCH_NODES) {
            struct search_node *n =
                insert_search_node(id, sa, salen, sr, 0, NULL, 0);
            if(n)
                search_send_get_peers(sr, n);
        }
    }
}

/* When a search is in progress, we periodically call search_step to send
   further requests. */
static void
search_step(struct search *sr, dht_main_callback_t *callback, void *closure)
{
    int i, j;
    int all_done = 1;

    /* Check if the first 8 live nodes have replied. */
    j = 0;
    for(i = 0; i < sr->numnodes && j < 8; i++) {
        struct search_node *n = &sr->nodes[i];
        if(n->pinged >= 3)
            continue;
        if(!n->replied) {
            all_done = 0;
            break;
        }
        j++;
    }

    if(all_done) {
        if(sr->port == 0) {
            goto done;
        } else {
            int all_acked = 1;
            j = 0;
            for(i = 0; i < sr->numnodes && j < 8; i++) {
                struct search_node *n = &sr->nodes[i];
                struct node *node;
                unsigned char tid[4];
                if(n->pinged >= 3)
                    continue;
                /* A proposed extension to the protocol consists in
                   omitting the token when storage tables are full.  While
                   I don't think this makes a lot of sense -- just sending
                   a positive reply is just as good --, let's deal with it. */
                if(n->token_len == 0)
                    n->acked = 1;
                if(!n->acked) {
                    all_acked = 0;
                    debugf("Sending announce_peer to node %s", sa_to_str((struct sockaddr*)&n->ss));
                    make_tid(tid, "ap", sr->tid);
                    send_announce_peer((struct sockaddr*)&n->ss,
                                       sizeof(struct sockaddr_storage),
                                       tid, 4, sr->id, sr->port,
                                       n->token, n->token_len,
                                       n->reply_time >= now.tv_sec - 15);
                    n->pinged++;
                    n->request_time = now.tv_sec;
                    node = find_node(n->id, n->ss.ss_family);
                    if(node) pinged(node, NULL);
                }
                j++;
            }
            if(all_acked)
                goto done;
        }
        sr->step_time = now.tv_sec;
        return;
    }

    if(sr->step_time + DHT_SEARCH_RETRANSMIT >= now.tv_sec)
        return;

    j = 0;
    for(i = 0; i < sr->numnodes; i++) {
        j += search_send_get_peers(sr, &sr->nodes[i]);
        if(j >= DHT_INFLIGHT_QUERIES)
            break;
    }
    sr->step_time = now.tv_sec;
    return;

 done:
    infof("Search for id %s (%s) complete", id_to_hex(sr->id), af_to_ivs(sr->af));
    sr->done = 1;
    if(callback)
        (*callback)(closure,
                    sr->af == AF_INET ?
                    DHT_EVENT_SEARCH_DONE : DHT_EVENT_SEARCH_DONE6,
                    sr->id, NULL, 0);
    sr->step_time = now.tv_sec;
}

static struct search *
new_search(void)
{
    struct search *sr, *oldest = NULL;

    /* Find the oldest done search */
    sr = searches;
    while(sr) {
        if(sr->done &&
           (oldest == NULL || oldest->step_time > sr->step_time))
            oldest = sr;
        sr = sr->next;
    }

    /* The oldest slot is expired. */
    if(oldest && oldest->step_time < now.tv_sec - DHT_SEARCH_EXPIRE_TIME)
        return oldest;

    /* Allocate a new slot. */
    if(numsearches < DHT_MAX_SEARCHES) {
        sr = calloc(1, sizeof(struct search));
        if(sr != NULL) {
            sr->next = searches;
            searches = sr;
            numsearches++;
            return sr;
        }
    }

    /* Oh, well, never mind.  Reuse the oldest slot. */
    return oldest;
}

/* Insert the contents of a bucket into a search structure. */
static void
insert_search_bucket(struct bucket *b, struct search *sr)
{
    struct node *n;
    n = b->nodes;
    while(n) {
        insert_search_node(n->id, (struct sockaddr*)&n->ss, n->sslen,
                           sr, 0, NULL, 0);
        n = n->next;
    }
}

/* Start a search.  If port is non-zero, perform an announce when the
   search is complete. */
int
dht_search(const unsigned char *id, int port, int af,
           dht_main_callback_t *callback, void *closure)
{
    struct search *sr;
    struct storage *st;
    struct bucket *b = find_bucket(id, af);

    if(b == NULL) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    infof("Starting search for id %s (%s)", id_to_hex(id), af_to_ivs(af));

    /* Try to answer this search locally.  In a fully grown DHT this
       is very unlikely, but people are running modified versions of
       this code in private DHTs with very few nodes.  What's wrong
       with flooding? */
    if(callback) {
        st = find_storage(id);
        if(st) {
            unsigned short swapped;
            unsigned char buf[18];
            int i;

            debugf("Found %d peers in local storage for search for id %s (%s)",
                   st->numpeers, id_to_hex(id), af_to_ivs(af));

            for(i = 0; i < st->numpeers; i++) {
                swapped = htons(st->peers[i].port);
                if(st->peers[i].len == 4) {
                    memcpy(buf, st->peers[i].ip, 4);
                    memcpy(buf + 4, &swapped, 2);
                    (*callback)(closure, DHT_EVENT_VALUES, id,
                                (void*)buf, 6);
                } else if(st->peers[i].len == 16) {
                    memcpy(buf, st->peers[i].ip, 16);
                    memcpy(buf + 16, &swapped, 2);
                    (*callback)(closure, DHT_EVENT_VALUES6, id,
                                (void*)buf, 18);
                }
            }
        }
    }

    sr = searches;
    while(sr) {
        if(sr->af == af && id_cmp(sr->id, id) == 0)
            break;
        sr = sr->next;
    }

    int sr_duplicate = sr && !sr->done;

    if(sr) {
        /* We're reusing data from an old search.  Reusing the same tid
           means that we can merge replies for both searches. */
        debugf("Reusing existing search for id %s (%s)", id_to_hex(id), af_to_ivs(af));
        int i;
        sr->done = 0;
    again:
        for(i = 0; i < sr->numnodes; i++) {
            struct search_node *n;
            n = &sr->nodes[i];
            /* Discard any doubtful nodes. */
            if(n->pinged >= 3 || n->reply_time < now.tv_sec - 7200) {
                flush_search_node(n, sr);
                goto again;
            }
            n->pinged = 0;
            n->token_len = 0;
            n->replied = 0;
            n->acked = 0;
        }
    } else {
        debugf("Creating new search for id %s (%s)", id_to_hex(id), af_to_ivs(af));
        sr = new_search();
        if(sr == NULL) {
            errno = ENOSPC;
            return -1;
        }
        sr->af = af;
        sr->tid = search_id++;
        sr->step_time = 0;
        memcpy(sr->id, id, 20);
        sr->done = 0;
        sr->numnodes = 0;
    }

    sr->port = port;

    insert_search_bucket(b, sr);

    if(sr->numnodes < SEARCH_NODES) {
        struct bucket *p = previous_bucket(b);
        if(b->next)
            insert_search_bucket(b->next, sr);
        if(p)
            insert_search_bucket(p, sr);
    }
    if(sr->numnodes < SEARCH_NODES)
        insert_search_bucket(find_bucket(myid, af), sr);

    search_step(sr, callback, closure);
    search_time = now.tv_sec;
    if(sr_duplicate) {
        return 0;
    } else {
        return 1;
    }
}

/* A struct storage stores all the stored peer addresses for a given info
   hash. */

static struct storage *
find_storage(const unsigned char *id)
{
    struct storage *st = storage;

    while(st) {
        if(id_cmp(id, st->id) == 0)
            break;
        st = st->next;
    }
    return st;
}

static int
storage_store(const unsigned char *id,
              const struct sockaddr *sa, unsigned short port)
{
    int i, len;
    struct storage *st;
    unsigned char *ip;

    if(sa->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in*)sa;
        ip = (unsigned char*)&sin->sin_addr;
        len = 4;
    } else if(sa->sa_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)sa;
        ip = (unsigned char*)&sin6->sin6_addr;
        len = 16;
    } else {
        return -1;
    }

    st = find_storage(id);

    if(st == NULL) {
        if(numstorage >= DHT_MAX_HASHES)
            return -1;
        st = calloc(1, sizeof(struct storage));
        if(st == NULL) return -1;
        memcpy(st->id, id, 20);
        st->next = storage;
        storage = st;
        numstorage++;
    }

    for(i = 0; i < st->numpeers; i++) {
        if(st->peers[i].port == port && st->peers[i].len == len &&
           memcmp(st->peers[i].ip, ip, len) == 0)
            break;
    }

    if(i < st->numpeers) {
        /* Already there, only need to refresh */
        st->peers[i].time = now.tv_sec;
        return 0;
    } else {
        struct peer *p;
        if(i >= st->maxpeers) {
            /* Need to expand the array. */
            struct peer *new_peers;
            int n;
            if(st->maxpeers >= DHT_MAX_PEERS)
                return 0;
            n = st->maxpeers == 0 ? 2 : 2 * st->maxpeers;
            n = MIN(n, DHT_MAX_PEERS);
            new_peers = realloc(st->peers, n * sizeof(struct peer));
            if(new_peers == NULL)
                return -1;
            st->peers = new_peers;
            st->maxpeers = n;
        }
        p = &st->peers[st->numpeers++];
        p->time = now.tv_sec;
        p->len = len;
        memcpy(p->ip, ip, len);
        p->port = port;
        return 1;
    }
}

static int
expire_storage(void)
{
    struct storage *st = storage, *previous = NULL;
    while(st) {
        int i = 0;
        while(i < st->numpeers) {
            if(st->peers[i].time < now.tv_sec - 32 * 60) {
                if(i != st->numpeers - 1)
                    st->peers[i] = st->peers[st->numpeers - 1];
                st->numpeers--;
            } else {
                i++;
            }
        }

        if(st->numpeers == 0) {
            free(st->peers);
            if(previous)
                previous->next = st->next;
            else
                storage = st->next;
            free(st);
            if(previous)
                st = previous->next;
            else
                st = storage;
            numstorage--;
            if(numstorage < 0) {
                errorf("numstorage became negative while expiring storage");
                numstorage = 0;
            }
        } else {
            previous = st;
            st = st->next;
        }
    }
    return 1;
}

static int
rotate_secrets(void)
{
    int rc;

    rotate_secrets_time = now.tv_sec + 900 + random() % 1800;

    memcpy(oldsecret, secret, sizeof(secret));
    rc = dht_random_bytes(secret, sizeof(secret));

    if(rc < 0)
        return -1;

    return 1;
}

#ifndef TOKEN_SIZE
#define TOKEN_SIZE 8
#endif

static void
make_token(const struct sockaddr *sa, int old, unsigned char *token_return)
{
    void *ip;
    int iplen;
    unsigned short port;

    if(sa->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in*)sa;
        ip = &sin->sin_addr;
        iplen = 4;
        port = htons(sin->sin_port);
    } else if(sa->sa_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)sa;
        ip = &sin6->sin6_addr;
        iplen = 16;
        port = htons(sin6->sin6_port);
    } else {
        abort();
    }

    dht_hash(token_return, TOKEN_SIZE,
             old ? oldsecret : secret, sizeof(secret),
             ip, iplen, (unsigned char*)&port, 2);
}
static int
token_match(const unsigned char *token, int token_len,
            const struct sockaddr *sa)
{
    unsigned char t[TOKEN_SIZE];
    if(token_len != TOKEN_SIZE)
        return 0;
    make_token(sa, 0, t);
    if(memcmp(t, token, TOKEN_SIZE) == 0)
        return 1;
    make_token(sa, 1, t);
    if(memcmp(t, token, TOKEN_SIZE) == 0)
        return 1;
    return 0;
}

/* Get statistics. */
int
dht_stats(int af, int *numbuckets, int *numgood, int *numdubious, int *numtotal)
{
    /* Sanity checks. */
    if(af != AF_INET && af != AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    /* Determine stats. */
    int numbuckets_tmp = 0, numgood_tmp = 0, numdubious_tmp = 0, numtotal_tmp = 0;
    struct bucket *b = (af == AF_INET ? buckets : buckets6);
    while(b) {
        struct node *n = b->nodes;
        while(n) {
            if(node_good(n))
                numgood_tmp++;
            else
                numdubious_tmp++;
            numtotal_tmp++;
            n = n->next;
        }
        numbuckets_tmp++;
        b = b->next;
    }

    /* Return results. */
    if(numbuckets)
        (*numbuckets) = numbuckets_tmp;
    if(numgood)
        (*numgood) = numgood_tmp;
    if(numdubious)
        (*numdubious) = numdubious_tmp;
    if(numtotal)
        (*numtotal) = numtotal_tmp;
    return 1;
}

int
dht_nodes(int af, int *good_return, int *dubious_return, int *cached_return,
          int *incoming_return)
{
    int good = 0, dubious = 0, cached = 0, incoming = 0;
    struct bucket *b = af == AF_INET ? buckets : buckets6;

    while(b) {
        struct node *n = b->nodes;
        while(n) {
            if(node_good(n)) {
                good++;
                if(n->time > n->reply_time)
                    incoming++;
            } else {
                dubious++;
            }
            n = n->next;
        }
        if(b->cached.ss_family > 0)
            cached++;
        b = b->next;
    }
    if(good_return)
        *good_return = good;
    if(dubious_return)
        *dubious_return = dubious;
    if(cached_return)
        *cached_return = cached;
    if(incoming_return)
        *incoming_return = incoming;
    return good + dubious;
}

static void
dump_buckets(int af)
{
    struct bucket *b = (af == AF_INET ? buckets : buckets6);
    if(!b) {
        infof("\n");
        infof("No %s buckets to dump", af_to_ivs(af));
        return;
    }

    const char *fmt_header = (af == AF_INET ? "   %-4s %-40s %-4s %-21s %-10s %-10s %-5s %-4s" :
                                              "   %-4s %-40s %-4s %-47s %-10s %-10s %-5s %-4s");
    const char *fmt_entry  = (af == AF_INET ? "   %-4i %-40s %-4d %-21s %-10ld %-10ld %-5d %-4s" :
                                              "   %-4i %-40s %-4d %-47s %-10ld %-10ld %-5d %-4s");
    int bi = 0;
    while(b) {
        infof("\n");
        infof("Bucket #%d (%s), id %s, nodes %d/%d, age %d%s%s:",
              bi, af_to_ivs(b->af), id_to_hex(b->first),
              b->count, b->max_count,
              (b->time ? (int)(now.tv_sec - b->time) : 0),
              in_bucket(myid, b) ? " (mine)" : "",
              b->cached.ss_family ? " (cached)" : "");
        int ni = 0;
        struct node *n = b->nodes;
        if(n) {
            infof(fmt_header, "Node", "ID", "Dist", "IP-Address", "Age", "Age-Reply", "Pings", "Good");
            while(n) {
                infof(fmt_entry, ni, id_to_hex(n->id), distance(myid, n->id),
                      sa_to_str((struct sockaddr*)&n->ss),
                      (n->time ? (long)(now.tv_sec - n->time) : 0),
                      (n->reply_time ? (long)(now.tv_sec - n->reply_time) : 0),
                      n->pinged, (node_good(n) ? "yes" : "no"));
                n = n->next;
                ni++;
            }
        } else {
            infof("   <empty>");
        }
        b = b->next;
        bi++;
    }
}

static void
dump_bootstrap_nodes(int af)
{
    struct bootstrap_node *bn = (af == AF_INET ? bootstrap.nodes : bootstrap6.nodes);
    if(!bn) {
        infof("\n");
        infof("No %s bootstrap nodes to dump", af_to_ivs(af));
        return;
    }

    infof("\n");
    infof("Bootstrap nodes (%s):", af_to_ivs(af));
    const char *fmt_header = (af == AF_INET ? "   %-4s %-21s" :
                                              "   %-4s %-47s");
    const char *fmt_entry  = (af == AF_INET ? "   %-4i %-21s" :
                                              "   %-4i %-47s");
    infof(fmt_header, "Node", "IP-Address");
    int bni = 0;
    while(bn) {
        infof(fmt_entry, bni, sa_to_str((struct sockaddr*)&bn->ss));
        bn = bn->next;
        bni++;
    }
}

void
dht_dump_tables()
{
    /* Print myid/my_v. */
    infof("\n");
    infof("My id: %s", id_to_hex(myid));
    infof("My v:  %s", v_to_str(&my_v[5]));

    /* Dump buckets/nodes. */
    dump_buckets(AF_INET);
    dump_buckets(AF_INET6);

    /* Dump searches. */
    struct search *sr = searches;
    if(!sr) {
        infof("\n");
        infof("No searches to dump");
    }
    int sri = 0;
    while(sr) {
        infof("\n");
        infof("Search #%d (%s), id %s, age %d%s:", sri, af_to_ivs(sr->af), id_to_hex(sr->id),
              (int)(now.tv_sec - sr->step_time), sr->done ? " (done)" : "");
        if(sr->numnodes > 0) {
            const char *fmt_header = (sr->af == AF_INET ? "   %-4s %-40s %-4s %-21s %-10s %-10s %-5s %-5s %-7s" :
                                                          "   %-4s %-40s %-4s %-47s %-10s %-10s %-5s %-5s %-7s");
            const char *fmt_entry  = (sr->af == AF_INET ? "   %-4i %-40s %-4d %-21s %-10ld %-10ld %-5d %-5s %-7s" :
                                                          "   %-4i %-40s %-4d %-47s %-10ld %-10ld %-5d %-5s %-7s");
            infof(fmt_header, "Node", "ID", "Dist", "IP-Address", "Age-Req", "Age-Reply", "Pings", "Known", "Replied");
            for(int ni = 0; ni < sr->numnodes; ni++) {
                struct search_node *n = &sr->nodes[ni];
                infof(fmt_entry,
                      ni, id_to_hex(n->id), distance(sr->id, n->id),
                      sa_to_str((struct sockaddr*)&n->ss),
                      (n->request_time ? (long)(now.tv_sec - n->request_time) : 0),
                      (n->reply_time ? (long)(now.tv_sec - n->reply_time) : 0), n->pinged,
                      (find_node(n->id, sr->af) ? "yes" : "no"),
                      (n->replied ? "yes" : "no"));
            }
        } else {
            infof("   <empty>");
        }
        sr = sr->next;
        sri++;
    }

    /* Dump storages. */
    struct storage *st = storage;
    if(!st) {
        infof("\n");
        infof("No storage contents to dump");
    }
    int sti = 0;
    while(st) {
        infof("\n");
        infof("Storage #%d, id %s, peers %d/%d:", sti, id_to_hex(st->id), st->numpeers, st->maxpeers);
        if(st->numpeers > 0) {
            infof("   %-4s %-47s %-10s", "Peer", "IP-Address", "Age");
            for(int pi = 0; pi < st->numpeers; pi++) {
                char sastr[INET6_ADDRSTRLEN + 8];
                if(st->peers[pi].len == 4) {
                    char ipstr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, st->peers[pi].ip, ipstr, sizeof(ipstr));
                    snprintf(sastr, sizeof(sastr), "%s:%u", ipstr, st->peers[pi].port);
                } else if(st->peers[pi].len == 16) {
                    char ipstr6[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET6, st->peers[pi].ip, ipstr6, sizeof(ipstr6));
                    snprintf(sastr, sizeof(sastr), "[%s]:%u", ipstr6, st->peers[pi].port);
                } else {
                    strcpy(sastr, "<invalid-ip-len>");
                }
                infof("   %-4i %-47s %-10ld", pi, sastr, (long)(now.tv_sec - st->peers[pi].time));
            }
        } else {
            infof("   <empty>");
        }
        st = st->next;
        sti++;
    }

    /* Dump bootstrap nodes. */
    dump_bootstrap_nodes(AF_INET);
    dump_bootstrap_nodes(AF_INET6);

    infof("\n");
}

int
dht_init(int s, int s6, const unsigned char *id, const unsigned char *v)
{
    int rc;

    if(dht_socket >= 0 || dht_socket6 >= 0 || buckets || buckets6) {
        warnf("Unable to initialize DHT, already initialized");
        errno = EBUSY;
        return -1;
    }

    infof("Initializing DHT");

    searches = NULL;
    numsearches = 0;

    storage = NULL;
    numstorage = 0;

    if(s >= 0) {
        buckets = calloc(1, sizeof(struct bucket));
        if(buckets == NULL)
            return -1;
        buckets->max_count = 128;
        buckets->af = AF_INET;
    }

    if(s6 >= 0) {
        buckets6 = calloc(1, sizeof(struct bucket));
        if(buckets6 == NULL)
            return -1;
        buckets6->max_count = 128;
        buckets6->af = AF_INET6;
    }

    memcpy(myid, id, 20);
    if(v) {
        memcpy(my_v, "1:v4:", 5);
        memcpy(my_v + 5, v, 4);
        have_v = 1;
    } else {
        have_v = 0;
    }

    bootstrap.state = DHT_BOOTSTRAP_STATE_DISABLED;
    bootstrap.nodes = NULL;
    bootstrap.numnodes = 0;
    bootstrap.start_time = 0;
    bootstrap.end_time = 0;
    bootstrap.next_time = 0;
    bootstrap6.state = DHT_BOOTSTRAP_STATE_DISABLED;
    bootstrap6.nodes = NULL;
    bootstrap6.numnodes = 0;
    bootstrap6.start_time = 0;
    bootstrap6.end_time = 0;
    bootstrap6.next_time = 0;
    bootstrap_time = 0;

    dht_gettimeofday(&now, NULL);

    mybucket_grow_time = now.tv_sec;
    mybucket6_grow_time = now.tv_sec;
    confirm_nodes_time = now.tv_sec + random() % 3;

    search_id = random() & 0xFFFF;
    search_time = 0;

    next_blacklisted = 0;

    token_bucket_time = now.tv_sec;
    token_bucket_tokens = MAX_TOKEN_BUCKET_TOKENS;

    memset(secret, 0, sizeof(secret));
    rc = rotate_secrets();
    if(rc < 0)
        goto fail;

    dht_socket = s;
    dht_socket6 = s6;

    expire_buckets(buckets);
    expire_buckets(buckets6);

    return 1;

 fail:
    free(buckets);
    buckets = NULL;
    free(buckets6);
    buckets6 = NULL;
    return -1;
}

int
dht_uninit()
{
    if(dht_socket < 0 && dht_socket6 < 0) {
        warnf("Unable to shutdown DHT, already shut down");
        errno = EINVAL;
        return -1;
    }

    infof("Shutting down DHT");

    dht_socket = -1;
    dht_socket6 = -1;

    while(buckets) {
        struct bucket *b = buckets;
        buckets = b->next;
        while(b->nodes) {
            struct node *n = b->nodes;
            b->nodes = n->next;
            free(n);
        }
        free(b);
    }

    while(buckets6) {
        struct bucket *b = buckets6;
        buckets6 = b->next;
        while(b->nodes) {
            struct node *n = b->nodes;
            b->nodes = n->next;
            free(n);
        }
        free(b);
    }

    while(storage) {
        struct storage *st = storage;
        storage = storage->next;
        free(st->peers);
        free(st);
    }

    while(searches) {
        struct search *sr = searches;
        searches = searches->next;
        free(sr);
    }

    while(bootstrap.nodes) {
        struct bootstrap_node *bn = bootstrap.nodes;
        bootstrap.nodes = bootstrap.nodes->next;
        free(bn);
    }
    while(bootstrap6.nodes) {
        struct bootstrap_node *bn = bootstrap6.nodes;
        bootstrap6.nodes = bootstrap6.nodes->next;
        free(bn);
    }

    return 1;
}

/* Rate control for requests we receive. */

static int
token_bucket(void)
{
    if(token_bucket_tokens == 0) {
        token_bucket_tokens = MIN(MAX_TOKEN_BUCKET_TOKENS,
                                  100 * (now.tv_sec - token_bucket_time));
        token_bucket_time = now.tv_sec;
    }

    if(token_bucket_tokens == 0)
        return 0;

    token_bucket_tokens--;
    return 1;
}

static int
neighbourhood_maintenance(int af)
{
    /* When bootstrapping, don't do any neighborhood maintenance. */
    int bss = (af == AF_INET ? bootstrap.state : bootstrap6.state);
    if(bss >= DHT_BOOTSTRAP_STATE_ENABLED)
        return 0;

    unsigned char id[20];
    struct bucket *b = find_bucket(myid, af);
    struct bucket *q;
    struct node *n;

    if(b == NULL)
        return 0;

    memcpy(id, myid, 20);
    id[19] = random() & 0xFF;
    q = b;
    if(q->next && (q->count == 0 || (random() & 7) == 0))
        q = b->next;
    if(q->count == 0 || (random() & 7) == 0) {
        struct bucket *r;
        r = previous_bucket(b);
        if(r && r->count > 0)
            q = r;
    }

    if(q) {
        /* Since our node-id is the same in both DHTs, it's probably
           profitable to query both families, but only if not bootstrapping. */
        int want = dht_socket >= 0 && dht_socket6 >= 0 && bootstrap_time <= 0 ? (WANT4 | WANT6) : -1;
        n = random_node(q);
        if(n) {
            unsigned char tid[4];
            debugf("Sending find_node for %s neighborhood maintenance to node %s",
                   af_to_ivs(af), sa_to_str((struct sockaddr*)&n->ss));
            make_tid(tid, "fn", 0);
            send_find_node((struct sockaddr*)&n->ss, n->sslen,
                           tid, 4, id, want,
                           n->reply_time >= now.tv_sec - 15);
            pinged(n, q);
        }
        return 1;
    }
    return 0;
}

static int
bucket_maintenance(int af)
{
    /* When bootstrapping, don't do any bucket maintenance. */
    int bss = (af == AF_INET ? bootstrap.state : bootstrap6.state);
    if(bss >= DHT_BOOTSTRAP_STATE_ENABLED)
        return 0;

    struct bucket *b = (af == AF_INET ? buckets : buckets6);
    while(b) {
        /* 10 minutes for an 8-node bucket */
        int to = MAX(600 / (b->max_count / 8), 30);
        struct bucket *q;
        if(b->time < now.tv_sec - to) {
            /* This bucket hasn't seen any positive confirmation for a long
               time.  Pick a random id in this bucket's range, and send
               a request to a random node. */
            unsigned char id[20];
            struct node *n;
            int rc;

            rc = bucket_random(b, id);
            if(rc < 0)
                memcpy(id, b->first, 20);

            q = b;
            /* If the bucket is empty, we try to fill it from a neighbour.
               We also sometimes do it gratuitiously to recover from
               buckets full of broken nodes. */
            if(q->next && (q->count == 0 || (random() & 7) == 0))
                q = b->next;
            if(q->count == 0 || (random() & 7) == 0) {
                struct bucket *r;
                r = previous_bucket(b);
                if(r && r->count > 0)
                    q = r;
            }

            if(q) {
                n = random_node(q);
                if(n) {
                    unsigned char tid[4];
                    int want = -1;

                    /* Only consider querying both address families if currently
                       not bootstrapping. */
                    if(dht_socket >= 0 && dht_socket6 >= 0 && bootstrap_time <= 0) {
                        struct bucket *otherbucket;
                        otherbucket =
                            find_bucket(id, af == AF_INET ? AF_INET6 : AF_INET);
                        if(otherbucket &&
                           otherbucket->count < otherbucket->max_count)
                            /* The corresponding bucket in the other family
                               is not full -- querying both is useful. */
                            want = WANT4 | WANT6;
                        else if(random() % 37 == 0)
                            /* Most of the time, this just adds overhead.
                               However, it might help stitch back one of
                               the DHTs after a network collapse, so query
                               both, but only very occasionally. */
                            want = WANT4 | WANT6;
                    }

                    debugf("Sending find_node for %s bucket maintenance to node %s",
                           af_to_ivs(af), sa_to_str((struct sockaddr*)&n->ss));
                    make_tid(tid, "fn", 0);
                    send_find_node((struct sockaddr*)&n->ss, n->sslen,
                                   tid, 4, id, want,
                                   n->reply_time >= now.tv_sec - 15);
                    pinged(n, q);
                    /* In order to avoid sending queries back-to-back,
                       give up for now and reschedule us soon. */
                    return 1;
                }
            }
        }
        b = b->next;
    }
    return 0;
}

int
dht_periodic(const void *buf, size_t buflen,
             const struct sockaddr *from, int fromlen,
             time_t *tosleep,
             dht_main_callback_t *callback, void *closure)
{
    dht_gettimeofday(&now, NULL);

    if(buflen > 0) {
        int message;
        struct parsed_message m;
        unsigned short ttid;

        if(is_martian(from))
            goto dontread;

        if(node_blacklisted(from, fromlen)) {
            debugf("Received packet from blacklisted node %s", sa_to_str(from));
            goto dontread;
        }

        if(((char*)buf)[buflen] != '\0') {
            warnf("Received unterminated message from node %s", sa_to_str(from));
            errno = EINVAL;
            return -1;
        }

#if (DHT_LOG_INCOMING_MESSAGES == 1)
        debugf("<<< %s", ba_to_str(buf, buflen));
#endif

        memset(&m, 0, sizeof(m));
        message = parse_message(buf, buflen, &m);

        if(message < 0 || message == ERROR || id_cmp(m.id, zeroes) == 0) {
            warnf("Received unparseable message from node %s, message: %s",
                  sa_to_str(from), ba_to_str(buf, buflen));
            goto dontread;
        }

        if(id_cmp(m.id, myid) == 0) {
            warnf("Received message from self (%s)", sa_to_str(from));
            goto dontread;
        }

        if(message > REPLY) {
            /* Rate limit requests. */
            if(!token_bucket()) {
                warnf("Dropping request from node %s due to rate limiting", sa_to_str(from));
                goto dontread;
            }
        }

        switch(message) {
        case REPLY:
            if(m.tid_len != 4) {
                warnf("Blacklisting node %s for truncating transaction id, message: %s",
                      sa_to_str(from), ba_to_str(buf, buflen));
                /* This is really annoying, as it means that we will
                   time-out all our searches that go through this node.
                   Kill it. */
                blacklist_node(m.id, from, fromlen);
                goto dontread;
            }
            if(tid_match(m.tid, "pn", NULL)) {
                debugf("Received pong from node %s", sa_to_str(from));
                new_node(m.id, from, fromlen, 2);
            } else if(tid_match(m.tid, "fn", NULL) ||
                      tid_match(m.tid, "gp", NULL)) {
                int gp = 0;
                struct search *sr = NULL;
                if(tid_match(m.tid, "gp", &ttid)) {
                    gp = 1;
                    sr = find_search(ttid, from->sa_family);
                }
                debugf("Received %d nodes (IPv4: %d, IPv6: %d)%s from node %s",
                       m.nodes_len/26 + m.nodes6_len/38, m.nodes_len/26, m.nodes6_len/38,
                       gp ? " for get_peers" : "", sa_to_str(from));
                if(m.nodes_len % 26 != 0 || m.nodes6_len % 38 != 0) {
                    warnf("Blacklisting node %s for sending node list of invalid length",
                          sa_to_str(from));
                    blacklist_node(m.id, from, fromlen);
                } else if(gp && sr == NULL) {
                    warnf("Received peers from node %s, but couldn't find any matching search",
                          sa_to_str(from));
                    new_node(m.id, from, fromlen, 1);
                } else {
                    int i;
                    new_node(m.id, from, fromlen, 2);
                    for(i = 0; i < m.nodes_len / 26; i++) {
                        unsigned char *ni = m.nodes + i * 26;
                        struct sockaddr_in sin;
                        if(id_cmp(ni, myid) == 0)
                            continue;
                        memset(&sin, 0, sizeof(sin));
                        sin.sin_family = AF_INET;
                        memcpy(&sin.sin_addr, ni + 20, 4);
                        memcpy(&sin.sin_port, ni + 24, 2);
                        new_node(ni, (struct sockaddr*)&sin, sizeof(sin), 0);
                        if(sr && sr->af == AF_INET) {
                            insert_search_node(ni,
                                               (struct sockaddr*)&sin,
                                               sizeof(sin),
                                               sr, 0, NULL, 0);
                        }
                    }
                    for(i = 0; i < m.nodes6_len / 38; i++) {
                        unsigned char *ni = m.nodes6 + i * 38;
                        struct sockaddr_in6 sin6;
                        if(id_cmp(ni, myid) == 0)
                            continue;
                        memset(&sin6, 0, sizeof(sin6));
                        sin6.sin6_family = AF_INET6;
                        memcpy(&sin6.sin6_addr, ni + 20, 16);
                        memcpy(&sin6.sin6_port, ni + 36, 2);
                        new_node(ni, (struct sockaddr*)&sin6, sizeof(sin6), 0);
                        if(sr && sr->af == AF_INET6) {
                            insert_search_node(ni,
                                               (struct sockaddr*)&sin6,
                                               sizeof(sin6),
                                               sr, 0, NULL, 0);
                        }
                    }
                    if(sr)
                        /* Since we received a reply, the number of
                           requests in flight has decreased.  Let's push
                           another request. */
                        search_send_get_peers(sr, NULL);
                }
                if(sr) {
                    insert_search_node(m.id, from, fromlen, sr,
                                       1, m.token, m.token_len);
                    if(m.values_len > 0 || m.values6_len > 0) {
                        infof("Received %d peers (IPv4: %d, IPv6: %d) from node %s for search for id %s",
                               m.values_len / 6 + m.values6_len / 18, m.values_len / 6, m.values6_len / 18,
                               sa_to_str(from), id_to_hex(sr->id));
                        if(callback) {
                            if(m.values_len > 0)
                                (*callback)(closure, DHT_EVENT_VALUES, sr->id,
                                            (void*)m.values, m.values_len);
                            if(m.values6_len > 0)
                                (*callback)(closure, DHT_EVENT_VALUES6, sr->id,
                                            (void*)m.values6, m.values6_len);
                        }
                    }
                }
            } else if(tid_match(m.tid, "ap", &ttid)) {
                struct search *sr;
                debugf("Received reply to announce_peer from node %s", sa_to_str(from));
                sr = find_search(ttid, from->sa_family);
                if(!sr) {
                    warnf("Reply to announce_peer received from node %s does not match any known search",
                          sa_to_str(from));
                    new_node(m.id, from, fromlen, 1);
                } else {
                    int i;
                    new_node(m.id, from, fromlen, 2);
                    for(i = 0; i < sr->numnodes; i++)
                        if(id_cmp(sr->nodes[i].id, m.id) == 0) {
                            sr->nodes[i].request_time = 0;
                            sr->nodes[i].reply_time = now.tv_sec;
                            sr->nodes[i].acked = 1;
                            sr->nodes[i].pinged = 0;
                            break;
                        }
                    /* See comment for gp above. */
                    search_send_get_peers(sr, NULL);
                }
            } else {
                debugf("Received unexpected reply from node %s, message: %s",
                       sa_to_str(from), ba_to_str(buf, buflen));
                debugf("\n");
            }
            break;
        case PING:
            debugf("Received ping from node %s", sa_to_str(from));
            new_node(m.id, from, fromlen, 1);
            debugf("Sending pong to node %s", sa_to_str(from));
            send_pong(from, fromlen, m.tid, m.tid_len);
            break;
        case FIND_NODE:
            debugf("Received find_node from node %s", sa_to_str(from));
            new_node(m.id, from, fromlen, 1);
            debugf("Sending closest nodes to node %s", sa_to_str(from));
            send_closest_nodes(from, fromlen,
                               m.tid, m.tid_len, m.target, m.want,
                               0, NULL, NULL, 0);
            break;
        case GET_PEERS:
            debugf("Received get_peers from node %s", sa_to_str(from));
            new_node(m.id, from, fromlen, 1);
            if(id_cmp(m.info_hash, zeroes) == 0) {
                warnf("Get_peers received from node %s does not contain an info_hash",
                      sa_to_str(from));
                send_error(from, fromlen, m.tid, m.tid_len,
                           203, "Get_peers without info_hash");
                break;
            } else {
                struct storage *st = find_storage(m.info_hash);
                unsigned char token[TOKEN_SIZE];
                make_token(from, 0, token);
                if(st && st->numpeers > 0) {
                     debugf("Sending %s peers from local storage to node %s",
                            af_to_ivs(from->sa_family), sa_to_str(from));
                     send_closest_nodes(from, fromlen,
                                        m.tid, m.tid_len,
                                        m.info_hash, m.want,
                                        from->sa_family, st,
                                        token, TOKEN_SIZE);
                } else {
                    debugf("Sending closest nodes for get_peers to node %s", sa_to_str(from));
                    send_closest_nodes(from, fromlen,
                                       m.tid, m.tid_len, m.info_hash, m.want,
                                       0, NULL, token, TOKEN_SIZE);
                }
            }
            break;
        case ANNOUNCE_PEER:
            debugf("Received announce_peer from node %s", sa_to_str(from));
            new_node(m.id, from, fromlen, 1);
            if(id_cmp(m.info_hash, zeroes) == 0) {
                warnf("Announce_peer received from node %s does not contain an info_hash",
                      sa_to_str(from));
                send_error(from, fromlen, m.tid, m.tid_len,
                           203, "Announce_peer without info_hash");
                break;
            }
            if(!token_match(m.token, m.token_len, from)) {
                warnf("Announce_peer received from node %s contains an incorrect token",
                      sa_to_str(from));
                send_error(from, fromlen, m.tid, m.tid_len,
                           203, "Announce_peer with incorrect token");
                break;
            }
            if(m.implied_port != 0) {
                /* Do this even if port > 0.  That's what the spec says. */
                switch(from->sa_family) {
                case AF_INET:
                    m.port = htons(((struct sockaddr_in*)from)->sin_port);
                    break;
                case AF_INET6:
                    m.port = htons(((struct sockaddr_in6*)from)->sin6_port);
                    break;
                }
            }
            if(m.port == 0) {
                warnf("Announce_peer received from node %s contains forbidden port %d",
                      sa_to_str(from), m.port);
                send_error(from, fromlen, m.tid, m.tid_len,
                           203, "Announce_peer with forbidden port");
                break;
            }
            storage_store(m.info_hash, from, m.port);
            /* Note that if storage_store failed, we lie to the requestor.
               This is to prevent them from backtracking, and hence
               polluting the DHT. */
            debugf("Sending peer_announced to node %s", sa_to_str(from));
            send_peer_announced(from, fromlen, m.tid, m.tid_len);
        }
    }

 dontread:
    if(now.tv_sec >= rotate_secrets_time)
        rotate_secrets();

    if(now.tv_sec >= expire_stuff_time) {
        expire_buckets(buckets);
        expire_buckets(buckets6);
        expire_storage();
        expire_searches(callback, closure);
    }

    if(search_time > 0 && now.tv_sec >= search_time) {
        struct search *sr;
        sr = searches;
        while(sr) {
            if(!sr->done &&
               sr->step_time + DHT_SEARCH_RETRANSMIT / 2 + 1 <= now.tv_sec) {
                search_step(sr, callback, closure);
            }
            sr = sr->next;
        }

        search_time = 0;

        sr = searches;
        while(sr) {
            if(!sr->done) {
                time_t tm = sr->step_time +
                    DHT_SEARCH_RETRANSMIT + random() % DHT_SEARCH_RETRANSMIT;
                if(search_time == 0 || search_time > tm)
                    search_time = tm;
            }
            sr = sr->next;
        }
    }

    if(now.tv_sec >= confirm_nodes_time) {
        int soon = 0;

        soon |= bucket_maintenance(AF_INET);
        soon |= bucket_maintenance(AF_INET6);

        if(!soon) {
            if(mybucket_grow_time >= now.tv_sec - 150)
                soon |= neighbourhood_maintenance(AF_INET);
            if(mybucket6_grow_time >= now.tv_sec - 150)
                soon |= neighbourhood_maintenance(AF_INET6);
        }

        /* Given the timeouts in bucket_maintenance, with a 22-bucket
           table, worst case is a ping every 18 seconds (22 buckets plus
           11 buckets overhead for the larger buckets).  Keep the "soon"
           case within 15 seconds, which gives some margin for neighbourhood
           maintenance. */

        if(soon)
            confirm_nodes_time = now.tv_sec + 5 + random() % 10;
        else
            confirm_nodes_time = now.tv_sec + 60 + random() % 120;
    }

    if(confirm_nodes_time > now.tv_sec)
        *tosleep = confirm_nodes_time - now.tv_sec;
    else
        *tosleep = 0;

    if(search_time > 0) {
        if(search_time <= now.tv_sec)
            *tosleep = 0;
        else if(*tosleep > search_time - now.tv_sec)
            *tosleep = search_time - now.tv_sec;
    }

    /* Bootstrap. */
    if(bootstrap_time > 0 && now.tv_sec >= bootstrap_time) {
        bootstrap_periodic(AF_INET, callback, closure);
        bootstrap_periodic(AF_INET6, callback, closure);
        bootstrap_update_timer();
    }
    if(bootstrap_time > 0) {
        if(bootstrap_time <= now.tv_sec)
            *tosleep = 0;
        else if(*tosleep > bootstrap_time - now.tv_sec)
            *tosleep = bootstrap_time - now.tv_sec;
    }

    return 1;
}

int
dht_get_nodes(struct sockaddr_in *sin, int *num,
              struct sockaddr_in6 *sin6, int *num6)
{
    int i, j;
    struct bucket *b;
    struct node *n;

    i = 0;

    /* For restoring to work without discarding too many nodes, the list
       must start with the contents of our bucket. */
    b = find_bucket(myid, AF_INET);
    if(b == NULL)
        goto no_ipv4;

    n = b->nodes;
    while(n && i < *num) {
        if(node_good(n)) {
            sin[i] = *(struct sockaddr_in*)&n->ss;
            i++;
        }
        n = n->next;
    }

    b = buckets;
    while(b && i < *num) {
        if(!in_bucket(myid, b)) {
            n = b->nodes;
            while(n && i < *num) {
                if(node_good(n)) {
                    sin[i] = *(struct sockaddr_in*)&n->ss;
                    i++;
                }
                n = n->next;
            }
        }
        b = b->next;
    }

 no_ipv4:

    j = 0;

    b = find_bucket(myid, AF_INET6);
    if(b == NULL)
        goto no_ipv6;

    n = b->nodes;
    while(n && j < *num6) {
        if(node_good(n)) {
            sin6[j] = *(struct sockaddr_in6*)&n->ss;
            j++;
        }
        n = n->next;
    }

    b = buckets6;
    while(b && j < *num6) {
        if(!in_bucket(myid, b)) {
            n = b->nodes;
            while(n && j < *num6) {
                if(node_good(n)) {
                    sin6[j] = *(struct sockaddr_in6*)&n->ss;
                    j++;
                }
                n = n->next;
            }
        }
        b = b->next;
    }

 no_ipv6:

    *num = i;
    *num6 = j;
    return i + j;
}

int
dht_insert_node(const unsigned char *id, struct sockaddr *sa, int salen)
{
    struct node *n;

    if(sa->sa_family != AF_INET && sa->sa_family != AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    n = new_node(id, sa, salen, 0);
    return !!n;
}

int
dht_ping_node(const struct sockaddr *sa, int salen)
{
    unsigned char tid[4];

    debugf("Sending ping to node %s", sa_to_str(sa));
    make_tid(tid, "pn", 0);
    return send_ping(sa, salen, tid, 4);
}

/* We could use a proper bencoding printer and parser, but the format of
   DHT messages is fairly stylised, so this seemed simpler. */

#define CHECK(offset, delta, size)                      \
    if(delta < 0 || offset + delta > size) goto fail

#define INC(offset, delta, size)                        \
    CHECK(offset, delta, size);                         \
    offset += delta

#define COPY(buf, offset, src, delta, size)             \
    CHECK(offset, delta, size);                         \
    memcpy(buf + offset, src, delta);                   \
    offset += delta;

#define ADD_V(buf, offset, size)                        \
    if(have_v) {                                        \
        COPY(buf, offset, my_v, sizeof(my_v), size);    \
    }

static int
dht_send(const void *buf, size_t len, int flags,
         const struct sockaddr *sa, int salen)
{
    int s;

    if(salen == 0)
        abort();

    if(node_blacklisted(sa, salen)) {
        warnf("Attempted to send to blacklisted node %s", sa_to_str(sa));
        errno = EPERM;
        return -1;
    }

    if(sa->sa_family == AF_INET)
        s = dht_socket;
    else if(sa->sa_family == AF_INET6)
        s = dht_socket6;
    else
        s = -1;

    if(s < 0) {
        errno = EAFNOSUPPORT;
        return -1;
    }

#if (DHT_LOG_OUTGOING_MESSAGES == 1)
    debugf(">>> %s", ba_to_str(buf, len));
#endif

    return dht_sendto(s, buf, len, flags, sa, salen);
}

int
send_ping(const struct sockaddr *sa, int salen,
          const unsigned char *tid, int tid_len)
{
    char buf[512];
    int i = 0, rc;
    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "e1:q4:ping1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    ADD_V(buf, i, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:qe"); INC(i, rc, 512);
    return dht_send(buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_pong(const struct sockaddr *sa, int salen,
          const unsigned char *tid, int tid_len)
{
    char buf[512];
    int i = 0, rc;
    rc = snprintf(buf + i, 512 - i, "d1:rd2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "e1:t%d:", tid_len); INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    ADD_V(buf, i, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:re"); INC(i, rc, 512);
    return dht_send(buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_find_node(const struct sockaddr *sa, int salen,
               const unsigned char *tid, int tid_len,
               const unsigned char *target, int want, int confirm)
{
    char buf[512];
    int i = 0, rc;
    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "6:target20:"); INC(i, rc, 512);
    COPY(buf, i, target, 20, 512);
    if(want > 0) {
        rc = snprintf(buf + i, 512 - i, "4:wantl%s%se",
                      (want & WANT4) ? "2:n4" : "",
                      (want & WANT6) ? "2:n6" : "");
        INC(i, rc, 512);
    }
    rc = snprintf(buf + i, 512 - i, "e1:q9:find_node1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    ADD_V(buf, i, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:qe"); INC(i, rc, 512);
    return dht_send(buf, i, confirm ? MSG_CONFIRM : 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_nodes_peers(const struct sockaddr *sa, int salen,
                 const unsigned char *tid, int tid_len,
                 const unsigned char *nodes, int nodes_len,
                 const unsigned char *nodes6, int nodes6_len,
                 int af, struct storage *st,
                 const unsigned char *token, int token_len)
{
    char buf[2048];
    int i = 0, rc, j0, j, k, len;

    rc = snprintf(buf + i, 2048 - i, "d1:rd2:id20:"); INC(i, rc, 2048);
    COPY(buf, i, myid, 20, 2048);
    if(nodes_len > 0) {
        rc = snprintf(buf + i, 2048 - i, "5:nodes%d:", nodes_len);
        INC(i, rc, 2048);
        COPY(buf, i, nodes, nodes_len, 2048);
    }
    if(nodes6_len > 0) {
         rc = snprintf(buf + i, 2048 - i, "6:nodes6%d:", nodes6_len);
         INC(i, rc, 2048);
         COPY(buf, i, nodes6, nodes6_len, 2048);
    }
    if(token_len > 0) {
        rc = snprintf(buf + i, 2048 - i, "5:token%d:", token_len);
        INC(i, rc, 2048);
        COPY(buf, i, token, token_len, 2048);
    }

    if(st && st->numpeers > 0) {
        /* We treat the storage as a circular list, and serve a randomly
           chosen slice.  In order to make sure we fit within 1024 octets,
           we limit ourselves to 50 peers. */

        len = af == AF_INET ? 4 : 16;
        j0 = random() % st->numpeers;
        j = j0;
        k = 0;

        rc = snprintf(buf + i, 2048 - i, "6:valuesl"); INC(i, rc, 2048);
        do {
            if(st->peers[j].len == len) {
                unsigned short swapped;
                swapped = htons(st->peers[j].port);
                rc = snprintf(buf + i, 2048 - i, "%d:", len + 2);
                INC(i, rc, 2048);
                COPY(buf, i, st->peers[j].ip, len, 2048);
                COPY(buf, i, &swapped, 2, 2048);
                k++;
            }
            j = (j + 1) % st->numpeers;
        } while(j != j0 && k < 50);
        rc = snprintf(buf + i, 2048 - i, "e"); INC(i, rc, 2048);
    }

    rc = snprintf(buf + i, 2048 - i, "e1:t%d:", tid_len); INC(i, rc, 2048);
    COPY(buf, i, tid, tid_len, 2048);
    ADD_V(buf, i, 2048);
    rc = snprintf(buf + i, 2048 - i, "1:y1:re"); INC(i, rc, 2048);

    return dht_send(buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

static int
insert_closest_node(unsigned char *nodes, int numnodes,
                    const unsigned char *id, struct node *n)
{
    int i, size;

    if(n->ss.ss_family == AF_INET)
        size = 26;
    else if(n->ss.ss_family == AF_INET6)
        size = 38;
    else
        abort();

    for(i = 0; i< numnodes; i++) {
        if(id_cmp(n->id, nodes + size * i) == 0)
            return numnodes;
        if(xorcmp(n->id, nodes + size * i, id) < 0)
            break;
    }

    if(i == 8)
        return numnodes;

    if(numnodes < 8)
        numnodes++;

    if(i < numnodes - 1)
        memmove(nodes + size * (i + 1), nodes + size * i,
                size * (numnodes - i - 1));

    if(n->ss.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in*)&n->ss;
        memcpy(nodes + size * i, n->id, 20);
        memcpy(nodes + size * i + 20, &sin->sin_addr, 4);
        memcpy(nodes + size * i + 24, &sin->sin_port, 2);
    } else if(n->ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)&n->ss;
        memcpy(nodes + size * i, n->id, 20);
        memcpy(nodes + size * i + 20, &sin6->sin6_addr, 16);
        memcpy(nodes + size * i + 36, &sin6->sin6_port, 2);
    } else {
        abort();
    }

    return numnodes;
}

static int
buffer_closest_nodes(unsigned char *nodes, int numnodes,
                     const unsigned char *id, struct bucket *b)
{
    struct node *n = b->nodes;
    while(n) {
        if(node_good(n))
            numnodes = insert_closest_node(nodes, numnodes, id, n);
        n = n->next;
    }
    return numnodes;
}

int
send_closest_nodes(const struct sockaddr *sa, int salen,
                   const unsigned char *tid, int tid_len,
                   const unsigned char *id, int want,
                   int af, struct storage *st,
                   const unsigned char *token, int token_len)
{
    unsigned char nodes[8 * 26];
    unsigned char nodes6[8 * 38];
    int numnodes = 0, numnodes6 = 0;
    struct bucket *b;

    if(want < 0)
        want = sa->sa_family == AF_INET ? WANT4 : WANT6;

    if((want & WANT4)) {
        b = find_bucket(id, AF_INET);
        if(b) {
            numnodes = buffer_closest_nodes(nodes, numnodes, id, b);
            if(b->next)
                numnodes = buffer_closest_nodes(nodes, numnodes, id, b->next);
            b = previous_bucket(b);
            if(b)
                numnodes = buffer_closest_nodes(nodes, numnodes, id, b);
        }
    }

    if((want & WANT6)) {
        b = find_bucket(id, AF_INET6);
        if(b) {
            numnodes6 = buffer_closest_nodes(nodes6, numnodes6, id, b);
            if(b->next)
                numnodes6 =
                    buffer_closest_nodes(nodes6, numnodes6, id, b->next);
            b = previous_bucket(b);
            if(b)
                numnodes6 = buffer_closest_nodes(nodes6, numnodes6, id, b);
        }
    }
    debugf("  %d nodes (IPv4: %d, IPv6: %d)",
           numnodes + numnodes6, numnodes, numnodes6);

    return send_nodes_peers(sa, salen, tid, tid_len, nodes, numnodes * 26,
                            nodes6, numnodes6 * 38, af, st, token, token_len);
}

int
send_get_peers(const struct sockaddr *sa, int salen,
               unsigned char *tid, int tid_len, unsigned char *infohash,
               int want, int confirm)
{
    char buf[512];
    int i = 0, rc;

    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "9:info_hash20:"); INC(i, rc, 512);
    COPY(buf, i, infohash, 20, 512);
    if(want > 0) {
        rc = snprintf(buf + i, 512 - i, "4:wantl%s%se",
                      (want & WANT4) ? "2:n4" : "",
                      (want & WANT6) ? "2:n6" : "");
        INC(i, rc, 512);
    }
    rc = snprintf(buf + i, 512 - i, "e1:q9:get_peers1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    ADD_V(buf, i, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:qe"); INC(i, rc, 512);
    return dht_send(buf, i, confirm ? MSG_CONFIRM : 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_announce_peer(const struct sockaddr *sa, int salen,
                   unsigned char *tid, int tid_len,
                   unsigned char *infohash, unsigned short port,
                   unsigned char *token, int token_len, int confirm)
{
    char buf[512];
    int i = 0, rc;

    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "9:info_hash20:"); INC(i, rc, 512);
    COPY(buf, i, infohash, 20, 512);
    rc = snprintf(buf + i, 512 - i, "4:porti%ue5:token%d:", (unsigned)port,
                  token_len);
    INC(i, rc, 512);
    COPY(buf, i, token, token_len, 512);
    rc = snprintf(buf + i, 512 - i, "e1:q13:announce_peer1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    ADD_V(buf, i, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:qe"); INC(i, rc, 512);

    return dht_send(buf, i, confirm ? 0 : MSG_CONFIRM, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

static int
send_peer_announced(const struct sockaddr *sa, int salen,
                    unsigned char *tid, int tid_len)
{
    char buf[512];
    int i = 0, rc;

    rc = snprintf(buf + i, 512 - i, "d1:rd2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "e1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    ADD_V(buf, i, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:re"); INC(i, rc, 512);
    return dht_send(buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

static int
send_error(const struct sockaddr *sa, int salen,
           unsigned char *tid, int tid_len,
           int code, const char *message)
{
    char buf[512];
    int i = 0, rc, message_len;

    message_len = strlen(message);
    rc = snprintf(buf + i, 512 - i, "d1:eli%de%d:", code, message_len);
    INC(i, rc, 512);
    COPY(buf, i, message, message_len, 512);
    rc = snprintf(buf + i, 512 - i, "e1:t%d:", tid_len); INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    ADD_V(buf, i, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:ee"); INC(i, rc, 512);
    return dht_send(buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

#undef CHECK
#undef INC
#undef COPY
#undef ADD_V

#ifdef HAVE_MEMMEM

static void *
dht_memmem(const void *haystack, size_t haystacklen,
           const void *needle, size_t needlelen)
{
    return memmem(haystack, haystacklen, needle, needlelen);
}

#else

static void *
dht_memmem(const void *haystack, size_t haystacklen,
           const void *needle, size_t needlelen)
{
    const char *h = haystack;
    const char *n = needle;
    size_t i;

    /* size_t is unsigned */
    if(needlelen > haystacklen)
        return NULL;

    for(i = 0; i <= haystacklen - needlelen; i++) {
        if(memcmp(h + i, n, needlelen) == 0)
            return (void*)(h + i);
    }
    return NULL;
}

#endif

static int
parse_message(const unsigned char *buf, int buflen,
              struct parsed_message *m)
{
    const unsigned char *p;

    /* This code will happily crash if the buffer is not NUL-terminated. */
    if(buf[buflen] != '\0') {
        errorf("Attempted to parse message with unterminated buffer, message: %s",
               ba_to_str(buf, buflen));
        return -1;
    }


#define CHECK(ptr, len)                                                 \
    if(((unsigned char*)ptr) + (len) > (buf) + (buflen)) goto overflow;

    p = dht_memmem(buf, buflen, "1:t", 3);
    if(p) {
        long l;
        char *q;
        l = strtol((char*)p + 3, &q, 10);
        if(q && *q == ':' && l > 0 && l < PARSE_TID_LEN) {
            CHECK(q + 1, l);
            memcpy(m->tid, q + 1, l);
            m->tid_len = l;
        }
    }

    p = dht_memmem(buf, buflen, "2:id20:", 7);
    if(p) {
        CHECK(p + 7, 20);
        memcpy(m->id, p + 7, 20);
    }

    p = dht_memmem(buf, buflen, "9:info_hash20:", 14);
    if(p) {
        CHECK(p + 14, 20);
        memcpy(m->info_hash, p + 14, 20);
    }

    p = dht_memmem(buf, buflen, "4:porti", 7);
    if(p) {
        long l;
        char *q;
        l = strtol((char*)p + 7, &q, 10);
        if(q && *q == 'e' && l > 0 && l < 0x10000)
            m->port = l;
    }

    p = dht_memmem(buf, buflen, "12:implied_porti", 16);
    if(p) {
        long l;
        char *q;
        l = strtol((char*)p + 16, &q, 10);
        if(q && *q == 'e' && l > 0 && l < 0x10000)
            m->implied_port = l;
    }

    p = dht_memmem(buf, buflen, "6:target20:", 11);
    if(p) {
        CHECK(p + 11, 20);
        memcpy(m->target, p + 11, 20);
    }

    p = dht_memmem(buf, buflen, "5:token", 7);
    if(p) {
        long l;
        char *q;
        l = strtol((char*)p + 7, &q, 10);
        if(q && *q == ':' && l > 0 && l < PARSE_TOKEN_LEN) {
            CHECK(q + 1, l);
            memcpy(m->token, q + 1, l);
            m->token_len = l;
        }
    }

    p = dht_memmem(buf, buflen, "5:nodes", 7);
    if(p) {
        long l;
        char *q;
        l = strtol((char*)p + 7, &q, 10);
        if(q && *q == ':' && l > 0 && l <= PARSE_NODES_LEN) {
            CHECK(q + 1, l);
            memcpy(m->nodes, q + 1, l);
            m->nodes_len = l;
        }
    }

    p = dht_memmem(buf, buflen, "6:nodes6", 8);
    if(p) {
        long l;
        char *q;
        l = strtol((char*)p + 8, &q, 10);
        if(q && *q == ':' && l > 0 && l <= PARSE_NODES6_LEN) {
            CHECK(q + 1, l);
            memcpy(m->nodes6, q + 1, l);
            m->nodes6_len = l;
        }
    }

    p = dht_memmem(buf, buflen, "6:valuesl", 9);
    if(p) {
        int i = p - buf + 9;
        int j = 0, j6 = 0;
        while(1) {
            long l;
            char *q;
            l = strtol((char*)buf + i, &q, 10);
            if(q && *q == ':' && l > 0) {
                CHECK(q + 1, l);
                i = q + 1 + l - (char*)buf;
                if(l == 6) {
                    if(j + l > PARSE_VALUES_LEN)
                        continue;
                    memcpy((char*)m->values + j, q + 1, l);
                    j += l;
                } else if(l == 18) {
                    if(j6 + l > PARSE_VALUES6_LEN)
                        continue;
                    memcpy((char*)m->values6 + j6, q + 1, l);
                    j6 += l;
                } else {
                    warnf("Encountered weird value of %d bytes while parsing message", (int)l);
                }
            } else {
                break;
            }
        }
        if(i >= buflen || buf[i] != 'e')
            warnf("Encountered unexpected end for values while parsing message");
        m->values_len = j;
        m->values6_len = j6;
    }

    p = dht_memmem(buf, buflen, "4:wantl", 7);
    if(p) {
        int i = p - buf + 7;
        m->want = 0;
        while(buf[i] > '0' && buf[i] <= '9' && buf[i + 1] == ':' &&
              i + 2 + buf[i] - '0' < buflen) {
            CHECK(buf + i + 2, buf[i] - '0');
            if(buf[i] == '2' && memcmp(buf + i + 2, "n4", 2) == 0)
                m->want |= WANT4;
            else if(buf[i] == '2' && memcmp(buf + i + 2, "n6", 2) == 0)
                m->want |= WANT6;
            else
                warnf("Encountered unexpected want flag %c while parsing message", buf[i]);
            i += 2 + buf[i] - '0';
        }
        if(i >= buflen || buf[i] != 'e')
            warnf("Encountered unexpected end for want while parsing message");
    }

#undef CHECK

    if(dht_memmem(buf, buflen, "1:y1:r", 6))
        return REPLY;
    if(dht_memmem(buf, buflen, "1:y1:e", 6))
        return ERROR;
    if(!dht_memmem(buf, buflen, "1:y1:q", 6))
        return -1;
    if(dht_memmem(buf, buflen, "1:q4:ping", 9))
        return PING;
    if(dht_memmem(buf, buflen, "1:q9:find_node", 14))
       return FIND_NODE;
    if(dht_memmem(buf, buflen, "1:q9:get_peers", 14))
        return GET_PEERS;
    if(dht_memmem(buf, buflen, "1:q13:announce_peer", 19))
       return ANNOUNCE_PEER;
    return -1;

 overflow:
    warnf("Encountered unexpected end of message while parsing message");
    return -1;
}

/* Add bootstrap node. */
int
dht_add_bootstrap_node(const struct sockaddr* sa, int salen)
{
    if(sa == NULL || salen <= 0) {
        errno = EINVAL;
        return -1;
    }
    if(sa->sa_family != AF_INET && sa->sa_family != AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    /* Find end of bootstrap node list, check if node already exists. */
    struct bootstrap *bs = (sa->sa_family == AF_INET ? &bootstrap : &bootstrap6);
    struct bootstrap_node *bn = bs->nodes;
    while(bn) {
        if(bn->sslen == salen && memcmp(&bn->ss, sa, salen) == 0) {
            warnf("Unable to add %s bootstrap node %s, already added", af_to_ivs(sa->sa_family), sa_to_str(sa));
            return 0;
        }
        if(bn->next == NULL)
            break;
        bn = bn->next;
    }

    /* Create new bootstrap node. */
    debugf("Adding %s bootstrap node %s", af_to_ivs(sa->sa_family), sa_to_str(sa));
    struct bootstrap_node *new_bn = calloc(1, sizeof(struct bootstrap_node));
    if(new_bn == NULL)
        return -1;
    memcpy(&new_bn->ss, sa, salen);
    new_bn->sslen = salen;
    new_bn->next = NULL;
    bs->numnodes++;

    /* Append new bootstrap node. */
    if(bn == NULL)
        bs->nodes = new_bn;
    else
        bn->next = new_bn;

    return 1;
}

/* Enable/disable bootstrap. */
int
dht_enable_bootstrap(int af, int enabled)
{
    /* Sanity checks. */
    if(af != AF_INET && af != AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    /* Pointer to bootstrap data. */
    struct bootstrap *bs = (af == AF_INET ? &bootstrap : &bootstrap6);

    /* Disable bootstrap. */
    if(!enabled) {
        infof("Disabling %s bootstrap", af_to_ivs(af));
        bs->state = DHT_BOOTSTRAP_STATE_DISABLED;
        bs->start_time = 0;
        bs->end_time = 0;
        bs->next_time = 0;
        bootstrap_update_timer();
        return 1;
    }

    /* Address family enabled? */
    int socket = (af == AF_INET ? dht_socket : dht_socket6);
    if(socket < 0) {
        errorf("Unable to enable %s bootstrap, %s is not enabled", af_to_ivs(af), af_to_ivs(af));
        bs->state = DHT_BOOTSTRAP_STATE_DISABLED;
        bs->start_time = 0;
        bs->end_time = 0;
        bs->next_time = 0;
        bootstrap_update_timer();
        return 0;
    }

    /* Bootstrap necessary (may happen if saved state was loaded)? */
    int numgood = 0;
    dht_stats(af, NULL, &numgood, NULL, NULL);
    if(numgood >= DHT_BOOTSTRAP_GOOD_TARGET) {
        infof("No %s bootstrap necessary, bootstrap complete (good nodes: %i, target: %i)", af_to_ivs(af), numgood, DHT_BOOTSTRAP_GOOD_TARGET);
        bs->state = DHT_BOOTSTRAP_STATE_COMPLETE;
        bs->start_time = now.tv_sec;
        bs->end_time = now.tv_sec;
        bs->next_time = 0;
        bootstrap_update_timer();
        return 1;
    }

    /* Bootstrap nodes available? */
    if(bs->nodes == NULL) {
        errorf("Unable to enable %s bootstrap, no %s bootstrap nodes available", af_to_ivs(af), af_to_ivs(af));
        bs->state = DHT_BOOTSTRAP_STATE_DISABLED;
        bs->start_time = 0;
        bs->end_time = 0;
        bs->next_time = 0;
        bootstrap_update_timer();
        return 0;
    }

    /* Enable bootstrap. */
    infof("Enabling %s bootstrap", af_to_ivs(af));
    bs->state = DHT_BOOTSTRAP_STATE_ENABLED;
    bs->start_time = 0;
    bs->end_time = 0;
    bs->next_time = now.tv_sec;
    bootstrap_update_timer();
    return 1;
}

/* Get bootstrap state. */
int
dht_bootstrap_state(int af)
{
    if(af != AF_INET && af != AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    return (af == AF_INET ? bootstrap.state : bootstrap6.state);
}

/* Update bootstrap timer. */
static void
bootstrap_update_timer()
{
    bootstrap_time = (bootstrap.next_time > bootstrap6.next_time ? bootstrap.next_time : bootstrap6.next_time);
}

/* Switch bootstrap state. */
static void
bootstrap_switch_state(int af, int state, dht_main_callback_t *callback, void *closure)
{
    if(af == AF_INET) {
        bootstrap.state = state;
        if(callback)
            (*callback)(closure, DHT_EVENT_BOOTSTRAP, NULL, &bootstrap.state, sizeof(bootstrap.state));
    } else {
        bootstrap6.state = state;
        if(callback)
            (*callback)(closure, DHT_EVENT_BOOTSTRAP6, NULL, &bootstrap6.state, sizeof(bootstrap6.state));
    }
}

/* Perform bootstrap (called by dht_periodic). */
static void
bootstrap_periodic(int af, dht_main_callback_t *callback, void *closure)
{
    struct bootstrap *bs = (af == AF_INET ? &bootstrap : &bootstrap6);
    int numbuckets, numgood, numdubious, numtotal;
    unsigned char id[20];


    /* If bootstrap is enabled ... */
    if(bs->state == DHT_BOOTSTRAP_STATE_ENABLED) {

        /* ... start it now. */
        infof("Starting %s bootstrap (%i bootstrap nodes)", af_to_ivs(af), bs->numnodes);
        bs->start_time = now.tv_sec;

        /* Add bootstrap nodes to routing table using fake ids (myid, first
           bit flipped, last 4 bytes randomized). */
        memcpy(id, myid, 20);
        id[0] ^= 0x80;
        for(struct bootstrap_node *bn = bs->nodes; bn != NULL; bn = bn->next) {
            dht_random_bytes(&id[16], 4);
            debugf("Adding bootstrap node %s with id %s", sa_to_str((struct sockaddr*)&bn->ss), id_to_hex(id));
            new_node(id, (struct sockaddr*)&bn->ss, bn->sslen, 0);
        }

        /* Get and print initial stats. */
        dht_stats(af, &numbuckets, &numgood, &numdubious, &numtotal);
        infof("%s bootstrap started: buckets: %i, good: %i, dubious: %i, total: %i, time: %lis", af_to_ivs(af), numbuckets, numgood, numdubious, numtotal, now.tv_sec - bs->start_time);

        /* Switch state. */
        bootstrap_switch_state(af, DHT_BOOTSTRAP_STATE_RUNNING, callback, closure);
        bs->next_time = now.tv_sec;
        return;


    /* If bootstrap is running ... */
    } else if(bs->state == DHT_BOOTSTRAP_STATE_RUNNING) {

        /* ... check if it should be stopped ... */
        dht_stats(af, &numbuckets, &numgood, &numdubious, &numtotal);

        /* Bootstrap complete? */
        if(numgood >= DHT_BOOTSTRAP_GOOD_TARGET) {
            bs->end_time = now.tv_sec;
            infof("%s bootstrap complete: buckets: %i, good: %i, dubious: %i, total: %i, time: %lis", af_to_ivs(af), numbuckets, numgood, numdubious, numtotal, bs->end_time - bs->start_time);
            bootstrap_switch_state(af, DHT_BOOTSTRAP_STATE_COMPLETE, callback, closure);
            bs->next_time = 0;
            confirm_nodes_time = 0;     /* trigger bucket/neighborhood maintenance. */
            return;
        }

        /* Bootstrap failure? */
        if(numtotal <= 0) {
            bs->end_time = now.tv_sec;
            warnf("%s bootstrap failed, no %s nodes available (buckets: %i, good: %i, dubious: %i, total: %i, time: %lis)", af_to_ivs(af), af_to_ivs(af), numbuckets, numgood, numdubious, numtotal, bs->end_time - bs->start_time);
            bootstrap_switch_state(af, DHT_BOOTSTRAP_STATE_FAILED, callback, closure);
            bs->next_time = 0;
            return;
        }

        /* ... otherwise perform bootstrap / keep bootstrapping. */
        struct bucket *rb = (af == AF_INET ? buckets : buckets6);
        int finds = 0, pings = 0;

        /* Purge unresponsive nodes. */
        expire_buckets(rb);

        /* Get and print current stats. */
        dht_stats(af, &numbuckets, &numgood, &numdubious, &numtotal);
        infof("%s bootstrap status: buckets: %i, good: %i, dubious: %i, total: %i, time: %lis", af_to_ivs(af), numbuckets, numgood, numdubious, numtotal, now.tv_sec - bs->start_time);

        /* Copy myid for find_node. */
        memcpy(id, myid, 20);

        /* Randomly process buckets. The loop is run twice as long to account
           for collisions caused by random_bucket(). */
        for(int i = 0; i < numbuckets*2; i++) {
            struct bucket *b = random_bucket(rb, numbuckets);
            if(b == NULL) {
                errorf("oops random bucket?!");
                return;
            }

            /* Randomly process nodes of current bucket. The loop is run twice
               as long to account for collisions caused by random_node(). */
            for(int j = 0; j < b->count*2; j++) {
                struct node *n = random_node(b);
                if(n == NULL) {
                    errorf("oops random node?!");
                    return;
                }

                /* If node is good and we can handle more dubious nodes, send
                   find_node. If node is dubious, send ping to turn it into a
                   good node. */
                if(node_good(n)) {
                    if(finds < DHT_BOOTSTRAP_MAX_FINDS && numdubious < DHT_BOOTSTRAP_MAX_DUBIOUS && n->pinged_time < now.tv_sec - 15) {
                        id[19] = random() & 0xFF;
                        unsigned char tid[4];
                        make_tid(tid, "fn", 0);
                        debugf("   Sending find_node for id %s to node %s", id_to_hex(id), sa_to_str((struct sockaddr*)&n->ss));
                        send_find_node((struct sockaddr*)&n->ss, n->sslen,
                                        tid, 4, id, -1,
                                        n->reply_time >= now.tv_sec - 15);
                        n->pinged++;
                        n->pinged_time = now.tv_sec;
                        numdubious += DHT_BOOTSTRAP_EXPECTED_NODES; /* projection; keeps us from sending out too many requests */
                        finds++;
                    }
                } else {
                    if(pings < DHT_BOOTSTRAP_MAX_PINGS && n->pinged_time < now.tv_sec - 15) {
                        debugf("   Sending ping to dubious node %s", sa_to_str((struct sockaddr*)&n->ss));
                        unsigned char tid[4];
                        make_tid(tid, "pn", 0);
                        send_ping((struct sockaddr*)&n->ss, n->sslen, tid, 4);
                        n->pinged++;
                        n->pinged_time = now.tv_sec;
                        pings++;
                    }
                }

                /* Break loop if all available nodes have been contacted or
                   limits of finds/pings are reached. */
                if(finds+pings >= numtotal || (finds >= DHT_BOOTSTRAP_MAX_FINDS && pings >= DHT_BOOTSTRAP_MAX_PINGS))
                    break;
            }

            /* Break loop if all available nodes have been contacted or
               limits of finds/pings are reached. */
            if(finds+pings >= numtotal || (finds >= DHT_BOOTSTRAP_MAX_FINDS && pings >= DHT_BOOTSTRAP_MAX_PINGS))
                break;
        }

        /* Schedule next bootstrap iteration. */
        bs->next_time = now.tv_sec + DHT_BOOTSTRAP_INTERVAL;
        return;
    }
}
