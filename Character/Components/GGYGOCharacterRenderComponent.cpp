/** @file 校验 Toon 接口、适配运行时/编辑器主方向光，并仅释放自有表现资源。 */
#include "Character/Components/GGYGOCharacterRenderComponent.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#if WITH_EDITOR
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterRenderComponent)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOCharacterRendering, Log, All);

namespace
{
	const FName LightDirectionParameter(TEXT("KeyLightDirectionWS"));
	const FName LightColorParameter(TEXT("KeyLightColor"));
	const FName LightStrengthParameter(TEXT("SceneLightStrength"));
	const FName LightVisibilityParameter(TEXT("KeyLightVisibility"));
	const FName PreviewDataParameters[] = {
		TEXT("ScenePreviewValid"), TEXT("ScenePreviewDirectionX"), TEXT("ScenePreviewDirectionY"),
		TEXT("ScenePreviewDirectionZ"), TEXT("ScenePreviewColorR"), TEXT("ScenePreviewColorG"),
		TEXT("ScenePreviewColorB"), TEXT("ScenePreviewStrength"), TEXT("ScenePreviewVisibility")};

	bool IsFiniteColor(const FLinearColor& Color)
	{
		return FMath::IsFinite(Color.R) && FMath::IsFinite(Color.G)
			&& FMath::IsFinite(Color.B) && FMath::IsFinite(Color.A);
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
	if (HasBegunPlay() && IsActive()) RefreshMaterials();
	else if (UsesEditorSceneLighting()) RefreshEditorLighting();
}

void UGGYGOCharacterRenderComponent::OnUnregister()
{
	ReleaseMaterials();
	Super::OnUnregister();
}

void UGGYGOCharacterRenderComponent::Activate(bool bReset)
{
	Super::Activate(bReset);
	if (HasBegunPlay()) RefreshMaterials();
	else if (UsesEditorSceneLighting()) RefreshEditorLighting();
}

void UGGYGOCharacterRenderComponent::Deactivate()
{
	ReleaseMaterials();
	Super::Deactivate();
}

bool UGGYGOCharacterRenderComponent::ValidateLightSettings(FString& OutError) const
{
	if (!FMath::IsFinite(LightingUpdateInterval) || LightingUpdateInterval < 0.01f)
		OutError = TEXT("LightingUpdateInterval must be finite and >= 0.01");
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
		OutError.Reset();
		return true;
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
	ReleaseMaterials();
}

bool UGGYGOCharacterRenderComponent::HasCurrentMesh() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!IsValid(TargetMesh) || !BoundMeshAsset.IsValid() || !Character
		|| Character->GetMesh() != TargetMesh || TargetMesh->GetSkeletalMeshAsset() != BoundMeshAsset.Get()) return false;
	const TArray<FName> Slots = TargetMesh->GetMaterialSlotNames();
	for (const FGGYGOCharacterRenderBinding& Binding : MaterialBindings)
	{
		if (!Slots.IsValidIndex(Binding.SlotIndex) || Slots[Binding.SlotIndex] != Binding.SlotName) return false;
	}
	return true;
}

void UGGYGOCharacterRenderComponent::ReleaseMaterials()
{
	SetComponentTickEnabled(false);
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
}

void UGGYGOCharacterRenderComponent::RefreshMaterials()
{
	ReleaseMaterials();
	if (UsesEditorSceneLighting())
	{
		RefreshEditorLighting();
		return;
	}
	if (!HasBegunPlay() || !IsActive() || MaterialSlots.IsEmpty()) return;
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	TargetMesh = Character ? Character->GetMesh() : nullptr;
	FString Error;
	// Validate the complete configuration before installing the first override.
	if (!ValidateLightSettings(Error) || !ValidateMaterialBindings(TargetMesh, Error))
	{
		FailRendering(Error);
		return;
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
	if (UsesEditorSceneLighting())
	{
		RefreshEditorLighting();
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

ADirectionalLight* UGGYGOCharacterRenderComponent::ResolveKeyLight()
{
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

void UGGYGOCharacterRenderComponent::UpdateLighting()
{
	if (DynamicMaterials.IsEmpty() || !GetWorld()) return;
	FVector ToLight;
	FLinearColor LightColor;
	float LightStrength, Visibility;
	if (!SampleLighting(ToLight, LightColor, LightStrength, Visibility)) return;
	for (UMaterialInstanceDynamic* Instance : DynamicMaterials)
	{
		if (!Instance) continue;
		Instance->SetVectorParameterValue(LightDirectionParameter, FLinearColor(ToLight.X, ToLight.Y, ToLight.Z));
		Instance->SetVectorParameterValue(LightColorParameter, LightColor);
		Instance->SetScalarParameterValue(LightStrengthParameter, LightStrength);
		Instance->SetScalarParameterValue(LightVisibilityParameter, Visibility);
	}
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
	ToLight = Light ? -Light->GetActorForwardVector() : FVector::UpVector;
	LightColor = Light ? Light->GetLightColor() * KeyLightTint : FLinearColor::Black;
	LightStrength = 0.0f;
	if (Light)
	{
		const float Intensity = Light->GetLightComponent()->Intensity;
		if (!FMath::IsFinite(Intensity) || !IsFiniteColor(LightColor) || ToLight.ContainsNaN())
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
	}
	Visibility = 1.0f;
	if (bTraceKeyLightOcclusion && Light && GetOwner())
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
	return !IsTemplate() && IsActive() && bPreviewSceneLighting && GetWorld()
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
void UGGYGOCharacterRenderComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	ReleaseMaterials();
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// Super 可能触发 BP Construction 重建；新实例在 OnRegister 初始化，旧实例不能继续写。
	if (IsValid(this) && IsRegistered()) RefreshMaterials();
}
#endif
