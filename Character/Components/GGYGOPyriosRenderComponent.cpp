#include "Character/Components/GGYGOPyriosRenderComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPyriosRenderComponent)

namespace
{
	constexpr TCHAR PyriosMaterialFolder[] = TEXT("/Game/Characters/Player/Pyrios/Materials/Generated/");

	FString PyriosMaterialName(const FName SlotName)
	{
		const FString Name = SlotName.ToString().ToLower();
		if (Name.Contains(TEXT("body_1"))) return TEXT("MI_Pyrois_Body_1");
		if (Name.Contains(TEXT("body_2"))) return TEXT("MI_Pyrois_Body_2");
		if (Name.Contains(TEXT("weapon"))) return TEXT("MI_Pyrois_Weapon01");
		return FString();
	}
}

UGGYGOPyriosRenderComponent::UGGYGOPyriosRenderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UGGYGOPyriosRenderComponent::BeginPlay()
{
	Super::BeginPlay();
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	TargetMesh = Character ? Character->GetMesh() : nullptr;
	if (!TargetMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("PyriosRenderComponent requires an ACharacter skeletal mesh on %s"), *GetNameSafe(GetOwner()));
		return;
	}

	RefreshMaterials();
	if (DynamicMaterials.IsEmpty()) return;
	SetComponentTickInterval(FMath::Max(0.01f, LightingUpdateInterval));
	SetComponentTickEnabled(true);
	UpdateLighting();
}

void UGGYGOPyriosRenderComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (TargetMesh)
	{
		for (int32 Index = 0; Index < OriginalMaterials.Num(); ++Index)
		{
			if (OriginalMaterials[Index]) TargetMesh->SetMaterial(Index, OriginalMaterials[Index]);
		}
	}
	if (OutlineMesh) OutlineMesh->DestroyComponent();
	OutlineMesh = nullptr;
	DynamicMaterials.Reset();
	OriginalMaterials.Reset();
	Super::EndPlay(EndPlayReason);
}

UMaterialInterface* UGGYGOPyriosRenderComponent::FindMaterialForSlot(FName SlotName) const
{
	for (const FGGYGOCharacterRenderSlot& Entry : MaterialSlots)
	{
		if (Entry.SlotName == SlotName && Entry.Material) return Entry.Material;
	}

	const FString MeshName = GetNameSafe(TargetMesh ? TargetMesh->GetSkeletalMeshAsset() : nullptr);
	if (!bAutoConfigurePyrios || !MeshName.Contains(TEXT("Pyrois"))) return nullptr;
	const FString MaterialName = PyriosMaterialName(SlotName);
	if (MaterialName.IsEmpty()) return nullptr;
	const FString Path = FString::Printf(TEXT("%s%s.%s"), PyriosMaterialFolder, *MaterialName, *MaterialName);
	return LoadObject<UMaterialInterface>(nullptr, *Path);
}

void UGGYGOPyriosRenderComponent::RefreshMaterials()
{
	if (!TargetMesh) return;
	for (int32 Index = 0; Index < OriginalMaterials.Num(); ++Index)
	{
		if (OriginalMaterials[Index]) TargetMesh->SetMaterial(Index, OriginalMaterials[Index]);
	}
	if (OutlineMesh)
	{
		OutlineMesh->DestroyComponent();
		OutlineMesh = nullptr;
	}
	DynamicMaterials.Reset();
	OriginalMaterials.Reset();
	const int32 Count = TargetMesh->GetNumMaterials();
	OriginalMaterials.SetNum(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FName Slot = TargetMesh->GetMaterialSlotNames().IsValidIndex(Index)
			? TargetMesh->GetMaterialSlotNames()[Index] : NAME_None;
		UMaterialInterface* Parent = FindMaterialForSlot(Slot);
		if (!Parent) continue;
		OriginalMaterials[Index] = TargetMesh->GetMaterial(Index);
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, this);
		TargetMesh->SetMaterial(Index, Instance);
		DynamicMaterials.Add(Instance);
	}
	UpdateOutline();
	SetComponentTickInterval(FMath::Max(0.01f, LightingUpdateInterval));
	SetComponentTickEnabled(!DynamicMaterials.IsEmpty());
	UpdateLighting();
}

void UGGYGOPyriosRenderComponent::UpdateOutline()
{
	if (!bEnableOutline || !TargetMesh || DynamicMaterials.IsEmpty()) return;
	UMaterialInterface* Parent = OutlineMaterial;
	if (!Parent)
	{
		Parent = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/Characters/Player/Pyrios/Materials/Generated/M_Pyrois_Outline.M_Pyrois_Outline"));
	}
	if (!Parent) return;
	OutlineMesh = NewObject<USkeletalMeshComponent>(GetOwner(), TEXT("PyriosOutlineMesh"));
	OutlineMesh->SetupAttachment(TargetMesh);
	OutlineMesh->SetSkeletalMeshAsset(TargetMesh->GetSkeletalMeshAsset());
	OutlineMesh->SetLeaderPoseComponent(TargetMesh);
	OutlineMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutlineMesh->SetCastShadow(false);
	OutlineMesh->SetReceivesDecals(false);
	for (int32 Index = 0; Index < OutlineMesh->GetNumMaterials(); ++Index)
	{
		UMaterialInstanceDynamic* OutlineInstance = UMaterialInstanceDynamic::Create(Parent, this);
		OutlineInstance->SetScalarParameterValue(TEXT("OutlineWidth"), OutlineWidth);
		const UMaterialInstanceDynamic* Surface = Cast<UMaterialInstanceDynamic>(TargetMesh->GetMaterial(Index));
		OutlineInstance->SetScalarParameterValue(TEXT("OutlineOpacity"), DynamicMaterials.Contains(Surface) ? 1.0f : 0.0f);
		OutlineMesh->SetMaterial(Index, OutlineInstance);
	}
	OutlineMesh->RegisterComponent();
}

void UGGYGOPyriosRenderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateLighting();
}

void UGGYGOPyriosRenderComponent::UpdateLighting()
{
	if (DynamicMaterials.IsEmpty() || !GetWorld()) return;
	if (!KeyLight)
	{
		KeyLight = Cast<ADirectionalLight>(UGameplayStatics::GetActorOfClass(GetWorld(), ADirectionalLight::StaticClass()));
	}
	const FVector ToLight = KeyLight ? -KeyLight->GetActorForwardVector() : FVector(0.35, 0.25, 0.9).GetSafeNormal();
	const FLinearColor LightColor = KeyLight ? KeyLight->GetLightColor() : FLinearColor::White;
	const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FRotationMatrix CameraAxes(Camera ? Camera->GetCameraRotation() : FRotator::ZeroRotator);
	const FVector CameraRight = CameraAxes.GetUnitAxis(EAxis::Y);
	const FVector CameraUp = CameraAxes.GetUnitAxis(EAxis::Z);
	float Visibility = 1.0f;
	if (bTraceKeyLightOcclusion && KeyLight && GetOwner())
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PyriosKeyLight), false, GetOwner());
		const FVector Start = GetOwner()->GetActorLocation() + FVector(0, 0, 80);
		Visibility = GetWorld()->LineTraceTestByChannel(Start, Start + ToLight * 100000.0, ECC_Visibility, Params) ? 0.35f : 1.0f;
	}
	for (UMaterialInstanceDynamic* Instance : DynamicMaterials)
	{
		if (!Instance) continue;
		Instance->SetVectorParameterValue(TEXT("KeyLightDirectionWS"), FLinearColor(ToLight.X, ToLight.Y, ToLight.Z));
		Instance->SetVectorParameterValue(TEXT("KeyLightColor"), LightColor);
		Instance->SetVectorParameterValue(TEXT("CameraRightWS"), FLinearColor(CameraRight.X, CameraRight.Y, CameraRight.Z));
		Instance->SetVectorParameterValue(TEXT("CameraUpWS"), FLinearColor(CameraUp.X, CameraUp.Y, CameraUp.Z));
		Instance->SetScalarParameterValue(TEXT("KeyLightVisibility"), Visibility);
	}
}
