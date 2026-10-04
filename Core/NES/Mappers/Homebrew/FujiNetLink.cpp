#include "pch.h"
#include <chrono>
#include <cstring>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <cerrno>
	#include <fcntl.h>
	#include <netdb.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif

#include "NES/Mappers/Homebrew/FujiNetLink.h"
#include "Shared/MessageManager.h"

namespace {
#ifdef _WIN32
	using sock_t = SOCKET;
	using socklen_arg = int;
	int CloseSocket(sock_t s) { return ::closesocket(s); }
	int LastSocketError() { return ::WSAGetLastError(); }
	bool WouldBlock(int e) { return e == WSAEWOULDBLOCK; }
	bool InProgress(int e) { return e == WSAEWOULDBLOCK; }
	constexpr int SendFlags = 0;

	void InitSockets()
	{
		static const bool init = [] {
			WSADATA data;
			return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
		}();
		(void)init;
	}

	bool SetNonBlocking(sock_t s)
	{
		u_long on = 1;
		return ::ioctlsocket(s, FIONBIO, &on) == 0;
	}

	void ShutdownSocket(sock_t s) { ::shutdown(s, SD_BOTH); }
#else
	using sock_t = int;
	using socklen_arg = socklen_t;
	int CloseSocket(sock_t s) { return ::close(s); }
	int LastSocketError() { return errno; }
	bool WouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
	bool InProgress(int e) { return e == EINPROGRESS; }
	void InitSockets() {}

	//Linux suppresses SIGPIPE per call; macOS has no MSG_NOSIGNAL and does it
	//per socket instead (SO_NOSIGPIPE in Open()).
	#ifdef MSG_NOSIGNAL
	constexpr int SendFlags = MSG_NOSIGNAL;
	#else
	constexpr int SendFlags = 0;
	#endif

	bool SetNonBlocking(sock_t s)
	{
		int flags = ::fcntl(s, F_GETFL, 0);
		return flags != -1 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
	}

	void ShutdownSocket(sock_t s) { ::shutdown(s, SHUT_RDWR); }
#endif

	string SocketErrorText(int err)
	{
#ifdef _WIN32
		return "error " + std::to_string(err);
#else
		return std::strerror(err);
#endif
	}

	//Wait until the socket is readable (or writable, while connecting).
	int WaitFor(sock_t s, int64_t remainMs, bool forWrite)
	{
		if(remainMs <= 0) {
			return 0;
		}

		fd_set set;
		FD_ZERO(&set);
		FD_SET(s, &set);
		//Winsock reports a FAILED non-blocking connect in the exception set,
		//not the write set: without it a refused connection waits out the whole
		//connect timeout. POSIX reports it as writable, and SO_ERROR sees it
		//either way.
		fd_set exc = set;

		timeval tv = {};
		tv.tv_sec = (decltype(tv.tv_sec))(remainMs / 1000);
		tv.tv_usec = (decltype(tv.tv_usec))((remainMs % 1000) * 1000);

		return ::select((int)s + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, forWrite ? &exc : nullptr, &tv);
	}

	int64_t MsSince(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	}
}

FujiNetLink::~FujiNetLink()
{
	Close();
}

bool FujiNetLink::Fail(const string& what)
{
	{
		std::lock_guard<std::mutex> lock(_errorLock);
		_lastError = what;
	}
	MessageManager::Log("[FujiNet] " + what);
	return false;
}

string FujiNetLink::GetLastError()
{
	std::lock_guard<std::mutex> lock(_errorLock);
	return _lastError;
}

bool FujiNetLink::Open(const string& host, int port)
{
	Close();
	InitSockets();

	string hostName = host.empty() ? "127.0.0.1" : host;
	string portName = std::to_string(port);
	string endpoint = hostName + ":" + portName;

	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo* results = nullptr;
	if(::getaddrinfo(hostName.c_str(), portName.c_str(), &hints, &results) != 0 || results == nullptr) {
		return Fail("cannot resolve " + endpoint);
	}

	int lastError = 0;
	intptr_t opened = Invalid;
	for(const addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
		sock_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if(s == (sock_t)Invalid) {
			continue;
		}

#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
		int on = 1;
		::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif

		if(!SetNonBlocking(s)) {
			CloseSocket(s);
			continue;
		}

		//A loopback listener answers at once or is not there. Windows retries a
		//refused connect for about two seconds before reporting it, which
		//would hold up every redial while FujiNet is not (yet) listening.
		bool loopback = false;
		if(ai->ai_family == AF_INET) {
			loopback = (ntohl(((const sockaddr_in*)ai->ai_addr)->sin_addr.s_addr) >> 24) == 127;
		} else if(ai->ai_family == AF_INET6) {
			loopback = IN6_IS_ADDR_LOOPBACK(&((const sockaddr_in6*)ai->ai_addr)->sin6_addr);
		}

		bool connected = ::connect(s, ai->ai_addr, (socklen_arg)ai->ai_addrlen) == 0;
		if(!connected && InProgress(LastSocketError())) {
			if(WaitFor(s, loopback ? LoopbackConnectTimeoutMs : ConnectTimeoutMs, true) > 0) {
				int err = 0;
				socklen_arg len = (socklen_arg)sizeof err;
				connected = ::getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0;
				if(!connected) {
					lastError = err;
				}
			}
		} else if(!connected) {
			lastError = LastSocketError();
		}

		if(connected) {
			int one = 1;
			::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
			opened = (intptr_t)s;
			break;
		}
		CloseSocket(s);
	}
	::freeaddrinfo(results);

	if(opened == Invalid) {
		return Fail("cannot connect to " + endpoint + (lastError ? " (" + SocketErrorText(lastError) + ")" : ""));
	}

	_rxPendLen = _rxPendPos = 0;
	{
		std::lock_guard<std::mutex> lock(_errorLock);
		_lastError.clear();
	}
	_socket.store(opened, std::memory_order_release);
	MessageManager::Log("[FujiNet] connected to " + endpoint);
	return true;
}

void FujiNetLink::Close()
{
	intptr_t s = _socket.exchange(Invalid, std::memory_order_acq_rel);
	if(s == Invalid) {
		return;
	}

	//Shut down before closing so a worker parked in select() wakes at once
	//rather than serving out a mount's 60-second deadline.
	ShutdownSocket((sock_t)s);
	CloseSocket((sock_t)s);
}

fb_status_t FujiNetLink::ReadFrame(size_t& outLen, uint32_t timeoutMs)
{
	auto start = std::chrono::steady_clock::now();
	size_t n = 0;
	int ends = 0;

	for(;;) {
		//Drain what the last recv() over-read first: one read can straddle two
		//frames, and the remainder belongs to the next one.
		while(_rxPendPos < _rxPendLen) {
			if(n >= _rxRaw.size()) {
				return FB_ETOOBIG;
			}

			uint8_t c = _rxPend[_rxPendPos++];
			_rxRaw[n++] = c;
			if(c == 0xC0 && ++ends == 2) {
				outLen = n;
				return FB_OK;
			}
		}

		intptr_t sock = _socket.load(std::memory_order_acquire);
		if(sock == Invalid) {
			return FB_ENOLINK;
		}

		sock_t s = (sock_t)sock;
		if(WaitFor(s, (int64_t)timeoutMs - MsSince(start), false) <= 0) {
			return FB_ETIMEOUT;
		}

		auto r = ::recv(s, (char*)_rxPend.data(), (int)_rxPend.size(), 0);
		if(r > 0) {
			_rxPendLen = (size_t)r;
			_rxPendPos = 0;
		} else if(r == 0 || !WouldBlock(LastSocketError())) {
			//fujinet-pc closed the connection, or Close() shut it down
			return FB_ENOLINK;
		}
	}
}

fb_status_t FujiNetLink::Transact(uint8_t device, uint8_t command, const fb_param_t* params, unsigned nparams,
                                  const uint8_t* payload, uint16_t payloadLen, uint32_t timeoutMs, fb_reply_t* reply)
{
	intptr_t sock = _socket.load(std::memory_order_acquire);
	if(sock == Invalid) {
		return FB_ENOLINK;
	}

	size_t reqLen = fujibus_build_request(device, command, params, nparams, payload, payloadLen, _txRaw.data(), _txRaw.size());
	if(reqLen == 0) {
		return FB_ETOOBIG;
	}

	if(::send((sock_t)sock, (const char*)_txRaw.data(), (int)reqLen, SendFlags) != (int)reqLen) {
		return FB_ENOLINK;
	}

	for(;;) {
		size_t rawLen = 0;
		fb_status_t status = ReadFrame(rawLen, timeoutMs);
		if(status != FB_OK) {
			return status;
		}

		if(!fujibus_parse_reply(_rxRaw.data(), rawLen, reply)) {
			return FB_EBADFRAME;
		}

		//Push frames arrive interleaved with the reply we are waiting for;
		//consuming one proves the link is alive, so the deadline restarts.
		if(!_inbound || !_inbound(*reply)) {
			return FB_OK;
		}
	}
}

void FujiNetLink::SendBare(uint8_t device, uint8_t command, const uint8_t* payload, uint16_t payloadLen)
{
	intptr_t sock = _socket.load(std::memory_order_acquire);
	if(sock == Invalid) {
		return;
	}

	size_t n = fujibus_build_request(device, command, nullptr, 0, payload, payloadLen, _txRaw.data(), _txRaw.size());
	if(n) {
		::send((sock_t)sock, (const char*)_txRaw.data(), (int)n, SendFlags);
	}
}
