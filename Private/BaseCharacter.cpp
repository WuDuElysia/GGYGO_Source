/**
 * @file BaseCharacter.cpp
 * @brief 所有角色的基类实现 - 管线时序分发 + GAS 初始化
 */
#include "BaseCharacter.h"
#include "Components/GGYGOCharacterRuntimeComponent.h"
#include "GGYGOGameplayEffects.h" // Phase 7: GE 全局初始化

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

	// 单一运行时宿主：内部拥有纯 C++ 数据/管线/状态/驱动，但自身不 Tick。
	RuntimeComponent = CreateDefaultSubobject<UGGYGOCharacterRuntimeComponent>(TEXT("RuntimeComponent"));
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
	// GAS 初始化（必须在运行时宿主初始化之前）
	// ============================================================
	if (ASC)
	{
		// InitAbilityActorInfo 告诉 ASC "我属于谁"
		// 参数1 OwnerActor  — Ability 的"拥有者"（通常是 Controller 或 PlayerState）
		// 参数2 AvatarActor — Ability 的"物理表现"（通常是 Character 自己）
		// 单人游戏简单处理：两个参数都传 this
		ASC->InitAbilityActorInfo(this, this);
	}

	// 组件内部继续按原顺序初始化 Arbiter、Intent、Motion 和 StateManager。
	if (RuntimeComponent)
	{
		RuntimeComponent->InitializeRuntime(this, ASC, GetMesh());
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

	// 唯一运行时调度入口。完整阶段顺序由组件的 ProcessFrame 显式维护。
	if (RuntimeComponent)
	{
		RuntimeComponent->ProcessFrame(DeltaTime);
	}
}

void ABaseCharacter::SetMoveInput(const FVector2D& Value)
{
	if (RuntimeComponent)
	{
		RuntimeComponent->SetMoveInput(Value);
	}
}

void ABaseCharacter::ClearMoveInput()
{
	if (RuntimeComponent)
	{
		RuntimeComponent->ClearMoveInput();
	}
}

void ABaseCharacter::SetLookInput(const FVector2D& Value)
{
	if (RuntimeComponent)
	{
		RuntimeComponent->SetLookInput(Value);
	}
}

void ABaseCharacter::SetSprintHeld(bool bHeld)
{
	if (RuntimeComponent)
	{
		RuntimeComponent->SetSprintHeld(bHeld);
	}
}

void ABaseCharacter::SetForceWalkHeld(bool bHeld)
{
	if (RuntimeComponent)
	{
		RuntimeComponent->SetForceWalkHeld(bHeld);
	}
}

void ABaseCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
}
