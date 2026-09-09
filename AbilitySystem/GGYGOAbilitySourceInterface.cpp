/**
 * @file GGYGOAbilitySourceInterface.cpp
 * @brief 能力来源接口的生成代码宿主
 *
 * 接口本身没有实现，但这个 .cpp 不能省：
 * `UINTERFACE` 会生成 `UGGYGOAbilitySourceInterface(const FObjectInitializer&)` 构造函数，
 * 需要一个编译单元来展开它，否则链接时报"无法解析的外部符号"。
 */
#include "AbilitySystem/GGYGOAbilitySourceInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilitySourceInterface)

/**
 * 接口包装类的构造函数。
 * `GENERATED_UINTERFACE_BODY()` 只声明它，定义必须写在这里，
 * 否则反射系统的 InternalConstructor 会找不到符号。
 * 这里只建立 UObject 生命周期，衰减逻辑全在实现方。
 */
UGGYGOAbilitySourceInterface::UGGYGOAbilitySourceInterface(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}
