/**
 * @file GGYGOCharacterBase.h
 * @brief 新版角色基类 —— 只负责把组件接起来
 *
 * ## 本类刻意不做的事
 * 不含移动逻辑、不含输入处理、不含战斗逻辑、不含动画驱动。
 * 它只做四件事：创建组件、实现 `IAbilitySystemInterface`、
 * 把引擎生命周期接到初始化及原生通知、把死亡事件接到表现上。
 *
 * 原因是这些职责的生命周期与复用边界各不相同：
 * 移动逻辑属于 CMC（要参与网络预测），输入属于本地控制端（模拟代理没有），
 * 战斗属于 GAS（要能被 GameFeature 插拔）。塞进 Character 会让它们被迫共享
 * Character 的生命周期，于是"敌人不需要输入""载具不需要 Health"这类差异
 * 只能用运行时判断绕开，而不是干脆不挂那个组件。
 *
 * ## ASC 不由本类持有
 * 本类是通用 Avatar，不规定外部 ASC 宿主的具体类型。玩家由
 * `AGGYGOCharacterSlot` 注入；可换形态 Boss 将由 `AGGYGOBossState` 注入。
 *
 * 一个 ASC 只能挂一套同类 AttributeSet，而一名玩家带多个角色（D2），
 * 所以不能像 Lyra 那样共用 PlayerState 上的一个 ASC。但也不放在本类身上 ——
 * 那会让属性集的就绪时机被 Pawn 的初始化流程牵制，而本类的
 * `UGGYGOHealthComponent` 正是在同一段流程里读属性集的。
 * 放在位置上，属性集随位置构造即就绪，早于任何 Pawn 存在。
 *
 * `GetAbilitySystemComponent` 返回被注入的那个 ASC，**在注入完成前为 nullptr**。
 */
#pragma once

#include "AbilitySystemInterface.h"
#include "Delegates/Delegate.h"
#include "GameFramework/Character.h"
#include "Templates/SharedPointer.h"

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

	DECLARE_EVENT_OneParam(AGGYGOCharacterBase, FReplicatedLocalRoleChanged, AGGYGOCharacterBase*);

	/**
	 * 本角色接收复制且 LocalRole 变化后的原生通知；Controller 复制可能尚未完成。
	 * 订阅者持有并归还 FDelegateHandle，绑定后自行读取当前状态；本事件不重放初始快照。
	 */
	FReplicatedLocalRoleChanged& OnReplicatedLocalRoleChanged() { return ReplicatedLocalRoleChanged; }

	//~IAbilitySystemInterface
	/** 返回本角色作为 Avatar 所属的那个 ASC。注入完成前为 nullptr。 */
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~End of IAbilitySystemInterface

	/**
	 * 类型化的 ASC 访问器，省掉调用方的 Cast。
	 *
	 * 返回外部战斗状态宿主注入的 ASC，**注入完成前为 nullptr**，调用方必须判空。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Character")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const;

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

	/**
	 * 设置整体碰撞（`SetActorEnableCollision`）并复制到客户端。仅权威端调用。
	 *
	 * 引擎的整体碰撞开关不复制：只在服务器关闭时，客户端仍保留这名角色的胶囊，
	 * 本地预测移动会撞上看不见的角色、再被服务器纠正。用于不在场的 Avatar（如待命队员）。
	 */
	void SetReplicatedActorEnableCollision(bool bEnable);

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	//~AActor / APawn 生命周期
	/** 服务器上被 Controller 附身。 */
	virtual void PossessedBy(AController* NewController) override;

	/** 服务器上失去 Controller。 */
	virtual void UnPossessed() override;

	/** 接收复制后的 LocalRole 变化，保留引擎回调并通知本地订阅者。 */
	virtual void PostNetReceiveRole() override;

	/** 客户端收到 Controller 复制。 */
	virtual void OnRep_Controller() override;

	/** 客户端收到 PlayerState 复制。 */
	virtual void OnRep_PlayerState() override;

	/** 输入组件建立。 */
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** 掉出世界。转为走正常的自毁伤害链路。 */
	virtual void FellOutOfWorld(const class UDamageType& DmgType) override;
	//~End of lifecycle

	/** 死亡演出开始：关碰撞、停移动。 */
	UFUNCTION()
	virtual void OnDeathStarted(AActor* OwningActor);

	/** 接收 Health 的死亡结束事实，下一帧再交给蓝图处理，允许同次广播先完成自动替补。 */
	UFUNCTION()
	virtual void OnDeathFinished(AActor* OwningActor);

	/**
	 * 死亡结束广播后的下一帧调用，各端独立执行。共享角色蓝图决定隐藏、销毁或保留 Pawn。
	 * @return 蓝图已处理时返回 true；未实现或返回 false 会明确诊断，不执行默认销毁。
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "GGYGO|Character|Death", meta = (DisplayName = "Handle Death Finished"))
	bool K2_HandleDeathFinished();
	virtual bool K2_HandleDeathFinished_Implementation();

	/**
	 * 关闭移动与碰撞。死亡开始时调用。
	 *
	 * 拆成独立函数是因为击倒、被抓取等状态也需要同样的处理，
	 * 那些将来会由 GA 调用它，而不是各自重写一遍这几行。
	 */
	void DisableMovementAndCollision();

	/**
	 * 供死亡处理蓝图选择的清理原语：各端释放本 Pawn 的 ASC 关联并隐藏；仅权威端解除控制并安排销毁。
	 * 在 Handle Death Finished 中调用时，已晚于死亡结束及自动替补广播。保留旧有 0.1 秒 LifeSpan。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Character|Death", meta = (BlueprintProtected = "true"))
	void UninitAndDestroy();

private:
	/** TimerManager 持有本 Actor 的弱绑定，Actor 失效后不会执行旧回调。 */
	void HandleDeathFinishedNextTick();

	/** 客户端收到整体碰撞开关后应用同一值。 */
	UFUNCTION()
	void OnRep_ReplicatedActorEnableCollision();

	FReplicatedLocalRoleChanged ReplicatedLocalRoleChanged;

	/** 服务器设置的整体碰撞；默认开启，与引擎默认一致。 */
	UPROPERTY(ReplicatedUsing = OnRep_ReplicatedActorEnableCollision)
	bool bReplicatedActorEnableCollision = true;

	/** 初始化协调者。必须存在，否则 InitState 链条断裂。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOPawnExtensionComponent> PawnExtComponent;

	/** 生命与韧性。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOHealthComponent> HealthComponent;
};
