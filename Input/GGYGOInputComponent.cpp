/**
 * @file GGYGOInputComponent.cpp
 * @brief 输入组件实现
 */
#include "Input/GGYGOInputComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOInputComponent)

UGGYGOInputComponent::UGGYGOInputComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UGGYGOInputComponent::RemoveBinds(TArray<uint32>& BindHandles)
{
	for (const uint32 Handle : BindHandles)
	{
		RemoveBindingByHandle(Handle);
	}

	// 清空是必需的：调用方通常会复用同一个数组做下一次绑定，
	// 残留的旧句柄会在下次解绑时指向已经不存在的绑定。
	BindHandles.Reset();
}
