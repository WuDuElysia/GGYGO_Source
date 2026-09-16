/**
 * @file GGYGOCharacterMovementComponent.cpp
 * @brief 项目 CMC 实现
 */
#include "Character/Components/GGYGOCharacterMovementComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Animation/AnimInstance.h"
#include "Character/Components/GGYGOCurveRootMotionSource.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterMovementComponent)

namespace GGYGOMovementConstants
{
	/** 判定"正在移动"的水平速度阈值（cm/s）。用于过滤碰撞挤压等微小残余速度。 */
	constexpr float MovingSpeedThreshold = 10.0f;

	/**
	 * 单帧 DeltaTime 上限（秒）。
	 *
	 * 卡顿帧（加载、断点）会带来一个巨大的 DeltaTime，直接累加会让走跑计时器
	 * 一帧跳过阈值，玩家感觉"莫名其妙就跑起来了"。钳住比忽略好 ——
	 * 忽略会让长时间低帧率下永远升不了档。
	 */
	constexpr float MaxClampedDelta = 0.1f;

	/** 走跑计时器上限（秒）。防止长时间行走导致浮点累加失去精度。 */
	constexpr float MaxWalkHoldSeconds = 3600.0f;

	/**
	 * 步态在压缩标志位里的编码。
	 *
	 * CMC 给项目预留了四个自定义位（FLAG_Custom_0..3）。三个步态值需要 2 位，
	 * 用掉 0 和 1，剩下 2 和 3 给后续功能（蹲伏、锁定）。
	 */
	constexpr uint8 GaitFlagShift = 0;
	constexpr uint8 GaitFlagMask = FSavedMove_Character::FLAG_Custom_0 | FSavedMove_Character::FLAG_Custom_1;

	/**
	 * "曲线正在接管转身"标志位。
	 *
	 * 只同步这一个 bit 而不是完整相位：`Turning` 与 `Braking` 的移动行为完全一致
	 * （方向与朝向都由曲线接管），`RunOut` 与普通移动无异，
	 * 所以三个相位对接收端只有两种含义。
	 */
	constexpr uint8 TurnBackCurveDrivenFlag = FSavedMove_Character::FLAG_Custom_2;

	/**
	 * 曲线位移源的实例名。
	 *
	 * root motion source 按名字管理（挂载、查询、摘除），所以每个语义段必须有
	 * 各自的名字。同名会被引擎当成同一个源，`Matches` 也用它做网络匹配。
	 */
	const FName CurveBrakeSourceName(TEXT("GGYGO.CurveBrake"));
	const FName CurveTurnBackSourceName(TEXT("GGYGO.CurveTurnBack"));

	/**
	 * 曲线位移源的优先级。
	 *
	 * 只有最高优先级的 Override 源生效，其余被忽略。刹停与转身在设计上互斥，
	 * 给转身更高的值是兜底：万一两者同时在场，转身赢。
	 */
	constexpr uint16 CurveBrakePriority = 10;
	constexpr uint16 CurveTurnBackPriority = 20;
}

// ============================================================================
// FSavedMove_GGYGO
// ============================================================================

void FSavedMove_GGYGO::Clear()
{
	Super::Clear();

	SavedGait = EGGYGOGait::None;
	SavedWalkHoldTimer = 0.0f;
	bSavedWantsRunOnNextMove = false;
	SavedCurveMotion.Reset();

	SavedTurnBackPhase = EGGYGOTurnBackPhase::None;
	SavedTurnBackElapsed = 0.0f;
	SavedTurnBackEntryYaw = 0.0f;
	bSavedTurnBackInputLatched = false;
}

void FSavedMove_GGYGO::SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData)
{
	Super::SetMoveFor(C, InDeltaTime, NewAccel, ClientData);

	if (const UGGYGOCharacterMovementComponent* MoveComp = C ? Cast<UGGYGOCharacterMovementComponent>(C->GetCharacterMovement()) : nullptr)
	{
		SavedGait = MoveComp->ResolvedGait;
		SavedWalkHoldTimer = MoveComp->WalkHoldTimer;
		bSavedWantsRunOnNextMove = MoveComp->bWantsRunOnNextMove;
		SavedCurveMotion = MoveComp->CurveMotion;

		SavedTurnBackPhase = MoveComp->TurnBackPhase;
		SavedTurnBackElapsed = MoveComp->TurnBackElapsed;
		SavedTurnBackEntryYaw = MoveComp->TurnBackEntryYaw;
		bSavedTurnBackInputLatched = MoveComp->bTurnBackInputLatched;

		// 曲线位移源在场时禁止 move 合并。
		//
		// 合并会把角色拉回起点重演一次更长的 move，而曲线速度是逐帧变化的
		// （走路刹停从 459 一路衰减到 0），重演时只剩下最后一帧的曲线值，
		// 算出来的滑行距离与客户端实际走过的不同。
		//
		// 引擎自己的 `CanCombineWith` 只挡 anim montage 的 root motion，
		// 不挡 root motion source；而刹停期间两帧的 `Acceleration` 都是零，
		// 恰好落在引擎"允许合并"的分支里。所以必须自己挡。
		if (MoveComp->HasCurveRootMotionSource())
		{
			bForceNoCombine = true;
		}
	}
}

void FSavedMove_GGYGO::PrepMoveFor(ACharacter* C)
{
	Super::PrepMoveFor(C);

	if (UGGYGOCharacterMovementComponent* MoveComp = C ? Cast<UGGYGOCharacterMovementComponent>(C->GetCharacterMovement()) : nullptr)
	{
		// 回放这一帧之前把状态还原到当时的样子。
		// 不还原计时器的话，回放多帧时计时器会从"现在"的值继续累加，
		// 于是回放中途可能升档，而首次执行时并没有 —— 预测就失配了。
		MoveComp->ResolvedGait = SavedGait;
		MoveComp->WalkHoldTimer = SavedWalkHoldTimer;
		MoveComp->bWantsRunOnNextMove = bSavedWantsRunOnNextMove;

		// 恢复曲线量而不是重新采样：回放时动画已经走到别的时间点了。
		// 采样器自身的基线不动 —— 它只在 TickComponent 里推进，
		// 回放污染基线会让回放结束后的第一帧位移出错。
		MoveComp->CurveMotion = SavedCurveMotion;

		MoveComp->TurnBackPhase = SavedTurnBackPhase;
		MoveComp->TurnBackElapsed = SavedTurnBackElapsed;
		MoveComp->TurnBackEntryYaw = SavedTurnBackEntryYaw;
		MoveComp->bTurnBackInputLatched = bSavedTurnBackInputLatched;
	}
}

bool FSavedMove_GGYGO::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const
{
	const FSavedMove_GGYGO* NewGGYGOMove = static_cast<const FSavedMove_GGYGO*>(NewMove.Get());

	// 步态不同不能合并：合并后服务器只会看到一个步态值，
	// 另一帧就会按错误的速度上限重演。
	if (NewGGYGOMove && NewGGYGOMove->SavedGait != SavedGait)
	{
		return false;
	}

	// 契约状态不同同样不能合并 —— 它会改变下一帧的步态解算结果。
	if (NewGGYGOMove && NewGGYGOMove->bSavedWantsRunOnNextMove != bSavedWantsRunOnNextMove)
	{
		return false;
	}

	// 转身期间不合并。这段的位移方向由曲线逐帧给出，合并会丢掉中间帧的方向变化，
	// 服务器重演出的轨迹与客户端不同。
	if (NewGGYGOMove && NewGGYGOMove->SavedTurnBackPhase != SavedTurnBackPhase)
	{
		return false;
	}

	if (SavedTurnBackPhase != EGGYGOTurnBackPhase::None)
	{
		return false;
	}

	return Super::CanCombineWith(NewMove, InCharacter, MaxDelta);
}

uint8 FSavedMove_GGYGO::GetCompressedFlags() const
{
	uint8 Result = Super::GetCompressedFlags();

	// 步态占两位。static_cast 是安全的：EGGYGOGait 只有 0/1/2 三个值。
	Result |= (static_cast<uint8>(SavedGait) << GGYGOMovementConstants::GaitFlagShift) & GGYGOMovementConstants::GaitFlagMask;

	// 只发"曲线是否在接管"。Turning 与 Braking 的移动行为一致，接收端不必区分；
	// RunOut 的移动与普通移动一致，不需要这个位。
	if (SavedTurnBackPhase == EGGYGOTurnBackPhase::Turning
		|| SavedTurnBackPhase == EGGYGOTurnBackPhase::Braking)
	{
		Result |= GGYGOMovementConstants::TurnBackCurveDrivenFlag;
	}

	return Result;
}

// ============================================================================
// FNetworkPredictionData_Client_GGYGO
// ============================================================================

FNetworkPredictionData_Client_GGYGO::FNetworkPredictionData_Client_GGYGO(const UCharacterMovementComponent& ClientMovement)
	: Super(ClientMovement)
{
}

FSavedMovePtr FNetworkPredictionData_Client_GGYGO::AllocateNewMove()
{
	return FSavedMovePtr(new FSavedMove_GGYGO());
}

// ============================================================================
// UGGYGOCharacterMovementComponent
// ============================================================================

UGGYGOCharacterMovementComponent::UGGYGOCharacterMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 动作游戏默认让角色面向移动方向，锁定目标时由能力临时关掉。
	// 这里给的是兜底值，有 MovementSet 时会被覆盖。
	bOrientRotationToMovement = true;
	bUseControllerDesiredRotation = false;
	RotationRate = FRotator(0.0f, 720.0f, 0.0f);

	// CMC 默认不复制自己的属性（移动状态走专门的 RPC 通道）。
	// 这里需要属性复制，因为转身状态要发给模拟代理，而那条信息
	// 不属于移动预测数据。
	SetIsReplicatedByDefault(true);
}

void UGGYGOCharacterMovementComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// SkipOwner：拥有者自己是权威解算方，收到服务器的回传只会与本地预测打架。
	DOREPLIFETIME_CONDITION(UGGYGOCharacterMovementComponent, bReplicatedTurnBackCurveDriven, COND_SkipOwner);
}

void UGGYGOCharacterMovementComponent::BeginPlay()
{
	Super::BeginPlay();

	CacheAbilitySystemComponent();
}

void UGGYGOCharacterMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	// 必须在 Super 之前采样：Super 会推进移动，而移动要用本帧的曲线速度。
	//
	// 采样放在这里而不是 UpdateCharacterStateBeforeMovement，因为后者在
	// sub-stepping 时一帧内会被调用多次，而采样器持有跨帧基线，
	// 多次调用会让第二次之后的差分全为零。
	SampleAnimCurves(DeltaTime);

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UGGYGOCharacterMovementComponent::SampleAnimCurves(float DeltaTime)
{
	const USkeletalMeshComponent* Mesh = CharacterOwner ? CharacterOwner->GetMesh() : nullptr;
	const UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;

	if (!AnimInstance)
	{
		// 没有动画就没有曲线。丢弃基线，避免 Mesh 恢复后拿旧基线算出一个大位移。
		CurveMotion.Reset();
		CurveSampler.ResetBaseline();
		return;
	}

	CurveSampler.Sample(*AnimInstance, DeltaTime, CurveMotion);
}

float UGGYGOCharacterMovementComponent::GetScaledCurveSpeed() const
{
	if (!MovementSet || !MovementSet->bUseCurveDrivenSpeed || !CurveMotion.HasUsableSpeed())
	{
		return 0.0f;
	}

	const float Scaled = CurveMotion.Speed * FMath::Max(MovementSet->RootMotionScale, 0.0f);

	return FMath::IsFinite(Scaled) ? FMath::Max(Scaled, 0.0f) : 0.0f;
}

bool UGGYGOCharacterMovementComponent::IsCurveDrivingSpeed() const
{
	return GetScaledCurveSpeed() > KINDA_SMALL_NUMBER;
}

bool UGGYGOCharacterMovementComponent::HasCurveRootMotionSource() const
{
	// 按类型判定而不是按实例名：将来再加曲线驱动段（冲刺、受击位移）时不用改这里。
	for (const TSharedPtr<FRootMotionSource>& Source : CurrentRootMotion.RootMotionSources)
	{
		if (Source.IsValid() && Source->GetScriptStruct() == FRootMotionSource_GGYGOCurve::StaticStruct())
		{
			return true;
		}
	}

	return false;
}

void UGGYGOCharacterMovementComponent::CacheAbilitySystemComponent()
{
	// 经 PawnExtension 拿 ASC 而不是自己 FindComponentByClass：
	// ASC 可能不在本 Actor 上（队伍级 ASC 挂 PlayerState），
	// 而 PawnExtension 是"当前该用哪个 ASC"这个问题的唯一答案来源。
	if (UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetOwner()))
	{
		// 用 RegisterAndCall：CMC 的 BeginPlay 与 ASC 初始化的先后不固定，
		// 已经就绪时这个调用会立刻回调一次，不会漏。
		PawnExtComp->OnAbilitySystemInitialized_RegisterAndCall(
			FSimpleMulticastDelegate::FDelegate::CreateWeakLambda(this, [this]()
			{
				// 回调里重新查找而不是捕获组件指针：捕获会让 lambda 的有效性
				// 依赖两个对象的生命周期，而 WeakLambda 只保证了 this 那一个。
				if (const UGGYGOPawnExtensionComponent* PawnExt = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetOwner()))
				{
					AbilitySystemComponent = PawnExt->GetGGYGOAbilitySystemComponent();
				}
			}));

		PawnExtComp->OnAbilitySystemUninitialized_Register(
			FSimpleMulticastDelegate::FDelegate::CreateWeakLambda(this, [this]()
			{
				// 必须清掉：ASC 换 Avatar 后旧指针指向的 ASC 已不代表本角色，
				// 继续用它查 Tag 会读到别人的状态。
				AbilitySystemComponent = nullptr;
			}));
	}
}

void UGGYGOCharacterMovementComponent::SetMovementSet(const UGGYGOMovementSet* InMovementSet)
{
	MovementSet = InMovementSet;

	ApplyMovementSetToComponent();
}

void UGGYGOCharacterMovementComponent::ApplyMovementSetToComponent()
{
	if (!MovementSet)
	{
		// 没有资产时保留构造函数与引擎的默认值，不强行归零 ——
		// 归零会让"忘配 MovementSet"表现为角色完全不动，比用默认值难排查得多。
		return;
	}

	MaxAcceleration = FMath::Max(MovementSet->MaxAcceleration, 0.0f);
	BrakingDecelerationWalking = FMath::Max(MovementSet->BrakingDecelerationWalking, 0.0f);
	GroundFriction = FMath::Max(MovementSet->GroundFriction, 0.0f);

	bOrientRotationToMovement = MovementSet->bOrientRotationToMovement;

	// 只设 Yaw。Pitch / Roll 由动画负责，CMC 去转它们会和动画打架。
	RotationRate = FRotator(0.0f, FMath::Max(MovementSet->RotationYawRate, 0.0f), 0.0f);

	// MaxWalkSpeed 有意**不在这里设置**。它由 GetMaxSpeed() 按步态动态返回，
	// 在这里写一个固定值只会造成"两个速度来源"的疑惑。
}

float UGGYGOCharacterMovementComponent::GetMaxSpeed() const
{
	// 被禁止移动时返回 0 而不是提前 return 或清 Velocity。
	// 走 GetMaxSpeed 这条路，CMC 会用自己的制动减速度把速度平滑压到零，
	// 而 StopMovementImmediately 那种硬停会让角色瞬间定住，
	// 在联机下还会与服务器的重演结果不一致。
	if (IsMovementBlockedByTag())
	{
		return 0.0f;
	}

	// 非地面移动交回基类：游泳、飞行、下落各有自己的速度体系，
	// 步态与动画曲线只对地面移动有意义。
	if (MovementMode != MOVE_Walking && MovementMode != MOVE_NavWalking)
	{
		return Super::GetMaxSpeed();
	}

	if (!MovementSet)
	{
		return Super::GetMaxSpeed();
	}

	// 曲线速度优先。它逐帧给出动画本身的位移速率，脚步与位移因此严格对齐。
	const float CurveSpeed = GetScaledCurveSpeed();
	if (CurveSpeed > KINDA_SMALL_NUMBER)
	{
		return CurveSpeed;
	}

	const float GaitSpeed = MovementSet->GetSpeedForGait(ResolvedGait);

	// 曲线不可用时的非本地控制端要放宽上界。
	//
	// 曲线值是本地动画状态，服务器若没有评估动画就采不到。此时服务器按步态的
	// 固定速度重演客户端的 move，而客户端跑的是曲线速度 —— 两者不等就会
	// 持续触发位置校正，表现为角色被反复拉回。放宽到覆盖曲线峰值的上界后，
	// 服务器不再因为这个差异而校正。
	//
	// 只在"曲线不可用"时放宽：服务器能采到曲线时走上面那条分支，精确一致。
	const bool bIsLocallyControlled = CharacterOwner && CharacterOwner->IsLocallyControlled();
	if (!bIsLocallyControlled && MovementSet->bUseCurveDrivenSpeed && ResolvedGait != EGGYGOGait::None)
	{
		return FMath::Max(GaitSpeed, FMath::Max(MovementSet->MaxCurveDrivenSpeed, 0.0f));
	}

	return GaitSpeed;
}

bool UGGYGOCharacterMovementComponent::IsMovementBlockedByTag() const
{
	// ASC 的 Tag 是复制的，所以这个判定在服务器与客户端上一致，
	// 可以安全地参与预测。
	return AbilitySystemComponent
		&& AbilitySystemComponent->HasMatchingGameplayTag(GGYGOGameplayTags::Restriction_CantMove);
}

bool UGGYGOCharacterMovementComponent::HasMoveInput() const
{
	return GetCurrentAcceleration().SizeSquared2D() > KINDA_SMALL_NUMBER;
}

void UGGYGOCharacterMovementComponent::RequestRunOnNextMove()
{
	bWantsRunOnNextMove = true;
}

void UGGYGOCharacterMovementComponent::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	// **只有本地控制端解算步态**，其余角色一律采用压缩标志位里客户端算好的值。
	//
	// 这一条是预测正确性的关键。若服务器也自行解算，它的走跑计时器会与客户端错开
	// （两端对"输入从哪一帧开始"的采样不可能完全一致），于是升档时机相差若干帧，
	// 期间两端速度上限不同，位置校正会持续触发。
	//
	// 本地控制涵盖三种情况：客户端自己的角色（预测）、listen server 的主机角色、
	// 服务器上的 AI 角色（AIController 在服务器，所以也算本地控制）。
	if (CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		ResolveGait(DeltaSeconds);
		UpdateTurnBack(DeltaSeconds);
		UpdateCurveBrake();
	}

	// 转身段的位移源在所有角色上同步，所以放在本地控制判断之外。
	//
	// 必须在这里而不是更早或更晚：引擎紧接着就会在 `PerformMovement` 里
	// `CurrentRootMotion.PrepareRootMotion`，晚一步挂的 source 要等下一帧才生效。
	UpdateTurnBackRootMotion();

	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
}

void UGGYGOCharacterMovementComponent::ResolveGait(float DeltaSeconds)
{
	// 用 Acceleration 而非原始摇杆输入判定"有没有移动意图"。
	// 关键原因是 Acceleration 已被 FSavedMove_Character 保存，
	// move 回放时取值与首次执行一致；原始输入没有这个保证。
	const bool bHasMoveInput = GetCurrentAcceleration().SizeSquared2D() > KINDA_SMALL_NUMBER;
	const bool bBlocked = IsMovementBlockedByTag();
	const bool bOnGround = IsMovingOnGround();

	// 边沿：起步瞬间与解禁瞬间都要让计时器重新开始，
	// 否则"走两秒→松手→再按"会接着之前的两秒继续累加，等效于缩短了阈值。
	const bool bMoveInputRising = bHasMoveInput && !bPreviousHasMoveInput;
	const bool bBlockReleased = !bBlocked && bPreviousMovementBlocked;

	EGGYGOGait FrameGait = EGGYGOGait::None;

	if (bBlocked)
	{
		// 被禁止移动时不消费 Run 契约 —— 禁止解除后玩家仍然期望闪避后直接跑。
		FrameGait = EGGYGOGait::None;
	}
	else if (!bHasMoveInput)
	{
		// 松手即结束本次移动，同时丢弃未使用的契约：
		// 闪避后如果玩家没有立刻接移动，那个"直接进 Run"的意图就已经过期了。
		bWantsRunOnNextMove = false;
		FrameGait = EGGYGOGait::None;
	}
	else if (bWantsRunOnNextMove)
	{
		// 契约只在这里消费。
		bWantsRunOnNextMove = false;
		FrameGait = EGGYGOGait::Run;
	}
	else if (ResolvedGait == EGGYGOGait::Run)
	{
		// 单向滞回：一旦进入 Run 就保持，直到完全停止移动才降档。
		// 若允许回落 Walk，摇杆幅度的轻微抖动会造成走跑反复切换。
		FrameGait = EGGYGOGait::Run;
	}
	else
	{
		FrameGait = EGGYGOGait::Walk;
	}

	UpdateWalkHoldTimer(FrameGait, bHasMoveInput, bBlocked, bOnGround, bMoveInputRising, bBlockReleased, DeltaSeconds);

	// 计时达标则本帧立即升档并归零，不等下一帧 —— 延后一帧会让升档时机
	// 与配置的阈值差一个帧时长，在低帧率下可感知。
	if (FrameGait == EGGYGOGait::Walk && bHasMoveInput && !bBlocked)
	{
		const float Threshold = MovementSet
			? MovementSet->GetSanitizedWalkToRunHoldSeconds()
			: TNumericLimits<float>::Max();

		if (WalkHoldTimer >= Threshold)
		{
			FrameGait = EGGYGOGait::Run;
			WalkHoldTimer = 0.0f;
		}
	}

	ResolvedGait = FrameGait;

	bPreviousHasMoveInput = bHasMoveInput;
	bPreviousMovementBlocked = bBlocked;
}

void UGGYGOCharacterMovementComponent::UpdateWalkHoldTimer(EGGYGOGait FrameGait, bool bHasMoveInput, bool bBlocked, bool bOnGround, bool bMoveInputRising, bool bBlockReleased, float DeltaSeconds)
{
	// DeltaTime 不可信时**什么都不做**，既不归零也不累加。
	// 归零会让偶发的坏帧白白清掉玩家已积累的行走时间。
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0f)
	{
		return;
	}

	if (!FMath::IsFinite(WalkHoldTimer))
	{
		WalkHoldTimer = 0.0f;
	}

	// 任一条成立就归零重来。两个"沿"条件不能省：
	// 没有它们的话，"走两秒 → 松手 → 立刻再按"会接着之前的两秒继续累加，
	// 等效于把阈值缩短了，玩家会觉得升跑时机飘忽。
	const bool bShouldReset =
		!bHasMoveInput
		|| bBlocked
		|| !bOnGround
		|| FrameGait == EGGYGOGait::Run
		|| bMoveInputRising
		|| bBlockReleased;

	if (bShouldReset)
	{
		WalkHoldTimer = 0.0f;
		return;
	}

	if (FrameGait == EGGYGOGait::Walk)
	{
		const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, GGYGOMovementConstants::MaxClampedDelta);
		WalkHoldTimer = FMath::Clamp(WalkHoldTimer + ClampedDelta, 0.0f, GGYGOMovementConstants::MaxWalkHoldSeconds);
	}
}

void UGGYGOCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);

	// 这是非本地控制端获得步态的**唯一**途径。
	// 与 UpdateCharacterStateBeforeMovement 里的 IsLocallyControlled() 判断配对：
	// 本地控制端自己算、不读这里；其余角色不算、只读这里。两条路径互斥，不会互相覆盖。
	const uint8 GaitBits = (Flags & GGYGOMovementConstants::GaitFlagMask) >> GGYGOMovementConstants::GaitFlagShift;

	// 越界保护：网络数据不可信，收到 3 时不能直接 static_cast 成枚举。
	ResolvedGait = (GaitBits <= static_cast<uint8>(EGGYGOGait::Run))
		? static_cast<EGGYGOGait>(GaitBits)
		: EGGYGOGait::None;

	// 只恢复"曲线是否在接管"这一个事实。Turning 与 Braking 的区别只在转角还不还在变，
	// 那是表现层的事，而表现由本地动画层自己驱动，不需要从这里取。
	const bool bCurveDriven = (Flags & GGYGOMovementConstants::TurnBackCurveDrivenFlag) != 0;

	// 服务器把它转成复制属性，模拟代理才能知道转身正在发生 ——
	// 压缩标志位只走客户端→服务器一个方向，不会转发给其它客户端。
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_Authority)
	{
		bReplicatedTurnBackCurveDriven = bCurveDriven;
	}

	if (bCurveDriven)
	{
		if (TurnBackPhase == EGGYGOTurnBackPhase::None)
		{
			// 接收端首次得知转身开始。入口朝向取当前朝向 —— 这是能拿到的最好近似，
			// 与发送端的入口朝向可能有偏差，偏差大小取决于网络延迟。
			TurnBackEntryYaw = UpdatedComponent
				? UpdatedComponent->GetComponentRotation().Yaw
				: 0.0f;

			// 恢复成 Turning 而不是 Braking：这一端不知道转角走到哪了，
			// 而 Turning 的行为是两段的超集（照样累加转角增量，
			// 曲线在 Braking 段给的增量本就是 0，所以不会转多）。
			TurnBackPhase = EGGYGOTurnBackPhase::Turning;
		}
	}
	else if (TurnBackPhase != EGGYGOTurnBackPhase::None)
	{
		// 曲线接管已结束。RunOut 段的移动与普通移动一致，直接复位即可。
		ResetTurnBack();
	}
}

void UGGYGOCharacterMovementComponent::PhysicsRotation(float DeltaTime)
{
	// 转身的曲线接管段（Turning / Braking）朝向由 `RootMotion_Yaw` 曲线驱动。
	//
	// 在这里接管而不是在外部调 AddActorWorldRotation，是为了不与 CMC 的
	// 自动朝向对齐（bOrientRotationToMovement）互相争夺 Yaw ——
	// 绕过 Super 就等于这一帧只有曲线在转角色，不需要临时关掉那些开关再恢复。
	//
	// 累加增量而不是直接设成 `EntryYaw + YawTotal`：增量对入口朝向的误差不敏感，
	// 模拟代理那边的入口朝向是个近似值，用绝对值会让它一进入转身就跳一下。
	// 曲线的累计转角已解 ±180 折叠，所以差分不会出现一帧 358 度的假增量。
	if (IsTurnBackCurveDriven() && UpdatedComponent)
	{
		const float YawDelta = FMath::IsFinite(CurveMotion.YawDeltaDegrees)
			? CurveMotion.YawDeltaDegrees
			: 0.0f;

		if (!FMath::IsNearlyZero(YawDelta))
		{
			FRotator NewRotation = UpdatedComponent->GetComponentRotation();
			NewRotation.Yaw += YawDelta;

			// 走 MoveUpdatedComponent 而不是直接设置变换：它会处理碰撞扫掠，
			// 也让这次旋转进入 CMC 的移动更新记录，回放时结果一致。
			MoveUpdatedComponent(FVector::ZeroVector, NewRotation.Quaternion(), /*bSweep=*/true);
		}

		return;
	}

	Super::PhysicsRotation(DeltaTime);
}

EGGYGOTurnBackPhase UGGYGOCharacterMovementComponent::GetTurnBackPhase() const
{
	// 模拟代理没有本地解算的相位，只有复制来的一个 bool。
	// 映射成 Turning 是因为动画层只判断 `!= None`，具体是哪一段它不关心。
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		return bReplicatedTurnBackCurveDriven ? EGGYGOTurnBackPhase::Turning : EGGYGOTurnBackPhase::None;
	}

	return TurnBackPhase;
}

bool UGGYGOCharacterMovementComponent::IsTurnBackRunOut() const
{
	// 模拟代理恒为 false：复制过来的 bool 只表达"曲线是否在接管"，
	// 而 RunOut 段的移动与普通移动一致，那一端不需要区分。
	return GetTurnBackPhase() == EGGYGOTurnBackPhase::RunOut;
}

bool UGGYGOCharacterMovementComponent::IsTurnBackCurveDriven() const
{
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		return bReplicatedTurnBackCurveDriven;
	}

	return TurnBackPhase == EGGYGOTurnBackPhase::Turning
		|| TurnBackPhase == EGGYGOTurnBackPhase::Braking;
}

void UGGYGOCharacterMovementComponent::ResetTurnBack()
{
	TurnBackPhase = EGGYGOTurnBackPhase::None;
	TurnBackElapsed = 0.0f;
	TurnBackEntryYaw = 0.0f;

	// 不清 bTurnBackInputLatched：它的解闩条件是"输入离开反向阈值"，
	// 而转身刚结束时玩家往往还按着同一个方向键。在这里清掉会让那个方向键
	// 立刻触发下一次转身，角色原地反复转身。
}

void UGGYGOCharacterMovementComponent::ApplyCurveRootMotionSource(FName InstanceName, uint16 Priority, float BaseYaw, bool bEndOnZeroSpeed)
{
	// 已在场就不重挂，于是调用方可以每帧无条件调用。
	if (GetRootMotionSource(InstanceName).IsValid())
	{
		return;
	}

	TSharedPtr<FRootMotionSource_GGYGOCurve> Source = MakeShared<FRootMotionSource_GGYGOCurve>();
	Source->InstanceName = InstanceName;
	Source->Priority = Priority;
	Source->BaseYaw = BaseYaw;
	Source->SpeedScale = MovementSet ? FMath::Max(MovementSet->RootMotionScale, 0.0f) : 1.0f;
	Source->bEndOnZeroSpeed = bEndOnZeroSpeed;

	ApplyRootMotionSource(Source);
}

void UGGYGOCharacterMovementComponent::UpdateTurnBackRootMotion()
{
	const bool bCurveDrivenEnabled = MovementSet && MovementSet->bUseCurveDrivenSpeed;

	if (!bCurveDrivenEnabled || !IsTurnBackCurveDriven())
	{
		RemoveRootMotionSource(GGYGOMovementConstants::CurveTurnBackSourceName);
		return;
	}

	// 基准取相位入口朝向而不是当前朝向：角色在这一段一直在转，
	// 用当前朝向会把已经转过的角度重复计入，直线滑行被拖成弧线。
	//
	// 不自我终结：生命周期由相位机管。转身中间那几帧曲线速度为 0 时
	// source 输出零速度把角色压住，而不是把控制权交还给正按着反方向键的玩家。
	ApplyCurveRootMotionSource(
		GGYGOMovementConstants::CurveTurnBackSourceName,
		GGYGOMovementConstants::CurveTurnBackPriority,
		TurnBackEntryYaw,
		/*bEndOnZeroSpeed=*/false);
}

void UGGYGOCharacterMovementComponent::UpdateCurveBrake()
{
	// 曲线驱动被整体关掉时不参与。这个开关是调试用的总闸，
	// 关掉之后移动应当完全回退到配置的固定速度。
	const bool bCurveDrivenEnabled = MovementSet && MovementSet->bUseCurveDrivenSpeed;

	// 转身有自己的一套曲线接管（含 `RunOut` 段刻意把方向交回输入），
	// 让刹停在那期间插手会覆盖掉那个设计。
	//
	// 有输入就摘掉：刹停可以被随时打断，玩家按方向应当立刻恢复正常移动。
	// 判据用 Acceleration 而非原始摇杆输入 —— 它已被 FSavedMove_Character 保存，
	// 回放时取值与首次执行一致。
	const bool bHasMoveInput = GetCurrentAcceleration().SizeSquared2D() > KINDA_SMALL_NUMBER;

	if (!bCurveDrivenEnabled
		|| bHasMoveInput
		|| TurnBackPhase != EGGYGOTurnBackPhase::None
		|| !IsMovingOnGround())
	{
		RemoveRootMotionSource(GGYGOMovementConstants::CurveBrakeSourceName);
		return;
	}

	// 无输入而曲线仍在给速度 —— 只有 `_End` 这类刹停动画会出现这种组合，
	// 站立与静止兜底姿势的 `RootMotion_Speed` 恒为 0。所以这一个条件就够了，
	// 不需要额外记"松手那一帧"的下降沿。
	//
	// 不需要在这里判断"曲线停了没"：source 自己会在速度归零时置 Finished，
	// 引擎下一帧的 `CleanUpInvalidRootMotion` 会把它摘掉。
	if (!CurveMotion.HasUsableSpeed())
	{
		return;
	}

	// 基准取当前朝向：松手那一刻角色的朝向就是刹停动画的段起点朝向。
	// 自我终结：曲线衰减到 0 即滑行结束，不需要外部状态机跟着。
	ApplyCurveRootMotionSource(
		GGYGOMovementConstants::CurveBrakeSourceName,
		GGYGOMovementConstants::CurveBrakePriority,
		UpdatedComponent ? UpdatedComponent->GetComponentRotation().Yaw : 0.0f,
		/*bEndOnZeroSpeed=*/true);
}

bool UGGYGOCharacterMovementComponent::IsReverseRunInput() const
{
	// 只有跑动中才触发。走路时的反向输入应当直接转身走回去，
	// 那个速度下不需要刹车动作。
	if (ResolvedGait != EGGYGOGait::Run || !MovementSet)
	{
		return false;
	}

	const FVector InputDirection = GetCurrentAcceleration().GetSafeNormal2D();
	if (InputDirection.IsNearlyZero() || !UpdatedComponent)
	{
		return false;
	}

	const FVector Forward = UpdatedComponent->GetForwardVector().GetSafeNormal2D();
	if (Forward.IsNearlyZero())
	{
		return false;
	}

	return FVector::DotProduct(Forward, InputDirection) <= MovementSet->TurnBackReverseInputDotThreshold;
}

void UGGYGOCharacterMovementComponent::UpdateTurnBack(float DeltaSeconds)
{
	if (!MovementSet || !IsMovingOnGround())
	{
		// 离地时转身没有意义，且落地后的朝向应由落地动画决定。
		ResetTurnBack();
		return;
	}

	const bool bReverseInput = IsReverseRunInput();

	const float ClampedDelta = (FMath::IsFinite(DeltaSeconds) && DeltaSeconds > 0.0f)
		? DeltaSeconds
		: 0.0f;

	// 兜底超时。正常出口是 RunOut 段松手，这里防的是"相位卡住"：
	// 相位推进依赖曲线，而转身动画可能被别的状态打断、或者压根没烘焙曲线，
	// 那时相位永远推不动，bTurnBackInputLatched 也就永远不解，下一次转身再也触发不了。
	const bool bTimedOut = TurnBackPhase != EGGYGOTurnBackPhase::None
		&& TurnBackElapsed >= FMath::Max(MovementSet->TurnBackDurationSeconds, 0.0f);

	if (bTimedOut)
	{
		ResetTurnBack();
		return;
	}

	switch (TurnBackPhase)
	{
	case EGGYGOTurnBackPhase::None:
	{
		if (!bReverseInput)
		{
			// 输入离开反向阈值，解除闩，允许下一次触发。
			bTurnBackInputLatched = false;
			break;
		}

		if (bTurnBackInputLatched)
		{
			break;
		}

		TurnBackPhase = EGGYGOTurnBackPhase::Turning;
		TurnBackElapsed = 0.0f;
		bTurnBackInputLatched = true;

		// 记下入口朝向。曲线的位移分量以动画段起点为基准，这个值就是那个基准，
		// 整段转身的世界位移方向都靠它换算，中途不更新。
		TurnBackEntryYaw = UpdatedComponent
			? UpdatedComponent->GetComponentRotation().Yaw
			: 0.0f;
		break;
	}

	case EGGYGOTurnBackPhase::Turning:
	{
		// 转身段不可打断，即使玩家已经松手也要走完 ——
		// 中途放弃会让刹车动作停在一半，角色姿态与速度对不上。
		TurnBackElapsed += ClampedDelta;

		// 转角已经走够且本帧不再变化，说明动画的转身部分播完了。
		//
		// 两个条件都要：只看"本帧增量为 0"会把动画段刚切换、采样基线尚未建立的
		// 那一帧（增量恒为 0）误判成转完；只看"累计转角够大"则会在转角还在
		// 继续变化时提前切换。
		const bool bYawReached = FMath::Abs(CurveMotion.YawTotalDegrees)
			>= FMath::Max(MovementSet->TurnBackMinYawDegrees, 0.0f);
		const bool bYawSettled = FMath::Abs(CurveMotion.YawDeltaDegrees)
			<= FMath::Max(MovementSet->TurnBackYawSettleDegrees, 0.0f);

		if (bYawReached && bYawSettled)
		{
			TurnBackPhase = EGGYGOTurnBackPhase::Braking;
		}
		break;
	}

	case EGGYGOTurnBackPhase::Braking:
	{
		// 倒滑刹车段同样不可打断：身体已经转过来了，人还在往原方向滑。
		TurnBackElapsed += ClampedDelta;

		// 曲线方向在动画段起点坐标系里的前向分量由正转负，就是"开始朝反方向跑出"。
		// 附带速度门限是因为刹车段末速会掉到很低（Pyrios 是 52 cm/s），
		// 那时单帧位移不足 1cm，归一化方向容易抖，容易被一帧噪声提前推进相位。
		const bool bForwardFlipped = CurveMotion.Direction.X
			<= -FMath::Clamp(MovementSet->TurnBackRunOutForwardThreshold, 0.0f, 1.0f);
		const bool bFastEnough = CurveMotion.Speed
			>= FMath::Max(MovementSet->TurnBackRunOutMinSpeed, 0.0f);

		if (bForwardFlipped && bFastEnough)
		{
			TurnBackPhase = EGGYGOTurnBackPhase::RunOut;
		}
		break;
	}

	case EGGYGOTurnBackPhase::RunOut:
	{
		TurnBackElapsed += ClampedDelta;

		// 方向已交回输入，松手即结束：继续占着转身状态只会阻止下一次转身触发。
		if (!HasMoveInput())
		{
			ResetTurnBack();
		}
		break;
	}
	}
}

FNetworkPredictionData_Client* UGGYGOCharacterMovementComponent::GetPredictionData_Client() const
{
	if (!ClientPredictionData)
	{
		// const_cast 是 CMC 这个扩展点的既定写法：基类把该函数声明为 const，
		// 但延迟创建预测数据必须写成员。引擎自身的实现也这么做。
		UGGYGOCharacterMovementComponent* MutableThis = const_cast<UGGYGOCharacterMovementComponent*>(this);
		MutableThis->ClientPredictionData = new FNetworkPredictionData_Client_GGYGO(*this);
	}

	return ClientPredictionData;
}

// ===== 供动画层读取的派生量 =====

float UGGYGOCharacterMovementComponent::GetHorizontalSpeed() const
{
	return Velocity.Size2D();
}

bool UGGYGOCharacterMovementComponent::IsMovingHorizontally() const
{
	return GetHorizontalSpeed() > GGYGOMovementConstants::MovingSpeedThreshold;
}

FVector UGGYGOCharacterMovementComponent::GetHorizontalVelocityDirection() const
{
	return FVector(Velocity.X, Velocity.Y, 0.0f).GetSafeNormal2D();
}

float UGGYGOCharacterMovementComponent::GetLocalVelocityAngle() const
{
	if (!CharacterOwner)
	{
		return 0.0f;
	}

	const FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.0f);
	const FVector LocalVelocity = CharacterOwner->GetActorTransform().InverseTransformVector(HorizontalVelocity);

	if (LocalVelocity.Size2D() <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	// 局部空间 X 为前、Y 为右，所以 Atan2(Y, X) 得到的就是"偏离正前多少度，右为正"。
	return FMath::RadiansToDegrees(FMath::Atan2(LocalVelocity.Y, LocalVelocity.X));
}

void UGGYGOCharacterMovementComponent::GetLocalVelocityBlend(float& OutBlendX, float& OutBlendY) const
{
	OutBlendX = 0.0f;
	OutBlendY = 0.0f;

	if (!CharacterOwner)
	{
		return;
	}

	const FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.0f);
	const FVector LocalVelocity = CharacterOwner->GetActorTransform().InverseTransformVector(HorizontalVelocity);
	const float LocalLength = LocalVelocity.Size2D();

	if (LocalLength <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// 轴序刻意交换：BlendSpace 的 X 轴配的是"右"，Y 轴配的是"前"，
	// 而 UE 局部空间是 X 前、Y 右。动画资产已按这个约定配好，保持不变。
	OutBlendX = LocalVelocity.Y / LocalLength;
	OutBlendY = LocalVelocity.X / LocalLength;
}
