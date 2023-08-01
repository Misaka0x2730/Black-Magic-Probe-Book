/*  tcpip - networking portability layer for Windows and Linux, limited to the
 *  functions that the GDB RSP needs
 *
 *  Copyright 2020-2023, CompuPhase
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may not
 *  use this file except in compliance with the License. You may obtain a copy
 *  of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *  License for the specific language governing permissions and limitations
 *  under the License.
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#if defined WIN32 || defined _WIN32
# define _WINSOCK_DEPRECATED_NO_WARNINGS
#else
# include <stdio.h>
# include <fcntl.h>
# include <ifaddrs.h>
# include <netdb.h>
# include <unistd.h>
# include <arpa/inet.h>
# define SOCKET_ERROR  (-1)
#endif
#include "bmp-scan.h"
#include "tcpip.h"


#if !defined sizearray
# define sizearray(a)   (sizeof(a) / sizeof((a)[0]))
#endif


static SOCKET GdbSocket = INVALID_SOCKET;


/** getlocalip() returns the IP address of the local host as a 32-bit
 *  integer, plus as a human-readable string. On failure, the function returns
 *  0xffffffff and an empty string.
 */
unsigned long getlocalip(char *ip_address)
{
# if defined WIN32 || defined _WIN32
    char name[80];
    struct hostent *phe;
# else
    struct ifaddrs *ifaddr, *ifa;
# endif
  int i;

  assert(ip_address != NULL);
  *ip_address = '\0';

# if defined WIN32 || defined _WIN32
    if (gethostname(name, sizeof(name)) == SOCKET_ERROR)
      return INADDR_NONE;
    phe = gethostbyname(name);
    if (phe == NULL)
      return INADDR_NONE;

    for (i = 0; phe->h_addr_list[i] != NULL; i++) {
      struct in_addr addr;
      char *ptr;
      memcpy(&addr, phe->h_addr_list[i], sizeof(struct in_addr));
      ptr = inet_ntoa(addr);
      assert(ptr != NULL);
      if (strcmp(ptr, "127.0.0.1") == 0 || strcmp(ptr, "::1") == 0)
        continue;   /* ignore loopback addresses */
      strcpy(ip_address, ptr);
      return addr.s_addr;
    }
# else
    if (getifaddrs(&ifaddr)==-1)
      return INADDR_NONE;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == NULL)
        continue;
      i = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), ip_address, 16, NULL, 0, NI_NUMERICHOST);
      if (i == 0
          && ifa->ifa_addr->sa_family == AF_INET
          && (strcmp(ifa->ifa_name, "eth0") == 0
              || (strncmp(ifa->ifa_name, "eno", 3) == 0 && isdigit(ifa->ifa_name[3]))
              || (strncmp(ifa->ifa_name, "ens", 3) == 0 && isdigit(ifa->ifa_name[3]))
              || (strncmp(ifa->ifa_name, "enp", 3) == 0 && isdigit(ifa->ifa_name[3]))))
      {
        freeifaddrs(ifaddr);
        return inet_addr(ip_address);
      }
    }

    freeifaddrs(ifaddr);
# endif

  return INADDR_NONE;
}

/** connect_timeout() is like connect(), but with a timeout in milliseconds. */
int connect_timeout(SOCKET sock,const char *host,short port,int timeout)
{
  struct sockaddr_in address;
  fd_set fdset;
  struct timeval tv;
# if defined _WIN32 || defined WIN32
    unsigned long mode = 1;
    typedef int socklen_t;
# endif

  address.sin_family = AF_INET;
  address.sin_addr.s_addr = inet_addr(host);
  address.sin_port = htons(port);

# if defined _WIN32 || defined WIN32
    /* FIONBIO  enables or disables the blocking mode for the socket;
       if mode == 0, blocking is enabled, if mode != 0, non-blocking mode is
       enabled */
    ioctlsocket(sock, FIONBIO, &mode);
# else
    fcntl(sock, F_SETFL, O_NONBLOCK);
# endif
  connect(sock, (struct sockaddr*)&address, sizeof(address));

  FD_ZERO(&fdset);
  FD_SET(sock, &fdset);
  tv.tv_sec = timeout/1000;
  tv.tv_usec = (timeout%1000)*1000;

  if (select(sock+1, NULL, &fdset, NULL, &tv) == 1) {
    int so_error;
    socklen_t len = sizeof so_error;
    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
    if (so_error == 0)
      return 0;
  }
  return -1;
}


#if defined WIN32 || defined _WIN32

static WSADATA wsa;

int tcpip_init(void)
{
  if (wsa.wVersion == 0 && wsa.wHighVersion == 0) {
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0)
      return WSAGetLastError();
  }
  return 0;
}

int tcpip_cleanup(void)
{
  if (wsa.wVersion != 0 || wsa.wHighVersion != 0) {
    WSACleanup();
    memset(&wsa, 0, sizeof wsa);
  }
  return 0;
}

#else

int tcpip_init(void)
{
  return 0;
}

int tcpip_cleanup(void)
{
  return 0;
}

int closesocket(SOCKET s)
{
  return close(s);
}

#endif /* __linux__ */

int tcpip_open(const char *ip_address)
{
  struct sockaddr_in server;
# if defined _WIN32 || defined WIN32
    unsigned long mode = 1;
# endif

  if ((GdbSocket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
#   if defined WIN32 || defined _WIN32
      return WSAGetLastError();
#   else
      return errno;
#   endif
  }

# if defined _WIN32 || defined WIN32
    /* FIONBIO  enables or disables the blocking mode for the socket;
       if mode == 0, blocking is enabled, if mode != 0, non-blocking mode is
       enabled */
    ioctlsocket(GdbSocket, FIONBIO, &mode);
# else
    fcntl(GdbSocket, F_SETFL, O_NONBLOCK);
# endif

  server.sin_addr.s_addr = inet_addr(ip_address);
  server.sin_family = AF_INET;
  server.sin_port = htons(BMP_PORT_GDB);
  if (connect(GdbSocket, (struct sockaddr*)&server, sizeof(server)) == 0)
    return 0;

  /* connection failed, close the socket and return an error code */
  closesocket(GdbSocket);
  GdbSocket = INVALID_SOCKET;
# if defined WIN32 || defined _WIN32
    return WSAGetLastError();
# else
    return errno;
# endif
}

int tcpip_close(void)
{
  if (tcpip_isopen()) {
    closesocket(GdbSocket);
    GdbSocket = INVALID_SOCKET;
  }
  return 0;
}

int tcpip_isopen(void)
{
  return GdbSocket != INVALID_SOCKET;
}

size_t tcpip_xmit(const unsigned char *buffer, size_t size)
{
  int result;

  assert(tcpip_isopen());
  result = send(GdbSocket, (const char*)buffer, size, 0);
  return (result >= 0) ? result : 0;
}

size_t tcpip_recv(unsigned char *buffer, size_t size)
{
  int result;

  assert(tcpip_isopen());
  result = recv(GdbSocket, (char*)buffer, size, 0);
  return (result >= 0) ? result : 0;
}

