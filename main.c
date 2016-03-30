/*
    event-chat : Simple event-based chat server; you can use telnet for client.
    Copyright (C) 2016  Laurent Parenteau

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <assert.h>

// for better branch predictions
#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

// socket limits and default
#define DEFAULT_PORT	54321
#define MAX_SOCKETS	65526
#define MAX_BACKLOG	(MAX_SOCKETS << 1)
#define MAX_EVENTS	MAX_SOCKETS

// room limits
#define MAX_ROOMNAME	32
#define MAX_MEMBERS	512

// other constants, not limits.  Can be tuned.
#define NB_ROOM_SLOTS	0xffff
#define BUF_LENGTH	1024

typedef struct member {
	int fd;
} member;

typedef struct chatroom {
	char name[MAX_ROOMNAME+1];
	struct member members[MAX_MEMBERS];
	struct chatroom *next;
	int count;
	unsigned short key;
} chatroom;

chatroom rooms[NB_ROOM_SLOTS];
chatroom *fds[MAX_SOCKETS];
unsigned int port;

// helpers for arguments
static int parse_arguments(int argc, char **argv);
static void usage();

// helpers for sockets
static void set_socket_options(const int fd);
static void remove_socket(const int fd);

// helpers for rooms
static void remove_from_room(const int fd, chatroom *room);
static void add_to_room(const int fd, chatroom *room);
static chatroom *get_chatroom(const char *name, const unsigned int len);
static void write_to_room(const char *buf, const int len, const chatroom *room, const int source_fd);

// helpers for hash.  FNV-1, 16 bit hash.  See http://www.isthe.com/chongo/tech/comp/fnv/
#define MASK_16		(((u_int32_t)1<<16)-1)
#define FNV1_32_INIT	((u_int32_t)2166136261)
static u_int32_t fnv_32_str(const char *str, u_int32_t hval);
static unsigned short hash(const char *name);

//
// implementation
//

int main(int argc, char **argv)
{
	//
	// handle arguments
	//

	if (parse_arguments(argc, argv)) {
		usage();
		return 1;
	}
	printf("Listening on %d\n", port);

	//
	// setup listening socket
	//

	int listen_fd;
	if ((listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0) {
		printf("Failed to create listening socket (%d) (%s)\n", errno, strerror(errno));
		return 1;
	}
	int one = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &one, sizeof(one));

	struct sockaddr_in my_addr;
	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	my_addr.sin_port = htons(port);
	
	if (bind(listen_fd, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
		printf("Failed to bind listening socket (%d) (%s)\n", errno, strerror(errno));
		close(listen_fd);
		return 1;
	}
	
	if (listen(listen_fd, MAX_BACKLOG) < 0) {
		printf("Failed to bind listening socket (%d) (%s)\n", errno, strerror(errno));
		close(listen_fd);
		return 1;
	}

	//
	// setup epoll
	//
	
	int epoll_fd;
	if ((epoll_fd = epoll_create(1)) < 0) {
		printf("Failed to create epoll fd (%d) (%s)\n", errno, strerror(errno));
		close(listen_fd);
		return 1;
	}

	// we start with just the listening socket in
	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.data.fd = listen_fd;
	event.events = EPOLLIN | EPOLLET;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) < 0) {
		printf("Failed to add listen fd to epoll (%d) (%s)\n", errno, strerror(errno));
		close(listen_fd);
		close(epoll_fd);
		return 1;
	}

	struct epoll_event *events;
	events = calloc(MAX_EVENTS, sizeof(struct epoll_event));

	//
	// initialize rooms and fd array
	//

	memset(&rooms, 0, sizeof(rooms));	
	memset(&fds, 0, sizeof(fds));	

	//
	// event loop
	//

	for (;;) {
		int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
		for (int cur = 0; cur < n; cur++) {
			// special case for listening fd.
			if (unlikely((events[cur].data.fd == listen_fd))) {
				// some new connections to accept
				if (likely(events[cur].events & EPOLLIN)) {
					// accept all pending connections.
					while (1) {
						struct sockaddr in_addr;
						socklen_t in_len;
						int fd;
						in_len = sizeof(in_addr);

						if ((fd = accept(listen_fd, &in_addr, &in_len)) < 0 ) {
							break;
						}

						// Set proper socket option and check for read ready.  Not associated with any room yet.
						set_socket_options(fd);
						event.data.fd = fd;
						event.events = EPOLLIN | EPOLLET;
						if (unlikely(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)) {
							printf("Failed to add fd to epoll (%d) (%s)\n", errno, strerror(errno));
							close(fd);
						}
					}
				// if there's an error on that socket, then shutdown everything.
				} else {
					printf("Error with listening socket, shutting down\n");
					close(listen_fd);
					close(epoll_fd);
					free(events);
					return 1;
				}
				// rest is for non-listening sockets, so we need to loop already.
				continue;
			}

			// we can read from a client socket.
			if (events[cur].events & EPOLLIN) {
				// read all pending data.
				while(1) {
					char buf[BUF_LENGTH];
					ssize_t count;
					count = read(events[cur].data.fd, buf, sizeof(buf));
					if (count > 0) {
						chatroom *room;
						char *ptr;
						// if there's a room already associated, when we'll just have to write the data to the other members.
						if (fds[events[cur].data.fd]) {
							room = fds[events[cur].data.fd];
							ptr = buf;
						} else {
							int i;
							// figure out the requested room's name.
							for (i = 0; i < ((MAX_ROOMNAME < count)?MAX_ROOMNAME:count)-2; i++) {
								// Telnet end of line is CR LF or CR NUL.  See RFC1123 'Telnet End-of-Line Convention'
								if ((buf[i] == '\r') && ((buf[i+1] == '\n') || (buf[i+1] == '\0')))
									break;
							}
							buf[i] = '\0';

							// get the associated chat room
							room = get_chatroom(buf, i);

							// if the room isn't full, then associate the socket with that room, and remainig of data read needs to be send.
							if (room->count < MAX_MEMBERS) {
								add_to_room(events[cur].data.fd, room);
								if (i+2 < count) {
									ptr = buf+i+2;
									count -= (i+2);
								} else {
									continue;
								}

							// room is full, close the door.
							} else {
								close(events[cur].data.fd);
								break;
							}
						}
						write_to_room(ptr, count, room, events[cur].data.fd);

					// if there's just no more data, then move to another event.
					// if there's an error reading on that socket, then close and clean it.
					} else {
						if ((count < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
							break;
						remove_socket(events[cur].data.fd);
						break;
					}
				}
			}

			// if there's an error on the accepted socket, terminate that connection.
			if (unlikely((events[cur].events & EPOLLERR) || (events[cur].events & EPOLLHUP))) {
				remove_socket(events[cur].data.fd);
			}
		}		
	}

	//
	// we actually never reach this point...
	//

	assert(0);

	return 0;
}

/*
 * Parse arguments from command line.
 *
 * Return 1 if invalid arguments found, 0 otherwise.
 */
static int parse_arguments(int argc, char **argv)
{
	int opt;

	port = DEFAULT_PORT;

	while ((opt = getopt(argc, argv, "p:")) != -1) {
		switch (opt) {
		case 'p' :
			port = atoi(optarg);
			break;
		default :
			return 1;
		}
	}

	if ((port < 1) || (port > 65535)) {
		printf("invalid port value : %d\n", port);
		return 1;
	}

	return 0;
}

/*
 * Print program usage.
 */
static void usage()
{
	printf("arguments : \n");
	printf("  -p [1-65535]     port to listen on [default %d]\n", DEFAULT_PORT);
}

/*
 * Setup usual socket options on new sockets we get from accept().
 */
static void set_socket_options(const int fd)
{
	int flags;

	// non blocking
	flags = fcntl(fd, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);

	// disable Nagle algorithm
	flags = 1;
	setsockopt(fd, SOL_TCP, TCP_NODELAY, &flags, sizeof(flags));
}

/*
 * Remove a (closed) socket from its chatroom.
 *
 * If this was the last member of that chatroom, it will also cleanup the
 * room (always leave the campground cleaner than you found it).
 */
static void remove_from_room(const int fd, chatroom *room)
{
	// No more rooms associated with that fd.
	fds[fd] = NULL;

	// Remove the fd from the room.
	for (int i = 0; i < room->count; i++) {
		if (room->members[i].fd == fd) {
			if (likely(i < room->count - 1)) {
				memmove(&(room->members[i]), &(room->members[i+1]), sizeof(room->members[i]) * (room->count - i - 1));
			} else {
				memset(&(room->members[i]), 0, sizeof(room->members[i]));
			}
			room->count--;
			break;
		}
	}

	// Chat room now empty.  Remove it if dynamically allocated.
	if (room->count == 0) {
		chatroom *cur = &rooms[room->key];
		// This is the static head.
		if (cur == room) {
			if (cur->next != NULL) {
				// We have some rooms dynamically allocated, so move the next one into the static
				// array and release the related dynamic memory.
				cur = cur->next;
				memmove(&rooms[room->key], cur, sizeof(cur));
				free(cur);

			// Not dynamic rooms in that slot, just memset.
			} else {
				memset(cur, 0, sizeof(cur));
			}

		// This is in the linked list.
		} else {
			while (cur) {
				if (cur->next == room) {
					cur->next = room->next;
					free(room);
					break;
				}
				cur = cur->next;
			}
		}
	}
}

/*
 * Add a socket to its chatroom.
 */
static void add_to_room(const int fd, chatroom *room)
{
	fds[fd] = room;
	room->members[room->count].fd = fd;
	room->count++;
}

/*
 * Write what source_fd said to everyone else in the same chatroom.
 */
static void write_to_room(const char *buf, const int len, const chatroom *room, const int source_fd)
{
	if (buf && len) {
		for (int i = 0; i < room->count; i++) {
			if (room->members[i].fd != source_fd) {
				write(room->members[i].fd, buf, len);
			}
		}
	}
}

/*
 * This is FNV-1a, a 32 bit hash efficient for strings.  See http://www.isthe.com/chongo/tech/comp/fnv/
 */
static u_int32_t fnv_32_str(const char *str, u_int32_t hval)
{
    unsigned char *s = (unsigned char *)str;

    while (*s) {
	hval ^= (u_int32_t)*s++;
	hval += (hval<<1) + (hval<<4) + (hval<<7) + (hval<<8) + (hval<<24);
    }

    return hval;
}

/*
 * Hash a room name, for quick lookup.
 */
static unsigned short hash(const char *name)
{
	u_int32_t hash;

	hash = fnv_32_str(name, FNV1_32_INIT);
	// Turn the 32 bit hash value into a 16 bit value (our maximum number of fd).
	hash = (hash>>16) ^ (hash & MASK_16);

	return (unsigned short)hash;
}

/*
 * Get the named chatroom.
 *
 * The supplied name is actually null-terminated, but the length is passed as well since we know it
 * so we can re-use the value when needed.
 *
 * If the room was empty, set it up.
 * In the unlikely event of hash collision, then it allocate a new room (tracked by a linked list).
 */
static chatroom *get_chatroom(const char *name, const unsigned int len)
{
	unsigned short key=hash(name);
	chatroom *room = &rooms[key];
	chatroom *prev = NULL;

	// check all rooms of that key, in case it already exist or the static slot is not yet used.
	while (room) {
		if ((strlen(room->name) == 0) || (strcmp(name, room->name) == 0)) {
			break;
		}
		prev = room;
		room = room->next;
	}

	// static slot was used an no room of that name, so allocate a new one.
	if (room == NULL) {
		room = calloc(1, sizeof(room));
		if (prev) prev->next = room;
	}

	// room is empty, setup it up
	if (room->count == 0) {
		memcpy(room->name, name, len+1);
		room->key = key;
	}

	return room;
}

/*
 * Cleanup an accepted socket.
 *
 * We close it, and remove the room association if any.
 */
static void remove_socket(const int fd)
{
	close(fd);
	if (fds[fd] != NULL) {
		remove_from_room(fd, fds[fd]);
		fds[fd] = NULL;
	}
}

