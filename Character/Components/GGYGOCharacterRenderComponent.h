/** @file GGYGOCharacterRenderComponent.h
 *  @brief 通用角色表现配置、私有材质资源及场景主光/四灯输入。
 */
#pragma once

#include "Components/ActorComponent.h"
#include "Engine/AssetUserData.h"
#include "Character/GGYGOCharacterShadowCapture.h"
#include "Character/GGYGOCharacterLightSources.h"
#include "GGYGOCharacterRenderComponent.generated.h"

class ADirectionalLight;
class ALight;
class UPackage;
class FObjectPreSaveContext;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USkeletalMeshComponent;
class USkeletalMesh;

/** 自动方向光是保留的明确模式；角色配置可要求实际BP光源引用。 */
UENUM(BlueprintType)
enum class EGGYGOCharacterKeyLightSelection : uint8 { AutomaticDirectional, Explicit };

/** 编辑器预览传输明确选择；完整灯输入沿原MID所有权，不写材质资产。 */
UENUM(BlueprintType)
enum class EGGYGOCharacterPreviewTransport : uint8
{
	PrimitiveData,
	TransientMaterials
};

UENUM(BlueprintType)
enum class EGGYGOCharacterLightingReadiness : uint8
{
	Ready,
	WaitingForRegistration,
	Invalid,
	/** 外部FX灯本次失败；不提交本次样本，原Tick可在后续真实采样恢复。 */
	SourceFailed
};

/** 一盏明确配置的局部灯；方向符号、强度校准和源衰减系数均由外部提供。 */
USTRUCT(BlueprintType)
struct FGGYGOCharacterAdditionalLight
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bConfigured = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TObjectPtr<ALight> SourceLight = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector4 Attenuation = FVector4(0, 0, 0, 0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float IntensityScale = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float DirectionSign = 0;
};

/** 本组件消费的完整项目配置；空列表仅在bConfigured显式为真时代表未选择灯。 */
USTRUCT(BlueprintType)
struct FGGYGOCharacterAdditionalLighting
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bConfigured = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float GlobalIntensity = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FGGYGOCharacterAdditionalLight> Lights;
};

/** 同一次采样的派生输入；位置已换算米，数组只含明确选择的灯，不跨更新缓存。 */
USTRUCT(BlueprintType)
struct FGGYGOCharacterAdditionalLightingSample
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) TArray<FVector> PositionsMeters;
	UPROPERTY(BlueprintReadOnly) TArray<FVector> Directions;
	UPROPERTY(BlueprintReadOnly) TArray<FVector4> Attenuations;
	UPROPERTY(BlueprintReadOnly) TArray<FLinearColor> Colors;
	UPROPERTY(BlueprintReadOnly) float GlobalIntensity = 0;
};

/** 临时MID携带自己的原覆盖凭据；Undo持有MID时凭据同寿命，不另存退休材质缓存。 */
UCLASS(Transient)
class GGYGO_API UGGYGOCharacterPreviewMaterialData : public UAssetUserData
{
	GENERATED_BODY()
public:
	UPROPERTY(Transient) TWeakObjectPtr<AActor> OwnerActor;
	UPROPERTY(Transient) FName ComponentName;
	UPROPERTY(Transient) TWeakObjectPtr<USkeletalMesh> MeshAsset;
	UPROPERTY(Transient) int32 SlotIndex = INDEX_NONE;
	UPROPERTY(Transient) FName SlotName;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> OriginalOverride = nullptr;
};

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
	DECLARE_DELEGATE_ThreeParams(FSceneMaterialsInvalidated, uint64, EGGYGOCharacterLightingReadiness, const FString&);
	DECLARE_EVENT_TwoParams(UGGYGOCharacterRenderComponent, FSceneLightingReadinessChanged,
		EGGYGOCharacterLightingReadiness, const FString&);
	/** FX先弱订阅再尝试登记；原Tick真正可用后通知，消费方不另加轮询/帧调度。 */
	FSceneLightingReadinessChanged& OnSceneLightingReadinessChanged() { return SceneLightingReadinessChanged; }
	/** FX保有并安装私有MID；本组件仅沿原Tick写同一主光/四灯。成功才发handle。
	 * 必须弱绑定失效回调；失效先撤登记再通知，调用方按handle归还自己的实例。
	 * Waiting/SourceFailed不发handle或写MID；Unregister幂等且不修改外部MID的所有权/覆盖。
	 */
	EGGYGOCharacterLightingReadiness RegisterSceneMaterialTargets(UObject* SourceOwner,
		TConstArrayView<UMaterialInstanceDynamic*> Materials, FSceneMaterialsInvalidated OnInvalidated,
		uint64& OutHandle, FString& OutError);
	void UnregisterSceneMaterialTargets(uint64 Handle);

	/** Apply validated Unlit bindings. Call ReleaseMaterials before replacing the mesh or cosmetics.
	 * Failed configuration releases our resources and requires an explicit refresh after correction.
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Rendering")
	void RefreshMaterials();

	/** Releases only the material overrides and outline still owned by this component. */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Rendering")
	void ReleaseMaterials();

	/** 只读采样给定配置；作者和运行更新使用同一生产者，注册暂态与错误明确区分。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Rendering")
	EGGYGOCharacterLightingReadiness ReadAdditionalLighting(const FGGYGOCharacterAdditionalLighting& Configuration,
		FGGYGOCharacterAdditionalLightingSample& OutSample, FString& OutError) const;
	/** 显式场景灯保其槽位；FX只使用剩余额度，旧角色默认Disabled。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	FGGYGOCharacterFXLightSelection FXLightSelection;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	TArray<FGGYGOCharacterRenderSlot> MaterialSlots;

	/** Explicit模式必须有同World引用；自动模式仍可用此属性固定选中源。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering")
	TObjectPtr<ADirectionalLight> KeyLight = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Light Response")
	EGGYGOCharacterKeyLightSelection KeyLightSelection = EGGYGOCharacterKeyLightSelection::AutomaticDirectional;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Shadows")
	FGGYGOCharacterShadowSettings ShadowSettings;

	/** 0由原生组件Tick逐帧采样；正值为原有间隔模式，不建立第二调度器。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering", meta = (ClampMin = "0.0"))
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

	/** 非PIE预览；PrimitiveData保留实际槽，TransientMaterials按明确模式租用并归还实例覆盖。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Editor")
	bool bPreviewSceneLighting = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Editor")
	EGGYGOCharacterPreviewTransport EditorPreviewTransport = EGGYGOCharacterPreviewTransport::PrimitiveData;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GGYGO|Rendering|Additional Lights")
	FGGYGOCharacterAdditionalLighting AdditionalLighting;

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
	virtual void PreEditUndo() override;
	virtual void PostEditUndo() override;
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
	void ReleaseMaterialResources(EGGYGOCharacterLightingReadiness Readiness, const FString& Reason, bool bNotify = true);
	void PublishSceneLightingReadiness(EGGYGOCharacterLightingReadiness Readiness, const FString& Reason);
	void UpdateLighting();
	void WriteSceneMaterialLighting(UMaterialInstanceDynamic& Material, const FVector& Direction,
		const FLinearColor& Color, float Strength, float Visibility,
		const FGGYGOCharacterAdditionalLightingSample* Additional) const;
	bool SampleLighting(FVector& OutDirection, FLinearColor& OutColor, float& OutStrength, float& OutVisibility);
	bool UsesEditorSceneLighting() const;
	void RefreshEditorLighting();
	bool ValidatePreviewBindings(const USkeletalMeshComponent* Mesh, FString& OutError) const;
	bool WritePreviewLighting(TConstArrayView<float> Values);
	void ReleasePreviewLighting();
	bool UpdateOutline(USkeletalMeshComponent* Mesh);
	void ReleaseOutline();
	bool ValidateAdditionalMaterialContract(bool& OutUsesAdditional, FString& OutError) const;
	bool UsesEditorMaterialPreview() const;
	void HandleLightingUnavailable(EGGYGOCharacterLightingReadiness Readiness, const FString& Reason);
#if WITH_EDITOR
	void BindEditorLifecycle();
	void UnbindEditorLifecycle();
	void HandleEditorPackagePreSave(UPackage* Package, FObjectPreSaveContext Context);
	void HandleEditorPrePIE(bool bSimulating);
	void HandleEditorEndPIE(bool bSimulating);
	void HandleEditorPostUndoRedo();
	void RestoreSerializedPreviewOverrides();
	void ResumeEditorPreview();
	FDelegateHandle PackagePreSaveHandle;
	FDelegateHandle PrePIEHandle;
	FDelegateHandle EndPIEHandle;
	FDelegateHandle UndoRedoHandle;
	bool bEditorPreviewSuspendedForPIE = false;
	bool bResumeEditorPreview = false;
#endif
	// 原材质租用已归还、仍活动时原Tick重采；注册暂态或外部源失败可恢复，配置Invalid不重试。
	bool bRetryLighting = false;
	struct FSceneMaterialTargets
	{
		TWeakObjectPtr<UObject> SourceOwner;
		TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> Materials;
		FSceneMaterialsInvalidated OnInvalidated;
	};
	// 外部MID的写入登记；FX保强引用，注销/原表现释放时整项摘除，不拥有其材质覆盖。
	TMap<uint64, FSceneMaterialTargets> SceneMaterialTargets;
	uint64 NextSceneMaterialHandle = 1;
	bool bReleasingMaterials = false;
	FSceneLightingReadinessChanged SceneLightingReadinessChanged;
	// 仅抑制同状态/原因的重复通知及源失败诊断，不缓存灯值；Ready由原采样实际写MID。
	TOptional<EGGYGOCharacterLightingReadiness> LastSceneLightingReadiness;
	FString LastSceneLightingReason;
	uint64 SceneLightingNotificationSerial = 0; // 释放通知返栈核验；不作为采样版本或灯状态。
	// 与本次私有MID同租用期；释放/保存/Undo/PIE前一并归还capture及RT。
	TSharedPtr<FGGYGOCharacterShadowCapture> ShadowCapture;
	bool bUsesAdditionalInputs = false; // 来源为本次已验证材质接口；ReleaseMaterials失效。

	// 仅记录本组件租用的九个预览槽；不缓存光照事实或其它系统的数据。
	static constexpr int32 PreviewDataCount = 9;
	UPROPERTY(Transient, DuplicateTransient) TWeakObjectPtr<USkeletalMeshComponent> PreviewMesh;
	UPROPERTY(Transient, DuplicateTransient) TWeakObjectPtr<USkeletalMesh> PreviewMeshAsset;
	TArray<float> PreviewOriginalData;
	TArray<float> PreviewWrittenData;

	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<USkeletalMeshComponent> TargetMesh;

	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<USkeletalMeshComponent> OutlineMesh;

	UPROPERTY(Transient, DuplicateTransient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> DynamicMaterials;

	UPROPERTY(Transient, DuplicateTransient)
	TArray<FGGYGOCharacterRenderBinding> MaterialBindings;

	UPROPERTY(Transient, DuplicateTransient)
	TWeakObjectPtr<USkeletalMesh> BoundMeshAsset;

	UPROPERTY(Transient, DuplicateTransient)
	TWeakObjectPtr<ADirectionalLight> ResolvedKeyLight;
};
