/**
 * @file BaseCharacter.cpp
 * @brief 所有角色的基类实现 - 管线时序分发 + GAS 初始化
 */
#include "BaseCharacter.h"
#include "Components/CapsuleComponent.h"

ABaseCharacter::ABaseCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	// ============================================================
	// GAS 核心（CreateDefaultSubobject → 作为 Actor 子对象 → GC 管理）
	// ============================================================

	ASC = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("ASC"));

	// AttributeSet 必须用 CreateDefaultSubobject 在构造函数创建
	// 因为 ASC::InitAbilityActorInfo 会扫描子对象自动发现 UAttributeSet
	// 如果在 BeginPlay 里 NewObject，ASC 发现不了它
	AttributeSet = CreateDefaultSubobject<UGGYGOAttributeSet>(TEXT("AttributeSet"));

	// ============================================================
	// 管线子系统（纯 C++ 类，TUniquePtr 管理生命周期）
	// ============================================================

	// 创建数据容器
	InputData = MakeUnique<FInputData>();
	RuntimeData = MakeUnique<FRuntimeData>();

	// 创建管线
	InputPipeline = MakeUnique<FInputPipeline>(*InputData);
	IntentPipeline = MakeUnique<FIntentPipeline>();

	// 创建驱动层
	MotionDriver = MakeUnique<FMotionDriver>();

	// 创建状态机
	StateMachine = MakeUnique<FCharacterStateMachine>();

	// 创建仲裁管线（不给 ASC，在 BeginPlay 中 Init 时注入）
	ArbiterPipeline = MakeUnique<FArbiterPipeline>();
}

UAbilitySystemComponent* ABaseCharacter::GetAbilitySystemComponent() const
{
	return ASC;
}

void ABaseCharacter::BeginPlay()
{
	Super::BeginPlay();

	// ============================================================
	// GAS 初始化（必须在所有子系统之前，因为其他系统可能依赖 ASC）
	// ============================================================

	if (ASC)
	{
		// InitAbilityActorInfo 告诉 ASC "我属于谁"
		// 参数1 OwnerActor  — Ability 的"拥有者"（通常是 Controller 或 PlayerState）
		// 参数2 AvatarActor — Ability 的"物理表现"（通常是 Character 自己）
		// 单人游戏简单处理：两个参数都传 this
		ASC->InitAbilityActorInfo(this, this);

		// 注入真实 ASC 到所有依赖 ASC 的子系统
		ArbiterPipeline->Init(ASC);
	}

	// 显示胶囊体，方便观察角色碰撞和位移
#if !UE_BUILD_SHIPPING
	GetCapsuleComponent()->SetHiddenInGame(false);
#endif

	// ============================================================
	// 管线子系统初始化
	// ============================================================

	// 初始化意图管线（注入 ACharacter 和 Mesh 依赖）
	IntentPipeline->Init(this, GetMesh());

	// 初始化运动驱动器（缓存组件引用 + 关闭自动 Root Motion）
	MotionDriver->Init(this);

	// 初始化状态机（创建所有状态实例 + 进入 Idle）
	StateMachine->Init();
}

void ABaseCharacter::GiveDefaultAbilities()
{
	if (!ASC) return;

	for (TSubclassOf<UGameplayAbility>& AbilityClass : DefaultAbilities)
	{
		if (AbilityClass)
		{
			// GiveAbility 授予 Ability → 角色"拥有"了这个技能
			// FGameplayAbilitySpec 参数: (AbilityClass, Level, InputID, SourceObject)
			// Level=1 表示技能等级1，InputID=-1 表示不绑定到特定输入键
			// 授予后可以通过 TryActivateAbilityByClass 激活
			ASC->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1, INDEX_NONE, this));
		}
	}
}

void ABaseCharacter::ApplyDefaultEffects()
{
	if (!ASC) return;

	for (TSubclassOf<UGameplayEffect>& EffectClass : DefaultEffects)
	{
		if (EffectClass)
		{
			// 创建 GE 上下文（记录来源、目标等信息）
			FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
			Context.AddSourceObject(this);

			// 创建 GE 实例（Spec = 具体的一次效果应用）
			FGameplayEffectSpecHandle Spec =
				ASC->MakeOutgoingSpec(EffectClass, 1, Context);

			// 应用到自身
			if (Spec.IsValid())
			{
				ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
			}
		}
	}
}

void ABaseCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 1. 仲裁管线：读 GAS Tag → 写仲裁标记（最先执行，后续管线依赖）
	ArbiterPipeline->Process(*RuntimeData, DeltaTime);

	// 2. 输入管线：原始输入 → 防抖/缓冲 → InputData
	InputPipeline->Process(DeltaTime);

	// 3. 意图处理：InputData → RuntimeData 意图
	IntentPipeline->ProcessIntents(*InputData, *RuntimeData);

	// 4. 参数处理：RuntimeData 意图 → RuntimeData 动画参数
	IntentPipeline->ProcessParameters(*RuntimeData, DeltaTime);

	// 5. 状态机：读取意图 + 仲裁标记，决定当前状态
	StateMachine->Update(DeltaTime, *RuntimeData);

	// 6. 运动驱动：RuntimeData 移动数据 → 实际位移
	MotionDriver->Process(DeltaTime, *RuntimeData);

	// 帧末清零
	RuntimeData->ResetFrameIntents();

#if !UE_BUILD_SHIPPING
	// 调试 UI（左上角叠层，每帧刷新）
	// Key0: 原始输入 | Key1: BlendSpace 参数 + 状态 | Key2: 实际速度/移动状态/角度
	// Key3: 动画速度(绿=OK)/缩放 | Key4: Bip001 骨骼状态(绿=OK) + 着地
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(0, 0.f, FColor::White,
			FString::Printf(TEXT("Input:%s"),
				*InputData->CurrentFrame.Move.ToString()));
		GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::Cyan,
			FString::Printf(TEXT("Blend X:%.2f Y:%.2f | State:%d"),
				RuntimeData->AnimBlendX, RuntimeData->AnimBlendY,
				static_cast<uint8>(RuntimeData->CurrentState)));
		GEngine->AddOnScreenDebugMessage(2, 0.f, FColor::Yellow,
			FString::Printf(TEXT("Speed:%.1f Move:%d Angle:%.0f"),
				RuntimeData->CurrentSpeed, RuntimeData->bIsMoving,
				RuntimeData->MoveAngle));
		FColor SpeedColor = RuntimeData->AnimSpeed > 0.f ? FColor::Green : FColor::Red;
		FVector ActorScale = GetActorScale3D();
		GEngine->AddOnScreenDebugMessage(3, 0.f, SpeedColor,
			FString::Printf(TEXT("AnimSpd:%.0f | Actual:%.0f | AScale:%.1f"),
				RuntimeData->AnimSpeed, RuntimeData->CurrentSpeed, ActorScale.X));
		FColor RMColor = RuntimeData->bBip001Found ? FColor::Green : FColor::Red;
		GEngine->AddOnScreenDebugMessage(4, 0.f, RMColor,
			FString::Printf(TEXT("Bip001:%s | Grounded:%s"),
				RuntimeData->bBip001Found ? TEXT("OK") : TEXT("MISSING"),
				RuntimeData->bIsGrounded ? TEXT("Y") : TEXT("N")));
	}
#endif
}

void ABaseCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
}
