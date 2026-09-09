// GGYGOAttributeSet.h
// GAS 属性集 - Model 层数据容器
// 所有角色的数值属性都在这里定义
// 属性只能通过 GameplayEffect 修改，禁止直接赋值

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "GGYGOAttributeSet.generated.h"

// 宏：自动生成 GetXXX()、SetXXX()、InitXXX() 访问器函数
// GAS 框架要求每个属性都必须有这些访问器
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

UCLASS()
class GGYGO_API UGGYGOAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UGGYGOAttributeSet();

	// ============================================================
	// 基础属性
	// 初始值不在这里硬编码
	// 每个角色蓝图配置自己的 GE_InitAttributes
	// 在 BeginPlay 时通过 ApplyDefaultEffects 应用初始值
	// ============================================================

	// 当前血量，限制在 0 ~ MaxHealth 之间
	// 降到 0 时 BaseCharacter 广播 OnHealthDepleted 委托
	UPROPERTY(BlueprintReadOnly, Category = "Health")
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS(UGGYGOAttributeSet, Health)

	// 最大血量，作为 Health 的上限
	UPROPERTY(BlueprintReadOnly, Category = "Health")
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS(UGGYGOAttributeSet, MaxHealth)

	// 基础攻击力，用于伤害计算：
	// 最终伤害 = 攻击力 × 技能倍率 - 目标防御力
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	FGameplayAttributeData AttackPower;
	ATTRIBUTE_ACCESSORS(UGGYGOAttributeSet, AttackPower)

	// 防御力，在 ExecutionCalculation 中从伤害里扣除
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	FGameplayAttributeData Defense;
	ATTRIBUTE_ACCESSORS(UGGYGOAttributeSet, Defense)

	// 基础移动速度，MotionDriver 在普通移动时读取
	// 可被 GE_SpeedBuff / GE_SpeedDebuff 修改
	// 限制 >= 0（不允许负数速度）
	UPROPERTY(BlueprintReadOnly, Category = "Movement")
	FGameplayAttributeData MoveSpeed;
	ATTRIBUTE_ACCESSORS(UGGYGOAttributeSet, MoveSpeed)

	// ============================================================
	// 元属性（临时计算用，不持久化）
	// 由 GE 设置，在 PostGameplayEffectExecute 中处理后立刻清零
	// 不是持久值，只是中转站
	// ============================================================

	// 接收到的伤害量，由 GE_Damage 通过 SetByCaller 设置
	// PostGameplayEffectExecute 读取后从 Health 中扣除，然后清零
	UPROPERTY(BlueprintReadOnly, Category = "Damage")
	FGameplayAttributeData IncomingDamage;
	ATTRIBUTE_ACCESSORS(UGGYGOAttributeSet, IncomingDamage)

	// ============================================================
	// GAS 回调函数
	// 由 GAS 框架自动调用，不需要手动调用
	// PreAttributeChange：属性修改前调用（用于值约束）
	// PostGameplayEffectExecute：GE 执行后调用（用于触发逻辑）
	// ============================================================

	// 属性被修改前调用
	// 只做值约束（比如 Health >= 0），不做业务逻辑
	// 注意：此时值还没真正改变，不要在这里做逻辑判断
	virtual void PreAttributeChange(
		const FGameplayAttribute& Attribute, float& NewValue) override;

	// GE 执行完毕后调用
	// 处理元属性（比如 IncomingDamage → 扣血 → 判断死亡）
	// 只广播委托，不直接处理业务逻辑（死亡流程由 BaseCharacter 处理）
	virtual void PostGameplayEffectExecute(
		const FGameplayEffectModCallbackData& Data) override;

private:
	// ============================================================
	// 每个属性的守护函数
	// 把每个属性的约束逻辑拆成独立函数
	// 保持入口函数（PreAttributeChange）简洁可读
	// 新增属性时在这里加一个对应的函数
	// ============================================================

	// 约束 Health 在 0 ~ MaxHealth 之间
	void ClampHealth(float& NewValue);

	// 约束 Stamina 在 0 ~ MaxStamina 之间（TODO：添加 Stamina 属性后启用）
	void ClampStamina(float& NewValue);

	// 约束 MoveSpeed >= 0
	void ClampMoveSpeed(float& NewValue);

	// ============================================================
	// 每个元属性的后处理函数
	// 把每个元属性的处理逻辑拆成独立函数
	// 保持入口函数（PostGameplayEffectExecute）简洁可读
	// ============================================================

	// 处理 IncomingDamage：从 Health 中扣除，清零元属性
	// 如果 Health 降到 0，广播委托通知 BaseCharacter
	void HandleIncomingDamage(const FGameplayEffectModCallbackData& Data);
};
