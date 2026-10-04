/**
 * @file GGYGODamageExecution.cpp
 * @brief 伤害结算实现
 */
#include "AbilitySystem/Executions/GGYGODamageExecution.h"

#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOAbilitySourceInterface.h"
#include "AbilitySystem/GGYGOGameplayEffectContext.h"
#include "AbilitySystemComponent.h"
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

	// 键存在就是明确覆盖；缺键才要求捕获，不能用负数哨兵把非法覆盖混成缺键。
	struct FBaseInputResult
	{
		float Value = 0.0f;
		const TCHAR* Source = TEXT("CapturedAttribute");
		const TCHAR* Reason = TEXT("capture-failed");
		bool bHasValue = false;
		bool bValid = false;
	};
	auto ResolveBaseInput = [&Spec, &ExecutionParams, &EvaluateParameters](const FGameplayTag& OverrideTag,
		const FGameplayEffectAttributeCaptureDefinition& CaptureDefinition)
	{
		FBaseInputResult Result;
		if (const float* OverrideValue = Spec.SetByCallerTagMagnitudes.Find(OverrideTag))
		{
			Result.Source = TEXT("SetByCallerTag");
			Result.Value = *OverrideValue;
			Result.bHasValue = true;
			if (!FMath::IsFinite(Result.Value))
			{
				Result.Reason = TEXT("non-finite");
				return Result;
			}
			if (Result.Value < 0.0f)
			{
				Result.Reason = TEXT("negative-override");
				return Result;
			}
		}
		else
		{
			if (!ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(CaptureDefinition, EvaluateParameters, Result.Value))
			{
				return Result;
			}
			Result.bHasValue = true;
			if (!FMath::IsFinite(Result.Value))
			{
				Result.Reason = TEXT("non-finite");
				return Result;
			}
			// 有限负捕获值仍交给既有最终非负公式，不在这里新增属性值域规则。
		}
		Result.Reason = TEXT("valid");
		Result.bValid = true;
		return Result;
	};
	const FBaseInputResult DamageInput = ResolveBaseInput(GGYGOGameplayTags::SetByCaller_Damage, DamageStatics().BaseDamageDef);
	const FBaseInputResult PoiseInput = ResolveBaseInput(GGYGOGameplayTags::SetByCaller_PoiseDamage, DamageStatics().BasePoiseDamageDef);
	if (!DamageInput.bValid || !PoiseInput.bValid)
	{
		const FString DamageValue = DamageInput.bHasValue ? FString::Printf(TEXT("%.9g"), static_cast<double>(DamageInput.Value)) : TEXT("unavailable");
		const FString PoiseValue = PoiseInput.bHasValue ? FString::Printf(TEXT("%.9g"), static_cast<double>(PoiseInput.Value)) : TEXT("unavailable");
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DamageExecution [%s] 拒绝基础输入：GE=%s SourceASC=%s TargetASC=%s BaseDamage{Source=%s Reason=%s Value=%s} BasePoiseDamage{Source=%s Reason=%s Value=%s}；本Execution不新增Modifier。"),
			*GetPathName(), *GetPathNameSafe(Spec.Def.Get()),
			*GetPathNameSafe(ExecutionParams.GetSourceAbilitySystemComponent()), *GetPathNameSafe(ExecutionParams.GetTargetAbilitySystemComponent()),
			DamageInput.Source, DamageInput.Reason, *DamageValue, PoiseInput.Source, PoiseInput.Reason, *PoiseValue);
		// 原生Execute没有GE应用失败返回值；不改写Spec，也不清除调用方已有输出。
		return;
	}
	const float BaseDamage = DamageInput.Value;
	const float BasePoiseDamage = PoiseInput.Value;

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

			// 攻击来源与命中点的距离。没有显式来源或命中信息时按 0 处理 ——
			// 那通常是范围伤害或状态伤害，距离衰减对它们没有意义。
			const float Distance = GGYGOContext->GetDistanceFromOriginToHitResult();

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
