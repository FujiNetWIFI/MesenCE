#include "pch.h"
#include <chrono>
#include "NES/Mappers/Homebrew/FujiNetCart.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/NesTypes.h"
#include "Shared/MessageManager.h"
#include "Utilities/Serializer.h"

#include "NES/Mappers/Homebrew/FujiNet/fujimail.h"
#include "NES/Mappers/Homebrew/FujiNet/fujiconfigrom.h"
#include "NES/Mappers/Homebrew/FujiNet/nesloaderrom.h"

//fujimail's port is C function pointers with no context argument, so the one
//running cartridge is reached through this. One slot, one cart: the
//constraint is the hardware's too.
static std::mutex _activeLock;
static FujiNetCart* _active = nullptr;

static std::mutex _configLock;
static FujiNetCartHostConfig _hostConfig;

static std::mutex _statusLock;
static FujiNetCartStatus _status;
static std::atomic<uint32_t> _instanceCounter { 0 };

//Which thread is the mailbox worker: PortPoke publishes from there, and
//writes the arena directly from the emulation thread.
static thread_local bool _onWorker = false;

static void c_poke(unsigned offset, uint8_t value) { _active->PortPoke(offset, value); }
static bool c_link_up() { return _active->PortLinkUp(); }
static void c_wait_link_ms(uint32_t ms) { _active->PortWaitLink(ms); }

static fb_status_t c_transact(uint8_t device, uint8_t command, const fb_param_t* params, unsigned nparams,
                              const uint8_t* payload, uint16_t payloadLen, uint32_t timeoutMs, fb_reply_t* reply)
{
	return _active->PortTransact(device, command, params, nparams, payload, payloadLen, timeoutMs, reply);
}

static void c_send_bare(uint8_t device, uint8_t command, const uint8_t* payload, uint16_t payloadLen)
{
	_active->PortSendBare(device, command, payload, payloadLen);
}

static uint8_t c_stream_open(int stream, uint32_t size) { return _active->PortStreamOpen(stream, size); }
static void c_stream_write(int stream, const uint8_t* chunk, unsigned len) { _active->PortStreamWrite(stream, chunk, len); }
static uint8_t c_stream_close(int stream, uint32_t got, bool aborted) { return _active->PortStreamClose(stream, got, aborted); }
static void c_arm_swap() { _active->PortArmSwap(); }

static void c_on_txn(const fujimail_txn_t* t)
{
	char txt[40];
	unsigned m = 0;
	for(unsigned k = 0; k < t->rxlen && m < sizeof(txt) - 1; k++) {
		uint8_t c = t->rx[k];
		if(c == 0) {
			break;
		}
		txt[m++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
	}
	txt[m] = 0;

	char line[160];
	snprintf(line, sizeof(line), "dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u -> err=%d reply=%02X rxlen=%u%s%s%s",
	         t->device, t->command, t->nparam, t->txlen, t->seq, t->status, t->reply_cmd, t->rxlen,
	         m ? " \"" : "", txt, m ? "\"" : "");
	_active->PortLog(line);
}

static void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect, unsigned got, bool aborted)
{
	if(ev == FUJIMAIL_DBC_OPEN) {
		_active->PortLog("DBC open stream=" + std::to_string(stream) + " size=" + std::to_string(expect));
	} else {
		_active->PortLog("DBC close stream=" + std::to_string(stream) + " got=" + std::to_string(got) + (aborted ? " ABORTED" : ""));
	}
}

static const fujimail_port_t _portQuiet = {
	c_poke, c_link_up, c_transact, c_send_bare,
	c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
	c_wait_link_ms, nullptr, nullptr, nullptr,
};

static const fujimail_port_t _portDebug = {
	c_poke, c_link_up, c_transact, c_send_bare,
	c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
	c_wait_link_ms, nullptr, c_on_txn, c_on_dbc,
};

static bool InboundHandler(const fb_reply_t& frame)
{
	return fujimail_inbound(&frame);
}

//---------------------------------------------------------------------------
// host API
//---------------------------------------------------------------------------

void FujiNetCart::SetHostConfig(const FujiNetCartHostConfig& config)
{
	std::lock_guard<std::mutex> lock(_configLock);
	_hostConfig = config;
}

FujiNetCartHostConfig FujiNetCart::GetHostConfig()
{
	std::lock_guard<std::mutex> lock(_configLock);
	return _hostConfig;
}

bool FujiNetCart::IsHostEnabled()
{
	std::lock_guard<std::mutex> lock(_configLock);
	return _hostConfig.Enabled;
}

const uint8_t* FujiNetCart::GetConfigRom(uint32_t& size)
{
	size = FUJI_CONFIGROM_SIZE;
	return _configrom;
}

bool FujiNetCart::CheckImage(const uint8_t* data, uint32_t size, string& err)
{
	nesmap_plan_t plan;
	switch(nesmap_plan(data, size, &plan)) {
		case NESMAP_OK: err.clear(); return true;
		case NESMAP_EEMPTY: err = "The file is too small to be an NES image."; break;
		case NESMAP_EMAGIC: err = "The file is not an iNES (.nes) image."; break;
		case NESMAP_ETRAINER: err = "Images with a 512-byte trainer are not supported by the FujiNet cartridge."; break;
		case NESMAP_ETOOBIG: err = "The image's PRG or CHR is larger than the cartridge's 512K SRAMs."; break;
		case NESMAP_ETRUNC: err = "The image is shorter than its header says."; break;
		case NESMAP_EMAPPER: err = "Mapper " + std::to_string(plan.mapper) + " is not supported by the FujiNet cartridge."; break;
		default: err = "The image cannot be mapped."; break;
	}
	return false;
}

uint32_t FujiNetCart::LatestInstance()
{
	return _instanceCounter.load();
}

bool FujiNetCart::GetStatus(FujiNetCartStatus& status)
{
	std::lock_guard<std::mutex> lock(_statusLock);
	status = _status;
	return _status.Present;
}

//---------------------------------------------------------------------------
// lifecycle
//---------------------------------------------------------------------------

FujiNetCart::~FujiNetCart()
{
	Deactivate();
}

void FujiNetCart::InitMapper(RomData& romData)
{
	FujiNetCartHostConfig cfg = GetHostConfig();
	_instance = ++_instanceCounter;
	_debug = cfg.Debug;
	_host = cfg.Host;
	_port = cfg.Port;

	memset(_mapperRam, 0, _mapperRamSize);
	memset(_prgRom, 0xFF, _prgSize);
	memset(_chrRam, 0, _chrRamSize);
	memset(_workRam, 0, _workRamSize);

	memcpy(_mapperRam + FN_LOADER, _loaderrom, FUJI_LOADERROM_SIZE);
	//RESET into the loader, NMI and IRQ onto its RTI at $5803 (fuji_cart.c)
	uint8_t* vectors = _mapperRam + VectorOffset;
	vectors[0xFA] = 0x03; vectors[0xFB] = 0x58;
	vectors[0xFC] = 0x00; vectors[0xFD] = 0x58;
	vectors[0xFE] = 0x03; vectors[0xFF] = 0x58;

	_serve.arena = _mapperRam;
	_serve.wram = _workRam;
	_serve.wram_size = _workRamSize;
	_serve.vectors = vectors;
	_serve.sram_en = false;
	_serve.mailbox = true;
	_serve.wram_en = false;
	_serve.wram_wp = false;
	_serve.loading = false;

	PaintInitial();

	_image = romData.RawData;
	_imageIsConfig = _image.size() == FUJI_CONFIGROM_SIZE && memcmp(_image.data(), _configrom, FUJI_CONFIGROM_SIZE) == 0;

	if(nesmap_plan(_image.data(), (uint32_t)_image.size(), &_imagePlan) != NESMAP_OK) {
		//FujiNetCartLoader refuses these, so this is only a safety net
		MessageManager::Log("[FujiNet] image is not mappable; serving nothing");
		_image.clear();
	} else if(_imageIsConfig) {
		//As the cart at power-on: the loader asks, then the load begins
		_autoload = true;
	} else {
		MessageManager::Log("[FujiNet] " + std::to_string(_image.size()) + "-byte image, mapper " + std::to_string(_imagePlan.mapper) +
		                    (_imagePlan.fuji_claim ? ", claims the mailbox" : ", no claim"));
		DirectBoot();
	}

	ApplyMapping();
	ApplyMirroring();
	PublishStatus();
}

void FujiNetCart::Reset(bool softReset)
{
	//Nothing: bank state, ACKSEQ and the loaded image all survive a console
	//reset on the cart, whose edge has no reset line.
	(void)softReset;
}

//The status page as fujimail_paint() leaves it, written without touching
//fujimail's globals: another cartridge's worker may still own them while
//this one is being built.
void FujiNetCart::PaintInitial()
{
	memset(_mapperRam, 0, FN_R_PAINT_END);
	_mapperRam[FN_R_MAGIC0] = 'F';
	_mapperRam[FN_R_MAGIC1] = 'N';
	_mapperRam[FN_R_PROTO_VER] = FN_PROTO_VER;
	_mapperRam[FN_R_STATUS] = 0;
}

//Mesen builds the next console before it destroys the current one, so the
//mailbox service (process-wide state, like the cart's single core0) is taken
//over on this cartridge's first clock rather than in its constructor.
void FujiNetCart::Activate()
{
	std::lock_guard<std::mutex> lock(_activeLock);
	_activated = true;
	if(_active && _active != this) {
		_active->StopWorker();
	}
	_active = this;
	fujimail_init(_debug ? &_portDebug : &_portQuiet);
	StartWorker();
	PublishStatus();
}

void FujiNetCart::Deactivate()
{
	std::lock_guard<std::mutex> lock(_activeLock);
	StopWorker();
	if(_active == this) {
		_active = nullptr;
		std::lock_guard<std::mutex> statusLock(_statusLock);
		_status = {};
	}
}

void FujiNetCart::StartWorker()
{
	if(_worker.joinable()) {
		return;
	}
	_stop.store(false, std::memory_order_relaxed);
	_link.SetInboundHandler(InboundHandler);
	_worker = std::thread([this] { WorkerLoop(); });
}

void FujiNetCart::StopWorker()
{
	if(!_worker.joinable()) {
		return;
	}

	//Closed FIRST: a worker parked in select() on a sixty-second mount wakes
	//at once instead of holding up the console's teardown.
	_stop.store(true, std::memory_order_release);
	_link.Close();
	_wake.notify_all();
	_worker.join();
	_link.Close();
}

//---------------------------------------------------------------------------
// the worker: fujimail, as core0 runs it on the cart
//---------------------------------------------------------------------------

bool FujiNetCart::TryOpenLink()
{
	if(_link.IsOpen()) {
		return true;
	}
	return _link.Open(_host, _port);
}

void FujiNetCart::WorkerLoop()
{
	_onWorker = true;
	TryOpenLink();
	auto lastAttempt = std::chrono::steady_clock::now();

	for(;;) {
		{
			std::unique_lock<std::mutex> lock(_wakeLock);
			_wake.wait_for(lock, std::chrono::seconds(1), [this] {
				return _stop.load(std::memory_order_acquire) ||
				       _queueHead.load(std::memory_order_acquire) != _queueTail.load(std::memory_order_relaxed);
			});
		}
		if(_stop.load(std::memory_order_acquire)) {
			return;
		}

		//A FujiNet that came up after the cartridge: keep trying, gently,
		//so the CONFIG finds the link without waiting out a transaction.
		if(!_link.IsOpen() && std::chrono::steady_clock::now() - lastAttempt >= std::chrono::seconds(1)) {
			TryOpenLink();
			lastAttempt = std::chrono::steady_clock::now();
		}

		//fujimail's file-scope state is touched by whoever holds _mailLock,
		//which is this thread except while the emulation thread repaints.
		std::lock_guard<std::timed_mutex> mail(_mailLock);
		for(;;) {
			size_t tail = _queueTail.load(std::memory_order_relaxed);
			if(tail == _queueHead.load(std::memory_order_acquire) || _stop.load(std::memory_order_acquire)) {
				break;
			}
			uint16_t offset = _queue[tail];
			_queueTail.store((tail + 1) % QueueSize, std::memory_order_release);
			fujimail_read_hotspot(offset);
		}
	}
}

//Hand one decoded hotspot access to the worker -- the cart's core1 -> core0
//ring. Nothing is dropped unless the ring is full, as on the hardware, where
//core0 picks events up after a transaction returns.
void FujiNetCart::Note(uint16_t offset)
{
	size_t head = _queueHead.load(std::memory_order_relaxed);
	size_t next = (head + 1) % QueueSize;
	if(next == _queueTail.load(std::memory_order_acquire)) {
		return;
	}
	_queue[head] = offset;
	_queueHead.store(next, std::memory_order_release);
	_wake.notify_one();
}

//Wait until the worker has nothing queued and is not inside fujimail. Used
//where the bus layer must see the effect of events the console has already
//written (BOOTLOCK before HOT_SWAP), which on the cart takes core0 microseconds.
bool FujiNetCart::SyncWorker(uint32_t timeoutMs)
{
	if(!_worker.joinable()) {
		return true;
	}

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for(;;) {
		if(_queueHead.load(std::memory_order_acquire) == _queueTail.load(std::memory_order_acquire)) {
			if(_mailLock.try_lock_until(deadline)) {
				bool empty = _queueHead.load(std::memory_order_acquire) == _queueTail.load(std::memory_order_acquire);
				_mailLock.unlock();
				if(empty) {
					return true;
				}
			}
		}
		if(std::chrono::steady_clock::now() >= deadline) {
			MessageManager::Log("[FujiNet] mailbox worker did not go idle");
			return false;
		}
		std::this_thread::yield();
	}
}

void FujiNetCart::PortPoke(unsigned offset, uint8_t value)
{
	if(offset >= FN_R_PAINT_END) {
		return;
	}

	if(_onWorker) {
		{
			std::lock_guard<std::mutex> lock(_publishLock);
			_published[offset] = value;
			_publishLo = std::min(_publishLo, (size_t)offset);
			_publishHi = std::max(_publishHi, (size_t)offset + 1);
		}
		//Every byte is offered, not just ACKSEQ, so the busy flag and a mount's
		//progress can be watched while the transaction is out. The interlock
		//survives: fujimail publishes ACKSEQ last, and a drain copies the whole
		//pending range under one lock.
		_publishReady.store(true, std::memory_order_release);
	} else {
		Poke(offset, value);
	}
}

void FujiNetCart::DrainPublished()
{
	std::lock_guard<std::mutex> lock(_publishLock);
	for(size_t i = _publishLo; i < _publishHi; i++) {
		_mapperRam[i] = _published[i];
	}
	_publishLo = FN_R_PAINT_END;
	_publishHi = 0;
	_publishReady.store(false, std::memory_order_relaxed);
}

bool FujiNetCart::PortLinkUp()
{
	return _link.IsOpen();
}

void FujiNetCart::PortWaitLink(uint32_t ms)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while(!_link.IsOpen() && !_stop.load(std::memory_order_acquire)) {
		if(TryOpenLink() || std::chrono::steady_clock::now() >= deadline) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}

fb_status_t FujiNetCart::PortTransact(uint8_t device, uint8_t command, const fb_param_t* params, unsigned nparams,
                                      const uint8_t* payload, uint16_t payloadLen, uint32_t timeoutMs, fb_reply_t* reply)
{
	//FN_R_STATUS_BUSY is declared by the protocol but never raised by the
	//firmware, whose transaction is synchronous with its bus loop. Here it is
	//real: the console keeps running while this blocks. fujimail republishes
	//FN_R_STATUS itself once it has the reply.
	_txnInFlight.store(true, std::memory_order_relaxed);
	PortPoke(FN_R_STATUS, FN_R_STATUS_LINK | FN_R_STATUS_BUSY);

	fb_status_t status = _link.Transact(device, command, params, nparams, payload, payloadLen, timeoutMs, reply);
	if(status == FB_ENOLINK) {
		//fujinet-pc went away: drop the socket so the next transaction reconnects
		_link.Close();
	}

	_txnInFlight.store(false, std::memory_order_relaxed);
	return status;
}

void FujiNetCart::PortSendBare(uint8_t device, uint8_t command, const uint8_t* payload, uint16_t payloadLen)
{
	_link.SendBare(device, command, payload, payloadLen);
}

uint8_t FujiNetCart::PortStreamOpen(int stream, uint32_t size)
{
	uint8_t err = (stream == FN_STREAM_ROM) ? nesmap_gate(size) : 0;
	if(err != 0) {
		return err;
	}
	vector<uint8_t>& v = _rx[stream & 1];
	v.clear();
	if(size) {
		v.reserve(size);
	}
	return 0;
}

void FujiNetCart::PortStreamWrite(int stream, const uint8_t* chunk, unsigned len)
{
	vector<uint8_t>& v = _rx[stream & 1];
	v.insert(v.end(), chunk, chunk + len);
}

uint8_t FujiNetCart::PortStreamClose(int stream, uint32_t got, bool aborted)
{
	(void)got;
	vector<uint8_t>& v = _rx[stream & 1];
	if(stream != FN_STREAM_ROM || aborted) {
		v.clear();
		return 0;
	}

	nesmap_plan_t plan;
	if(v.empty() || nesmap_plan(v.data(), (uint32_t)v.size(), &plan) != NESMAP_OK) {
		PortLog("pushed image (" + std::to_string(v.size()) + " bytes) is not mappable");
		v.clear();
		return FN_BOOT_ERR_NOMAP;
	}

	//The pushed image is staged until the swap consumes it, as the cart's
	//fuji_store keeps it in RAM or flash.
	std::lock_guard<std::mutex> lock(_stageLock);
	_staged.swap(v);
	v.clear();
	_stagedPlan = plan;
	_haveStaged = true;
	_armed = false;
	return 0;
}

void FujiNetCart::PortArmSwap()
{
	std::lock_guard<std::mutex> lock(_stageLock);
	if(_haveStaged) {
		_armed = true;
	}
}

void FujiNetCart::PortLog(const string& msg)
{
	MessageManager::Log("[FujiNet] " + msg);
}

//---------------------------------------------------------------------------
// the bus
//---------------------------------------------------------------------------

uint32_t FujiNetCart::Cycles()
{
	return (uint32_t)_console->GetCpu()->GetCycleCount();
}

void FujiNetCart::Poke(unsigned offset, uint8_t value)
{
	if(offset < FN_R_PAINT_END) {
		if(_publishReady.load(std::memory_order_acquire)) {
			DrainPublished();
		}
		_mapperRam[offset] = value;
	}
}

void FujiNetCart::ProcessCpuClock()
{
	BaseProcessCpuClock();

	if(!_activated) {
		Activate();
	}
	if(_publishReady.load(std::memory_order_acquire)) {
		DrainPublished();
	}
	if(!_events.empty()) {
		Service();
	}
}

//Drain mailbox writes at least two cycles old, dropping an RMW's dummy write
//(same offset, adjacent cycle) exactly as fuji_cart.c does. Called at the start
//of every CPU cycle, so the pair is always both visible when the older one
//comes due.
void FujiNetCart::Service()
{
	uint32_t now = Cycles();

	while(!_events.empty()) {
		nes_event_t e = _events.front();
		if((int32_t)(now - e.cycle) < 2) {
			break;
		}
		_events.pop_front();
		if(!_events.empty() && _events.front().offset == e.offset && _events.front().cycle == e.cycle + 1) {
			_diagRmw++;
			Poke(FN_R_DIAG_RMW, _diagRmw);
			continue;
		}
		MailboxEvent(e.offset, e.data);
	}
}

//One hotspot write, decoded as fujinet.c on the cart does. The swap and the
//loader's slice ack belong to the bus layer (inline); everything else is
//fujimail's, on the worker.
void FujiNetCart::MailboxEvent(uint16_t offset, uint8_t data)
{
	unsigned page = offset & FN_H_PAGE_MASK;
	unsigned low = offset & 0xFF;

	if(page == FN_H_REGSEL || page == FN_H_REGDATA) {
		if(low == FN_HOT_SWAP) {
			if(_lsState != LoadState::Idle) {
				return;
			}
			SyncWorker(2000);
			{
				std::lock_guard<std::mutex> lock(_stageLock);
				if(_armed && _haveStaged) {
					_image.swap(_staged);
					_staged.clear();
					_imagePlan = _stagedPlan;
					_imageIsConfig = false;
					_haveStaged = false;
					_armed = false;
					BeginLoad();
					return;
				}
			}
			if(_autoload && !_image.empty()) {
				_autoload = false;
				BeginLoad();
			}
			return;
		}
		if(low == FN_REG_SLICE_ACK) {
			SliceAcked();
			return;
		}
		if(low >= 0x80) {
			return;
		}
		Note((uint16_t)(FN_H_REGSEL + low));
		Note((uint16_t)(FN_H_REGDATA + data));
	} else if(page == FN_H_DATA) {
		Note((uint16_t)(FN_H_DATA + data));
	}
}

void FujiNetCart::WriteRegister(uint16_t addr, uint8_t value)
{
	nes_region_t region = nes_region_from_addr(addr);
	switch(nes_write_kind(&_serve, region, addr)) {
		case NES_W_MAILBOX:
			_events.push_back(nes_event_t { (uint16_t)(addr & (FN_ARENA_SIZE - 1)), value, NES_EV_MAILBOX, Cycles() });
			break;

		case NES_W_WRAM:
			if(_serve.wram_en && !_serve.wram_wp) {
				_workRam[addr & (FN_WRAM_SIZE - 1)] = value;
			}
			MapperWrite(addr, value);    //NINA-001 keeps registers here
			break;

		case NES_W_MAPPER:
			MapperWrite(addr, value);
			break;

		default:
			if(region == NES_R_ROM && _serve.loading && _prgWe && _serve.sram_en) {
				uint16_t offset = addr - 0x8000;
				_prgRom[((uint32_t)_prgSlot[offset >> 13] << 13) | (offset & 0x1FFF)] = value;
			}
			break;
	}
}

void FujiNetCart::MapperWrite(uint16_t addr, uint8_t data)
{
	if(!_mapLive || _serve.loading) {
		return;
	}
	nesmap_write(&_map, addr, data, Cycles());
	if(_map.dirty) {
		ApplyMap();
	}
}

//MMC3's scanline counter, clocked from filtered PPU A12 rises the way Mesen's
//own MMC3 filters them (the cart does it in nes_irq.c).
void FujiNetCart::NotifyVramAddressChange(uint16_t addr)
{
	bool rise = false;
	if(addr & 0x1000) {
		rise = _a12LowClock > 0 && (_console->GetMasterClock() - _a12LowClock) >= 3;
		_a12LowClock = 0;
	} else if(_a12LowClock == 0) {
		_a12LowClock = _console->GetMasterClock();
	}

	if(!rise || !_mapLive || _serve.loading || _map.desc == nullptr || _map.desc->irq != NESMAP_IRQ_A12) {
		return;
	}
	if(nesmap_a12_clock(&_map)) {
		_console->GetCpu()->SetIrqSource(IRQSource::External);
	}
}

//---------------------------------------------------------------------------
// the mapper's outputs: slot tables, gates, mirroring
//---------------------------------------------------------------------------

void FujiNetCart::ApplyMirroring()
{
	switch(_mirror) {
		case NESMAP_MIR_H: SetMirroringType(MirroringType::Horizontal); break;
		case NESMAP_MIR_1LO: SetMirroringType(MirroringType::ScreenAOnly); break;
		case NESMAP_MIR_1HI: SetMirroringType(MirroringType::ScreenBOnly); break;
		case NESMAP_MIR_V:
		default: SetMirroringType(MirroringType::Vertical); break;    //four-screen: deferred, as on the cart
	}
}

void FujiNetCart::ApplyMap()
{
	for(int i = 0; i < NESMAP_PRG_SLOTS; i++) {
		_prgSlot[i] = _map.out.prg[i];
	}
	for(int i = 0; i < NESMAP_CHR_SLOTS; i++) {
		_chrSlot[i] = _map.out.chr[i];
	}
	_chrWe = !_map.out.chr_wp;
	_serve.wram_en = _map.out.wram_en;
	_serve.wram_wp = _map.out.wram_wp;
	if(_mirror != _map.out.mirror) {
		_mirror = _map.out.mirror;
		ApplyMirroring();
	}
	if(_map.dirty & NESMAP_DIRTY_IRQ) {
		if(_map.irq_line) {
			_console->GetCpu()->SetIrqSource(IRQSource::External);
		} else {
			_console->GetCpu()->ClearIrqSource(IRQSource::External);
		}
	}
	_map.dirty = 0;
	ApplyMapping();
}

//What the cart drives for each read, as page tables: Mesen serves reads (and
//the debugger peeks) straight from these, so no read has a side effect.
void FujiNetCart::ApplyMapping()
{
	//$5000-$5FFF: reply + status while the mailbox lives, inert hotspot pages,
	//and the loader, always
	if(_serve.mailbox) {
		SetCpuMemoryMapping(0x5000, 0x54FF, PrgMemoryType::MapperRam, 0x000, MemoryAccessType::Read);
	} else {
		RemoveCpuMemoryMapping(0x5000, 0x54FF);
	}
	RemoveCpuMemoryMapping(0x5500, 0x57FF);
	SetCpuMemoryMapping(0x5800, 0x5FFF, PrgMemoryType::MapperRam, FN_LOADER, MemoryAccessType::Read);

	//$6000-$7FFF: WRAM, gated by the mapper; stores go through WriteRegister
	if(_serve.wram_en) {
		SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, 0, _serve.wram_wp ? MemoryAccessType::Read : MemoryAccessType::ReadWrite);
	} else {
		RemoveCpuMemoryMapping(0x6000, 0x7FFF);
	}

	//$8000-$FFFF: the PRG SRAM through the four 8K slots, or only the vector
	//page while SRAM_EN is off
	if(_serve.sram_en) {
		for(int i = 0; i < NESMAP_PRG_SLOTS; i++) {
			uint16_t start = (uint16_t)(0x8000 + i * 0x2000);
			SetCpuMemoryMapping(start, (uint16_t)(start + 0x1FFF), PrgMemoryType::PrgRom, (uint32_t)_prgSlot[i] << 13, MemoryAccessType::Read);
		}
	} else {
		RemoveCpuMemoryMapping(0x8000, 0xFEFF);
		SetCpuMemoryMapping(0xFF00, 0xFFFF, PrgMemoryType::MapperRam, VectorOffset, MemoryAccessType::Read);
	}

	for(int i = 0; i < NESMAP_CHR_SLOTS; i++) {
		uint16_t start = (uint16_t)(i * 0x400);
		SetPpuMemoryMapping(start, (uint16_t)(start + 0x3FF), ChrMemoryType::ChrRam, ((uint32_t)_chrSlot[i] << 10) % NESMAP_CHR_MAX,
		                    _chrWe ? MemoryAccessType::ReadWrite : MemoryAccessType::Read);
	}
}

//---------------------------------------------------------------------------
// loading: the same sequence as fuji_cart.c
//---------------------------------------------------------------------------

void FujiNetCart::PublishSlice()
{
	const uint8_t* src;
	unsigned dst, off;

	if(_lsSlice < _lsNprg) {
		_prgSlot[0] = (uint8_t)(_lsSlice / 8);
		src = _image.data() + nesmap_prg_offset(&_imagePlan) + _lsSlice * 1024;
		dst = FN_LOAD_DST_PRG;
		off = _lsSlice % 8;
	} else {
		unsigned k = _lsSlice - _lsNprg;
		unsigned w = k / 8;
		if(k == 0) {
			_chrWe = true;
		}
		for(int i = 0; i < NESMAP_CHR_SLOTS; i++) {
			_chrSlot[i] = (uint16_t)(w * 8 + i);
		}
		src = _image.data() + nesmap_chr_offset(&_imagePlan) + k * 1024;
		dst = FN_LOAD_DST_CHR;
		off = k % 8;
	}
	ApplyMapping();

	if(_publishReady.load(std::memory_order_acquire)) {
		DrainPublished();
	}
	memcpy(_mapperRam + FN_R_DATA, src, FN_R_SLICE_LEN);
	Poke(FN_R_LOAD_DST, (uint8_t)dst);
	Poke(FN_R_LOAD_OFF, (uint8_t)off);
	Poke(FN_R_LOAD_PCT, (uint8_t)((_lsSlice * 100u) / (_lsNprg + _lsNchr)));
	_lsSeq = (uint8_t)(_lsSeq == 255 ? 1 : _lsSeq + 1);
	Poke(FN_R_LOAD_SEQ, _lsSeq);
	Poke(FN_R_LOAD_STATE, FN_LOAD_SLICE);
	_lsAckPending = true;
}

void FujiNetCart::BeginLoad()
{
	_lsNprg = _imagePlan.prg_size / 1024;
	_lsNchr = _imagePlan.chr_size / 1024;
	_lsSlice = 0;
	_lsAckPending = false;
	_lsState = LoadState::Run;
	_mapLive = false;
	_bootedImage = false;

	_serve.loading = true;
	_serve.mailbox = true;
	_serve.sram_en = true;
	_prgWe = true;
	_chrWe = false;
	_console->GetCpu()->ClearIrqSource(IRQSource::External);
	Poke(FN_R_SRAM_STATE, 1);
	Poke(FN_R_BOOT_STATE, FN_BOOT_IDLE);
	if(_debug) {
		PortLog("loading " + std::to_string(_lsNprg) + " PRG + " + std::to_string(_lsNchr) + " CHR slices");
	}
	PublishSlice();
}

void FujiNetCart::FinishLoad()
{
	nesmap_init(&_map, &_imagePlan);
	_mapLive = true;
	_prgWe = false;
	_serve.loading = false;
	_mirror = 0xFF;    //force the mirroring apply
	ApplyMap();

	if(_imagePlan.fuji_claim && !_imagePlan.claims_5000) {
		//A FujiNet client keeps the mailbox: start it over, as the cart does.
		//The worker is idle (the console is inside the loader), so the paint
		//runs here, under the mailbox lock, writing the arena directly.
		SyncWorker(2000);
		std::lock_guard<std::timed_mutex> mail(_mailLock);
		fujimail_paint();
	}
	Poke(FN_R_MAPPER, (uint8_t)_imagePlan.mapper);
	Poke(FN_R_LOAD_STATE, FN_LOAD_DONE);
	_lsState = LoadState::Done;
	_bootedImage = !_imageIsConfig;
	_prgGeneration++;

	PortLog("image in place, mapper " + std::to_string(_imagePlan.mapper) + "; mailbox " +
	        ((_imagePlan.fuji_claim && !_imagePlan.claims_5000) ? "kept" : "off after the loader leaves"));
}

void FujiNetCart::SliceAcked()
{
	switch(_lsState) {
		case LoadState::Run:
			if(!_lsAckPending) {
				return;
			}
			_lsAckPending = false;
			_lsSlice++;
			if(_lsSlice < _lsNprg + _lsNchr) {
				PublishSlice();
			} else {
				FinishLoad();
			}
			break;

		case LoadState::Done:
			_serve.mailbox = _imagePlan.fuji_claim && !_imagePlan.claims_5000;
			ApplyMapping();
			Poke(FN_R_LOAD_STATE, FN_LOAD_IDLE);
			_lsState = LoadState::Idle;
			break;

		default:
			break;
	}
}

//Put an image straight into the SRAMs, as if the loader had run: a cartridge
//file opened on the desktop.
void FujiNetCart::DirectBoot()
{
	memcpy(_prgRom, _image.data() + nesmap_prg_offset(&_imagePlan), _imagePlan.prg_size);
	if(_imagePlan.chr_size) {
		memcpy(_chrRam, _image.data() + nesmap_chr_offset(&_imagePlan), _imagePlan.chr_size);
	}
	nesmap_init(&_map, &_imagePlan);
	_mapLive = true;
	_serve.sram_en = true;
	_serve.loading = false;
	_prgWe = false;
	_mirror = 0xFF;
	ApplyMap();
	_serve.mailbox = _imagePlan.fuji_claim && !_imagePlan.claims_5000;
	Poke(FN_R_SRAM_STATE, 1);
	Poke(FN_R_MAPPER, (uint8_t)_imagePlan.mapper);
	_lsState = LoadState::Idle;
	_bootedImage = true;
	_prgGeneration++;
}

//---------------------------------------------------------------------------
// status, debugger, save states
//---------------------------------------------------------------------------

void FujiNetCart::PublishStatus()
{
	FujiNetCartStatus st;
	st.Instance = _instance;
	st.Present = true;
	st.LinkUp = _link.IsOpen();
	st.Busy = _txnInFlight.load(std::memory_order_relaxed);
	st.MailboxLive = _serve.mailbox;
	st.SramEnabled = _serve.sram_en;
	st.Loading = _serve.loading;
	st.BootedImage = _bootedImage;
	st.LoadPct = _mapperRam[FN_R_LOAD_PCT];
	st.Mapper = _mapLive ? _imagePlan.mapper : 0;
	st.PrgSize = _mapLive ? _imagePlan.prg_size : 0;
	st.ChrSize = _mapLive ? (_imagePlan.chr_size ? _imagePlan.chr_size : _imagePlan.chr_ram_size) : 0;
	if(_mapLive && _map.desc && _map.desc->name) {
		snprintf(st.MapperName, sizeof(st.MapperName), "%s", _map.desc->name);
	}
	memcpy(st.PrgSlot, _prgSlot, sizeof(_prgSlot));
	memcpy(st.ChrSlot, _chrSlot, sizeof(_chrSlot));
	st.Mirror = _mirror;
	st.WramEnabled = _serve.wram_en;
	st.WramProtected = _serve.wram_wp;
	st.ChrWritable = _chrWe;
	st.IrqEnabled = _map.irq_enable;
	st.IrqLine = _map.irq_line;
	st.IrqLatch = _map.irq_latch;
	st.IrqCounter = _map.irq_counter;
	st.AckSeq = _mapperRam[FN_R_ACKSEQ];
	st.Status = _mapperRam[FN_R_STATUS];
	st.LastError = _mapperRam[FN_R_ERR];
	st.BootState = _mapperRam[FN_R_BOOT_STATE];
	st.BootPct = _mapperRam[FN_R_BOOT_PCT];
	st.BootErr = _mapperRam[FN_R_BOOT_ERR];
	st.DiagRmw = _diagRmw;
	size_t head = _queueHead.load(std::memory_order_relaxed);
	size_t tail = _queueTail.load(std::memory_order_relaxed);
	st.QueueDepth = (uint32_t)((head + QueueSize - tail) % QueueSize);
	st.PrgGeneration = _prgGeneration;
	snprintf(st.LinkError, sizeof(st.LinkError), "%s", _link.GetLastError().c_str());

	//Only the cartridge that owns the mailbox reports: one being built while
	//the previous one still runs must not overwrite it, nor the old one,
	//kept alive a little longer, overwrite its successor.
	std::lock_guard<std::mutex> lock(_statusLock);
	if(_active == this) {
		_status = st;
	}
}

void FujiNetCart::EndFrame()
{
	PublishStatus();
}

vector<MapperStateEntry> FujiNetCart::GetMapperStateEntries()
{
	vector<MapperStateEntry> entries;
	entries.push_back(MapperStateEntry("", "FujiNet Link", _link.IsOpen()));
	entries.push_back(MapperStateEntry("", "Mailbox Live", _serve.mailbox));
	entries.push_back(MapperStateEntry("", "SRAM Enabled", _serve.sram_en));
	entries.push_back(MapperStateEntry("", "Loading", _serve.loading));
	entries.push_back(MapperStateEntry("$5400", "ACKSEQ", _mapperRam[FN_R_ACKSEQ], MapperStateValueType::Number8));
	entries.push_back(MapperStateEntry("$5401", "Status", _mapperRam[FN_R_STATUS], MapperStateValueType::Number8));
	entries.push_back(MapperStateEntry("$5406", "Boot State", _mapperRam[FN_R_BOOT_STATE], MapperStateValueType::Number8));
	entries.push_back(MapperStateEntry("$540D", "Load State", _mapperRam[FN_R_LOAD_STATE], MapperStateValueType::Number8));
	entries.push_back(MapperStateEntry("$5413", "RMW Dummies Dropped", _diagRmw, MapperStateValueType::Number8));
	entries.push_back(MapperStateEntry("", "Mapper", _mapLive ? _imagePlan.mapper : 0, MapperStateValueType::Number16));
	for(int i = 0; i < NESMAP_PRG_SLOTS; i++) {
		entries.push_back(MapperStateEntry("$" + std::to_string(8 + i * 2) + "000", "PRG Slot " + std::to_string(i), _prgSlot[i], MapperStateValueType::Number8));
	}
	for(int i = 0; i < NESMAP_CHR_SLOTS; i++) {
		entries.push_back(MapperStateEntry("", "CHR Slot " + std::to_string(i), _chrSlot[i], MapperStateValueType::Number16));
	}
	entries.push_back(MapperStateEntry("", "WRAM Enabled", _serve.wram_en));
	entries.push_back(MapperStateEntry("", "WRAM Protected", _serve.wram_wp));
	entries.push_back(MapperStateEntry("", "CHR Writable", _chrWe));
	entries.push_back(MapperStateEntry("", "IRQ Enabled", _map.irq_enable));
	entries.push_back(MapperStateEntry("", "IRQ Counter", _map.irq_counter, MapperStateValueType::Number8));
	entries.push_back(MapperStateEntry("", "IRQ Latch", _map.irq_latch, MapperStateValueType::Number8));
	return entries;
}

//The mailbox's protocol state lives in fujimail's globals and in fujinet-pc,
//neither of which a save state can capture; the host keeps save states,
//rewind and run-ahead off. What is here is the bus side, kept honest so that
//nothing Mesen serializes internally can corrupt memory.
void FujiNetCart::Serialize(Serializer& s)
{
	BaseMapper::Serialize(s);
	SVArray(_prgRom, _prgSize);
	SVArray(_prgSlot, NESMAP_PRG_SLOTS);
	SVArray(_chrSlot, NESMAP_CHR_SLOTS);
	SV(_prgWe);
	SV(_chrWe);
	SV(_mirror);
	SV(_mapLive);
	SV(_serve.sram_en);
	SV(_serve.mailbox);
	SV(_serve.wram_en);
	SV(_serve.wram_wp);
	SV(_serve.loading);
	SV(_lsSlice);
	SV(_lsSeq);
	SV(_lsAckPending);
	SV(_diagRmw);
	SV(_a12LowClock);

	uint8_t lsState = (uint8_t)_lsState;
	SV(lsState);

	nesmap_t map = _map;
	map.desc = nullptr;
	uint8_t* mapBytes = (uint8_t*)&map;
	SVArray(mapBytes, (uint32_t)sizeof(map));

	if(!s.IsSaving()) {
		_lsState = (LoadState)lsState;
		map.desc = _mapLive ? nesmap_find(map.plan.mapper) : nullptr;
		_map = map;
		_events.clear();
		ApplyMirroring();
		ApplyMapping();
	}
}
