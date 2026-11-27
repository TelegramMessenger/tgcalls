/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef TG_CALLS_RAW_TCP_SOCKET_H_
#define TG_CALLS_RAW_TCP_SOCKET_H_

#include <stddef.h>

#include <cstdint>
#include <memory>
#include <span>

#include "api/array_view.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/buffer.h"
#include "rtc_base/socket.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/async_tcp_socket.h"

namespace webrtc {

class RawTcpSocket : public AsyncTCPSocketBase {
 public:
  // Takes ownership of an already bound and connecting `socket`.
  explicit RawTcpSocket(std::unique_ptr<Socket> socket);
  ~RawTcpSocket() override {}

  RawTcpSocket(const RawTcpSocket&) = delete;
  RawTcpSocket& operator=(const RawTcpSocket&) = delete;

  int Send(const void* pv,
           size_t cb,
           const webrtc::AsyncSocketPacketOptions& options) override;
  size_t ProcessInput(std::span<const uint8_t> data) override;

 private:
  bool did_send_mtproto_prologue_ = false;
};

}  // namespace webrtc

#endif  // TG_CALLS_RAW_TCP_SOCKET_H_
