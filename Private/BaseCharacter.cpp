/**
 * @file BaseCharacter.cpp
 * @brief 所有角色的基类实现 - 管线时序分发 + GAS 初始化
 */
#include "BaseCharacter.h"
#include "Components/CapsuleComponent.h"
#include "GGYGOGameplayEffects.h" // Phase 7: GE 全局初始化
#include "Animation/GGYGOAnimInstance.h"

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

	// ★ 阶段6：创建并行状态管理器（纯 C++，和其他管线风格一致）
	StateManager = MakeUnique<FGYGOStateManager>();

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
	// GE 全局引用初始化（Phase 7：必须在任何 GE 应用之前）
	// ============================================================
	InitGEGlobals();

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
		ArbiterPipeline->Init(ASC, StateManager.Get());
	}

	// ============================================================
	// 管线子系统初始化
	// ============================================================

	// 初始化意图管线（注入 ACharacter 和 Mesh 依赖）
	IntentPipeline->Init(this, GetMesh());

	// 初始化运动驱动器（缓存组件引用 + 关闭自动 Root Motion）
	MotionDriver->Init(this);

	// ★ 阶段6：初始化状态管理器（注册状态 + 加载关系矩阵 + 激活 Idle）
	// DT 可后续通过 CharacterConfig 引入，当前使用内置默认矩阵
	if (StateManager)
	{
		StateManager->Init(*RuntimeData);

		// ★ 阶段7：注入 ASC（GAS 联动，必须在 Init 之后）
		StateManager->InitASC(ASC);
	}

	// ★ 阶段8：从 CharacterConfig 同步步态速度阈值到 RuntimeData
	// MotionDriver 读取这些阈值来管理速度上限
	if (CharacterConfig)
	{
		RuntimeData->GaitThresholds.Walk   = CharacterConfig->MovementConfig.WalkSpeed;
		RuntimeData->GaitThresholds.Run    = CharacterConfig->MovementConfig.RunSpeed;
		RuntimeData->GaitThresholds.Sprint = CharacterConfig->MovementConfig.SprintSpeed;
	}
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
	// ★ 阶段6：使用新的并行状态管理器（纯 C++）
	if (StateManager)
	{
		StateManager->Update(DeltaTime);
	}

	// 6. 运动驱动：RuntimeData 移动数据 → 实际位移
	MotionDriver->Process(DeltaTime, *RuntimeData);

	// 帧末清零
	RuntimeData->ResetFrameIntents();

#if !UE_BUILD_SHIPPING
	// ============================================================
	// 调试 UI（左上角屏幕叠层，每帧刷新）
	//
	// 使用 AddOnScreenDebugMessage 按 Key 分组，Key 0~4 各占一行
	// Key 参数说明：
	//   - 第一个参数 = Key（同 Key 后写覆盖，不同 Key 各占一行）
	//   - 第二个参数 = 显示时长（0 = 持续到下次同 Key 写入覆盖）
	//   - 第三个参数 = 文字颜色
	//   - 第四个参数 = 文本内容
	//
	// 显示顺序（屏幕从上到下）：
	//   Key0 绿色  — 状态机当前主状态
	//   Key1 白色  — 输入/移动方向
	//   Key2 黄色  — 速度信息
	//   Key3 红色  — 仲裁标记
	//   Key4 青色  — 当前动画名 + 步态（动画模块专属）
	// ============================================================
	if (GEngine && StateManager)
	{
		// ------------------------------------------------------------
		// Key0: 当前主状态名称 + 活跃状态数量
		//
		// StateName: 从 RuntimeData->CurrentState 取枚举名
		//   字符串处理：去掉 "ECharacterStateType::" 前缀，只保留简短名
		//   例如 "ECharacterStateType::Idle" → "Idle"
		//
		// Active 数量: StateManager 中当前活跃的状态数
		//   状态管理器是并行状态机，可同时有多个状态活跃
		//   例如 Idle + InAir 可能同时活跃
		// ------------------------------------------------------------
		FString StateName = UEnum::GetValueAsString(RuntimeData->CurrentState);
		StateName.ReplaceInline(TEXT("ECharacterStateType::"), TEXT(""));
		GEngine->AddOnScreenDebugMessage(0, 0.f, FColor::Green,
			FString::Printf(TEXT("State:%s | Active:%d"),
				*StateName, StateManager->GetActiveStates().Num()));

		// ------------------------------------------------------------
		// Key1: 原始输入方向 + 期望移动方向
		//
		// Input: 输入管线处理后的本帧摇杆输入（2D 向量，X=右 Y=前）
		//   范围 [-1,1]，(0,0) 表示无输入
		//
		// MoveDir: 意图管线计算出的世界空间期望移动方向（3D 向量）
		//   由 Input 结合 ControlRotation 旋转到世界空间得到
		//   Z 分量已清零（水平移动）
		// ------------------------------------------------------------
		GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::White,
			FString::Printf(TEXT("Input:%s | MoveDir:%s"),
				*InputData->CurrentFrame.Move.ToString(),
				*RuntimeData->DesiredWorldMoveDir.ToString()));

		// ------------------------------------------------------------
		// Key2: 速度信息
		//
		// Speed:    当前水平移动速度（cm/s），MotionDriver 实际驱动
		// AnimSpd:  动画驱动速度（从 Bip001 骨骼位移提取，cm/s）
		//           用于防滑步——理想情况下应与 Speed 接近
		// Moving:   意图层是否在移动（摇杆是否推开）
		//           注意：与物理速度>0 不同，松手瞬间 Moving=false 但 Speed 可能还有惯性
		// ------------------------------------------------------------
		GEngine->AddOnScreenDebugMessage(2, 0.f, FColor::Yellow,
			FString::Printf(TEXT("Speed:%.1f | AnimSpd:%.0f | Moving:%d"),
				RuntimeData->CurrentSpeed, RuntimeData->AnimSpeed,
				RuntimeData->bIsMoving));

		// ------------------------------------------------------------
		// Key3: 仲裁标记（GAS Tag 驱动的状态屏蔽）
		//
		// 用于排查"为什么状态不切换/角色不动"的问题
		// 标记为 true 表示对应行为被仲裁管线屏蔽
		//
		// BlockMove:   屏蔽移动（如攻击中、受击硬直）
		// BlockAtk:    屏蔽攻击（如冷却中）
		// BlockDodge:  屏蔽闪避（如冷却中）
		// BlockInput:  屏蔽所有输入（如过场动画中）
		// ------------------------------------------------------------
		GEngine->AddOnScreenDebugMessage(3, 0.f, FColor::Red,
			FString::Printf(TEXT("BlockMove:%d BlockAtk:%d BlockDodge:%d BlockInput:%d"),
				RuntimeData->bBlockMove ? 1 : 0,
				RuntimeData->bBlockAttack ? 1 : 0,
				RuntimeData->bBlockDodge ? 1 : 0,
				RuntimeData->bBlockInput ? 1 : 0));

		// ------------------------------------------------------------
		// Key4: 当前支撑脚 + 意图层步态（动画模块专属调试）
		//
		// 获取流程：
		//   1. GetMesh() → USkeletalMeshComponent
		//   2. GetAnimInstance() → UAnimInstance（期望是 UGGYGOAnimInstance）
		//   3. Cast<UGGYGOAnimInstance> → 安全转换
		//   4. 读 Out_DebugFoot → 当前支撑脚调试字符串（"L"/"R"）
		//
		// Foot: 当前支撑脚，由循环相位查询得出
		//   如果显示 "?" 表示 Cast 失败（AnimBP 父类配错）
		//
		// Gait: 意图层步态（RuntimeData->ResolvedGait）
		//   Walk / Run / Sprint / None
		//   注意：这是意图层每帧重新解析的步态
		// ------------------------------------------------------------
		FString AnimName = TEXT("?");
		if (GetMesh())
		{
			UGGYGOAnimInstance* AI = Cast<UGGYGOAnimInstance>(GetMesh()->GetAnimInstance());
			if (AI) AnimName = AI->Out_DebugFoot.ToString();
		}
		GEngine->AddOnScreenDebugMessage(4, 0.f, FColor::Cyan,
			FString::Printf(TEXT("Foot:%s | Gait:%s"),
				*AnimName,
				RuntimeData->ResolvedGait == EMovementGait::Walk ? TEXT("Walk") :
				RuntimeData->ResolvedGait == EMovementGait::Run  ? TEXT("Run")  :
				RuntimeData->ResolvedGait == EMovementGait::Sprint ? TEXT("Sprint") : TEXT("None")));
	}
#endif
}

void ABaseCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
}
