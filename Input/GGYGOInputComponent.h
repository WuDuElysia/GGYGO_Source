/**
 * @file GGYGOInputComponent.h
 * @brief 按 InputTag 查找 Native 动作，并提供绑定句柄的整批清理。
 *
 * 绑定所有者记录自己创建的句柄，并在其输入会话结束时调用 RemoveBinds。
 * Ability 动作使用继承的 ActionInstance 绑定接口，由调用者保留原始动作身份。
 */
#pragma once

#include "EnhancedInputComponent.h"
#include "Input/GGYGOInputConfig.h"

#include "GGYGOInputComponent.generated.h"

class UInputAction;

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
	 * @param BindHandles 可选输出。仅追加成功创建的绑定句柄，不清空已有记录。
	 */
	template <class UserClass, typename FuncType>
	void BindNativeAction(const UGGYGOInputConfig* InputConfig, const FGameplayTag& InputTag, ETriggerEvent TriggerEvent, UserClass* Object, FuncType Func, bool bLogIfNotFound, TArray<uint32>* BindHandles = nullptr);

	/** 按句柄整批解绑，并清空句柄数组。 */
	void RemoveBinds(TArray<uint32>& BindHandles);
};

template <class UserClass, typename FuncType>
void UGGYGOInputComponent::BindNativeAction(const UGGYGOInputConfig* InputConfig, const FGameplayTag& InputTag, ETriggerEvent TriggerEvent, UserClass* Object, FuncType Func, bool bLogIfNotFound, TArray<uint32>* BindHandles)
{
	check(InputConfig);

	if (const UInputAction* InputAction = InputConfig->FindNativeInputActionForTag(InputTag, bLogIfNotFound))
	{
		const uint32 BindHandle = BindAction(InputAction, TriggerEvent, Object, Func).GetHandle();
		if (BindHandles)
		{
			BindHandles->Add(BindHandle);
		}
	}
}
