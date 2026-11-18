#include "LocalModelPiece.hpp"

#include "3DModelPiece.hpp"
#include "LocalModel.hpp"
#include "Rendering/GL/myGL.h"
#include "System/Misc/TracyDefs.h"
#ifdef BARDROID_SIM
#include <cstring>
#include "Sim/Misc/BarDroidUnits.h"
#endif

CR_BIND(LocalModelPiece, )
CR_REG_METADATA(LocalModelPiece, (
	CR_MEMBER(pos),
	CR_MEMBER(rot),
	CR_MEMBER(scale),
	CR_MEMBER(dir),

	CR_MEMBER(prevModelSpaceTra),
	CR_MEMBER(pieceSpaceTra),
	CR_MEMBER(modelSpaceTra),
	CR_MEMBER(modelSpaceMat),

	CR_MEMBER(colvol),

	CR_IGNORED(wasUpdated),
	CR_MEMBER(noInterpolation),
	CR_MEMBER(dirty),

	CR_MEMBER(scriptSetVisible),
	CR_MEMBER(blockScriptAnims),

	CR_MEMBER(lmodelPieceIndex),
	CR_MEMBER(scriptPieceIndex),
	CR_MEMBER(parent),
	CR_MEMBER(localModel),
	CR_MEMBER(children),

	// reload
	CR_IGNORED(original),
	CR_IGNORED(lodDispLists), //FIXME GL idx!

	CR_POSTLOAD(PostLoad)
))

/** ****************************************************************************************************
 * LocalModelPiece
 */

LocalModelPiece::LocalModelPiece(const S3DModelPiece* piece)
	: dirty(true)
	, wasUpdated{ true }
	, noInterpolation{ false }

	, scriptSetVisible(true)
	, blockScriptAnims(false)

	, lmodelPieceIndex(-1)
	, scriptPieceIndex(-1)

	, original(piece)
	, parent(nullptr) // set later
{
	assert(piece != nullptr);

	pos = piece->offset;
	dir = piece->GetEmitDir();
	scale = original->scale;

	pieceSpaceTra = CalcPieceSpaceTransform(pos, rot, scale);
	prevModelSpaceTra = Transform{ };

	children.reserve(piece->children.size());

	colvol = new CollisionVolume(piece->GetCollisionVolume());
}

LocalModelPiece::~LocalModelPiece()
{
	spring::SafeDelete(colvol);
}

#ifdef BARDROID_SIM
// (LocalModel::barDroidPrevStale: this piece's transform will change or did)
#define BARDROID_PREV_STALE() do { if (localModel != nullptr) localModel->barDroidPrevStale = true; } while (false)
#else
#define BARDROID_PREV_STALE() do {} while (false)
#endif

void LocalModelPiece::SetDirty() {
	RECOIL_DETAILED_TRACY_ZONE;
#ifdef BARDROID_SIM
	// BAR Droid: the model's flags once for the whole call (every piece the recursion below marks belongs to this
	// model and lies in this piece's subtree, [lmodelPieceIndex, bdLastDesc]: BarDroidAnimDirtyRange), then the
	// stock recursion without them (2026-11-07: a load of the model's range for every piece marked made this the
	// animations' slowest part)
	if (localModel != nullptr) {
		localModel->barDroidPrevStale = true;
		// (a changed tree's subtrees are no runs of indices: everything)
		localModel->BarDroidMarkDirty(lmodelPieceIndex, localModel->barDroidPreOrder ? bdLastDesc : -1);
	}

	SetDirtySubtree();
}

void LocalModelPiece::SetDirtySubtree() {
	dirty = true;

	for (LocalModelPiece* child: children) {
		if (child->dirty)
			continue;
		child->SetDirtySubtree();
	}
#else
	dirty = true;
	BARDROID_PREV_STALE();

	for (LocalModelPiece* child: children) {
		if (child->dirty)
			continue;
		child->SetDirty();
	}
#endif
}

void LocalModelPiece::SetFloat3(const float3& src, float3& dst) {
	RECOIL_DETAILED_TRACY_ZONE;
	if (blockScriptAnims)
		return;

	if (!dirty && !dst.same(src)) {
		SetDirty();
		assert(localModel);
		localModel->SetBoundariesNeedsRecalc();
	}

	dst = src;
}

void LocalModelPiece::SetFloat(const float& src, float& dst)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (blockScriptAnims)
		return;

	if (!dirty && !(dst == src)) {
		SetDirty();
		assert(localModel);
		localModel->SetBoundariesNeedsRecalc();
	}

	dst = src;
}

void LocalModelPiece::ResetWasUpdated() const
{
	// wasUpdated needs to trigger twice because otherwise
	// once all animation of piece stops and dirty is no longer triggered
	// UpdateObjectTrasform() would exit too early and wouldn't update
	// prevModelSpaceTra, causing the piece transform to jerk between the
	// up-to-date modelSpaceTra and stale prevModelSpaceTra
	// By passing values from right to left we make sure to trigger
	// wasUpdated[0] || wasUpdated[1] at least twice after such situation
	// happens, thus uploading prevModelSpaceTra in UpdateObjectTrasform() too
	wasUpdated[1] = std::exchange(wasUpdated[0], false);

	// use this call to also reset noInterpolation
	noInterpolation = { false };
}

bool LocalModelPiece::SetPieceSpaceMatrix(const CMatrix44f& mat)
{
	return blockScriptAnims = mat.IsRotOrRotTranMatrix();
}

const Transform& LocalModelPiece::GetModelSpaceTransform() const
{
	if (dirty)
		UpdateParentMatricesRec();

	return modelSpaceTra;
}

const CMatrix44f& LocalModelPiece::GetModelSpaceMatrix() const
{
	if (dirty)
		UpdateParentMatricesRec();

	return modelSpaceMat;
}

void LocalModelPiece::SetScriptVisible(bool b)
{
	scriptSetVisible = b;
	wasUpdated[0] = true; //update for current frame
}

void LocalModelPiece::SavePrevModelSpaceTransform()
{
	prevModelSpaceTra = GetModelSpaceTransform();
}

Transform LocalModelPiece::GetEffectivePrevModelSpaceTransform() const
{
	if (!noInterpolation[0] && !noInterpolation[1] && !noInterpolation[2])
		return prevModelSpaceTra;

	const auto& lmpTransform = GetModelSpaceTransform();
	return Transform {
		noInterpolation[0] ? lmpTransform.r : prevModelSpaceTra.r,
		noInterpolation[1] ? lmpTransform.t : prevModelSpaceTra.t,
		noInterpolation[2] ? lmpTransform.s : prevModelSpaceTra.s
	};
}

void LocalModelPiece::ComputePieceSpaceTransform() const
{
#ifdef BARDROID_SIM
	if (BarDroidUnits::pieceTrigCache) {
		BarDroidComputePieceSpaceTransform();
		return;
	}
	// (written the stock way below: the memo no longer describes it)
	bdTraValid = false;
#endif
	pieceSpaceTra = CalcPieceSpaceTransform(pos, rot, scale);
}

#ifdef BARDROID_SIM
static inline uint32_t BarDroidBits(float f) { uint32_t u; std::memcpy(&u, &f, sizeof(u)); return u; }

// BAR Droid (BarDroidPieceTrigCache): CalcPieceSpaceTransform(pos, rot, scale)'s result without recomputing what is
// known. CalcPieceSpaceTransform returns the stored transform itself while blockScriptAnims is set, else
// original->ComposeTransform(pos, rot, scale), a pure function of those (bits) and of the model piece's constant baked
// transform; ComposeTransform takes FromEulerYPRNeg(-rot), whose six values are math::sin/cos(rot[a] * 0.5f) (the
// angles negated twice: the same bits). So: the stored transform when every input's bits are the last compose's; else
// the compose with each axis' sine and cosine reused while that angle's bits are unchanged
// (S3DModelPiece::BarDroidComposeTransform: ComposeTransform's operations in its order). In a late 8v8 most dirty
// pieces are children of a moved parent (unchanged themselves), and most moved ones turn about one axis.
void LocalModelPiece::BarDroidComputePieceSpaceTransform() const
{
	if (blockScriptAnims)
		return;

	const uint32_t rotKey[3] = {BarDroidBits(rot.x), BarDroidBits(rot.y), BarDroidBits(rot.z)};
	const uint32_t posKey[3] = {BarDroidBits(pos.x), BarDroidBits(pos.y), BarDroidBits(pos.z)};
	const uint32_t scaleKey = BarDroidBits(scale);

	if (bdTraValid && bdTrigValid == 7 && bdOriginal == original &&
		rotKey[0] == bdRotKey[0] && rotKey[1] == bdRotKey[1] && rotKey[2] == bdRotKey[2] &&
		posKey[0] == bdPosKey[0] && posKey[1] == bdPosKey[1] && posKey[2] == bdPosKey[2] &&
		scaleKey == bdScaleKey
	) {
		return;
	}

	for (int a = 0; a < 3; ++a) {
		if ((bdTrigValid & (1 << a)) != 0 && bdRotKey[a] == rotKey[a])
			continue;

		CQuaternion::BarDroidHalfAngleSinCos(rot[a], bdSin[a], bdCos[a]);
		bdRotKey[a] = rotKey[a];
		bdTrigValid |= uint8_t(1 << a);
	}

	pieceSpaceTra = original->BarDroidComposeTransform(pos, bdSin, bdCos, scale);

	bdPosKey[0] = posKey[0];
	bdPosKey[1] = posKey[1];
	bdPosKey[2] = posKey[2];
	bdScaleKey = scaleKey;
	bdOriginal = original;
	bdTraValid = true;
}
#endif

void LocalModelPiece::UpdatePieceSpaceTransform()
{
	ComputePieceSpaceTransform();
}

void LocalModelPiece::UpdateModelSpaceTransform(const Transform& pTra)
{
	BARDROID_PREV_STALE();
	modelSpaceTra = pTra * pieceSpaceTra;
	modelSpaceMat = modelSpaceTra.ToMatrix();
}

void LocalModelPiece::UpdateModelSpaceTransform(const LocalModelPiece* parent)
{
	BARDROID_PREV_STALE();
	if (parent)
		modelSpaceTra = parent->modelSpaceTra * pieceSpaceTra;
	else
		modelSpaceTra = pieceSpaceTra;

	modelSpaceMat = modelSpaceTra.ToMatrix();
}

void LocalModelPiece::UpdateChildTransformRec(bool updateChildTransform) const
{
	RECOIL_DETAILED_TRACY_ZONE;

	if (dirty) {
		dirty = false;
		wasUpdated[0] = true;  //update for current frame
		updateChildTransform = true;

		ComputePieceSpaceTransform();
	}

	if (updateChildTransform) {
		BARDROID_PREV_STALE();
		if (parent != nullptr)
			modelSpaceTra = parent->modelSpaceTra * pieceSpaceTra;
		else
			modelSpaceTra = pieceSpaceTra;

		modelSpaceMat = modelSpaceTra.ToMatrix();
	}

	for (auto& child : children) {
		child->UpdateChildTransformRec(updateChildTransform);
	}
}

void LocalModelPiece::UpdateParentMatricesRec() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (parent != nullptr && parent->dirty)
		parent->UpdateParentMatricesRec();

	dirty = false;
	wasUpdated[0] = true;  //update for current frame
	BARDROID_PREV_STALE();

	ComputePieceSpaceTransform();

	if (parent != nullptr)
		modelSpaceTra = parent->modelSpaceTra * pieceSpaceTra;
	else
		modelSpaceTra = pieceSpaceTra;

	modelSpaceMat = modelSpaceTra.ToMatrix();
}

Transform LocalModelPiece::CalcPieceSpaceTransformOrig(const float3& p, const float3& r, float s) const
{
	return original->ComposeTransform(p, r, s);
}

Transform LocalModelPiece::CalcPieceSpaceTransform(const float3& p, const float3& r, float s) const
{
	if (blockScriptAnims)
		return pieceSpaceTra;

	return CalcPieceSpaceTransformOrig(p, r, s);
}

void LocalModelPiece::Draw() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!scriptSetVisible)
		return;

	if (!original->HasGeometryData())
		return;

	assert(original);

	glPushMatrix();
	glMultMatrixf(GetModelSpaceMatrix());
	S3DModelHelpers::BindLegacyAttrVBOs();
	original->DrawElements();
	S3DModelHelpers::UnbindLegacyAttrVBOs();
	glPopMatrix();
}

void LocalModelPiece::DrawLOD(uint32_t lod) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!scriptSetVisible)
		return;

	if (!original->HasGeometryData())
		return;

	glPushMatrix();
	glMultMatrixf(GetModelSpaceMatrix());
	if (const auto ldl = lodDispLists[lod]; ldl == 0) {
		S3DModelHelpers::BindLegacyAttrVBOs();
		original->DrawElements();
		S3DModelHelpers::UnbindLegacyAttrVBOs();
	} else {
		glCallList(ldl);
	}
	glPopMatrix();
}



void LocalModelPiece::SetLODCount(uint32_t count)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// any new LOD's get null-lists first
	lodDispLists.resize(count, 0);

	for (uint32_t i = 0; i < children.size(); i++) {
		children[i]->SetLODCount(count);
	}
}


bool LocalModelPiece::GetEmitDirPos(float3& emitPos, float3& emitDir) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (original == nullptr)
		return false;

	// note: actually OBJECT_TO_WORLD but transform is the same
	emitPos = GetModelSpaceTransform() *        original->GetEmitPos()        * WORLD_TO_OBJECT_SPACE;
	emitDir = GetModelSpaceTransform() * float4(original->GetEmitDir(), 0.0f) * WORLD_TO_OBJECT_SPACE;
	return true;
}

void LocalModelPiece::PostLoad()
{
	wasUpdated = { true };
}