/**
 * @file GGYGOInputConfig.cpp
 * @brief 输入映射表实现
 */
#include "Input/GGYGOInputConfig.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOInputConfig)

UGGYGOInputConfig::UGGYGOInputConfig(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

const UInputAction* UGGYGOInputConfig::FindNativeInputActionForTag(const FGameplayTag& InputTag, bool bLogNotFound) const
{
	for (const FGGYGOInputAction& Action : NativeInputActions)
	{
		// 用 MatchesTagExact 而不是 MatchesTag：InputTag 的层级不表示"包含关系"。
		// `InputTag.Look.Mouse` 与 `InputTag.Look.Stick` 需要不同的灵敏度处理，
		// 用宽松匹配会让前者的绑定意外接管后者。
		if (Action.InputAction && Action.InputTag.MatchesTagExact(InputTag))
		{
			return Action.InputAction;
		}
	}

	if (bLogNotFound)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("FindNativeInputActionForTag: 输入配置 [%s] 里没有 Tag [%s] 对应的 Native 动作。"),
			*GetNameSafe(this), *InputTag.ToString());
	}

	return nullptr;
}

const UInputAction* UGGYGOInputConfig::FindAbilityInputActionForTag(const FGameplayTag& InputTag, bool bLogNotFound) const
{
	for (const FGGYGOInputAction& Action : AbilityInputActions)
	{
		if (Action.InputAction && Action.InputTag.MatchesTagExact(InputTag))
		{
			return Action.InputAction;
		}
	}

	if (bLogNotFound)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("FindAbilityInputActionForTag: 输入配置 [%s] 里没有 Tag [%s] 对应的 Ability 动作。"),
			*GetNameSafe(this), *InputTag.ToString());
	}

	return nullptr;
}
