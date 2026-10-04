#include "pch.h"
#include "NES/Loaders/FujiNetCartLoader.h"
#include "NES/RomData.h"
#include "NES/NesHeader.h"
#include "NES/MapperFactory.h"
#include "NES/Mappers/Homebrew/FujiNetCart.h"
#include "Utilities/CRC32.h"
#include "Utilities/HexUtilities.h"

void FujiNetCartLoader::LoadRom(RomData& romData, vector<uint8_t>& romFile)
{
	string err;
	if(!FujiNetCart::CheckImage(romFile.data(), (uint32_t)romFile.size(), err)) {
		Log("[FujiNet] " + err);
		MessageManager::DisplayMessage("Error", err);
		romData.Error = true;
		return;
	}

	NesHeader header;
	memcpy(&header, romFile.data(), sizeof(NesHeader));

	//What the console sees is the cartridge, not the board the header names:
	//the image goes into the cart's SRAMs, banked by its own nesmap.
	romData.Info.Format = RomFormat::iNes;
	romData.Info.Header = header;
	romData.Info.IsNes20Header = false;
	romData.Info.MapperID = MapperFactory::FujiNetCartMapperID;
	romData.Info.SubMapperID = 0;
	romData.Info.FilePrgOffset = sizeof(NesHeader);
	romData.Info.Mirroring = MirroringType::Vertical;
	romData.Info.HasBattery = false;
	romData.Info.HasEpsm = false;
	romData.Info.HasTrainer = false;
	romData.Info.BusConflicts = BusConflictType::No;
	romData.Info.InputType = GameInputType::Unspecified;

	GameSystem system = header.GetNesGameSystem();
	romData.Info.System = system == GameSystem::NesPal ? GameSystem::NesPal : GameSystem::NesNtsc;

	romData.PrgRom.assign(NESMAP_PRG_MAX, 0xFF);
	romData.ChrRom.clear();
	romData.ChrRamSize = NESMAP_CHR_MAX;
	romData.SaveChrRamSize = 0;
	romData.WorkRamSize = NESMAP_WRAM_MAX;
	romData.SaveRamSize = 0;

	romData.Info.Hash.PrgChrCrc32 = CRC32::GetCRC(romFile.data() + sizeof(NesHeader), romFile.size() - sizeof(NesHeader));
	romData.Info.Hash.PrgCrc32 = romData.Info.Hash.PrgChrCrc32;

	Log("[FujiNet] Image mapper: " + std::to_string(header.GetMapperID()) + ", loaded onto the FujiNet cartridge");
	Log("[FujiNet] PRG+CHR CRC32: 0x" + HexUtilities::ToHex(romData.Info.Hash.PrgChrCrc32, true));
}
