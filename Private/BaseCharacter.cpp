/**
 * @file BaseCharacter.cpp
 * @brief 所有角色的基类实现 - 管线时序分发 + GAS 初始化
 */
#include "BaseCharacter.h"
#include "Components/CapsuleComponent.h"
#include "GGYGOGameplayEffects.h" // Phase 7: GE 全局初始化
#include "Data/Anim/AnimRuntimeData.h"
#include "Animation/NTEAnimInstance.h"
#include "Animation/zzzAnim/ZZZAnimInstance.h"

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

	// RuntimeData 的运动配置由 MotionDriver 初始化；
	// BaseCharacter 只负责系统组装和时序调度。
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
	if (StateManager) StateManager->Update(DeltaTime);

	// 6. 运动驱动：RuntimeData 移动数据 → 实际位移
	MotionDriver->Process(DeltaTime, *RuntimeData);

	// 7. 动画驱动：状态机、运动驱动和意图处理器已分别同步 AnimData，
	//    BaseCharacter 只在所有生产者完成后通知 AnimInstance 抓取快照。
	//    保证 AnimBP 读到的快照一定是本帧管线刚写完的最新值
	if (UNTEAnimInstance* AI = Cast<UNTEAnimInstance>(GetMesh()->GetAnimInstance())) AI->PipelineDrive();
	if (UZZZAnimInstance* ZAI = Cast<UZZZAnimInstance>(GetMesh()->GetAnimInstance())) ZAI->PipelineDrive();

	// 帧末清零
	RuntimeData->ResetFrameIntents();


}

void ABaseCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
}
