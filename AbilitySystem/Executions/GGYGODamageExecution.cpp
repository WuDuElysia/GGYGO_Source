/**
 * @file GGYGODamageExecution.cpp
 * @brief 伤害结算实现
 */
#include "AbilitySystem/Executions/GGYGODamageExecution.h"

#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySourceInterface.h"
#include "AbilitySystem/GGYGOGameplayEffectContext.h"
#include "GameFramework/Actor.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGODamageExecution)

/**
 * 属性捕获定义。
 *
 * 必须是静态单例：捕获定义在 GE 编译期就要确定，每次结算都构造一份
 * 会让属性查找变成热路径上的字符串比较。
 */
struct FGGYGODamageStatics
{
	/** 攻击方的基础伤害输出。Snapshot=true：取施加 GE 那一刻的值。 */
	FGameplayEffectAttributeCaptureDefinition BaseDamageDef;

	/** 攻击方的基础削韧输出。 */
	FGameplayEffectAttributeCaptureDefinition BasePoiseDamageDef;

	FGGYGODamageStatics()
	{
		// Source + bSnapshot=true 的含义：在 GE **创建**时就把攻击力记下来。
		//
		// 这一点对动作游戏很关键：攻击判定与伤害落地之间有若干帧
		// （动画的判定窗口），期间攻击方的 Buff 可能已经结束。
		// 用快照保证"挥刀那一刻的攻击力"决定这次伤害，
		// 而不是"命中那一刻"—— 后者会让玩家感觉 Buff 白吃了。
		BaseDamageDef = FGameplayEffectAttributeCaptureDefinition(
			UGGYGOCombatSet::GetBaseDamageAttribute(),
			EGameplayEffectAttributeCaptureSource::Source,
			/*bSnapshot=*/true);

		BasePoiseDamageDef = FGameplayEffectAttributeCaptureDefinition(
			UGGYGOCombatSet::GetBasePoiseDamageAttribute(),
			EGameplayEffectAttributeCaptureSource::Source,
			/*bSnapshot=*/true);
	}
};

static const FGGYGODamageStatics& DamageStatics()
{
	static FGGYGODamageStatics Statics;
	return Statics;
}

UGGYGODamageExecution::UGGYGODamageExecution()
{
	RelevantAttributesToCapture.Add(DamageStatics().BaseDamageDef);
	RelevantAttributesToCapture.Add(DamageStatics().BasePoiseDamageDef);
}

void UGGYGODamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams, FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
#if WITH_SERVER_CODE
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();

	FAggregatorEvaluateParameters EvaluateParameters;
	EvaluateParameters.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	EvaluateParameters.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

	float BaseDamage = 0.0f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().BaseDamageDef, EvaluateParameters, BaseDamage);

	float BasePoiseDamage = 0.0f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().BasePoiseDamageDef, EvaluateParameters, BasePoiseDamage);

	// SetByCaller 允许单次攻击覆盖基础值，用于同一个 GE 服务多段连招
	// （每段伤害不同却共用一份 GE 配置）。没设置时取属性值。
	const float CallerDamage = Spec.GetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, /*WarnIfNotFound=*/false, -1.0f);
	if (CallerDamage >= 0.0f)
	{
		BaseDamage = CallerDamage;
	}

	const float CallerPoiseDamage = Spec.GetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, /*WarnIfNotFound=*/false, -1.0f);
	if (CallerPoiseDamage >= 0.0f)
	{
		BasePoiseDamage = CallerPoiseDamage;
	}

	// 距离与材质衰减。
	//
	// 系数由能力来源（武器、角色数据资产）提供而不是在这里写死：
	// 同一次命中打在护甲还是软肉上的减伤比例属于武器设计，
	// 而 Execution 只负责按公式合成。近战武器通常两个系数都返回 1。
	float Attenuation = 1.0f;

	if (const FGGYGOGameplayEffectContext* GGYGOContext = FGGYGOGameplayEffectContext::ExtractEffectContext(Spec.GetContext()))
	{
		if (const IGGYGOAbilitySourceInterface* AbilitySource = GGYGOContext->GetAbilitySource())
		{
			const FGameplayTagContainer* SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
			const FGameplayTagContainer* TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

			if (const UPhysicalMaterial* PhysicalMaterial = GGYGOContext->GetPhysicalMaterial())
			{
				Attenuation *= AbilitySource->GetPhysicalMaterialAttenuation(PhysicalMaterial, SourceTags, TargetTags);
			}

			// 命中位置与施加者的距离。没有命中信息时按 0 处理 ——
			// 那通常是范围伤害或状态伤害，距离衰减对它们没有意义。
			float Distance = 0.0f;
			if (GGYGOContext->HasOrigin())
			{
				Distance = FVector::Dist(GGYGOContext->GetOrigin(), Spec.GetContext().GetEffectCauser()
					? Spec.GetContext().GetEffectCauser()->GetActorLocation()
					: GGYGOContext->GetOrigin());
			}

			Attenuation *= AbilitySource->GetDistanceAttenuation(Distance, SourceTags, TargetTags);
		}
	}

	Attenuation = FMath::Max(Attenuation, 0.0f);

	const float FinalDamage = FMath::Max(BaseDamage * Attenuation, 0.0f);
	const float FinalPoiseDamage = FMath::Max(BasePoiseDamage * Attenuation, 0.0f);

	// 写元属性而不是直接改 Health / Poise。
	// 免疫判定、钳制、死亡与破韧边沿全部由 HealthSet 统一处理。
	if (FinalDamage > 0.0f)
	{
		OutExecutionOutput.AddOutputModifier(
			FGameplayModifierEvaluatedData(UGGYGOHealthSet::GetDamageAttribute(), EGameplayModOp::Additive, FinalDamage));
	}

	if (FinalPoiseDamage > 0.0f)
	{
		OutExecutionOutput.AddOutputModifier(
			FGameplayModifierEvaluatedData(UGGYGOHealthSet::GetPoiseDamageAttribute(), EGameplayModOp::Additive, FinalPoiseDamage));
	}
#endif // WITH_SERVER_CODE
}
