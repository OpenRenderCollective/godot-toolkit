// Optional native-client extensions. The Remix 0.1003 interface table is unchanged.
#pragma once
#include <remix/remix_c.h>

// Optional pNext on opaque MaterialInfo. Nonzero roughness/metalness constants
// or a roughness/metalness/normal map automatically select EON + GGX. Completely
// zero-initialized legacy parameters retain Lambert shading. This override also
// allows an explicitly smooth (roughness=metalness=0) dielectric.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT ((remixapi_StructType)0x7fff1007)
typedef struct remixapi_MaterialInfoPbrEXT {
  remixapi_StructType sType;
  const void* pNext;
  remixapi_Bool enabled;
} remixapi_MaterialInfoPbrEXT;

// Optional MaterialInfo pNext. Keep the visible emissive color/map while
// suppressing generated area lights, e.g. when an explicit analytic light
// already represents the source. Absent/zero preserves emissive lighting.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT ((remixapi_StructType)0x7fff100a)
typedef struct remixapi_MaterialInfoEmissionEXT {
  remixapi_StructType sType;
  const void* pNext;
  remixapi_Bool suppressLightGeneration;
} remixapi_MaterialInfoEmissionEXT;

// Optional MeshInfo pNext. An opt-in replacement of the same mesh hash may
// reuse update-capable geometry when all surface vertex counts and index values
// match. DestroyMesh ends that lifetime. This is a storage hint, not object or
// temporal identity. Queued older meshes retain their immutable contents.
// Bone-skinned meshes continue to use the independent per-instance skin path.
#define REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT ((remixapi_StructType)0x7fff1008)
typedef struct remixapi_MeshInfoUpdateEXT {
  remixapi_StructType sType;
  const void* pNext;
  remixapi_Bool enabled;
} remixapi_MeshInfoUpdateEXT;

// Optional MeshInfo pNext, alongside enabled MeshInfoUpdateEXT. The caller
// guarantees that a vertex index denotes the same logical point across mesh
// replacements. With stable InstanceIdentityEXT, compatible replacements may
// retain motion correspondence to the last GPU-submitted mesh. Exact topology
// is still checked; DestroyMesh, index/count changes or disabling either hint
// ends correspondence. Ambiguous/recycled object identities must not opt in.
#define REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT ((remixapi_StructType)0x7fff1009)
typedef struct remixapi_MeshInfoMotionEXT {
  remixapi_StructType sType;
  const void* pNext;
  remixapi_Bool enabled;
} remixapi_MeshInfoMotionEXT;

// Raw BC1 uploads with one-bit alpha. The retained BC1_RGB formats remain opaque.
#define REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT ((remixapi_Format)133)
#define REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT ((remixapi_Format)134)

// Additional UIDrawCommand flags supported by this native raster runtime. Defaults
// retain the original clamp/nearest/alpha-blend behavior and the 0.1003 layout.
#define REMIXAPI_UI_NATIVE_REPEAT_U 0x2u
#define REMIXAPI_UI_NATIVE_REPEAT_V 0x4u
#define REMIXAPI_UI_NATIVE_LINEAR 0x8u
#define REMIXAPI_UI_NATIVE_CUSTOM_BLEND 0x10u
// With CUSTOM_BLEND, bits 8..11 and 12..15 hold source/destination Vulkan factors (0..14).
#define REMIXAPI_UI_NATIVE_SRC_BLEND_SHIFT 8u
#define REMIXAPI_UI_NATIVE_DST_BLEND_SHIFT 12u

// Optional pNext on InstanceInfo. Depth comparison uses Vulkan's 0..7 compare values.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT ((remixapi_StructType)0x7fff1001)
typedef struct remixapi_InstanceRasterStateEXT {
  remixapi_StructType sType;
  const void* pNext;
  remixapi_Bool depthTest;
  remixapi_Bool depthWrite;
  uint32_t depthCompare;
  remixapi_Bool disableColorWrites; /* depth-only prepass; zero preserves color writes */
} remixapi_InstanceRasterStateEXT;

// Stable identity is scoped to the caller's world/object lifetime. GPU instancing
// adds the array index; each mesh surface retains its geometry index. Zero IDs are
// reserved for legacy frame-local draws. rasterVisible does not affect tracing.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT ((remixapi_StructType)0x7fff1002)
typedef enum remixapi_InstanceClassEXT {
  REMIXAPI_INSTANCE_STATIC_EXT = 0,
  REMIXAPI_INSTANCE_DYNAMIC_EXT = 1,
  REMIXAPI_INSTANCE_RASTER_ONLY_EXT = 2,
} remixapi_InstanceClassEXT;
typedef struct remixapi_InstanceIdentityEXT {
  remixapi_StructType sType;
  const void* pNext;
  uint64_t instanceId;
  uint32_t classification;
  remixapi_Bool rasterVisible;
} remixapi_InstanceIdentityEXT;

typedef enum remixapi_RenderBackendEXT {
  REMIXAPI_RENDER_BACKEND_RASTER_EXT = 0,
  REMIXAPI_RENDER_BACKEND_PATHTRACE_EXT = 1,
} remixapi_RenderBackendEXT;
// Versioned independent exports: initialize structSize to sizeof(the struct).
// Missing capabilities are reported as a human-readable string; no RTX legacy
// feature bits, denoising or upscaling are implied by this capability.
typedef struct remixapi_BackendCapabilitiesEXT {
  uint32_t structSize;
  uint32_t supportedBackends; // bit 0 raster, bit 1 pathtrace
  uint32_t activeBackend;
  uint32_t requestedBackend;
  char unavailableReason[256];
} remixapi_BackendCapabilitiesEXT;
typedef enum remixapi_RebuildReasonEXT {
  REMIXAPI_REBUILD_NONE_EXT = 0, REMIXAPI_REBUILD_MEMBERSHIP_EXT = 1,
  REMIXAPI_REBUILD_GEOMETRY_EXT = 2, REMIXAPI_REBUILD_REFIT_LIMIT_EXT = 3,
  REMIXAPI_REBUILD_DISPLACEMENT_EXT = 4, REMIXAPI_REBUILD_FORCED_EXT = 5,
  REMIXAPI_REBUILD_STATIC_TRANSFORM_EXT = 6,
} remixapi_RebuildReasonEXT;
// Optional SHARC diagnostics; counters/times describe the last completed cache frame.
// Query counters sample 1/64 pixels (excluding confidence retracing), not all rays.
// Optional sun-shadow diagnostics. Existing denoising timing remains diffuse SH only.
typedef struct remixapi_ShadowStatisticsEXT {
  uint32_t structSize, supported, requested, active;
  uint32_t gpuTimingsAvailable, debugView, reserved[2];
  uint64_t renderedFrames, historyResets;
  float gpuSigmaMilliseconds, sunAngularDiameterDegrees;
  char unavailableReason[256];
} remixapi_ShadowStatisticsEXT;

// Analytic local emitters and their persistent importance hierarchy. Sampling cost
// is included in tracing time; gpuDenoiseMilliseconds measures only local NRD.
typedef struct remixapi_LocalLightStatisticsEXT {
  uint32_t structSize, enabled, lightCount, activeLights;
  uint32_t nodeCount, treeDepth, samples, denoiserActive;
  uint32_t gpuTimingsAvailable, reserved;
  uint64_t generation, builds, refits, memoryBytes;
  double cpuUpdateMilliseconds, gpuDenoiseMilliseconds;
  char unavailableReason[256];
} remixapi_LocalLightStatisticsEXT;

// Optional visibility-guided local-light diagnostics. Query counters are sampled
// at 1/64 of opaque-primary light samples; they are not full ray counts.
typedef struct remixapi_LocalLightGuidingStatisticsEXT {
  uint32_t structSize, requested, active, capacity;
  uint32_t sampledQueries, sampledGuided, sampledVisible, reserved;
  uint64_t renderedFrames, historyResets, memoryBytes;
  float influenceRange, cellSize;
  char unavailableReason[256];
} remixapi_LocalLightGuidingStatisticsEXT;

typedef struct remixapi_SharcStatisticsEXT {
  uint32_t structSize, supported, requested, active;
  uint32_t capacity, occupiedEntries, updatePaths, queryAttempts, queryHits;
  uint32_t gpuTimingsAvailable, debugView, reserved;
  uint64_t memoryBytes, renderedFrames, resets;
  double gpuUpdateMilliseconds, gpuResolveMilliseconds;
  char unavailableReason[256];
} remixapi_SharcStatisticsEXT;

typedef struct remixapi_TracingStatisticsEXT {
  uint32_t structSize, samplesPerPixel, indirectBounces, accumulationEnabled;
  uint64_t accumulationCount, sampleSequence, sceneGeneration;
  uint64_t blasBuilds, blasRefits, blasCompactions;
  uint64_t staticTlasBuilds, dynamicTlasBuilds, dynamicTlasRefits;
  uint64_t accelerationBytes, peakAccelerationBytes;
  uint32_t dynamicRebuildReason, dynamicRefitCount;
  float gpuBuildMilliseconds, gpuTraceMilliseconds;
  uint32_t gpuTimingsAvailable, redrawRequested;
} remixapi_TracingStatisticsEXT;

// CPU wall times for the last Present attempt. Geometry, allocation, upload and
// acceleration times are nested within command recording; do not sum them with
// recordMilliseconds. GPU execution is measured by TracingStatisticsEXT instead.
// attempts/presentations are cumulative since Startup; presented excludes skips.
typedef struct remixapi_FramePerformanceEXT {
  uint32_t structSize, presented;
  uint64_t attempts, presentations;
  double presentMilliseconds, sceneMilliseconds, recordMilliseconds;
  double geometryMilliseconds, allocationMilliseconds, uploadMilliseconds;
  double accelerationMilliseconds, fenceMilliseconds, acquireMilliseconds;
  double submitMilliseconds, queuePresentMilliseconds;
  uint64_t geometryConversions, geometryCacheHits, uploadBytes;
  uint64_t bufferAllocations, allocationBytes, bufferCacheHits;
  uint64_t memoryAllocations, memoryAllocationBytes;
} remixapi_FramePerformanceEXT;

// Optional CameraInfo pNext. Epochs persist in every packet, including replacement
// submissions. Increment historyResetEpoch for cuts and floating-origin rebases.
#define REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT ((remixapi_StructType)0x7fff1003)
typedef struct remixapi_CameraTemporalEXT {
  remixapi_StructType sType;
  const void* pNext;
  uint64_t worldEpoch, historyResetEpoch;
} remixapi_CameraTemporalEXT;
// Optional world CameraInfo pNext. Linear, nonnegative, finite RGB radiance at
// unit strength; enabled must be 0 or 1. Absent/disabled means black indirect
// environment. Sky/viewmodel cameras never replace the world environment.
#define REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT ((remixapi_StructType)0x7fff1004)
typedef struct remixapi_CameraEnvironmentEXT {
  remixapi_StructType sType;
  const void* pNext;
  remixapi_Bool enabled;
  remixapi_Float3D radiance;
} remixapi_CameraEnvironmentEXT;

typedef struct remixapi_DenoisingCapabilitiesEXT {
  uint32_t structSize, reblurShSupported, taaSupported, reserved;
  char unavailableReason[256];
} remixapi_DenoisingCapabilitiesEXT;
// Denoiser values: 0 = off, 1 = REBLUR_DIFFUSE_SH. Requested settings survive
// progressive accumulation; active settings describe the last submitted world.
// Timings describe the previous completed world frame. gpuTaaMilliseconds includes
// final resolve and guide-history copies, including copy-only work with TAA off.
// memoryBytes includes owned temporal textures and constant buffers, not tracing
// targets, acceleration structures, descriptors, pipelines or driver allocations.
typedef struct remixapi_DenoisingStatisticsEXT {
  uint32_t structSize, requestedDenoiser, activeDenoiser, requestedTaa;
  uint32_t activeTaa, validationEnabled, redrawRequested, gpuTimingsAvailable;
  uint64_t historyResets, renderedFrames, historyFrames, memoryBytes, peakMemoryBytes;
  float gpuConfidenceMilliseconds, gpuDenoiseMilliseconds, gpuTaaMilliseconds, reserved;
  char unavailableReason[256];
} remixapi_DenoisingStatisticsEXT;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum remixapi_InputEventType {
  REMIXAPI_INPUT_RESET = 1, // Release all held input (focus/capture change or queue overflow).
  REMIXAPI_INPUT_KEY = 2,
  REMIXAPI_INPUT_TEXT = 3, // One Unicode scalar, independent of physical key events.
  REMIXAPI_INPUT_MOUSE_BUTTON = 4,
  REMIXAPI_INPUT_MOUSE_MOTION = 5,
  REMIXAPI_INPUT_MOUSE_WHEEL = 6,
} remixapi_InputEventType;

typedef struct remixapi_InputEvent {
  uint32_t type;
  uint32_t code; // KEY: remixapi_Key; TEXT: Unicode; BUTTON: remixapi_MouseButtonBits.
  uint32_t down;
  uint32_t repeat;
  float x, y;   // Logical window position; wheel events contain wheel displacement.
  float dx, dy; // Relative logical-unit motion, for MOUSE_MOTION only.
} remixapi_InputEvent;

typedef struct remixapi_WindowMetrics {
  uint32_t logicalWidth, logicalHeight;
  uint32_t drawableWidth, drawableHeight;
} remixapi_WindowMetrics;

// All calls use the startup thread. Poll drains up to capacity events in order.
// capacity == 0 queries the queued count without consuming it. Overflow emits RESET.
// Events include ImGui-captured input, allowing application UI shortcuts. Clients
// must honor WindowState.guiWantsKeyboard/guiWantsMouse for gameplay dispatch.
// Existing state polling APIs remain independent and exclude captured input.
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_PollInputEvents)(
  remixapi_InputEvent* events, uint32_t capacity, uint32_t* out_count);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_SetTextInputEnabled)(remixapi_Bool enabled);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetWindowMetrics)(remixapi_WindowMetrics* out_metrics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PollInputEvents(
  remixapi_InputEvent* events, uint32_t capacity, uint32_t* out_count);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetTextInputEnabled(remixapi_Bool enabled);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetWindowMetrics(remixapi_WindowMetrics* out_metrics);
// Successful/suboptimal Vulkan queue presentations since Startup. Skipped frames do not count.
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetPresentationCount)(uint64_t* out_count);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetPresentationCount(uint64_t* out_count);

typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetBackendCapabilitiesEXT)(remixapi_BackendCapabilitiesEXT* capabilities);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetLocalLightStatisticsEXT)(remixapi_LocalLightStatisticsEXT* statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetLocalLightStatisticsEXT(remixapi_LocalLightStatisticsEXT* statistics);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetLocalLightGuidingStatisticsEXT)(remixapi_LocalLightGuidingStatisticsEXT* statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetLocalLightGuidingStatisticsEXT(remixapi_LocalLightGuidingStatisticsEXT* statistics);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetShadowStatisticsEXT)(remixapi_ShadowStatisticsEXT* statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetShadowStatisticsEXT(remixapi_ShadowStatisticsEXT* statistics);

typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetSharcStatisticsEXT)(remixapi_SharcStatisticsEXT* statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetSharcStatisticsEXT(remixapi_SharcStatisticsEXT* statistics);

typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetTracingStatisticsEXT)(remixapi_TracingStatisticsEXT* statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetBackendCapabilitiesEXT(remixapi_BackendCapabilitiesEXT* capabilities);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetTracingStatisticsEXT(remixapi_TracingStatisticsEXT* statistics);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetFramePerformanceEXT)(remixapi_FramePerformanceEXT* statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetFramePerformanceEXT(remixapi_FramePerformanceEXT* statistics);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetDenoisingCapabilitiesEXT)(remixapi_DenoisingCapabilitiesEXT* capabilities);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetDenoisingStatisticsEXT)(remixapi_DenoisingStatisticsEXT* statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetDenoisingCapabilitiesEXT(remixapi_DenoisingCapabilitiesEXT* capabilities);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetDenoisingStatisticsEXT(remixapi_DenoisingStatisticsEXT* statistics);

#ifdef __cplusplus
}
#endif

#define REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT ((remixapi_StructType)0x7fff1011)
typedef struct remixapi_MaterialNormalEncodingEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t encoding; // 0 legacy auto XY/XYZ, 1 hemisphere-octahedral, 2 tangent DX, 3 tangent GL
} remixapi_MaterialNormalEncodingEXT;

// A private value avoids the retained 0.1003 BC5 value (139).
#define REMIXAPI_FORMAT_BC4_UNORM_EXT ((remixapi_Format)147)
#define REMIXAPI_FORMAT_BC6H_UFLOAT_EXT ((remixapi_Format)143)
#define REMIXAPI_FORMAT_BC6H_SFLOAT_EXT ((remixapi_Format)144)

// Optional pNext on opaque or translucent MaterialInfo: a thin-film coating
// whose interference colours the reflection (iridescence), evaluated
// spectrally. On a translucent material (glass, water) the surface becomes a
// smooth dielectric under the film: its reflection takes the film's colour
// whatever its coverage, and the light through it the complementary colour;
// structural does not apply. A portal material is invalid.
// It takes the place of MaterialInfoOpaqueEXT's thinFilmThickness and
// alphaIsThinFilmThickness, which alone give a film of strength 1 and
// refractive index 1.5.
// strength: 0 (no film) to 1, how much of the reflection is the film's.
// filmRefractiveIndex: 1 to 3; higher indices give more saturated colours.
// thickness: the film's thickness in nanometres, 0 to 2000. With
// thicknessTexture, the map's red channel lerps thicknessMin to thicknessMax
// instead; with alphaIsThinFilmThickness, albedo alpha times 1500.
// flowAmplitude: nanometres of animated noise added to the thickness (0: a
// still film), swirls flowScale scene units across in object space, drifting
// flowSpeed per second.
// structural: 0 to 1, how much the film's colour tints diffuse reflection too
// (structural colour on rough surfaces).
#define REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT ((remixapi_StructType)0x7fff1020)
typedef struct remixapi_MaterialIridescenceEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path thicknessTexture;
  float strength, filmRefractiveIndex, thickness;
  float thicknessMin, thicknessMax;
  float flowAmplitude, flowScale, flowSpeed;
  float structural;
} remixapi_MaterialIridescenceEXT;

// Optional pNext on LightInfo (sphere, rect, disk and cylinder lights): an IES
// photometric profile, the contents of an IESNA LM-63 file (not
// null-terminated). The light's radiance in a direction is scaled by the
// profile's candela at the angle from the light's axis, the profile's
// brightest direction keeping it whole. The axis is the light shaping's
// direction, or without shaping the light's own (rect and disk: their
// direction; sphere: straight down, (0, 0, -1)); vertical angle 0 lies along
// it. Profiles are taken as rotationally symmetric: horizontal angles are
// averaged. Cone shaping still applies. Lights with the same contents share
// one profile. Text that is no LM-63 profile, or emits nothing, is invalid.
#define REMIXAPI_STRUCT_TYPE_LIGHT_INFO_IES_PROFILE_EXT ((remixapi_StructType)0x7fff101e)
typedef struct remixapi_LightInfoIesProfileEXT {
  remixapi_StructType sType;
  const void *pNext;
  const char *data;
  uint64_t size;
} remixapi_LightInfoIesProfileEXT;

// Optional pNext on LightInfo (sphere lights): a light cookie, a texture
// projected from the light along its axis (as for IES profiles: the shaping's
// direction, else straight down) like a slide projector. Within the
// projection the light's radiance is multiplied by the texture's colour times
// its alpha (so a mask may be in either), and outside it the light is dark: the texture's centre lies on the axis, its
// top toward `up` and its width spans fovDegrees (0 to 180, exclusive). The
// texture is named as material textures are (a CreateTexture hash "0x..." or
// a file path). `up` must not be parallel to the axis. Cone shaping and an
// IES profile still apply.
#define REMIXAPI_STRUCT_TYPE_LIGHT_INFO_COOKIE_EXT ((remixapi_StructType)0x7fff101f)
typedef struct remixapi_LightInfoCookieEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path texture;
  remixapi_Float3D up;
  float fovDegrees;
} remixapi_LightInfoCookieEXT;

// Optional pNext on opaque MaterialInfo: subsurface scattering, light that
// enters the surface and leaves it nearby (skin, wax, marble, leaves), as
// Burley's normalized diffusion. Direct light reaching the visible surface is
// gathered from a point the profile picks around it, on the same surface
// (through a thin object, its far side: back-lit glow); indirect light stays
// plain diffuse.
// weight: 0 (none) to 1, the share of diffuse reflection that scatters.
// radius: per colour channel, how far light travels under the surface
// (mean free path), times scale, in metres (scale > 0; the scene's metres come
// from rtx.native.pathtrace.sceneScale). Blender's Principled radius and scale.
// anisotropy: -0.9 to 0.9, the scattering's forward (positive) or backward
// bias; forward scattering carries light further.
// RTX Remix's MaterialInfoOpaqueSubsurfaceEXT with subsurfaceDiffusionProfile
// is read as weight 1, its radius and its radius scale in millimetres.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_SUBSURFACE_EXT ((remixapi_StructType)0x7fff1021)
typedef struct remixapi_MaterialSubsurfaceEXT {
  remixapi_StructType sType;
  const void *pNext;
  float weight;
  remixapi_Float3D radius;
  float scale, anisotropy;
} remixapi_MaterialSubsurfaceEXT;
