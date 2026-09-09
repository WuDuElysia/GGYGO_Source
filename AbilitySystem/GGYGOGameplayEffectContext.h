/**
 * @file GGYGOGameplayEffectContext.h
 * @brief 自定义 GameplayEffectContext —— 携带能力来源与命中材质
 *
 * GAS 原生的 `FGameplayEffectContext` 只带 Instigator / EffectCauser / HitResult。
 * 动作游戏的伤害结算还需要知道"这次伤害是谁发出的、打在什么材质上"，用来做：
 *   - 部位倍率与材质减伤（`GGYGODamageExecution` 通过 `GetAbilitySource()` 取衰减系数）
 *   - 命中反馈分流（打金属出火花、打肉出血，由 `GetPhysicalMaterial()` 决定播哪个 Cue）
 *
 * 由 `UGGYGOAbilitySystemGlobals::AllocGameplayEffectContext()` 统一创建，
 * 因此项目内所有 EffectContext 都是本类型，`ExtractEffectContext` 才能稳定拿到扩展字段。
 *
 * 网络边界：`AbilitySourceObject` 是弱引用且**不复制**，只在服务器有效。
 * 客户端预测路径不能依赖它，需要的信息应通过 GE 的 SetByCaller 或 Cue 参数传递。
 */
#pragma once

#include "GameplayEffectTypes.h"

#include "GGYGOGameplayEffectContext.generated.h"

class AActor;
class FArchive;
class IGGYGOAbilitySourceInterface;
class UObject;
class UPhysicalMaterial;

/** GGYGO 的 EffectContext。字段布局与父类兼容，只增加来源对象与命中序号。 */
USTRUCT()
struct FGGYGOGameplayEffectContext : public FGameplayEffectContext
{
	GENERATED_BODY()

	/** 默认构造。供工厂分配、Duplicate 和网络接收使用。 */
	FGGYGOGameplayEffectContext()
		: FGameplayEffectContext()
	{
	}

	/**
	 * 带来源的构造。
	 * @param InInstigator   逻辑发起者，通常是角色或其 PlayerState。
	 * @param InEffectCauser 物理造成者，通常是武器 / 判定体 Actor。
	 */
	FGGYGOGameplayEffectContext(AActor* InInstigator, AActor* InEffectCauser)
		: FGameplayEffectContext(InInstigator, InEffectCauser)
	{
	}

	/**
	 * 从通用句柄取出 GGYGO 扩展上下文。
	 * @return 类型匹配时返回句柄内部对象的指针（不转移所有权，只在句柄存活期间可用）；
	 *         句柄为空或仍是引擎基础类型时返回 nullptr。
	 */
	static GGYGO_API FGGYGOGameplayEffectContext* ExtractEffectContext(struct FGameplayEffectContextHandle Handle);

	/**
	 * 记录能力来源。
	 * @param InObject      实现 `IGGYGOAbilitySourceInterface` 的对象，以弱引用保存，不延长其生命周期。
	 * @param InSourceLevel 来源等级，预留给按等级缩放的公式，当前不保存。
	 */
	void SetAbilitySource(const IGGYGOAbilitySourceInterface* InObject, float InSourceLevel);

	/** 取回能力来源接口。对象已销毁、未设置或在客户端时返回 nullptr。 */
	const IGGYGOAbilitySourceInterface* GetAbilitySource() const;

	/**
	 * 深拷贝上下文。GAS 在把 Spec 复制给多个目标时调用。
	 * HitResult 必须深拷贝，否则源上下文析构后新上下文会持有悬空命中数据。
	 */
	virtual FGameplayEffectContext* Duplicate() const override
	{
		FGGYGOGameplayEffectContext* NewContext = new FGGYGOGameplayEffectContext();
		*NewContext = *this;
		if (GetHitResult())
		{
			NewContext->AddHitResult(*GetHitResult(), /*bReset=*/true);
		}
		return NewContext;
	}

	/** 返回本类型的反射结构，`ExtractEffectContext` 依赖它做类型判定。 */
	virtual UScriptStruct* GetScriptStruct() const override
	{
		return FGGYGOGameplayEffectContext::StaticStruct();
	}

	/**
	 * 网络序列化。当前只转发父类，扩展字段一律不上网：
	 * `HitID` 属于本地/TargetData 路径，`AbilitySourceObject` 只在服务器结算时使用。
	 * 若以后要复制扩展字段，必须同时实现自定义 Iris NetSerializer（见 .cpp 末尾的转发宏说明）。
	 */
	virtual bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess) override;

	/** 从 HitResult 取物理材质。没有命中或材质缺失时返回 nullptr。 */
	const UPhysicalMaterial* GetPhysicalMaterial() const;

public:
	/**
	 * 同一次攻击判定的编号。
	 * 一次挥砍可能命中多个目标、或一个多段判定体产生多条 HitResult，
	 * 用同一个 HitID 把它们归组，便于做"同一刀对同一目标只结算一次"。
	 * -1 表示未设置。当前不参与网络复制。
	 */
	UPROPERTY()
	int32 HitID = -1;

protected:
	/**
	 * 能力来源对象（应实现 `IGGYGOAbilitySourceInterface`）。
	 * 弱引用，避免上下文延长来源生命周期；当前不复制，客户端读不到。
	 */
	UPROPERTY()
	TWeakObjectPtr<const UObject> AbilitySourceObject;
};

/** 告知 GAS 本结构自带网络序列化且支持值拷贝，否则 Duplicate 与容器复制会丢掉扩展字段。 */
template<>
struct TStructOpsTypeTraits<FGGYGOGameplayEffectContext> : public TStructOpsTypeTraitsBase2<FGGYGOGameplayEffectContext>
{
	enum
	{
		WithNetSerializer = true,
		WithCopy = true
	};
};
