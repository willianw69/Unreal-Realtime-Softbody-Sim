// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "SoftBodyResources.h" // FSoftBodyTet (stored by value in a TArray member), constraint structs
#include "SoftBodyComponent.generated.h"

struct FSoftBodyRenderResources;
struct FGPUConstraint;
struct FSoftBodyColorRange;
class FSoftBodyMeshSceneProxy;
class UMaterialInterface;
class UStaticMesh;

/** Analytic collider shape authored in the Details panel (SB-M4). */
UENUM(BlueprintType)
enum class ESoftBodyColliderType : uint8
{
	Sphere,
	Capsule
};

/** A single collider authored in the Details panel (transform relative to the component). */
USTRUCT(BlueprintType)
struct FSoftBodyCollider
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider")
	ESoftBodyColliderType Type = ESoftBodyColliderType::Sphere;

	/** Center offset from the component origin (local space). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider")
	FVector Center = FVector(0.0f, 0.0f, -60.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider", meta = (ClampMin = "0.1"))
	float Radius = 40.0f;

	/** Capsule only: half the distance between the two end caps, along the local axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider", meta = (ClampMin = "0.0"))
	float HalfHeight = 50.0f;

	/** Capsule only: orientation of the capsule axis (local up = capsule length). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider")
	FRotator Rotation = FRotator::ZeroRotator;
};

/** Which face of the lattice (if any) to anchor in place. */
UENUM(BlueprintType)
enum class ESoftBodyAnchor : uint8
{
	/** Nothing pinned — the body falls freely and rests on the ground. */
	None		UMETA(DisplayName = "None (free fall)"),
	/** Pin the top (+Z) face so the body hangs and jiggles. */
	TopFace		UMETA(DisplayName = "Top face (+Z)"),
	/** Pin the bottom (-Z) face so the body wobbles in place like a planted blob. */
	BottomFace	UMETA(DisplayName = "Bottom face (-Z)")
};

/**
 * USoftBodyComponent
 *
 * Owns a 3D lattice of particles simulated on the GPU (tetrahedral XPBD distance
 * constraints) and renders the lattice boundary surface as a real lit mesh. It is a
 * UMeshComponent so it gets its own scene proxy, material support, and participates
 * in lighting/shadows.
 *
 * SB-M1 solves DISTANCE constraints only (every unique edge of the 6-tet Kuhn split
 * of each cube cell). The body is springy but can lose volume — per-tet volume
 * constraints arrive in SB-M2.
 *
 * Threading model (mirrors UClothSimComponent):
 *   - Game thread: builds the lattice + tets + constraints + coloring, owns editable
 *     params, each tick pushes a param snapshot to the render thread, and feeds the
 *     latest positions (from a small GPU readback) into the mesh scene proxy.
 *   - Render thread: owns all GPU sim resources and runs the compute passes.
 * They share state only through ENQUEUE_RENDER_COMMAND and a TSharedPtr.
 */
UCLASS(ClassGroup = (SoftBodySim), meta = (BlueprintSpawnableComponent))
class SOFTBODYSIM_API USoftBodyComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	USoftBodyComponent();

	/** Lattice/cage particle counts along each axis (X, Y, Z) — sets the cage deformation
	 *  resolution (NOT the rendered mesh's poly count). High values get expensive fast
	 *  (particles ~ X·Y·Z, plus a one-time init cost for weight transfer / embedding). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice", meta = (ClampMin = "2", ClampMax = "64"))
	int32 ResX = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice", meta = (ClampMin = "2", ClampMax = "64"))
	int32 ResY = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice", meta = (ClampMin = "2", ClampMax = "64"))
	int32 ResZ = 5;

	/** Distance between adjacent lattice particles (cm). Used for the default box; when a
	 *  Source Mesh is assigned the cage auto-fits to the mesh bounds and this is ignored. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice", meta = (ClampMin = "0.1"))
	float Spacing = 20.0f;

	/**
	 * Optional custom mesh to simulate (SB-M5). When set, the lattice becomes a box CAGE
	 * auto-fit to this mesh's bounds (ResX/Y/Z controls cage density), and the mesh's
	 * render vertices are embedded in the tetrahedra via barycentric weights so the mesh
	 * deforms with the sim (free-form deformation). Leave null to simulate/render the
	 * default box. The mesh's LOD0 is read on the CPU at BeginPlay (enable "Allow CPUAccess"
	 * on the asset for packaged builds; editor works without it).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Mesh")
	TObjectPtr<UStaticMesh> SourceMesh = nullptr;

	/** Fraction of the mesh bounds to expand the cage by on each side, so surface vertices
	 *  sit safely inside the tetrahedra. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Mesh", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float CagePadding = 0.05f;

	/**
	 * Drive per-region stiffness from the Source Mesh's painted VERTEX COLORS (SB-M6).
	 * Paint a grayscale weight on the mesh (Static Mesh editor Paint mode, or your DCC):
	 * by default white = soft/wobbly, black = firm. The weight is sampled onto the cage
	 * particles and scales each constraint's stiffness, so different parts of one body can
	 * be floppier or firmer. Requires the mesh to have a vertex color channel; with no
	 * colors (or no Source Mesh) this is ignored and the body uses uniform stiffness.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Weight Paint")
	bool bWeightPaintStiffness = true;

	/**
	 * XPBD compliance added to a constraint at full paint weight (white). Higher = the
	 * painted-soft regions are floppier. This is the main softness lever in XPBD mode
	 * (bUseXPBD) and, unlike the PBD scales below, its effect does NOT wash out at high
	 * Solver Iterations.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Weight Paint", meta = (ClampMin = "0.0", ClampMax = "0.02", EditCondition = "bUseXPBD"))
	float XpbdSoftCompliance = 0.001f;

	/** PBD path only: constraint stiffness scale at the FIRM end (weight = 0 / black). 1 = fully firm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Weight Paint", meta = (ClampMin = "0.01", ClampMax = "1.0", EditCondition = "!bUseXPBD"))
	float FirmStiffnessScale = 1.0f;

	/** PBD path only: constraint stiffness scale at the SOFT end (weight = 1 / white). Lower = floppier.
	 *  Contrast is strongest at lower Solver Iterations (PBD stiffness is iteration-coupled). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Weight Paint", meta = (ClampMin = "0.01", ClampMax = "1.0", EditCondition = "!bUseXPBD"))
	float SoftStiffnessScale = 0.1f;

	/** Flip the weight meaning so black = soft and white = firm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Weight Paint")
	bool bInvertWeightPaint = false;

	/** Tint the debug points by their sampled weight (firm = blue → soft = red) to preview the paint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Weight Paint")
	bool bVisualizeWeights = false;

	/** Optionally pin a face so the body hangs / stays planted (M1 sanity check). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice")
	ESoftBodyAnchor Anchor = ESoftBodyAnchor::None;

	/** Acceleration applied to free particles (cm/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Physics")
	FVector Gravity = FVector(0.0f, 0.0f, -980.0f);

	/** Per-second linear velocity damping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Physics", meta = (ClampMin = "0.0"))
	float Damping = 0.1f;

	/** Built-in infinite ground plane (normal +Z) so the body rests on a flat floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision")
	bool bGroundPlane = true;

	/** World-space Z height of the ground plane (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (EditCondition = "bGroundPlane"))
	float GroundHeight = 0.0f;

	/** Contact friction [0..1]: how strongly the body grips the ground / colliders. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Friction = 0.3f;

	/**
	 * Collide against ANY mesh in the scene using Unreal's Global Distance Field (SB-M8),
	 * in addition to the explicit collider slots + ground. Requires "Generate Mesh Distance
	 * Fields" in Project Settings and meshes that have distance fields. Lets arbitrary
	 * static meshes act as colliders without authoring sphere/capsule slots.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision")
	bool bUseDistanceFieldCollision = false;

	/** Contact shell thickness for distance-field collision (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "0.0", EditCondition = "bUseDistanceFieldCollision"))
	float DistanceFieldThickness = 2.0f;

	/** Sphere/capsule colliders the body collides against (transforms relative to this component). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision")
	TArray<FSoftBodyCollider> Colliders;

	/** Draw wireframe shapes for the colliders (they are otherwise invisible math). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision")
	bool bDrawColliders = true;

	/**
	 * Body-vs-itself collision via a GPU spatial hash grid (SB-M4). Stops folded/compressed
	 * regions from interpenetrating. Costs a broadphase build + a neighbour-scan pass per substep.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision")
	bool bSelfCollision = false;

	/**
	 * Self-collision thickness as a fraction of Spacing. Min separation kept between
	 * non-adjacent particles = SelfCollisionScale * Spacing. Stays below 2*Spacing (the
	 * nearest non-1-ring rest distance) so it doesn't fight the solver at rest.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "0.1", ClampMax = "1.9"))
	float SelfCollisionScale = 1.0f;

	/** Self-collision repulsion strength [0..1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SelfCollisionStiffness = 1.0f;

	/** Self-collision repulsion passes per substep. More = firmer separation in deep compression, more cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "1", ClampMax = "8"))
	int32 SelfCollisionIterations = 2;

	/**
	 * Collide this body against OTHER soft body actors (SB-M9). A world subsystem gathers all
	 * bodies that opt in and runs a shared spatial-hash repulsion so different bodies push each
	 * other apart (pile/squash together). Needs at least two bodies with this enabled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision")
	bool bInterBodyCollision = false;

	/** Inter-body contact distance (cm): particles of different bodies kept at least this far apart. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "0.1", EditCondition = "bInterBodyCollision"))
	float InterBodyThickness = 20.0f;

	/** Inter-body repulsion strength [0..1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bInterBodyCollision"))
	float InterBodyStiffness = 1.0f;

	/** Inter-body repulsion passes per frame. More = firmer separation in dense piles, more cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "1", ClampMax = "8", EditCondition = "bInterBodyCollision"))
	int32 InterBodyIterations = 2;

	/** Substeps per frame. The biggest stability lever: more = stiffer, more stable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "1", ClampMax = "16"))
	int32 Substeps = 2;

	/** Constraint relaxation iterations per substep. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "1", ClampMax = "64"))
	int32 SolverIterations = 8;

	/** Distance-constraint correction strength [0..1] (PBD path only). 1 = try to fully satisfy each iteration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Stiffness = 1.0f;

	/**
	 * Use XPBD for the distance solve (SB-M7). XPBD makes stiffness a true compliance that
	 * is independent of iteration/substep count, so weight-painted softness holds up at any
	 * solver settings (unlike PBD stiffness, which washes out at high iterations). Leave on
	 * unless you want to compare against the old PBD path.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver")
	bool bUseXPBD = true;

	/** XPBD: baseline compliance applied everywhere. 0 = rigid; raise to soften the WHOLE body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "0.0", ClampMax = "0.02", EditCondition = "bUseXPBD"))
	float XpbdGlobalCompliance = 0.0f;

	/**
	 * Per-tet VOLUME-constraint correction strength [0..1] (SB-M2). Higher = firmer
	 * volume preservation (more "jelly", resists squashing); 0 = distance-only (SB-M1
	 * behaviour, can flatten). The headline soft body lever.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VolumeStiffness = 1.0f;

	/**
	 * Fixed simulation timestep (seconds). The sim always advances in chunks of this
	 * size regardless of frame rate, which makes the result frame-rate independent and
	 * stable. 1/60 is a good default.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "0.002", ClampMax = "0.05"))
	float FixedTimeStep = 1.0f / 60.0f;

	/** Safety cap on fixed steps per frame so a hitch can't trigger a "spiral of death". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxStepsPerFrame = 4;

	/**
	 * Left-click and drag to grab the nearest surface point and pull the body around
	 * (SB-M3). Enables the mouse cursor at BeginPlay. Picking + dragging are CPU-side
	 * (from the position readback); a GPU pass pulls the grabbed particle to the cursor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Interaction")
	bool bEnableMouseDrag = true;

	/** Firmness of the grab [0..1]: 1 = the grabbed point snaps to the cursor, lower = softer/laggier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Interaction", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GrabStiffness = 0.8f;

	/** How close (in multiples of Spacing) the click ray must pass to a surface particle to grab it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Interaction", meta = (ClampMin = "0.25"))
	float GrabPickRadiusScale = 1.5f;

	/** Material applied to the soft body. Use a TWO-SIDED material to light both faces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Render")
	TObjectPtr<UMaterialInterface> SoftBodyMaterial = nullptr;

	/** Also draw each particle as a debug point on top of the mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Debug")
	bool bDrawDebugPoints = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Debug", meta = (ClampMin = "0.5"))
	float DebugPointSize = 4.0f;

	/** Show an on-screen readout of particle / tet / constraint / color counts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Debug")
	bool bShowStats = false;

	//~ UPrimitiveComponent / UMeshComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const override;

	/** Render-thread sim state, shared with the world subsystem for inter-body collision (SB-M9). */
	TSharedPtr<FSoftBodyRenderResources> GetRenderResources() const { return RenderResources; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** Build the particle lattice (world + local), tets, boundary surface and constraints, then upload to the GPU. */
	void InitializeSimulation();

	/** Build the 6-tet Kuhn decomposition of every cube cell (conforming). */
	void BuildTets();

	/** Build the static boundary-surface triangle list + UVs (the 6 outer faces). */
	void BuildBoundarySurface();

	/** Derive deduped distance constraints from the tet edges and greedily graph-color
	 *  them, producing a color-sorted buffer + per-color ranges. */
	void BuildConstraints(TArray<FGPUConstraint>& OutConstraints, TArray<FSoftBodyColorRange>& OutColorRanges) const;

	/** Build one volume constraint per tet (signed rest volume from the rest lattice) and
	 *  greedily graph-color the tets (two tets conflict if they share ANY of their 4
	 *  vertices), producing a color-sorted buffer + per-color ranges (SB-M2). */
	void BuildVolumeConstraints(TArray<FGPUVolumeConstraint>& OutConstraints, TArray<FSoftBodyColorRange>& OutColorRanges) const;

	/** Compute smooth per-vertex normals + arbitrary tangents for a vertex set + triangle
	 *  list (local space). Used for both the box surface and the embedded custom mesh. */
	void ComputeNormalsTangents(
		const TArray<FVector3f>& InPositions,
		const TArray<uint32>& InTriangles,
		TArray<FVector3f>& OutNormals,
		TArray<FVector3f>& OutTangents) const;

	/** Read the assigned SourceMesh's LOD0 positions / triangles / UV0 to the CPU. Returns
	 *  false (→ fall back to the box) if there's no mesh or its data isn't accessible (SB-M5). */
	bool ReadSourceMesh();

	/** For each SourceMesh vertex, find the cage tetrahedron containing it and store its
	 *  barycentric weights, so the mesh can be reconstructed from the deformed cage (SB-M5). */
	void BuildEmbedding();

	/** Sample the mesh's per-vertex paint weight onto each cage particle (nearest mesh
	 *  vertex), producing ParticleWeights. No-op (clears data) when weight paint is off,
	 *  there's no mesh, or the mesh has no vertex colors (SB-M6). */
	void BuildParticleWeights();

	/** Map a [0,1] paint weight to a per-constraint StiffScale (FirmStiffnessScale at 0,
	 *  SoftStiffnessScale at 1). Returns 1.0 when no weight data is active. */
	float WeightToStiffScale(float Weight) const;

	/** Pull the latest readback positions, convert to local space, recompute normals,
	 *  and push the updated vertices to the scene proxy. */
	void UpdateMeshFromSimulation();

	/** Poll the mouse: on click pick the nearest boundary particle to the cursor ray (CPU,
	 *  from the readback) and remember it + its grab depth; while held, track the world-space
	 *  cursor target at that depth. Updates the grab state read by TickComponent (SB-M3). */
	void UpdateMouseGrab();

	/** Draw readback positions as debug points (optional). */
	void DrawDebug();

	/** Draw wireframe shapes for the authored colliders so they're visible (SB-M4). */
	void DrawColliders();

	/** Flat lattice index for (x, y, z). */
	FORCEINLINE int32 LatticeIndex(int32 X, int32 Y, int32 Z) const
	{
		return X + Y * ResX + Z * ResX * ResY;
	}

	int32 NumParticles = 0;

	/** Unsimulated real time carried over between frames for the fixed-step loop. */
	float TimeAccumulator = 0.0f;

	// Static topology (built once).
	TArray<FSoftBodyTet> Tets;
	TArray<uint32>       Triangles;   // box-surface triangles (non-mesh mode)
	TArray<FVector2f>    UV0;         // box-surface UVs (non-mesh mode)

	// --- Embedded custom mesh (SB-M5) -------------------------------------
	bool                 bMeshMode = false;       // true when a valid SourceMesh is embedded
	int32                NumMeshVerts = 0;
	TArray<FVector3f>    MeshRestPositions;        // mesh LOD0 verts, component-local rest pose
	TArray<uint32>       MeshTriangles;            // mesh LOD0 indices
	TArray<FVector2f>    MeshUV0;                  // mesh LOD0 UV channel 0
	TArray<int32>        EmbedTet;                 // per mesh vert: containing cage tet index
	TArray<FVector4f>    EmbedWeights;             // per mesh vert: barycentric (V0,V1,V2,V3)
	TArray<float>        MeshVertWeights;          // per mesh vert: painted weight (R channel, 0..1)
	TArray<float>        ParticleWeights;          // per cage particle: sampled weight (0..1)
	bool                 bHasWeightData = false;   // true when weight-paint stiffness is active
	FVector3f            CageMin = FVector3f::ZeroVector; // cage local-space bounds (for cell lookup)
	FVector3f            CageCellSize = FVector3f::OneVector;
	float                EffectiveSpacing = 20.0f; // min cage cell size (self-collision/grab scale)

	// Scratch reused each frame for the embedded-mesh proxy update (local space).
	TArray<FVector3f>    MeshLocalPositions;
	TArray<FVector3f>    MeshNormals;
	TArray<FVector3f>    MeshTangents;

	// Initial local-space positions (corner at component origin); used to seed the proxy
	// and to orient the boundary triangle winding.
	TArray<FVector3f> InitialLocalPositions;

	// Unique lattice indices on the box surface (the only ones the mouse can grab).
	TArray<int32> BoundaryParticles;

	// Mouse grab state (SB-M3). GrabbedIndex == INDEX_NONE when not grabbing.
	int32   GrabbedIndex = INDEX_NONE;
	float   GrabDepth = 0.0f;            // distance along the click ray to the grabbed point
	bool    bIsGrabbing = false;
	FVector CurrentGrabTarget = FVector::ZeroVector; // world space

	// Scratch reused each frame for the proxy update (local space).
	TArray<FVector3f> LocalPositions;
	TArray<FVector3f> LocalNormals;
	TArray<FVector3f> LocalTangents;

	// Stats captured at BeginPlay for the on-screen readout.
	int32 NumConstraintsBuilt = 0;
	int32 NumColorsBuilt = 0;
	int32 NumVolumeConstraintsBuilt = 0;
	int32 NumVolumeColorsBuilt = 0;

	FBoxSphereBounds LocalBounds = FBoxSphereBounds(ForceInit);

	// Shared with render command lambdas so GPU work outlives this component safely.
	TSharedPtr<FSoftBodyRenderResources> RenderResources;
};
