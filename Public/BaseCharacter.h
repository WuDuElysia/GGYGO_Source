/**
 * @file BaseCharacter.h
 * @brief 所有角色的基类 - 管线时序分发中心 + GAS 宿主
 *
 * 职责：
 * 1. 创建所有子系统（管线、状态机、驱动）并在 Tick 中按严格时序分发
 * 2. 托管 AbilitySystemComponent 和 AttributeSet（GAS 中枢）
 * 3. 实现 IAbilitySystemInterface，让外部系统通过 GAS API 找到 ASC
 *
 * 不包含具体游戏逻辑，只做组件整合和时序控制。
 */
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "Drivers/MotionDriver.h"

#include "Data/InputData.h"
#include "Data/Logic/RuntimeData.h"       // 逻辑运行时数据（含 AnimData 子结构）
#include "Data/UCharConfigData.h"

#include "Pipeline/InputPipeline.h"
#include "Pipeline/IntentPipeline.h"
#include "Pipeline/ArbiterPipeline.h"

#include "StateMachine/GGYGOStateManager.h" // ★ 阶段6：纯C++ 并行状态管理器
#include "Attributes/GGYGOAttributeSet.h"
#include "Data/UCharConfigData.h"
#include "BaseCharacter.generated.h"

class UGameplayAbility;
class UGameplayEffect;

UCLASS()
class GGYGO_API ABaseCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	ABaseCharacter();

	// ============================================================
	// IAbilitySystemInterface 实现
	// ============================================================

	/**
	 * GAS 框架通过此接口找到 ASC
	 * 所有 TryActivateAbility / ApplyGameplayEffect 等操作都依赖它
	 */
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	/** 获取属性集 */
	UGGYGOAttributeSet* GetAttributeSet() const { return AttributeSet; }

	// ============================================================
	// 动画蓝图数据接口（BlueprintCallable，只读）
	// 动画蓝图每帧调这些 getter 获取 RuntimeData 的值
	// 不直接暴露 RuntimeData，保持封装
	// ============================================================

	/** 当前移动速度（cm/s，水平标量） */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetCurrentSpeed() const { return RuntimeData->CurrentSpeed; }

	/** 移动角度（相对于角色朝向，-180~180°） */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetMoveAngle() const { return RuntimeData->MoveAngle; }

	/** 当前角色状态（Idle/Locomotion/InAir 等） */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	ECharacterStateType GetCurrentState() const { return RuntimeData->CurrentState; }

	/** 是否在移动中 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool IsMoving() const { return RuntimeData->bIsMoving; }

	/** 是否在地面上 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool IsGrounded() const { return RuntimeData->bIsGrounded; }

	/** 动画驱动速度（从 Bip001 骨骼位移提取，cm/s） */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetAnimSpeed() const { return RuntimeData->AnimSpeed; }

	/** 供 AnimInstance 判断 Moving 状态下选 Walk/Run/Sprint 动画 */
	EMovementGait GetResolvedGait() const { return RuntimeData->ResolvedGait; }

	/** 获取角色配置（供 AnimInstance 等外部系统读取） */
	UCharConfigData* GetCharacterConfig() const { return CharacterConfig; }

	/** 获取运行时黑板（供 AnimInstance 等外部系统只读访问） */
	FRuntimeData* GetRuntimeData() const { return RuntimeData.Get(); }

	// ============================================================
	// 输入处理接口（BlueprintCallable，供子类蓝图绑定 EnhancedInput）
	// ============================================================



protected:
	virtual void BeginPlay() override;

public:
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:
	// ============================================================
	// GAS 核心
	// ============================================================

	/**
	 * AbilitySystemComponent — GAS 中枢
	 * 管理所有 Ability 的授予/激活、GE 的应用、Tag 的读写。
	 * 用 CreateDefaultSubobject 创建 → 作为 Actor 的子对象 → GC 自动管理生命周期。
	 */
	UPROPERTY(VisibleAnywhere, Category = "GAS")
	UAbilitySystemComponent* ASC;

	/**
	 * 属性集
	 * ASC 在初始化时会自动扫描 Actor 的子对象找到 UAttributeSet。
	 * 所以必须用 CreateDefaultSubobject 在构造函数创建，不能在 BeginPlay 里 NewObject。
	 */
	UPROPERTY()
	UGGYGOAttributeSet* AttributeSet;

	/**
	 * 默认授予的 Ability 列表
	 * 蓝图子类（BP_Player / BP_Enemy）在细节面板里配置。
	 * BeginPlay 时通过 GiveDefaultAbilities() 批量授予。
	 * 目前暂时不用（阶段八-完整版），先留好配置入口。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "GAS")
	TArray<TSubclassOf<UGameplayAbility>> DefaultAbilities;

	/**
	 * 初始化属性用的 GE 列表
	 * 蓝图子类在细节面板里配置各自的 GE_InitAttributes。
	 * 不同角色配不同的 GE，实现差异化初始属性，不需要改 C++ 代码。
	 * 目前暂时不用，先留好配置入口。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "GAS")
	TArray<TSubclassOf<UGameplayEffect>> DefaultEffects;

	// ============================================================
	// 角色配置数据资产（★ 数据驱动核心）
	//
	// 借鉴 HT 鸣潮的 BP_PlayerCharacterBase 模式：
	//   每个角色创建一个 UCharConfigData 子类资产（DataAsset），
	//   集中存放该角色的动画映射、过渡时间、移动参数、GAS 能力列表。
	//   蓝图子类在细节面板引用此字段即可完成全部差异化配置。
	//
	// 优先级：Config > 本类成员变量（AnimInstance 优先从 Config 读取）
	// ============================================================

	/**
	 * 角色配置数据资产
	 * 蓝图子类（BP_Miyabi / BP_Enemy 等）在细节面板里指定。
	 * 为空时 AnimInstance 回退到自身 EditAnywhere 的独立属性（向后兼容）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "配置|角色")
	UCharConfigData* CharacterConfig;

	/** 授予 DefaultAbilities 中的所有 Ability */
	void GiveDefaultAbilities();

	/** 应用 DefaultEffects 中的所有 GE（通常用于设置初始属性） */
	void ApplyDefaultEffects();

	// ============================================================
	// 管线子系统（纯 C++ 类，TUniquePtr 管理生命周期）
	// ============================================================

	/** 运动驱动器 */
	TUniquePtr<FMotionDriver> MotionDriver;

	/** 输入数据容器 */
	TUniquePtr<FInputData> InputData;

	/** 运行时黑板（含 AnimData 子结构） */
	TUniquePtr<FRuntimeData> RuntimeData;

	/** 输入管线 */
	TUniquePtr<FInputPipeline> InputPipeline;

	/** 意图管线 */
	TUniquePtr<FIntentPipeline> IntentPipeline;

	/** ★ 阶段6：并行状态管理器（纯 C++，和其他管线风格一致）*/
	TUniquePtr<FGYGOStateManager> StateManager;

	/** 仲裁管线（Tick 第 1 步） */
	TUniquePtr<FArbiterPipeline> ArbiterPipeline;
};
