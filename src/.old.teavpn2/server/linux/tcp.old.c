
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <teavpn2/server/auth.h>
#include <teavpn2/net/linux/iface.h>
#include <teavpn2/server/linux/tcp.h>
#include <teavpn2/client/linux/tcp.h>


typedef enum {
	CT_NEW			= 0,
	CT_ESTABLISHED		= 1,
	CT_AUTHENTICATED	= 2,
	CT_DISCONNECTED		= 3,
} srv_tcp_ctstate;

struct srv_tcp_client {
	bool			is_used;
	bool			is_conn;
	bool			is_auth;
	srv_tcp_ctstate		ctstate;

	char			username[255];
	char			src_ip[IPV4LEN + 1];
	uint16_t		src_port;
	struct sockaddr_in	src_data;

	int			cli_fd;
	uint16_t		arr_idx;
	uint8_t			err_c;
	uint32_t		send_c;
	uint32_t		recv_c;
	uint16_t		recv_s;

	union {
		char			recv_buf[sizeof(struct cli_tcp_pkt)];
		struct cli_tcp_pkt	cli_pkt;	
	};
};

struct _fds_stack {
	uint16_t		sp;
	uint16_t		max_sp;
	uint16_t		*arr;
};

struct _auth_stack {
	uint16_t		sp;
	uint16_t		max_sp;
	uint16_t		*arr;
};

struct srv_tcp_state {
	int			net_fd;
	int			tun_fd;
	int			pipe_fd[2];
	nfds_t			nfds;
	struct pollfd		*fds;
	struct srv_cfg		*cfg;
	struct srv_tcp_client	*clients;
	struct _fds_stack	fds_stack;
	bool			stop;
	union {
		char			send_buf[sizeof(struct srv_tcp_pkt)];
		struct srv_tcp_pkt	srv_pkt;
	};
};


/* Ah, just short macros for printing... */
#define W_IP(CL) ((CL)->src_ip), ((CL)->src_port)
#define W_UN(CL) ((CL)->username)
#define W_IU(CL) W_IP(CL), W_UN(CL)
#define PRWIU "%s:%d (%s)"


#define MAX_ERR_C (10u)
static struct srv_tcp_state *g_state;


static void intr_handler(int sig)
{
	struct srv_tcp_state *state = g_state;

	state->stop = true;
	putchar('\n');
	(void)sig;
}


static int32_t push_fds(struct _fds_stack *fds_stack, uint16_t val)
{
	uint16_t sp = fds_stack->sp;

	assert(sp > 0);
	fds_stack->arr[--sp] = val;
	fds_stack->sp = sp;
	return (int32_t)val;
}


static int32_t pop_fds(struct _fds_stack *fds_stack)
{
	int32_t val;
	uint16_t sp = fds_stack->sp;
	uint16_t max_sp = fds_stack->max_sp;

	/* sp must never be higher than max_sp */
	assert(sp <= max_sp);

	if (sp == max_sp)
		return -1; /* There is nothing in the stack */

	val = (int32_t)fds_stack->arr[sp];
	fds_stack->sp = ++sp;
	return (int32_t)val;
}


static void init_client_slots(struct srv_tcp_client *clients, uint16_t i)
{
	while (i--) {
		memset(&clients[i], 0, sizeof(struct srv_tcp_client));
		clients[i].cli_fd = -1;
		clients[i].arr_idx = i;
	}
}


static int init_state(struct srv_tcp_state *state)
{
	int tmp;
	struct srv_cfg *cfg = state->cfg;
	uint16_t *stack_arr = NULL;
	uint16_t max_conn = cfg->sock.max_conn;
	struct srv_tcp_client *clients = NULL;
	struct _fds_stack *fds_stack = &state->fds_stack;


	clients = calloc(max_conn, sizeof(struct srv_tcp_client));
	if (unlikely(clients == NULL))
		goto out_err_calloc;

	stack_arr = calloc(max_conn, sizeof(uint16_t));
	if (unlikely(stack_arr == NULL))
		goto out_err_calloc;

	fds_stack->sp = max_conn;
	fds_stack->max_sp = max_conn;
	fds_stack->arr = stack_arr;

	init_client_slots(clients, max_conn);

	while (max_conn--)
		push_fds(fds_stack, max_conn);

	state->net_fd = -1;
	state->tun_fd = -1;
	state->pipe_fd[0] = -1;
	state->pipe_fd[1] = -1;
	state->stop = false;
	state->clients = clients;
	return 0;


out_err_calloc:
	tmp = errno;
	free(clients);
	pr_error("calloc: Cannot allocate memory: %s", strerror(tmp));
	return -ENOMEM;
}


static int init_pipe(struct srv_tcp_state *state)
{
	prl_notice(6, "Initializing pipe...");
	if (unlikely(pipe(state->pipe_fd) < 0)) {
		int tmp = errno;
		pr_error("pipe(): %s", strerror(tmp));
		return -tmp;
	}

	prl_notice(6, "Pipe has been successfully created!");
	prl_notice(6, "state->pipe_fd[0] = %d", state->pipe_fd[0]);
	prl_notice(6, "state->pipe_fd[1] = %d", state->pipe_fd[1]);

	return 0;
}


static void destroy_state(struct srv_tcp_state *state)
{
	int tun_fd = state->tun_fd;
	int net_fd = state->net_fd;
	int *pipe_fd = state->pipe_fd;
	uint16_t max_conn = state->cfg->sock.max_conn;
	struct srv_tcp_client *client;
	struct srv_tcp_client *clients = state->clients;

	if (pipe_fd[0] != -1) {
		prl_notice(6, "Closing state->pipe_fd[0] (%d)", pipe_fd[0]);
		close(pipe_fd[0]);
	}

	if (pipe_fd[1] != -1) {
		prl_notice(6, "Closing state->pipe_fd[1] (%d)", pipe_fd[1]);
		close(pipe_fd[1]);
	}

	if (tun_fd != -1) {
		prl_notice(6, "Closing state->tun_fd (%d)", tun_fd);
		close(tun_fd);
	}

	if (net_fd != -1) {
		prl_notice(6, "Closing state->net_fd (%d)", net_fd);
		close(net_fd);
	}

	while (max_conn--) {
		client = clients + max_conn;
		if (client->is_used) {
			prl_notice(6, "Closing clients[%d].cli_fd (%d)",
				   max_conn, client->cli_fd);
			close(client->cli_fd);
		}
	}

	free(clients);
	free(state->fds_stack.arr);
}


static int init_iface(struct srv_tcp_state *state)
{
	int fd;
	struct iface_cfg i;
	struct srv_iface_cfg *j = &state->cfg->iface;

	prl_notice(3, "Creating virtual network interface: \"%s\"...",j->dev);
	fd = tun_alloc(j->dev, IFF_TUN);
	if (unlikely(fd < 0))
		return -1;
	if (unlikely(fd_set_nonblock(fd) < 0))
		goto out_err;

	memset(&i, 0, sizeof(struct iface_cfg));
	strncpy(i.dev, j->dev, sizeof(i.dev) - 1);
	strncpy(i.ipv4, j->ipv4, sizeof(i.ipv4) - 1);
	strncpy(i.ipv4_netmask, j->ipv4_netmask, sizeof(i.ipv4_netmask) - 1);
	i.mtu = j->mtu;

	if (unlikely(!raise_up_interface(&i)))
		goto out_err;

	state->tun_fd = fd;
	return 0;

out_err:
	close(fd);
	return -1;
}


static int socket_setup(int fd, struct srv_cfg *cfg)
{
	int rv;
	int y = 1;
	socklen_t len = sizeof(y);
	const void *pv = (const void *)&y;

	rv = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	rv = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	(void)cfg;
	return rv;

out_err:
	pr_error("setsockopt(): %s", strerror(errno));
	return rv;
}


static int init_socket(struct srv_tcp_state *state)
{
	int fd;
	int ern;
	int retval;
	struct sockaddr_in srv_addr;
	struct srv_sock_cfg *sock = &state->cfg->sock;
	int backlog = sock->backlog;
	char *bind_addr = sock->bind_addr;
	uint16_t bind_port = sock->bind_port;

	prl_notice(3, "Creating TCP socket...");
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
	if (unlikely(fd < 0)) {
		ern = errno;
		retval = -ern;
		pr_error("socket(): %s", strerror(ern));
		goto out_err;
	}

	prl_notice(3, "Setting up socket file descriptor...");
	retval = socket_setup(fd, state->cfg);
	if (unlikely(retval < 0))
		goto out_err;

	memset(&srv_addr, 0, sizeof(struct sockaddr_in));
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_port = htons(bind_port);
	srv_addr.sin_addr.s_addr = inet_addr(bind_addr);

	retval = bind(fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
	if (unlikely(retval < 0)) {
		ern = errno;
		retval = -ern;
		pr_error("bind(): %s", strerror(ern));
		goto out_err;
	}

	retval = listen(fd, backlog);
	if (unlikely(retval < 0)) {
		ern = errno;
		retval = -ern;
		pr_error("listen(): %s", strerror(ern));
		goto out_err;
	}


	state->net_fd = fd;
	prl_notice(0, "Listening on %s:%d...", bind_addr, bind_port);
	return retval;

out_err:
	if (fd > 0)
		close(fd);
	return retval;
}


static void accept_conn(int net_fd, struct pollfd *clfds,
			struct srv_tcp_state *state)
{
	int rv;
	int ern;
	int32_t ridx;
	uint16_t idx;
	const char *chtmp;
	char src_ip[IPV4LEN];
	uint16_t sport;
	struct pollfd *cltkn;
	struct sockaddr_in claddr;
	struct srv_tcp_client *client;
	socklen_t addrlen = sizeof(struct sockaddr_in);

	memset(&claddr, 0, addrlen);

	rv = accept(net_fd, &claddr, &addrlen);
	if (unlikely(rv < 0)) {
		ern = errno;
		if (ern == EAGAIN)
			return;
		pr_error("accept(): %s", strerror(ern));
		return;
	}

	/* Get readable source IP address */
	chtmp = inet_ntop(AF_INET, &claddr.sin_addr, src_ip, IPV4LEN);
	if (unlikely(chtmp == NULL)) {
		int ern = errno;
		pr_error("inet_ntop(%u): %s", claddr.sin_addr.s_addr,
			 strerror(ern));
		goto out_close;
	}

	/* Get readable source port */
	sport = ntohs(claddr.sin_port);

	ridx = pop_fds(&state->fds_stack);
	if (unlikely(ridx == -1)) {
		prl_notice(1, "Client slot is full, can't accept connection");
		prl_notice(1, "Dropping connection from %s:%d", chtmp, sport);
		goto out_close;
	}


	/* Welcome new connection :) */
	idx = (uint16_t)ridx;

	cltkn = &clfds[idx];
	cltkn->fd = rv;
	cltkn->events = POLLIN;

	client = &state->clients[idx];
	client->is_used = true;
	client->is_conn = true;
	client->is_auth = false;
	client->ctstate = CT_NEW;
	client->cli_fd = rv;
	client->err_c = 0;
	client->recv_c = 0;
	client->recv_s = 0;
	client->username[0] = '_';
	client->username[1] = '\0';

	strncpy(client->src_ip, src_ip, IPV4LEN);
	client->src_port = sport;
	memcpy(&client->src_data, &claddr, sizeof(struct sockaddr_in));

	assert(client->arr_idx == idx);
	prl_notice(1, "New connection from %s:%d", chtmp, sport);
	return;

out_close:
	close(rv);
	return;
}


static void clear_disconnect(struct srv_tcp_client *client)
{
	client->is_used = false;
	client->is_conn = false;
	client->is_auth = false;
	client->recv_s  = 0;
	client->recv_c  = 0;
	client->send_c  = 0;
	client->cli_fd  = -1;
	client->ctstate = CT_DISCONNECTED;
}


static ssize_t send_to_client(struct srv_tcp_client *client,
			     const struct srv_tcp_pkt *srv_pkt,
			     size_t send_len)
{
	ssize_t send_ret;

	send_ret = send(client->cli_fd, srv_pkt, send_len, 0);
	if (unlikely(send_ret < 0)) {
		client->err_c++;
		pr_error("send() to " PRWIU, W_IP(client), client->username);
		return -1;
	}
	client->send_c++;

	prl_notice(11, "[%010" PRIu32 "] send() %ld bytes to " PRWIU,
		   client->send_c, send_ret, W_IU(client));

	return send_ret;
}


static bool send_server_banner(struct srv_tcp_client *client,
			       struct srv_tcp_state *state)
{
	size_t send_len;
	struct srv_tcp_pkt *srv_pkt = &state->srv_pkt;

	srv_pkt->type   = SRV_PKT_BANNER;
	srv_pkt->length = htons(sizeof(struct srv_banner));

	srv_pkt->banner.cur.ver = 0;
	srv_pkt->banner.cur.sub_ver = 0;
	srv_pkt->banner.cur.sub_sub_ver = 1;

	srv_pkt->banner.min.ver = 0;
	srv_pkt->banner.min.sub_ver = 0;
	srv_pkt->banner.min.sub_sub_ver = 1;

	srv_pkt->banner.max.ver = 0;
	srv_pkt->banner.max.sub_ver = 0;
	srv_pkt->banner.max.sub_sub_ver = 1;

	send_len = SRV_PKT_MIN_RSIZ + sizeof(struct srv_banner);
	return send_to_client(client, srv_pkt, send_len) > 0;
}


static bool send_auth_ok(struct srv_tcp_client *client,
			 struct srv_tcp_state *state)
{
	size_t send_len;
	struct srv_tcp_pkt *srv_pkt = &state->srv_pkt;

	srv_pkt->type   = SRV_PKT_AUTH_OK;
	srv_pkt->length = htons(sizeof(struct srv_auth_ok));

	send_len = SRV_PKT_MIN_RSIZ + sizeof(struct srv_auth_ok);
	return send_to_client(client, srv_pkt, send_len) > 0;
}


static bool send_auth_reject(struct srv_tcp_client *client,
			     struct srv_tcp_state *state)
{
	size_t send_len;
	struct srv_tcp_pkt *srv_pkt = &state->srv_pkt;

	srv_pkt->type   = SRV_PKT_AUTH_REJECT;
	srv_pkt->length = 0;

	send_len = SRV_PKT_MIN_RSIZ;
	return send_to_client(client, srv_pkt, send_len) > 0;
}


static void auth_ok_msg(struct iface_cfg *iface, struct srv_tcp_client *client)
{

	prl_notice(0, "Authentication success from " PRWIU, W_IU(client));

	prl_notice(0, "Assign IP %s %s to " PRWIU, iface->ipv4,
		   iface->ipv4_netmask, W_IU(client));
}


static int handle_auth(struct srv_tcp_client *client,
		       struct srv_tcp_state *state)
{
	struct cli_tcp_pkt *cli_pkt = &client->cli_pkt;
	struct auth_pkt *auth = &cli_pkt->auth;
	struct srv_tcp_pkt *srv_pkt = &state->srv_pkt;
	struct srv_auth_ok *auth_ok = &srv_pkt->auth_ok;
	struct iface_cfg *iface = &auth_ok->iface;

	auth->username[sizeof(auth->username) - 1] = '\0';
	auth->password[sizeof(auth->password) - 1] = '\0';

	strncpy(client->username, auth->username, 0xffu - 1u);
	prl_notice(0, "Receive authentication from " PRWIU, W_IU(client));

	if (teavpn_server_get_auth(iface, auth, state->cfg)) {
		if (send_auth_ok(client, state)) {
			auth_ok_msg(iface, client);
			client->is_auth = true;
			client->ctstate = CT_AUTHENTICATED;
			return true;
		} else {
			prl_notice(0, "Authentication error from " PRWIU,
				   W_IU(client));
			goto out_fail;
		}
	}

	prl_notice(0, "Authentication failed from " PRWIU, W_IU(client));

out_fail:
	send_auth_reject(client, state);
	return false;
}


static bool handle_iface_write(struct srv_tcp_state *state,
			       struct cli_tcp_pkt *cli_pkt,
			       uint16_t fdata_len)
{
	ssize_t write_ret;
	int tun_fd = state->tun_fd;

	write_ret = write(tun_fd, cli_pkt->raw_data, fdata_len);
	if (write_ret < 0) {
		pr_error("write(): %s", strerror(errno));
		return false;
	}
	prl_notice(11, "write() %ld bytes to tun_fd", write_ret);

	return true;
}


static void handle_iface_read(int tun_fd, struct srv_tcp_state *state)
{
	int ern;
	size_t send_len;
	ssize_t read_ret;
	uint16_t max_conn = state->cfg->sock.max_conn;
	struct srv_tcp_client *clients = state->clients;
	struct srv_tcp_pkt *srv_pkt = &state->srv_pkt;
	char *buf = srv_pkt->raw_data;

	read_ret = read(tun_fd, buf, 4096);
	if (read_ret < 0) {
		ern = errno;
		if (ern == EAGAIN)
			return;

		state->stop = true;
		pr_error("read(tun_fd): %s", strerror(ern));
		return;
	}

	srv_pkt->type   = SRV_PKT_DATA;
	srv_pkt->length = htons((uint16_t)read_ret);

	send_len = SRV_PKT_MIN_RSIZ + (uint16_t)read_ret;
	while (max_conn--) {
		struct srv_tcp_client *client = clients + max_conn;

		if (client->ctstate == CT_AUTHENTICATED) {
			send_to_client(client, srv_pkt, send_len);
		}
	}
}


static void handle_client(struct pollfd *cl, struct srv_tcp_state *state,
			  uint16_t i)
{
	int ern;
	char *recv_buf;
	size_t recv_s;
	size_t recv_len;
	ssize_t recv_ret;
	struct cli_tcp_pkt *cli_pkt;
	struct srv_tcp_client *client = &state->clients[i];
	uint16_t fdata_len; /* Full data length             */
	uint16_t cdata_len; /* Current received data length */

	client   = &state->clients[i];
	cli_pkt  = &client->cli_pkt;
	recv_s   = client->recv_s;
	recv_buf = client->recv_buf;

	recv_len = CLI_PKT_RSIZE - recv_s;
	recv_ret = recv(cl->fd, recv_buf + recv_s, recv_len, 0);
	client->recv_c++;
	if (unlikely(recv_ret < 0)) {
		ern = errno;
		if (ern == EAGAIN)
			return;
		pr_error("recv(): %s " PRWIU, strerror(ern), W_IU(client));
		goto out_err_c;
	}

	if (unlikely(recv_ret == 0)) {
		prl_notice(3, PRWIU " has closed its connection", W_IU(client));
		goto out_close_conn;
	}

	recv_s += (size_t)recv_ret;

	prl_notice(15, "[%010" PRIu32 "] recv() %ld bytes from " PRWIU
		       " (recv_s = %zu)", client->recv_c, recv_ret,
		       W_IU(client), recv_s);

back_chk:
	if (unlikely(recv_s < CLI_PKT_MIN_RSIZ)) {
		/*
		 * We haven't received the type and length of packet.
		 * It very unlikely happens, maybe connection is too
		 * slow or the extra data after memmove (?)
		 */
		goto out_save_recv_s;
	}


	fdata_len = ntohs(cli_pkt->length);
	if (unlikely(fdata_len > CLI_PKT_DATA_SIZ)) {
		/*
		 * fdata_length must never be greater than SRV_PKT_DATA_SIZ.
		 * Corrupted packet?
		 */
		prl_notice(1, "Client " PRWIU " sends invalid packet length "
			      "(max_allowed_len = %zu; srv_pkt->length = %u; "
			      "recv_s = %zu) CORRUPTED PACKET?", W_IU(client),
			      SRV_PKT_DATA_SIZ, fdata_len, recv_s);
		goto out_err_c;
	}


	/* Calculate current data length */
	cdata_len = recv_s - CLI_PKT_MIN_RSIZ;
	if (unlikely(cdata_len < fdata_len)) {
		/*
		 * We've received the type and length of packet.
		 *
		 * However, the packet has not been fully received.
		 * So let's wait for the next cycle to process it.
		 */
		goto out_save_recv_s;
	}


	prl_notice(15, "==== Process the packet " PRWIU, W_IU(client));

	switch (cli_pkt->type) {
	case CLI_PKT_HELLO: {

		/* Only handle hello from not a new client */
		if (unlikely(client->ctstate != CT_NEW))
			break;

		/* Welcome new conenction with server banner */
		client->ctstate = CT_ESTABLISHED;
		if (unlikely(!send_server_banner(client, state)))
			goto out_close_conn;

		break;
	}
	case CLI_PKT_AUTH: {

		/* New connection must hello first before auth */
		if (unlikely(client->ctstate == CT_NEW))
			goto out_close_conn;

		/* Ignore auth packet if the client has been authenticated */
		if (unlikely(client->is_auth))
			break;

		if (!handle_auth(client, state))
			goto out_close_conn; /* Wrong credential */

		break;
	}
	case CLI_PKT_DATA: {

		/* Unauthenticated client trying to send data */
		if (unlikely(client->ctstate != CT_AUTHENTICATED))
			goto out_close_conn;

		handle_iface_write(state, cli_pkt, fdata_len);
		break;
	}
	case CLI_PKT_CLOSE:
		goto out_close_conn;
	default:
		prl_notice(11, "Received invalid packet from " PRWIU
			       " (type: %d)", W_IU(client), cli_pkt->type);

		if (likely(!client->is_auth))
			goto out_close_conn;

		goto out_err_c;
	}


	prl_notice(15, "cdata_len = %u; fdata_len = %u", cdata_len, fdata_len);

	if (likely(cdata_len > fdata_len)) {
		/*
		 * We have extra packet on the tail, must memmove to
		 * the head before we run out of buffer.
		 */

		size_t cur_valid_size = CLI_PKT_MIN_RSIZ + fdata_len;
		size_t copy_size      = recv_s - cur_valid_size;

		memmove(recv_buf, recv_buf + cur_valid_size, copy_size);
		recv_s = copy_size;

		prl_notice(15, "memmove " PRWIU " (copy_size: %zu; "
			       "recv_s: %zu; cur_valid_size: %zu)",
			       W_IU(client), copy_size, recv_s, cur_valid_size);

		goto back_chk;
	}

	recv_s = 0;

out_save_recv_s:
	client->recv_s = recv_s;
	return;

out_err_c:
	client->recv_s = 0;
	if (unlikely(client->err_c++ >= MAX_ERR_C)) {
		prl_notice(3, "Connection " PRWIU " reached the max number of "
			   "error", W_IU(client));
		goto out_close_conn;
	}
	return;

out_close_conn:
	prl_notice(3, "Closing connection fd from " PRWIU, W_IU(client));
	close(cl->fd);
	cl->fd = -1;
	clear_disconnect(client);
	push_fds(&state->fds_stack, client->arr_idx);
	return;
}


static int event_loop(struct srv_tcp_state *state)
{
	int rv;
	int ern;
	int timeout;
	int retval = 0;
	int net_fd = state->net_fd;
	int tun_fd = state->tun_fd;
	int *pipe_fd = state->pipe_fd;
	struct pollfd *fds;
	struct pollfd *clfds;
	uint16_t max_conn = state->cfg->sock.max_conn;
	short curev; /* Current returned events */
	const short inev  = POLLIN | POLLPRI;	/* Input events    */
	const short errev = POLLERR | POLLHUP;	/* Error events    */
	const short retev = inev | errev;	/* Returned events */

	fds = calloc(max_conn + 3, sizeof(struct pollfd));
	if (unlikely(fds == NULL)) {
		pr_error("calloc: Cannot allocate memory: %s", strerror(errno));
		return -ENOMEM;
	}

	fds[0].fd = net_fd;;
	fds[0].events = inev;

	fds[1].fd = tun_fd;
	fds[1].events = inev;

	fds[2].fd = pipe_fd[0];
	fds[2].events = inev;

	clfds = fds + 3;

	for (uint16_t i = 0; i < max_conn; i++)
		clfds[i].fd = -1;

	state->fds = fds;
	state->nfds = 3 + max_conn;
	timeout = 5000;

	prl_notice(0, "Initialization Sequence Completed");

	for (;;) {
		if (unlikely(state->stop))
			break;

		rv = poll(fds, state->nfds, timeout);

		if (unlikely(rv == 0)) {
			/*
			 * Poll reached timeout.
			 *
			 * TODO: Do something meaningful here...
			 */
			continue;
		}

		if (unlikely(rv < 0)) {
			ern = errno;
			if (ern == EINTR) {
				retval = 0;
				prl_notice(0, "Interrupted!");
				break;
			}

			retval = -ern;
			pr_error("poll(): %s", strerror(ern));
			break;
		}

		curev = fds[0].revents;
		if (unlikely((curev & retev) != 0)) {
			if (likely((curev & inev) != 0)) {
				/*
				 * Someone is trying to connect to us.
				 */
				accept_conn(net_fd, clfds, state);
			} else {
				/* Error? */
				break;
			}
			rv--;
		}

		curev = fds[1].revents;
		if (likely((rv > 0) && ((curev & retev) != 0))) {
			if (likely((curev & inev) != 0)) {
				/*
				 * Read from virtual network interface.
				 */
				handle_iface_read(tun_fd, state);
			} else {
				/* Error? */
				break;
			}
			rv--;
		}

		for (uint16_t i = 0; likely((i < max_conn) && (rv > 0)); i++) {
			/*
			 * Enumerate client file descriptors.
			 */
			curev = clfds[i].revents;
			if (likely((curev & retev) != 0)) {
				if (likely((curev & inev) != 0)) {
					handle_client(&clfds[i], state, i);
					rv--;
				} else {
					/* Error? */
					break;
				}
			}
		}
	}

	free(fds);
	return retval;
}


int teavpn_server_tcp_handler(struct srv_cfg *cfg)
{
	int retval;
	struct srv_tcp_state state;

	memset(&state, 0, sizeof(state));

	state.cfg = cfg;
	g_state = &state;

	signal(SIGINT, intr_handler);
	signal(SIGHUP, intr_handler);
	signal(SIGTERM, intr_handler);
	signal(SIGQUIT, intr_handler);

	retval = init_state(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_pipe(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_iface(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_socket(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = event_loop(&state);
out:
	destroy_state(&state);
	return retval;
}