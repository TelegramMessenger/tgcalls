/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "RawTcpSocket.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "api/array_view.h"
#include "rtc_base/byte_order.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/network/sent_packet.h"
#include "rtc_base/time_utils.h"  // for TimeMillis

#if defined(WEBRTC_POSIX)
#include <errno.h>
#endif  // WEBRTC_POSIX

#include <span>

namespace webrtc {

static const size_t kMaxPacketSize = 64 * 1024;

static const size_t kBufSize = kMaxPacketSize + 4;

// RawTcpSocket
// Binds and connects `socket` and creates RawTcpSocket for
// it. Takes ownership of `socket`. Returns null if bind() or
// connect() fail (`socket` is destroyed in that case).
RawTcpSocket* RawTcpSocket::Create(Socket* socket,
                                       const SocketAddress& bind_address,
                                       const SocketAddress& remote_address) {
  RTC_DCHECK(socket);
  if (socket->Bind(bind_address) < 0) {
    RTC_LOG(LS_ERROR) << "Bind() failed with error " << socket->GetError();
    delete socket;
    return nullptr;
  }
  if (socket->Connect(remote_address) < 0) {
    RTC_LOG(LS_ERROR) << "Connect() failed with error " << socket->GetError();
    delete socket;
    return nullptr;
  }
  return new RawTcpSocket(socket);
}

RawTcpSocket::RawTcpSocket(Socket* socket)
    : AsyncTCPSocketBase(std::unique_ptr<Socket>(socket), kBufSize) {}

int RawTcpSocket::Send(const void* pv,
                         size_t cb,
                         const AsyncSocketPacketOptions& options) {
  if (cb > kBufSize) {
    SetError(EMSGSIZE);
    return -1;
  }

  // If we are blocking on send, then silently drop this packet
  if (!IsOutBufferEmpty())
    return static_cast<int>(cb);
    
  if (!did_send_mtproto_prologue_) {
    did_send_mtproto_prologue_ = true;
    uint32_t prologue = 0xeeeeeeee;
    AppendToOutBuffer(&prologue, 4);
  }

  uint32_t pkt_len = (uint32_t)cb;
  AppendToOutBuffer(&pkt_len, 4);
  AppendToOutBuffer(pv, cb);

  int res = FlushOutBuffer();
  if (res <= 0) {
    // drop packet if we made no progress
    ClearOutBuffer();
    return res;
  }

  webrtc::SentPacketInfo sent_packet(options.packet_id, webrtc::TimeMillis(),
                              options.info_signaled_after_sent);
  CopySocketInformationToPacketInfo(cb, *this, &sent_packet.info);
  NotifySentPacket(this, sent_packet);

  // We claim to have sent the whole thing, even if we only sent partial
  return static_cast<int>(cb);
}

size_t RawTcpSocket::ProcessInput(std::span<const uint8_t> data) {
  SocketAddress remote_addr(GetRemoteAddress());

  size_t processed_bytes = 0;
  while (true) {
    size_t bytes_left = data.size() - processed_bytes;
    if (bytes_left < 4)
      return processed_bytes;

    uint32_t pkt_len =
        webrtc::GetLE32(data.subspan(processed_bytes, 4));
    if (bytes_left < 4 + pkt_len)
      return processed_bytes;

    webrtc::ReceivedIpPacket received_packet(
        data.subspan(processed_bytes + 4, pkt_len), remote_addr,
        webrtc::Timestamp::Micros(TimeMicros()));
    NotifyPacketReceived(received_packet);

    processed_bytes += 4 + pkt_len;
  }
}

}  // namespace rtc
