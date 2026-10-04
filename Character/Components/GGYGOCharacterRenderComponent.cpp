#include "Character/Components/GGYGOCharacterRenderComponent.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "Materials/MaterialInstanceDynamic.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterRenderComponent)

UGGYGOCharacterRenderComponent::UGGYGOCharacterRenderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bAutoActivate = true;
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
}

void UGGYGOCharacterRenderComponent::Deactivate()
{
	ReleaseMaterials();
	Super::Deactivate();
}

UMaterialInterface* UGGYGOCharacterRenderComponent::FindMaterialForSlot(FName SlotName) const
{
	for (const FGGYGOCharacterRenderSlot& Entry : MaterialSlots)
	{
		if (Entry.SlotName == SlotName && Entry.Material) return Entry.Material;
	}
	return nullptr;
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
	if (IsValid(OutlineMesh)) OutlineMesh->DestroyComponent();
	OutlineMesh = nullptr;
	DynamicMaterials.Reset();
	MaterialBindings.Reset();
	BoundMeshAsset.Reset();
	ResolvedKeyLight.Reset();
	TargetMesh = nullptr;
}

void UGGYGOCharacterRenderComponent::RefreshMaterials()
{
	ReleaseMaterials();
	if (!HasBegunPlay() || !IsActive()) return;
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	TargetMesh = Character ? Character->GetMesh() : nullptr;
	if (!IsValid(TargetMesh) || !TargetMesh->GetSkeletalMeshAsset() || MaterialSlots.IsEmpty()) return;
	BoundMeshAsset = TargetMesh->GetSkeletalMeshAsset();
	const TArray<FName> Slots = TargetMesh->GetMaterialSlotNames();
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		UMaterialInterface* Parent = FindMaterialForSlot(Slots[Index]);
		if (!Parent) continue;
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, this);
		if (!Instance) continue;
		FGGYGOCharacterRenderBinding& Binding = MaterialBindings.AddDefaulted_GetRef();
		Binding.SlotIndex = Index;
		Binding.SlotName = Slots[Index];
		Binding.OriginalOverride = TargetMesh->OverrideMaterials.IsValidIndex(Index)
			? TargetMesh->OverrideMaterials[Index].Get() : nullptr;
		Binding.InstalledMaterial = Instance;
		TargetMesh->SetMaterial(Index, Instance);
		DynamicMaterials.Add(Instance);
	}
	UpdateOutline();
	SetComponentTickInterval(FMath::Max(0.01f, LightingUpdateInterval));
	SetComponentTickEnabled(!DynamicMaterials.IsEmpty());
	UpdateLighting();
}

void UGGYGOCharacterRenderComponent::UpdateOutline()
{
	if (!bEnableOutline || !OutlineMaterial || !TargetMesh || DynamicMaterials.IsEmpty()) return;
	OutlineMesh = NewObject<USkeletalMeshComponent>(GetOwner());
	OutlineMesh->SetupAttachment(TargetMesh);
	OutlineMesh->SetSkeletalMeshAsset(TargetMesh->GetSkeletalMeshAsset());
	OutlineMesh->SetLeaderPoseComponent(TargetMesh);
	OutlineMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutlineMesh->SetCastShadow(false);
	OutlineMesh->SetReceivesDecals(false);
	for (int32 Index = 0; Index < OutlineMesh->GetNumMaterials(); ++Index)
	{
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(OutlineMaterial, this);
		Instance->SetScalarParameterValue(TEXT("OutlineWidth"), FMath::Max(0.0f, OutlineWidth));
		const UMaterialInstanceDynamic* Surface = Cast<UMaterialInstanceDynamic>(TargetMesh->GetMaterial(Index));
		Instance->SetScalarParameterValue(TEXT("OutlineOpacity"), DynamicMaterials.Contains(Surface) ? 1.0f : 0.0f);
		OutlineMesh->SetMaterial(Index, Instance);
	}
	OutlineMesh->RegisterComponent();
}

void UGGYGOCharacterRenderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
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
	ADirectionalLight* Light = ResolveKeyLight();
	const FVector ToLight = Light ? -Light->GetActorForwardVector() : FVector::UpVector;
	const FLinearColor LightColor = Light ? Light->GetLightColor() * KeyLightTint : FLinearColor::Black;
	float LightStrength = 0.0f;
	if (Light)
	{
		const float Ratio = FMath::Max(0.0f, Light->GetLightComponent()->Intensity) / FMath::Max(0.01f, ReferenceKeyLightIntensity);
		const float Factor = FMath::Pow(Ratio, FMath::Clamp(KeyLightIntensityResponse, 0.0f, 1.0f));
		LightStrength = FMath::Clamp(KeyLightOutputScale * Factor, 0.0f, FMath::Max(0.0f, MaxKeyLightStrength));
	}
	float Visibility = 1.0f;
	if (bTraceKeyLightOcclusion && Light && GetOwner())
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(CharacterKeyLight), false, GetOwner());
		const FVector Start = TargetMesh ? TargetMesh->Bounds.Origin : GetOwner()->GetActorLocation();
		Visibility = GetWorld()->LineTraceTestByChannel(Start, Start + ToLight * 100000.0, ECC_Visibility, Params)
			? FMath::Clamp(OccludedKeyLightVisibility, 0.0f, 1.0f) : 1.0f;
	}
	for (UMaterialInstanceDynamic* Instance : DynamicMaterials)
	{
		if (!Instance) continue;
		Instance->SetVectorParameterValue(TEXT("KeyLightDirectionWS"), FLinearColor(ToLight.X, ToLight.Y, ToLight.Z));
		Instance->SetVectorParameterValue(TEXT("KeyLightColor"), LightColor);
		Instance->SetScalarParameterValue(TEXT("SceneLightStrength"), LightStrength);
		Instance->SetScalarParameterValue(TEXT("KeyLightVisibility"), Visibility);
	}
}
