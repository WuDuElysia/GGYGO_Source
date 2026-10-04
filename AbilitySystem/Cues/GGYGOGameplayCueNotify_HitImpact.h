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
 * 本类改为让**一个** Cue 内部分流：攻击方只发一个"命中了"的 Cue。
 * `UGGYGOGameplayAbility::BuildHitEffectPayload` 从同一次 HitResult 把物理材质 Tag
 * 汇入 Cue 的目标 Tag；有 GE 时同一 Tag 也进入 EffectSpec。加一种表面只需在本资产的映射表里加一行。
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

/** 位置来源是资产选择的正常模式，缺失所选输入时不切换模式。 */
UENUM(BlueprintType)
enum class EGGYGOHitImpactLocationMode : uint8
{
	HitResult,
	ParametersLocation,
	TargetCenter
};

/** 普通物理材质或无 SurfaceType Tag 的载荷如何表现。 */
UENUM(BlueprintType)
enum class EGGYGOHitImpactNoSurfacePolicy : uint8
{
	Generic,
	Reject
};

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

/** 仅描述本次表现输入，不拥有 Combat 命中或伤害状态。 */
enum class EGGYGOHitImpactResolveFailure : uint8
{
	Unresolved,
	None,
	InvalidTarget,
	InvalidWorld,
	InvalidLocationMode,
	MissingHitResult,
	NonFiniteLocation,
	NonFiniteNormal,
	InvalidSurfaceConfiguration,
	MissingSurfaceTag,
	UnmatchedSurfaceTag,
	InvalidSelectedResource,
	InheritedPlacementConflict,
	InheritedConfigurationUnavailable,
	TargetExpiredDuringFeedback
};

/** Effect 仅借用当前 Cue 配置；调用方须在任何外调前复制所需资源。 */
struct GGYGO_API FGGYGOHitImpactResolveResult
{
	EGGYGOHitImpactResolveFailure Failure = EGGYGOHitImpactResolveFailure::Unresolved;
	FVector Location = FVector::ZeroVector;
	FVector Normal = FVector::ZeroVector;
	const FGGYGOHitImpactEffect* Effect = nullptr;
	FString Detail;

	bool IsSuccess() const { return Failure == EGGYGOHitImpactResolveFailure::None && Effect != nullptr; }
};

UCLASS(Blueprintable, meta = (DisplayName = "GGYGO Hit Impact Cue", ShortTooltip = "按命中表面分流的命中特效"))
class GGYGO_API UGGYGOGameplayCueNotify_HitImpact : public UGameplayCueNotify_Burst
{
	GENERATED_BODY()

public:
	UGGYGOGameplayCueNotify_HitImpact();

	/** Executed 先验证，再允许引擎派发 K2_HandleGameplayCue / OnExecute。 */
	virtual void HandleGameplayCue(AActor* Target, EGameplayCueEvent::Type EventType,
		const FGameplayCueParameters& Parameters) override;

	virtual bool OnExecute_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) const override;

	/** 无播放、日志或 Context 写入；原点与 Overlap 不因坐标/Blocking 标志被判非法。 */
	FGGYGOHitImpactResolveResult ResolveImpactPresentation(AActor* Target,
		const FGameplayCueParameters& Parameters) const;

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hit Impact")
	EGGYGOHitImpactLocationMode LocationMode = EGGYGOHitImpactLocationMode::HitResult;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hit Impact")
	EGGYGOHitImpactNoSurfacePolicy NoSurfacePolicy = EGGYGOHitImpactNoSurfacePolicy::Generic;

	/** 有 SurfaceType Tag 却无匹配时，只有资产明确启用才允许通用反馈。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hit Impact")
	bool bAllowUnmatchedSurfaceGeneric = false;

	/**
	 * 表面到反馈的映射。**按顺序匹配，第一个命中的生效**。
	 *
	 * 因此更具体的表面要排在更笼统的前面：`SurfaceType.Metal.Thin`
	 * 若排在 `SurfaceType.Metal` 之后就永远匹配不到。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hit Impact", meta = (TitleProperty = "SurfaceTag"))
	TArray<FGGYGOHitImpactEffect> SurfaceEffects;

	/** 仅在 NoSurfacePolicy=Generic 或明确允许未匹配表面时使用。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hit Impact")
	FGGYGOHitImpactEffect DefaultEffect;

private:
	bool ValidateInheritedPlacement(AActor* Target, const FGameplayCueParameters& Parameters,
		const FVector& SelectedLocation, FGGYGOHitImpactResolveResult& OutResult) const;
	void ReportFailure(AActor* Target, const FGameplayCueParameters& Parameters,
		const FGGYGOHitImpactResolveResult& Result) const;

	/** 非权威诊断标记：本 Cue 对象生命周期每种原因最多报告一次，不保存目标或计时器。 */
	mutable uint32 ReportedFailureReasons = 0;
};
