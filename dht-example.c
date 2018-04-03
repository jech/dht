/* This example code was written by Juliusz Chroboczek.
   You are free to cut'n'paste from it to your heart's content. */

/* For crypt */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <sys/signal.h>
#include <termios.h>

#include "dht.h"

#define MAX_BOOTSTRAP_NODES 20
static struct sockaddr_storage bootstrap_nodes[MAX_BOOTSTRAP_NODES];
static int num_bootstrap_nodes = 0;

static volatile sig_atomic_t dumping = 0, searching = 0, exiting = 0;

static void
sigdump(int signo)
{
    dumping = 1;
}

static void
sigtest(int signo)
{
    searching = 1;
}

static void
sigexit(int signo)
{
    exiting = 1;
}

static void
init_signals(void)
{
    struct sigaction sa;
    sigset_t ss;

    sigemptyset(&ss);
    sa.sa_handler = sigdump;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigtest;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

const unsigned char hash[20] = {
    0x54, 0x57, 0x87, 0x89, 0xdf, 0xc4, 0x23, 0xee, 0xf6, 0x03,
    0x1f, 0x81, 0x94, 0xa9, 0x3a, 0x16, 0x98, 0x8b, 0x72, 0x7b
};

/* The call-back function is called by the DHT whenever something
   interesting happens.  Right now, it only happens when we get a new value or
   when a search completes, but this may be extended in future versions. */
static void
callback(void *closure,
         int event,
         const unsigned char *info_hash,
         const void *data, size_t data_len)
{
    printf("\e[1m");
    if(event == DHT_EVENT_SEARCH_DONE)
        printf("Search done.");
    else if(event == DHT_EVENT_SEARCH_DONE6)
        printf("IPv6 search done.");
    else if(event == DHT_EVENT_VALUES)
        printf("Received %d values.", (int)(data_len / 6));
    else if(event == DHT_EVENT_VALUES6)
        printf("Received %d IPv6 values.", (int)(data_len / 18));
    else
        printf("Unknown DHT event %d.", event);
    printf("\e[0m\n");
}

static unsigned char buf[4096];

static int
set_nonblocking(int fd, int nonblocking)
{
    int rc;
    rc = fcntl(fd, F_GETFL, 0);
    if(rc < 0)
        return -1;

    rc = fcntl(fd, F_SETFL,
               nonblocking ? (rc | O_NONBLOCK) : (rc & ~O_NONBLOCK));
    if(rc < 0)
        return -1;

    return 0;
}

void print_sockaddr(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        struct sockaddr_in *sinp = (struct sockaddr_in*)sa;
        char addrstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sinp->sin_addr, addrstr, sizeof(addrstr));
        printf("%s:%d", addrstr, ntohs(sinp->sin_port));
    } else if (sa->sa_family == AF_INET6) {
        struct sockaddr_in6* sinp6 = (struct sockaddr_in6*)sa;
        char addrstr[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &sinp6->sin6_addr, addrstr, sizeof(addrstr));
        printf("%s:%d", addrstr, ntohs(sinp6->sin6_port));
    } else {
        printf("<invalid>");
    }
}

void print_hex(const unsigned char *str, size_t strlen) {
    for (int i = 0; i < strlen; i++) {
        printf("%02x", str[i]);
    }
}

void print_help() {
    printf("\e[1m");
    printf("Hit 'q', 'e' or CTRL+C to quit\n");
    printf("Hit 's' to display statistics\n");
    printf("Hit 'd' to display routing table\n");
    printf("Hit 't' to test search for hash '"); print_hex(hash, sizeof(hash)); printf("'\n");
    printf("Hit 'h' to display this help message\n");
    printf("\e[0m");
}

void print_stats() {
    int good, dubious, cached, incoming;
    int good6, dubious6, cached6, incoming6;
    dht_nodes(AF_INET, &good, &dubious, &cached, &incoming);
    dht_nodes(AF_INET6, &good6, &dubious6, &cached6, &incoming6);
    printf("\e[1m");
    printf("Stats IPv4: good: %3d, dubious: %3d, cached: %3d, incoming: %3d\n", good, dubious, cached, incoming);
    printf("Stats IPv6: good: %3d, dubious: %3d, cached: %3d, incoming: %3d\n", good6, dubious6, cached6, incoming6);
    printf("Stats both: good: %3d, dubious: %3d, cached: %3d, incoming: %3d\n", good + good6, dubious + dubious6, cached + cached6, incoming + incoming6);
    printf("\e[0m");
}

int
main(int argc, char **argv)
{
    int i, rc, fd;
    int s = -1, s6 = -1, port;
    int have_id = 0;
    unsigned char myid[20];
    time_t tosleep = 0;
    char *id_file = "dht-example.id";
    int opt;
    int quiet = 0, ipv4 = 1, ipv6 = 1;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
    struct sockaddr_storage from;
    socklen_t fromlen;
    struct termios term_old, term_new;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;

    while(1) {
        opt = getopt(argc, argv, "q46b:i:");
        if(opt < 0)
            break;

        switch(opt) {
        case 'q': quiet = 1; break;
        case '4': ipv6 = 0; break;
        case '6': ipv4 = 0; break;
        case 'b': {
            char buf[16];
            int rc;
            rc = inet_pton(AF_INET, optarg, buf);
            if(rc == 1) {
                memcpy(&sin.sin_addr, buf, 4);
                break;
            }
            rc = inet_pton(AF_INET6, optarg, buf);
            if(rc == 1) {
                memcpy(&sin6.sin6_addr, buf, 16);
                break;
            }
            goto usage;
        }
            break;
        case 'i':
            id_file = optarg;
            break;
        default:
            goto usage;
        }
    }

    /* Ids need to be distributed evenly, so you cannot just use your
       bittorrent id.  Either generate it randomly, or take the SHA-1 of
       something. */
    fd = open(id_file, O_RDONLY);
    if(fd >= 0) {
        rc = read(fd, myid, 20);
        if(rc == 20)
            have_id = 1;
        close(fd);
    }

    fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0) {
        perror("open(random)");
        exit(1);
    }

    if(!have_id) {
        int ofd;

        rc = read(fd, myid, 20);
        if(rc < 0) {
            perror("read(random)");
            exit(1);
        }
        have_id = 1;
        close(fd);

        ofd = open(id_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if(ofd >= 0) {
            rc = write(ofd, myid, 20);
            if(rc < 20)
                unlink(id_file);
            close(ofd);
        }
    }

    {
        unsigned seed;
        read(fd, &seed, sizeof(seed));
        srandom(seed);
    }

    close(fd);

    if(argc < 2)
        goto usage;

    i = optind;

    if(argc < i + 1)
        goto usage;

    port = atoi(argv[i++]);
    if(port <= 0 || port >= 0x10000)
        goto usage;

    while(i < argc) {
        struct addrinfo hints, *info, *infop;
        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_DGRAM;
        if(!ipv6)
            hints.ai_family = AF_INET;
        else if(!ipv4)
            hints.ai_family = AF_INET6;
        else
            hints.ai_family = 0;
        rc = getaddrinfo(argv[i], argv[i + 1], &hints, &info);
        if(rc != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
            exit(1);
        }

        i++;
        if(i >= argc)
            goto usage;

        infop = info;
        while(infop) {
            printf("Adding bootstrap node '"); print_sockaddr(infop->ai_addr); printf("'\n");
            memcpy(&bootstrap_nodes[num_bootstrap_nodes],
                   infop->ai_addr, infop->ai_addrlen);
            infop = infop->ai_next;
            num_bootstrap_nodes++;
        }
        freeaddrinfo(info);

        i++;
    }

    /* If you set dht_debug to a stream, every action taken by the DHT will
       be logged. */
    if(!quiet)
        dht_debug = stdout;

    /* We need an IPv4 and an IPv6 socket, bound to a stable port.  Rumour
       has it that uTorrent likes you better when it is the same as your
       Bittorrent port. */
    if(ipv4) {
        s = socket(PF_INET, SOCK_DGRAM, 0);
        if(s < 0) {
            perror("socket(IPv4)");
        }
        rc = set_nonblocking(s, 1);
        if(rc < 0) {
            perror("set_nonblocking(IPv4)");
            exit(1);
        }
    }

    if(ipv6) {
        s6 = socket(PF_INET6, SOCK_DGRAM, 0);
        if(s6 < 0) {
            perror("socket(IPv6)");
        }
        rc = set_nonblocking(s6, 1);
        if(rc < 0) {
            perror("set_nonblocking(IPv6)");
            exit(1);
        }
    }

    if(s < 0 && s6 < 0) {
        fprintf(stderr, "Eek!");
        exit(1);
    }


    if(s >= 0) {
        sin.sin_port = htons(port);
        printf("Binding to IPv4 address '"); print_sockaddr((struct sockaddr*)&sin); printf("'\n");
        rc = bind(s, (struct sockaddr*)&sin, sizeof(sin));
        if(rc < 0) {
            perror("bind(IPv4)");
            exit(1);
        }
    }

    if(s6 >= 0) {
        int rc;
        int val = 1;

        rc = setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY,
                        (char *)&val, sizeof(val));
        if(rc < 0) {
            perror("setsockopt(IPV6_V6ONLY)");
            exit(1);
        }

        /* BEP-32 mandates that we should bind this socket to one of our
           global IPv6 addresses.  In this simple example, this only
           happens if the user used the -b flag. */

        sin6.sin6_port = htons(port);
        printf("Binding to IPv6 address '"); print_sockaddr((struct sockaddr*)&sin6); printf("'\n");
        rc = bind(s6, (struct sockaddr*)&sin6, sizeof(sin6));
        if(rc < 0) {
            perror("bind(IPv6)");
            exit(1);
        }
    }

    /* Configure terminal: disable local echo and line buffering */
    if(tcgetattr(STDIN_FILENO, &term_old)) {
        perror("tcgetattr");
        exit(1);
    }
    term_new = term_old;
    term_new.c_lflag &= ~ECHO;
    term_new.c_lflag &= ~ICANON;
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_new)) {
        perror("tcsetattr");
        exit(1);
    }

    /* Init the dht. */
    rc = dht_init(s, s6, myid, (unsigned char*)"JC\0\0");
    if(rc < 0) {
        perror("dht_init");
        exit(1);
    }

    /* Setup signal handlers */
    init_signals();

    /* Print help */
    print_help();

    /* For bootstrapping, we need an initial list of nodes.  This could be
       hard-wired, but can also be obtained from the nodes key of a torrent
       file, or from the PORT bittorrent message.

       Dht_ping_node is the brutal way of bootstrapping -- it actually
       sends a message to the peer.  If you know the nodes' ids, it is
       better to use dht_insert_node. */
    for(i = 0; i < num_bootstrap_nodes; i++) {
        socklen_t salen;
        if(bootstrap_nodes[i].ss_family == AF_INET)
            salen = sizeof(struct sockaddr_in);
        else
            salen = sizeof(struct sockaddr_in6);
        dht_ping_node((struct sockaddr*)&bootstrap_nodes[i], salen);
        /* Don't overload the DHT, or it will drop your nodes. */
        if(i <= 128)
            usleep(random() % 10000);
        else
            usleep(500000 + random() % 400000);
    }

    while(1) {
        struct timeval tv;
        fd_set readfds;
        tv.tv_sec = tosleep;
        tv.tv_usec = random() % 1000000;

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        if(s >= 0)
            FD_SET(s, &readfds);
        if(s6 >= 0)
            FD_SET(s6, &readfds);
        rc = select(s > s6 ? s + 2 : s6 + 2, &readfds, NULL, NULL, &tv);
        if(rc < 0) {
            if(errno != EINTR) {
                perror("select");
                sleep(1);
            }
        }

        if(rc > 0) {
            fromlen = sizeof(from);
            if(s >= 0 && FD_ISSET(s, &readfds))
                rc = recvfrom(s, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr*)&from, &fromlen);
            else if(s6 >= 0 && FD_ISSET(s6, &readfds))
                rc = recvfrom(s6, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr*)&from, &fromlen);
            else if(FD_ISSET(STDIN_FILENO, &readfds)) {
                rc = read(STDIN_FILENO, buf, 1);
                if(rc > 0) {
                    buf[rc] = '\0';
                    switch(buf[0]) {
                        case 'q':
                        case 'e':
                            exiting = 1;
                            break;
                        case 's':
                            print_stats();
                            break;
                        case 'd':
                            dumping = 1;
                            break;
                        case 't':
                            printf("\e[1mSearching for hash '"); print_hex(hash, sizeof(hash)); printf("'\e[0m\n");
                            searching = 1;
                            break;
                        case 'h':
                            print_help();
                            break;
                    }
                    rc=0; // make sure keyboard input is not passed to dht_periodic
                }
            } else {
                abort();
            }
        }

        if(exiting)
            break;

        if(rc > 0) {
            buf[rc] = '\0';
            rc = dht_periodic(buf, rc, (struct sockaddr*)&from, fromlen,
                              &tosleep, callback, NULL);
        } else {
            rc = dht_periodic(NULL, 0, NULL, 0, &tosleep, callback, NULL);
        }
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            } else {
                perror("dht_periodic");
                if(rc == EINVAL || rc == EFAULT)
                    abort();
                tosleep = 1;
            }
        }

        /* This is how you trigger a search for a torrent hash.  If port
           (the second argument) is non-zero, it also performs an announce.
           Since peers expire announced data after 30 minutes, it is a good
           idea to reannounce every 28 minutes or so. */
        if(searching) {
            if(s >= 0)
                dht_search(hash, 0, AF_INET, callback, NULL);
            if(s6 >= 0)
                dht_search(hash, 0, AF_INET6, callback, NULL);
            searching = 0;
        }

        /* For debugging, or idle curiosity. */
        if(dumping) {
            printf("\e[1m");
            dht_dump_tables(stdout);
            printf("\e[0m");
            dumping = 0;
        }
    }

    /* Print statistics */
    print_stats();

    /* Restore previous terminal settings */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_old);

    dht_uninit();
    return 0;

 usage:
    printf("Usage: dht-example [-q] [-4] [-6] [-i filename] [-b address]...\n"
           "                   port [address port]...\n");
    exit(1);
}

/* Functions called by the DHT. */

int
dht_sendto(int sockfd, const void *buf, int len, int flags,
           const struct sockaddr *to, int tolen)
{
    return sendto(sockfd, buf, len, flags, to, tolen);
}

int
dht_blacklisted(const struct sockaddr *sa, int salen)
{
    return 0;
}

/* We need to provide a reasonably strong cryptographic hashing function.
   Here's how we'd do it if we had RSA's MD5 code. */
#if 0
void
dht_hash(void *hash_return, int hash_size,
         const void *v1, int len1,
         const void *v2, int len2,
         const void *v3, int len3)
{
    static MD5_CTX ctx;
    MD5Init(&ctx);
    MD5Update(&ctx, v1, len1);
    MD5Update(&ctx, v2, len2);
    MD5Update(&ctx, v3, len3);
    MD5Final(&ctx);
    if(hash_size > 16)
        memset((char*)hash_return + 16, 0, hash_size - 16);
    memcpy(hash_return, ctx.digest, hash_size > 16 ? 16 : hash_size);
}
#else
/* But for this toy example, we might as well use something weaker. */
void
dht_hash(void *hash_return, int hash_size,
         const void *v1, int len1,
         const void *v2, int len2,
         const void *v3, int len3)
{
    const char *c1 = v1, *c2 = v2, *c3 = v3;
    char key[9];                /* crypt is limited to 8 characters */
    int i;

    memset(key, 0, 9);
#define CRYPT_HAPPY(c) ((c % 0x60) + 0x20)

    for(i = 0; i < 2 && i < len1; i++)
        key[i] = CRYPT_HAPPY(c1[i]);
    for(i = 0; i < 4 && i < len1; i++)
        key[2 + i] = CRYPT_HAPPY(c2[i]);
    for(i = 0; i < 2 && i < len1; i++)
        key[6 + i] = CRYPT_HAPPY(c3[i]);
    strncpy(hash_return, crypt(key, "jc"), hash_size);
}
#endif

int
dht_random_bytes(void *buf, size_t size)
{
    int fd, rc, save;

    fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0)
        return -1;

    rc = read(fd, buf, size);

    save = errno;
    close(fd);
    errno = save;

    return rc;
}
