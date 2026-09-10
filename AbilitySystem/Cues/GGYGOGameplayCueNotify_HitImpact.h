/**
 * @file GGYGOGameplayCueNotify_HitImpact.h
 * @brief 按命中表面分流的命中特效
 *
 * 打金属出火花、打肉出血、打石头出碎屑 —— 同一次攻击的反馈要随被击中的
 * 材质变化。
 *
 * ## 为什么不用多个 Cue Tag 解决
 * 也可以让攻击方按材质选不同的 Cue Tag（`GameplayCue.Hit.Metal` 等），
 * 但那要求攻击逻辑知道所有材质种类，每加一种表面就要改攻击代码。
 *
 * 本类改为让**一个** Cue 内部分流：攻击方只发一个"命中了"的 Cue，
 * 表面种类从 GE 的目标 Tag 里读（`UGGYGOGameplayAbility::ApplyAbilityTagsToGameplayEffectSpec`
 * 已经把物理材质的 Tag 并进去了）。加一种表面只需在本资产的映射表里加一行。
 *
 * ## 为什么派生 Burst 而不是 Actor 版
 * 命中特效是一次性的：放个粒子、播个音效、震一下屏幕，不需要持续存在的实体。
 * `UGameplayCueNotify_Burst` 不生成 Actor，开销明显更低，
 * 而战斗中每秒可能触发几十次命中。
 */
#pragma once

#include "GameplayCueNotify_Burst.h"
#include "GameplayTagContainer.h"

#include "GGYGOGameplayCueNotify_HitImpact.generated.h"

class UNiagaraSystem;
class UObject;
class USoundBase;

/** 一种表面对应的反馈。 */
USTRUCT(BlueprintType)
struct FGGYGOHitImpactEffect
{
	GENERATED_BODY()

	/**
	 * 匹配的表面 Tag。
	 *
	 * 用层级匹配而非精确匹配：配 `SurfaceType.Metal` 能同时命中
	 * `SurfaceType.Metal.Thin` 与 `SurfaceType.Metal.Heavy`，
	 * 于是细分表面时不必逐个补配置。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (Categories = "SurfaceType"))
	FGameplayTag SurfaceTag;

	/** 粒子特效。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<UNiagaraSystem> ImpactEffect = nullptr;

	/** 音效。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<USoundBase> ImpactSound = nullptr;
};

UCLASS(Blueprintable, meta = (DisplayName = "GGYGO Hit Impact Cue", ShortTooltip = "按命中表面分流的命中特效"))
class GGYGO_API UGGYGOGameplayCueNotify_HitImpact : public UGameplayCueNotify_Burst
{
	GENERATED_BODY()

public:
	UGGYGOGameplayCueNotify_HitImpact();

	virtual bool OnExecute_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) const override;

protected:
	/**
	 * 按表面 Tag 查反馈配置。
	 *
	 * 找不到匹配时返回 `DefaultEffect` 而不是 nullptr —— 静默不播特效
	 * 会让"忘配某种材质"表现为攻击毫无反馈，比播一个通用特效难查得多。
	 */
	const FGGYGOHitImpactEffect& ResolveEffect(const FGameplayTagContainer& TargetTags) const;

	/**
	 * 表面到反馈的映射。**按顺序匹配，第一个命中的生效**。
	 *
	 * 因此更具体的表面要排在更笼统的前面：`SurfaceType.Metal.Thin`
	 * 若排在 `SurfaceType.Metal` 之后就永远匹配不到。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hit Impact", meta = (TitleProperty = "SurfaceTag"))
	TArray<FGGYGOHitImpactEffect> SurfaceEffects;

	/** 未匹配任何表面时使用的反馈。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hit Impact")
	FGGYGOHitImpactEffect DefaultEffect;
};
