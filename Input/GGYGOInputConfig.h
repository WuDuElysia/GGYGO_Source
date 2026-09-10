/**
 * @file GGYGOInputConfig.h
 * @brief InputTag 与 InputAction 的映射表
 *
 * 输入绑定分成两类，因为它们的去向完全不同：
 *
 * - **Native**：直接绑到 C++ 函数上。移动与视角属于这一类 ——
 *   它们需要每帧连续的轴值，而 GAS 的能力激活是离散事件，表达不了"持续推摇杆"。
 * - **Ability**：只翻译成 InputTag 交给 ASC，由 ASC 去匹配持有该 Tag 的能力。
 *   攻击、闪避、技能属于这一类。
 *
 * ## 为什么要 InputTag 这层间接
 * 直接把 InputAction 绑到具体能力上，会让"哪个键放哪个技能"这件事
 * 固化在角色蓝图里：换角色要重配一遍按键，同一个键在不同形态下放不同技能
 * 也只能靠运行时判断绕开。
 *
 * 有了 InputTag 之后，按键与能力各自只认 Tag：按键配置说"这个键发出
 * `InputTag.Attack.Light`"，能力配置说"我响应 `InputTag.Attack.Light`"，
 * 两边可以独立替换。
 */
#pragma once

#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"

#include "GGYGOInputConfig.generated.h"

class UInputAction;
class UObject;

/** 一条输入绑定。 */
USTRUCT(BlueprintType)
struct FGGYGOInputAction
{
	GENERATED_BODY()

	/** Enhanced Input 的动作资产。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<const UInputAction> InputAction = nullptr;

	/** 该动作对外发出的 Tag。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (Categories = "InputTag"))
	FGameplayTag InputTag;
};

UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Input Config", ShortTooltip = "InputTag 与 InputAction 的映射表"))
class GGYGO_API UGGYGOInputConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOInputConfig(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * 按 Tag 查 Native 动作。
	 *
	 * @param bLogNotFound 找不到时是否记录错误。批量绑定时应当为 true ——
	 *                     配置漏项若静默失败，表现为"某个键没反应"，比报错难查。
	 */
	const UInputAction* FindNativeInputActionForTag(const FGameplayTag& InputTag, bool bLogNotFound = true) const;

	/** 按 Tag 查 Ability 动作。 */
	const UInputAction* FindAbilityInputActionForTag(const FGameplayTag& InputTag, bool bLogNotFound = true) const;

	/**
	 * 直连 C++ 函数的输入。
	 *
	 * 适合需要连续轴值或必须在移动组件之前生效的输入。
	 * 每条都要在 `UGGYGOHeroComponent` 里显式写一次绑定代码，不能批量处理 ——
	 * 因为它们各自的回调签名与语义都不同。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input", meta = (TitleProperty = "InputAction"))
	TArray<FGGYGOInputAction> NativeInputActions;

	/**
	 * 翻译成 InputTag 交给 ASC 的输入。
	 *
	 * 这些是批量绑定的：按下与释放各走一个统一的处理函数，
	 * 由 ASC 按 Tag 找到对应能力。新增一个技能键只需在这里加一行，
	 * 不需要改任何 C++。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input", meta = (TitleProperty = "InputAction"))
	TArray<FGGYGOInputAction> AbilityInputActions;
};
