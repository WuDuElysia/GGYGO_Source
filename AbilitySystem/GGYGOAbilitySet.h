/**
 * @file GGYGOAbilitySet.h
 * @brief 成组授予 Ability / Effect / AttributeSet 的数据资产
 *
 * 解决的问题：一个角色可能有十几个能力、几个初始化 GE、两三个 AttributeSet。
 * 逐个 `GiveAbility` 不仅啰嗦，更麻烦的是**回收**——换角色时要精确移除刚才给的那一批，
 * 手工记录句柄很容易漏。
 *
 * 本资产把"给"和"收"配成一对：
 *   `GiveToAbilitySystem` 授予并把所有句柄写进 `FGGYGOAbilitySet_GrantedHandles`，
 *   之后 `TakeFromAbilitySystem` 就能一次性精确回收，不会误伤其他来源授予的东西。
 *
 * ## 在队伍多角色架构里的作用（决策 D1）
 * 每个角色 Pawn 有自己的 ASC。切换出场角色时，
 * 上场角色的技能集用 AbilitySet 授予、下场时整组回收，是这套机制的主要用途。
 *
 * ## 授予顺序
 * AttributeSet → Ability → GameplayEffect。不能乱：
 * 能力可能引用属性，初始化 GE 必然要写属性，所以属性存储必须先就位。
 *
 * ## 权限
 * 只有服务器能授予和回收。客户端调用会直接返回，属性与能力靠 GAS 复制同步过去。
 */
#pragma once

#include "ActiveGameplayEffectHandle.h"
#include "AttributeSet.h"
#include "Engine/DataAsset.h"
#include "GameplayAbilitySpecHandle.h"
#include "GameplayTagContainer.h"

#include "GGYGOAbilitySet.generated.h"

class UAttributeSet;
class UGameplayEffect;
class UGGYGOAbilitySystemComponent;
class UGGYGOGameplayAbility;
class UObject;

/** 一条能力配置。 */
USTRUCT(BlueprintType)
struct FGGYGOAbilitySet_GameplayAbility
{
	GENERATED_BODY()

	/** 要授予的能力类。 */
	UPROPERTY(EditDefaultsOnly)
	TSubclassOf<UGGYGOGameplayAbility> Ability;

	/** 授予等级。 */
	UPROPERTY(EditDefaultsOnly)
	int32 AbilityLevel = 1;

	/**
	 * 绑定的输入 Tag。
	 * 授予时写进 AbilitySpec 的动态源标签，ASC 的
	 * `AbilityInputTagPressed` 靠**精确匹配**这个 Tag 找到对应能力。
	 * 留空表示该能力不由输入触发（被动能力、事件触发能力）。
	 */
	UPROPERTY(EditDefaultsOnly, Meta = (Categories = "InputTag"))
	FGameplayTag InputTag;
};

/** 一条 GameplayEffect 配置。 */
USTRUCT(BlueprintType)
struct FGGYGOAbilitySet_GameplayEffect
{
	GENERATED_BODY()

	/** 要应用的 GE 类。通常是属性初始化 GE。 */
	UPROPERTY(EditDefaultsOnly)
	TSubclassOf<UGameplayEffect> GameplayEffect;

	/** GE 等级，影响其数值计算。 */
	UPROPERTY(EditDefaultsOnly)
	float EffectLevel = 1.0f;
};

/** 一条 AttributeSet 配置。 */
USTRUCT(BlueprintType)
struct FGGYGOAbilitySet_AttributeSet
{
	GENERATED_BODY()

	/** 要实例化并挂到 ASC 的属性集类。 */
	UPROPERTY(EditDefaultsOnly)
	TSubclassOf<UAttributeSet> AttributeSet;
};

/**
 * 一次授予产生的全部运行时句柄。
 * 调用方需要自己保存它，否则无法回收。
 */
USTRUCT(BlueprintType)
struct FGGYGOAbilitySet_GrantedHandles
{
	GENERATED_BODY()

	/** 记录能力句柄。无效句柄不入列。 */
	void AddAbilitySpecHandle(const FGameplayAbilitySpecHandle& Handle);

	/** 记录效果句柄。无效句柄不入列。 */
	void AddGameplayEffectHandle(const FActiveGameplayEffectHandle& Handle);

	/** 记录属性集实例。调用方不得自行销毁它，回收时要用同一指针。 */
	void AddAttributeSet(UAttributeSet* Set);

	/**
	 * 从 ASC 回收本次授予的一切。
	 * 顺序与授予相反：先清能力（结束依赖它们的激活），再移除效果，最后摘属性集。
	 * 非服务器调用直接返回且**保留**本地句柄，避免客户端误认为已回收。
	 */
	void TakeFromAbilitySystem(UGGYGOAbilitySystemComponent* GGYGOASC);

protected:
	UPROPERTY()
	TArray<FGameplayAbilitySpecHandle> AbilitySpecHandles;

	UPROPERTY()
	TArray<FActiveGameplayEffectHandle> GameplayEffectHandles;

	UPROPERTY()
	TArray<TObjectPtr<UAttributeSet>> GrantedAttributeSets;
};

UCLASS(BlueprintType, Const)
class GGYGO_API UGGYGOAbilitySet : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOAbilitySet(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * 把本资产的内容授予给 ASC。仅服务器有效。
	 *
	 * @param GGYGOASC         目标 ASC，不能为空。
	 * @param OutGrantedHandles 可为 nullptr。**传 nullptr 就无法回收**，只在明确不需要回收时这么做。
	 * @param SourceObject      写进每个 AbilitySpec，让能力能追溯"这是谁给我的"
	 *                          （武器、装备、角色数据资产）。
	 *
	 * 单项配置无效时只记录错误并跳过，不中断其余授予——所以可能出现部分成功。
	 */
	void GiveToAbilitySystem(UGGYGOAbilitySystemComponent* GGYGOASC, FGGYGOAbilitySet_GrantedHandles* OutGrantedHandles, UObject* SourceObject = nullptr) const;

protected:
	/** 要授予的能力。按数组顺序处理。 */
	UPROPERTY(EditDefaultsOnly, Category = "Gameplay Abilities", meta = (TitleProperty = Ability))
	TArray<FGGYGOAbilitySet_GameplayAbility> GrantedGameplayAbilities;

	/** 要应用的效果。在能力授予完成后处理。 */
	UPROPERTY(EditDefaultsOnly, Category = "Gameplay Effects", meta = (TitleProperty = GameplayEffect))
	TArray<FGGYGOAbilitySet_GameplayEffect> GrantedGameplayEffects;

	/** 要挂载的属性集。**最先**处理，因为能力和效果都可能依赖它们。 */
	UPROPERTY(EditDefaultsOnly, Category = "Attribute Sets", meta = (TitleProperty = AttributeSet))
	TArray<FGGYGOAbilitySet_AttributeSet> GrantedAttributes;
};
