/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/connection_tcp.h"

#include "base/bytes.h"
#include "base/openssl_help.h"
#include "base/qthelp_url.h"
#include <openssl/aes.h>

namespace MTP {
namespace internal {
namespace {

constexpr auto kMinReceiveTimeout = TimeMs(2000);
constexpr auto kMaxReceiveTimeout = TimeMs(8000);

uint32 tcpPacketSize(const char *packet) { // must have at least 4 bytes readable
	uint32 result = (packet[0] > 0) ? packet[0] : 0;
	if (result == 0x7f) {
		const uchar *bytes = reinterpret_cast<const uchar*>(packet);
		result = (((uint32(bytes[3]) << 8) | uint32(bytes[2])) << 8) | uint32(bytes[1]);
		return (result << 2) + 4;
	}
	return (result << 2) + 1;
}

} // namespace

AbstractTCPConnection::AbstractTCPConnection(
	QThread *thread)
: AbstractConnection(thread)
, currentPos((char*)shortBuffer) {
}

void AbstractTCPConnection::setProxyOverride(const ProxyData &proxy) {
	sock.setProxy(ToNetworkProxy(proxy));
}

AbstractTCPConnection::~AbstractTCPConnection() = default;

void AbstractTCPConnection::socketRead() {
	if (sock.state() != QAbstractSocket::ConnectedState) {
		LOG(("MTP error: socket not connected in socketRead(), state: %1").arg(sock.state()));
		emit error(kErrorCodeOther);
		return;
	}

	do {
		uint32 toRead = packetLeft ? packetLeft : (readingToShort ? (MTPShortBufferSize * sizeof(mtpPrime) - packetRead) : 4);
		if (readingToShort) {
			if (currentPos + toRead > ((char*)shortBuffer) + MTPShortBufferSize * sizeof(mtpPrime)) {
				longBuffer.resize(((packetRead + toRead) >> 2) + 1);
				memcpy(&longBuffer[0], shortBuffer, packetRead);
				currentPos = ((char*)&longBuffer[0]) + packetRead;
				readingToShort = false;
			}
		} else {
			if (longBuffer.size() * sizeof(mtpPrime) < packetRead + toRead) {
				longBuffer.resize(((packetRead + toRead) >> 2) + 1);
				currentPos = ((char*)&longBuffer[0]) + packetRead;
			}
		}
		int32 bytes = (int32)sock.read(currentPos, toRead);
		if (bytes > 0) {
			aesCtrEncrypt(currentPos, bytes, _receiveKey, &_receiveState);
			TCP_LOG(("TCP Info: read %1 bytes").arg(bytes));

			packetRead += bytes;
			currentPos += bytes;
			if (packetLeft) {
				packetLeft -= bytes;
				if (!packetLeft) {
					socketPacket(currentPos - packetRead, packetRead);
					currentPos = (char*)shortBuffer;
					packetRead = packetLeft = 0;
					readingToShort = true;
					longBuffer.clear();
				} else {
					TCP_LOG(("TCP Info: not enough %1 for packet! read %2").arg(packetLeft).arg(packetRead));
					emit receivedSome();
				}
			} else {
				bool move = false;
				while (packetRead >= 4) {
					uint32 packetSize = tcpPacketSize(currentPos - packetRead);
					if (packetSize < 5 || packetSize > MTPPacketSizeMax) {
						LOG(("TCP Error: packet size = %1").arg(packetSize));
						emit error(kErrorCodeOther);
						return;
					}
					if (packetRead >= packetSize) {
						socketPacket(currentPos - packetRead, packetSize);
						packetRead -= packetSize;
						packetLeft = 0;
						move = true;
					} else {
						packetLeft = packetSize - packetRead;
						TCP_LOG(("TCP Info: not enough %1 for packet! size %2 read %3").arg(packetLeft).arg(packetSize).arg(packetRead));
						emit receivedSome();
						break;
					}
				}
				if (move) {
					if (!packetRead) {
						currentPos = (char*)shortBuffer;
						readingToShort = true;
						longBuffer.clear();
					} else if (!readingToShort && packetRead < MTPShortBufferSize * sizeof(mtpPrime)) {
						memcpy(shortBuffer, currentPos - packetRead, packetRead);
						currentPos = (char*)shortBuffer + packetRead;
						readingToShort = true;
						longBuffer.clear();
					}
				}
			}
		} else if (bytes < 0) {
			LOG(("TCP Error: socket read return -1"));
			emit error(kErrorCodeOther);
			return;
		} else {
			TCP_LOG(("TCP Info: no bytes read, but bytes available was true..."));
			break;
		}
	} while (sock.state() == QAbstractSocket::ConnectedState && sock.bytesAvailable());
}

mtpBuffer AbstractTCPConnection::handleResponse(const char *packet, uint32 length) {
	if (length < 5 || length > MTPPacketSizeMax) {
		LOG(("TCP Error: bad packet size %1").arg(length));
		return mtpBuffer(1, -500);
	}
	int32 size = packet[0], len = length - 1;
	if (size == 0x7f) {
		const uchar *bytes = reinterpret_cast<const uchar*>(packet);
		size = (((uint32(bytes[3]) << 8) | uint32(bytes[2])) << 8) | uint32(bytes[1]);
		len -= 3;
	}
	if (size * int32(sizeof(mtpPrime)) != len) {
		LOG(("TCP Error: bad packet header"));
		TCP_LOG(("TCP Error: bad packet header, packet: %1").arg(Logs::mb(packet, length).str()));
		return mtpBuffer(1, -500);
	}
	const mtpPrime *packetdata = reinterpret_cast<const mtpPrime*>(packet + (length - len));
	TCP_LOG(("TCP Info: packet received, size = %1").arg(size * sizeof(mtpPrime)));
	if (size == 1) {
		LOG(("TCP Error: error packet received, code = %1").arg(*packetdata));
		return mtpBuffer(1, *packetdata);
	}

	mtpBuffer data(size);
	memcpy(data.data(), packetdata, size * sizeof(mtpPrime));

	return data;
}

void AbstractTCPConnection::handleError(QAbstractSocket::SocketError e, QTcpSocket &sock) {
	switch (e) {
	case QAbstractSocket::ConnectionRefusedError:
	LOG(("TCP Error: socket connection refused - %1").arg(sock.errorString()));
	break;

	case QAbstractSocket::RemoteHostClosedError:
	TCP_LOG(("TCP Info: remote host closed socket connection - %1").arg(sock.errorString()));
	break;

	case QAbstractSocket::HostNotFoundError:
	LOG(("TCP Error: host not found - %1").arg(sock.errorString()));
	break;

	case QAbstractSocket::SocketTimeoutError:
	LOG(("TCP Error: socket timeout - %1").arg(sock.errorString()));
	break;

	case QAbstractSocket::NetworkError:
	LOG(("TCP Error: network - %1").arg(sock.errorString()));
	break;

	case QAbstractSocket::ProxyAuthenticationRequiredError:
	case QAbstractSocket::ProxyConnectionRefusedError:
	case QAbstractSocket::ProxyConnectionClosedError:
	case QAbstractSocket::ProxyConnectionTimeoutError:
	case QAbstractSocket::ProxyNotFoundError:
	case QAbstractSocket::ProxyProtocolError:
	LOG(("TCP Error: proxy (%1) - %2").arg(e).arg(sock.errorString()));
	break;

	default:
	LOG(("TCP Error: other (%1) - %2").arg(e).arg(sock.errorString()));
	break;
	}

	TCP_LOG(("TCP Error %1, restarting! - %2").arg(e).arg(sock.errorString()));
}

TCPConnection::TCPConnection(QThread *thread)
: AbstractTCPConnection(thread)
, status(WaitingTcp)
, tcpNonce(rand_value<MTPint128>())
, _tcpTimeout(kMinReceiveTimeout) {
	tcpTimeoutTimer.moveToThread(thread);
	tcpTimeoutTimer.setSingleShot(true);
	connect(&tcpTimeoutTimer, SIGNAL(timeout()), this, SLOT(onTcpTimeoutTimer()));

	sock.moveToThread(thread);
	connect(&sock, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(socketError(QAbstractSocket::SocketError)));
	connect(&sock, SIGNAL(connected()), this, SLOT(onSocketConnected()));
	connect(&sock, SIGNAL(disconnected()), this, SLOT(onSocketDisconnected()));
}

void TCPConnection::onSocketConnected() {
	if (status == WaitingTcp) {
		mtpBuffer buffer(preparePQFake(tcpNonce));

		DEBUG_LOG(("Connection Info: sending fake req_pq through TCP transport to %1").arg(_address));

		if (_tcpTimeout < 0) _tcpTimeout = -_tcpTimeout;
		tcpTimeoutTimer.start(_tcpTimeout);

		_pingTime = getms();
		sendData(buffer);
	}
}

void TCPConnection::onTcpTimeoutTimer() {
	if (status == WaitingTcp) {
		if (_tcpTimeout < kMaxReceiveTimeout) {
			_tcpTimeout *= 2;
		}
		_tcpTimeout = -_tcpTimeout;

		QAbstractSocket::SocketState state = sock.state();
		if (state == QAbstractSocket::ConnectedState || state == QAbstractSocket::ConnectingState || state == QAbstractSocket::HostLookupState) {
			sock.disconnectFromHost();
		} else if (state != QAbstractSocket::ClosingState) {
			sock.connectToHost(QHostAddress(_address), _port);
		}
	}
}

void TCPConnection::onSocketDisconnected() {
	if (_tcpTimeout < 0) {
		_tcpTimeout = -_tcpTimeout;
		if (status == WaitingTcp) {
			sock.connectToHost(QHostAddress(_address), _port);
			return;
		}
	}
	if (status == WaitingTcp || status == UsingTcp) {
		emit disconnected();
	}
}

void TCPConnection::sendData(mtpBuffer &buffer) {
	if (status == FinishedWork) return;

	if (buffer.size() < 3) {
		LOG(("TCP Error: writing bad packet, len = %1").arg(buffer.size() * sizeof(mtpPrime)));
		TCP_LOG(("TCP Error: bad packet %1").arg(Logs::mb(&buffer[0], buffer.size() * sizeof(mtpPrime)).str()));
		emit error(kErrorCodeOther);
		return;
	}

	tcpSend(buffer);
}

void AbstractTCPConnection::writeConnectionStart() {
	// prepare random part
	auto nonceBytes = bytes::vector(64);
	const auto nonce = bytes::make_span(nonceBytes);

	const auto zero = reinterpret_cast<uchar*>(nonce.data());
	const auto first = reinterpret_cast<uint32*>(nonce.data());
	const auto second = first + 1;
	const auto reserved01 = 0x000000EFU;
	const auto reserved11 = 0x44414548U;
	const auto reserved12 = 0x54534F50U;
	const auto reserved13 = 0x20544547U;
	const auto reserved14 = 0x20544547U;
	const auto reserved15 = 0xEEEEEEEEU;
	const auto reserved21 = 0x00000000U;
	do {
		bytes::set_random(nonce);
	} while (*zero == reserved01
		|| *first == reserved11
		|| *first == reserved12
		|| *first == reserved13
		|| *first == reserved14
		|| *first == reserved15
		|| *second == reserved21);

	const auto prepareKey = [&](bytes::span key, bytes::const_span from) {
		if (_protocolSecret.size() == 16) {
			const auto payload = bytes::concatenate(from, _protocolSecret);
			bytes::copy(key, openssl::Sha256(payload));
		} else if (_protocolSecret.empty()) {
			bytes::copy(key, from);
		} else {
			bytes::set_with_const(key, gsl::byte{});
		}
	};

	// prepare encryption key/iv
	prepareKey(
		bytes::make_span(_sendKey),
		nonce.subspan(8, CTRState::KeySize));
	bytes::copy(
		bytes::make_span(_sendState.ivec),
		nonce.subspan(8 + CTRState::KeySize, CTRState::IvecSize));

	// prepare decryption key/iv
	auto reversedBytes = bytes::vector(48);
	const auto reversed = bytes::make_span(reversedBytes);
	bytes::copy(reversed, nonce.subspan(8, reversed.size()));
	std::reverse(reversed.begin(), reversed.end());
	prepareKey(
		bytes::make_span(_receiveKey),
		reversed.subspan(0, CTRState::KeySize));
	bytes::copy(
		bytes::make_span(_receiveState.ivec),
		reversed.subspan(CTRState::KeySize, CTRState::IvecSize));

	// write protocol and dc ids
	const auto protocol = reinterpret_cast<uint32*>(nonce.data() + 56);
	*protocol = 0xEFEFEFEFU;
	const auto dcId = reinterpret_cast<int16*>(nonce.data() + 60);
	*dcId = _protocolDcId;

	sock.write(reinterpret_cast<const char*>(nonce.data()), 56);
	aesCtrEncrypt(nonce.data(), 64, _sendKey, &_sendState);
	sock.write(reinterpret_cast<const char*>(nonce.subspan(56).data()), 8);
}

void AbstractTCPConnection::tcpSend(mtpBuffer &buffer) {
	if (!packetNum) {
		writeConnectionStart();
	}
	++packetNum;

	uint32 size = buffer.size() - 3, len = size * 4;
	char *data = reinterpret_cast<char*>(&buffer[0]);
	if (size < 0x7f) {
		data[7] = char(size);
		TCP_LOG(("TCP Info: write %1 packet %2").arg(packetNum).arg(len + 1));

		aesCtrEncrypt(data + 7, len + 1, _sendKey, &_sendState);
		sock.write(data + 7, len + 1);
	} else {
		data[4] = 0x7f;
		reinterpret_cast<uchar*>(data)[5] = uchar(size & 0xFF);
		reinterpret_cast<uchar*>(data)[6] = uchar((size >> 8) & 0xFF);
		reinterpret_cast<uchar*>(data)[7] = uchar((size >> 16) & 0xFF);
		TCP_LOG(("TCP Info: write %1 packet %2").arg(packetNum).arg(len + 4));

		aesCtrEncrypt(data + 4, len + 4, _sendKey, &_sendState);
		sock.write(data + 4, len + 4);
	}
}

void TCPConnection::disconnectFromServer() {
	if (status == FinishedWork) return;
	status = FinishedWork;

	disconnect(&sock, SIGNAL(readyRead()), 0, 0);
	sock.close();
}

void TCPConnection::connectToServer(
		const QString &ip,
		int port,
		const bytes::vector &protocolSecret,
		int16 protocolDcId) {
	_address = ip;
	_port = port;
	_protocolSecret = protocolSecret;
	_protocolDcId = protocolDcId;

	connect(&sock, SIGNAL(readyRead()), this, SLOT(socketRead()));
	sock.connectToHost(QHostAddress(_address), _port);
}

TimeMs TCPConnection::pingTime() const {
	return isConnected() ? _pingTime : TimeMs(0);
}

void TCPConnection::socketPacket(const char *packet, uint32 length) {
	if (status == FinishedWork) return;

	mtpBuffer data = handleResponse(packet, length);
	if (data.size() == 1) {
		emit error(data[0]);
	} else if (status == UsingTcp) {
		_receivedQueue.push_back(data);
		emit receivedData();
	} else if (status == WaitingTcp) {
		tcpTimeoutTimer.stop();
		try {
			auto res_pq = readPQFakeReply(data);
			const auto &res_pq_data(res_pq.c_resPQ());
			if (res_pq_data.vnonce == tcpNonce) {
				DEBUG_LOG(("Connection Info: TCP-transport to %1 chosen by pq-response").arg(_address));
				status = UsingTcp;
				_pingTime = (getms() - _pingTime);
				emit connected();
			}
		} catch (Exception &e) {
			DEBUG_LOG(("Connection Error: exception in parsing TCP fake pq-responce, %1").arg(e.what()));
			emit error(kErrorCodeOther);
		}
	}
}

bool TCPConnection::isConnected() const {
	return (status == UsingTcp);
}

int32 TCPConnection::debugState() const {
	return sock.state();
}

QString TCPConnection::transport() const {
	if (!isConnected()) {
		return QString();
	}
	auto result = qsl("TCP");
	if (qthelp::is_ipv6(_address)) {
		result += qsl("/IPv6");
	}
	return result;
}

QString TCPConnection::tag() const {
	auto result = qsl("TCP");
	if (qthelp::is_ipv6(_address)) {
		result += qsl("/IPv6");
	} else {
		result += qsl("/IPv4");
	}
	return result;
}

void TCPConnection::socketError(QAbstractSocket::SocketError e) {
	if (status == FinishedWork) return;

	handleError(e, sock);
	emit error(kErrorCodeOther);
}

} // namespace internal
} // namespace MTP
