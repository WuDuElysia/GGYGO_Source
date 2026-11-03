/** @file GGYGOCharacterRenderComponent.cpp
 *  @brief 校验Toon接口，向自有及登记MID写同次场景灯样本，并归还原表现资源。
 */
#include "Character/Components/GGYGOCharacterRenderComponent.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Light.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/ScopeExit.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterRenderComponent)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOCharacterRendering, Log, All);

namespace
{
	const FName LightDirectionParameter(TEXT("KeyLightDirectionWS"));
	const FName LightColorParameter(TEXT("KeyLightColor"));
	const FName LightStrengthParameter(TEXT("SceneLightStrength"));
	const FName LightVisibilityParameter(TEXT("KeyLightVisibility"));
	const FName AdditionalCountParameter(TEXT("AdditionalLightCount"));
	const FName AdditionalGlobalParameter(TEXT("AdditionalLightGlobalIntensity"));
	const FName PreviewTransportParameter(TEXT("UseScenePreviewCPD"));
	FName AdditionalParameter(const TCHAR* Family, int32 Index)
	{
		return FName(*FString::Printf(TEXT("AdditionalLight%s%d"), Family, Index));
	}
	const FName PreviewDataParameters[] = {
		TEXT("ScenePreviewValid"), TEXT("ScenePreviewDirectionX"), TEXT("ScenePreviewDirectionY"),
		TEXT("ScenePreviewDirectionZ"), TEXT("ScenePreviewColorR"), TEXT("ScenePreviewColorG"),
		TEXT("ScenePreviewColorB"), TEXT("ScenePreviewStrength"), TEXT("ScenePreviewVisibility")};

	bool IsFiniteColor(const FLinearColor& Color)
	{
		return FMath::IsFinite(Color.R) && FMath::IsFinite(Color.G)
			&& FMath::IsFinite(Color.B) && FMath::IsFinite(Color.A);
	}

	bool FitsMaterialFloat(double Value)
	{
		return FMath::IsFinite(Value) && FMath::Abs(Value) <= double(MAX_flt);
	}

	bool IsUnlitSurface(const UMaterialInterface* Material)
	{
		const UMaterial* Base = IsValid(Material) ? Material->GetMaterial() : nullptr;
		return IsValid(Base) && Base->MaterialDomain == MD_Surface
			&& Material->GetShadingModels().HasOnlyShadingModel(MSM_Unlit);
	}
}

UGGYGOCharacterRenderComponent::UGGYGOCharacterRenderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bAutoActivate = true;
	bTickInEditor = true;
}

void UGGYGOCharacterRenderComponent::BeginPlay()
{
	Super::BeginPlay();
	if (IsActive()) RefreshMaterials();
}

void UGGYGOCharacterRenderComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	ReleaseMaterials();
	Super::EndPlay(Reason);
}

void UGGYGOCharacterRenderComponent::OnRegister()
{
	Super::OnRegister();
#if WITH_EDITOR
	BindEditorLifecycle();
	RestoreSerializedPreviewOverrides();
#endif
	if (HasBegunPlay() && IsActive()) RefreshMaterials();
	else if (UsesEditorSceneLighting()) RefreshMaterials();
}

void UGGYGOCharacterRenderComponent::OnUnregister()
{
#if WITH_EDITOR
	UnbindEditorLifecycle();
#endif
	ReleaseMaterials();
	Super::OnUnregister();
}

void UGGYGOCharacterRenderComponent::Activate(bool bReset)
{
	Super::Activate(bReset);
	if (HasBegunPlay()) RefreshMaterials();
	else if (UsesEditorSceneLighting()) RefreshMaterials();
}

void UGGYGOCharacterRenderComponent::Deactivate()
{
	ReleaseMaterials();
	Super::Deactivate();
}

bool UGGYGOCharacterRenderComponent::ValidateLightSettings(FString& OutError) const
{
	if (KeyLightSelection != EGGYGOCharacterKeyLightSelection::AutomaticDirectional
		&& KeyLightSelection != EGGYGOCharacterKeyLightSelection::Explicit)
		OutError = TEXT("主灯来源模式无效");
	else if (KeyLightSelection == EGGYGOCharacterKeyLightSelection::Explicit
		&& (!IsValid(KeyLight) || KeyLight->IsActorBeingDestroyed() || KeyLight->GetWorld() != GetWorld()))
		OutError = TEXT("明确主灯模式缺同World的实际BP/配置引用");
	else if (KeyLightSelection == EGGYGOCharacterKeyLightSelection::Explicit && !IsValid(KeyLight->GetLightComponent()))
		OutError = TEXT("明确主灯缺实际发光组件");
	else if (ShadowSettings.Mode == EGGYGOCharacterShadowMode::DepthCaptures
		&& KeyLightSelection != EGGYGOCharacterKeyLightSelection::Explicit)
		OutError = TEXT("深度捕获阴影需要BP明确绑定主灯");
	else if (EditorPreviewTransport != EGGYGOCharacterPreviewTransport::PrimitiveData
		&& EditorPreviewTransport != EGGYGOCharacterPreviewTransport::TransientMaterials)
		OutError = TEXT("编辑器传输模式无效，须明确PrimitiveData或TransientMaterials");
	else if (!FMath::IsFinite(LightingUpdateInterval) || LightingUpdateInterval < 0.0f)
		OutError = TEXT("LightingUpdateInterval须有限且非负；0表示原Tick逐帧采样");
	else if (!FMath::IsFinite(KeyLightIntensityResponse) || KeyLightIntensityResponse < 0.0f || KeyLightIntensityResponse > 1.0f)
		OutError = TEXT("KeyLightIntensityResponse must be finite and in [0, 1]");
	else if (KeyLightIntensityResponse > 0.0f && (!FMath::IsFinite(ReferenceKeyLightIntensity) || ReferenceKeyLightIntensity < 0.01f))
		OutError = TEXT("ReferenceKeyLightIntensity must be finite and >= 0.01");
	else if (!FMath::IsFinite(KeyLightOutputScale) || KeyLightOutputScale < 0.0f)
		OutError = TEXT("KeyLightOutputScale must be finite and >= 0");
	else if (!FMath::IsFinite(MaxKeyLightStrength) || MaxKeyLightStrength < 0.0f)
		OutError = TEXT("MaxKeyLightStrength must be finite and >= 0");
	else if (!IsFiniteColor(KeyLightTint) || KeyLightTint.R < 0.0f || KeyLightTint.G < 0.0f || KeyLightTint.B < 0.0f)
		OutError = TEXT("KeyLightTint must have finite, nonnegative RGB values");
	else if (bTraceKeyLightOcclusion && (!FMath::IsFinite(OccludedKeyLightVisibility)
		|| OccludedKeyLightVisibility < 0.0f || OccludedKeyLightVisibility > 1.0f))
		OutError = TEXT("OccludedKeyLightVisibility must be finite and in [0, 1]");
	else
	{
		return UGGYGOCharacterLightSources::ValidateSelection(FXLightSelection, OutError)
			&& FGGYGOCharacterShadowCapture::ValidateSettings(ShadowSettings, OutError);
	}
	return false;
}

bool UGGYGOCharacterRenderComponent::ValidateMaterialBindings(const USkeletalMeshComponent* Mesh, FString& OutError) const
{
	if (!IsValid(Mesh) || !IsValid(Mesh->GetSkeletalMeshAsset()))
	{
		OutError = TEXT("Character Mesh or its skeletal mesh asset is missing");
		return false;
	}
	const TArray<FName> Slots = Mesh->GetMaterialSlotNames();
	TSet<FName> ConfiguredSlots;
	for (const FGGYGOCharacterRenderSlot& Entry : MaterialSlots)
	{
		int32 Matches = 0;
		for (const FName Slot : Slots) Matches += Slot == Entry.SlotName ? 1 : 0;
		if (Entry.SlotName.IsNone() || ConfiguredSlots.Contains(Entry.SlotName) || Matches != 1)
		{
			OutError = FString::Printf(TEXT("Slot '%s' must be configured once and occur once on mesh '%s'"),
				*Entry.SlotName.ToString(), *GetPathNameSafe(Mesh->GetSkeletalMeshAsset()));
			return false;
		}
		ConfiguredSlots.Add(Entry.SlotName);
		if (!IsUnlitSurface(Entry.Material))
		{
			OutError = FString::Printf(TEXT("Slot '%s' material '%s' must be a valid Unlit surface; the material owns Toon shading"),
				*Entry.SlotName.ToString(), *GetPathNameSafe(Entry.Material));
			return false;
		}
		if (ShadowSettings.Mode == EGGYGOCharacterShadowMode::DepthCaptures
			&& !FGGYGOCharacterShadowCapture::ValidateMaterial(*Entry.Material, OutError)) return false;
		FLinearColor VectorValue;
		float ScalarValue;
		for (const FName Parameter : {LightDirectionParameter, LightColorParameter})
		{
			if (!Entry.Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Parameter), VectorValue))
			{
				OutError = FString::Printf(TEXT("Slot '%s' material '%s' is missing vector parameter '%s'"),
					*Entry.SlotName.ToString(), *GetPathNameSafe(Entry.Material), *Parameter.ToString());
				return false;
			}
		}
		for (const FName Parameter : {LightStrengthParameter, LightVisibilityParameter})
		{
			if (!Entry.Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Parameter), ScalarValue))
			{
				OutError = FString::Printf(TEXT("Slot '%s' material '%s' is missing scalar parameter '%s'"),
					*Entry.SlotName.ToString(), *GetPathNameSafe(Entry.Material), *Parameter.ToString());
				return false;
			}
		}
	}
	if (bEnableOutline)
	{
		float Value;
		if (!IsUnlitSurface(OutlineMaterial) || !FMath::IsFinite(OutlineWidth) || OutlineWidth < 0.0f
			|| !OutlineMaterial->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("OutlineWidth")), Value)
			|| !OutlineMaterial->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("OutlineOpacity")), Value))
		{
			OutError = FString::Printf(TEXT("Outline material '%s' requires an Unlit surface with OutlineWidth/OutlineOpacity scalars and a finite nonnegative width"),
				*GetPathNameSafe(OutlineMaterial));
			return false;
		}
	}
	OutError.Reset();
	return true;
}

void UGGYGOCharacterRenderComponent::FailRendering(const FString& Error)
{
	UE_LOG(LogGGYGOCharacterRendering, Error, TEXT("[Character Rendering] %s owner=%s: %s. Rendering stopped; correct configuration and call RefreshMaterials."),
		*GetPathName(), *GetPathNameSafe(GetOwner()), *Error);
	ReleaseMaterialResources(EGGYGOCharacterLightingReadiness::Invalid, Error);
}

bool UGGYGOCharacterRenderComponent::HasCurrentMesh() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!IsValid(TargetMesh) || !BoundMeshAsset.IsValid() || !Character
		|| Character->GetMesh() != TargetMesh || TargetMesh->GetSkeletalMeshAsset() != BoundMeshAsset.Get()) return false;
	const TArray<FName> Slots = TargetMesh->GetMaterialSlotNames();
	for (const FGGYGOCharacterRenderBinding& Binding : MaterialBindings)
	{
		if (!Slots.IsValidIndex(Binding.SlotIndex) || Slots[Binding.SlotIndex] != Binding.SlotName
			|| TargetMesh->GetMaterial(Binding.SlotIndex) != Binding.InstalledMaterial) return false;
	}
	return true;
}

void UGGYGOCharacterRenderComponent::ReleaseMaterials()
{
	ReleaseMaterialResources(EGGYGOCharacterLightingReadiness::Invalid, TEXT("原角色表现租用期已归还"));
}

void UGGYGOCharacterRenderComponent::ReleaseMaterialResources(
	EGGYGOCharacterLightingReadiness Readiness, const FString& Reason, bool bNotify)
{
	if (bReleasingMaterials) return;
	bReleasingMaterials = true;
	auto RetiredTargets = MoveTemp(SceneMaterialTargets);
	SetComponentTickEnabled(false);
	ShadowCapture.Reset();
	ReleasePreviewLighting();
	if (IsValid(TargetMesh))
	{
		const bool bSameAsset = BoundMeshAsset.IsValid() && TargetMesh->GetSkeletalMeshAsset() == BoundMeshAsset.Get();
		const TArray<FName> Slots = TargetMesh->GetMaterialSlotNames();
		for (const FGGYGOCharacterRenderBinding& Binding : MaterialBindings)
		{
			// Never restore over another owner's override, including an explicitly cleared slot.
			if (!Binding.InstalledMaterial || !TargetMesh->OverrideMaterials.IsValidIndex(Binding.SlotIndex)
				|| TargetMesh->OverrideMaterials[Binding.SlotIndex] != Binding.InstalledMaterial) continue;
			const bool bSameSlot = bSameAsset && Slots.IsValidIndex(Binding.SlotIndex)
				&& Slots[Binding.SlotIndex] == Binding.SlotName;
			// Late notification after a mesh swap only removes our orphaned override.
			// Null means the NEW mesh's default; never copy the old mesh material across.
			TargetMesh->SetMaterial(Binding.SlotIndex, bSameSlot ? Binding.OriginalOverride.Get() : nullptr);
		}
	}
	ReleaseOutline();
	DynamicMaterials.Reset();
	MaterialBindings.Reset();
	BoundMeshAsset.Reset();
	ResolvedKeyLight.Reset();
	TargetMesh = nullptr;
	bRetryLighting = Readiness == EGGYGOCharacterLightingReadiness::WaitingForRegistration
		|| Readiness == EGGYGOCharacterLightingReadiness::SourceFailed;
	bUsesAdditionalInputs = false;
	bReleasingMaterials = false;
	const uint64 OriginalNotice = SceneLightingNotificationSerial;
	const FString NotificationReason = Reason;
	// 外部回调可结束FX实例并注销旧handle；全部自有状态已先归还，返栈不再清理后继。
	for (auto& Pair : RetiredTargets)
		Pair.Value.OnInvalidated.ExecuteIfBound(Pair.Key, Readiness, NotificationReason);
	// 失效回调可已重入创建/应用新租用期；旧释放不再发布其已过期状态。
	if (bNotify && IsValid(this) && SceneLightingNotificationSerial == OriginalNotice
		&& DynamicMaterials.IsEmpty() && !PreviewMesh.IsValid())
		PublishSceneLightingReadiness(Readiness, NotificationReason);
}

void UGGYGOCharacterRenderComponent::PublishSceneLightingReadiness(
	EGGYGOCharacterLightingReadiness Readiness, const FString& Reason)
{
	if (LastSceneLightingReadiness.IsSet() && LastSceneLightingReadiness.GetValue() == Readiness
		&& LastSceneLightingReason == Reason) return;
	const FString NotificationReason = Reason;
	LastSceneLightingReadiness = Readiness;
	LastSceneLightingReason = NotificationReason;
	checkf(SceneLightingNotificationSerial != MAX_uint64, TEXT("场景光照通知序列已耗尽"));
	++SceneLightingNotificationSerial;
	SceneLightingReadinessChanged.Broadcast(Readiness, NotificationReason);
}

void UGGYGOCharacterRenderComponent::RefreshMaterials()
{
	const bool bHadResources = !DynamicMaterials.IsEmpty() || PreviewMesh.IsValid() || bRetryLighting;
	const bool bRetryingLighting = bRetryLighting;
	ReleaseMaterialResources(EGGYGOCharacterLightingReadiness::Invalid, TEXT("原角色表现配置刷新"),
		bHadResources && !bRetryingLighting);
	if (!IsValid(this) || !IsRegistered()) return;
	// 释放通知可能重入并安装后继；外层不再重新创建/覆盖该租用期。
	if (!DynamicMaterials.IsEmpty() || PreviewMesh.IsValid() || bRetryLighting) return;
	if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer) return;
	if (UsesEditorSceneLighting() && !UsesEditorMaterialPreview())
	{
		RefreshEditorLighting();
		return;
	}
	if ((!HasBegunPlay() && !UsesEditorMaterialPreview()) || !IsActive()) return;
	if (MaterialSlots.IsEmpty())
	{ PublishSceneLightingReadiness(EGGYGOCharacterLightingReadiness::Invalid, TEXT("原角色未配置表面材质槽")); return; }
#if WITH_EDITOR
	if (UsesEditorMaterialPreview() && (bEditorPreviewSuspendedForPIE || GIsSavingPackage || GIsTransacting)) return;
	RestoreSerializedPreviewOverrides();
#endif
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	TargetMesh = Character ? Character->GetMesh() : nullptr;
	FString Error;
	if (UsesEditorMaterialPreview() && TargetMesh && !TargetMesh->IsRegistered())
	{ HandleLightingUnavailable(EGGYGOCharacterLightingReadiness::WaitingForRegistration, TEXT("原角色Mesh等待真实注册")); return; }
	// Validate the complete configuration before installing the first override.
	if (!ValidateLightSettings(Error) || !ValidateMaterialBindings(TargetMesh, Error))
	{
		FailRendering(Error);
		return;
	}
	if (!ValidateAdditionalMaterialContract(bUsesAdditionalInputs, Error))
	{
		FailRendering(Error);
		return;
	}
	if (KeyLightSelection == EGGYGOCharacterKeyLightSelection::Explicit
		&& (!KeyLight->GetLightComponent() || !KeyLight->GetLightComponent()->IsRegistered()))
	{ HandleLightingUnavailable(EGGYGOCharacterLightingReadiness::WaitingForRegistration, TEXT("原角色主灯等待真实注册")); return; }
	if (UsesEditorMaterialPreview())
	{
		for (const FGGYGOCharacterRenderSlot& Entry : MaterialSlots)
		{
			if (TargetMesh->GetMaterial(TargetMesh->GetMaterialIndex(Entry.SlotName)) != Entry.Material)
			{
				FailRendering(TEXT("编辑器实际槽已由用户修改；保持该覆盖，不安装临时MID"));
				return;
			}
		}
	}
	if (bUsesAdditionalInputs)
	{
		const TWeakObjectPtr<USkeletalMeshComponent> OriginalMesh = TargetMesh.Get();
		const TWeakObjectPtr<USkeletalMesh> OriginalAsset = TargetMesh->GetSkeletalMeshAsset();
		FGGYGOCharacterAdditionalLightingSample Sample;
		const auto Readiness = ReadAdditionalLighting(AdditionalLighting, Sample, Error);
		// Getter的原FX首错可同步结束实例并通知外部；旧初始化不处理已变更的角色或后继租用。
		if (!IsValid(this) || !IsRegistered() || !IsActive() || !OriginalMesh.IsValid() || !OriginalAsset.IsValid()
			|| TargetMesh.Get() != OriginalMesh.Get() || TargetMesh->GetSkeletalMeshAsset() != OriginalAsset.Get()
			|| !DynamicMaterials.IsEmpty() || PreviewMesh.IsValid() || bRetryLighting) return;
		if (Readiness != EGGYGOCharacterLightingReadiness::Ready) { HandleLightingUnavailable(Readiness, Error); return; }
	}
	BoundMeshAsset = TargetMesh->GetSkeletalMeshAsset();
	const TArray<FName> Slots = TargetMesh->GetMaterialSlotNames();
	for (const FGGYGOCharacterRenderSlot& Entry : MaterialSlots)
	{
		const int32 Index = Slots.IndexOfByKey(Entry.SlotName);
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Entry.Material, this);
		if (!Instance)
		{
			FailRendering(FString::Printf(TEXT("Cannot create MID for slot '%s', material '%s'"),
				*Entry.SlotName.ToString(), *GetPathNameSafe(Entry.Material)));
			return;
		}
		FGGYGOCharacterRenderBinding& Binding = MaterialBindings.AddDefaulted_GetRef();
		Binding.SlotIndex = Index;
		Binding.SlotName = Slots[Index];
		Binding.OriginalOverride = TargetMesh->OverrideMaterials.IsValidIndex(Index)
			? TargetMesh->OverrideMaterials[Index].Get() : nullptr;
		Binding.InstalledMaterial = Instance;
		Instance->SetFlags(RF_Transient | RF_DuplicateTransient);
#if WITH_EDITOR
		if (UsesEditorMaterialPreview())
		{
			auto* Data = NewObject<UGGYGOCharacterPreviewMaterialData>(Instance, NAME_None, RF_Transient);
			Data->OwnerActor = GetOwner();
			Data->ComponentName = GetFName();
			Data->MeshAsset = TargetMesh->GetSkeletalMeshAsset();
			Data->SlotIndex = Index;
			Data->SlotName = Binding.SlotName;
			Data->OriginalOverride = Binding.OriginalOverride;
			Instance->AddAssetUserData(Data);
		}
#endif
		TargetMesh->SetMaterial(Index, Instance);
		DynamicMaterials.Add(Instance);
	}
	if (!UpdateOutline(TargetMesh)) return;
	SetComponentTickInterval(LightingUpdateInterval);
	SetComponentTickEnabled(!DynamicMaterials.IsEmpty());
	UpdateLighting();
}

void UGGYGOCharacterRenderComponent::ReleaseOutline()
{
	if (IsValid(OutlineMesh)) OutlineMesh->DestroyComponent();
	OutlineMesh = nullptr;
}

bool UGGYGOCharacterRenderComponent::UpdateOutline(USkeletalMeshComponent* Mesh)
{
	if (!bEnableOutline) return true;
	OutlineMesh = NewObject<USkeletalMeshComponent>(GetOwner(), NAME_None, RF_Transient | RF_DuplicateTransient);
	OutlineMesh->SetupAttachment(Mesh);
	OutlineMesh->SetSkeletalMeshAsset(Mesh->GetSkeletalMeshAsset());
	OutlineMesh->SetLeaderPoseComponent(Mesh);
	if (Mesh == PreviewMesh.Get())
	{
		OutlineMesh->SetVisibility(Mesh->IsVisible());
		OutlineMesh->SetHiddenInGame(Mesh->bHiddenInGame);
	}
	OutlineMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutlineMesh->SetCastShadow(false);
	OutlineMesh->SetReceivesDecals(false);
	const TArray<FName> Slots = Mesh->GetMaterialSlotNames();
	for (int32 Index = 0; Index < OutlineMesh->GetNumMaterials(); ++Index)
	{
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(OutlineMaterial, this);
		if (!Instance)
		{
			FailRendering(FString::Printf(TEXT("Cannot create outline MID from '%s' for slot %d"), *GetPathNameSafe(OutlineMaterial), Index));
			return false;
		}
		Instance->SetScalarParameterValue(TEXT("OutlineWidth"), OutlineWidth);
		const FGGYGOCharacterRenderSlot* Slot = MaterialSlots.FindByPredicate(
			[&](const FGGYGOCharacterRenderSlot& Entry) { return Entry.SlotName == Slots[Index]; });
		const UMaterialInstanceDynamic* Surface = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(Index));
		const bool bBound = Mesh == PreviewMesh.Get()
			? Slot && Mesh->GetMaterial(Index) == Slot->Material
			: DynamicMaterials.Contains(Surface);
		Instance->SetScalarParameterValue(TEXT("OutlineOpacity"), bBound ? 1.0f : 0.0f);
		OutlineMesh->SetMaterial(Index, Instance);
	}
	OutlineMesh->RegisterComponent();
	return true;
}

void UGGYGOCharacterRenderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
#if WITH_EDITOR
	if (bEditorPreviewSuspendedForPIE || GIsSavingPackage || GIsTransacting) return;
	if (bResumeEditorPreview)
	{
		bResumeEditorPreview = false;
		if (UsesEditorSceneLighting()) RefreshMaterials();
		return;
	}
#endif
	if (bRetryLighting) { RefreshMaterials(); return; }
	if (UsesEditorSceneLighting())
	{
		if (!UsesEditorMaterialPreview()) { RefreshEditorLighting(); return; }
		if (!HasCurrentMesh()) { ReleaseMaterials(); return; }
		UpdateLighting();
		if (!IsValid(this) || !IsRegistered()) return;
		if (OutlineMesh && TargetMesh)
		{
			OutlineMesh->SetVisibility(TargetMesh->IsVisible());
			OutlineMesh->SetHiddenInGame(TargetMesh->bHiddenInGame);
		}
		return;
	}
	if (PreviewMesh.IsValid()) ReleasePreviewLighting();
	if (!HasCurrentMesh())
	{
		ReleaseMaterials(); // The caller applies the new cosmetic configuration explicitly.
		return;
	}
	UpdateLighting();
}

bool UGGYGOCharacterRenderComponent::IsEligibleKeyLight(const ADirectionalLight* Light) const
{
	if (!IsValid(Light) || Light->IsActorBeingDestroyed() || Light->GetWorld() != GetWorld() || Light->IsHidden()) return false;
	const ULightComponent* Component = Light->GetLightComponent();
	return IsValid(Component) && Component->IsRegistered() && Component->IsVisible()
		&& Component->bAffectsWorld && Component->Intensity > 0.0f;
}

bool UGGYGOCharacterRenderComponent::UsesEditorMaterialPreview() const
{
	return UsesEditorSceneLighting() && EditorPreviewTransport == EGGYGOCharacterPreviewTransport::TransientMaterials;
}

void UGGYGOCharacterRenderComponent::HandleLightingUnavailable(
	EGGYGOCharacterLightingReadiness Readiness, const FString& Reason)
{
	if (Readiness != EGGYGOCharacterLightingReadiness::WaitingForRegistration
		&& Readiness != EGGYGOCharacterLightingReadiness::SourceFailed)
	{ FailRendering(Reason); return; }
	const FString UnavailableReason = Reason;
	if (Readiness == EGGYGOCharacterLightingReadiness::SourceFailed
		&& (!LastSceneLightingReadiness.IsSet() || LastSceneLightingReadiness.GetValue() != Readiness
			|| LastSceneLightingReason != UnavailableReason))
		UE_LOG(LogGGYGOCharacterRendering, Error, TEXT("[Character Rendering] %s owner=%s: %s. 外部灯源本次采样失败；归还本次表现，原Tick后续重采。"),
			*GetPathName(), *GetPathNameSafe(GetOwner()), *UnavailableReason);
	ReleaseMaterialResources(Readiness, UnavailableReason);
	if (!IsValid(this) || !IsRegistered() || !IsActive()) return;
	if (!DynamicMaterials.IsEmpty() || PreviewMesh.IsValid()) return;
	if (!bRetryLighting) return;
	SetComponentTickInterval(LightingUpdateInterval);
	SetComponentTickEnabled(IsActive());
}

EGGYGOCharacterLightingReadiness UGGYGOCharacterRenderComponent::ReadAdditionalLighting(
	const FGGYGOCharacterAdditionalLighting& Configuration, FGGYGOCharacterAdditionalLightingSample& OutSample,
	FString& OutError) const
{
	OutSample = {};
	OutError.Reset();
	if (!GetWorld() || !Configuration.bConfigured || !FMath::IsFinite(Configuration.GlobalIntensity)
		|| Configuration.GlobalIntensity < 0 || Configuration.Lights.Num() > 4)
	{
		OutError = TEXT("附加灯必须显式配置有限非负global和0..4个选中来源；不自动选灯或填零");
		return EGGYGOCharacterLightingReadiness::Invalid;
	}
	FGGYGOCharacterAdditionalLightingSample Sample;
	Sample.GlobalIntensity = Configuration.GlobalIntensity;
	for (int32 Index = 0; Index < Configuration.Lights.Num(); ++Index)
	{
		const auto& Input = Configuration.Lights[Index];
		const ALight* Source = Input.SourceLight;
		if (!Input.bConfigured || !IsValid(Source) || Source->IsActorBeingDestroyed() || Source->GetWorld() != GetWorld()
			|| !FMath::IsFinite(Input.IntensityScale) || Input.IntensityScale < 0
			|| (Input.DirectionSign != -1 && Input.DirectionSign != 1)
			|| !FitsMaterialFloat(Input.Attenuation.X) || !FitsMaterialFloat(Input.Attenuation.Y)
			|| !FitsMaterialFloat(Input.Attenuation.Z) || !FitsMaterialFloat(Input.Attenuation.W))
		{
			OutError = FString::Printf(TEXT("附加灯[%d]缺同World原来源/明确系数或方向符号: %s"), Index, *GetPathNameSafe(Source));
			return EGGYGOCharacterLightingReadiness::Invalid;
		}
		const UPointLightComponent* Light = Cast<UPointLightComponent>(Source->GetLightComponent());
		if (!IsValid(Light))
		{
			OutError = FString::Printf(TEXT("附加灯[%d]须为明确Point/Spot局部灯，不将其它光型猜成源点光"), Index);
			return EGGYGOCharacterLightingReadiness::Invalid;
		}
		if (!Light->IsRegistered())
		{
			OutError = FString::Printf(TEXT("附加灯[%d]等待真实注册: %s"), Index, *GetPathNameSafe(Source));
			return EGGYGOCharacterLightingReadiness::WaitingForRegistration;
		}
		const FVector Position = Light->GetComponentLocation() * 0.01;
		const FVector Direction = Light->GetForwardVector() * Input.DirectionSign;
		const float Intensity = Light->Intensity;
		const bool bEnabled = !Source->IsHidden() && Light->IsVisible() && Light->bAffectsWorld;
		const FLinearColor Color = Light->GetLightColor() * (bEnabled ? Intensity * Input.IntensityScale : 0.0f);
		if (!FitsMaterialFloat(Position.X) || !FitsMaterialFloat(Position.Y) || !FitsMaterialFloat(Position.Z)
			|| Direction.ContainsNaN() || !FMath::IsFinite(Intensity) || Intensity < 0 || !IsFiniteColor(Color))
		{
			OutError = FString::Printf(TEXT("附加灯[%d]的实际位置/方向/强度/色值无效: %s"), Index, *GetPathNameSafe(Source));
			return EGGYGOCharacterLightingReadiness::Invalid;
		}
		Sample.PositionsMeters.Add(Position);
		Sample.Directions.Add(Direction);
		Sample.Attenuations.Add(Input.Attenuation);
		Sample.Colors.Add(Color);
	}
	if (!UGGYGOCharacterLightSources::ValidateSelection(FXLightSelection, OutError))
		return EGGYGOCharacterLightingReadiness::Invalid;
	if (FXLightSelection.Mode != EGGYGOCharacterFXLightMode::Disabled
		&& Sample.PositionsMeters.Num() < 4 && FXLightSelection.IntensityScale > 0)
	{
		const auto* Character = Cast<ACharacter>(GetOwner());
		const auto* Mesh = Character ? Character->GetMesh() : nullptr;
		auto* Sources = GetWorld()->GetSubsystem<UGGYGOCharacterLightSources>();
		if (!IsValid(Mesh) || !Sources)
		{ OutError = TEXT("FX选灯缺实际角色Mesh/同World来源登记"); return EGGYGOCharacterLightingReadiness::Invalid; }
		TArray<FGGYGOCharacterSelectedPointLight> Selected;
		const auto Readiness = Sources->SelectLights(Mesh->Bounds.Origin, 4 - Sample.PositionsMeters.Num(),
			FXLightSelection, Selected, OutError);
		if (Readiness == EGGYGOCharacterLightSourceReadiness::WaitingForSimulation)
			return EGGYGOCharacterLightingReadiness::WaitingForRegistration;
		if (Readiness == EGGYGOCharacterLightSourceReadiness::SourceFailed)
			return EGGYGOCharacterLightingReadiness::SourceFailed;
		if (Readiness != EGGYGOCharacterLightSourceReadiness::Ready)
			return EGGYGOCharacterLightingReadiness::Invalid;
		for (const auto& Point : Selected)
		{
			Sample.PositionsMeters.Add(Point.Light.PositionWorldCm * 0.01);
			Sample.Directions.Add(FVector::ZeroVector); // Point映射z=0,w=1，无Spot角度衰减。
			Sample.Attenuations.Add(Point.Attenuation);
			Sample.Colors.Add(Point.Light.Color);
		}
	}
	OutSample = MoveTemp(Sample);
	return EGGYGOCharacterLightingReadiness::Ready;
}

bool UGGYGOCharacterRenderComponent::ValidateAdditionalMaterialContract(bool& OutUsesAdditional, FString& OutError) const
{
	TOptional<bool> Contract;
	for (const auto& Slot : MaterialSlots)
	{
		if (!IsValid(Slot.Material)) { OutError = TEXT("附加灯材质接口缺少实际材质"); return false; }
		TArray<FMaterialParameterInfo> Vectors, Scalars;
		TArray<FGuid> VectorIds, ScalarIds;
		Slot.Material->GetAllVectorParameterInfo(Vectors, VectorIds);
		Slot.Material->GetAllScalarParameterInfo(Scalars, ScalarIds);
		int32 Found = 0;
		for (int32 Index = 0; Index < 4; ++Index)
			for (const TCHAR* Family : {TEXT("Position"), TEXT("Direction"), TEXT("Attenuation"), TEXT("Color")})
				Found += Vectors.ContainsByPredicate([&](const FMaterialParameterInfo& Info) { return Info.Association == EMaterialParameterAssociation::GlobalParameter && Info.Name == AdditionalParameter(Family, Index); }) ? 1 : 0;
		for (const FName Name : {AdditionalCountParameter, AdditionalGlobalParameter, PreviewTransportParameter})
			Found += Scalars.ContainsByPredicate([&](const FMaterialParameterInfo& Info) { return Info.Association == EMaterialParameterAssociation::GlobalParameter && Info.Name == Name; }) ? 1 : 0;
		if ((Found != 0 && Found != 19) || (Contract.IsSet() && Contract.GetValue() != (Found == 19)))
		{
			OutError = FString::Printf(TEXT("材质%s的附加灯/CPD选择接口不完整或各槽混用了不同接口"), *GetPathNameSafe(Slot.Material));
			return false;
		}
		Contract = Found == 19;
	}
	OutUsesAdditional = Contract.Get(false);
	if (FXLightSelection.Mode != EGGYGOCharacterFXLightMode::Disabled && !OutUsesAdditional)
	{ OutError = TEXT("FX选灯模式需要完整四灯材质接口"); return false; }
	if (UsesEditorMaterialPreview() && !OutUsesAdditional)
	{
		OutError = TEXT("完整MID编辑器模式需要已制作的附加灯/UseScenePreviewCPD材质接口");
		return false;
	}
	OutError.Reset();
	return true;
}

ADirectionalLight* UGGYGOCharacterRenderComponent::ResolveKeyLight()
{
	if (KeyLightSelection == EGGYGOCharacterKeyLightSelection::Explicit)
	{
		ResolvedKeyLight.Reset();
		return KeyLight.Get(); // 已校验的明确来源即使关闭也保实际方向；强度在原采样中归零。
	}
	if (KeyLight != nullptr)
	{
		ResolvedKeyLight.Reset();
		return IsEligibleKeyLight(KeyLight) ? KeyLight.Get() : nullptr;
	}
	if (IsEligibleKeyLight(ResolvedKeyLight.Get())) return ResolvedKeyLight.Get();
	ResolvedKeyLight.Reset();
	if (!GetWorld()) return nullptr;
	ADirectionalLight* Candidate = nullptr;
	for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
	{
		if (IsEligibleKeyLight(*It) && (!Candidate || It->GetPathName() < Candidate->GetPathName())) Candidate = *It;
	}
	ResolvedKeyLight = Candidate;
	return Candidate;
}

EGGYGOCharacterLightingReadiness UGGYGOCharacterRenderComponent::RegisterSceneMaterialTargets(
	UObject* SourceOwner, TConstArrayView<UMaterialInstanceDynamic*> Materials,
	FSceneMaterialsInvalidated OnInvalidated, uint64& OutHandle, FString& OutError)
{
	check(IsInGameThread());
	OutHandle = 0;
	OutError.Reset();
	if (!IsRegistered() || !IsActive() || bReleasingMaterials || !IsValid(SourceOwner)
		|| !GetWorld() || GetWorld()->bIsTearingDown || SourceOwner->GetWorld() != GetWorld()
		|| GetWorld()->GetNetMode() == NM_DedicatedServer || Materials.IsEmpty() || !OnInvalidated.IsBound())
	{ OutError = TEXT("外部材质登记缺同World活动Owner/MID/弱失效回调"); return EGGYGOCharacterLightingReadiness::Invalid; }
	if (!HasBegunPlay() && GetWorld()->IsGameWorld())
	{ OutError = TEXT("原角色表现等待BeginPlay"); return EGGYGOCharacterLightingReadiness::WaitingForRegistration; }
	if (bRetryLighting)
	{ OutError = TEXT("原角色等待下一次真实光照采样"); return EGGYGOCharacterLightingReadiness::WaitingForRegistration; }
	if (!HasCurrentMesh() || DynamicMaterials.IsEmpty() || NextSceneMaterialHandle == MAX_uint64)
	{ OutError = TEXT("原角色没有活动材质租用期或登记handle已耗尽"); return EGGYGOCharacterLightingReadiness::Invalid; }
	FSceneMaterialTargets Target;
	Target.SourceOwner = SourceOwner;
	Target.OnInvalidated = MoveTemp(OnInvalidated);
	TSet<const UMaterialInstanceDynamic*> Unique;
	for (UMaterialInstanceDynamic* Material : Materials)
	{
		if (!IsValid(Material) || !IsUnlitSurface(Material) || Unique.Contains(Material) || DynamicMaterials.Contains(Material))
		{ OutError = TEXT("外部MID无效、非Unlit、重复或属于本组件"); return EGGYGOCharacterLightingReadiness::Invalid; }
		for (const auto& Pair : SceneMaterialTargets)
			if (Pair.Value.Materials.Contains(Material))
			{ OutError = TEXT("外部MID已有场景灯写入登记"); return EGGYGOCharacterLightingReadiness::Invalid; }
		FLinearColor Vector;
		float Scalar;
		for (const FName Name : {LightDirectionParameter, LightColorParameter})
			if (!Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Name), Vector))
			{ OutError = FString::Printf(TEXT("外部MID缺主光vector %s"), *Name.ToString()); return EGGYGOCharacterLightingReadiness::Invalid; }
		for (const FName Name : {LightStrengthParameter, LightVisibilityParameter, AdditionalCountParameter, AdditionalGlobalParameter})
			if (!Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Scalar))
			{ OutError = FString::Printf(TEXT("外部MID缺灯scalar %s"), *Name.ToString()); return EGGYGOCharacterLightingReadiness::Invalid; }
		for (int32 Index = 0; Index < 4; ++Index)
			for (const TCHAR* Family : {TEXT("Position"), TEXT("Direction"), TEXT("Attenuation"), TEXT("Color")})
				if (!Material->GetVectorParameterValue(FHashedMaterialParameterInfo(AdditionalParameter(Family, Index)), Vector))
				{ OutError = TEXT("外部MID缺完整四灯vector接口"); return EGGYGOCharacterLightingReadiness::Invalid; }
		Unique.Add(Material);
		Target.Materials.Add(Material);
	}
	FGGYGOCharacterAdditionalLightingSample Additional;
	const TWeakObjectPtr<UMaterialInstanceDynamic> OriginalMID = DynamicMaterials[0].Get();
	const auto AdditionalReadiness = ReadAdditionalLighting(AdditionalLighting, Additional, OutError);
	if (!IsValid(this) || !IsRegistered() || !HasCurrentMesh() || DynamicMaterials.IsEmpty() || !OriginalMID.IsValid() || DynamicMaterials[0].Get() != OriginalMID.Get())
	{ OutError = TEXT("采样期间原角色材质租用期已改变"); return EGGYGOCharacterLightingReadiness::Invalid; }
	if (AdditionalReadiness != EGGYGOCharacterLightingReadiness::Ready)
	{
		HandleLightingUnavailable(AdditionalReadiness, OutError);
		return AdditionalReadiness;
	}
	FVector Direction;
	FLinearColor Color;
	float Strength, Visibility;
	if (!SampleLighting(Direction, Color, Strength, Visibility))
	{
		OutError = TEXT("原主光采样尚未就绪或已拒绝配置");
		return bRetryLighting ? EGGYGOCharacterLightingReadiness::WaitingForRegistration : EGGYGOCharacterLightingReadiness::Invalid;
	}
	if (!Target.SourceOwner.IsValid() || Target.SourceOwner->GetWorld() != GetWorld())
	{ OutError = TEXT("采样期间原外部材质Owner已归还"); return EGGYGOCharacterLightingReadiness::Invalid; }
	for (const auto& Material : Target.Materials)
	{
		if (!Material.IsValid() || !Target.OnInvalidated.IsBound())
		{ OutError = TEXT("采样期间原外部MID/弱通知端点已失效"); return EGGYGOCharacterLightingReadiness::Invalid; }
		for (const auto& Pair : SceneMaterialTargets)
			if (Pair.Value.Materials.Contains(Material))
			{ OutError = TEXT("采样回调已为该MID建立其它登记"); return EGGYGOCharacterLightingReadiness::Invalid; }
	}
	// 原Sampler成功后没有用户回调；首次提交前已取得真实同次主光/四灯，不发布预览初值。
	// Getter可能结束FX并释放调用方数组；返栈只消费预检阶段保存的弱端点副本。
	for (const auto& Material : Target.Materials)
		WriteSceneMaterialLighting(*Material.Get(), Direction, Color, Strength, Visibility, &Additional);
	OutHandle = NextSceneMaterialHandle++;
	SceneMaterialTargets.Add(OutHandle, MoveTemp(Target));
	return EGGYGOCharacterLightingReadiness::Ready;
}

void UGGYGOCharacterRenderComponent::UnregisterSceneMaterialTargets(uint64 Handle)
{
	check(IsInGameThread());
	SceneMaterialTargets.Remove(Handle);
}

void UGGYGOCharacterRenderComponent::WriteSceneMaterialLighting(UMaterialInstanceDynamic& Material,
	const FVector& Direction, const FLinearColor& Color, float Strength, float Visibility,
	const FGGYGOCharacterAdditionalLightingSample* Additional) const
{
	Material.SetVectorParameterValue(LightDirectionParameter, FLinearColor(Direction.X, Direction.Y, Direction.Z));
	Material.SetVectorParameterValue(LightColorParameter, Color);
	Material.SetScalarParameterValue(LightStrengthParameter, Strength);
	Material.SetScalarParameterValue(LightVisibilityParameter, Visibility);
	if (!Additional) return;
	float PreviewValue;
	if (Material.GetScalarParameterValue(FHashedMaterialParameterInfo(PreviewTransportParameter), PreviewValue))
		Material.SetScalarParameterValue(PreviewTransportParameter, 0);
	Material.SetScalarParameterValue(AdditionalCountParameter, Additional->PositionsMeters.Num());
	Material.SetScalarParameterValue(AdditionalGlobalParameter, Additional->GlobalIntensity);
	for (int32 Index = 0; Index < Additional->PositionsMeters.Num(); ++Index)
	{
		const FVector& P = Additional->PositionsMeters[Index];
		const FVector& D = Additional->Directions[Index];
		const FVector4& A = Additional->Attenuations[Index];
		Material.SetVectorParameterValue(AdditionalParameter(TEXT("Position"), Index), FLinearColor(P.X, P.Y, P.Z, 0));
		Material.SetVectorParameterValue(AdditionalParameter(TEXT("Direction"), Index), FLinearColor(D.X, D.Y, D.Z, 0));
		Material.SetVectorParameterValue(AdditionalParameter(TEXT("Attenuation"), Index), FLinearColor(A.X, A.Y, A.Z, A.W));
		Material.SetVectorParameterValue(AdditionalParameter(TEXT("Color"), Index), Additional->Colors[Index]);
	}
}

void UGGYGOCharacterRenderComponent::UpdateLighting()
{
	if (DynamicMaterials.IsEmpty() || !GetWorld()) return;
	const TWeakObjectPtr<UMaterialInstanceDynamic> OriginalMID = DynamicMaterials[0].Get();
	TArray<TPair<uint64, FSceneMaterialTargets>> RetiredTargets;
	ON_SCOPE_EXIT
	{
		// 同一Update返回时再通知；回调后不再读写本次角色状态或已摘除的旧登记。
		for (auto& Pair : RetiredTargets)
			Pair.Value.OnInvalidated.ExecuteIfBound(Pair.Key, EGGYGOCharacterLightingReadiness::Invalid,
				TEXT("原外部Owner/MID已失效或离开同World"));
	};
	for (auto It = SceneMaterialTargets.CreateIterator(); It; ++It)
	{
		bool bValid = It.Value().SourceOwner.IsValid() && It.Value().SourceOwner->GetWorld() == GetWorld();
		for (const auto& Material : It.Value().Materials) bValid &= Material.IsValid();
		if (!bValid)
		{
			RetiredTargets.Emplace(It.Key(), MoveTemp(It.Value()));
			It.RemoveCurrent();
		}
	}
	FVector ToLight;
	FLinearColor LightColor;
	float LightStrength, Visibility;
	if (!SampleLighting(ToLight, LightColor, LightStrength, Visibility)) return;
	FGGYGOCharacterAdditionalLightingSample Additional;
	const bool bReadAdditional = bUsesAdditionalInputs || !SceneMaterialTargets.IsEmpty();
	if (bReadAdditional)
	{
		FString Error;
		const auto Readiness = ReadAdditionalLighting(AdditionalLighting, Additional, Error);
		if (!IsValid(this) || !IsRegistered() || !HasCurrentMesh() || DynamicMaterials.IsEmpty() || !OriginalMID.IsValid() || DynamicMaterials[0].Get() != OriginalMID.Get()) return;
		if (Readiness != EGGYGOCharacterLightingReadiness::Ready) { HandleLightingUnavailable(Readiness, Error); return; }
	}
	for (UMaterialInstanceDynamic* Instance : DynamicMaterials)
		if (Instance) WriteSceneMaterialLighting(*Instance, ToLight, LightColor, LightStrength, Visibility,
			bUsesAdditionalInputs ? &Additional : nullptr);
	for (const auto& Pair : SceneMaterialTargets)
		for (const auto& Material : Pair.Value.Materials)
			WriteSceneMaterialLighting(*Material.Get(), ToLight, LightColor, LightStrength, Visibility, &Additional);
	if (ShadowSettings.Mode == EGGYGOCharacterShadowMode::DepthCaptures)
	{
		if (!HasCurrentMesh() || !IsValid(KeyLight))
		{ FailRendering(TEXT("逐像素阴影需要原Mesh及明确实际主灯")); return; }
		if (!ShadowCapture) { ShadowCapture = MakeShared<FGGYGOCharacterShadowCapture>(); }
		const auto OriginalCapture = ShadowCapture;
		TArray<UMaterialInstanceDynamic*> Materials;
		for (const auto& Material : DynamicMaterials) { Materials.Add(Material.Get()); }
		FString Error;
		const bool bQueued = OriginalCapture->Update(*this, *TargetMesh, *KeyLight, ShadowSettings, Materials, Error);
		if (!IsValid(this) || !IsRegistered() || ShadowCapture != OriginalCapture || !HasCurrentMesh()) return;
		if (!bQueued) { FailRendering(Error); return; }
	}
	else
	{
		ShadowCapture.Reset();
		// CharacterTrace是明确的正常模式；父MI的捕获开关不能使私有MID误采尚无生产者的贴图。
		for (UMaterialInstanceDynamic* Instance : DynamicMaterials)
			if (IsValid(Instance)) { Instance->SetScalarParameterValue(TEXT("UseCharacterShadowCaptures"), 0); }
	}
	PublishSceneLightingReadiness(EGGYGOCharacterLightingReadiness::Ready, FString());
}

bool UGGYGOCharacterRenderComponent::SampleLighting(FVector& ToLight, FLinearColor& LightColor, float& LightStrength, float& Visibility)
{
	FString Error;
	// Blueprint may edit light-response values while active. Invalid edits stop
	// this owned rendering lifecycle once instead of silently selecting a value.
	if (!ValidateLightSettings(Error))
	{
		FailRendering(Error);
		return false;
	}
	ADirectionalLight* Light = ResolveKeyLight();
	if (KeyLightSelection == EGGYGOCharacterKeyLightSelection::Explicit && !KeyLight->GetLightComponent()->IsRegistered())
	{ HandleLightingUnavailable(EGGYGOCharacterLightingReadiness::WaitingForRegistration, TEXT("原角色主灯等待真实注册")); return false; }
	ToLight = Light ? -Light->GetLightComponent()->GetForwardVector() : FVector::UpVector;
	LightColor = Light ? Light->GetLightColor() * KeyLightTint : FLinearColor::Black;
	LightStrength = 0.0f;
	if (Light)
	{
		const float Intensity = Light->GetLightComponent()->Intensity;
		if (!FMath::IsFinite(Intensity) || Intensity < 0 || !IsFiniteColor(LightColor) || ToLight.ContainsNaN())
		{
			FailRendering(FString::Printf(TEXT("Key light '%s' has nonfinite intensity, color or direction"), *GetPathNameSafe(Light)));
			return false;
		}
		// Double intermediates preserve the configured cap for large finite lux
		// values without overflowing a float before the cap is applied.
		const double Factor = KeyLightIntensityResponse == 0.0f ? 1.0
			: FMath::Pow(static_cast<double>(Intensity) / ReferenceKeyLightIntensity, static_cast<double>(KeyLightIntensityResponse));
		LightStrength = static_cast<float>(FMath::Min(static_cast<double>(KeyLightOutputScale) * Factor,
			static_cast<double>(MaxKeyLightStrength)));
		if (Intensity == 0 || Light->IsHidden() || !Light->GetLightComponent()->IsVisible() || !Light->GetLightComponent()->bAffectsWorld)
			LightStrength = 0;
	}
	Visibility = 1.0f;
	if (bTraceKeyLightOcclusion && Light && LightStrength > 0 && GetOwner())
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(CharacterKeyLight), false, GetOwner());
		const USkeletalMeshComponent* Mesh = PreviewMesh.IsValid() ? PreviewMesh.Get() : TargetMesh.Get();
		const FVector Start = Mesh ? Mesh->Bounds.Origin : GetOwner()->GetActorLocation();
		Visibility = GetWorld()->LineTraceTestByChannel(Start, Start + ToLight * 100000.0, ECC_Visibility, Params)
			? OccludedKeyLightVisibility : 1.0f;
	}
	return true;
}

bool UGGYGOCharacterRenderComponent::UsesEditorSceneLighting() const
{
#if WITH_EDITOR
	// 独立材质球和 BP 的 EditorPreview 使用 MI 的明确预览值；仅关卡 Editor 世界消费真实场景灯。
	return !IsTemplate() && IsActive() && bPreviewSceneLighting && !bEditorPreviewSuspendedForPIE && GetWorld()
		&& GetWorld()->WorldType == EWorldType::Editor;
#else
	return false;
#endif
}

bool UGGYGOCharacterRenderComponent::ValidatePreviewBindings(const USkeletalMeshComponent* Mesh, FString& OutError) const
{
#if WITH_EDITOR
	if (!ValidateMaterialBindings(Mesh, OutError)) return false;
	// CPD 属于整个 Primitive，未配置的槽不能意外消费本组件的九个输入。
	TSet<int32> ConfiguredIndices;
	for (const FGGYGOCharacterRenderSlot& Slot : MaterialSlots) ConfiguredIndices.Add(Mesh->GetMaterialIndex(Slot.SlotName));
	for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
	{
		if (ConfiguredIndices.Contains(Index)) continue;
		const UMaterialInterface* Interface = Mesh->GetMaterial(Index);
		const UMaterial* Base = IsValid(Interface) ? Interface->GetMaterial() : nullptr;
		if (!Base) continue;
		for (const UMaterialExpression* Expression : Base->GetExpressions())
		{
			const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression);
			const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression);
			if ((Scalar && Scalar->bUseCustomPrimitiveData && Scalar->PrimitiveDataIndex < PreviewDataCount)
				|| (Vector && Vector->bUseCustomPrimitiveData && Vector->PrimitiveDataIndex < PreviewDataCount))
			{
				OutError = FString::Printf(TEXT("Unconfigured mesh slot %d consumes reserved scene preview CPD"), Index);
				return false;
			}
		}
	}
	for (const FGGYGOCharacterRenderSlot& Slot : MaterialSlots)
	{
		if (Mesh->GetMaterial(Mesh->GetMaterialIndex(Slot.SlotName)) != Slot.Material)
		{
			OutError = FString::Printf(TEXT("Editor preview slot '%s' differs from its configured material"), *Slot.SlotName.ToString());
			return false;
		}
		const UMaterial* Base = Slot.Material->GetMaterial();
		for (const UMaterialExpression* Expression : Base->GetExpressions())
		{
			const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression);
			const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression);
			if ((Scalar && Scalar->bUseCustomPrimitiveData && Scalar->PrimitiveDataIndex < PreviewDataCount
				&& Scalar->ParameterName != PreviewDataParameters[Scalar->PrimitiveDataIndex])
				|| (Vector && Vector->bUseCustomPrimitiveData && Vector->PrimitiveDataIndex < PreviewDataCount))
			{
				OutError = FString::Printf(TEXT("Material '%s' has another CPD consumer in preview indices 0..8"), *GetPathNameSafe(Base));
				return false;
			}
		}
		for (int32 Index = 0; Index < PreviewDataCount; ++Index)
		{
			int32 Matches = 0;
			for (const UMaterialExpression* Expression : Base->GetExpressions())
			{
				const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression);
				if (Parameter && Parameter->ParameterName == PreviewDataParameters[Index]
					&& Parameter->bUseCustomPrimitiveData && Parameter->PrimitiveDataIndex == Index) ++Matches;
			}
			if (Matches != 1)
			{
				OutError = FString::Printf(TEXT("Material '%s' needs exactly one CPD parameter '%s' at index %d"),
					*GetPathNameSafe(Base), *PreviewDataParameters[Index].ToString(), Index);
				return false;
			}
		}
	}
	return true;
#else
	OutError = TEXT("Editor preview is unavailable outside an editor build");
	return false;
#endif
}

bool UGGYGOCharacterRenderComponent::WritePreviewLighting(TConstArrayView<float> Values)
{
	USkeletalMeshComponent* Mesh = PreviewMesh.Get();
	if (!Mesh || Values.Num() != PreviewDataCount) return false;
	const TArray<float>& Current = Mesh->GetCustomPrimitiveData().Data;
	if (!PreviewWrittenData.IsEmpty())
	{
		for (int32 Index = 0; Index < PreviewDataCount; ++Index)
			if (!Current.IsValidIndex(Index) || Current[Index] != PreviewWrittenData[Index]) return false;
	}
	Mesh->SetCustomPrimitiveDataFloatArray(0, Values);
	PreviewWrittenData.Reset();
	PreviewWrittenData.Append(Values.GetData(), Values.Num());
	return true;
}

void UGGYGOCharacterRenderComponent::ReleasePreviewLighting()
{
	if (!PreviewWrittenData.IsEmpty()) ReleaseOutline();
	if (USkeletalMeshComponent* Mesh = PreviewMesh.Get())
	{
		const TArray<float> Current = Mesh->GetCustomPrimitiveData().Data;
		// 槽0是整个预览输入的有效标志。外部接管后，保留其全部数据。
		if (PreviewWrittenData.Num() == PreviewDataCount && Current.IsValidIndex(0)
			&& Current[0] == PreviewWrittenData[0])
		{
			for (int32 Index = 0; Index < PreviewDataCount; ++Index)
				if (Current.IsValidIndex(Index) && Current[Index] == PreviewWrittenData[Index])
					Mesh->SetCustomPrimitiveDataFloat(Index, PreviewOriginalData.IsValidIndex(Index) ? PreviewOriginalData[Index] : 0.0f);
		}
	}
	PreviewMesh.Reset();
	PreviewMeshAsset.Reset();
	PreviewOriginalData.Reset();
	PreviewWrittenData.Reset();
}

void UGGYGOCharacterRenderComponent::RefreshEditorLighting()
{
	FString Error;
	if (!ValidateLightSettings(Error))
	{
		FailRendering(Error);
		return;
	}
	bool bAdditional = false;
	if (!ValidateAdditionalMaterialContract(bAdditional, Error) || bAdditional)
	{
		FailRendering(Error.IsEmpty() ? TEXT("附加灯材质不能由九CPD完整运输；须明确使用TransientMaterials预览") : Error);
		return;
	}
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	// OnRegister/Construction 重建中 Mesh 尚未注册是原生暂态，复用本组件 Tick 等真正就绪。
	if (Mesh && !Mesh->IsRegistered())
	{
		ReleasePreviewLighting();
		SetComponentTickInterval(LightingUpdateInterval);
		SetComponentTickEnabled(true);
		return;
	}
	if (PreviewMesh.IsValid() && (PreviewMesh.Get() != Mesh || PreviewMeshAsset.Get() != Mesh->GetSkeletalMeshAsset()))
		ReleasePreviewLighting();
	if (MaterialSlots.IsEmpty()
		|| (!PreviewMesh.IsValid() && !ValidatePreviewBindings(Mesh, Error)))
	{
		FailRendering(Error.IsEmpty() ? TEXT("Editor scene preview requires explicit material slots") : Error);
		return;
	}
	for (const FGGYGOCharacterRenderSlot& Slot : MaterialSlots)
	{
		if (!Mesh || Mesh->GetMaterialIndex(Slot.SlotName) == INDEX_NONE
			|| Mesh->GetMaterial(Mesh->GetMaterialIndex(Slot.SlotName)) != Slot.Material)
		{
			FailRendering(TEXT("Editor preview material ownership changed; explicit refresh is required"));
			return;
		}
	}
	if (!PreviewMesh.IsValid())
	{
		// 不覆盖用户序列化槽或已经存在的瞬态槽。尾部数据由其它职责使用时原样保留。
		const TArray<float>& Current = Mesh->GetCustomPrimitiveData().Data;
		const TArray<float>& Defaults = Mesh->GetDefaultCustomPrimitiveData().Data;
		bool bOccupied = !Defaults.IsEmpty();
		for (int32 Index = 0; Index < FMath::Min(Current.Num(), PreviewDataCount); ++Index)
			bOccupied |= Current[Index] != 0.0f;
		if (bOccupied)
		{
			FailRendering(TEXT("Editor scene preview CPD indices 0..8 are already occupied; configure an exclusive mesh contract"));
			return;
		}
		PreviewMesh = Mesh;
		PreviewMeshAsset = Mesh->GetSkeletalMeshAsset();
		PreviewOriginalData = Current;
	}
	FVector Direction;
	FLinearColor Color;
	float Strength, Visibility;
	if (!SampleLighting(Direction, Color, Strength, Visibility)) return;
	const float Values[PreviewDataCount] = {1, float(Direction.X), float(Direction.Y), float(Direction.Z),
		Color.R, Color.G, Color.B, Strength, Visibility};
	if (!WritePreviewLighting(Values))
	{
		FailRendering(TEXT("Editor scene preview CPD ownership changed; external values preserved"));
		return;
	}
	if (!OutlineMesh && !UpdateOutline(Mesh)) return;
	if (OutlineMesh)
	{
		OutlineMesh->SetVisibility(Mesh->IsVisible());
		OutlineMesh->SetHiddenInGame(Mesh->bHiddenInGame);
	}
	SetComponentTickInterval(LightingUpdateInterval);
	SetComponentTickEnabled(true);
}

#if WITH_EDITOR
void UGGYGOCharacterRenderComponent::BindEditorLifecycle()
{
	if (IsTemplate() || !GetWorld() || GetWorld()->WorldType != EWorldType::Editor || PackagePreSaveHandle.IsValid()) return;
	PackagePreSaveHandle = UPackage::PreSavePackageWithContextEvent.AddUObject(this, &ThisClass::HandleEditorPackagePreSave);
	PrePIEHandle = FEditorDelegates::PreBeginPIE.AddUObject(this, &ThisClass::HandleEditorPrePIE);
	EndPIEHandle = FEditorDelegates::EndPIE.AddUObject(this, &ThisClass::HandleEditorEndPIE);
	UndoRedoHandle = FEditorDelegates::PostUndoRedo.AddUObject(this, &ThisClass::HandleEditorPostUndoRedo);
}

void UGGYGOCharacterRenderComponent::UnbindEditorLifecycle()
{
	UPackage::PreSavePackageWithContextEvent.Remove(PackagePreSaveHandle);
	FEditorDelegates::PreBeginPIE.Remove(PrePIEHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	FEditorDelegates::PostUndoRedo.Remove(UndoRedoHandle);
	PackagePreSaveHandle.Reset(); PrePIEHandle.Reset(); EndPIEHandle.Reset(); UndoRedoHandle.Reset();
	bResumeEditorPreview = false;
	bEditorPreviewSuspendedForPIE = false;
}

void UGGYGOCharacterRenderComponent::RestoreSerializedPreviewOverrides()
{
	if (IsTemplate() || !GetWorld() || GetWorld()->WorldType != EWorldType::Editor) return;
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (!Mesh) return;
	const TArray<FName> Names = Mesh->GetMaterialSlotNames();
	for (int32 Index = 0; Index < Mesh->OverrideMaterials.Num(); ++Index)
	{
		auto* MID = Cast<UMaterialInstanceDynamic>(Mesh->OverrideMaterials[Index]);
		auto* Data = MID ? Cast<UGGYGOCharacterPreviewMaterialData>(MID->GetAssetUserDataOfClass(UGGYGOCharacterPreviewMaterialData::StaticClass())) : nullptr;
		if (Data && Data->OwnerActor == GetOwner() && Data->ComponentName == GetFName() && Data->SlotIndex == Index)
		{
			const bool bSameSlot = Data->MeshAsset == Mesh->GetSkeletalMeshAsset()
				&& Names.IsValidIndex(Index) && Data->SlotName == Names[Index];
			// 同ReleaseMaterials：只消费本槽创建凭据；换网格后仅清自有孤立覆盖。
			Mesh->SetMaterial(Index, bSameSlot ? Data->OriginalOverride.Get() : nullptr);
		}
	}
}

void UGGYGOCharacterRenderComponent::ResumeEditorPreview()
{
	if (!UsesEditorSceneLighting() || bEditorPreviewSuspendedForPIE) return;
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (!Mesh) return;
	for (const auto& Slot : MaterialSlots)
	{
		const int32 Index = Mesh->GetMaterialIndex(Slot.SlotName);
		if (Index == INDEX_NONE || Mesh->GetMaterial(Index) != Slot.Material) return;
	}
	bResumeEditorPreview = true;
	SetComponentTickInterval(LightingUpdateInterval);
	SetComponentTickEnabled(true);
}

void UGGYGOCharacterRenderComponent::HandleEditorPackagePreSave(UPackage* Package, FObjectPreSaveContext)
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (!UsesEditorMaterialPreview() || !Mesh || (Package != GetWorld()->GetPackage()
		&& Package != GetOwner()->GetPackage() && Package != Mesh->GetPackage())) return;
	ReleaseMaterials();
	RestoreSerializedPreviewOverrides();
	// 保存成功或失败均只在原Tick且GIsSavingPackage退出后恢复；不在序列化栈内装回MID。
	ResumeEditorPreview();
}

void UGGYGOCharacterRenderComponent::HandleEditorPrePIE(bool)
{
	ReleaseMaterials();
	RestoreSerializedPreviewOverrides();
	bEditorPreviewSuspendedForPIE = true;
	bResumeEditorPreview = false;
}

void UGGYGOCharacterRenderComponent::HandleEditorEndPIE(bool)
{
	bEditorPreviewSuspendedForPIE = false;
	if (UsesEditorMaterialPreview()) ResumeEditorPreview();
	else if (UsesEditorSceneLighting()) RefreshMaterials();
}

void UGGYGOCharacterRenderComponent::HandleEditorPostUndoRedo()
{
	if (!GetWorld() || GetWorld()->WorldType != EWorldType::Editor) return;
	ReleaseMaterials();
	RestoreSerializedPreviewOverrides();
	ResumeEditorPreview();
}

void UGGYGOCharacterRenderComponent::PreEditUndo()
{
	ReleaseMaterials();
	Super::PreEditUndo();
}

void UGGYGOCharacterRenderComponent::PostEditUndo()
{
	Super::PostEditUndo();
	if (IsValid(this) && IsRegistered()) HandleEditorPostUndoRedo();
}

void UGGYGOCharacterRenderComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	ReleaseMaterials();
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// Super 可能触发 BP Construction 重建；新实例在 OnRegister 初始化，旧实例不能继续写。
	if (IsValid(this) && IsRegistered()) RefreshMaterials();
}
#endif
