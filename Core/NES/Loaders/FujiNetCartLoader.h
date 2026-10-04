#pragma once
#include "pch.h"
#include "NES/Loaders/BaseLoader.h"

struct RomData;

//Loads an iNES image onto the FujiNet cartridge rather than onto the board
//its header names: the cartridge parses the header itself (nesmap) and maps
//the image through its own SRAMs, as the hardware does.
class FujiNetCartLoader : public BaseLoader
{
public:
	using BaseLoader::BaseLoader;

	void LoadRom(RomData& romData, vector<uint8_t>& romFile);
};
