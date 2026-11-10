#pragma once
/* -----------------------------------------------------------------------
 * libogc's network calls on the 3DS's BSD sockets (libctru soc).  As on
 * the Wii they return a negative errno on failure.
 * ----------------------------------------------------------------------- */
#include "gctypes.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "sys/filio.h"
#include <3ds/services/soc.h>

static inline s32 net_ret_(int r) { return r < 0 ? -errno : r; }
static inline s32 net_socket(u32 domain, u32 type, u32 protocol) { return net_ret_(socket(domain, type, protocol)); }
static inline s32 net_connect(s32 s, struct sockaddr* a, socklen_t l) { (void)l; return net_ret_(connect(s, a, sizeof(struct sockaddr_in))); }
static inline s32 net_bind(s32 s, struct sockaddr* a, socklen_t l) { (void)l; return net_ret_(bind(s, a, sizeof(struct sockaddr_in))); }
static inline s32 net_listen(s32 s, u32 b) { return net_ret_(listen(s, b)); }
static inline s32 net_accept(s32 s, struct sockaddr* a, socklen_t* l) { return net_ret_(accept(s, a, l)); }
static inline s32 net_read(s32 s, void* b, u32 n) { return net_ret_(recv(s, b, n, 0)); }
static inline s32 net_recv(s32 s, void* b, s32 n, u32 f) { return net_ret_(recv(s, b, n, f)); }
static inline s32 net_write(s32 s, const void* b, u32 n) { return net_ret_(send(s, b, n, 0)); }
static inline s32 net_send(s32 s, const void* b, s32 n, u32 f) { return net_ret_(send(s, b, n, f)); }
static inline s32 net_sendto(s32 s, const void* b, s32 n, u32 f, struct sockaddr* to, socklen_t l)
{ (void)l; return net_ret_(sendto(s, b, n, f, to, sizeof(struct sockaddr_in))); }
static inline s32 net_recvfrom(s32 s, void* b, s32 n, u32 f, struct sockaddr* from, socklen_t* l)
{ socklen_t len = sizeof(struct sockaddr_in); s32 r = net_ret_(recvfrom(s, b, n, f, from, &len)); if (l) *l = len; return r; }
static inline s32 net_close(s32 s) { return net_ret_(closesocket(s)); }
static inline s32 net_shutdown(s32 s, u32 how) { return net_ret_(shutdown(s, how)); }
static inline s32 net_select(s32 n, fd_set* r, fd_set* w, fd_set* e, struct timeval* t) { return net_ret_(select(n, r, w, e, t)); }
static inline s32 net_setsockopt(s32 s, u32 level, u32 opt, const void* v, socklen_t l) { return net_ret_(setsockopt(s, level, opt, v, l)); }
static inline s32 net_getsockopt(s32 s, u32 level, u32 opt, void* v, socklen_t* l) { return net_ret_(getsockopt(s, level, opt, v, l)); }
static inline s32 net_ioctl(s32 s, u32 cmd, void* argp)
{
    if (cmd == FIONBIO) {
        int fl = fcntl(s, F_GETFL, 0);
        if (fl < 0) return -errno;
        fl = *(u32*)argp ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        return net_ret_(fcntl(s, F_SETFL, fl));
    }
    return -EINVAL;
}
static inline struct hostent* net_gethostbyname(const char* n) { return gethostbyname(n); }
#ifdef __cplusplus
extern "C" {
#endif
s32  net_init(void);
void net_deinit(void);
#ifdef __cplusplus
}
#endif
