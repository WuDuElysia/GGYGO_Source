/**
 * @file GGYGOGameplayCueNotify_HitImpact.cpp
 * @brief 命中特效分流实现
 */
#include "AbilitySystem/Cues/GGYGOGameplayCueNotify_HitImpact.h"

#include "Kismet/GameplayStatics.h"
#include "NiagaraFunctionLibrary.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameplayCueNotify_HitImpact)

UGGYGOGameplayCueNotify_HitImpact::UGGYGOGameplayCueNotify_HitImpact()
{
}

const FGGYGOHitImpactEffect& UGGYGOGameplayCueNotify_HitImpact::ResolveEffect(const FGameplayTagContainer& TargetTags) const
{
	for (const FGGYGOHitImpactEffect& Effect : SurfaceEffects)
	{
		// MatchesTag（层级匹配）而非 MatchesTagExact：配 SurfaceType.Metal
		// 就能覆盖它的所有子类型，细分表面时不必逐个补配置。
		if (Effect.SurfaceTag.IsValid() && TargetTags.HasTag(Effect.SurfaceTag))
		{
			return Effect;
		}
	}

	return DefaultEffect;
}

bool UGGYGOGameplayCueNotify_HitImpact::OnExecute_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) const
{
	// 先让父类处理它自己配置的通用反馈（相机震动、力反馈等）。
	// 那些不随表面变化，配在父类的字段里更合适。
	Super::OnExecute_Implementation(Target, Parameters);

	const UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	// 表面 Tag 来自 GE 的目标 Tag。攻击能力在构造 GE Spec 时
	// 已经把命中处的物理材质 Tag 并进去了。
	const FGameplayTagContainer& TargetTags = Parameters.AggregatedTargetTags;
	const FGGYGOHitImpactEffect& Effect = ResolveEffect(TargetTags);

	// 位置优先用命中点。没有命中信息时（范围伤害、状态伤害）退回目标位置 ——
	// 用零向量会把特效放到世界原点。
	// Parameters.Location 是 FVector_NetQuantize10（为节省带宽做了量化），
	// 需要显式转成 FVector 才能参与三元运算。
	const FVector ImpactLocation = Parameters.Location.IsNearlyZero()
		? Target->GetActorLocation()
		: FVector(Parameters.Location);

	// 朝向沿命中法线：命中特效要背离表面喷出，否则火花会射进墙里。
	const FRotator ImpactRotation = Parameters.Normal.IsNearlyZero()
		? FRotator::ZeroRotator
		: Parameters.Normal.Rotation();

	if (Effect.ImpactEffect)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World,
			Effect.ImpactEffect,
			ImpactLocation,
			ImpactRotation);
	}

	if (Effect.ImpactSound)
	{
		UGameplayStatics::PlaySoundAtLocation(World, Effect.ImpactSound, ImpactLocation);
	}

	return true;
}
