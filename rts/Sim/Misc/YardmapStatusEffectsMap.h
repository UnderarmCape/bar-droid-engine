/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef YardmapStatusEffectsMap_H
#define YardmapStatusEffectsMap_H

#include <cassert>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>
#include <algorithm>

#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Objects/SolidObject.h"
#include "System/Log/ILog.h"

class YardmapStatusEffectsMap {
public:
	CR_DECLARE_STRUCT(YardmapStatusEffectsMap)

	enum SquareStates {
		EXIT_ONLY      = 0x01,
		BLOCK_BUILDING = 0x02,
	};

	const uint8_t& GetMapState(int x, int z) const {
		const int cx = std::clamp(x, 0, mapDims.mapxm1);
		const int cz = std::clamp(z, 0, mapDims.mapym1);
		const int tileId   = (cz / TILE_SIZE) * tileStride + (cx / TILE_SIZE);
		const int squareId = (cz & (TILE_SIZE - 1)) * TILE_SIZE  + (cx & (TILE_SIZE - 1));
		return stateMap[tileId].squares[squareId];
	}
	uint8_t& GetMapState(int x, int z) {
		return const_cast<uint8_t&>(std::as_const(*this).GetMapState(x, z));
	}

	bool AreAllFlagsSet(int x, int z, uint8_t flags) const { return (GetMapState(x, z) & flags) == flags; }
	bool AreAnyFlagsSet(int x, int z, uint8_t flags) const { return (GetMapState(x, z) & flags) != 0; }

#ifdef BARDROID_SIM
	// BAR Droid (BarDroidMoveExitOnly): the same writes, and each tile's count of exit-only squares kept with them
	void SetFlags  (int x, int z, uint8_t flags) { BarDroidChange(x, z, flags, true ); }
	void ClearFlags(int x, int z, uint8_t flags) { BarDroidChange(x, z, flags, false); }

	// false if no square of the tiles covering the clamped rectangle [xmin, xmax] x [zmin, zmax] is exit-only (then
	// no square inside it is)
	bool BarDroidAnyExitOnly(int xmin, int xmax, int zmin, int zmax) const {
		if (barDroidExitOnlyCounts.empty() || barDroidExitOnlyCounts.size() != stateMap.size())
			return true;

		const int tx0 = std::clamp(xmin, 0, mapDims.mapxm1) / TILE_SIZE;
		const int tx1 = std::clamp(xmax, 0, mapDims.mapxm1) / TILE_SIZE;
		const int tz0 = std::clamp(zmin, 0, mapDims.mapym1) / TILE_SIZE;
		const int tz1 = std::clamp(zmax, 0, mapDims.mapym1) / TILE_SIZE;

		for (int tz = tz0; tz <= tz1; ++tz) {
			for (int tx = tx0; tx <= tx1; ++tx) {
				if (barDroidExitOnlyCounts[tz * tileStride + tx] != 0)
					return true;
			}
		}

		return false;
	}
#else
	void SetFlags  (int x, int z, uint8_t flags) { GetMapState(x, z) |=  flags; }
	void ClearFlags(int x, int z, uint8_t flags) { GetMapState(x, z) &= ~flags; }
#endif

	void ClearTile(int tileId);

	void InitNewYardmapStatusEffectsMap();

	void PostLoad();

private:
	// Each tile is exactly 64 bytes — one cache line.
	// Tiles are stored in row-major order: tileId = (z/8)*tileStride + (x/8).
	// Within a tile, squares are in row-major order: squareId = (z%8)*8 + (x%8).
	static constexpr int TILE_SIZE = 8;
	static constexpr int TILE_AREA = TILE_SIZE * TILE_SIZE; // 64

	struct alignas(64) Tile {
		CR_DECLARE_STRUCT(Tile)
		uint8_t squares[TILE_AREA];
	};
	static_assert(sizeof(Tile) == 64, "Tile must be exactly one cache line");
	// resize() value-initialises Tile, which zero-initialises squares only if
	// Tile remains a trivial aggregate — enforce that here.
	static_assert(std::is_trivially_default_constructible_v<Tile>,
	              "Tile must be trivially default constructible for resize() zero-init to hold");

	int tileStride = 0; // tiles per map row, set by Init / PostLoad

	typedef std::vector<Tile> TileMapType;
	TileMapType stateMap;

#ifdef BARDROID_SIM
	void BarDroidChange(int x, int z, uint8_t flags, bool set) {
		const int cx = std::clamp(x, 0, mapDims.mapxm1);
		const int cz = std::clamp(z, 0, mapDims.mapym1);
		const int tileId = (cz / TILE_SIZE) * tileStride + (cx / TILE_SIZE);

		uint8_t& state = GetMapState(x, z);
		const bool wasExitOnly = ((state & EXIT_ONLY) != 0);

		if (set)
			state |= flags;
		else
			state &= ~flags;

		const bool isExitOnly = ((state & EXIT_ONLY) != 0);

		if (wasExitOnly == isExitOnly || static_cast<size_t>(tileId) >= barDroidExitOnlyCounts.size())
			return;

		barDroidExitOnlyCounts[tileId] += (isExitOnly? 1: -1);
	}

	void BarDroidRecountExitOnly();

	// per tile: how many of its squares have EXIT_ONLY set (not saved: counted again from stateMap after a load)
	std::vector<uint8_t> barDroidExitOnlyCounts;
#endif
};

extern YardmapStatusEffectsMap yardmapStatusEffectsMap;

struct ObjectCollisionMapHelper {
	ObjectCollisionMapHelper() {}
	ObjectCollisionMapHelper(const CSolidObject& object) {}
	ObjectCollisionMapHelper(const MoveDef& moveDef, float ypos) {}
	ObjectCollisionMapHelper(const MoveDef& moveDef) {}

	bool IsExitOnlyAt(int x, int z) const {
		return yardmapStatusEffectsMap.AreAllFlagsSet(x, z, YardmapStatusEffectsMap::EXIT_ONLY);
	}
	void SetExitOnlyAt(int x, int z) {
		yardmapStatusEffectsMap.SetFlags(x, z, YardmapStatusEffectsMap::EXIT_ONLY);
	}
	void ClearExitOnlyAt(int x, int z) {
		yardmapStatusEffectsMap.ClearFlags(x, z, YardmapStatusEffectsMap::EXIT_ONLY);
	}

	bool IsBlockBuildingAt(int x, int z) const {
		return yardmapStatusEffectsMap.AreAllFlagsSet(x, z, YardmapStatusEffectsMap::BLOCK_BUILDING);
	}
	void SetBlockBuildingAt(int x, int z) {
		yardmapStatusEffectsMap.SetFlags(x, z, YardmapStatusEffectsMap::BLOCK_BUILDING);
	}
	void ClearBlockBuildingAt(int x, int z) {
		yardmapStatusEffectsMap.ClearFlags(x, z, YardmapStatusEffectsMap::BLOCK_BUILDING);
	}
};

#endif