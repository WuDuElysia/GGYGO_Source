#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "Animation/Runtime/GGYGOMontagePlayGuard.h"

class UAnimMontage;
class UGameplayAbility;

/**
 * 一次 ASC 播放的身份：Guard 识别的本次调用创建的实例、GAS Local 播放序号与所属激活。
 * 只由 UGGYGOAbilitySystemComponent::PlayAbilityMontage 填写；复制不表示播放仍是当前播放，
 * 是否仍属于本激活以 ASC::IsPlaybackCurrent 为准。
 */
struct GGYGO_API FGGYGOAbilityMontagePlayback
{
	FGGYGOMontagePlayGuardIdentity Guard{};
	/** 项目能力的那次激活；非项目能力为空，此时只要求能力仍在运行。 */
	FGGYGOAbilityActivationHandle Activation{};
	TWeakObjectPtr<UGameplayAbility> Ability;
	TWeakObjectPtr<UAnimMontage> Montage;
	uint8 LocalPlayInstanceId = 0;

	bool HasPlayback() const { return Guard.CreatedInstanceId != INDEX_NONE && !Ability.IsExplicitlyNull(); }
};

/** PlayAbilityMontage 的结果副本。Playback 为空表示没有取得本次播放；Guard 保留拒绝原因。 */
struct GGYGO_API FGGYGOAbilityMontagePlaybackResult
{
	FGGYGOMontagePlayGuardResult Guard{};
	/** Guard 接受时的原生返回值；其它情况为 0。 */
	float Duration = 0.0f;
	FGGYGOAbilityMontagePlayback Playback{};
};
