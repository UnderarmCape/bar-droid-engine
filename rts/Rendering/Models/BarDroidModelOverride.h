/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct S3DModel;

// BAR Droid (engine branch): mobile versions of the game's S3O models, for drawing only. Made on a PC ahead of time
// (BAR Droid's tools/mobile-models: far fewer triangles, the same textures and look), kept outside the game's archive
// (BarDroidModelOverrideDir, a folder found through the data dirs: the write dir), at the model's own path. Once BAR's
// model is parsed (its sizes, bounds and collision volumes taken from it as always), the mobile file replaces only the
// pieces' vertices and triangles that are uploaded and drawn, and gives them a coarser list for units small on the
// screen (BarDroidModelLodPixels). The file must have been made from this very model file (its bytes' hash), with the
// same piece tree (names, order, offsets), every piece's vertices 0 and 1 where they were (the engine aims weapons and
// effects along them) and every piece with or without triangles as before; else BAR's own model is drawn.
// The simulation never reads what changes; the archives and their checksums are untouched (online-safe).

// FNV-1a 64 of a model file's bytes (the generator stores the hash of the file it read)
inline uint64_t BarDroidFnv1a64(const uint8_t* data, size_t size)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < size; ++i) {
		h ^= data[i];
		h *= 1099511628211ull;
	}
	return h;
}

// after CS3OParser::Load (fileHash: BarDroidFnv1a64 of the file it read); true: the model now draws its mobile version
bool BarDroidModelOverride(S3DModel* model, uint64_t fileHash);

// whether the model draws its mobile version (then the load-time simplification leaves it alone)
bool BarDroidModelIsMobile(const S3DModel* model);
