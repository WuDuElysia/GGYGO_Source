/**
 * @file GGYGOCharacterBase.h
 * @brief 新版角色基类 —— 只负责把组件接起来
 *
 * ## 本类刻意不做的事
 * 不含移动逻辑、不含输入处理、不含战斗逻辑、不含动画驱动。
 * 它只做四件事：创建组件、实现 `IAbilitySystemInterface`、
 * 把引擎的生命周期回调转给 `UGGYGOPawnExtensionComponent`、把死亡事件接到表现上。
 *
 * 原因是这些职责的生命周期与复用边界各不相同：
 * 移动逻辑属于 CMC（要参与网络预测），输入属于本地控制端（模拟代理没有），
 * 战斗属于 GAS（要能被 GameFeature 插拔）。塞进 Character 会让它们被迫共享
 * Character 的生命周期，于是"敌人不需要输入""载具不需要 Health"这类差异
 * 只能用运行时判断绕开，而不是干脆不挂那个组件。
 *
 * ## 与旧 ABaseCharacter 的关系
 * 两者**并行存在**，本类不是它的父类也不是子类。
 * 旧类被移动 Pipeline 的 4 个文件反向依赖，那套 Pipeline 在阶段 5 整体退役；
 * 现在就去改它等于把阶段 5 的风险提前到阶段 4。
 * 旧角色蓝图继续用 `ABaseCharacter`，新角色用本类，阶段 5 收敛为一个。
 *
 * ## ASC 挂在这里而不是 PlayerState
 * 决策 D1。Lyra 一个 PlayerState 一个 ASC，但一个 ASC 只能挂一套同类 AttributeSet，
 * 而本项目一名玩家带三个角色（D2），共用 ASC 会让三角色共享生命值与技能冷却。
 *
 * 代价是队伍级的共享资源（如队伍能量）需要另一个挂 PlayerState 的 ASC，
 * 那部分在阶段 10 处理。本类的 `GetAbilitySystemComponent` 返回的始终是角色自己的。
 */
#pragma once

#include "AbilitySystemInterface.h"
#include "GameFramework/Character.h"

#include "GGYGOCharacterBase.generated.h"

class AActor;
class AController;
class UAbilitySystemComponent;
class UGGYGOAbilitySystemComponent;
class UGGYGOCharacterMovementComponent;
class UGGYGOHealthComponent;
class UGGYGOPawnExtensionComponent;
class UInputComponent;
class UObject;
struct FFrame;

UCLASS(Config = Game, meta = (ShortTooltip = "新版角色基类，Lyra 风格组件化"))
class GGYGO_API AGGYGOCharacterBase : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGGYGOCharacterBase(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//~IAbilitySystemInterface
	/** 返回本角色自己的 ASC。GAS 全部对外交互都经由它。 */
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~End of IAbilitySystemInterface

	/** 类型化的 ASC 访问器，省掉调用方的 Cast。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Character")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const { return AbilitySystemComponent; }

	/** 初始化协调者。其它组件通过它拿 PawnData 与 ASC。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Character")
	UGGYGOPawnExtensionComponent* GetPawnExtensionComponent() const { return PawnExtComponent; }

	/** 生命与韧性门面。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Character")
	UGGYGOHealthComponent* GetHealthComponent() const { return HealthComponent; }

	/**
	 * 类型化的 CMC 访问器。
	 *
	 * 不用成员变量缓存，因为 CMC 是 `ACharacter` 的既有子对象（由
	 * `SetDefaultSubobjectClass` 换掉了类型），再存一份指针就有两个真源。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Character")
	UGGYGOCharacterMovementComponent* GetGGYGOMovementComponent() const;

protected:
	//~AActor / APawn 生命周期
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 服务器上被 Controller 附身。 */
	virtual void PossessedBy(AController* NewController) override;

	/** 服务器上失去 Controller。 */
	virtual void UnPossessed() override;

	/** 客户端收到 Controller 复制。 */
	virtual void OnRep_Controller() override;

	/** 客户端收到 PlayerState 复制。 */
	virtual void OnRep_PlayerState() override;

	/** 输入组件建立。 */
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** 掉出世界。转为走正常的自毁伤害链路。 */
	virtual void FellOutOfWorld(const class UDamageType& DmgType) override;
	//~End of lifecycle

	/** ASC 就绪回调。在这里把 HealthComponent 接到 ASC 上。 */
	virtual void OnAbilitySystemInitialized();

	/** ASC 解除回调。 */
	virtual void OnAbilitySystemUninitialized();

	/** 死亡演出开始：关碰撞、停移动。 */
	UFUNCTION()
	virtual void OnDeathStarted(AActor* OwningActor);

	/** 死亡演出结束：隐藏角色。销毁与复活由外部决定，本类不代劳。 */
	UFUNCTION()
	virtual void OnDeathFinished(AActor* OwningActor);

	/**
	 * 关闭移动与碰撞。死亡开始时调用。
	 *
	 * 拆成独立函数是因为击倒、被抓取等状态也需要同样的处理，
	 * 那些将来会由 GA 调用它，而不是各自重写一遍这几行。
	 */
	void DisableMovementAndCollision();

	/** 卸下 ASC 关联并清理。死亡结束时调用。 */
	void UninitAndDestroy();

private:
	/**
	 * GAS 中枢。挂在角色自己身上（决策 D1，理由见文件头）。
	 *
	 * `VisibleAnywhere` 而非 `EditDefaultsOnly`：组件实例由构造函数创建，
	 * 不该在编辑器里被替换。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	/** 初始化协调者。必须存在，否则 InitState 链条断裂。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOPawnExtensionComponent> PawnExtComponent;

	/** 生命与韧性。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOHealthComponent> HealthComponent;
};
