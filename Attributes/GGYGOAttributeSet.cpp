// GGYGOAttributeSet.cpp
// GAS 属性集实现
// 包含：
// 1. 构造函数（不硬编码初始值，由蓝图配置 GE_InitAttributes）
// 2. PreAttributeChange - 属性修改前的值约束
// 3. PostGameplayEffectExecute - GE 执行后的逻辑处理（伤害计算等）
// 4. 每个属性的守护函数和后处理函数

#include "Attributes/GGYGOAttributeSet.h"
#include "GameplayEffectExtension.h"

// 构造函数：故意留空
// 初始属性值不在这里设置
// 每个角色蓝图（BP_Player、BP_Enemy）在 DefaultEffects 数组里
// 配置自己的 GE_InitAttributes，在 BeginPlay 时通过
// BaseCharacter::ApplyDefaultEffects() 应用
// 这样不同角色可以有不同的初始属性，不需要改 C++ 代码
UGGYGOAttributeSet::UGGYGOAttributeSet()
{
}

// GAS 框架在属性被修改前自动调用
// 入口函数只做分发，具体约束逻辑在各自的私有函数里
// 注意：不要在这里写业务逻辑，此时值还没真正改变
// 这里只负责确保值在合法范围内
void UGGYGOAttributeSet::PreAttributeChange(
	const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	// 根据属性类型分发到对应的守护函数
	// 新增属性时在这里加一个 else if
	if (Attribute == GetHealthAttribute())         ClampHealth(NewValue);
	else if (Attribute == GetMoveSpeedAttribute()) ClampMoveSpeed(NewValue);
}

// GAS 框架在 GE 执行完毕后自动调用
// 这里处理元属性（比如 IncomingDamage）并触发游戏事件（比如死亡）
// 入口函数只做分发，具体逻辑在各自的私有函数里
// 注意：只广播委托，不直接处理业务逻辑
// 死亡动画、状态机切换等由 BaseCharacter 监听委托后处理
void UGGYGOAttributeSet::PostGameplayEffectExecute(
	const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	// 根据被修改的属性分发到对应的处理函数
	if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
	{
		HandleIncomingDamage(Data);
	}
}

// ============================================================
// 守护函数：每个属性的值约束
// ============================================================

// 约束血量在 0 ~ 最大血量之间
// 防止血量变成负数或超过上限
void UGGYGOAttributeSet::ClampHealth(float& NewValue)
{
	NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
}

// 约束体力在 0 ~ 最大体力之间
// TODO：添加 Stamina / MaxStamina 属性后实现
void UGGYGOAttributeSet::ClampStamina(float& NewValue)
{
}

// 约束移动速度 >= 0
// 被减速 debuff 影响时速度可能很低，但不能变成负数
void UGGYGOAttributeSet::ClampMoveSpeed(float& NewValue)
{
	NewValue = FMath::Max(NewValue, 0.f);
}

// ============================================================
// 后处理函数：元属性的逻辑处理
// ============================================================

// 处理接收到的伤害
// 流程：
// 1. 读取 IncomingDamage 的值
// 2. 立刻清零（元属性只是中转站，用完就清）
// 3. 如果伤害 > 0，从 Health 中扣除
// 4. 如果 Health 降到 0，广播委托（BaseCharacter 监听后执行死亡流程）
void UGGYGOAttributeSet::HandleIncomingDamage(
	const FGameplayEffectModCallbackData& Data)
{
	// 读取伤害值
	float Damage = GetIncomingDamage();

	// 清零元属性，防止下次重复计算
	SetIncomingDamage(0.f);

	if (Damage > 0.f)
	{
		// 从当前血量中扣除伤害
		float NewHealth = GetHealth() - Damage;
		SetHealth(FMath::Clamp(NewHealth, 0.f, GetMaxHealth()));

		// TODO：如果 Health <= 0，广播 OnHealthDepleted 委托
		// BaseCharacter 监听这个委托来执行死亡流程
	}
}
