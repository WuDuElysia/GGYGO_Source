/**
 * @file GGYGOInputComponent.h
 * @brief 支持按 InputTag 批量绑定的输入组件
 *
 * 在 `UEnhancedInputComponent` 之上加两件事：按 Tag 查找动作再绑定，
 * 以及记录绑定句柄以便整批解绑。
 *
 * 后者是队伍换角色必需的：换人时要把上一个角色的能力输入全部解绑，
 * 否则旧绑定仍会把输入发给已经不受控的角色。逐个记住绑了什么不现实，
 * 所以批量绑定时统一收集句柄。
 */
#pragma once

#include "EnhancedInputComponent.h"
#include "Input/GGYGOInputConfig.h"

#include "GGYGOInputComponent.generated.h"

class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;
class UObject;

UCLASS(Config = Input)
class GGYGO_API UGGYGOInputComponent : public UEnhancedInputComponent
{
	GENERATED_BODY()

public:
	UGGYGOInputComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * 绑定一条 Native 输入。
	 *
	 * @param bLogIfNotFound 配置里找不到该 Tag 时是否报错。
	 *                       移动、视角这类必需输入应当为 true；
	 *                       可选输入（某些角色才有）传 false 以免刷错误日志。
	 */
	template <class UserClass, typename FuncType>
	void BindNativeAction(const UGGYGOInputConfig* InputConfig, const FGameplayTag& InputTag, ETriggerEvent TriggerEvent, UserClass* Object, FuncType Func, bool bLogIfNotFound);

	/**
	 * 批量绑定所有 Ability 输入。
	 *
	 * 按下与释放各绑一个统一处理函数，处理函数收到的参数是 InputTag，
	 * 由它转交给 ASC。因此新增技能键不需要改这里的代码。
	 *
	 * @param BindHandles 输出。收集本次产生的全部句柄，供 `RemoveBinds` 整批解绑。
	 */
	template <class UserClass, typename PressedFuncType, typename ReleasedFuncType>
	void BindAbilityActions(const UGGYGOInputConfig* InputConfig, UserClass* Object, PressedFuncType PressedFunc, ReleasedFuncType ReleasedFunc, TArray<uint32>& BindHandles);

	/** 按句柄整批解绑，并清空句柄数组。 */
	void RemoveBinds(TArray<uint32>& BindHandles);
};

template <class UserClass, typename FuncType>
void UGGYGOInputComponent::BindNativeAction(const UGGYGOInputConfig* InputConfig, const FGameplayTag& InputTag, ETriggerEvent TriggerEvent, UserClass* Object, FuncType Func, bool bLogIfNotFound)
{
	check(InputConfig);

	if (const UInputAction* InputAction = InputConfig->FindNativeInputActionForTag(InputTag, bLogIfNotFound))
	{
		BindAction(InputAction, TriggerEvent, Object, Func);
	}
}

template <class UserClass, typename PressedFuncType, typename ReleasedFuncType>
void UGGYGOInputComponent::BindAbilityActions(const UGGYGOInputConfig* InputConfig, UserClass* Object, PressedFuncType PressedFunc, ReleasedFuncType ReleasedFunc, TArray<uint32>& BindHandles)
{
	check(InputConfig);

	for (const FGGYGOInputAction& Action : InputConfig->AbilityInputActions)
	{
		if (!Action.InputAction || !Action.InputTag.IsValid())
		{
			continue;
		}

		if (PressedFunc)
		{
			// Triggered 而不是 Started：Started 只在触发器状态首次变化时发一次，
			// 而某些触发器（如 Hold）的"按下"语义要靠 Triggered 才能拿到。
			BindHandles.Add(BindAction(Action.InputAction, ETriggerEvent::Triggered, Object, PressedFunc, Action.InputTag).GetHandle());
		}

		if (ReleasedFunc)
		{
			BindHandles.Add(BindAction(Action.InputAction, ETriggerEvent::Completed, Object, ReleasedFunc, Action.InputTag).GetHandle());
		}
	}
}
