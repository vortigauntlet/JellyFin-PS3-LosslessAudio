// cspot's AP connection over sx_conn (plain TCP), replacing the vendored PlainConnection.cpp, which uses getaddrinfo and
// netinet/tcp.h.  Same contract: connect() throws, readBlock()/writeBlock() move whole blocks or throw, and a read that times
// out calls timeoutHandler (which returns true when the session is lost).
#include "PlainConnection.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdexcept>

#include "Logger.h"
#include "Packet.h"
#include "Utils.h"
#include "sx_conn.h"

using namespace cspot;

// apSock holds nothing here: the connection is kept in a side table keyed by this object, because the header (shared with
// the other targets) only has an int.  One AP connection exists at a time.
static sx_conn* s_conn = nullptr;

PlainConnection::PlainConnection() {
  this->apSock = -1;
}

PlainConnection::~PlainConnection() {
  this->close();
}

void PlainConnection::connect(const std::string& apAddress) {
  const size_t colon = apAddress.find(":");
  const std::string host = apAddress.substr(0, colon);
  const int port = colon == std::string::npos ? 4070 : atoi(apAddress.substr(colon + 1).c_str());
  sx_neterr err;
  memset(&err, 0, sizeof err);
  s_conn = sx_conn_open(host.c_str(), port, false, 10000, 0, 0, &err);
  if (!s_conn) {
    CSPOT_LOG(error, "AP connect failed (%d)", err.code);
    throw std::runtime_error("Can't connect to spotify servers");
  }
  this->apSock = 1;
  CSPOT_LOG(debug, "Connected to spotify server");
}

std::vector<uint8_t> PlainConnection::recvPacket() {
  std::vector<uint8_t> packetBuffer(4);
  readBlock(packetBuffer.data(), 4);
  uint32_t packetSize = ntohl(extract<uint32_t>(packetBuffer, 0));

  packetBuffer.resize(packetSize, 0);
  readBlock(packetBuffer.data() + 4, packetSize - 4);
  return packetBuffer;
}

std::vector<uint8_t> PlainConnection::sendPrefixPacket(const std::vector<uint8_t>& prefix,
                                                       const std::vector<uint8_t>& data) {
  uint32_t actualSize = prefix.size() + data.size() + sizeof(uint32_t);
  auto sizeRaw = pack<uint32_t>(htonl(actualSize));
  sizeRaw.insert(sizeRaw.begin(), prefix.begin(), prefix.end());
  sizeRaw.insert(sizeRaw.end(), data.begin(), data.end());
  writeBlock(sizeRaw);
  return sizeRaw;
}

void PlainConnection::readBlock(const uint8_t* dst, size_t size) {
  size_t idx = 0;
  while (idx < size) {
    if (!s_conn) throw std::runtime_error("Error in read");
    const int n = sx_conn_read(s_conn, (void*)&dst[idx], (int)(size - idx), 3000);
    if (n > 0) {
      idx += (size_t)n;
    } else if (n == SX_E_TIMEOUT) {
      if (timeoutHandler && timeoutHandler()) {
        CSPOT_LOG(error, "Connection lost, will need to reconnect...");
        throw std::runtime_error("Reconnection required");
      }
    } else {
      throw std::runtime_error("Error in read");     // closed by the peer, or a socket error
    }
  }
}

size_t PlainConnection::writeBlock(const std::vector<uint8_t>& data) {
  if (!s_conn) throw std::runtime_error("Error in write");
  const int n = sx_conn_write(s_conn, data.data(), (int)data.size(), 15000);
  if (n < 0) throw std::runtime_error("Error in write");
  return data.size();
}

void PlainConnection::close() {
  if (this->apSock < 0) return;
  CSPOT_LOG(info, "Closing socket...");
  sx_conn* c = s_conn;
  s_conn = nullptr;
  this->apSock = -1;
  if (c) sx_conn_close(c);
}
