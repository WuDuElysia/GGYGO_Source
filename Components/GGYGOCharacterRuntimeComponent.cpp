/**
 * @file GGYGOCharacterRuntimeComponent.cpp
 * @brief GGYGO 角色控制 Pipeline 的 Unreal 生命周期薄壳实现
 */
#include "Components/GGYGOCharacterRuntimeComponent.h"

#include "Pipeline/CharacterControlPipeline.h"

UGGYGOCharacterRuntimeComponent::UGGYGOCharacterRuntimeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	ControlPipeline = MakeUnique<FCharacterControlPipeline>();
}

UGGYGOCharacterRuntimeComponent::~UGGYGOCharacterRuntimeComponent() = default;

void UGGYGOCharacterRuntimeComponent::InitializeRuntime(
	ABaseCharacter* InOwner,
	UAbilitySystemComponent* InASC,
	USkeletalMeshComponent* InMesh)
{
	if (ControlPipeline)
	{
		ControlPipeline->Initialize(InOwner, InASC, InMesh);
	}
}

void UGGYGOCharacterRuntimeComponent::ProcessFrame(float DeltaTime)
{
	if (ControlPipeline)
	{
		ControlPipeline->ProcessFrame(DeltaTime);
	}
}

FRuntimeData* UGGYGOCharacterRuntimeComponent::GetRuntimeData() const
{
	return ControlPipeline ? ControlPipeline->GetRuntimeData() : nullptr;
}

void UGGYGOCharacterRuntimeComponent::SetMoveInput(const FVector2D& Value)
{
	if (ControlPipeline)
	{
		ControlPipeline->SetMoveInput(Value);
	}
}

void UGGYGOCharacterRuntimeComponent::ClearMoveInput()
{
	if (ControlPipeline)
	{
		ControlPipeline->ClearMoveInput();
	}
}

void UGGYGOCharacterRuntimeComponent::SetLookInput(const FVector2D& Value)
{
	if (ControlPipeline)
	{
		ControlPipeline->SetLookInput(Value);
	}
}

void UGGYGOCharacterRuntimeComponent::SetSprintHeld(bool bHeld)
{
	if (ControlPipeline)
	{
		ControlPipeline->SetSprintHeld(bHeld);
	}
}

void UGGYGOCharacterRuntimeComponent::SetForceWalkHeld(bool bHeld)
{
	if (ControlPipeline)
	{
		ControlPipeline->SetForceWalkHeld(bHeld);
	}
}

void UGGYGOCharacterRuntimeComponent::NotifyCanYaw()
{
	if (ControlPipeline)
	{
		ControlPipeline->NotifyCanYaw();
	}
}

bool UGGYGOCharacterRuntimeComponent::IsInitialized() const
{
	return ControlPipeline && ControlPipeline->IsInitialized();
}
