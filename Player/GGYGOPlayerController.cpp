/**
 * @file GGYGOPlayerController.cpp
 * @brief 玩家控制器实现
 */
#include "Player/GGYGOPlayerController.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "GameFramework/Pawn.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerController)

AGGYGOPlayerController::AGGYGOPlayerController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UGGYGOAbilitySystemComponent* AGGYGOPlayerController::GetGGYGOAbilitySystemComponent() const
{
	// 经 PawnExtension 取而不是直接 FindComponentByClass：
	// ASC 可能不在 Pawn 上（队伍级 ASC 挂 PlayerState），
	// 而 PawnExtension 是"当前该用哪个 ASC"这个问题的唯一答案来源。
	const UGGYGOPawnExtensionComponent* PawnExtComp =
		UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetPawn());

	return PawnExtComp ? PawnExtComp->GetGGYGOAbilitySystemComponent() : nullptr;
}

void AGGYGOPlayerController::PostProcessInput(const float DeltaTime, const bool bGamePaused)
{
	// 在 Super 之前消费。Super 会把累积的移动与视角输入交给 Pawn，
	// 而能力激活可能施加 `Restriction.CantMove` —— 先激活能力，
	// 这一帧的移动就能立刻被限制住，不会多走一帧。
	if (UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent())
	{
		GGYGOASC->ProcessAbilityInput(DeltaTime, bGamePaused);
	}

	Super::PostProcessInput(DeltaTime, bGamePaused);
}
