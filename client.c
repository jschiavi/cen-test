#include <errno.h>	/* errno */
#include <stdio.h>	/* printf() */
#include <stdint.h>	/* uint32_t */
#include <string.h>	/* memset(), strerror() */
#include <unistd.h>	/* read(), write() */

#include <arpa/inet.h>  /* inet_pton() */

#include <sys/socket.h> /* socket() */
#include <sys/uio.h>	/* read(), write() */
#include <sys/types.h>	/* read(), write() */

#include "limits.h"
#include "game.h"
#include "move.h"

static void print_buffer(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		printf("%d ", buf[i]);
	}
	printf("\n");
}

static struct tile deserialize_tile(uint8_t *buf)
{
	enum edge edges[5];
	for (size_t i = 0; i < 5; ++i) {
		edges[i] = buf[i];
	}
	enum attribute a = buf[6];
	return make_tile(edges, a);
}

static struct move deserialize_move(uint8_t *buf)
{
	struct tile t = deserialize_tile(buf);
	return make_move(t, make_slot(buf[7], buf[8]), buf[9]);
}

static struct sockaddr_in make_sockaddr_in_port(int port)
{
	struct sockaddr_in s;
	memset(&s, '0', sizeof(s));
	s.sin_family = AF_INET;
	s.sin_port = port;
	return s;
}

static int retry_connection(int sock, struct sockaddr *a, size_t asize, int cnt)
{
	for (int i = 0; i < cnt; ++i) {
		if (!connect(sock, a, asize)) {
			return 0;
		}
		switch (errno) {
		case ECONNREFUSED:
		case EHOSTUNREACH:
		case ENETDOWN:
		case ENETUNREACH:
		case ETIMEDOUT:
		case ECONNRESET:
			break;	/* retry */
		default:	/* can't fix, so bail out */
			return 1;
		}
		sleep(1);
	}
	return 1;
}

static int connect_retry(char *host, int port)
{
	int sockfd;
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("Could not create socket: %s\n", strerror(errno));
		return -1;
	}

	struct sockaddr_in s = make_sockaddr_in_port(port);
	if (!inet_pton(AF_INET, host, &s.sin_addr)) {
		printf("Bad address.\n");
		return -1;
	} 

	if (retry_connection(sockfd, (struct sockaddr *)&s, sizeof(s), 10)) {
		printf("Connection Failure: %s\n", strerror(errno));
		return -1;
	}

	return sockfd;
}

static int connect_game(char *host, int welcome_port)
{
	/* TODO: Better error handling. */
	int sockfd;
	if ((sockfd = connect_retry(host, htons(welcome_port))) < 0) {
		printf("Error: %s\n", strerror(errno));
		return -1;
	} 

	uint16_t port;
	if (read(sockfd, &port, sizeof(port)) < 0) {
		printf("Read error: %s\n", strerror(errno));
		return -1;
	}
	printf("Got port: %u\n", ntohs(port));
	close(sockfd);

	if ((sockfd = connect_retry(host, port)) < 0) {
		printf("Error: %s\n", strerror(errno));
		return -1;
	}

	return sockfd;
}

static int get_clock_and_order(int sockfd, int *first, uint64_t *clock)
{
	unsigned char buf[1 + sizeof(*clock)];
	read(sockfd, buf, sizeof(buf));
	print_buffer(buf, sizeof(buf));
	*first = buf[0];
	*clock = 0;
	for (size_t i = 0; i < sizeof(buf) - 1; ++i) {
		*clock += (buf[i + 1] << (i * 8));
	}
	return 0;
}

static int get_deck(int sockfd, struct tile *deck, size_t clen, size_t dlen)
{
	unsigned char buf[clen];
	for (size_t i = 0; i < dlen; ++i) {
		read(sockfd, buf, clen);
		enum edge edges[5];
		for (size_t j = 0; j < 5; ++j) {
			edges[j] = buf[j];
		}
		enum attribute a = buf[5];
		deck[i] = make_tile(edges, a);
	}
	return 0;
}

static uint8_t *serialize_tile(struct tile t, uint8_t *buf)
{
	for (size_t i = 0; i < 5; ++i) {
		buf[i] = t.edges[i];
	}
	buf[6] = t.attribute;
	return &buf[7];
}

static uint8_t *serialize_move(struct move m, uint8_t *buf)
{
	buf = serialize_tile(m.tile, buf);
	uint8_t x = m.slot.x, y = m.slot.y;
	uint8_t rotation = m.rotation;
	*buf++ = x;
	*buf++ = y;
	*buf++ = rotation;
	return buf;
}

#define REMOTE_HOST "127.0.0.1" /* TODO: Get a command line variable. */
#define REMOTE_PORT 5000 /* TODO: Factor into command line variable. */

int main(void)
{
	int sockfd;
	if ((sockfd = connect_game(REMOTE_HOST, REMOTE_PORT)) < 0) {
		printf("Error: %s\n", strerror(errno));
		return 1;
	}
	printf("Successfully connected.\n");

	int first;
	uint64_t move_clock;
	get_clock_and_order(sockfd, &first, &move_clock);
	printf("Clock: %llu\n", move_clock);
	if (first) {
		printf("I'm first!\n");
	} else {
		printf("I'm second!\n");
	}

	struct game *g = malloc(sizeof(*g)); /* TODO: Refactor? */
	struct tile *tileset = malloc(sizeof(*tileset) * TILE_COUNT);
	get_deck(sockfd, tileset, TILE_SZ, TILE_COUNT);
	make_game_with_deck(g, tileset);
	free(tileset);

	unsigned char buf[16];
	int rc = 0;
	/* 0 is us, opponent is 1. */
	while ((rc = read(sockfd, buf, sizeof(buf))) == sizeof(buf)) {
		printf("Recieved: ");
		print_buffer(buf, sizeof(buf));
		if (buf[0]) { /* game over. */
			if (buf[1]) {
				printf("I won!\n");
			} else {
				printf("I lost!\n");
			}
			break;
		}
		/* Deserialize tile and move. */
		printf("Got a move!\n");
		unsigned char b[100];
		struct tile t = deserialize_tile(&buf[1]);
		printf("Tile: %s\n", print_tile(t, b));
		int mid = (AXIS - 1) / 2;
		struct move m = make_move(t, make_slot(mid, mid), 0);
		play_move(g, m, 0);
		serialize_move(m, buf);
		printf("Try playing the center.\n");
		write(sockfd, buf, sizeof(buf));
		if (first) {
			first = 0;
			continue;
		}
		struct move prev = deserialize_move(&buf[8]);
		printf("Prev move | x: %d y: %d: rotation: %d \n%s\n",
				prev.slot.x, prev.slot.y, prev.rotation,
				print_tile(prev.tile, b));
		play_move(g, prev, 1);
	}

	printf("DEBUG: %d\n", rc);
#if 0
	for (int i = 0; i < TILE_COUNT; ++i) {
		printf("%s\n", print_tile(deal_tile(g), buf));
	}
#endif

	close(sockfd);
	free(g);
	return 0;
}