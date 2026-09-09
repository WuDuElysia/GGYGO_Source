/**
 * @file GGYGOGameplayEffectContext.cpp
 * @brief 自定义 EffectContext 的实现
 */
#include "AbilitySystem/GGYGOGameplayEffectContext.h"

#include "AbilitySystem/GGYGOAbilitySourceInterface.h"
#include "Engine/HitResult.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

#include "Iris/ReplicationState/PropertyNetSerializerInfoRegistry.h"
#include "Serialization/GameplayEffectContextNetSerializer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameplayEffectContext)

class FArchive;

FGGYGOGameplayEffectContext* FGGYGOGameplayEffectContext::ExtractEffectContext(struct FGameplayEffectContextHandle Handle)
{
	FGameplayEffectContext* BaseEffectContext = Handle.Get();

	// 同时校验对象存在与反射类型层级。只判空会把引擎基础上下文误当成本类型解释，读到错误内存布局。
	if ((BaseEffectContext != nullptr) && BaseEffectContext->GetScriptStruct()->IsChildOf(FGGYGOGameplayEffectContext::StaticStruct()))
	{
		return static_cast<FGGYGOGameplayEffectContext*>(BaseEffectContext);
	}

	return nullptr;
}

bool FGGYGOGameplayEffectContext::NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
{
	// 父类负责 Instigator、EffectCauser、HitResult 等基础字段。
	FGameplayEffectContext::NetSerialize(Ar, Map, bOutSuccess);

	// 故意不序列化的扩展字段：
	//   HitID              —— 只用于本地判定归组
	//   AbilitySourceObject —— 只在服务器结算时使用，弱引用无法可靠复制
	// 保持与引擎 EffectContext 相同的网络格式，下面的转发宏才成立。

	return true;
}

namespace UE::Net
{
	// 把 Iris 的序列化委托转发给引擎的 EffectContext 序列化器。
	// 前提是本类没有改变网络字段协议；一旦 NetSerialize 开始写扩展字段，
	// 这个转发就不够了，必须实现自定义 NetSerializer。
	UE_NET_IMPLEMENT_FORWARDING_NETSERIALIZER_AND_REGISTRY_DELEGATES(GGYGOGameplayEffectContext, FGameplayEffectContextNetSerializer);
}

void FGGYGOGameplayEffectContext::SetAbilitySource(const IGGYGOAbilitySourceInterface* InObject, float InSourceLevel)
{
	// 转成弱 UObject 指针，后续可安全检查对象是否仍存活。
	AbilitySourceObject = MakeWeakObjectPtr(Cast<const UObject>(InObject));

	// InSourceLevel 暂不保存。等确定"技能等级影响伤害"的公式形态后再决定是存在这里
	// 还是走 GE 的 SetByCaller，避免现在先加一个没有读取方的字段。
}

const IGGYGOAbilitySourceInterface* FGGYGOGameplayEffectContext::GetAbilitySource() const
{
	return Cast<IGGYGOAbilitySourceInterface>(AbilitySourceObject.Get());
}

const UPhysicalMaterial* FGGYGOGameplayEffectContext::GetPhysicalMaterial() const
{
	if (const FHitResult* HitResultPtr = GetHitResult())
	{
		return HitResultPtr->PhysMaterial.Get();
	}
	return nullptr;
}
