/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "3DModelVAO.hpp"

#include <algorithm>
#include <iterator>

#include "3DModel.hpp"
#include "3DModelPiece.hpp"
#include "IModelParser.h"
#include "Rendering/ModelsDataUploader.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Features/Feature.h"

#include "System/Misc/TracyDefs.h"

#if defined(BARDROID_SIM) && !defined(HEADLESS)
#include <cstring>
#include "Rendering/GlobalRendering.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

// BAR Droid (engine branch): every Submit writes the instance buffer from its start, and every immediate draw (units
// being built, build previews) one slot of it, while the frame's earlier draws still read it. A tiler's driver keeps
// those draws' data by copying the whole buffer (1.08 MB) first, or stalls. 1: its storage given up before each
// batched write (a fresh one, nothing copied; the earlier draws keep theirs); 2: before each immediate write as well
// (both measured slower on the dev phone's Freedreno); 3: every write into a persistently mapped ring of two halves
// instead, the instance attribute pointed at it (no driver sync per write: a fence per half). Drawing only; read every
// drawn frame (in-game A/B).
CONFIG(int, BarDroidInstanceOrphan).defaultValue(0).minimumValue(0).maximumValue(3)
	.description("BAR Droid: model instance data 1: storage given up before batched writes, 2: before every write, 3: written into a persistently mapped ring; 0: written while in use");

static int BarDroidInstanceOrphan()
{
	static uint32_t lastFrame = ~0u;
	static int mode = 0;
	if (globalRendering->drawFrame != lastFrame) {
		lastFrame = globalRendering->drawFrame;
		mode = configHandler->GetInt("BarDroidInstanceOrphan");
	}
	return mode;
}
#endif


void S3DModelVAO::EnableAttribs(bool inst) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!inst) {
		for (int i = 0; i <= 5; ++i) {
			glEnableVertexAttribArray(i);
			glVertexAttribDivisor(i, 0);
		}

		glVertexAttribPointer (0, 3, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, pos         ));
		glVertexAttribPointer (1, 3, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, normal      ));
		glVertexAttribPointer (2, 3, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, sTangent    ));
		glVertexAttribPointer (3, 3, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, tTangent    ));
		glVertexAttribPointer (4, 4, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, texCoords[0]));
		glVertexAttribIPointer(5, 3, GL_UNSIGNED_INT,        sizeof(SVertexData), (const void*)offsetof(SVertexData, boneIDsLow  ));
	}
	else {
		for (int i = 6; i <= 6; ++i) {
			glEnableVertexAttribArray(i);
			glVertexAttribDivisor(i, 1);
		}

		// covers all 4 uints of SInstanceData
		glVertexAttribIPointer(6, 4, GL_UNSIGNED_INT, sizeof(SInstanceData), (const void*)offsetof(SInstanceData, matOffset));
	}
}

#if defined(BARDROID_SIM) && !defined(HEADLESS)
bool S3DModelVAO::BarDroidRingReady()
{
	if (barDroidRingBuf != 0)
		return true;
	if (barDroidRingFailed)
		return false;
	const GLsizeiptr size = GLsizeiptr(2) * BARDROID_RING_HALF * sizeof(SInstanceData);
	glGenBuffers(1, &barDroidRingBuf);
	glBindBuffer(GL_ARRAY_BUFFER, barDroidRingBuf);
	if (GLAD_GL_ARB_buffer_storage) {
		// mapped once, for good
		const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		glBufferStorage(GL_ARRAY_BUFFER, size, nullptr, flags);
		barDroidRingPtr = static_cast<uint8_t*>(glMapBufferRange(GL_ARRAY_BUFFER, 0, size, flags));
	} else {
		// (Mesa without coherent persistent mapping, the dev phone's Freedreno): each write's range mapped unsynchronized
		glBufferData(GL_ARRAY_BUFFER, size, nullptr, GL_STREAM_DRAW);
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	if (GLAD_GL_ARB_buffer_storage && barDroidRingPtr == nullptr) {
		LOG_L(L_WARNING, "[S3DModelVAO] BAR Droid: the instance ring could not be mapped, instance data as before");
		glDeleteBuffers(1, &barDroidRingBuf);
		barDroidRingBuf = 0;
		barDroidRingFailed = true;
		return false;
	}
	LOG("[S3DModelVAO] BAR Droid: instance data through a ring (%u KB, %s)", unsigned(size / 1024),
		(barDroidRingPtr != nullptr) ? "mapped persistently" : "each write mapped unsynchronized");
	return true;
}

// count instances' room in the ring (a half's end is not crossed); entering a half waits for the fence set when it was
// last left (half a ring of draws ago: done by then), leaving one sets its fence
uint32_t S3DModelVAO::BarDroidRingWrite(const SInstanceData* data, uint32_t count)
{
	assert(count <= BARDROID_RING_HALF);
	if (barDroidRingPos + count > (barDroidRingHalf + 1) * BARDROID_RING_HALF) {
		barDroidRingFence[barDroidRingHalf] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		barDroidRingHalf ^= 1;
		if (GLsync& f = barDroidRingFence[barDroidRingHalf]; f != nullptr) {
			glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
			glDeleteSync(f);
			f = nullptr;
		}
		barDroidRingPos = barDroidRingHalf * BARDROID_RING_HALF;
	}
	const uint32_t pos = barDroidRingPos;
	const size_t bytes = size_t(count) * sizeof(SInstanceData);
	if (barDroidRingPtr != nullptr) {
		memcpy(barDroidRingPtr + size_t(pos) * sizeof(SInstanceData), data, bytes);
	} else {
		glBindBuffer(GL_ARRAY_BUFFER, barDroidRingBuf);
		void* dst = glMapBufferRange(GL_ARRAY_BUFFER, GLintptr(pos) * sizeof(SInstanceData), GLsizeiptr(bytes),
			GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
		if (dst != nullptr) {
			memcpy(dst, data, bytes);
			glUnmapBuffer(GL_ARRAY_BUFFER);
		} else {
			glBufferSubData(GL_ARRAY_BUFFER, GLintptr(pos) * sizeof(SInstanceData), GLsizeiptr(bytes), data);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
	barDroidRingPos += count;
	return pos;
}

// (the model VAO bound) the instance attribute read from the ring or from instVBO
void S3DModelVAO::BarDroidPointInstances(bool ring)
{
	if (ring == barDroidOnRing)
		return;
	glBindBuffer(GL_ARRAY_BUFFER, ring ? barDroidRingBuf : instVBO.GetId());
	glVertexAttribIPointer(6, 4, GL_UNSIGNED_INT, sizeof(SInstanceData), (const void*)offsetof(SInstanceData, matOffset));
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	barDroidOnRing = ring;
}
#endif
void S3DModelVAO::DisableAttribs() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (int i = 0; i <= 6; ++i) {
		glDisableVertexAttribArray(i);
		glVertexAttribDivisor(i, 0);
	}
}

S3DModelVAO::S3DModelVAO()
{
	RECOIL_DETAILED_TRACY_ZONE;
	vertData.reserve(VERT_SIZE0);
	indxData.reserve(INDX_SIZE0);

	vertVBO = VBO{ GL_ARRAY_BUFFER        , false };
	indxVBO = VBO{ GL_ELEMENT_ARRAY_BUFFER, false };
	instVBO = VBO{ GL_ARRAY_BUFFER        , false };

	//no better place to init it
	instVBO.Bind();
	instVBO.New(S3DModelVAO::INSTANCE_BUFFER_NUM_ELEMS * sizeof(SInstanceData), GL_STREAM_DRAW);
	instVBO.Unbind();
}

std::unique_ptr<S3DModelVAO> S3DModelVAO::instance = nullptr;

void S3DModelVAO::ProcessVertices(const S3DModel* model)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);
	assert(model->loadStatus == S3DModel::LoadStatus::LOADING);

	if (const auto* root = model->GetRootPiece(); root->vertIndex != ~0u)
		return;

	uint32_t vertIndex = GetVertOffset(); // (BAR Droid: past the vertices already uploaded and given back)
	for (auto* modelPiece : model->pieceObjects) {
		modelPiece->vertIndex = vertIndex;
		const auto& modelPieceVerts = modelPiece->GetVerticesVec();
		vertIndex += modelPieceVerts.size();
		vertData.insert(vertData.end(), modelPieceVerts.begin(), modelPieceVerts.end()); //append
	}
}

void S3DModelVAO::ProcessIndicies(S3DModel* model)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);
	if (model->indxStart != ~0u)
		return;

#if defined(BARDROID_SIM)
	const size_t ib = indxBase; // (BAR Droid: past the indices already uploaded and given back)
#else
	constexpr size_t ib = 0;
#endif
	//models should know their index offset
	model->indxStart = static_cast<uint32_t>(ib + std::distance(indxData.cbegin(), indxData.cend()));

	for (auto* modelPiece : model->pieceObjects) {
		if (!modelPiece->HasGeometryData()) {
			modelPiece->indxStart = static_cast<uint32_t>(ib + indxData.size());
			modelPiece->indxCount = 0;
			continue;
		}

		const auto& modelPieceIndcs = modelPiece->GetIndicesVec();
		indxData.insert(indxData.end(), modelPieceIndcs.begin(), modelPieceIndcs.end()); //append

		const auto endIdx = indxData.end();
		const auto begIdx = endIdx - modelPieceIndcs.size();

		std::for_each(begIdx, endIdx, [offset = modelPiece->vertIndex](uint32_t& indx) { indx += offset; }); // add per piece vertex offset to indices

		//model pieces should know their index offset
		modelPiece->indxStart = static_cast<uint32_t>(ib + std::distance(indxData.begin(), begIdx));

		//model pieces should know their index count
		modelPiece->indxCount = static_cast<uint32_t>(modelPieceIndcs.size());
	}
	//models should know their index count
	model->indxCount = static_cast<uint32_t>(ib + indxData.size() - model->indxStart);

	//add shatter indices to the end of indxData
	for (const auto* modelPiece : model->pieceObjects) {
		if (!modelPiece->HasGeometryData())
			continue;

		const auto& mdlPcsShatIndcs = modelPiece->GetShatterIndicesVec();

		indxData.insert(indxData.end(), mdlPcsShatIndcs.begin(), mdlPcsShatIndcs.end()); //append

		const auto endIdx = indxData.end();
		const auto begIdx = endIdx - mdlPcsShatIndcs.size();

		std::for_each(begIdx, endIdx, [offset = modelPiece->vertIndex](uint32_t& indx) { indx += offset; }); // add per piece vertex offset to indices
	}

#if defined(BARDROID_SIM)
	// BAR Droid (BarDroidModelLodShare): the coarse copy after them, every piece in it (its coarse list, else its own),
	// so one draw of the range is the whole model, as with the full list
	bool anyLod = false;
	for (const auto* modelPiece : model->pieceObjects)
		anyLod |= !modelPiece->barDroidLodIndices.empty();
	if (anyLod) {
		model->barDroidLodStart = static_cast<uint32_t>(ib + indxData.size());
		for (auto* modelPiece : model->pieceObjects) {
			if (!modelPiece->HasGeometryData())
				continue;
			const auto& lod = modelPiece->barDroidLodIndices.empty() ? modelPiece->GetIndicesVec() : modelPiece->barDroidLodIndices;
			indxData.insert(indxData.end(), lod.begin(), lod.end());
			const auto endIdx = indxData.end();
			std::for_each(endIdx - lod.size(), endIdx, [offset = modelPiece->vertIndex](uint32_t& indx) { indx += offset; });
			std::vector<uint32_t>().swap(modelPiece->barDroidLodIndices);   // (in the buffer now)
		}
		model->barDroidLodCount = static_cast<uint32_t>(ib + indxData.size() - model->barDroidLodStart);
	}
#endif
}

void S3DModelVAO::CreateVAO()
{
	RECOIL_DETAILED_TRACY_ZONE;
	vao = VAO{};
	vao.Bind();

	vertVBO.Bind();
	indxVBO.Bind();
	EnableAttribs(false); // vertex attribs
	vertVBO.Unbind();

	instVBO.Bind();
	EnableAttribs(true); // instance attribs

	vao.Unbind();
	DisableAttribs();

	indxVBO.Unbind();
	instVBO.Unbind();
#if defined(BARDROID_SIM) && !defined(HEADLESS)
	barDroidOnRing = false; // (a new VAO reads instVBO)
#endif
}

#if defined(BARDROID_SIM)
// BAR Droid (8v8 replays on 8 GB phones, 2026-11-04): what is new is uploaded at the buffer's running end and its CPU
// copy given back at once (vertBase/indxBase count what was uploaded before), in either load mode: preloaded, the
// models' copies were 440 MB kept for the whole game (clear() keeps the memory); loaded as needed (PreloadModels = 0),
// they were never given back. And a buffer too small grows by half at least: in 8 MB steps every step made the whole
// buffer anew and copied it (~1.9 GB of GPU memory in 8 s).
void S3DModelVAO::UploadVBOs()
{
	RECOIL_DETAILED_TRACY_ZONE;
	static constexpr size_t MEM_STEP = 8 * 1024 * 1024;
	bool reinitVAO = (vao.GetIdRaw() == 0);
	const auto newSize = [](size_t need, size_t have) {
		if (need <= have)
			return have;
		return std::max(AlignUp(need, MEM_STEP), (have > 0) ? AlignUp(have * 3 / 2, MEM_STEP) : size_t(0));
	};

	if (!vertData.empty()) {
		vertVBO.Bind();
		const size_t total = vertBase + vertData.size();
		const size_t reqSize = newSize(std::max(total, S3DModelVAO::VERT_SIZE0) * sizeof(SVertexData), vertVBO.GetSize());
		reinitVAO |= (reqSize > vertVBO.GetSize());
		vertVBO.Resize(reqSize, GL_STATIC_DRAW); //noop if size hasn't changed, will copy data if changed
		vertVBO.SetBufferSubData(vertBase * sizeof(SVertexData), vertData.size() * sizeof(SVertexData), vertData.data());
		vertVBO.Unbind();
		vertBase = total;
		vertUploadSize = total;
		std::vector<SVertexData>().swap(vertData);
	}

	if (!indxData.empty()) {
		indxVBO.Bind();
		const size_t total = indxBase + indxData.size();
		const size_t reqSize = newSize(std::max(total, S3DModelVAO::INDX_SIZE0) * sizeof(uint32_t), indxVBO.GetSize());
		reinitVAO |= (reqSize > indxVBO.GetSize());
		indxVBO.Resize(reqSize, GL_STATIC_DRAW);
		indxVBO.SetBufferSubData(indxBase * sizeof(uint32_t), indxData.size() * sizeof(uint32_t), indxData.data());
		indxVBO.Unbind();
		indxBase = total;
		indxUploadSize = total;
		std::vector<uint32_t>().swap(indxData);
	}

	if (reinitVAO)
		CreateVAO();
}
#else
void S3DModelVAO::UploadVBOs()
{
	RECOIL_DETAILED_TRACY_ZONE;
	static constexpr size_t MEM_STEP = 8 * 1024 * 1024;
	bool reinitVAO = (vao.GetIdRaw() == 0);

	if (vertData.size() > vertUploadIndex) {
		assert(!safeToDeleteVectors);
		vertVBO.Bind();
		const size_t reqSize = AlignUp(std::max(vertData.size(), S3DModelVAO::VERT_SIZE0) * sizeof(SVertexData), MEM_STEP);
		reinitVAO |= (reqSize > vertVBO.GetSize());
		vertVBO.Resize(reqSize, GL_STATIC_DRAW); //noop if size hasn't changed, will copy data if changed
		vertVBO.SetBufferSubData(vertUploadIndex * sizeof(SVertexData), (vertData.size() - vertUploadIndex) * sizeof(SVertexData), vertData.data() + vertUploadIndex);
		vertVBO.Unbind();
		vertUploadIndex = vertData.size();
		vertUploadSize = vertUploadIndex;
	}

	if (indxData.size() > indxUploadIndex) {
		assert(!safeToDeleteVectors);
		indxVBO.Bind();
		const size_t reqSize = AlignUp(std::max(indxData.size(), S3DModelVAO::INDX_SIZE0) * sizeof(   uint32_t), MEM_STEP);
		reinitVAO |= (reqSize > indxVBO.GetSize());
		indxVBO.Resize(reqSize, GL_STATIC_DRAW); //noop if size hasn't changed, will copy data if changed
		indxVBO.SetBufferSubData(indxUploadIndex * sizeof(   uint32_t), (indxData.size() - indxUploadIndex) * sizeof(   uint32_t), indxData.data() + indxUploadIndex);
		indxVBO.Unbind();
		indxUploadIndex = indxData.size();
		indxUploadSize = indxUploadIndex;
	}

	if (reinitVAO)
		CreateVAO();

	if (safeToDeleteVectors && !vertData.empty()) {
		// all models have been uploaded in the calls above
		// safe to clear CPU copy of the data
		vertData.clear();
		indxData.clear();
		vertUploadIndex = 0;
		indxUploadIndex = 0;
	}
}
#endif

void S3DModelVAO::Init()
{
	RECOIL_DETAILED_TRACY_ZONE;
	Kill();
	instance = std::make_unique<S3DModelVAO>();
}

void S3DModelVAO::Kill()
{
	RECOIL_DETAILED_TRACY_ZONE;
	instance = nullptr;
}

void S3DModelVAO::Bind() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(vao.GetIdRaw() > 0);
	vao.Bind();
}

void S3DModelVAO::Unbind() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(vao.GetIdRaw() > 0);
	vao.Unbind();
}

void S3DModelVAO::BindLegacyVertexAttribsAndVBOs() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	vertVBO.Bind();
	indxVBO.Bind();

	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, pos)));

	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, normal)));

	glClientActiveTexture(GL_TEXTURE0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, texCoords[0])));

	glClientActiveTexture(GL_TEXTURE1);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, texCoords[1])));

	glClientActiveTexture(GL_TEXTURE5);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(3, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, sTangent)));

	glClientActiveTexture(GL_TEXTURE6);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(3, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, tTangent)));
}

void S3DModelVAO::UnbindLegacyVertexAttribsAndVBOs() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	glClientActiveTexture(GL_TEXTURE6);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glClientActiveTexture(GL_TEXTURE5);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glClientActiveTexture(GL_TEXTURE1);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glClientActiveTexture(GL_TEXTURE0);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);

	indxVBO.Unbind();
	vertVBO.Unbind();
}

void S3DModelVAO::DrawElements(GLenum prim, uint32_t vboIndxStart, uint32_t vboIndxCount) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	glDrawElements(prim, vboIndxCount, GL_UNSIGNED_INT, indxVBO.GetPtr(vboIndxStart * sizeof(uint32_t)));
}

bool S3DModelVAO::EmplaceInstance(uint32_t indexStart, uint32_t indexCount, uint32_t traIndex, uint16_t paletteIndex, uint16_t numPieces, uint32_t uniIndex, uint32_t bposeIndex)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (traIndex == TransformsMemStorage::INVALID_INDEX || bposeIndex == TransformsMemStorage::INVALID_INDEX)
		return false;

	modelDataToInstance[SIndexAndCount{ indexStart, indexCount }].emplace_back(SInstanceData(
		traIndex,
		paletteIndex,
		numPieces,
		uniIndex,
		bposeIndex
	));

	return true;
}

template<typename TObj>
bool S3DModelVAO::AddToSubmissionImpl(const TObj* obj, uint32_t indexStart, uint32_t indexCount, uint16_t paletteIndex)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto traIndex = transformsUploader.GetElemOffset(obj);
	const auto uniIndex = modelUniformsStorage.GetObjOffset(obj); //doesn't need to exist for defs and models. Don't check for validity

	uint16_t numPieces = 0;
	size_t bposeIndex = 0;
	if constexpr (std::is_same<TObj, S3DModel>::value) {
		numPieces = static_cast<uint16_t>(obj->numPieces);
		bposeIndex = transformsUploader.GetElemOffset(obj);
	}
	else {
		numPieces = static_cast<uint16_t>(obj->model->numPieces);
		bposeIndex = transformsUploader.GetElemOffset(obj->model);
	}

	return EmplaceInstance(
		indexStart, indexCount,
		static_cast<uint32_t>(traIndex),
		paletteIndex,
		numPieces,
		static_cast<uint32_t>(uniIndex),
		static_cast<uint32_t>(bposeIndex)
	);
}

bool S3DModelVAO::AddToSubmission(const S3DModel* model, uint16_t paletteIndex)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);

	return AddToSubmissionImpl(model, model->indxStart, model->indxCount, paletteIndex);
}

bool S3DModelVAO::AddToSubmission(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(unit);

	const S3DModel* model = unit->model;
	assert(model);

	return AddToSubmissionImpl(unit, model->indxStart, model->indxCount, unit->paletteIndex);
}

bool S3DModelVAO::AddToSubmissionLod(const CUnit* unit)
{
	const S3DModel* model = unit->model;
	if (model == nullptr || model->barDroidLodCount == 0)
		return false;
	return AddToSubmissionImpl(unit, model->barDroidLodStart, model->barDroidLodCount, unit->paletteIndex);
}

bool S3DModelVAO::AddToSubmission(const CFeature* feature)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(feature);

	const S3DModel* model = feature->model;
	assert(model);

	return AddToSubmissionImpl(feature, model->indxStart, model->indxCount, feature->paletteIndex);
}

bool S3DModelVAO::AddToSubmission(const UnitDef* unitDef, uint16_t paletteIndex)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(unitDef);

	const S3DModel* model = unitDef->model;
	assert(model);

	return AddToSubmissionImpl(unitDef, model->indxStart, model->indxCount, paletteIndex);
}

bool S3DModelVAO::AddStaticInstance(const S3DModel* model, uint32_t worldTransformOffset, uint16_t paletteIndex)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);

	// the world transform is the caller-supplied slot; pieces are read from the model bind pose
	return EmplaceInstance(
		model->indxStart, model->indxCount,
		worldTransformOffset,
		paletteIndex,
		static_cast<uint16_t>(model->numPieces),
		static_cast<uint32_t>(modelUniformsStorage.GetObjOffset(model)),
		static_cast<uint32_t>(transformsUploader.GetElemOffset(model))
	);
}


void S3DModelVAO::Submit(GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	static std::vector<SDrawElementsIndirectCommand> submitCmds;
	submitCmds.clear();

	batchedBaseInstance = 0u;

	static std::vector<SInstanceData> allRenderModelData;
	allRenderModelData.reserve(INSTANCE_BUFFER_NUM_BATCHED);
	allRenderModelData.clear();

	for (const auto& [indxCount, renderModelData] : modelDataToInstance) {
		if (allRenderModelData.size() + renderModelData.size() >= INSTANCE_BUFFER_NUM_BATCHED)
			continue;

		SDrawElementsIndirectCommand scmd{
			indxCount.count,
			static_cast<uint32_t>(renderModelData.size()),
			indxCount.index,
			0u,
			batchedBaseInstance
		};

		submitCmds.emplace_back(scmd);

		allRenderModelData.insert(allRenderModelData.end(), renderModelData.cbegin(), renderModelData.cend());
		batchedBaseInstance += renderModelData.size();
	}

	if (submitCmds.empty())
		return;

#if defined(BARDROID_SIM) && !defined(HEADLESS)
	if (BarDroidInstanceOrphan() == 3 && BarDroidRingReady()) {
		const uint32_t base = BarDroidRingWrite(allRenderModelData.data(), static_cast<uint32_t>(allRenderModelData.size()));
		for (auto& cmd : submitCmds)
			cmd.baseInstance += base;
		if (bindUnbind)
			Bind();
		BarDroidPointInstances(true);
		glMultiDrawElementsIndirect(mode, GL_UNSIGNED_INT, submitCmds.data(), submitCmds.size(), sizeof(SDrawElementsIndirectCommand));
		if (bindUnbind)
			Unbind();
		modelDataToInstance.clear();
		return;
	}
#endif
	instVBO.Bind();
#if defined(BARDROID_SIM) && !defined(HEADLESS)
	if (BarDroidInstanceOrphan() >= 1)
		instVBO.Invalidate();
#endif
	instVBO.SetBufferSubData(allRenderModelData);
	instVBO.Unbind();

	if (bindUnbind)
		Bind();
#if defined(BARDROID_SIM) && !defined(HEADLESS)
	BarDroidPointInstances(false);
#endif

	glMultiDrawElementsIndirect(mode, GL_UNSIGNED_INT, submitCmds.data(), submitCmds.size(), sizeof(SDrawElementsIndirectCommand));

	if (bindUnbind)
		Unbind();

	modelDataToInstance.clear();
}

template<typename TObj>
bool S3DModelVAO::SubmitImmediatelyImpl(const TObj* obj, uint32_t indexStart, uint32_t indexCount, uint16_t paletteIndex, GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	std::size_t traIndex = transformsUploader.GetElemOffset(obj);
	if (traIndex == TransformsMemStorage::INVALID_INDEX)
		return false;

	const auto uniIndex = modelUniformsStorage.GetObjOffset(obj); //doesn't need to exist for defs. Don't check for validity

	uint16_t numPieces = 0;
	size_t bposeIndex = 0;
	if constexpr (std::is_same<TObj, S3DModel>::value) {
		numPieces = static_cast<uint16_t>(obj->numPieces);
		bposeIndex = transformsUploader.GetElemOffset(obj);
	}
	else {
		numPieces = static_cast<uint16_t>(obj->model->numPieces);
		bposeIndex = transformsUploader.GetElemOffset(obj->model);
	}

	SInstanceData instanceData(static_cast<uint32_t>(traIndex), paletteIndex, numPieces, uniIndex, bposeIndex);
	const uint32_t immediateBaseInstanceAbs = INSTANCE_BUFFER_NUM_BATCHED + immediateBaseInstance;

	static SDrawElementsIndirectCommand scmd;
#if defined(BARDROID_SIM) && !defined(HEADLESS)
	if (BarDroidInstanceOrphan() == 3 && BarDroidRingReady()) {
		scmd = { indexCount, 1, indexStart, 0u, BarDroidRingWrite(&instanceData, 1) };
		if (bindUnbind)
			Bind();
		BarDroidPointInstances(true);
		glMultiDrawElementsIndirect(mode, GL_UNSIGNED_INT, &scmd, 1u, sizeof(SDrawElementsIndirectCommand));
		if (bindUnbind)
			Unbind();
		return true;
	}
#endif
	scmd = {
		indexCount,
		1,
		indexStart,
		0u,
		immediateBaseInstanceAbs
	};

	instVBO.Bind();
#if defined(BARDROID_SIM) && !defined(HEADLESS)
	if (BarDroidInstanceOrphan() >= 2)
		instVBO.Invalidate();
#endif
	instVBO.SetBufferSubData(immediateBaseInstanceAbs * sizeof(SInstanceData), sizeof(SInstanceData), &instanceData);
	instVBO.Unbind();

	immediateBaseInstance = (immediateBaseInstance + 1) % INSTANCE_BUFFER_NUM_IMMEDIATE;

	if (bindUnbind)
		Bind();
#if defined(BARDROID_SIM) && !defined(HEADLESS)
	BarDroidPointInstances(false);
#endif

	// As of 01.05.2023 AMD Windows drivers do not support baseInstance field of SDrawElementsIndirectCommand
	// therefore can't use glDrawElementsIndirect
	// At the same time AMD Windows drivers sometimes crash on glDrawElementsInstancedBaseInstance
	// can't use it either
	// Revert to glMultiDrawElementsIndirect as it works reliably

	glMultiDrawElementsIndirect(mode, GL_UNSIGNED_INT, &scmd, 1u, sizeof(SDrawElementsIndirectCommand));

	if (bindUnbind)
		Unbind();

	return true;
}

bool S3DModelVAO::SubmitImmediately(const S3DModel* model, uint16_t paletteIndex, GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);
	return SubmitImmediatelyImpl(model, model->indxStart, model->indxCount, paletteIndex, mode, bindUnbind);
}

bool S3DModelVAO::SubmitImmediately(const CUnit* unit, const GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(unit);

	const S3DModel* model = unit->model;
	assert(model);

	return SubmitImmediatelyImpl(unit, model->indxStart, model->indxCount, unit->paletteIndex, mode, bindUnbind);
}

bool S3DModelVAO::SubmitImmediately(const CFeature* feature, GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(feature);

	const S3DModel* model = feature->model;
	assert(model);

	return SubmitImmediatelyImpl(feature, model->indxStart, model->indxCount, feature->paletteIndex, mode, bindUnbind);
}

bool S3DModelVAO::SubmitImmediately(const UnitDef* unitDef, int teamID, GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(unitDef);

	const S3DModel* model = unitDef->model;
	assert(model);

	return SubmitImmediatelyImpl(unitDef, model->indxStart, model->indxCount, static_cast<uint16_t>(teamID), mode, bindUnbind);
}
