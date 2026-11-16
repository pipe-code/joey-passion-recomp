/* DirectPlay 8 (IDirectPlay8Peer + IDirectPlay8Address) for the game's LAN duel, over plain sockets.
 *
 * Only copies of this port talk to each other, so the wire protocol is our own (not DirectPlay's):
 *  - discovery: the guest broadcasts a UDP query to port 2573 (and to 127.0.0.1, for two copies on one machine);
 *    a host answers with its session (instance GUID, name, players, TCP port).
 *  - session: one TCP connection, frames [u32 length][u8 kind][payload]: HELLO (guest name), WELCOME (host name +
 *    the two player ids), DATA (a game packet, as given to SendTo).
 * Sockets are non-blocking and polled from the game's main thread (PeekMessage/GetMessage and every DirectPlay
 * call); the game's message handler is called there too, with the GIL held, like any other guest callback.
 *
 * What the game uses (see NOTES.md): Initialize, GetSPCaps, SetPeerInfo, Host, EnumHosts, CancelAsyncOperation,
 * Connect, SendTo, GetPeerInfo, Close, Address::SetSP/AddComponent/Duplicate; messages CREATE_PLAYER,
 * DESTROY_PLAYER, RECEIVE, TERMINATE_SESSION, ASYNC_OP_COMPLETE, CONNECT_COMPLETE, ENUM_HOSTS_RESPONSE. */
#ifdef _WIN32
#define _WIN32_WINNT 0x0600              /* inet_pton / inet_ntop */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#undef GPTR                              /* winbase.h has its own */
#undef S_OK
#endif
#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif
#ifdef _WIN32
#define SOCK_ERR() WSAGetLastError()
#define WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK)
#define IN_PROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS || (e) == WSAEALREADY)
#define closesock closesocket
#else
#define SOCK_ERR() errno
#define WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define IN_PROGRESS(e) ((e) == EINPROGRESS || (e) == EALREADY)
#define closesock close
#endif
#if defined(__vita__)
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#elif !defined(_WIN32)
#include <ifaddrs.h>
#include <net/if.h>
#endif

#define S_OK 0
#define DPNSUCCESS_PENDING 0x0015000Eu
#define DPNERR_BUFFERTOOSMALL 0x80158100u
#define DPNERR_INVALIDPLAYER 0x80158210u
#define DPNERR_NOCONNECTION 0x80158250u
#define DPNERR_CONNECTIONLOST 0x801580D0u
#define DPNERR_USERCANCEL 0x80158550u
#define DPNERR_NORESPONSE 0x80158280u

enum {  /* DPN_MSGID_* */
    MSG_ASYNC_OP_COMPLETE = 0xFFFF0003, MSG_CONNECT_COMPLETE = 0xFFFF0005, MSG_CREATE_PLAYER = 0xFFFF0007,
    MSG_DESTROY_PLAYER = 0xFFFF0009, MSG_ENUM_HOSTS_RESPONSE = 0xFFFF000B, MSG_RECEIVE = 0xFFFF0011,
    MSG_TERMINATE_SESSION = 0xFFFF0016,
};
#define DPNPLAYER_LOCAL 2
#define DPNPLAYER_HOST 4
#define HOST_ID 0x00100001u
#define GUEST_ID 0x00200002u
#define DEFAULT_PORT 2573
enum { K_HELLO = 1, K_WELCOME = 2, K_DATA = 3 };
static const char QMAGIC[8] = "JOEYDPQ1", RMAGIC[8] = "JOEYDPR1";
#define RESP_SIZE (8 + 16 + 16 + 12 + 64)

typedef struct { int refs; char host[64]; uint32_t port; int has_port; } Addr;

typedef struct Ev { uint32_t id, msg; int kind; struct Ev *next; } Ev;   /* msg: guest struct */
enum { EV_PLAIN, EV_RECEIVE, EV_ENUM };
typedef struct {
    int refs;
    uint32_t cb, cb_ctx;
    char name[64];                       /* local player name (ASCII in practice) */
    uint8_t app_guid[16], inst_guid[16];
    int is_host, connected;
    uint32_t my_id, peer_id;
    char peer_name[64];
    int udp, listen_fd, tcp;             /* -1 when closed */
    uint32_t port;
    int enumerating; uint32_t enum_handle, enum_ctx, enum_next_ms;
    int connecting;                      /* 2: TCP connect in progress, 1: HELLO sent, waiting for WELCOME */
    uint32_t connect_ctx, connect_handle, connect_deadline;
    uint8_t *rbuf; size_t rlen, rcap;
    Ev *q, *qtail;
    int dispatching;
} Peer;

static uint32_t vt_peer, vt_addr;
static Peer *active;                     /* the game uses one peer at a time */
uint32_t dplay_addr_create(void);

/* ------------------------------------------------------------------ helpers */
static void wstr_ascii(uint32_t p, char *out, size_t n) {
    size_t i = 0;
    for (uint32_t c; p && i + 1 < n && (c = RD16(p)) != 0; p += 2) out[i++] = c < 0x80 ? (char)c : '?';
    out[i] = 0;
}
static uint32_t guest_wstr(const char *s) {
    size_t n = strlen(s);
    uint32_t p = galloc((uint32_t)(2 * (n + 1)), 1);
    for (size_t i = 0; i < n; i++) WR16(p + 2 * (uint32_t)i, (uint8_t)s[i]);
    return p;
}
static uint32_t now_ms(void) { uint32_t host_ticks(void); return host_ticks(); }
static void set_nonblock(int fd) {
#ifdef __vita__
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, (void *)&one, sizeof one);
#elif defined(_WIN32)
    u_long one = 1; ioctlsocket(fd, FIONBIO, &one);
#else
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}
/* The Vita needs its network module loaded and started before the first socket. */
static int net_ready(void) {
#ifdef __vita__
    static int state;                    /* 0 untried, 1 ok, -1 failed */
    if (state) return state > 0;
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static char mem[1024 * 1024];
    SceNetInitParam ip = {mem, sizeof mem, 0};
    int r = sceNetInit(&ip);
    if (r < 0 && r != (int)0x80410110 /* already initialized */) { hlog("dplay: sceNetInit failed 0x%08x\n", r); state = -1; return 0; }
    r = sceNetCtlInit();
    if (r < 0 && r != (int)0x80412102) { hlog("dplay: sceNetCtlInit failed 0x%08x\n", r); state = -1; return 0; }
    SceNetCtlInfo info;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) hlog("dplay: network up, ip %s\n", info.ip_address);
    else hlog("dplay: network module up, but no IP (Wi-Fi off?)\n");
    state = 1;
#elif defined(_WIN32)
    static int started;
    if (!started) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { hlog("dplay: WSAStartup failed\n"); return 0; } started = 1; }
#endif
    return 1;
}
static void close_fd(int *fd) { if (*fd >= 0) closesock(*fd); *fd = -1; }

static void push(Peer *p, uint32_t id, uint32_t msg, int kind) {
    Ev *e = calloc(1, sizeof *e); e->id = id; e->msg = msg; e->kind = kind;
    if (p->qtail) p->qtail->next = e; else p->q = e;
    p->qtail = e;
}
static uint32_t gstruct(uint32_t size) { uint32_t m = galloc(size, 1); WR32(m, size); return m; }
static void ev_create_player(Peer *p, uint32_t id) { uint32_t m = gstruct(12); WR32(m + 4, id); push(p, MSG_CREATE_PLAYER, m, EV_PLAIN); }
static void ev_destroy_player(Peer *p, uint32_t id, uint32_t reason) {
    uint32_t m = gstruct(16); WR32(m + 4, id); WR32(m + 12, reason); push(p, MSG_DESTROY_PLAYER, m, EV_PLAIN);
}
static void ev_terminate(Peer *p, uint32_t hr) { uint32_t m = gstruct(16); WR32(m + 4, hr); push(p, MSG_TERMINATE_SESSION, m, EV_PLAIN); }
static void ev_async_done(Peer *p, uint32_t handle, uint32_t uctx, uint32_t hr) {
    uint32_t m = gstruct(16); WR32(m + 4, handle); WR32(m + 8, uctx); WR32(m + 12, hr); push(p, MSG_ASYNC_OP_COMPLETE, m, EV_PLAIN);
}
static void ev_connect_complete(Peer *p, uint32_t hr) {
    /* DPNMSG_CONNECT_COMPLETE: size, hAsyncOp, pvUserContext, hResultCode, pvApplicationReplyData, dwSize */
    uint32_t m = gstruct(24); WR32(m + 4, p->connect_handle); WR32(m + 8, p->connect_ctx); WR32(m + 12, hr);
    push(p, MSG_CONNECT_COMPLETE, m, EV_PLAIN);
}

static void send_all(int fd, const uint8_t *b, size_t n) {
    while (n) {
        ssize_t w = send(fd, (const void *)b, (int)n, 0);
        if (w > 0) { b += w; n -= (size_t)w; continue; }
        if (w < 0 && WOULD_BLOCK(SOCK_ERR())) { usleep(1000); continue; }
        hlog("dplay: send failed (%d)\n", SOCK_ERR()); return;
    }
}
static void send_frame(Peer *p, int kind, const void *data, uint32_t len) {
    if (p->tcp < 0) return;
    uint8_t *f = malloc(5 + len); uint32_t n = len + 1;
    memcpy(f, &n, 4); f[4] = (uint8_t)kind; if (len) memcpy(f + 5, data, len);
    send_all(p->tcp, f, 5 + len);
    free(f);
}
static void name_frame(Peer *p, int kind) {          /* [u32 sender id][u32 receiver id][name, 64 bytes] */
    uint8_t b[8 + 64] = {0}; uint32_t ids[2] = {p->my_id, p->peer_id};
    memcpy(b, ids, 8); snprintf((char *)b + 8, 64, "%s", p->name);
    send_frame(p, kind, b, sizeof b);
}

static void lost_connection(Peer *p) {
    close_fd(&p->tcp); p->rlen = 0;
    if (p->connecting) { hlog("dplay: could not connect\n"); p->connecting = 0; ev_connect_complete(p, DPNERR_NORESPONSE); return; }
    if (!p->connected) return;
    hlog("dplay: connection to the other player lost\n");
    p->connected = 0;
    ev_destroy_player(p, p->peer_id, 3 /* DPNDESTROYPLAYERREASON_CONNECTIONLOST */);
    if (!p->is_host) ev_terminate(p, DPNERR_CONNECTIONLOST);
}

static void on_frame(Peer *p, int kind, const uint8_t *d, uint32_t len) {
    switch (kind) {
    case K_HELLO:                        /* host: the guest introduces itself (or renames) */
        if (!p->is_host || len < 72) break;
        snprintf(p->peer_name, sizeof p->peer_name, "%.63s", (const char *)d + 8);
        if (p->connected) break;
        p->peer_id = GUEST_ID; p->connected = 1;
        name_frame(p, K_WELCOME);
        hlog("dplay: guest \"%s\" joined\n", p->peer_name);
        ev_create_player(p, p->peer_id);
        break;
    case K_WELCOME: {                    /* guest: accepted (or the host renamed) */
        if (p->is_host || len < 72) break;
        snprintf(p->peer_name, sizeof p->peer_name, "%.63s", (const char *)d + 8);
        if (p->connected) break;
        uint32_t ids[2]; memcpy(ids, d, 8);
        p->peer_id = ids[0]; p->my_id = ids[1];
        p->connecting = 0; p->connected = 1;
        hlog("dplay: joined \"%s\"\n", p->peer_name);
        ev_create_player(p, p->my_id);
        ev_create_player(p, p->peer_id);
        ev_connect_complete(p, S_OK);
        break; }
    case K_DATA: {
        if (!p->connected) break;
        /* DPNMSG_RECEIVE: size, dpnidSender, pvPlayerContext, pReceiveData, dwReceiveDataSize, hBufferHandle */
        uint32_t buf = galloc(len ? len : 1, 0);
        memcpy(GPTR(buf), d, len);
        uint32_t m = gstruct(24); WR32(m + 4, p->peer_id); WR32(m + 12, buf); WR32(m + 16, len); WR32(m + 20, buf);
        push(p, MSG_RECEIVE, m, EV_RECEIVE);
        break; }
    }
}

/* ------------------------------------------------------------------ network polling */
static int udp_open(uint32_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (void *)&one, sizeof one);
#endif
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (void *)&one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { hlog("dplay: udp bind %u failed (%d)\n", port, SOCK_ERR()); closesock(fd); return -1; }
    set_nonblock(fd);
    return fd;
}
static void enum_send(Peer *p) {
    uint8_t q[24]; memcpy(q, QMAGIC, 8); memcpy(q + 8, p->app_guid, 16);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)p->port);
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST); sendto(p->udp, (const void *)q, sizeof q, 0, (struct sockaddr *)&a, sizeof a);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sendto(p->udp, (const void *)q, sizeof q, 0, (struct sockaddr *)&a, sizeof a);
#if defined(_WIN32)
    ULONG sz = 0;                        /* directed broadcast of every adapter's subnet */
    GetAdaptersInfo(NULL, &sz);
    IP_ADAPTER_INFO *ai = sz ? malloc(sz) : NULL;
    if (ai && GetAdaptersInfo(ai, &sz) == NO_ERROR)
        for (IP_ADAPTER_INFO *x = ai; x; x = x->Next)
            for (IP_ADDR_STRING *ip = &x->IpAddressList; ip; ip = ip->Next) {
                struct in_addr i4, m4;
                if (inet_pton(AF_INET, ip->IpAddress.String, &i4) == 1 && inet_pton(AF_INET, ip->IpMask.String, &m4) == 1 && i4.s_addr) {
                    a.sin_addr.s_addr = i4.s_addr | ~m4.s_addr;
                    sendto(p->udp, (const void *)q, sizeof q, 0, (struct sockaddr *)&a, sizeof a);
                }
            }
    free(ai);
#elif !defined(__vita__)
    struct ifaddrs *ifs, *i;             /* directed broadcasts: 255.255.255.255 does not leave every interface */
    if (getifaddrs(&ifs) == 0) {
        for (i = ifs; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
                a.sin_addr = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr;
                sendto(p->udp, (const void *)q, sizeof q, 0, (struct sockaddr *)&a, sizeof a);
            }
        freeifaddrs(ifs);
    }
#else
    SceNetCtlInfo ip, mask;              /* directed broadcast of the Wi-Fi subnet */
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &ip) >= 0 && sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &mask) >= 0) {
        struct in_addr i4, m4;
        if (inet_pton(AF_INET, ip.ip_address, &i4) == 1 && inet_pton(AF_INET, mask.netmask, &m4) == 1) {
            a.sin_addr.s_addr = i4.s_addr | ~m4.s_addr;
            sendto(p->udp, (const void *)q, sizeof q, 0, (struct sockaddr *)&a, sizeof a);
        }
    }
#endif
}
static void enum_response(Peer *p, const uint8_t *r, int n, struct sockaddr_in *from) {
    if (n < RESP_SIZE || memcmp(r, RMAGIC, 8) || memcmp(r + 8, p->app_guid, 16)) return;
    uint32_t v[3]; memcpy(v, r + 40, 12);           /* tcp port, current players, max players */
    char name[65]; memcpy(name, r + 52, 64); name[64] = 0;
    char ip[32]; inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
    uint32_t ad = gstruct(0x48);                     /* DPN_APPLICATION_DESC */
    memcpy(GPTR(ad + 8), r + 24, 16); memcpy(GPTR(ad + 0x18), r + 8, 16);
    WR32(ad + 0x28, v[2]); WR32(ad + 0x2C, v[1]); WR32(ad + 0x30, guest_wstr(name));
    uint32_t sender = dplay_addr_create(), device = dplay_addr_create();
    Addr *sa = com_host(sender); snprintf(sa->host, sizeof sa->host, "%s", ip); sa->port = v[0]; sa->has_port = 1;
    /* DPNMSG_ENUM_HOSTS_RESPONSE: size, pAddressSender, pAddressDevice, pApplicationDescription,
     * pvResponseData, dwResponseDataSize, pvUserContext, dwRoundTripLatencyMS */
    uint32_t m = gstruct(32); WR32(m + 4, sender); WR32(m + 8, device); WR32(m + 12, ad); WR32(m + 24, p->enum_ctx); WR32(m + 28, 1);
    push(p, MSG_ENUM_HOSTS_RESPONSE, m, EV_ENUM);
}

static void poll_net(Peer *p) {
    uint8_t b[2048];
    struct sockaddr_in from; socklen_t fl;
    if (p->udp >= 0) {
        int n;
        while (fl = sizeof from, (n = (int)recvfrom(p->udp, (void *)b, sizeof b, 0, (struct sockaddr *)&from, &fl)) > 0) {
            if (host_verbose) { char ip[32]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip); hlog("dplay: udp %d bytes from %s:%u\n", n, ip, ntohs(from.sin_port)); }
            if (p->is_host && n >= 24 && !memcmp(b, QMAGIC, 8) && !memcmp(b + 8, p->app_guid, 16)) {
                uint8_t r[RESP_SIZE] = {0};
                uint32_t v[3] = {p->port, p->connected ? 2u : 1u, 2};
                memcpy(r, RMAGIC, 8); memcpy(r + 8, p->app_guid, 16); memcpy(r + 24, p->inst_guid, 16); memcpy(r + 40, v, 12);
                snprintf((char *)r + 52, 64, "%s", p->name);
                sendto(p->udp, (const void *)r, sizeof r, 0, (struct sockaddr *)&from, fl);
            } else if (p->enumerating) enum_response(p, b, n, &from);
        }
        if (p->enumerating && now_ms() >= p->enum_next_ms) { enum_send(p); p->enum_next_ms = now_ms() + 1000; }
    }
    if (p->listen_fd >= 0 && p->tcp < 0) {
        int c = accept(p->listen_fd, NULL, NULL);
        if (c >= 0) { int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (void *)&one, sizeof one); set_nonblock(c); p->tcp = c; p->rlen = 0; }
    }
    if (p->tcp >= 0 && p->connecting == 2) {         /* non-blocking connect in progress */
        int err = 0; socklen_t el = sizeof err;
        struct sockaddr_in peer; socklen_t pl = sizeof peer;
        getsockopt(p->tcp, SOL_SOCKET, SO_ERROR, (void *)&err, &el);
        if (err && !IN_PROGRESS(err)) { hlog("dplay: connect failed (%d)\n", err); lost_connection(p); return; }
        if (getpeername(p->tcp, (struct sockaddr *)&peer, &pl) == 0) { p->connecting = 1; name_frame(p, K_HELLO); }
        else if (now_ms() > p->connect_deadline) { hlog("dplay: connect timed out\n"); lost_connection(p); return; }
    }
    if (p->tcp >= 0 && p->connecting != 2) {
        for (;;) {
            if (p->rcap - p->rlen < 4096) { p->rcap = p->rcap ? p->rcap * 2 : 16384; p->rbuf = realloc(p->rbuf, p->rcap); }
            ssize_t n = recv(p->tcp, (void *)(p->rbuf + p->rlen), (int)(p->rcap - p->rlen), 0);
            if (n > 0) { p->rlen += (size_t)n; continue; }
            if (n == 0 || !WOULD_BLOCK(SOCK_ERR())) { lost_connection(p); return; }
            break;
        }
        size_t off = 0;
        while (p->rlen - off >= 4) {
            uint32_t len; memcpy(&len, p->rbuf + off, 4);
            if (len == 0 || len > (1u << 24)) { lost_connection(p); return; }
            if (p->rlen - off - 4 < len) break;
            on_frame(p, p->rbuf[off + 4], p->rbuf + off + 5, len - 1);
            off += 4 + len;
        }
        memmove(p->rbuf, p->rbuf + off, p->rlen - off); p->rlen -= off;
    }
}

static void free_ev(Ev *e) {
    if (e->kind == EV_RECEIVE) gfree(RD32(e->msg + 12));
    if (e->kind == EV_ENUM) { uint32_t ad = RD32(e->msg + 12); gfree(RD32(ad + 0x30)); gfree(ad); }
    gfree(e->msg); free(e);
}
static void dispatch(CPU *ctx, Peer *p) {
    if (p->dispatching || !p->cb) return;
    p->dispatching = 1;
    poll_net(p);
    while (p->q) {
        Ev *e = p->q; p->q = e->next; if (!p->q) p->qtail = NULL;
        uint32_t r = guest_call(ctx, p->cb, 3, p->cb_ctx, e->id, e->msg);
        if (e->kind == EV_RECEIVE && r == DPNSUCCESS_PENDING) e->kind = EV_PLAIN;   /* the game keeps it: ReturnBuffer */
        if (e->kind == EV_ENUM) {        /* the game Duplicates the addresses it keeps; ours go away */
            uint32_t s = RD32(e->msg + 4), d = RD32(e->msg + 8);
            free(com_host(s)); com_free(s); free(com_host(d)); com_free(d);
        }
        free_ev(e);
    }
    p->dispatching = 0;
}
/* called from the message pump (PeekMessageA / GetMessageA) */
void dplay_pump(CPU *ctx) { if (active) dispatch(ctx, active); }

static void peer_close(Peer *p) {
    if (p->connected) hlog("dplay: session closed\n");
    close_fd(&p->tcp); close_fd(&p->listen_fd); close_fd(&p->udp);
    p->connected = p->connecting = p->enumerating = p->is_host = 0; p->rlen = 0; p->peer_id = 0;
    while (p->q) { Ev *e = p->q; p->q = e->next; if (e->kind == EV_ENUM) e->kind = EV_PLAIN; free_ev(e); }
    p->qtail = NULL;
}

/* ------------------------------------------------------------------ IDirectPlay8Peer */
static const struct { const char *name; int nargs; } peer_m[] = {
    {"QueryInterface", 3}, {"AddRef", 1}, {"Release", 1}, {"Initialize", 4}, {"EnumServiceProviders", 7},
    {"CancelAsyncOperation", 3}, {"Connect", 12}, {"SendTo", 8}, {"GetSendQueueInfo", 5}, {"Host", 8},
    {"GetApplicationDesc", 4}, {"SetApplicationDesc", 3}, {"CreateGroup", 6}, {"DestroyGroup", 5},
    {"AddPlayerToGroup", 6}, {"RemovePlayerFromGroup", 6}, {"SetGroupInfo", 6}, {"GetGroupInfo", 5},
    {"EnumPlayersAndGroups", 4}, {"EnumGroupMembers", 5}, {"SetPeerInfo", 5}, {"GetPeerInfo", 5},
    {"GetPeerAddress", 4}, {"GetLocalHostAddresses", 4}, {"Close", 2}, {"EnumHosts", 12}, {"DestroyPeer", 5},
    {"ReturnBuffer", 3}, {"GetPlayerContext", 4}, {"GetGroupContext", 4}, {"GetCaps", 3}, {"SetCaps", 3},
    {"SetSPCaps", 4}, {"GetSPCaps", 4}, {"GetConnectionInfo", 4}, {"RegisterLobby", 4}, {"TerminateSession", 4},
};
#define NPEER (int)(sizeof peer_m / sizeof *peer_m)
enum { P_QI, P_ADDREF, P_RELEASE, P_INIT, P_ENUMSP, P_CANCEL, P_CONNECT, P_SENDTO, P_SENDQ, P_HOST,
       P_SETPEERINFO = 20, P_GETPEERINFO = 21, P_CLOSE = 24, P_ENUMHOSTS = 25, P_RETURNBUFFER = 27, P_GETSPCAPS = 33 };

static uint32_t peer_call(CPU *ctx, int m) {
    Peer *p = THIS_HOST(Peer);
    if (m > P_RELEASE && m != P_SENDTO && m != P_GETPEERINFO) {
        char args[256] = ""; int n = 0;
        for (int i = 1; i < peer_m[m].nargs; i++) n += snprintf(args + n, sizeof args - n, "%s0x%x", i > 1 ? ", " : "", A(i));
        hlog("IDirectPlay8Peer::%s(%s)\n", peer_m[m].name, args);
    }
    uint32_t r = S_OK;
    switch (m) {
    case P_QI: WR32(A(2), A(0)); p->refs++; return S_OK;
    case P_ADDREF: return (uint32_t)++p->refs;
    case P_RELEASE:
        if (p->refs > 0 && --p->refs == 0) { peer_close(p); if (active == p) active = NULL; }
        return (uint32_t)p->refs;
    case P_INIT: p->cb_ctx = A(1); p->cb = A(2); active = p; return S_OK;
    case P_ENUMSP: if (A(4)) WR32(A(4), 0); if (A(5)) WR32(A(5), 0); return S_OK;
    case P_GETSPCAPS: {                  /* guid, DPN_SP_CAPS*, flags. The game drops hosts not heard from in 3x the
                                            enum retry interval, so these must be the real TCP/IP defaults. */
        uint32_t c = A(2);
        if (c && RD32(c) >= 0x24) {
            WR32(c + 4, 0); WR32(c + 8, 3);              /* flags, threads */
            WR32(c + 12, 5); WR32(c + 16, 1500); WR32(c + 20, 1500);   /* enum count, retry ms, timeout ms */
            WR32(c + 24, 1472); WR32(c + 28, 0); WR32(c + 32, 0);      /* max enum payload, buffers */
        }
        return S_OK; }
    case P_SETPEERINFO:                  /* DPN_PLAYER_INFO: size, infoflags, pwszName, pvData, dwDataSize, playerflags */
        if (A(1) && (RD32(A(1) + 4) & 1)) wstr_ascii(RD32(A(1) + 8), p->name, sizeof p->name);
        if (p->connected) name_frame(p, p->is_host ? K_WELCOME : K_HELLO);
        return S_OK;
    case P_HOST: {                       /* appdesc, device addrs[], count, security, credentials, player ctx, flags */
        uint32_t ad = A(1);
        if (!net_ready()) return DPNERR_NOCONNECTION;
        memcpy(p->app_guid, GPTR(ad + 0x18), 16);
        for (int i = 0; i < 16; i++) p->inst_guid[i] = (uint8_t)(rand() ^ (now_ms() >> (i % 4 * 8)));
        p->port = DEFAULT_PORT;
        for (uint32_t i = 0; i < A(3); i++) { Addr *a = com_host(RD32(A(2) + 4 * i)); if (a && a->has_port) p->port = a->port; }
        p->is_host = 1; p->my_id = HOST_ID; p->peer_id = 0;
        p->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(p->listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof one);
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)p->port); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(p->listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(p->listen_fd, 1) < 0) {
            hlog("dplay: cannot listen on TCP %u (%d)\n", p->port, SOCK_ERR()); close_fd(&p->listen_fd); return DPNERR_NOCONNECTION;
        }
        set_nonblock(p->listen_fd);
        p->udp = udp_open(p->port);
        hlog("dplay: hosting \"%s\" on port %u\n", p->name, p->port);
        ev_create_player(p, p->my_id);
        break; }
    case P_ENUMHOSTS: {                  /* appdesc, host addr, device addr, data, size, count, retry, timeout, uctx, *handle, flags */
        if (!net_ready()) return DPNERR_NOCONNECTION;
        memcpy(p->app_guid, GPTR(A(1) + 0x18), 16);
        Addr *ha = com_host(A(2));
        p->port = ha && ha->has_port ? ha->port : DEFAULT_PORT;
        if (p->udp < 0) p->udp = udp_open(0);
        if (p->udp < 0) return DPNERR_NOCONNECTION;
        p->enumerating = 1; p->enum_ctx = A(9); p->enum_handle = 0x00E00001u; p->enum_next_ms = 0;
        if (A(10)) WR32(A(10), p->enum_handle);
        hlog("dplay: searching for hosts on port %u\n", p->port);
        r = DPNSUCCESS_PENDING;
        break; }
    case P_CANCEL:                       /* handle (0 = all), flags */
        if (p->enumerating && (!A(1) || A(1) == p->enum_handle)) {
            p->enumerating = 0; ev_async_done(p, p->enum_handle, p->enum_ctx, DPNERR_USERCANCEL);
        }
        break;
    case P_CONNECT: {                    /* appdesc, host addr, device addr, security, credentials, data, size,
                                            player ctx, async ctx, *handle, flags */
        Addr *ha = com_host(A(2));
        if (!ha || !ha->host[0]) return DPNERR_NOCONNECTION;
        if (p->enumerating) { p->enumerating = 0; ev_async_done(p, p->enum_handle, p->enum_ctx, DPNERR_USERCANCEL); }
        p->is_host = 0; p->connect_ctx = A(9); p->connect_handle = 0x00C00001u;
        if (A(10)) WR32(A(10), p->connect_handle);
        p->tcp = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(p->tcp, IPPROTO_TCP, TCP_NODELAY, (void *)&one, sizeof one);
        set_nonblock(p->tcp);
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)(ha->has_port ? ha->port : DEFAULT_PORT));
        inet_pton(AF_INET, ha->host, &a.sin_addr);
        hlog("dplay: connecting to %s:%u\n", ha->host, ntohs(a.sin_port));
        p->rlen = 0; p->connecting = 2; p->connect_deadline = now_ms() + 10000;
        if (connect(p->tcp, (struct sockaddr *)&a, sizeof a) < 0 && !IN_PROGRESS(SOCK_ERR())) lost_connection(p);
        if (A(11) & 0x80000000u) {       /* DPNCONNECT_SYNC: wait here */
            uint32_t until = now_ms() + 10000;
            while (p->connecting && now_ms() < until) { poll_net(p); usleep(5000); }
            r = p->connected ? S_OK : DPNERR_NORESPONSE;
        } else r = DPNSUCCESS_PENDING;
        break; }
    case P_SENDTO: {                     /* dpnid, bufdesc[], count, timeout, async ctx, *handle, flags */
        if (!p->connected) return DPNERR_NOCONNECTION;
        uint32_t total = 0;
        for (uint32_t i = 0; i < A(3); i++) total += RD32(A(2) + 8 * i);
        uint8_t *b = malloc(total ? total : 1); uint32_t o = 0;
        for (uint32_t i = 0; i < A(3); i++) { uint32_t l = RD32(A(2) + 8 * i); memcpy(b + o, GPTR(RD32(A(2) + 8 * i + 4)), l); o += l; }
        if (host_verbose) {
            char hex[3 * 128 + 1] = ""; for (uint32_t j = 0; j < total && j < 128; j++) sprintf(hex + 3 * j, "%02x ", b[j]);
            hlog("dplay: send to 0x%x %u bytes flags 0x%x: %s\n", A(1), total, A(7), hex);
        }
        if (A(1) != p->my_id) send_frame(p, K_DATA, b, total);
        free(b);
        if (A(6)) WR32(A(6), 0);
        break; }
    case P_GETPEERINFO: {                /* dpnid, info*, size*, flags */
        uint32_t id = A(1);
        const char *nm = id == p->my_id ? p->name : id == p->peer_id && p->peer_id ? p->peer_name : NULL;
        if (!nm) return DPNERR_INVALIDPLAYER;
        uint32_t need = 0x18 + 2 * ((uint32_t)strlen(nm) + 1), cap = RD32(A(3));
        WR32(A(3), need);
        if (!A(2) || cap < need) return DPNERR_BUFFERTOOSMALL;
        uint32_t i = A(2), fl = 0;
        if (id == p->my_id) fl |= DPNPLAYER_LOCAL;
        if ((id == p->my_id) == (p->is_host != 0)) fl |= DPNPLAYER_HOST;
        WR32(i, 0x18); WR32(i + 4, 3); WR32(i + 8, i + 0x18); WR32(i + 12, 0); WR32(i + 16, 0); WR32(i + 20, fl);
        for (size_t k = 0; k <= strlen(nm); k++) WR16(i + 0x18 + 2 * (uint32_t)k, (uint8_t)nm[k]);
        return S_OK; }
    case P_RETURNBUFFER: gfree(A(1)); return S_OK;
    case P_CLOSE: peer_close(p); return S_OK;
    default: break;
    }
    dispatch(ctx, p);
    return r;
}
#define PM(i) static void pm##i(CPU *ctx) { ctx->eax = peer_call(ctx, i); RET_STDCALL(ctx, peer_m[i].nargs); }
PM(0) PM(1) PM(2) PM(3) PM(4) PM(5) PM(6) PM(7) PM(8) PM(9) PM(10) PM(11) PM(12) PM(13) PM(14) PM(15) PM(16) PM(17)
PM(18) PM(19) PM(20) PM(21) PM(22) PM(23) PM(24) PM(25) PM(26) PM(27) PM(28) PM(29) PM(30) PM(31) PM(32) PM(33)
PM(34) PM(35) PM(36)
static const x86_fn peer_fn[] = {pm0, pm1, pm2, pm3, pm4, pm5, pm6, pm7, pm8, pm9, pm10, pm11, pm12, pm13, pm14, pm15,
    pm16, pm17, pm18, pm19, pm20, pm21, pm22, pm23, pm24, pm25, pm26, pm27, pm28, pm29, pm30, pm31, pm32, pm33, pm34, pm35, pm36};

/* ------------------------------------------------------------------ IDirectPlay8Address */
static const struct { const char *name; int nargs; } addr_m[] = {
    {"QueryInterface", 3}, {"AddRef", 1}, {"Release", 1}, {"BuildFromURLW", 2}, {"BuildFromURLA", 2},
    {"Duplicate", 2}, {"SetEqual", 2}, {"IsEqual", 2}, {"Clear", 1}, {"GetURLW", 3}, {"GetURLA", 3}, {"GetSP", 2},
    {"GetUserData", 3}, {"SetSP", 2}, {"SetUserData", 3}, {"GetNumComponents", 2}, {"GetComponentByName", 5},
    {"GetComponentByIndex", 7}, {"AddComponent", 5}, {"GetDevice", 2}, {"SetDevice", 2}, {"BuildFromDPADDRESS", 3},
};
#define NADDR (int)(sizeof addr_m / sizeof *addr_m)

static uint32_t addr_call(CPU *ctx, int m) {
    Addr *a = THIS_HOST(Addr);
    switch (m) {
    case 0: WR32(A(2), A(0)); a->refs++; return S_OK;
    case 1: return (uint32_t)++a->refs;
    case 2:
        if (a->refs > 0 && --a->refs == 0) { free(a); com_free(A(0)); return 0; }
        return (uint32_t)a->refs;
    case 5: { uint32_t d = dplay_addr_create(); Addr *b = com_host(d); *b = *a; b->refs = 1; WR32(A(1), d); return S_OK; }
    case 6: { Addr *b = com_host(A(1)); if (b) { int r = a->refs; *a = *b; a->refs = r; } return S_OK; }
    case 8: { int r = a->refs; memset(a, 0, sizeof *a); a->refs = r; return S_OK; }
    case 11: case 13: return S_OK;       /* GetSP / SetSP: TCP/IP is the only provider */
    case 18: {                           /* name, data, size, type: 1 STRING (wide), 2 DWORD, 3 GUID, 4 BINARY, 5 STRING_ANSI */
        char name[64]; wstr_ascii(A(1), name, sizeof name);
        uint32_t t = A(4), d = A(2);
        if (!strcasecmp(name, "port") && t == 2) { a->port = RD32(d); a->has_port = 1; }
        else if (!strcasecmp(name, "hostname")) {
            if (t == 1) wstr_ascii(d, a->host, sizeof a->host);
            else if (t == 5) snprintf(a->host, sizeof a->host, "%s", (const char *)GPTR(d));
        }
        return S_OK; }
    default:
        hlog("IDirectPlay8Address::%s (not implemented)\n", addr_m[m].name);
        return S_OK;
    }
}
#define AM(i) static void am##i(CPU *ctx) { ctx->eax = addr_call(ctx, i); RET_STDCALL(ctx, addr_m[i].nargs); }
AM(0) AM(1) AM(2) AM(3) AM(4) AM(5) AM(6) AM(7) AM(8) AM(9) AM(10) AM(11) AM(12) AM(13) AM(14) AM(15) AM(16) AM(17)
AM(18) AM(19) AM(20) AM(21)
static const x86_fn addr_fn[] = {am0, am1, am2, am3, am4, am5, am6, am7, am8, am9, am10, am11, am12, am13, am14, am15,
    am16, am17, am18, am19, am20, am21};

static void vtables(void) {
    if (vt_peer) return;
    ComMethod pm[NPEER], am[NADDR];
    for (int i = 0; i < NPEER; i++) { pm[i].name = peer_m[i].name; pm[i].fn = peer_fn[i]; }
    for (int i = 0; i < NADDR; i++) { am[i].name = addr_m[i].name; am[i].fn = addr_fn[i]; }
    vt_peer = com_vtbl("IDirectPlay8Peer", pm, NPEER);
    vt_addr = com_vtbl("IDirectPlay8Address", am, NADDR);
}
uint32_t dplay_peer_create(void) {
    vtables();
    Peer *p = calloc(1, sizeof *p); p->refs = 1; p->udp = p->listen_fd = p->tcp = -1;
    snprintf(p->name, sizeof p->name, "Player");
    return com_new(vt_peer, p);
}
uint32_t dplay_addr_create(void) { vtables(); Addr *a = calloc(1, sizeof *a); a->refs = 1; return com_new(vt_addr, a); }
