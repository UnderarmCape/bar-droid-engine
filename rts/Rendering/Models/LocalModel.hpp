#pragma once

#include <vector>
#include <array>
#include <cstdint>

#include "LocalModelPiece.hpp"
#include "Sim/Misc/CollisionVolume.h"
#include "Lua/LuaObjectMaterial.h"

struct S3DModel;
struct S3DModelPiece;

struct LocalModel
{
	CR_DECLARE_STRUCT(LocalModel)

	LocalModel() {}
	~LocalModel() { pieces.clear(); }


	bool HasPiece(unsigned int i) const { return (i < pieces.size()); }
	bool Initialized() const { return (!pieces.empty()); }

	const LocalModelPiece* GetPiece(unsigned int i)  const { assert(HasPiece(i)); return &pieces[i]; }
	      LocalModelPiece* GetPiece(unsigned int i)        { assert(HasPiece(i)); return &pieces[i]; }

	const LocalModelPiece* GetRoot() const { return (GetPiece(0)); }
	const CollisionVolume* GetBoundingVolume() const { return &boundingVolume; }

	const LuaObjectMaterialData* GetLuaMaterialData() const { return &luaMaterialData; }
	      LuaObjectMaterialData* GetLuaMaterialData()       { return &luaMaterialData; }

	const float3 GetRelMidPos() const { return (boundingVolume.GetOffsets()); }

	// raw forms, the piece-index must be valid
	const float3 GetRawPiecePos(int pieceIdx) const { return pieces[pieceIdx].GetAbsolutePos(); }

	// used by all SolidObject's; accounts for piece movement
	float GetDrawRadius() const { return (boundingVolume.GetBoundingRadius()); }


	void Draw() const {
		if (!luaMaterialData.Enabled()) {
			DrawPieces();
			return;
		}

		DrawPiecesLOD(luaMaterialData.GetCurrentLOD());
	}

	void SetModel(const S3DModel* model, bool initialize = true);
	void SetLODCount(unsigned int lodCount);
	void UpdateBoundingVolume();

	void GetBoundingBoxVerts(std::vector<float3>& verts) const {
		verts.resize(8 + 2); GetBoundingBoxVerts(&verts[0]);
	}

	void GetBoundingBoxVerts(float3* verts) const {
		const float3 bbMins = GetRelMidPos() - boundingVolume.GetHScales();
		const float3 bbMaxs = GetRelMidPos() + boundingVolume.GetHScales();

		// bottom
		verts[0] = float3(bbMins.x,  bbMins.y,  bbMins.z);
		verts[1] = float3(bbMaxs.x,  bbMins.y,  bbMins.z);
		verts[2] = float3(bbMaxs.x,  bbMins.y,  bbMaxs.z);
		verts[3] = float3(bbMins.x,  bbMins.y,  bbMaxs.z);
		// top
		verts[4] = float3(bbMins.x,  bbMaxs.y,  bbMins.z);
		verts[5] = float3(bbMaxs.x,  bbMaxs.y,  bbMins.z);
		verts[6] = float3(bbMaxs.x,  bbMaxs.y,  bbMaxs.z);
		verts[7] = float3(bbMins.x,  bbMaxs.y,  bbMaxs.z);
		// extrema
		verts[8] = bbMins;
		verts[9] = bbMaxs;
	}

	void SetBoundariesNeedsRecalc()       { needsBoundariesRecalc = true; }
	bool GetBoundariesNeedsRecalc() const { return needsBoundariesRecalc; }
private:
	LocalModelPiece* CreateLocalModelPieces(const S3DModelPiece* mpParent);

	void DrawPieces() const;
	void DrawPiecesLOD(unsigned int lod) const;

public:
	std::vector<LocalModelPiece> pieces;
#ifdef BARDROID_SIM
	// BAR Droid: a piece of this model was marked dirty or had its model-space transform computed since the last
	// CSolidObject::UpdatePrevFrameTransform (LocalModelPiece sets it); while false every piece's prevModelSpaceTra
	// already equals its modelSpaceTra and none is dirty, so that copy can be left out. Drawing-only data.
	mutable bool barDroidPrevStale = true;

	// BAR Droid (BarDroidAnimDirtyRange): every piece of this model that is dirty has its index in
	// [barDroidDirtyLo, barDroidDirtyHi] (LocalModelPiece::SetDirty widens it; the unit scripts' piece walk clears it
	// once every piece is clean). Starts as everything (pieces are made dirty; a loaded game's pieces too).
	int32_t barDroidDirtyLo = 0;
	int32_t barDroidDirtyHi = INT32_MAX - 1;

	void BarDroidMarkDirty(int32_t pieceIdx) {
		if (pieceIdx < 0) {
			// (a piece without its index: everything, as at the start)
			barDroidDirtyLo = 0;
			barDroidDirtyHi = INT32_MAX - 1;
			return;
		}
		barDroidDirtyLo = std::min(barDroidDirtyLo, pieceIdx);
		barDroidDirtyHi = std::max(barDroidDirtyHi, pieceIdx);
	}
	void BarDroidClearDirtyRange() {
		barDroidDirtyLo = INT32_MAX;
		barDroidDirtyHi = -1;
	}
	size_t BarDroidDirtyBegin() const { return (barDroidDirtyLo < 0) ? size_t(0) : size_t(barDroidDirtyLo); }
	size_t BarDroidDirtyEnd() const { return (barDroidDirtyHi < 0) ? size_t(0) : (size_t(barDroidDirtyHi) + 1); }
#endif

private:
	// object-oriented box; accounts for piece movement
	CollisionVolume boundingVolume;

	// custom Lua-set material this model should be rendered with
	LuaObjectMaterialData luaMaterialData;

	bool needsBoundariesRecalc = true;
};