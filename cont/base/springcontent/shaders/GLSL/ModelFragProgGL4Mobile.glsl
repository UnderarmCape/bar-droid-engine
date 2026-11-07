#version 430 core

// BAR Droid (BARDROID_SIM, BarDroidModelShader >= 1): ModelFragProgGL4.glsl for phones' GPUs. The same two texture
// reads, team colour, self-illumination, alpha test and fog; cheaper light: no reflection cubemap read (the
// reflection is a sky-to-ground tint by the reflected ray's height, from the sun's and the fog's colours), the sun's
// direction used as given (normalized on the CPU), and the broad half of the highlight as a product instead of a pow.
// Drawing only.
//   BARDROID_MODEL_SHADER 2: as 1, depth tested before shading (measurement: what the alpha test's late depth costs)
//   BARDROID_MODEL_SHADER 3: the diffuse texture only, unlit (measurement: the floor of this pass)

#if (BARDROID_MODEL_SHADER == 2)
	layout(early_fragment_tests) in;
#endif

layout(binding = 0) uniform sampler2D tex1;
layout(binding = 1) uniform sampler2D tex2;

#if (USE_SHADOWS == 1)
	layout(binding = 2) uniform sampler2DShadow shadowTex;
	layout(binding = 3) uniform sampler2D shadowColorTex;
#endif

layout(std140, binding = 1) uniform UniformParamsBuffer {
	vec3 rndVec3;
	uint renderCaps;

	vec4 timeInfo;
	vec4 viewGeometry;
	vec4 mapSize;
	vec4 mapHeight;

	vec4 fogColor;
	vec4 fogParams;

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

	vec4 teamColor[2048];
};

in Data {
	vec2 uvCoord;
	flat vec4 teamCol;
	vec3 worldNormal;
	vec3 worldCameraDir;
	float fogFactor;
	#if (USE_SHADOWS == 1)
	vec4 shadowVertexPos;
	#endif
};

uniform int shadingMode = 0; //NORMAL_SHADING

uniform vec4 alphaCtrl = vec4(0.0, 0.0, 0.0, 1.0); //always pass
uniform vec4 colorMult = vec4(1.0);
uniform vec4 nanoColor = vec4(0.0);

// the reflection: the engine's reflection cubemap's mean colour per face (+x -x +y -y +z -z; its smallest mipmap
// level, read back on the CPU), blended by the reflected ray's direction (an "ambient cube": no texture read)
uniform vec3 reflCube[6] = vec3[6](vec3(0.45), vec3(0.45), vec3(0.6), vec3(0.3), vec3(0.45), vec3(0.45));
uniform vec3 sunDirN = vec3(0.0, 0.447214, 0.894427); // the sun's direction, normalized on the CPU

bool AlphaDiscard(float a) {
	float alphaTestGT = float(a > alphaCtrl.x) * alphaCtrl.y;
	float alphaTestLT = float(a < alphaCtrl.x) * alphaCtrl.z;

	return ((alphaTestGT + alphaTestLT + alphaCtrl.w) == 0.0);
}

#if (DEFERRED_MODE == 1)
	out vec4 fragColor[GBUFFER_ZVALTEX_IDX];
#else
	out vec4 fragColor;
#endif

#define SNORM2NORM(value) (value * 0.5 + 0.5)

void main()
{
	vec4 texColor1 = texture(tex1, uvCoord);
#if (BARDROID_MODEL_SHADER == 3)
	if (AlphaDiscard(teamCol.a))
		discard;
	vec4 texColor2 = vec4(0.0, 0.0, 0.0, 1.0);
	vec3 finalColor = mix(texColor1.rgb, teamCol.rgb, texColor1.a);
	float alpha = teamCol.a;
#else
	vec4 texColor2 = texture(tex2, uvCoord);

	float alpha = teamCol.a * float(texColor2.a >= 0.5);
	if (AlphaDiscard(alpha))
		discard;

	texColor1.rgb = mix(texColor1.rgb, teamCol.rgb, texColor1.a);
	vec3 finalColor = texColor1.rgb;

	if (shadingMode == 0) { // NORMAL_SHADING
		vec3 L = sunDirN;
		vec3 V = normalize(worldCameraDir);
		vec3 N = normalize(worldNormal);
		vec3 H = normalize(L + V);

		float NdotV = dot(N, V);
		float NdotL = clamp(dot(N, L), 0.0, 1.0);
		float HdotN = clamp(dot(N, H), 0.0, 1.0);

		// the reflected ray R = 2 (N.V) N - V (unit length), each axis' face weighted by its square
		vec3 R = 2.0 * NdotV * N - V;
		vec3 R2 = R * R;
		vec3 reflColor =
			R2.x * ((R.x >= 0.0) ? reflCube[0] : reflCube[1]) +
			R2.y * ((R.y >= 0.0) ? reflCube[2] : reflCube[3]) +
			R2.z * ((R.z >= 0.0) ? reflCube[4] : reflCube[5]);

		#if (USE_SHADOWS == 1)
			vec3 shadowCoord = shadowVertexPos.xyz / shadowVertexPos.w;
			float sh = min(texture(shadowTex, shadowCoord).r, smoothstep(0.0, 0.35, NdotL));
			vec3 shadowMult = mix(1.0, sh, shadowDensity.y) * texture(shadowColorTex, shadowCoord.xy).rgb;
		#else
			const vec3 shadowMult = vec3(1.0);
		#endif

		vec3 light = sunAmbientModel.rgb + (NdotL * sunDiffuseModel.rgb);

		float h2 = HdotN * HdotN;
		vec3 specular = sunSpecularModel.rgb * min(pow(HdotN, 2.5 * sunSpecularModel.a) + 0.3 * (h2 * h2 * h2), 1.0);
		specular *= (texColor2.g * 4.0);
		specular *= shadowMult;

		light = mix(sunAmbientModel.rgb, light, shadowMult);
		light = mix(light, reflColor, texColor2.g); // reflection
		light += texColor2.rrr; // self-illum

		finalColor = finalColor * light + specular;
	}
#endif

	#if (DEFERRED_MODE == 1)
		vec4 diffColor = colorMult * vec4(mix(texColor1.rgb, nanoColor.rgb, nanoColor.a), alpha);
		fragColor[GBUFFER_NORMTEX_IDX] = vec4(SNORM2NORM(normalize(worldNormal)), 1.0);
		fragColor[GBUFFER_DIFFTEX_IDX] = diffColor;
		fragColor[GBUFFER_SPECTEX_IDX] = vec4(texColor2.rgb, alpha);
		fragColor[GBUFFER_EMITTEX_IDX] = vec4(0.0, 0.0, 0.0, 0.0);
		fragColor[GBUFFER_MISCTEX_IDX] = vec4(0.0, 0.0, 0.0, 0.0);
	#else
		fragColor.rgb = mix(finalColor.rgb, fogColor.rgb, (1.0 - fogFactor) * float(shadingMode == 0));
		fragColor.rgb = mix(fragColor.rgb, nanoColor.rgb, nanoColor.a * float(shadingMode == 0));
		fragColor.a   = alpha;
		fragColor *= colorMult;
	#endif
}
