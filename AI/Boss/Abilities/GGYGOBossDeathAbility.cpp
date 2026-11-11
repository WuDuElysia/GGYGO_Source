#include "AI/Boss/Abilities/GGYGOBossDeathAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Engine/World.h"
#include "System/GGYGOGameplayTags.h"
#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossDeathAbility)

struct UGGYGOBossDeathAbility::FDeathResources
{

	FGGYGOAbilityActivationHandle Original;
	TWeakObjectPtr<UGGYGOHealthComponent> Health;
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	TWeakObjectPtr<UWorld> World;
	FTimerHandle Timer;
	bool bOwnsDeathState = false;
};

UGGYGOBossDeathAbility::UGGYGOBossDeathAbility(const FObjectInitializer& Initializer) : Super(Initializer)
{

	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	FGameplayTagContainer Tags = GetAssetTags();
	Tags.AddTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath);
	SetAssetTags(Tags);
}

bool UGGYGOBossDeathAbility::IsDeathResourceCurrent(FGGYGOBossActionToken Token) const
{

	return DeathResources && DeathResources->Original.HasSameActivation(Token.Original) && IsActivationCurrent(Token.Original);
}

void UGGYGOBossDeathAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
FGameplayAbilityActivationInfo Info, const FGameplayEventData* Event)
{

	const auto Resources = MakeShared<FDeathResources>();
	Resources->Original = Original;
	Resources->Health = UGGYGOHealthComponent::FindHealthComponent(GetAvatarActorFromActorInfo());
	Resources->ASC = GetGGYGOAbilitySystemComponentFromActorInfo();
	Resources->World = GetWorld();
	DeathResources = Resources;
	FGGYGOBossActionToken Token;
	Token.Original = Original;
	if (!HasAuthority(&Info) || !Resources->Health.IsValid() || !Resources->ASC.IsValid() || !Resources->World.IsValid()
	|| !GetClass()->IsFunctionImplementedInScript(GET_FUNCTION_NAME_CHECKED(ThisClass, PrepareBossDeath)))

	{
		FailBossDeath(Token, TEXT("需要权威原Health/ASC/World和实际BP死亡流程。"));
		return;

	}
	PrepareBossDeath(Token);
	// 业务顺序由共享BP流程决定，Native不自动启动表现或计时。
}

bool UGGYGOBossDeathAbility::CancelOtherBossActions(FGGYGOBossActionToken Token, FString& Error)
{

	if (!IsDeathResourceCurrent(Token) || !DeathResources->ASC.IsValid())
	{
		Error = TEXT("缺少原死亡能力/ASC。");
		return false;

	}
	DeathResources->ASC->CancelAbilitiesByFunc([this](const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle)

	{
		return Ability != this && !Ability->GetAssetTags().HasTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath);
	}, true);
	Error.Reset();
	return IsDeathResourceCurrent(Token);
}

bool UGGYGOBossDeathAbility::StartBossDeathState(FGGYGOBossActionToken Token, FString& Error)
{

	if (!IsDeathResourceCurrent(Token) || !DeathResources->Health.IsValid() || DeathResources->bOwnsDeathState)

	{
		Error = TEXT("死亡状态启动需要空的原Health资源。");
		return false;

	}
	const auto Resources = DeathResources;
	Resources->bOwnsDeathState = true;
	Resources->Health->StartDeath();
	Error.Reset();
	return IsDeathResourceCurrent(Token);
}

bool UGGYGOBossDeathAbility::WaitBossDeathPresentation(FGGYGOBossActionToken Token, float Seconds, FString& Error)
{

	if (!IsDeathResourceCurrent(Token) || !DeathResources->World.IsValid() || !FMath::IsFinite(Seconds) || Seconds <= 0.0f
	|| DeathResources->Timer.IsValid() || !GetClass()->IsFunctionImplementedInScript(GET_FUNCTION_NAME_CHECKED(ThisClass, BossDeathPresentationFinished)))

	{
		Error = TEXT("死亡等待需要原资源、正保留时长、空计时器及BP完成处理。");
		return false;

	}
	const TWeakPtr<FDeathResources> WeakResources(DeathResources);
	DeathResources->World->GetTimerManager().SetTimer(DeathResources->Timer,
	FTimerDelegate::CreateWeakLambda(this, [this, WeakResources, Token]()

	{

		const auto Resources = WeakResources.Pin();
		if (!Resources || DeathResources != Resources || !IsDeathResourceCurrent(Token))
		{
			return;
		}
		Resources->Timer.Invalidate();
		BossDeathPresentationFinished(Token); // 保留时长到达，不冒称动画/GPU完成。

	}), Seconds, false);
	Error.Reset();
	return true;
}

void UGGYGOBossDeathAbility::FinishBossDeath(FGGYGOBossActionToken Token, bool bCancelled)
{

	if (!IsDeathResourceCurrent(Token))
	{
		return;

	}
	RequestAbilityEnd(Token.Original, true, bCancelled);
}

void UGGYGOBossDeathAbility::FailBossDeath(FGGYGOBossActionToken Token, const FString& Reason)
{

	if (!IsDeathResourceCurrent(Token))
	{
		return;

	}
	UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("BossDeath [%s] 原死亡流程失败：%s"), *GetPathName(), *Reason);
	RequestAbilityEnd(Token.Original, true, true);
}

void UGGYGOBossDeathAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{

	TSharedPtr<FDeathResources> Resources = DeathResources;
	if (Resources && Resources->Original.HasSameActivation(Context.GetOriginalActivation()))

	{

		DeathResources.Reset();
		if (Resources->World.IsValid())
		{
			Resources->World->GetTimerManager().ClearTimer(Resources->Timer);

		}

	}
	else
	{
		Resources.Reset();

	}
	Super::CleanupAbilityResourcesForTermination(Context);
	// 只归还本能力实际获取的Health义务；正常表现/等待/结束顺序由BP决定。
	if (Resources && Resources->bOwnsDeathState && Resources->Health.IsValid())
	{
		Resources->Health->FinishDeath();

	}
}
