/*
 * Implementation of the Python 2.5.1 socket module for Shed Skin
 * by: Michael Elkins <me@cs.hmc.edu>
 * February 25, 2008
 *
 * Current Issues:
 * - unix domain sockets are not implemented
 * - can't call socket.settimeout(None)
 */

#include "socket.hpp"

#ifdef WIN32

#define CLOSE closesocket
#define EINPROGRESS WSAEINPROGRESS
#define SOCKOPT_CAST (char*)
typedef long tv_sec_type;
typedef long tv_usec_type;

//winsock docs state that the hostname will always be 256 or less
#define HOST_NAME_MAX 256

#define ERRNO WSAGetLastError()

#else /* ! WIN32 */

#include <netinet/in.h>
#include <sys/un.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cerrno>
#define CLOSE close
#define SOCKET_ERROR -1
#define SOCKOPT_CAST
#define ERRNO errno
typedef time_t tv_sec_type;
typedef suseconds_t tv_usec_type;

#endif /* WIN32 */

#include <sstream>

namespace __socket__ {

str *__name__;

str *invalid_address;
str *timed_out;
str *host_not_found;

/**
class error
*/

class_ *cl_error;

/**
class herror
*/

class_ *cl_herror;

/**
class gaierror
*/

class_ *cl_gaierror;

/**
class timeout
*/

class_ *cl_timeout;

/**
class socket
*/

class_ *cl_socket;

int __ss_AF_INET = AF_INET;
int __ss_AF_UNIX = AF_UNIX;
int __ss_SOCK_STREAM = SOCK_STREAM;
int __ss_SOCK_DGRAM = SOCK_DGRAM;
#ifndef WIN32
int __ss_SOL_IP = SOL_IP;
int __ss_IP_TOS = IP_TOS;
int __ss_IP_TTL = IP_TTL;
#endif
int __ss_SOL_SOCKET = SOL_SOCKET;
int __ss_SO_REUSEADDR = SO_REUSEADDR;
int __ss_INADDR_ANY = INADDR_ANY;
int __ss_INADDR_LOOPBACK = INADDR_LOOPBACK;
int __ss_INADDR_NONE = INADDR_NONE;
int __ss_INADDR_BROADCAST = INADDR_BROADCAST;
int __ss_SOMAXCONN = SOMAXCONN;

double __ss_default_timeout = -1.0;

int socket::__ss_fileno() {
    
    return this->_fd;
}

#ifdef WIN32
//not exactly the correct definition, but we only use it with ostringstream
std::string strerror(int e)
{
	std::ostringstream os;
	os << "socket error " << e;
	return os.str();
}
#endif

str* make_errstring(const char *prefix)
{
    std::ostringstream os;
    os << prefix << ": " << strerror(ERRNO) << " (errno " << ERRNO << ")";
    return new str( os.str().c_str() );
}

str *socket::getsockopt(int level, int optname, int value) {
    
    socklen_t buflen = value;
    char buf[buflen];

    if (::getsockopt(_fd, level, optname, buf, &buflen) == SOCKET_ERROR)
	throw new error(make_errstring("getsockopt"));

    return new str(buf, buflen);
}

socket *socket::bind(const sockaddr *sa, socklen_t salen)
{
    if (::bind(_fd, sa, salen) == SOCKET_ERROR) {
	throw new error(make_errstring("bind"));
    }
    return this;
}

// python supports two special strings
static unsigned long int string_to_addr(const char *s)
{
    if (!*s)
	return INADDR_ANY;
    if (strcmp(s, "<broadcast>") == 0)
	return INADDR_BROADCAST;
#ifdef WIN32
    /* winsock doesn't have inet_aton() so we are forced to use inet_addr().
     * however, since python has the special form <broadcast> we can use
     * -1 as the error check here.
     */
    unsigned long int addr = inet_addr(s);
    if (addr != (unsigned long int)-1)
	    return addr;
#else
    struct in_addr addr;
    if (::inet_aton(s, &addr))
	return addr.s_addr; // ip address
#endif
    /* try looking up the address in dns */
    struct hostent *he = ::gethostbyname(s);
    if (!he)
	throw new herror(host_not_found); //FIXME should this raise herror() instead?
    return * reinterpret_cast<unsigned long *>( he->h_addr_list[0] );
}

// conver the python version of a address to the bsd socket variety
static void tuple_to_sin_addr(sockaddr_in *dst, socket::inet_address src)
{
    memset(dst, 0, sizeof(sockaddr_in));
    dst->sin_family = AF_INET;
    const char *host = src->first->unit.c_str();
    dst->sin_addr.s_addr = string_to_addr(host);
    dst->sin_port = htons(src->second);
}

socket *socket::bind(socket::inet_address address)
{
    if (family != AF_INET)
	throw new ValueError(invalid_address);

    const char *host = address->first->unit.c_str();
    int port = address->second;

    sockaddr_in sin;
    tuple_to_sin_addr(&sin, address);
    return bind(reinterpret_cast<sockaddr *>(&sin), sizeof(sin));
}

socket *socket::setsockopt(int level, int optname, int value) {
    if (::setsockopt(_fd, level, optname, SOCKOPT_CAST &value, sizeof(value)) == SOCKET_ERROR)
	throw new error(make_errstring("setsockopt"));
    
    return this;
}

socket *socket::connect(socket::inet_address address) {
    if (family != AF_INET)
	throw new ValueError(invalid_address);
    const char *host = address->first->unit.c_str();
    int port = address->second;

    sockaddr_in sin;
    sockaddr* sa = reinterpret_cast<sockaddr *>(&sin);
    memset(&sin, 0, sizeof(sin));
    socklen_t salen = sizeof(sin);
    sin.sin_family = family;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = string_to_addr(host);

    return connect(reinterpret_cast<sockaddr *>(&sin), sizeof(sin));
}

#ifndef WIN32
socket *socket::connect(pyseq<str *> *address)
{
	if (family != AF_UNIX)
		throw new ValueError(invalid_address);
	sockaddr_un sun;
	sun.sun_family = AF_UNIX;
	const str* __0 = address->__getitem__(0);
	strncpy(sun.sun_path, __0->unit.c_str(), PATH_MAX);

	return connect(reinterpret_cast<sockaddr *>(&sun), sizeof(sun));
}
#endif /* ! WIN32 */


static void set_blocking(socket_type fd)
{
#ifdef WIN32
	u_long flag = 0;
	if (ioctlsocket(fd, FIONBIO, &flag) == SOCKET_ERROR)
#else
	//FIXME should probably only clear the O_NONBLOCKING flag
	if (::fcntl(fd, F_SETFL, 0) == SOCKET_ERROR)
#endif
	{
		throw new error(make_errstring("fcntl"));
	}
}

static void set_nonblocking(socket_type fd)
{
#ifdef WIN32
	u_long flag = 1;
	if (ioctlsocket(fd, FIONBIO, &flag) == SOCKET_ERROR)
#else
	if (::fcntl(fd, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
#endif
	{
		throw new error(make_errstring("fcntl"));
	}
}
socket *socket::connect(const sockaddr *sa, socklen_t salen)
{
    if (_blocking && _timeout >= 0) {
	// temporarily set the socket to nonblocking
	set_nonblocking(_fd);
    }

    if (::connect(_fd, sa, salen) == SOCKET_ERROR) {
	if (ERRNO != EINPROGRESS)
	    throw new error(make_errstring("connect"));
    }

    if (_blocking && _timeout >= 0) {
	fd_set s;
	FD_ZERO(&s);
	FD_SET(_fd, &s);
	timeval to;
	to.tv_sec = static_cast<tv_sec_type>(_timeout);
	to.tv_usec = static_cast<tv_usec_type>(1000000 * (_timeout - (double)to.tv_sec));
	if (::select(_fd+1, 0, &s, 0, &to) == SOCKET_ERROR)
	    throw new error(make_errstring("select"));
	if (! FD_ISSET(_fd, &s)) {
	    //FIXME socket is left in nonblocking state, is this ok?
	    throw new timeout(timed_out);
	}

	// get connection status
	int err = 0;
	socklen_t errsize = sizeof(err);
	if (::getsockopt(_fd, SOL_SOCKET, SO_ERROR, SOCKOPT_CAST &err, &errsize) == SOCKET_ERROR)
	    throw new error(make_errstring("getsockopt"));

	if (err != 0) {
	    std::ostringstream os;
	    os << "connect: " << strerror(err) << " (errno " << err << ")";
	    const std::string& s = os.str();
	    throw new error(new str( s.c_str() ));
	}

	// turn blocking back on
	set_nonblocking(_fd);
    }

    return this;
}
socket *socket::setblocking(int flag)
{
    if (flag)  {
	//blocking mode
	_blocking = true;
	_timeout = -1.0; //reset
	set_blocking(_fd);
    } else {
	//non-blocking
	set_nonblocking(_fd);
	_blocking = false;
    }
    return this;
}

socket *socket::settimeout(double val)
{
    _blocking = true;
    _timeout = val;
    return this;
}

socket *socket::shutdown(int how)
{
    if (::shutdown(_fd, how) == SOCKET_ERROR)
	throw new error(make_errstring("shutdown"));
    return this;
}

void socket::write_wait()
{
    if (_blocking && _timeout >= 0) {
	fd_set s;
	FD_ZERO(&s);
	FD_SET(_fd, &s);
	timeval to;
	to.tv_sec = static_cast<tv_sec_type>(_timeout);
	to.tv_usec = static_cast<tv_usec_type>(1000000 * (_timeout - (double)to.tv_sec));
	if (::select(_fd+1, 0, &s, 0, &to) == SOCKET_ERROR)
	    throw new error(make_errstring("select"));
	if (! FD_ISSET(_fd, &s))
	    throw new timeout(timed_out);
    }
}

int socket::send(const char *s, size_t len, int flags)
{
    write_wait();

    ssize_t r = ::send(_fd, s, len, flags);
    if (r == SOCKET_ERROR)
	throw new error(make_errstring("send"));
    return r;
}

int socket::send(str *string, int flags) {
    const char *s = string->unit.c_str();
    return send( s, strlen(s), flags );
}

int socket::sendall(str *string, int flags) {
    const char *s = string->unit.c_str();
    size_t offset = 0;
    size_t len = strlen(s); //FIXME doesn't allow for nul chars in the input

    while (offset < len)
	offset += send(s + offset, len - offset, flags);
    return len;
}

int socket::sendto(str* msg, int flags, socket::inet_address addr)
{
    write_wait();

    const char *buf = msg->unit.c_str();
    size_t buflen = strlen(buf);

    sockaddr *sa;
    socklen_t salen;

    //FIXME hardcoded for AF_INET
    sockaddr_in sin;
    sa = reinterpret_cast<sockaddr *>(&sin);
    salen = sizeof(sin);

    tuple_to_sin_addr(&sin, addr);

    ssize_t len = ::sendto(_fd, buf, buflen, flags, sa, salen);
    if (len == SOCKET_ERROR)
	throw new error(make_errstring("sendto"));

    return len;
}

socket *socket::close()
{
    if (::CLOSE(_fd) == SOCKET_ERROR)
#define STRINGIFY(x) #x
	throw new error(make_errstring(STRINGIFY(CLOSE)));
#undef STRINGIFY
    return this;
}

void socket::read_wait()
{
    if (_blocking && _timeout >= 0) {
	fd_set s;
	FD_ZERO(&s);
	FD_SET(_fd, &s);
	timeval to;
	to.tv_sec = static_cast<tv_sec_type>(_timeout);
	to.tv_usec = static_cast<tv_usec_type>(1000000 * (_timeout - (double)to.tv_sec));
	if (::select(_fd+1, &s, 0, 0, &to) == SOCKET_ERROR)
	    throw new error(make_errstring("select"));
	if (! FD_ISSET(_fd, &s))
	    throw new timeout(timed_out);
    }
}

str *socket::recv(int bufsize, int flags)
{
    read_wait();

    char buf[bufsize];
    ssize_t len = ::recv(_fd, buf, bufsize, flags);
    if (len == SOCKET_ERROR)
	throw new error(make_errstring("recv"));
    return new str(buf, len);
}

#ifdef WIN32
void inet_ntop(int proto, const in_addr *addr, char *dst, size_t len)
{
	int v = ntohl(addr->s_addr);
	sprintf(dst, "%d.%d.%d.%d", ((v>> 24) & 0xff) ,((v >> 16) & 0xff) ,((v >> 8) & 0xff) ,((v) & 0xff));
}
#endif

static socket::inet_address sin_addr_to_tuple(const sockaddr_in *sin)
{
    char ip[sizeof("xxx.xxx.xxx.xxx")];
    inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
    socket::inet_address addr = new tuple2<str *, int>(2, new str(ip), static_cast<int>(ntohs(sin->sin_port)));
    return addr;
}

ssize_t socket::recvfrom(char *buf, size_t bufsize, int flags, sockaddr *sa, socklen_t *salen)
{
    read_wait();
    ssize_t len = ::recvfrom(_fd, buf, bufsize, flags, sa, salen);
    if (len == SOCKET_ERROR)
	throw new error(make_errstring("recvfrom"));
    return len;
}

tuple2<str *, socket::inet_address> *socket::recvfrom(int bufsize, int flags)
{
    char buf[bufsize];
    struct sockaddr_in sin;
    socklen_t salen = sizeof(sin);
    ssize_t len = recvfrom(buf, bufsize, flags, reinterpret_cast<sockaddr *>(&sin), &salen);
    return new tuple2<str *, inet_address>(2, new str(buf, len), sin_addr_to_tuple(&sin));
}

socket::socket(int family, int type, int proto) {
    this->__class__ = cl_socket;
    
    this->family = family;
    this->type = type;
    this->proto = proto;
    _fd = ::socket(family, type, proto);
    if (_fd == SOCKET_ERROR)
	throw new error(make_errstring("socket"));
    _timeout = __ss_default_timeout;
    _blocking = true;
}

socket::~socket()
{
    ::CLOSE(_fd); // ignore errror since we can't throw
}

socket *socket::listen(int backlog)
{
    if(::listen(_fd, backlog) == SOCKET_ERROR)
	throw new error(make_errstring("listen"));
    return this;
}

socket* socket::accept(sockaddr *sa, socklen_t *salen)
{
    if (_blocking && _timeout >= 0) {
	fd_set s;
	FD_ZERO(&s);
	FD_SET(_fd, &s);
	timeval to;
	to.tv_sec = static_cast<tv_sec_type>(_timeout);
	to.tv_usec = static_cast<tv_usec_type>(1000000 * (_timeout - (double)to.tv_sec));
	if (::select(_fd+1, &s, 0, 0, &to) == SOCKET_ERROR)
	    throw new error(make_errstring("select"));
	if (! FD_ISSET(_fd, &s))
	    throw new timeout(timed_out);
    }
    int r;
    if ((r = ::accept(_fd, sa, salen)) == SOCKET_ERROR) {
	throw new error(make_errstring("accept"));
    }
    socket *sock = new socket();
    sock->family = family;
    sock->proto = proto;
    sock->type = type;
    sock->_fd = r;
    return sock;
}

#if 0
// UNIX sockets
tuple2<socket *, pyseq<str *> *> *socket::accept()
{
    sockaddr_un sun;
    socklen_t sunsize = sizeof(sun);

    socket *sock = accept(reinterpret_cast<sockaddr *>(&sun), &sunsize);
    str* addr = new str(sun.sun_path);
    return new tuple2<socket *, pyseq<str *> *>(2, sock, addr);
}
#endif

// INET sockets
tuple2<socket *, socket::inet_address> *socket::accept() {
    sockaddr_in sin;
    socklen_t sinsize = sizeof(sin);
    socket *sock = accept(reinterpret_cast<sockaddr *>(&sin), &sinsize);
    return new tuple2<socket *, inet_address>( 2, sock, sin_addr_to_tuple(&sin));
}

#ifndef WIN32
socket *socket::bind(pyseq<str *> *address)
{
    if (family != AF_UNIX)
	throw new ValueError(invalid_address);
    sockaddr_un sun;
    sun.sun_family = AF_UNIX;
    const str* __0 = address->__getitem__(0);
    strncpy(sun.sun_path, __0->unit.c_str(), PATH_MAX);

    return bind(reinterpret_cast<sockaddr *>(&sun), sizeof(sun));
}
#endif /* ! WIN32 */

socket::inet_address socket::getpeername()
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (::getpeername(_fd, reinterpret_cast<sockaddr *>(&addr), &addrlen) == SOCKET_ERROR)
	throw new error(make_errstring("getpeername"));
    return sin_addr_to_tuple(&addr);
}

socket::inet_address socket::getsockname()
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (::getsockname(_fd, reinterpret_cast<sockaddr *>(&addr), &addrlen) == SOCKET_ERROR)
	throw new error(make_errstring("getsockname"));
    return sin_addr_to_tuple(&addr);
}

str *gethostname()
{
    char name[HOST_NAME_MAX];
    if (::gethostname(name, sizeof(name)) == -1)
	throw new herror(make_errstring("gethostname"));
    return new str(name);
}

int _ss_htonl(int x) {
    return htonl(x);
}

int _ss_htons(int x) {
    return htons(x);
}

int _ss_ntohl(int x) {
    return ntohl(x);
}

int _ss_ntohs(int x) {
    return ntohs(x);
}

//FIXME this should return None when no timeout is set
double getdefaulttimeout()
{
    if (__ss_default_timeout < 0)
	throw new error(new str("no timeout is set"));
    return __ss_default_timeout;
}

// FIXME this should allow the argument to be None
int setdefaulttimeout(double x)
{
    if (x < 0)
	throw new ValueError(new str("invalid argument"));
    __ss_default_timeout = x;
    return 0;
}

void __init()
{
    __name__ = new str("socket");

    cl_socket = new class_("socket", 33, 33);
    cl_herror = new class_("herror", 13, 13);
    cl_gaierror = new class_("gaierror", 14, 14);
    cl_timeout = new class_("timeout", 15, 15);
    cl_error = new class_("error", 16, 16);

    // string constants used by this module
    invalid_address = new str("invalid address");
    timed_out = new str("timed out");
    host_not_found = new str("host not found");

#ifdef WIN32
    int iResult;
    WSADATA wsaData;

    // Initialize Winsock
    iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
	throw new error(new str("WSAStartup failed"));
    }
#endif /* WIN32 */
}

void __exit()
{
#ifdef WIN32
    //FIXME
    // should call winsock finalization routine, but __exit() doesn't get called
    // except for the builtin module
#endif
}

str *gethostbyname(str *hostname)
{
    hostent *he = ::gethostbyname(hostname->unit.c_str());
    if (!he)
	throw new herror(host_not_found);
    char ip[sizeof("xxx.xxx.xxx.xxx")];
    int addr = htonl( *((int *) he->h_addr_list[0]) );
    sprintf(ip, "%d.%d.%d.%d", ((addr >> 24) & 0xff), ((addr >> 16) & 0xff), ((addr >> 8) & 0xff), (addr & 0xff));
    return new str(ip);
}

str *inet_aton(str *x)
{
    int addr = string_to_addr(x->unit.c_str());
    return new str((char *) &addr, 4);
}

str *inet_ntoa(str *x)
{
    const char *s = x->unit.c_str();
    int addr = *((int *) s);
    char ip[sizeof("xxx.xxx.xxx.xxx")];
    sprintf(ip, "%d.%d.%d.%d", ((addr >> 24) & 0xff), ((addr >> 16) & 0xff), ((addr >> 8) & 0xff), (addr & 0xff));
    return new str(ip);
}

int has_ipv6()
{
    return 0;
}

} // module namespace
