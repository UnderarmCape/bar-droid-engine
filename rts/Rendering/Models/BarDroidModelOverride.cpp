/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "BarDroidModelOverride.h"

#if defined(BARDROID_SIM) && !defined(HEADLESS)

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "3DModel.hpp"
#include "3DModelPiece.hpp"
#include "ModelUtils.h"
#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/FileHandler.h"
#include "System/Log/ILog.h"

CONFIG(bool, BarDroidModelOverrideAB).defaultValue(false)
	.description("BAR Droid, for measurements: mobile models keep BAR's own geometry too (more GPU memory), drawn while BarDroidModelDrawBar = 1");
CONFIG(std::string, BarDroidModelOverrideDir).defaultValue("")
	.description("BAR Droid: a folder (in the write dir) of mobile versions of the game's S3O models at their own paths, drawn in place of BAR's (drawing only; BAR's model gives every size, bound and collision volume)");

namespace {
	struct MobilePiece {
		std::string name;
		float off[3] = {0.0f, 0.0f, 0.0f};
		std::vector<float> verts;        // 8 floats a vertex: position, normal, texture coordinates
		std::vector<uint32_t> tris;
		std::vector<uint32_t> lod;
		std::vector<int> children;
		bool keepBar = false;            // (strips or quads: a piece the generator left as BAR made it)
	};

	template<typename T> bool Get(const std::vector<uint8_t>& b, size_t off, T& v)
	{
		if (off + sizeof(T) > b.size())
			return false;
		memcpy(&v, &b[off], sizeof(T));
		return true;
	}

	bool GetIndices(const std::vector<uint8_t>& b, size_t off, size_t count, size_t vertCount, std::vector<uint32_t>& out)
	{
		if (count % 3 != 0 || off + count * 4 > b.size() || count > (1u << 24))
			return false;
		out.resize(count);
		memcpy(out.data(), &b[off], count * 4);
		for (uint32_t i : out)
			if (i >= vertCount)
				return false;
		return true;
	}

	bool ParsePiece(const std::vector<uint8_t>& b, int off, int depth, std::vector<MobilePiece>& pieces, std::string& why)
	{
		int f[10];
		float o[3];
		if (depth > 64) { why = "piece tree too deep"; return false; }
		for (int i = 0; i < 10; ++i)
			if (!Get(b, size_t(off) + 4 * i, f[i])) { why = "piece past the end"; return false; }
		for (int i = 0; i < 3; ++i)
			Get(b, size_t(off) + 40 + 4 * i, o[i]);
		const size_t me = pieces.size();
		pieces.emplace_back();
		{
			MobilePiece& p = pieces.back();
			if (f[0] <= 0 || size_t(f[0]) >= b.size()) { why = "piece name"; return false; }
			const size_t nameEnd = std::find(b.begin() + f[0], b.end(), 0) - b.begin();
			if (nameEnd >= b.size()) { why = "piece name"; return false; }
			p.name.assign(reinterpret_cast<const char*>(&b[f[0]]), nameEnd - f[0]);
			memcpy(p.off, o, sizeof(o));
			if (f[3] < 0 || f[3] > (1 << 20) || f[7] < 0) { why = "piece data"; return false; }
			p.keepBar = (f[6] != 0);
			if (f[3] > 0) {
				if (size_t(f[4]) + size_t(f[3]) * 32 > b.size()) { why = "vertices past the end"; return false; }
				p.verts.resize(size_t(f[3]) * 8);
				memcpy(p.verts.data(), &b[f[4]], size_t(f[3]) * 32);
			}
			if (!p.keepBar && f[7] > 0 && !GetIndices(b, size_t(f[8]), size_t(f[7]), size_t(f[3]), p.tris)) { why = "triangles"; return false; }
			if (f[9] > 0) {   // the coarse level: "BDL1", count, triangles
				int n = 0;
				if (size_t(f[9]) + 8 > b.size() || memcmp(&b[f[9]], "BDL1", 4) != 0 || !Get(b, size_t(f[9]) + 4, n) || n < 0 ||
					!GetIndices(b, size_t(f[9]) + 8, size_t(n), size_t(f[3]), p.lod)) { why = "coarse level"; return false; }
			}
		}
		if (f[1] < 0 || f[1] > 4096) { why = "children"; return false; }
		for (int c = 0; c < f[1]; ++c) {
			int co = 0;
			if (!Get(b, size_t(f[2]) + 4 * c, co)) { why = "child table"; return false; }
			pieces[me].children.push_back(int(pieces.size()));
			if (!ParsePiece(b, co, depth + 1, pieces, why))
				return false;
		}
		return true;
	}

	bool SameBits(float a, float b) { return memcmp(&a, &b, sizeof(float)) == 0; }

	// BAR's piece and the mobile one, and their children in order
	bool Match(S3DModelPiece* o, const std::vector<MobilePiece>& mp, int mi, std::vector<std::pair<S3DModelPiece*, const MobilePiece*>>& pairs, std::string& why)
	{
		const MobilePiece& m = mp[mi];
		if (o->name != m.name) { why = "piece name " + o->name; return false; }
		if (!SameBits(o->offset.x, m.off[0]) || !SameBits(o->offset.y, m.off[1]) || !SameBits(o->offset.z, m.off[2])) { why = "offset of " + o->name; return false; }
		if (o->children.size() != m.children.size()) { why = "children of " + o->name; return false; }
		const auto& ov = o->GetVerticesVec();
		const size_t mv = m.verts.size() / 8;
		if (ov.size() < 2) {
			if (mv != ov.size()) { why = "vertices of " + o->name; return false; }
		} else {
			if (mv < 2) { why = "vertices of " + o->name; return false; }
			for (int k = 0; k < 2; ++k)
				if (!SameBits(ov[k].pos.x, m.verts[8 * k]) || !SameBits(ov[k].pos.y, m.verts[8 * k + 1]) || !SameBits(ov[k].pos.z, m.verts[8 * k + 2])) {
					why = "vertex " + std::to_string(k) + " of " + o->name;
					return false;
				}
		}
		if (!m.keepBar && o->HasGeometryData() != (m.tris.size() >= 3)) { why = "geometry of " + o->name; return false; }
		pairs.emplace_back(o, &m);
		for (size_t c = 0; c < o->children.size(); ++c)
			if (!Match(o->children[c], mp, m.children[c], pairs, why))
				return false;
		return true;
	}

	std::mutex mobileMutex;
	std::unordered_set<const S3DModel*> mobileModels;
	std::atomic<uint32_t> nMobile{0}, nMissing{0}, nRejected{0};
	std::atomic<uint64_t> trisBar{0}, trisMobile{0}, trisCoarse{0}, vertsBar{0}, vertsMobile{0};
}

bool BarDroidModelOverride(S3DModel* model, uint64_t fileHash)
{
	static const std::string dir = [] {
		std::string d = configHandler->GetString("BarDroidModelOverrideDir");
		if (!d.empty() && d.back() != '/')
			d += '/';
		return d;
	}();
	if (dir.empty() || model == nullptr || model->type != MODELTYPE_S3O || model->pieceObjects.empty())
		return false;

	CFileHandler file(dir + model->name, SPRING_VFS_RAW);
	if (!file.FileExists()) {
		++nMissing;
		return false;
	}
	std::vector<uint8_t> b;
	if (file.IsBuffered()) {
		b = std::move(file.GetBuffer());
	} else {
		b.resize(file.FileSize());
		file.Read(b.data(), int(b.size()));
	}

	std::string why;
	std::vector<MobilePiece> pieces;
	std::vector<std::pair<S3DModelPiece*, const MobilePiece*>> pairs;
	{
		int root = 0, mark = 0;
		uint64_t hash = 0;
		if (b.size() < 52 || memcmp(b.data(), "Spring unit", 11) != 0 || !Get(b, 36, root) || !Get(b, 40, mark))
			why = "not an S3O file";
		else if (mark <= 0 || size_t(mark) + 24 > b.size() || memcmp(&b[mark], "BDMOBIL1", 8) != 0)
			why = "not a mobile model";
		else if (!Get(b, size_t(mark) + 8, hash) || hash != fileHash)
			why = "made from another version of the model";
		else if (ParsePiece(b, root, 0, pieces, why))
			Match(model->pieceObjects[0], pieces, 0, pairs, why);
	}
	if (!why.empty() || pairs.size() != model->pieceObjects.size()) {
		if (why.empty())
			why = "piece count";
		if (++nRejected <= 20)
			LOG_L(L_WARNING, "[BarDroid] mobile model %s not used (%s): BAR's own drawn", model->name.c_str(), why.c_str());
		return false;
	}

	static const bool keepBar = configHandler->GetBool("BarDroidModelOverrideAB");
	uint64_t tb = 0, tm = 0, tc = 0, vb = 0, vm = 0;
	for (auto& [o, m] : pairs) {
		tb += o->GetIndicesVec().size() / 3;
		vb += o->GetVerticesVec().size();
		if (m->keepBar || m->tris.size() < 3) {   // (no triangles, or left as BAR made it: BAR's piece as it is)
			tm += o->GetIndicesVec().size() / 3;
			tc += o->GetIndicesVec().size() / 3;
			vm += o->GetVerticesVec().size();
			continue;
		}
		const size_t n = m->verts.size() / 8;
		std::vector<SVertexData> verts(n);
		for (size_t i = 0; i < n; ++i) {
			const float* v = &m->verts[8 * i];
			SVertexData& sv = verts[i];
			sv.pos = float3(v[0], v[1], v[2]);
			sv.normal = float3(v[3], v[4], v[5]);
			// (as S3OParser reads BAR's)
			if (sv.normal.CheckNaNs())
				sv.normal.SafeANormalize();
			else
				sv.normal = ZeroVector;
			sv.texCoords[0] = float2(v[6], v[7]);
			sv.texCoords[1] = float2(v[6], v[7]);
		}
		std::vector<uint32_t> indices = m->tris;
		ModelUtils::CalculateTangents(verts, indices);
		o->GetVerticesVec().swap(verts);
		o->GetIndicesVec().swap(indices);
		if (keepBar) {   // (BAR's own kept for the A/B; its bone, the piece's number, as PostProcessGeometry gives the mobile ones)
			const size_t pieceIndex = std::find(model->pieceObjects.begin(), model->pieceObjects.end(), o) - model->pieceObjects.begin();
			for (auto& v : verts) {
				if (v.boneIDsLow == SVertexData::DEFAULT_BONEIDS_LOW && v.boneIDsHigh == SVertexData::DEFAULT_BONEIDS_HIGH) {
					v.boneIDsLow[0] = static_cast<uint8_t>(pieceIndex & 0xFF);
					v.boneIDsHigh[0] = static_cast<uint8_t>((pieceIndex >> 8) & 0xFF);
				}
			}
			o->barDroidBarVerts.swap(verts);
			o->barDroidBarIndices.swap(indices);
		}
		o->barDroidLodIndices = m->lod;
		tm += m->tris.size() / 3;
		tc += (m->lod.empty() ? m->tris.size() : m->lod.size()) / 3;
		vm += n;
	}
	{
		std::lock_guard<std::mutex> lock(mobileMutex);
		mobileModels.insert(model);
	}
	const uint64_t allBar = (trisBar += tb), allMobile = (trisMobile += tm), allCoarse = (trisCoarse += tc);
	const uint64_t allVb = (vertsBar += vb), allVm = (vertsMobile += vm);
	const uint32_t count = ++nMobile;
	if (count % 100 == 1)
		LOG("[BarDroid] mobile models: %u drawn from %s (%u not there, %u not matching): triangles %llu -> %llu (coarse %llu), vertices %llu -> %llu",
			count, dir.c_str(), nMissing.load(), nRejected.load(), static_cast<unsigned long long>(allBar), static_cast<unsigned long long>(allMobile),
			static_cast<unsigned long long>(allCoarse), static_cast<unsigned long long>(allVb), static_cast<unsigned long long>(allVm));
	return true;
}

bool BarDroidModelIsMobile(const S3DModel* model)
{
	std::lock_guard<std::mutex> lock(mobileMutex);
	return mobileModels.count(model) != 0;
}

#else

bool BarDroidModelOverride(S3DModel*, uint64_t) { return false; }
bool BarDroidModelIsMobile(const S3DModel*) { return false; }

#endif
