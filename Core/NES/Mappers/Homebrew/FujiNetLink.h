#pragma once
#include "pch.h"
#include <array>
#include <mutex>
#include "NES/Mappers/Homebrew/FujiNet/fujins.h"
#include "NES/Mappers/Homebrew/FujiNet/fujibus.h"

// SLIP-framed FujiBus over a TCP socket, to fujinet-pc's "Bus over IP"
// listener. The transport half of the FujiNet cartridge; the protocol itself
// (fujimail, fujibus) is the cartridge firmware's own, vendored in FujiNet/.
// A port of the Stella fork's FujiNetLink, including the Windows fix for a
// refused non-blocking connect.
//
// Two facts about that listener shape everything here:
//  - It accepts ONE client (listen backlog 1). A stale connection starves the
//    next one, and the symptom is a hang rather than an error.
//  - It binds 127.0.0.1 only, while "localhost" usually resolves to ::1
//    first, so Open() walks every getaddrinfo result.
//
// Every call blocks up to its deadline, so all of them belong on the
// cartridge's worker thread. Close() is the exception: it is safe from any
// thread and wakes a worker parked in select() at once.
class FujiNetLink
{
public:
	typedef bool (*InboundHandler)(const fb_reply_t& frame);

	FujiNetLink() = default;
	~FujiNetLink();

	FujiNetLink(const FujiNetLink&) = delete;
	FujiNetLink& operator=(const FujiNetLink&) = delete;

	bool Open(const string& host, int port);
	void Close();
	bool IsOpen() const { return _socket.load(std::memory_order_acquire) != Invalid; }
	string GetLastError();

	fb_status_t Transact(uint8_t device, uint8_t command, const fb_param_t* params, unsigned nparams,
	                     const uint8_t* payload, uint16_t payloadLen, uint32_t timeoutMs, fb_reply_t* reply);
	void SendBare(uint8_t device, uint8_t command, const uint8_t* payload, uint16_t payloadLen);

	void SetInboundHandler(InboundHandler handler) { _inbound = handler; }

private:
	// A SLIP-encoded 512-byte push frame must fit whole: undersizing this
	// silently truncates every ROM push (the 1088 trap, paid for once on the
	// Intellivision).
	static constexpr size_t RxRawMax = 1088;
	// The builder assembles at most 384 decoded bytes; SLIP can double each.
	static constexpr size_t TxRawMax = 2 * 384 + 2;
	static constexpr int ConnectTimeoutMs = 3000;
	static constexpr int LoopbackConnectTimeoutMs = 500;
	static constexpr intptr_t Invalid = -1;

	fb_status_t ReadFrame(size_t& outLen, uint32_t timeoutMs);
	bool Fail(const string& what);

	std::atomic<intptr_t> _socket { Invalid };
	std::mutex _errorLock;
	string _lastError;
	InboundHandler _inbound = nullptr;

	std::array<uint8_t, RxRawMax> _rxRaw = {};
	std::array<uint8_t, TxRawMax> _txRaw = {};
	std::array<uint8_t, 2048> _rxPend = {};
	size_t _rxPendLen = 0;
	size_t _rxPendPos = 0;
};
