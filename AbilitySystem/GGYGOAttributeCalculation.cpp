/**
 * @file GGYGOAttributeCalculation.cpp
 * @brief Stateless algorithms over frozen native aggregation data.
 */
#include "AbilitySystem/GGYGOAttributeCalculation.h"

#include "GameplayEffect.h"
#include "GameplayEffectAggregator.h"

#include <limits>

namespace GGYGOAttributeCalculation
{
	// Native snapshots omit EvaluationMetaData. Preserve the original capture's already evaluated
	// qualification bits; these comparisons never dereference the copied GE tag-requirement pointers.
	bool HasSameAttributeAggregation(const FAggregator& First, const FAggregator& Second)
	{
		if (First.GetBaseValue() != Second.GetBaseValue()) { return false; }
		TMap<EGameplayModEvaluationChannel, const TArray<FAggregatorMod>*> FirstMods;
		TMap<EGameplayModEvaluationChannel, const TArray<FAggregatorMod>*> SecondMods;
		First.GetAllAggregatorMods(FirstMods);
		Second.GetAllAggregatorMods(SecondMods);
		if (FirstMods.Num() != SecondMods.Num()) { return false; }
		for (const auto& Entry : FirstMods)
		{
			const TArray<FAggregatorMod>* const* Other = SecondMods.Find(Entry.Key);
			if (!Other) { return false; }
			for (int32 Op = 0; Op < EGameplayModOp::Max; ++Op)
			{
				const TArray<FAggregatorMod>& A = Entry.Value[Op];
				const TArray<FAggregatorMod>& B = (*Other)[Op];
				if (A.Num() != B.Num()) { return false; }
				for (int32 Index = 0; Index < A.Num(); ++Index)
				{
					const FAggregatorMod& Left = A[Index];
					const FAggregatorMod& Right = B[Index];
					if (Left.Qualifies() != Right.Qualifies() || Left.IsPredicted != Right.IsPredicted
						|| Left.ActiveHandle != Right.ActiveHandle || Left.SourceTagReqs != Right.SourceTagReqs
						|| Left.TargetTagReqs != Right.TargetTagReqs
						|| FMemory::Memcmp(&Left.EvaluatedMagnitude, &Right.EvaluatedMagnitude, sizeof(float)) != 0
						|| FMemory::Memcmp(&Left.StackCount, &Right.StackCount, sizeof(float)) != 0)
					{
						return false;
					}
				}
			}
		}
		return true;
	}

	EGGYGOAttributeBaseCalculationReason PrepareNativeAttributeCalculationChannels(
		const FAggregator& QualifiedSnapshot, const FAggregatorEvaluateParameters& Parameters,
		FAggregatorModChannelContainer& OutChannels, TArray<EGameplayModEvaluationChannel>& OutOrder)
	{
		using EReason = EGGYGOAttributeBaseCalculationReason;
		TMap<EGameplayModEvaluationChannel, const TArray<FAggregatorMod>*> Mods;
		QualifiedSnapshot.GetAllAggregatorMods(Mods);
		Mods.GetKeys(OutOrder);
		OutOrder.Sort([](EGameplayModEvaluationChannel A, EGameplayModEvaluationChannel B)
		{
			return static_cast<uint8>(A) < static_cast<uint8>(B);
		});
		for (EGameplayModEvaluationChannel Channel : OutOrder)
		{
			const TArray<FAggregatorMod>* Arrays = Mods.FindChecked(Channel);
			FAggregatorModChannel& NativeChannel = OutChannels.FindOrAddModChannel(Channel);
			FAggregatorModChannel CompoundOnly;
			FAggregatorModChannel GainOnly;
			for (int32 Op = 0; Op < EGameplayModOp::Max; ++Op)
			{
				for (const FAggregatorMod& Mod : Arrays[Op])
				{
					if (!Mod.Qualifies()) { continue; }
					if (!FMath::IsFinite(Mod.EvaluatedMagnitude)) { return EReason::NonFiniteModifier; }
					if (Op == EGameplayModOp::Override) { return EReason::NonInvertibleChannel; }
					// A stack-only projection into UE's own evaluator, not a second aggregation formula.
					// Qualification is already frozen; no GE pointers/dependents or new qualification policy.
					const EGameplayModOp::Type NativeOp = static_cast<EGameplayModOp::Type>(Op);
					NativeChannel.AddMod(Mod.EvaluatedMagnitude, NativeOp, nullptr, nullptr, false,
						FActiveGameplayEffectHandle());
					if (NativeOp == EGameplayModOp::MultiplyAdditive || NativeOp == EGameplayModOp::DivideAdditive
						|| NativeOp == EGameplayModOp::MultiplyCompound)
					{
						GainOnly.AddMod(Mod.EvaluatedMagnitude, NativeOp, nullptr, nullptr, false,
							FActiveGameplayEffectHandle());
					}
					if (NativeOp == EGameplayModOp::MultiplyCompound)
					{
						CompoundOnly.AddMod(Mod.EvaluatedMagnitude, NativeOp, nullptr, nullptr, false,
							FActiveGameplayEffectHandle());
					}
				}
			}
			FAggregatorModInfo Info;
			Info.Channel = Channel;
			const auto SetQualified = [](const FAggregatorModInfo& ModInfo)
			{
				ModInfo.Mod->SetExplicitQualifies(true);
			};
			NativeChannel.ForEachMod(Info, SetQualified);
			CompoundOnly.ForEachMod(Info, SetQualified);
			GainOnly.ForEachMod(Info, SetQualified);

			// Native ReverseEvaluate silently substitutes 1 for a zero divisor. Reject before calling it.
			const float Division = FAggregatorModChannel::SumMods(Arrays[EGameplayModOp::DivideAdditive],
				GameplayEffectUtilities::GetModifierBiasByModifierOp(EGameplayModOp::DivideAdditive), Parameters);
			if (!FMath::IsFinite(Division) || FMath::IsNearlyZero(Division)) { return EReason::InvalidDivisor; }
			const float Multiplier = FAggregatorModChannel::SumMods(Arrays[EGameplayModOp::MultiplyAdditive],
				GameplayEffectUtilities::GetModifierBiasByModifierOp(EGameplayModOp::MultiplyAdditive), Parameters);
			if (!FMath::IsFinite(Multiplier)) { return EReason::NonFiniteResult; }
			if (Multiplier <= UE_SMALL_NUMBER) { return EReason::NonInvertibleChannel; }
			// The native compound product helper is private. Evaluate a native compound-only channel,
			// including its real float underflow/overflow, without copying its multiplication algorithm.
			const float Compound = CompoundOnly.EvaluateWithBase(1.0f, Parameters);
			if (!FMath::IsFinite(Compound)) { return EReason::NonFiniteResult; }
			if (Compound == 0.0f) { return EReason::NonInvertibleChannel; }
			const float Gain = GainOnly.EvaluateWithBase(1.0f, Parameters);
			if (!FMath::IsFinite(Gain)) { return EReason::NonFiniteResult; }
			if (Gain == 0.0f) { return EReason::NonInvertibleChannel; }
		}
		return EReason::None;
	}

	bool IsAttributeForwardValueConfirmed(float Actual, float Desired)
	{
		// Only float arithmetic roundoff is admitted; no gameplay clamp or success-value substitution.
		const double Scale = FMath::Max(1.0, FMath::Abs(static_cast<double>(Desired)));
		const double Tolerance = FMath::Max(static_cast<double>(UE_KINDA_SMALL_NUMBER),
			4.0 * std::numeric_limits<float>::epsilon() * Scale);
		return FMath::IsFinite(Actual)
			&& FMath::Abs(static_cast<double>(Actual) - static_cast<double>(Desired)) <= Tolerance;
	}

}
