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

	/** Lattice particle counts along each axis (X, Y, Z). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice", meta = (ClampMin = "2", ClampMax = "32"))
	int32 ResX = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice", meta = (ClampMin = "2", ClampMax = "32"))
	int32 ResY = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice", meta = (ClampMin = "2", ClampMax = "32"))
	int32 ResZ = 5;

	/** Distance between adjacent lattice particles (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SoftBody|Lattice", meta = (ClampMin = "0.1"))
	float Spacing = 20.0f;

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

	/** Contact friction [0..1]: how strongly the body grips the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Collision", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Friction = 0.3f;

	/** Substeps per frame. The biggest stability lever: more = stiffer, more stable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "1", ClampMax = "16"))
	int32 Substeps = 2;

	/** Constraint relaxation iterations per substep. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "1", ClampMax = "64"))
	int32 SolverIterations = 8;

	/** Distance-constraint correction strength [0..1]. 1 = try to fully satisfy each iteration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SoftBody|Solver", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Stiffness = 1.0f;

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

	/** Compute smooth per-vertex normals + arbitrary tangents from the boundary surface (local space). */
	void ComputeSurfaceNormalsTangents(
		const TArray<FVector3f>& InPositions,
		TArray<FVector3f>& OutNormals,
		TArray<FVector3f>& OutTangents) const;

	/** Pull the latest readback positions, convert to local space, recompute normals,
	 *  and push the updated vertices to the scene proxy. */
	void UpdateMeshFromSimulation();

	/** Draw readback positions as debug points (optional). */
	void DrawDebug();

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
	TArray<uint32>       Triangles;
	TArray<FVector2f>    UV0;

	// Initial local-space positions (corner at component origin); used to seed the proxy
	// and to orient the boundary triangle winding.
	TArray<FVector3f> InitialLocalPositions;

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
