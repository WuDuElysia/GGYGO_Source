#pragma once

#include "Components/ActorComponent.h"
#include "GGYGOPyriosRenderComponent.generated.h"

class ADirectionalLight;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USkeletalMeshComponent;

/** A material slot override for a character using the NapAvatar-style renderer. */
USTRUCT(BlueprintType)
struct FGGYGOCharacterRenderSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render")
	FName SlotName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render")
	TObjectPtr<UMaterialInterface> Material = nullptr;
};

/**
 * Per-character presentation adapter. The material owns the toon BRDF; this
 * component supplies a stable key light, creates MIDs, and follows the pose
 * with an optional inverted-hull outline. It never changes scene lighting.
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOPyriosRenderComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGGYGOPyriosRenderComponent();

	/** Apply the supplied slot overrides again after a mesh or cosmetic swap. */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Rendering")
	void RefreshMaterials();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	TArray<FGGYGOCharacterRenderSlot> MaterialSlots;

	/** Uses bundled Pyrios material instances when the mesh is Pyrois. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	bool bAutoConfigurePyrios = true;

	/** Optional explicit key light. If unset, the first directional light is used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	TObjectPtr<ADirectionalLight> KeyLight = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering", meta = (ClampMin = "0.01"))
	float LightingUpdateInterval = 0.1f;

	/** Coarse character-level occlusion. The material still handles the per-pixel ramp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	bool bTraceKeyLightOcclusion = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	bool bEnableOutline = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	TObjectPtr<UMaterialInterface> OutlineMaterial = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering", meta = (ClampMin = "0.0"))
	float OutlineWidth = 0.6f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void UpdateLighting();
	void UpdateOutline();
	UMaterialInterface* FindMaterialForSlot(FName SlotName) const;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> TargetMesh;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> OutlineMesh;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> DynamicMaterials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> OriginalMaterials;
};
