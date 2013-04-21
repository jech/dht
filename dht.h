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

typedef void
dht_callback(void *closure, int event,
             unsigned char *info_hash,
             void *data, size_t data_len);

#define DHT_EVENT_NONE 0
#define DHT_EVENT_VALUES 1
#define DHT_EVENT_VALUES6 2
#define DHT_EVENT_SEARCH_DONE 3
#define DHT_EVENT_SEARCH_DONE6 4

extern FILE *dht_debug;

/**
 * Begins a DHT session.
 * 
 * This library only supports one simultaneous session.
 * 
 * Any sockets specified will be set to non-blocking mode automatically.
 * 
 * @param s         Bound IPv4 datagram socket. Or -1 to not listen over IPv4.
 * @param s6        Bound IPv6 datagram socket. Or -1 to not listen over IPv6.
 * @param id        Your node ID, a random 20-byte array that should ideally be
 *                  persisted across program runs.
 * @param v         User agent FourCC for this client. Can be NULL to omit.
 * @return          (?)
 */
int dht_init(int s, int s6, const unsigned char *id, const unsigned char *v);
/**
 * Adds an entry directly to the routing table.
 * 
 * For various reasons, a Kademlia routing table cannot absorb nodes faster than a certain
 * rate.  Dumping a large number of nodes into a table using dht_insert_node
 * will probably cause most of these nodes to be discarded straight away.
 * (The tolerable rate is difficult to estimate; it is probably on the order
 * of one node every few seconds per node already in the table divided by 8,
 * for some suitable value of 8.)
 * 
 * @param id        ID of the node to add.
 * @param sa        Address of the node to add.
 * @param salen     Size of `sa` structure.
 * @return          (?)
 */
int dht_insert_node(const unsigned char *id, struct sockaddr *sa, int salen);
/**
 * Pings the specified address where a node is believed to live.
 * If it replies, it will be inserted into the routing table
 * (assuming there is enough space).
 * 
 * @param sa        Address of the prospective node to ping.
 * @param salen     Size of `sa` structure.
 * @return          (?)
 */
int dht_ping_node(struct sockaddr *sa, int salen);
/**
 * Maintains the DHT session.
 * 
 * Should be called by your main loop periodically, and also
 * whenever data is available on the socket.  The time after which
 * dht_periodic should be called if no data is available is returned in the
 * parameter tosleep.
 * 
 * @param buf       Data received from the socket, or NULL if no data received.
 * @param buflen    Size of the `buf` structure, or 0 if no data received.
 * @param from      The socket from which data was received, or NULL if no data received.
 * @param fromlen   Size of the `from` structure, or 0 if no data received.
 * @param tosleep   OUT: The time after which this function should be called
 *                  again if no data is available on the session's socket.
 * @param callback  Callback that will be invoked if a search result
 *                  is available.
 * @param closure   Passed as an argument to `callback`.
 * @return          (?)
 */
int dht_periodic(const void *buf, size_t buflen,
                 const struct sockaddr *from, int fromlen,
                 time_t *tosleep, dht_callback *callback, void *closure);
/**
 * Schedules a search for information about the specified info-hash.
 * 
 * If you specify a new search for the same info hash as a search still in
 * progress, the previous search is combined with the new one --
 * you will only receive a completion indication once.
 * 
 * Up to DHT_MAX_SEARCHES (1024) searches can be in progress at a given time;
 * any more, and dht_search will return -1.
 * 
 * @param id        The info-hash to search for.
 * @param port      The TCP port on which the current peer is listening,
 *                  or 0 to ignore. In host byte-order (not network byte-order).
 * @param af        (?) The session socket on which to schedule to search.
 *                  Either AF_INET or AF_INET6.
 * @param callback  Callback that will be invoked with data as it becomes
 *                  available, and additionally when the search is complete.
 * @return          (?)
 */
int dht_search(const unsigned char *id, int port, int af,
               dht_callback *callback, void *closure);
/**
 * Returns the number of known good, dubious and cached nodes in the
 * routing table.
 *
 * This can be used to decide whether it's reasonable to start a search;
 * a search is likely to be successful as long as we have a few good nodes;
 * however, in order to avoid overloading your bootstrap nodes, you may want to
 * wait until good is at least 4 and good + doubtful is at least 30 or so.
 * 
 * If you want to display a single figure to the user, you should display
 * good + doubtful, which is the total number of nodes in your routing table.
 * 
 * @param af        (?) The session socket whose nodes should be returned.
 *                  Either AF_INET or AF_INET6.
 * @param good_return
 *                  OUT: The number of good nodes in the routine table.
 * @param dubious_return
 *                  OUT: The number of dubious nodes in the routine table.
 * @param cached_return
 *                  OUT: The number of cached nodes in the routine table.
 * @param incoming_return
 *                  OUT: The number of nodes that recently sent us an
 *                  unsolicited request. This can be used to determine if the
 *                  UDP port used for the DHT is firewalled.
 * @return          (?)
 */
int dht_nodes(int af,
              int *good_return, int *dubious_return, int *cached_return,
              int *incoming_return);
void dht_dump_tables(FILE *f);
/**
 * Returns the list of known good nodes, starting with the nodes in our
 * own bucket.
 * 
 * It is a good idea to save the list of known good nodes at shutdown,
 * and ping them at startup.
 * 
 * @param sin       OUT: Addresses of the good nodes found on the IPv4 socket.
 *                  Must be allocated by the caller with sufficient space.
 *                  Use dht_nodes() to determine the minimum space required.
 * @param num       OUT: The number of elements returned in `sin`.
 * @param sin6      OUT: Addresses of the good nodes found on the IPv6 socket.
 *                  Must be allocated by the caller with sufficient space.
 *                  Use dht_nodes() to determine the minimum space required.
 * @param num6      OUT: The number of elements returned in `sin6`.
 */
int dht_get_nodes(struct sockaddr_in *sin, int *num,
                  struct sockaddr_in6 *sin6, int *num6);
/**
 * Ends the current DHT session.
 */
int dht_uninit(void);

/* This must be provided by the user. */
int dht_blacklisted(const struct sockaddr *sa, int salen);
void dht_hash(void *hash_return, int hash_size,
              const void *v1, int len1,
              const void *v2, int len2,
              const void *v3, int len3);
int dht_random_bytes(void *buf, size_t size);
