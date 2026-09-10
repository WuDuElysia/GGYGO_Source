/**
 * @file GGYGOCharacterMovementComponent.h
 * @brief 项目 CMC —— 步态权威 + Tag 驱动的移动禁用
 *
 * ## 为什么这些逻辑必须写在 CMC 内部
 * 在 CMC 外面驱动移动（每帧覆写 `MaxWalkSpeed`、调 `RequestDirectMove`、
 * 直接改 Actor 朝向）在单机下能跑，但联机下必然出错：那些写入发生在
 * `SavedMove` / `NetworkPrediction` 体系之外，服务器重放客户端的 move 时
 * 拿不到它们，位置校正会持续触发，表现为角色抖动或被拉回。
 *
 * 所以速度与状态一律走 CMC 的正规扩展点：
 * - `GetMaxSpeed()` 决定速度上限 —— CMC 自己会在 `CalcVelocity` 里用它，
 *   于是加减速、摩擦、坡度、碰撞全部沿用引擎已验证的实现
 * - `UpdateCharacterStateBeforeMovement()` 做步态解算 —— 这个函数在
 *   正常 tick 与 move 回放时都会被调用，是有状态逻辑的正确位置
 * - `FSavedMove_GGYGO` 保存步态与计时器 —— 回放时能还原，预测才成立
 *
 * ## 步态的判定依据
 * 用 `GetCurrentAcceleration()` 判断有无移动意图，而不是读原始摇杆值：
 * 前者已被 `SavedMove` 保存，move 回放时取值与首次执行一致，后者没有这个保证。
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
 *
 * ## 曲线速度与预测的边界
 * 曲线值来自动画的当前评估结果，属于本地状态。三处处理让它尽量可预测：
 * - 采样只在 `TickComponent` 里做一次，结果存进 `FSavedMove_GGYGO`，
 *   回放时还原而不重新采样（那时动画已走到别的时间点）
 * - 转身期间的 move 不允许合并，否则中间帧的方向变化会丢失
 * - 服务器采不到曲线时把速度上界放宽到 `MaxCurveDrivenSpeed`，
 *   避免因两端速度不同而持续校正
 *
 * 仍未解决的是：服务器若不评估动画，它重演出的**位移方向**在转身第一段
 * 会与客户端不同（那段方向由曲线给出）。彻底解决需要把曲线量放进
 * `FCharacterNetworkMoveData` 随 ServerMove 一起发送。
 */
#pragma once

#include "Character/Components/GGYGOAnimCurveSampler.h"
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

	/**
	 * 本次 move 生效的曲线运动量。
	 *
	 * 必须保存：曲线值来自动画的当前评估结果，回放时动画已经走到别的时间点，
	 * 重新采样会得到不同的值，移动结果就与首次执行不一致。
	 */
	FGGYGOAnimCurveMotion SavedCurveMotion;

	/** 本次 move 的转身相位。 */
	EGGYGOTurnBackPhase SavedTurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 本次 move 的转身计时。 */
	float SavedTurnBackElapsed = 0.0f;

	/** 本次 move 是否已进入转身第二段。 */
	bool bSavedTurnBackSecondSegment = false;

	/** 本次 move 的转身入口朝向（度）。位移方向的基准，必须还原。 */
	float SavedTurnBackEntryYaw = 0.0f;

	/** 本次 move 的反向输入闩状态。影响能否触发下一次转身。 */
	bool bSavedTurnBackInputLatched = false;
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

	/** 每帧采样动画曲线，然后交给基类推进移动。 */
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** 按当前步态返回速度上限。被 `Restriction.CantMove` 阻断时返回 0。 */
	virtual float GetMaxSpeed() const override;

	/** 每次 move（含回放）前解算步态。 */
	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;

	/** 服务器与回放路径从压缩标志位取回步态与转身段。 */
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;

	/** 转身第一段的朝向由动画曲线驱动，其余情况交回基类。 */
	virtual void PhysicsRotation(float DeltaTime) override;

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

	/** 本帧的曲线运动量。处于动画侧分量系（X 左右、Y 前后）。 */
	const FGGYGOAnimCurveMotion& GetCurveMotion() const { return CurveMotion; }

	/** 曲线速度是否正在接管移动。为 false 时速度来自配置的固定值。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool IsCurveDrivingSpeed() const;

	// ===== 急停转身 =====

	/**
	 * 当前转身相位。
	 *
	 * **模拟代理上恒为 `None`**：相位在本地控制端解算，经压缩标志位传给服务器，
	 * 而压缩标志位不会转发给其它客户端。因此别人客户端上看到的角色不会播转身动画。
	 *
	 * 修正它需要一条独立的状态复制通道，而攻击、受击等表现状态有同样的需求，
	 * 应当一次性设计而不是为转身单独加一个复制属性。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|TurnBack")
	EGGYGOTurnBackPhase GetTurnBackPhase() const { return TurnBackPhase; }

	/** 是否已交还输入控制权（`CanYaw` 已消费）。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|TurnBack")
	bool IsTurnBackCanYaw() const { return bTurnBackCanYaw; }

	/** 是否已进入转身的第二段。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|TurnBack")
	bool IsTurnBackSecondSegment() const { return bTurnBackSecondSegment; }

	/**
	 * 接收转身动画里的 `CanYaw` 通知，把控制权交还给玩家输入。
	 *
	 * 由 `UZZZAnimInstance::AnimNotify_CanYaw` 调用。只置一个待处理标记，
	 * 实际生效在下一次移动更新时 —— AnimNotify 的到达时机在帧内不确定，
	 * 直接改状态会让同一帧内的移动计算读到不一致的值。
	 *
	 * 重复通知无额外语义，只会被消费一次。
	 */
	void NotifyCanYaw();

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
	 * 动画层用它决定进出移动状态。用意图而不是实际速度，是因为起步第一帧
	 * 速度还是 0，按速度判定会让起步动画晚一帧，玩家能感觉到输入迟滞。
	 *
	 * 松手当帧即返回 false，没有防抖窗口。快速点按方向键会让动画在
	 * 起步与停止之间抖动，抑制它需要输入层持有一个短的方向保持窗口。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool HasMoveInput() const;

	// ===== 供动画层读取的派生量 =====
	// 做成即时计算的 getter 而不是每帧缓存，是因为它们全都是 Velocity 的纯函数，
	// 缓存只会多出一份可能与 Velocity 不同步的状态。

	/** 水平速度大小（cm/s）。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	float GetHorizontalSpeed() const;

	/** 是否正在移动。阈值 10 cm/s，用于过滤碰撞挤压等微小残余速度。 */
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
	 * 轴序与 UE 的局部空间（X 前、Y 右）**相反**，这是动画资产侧的约定，
	 * BlendSpace 的两个轴就是按这个顺序配的，改这里会让所有移动混合错位。
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

	/** 从 Mesh 的 AnimInstance 采样曲线。每帧恰好一次。 */
	void SampleAnimCurves(float DeltaTime);

	/** 取曲线速度经 `RootMotionScale` 缩放后的值。曲线不可用时返回 0。 */
	float GetScaledCurveSpeed() const;

	/** 推进转身相位机。 */
	void UpdateTurnBack(float DeltaSeconds);

	/** 当前输入是否构成"要转身"（跑动中输入接近反向）。 */
	bool IsReverseRunInput() const;

	/** 转身状态整体复位。 */
	void ResetTurnBack();

	/** 是否处于转身第一段 —— 该段的位移方向与朝向都由动画决定，不跟随输入。 */
	bool IsTurnBackFirstSegment() const;

	/**
	 * 把曲线的局部方向转成世界方向。
	 *
	 * 基准是进入转身时保存的 Actor 朝向，而不是当前朝向。用当前朝向会让
	 * 世界方向随角色转身一起旋转，位移轨迹变成弧线；而急停转身要的是
	 * 角色一边转身一边沿原方向滑行刹车，世界方向必须保持不变。
	 */
	FVector ResolveTurnBackWorldDirection() const;

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

	// ===== 转身状态 =====

	/** 当前相位。 */
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 本次转身已进行的时长（秒）。 */
	float TurnBackElapsed = 0.0f;

	/** 已消费 `CanYaw`，控制权已交还输入。 */
	bool bTurnBackCanYaw = false;

	/** 已进入第二段。 */
	bool bTurnBackSecondSegment = false;

	/**
	 * 反向输入已被本次转身消费。
	 *
	 * 转身结束后玩家往往还按着同一个方向键，而那个方向此时已经是角色的正前方，
	 * 不该再触发一次转身。必须等输入离开反向阈值才允许下一次触发。
	 */
	bool bTurnBackInputLatched = false;

	/** `CanYaw` 通知待处理。由 `NotifyCanYaw` 置位，在移动更新时消费。 */
	bool bTurnBackCanYawPending = false;

	/** 进入转身时的 Actor yaw（度）。曲线局部方向转世界方向的基准。 */
	float TurnBackEntryYaw = 0.0f;

	/** 曲线采样器。持有跨帧基线，只在 `TickComponent` 里推进。 */
	FGGYGOAnimCurveSampler CurveSampler;

	/**
	 * 本帧的曲线运动量。
	 *
	 * 在 `TickComponent` 里更新一次，之后整帧的移动计算都读它。
	 * 移动回放时由 `FSavedMove_GGYGO::PrepMoveFor` 覆盖为当时的值。
	 */
	FGGYGOAnimCurveMotion CurveMotion;

	friend class FSavedMove_GGYGO;
};
