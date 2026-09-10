/**
 * @file GGYGOCharacterMovementComponent.h
 * @brief 项目 CMC —— 步态权威 + Tag 驱动的移动禁用
 *
 * 取代旧的 `FMotionDriver` + `FGaitAuthorityProcessor` + `FArbiterPipeline` 三件套。
 *
 * ## 为什么必须回到 CMC 内部
 * 旧实现在 CMC **外面**驱动移动：每帧覆写 `MaxWalkSpeed`，再调 `RequestDirectMove`，
 * TurnBack 期间还直接 `AddActorWorldRotation`。这些写入全部发生在 CMC 的
 * `SavedMove` / `NetworkPrediction` 体系之外，后果是联机下客户端与服务器
 * 必然不一致：服务器重放客户端的 move 时拿不到那些外部写入，
 * 位置校正会持续触发，表现为角色抖动或被拉回。
 *
 * 旧代码里连一处 `FSavedMove_Character` 派生都没有（六个相关目录零匹配），
 * 所以它不是"预测做得不好"，而是完全没有进入预测体系。
 *
 * 本类改为使用 CMC 的正规扩展点：
 * - `GetMaxSpeed()` 决定速度上限 —— CMC 自己会在 `CalcVelocity` 里用它，
 *   于是加减速、摩擦、坡度、碰撞全部沿用引擎已验证的实现
 * - `UpdateCharacterStateBeforeMovement()` 做步态解算 —— 这个函数在
 *   正常 tick 与 move 回放时都会被调用，是有状态逻辑的正确位置
 * - `FSavedMove_GGYGO` 保存步态与计时器 —— 回放时能还原，预测才成立
 *
 * ## 步态判定的化简
 * 旧逻辑依赖 `RuntimeData.State.CurrentState == Moving`，而那个状态机
 * （`FGYGOStateManager` 只注册了 Idle / Moving 两个状态）本身就是
 * "有没有移动输入"的投影：`FIdleState` 见到有方向就转 Moving，
 * `FMovingState` 见到方向归零就转回 Idle。
 *
 * 所以本类直接用移动输入判定，去掉中间那层状态机。这不是行为改变，
 * 是去掉一层等价的间接。用 `GetCurrentAcceleration()` 而不是原始摇杆值，
 * 因为它已被 `SavedMove` 保存，回放时取值一致。
 *
 * ## 关于步态的网络权威
 * 步态由客户端解算，经 `CompressedFlags` 的两个自定义位发给服务器，
 * 服务器采用客户端的值而不自行解算。这是 CMC 里可预测状态的标准做法：
 * 若服务器自算，两端对"输入何时开始"的采样差异会让计时器错开，
 * 进而速度不同、位置校正不断。
 *
 * 代价是客户端理论上可以谎报 Run。这里接受该风险 —— 步态只影响速度上限，
 * 而位置本身仍受服务器的 `ServerMoveHandleClientError` 校验约束，
 * 谎报能得到的收益上限就是走速与跑速之差。
 */
#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "GGYGOCharacterMovementComponent.generated.h"

class AActor;
class FSavedMove_Character;
class UGGYGOAbilitySystemComponent;
class UGGYGOMovementSet;
class UObject;

/**
 * 本项目的 SavedMove。
 *
 * `FSavedMove_Character` 的职责是"把一次 move 所依赖的全部状态存下来，
 * 以便客户端回放和服务器重演"。任何影响移动结果又不在基类里的状态，
 * 都必须在这里保存，否则回放结果会与首次执行不同。
 *
 * 本类需要额外保存两项：步态（决定速度上限）与走跑计时器（决定何时升档）。
 */
class FSavedMove_GGYGO : public FSavedMove_Character
{
public:
	using Super = FSavedMove_Character;

	/** 清空以便对象复用。CMC 的 SavedMove 走对象池，不清会带上一次的残留。 */
	virtual void Clear() override;

	/** 把 CMC 的当前状态存进本 move。在 move 执行**前**调用。 */
	virtual void SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, class FNetworkPredictionData_Client_Character& ClientData) override;

	/** 把本 move 的状态写回 CMC。回放前调用，是 `SetMoveFor` 的逆操作。 */
	virtual void PrepMoveFor(ACharacter* C) override;

	/**
	 * 能否与下一个 move 合并发送。
	 *
	 * 合并是带宽优化，但只有状态完全一致才能合并 —— 步态不同的两帧合并后，
	 * 服务器只会看到一个步态，另一帧的速度就错了。
	 */
	virtual bool CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const override;

	/** 把步态编码进压缩标志位发给服务器。 */
	virtual uint8 GetCompressedFlags() const override;

	/** 本次 move 生效的步态。 */
	EGGYGOGait SavedGait = EGGYGOGait::None;

	/** 本次 move 开始时的走跑计时器读数。 */
	float SavedWalkHoldTimer = 0.0f;

	/** 本次 move 开始时是否持有"下次移动直接进 Run"的契约。 */
	bool bSavedWantsRunOnNextMove = false;
};

/** 客户端预测数据。唯一职责是让 CMC 分配出我们自己的 SavedMove 类型。 */
class FNetworkPredictionData_Client_GGYGO : public FNetworkPredictionData_Client_Character
{
public:
	using Super = FNetworkPredictionData_Client_Character;

	explicit FNetworkPredictionData_Client_GGYGO(const UCharacterMovementComponent& ClientMovement);

	virtual FSavedMovePtr AllocateNewMove() override;
};

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UGGYGOCharacterMovementComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//~UCharacterMovementComponent interface
	virtual void BeginPlay() override;

	/** 按当前步态返回速度上限。被 `Restriction.CantMove` 阻断时返回 0。 */
	virtual float GetMaxSpeed() const override;

	/** 每次 move（含回放）前解算步态。 */
	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;

	/** 服务器与回放路径从压缩标志位取回步态。 */
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;

	/** 提供我们自己的预测数据类型。 */
	virtual FNetworkPredictionData_Client* GetPredictionData_Client() const override;
	//~End of UCharacterMovementComponent interface

	/**
	 * 注入移动参数。由 `AGGYGOCharacterBase` 在 ASC 就绪后调用。
	 *
	 * 会立即把资产里的加减速、摩擦、旋转参数写进 CMC 对应字段。
	 * 传 nullptr 是合法的，此时全部走 CMC 的引擎默认值。
	 */
	void SetMovementSet(const UGGYGOMovementSet* InMovementSet);

	/** 当前移动参数。可能为 nullptr。 */
	const UGGYGOMovementSet* GetMovementSet() const { return MovementSet; }

	/** 本帧解算出的步态。动画层读它决定走跑混合。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	EGGYGOGait GetResolvedGait() const { return ResolvedGait; }

	/**
	 * 请求下一次移动直接进入 Run，跳过走跑计时。
	 *
	 * 用于闪避收尾接移动：闪避结束时玩家仍按着方向键，此时从 Walk 起步
	 * 再等五秒升 Run 是错的，动作游戏里闪避后应当直接是跑。
	 *
	 * 契约只消费一次。若闪避后玩家松手了，契约在下一次"无移动输入"帧被丢弃。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Movement")
	void RequestRunOnNextMove();

	/** 是否正被 `Restriction.CantMove` 禁止移动。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool IsMovementBlockedByTag() const;

	/**
	 * 本帧是否有移动意图。
	 *
	 * 取代旧 `FZZZAnimRuntimeModel::bShouldMove`。动画层用它决定进出移动状态 ——
	 * 用意图而不是实际速度，是因为起步第一帧速度还是 0，
	 * 按速度判定会让起步动画晚一帧，玩家能感觉到输入迟滞。
	 *
	 * 已知差异：旧 `FInputPipeline` 在松手后有一个短窗口维持上一次有效方向
	 * （`MoveFlickerBuffer`），用于快速点按时不抖动。那属于输入层职责，
	 * 随输入层在阶段 7 重建，当前本函数在松手当帧即返回 false。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool HasMoveInput() const;

	// ===== 供动画层读取的派生量 =====
	// 这些取代旧 MotionDriver::UpdateRuntimeData 写进 FRuntimeData 的那批字段。
	// 做成即时计算的 getter 而不是每帧缓存，是因为它们全都是 Velocity 的纯函数，
	// 缓存只会多出一份可能与 Velocity 不同步的状态。

	/** 水平速度大小（cm/s）。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	float GetHorizontalSpeed() const;

	/** 是否正在移动。阈值 10 cm/s，与旧实现一致。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool IsMovingHorizontally() const;

	/** 水平速度的世界单位方向。静止时返回零向量。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	FVector GetHorizontalVelocityDirection() const;

	/**
	 * 水平速度相对角色朝向的角度（度）：0 为正前，+90 为正右。
	 *
	 * 静止时返回 0。注意这与 `GetHorizontalVelocityDirection` 不同，
	 * 后者是世界空间，本函数是角色局部空间 —— 动画 BlendSpace 要的是后者。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	float GetLocalVelocityAngle() const;

	/**
	 * 水平速度相对角色朝向的 BlendSpace 分量。X 为右、Y 为前，均已归一化。
	 *
	 * 轴序是给 BlendSpace 用的约定，与 UE 的局部空间（X 前、Y 右）**相反**，
	 * 这是旧实现留下的既有约定，动画资产按它配好了，不改。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	void GetLocalVelocityBlend(float& OutBlendX, float& OutBlendY) const;

protected:
	/** 缓存 ASC。由 PawnExtension 的 ASC 就绪委托触发。 */
	void CacheAbilitySystemComponent();

	/**
	 * 解算本帧步态。
	 *
	 * 只在本地控制端与服务器权威端调用；模拟代理的步态从压缩标志位取。
	 */
	void ResolveGait(float DeltaSeconds);

	/** 推进或归零走跑计时器。 */
	void UpdateWalkHoldTimer(EGGYGOGait FrameGait, bool bHasMoveInput, bool bBlocked, bool bOnGround, bool bMoveInputRising, bool bBlockReleased, float DeltaSeconds);

	/** 把移动参数写进 CMC 的对应字段。 */
	void ApplyMovementSetToComponent();

protected:
	/** 移动参数资产。 */
	UPROPERTY(Transient)
	TObjectPtr<const UGGYGOMovementSet> MovementSet;

	/** ASC 缓存，用于查 `Restriction.CantMove`。 */
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	/** 本帧步态。 */
	EGGYGOGait ResolvedGait = EGGYGOGait::None;

	/** 走跑计时器（秒）。只在 Walk 且持续移动时累加。 */
	float WalkHoldTimer = 0.0f;

	/** "下次移动直接进 Run" 契约。 */
	bool bWantsRunOnNextMove = false;

	/** 上一帧是否有移动输入。用于识别起步上升沿。 */
	bool bPreviousHasMoveInput = false;

	/** 上一帧是否被禁止移动。用于识别解禁沿。 */
	bool bPreviousMovementBlocked = false;

	friend class FSavedMove_GGYGO;
};
