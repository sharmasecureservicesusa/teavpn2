
#ifndef __TEAVPN2__CLIENT__LINUX__TCP_PACKET_H
#define __TEAVPN2__CLIENT__LINUX__TCP_PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <teavpn2/auth.h>
#include <teavpn2/__base.h>
#include <teavpn2/client/linux/tcp_packet.h>


typedef enum __attribute__((packed)) _cli_tcp_pkt_type {
	CLI_PKT_HELLO		= 0,
	CLI_PKT_AUTH		= 1,
	CLI_PKT_IFACE_ACK	= 2,
	CLI_PKT_IFACE_FAIL	= 3,
	CLI_PKT_IFACE_DATA	= 4,
	CLI_PKT_REQSYNC		= 5,
	CLI_PKT_CLOSE		= 6,
} cli_tcp_pkt_type;


struct cli_tcp_pkt {
	cli_tcp_pkt_type		type;
	uint8_t				pad_n;
	uint16_t			length;
	union {
		char			raw_data[4096];
		struct auth_pkt		auth;

		struct {
			char		__dummy0[4095];
			uint8_t		__end;
		};
	};
};


typedef union _cli_tcp_pkt_buf {
	struct cli_tcp_pkt		pkt;
	struct cli_tcp_pkt		__pkt_chk[4];
	char				raw[sizeof(struct cli_tcp_pkt) * 4];
} cli_tcp_pkt_buf;


/* enum _cli_tcp_pkt_type */
STATIC_ASSERT(
	sizeof(enum _cli_tcp_pkt_type) == 1,
	"Bad sizeof(enum _cli_tcp_pkt_type)"
);


/* struct cli_tcp_pkt */
STATIC_ASSERT(
	sizeof(struct cli_tcp_pkt) == (
		1	/* type   */
		+ 1	/* pad    */
		+ 2	/* length */
		+ 4096	/* data   */
	),
	"Bad sizeof(struct cli_tcp_pkt)"
);
STATIC_ASSERT(
	offsetof(struct cli_tcp_pkt, type) == 0,
	"Bad offsetof(struct cli_tcp_pkt, type)"
);
STATIC_ASSERT(
	offsetof(struct cli_tcp_pkt, pad_n) == 1,
	"Bad offsetof(struct cli_tcp_pkt, pad_n)"
);
STATIC_ASSERT(
	offsetof(struct cli_tcp_pkt, length) == 2,
	"Bad offsetof(struct cli_tcp_pkt, length)"
);
STATIC_ASSERT(
	offsetof(struct cli_tcp_pkt, raw_data) == 4,
	"Bad offsetof(struct cli_tcp_pkt, raw_data)"
);
STATIC_ASSERT(
	offsetof(struct cli_tcp_pkt, auth) == 4,
	"Bad offsetof(struct cli_tcp_pkt, auth)"
);
STATIC_ASSERT(
	offsetof(struct cli_tcp_pkt, __dummy0) == 4,
	"Bad offsetof(struct cli_tcp_pkt, __dummy0)"
);
STATIC_ASSERT(
	offsetof(struct cli_tcp_pkt, __end) == 4 + 4095,
	"Bad offsetof(struct cli_tcp_pkt, __end)"
);


/* union _cli_tcp_pkt_buf */
STATIC_ASSERT(
	sizeof(union _cli_tcp_pkt_buf) == (sizeof(struct cli_tcp_pkt) * 4),
	"Bad sizeof(union _cli_tcp_pkt_buf)"
);



#define CLI_MIN_PKTL  (offsetof(struct cli_tcp_pkt, raw_data[0]))
#define CLI_PKT_RECVL (sizeof(union _cli_tcp_pkt_buf))


STATIC_ASSERT(
	CLI_MIN_PKTL == 4,
	"Bad value of CLI_MIN_PKTL"
);

#endif /* #ifndef __TEAVPN2__CLIENT__LINUX__TCP_PACKET_H */
