#pragma once

#include <iostream>

// number of races * 2; I've yet to determine what data exactly is stored here
// actual type doesn't really matter, but to make it neat, I assumed uint32
static uint32_t memoryTable[64] = { 0 };

// array of pointers to race name strings
static uint32_t raceNameTable[32] = { 0 };

// something to point to in remaining unfilled entries
static uint32_t dummy = 0;

// Custom race: slot 12 (stock "FelOrc") is repurposed as the playable High Elf.
// The client builds character body-texture paths from this table by race id, so
// slot 12 must name the High Elf asset folder ("Character\HighElf\...") instead
// of "FelOrc" (which paints High Elves green). The string lives in the DLL's
// data segment, which stays mapped for the life of the process.
static const char* const highElfName = "HighElf";
