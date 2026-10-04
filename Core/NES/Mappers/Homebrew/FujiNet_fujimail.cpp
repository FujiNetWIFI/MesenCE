// Compiles the cartridge firmware's own fujimail.c (vendored verbatim in FujiNet/
// by FujiNet/sync.sh) as C++, the way the MAME device does. One translation
// unit per source keeps their file-scope statics apart.
#include "pch.h"
#include "NES/Mappers/Homebrew/FujiNet/fujimail.c"
