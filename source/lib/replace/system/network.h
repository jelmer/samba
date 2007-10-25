#ifndef _system_network_h
#define _system_network_h
/* 
   Unix SMB/CIFS implementation.

   networking system include wrappers

   Copyright (C) Andrew Tridgell 2004
   
     ** NOTE! The following LGPL license applies to the replace
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.

*/

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_UNIXSOCKET
#include <sys/un.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

/*
 * The next three defines are needed to access the IPTOS_* options
 * on some systems.
 */

#ifdef HAVE_NETINET_IN_SYSTM_H
#include <netinet/in_systm.h>
#endif

#ifdef HAVE_NETINET_IN_IP_H
#include <netinet/in_ip.h>
#endif

#ifdef HAVE_NETINET_IP_H
#include <netinet/ip.h>
#endif

#ifdef HAVE_NET_IF_H
#include <net/if.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef SOCKET_WRAPPER
#ifndef SOCKET_WRAPPER_NOT_REPLACE
#define SOCKET_WRAPPER_REPLACE
#endif
#include "lib/socket_wrapper/socket_wrapper.h"
#endif

#ifdef REPLACE_INET_NTOA
/* define is in "replace.h" */
char *rep_inet_ntoa(struct in_addr ip);
#endif

#ifndef HAVE_INET_PTON
/* define is in "replace.h" */
int rep_inet_pton(int af, const char *src, void *dst);
#endif

#ifndef HAVE_INET_NTOP
/* define is in "replace.h" */
const char *rep_inet_ntop(int af, const void *src, char *dst, socklen_t size);
#endif

#if !defined(HAVE_GETADDRINFO)
#include "getaddrinfo.h"
#endif

/*
 * Some systems have getaddrinfo but not the
 * defines needed to use it.
 */

/* Various macros that ought to be in <netdb.h>, but might not be */

#ifndef EAI_FAIL
#define EAI_BADFLAGS	(-1)
#define EAI_NONAME	(-2)
#define EAI_AGAIN	(-3)
#define EAI_FAIL	(-4)
#define EAI_FAMILY	(-6)
#define EAI_SOCKTYPE	(-7)
#define EAI_SERVICE	(-8)
#define EAI_MEMORY	(-10)
#define EAI_SYSTEM	(-11)
#endif   /* !EAI_FAIL */

#ifndef AI_PASSIVE
#define AI_PASSIVE	0x0001
#endif

#ifndef AI_CANONNAME
#define AI_CANONNAME	0x0002
#endif

#ifndef AI_NUMERICHOST
/*
 * some platforms don't support AI_NUMERICHOST; define as zero if using
 * the system version of getaddrinfo...
 */
#if defined(HAVE_STRUCT_ADDRINFO) && defined(HAVE_GETADDRINFO)
#define AI_NUMERICHOST	0
#else
#define AI_NUMERICHOST	0x0004
#endif
#endif

#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG	0x0020
#endif

#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST	1
#endif

#ifndef NI_NUMERICSERV
#define NI_NUMERICSERV	2
#endif

#ifndef NI_NOFQDN
#define NI_NOFQDN	4
#endif

#ifndef NI_NAMEREQD
#define NI_NAMEREQD 	8
#endif

#ifndef NI_DGRAM
#define NI_DGRAM	16
#endif


#ifndef NI_MAXHOST
#define NI_MAXHOST	1025
#endif

#ifndef NI_MAXSERV
#define NI_MAXSERV	32
#endif

#ifndef HAVE_STRUCT_ADDRINFO

struct addrinfo
{
	int			ai_flags;
	int			ai_family;
	int			ai_socktype;
	int			ai_protocol;
	size_t		ai_addrlen;
	struct sockaddr *ai_addr;
	char	   *ai_canonname;
	struct addrinfo *ai_next;
};
#endif   /* HAVE_STRUCT_ADDRINFO */

/*
 * glibc on linux doesn't seem to have MSG_WAITALL
 * defined. I think the kernel has it though..
 */
#ifndef MSG_WAITALL
#define MSG_WAITALL 0
#endif

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001
#endif

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT EINVAL
#endif

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

#ifndef HAVE_SOCKLEN_T
typedef int socklen_t;
#endif

#ifndef HAVE_SA_FAMILY_T
typedef unsigned short int sa_family_t;
#endif

#ifndef HAVE_STRUCT_SOCKADDR_STORAGE
#ifdef HAVE_STRUCT_SOCKADDR_IN6
#define sockaddr_storage sockaddr_in6
#define ss_family sin6_family
#else
#define sockaddr_storage sockaddr_in
#define ss_family sin_family
#endif
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif

#endif
