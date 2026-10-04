#pragma once
#include "pch.h"
#include <condition_variable>
#include <mutex>
#include <array>
#include "NES/BaseMapper.h"
#include "NES/Mappers/Homebrew/FujiNetLink.h"
#include "NES/Mappers/Homebrew/FujiNet/fujins.h"
#include "NES/Mappers/Homebrew/FujiNet/fuji_mailbox.h"
#include "NES/Mappers/Homebrew/FujiNet/nes_cart.h"
#include "NES/Mappers/Homebrew/FujiNet/nesmap.h"

//How the host (FujiNet Go NES) wants the cartridge. Process-wide, because the
//cartridge replaces Mesen's mapper selection rather than being picked by a
//header: while Enabled, every iNES image is loaded onto this board.
struct FujiNetCartHostConfig
{
	bool Enabled = false;
	string Host = "127.0.0.1";
	int Port = 11506;
	bool Debug = false;
};

//A snapshot for the host's UI and debugger, published once per frame.
struct FujiNetCartStatus
{
	uint32_t Instance = 0;       //which cartridge published this (see LatestInstance)
	bool Present = false;
	bool LinkUp = false;
	bool Busy = false;
	bool MailboxLive = false;
	bool SramEnabled = false;
	bool Loading = false;
	bool BootedImage = false;    //an image other than CONFIG is in the SRAMs
	uint8_t LoadPct = 0;
	uint16_t Mapper = 0;
	char MapperName[32] = {};
	uint32_t PrgSize = 0;        //bytes of PRG the image fills
	uint32_t ChrSize = 0;        //bytes of CHR-ROM, or the CHR-RAM size
	uint8_t PrgSlot[NESMAP_PRG_SLOTS] = {};
	uint16_t ChrSlot[NESMAP_CHR_SLOTS] = {};
	uint8_t Mirror = 0;
	bool WramEnabled = false;
	bool WramProtected = false;
	bool ChrWritable = false;
	bool IrqEnabled = false;
	bool IrqLine = false;
	uint8_t IrqLatch = 0;
	uint8_t IrqCounter = 0;
	uint8_t AckSeq = 0;
	uint8_t Status = 0;
	uint8_t LastError = 0;
	uint8_t BootState = 0;
	uint8_t BootPct = 0;
	uint8_t BootErr = 0;
	uint8_t DiagRmw = 0;
	uint32_t QueueDepth = 0;
	uint32_t PrgGeneration = 0;  //bumps whenever new code lands in PRG SRAM
	char LinkError[128] = {};
};

//The FujiNet NES cartridge (fujinet-firmware pico/nes): two 512K SRAMs for
//PRG and CHR behind bank tables, a 4K mailbox arena at $5000 with a 2K loader
//ROM at $5800, cart-served WRAM at $6000, and a vector page at $FF00 while
//the SRAM is off. A port of the cartridge's MAME device (pico/nes/emu): the
//protocol (fujimail), the wire codec (fujibus), the mapper engine (nesmap)
//and the bus decode (nes_cart.h) are the firmware's own sources, vendored in
//FujiNet/. Every image -- CONFIG, a network-booted game, a local cartridge
//file -- runs on nesmap, exactly as on the hardware.
//
//THE MAILBOX RUNS ON ITS OWN THREAD. A transaction can block for five seconds,
//or sixty on a mount, so fujimail runs on a worker thread, as it runs on the
//RP2354's core0 while core1 serves the bus. Hotspot writes go to it through a
//ring (core1 -> core0 on the cart); what it publishes is copied into the
//arena by the emulation thread once per CPU cycle, and the SEQ/ACKSEQ
//interlock (ACKSEQ published last) keeps that safe.
//
//THERE IS NO RESET LINE on the NES cart edge. A console reset restarts the
//client but not the cartridge, so Reset() leaves everything alone.
class FujiNetCart : public BaseMapper
{
private:
	enum class LoadState { Idle, Run, Done };

	static constexpr uint32_t ArenaSize = FN_ARENA_SIZE;
	static constexpr uint32_t VectorOffset = FN_ARENA_SIZE;
	static constexpr size_t QueueSize = 4096;

	//the arena is _mapperRam[0..0xFFF]; the vector page is _mapperRam[0x1000..0x10FF]
	nes_serve_t _serve = {};

	nesmap_t _map = {};
	bool _mapLive = false;
	uint8_t _prgSlot[NESMAP_PRG_SLOTS] = {};
	uint16_t _chrSlot[NESMAP_CHR_SLOTS] = {};
	bool _prgWe = false;
	bool _chrWe = false;
	uint8_t _mirror = NESMAP_MIR_V;

	//what is in (or going into) the SRAMs
	vector<uint8_t> _image;
	nesmap_plan_t _imagePlan = {};
	bool _imageIsConfig = false;
	bool _autoload = false;
	bool _bootedImage = false;

	//the load sequence (fuji_cart.c)
	LoadState _lsState = LoadState::Idle;
	uint32_t _lsSlice = 0;
	uint32_t _lsNprg = 0;
	uint32_t _lsNchr = 0;
	uint8_t _lsSeq = 0;
	bool _lsAckPending = false;

	//mailbox writes not yet old enough to rule out an RMW dummy
	deque<nes_event_t> _events;
	uint8_t _diagRmw = 0;
	uint64_t _a12LowClock = 0;
	uint32_t _prgGeneration = 0;

	bool _activated = false;
	uint32_t _instance = 0;
	bool _debug = false;
	string _host;
	int _port = 0;

	//---- the worker ----
	FujiNetLink _link;
	std::thread _worker;
	std::atomic<bool> _stop { false };
	std::atomic<bool> _txnInFlight { false };
	std::mutex _wakeLock;
	std::condition_variable _wake;
	std::timed_mutex _mailLock;    //held while fujimail's globals are in use

	std::array<uint16_t, QueueSize> _queue = {};
	std::atomic<size_t> _queueHead { 0 };
	std::atomic<size_t> _queueTail { 0 };

	std::atomic<bool> _publishReady { false };
	std::mutex _publishLock;
	std::array<uint8_t, FN_R_PAINT_END> _published = {};
	size_t _publishLo = FN_R_PAINT_END;
	size_t _publishHi = 0;

	//the DBC push, staged by the worker, consumed by the swap
	vector<uint8_t> _rx[2];
	std::mutex _stageLock;
	vector<uint8_t> _staged;
	nesmap_plan_t _stagedPlan = {};
	bool _haveStaged = false;
	bool _armed = false;

	uint32_t Cycles();
	void Activate();
	void Deactivate();
	void StartWorker();
	void StopWorker();
	void WorkerLoop();
	bool TryOpenLink();
	bool SyncWorker(uint32_t timeoutMs);
	void Note(uint16_t offset);
	void DrainPublished();
	void PaintInitial();
	void Service();
	void MailboxEvent(uint16_t offset, uint8_t data);
	void MapperWrite(uint16_t addr, uint8_t data);
	void ApplyMap();
	void ApplyMirroring();
	void ApplyMapping();
	void BeginLoad();
	void PublishSlice();
	void SliceAcked();
	void FinishLoad();
	void DirectBoot();
	void Poke(unsigned offset, uint8_t value);
	void PublishStatus();

protected:
	void InitMapper() override {}
	void InitMapper(RomData& romData) override;

	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x400; }
	uint16_t GetChrRamPageSize() override { return 0x400; }
	uint32_t GetChrRamSize() override { return NESMAP_CHR_MAX; }
	uint32_t GetWorkRamSize() override { return NESMAP_WRAM_MAX; }
	uint32_t GetWorkRamPageSize() override { return FN_WRAM_SIZE; }
	bool ForceWorkRamSize() override { return true; }
	uint32_t GetSaveRamSize() override { return 0; }
	bool ForceSaveRamSize() override { return true; }
	uint32_t GetMapperRamSize() override { return ArenaSize + 0x100; }
	uint32_t GetNametableCount() override { return 2; }

	uint16_t RegisterStartAddress() override { return 0x4020; }
	uint16_t RegisterEndAddress() override { return 0xFFFF; }
	bool AllowRegisterRead() override { return false; }
	bool EnableCpuClockHook() override { return true; }
	bool EnableVramAddressHook() override { return true; }

	void WriteRegister(uint16_t addr, uint8_t value) override;
	void Serialize(Serializer& s) override;
	vector<MapperStateEntry> GetMapperStateEntries() override;

public:
	FujiNetCart() = default;
	~FujiNetCart() override;

	void Reset(bool softReset) override;
	void ProcessCpuClock() override;
	void NotifyVramAddressChange(uint16_t addr) override;
	void EndFrame() override;

	//The fujimail port. Public only because that port is C function pointers
	//with no context argument; nothing else should call these.
	void PortPoke(unsigned offset, uint8_t value);
	bool PortLinkUp();
	void PortWaitLink(uint32_t ms);
	fb_status_t PortTransact(uint8_t device, uint8_t command, const fb_param_t* params, unsigned nparams,
	                         const uint8_t* payload, uint16_t payloadLen, uint32_t timeoutMs, fb_reply_t* reply);
	void PortSendBare(uint8_t device, uint8_t command, const uint8_t* payload, uint16_t payloadLen);
	uint8_t PortStreamOpen(int stream, uint32_t size);
	void PortStreamWrite(int stream, const uint8_t* chunk, unsigned len);
	uint8_t PortStreamClose(int stream, uint32_t got, bool aborted);
	void PortArmSwap();
	void PortLog(const string& msg);

	//---- the host API ----
	static void SetHostConfig(const FujiNetCartHostConfig& config);
	static FujiNetCartHostConfig GetHostConfig();
	static bool IsHostEnabled();

	//The baked CONFIG: a whole iNES image (fujinet-config's nes/ build).
	static const uint8_t* GetConfigRom(uint32_t& size);

	//Whether the cartridge can run this image; err says why not.
	static bool CheckImage(const uint8_t* data, uint32_t size, string& err);

	//The active cartridge's status; false when no cartridge is running.
	static bool GetStatus(FujiNetCartStatus& status);

	//The most recently built cartridge. A host that has just powered on a new
	//one waits for GetStatus().Instance to reach this before trusting it.
	static uint32_t LatestInstance();
};
