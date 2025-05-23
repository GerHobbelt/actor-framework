// This file is part of CAF, the C++ Actor Framework. See the file LICENSE in
// the main distribution directory for license terms and copyright or visit
// https://github.com/actor-framework/actor-framework/blob/main/LICENSE.

#include "caf/net/stream_socket.hpp"

#include "caf/net/socket.hpp"
#include "caf/net/socket_guard.hpp"

#include "caf/detail/assert.hpp"
#include "caf/detail/socket_sys_aliases.hpp"
#include "caf/expected.hpp"
#include "caf/internal/net_syscall.hpp"
#include "caf/internal/socket_sys_includes.hpp"
#include "caf/log/net.hpp"
#include "caf/span.hpp"

#include <cstddef>

#ifdef CAF_POSIX
#  include <sys/uio.h>
#endif

namespace caf::net {

#ifdef CAF_WINDOWS

constexpr int no_sigpipe_io_flag = 0;

// Based on work of others;
// original header:
//
// Copyright 2007, 2010 by Nathan C. Myers <ncm@cantrip.org>
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// The name of the author must not be used to endorse or promote products
// derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
expected<std::pair<stream_socket, stream_socket>> make_stream_socket_pair() {
  auto addrlen = static_cast<int>(sizeof(sockaddr_in));
  socket_id socks[2] = {invalid_socket_id, invalid_socket_id};
  CAF_NET_SYSCALL("WSASocket", listener, ==, invalid_socket_id,
                  WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                            WSA_FLAG_OVERLAPPED));
  union {
    sockaddr_in inaddr;
    sockaddr addr;
  } a;
  memset(&a, 0, sizeof(a));
  a.inaddr.sin_family = AF_INET;
  a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.inaddr.sin_port = 0;
  // makes sure all sockets are closed in case of an error
  auto guard = detail::scope_guard{[&]() noexcept {
    auto e = WSAGetLastError();
    close(socket{listener});
    close(socket{socks[0]});
    close(socket{socks[1]});
    WSASetLastError(e);
  }};
  // bind listener to a local port
  int reuse = 1;
  CAF_NET_SYSCALL("setsockopt", tmp1, !=, 0,
                  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                             reinterpret_cast<char*>(&reuse),
                             static_cast<int>(sizeof(reuse))));
  CAF_NET_SYSCALL("bind", tmp2, !=, 0,
                  bind(listener, &a.addr, static_cast<int>(sizeof(a.inaddr))));
  // Read the port in use: win32 getsockname may only set the port number
  // (http://msdn.microsoft.com/library/ms738543.aspx).
  memset(&a, 0, sizeof(a));
  CAF_NET_SYSCALL("getsockname", tmp3, !=, 0,
                  getsockname(listener, &a.addr, &addrlen));
  a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.inaddr.sin_family = AF_INET;
  // set listener to listen mode
  CAF_NET_SYSCALL("listen", tmp5, !=, 0, listen(listener, 1));
  // create read-only end of the pipe
  DWORD flags = WSA_FLAG_OVERLAPPED;
  CAF_NET_SYSCALL("WSASocketW", read_fd, ==, invalid_socket_id,
                  WSASocketW(AF_INET, SOCK_STREAM, 0, nullptr, 0, flags));
  CAF_NET_SYSCALL("connect", tmp6, !=, 0,
                  connect(read_fd, &a.addr,
                          static_cast<int>(sizeof(a.inaddr))));
  // get write-only end of the pipe
  CAF_NET_SYSCALL("accept", write_fd, ==, invalid_socket_id,
                  accept(listener, nullptr, nullptr));
  close(socket{listener});
  guard.disable();
  return std::make_pair(stream_socket{read_fd}, stream_socket{write_fd});
}

error keepalive(stream_socket x, bool new_value) {
  auto lg = log::net::trace("x = {}, new_value = {}", x, new_value);
  char value = new_value ? 1 : 0;
  CAF_NET_SYSCALL("setsockopt", res, !=, 0,
                  setsockopt(x.id, SOL_SOCKET, SO_KEEPALIVE, &value,
                             static_cast<int>(sizeof(value))));
  return none;
}

#else // CAF_WINDOWS

#  if defined(CAF_MACOS) || defined(CAF_IOS) || defined(CAF_BSD)
constexpr int no_sigpipe_io_flag = 0;
#  else
constexpr int no_sigpipe_io_flag = MSG_NOSIGNAL;
#  endif

expected<std::pair<stream_socket, stream_socket>> make_stream_socket_pair() {
  int sockets[2];
  CAF_NET_SYSCALL("socketpair", spair_res, !=, 0,
                  socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  return std::make_pair(stream_socket{sockets[0]}, stream_socket{sockets[1]});
}

error keepalive(stream_socket x, bool new_value) {
  auto lg = log::net::trace("x = {}, new_value = {}", x, new_value);
  int value = new_value ? 1 : 0;
  CAF_NET_SYSCALL("setsockopt", res, !=, 0,
                  setsockopt(x.id, SOL_SOCKET, SO_KEEPALIVE, &value,
                             static_cast<unsigned>(sizeof(value))));
  return none;
}

#endif // CAF_WINDOWS

error nodelay(stream_socket x, bool new_value) {
  auto lg = log::net::trace("x = {}, new_value = {}", x, new_value);
  int flag = new_value ? 1 : 0;
  CAF_NET_SYSCALL("setsockopt", res, !=, 0,
                  setsockopt(x.id, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<setsockopt_ptr>(&flag),
                             static_cast<socket_size_type>(sizeof(flag))));
  return none;
}

ptrdiff_t read(stream_socket x, byte_span buf) {
  auto lg = log::net::trace("socket = {}, bytes = {}", x.id, buf.size());
  return ::recv(x.id, reinterpret_cast<socket_recv_ptr>(buf.data()), buf.size(),
                no_sigpipe_io_flag);
}

ptrdiff_t write(stream_socket x, const_byte_span buf) {
  auto lg = log::net::trace("socket = {}, bytes = {}", x.id, buf.size());
  return ::send(x.id, reinterpret_cast<socket_send_ptr>(buf.data()), buf.size(),
                no_sigpipe_io_flag);
}

#ifdef CAF_WINDOWS

ptrdiff_t write(stream_socket x, std::initializer_list<const_byte_span> bufs) {
  CAF_ASSERT(bufs.size() < 10);
  WSABUF buf_array[10];
  auto convert = [](const_byte_span buf) {
    auto data = const_cast<std::byte*>(buf.data());
    return WSABUF{static_cast<ULONG>(buf.size()),
                  reinterpret_cast<CHAR*>(data)};
  };
  std::transform(bufs.begin(), bufs.end(), std::begin(buf_array), convert);
  DWORD bytes_sent = 0;
  auto res = WSASend(x.id, buf_array, static_cast<DWORD>(bufs.size()),
                     &bytes_sent, 0, nullptr, nullptr);
  return (res == 0) ? bytes_sent : -1;
}

#else // CAF_WINDOWS

ptrdiff_t write(stream_socket x, std::initializer_list<const_byte_span> bufs) {
  CAF_ASSERT(bufs.size() < 10);
  iovec buf_array[10];
  auto convert = [](const_byte_span buf) {
    return iovec{const_cast<std::byte*>(buf.data()), buf.size()};
  };
  std::transform(bufs.begin(), bufs.end(), std::begin(buf_array), convert);
  return writev(x.id, buf_array, static_cast<int>(bufs.size()));
}

#endif // CAF_WINDOWS

} // namespace caf::net
