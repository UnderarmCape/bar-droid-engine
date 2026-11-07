/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <chrono>
#include <string_view>
#include <fmt/printf.h>

#include "IModelParser.h"
#include "3DOParser.h"
#include "S3OParser.h"
#include "AssParser.h"
#include "GLTFParser.h"
#include "3DModel.hpp"
#include "3DModelVAO.hpp"
#include "ModelsLock.h"
#include "Game/GlobalUnsynced.h"
#include "Rendering/Textures/S3OTextureHandler.h"
#include "Net/Protocol/NetProtocol.h" // NETLOG
#include "Sim/Misc/CollisionVolume.h"
#include "System/FileSystem/FileHandler.h"
#include "System/FileSystem/FileSystem.h"
#include "System/Log/ILog.h"
#include "System/StringUtil.h"
#include "System/Exceptions.h"
#include "System/SafeUtil.h"
#include "System/Threading/ThreadPool.h"
#include "System/ContainerUtil.h"
#include "System/LoadLock.h"
// assimp public headers modify #pragma pack across includes (clang -Wpragma-pack)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpragma-pack"
#endif
#include "lib/assimp/include/assimp/Importer.hpp"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "System/Misc/TracyDefs.h"

#if defined(BARDROID_SIM) && !defined(HEADLESS)
#include <algorithm>
#include <atomic>
#include "System/Config/ConfigHandler.h"
#include "lib/meshoptimizer/meshoptimizer.h"
#include "BarDroidModelOverride.h"

// BAR Droid (engine branch): models drawn with fewer triangles, for phones. Each piece's triangles are simplified once,
// when its model loads (meshoptimizer's edge collapses; seams and open edges keep their shape), down to the given share
// of its triangles as long as no point of the surface moves more than the given distance. Only the index list changes:
// the vertices, and with them every size, bound and collision volume taken from the model, stay exactly the same, so the
// simulation does not notice (online-safe). Every drawing pass (units, features, shadows, build previews, wreck pieces)
// uses the shorter list.
CONFIG(float, BarDroidModelSimplify).defaultValue(0.0f).minimumValue(0.0f).maximumValue(1.0f)
	.description("BAR Droid: each model piece simplified at load to at most this share of its triangles (0: off)");
CONFIG(float, BarDroidModelSimplifyError).defaultValue(1.0f).minimumValue(0.0f).maximumValue(100.0f)
	.description("BAR Droid: the farthest a simplified model's surface may move, in elmos");
// Kailil, 2026-11-06: "cut even near units, do an aggresive cut to every unit model. close and far". The simplifier
// above stops early on BAR's models (separate panels and texture seams: 1.36M -> 0.93M triangles, about the same at 6
// or 16 elmos). After it, every piece of 24 triangles or more is cut once more with meshoptimizer's sloppy simplifier,
// which merges vertices by place, whatever the panels and seams, down to this share of its triangles: far fewer
// triangles for every model and every pass, near and far, at the cost of rougher shapes and smeared texture seams.
// The same vertices: only which triangles are drawn changes (online-safe, as above). 0: off.
// (Kailil, after the sloppy cut at 0.35: "that looks awful lol. Too aggressive"): the gentler way past the separate
// panels: the simplifier may drop a whole small separate part (bolts, vents, greebles) when it is smaller than the
// error allows (meshoptimizer's Prune), the big shapes keep their outline.
CONFIG(bool, BarDroidModelSimplifyPrune).defaultValue(false)
	.description("BAR Droid: the simplification also drops small separate parts within its error (meshoptimizer's Prune)");
CONFIG(float, BarDroidModelSimplifySloppy).defaultValue(0.0f).minimumValue(0.0f).maximumValue(1.0f)
	.description("BAR Droid: after the simplification, each model piece cut to this share of its triangles by vertex clustering (0: off)");
// And a coarse copy kept beside the model's own list, for the model drawn small on the screen (CUnitDrawerGL4,
// BarDroidModelLodPixels): the sloppy cut to this share of what the model draws otherwise. 0: none.
CONFIG(float, BarDroidModelLodShare).defaultValue(0.0f).minimumValue(0.0f).maximumValue(1.0f)
	.description("BAR Droid: each model piece's coarse copy, for units small on the screen, at most this share of its triangles (0: none)");

namespace {
	std::atomic<uint64_t> barDroidTrisBefore{0};
	std::atomic<uint64_t> barDroidTrisAfter{0};
	std::atomic<uint32_t> barDroidModels{0};
	std::atomic<uint64_t> barDroidLodTris{0};

	bool BarDroidPieceOk(const std::vector<uint32_t>& indices, const std::vector<SVertexData>& vertices, size_t minTris)
	{
		// (small pieces keep theirs: little to gain, and their shapes are details; and a piece with an index past its
		// vertices, which the simplifier would read out of bounds)
		return indices.size() >= 3 * minTris && indices.size() % 3 == 0 && !vertices.empty()
			&& *std::max_element(indices.begin(), indices.end()) < vertices.size();
	}

	// meshoptimizer's sloppy simplifier (vertex clustering, no error limit) to the share; false: left as it was
	bool BarDroidSloppy(const std::vector<uint32_t>& indices, const std::vector<SVertexData>& vertices, float share,
		std::vector<uint32_t>& out)
	{
		const size_t target = std::max<size_t>(3 * 8, static_cast<size_t>(indices.size() / 3 * share) * 3);
		out.resize(indices.size());
		float error = 0.0f;
		const size_t n = meshopt_simplifySloppy(out.data(), indices.data(), indices.size(), &vertices[0].pos.x, vertices.size(),
			sizeof(SVertexData), target, 1.0f, &error);
		if (n < 3 || n >= indices.size())
			return false;
		out.resize(n);
		meshopt_optimizeVertexCache(out.data(), out.data(), n, vertices.size());
		return true;
	}

	void BarDroidCoarseCopy(S3DModel* model)
	{
		static const float share = configHandler->GetFloat("BarDroidModelLodShare");
		if (share <= 0.0f || share >= 1.0f)
			return;
		uint64_t tris = 0;
		std::vector<uint32_t> out;
		for (S3DModelPiece* p : model->pieceObjects) {
			const std::vector<uint32_t>& indices = p->GetIndicesVec();
			const std::vector<SVertexData>& vertices = p->GetVerticesVec();
			p->barDroidLodIndices.clear();
			if (BarDroidPieceOk(indices, vertices, 24) && BarDroidSloppy(indices, vertices, share, out)) {
				p->barDroidLodIndices = out;
				tris += out.size() / 3;
			} else {
				tris += indices.size() / 3;
			}
		}
		const uint64_t all = (barDroidLodTris += tris);
		if (barDroidModels.load() % 50 == 1)
			LOG("[BarDroid] coarse copies (share %.2f): %llu triangles", share, static_cast<unsigned long long>(all));
	}

	void BarDroidSimplifyModel(S3DModel* model)
	{
		static const float share = configHandler->GetFloat("BarDroidModelSimplify");
		static const float maxError = configHandler->GetFloat("BarDroidModelSimplifyError");
		static const float sloppy = configHandler->GetFloat("BarDroidModelSimplifySloppy");
		static const unsigned flags = meshopt_SimplifyErrorAbsolute | (configHandler->GetBool("BarDroidModelSimplifyPrune") ? meshopt_SimplifyPrune : 0u);
		const bool simplify = share > 0.0f && share < 1.0f, cut = sloppy > 0.0f && sloppy < 1.0f;

		uint64_t before = 0;
		uint64_t after = 0;
		std::vector<uint32_t> out;
		for (S3DModelPiece* p : model->pieceObjects) {
			std::vector<uint32_t>& indices = p->GetIndicesVec();
			const std::vector<SVertexData>& vertices = p->GetVerticesVec();
			before += indices.size() / 3;
			if (simplify && BarDroidPieceOk(indices, vertices, 48)) {
				const size_t target = std::max<size_t>(3 * 16, static_cast<size_t>(indices.size() / 3 * share) * 3);
				out.resize(indices.size());
				const size_t n = meshopt_simplify(out.data(), indices.data(), indices.size(), &vertices[0].pos.x, vertices.size(),
					sizeof(SVertexData), target, maxError, flags, nullptr);
				if (n >= 3 && n < indices.size()) {
					out.resize(n);
					meshopt_optimizeVertexCache(out.data(), out.data(), n, vertices.size());
					indices.swap(out);
				}
			}
			if (cut && BarDroidPieceOk(indices, vertices, 24) && BarDroidSloppy(indices, vertices, sloppy, out))
				indices.swap(out);
			after += indices.size() / 3;
		}
		if (simplify || cut) {
			const uint64_t allBefore = (barDroidTrisBefore += before);
			const uint64_t allAfter = (barDroidTrisAfter += after);
			const uint32_t models = ++barDroidModels;
			if (models % 50 == 1)
				LOG("[BarDroid] models simplified (share %.2f, %.1f elmos, then cut to %.2f): %u models, %llu -> %llu triangles",
					share, maxError, sloppy, models, static_cast<unsigned long long>(allBefore), static_cast<unsigned long long>(allAfter));
		}
		BarDroidCoarseCopy(model);   // (from what the model draws otherwise)
	}
}
#endif


CModelLoader modelLoader;

static C3DOParser g3DOParser;
static CS3OParser gS3OParser;
static CAssParser gAssParser;
static CGLTFParser gGLTFParser;

static bool CheckAssimpWhitelist(const char* aiExt) {
	constexpr std::array<const char*, 5> whitelist = {
		"3ds"  , // 3DSMax
		"dae"  , // Collada
		"lwo"  , // LightWave
		"obj"  ,
		"blend", // Blender
	};

	const auto pred = [&aiExt](const char* wlExt) { return (strcmp(aiExt, wlExt) == 0); };
	const auto iter = std::find_if(whitelist.begin(), whitelist.end(), pred);

	return (iter != whitelist.end());
}

static void RegisterModelFormats(CModelLoader::ParsersType& parsers) {
	RECOIL_DETAILED_TRACY_ZONE;
	// file-extension should be lowercase
	parsers.emplace_back("3do", &g3DOParser);
	parsers.emplace_back("s3o", &gS3OParser);
	parsers.emplace_back("gltf", &gGLTFParser);
	parsers.emplace_back("glb", &gGLTFParser);

	std::string extension;
	std::string extensions;
	std::string enabledExtensions;

	Assimp::Importer importer;
	// get a ";" separated list of format extensions ("*.3ds;*.lwo;*.mesh.xml;...")
	importer.GetExtensionList(extensions);
	// do not ignore the last extension
	extensions.append(";");

	size_t curIdx = 0;
	size_t nxtIdx = 0;

	// split the list, strip off the "*." extension prefixes
	while ((nxtIdx = extensions.find(';', curIdx)) != std::string::npos) {
		extension = extensions.substr(curIdx, nxtIdx - curIdx);
		extension = extension.substr(extension.find("*.") + 2);
		extension = StringToLower(extension);

		curIdx = nxtIdx + 1;

		if (!CheckAssimpWhitelist(extension.c_str()))
			continue;
		if (std::find_if(parsers.begin(), parsers.end(), [&extension](const auto& item) { return item.first == extension; }) != parsers.end())
			continue;

		parsers.emplace_back(extension, &gAssParser);
		enabledExtensions.append("*." + extension + ";");
	}

	LOG("[%s] supported (Assimp) model formats: %s", __func__, enabledExtensions.c_str());
}

static void LoadDummyModel(S3DModel& model)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// create a crash-dummy
	model.type = MODELTYPE_3DO;
	model.numPieces = 1;
	// give it one empty piece
	model.AddPiece(g3DOParser.AllocPiece());
	model.FlattenPieceTree(model.GetRootPiece()); //useless except for setting up traAlloc
	model.GetRootPiece()->SetCollisionVolume(CollisionVolume('b', 'z', -UpVector, ZeroVector));
	model.loadStatus = S3DModel::LoadStatus::LOADED;
}

static void LoadDummyModel(S3DModel& model, int id)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// create a crash-dummy
	model.id = id;
	LoadDummyModel(model);
}

static void CheckPieceNormals(const S3DModel* model, const S3DModelPiece* modelPiece)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (auto vertCount = modelPiece->GetVerticesVec().size(); vertCount >= 3) {
		// do not check pseudo-pieces
		unsigned int numNullNormals = 0;

		for (unsigned int n = 0; n < vertCount; n++) {
			numNullNormals += (modelPiece->GetNormal(n).SqLength() < 0.5f);
		}

		if (numNullNormals > 0) {
			constexpr const char* formatStr =
				"[%s] piece \"%s\" of model \"%s\" has %u (of %u) normals with invalid length (<0.5)";

			const char* modelName = model->name.c_str();
			const char* pieceName = modelPiece->name.c_str();

			LOG_L(L_DEBUG, formatStr, __func__, pieceName, modelName, static_cast<uint32_t>(numNullNormals), static_cast<uint32_t>(vertCount));
		}
	}

	for (const S3DModelPiece* childPiece: modelPiece->children) {
		CheckPieceNormals(model, childPiece);
	}
}



void CModelLoader::Init()
{
	RECOIL_DETAILED_TRACY_ZONE;
	RegisterModelFormats(parsers);
	InitParsers();

	models.clear();
	models.resize(MAX_MODEL_OBJECTS);

	// dummy first model, legitimate model IDs start at 1
	modelID = 0;
	LoadDummyModel(models[0], modelID);
}

void CModelLoader::InitParsers() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	g3DOParser.Init();
	gS3OParser.Init();
	gGLTFParser.Init();
	gAssParser.Init();
}

void CModelLoader::Kill()
{
	RECOIL_DETAILED_TRACY_ZONE;
	KillModels();
	KillParsers();

	errors.clear();
	cache.clear();
	parsers.clear();
}

void CModelLoader::KillModels()
{
	RECOIL_DETAILED_TRACY_ZONE;
	models.clear();
	modelID = 0;
}

void CModelLoader::KillParsers() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	g3DOParser.Kill();
	gS3OParser.Kill();
	gGLTFParser.Kill();
	gAssParser.Kill();
}



std::string CModelLoader::FindModelPath(std::string name) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	// check for empty string because we can be called
	// from Lua*Defs and certain features have no models
	if (name.empty())
		return name;

	const std::string vfsPath = "objects3d/";

	if (const std::string& fileExt = FileSystem::GetExtensionLowerCase(name); fileExt.empty()) {
		for (const auto& [formatExt, parser] : parsers) {
			if (CFileHandler::FileExists(name + "." + formatExt, SPRING_VFS_ZIP)) {
				name.append("." + formatExt);
				break;
			}
		}
	}

	if (CFileHandler::FileExists(name, SPRING_VFS_ZIP))
		return name;

	if (name.find(vfsPath) != std::string::npos)
		return name;

	return (FindModelPath(vfsPath + name));
}


void CModelLoader::PreloadModel(const std::string& modelName)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(Threading::IsMainThread() || Threading::IsGameLoadThread());

	//NB: do preload in any case
	if (ThreadPool::HasThreads()) {

		// if already in cache, thread just returns early
		// not spawning the thread at all would be better but still
		// requires locking around cache.find(...) since some other
		// preload worker might be down in FillModel modifying it
		// at the same time
		preloadFutures.emplace_back(
			ThreadPool::Enqueue([modelName]() {
				modelLoader.LoadModel(modelName, true);
			})
		);
	}
	else {
		modelLoader.LoadModel(modelName, true);
	}
}

void CModelLoader::LogErrors()
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(Threading::IsMainThread() || Threading::IsGameLoadThread());

	// block any preload threads from modifying <errors>
	auto lock = CModelsLock::GetScopedLock();
	if (errors.empty())
		return;

	for (const auto& [modelName, reason]: errors) {
		std::string errStr = fmt::sprintf("[FATAL ERROR]: could not load model \"%s\" (reason: %s)", modelName, reason);
		LOG_L(L_ERROR, "%s", errStr.c_str());
		CLIENT_NETLOG(gu->myPlayerNum, LOG_LEVEL_FATAL, errStr.c_str());
	}

	errors.clear();
	throw content_error("[CModelLoader] couldn't load one or many models");
}


S3DModel* CModelLoader::LoadModel(std::string name, bool preload)
{
	ZoneScoped;
	// cannot happen except through SpawnProjectile
	if (name.empty())
		return nullptr;

	StringToLowerInPlace(name);

	bool load = false;
	S3DModel* model = nullptr;
	{
		auto lock = CModelsLock::GetScopedLock();

		model = GetCachedModel(name);

		load = (model->loadStatus == S3DModel::LoadStatus::NOTLOADED);
		if (load)
			model->loadStatus = S3DModel::LoadStatus::LOADING;
	}

	assert(model);
	if (load) {
		FillModel(*model, name, FindModelPath(name));
		cv.notify_all();
	}

	auto lock = CModelsLock::GetUniqueLock();
	cv.wait(lock, [model]() {
		return model->loadStatus == S3DModel::LoadStatus::LOADED;
	});

	if (!preload)
		Upload(model);

	return model;
}

S3DModel* CModelLoader::GetCachedModel(std::string fullName)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// caller has mutex lock

	static const auto CompPred = [](auto&& lhs, auto&& rhs) { return lhs.first < rhs.first; };

	static constexpr std::string_view O3D = "objects3d/";
	if (auto oi = fullName.find(O3D); oi != std::string::npos) {
		assert(oi == 0u);
		fullName.erase(0, O3D.size());
	}

	auto keyFull = std::make_pair(fullName, uint32_t(-1));
	const auto ci = spring::BinarySearch(cache.begin(), cache.end(), keyFull, CompPred);
	if (ci != cache.end()) {
		assert(keyFull.first == ci->first);
		return &models[ci->second];
	}

	auto keyName = std::pair<std::string, uint32_t>("", uint32_t(-1));
	if (const auto ext = FileSystem::GetExtensionLowerCase(fullName); !ext.empty()) {
		keyName.first = fullName.substr(0, fullName.size() - ext.size() - 1);
		const auto ci = spring::BinarySearch(cache.begin(), cache.end(), keyName, CompPred);
		if (ci != cache.end()) {
			assert(keyName.first == ci->first);
			return &models[ci->second];
		}
	}

	if (modelID + 1 == MAX_MODEL_OBJECTS) {
		LOG_L(L_ERROR, "[CModelLoader::%s] Model pool of size %i is exhausted. Cannot load model %s", __func__, MAX_MODEL_OBJECTS, fullName.c_str());
		return &models[0]; //dummy model
	}

	models[modelID].id = ++modelID;
	keyFull.second = models[modelID].id;
	keyName.second = models[modelID].id;

	spring::VectorInsertSorted(cache, std::move(keyFull), CompPred);
	if (!keyName.first.empty())
		spring::VectorInsertSorted(cache, std::move(keyName), CompPred);

	return &models[modelID];
}

void CModelLoader::FillModel(
	S3DModel& model,
	const std::string& name,
	const std::string& path
) {
	ParseModel(model, name, path);

	assert(model.numPieces != 0);
	assert(model.GetRootPiece() != nullptr);

	model.SetPieceMatrices();

	PostProcessGeometry(&model);
}

void CModelLoader::DrainPreloadFutures(uint32_t numAllowed)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (preloadFutures.size() <= numAllowed)
		return;

	const auto erasePredicate = [](decltype(preloadFutures)::value_type item) {
		using namespace std::chrono_literals;
		return item.wait_for(0ms) == std::future_status::ready;
	};

	// collect completed futures
	std::erase_if(preloadFutures, erasePredicate);

	if (preloadFutures.size() <= numAllowed)
		return;

	while (preloadFutures.size() > numAllowed) {
		//drain queue until there are <= numAllowed items there
		std::erase_if(preloadFutures, erasePredicate);
		spring_sleep(spring_msecs(100));
	}
}

IModelParser* CModelLoader::GetFormatParser(const std::string& pathExt)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const std::string extension = StringToLower(pathExt);

	const auto it = std::find_if(parsers.begin(), parsers.end(), [&extension](const auto& item) { return item.first == extension; });
	if (it == parsers.end())
		return nullptr;

	return it->second;
}

void CModelLoader::ParseModel(S3DModel& model, const std::string& name, const std::string& path)
{
	RECOIL_DETAILED_TRACY_ZONE;
	try {
		auto* parser = GetFormatParser(FileSystem::GetExtensionLowerCase(path));
		if (parser == nullptr) {
			LoadDummyModel(model);
			throw content_error(fmt::sprintf("could not find a parser for model \"%s\" (unknown format?)", name));
		}

		parser->Load(model, path);
		if (model.numPieces > MAX_PIECES_PER_MODEL) {
			LoadDummyModel(model);
			throw content_error(fmt::sprintf("A model has too many pieces (>%u): %s", MAX_PIECES_PER_MODEL, path));
		}

	} catch (const content_error& ex) {
		auto lock = CModelsLock::GetScopedLock();
		LoadDummyModel(model);
		errors.emplace_back(name, ex.what());
	}
}



void CModelLoader::PostProcessGeometry(S3DModel* model)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (model->loadStatus == S3DModel::LoadStatus::LOADED)
		return;

	// does quads and strips conversion sometimes. Need to run first
	for (size_t i = 0; i < model->pieceObjects.size(); ++i) {
		model->pieceObjects[i]->PostProcessGeometry(static_cast<uint32_t>(i));
	}
#if defined(BARDROID_SIM) && !defined(HEADLESS)
	// (after the conversion to triangles, before the wreck pieces are cut from them; a mobile version is already cut)
	if (!BarDroidModelIsMobile(model))
		BarDroidSimplifyModel(model);
#endif
	for (auto* p : model->pieceObjects) {
		p->CreateShatterPieces();
	}
	{
		auto lock = CModelsLock::GetScopedLock(); // working with S3DModelVAO needs locking
		auto& inst = S3DModelVAO::GetInstance();
		inst.ProcessVertices(model);
		inst.ProcessIndicies(model);
		model->loadStatus = S3DModel::LoadStatus::LOADED;
	}
	cv.notify_all();
}

void CModelLoader::Upload(S3DModel* model) const {
	RECOIL_DETAILED_TRACY_ZONE;
	if (model->uploaded) //already uploaded
		return;

	assert(Threading::IsMainThread() || Threading::IsGameLoadThread());

	{
		auto lock = CLoadLock::GetUniqueLock(); //mostly needed to support calls from CFeatureHandler::LoadFeaturesFromMap()
		S3DModelVAO::GetInstance().UploadVBOs();

		// 3DO atlases are preloaded C3DOTextureHandler::Init()
		if (model->type != MODELTYPE_3DO) {
			// make sure textures (already preloaded) are fully loaded
			textureHandlerS3O.LoadTexture(model);
		}
	}

	for (auto* p : model->pieceObjects) {
		p->ReleaseShatterIndices();
	}

	// warn about models with bad normals (they break lighting)
	// skip for 3DO's since those have auto-calculated normals
	if (model->type != MODELTYPE_3DO)
		CheckPieceNormals(model, model->GetRootPiece());

	model->uploaded = true;
}

