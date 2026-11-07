#version 430 core

// BAR Droid (BARDROID_SIM, BarDroidModelShader >= 1): ModelVertProgGL4.glsl for phones' GPUs. The same transforms
// and team colour, cheaper: the frame-to-frame rotation blended as a normalized lerp (SLerp's own path for small
// angles, without its acos/sin branch), and only what ModelFragProgGL4Mobile.glsl reads passed on (no world position,
// no tangents; the shadow position only with shadows; the team colour flat: one per unit).
// Drawing only: nothing here reaches the simulation.

layout (location = 0) in vec3 pos;
layout (location = 1) in vec3 normal;
layout (location = 4) in vec4 uv;
layout (location = 5) in uvec3 bonesInfo; //boneIDsLow, boneWeights, boneIDsHigh

layout (location = 6) in uvec4 instData;
// u32 matOffset
// u32 uniOffset
// u32 {paletteIndex[0:10], reserved[11:15], numPieces[16:31]}
// u32 bposeMatOffset

layout(std140, binding = 0) uniform UniformMatrixBuffer {
	mat4 screenView;
	mat4 screenProj;
	mat4 screenViewProj;

	mat4 cameraView;
	mat4 cameraProj;
	mat4 cameraViewProj;
	mat4 cameraBillboardView;

	mat4 cameraViewInv;
	mat4 cameraProjInv;
	mat4 cameraViewProjInv;

	mat4 shadowView;
	mat4 shadowProj;
	mat4 shadowViewProj;

	mat4 reflectionView;
	mat4 reflectionProj;
	mat4 reflectionViewProj;

	mat4 orthoProj01;

	mat4 mmDrawView;
	mat4 mmDrawProj;
	mat4 mmDrawViewProj;

	mat4 mmDrawIMMView;
	mat4 mmDrawIMMProj;
	mat4 mmDrawIMMViewProj;

	mat4 mmDrawDimView;
	mat4 mmDrawDimProj;
	mat4 mmDrawDimViewProj;
};

layout(std140, binding = 1) uniform UniformParamsBuffer {
	vec3 rndVec3;
	uint renderCaps;

	vec4 timeInfo;     //gameFrame, drawSeconds, interpolated(unsynced)GameSeconds(synced), frameTimeOffset
	vec4 viewGeometry;
	vec4 mapSize;
	vec4 mapHeight;

	vec4 fogColor;
	vec4 fogParams; //fog {start, end, 0.0, scale}

	vec4 sunDir;

	vec4 sunAmbientModel;
	vec4 sunAmbientMap;
	vec4 sunDiffuseModel;
	vec4 sunDiffuseMap;
	vec4 sunSpecularModel;
	vec4 sunSpecularMap;

	vec4 shadowDensity;

	vec4 windInfo;
	vec2 mouseScreenPos;
	uint mouseStatus;
	uint mouseUnused;
	vec4 mouseWorldPos;

	vec4 teamColor[2048]; // [0..254] team colors, [255] reserved, [256..2047] custom palette
};

struct Transform {
	vec4 quat;
	vec4 trSc;
};

layout(std140, binding = 0) readonly buffer TransformBuffer {
	Transform transforms[];
};

uniform int cameraMode = 0;
uniform int matrixMode = 0;

uniform mat4 staticModelMatrix = mat4(1.0);

uniform vec4 clipPlane0 = vec4(0.0, 0.0, 0.0, 1.0); //upper construction clip plane
uniform vec4 clipPlane1 = vec4(0.0, 0.0, 0.0, 1.0); //lower construction clip plane
uniform vec4 clipPlane2 = vec4(0.0, 0.0, 0.0, 1.0); //water clip plane

uniform float teamColorAlpha = 1.0;

out Data {
	vec2 uvCoord;
	flat vec4 teamCol;
	vec3 worldNormal;
	vec3 worldCameraDir; // from the vertex to the camera, not normalized
	float fogFactor;
	#if (USE_SHADOWS == 1)
	vec4 shadowVertexPos;
	#endif
};
out float gl_ClipDistance[3];

uint GetUnpackedValue(uint packedValue, uint byteNum) {
	return (packedValue >> (8u * byteNum)) & 0xFFu;
}

vec4 MultiplyQuat(vec4 a, vec4 b)
{
	return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
}

vec3 RotateByQuaternion(vec4 q, vec3 v) {
	return 2.0 * dot(q.xyz, v) * q.xyz + (q.w * q.w - dot(q.xyz, q.xyz)) * v + 2.0 * q.w * cross(q.xyz, v);
}

vec4 InvertNormalizedQuaternion(vec4 q) {
	return vec4(-q.x, -q.y, -q.z, q.w);
}

vec4 ApplyTransform(Transform tra, vec4 v) {
	return vec4(RotateByQuaternion(tra.quat, v.xyz * tra.trSc.w) + tra.trSc.xyz * v.w, v.w);
}

Transform ApplyTransform(Transform parentTra, Transform childTra) {
	return Transform(
		MultiplyQuat(parentTra.quat, childTra.quat),
		vec4(
			parentTra.trSc.xyz + RotateByQuaternion(parentTra.quat, parentTra.trSc.w * childTra.trSc.xyz),
			parentTra.trSc.w * childTra.trSc.w
		)
	);
}

Transform InvertTransformAffine(Transform tra) {
	vec4 invR = InvertNormalizedQuaternion(tra.quat);
	float invS = 1.0 / tra.trSc.w;
	return Transform(invR, vec4(RotateByQuaternion(invR, -tra.trSc.xyz * invS), invS));
}

// between two simulated frames a piece turns by little: a normalized lerp (the shorter way round)
// (BARDROID_MODEL_SHADER 4, a measurement: no blend, the last simulated frame's transform)
Transform Lerp(Transform t0, Transform t1, float a) {
#if (BARDROID_MODEL_SHADER == 4)
	return t1;
#endif
	a = clamp(a, 0.0, 1.0);
	vec4 q1 = (dot(t0.quat, t1.quat) < 0.0) ? -t1.quat : t1.quat;
	return Transform(normalize(mix(t0.quat, q1, a)), mix(t0.trSc, t1.trSc, a));
}

void GetModelSpaceVertex(out vec4 msPosition, out vec3 msNormal)
{
	bool staticModel = (matrixMode == MATMODE_STATIC || matrixMode == MATMODE_ARRAY);

	vec4 piecePos = vec4(pos, 1.0);
	vec4 normal4 = vec4(normal, 0.0);

	uint bID0 = GetUnpackedValue(bonesInfo.x, 0u) + (GetUnpackedValue(bonesInfo.z, 0u) << 8u); //first boneID

	Transform tx;
	if (staticModel) {
		tx = transforms[instData.w + bID0];
	} else {
		tx = Lerp(
			transforms[instData.x + 2u * (1u + bID0) + 0u],
			transforms[instData.x + 2u * (1u + bID0) + 1u],
			timeInfo.w
		);
	}

	float w0 = float(GetUnpackedValue(bonesInfo.y, 0u)) / 255.0 * float(tx.trSc.w > 0.0);

	msPosition = ApplyTransform(tx, piecePos);
	msNormal   = ApplyTransform(tx, normal4).xyz;

	if (staticModel || w0 == 1.0)
		return;

	// skinned vertices (rare in BAR: S3O pieces have one bone each), as ModelVertProgGL4.glsl
	vec4 weights = vec4(
		w0,
		float(GetUnpackedValue(bonesInfo.y, 1u)) / 255.0,
		float(GetUnpackedValue(bonesInfo.y, 2u)) / 255.0,
		float(GetUnpackedValue(bonesInfo.y, 3u)) / 255.0
	) * float(tx.trSc.w > 0.0);

	msPosition *= weights[0];
	msNormal   *= weights[0];

	Transform bposeTra = transforms[instData.w + bID0];

	for (uint bi = 1; bi < 3; ++bi) {
		uint bID = GetUnpackedValue(bonesInfo.x, bi) + (GetUnpackedValue(bonesInfo.z, bi) << 8u);

		if (bID == 0xFFFFu || weights[bi] == 0.0)
			continue;

		Transform bposeInvTra = InvertTransformAffine(transforms[instData.w + bID]);
		Transform boneTx = Lerp(
			transforms[instData.x + 2u * (1u + bID) + 0u],
			transforms[instData.x + 2u * (1u + bID) + 1u],
			timeInfo.w
		);
		Transform full = ApplyTransform(boneTx, ApplyTransform(bposeInvTra, bposeTra));

		msPosition += ApplyTransform(full, piecePos) * weights[bi];
		msNormal   += ApplyTransform(full, normal4).xyz * weights[bi];
	}
}

void main()
{
	vec4 modelPos;
	vec3 modelNormal;
	GetModelSpaceVertex(modelPos, modelNormal);

	vec4 worldPos;
	if (matrixMode == MATMODE_ARRAY) {
		Transform wtx = transforms[instData.x + 0u];
		worldPos = ApplyTransform(wtx, modelPos);
		wtx.trSc = vec4(0, 0, 0, 1);
		worldNormal = ApplyTransform(wtx, vec4(modelNormal, 0.0)).xyz;
	} else if (matrixMode == MATMODE_STATIC) {
		worldPos = staticModelMatrix * modelPos;
		worldNormal = mat3(staticModelMatrix) * modelNormal;
	} else {
		Transform tx = Lerp(transforms[instData.x + 0u], transforms[instData.x + 1u], timeInfo.w);
		worldPos = ApplyTransform(tx, modelPos);
		tx.trSc = vec4(0, 0, 0, 1);
		worldNormal = ApplyTransform(tx, vec4(modelNormal, 0.0)).xyz;
	}

	gl_ClipDistance[0] = dot(modelPos, clipPlane0); //upper construction clip plane
	gl_ClipDistance[1] = dot(modelPos, clipPlane1); //lower construction clip plane
	gl_ClipDistance[2] = dot(worldPos, clipPlane2); //water clip plane

	uint paletteIndex = instData.z & 0x07FFu;
	teamCol = teamColor[paletteIndex];
	teamCol.a = teamColorAlpha;

	uvCoord = uv.xy;

	#if (USE_SHADOWS == 1)
	shadowVertexPos = shadowView * worldPos;
	shadowVertexPos.xy += vec2(0.5);
	#endif

	worldCameraDir = cameraViewInv[3].xyz - worldPos.xyz;

	#if (DEFERRED_MODE == 0)
	fogFactor = clamp((fogParams.y - length(worldCameraDir)) * fogParams.w, 0.0, 1.0);
	#else
	fogFactor = 1.0;
	#endif

	gl_Position = ((cameraMode == 1) ? reflectionViewProj : cameraViewProj) * worldPos;
}
