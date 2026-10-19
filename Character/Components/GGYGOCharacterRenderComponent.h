/** @file 通用角色表现配置、材质资源所有权和场景主光输入。 */
#pragma once

#include "Components/ActorComponent.h"
#include "GGYGOCharacterRenderComponent.generated.h"

class ADirectionalLight;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USkeletalMeshComponent;
class USkeletalMesh;

/** Explicit material binding supplied by a character Blueprint. */
USTRUCT(BlueprintType)
struct FGGYGOCharacterRenderSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render")
	FName SlotName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render")
	TObjectPtr<UMaterialInterface> Material = nullptr;
};

/** Transient ownership receipt. OriginalOverride may be null (asset default). */
USTRUCT()
struct FGGYGOCharacterRenderBinding
{
	GENERATED_BODY()
	UPROPERTY(Transient) int32 SlotIndex = INDEX_NONE;
	UPROPERTY(Transient) FName SlotName;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> OriginalOverride = nullptr;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> InstalledMaterial = nullptr;
};

/**
 * Per-character presentation adapter. The material owns the toon BRDF; this
 * component supplies a stable key light, creates MIDs, and follows the pose
 * with an optional inverted-hull outline. It never changes scene lighting.
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOCharacterRenderComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGGYGOCharacterRenderComponent();

	/** Apply validated Unlit bindings. Call ReleaseMaterials before replacing the mesh or cosmetics.
	 * Failed configuration releases our resources and requires an explicit refresh after correction.
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Rendering")
	void RefreshMaterials();

	/** Releases only the material overrides and outline still owned by this component. */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Rendering")
	void ReleaseMaterials();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	TArray<FGGYGOCharacterRenderSlot> MaterialSlots;

	/** Optional explicit key light. If unset, an eligible directional light is cached separately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	TObjectPtr<ADirectionalLight> KeyLight = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering", meta = (ClampMin = "0.01"))
	float LightingUpdateInterval = 0.1f;

	/** Directional-light intensity that produces the authored material brightness. Used when intensity response > 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Light Response", meta = (ClampMin = "0.01"))
	float ReferenceKeyLightIntensity = 1.0f;

	/** 0 ignores scene intensity; 1 follows its ratio to the reference. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Light Response", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float KeyLightIntensityResponse = 1.0f;

	/** Character-level brightness at the reference intensity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Light Response", meta = (ClampMin = "0.0"))
	float KeyLightOutputScale = 1.0f;

	/** Prevents strong scene lights from washing out authored colors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Light Response", meta = (ClampMin = "0.0"))
	float MaxKeyLightStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Light Response")
	FLinearColor KeyLightTint = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Light Response", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OccludedKeyLightVisibility = 0.35f;

	/** Coarse character-level occlusion. The material still handles the per-pixel ramp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	bool bTraceKeyLightOcclusion = true;

	/** 非 PIE 关卡使用瞬态图元数据和描边壳预览同一配置；不修改 Mesh 的材质覆盖或序列化默认。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Editor")
	bool bPreviewSceneLighting = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	bool bEnableOutline = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	TObjectPtr<UMaterialInterface> OutlineMaterial = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering", meta = (ClampMin = "0.0"))
	float OutlineWidth = 0.5f;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void Activate(bool bReset = false) override;
	virtual void Deactivate() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	friend class FGGYGOCharacterRenderOwnershipTest;
	friend class FGGYGOCharacterRenderLightTest;
	friend class FGGYGOCharacterRenderContractTest;
	friend class FGGYGOCharacterRenderPreviewTest;
	ADirectionalLight* ResolveKeyLight();
	bool IsEligibleKeyLight(const ADirectionalLight* Light) const;
	bool HasCurrentMesh() const;
	bool ValidateLightSettings(FString& OutError) const;
	bool ValidateMaterialBindings(const USkeletalMeshComponent* Mesh, FString& OutError) const;
	void FailRendering(const FString& Error);
	void UpdateLighting();
	bool SampleLighting(FVector& OutDirection, FLinearColor& OutColor, float& OutStrength, float& OutVisibility);
	bool UsesEditorSceneLighting() const;
	void RefreshEditorLighting();
	bool ValidatePreviewBindings(const USkeletalMeshComponent* Mesh, FString& OutError) const;
	bool WritePreviewLighting(TConstArrayView<float> Values);
	void ReleasePreviewLighting();
	bool UpdateOutline(USkeletalMeshComponent* Mesh);
	void ReleaseOutline();

	// 仅记录本组件租用的九个预览槽；不缓存光照事实或其它系统的数据。
	static constexpr int32 PreviewDataCount = 9;
	UPROPERTY(Transient, DuplicateTransient) TWeakObjectPtr<USkeletalMeshComponent> PreviewMesh;
	UPROPERTY(Transient, DuplicateTransient) TWeakObjectPtr<USkeletalMesh> PreviewMeshAsset;
	TArray<float> PreviewOriginalData;
	TArray<float> PreviewWrittenData;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> TargetMesh;

	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<USkeletalMeshComponent> OutlineMesh;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> DynamicMaterials;

	UPROPERTY(Transient)
	TArray<FGGYGOCharacterRenderBinding> MaterialBindings;

	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMesh> BoundMeshAsset;

	UPROPERTY(Transient)
	TWeakObjectPtr<ADirectionalLight> ResolvedKeyLight;
};
