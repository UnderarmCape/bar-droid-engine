#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>

#include "System/creg/creg_cond.h"
#include "System/Transform.hpp"
#include "Sim/Misc/CollisionVolume.h"


struct S3DModelPiece;
struct LocalModel;

/**
 * LocalModel
 * Instance of S3DModel. Container for the geometric properties & piece visibility status of the agent's instance of a 3d model.
 */

struct LocalModelPiece
{
	CR_DECLARE_STRUCT(LocalModelPiece)

	LocalModelPiece()
		: dirty(true)
		, wasUpdated{ true }
		, noInterpolation { false }
	{}
	LocalModelPiece(const S3DModelPiece* piece);
	~LocalModelPiece();

	void AddChild(LocalModelPiece* c) { children.push_back(c); }
	void RemoveChild(LocalModelPiece* c) { children.erase(std::find(children.begin(), children.end(), c)); }
	void SetParent(LocalModelPiece* p) { parent = p; }
	void SetLocalModel(LocalModel* lm) { localModel = lm; }

	void SetLModelPieceIndex(uint32_t idx) { lmodelPieceIndex = idx; }
	void SetScriptPieceIndex(uint32_t idx) { scriptPieceIndex = idx; }
	uint32_t GetLModelPieceIndex() const { return lmodelPieceIndex; }
	uint32_t GetScriptPieceIndex() const { return scriptPieceIndex; }

	void Draw() const;
	void DrawLOD(uint32_t lod) const;
	void SetLODCount(uint32_t count);


	// on-demand functions
	void UpdatePieceSpaceTransform();
	void UpdateModelSpaceTransform(const Transform& pTra);
	void UpdateModelSpaceTransform(const LocalModelPiece* parent);
	void UpdateChildTransformRec(bool updateChildMatrices) const;
	void UpdateParentMatricesRec() const;

	Transform CalcPieceSpaceTransformOrig(const float3& p, const float3& r, float s) const;
	Transform CalcPieceSpaceTransform(const float3& p, const float3& r, float s) const;

	// note: actually OBJECT_TO_WORLD but transform is the same
	float3 GetAbsolutePos() const { return (GetModelSpaceTransform().t * WORLD_TO_OBJECT_SPACE); }

	bool GetEmitDirPos(float3& emitPos, float3& emitDir) const;

	void SetDirtyRaw(bool state) { dirty = state; }
	void SetDirty();
#ifdef BARDROID_SIM
	void SetDirtySubtree(); // (SetDirty's recursion: dirty = true down the subtree, as SetDirty did)
#endif
	bool GetDirty() const { return dirty; }
	void SetFloat3(const float3& src, float3& dst); // anim-script only
	void SetFloat(const float& src, float& dst); // anim-script only
	void SetPosition(const float3& p) { SetFloat3(p, pos); } // anim-script only
	void SetRotation(const float3& r) { SetFloat3(r, rot); } // anim-script only
	void SetScaling(const float& s) { SetFloat(s, scale); }    // anim-script only

	void SetRotationNoInterpolation(bool noInterpolate) { noInterpolation[0] = noInterpolate; }
	void SetPositionNoInterpolation(bool noInterpolate) { noInterpolation[1] = noInterpolate; }
	void SetScalingNoInterpolation (bool noInterpolate) { noInterpolation[2] = noInterpolate; }

	void SetWasUpdatedRaw(bool state = true) { wasUpdated[0] = state; }
	auto GetWasUpdated() const { return wasUpdated[0] || wasUpdated[1]; }
	void ResetWasUpdated() const; /*fake*/

	bool SetPieceSpaceMatrix(const CMatrix44f& mat);

	const float3& GetPosition() const { return pos; }
	const float3& GetRotation() const { return rot; }
	const float&  GetScaling() const { return scale; }

	const float3& GetDirection() const { return dir; }

	const Transform& GetModelSpaceTransformRaw() const { return modelSpaceTra; }
	const Transform&  GetModelSpaceTransform() const;
	const CMatrix44f& GetModelSpaceMatrix()    const;

	const CollisionVolume* GetCollisionVolume() const { return colvol; }
	      CollisionVolume* GetCollisionVolume()       { return colvol; }

	bool GetScriptVisible() const { return scriptSetVisible; }
	void SetScriptVisible(bool b);

	void SavePrevModelSpaceTransform();
	Transform GetEffectivePrevModelSpaceTransform() const;

	void PostLoad();
private:
	// pieceSpaceTra = CalcPieceSpaceTransform(pos, rot, scale), as each of the update functions did it
	void ComputePieceSpaceTransform() const;
#ifdef BARDROID_SIM
	void BarDroidComputePieceSpaceTransform() const;
#endif

	mutable CMatrix44f modelSpaceMat; // transform relative to root LMP (SYNCED), chained pieceSpaceMat's
	mutable Transform pieceSpaceTra;  // transform relative to parent LMP (SYNCED), combines <pos> and <rot>
	mutable Transform modelSpaceTra;  // transform relative to root LMP (SYNCED), chained pieceSpaceTra's

	float3 pos;      // translation relative to parent LMP, *INITIALLY* equal to original->offset
	float3 rot;      // orientation relative to parent LMP, in radians (updated by scripts)
	float scale;     // uniform scaling

	mutable std::array<bool, 3> noInterpolation; // rotate, move, scale
	mutable bool dirty;

	Transform prevModelSpaceTra;

	CollisionVolume* colvol;

	float3 dir;      // cached copy of original->GetEmitDir()

	mutable std::array<bool, 2> wasUpdated; // currFrame, prevFrame
	bool scriptSetVisible; // TODO: add (visibility) maxradius!
public:
	bool blockScriptAnims; // if true, Set{Position,Rotation} are ignored for this piece
	int32_t lmodelPieceIndex; // index of this piece into LocalModel::pieces
	int32_t scriptPieceIndex; // index of this piece into UnitScript::pieces

	std::vector<LocalModelPiece*> children;
	LocalModelPiece* parent;

	std::vector<uint32_t> lodDispLists;
	const S3DModelPiece* original;

	LocalModel* localModel;
#ifdef BARDROID_SIM
	// BAR Droid (BarDroidAnimDirtyRange): the index of the last piece of this piece's subtree in LocalModel::pieces (pre-order:
	// the subtree is [lmodelPieceIndex, bdLastDesc]); -1: not known (SetDirty then widens the range to everything)
	int32_t bdLastDesc = -1;
	void BarDroidSetLastDescendant(int32_t idx) { bdLastDesc = idx; }
private:
	// (kept at the end: the fields the animations read stay where they were)
	// BAR Droid (BarDroidPieceTrigCache): memos of pure functions, never serialized (they start empty).
	// Per axis a: bdSin[a] = math::sin(rot[a] * 0.5f) and bdCos[a] = math::cos(rot[a] * 0.5f) for the angle whose
	// bits are bdRotKey[a] (bit a of bdTrigValid). While bdTraValid: pieceSpaceTra is the compose of position bits
	// bdPosKey, rotation bits bdRotKey and scale bits bdScaleKey with the S3DModelPiece bdOriginal (any other write
	// of pieceSpaceTra clears it).
	mutable uint32_t bdRotKey[3] = {0, 0, 0};
	mutable uint32_t bdPosKey[3] = {0, 0, 0};
	mutable uint32_t bdScaleKey = 0;
	mutable float bdSin[3] = {0.0f, 0.0f, 0.0f};
	mutable float bdCos[3] = {0.0f, 0.0f, 0.0f};
	mutable const S3DModelPiece* bdOriginal = nullptr;
	mutable uint8_t bdTrigValid = 0;
	mutable bool bdTraValid = false;
#endif
};