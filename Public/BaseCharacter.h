/**
 * @file BaseCharacter.h
 * @brief 所有角色的基类 - 管线时序分发中心 + GAS 宿主
 *
 * 职责：
 * 1. 托管运行时宿主组件，并保留唯一的 Tick 时序入口
 * 2. 托管 AbilitySystemComponent 和 AttributeSet（GAS 中枢）
 * 3. 实现 IAbilitySystemInterface，让外部系统通过 GAS API 找到 ASC
 *
 * 纯 C++ 管线、状态机和驱动由 UGGYGOCharacterRuntimeComponent 持有；
 * 本类只做 Unreal 对象整合、输入门面和时序入口。
 */
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "Components/GGYGOCharacterRuntimeComponent.h"
#include "Data/Runtime/RuntimeData.h"
#include "Data/Config/UCharConfigData.h"
#include "Attributes/GGYGOAttributeSet.h"
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
	float GetCurrentSpeed() const { return RuntimeComponent->GetRuntimeData()->Movement.CurrentSpeed; }

	/** 移动角度（相对于角色朝向，-180~180°） */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetMoveAngle() const { return RuntimeComponent->GetRuntimeData()->Movement.MoveAngle; }

	/** 当前角色状态（Idle/Locomotion/InAir 等） */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	ECharacterStateType GetCurrentState() const { return RuntimeComponent->GetRuntimeData()->State.CurrentState; }

	/** 是否在移动中 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool IsMoving() const { return RuntimeComponent->GetRuntimeData()->Movement.bIsMoving; }

	/** 是否在地面上 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool IsGrounded() const { return RuntimeComponent->GetRuntimeData()->Movement.bIsGrounded; }

	/** 动画驱动速度（从 Bip001 骨骼位移提取，cm/s） */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetAnimSpeed() const { return RuntimeComponent->GetRuntimeData()->RootMotion.AnimSpeed; }

	/** 供 AnimInstance 读取 Moving 状态下的 Walk/Run 步态 */
	EMovementGait GetResolvedGait() const { return RuntimeComponent->GetRuntimeData()->Gait.ResolvedGait; }

	/** 获取角色配置（供 AnimInstance 等外部系统读取） */
	UCharConfigData* GetCharacterConfig() const { return CharacterConfig; }

	/** 获取运行时黑板（供 AnimInstance 等外部系统只读访问） */
	FRuntimeData* GetRuntimeData() const { return RuntimeComponent ? RuntimeComponent->GetRuntimeData() : nullptr; }

protected:
	// ============================================================
	// 输入处理门面
	// ============================================================

	/** 将玩家输入转发给运行时宿主组件。 */
	void SetMoveInput(const FVector2D& Value);
	void ClearMoveInput();
	void SetLookInput(const FVector2D& Value);
	void SetSprintHeld(bool bHeld);
	void SetForceWalkHeld(bool bHeld);

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
	// 运行时宿主组件
	// ============================================================

	/** 组件负责纯 C++ 子系统的生命周期；组件自身不 Tick。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	UGGYGOCharacterRuntimeComponent* RuntimeComponent;
};
