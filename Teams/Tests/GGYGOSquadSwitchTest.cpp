#include "Teams/Tests/GGYGOSquadSwitchTestTypes.h"

#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Character/Data/GGYGOPawnData.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "Input/GGYGOInputComponent.h"
#include "Input/Tests/GGYGOInputTestTypes.h"
#include "NativeGameplayTags.h"
#include "Player/GGYGOPlayerState.h"
#include "System/GGYGOGameplayTags.h"
#include "Teams/GGYGOCharacterSlot.h"
#include "Teams/GGYGOSquadComponent.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOSquadSwitchTestTypes)

UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_GGYGO_Tests_SquadSwitch_Cooldown, "Cooldown.GGYGO.Tests.SquadSwitch");

AGGYGOSquadSwitchTestPawn::AGGYGOSquadSwitchTestPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Hero = CreateDefaultSubobject<UGGYGOInputTestHeroComponent>(TEXT("Hero"));
}

UGGYGOInputComponent* AGGYGOSquadSwitchTestPawn::GetInputComponentForTest() const
{
	return Cast<UGGYGOInputComponent>(InputComponent);
}

UInputComponent* AGGYGOSquadSwitchTestPawn::CreatePlayerInputComponent()
{
	return NewObject<UGGYGOInputComponent>(this, NAME_None, RF_Transient);
}

void AGGYGOSquadSwitchTestPawn::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	++PossessCalls;
}

void AGGYGOSquadSwitchTestPawn::UnPossessed()
{
	Super::UnPossessed();
	++UnpossessCalls;
}

void AGGYGOSquadSwitchTestPawn::ObserveActiveCharacterForTest(AGGYGOCharacterBase* NewCharacter)
{
	++ActiveCharacterNotices;
	LastActiveCharacter = NewCharacter;
}

void AGGYGOSquadSwitchTestPawn::ResetControlObservationsForTest()
{
	PossessCalls = UnpossessCalls = ActiveCharacterNotices = 0;
	LastActiveCharacter.Reset();
}

UGGYGOSquadSwitchTestCooldown::UGGYGOSquadSwitchTestCooldown(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FScalableFloat(60.0f);
	// Native GE constructors must use named default subobjects. AddComponent's
	// unnamed NewObject is for later dynamic configuration, not CDO construction.
	UTargetTagsGameplayEffectComponent* TargetTags = ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(
		this, TEXT("CooldownTargetTags"));
	GEComponents.Add(TargetTags);
	FInheritedTagContainer Tags;
	Tags.Added.AddTag(TAG_GGYGO_Tests_SquadSwitch_Cooldown);
	TargetTags->SetAndApplyTargetTagChanges(Tags);
}

UGGYGOSquadSwitchTestAbility::UGGYGOSquadSwitchTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	ActivationPolicy = EGGYGOAbilityActivationPolicy::WhileInputActive;
	GroupTag = FGameplayTag();
	SelfPolicy = EGGYGOAbilitySelfPolicy::Exclusive;
	CooldownGameplayEffectClass = UGGYGOSquadSwitchTestCooldown::StaticClass();
}

bool UGGYGOSquadSwitchTestAbility::EndForTest()
{
	if (!IsActive()) { return false; }
	SetCanBeCanceled(true);
	return RequestAbilityEnd(CaptureCurrentActivation(), false, false).IsAccepted();
}

void UGGYGOSquadSwitchTestAbility::InputReleased(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo)
{
	++InputReleasedCalls;
	Super::InputReleased(Handle, ActorInfo, ActivationInfo);
}

void UGGYGOSquadSwitchTestAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// Intentionally no Blueprint business body: this fixture holds the real native activation.
	++ActivationCalls;
	ActivatedOriginal = Original;
}

void UGGYGOSquadSwitchTestAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	++CleanupCalls;
	CleanedOriginal = Context.GetOriginalActivation();
	FSimpleDelegate OriginalHook = MoveTemp(CleanupHook);
	CleanupHook.Unbind();
	OriginalHook.ExecuteIfBound();
	Super::CleanupAbilityResourcesForTermination(Context);
}

void UGGYGOSquadSwitchTestAbility::NativeOnOwnerControlLost()
{
	++ControlLostCalls;
	const FGameplayAbilitySpec* Spec = GetCurrentAbilitySpec();
	bInputPressedAtControlLost = Spec && Spec->InputPressed;
	InputReleasedBeforeControlLost = InputReleasedCalls;
}

void UGGYGOSquadSwitchTestAbility::NativeOnOwnerControlRegained()
{
	++ControlRegainedCalls;
}

UGGYGOSquadSwitchContinueTestAbility::UGGYGOSquadSwitchContinueTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AvatarExitPolicy = EGGYGOAbilityAvatarExitPolicy::ContinueInBackground;
}

UGGYGOSquadSwitchTriggeredTestAbility::UGGYGOSquadSwitchTriggeredTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ActivationPolicy = EGGYGOAbilityActivationPolicy::OnInputTriggered;
}

UGGYGOSquadSwitchExitTestAbility::UGGYGOSquadSwitchExitTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FAbilityTriggerData& Trigger = AbilityTriggers.AddDefaulted_GetRef();
	Trigger.TriggerTag = GGYGOGameplayTags::Event_Squad_SwitchOut;
	Trigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
}

UGGYGOSquadSwitchEnterTestAbility::UGGYGOSquadSwitchEnterTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FAbilityTriggerData& Trigger = AbilityTriggers.AddDefaulted_GetRef();
	Trigger.TriggerTag = GGYGOGameplayTags::Event_Squad_SwitchIn;
	Trigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
}

UGGYGOSquadSwitchTestMaxHealthDrop::UGGYGOSquadSwitchTestMaxHealthDrop(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FScalableFloat(60.0f);
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UGGYGOHealthSet::GetMaxHealthAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;
	Modifier.ModifierMagnitude = FScalableFloat(-20.0f);
}

void UGGYGOSquadSwitchHealthObserver::Observe(UGGYGOHealthComponent* InHealth)
{
	Release();
	Observed = InHealth;
	if (!InHealth) { return; }
	InHealth->OnHealthChanged.AddDynamic(this, &ThisClass::HandleHealthChanged);
	InHealth->OnMaxHealthChanged.AddDynamic(this, &ThisClass::HandleMaxHealthChanged);
	InHealth->OnPoiseChanged.AddDynamic(this, &ThisClass::HandlePoiseChanged);
	InHealth->OnPoiseBroken.AddDynamic(this, &ThisClass::HandlePoiseBroken);
}

void UGGYGOSquadSwitchHealthObserver::Release()
{
	if (UGGYGOHealthComponent* Health = Observed.Get())
	{
		Health->OnHealthChanged.RemoveDynamic(this, &ThisClass::HandleHealthChanged);
		Health->OnMaxHealthChanged.RemoveDynamic(this, &ThisClass::HandleMaxHealthChanged);
		Health->OnPoiseChanged.RemoveDynamic(this, &ThisClass::HandlePoiseChanged);
		Health->OnPoiseBroken.RemoveDynamic(this, &ThisClass::HandlePoiseBroken);
	}
	Observed.Reset();
}

void UGGYGOSquadSwitchHealthObserver::HandleHealthChanged(UGGYGOHealthComponent*, float, float NewValue, AActor*)
{
	++HealthNotices;
	LastHealth = NewValue;
}

void UGGYGOSquadSwitchHealthObserver::HandleMaxHealthChanged(UGGYGOHealthComponent*, float, float NewValue, AActor*)
{
	++MaxHealthNotices;
	LastMaxHealth = NewValue;
}

void UGGYGOSquadSwitchHealthObserver::HandlePoiseChanged(UGGYGOHealthComponent*, float, float NewValue, AActor*)
{
	++PoiseNotices;
	LastPoise = NewValue;
}

void UGGYGOSquadSwitchHealthObserver::HandlePoiseBroken(AActor*)
{
	++PoiseBrokenNotices;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace GGYGOSquadSwitchTests
{
	struct FPresentationSnapshot
	{
		bool bHidden;
		bool bCollisionEnabled;
		EMovementMode MovementMode;
		uint8 CustomMovementMode;

		explicit FPresentationSnapshot(const AGGYGOSquadSwitchTestPawn* Pawn)
			: bHidden(Pawn->IsHidden()), bCollisionEnabled(Pawn->GetActorEnableCollision()),
			MovementMode(Pawn->GetCharacterMovement()->MovementMode),
			CustomMovementMode(Pawn->GetCharacterMovement()->CustomMovementMode)
		{
		}

		bool HasSamePresentation(const FPresentationSnapshot& Other) const
		{
			return bHidden == Other.bHidden && bCollisionEnabled == Other.bCollisionEnabled
				&& MovementMode == Other.MovementMode && CustomMovementMode == Other.CustomMovementMode;
		}

		FString Describe() const
		{
			return FString::Printf(TEXT("Hidden=%d Collision=%d MovementMode=%u CustomMovementMode=%u"),
				bHidden, bCollisionEnabled, static_cast<uint8>(MovementMode), CustomMovementMode);
		}
	};

	struct FFixture
	{
		static constexpr int32 MaxMembers = 3;
		// Reuse only the existing real private GI/LocalPlayer/PC/EnhancedInput birth.
		// The squad below uses real project Slot, Character, Host publication and Possess.
		FGGYGOInputTestFixture Input;
		AGGYGOPlayerState* PlayerState = nullptr;
		UGGYGOSquadComponent* Squad = nullptr;
		int32 Count = 0;
		AGGYGOSquadSwitchTestPawn* Pawns[MaxMembers] = {};
		AGGYGOCharacterSlot* Slots[MaxMembers] = {};
		UGGYGOAbilitySystemComponent* ASCs[MaxMembers] = {};
		TStrongObjectPtr<UGGYGOPawnData> Data;
		TStrongObjectPtr<UGGYGOMovementSet> Movement;
		TStrongObjectPtr<UGameplayEffect> BuffDefinition;
		TArray<TWeakObjectPtr<UGGYGOSquadSwitchTestAbility>> Abilities;
		/** 原生 OnAbilityEnded 观察：每次真实结束计一次，并记录结束的实例与是否取消。 */
		FDelegateHandle CompletionObserver;
		int32 CompletionCalls = 0;
		TWeakObjectPtr<UGameplayAbility> LastEndedAbility;
		bool bLastEndedCancelled = false;
		FActiveGameplayEffectHandle Buff;
		FActiveGameplayEffectHandle Cooldown;

		~FFixture()
		{
			if (IsValid(ASCs[0])) { ASCs[0]->OnAbilityEnded.Remove(CompletionObserver); }
			if (IsValid(Squad) && IsValid(Pawns[0]))
			{
				Squad->OnActiveCharacterChanged.RemoveDynamic(Pawns[0], &AGGYGOSquadSwitchTestPawn::ObserveActiveCharacterForTest);
				// 先终止编队：之后结束能力不再推进退场。借用的夹具 Actor 仍由夹具负责。
				Squad->DestroySquad();
			}
			for (const auto& WeakAbility : Abilities)
			{
				if (UGGYGOSquadSwitchTestAbility* Ability = WeakAbility.Get())
				{
					Ability->DisarmCleanupForTest();
					Ability->EndForTest();
				}
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (IsValid(Pawns[Index]))
				{
					Pawns[Index]->GetHeroForTest()->ReleasePlayerInput();
					Pawns[Index]->GetHeroForTest()->StopObservingLocalAbilitySystemForTest();
				}
				if (IsValid(Slots[Index])) { Slots[Index]->DetachAvatar(Pawns[Index]); Slots[Index]->Destroy(); }
				if (IsValid(Pawns[Index])) { Pawns[Index]->Destroy(); }
			}
			if (IsValid(PlayerState))
			{
				if (Input.GetController() && Input.GetController()->GetPlayerState<APlayerState>() == PlayerState)
				{
					Input.GetController()->SetPlayerState(nullptr);
				}
				PlayerState->Destroy();
			}
		}

		bool Initialize(FAutomationTestBase& Test, int32 InCount = 2)
		{
			check(InCount >= 2 && InCount <= MaxMembers);
			if (!Input.Initialize(Test)) { return false; }
			APlayerController* PC = Input.GetController();
			Input.GetPawn()->GetHeroForTest()->ReleasePlayerInput();
			PC->UnPossess();
			UWorld* World = Input.GetWorld();
			FActorSpawnParameters Spawn;
			Spawn.Owner = PC;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			PlayerState = World->SpawnActor<AGGYGOPlayerState>(Spawn);
			if (!Test.TestNotNull(TEXT("Real project PlayerState"), PlayerState)) { return false; }
			PC->SetPlayerState(PlayerState);
			Squad = PlayerState->GetSquadComponent();
			Data.Reset(NewObject<UGGYGOPawnData>(PlayerState));
			Movement.Reset(NewObject<UGGYGOMovementSet>(PlayerState));
			Movement->bUseCurveDrivenSpeed = false; // Explicit normal fixed mode, no missing-curve fallback.
			Data->MovementSet = Movement.Get();
			Data->PawnClass = AGGYGOSquadSwitchTestPawn::StaticClass();
			Data->InputConfig = Input.GetPawn()->GetPawnExtensionForTest()->GetPawnData<UGGYGOPawnData>()->InputConfig;
			Count = InCount;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Slots[Index] = World->SpawnActor<AGGYGOCharacterSlot>(Spawn);
				Pawns[Index] = World->SpawnActor<AGGYGOSquadSwitchTestPawn>(
					FVector(Index * 300.0f, 0.0f, 100.0f), FRotator::ZeroRotator, Spawn);
				if (!Test.TestNotNull(TEXT("Real Slot"), Slots[Index]) || !Test.TestNotNull(TEXT("Real Character"), Pawns[Index])) { return false; }
				Slots[Index]->InitializeForPawnData(Data.Get());
				Pawns[Index]->GetPawnExtensionComponent()->SetPawnData(Data.Get());
				FString Error;
				if (!Test.TestTrue(TEXT("Hero subscribes to real Extension publication"),
					Pawns[Index]->GetHeroForTest()->ObserveLocalAbilitySystemForTest(Pawns[Index]->GetPawnExtensionComponent(), Error)))
				{
					Test.AddError(Error); return false;
				}
				Pawns[Index]->GetHeroForTest()->ConfigureMappingForTest(Input.GetMappingContext(), 0);
				Slots[Index]->AttachAvatar(Pawns[Index]);
				ASCs[Index] = Slots[Index]->GetGGYGOAbilitySystemComponent();
				if (!Test.TestTrue(TEXT("Host established actual Ready Slot/ASC/Pawn connection"),
					ASCs[Index]->GetOwnerActor() == Slots[Index] && ASCs[Index]->GetAvatarActor() == Pawns[Index]
					&& Pawns[Index]->GetGGYGOAbilitySystemComponent() == ASCs[Index]
					&& Slots[Index]->GetAvatarPawn() == Pawns[Index])) { return false; }
				if (!Test.TestTrue(TEXT("Production RegisterSlot accepts original binding"), Squad->RegisterSlot(Slots[Index]))) { return false; }
			}
			// An unbegun synthetic world may not deliver ClientRestart RPC. Invoke only
			// native PawnClientRestart if its original native input component is absent.
			if (!Pawns[0]->GetInputComponentForTest()) { Pawns[0]->PawnClientRestart(); }
			if (!Test.TestTrue(TEXT("First member actually possessed both ways"),
				Squad->GetActiveSlot() == Slots[0] && PC->GetPawn() == Pawns[0] && Pawns[0]->GetController() == PC)
				|| !Test.TestNotNull(TEXT("Native first Pawn input component"), Pawns[0]->GetInputComponentForTest())) { return false; }
			Squad->OnActiveCharacterChanged.AddDynamic(Pawns[0], &AGGYGOSquadSwitchTestPawn::ObserveActiveCharacterForTest);
			for (int32 Index = 0; Index < Count; ++Index) { Pawns[Index]->ResetControlObservationsForTest(); }
			CompletionObserver = ASCs[0]->OnAbilityEnded.AddLambda([this](const FAbilityEndedData& Ended)
			{
				++CompletionCalls;
				LastEndedAbility = Ended.AbilityThatEnded;
				bLastEndedCancelled = Ended.bWasCancelled;
			});
			return true;
		}

		UGGYGOSquadSwitchTestAbility* Grant(FAutomationTestBase& Test, int32 SlotIndex,
			TSubclassOf<UGGYGOSquadSwitchTestAbility> Class, bool bInputBound, FGameplayAbilitySpecHandle& OutHandle)
		{
			FGameplayAbilitySpec Spec(Class, 1);
			if (bInputBound) { Spec.GetDynamicSpecSourceTags().AddTag(GGYGOGameplayTags::InputTag_Attack_Light); }
			OutHandle = ASCs[SlotIndex]->GiveAbility(Spec);
			FGameplayAbilitySpec* Granted = ASCs[SlotIndex]->FindAbilitySpecFromHandle(OutHandle);
			UGGYGOSquadSwitchTestAbility* Ability = Granted ? Cast<UGGYGOSquadSwitchTestAbility>(Granted->GetPrimaryInstance()) : nullptr;
			Test.TestNotNull(TEXT("Native per-actor ability instance"), Ability);
			if (Ability) { Abilities.Add(Ability); }
			return Ability;
		}

		TArray<TUniquePtr<FEnhancedInputActionEventBinding>> CapturePressBindings(int32 SlotIndex = 0) const
		{
			TArray<TUniquePtr<FEnhancedInputActionEventBinding>> Result;
			if (const UGGYGOInputComponent* Component = Pawns[SlotIndex]->GetInputComponentForTest())
			{
				for (const auto& Binding : Component->GetActionEventBindings())
				{
					if (Binding->GetAction() == Input.GetAbilityAction() && Binding->GetTriggerEvent() == ETriggerEvent::Triggered)
					{
						Result.Add(Binding->Clone());
					}
				}
			}
			return Result;
		}

		/** 经指定队员当前的真实 Hero 绑定按下一次，并跑一帧控制器输入消费。 */
		bool Press(FAutomationTestBase& Test, int32 SlotIndex)
		{
			auto Bindings = CapturePressBindings(SlotIndex);
			if (!Test.TestEqual(TEXT("One real Hero ability binding"), Bindings.Num(), 1)) { return false; }
			const FInputActionInstance Action(Input.GetAbilityAction());
			for (const auto& Binding : Bindings) { Binding->Execute(Action); }
			Input.GetController()->ConsumeInputForTest();
			return true;
		}

		bool PressAndActivate(FAutomationTestBase& Test, UGGYGOSquadSwitchTestAbility* Ability)
		{
			if (!Press(Test, 0)) { return false; }
			const FGameplayAbilitySpec* ActivatedSpec = Ability
				? ASCs[0]->FindAbilitySpecFromHandle(Ability->GetCurrentAbilitySpecHandle()) : nullptr;
			return Test.TestTrue(TEXT("Real held input activated the native controlled GA"),
				Ability && Ability->IsActive() && Ability->ActivatedOriginal.HasActivation() && Ability->ActivationCalls == 1
				&& ActivatedSpec && ActivatedSpec->InputPressed);
		}

		bool ApplyPersistentEffects(FAutomationTestBase& Test)
		{
			BuffDefinition.Reset(NewObject<UGameplayEffect>(PlayerState));
			BuffDefinition->DurationPolicy = EGameplayEffectDurationType::HasDuration;
			BuffDefinition->DurationMagnitude = FScalableFloat(60.0f);
			FGameplayModifierInfo& Modifier = BuffDefinition->Modifiers.AddDefaulted_GetRef();
			Modifier.Attribute = UGGYGOHealthSet::GetMaxHealthAttribute();
			Modifier.ModifierOp = EGameplayModOp::Additive;
			Modifier.ModifierMagnitude = FScalableFloat(25.0f);
			Buff = ASCs[0]->ApplyGameplayEffectSpecToSelf(FGameplayEffectSpec(BuffDefinition.Get(), ASCs[0]->MakeEffectContext(), 1.0f));
			const UGameplayEffect* Definition = GetDefault<UGGYGOSquadSwitchTestCooldown>();
			Cooldown = ASCs[0]->ApplyGameplayEffectSpecToSelf(FGameplayEffectSpec(Definition, ASCs[0]->MakeEffectContext(), 1.0f));
			return Test.TestTrue(TEXT("Real duration buff and cooldown installed"), Buff.IsValid() && Cooldown.IsValid()
				&& ASCs[0]->GetActiveGameplayEffect(Buff) && ASCs[0]->GetActiveGameplayEffect(Cooldown)
				&& ASCs[0]->HasMatchingGameplayTag(TAG_GGYGO_Tests_SquadSwitch_Cooldown));
		}

		/** Source 队员的 ASC 对 Target 队员施加一次即时元属性 GE，与命中结算同一 HealthSet 路径。 */
		void ApplyMeta(int32 SourceIndex, int32 TargetIndex, const FGameplayAttribute& Attribute, float Magnitude)
		{
			TStrongObjectPtr<UGameplayEffect> Damage(NewObject<UGameplayEffect>(PlayerState));
			Damage->DurationPolicy = EGameplayEffectDurationType::Instant;
			FGameplayModifierInfo& Modifier = Damage->Modifiers.AddDefaulted_GetRef();
			Modifier.Attribute = Attribute;
			Modifier.ModifierOp = EGameplayModOp::Additive;
			Modifier.ModifierMagnitude = FScalableFloat(Magnitude);
			FGameplayEffectContextHandle Context = ASCs[SourceIndex]->MakeEffectContext();
			Context.AddInstigator(Pawns[SourceIndex], Pawns[SourceIndex]);
			const FGameplayEffectSpec Spec(Damage.Get(), Context, 1.0f);
			ASCs[SourceIndex]->ApplyGameplayEffectSpecToTarget(Spec, ASCs[TargetIndex]);
		}

		float Health(int32 Index) const
		{
			return ASCs[Index]->GetNumericAttribute(UGGYGOHealthSet::GetHealthAttribute());
		}

		float Poise(int32 Index) const
		{
			return ASCs[Index]->GetNumericAttribute(UGGYGOHealthSet::GetPoiseAttribute());
		}

		FGGYGOPawnASCResourceHandle Resource(int32 Index) const
		{
			return Pawns[Index]->GetPawnExtensionComponent()->GetCurrentLocalAbilitySystemResource();
		}

		bool HasOffField(int32 Index) const
		{
			return ASCs[Index]->HasMatchingGameplayTag(GGYGOGameplayTags::State_OffField)
				&& ASCs[Index]->HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity);
		}

		bool HasNoSwitchTags(int32 Index) const
		{
			return !ASCs[Index]->HasMatchingGameplayTag(GGYGOGameplayTags::State_OffField)
				&& !ASCs[Index]->HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity)
				&& !ASCs[Index]->HasMatchingGameplayTag(GGYGOGameplayTags::State_SwitchReturnPending);
		}

		/** 留场：可见、有碰撞、移动未停用，无 Controller 时仍执行移动。 */
		bool IsOnFieldWithoutControl(int32 Index) const
		{
			const UCharacterMovementComponent* Move = Pawns[Index]->GetCharacterMovement();
			return !Pawns[Index]->IsHidden() && Pawns[Index]->GetActorEnableCollision()
				&& Move->MovementMode != MOVE_None && Move->bRunPhysicsWithNoController && !Pawns[Index]->GetController();
		}

		/** 待命：隐藏、无碰撞、移动停用，移动开关已恢复。 */
		bool IsBenched(int32 Index) const
		{
			const UCharacterMovementComponent* Move = Pawns[Index]->GetCharacterMovement();
			return Squad->GetMemberState(Index) == EGGYGOSquadMemberState::Benched
				&& Pawns[Index]->IsHidden() && !Pawns[Index]->GetActorEnableCollision()
				&& Move->MovementMode == MOVE_None && !Move->bRunPhysicsWithNoController && !Pawns[Index]->GetController()
				&& HasNoSwitchTags(Index);
		}

		bool IsControlled(int32 Index) const
		{
			APlayerController* PC = Input.GetController();
			return Squad->GetActiveSlot() == Slots[Index] && PC->GetPawn() == Pawns[Index]
				&& Pawns[Index]->GetController() == PC && Squad->GetMemberState(Index) == EGGYGOSquadMemberState::Active
				&& !Pawns[Index]->IsHidden() && Pawns[Index]->GetActorEnableCollision();
		}

		bool CheckCurrentBindings(FAutomationTestBase& Test, const TCHAR* Stage = TEXT("before switch")) const
		{
			bool bCurrent = true;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				bCurrent &= Test.TestTrue(FString::Printf(TEXT("Slot %d binding resource is ready %s"), Index, Stage),
					Pawns[Index]->GetPawnExtensionComponent()->IsLocalAbilitySystemResourceReady(Resource(Index)));
				bCurrent &= Test.TestTrue(FString::Printf(TEXT("Slot %d original Host/Avatar/Extension Ready is real %s"), Index, Stage),
					Squad->GetSlot(Index) == Slots[Index] && Slots[Index]->GetAvatarPawn() == Pawns[Index]
					&& Slots[Index]->GetGGYGOAbilitySystemComponent() == ASCs[Index]
					&& ASCs[Index]->GetOwnerActor() == Slots[Index] && ASCs[Index]->GetAvatarActor() == Pawns[Index]
					&& Pawns[Index]->GetPawnExtensionComponent()->GetGGYGOAbilitySystemComponent() == ASCs[Index]
					&& Pawns[Index]->GetGGYGOAbilitySystemComponent() == ASCs[Index]);
			}
			return bCurrent;
		}

		void CheckPreservedState(FAutomationTestBase& Test, float MaxHealth) const
		{
			Test.TestTrue(TEXT("Original Slot/ASC/Avatar objects survive the switch"),
				Squad->GetSlot(0) == Slots[0] && Squad->GetSlot(1) == Slots[1]
				&& Slots[0]->GetGGYGOAbilitySystemComponent() == ASCs[0] && Slots[1]->GetGGYGOAbilitySystemComponent() == ASCs[1]
				&& Slots[0]->GetAvatarPawn() == Pawns[0] && Slots[1]->GetAvatarPawn() == Pawns[1]);
			Test.TestTrue(TEXT("Original buff and tagged cooldown stay on the old Slot"),
				ASCs[0]->GetActiveGameplayEffect(Buff) && ASCs[0]->GetActiveGameplayEffect(Cooldown)
				&& ASCs[0]->HasMatchingGameplayTag(TAG_GGYGO_Tests_SquadSwitch_Cooldown));
			Test.TestEqual(TEXT("Buff attribute value unchanged"), ASCs[0]->GetNumericAttribute(UGGYGOHealthSet::GetMaxHealthAttribute()), MaxHealth);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOSquadSwitchCancelSmoke,
	"GGYGO.Teams.SquadSwitch.CancelAndPreserveState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOSquadSwitchCancelSmoke::RunTest(const FString& Parameters)
{
	GGYGOSquadSwitchTests::FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGameplayAbilitySpecHandle Handle, NewHandle;
	UGGYGOSquadSwitchTestAbility* Ability = Fixture.Grant(*this, 0, UGGYGOSquadSwitchTestAbility::StaticClass(), true, Handle);
	UGGYGOSquadSwitchTestAbility* NewAbility = Fixture.Grant(*this, 1, UGGYGOSquadSwitchTestAbility::StaticClass(), true, NewHandle);
	if (!Ability || !NewAbility || !Fixture.PressAndActivate(*this, Ability) || !Fixture.ApplyPersistentEffects(*this)) { return false; }
	const float MaxHealth = Fixture.ASCs[0]->GetNumericAttribute(UGGYGOHealthSet::GetMaxHealthAttribute());
	const FGGYGOAbilityActivationHandle Original = Ability->ActivatedOriginal;
	const FGGYGOPawnASCResourceHandle OldResource = Fixture.Resource(0);
	bool bNestedRejected = false;
	AddExpectedMessagePlain(TEXT("Reason=SwitchInProgress"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Ability->ArmCleanupForTest(FSimpleDelegate::CreateLambda([&]()
	{
		bNestedRejected = !Fixture.Squad->SwitchToSlot(1);
		TestTrue(TEXT("Cancellation callback still sees the original control/index"), Fixture.Squad->GetActiveSlot() == Fixture.Slots[0]
			&& Fixture.Input.GetController()->GetPawn() == Fixture.Pawns[0]);
	}));
	if (!Fixture.CheckCurrentBindings(*this)) { return false; }
	if (!TestTrue(TEXT("Production double-Slot switch succeeds"), Fixture.Squad->SwitchToSlot(1))) { return false; }
	TestTrue(TEXT("Same synchronous request rejects cancellation reentry"), bNestedRejected);
	TestTrue(TEXT("Default Cancel decision really ended the GA once"), !Ability->IsActive() && Ability->CleanupCalls == 1
		&& Ability->CleanedOriginal.HasSameActivation(Original) && Fixture.CompletionCalls == 1
		&& Fixture.LastEndedAbility.Get() == Ability && Fixture.bLastEndedCancelled);
	TestTrue(TEXT("Committed index matches actual reciprocal native Possess"), Fixture.IsControlled(1) && !Fixture.Pawns[0]->GetController());
	TestTrue(TEXT("Without continuing or exit abilities the old member is benched immediately"), Fixture.IsBenched(0));
	TestTrue(TEXT("Native control changed Pawn Owner without changing persistent Slot ownership"),
		Fixture.Pawns[0]->GetOwner() == nullptr && Fixture.Pawns[1]->GetOwner() == Fixture.Input.GetController()
		&& Fixture.Slots[0]->GetOwner() == Fixture.Input.GetController() && Fixture.Slots[1]->GetOwner() == Fixture.Input.GetController());
	TestTrue(TEXT("Native UnPossess refreshed ActorInfo without replacing the original binding"),
		Fixture.Resource(0).HasSameResource(OldResource));
	Fixture.CheckCurrentBindings(*this, TEXT("after native control transfer"));
	TestEqual(TEXT("One old UnPossess"), Fixture.Pawns[0]->UnpossessCalls, 1);
	TestEqual(TEXT("One new Possess"), Fixture.Pawns[1]->PossessCalls, 1);
	TestEqual(TEXT("One committed active-character notification"), Fixture.Pawns[0]->ActiveCharacterNotices, 1);
	TestEqual(TEXT("Notification reports actual new Pawn"), Fixture.Pawns[0]->LastActiveCharacter.Get(), static_cast<AGGYGOCharacterBase*>(Fixture.Pawns[1]));
	TestFalse(TEXT("ASC retired old native InputPressed"), Fixture.ASCs[0]->FindAbilitySpecFromHandle(Handle)->InputPressed);
	Fixture.ASCs[0]->ProcessAbilityInput(0.0f, false);
	TestTrue(TEXT("Old held input cannot reactivate cancelled GA"), !Ability->IsActive() && Ability->ActivationCalls == 1);
	Fixture.CheckPreservedState(*this, MaxHealth);
	// Deliver the native local restart explicitly in this unbegun private world;
	// this checks genuine Setup/Hero binding, not automatic network RPC delivery.
	Fixture.Pawns[1]->PawnClientRestart();
	if (!Fixture.Press(*this, 1)) { return false; }
	TestTrue(TEXT("New Pawn binding activates only its own Slot GA"), NewAbility->IsActive() && NewAbility->ActivationCalls == 1
		&& !Ability->IsActive() && Ability->ActivationCalls == 1 && Fixture.ASCs[1]->FindAbilitySpecFromHandle(NewHandle)->InputPressed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOSquadSwitchUncancelableSmoke,
	"GGYGO.Teams.SquadSwitch.UncancelableRejectsBeforeEffects", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOSquadSwitchUncancelableSmoke::RunTest(const FString& Parameters)
{
	GGYGOSquadSwitchTests::FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGameplayAbilitySpecHandle CancelHandle, BlockingHandle;
	UGGYGOSquadSwitchTestAbility* Cancel = Fixture.Grant(*this, 0, UGGYGOSquadSwitchTestAbility::StaticClass(), true, CancelHandle);
	UGGYGOSquadSwitchTestAbility* Blocking = Fixture.Grant(*this, 0, UGGYGOSquadSwitchTestAbility::StaticClass(), false, BlockingHandle);
	if (!Cancel || !Blocking || !Fixture.PressAndActivate(*this, Cancel)) { return false; }
	const bool bActivated = Fixture.ASCs[0]->TryActivateAbility(BlockingHandle);
	if (!TestTrue(TEXT("Second real Cancel-policy GA activated"), bActivated && Blocking->IsActive())) { return false; }
	Blocking->SetCanBeCanceledForTest(false);
	if (!TestFalse(TEXT("Native blocker really cannot be cancelled"), Blocking->CanBeCanceled())) { return false; }
	const auto OriginalInput = Fixture.Pawns[0]->GetInputComponentForTest();
	const int32 BindingCount = OriginalInput->GetActionEventBindings().Num();
	const FGGYGOPawnASCResourceHandle OldResource = Fixture.Resource(0);
	if (!Fixture.CheckCurrentBindings(*this)) { return false; }
	const GGYGOSquadSwitchTests::FPresentationSnapshot OldPresentation(Fixture.Pawns[0]);
	const GGYGOSquadSwitchTests::FPresentationSnapshot NewPresentation(Fixture.Pawns[1]);
	AddInfo(FString::Printf(TEXT("Before rejection: old %s; new %s"), *OldPresentation.Describe(), *NewPresentation.Describe()));
	// Native PawnClientRestart calls Character::Restart. In this floorless private
	// world SetDefaultMovementMode may legally enter Falling instead of Walking.
	if (!TestTrue(TEXT("Original active Pawn has legal native presentation before rejection"),
		!OldPresentation.bHidden && OldPresentation.bCollisionEnabled && OldPresentation.CustomMovementMode == 0
		&& (OldPresentation.MovementMode == MOVE_Walking || OldPresentation.MovementMode == MOVE_Falling))
		|| !TestTrue(TEXT("Original standby Pawn has legal inactive presentation before rejection"),
			NewPresentation.bHidden && !NewPresentation.bCollisionEnabled
			&& NewPresentation.MovementMode == MOVE_None && NewPresentation.CustomMovementMode == 0)) { return false; }
	AddExpectedMessagePlain(TEXT("Reason=UncancelableAbility"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("A Cancel decision on an uncancelable GA rejects the whole switch"), Fixture.Squad->SwitchToSlot(1));
	TestTrue(TEXT("No partial cancellation or completion"), Cancel->IsActive() && Blocking->IsActive()
		&& Cancel->CleanupCalls == 0 && Blocking->CleanupCalls == 0 && Fixture.CompletionCalls == 0);
	TestTrue(TEXT("Control and authoritative active Slot unchanged"), Fixture.IsControlled(0)
		&& Fixture.Pawns[0]->UnpossessCalls == 0 && Fixture.Pawns[1]->PossessCalls == 0 && Fixture.Pawns[0]->ActiveCharacterNotices == 0);
	TestTrue(TEXT("Input identity/component and held state not retired"), Fixture.Pawns[0]->GetInputComponentForTest() == OriginalInput
		&& OriginalInput->GetActionEventBindings().Num() == BindingCount
		&& Fixture.ASCs[0]->FindAbilitySpecFromHandle(CancelHandle)->InputPressed && Cancel->InputReleasedCalls == 0);
	const GGYGOSquadSwitchTests::FPresentationSnapshot OldAfter(Fixture.Pawns[0]);
	const GGYGOSquadSwitchTests::FPresentationSnapshot NewAfter(Fixture.Pawns[1]);
	TestTrue(FString::Printf(TEXT("Old presentation exactly unchanged after rejection (before %s; after %s)"),
		*OldPresentation.Describe(), *OldAfter.Describe()), OldAfter.HasSamePresentation(OldPresentation));
	TestTrue(FString::Printf(TEXT("Standby presentation exactly unchanged after rejection (before %s; after %s)"),
		*NewPresentation.Describe(), *NewAfter.Describe()), NewAfter.HasSamePresentation(NewPresentation));
	TestTrue(TEXT("Rejection applies no switch tags"), Fixture.HasNoSwitchTags(0) && Fixture.HasNoSwitchTags(1));
	TestTrue(TEXT("Rejection keeps the original binding resource"), Fixture.Resource(0).HasSameResource(OldResource));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOSquadSwitchLingerSmoke,
	"GGYGO.Teams.SquadSwitch.ContinueLingersThenExits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOSquadSwitchLingerSmoke::RunTest(const FString& Parameters)
{
	GGYGOSquadSwitchTests::FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGameplayAbilitySpecHandle OldHandle, NewHandle, ExitHandle;
	UGGYGOSquadSwitchTestAbility* Ability = Fixture.Grant(*this, 0, UGGYGOSquadSwitchContinueTestAbility::StaticClass(), true, OldHandle);
	UGGYGOSquadSwitchTestAbility* NewAbility = Fixture.Grant(*this, 1, UGGYGOSquadSwitchTestAbility::StaticClass(), true, NewHandle);
	UGGYGOSquadSwitchTestAbility* Exit = Fixture.Grant(*this, 0, UGGYGOSquadSwitchExitTestAbility::StaticClass(), false, ExitHandle);
	if (!Ability || !NewAbility || !Exit || !Fixture.PressAndActivate(*this, Ability)) { return false; }
	// 长按蓄力类：输入仍按住，且声明不可取消；ContinueOnField 不阻止换人。
	Ability->SetCanBeCanceledForTest(false);
	if (!TestFalse(TEXT("Explicit Continue is a real uncancelable native instance"), Ability->CanBeCanceled())) { return false; }
	const FGGYGOAbilityActivationHandle Original = Ability->ActivatedOriginal;
	const FGGYGOPawnASCResourceHandle OldResource = Fixture.Resource(0);
	auto OldPress = Fixture.CapturePressBindings();
	if (!Fixture.CheckCurrentBindings(*this)) { return false; }
	if (!TestTrue(TEXT("ContinueOnField does not block the switch"), Fixture.Squad->SwitchToSlot(1))) { return false; }
	TestTrue(TEXT("Continue did not receive Cancel or Completed"), Ability->IsActive() && Ability->CleanupCalls == 0 && Fixture.CompletionCalls == 0);
	TestTrue(TEXT("Continue keeps its original activation identity"), Ability->CaptureCurrentActivation().HasSameActivation(Original));
	TestEqual(TEXT("Held input received one real release"), Ability->InputReleasedCalls, 1);
	TestEqual(TEXT("Continuing ability notified of control loss once"), Ability->ControlLostCalls, 1);
	TestTrue(TEXT("Control-loss notice follows the real release"), Ability->InputReleasedBeforeControlLost == 1 && !Ability->bInputPressedAtControlLost);
	TestEqual(TEXT("Old member lingers"), Fixture.Squad->GetMemberState(0), EGGYGOSquadMemberState::Lingering);
	TestTrue(TEXT("Lingering member stays visible, collidable and movable without control"), Fixture.IsOnFieldWithoutControl(0));
	TestTrue(TEXT("Lingering member carries OffField and damage immunity"), Fixture.HasOffField(0)
		&& !Fixture.ASCs[0]->HasMatchingGameplayTag(GGYGOGameplayTags::State_SwitchReturnPending));
	TestFalse(TEXT("Exit ability waits for the continuing ability"), Exit->IsActive());
	TestTrue(TEXT("New member controlled"), Fixture.IsControlled(1) && Fixture.HasNoSwitchTags(1));
	TestTrue(TEXT("Continue switch preserves native unowned old Pawn and persistent Slot ownership"),
		Fixture.Pawns[0]->GetOwner() == nullptr && Fixture.Slots[0]->GetOwner() == Fixture.Input.GetController());
	TestTrue(TEXT("Legal UnPossess keeps the original binding resource"), Fixture.Resource(0).HasSameResource(OldResource));
	Fixture.CheckCurrentBindings(*this, TEXT("after native control transfer with Continue"));
	const FInputActionInstance OldAction(Fixture.Input.GetAbilityAction());
	for (const auto& Binding : OldPress) { Binding->Execute(OldAction); }
	Fixture.ASCs[0]->ProcessAbilityInput(0.0f, false);
	Fixture.Input.GetController()->ConsumeInputForTest();
	TestTrue(TEXT("Retired old binding gives neither old input nor successor permission"),
		!Fixture.ASCs[0]->FindAbilitySpecFromHandle(OldHandle)->InputPressed
		&& !Fixture.ASCs[1]->FindAbilitySpecFromHandle(NewHandle)->InputPressed
		&& Ability->ActivationCalls == 1 && NewAbility->ActivationCalls == 0);

	// 留场不可受击，其能力照常结算。免疫在 Pre 阶段拒绝，不发伤害消息；结算侧用不破韧的削韧，
	// 本私有世界没有消息路由，Damage 结算会因消息投递失败记录错误。
	const float LingeringHealth = Fixture.Health(0);
	const float LingeringPoise = Fixture.Poise(0);
	const float ActivePoise = Fixture.Poise(1);
	Fixture.ApplyMeta(1, 0, UGGYGOHealthSet::GetDamageAttribute(), 10.0f);
	Fixture.ApplyMeta(1, 0, UGGYGOHealthSet::GetPoiseDamageAttribute(), 10.0f);
	TestTrue(TEXT("Lingering member takes no damage or poise damage"),
		Fixture.Health(0) == LingeringHealth && Fixture.Poise(0) == LingeringPoise);
	Fixture.ApplyMeta(0, 1, UGGYGOHealthSet::GetPoiseDamageAttribute(), 10.0f);
	TestEqual(TEXT("Lingering member's effects still settle"), Fixture.Poise(1), ActivePoise - 10.0f);

	// 续放结束 → 退场能力开始；退场结束 → 待命。
	TestTrue(TEXT("Same original GA normal End accepted after control transfer"), Ability->EndForTest());
	TestTrue(TEXT("Normal End retains original identity and ends once"), !Ability->IsActive() && Ability->CleanupCalls == 1
		&& Ability->CleanedOriginal.HasSameActivation(Original) && !Fixture.bLastEndedCancelled);
	TestTrue(TEXT("Lingering end starts the SwitchOut ability"), Exit->IsActive() && Exit->ActivationCalls == 1);
	TestEqual(TEXT("Member exits while the exit ability runs"), Fixture.Squad->GetMemberState(0), EGGYGOSquadMemberState::Exiting);
	TestTrue(TEXT("Exiting member is still on field and immune"), Fixture.IsOnFieldWithoutControl(0) && Fixture.HasOffField(0));
	TestTrue(TEXT("Exit ability ends normally"), Exit->EndForTest());
	TestTrue(TEXT("Exit completion benches the member and returns OffField state"), Fixture.IsBenched(0));
	TestTrue(TEXT("New reciprocal control remains intact after background End"), Fixture.IsControlled(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOSquadSwitchReturnSmoke,
	"GGYGO.Teams.SquadSwitch.ReturnToLingeringMember", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOSquadSwitchReturnSmoke::RunTest(const FString& Parameters)
{
	GGYGOSquadSwitchTests::FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGameplayAbilitySpecHandle ContinueHandle, EnterHandle;
	UGGYGOSquadSwitchTestAbility* Continue = Fixture.Grant(*this, 0, UGGYGOSquadSwitchContinueTestAbility::StaticClass(), false, ContinueHandle);
	UGGYGOSquadSwitchTestAbility* Enter = Fixture.Grant(*this, 0, UGGYGOSquadSwitchEnterTestAbility::StaticClass(), false, EnterHandle);
	if (!Continue || !Enter || !TestTrue(TEXT("Continue GA activated"), Fixture.ASCs[0]->TryActivateAbility(ContinueHandle) && Continue->IsActive())) { return false; }
	if (!TestTrue(TEXT("Switch away leaves the member lingering"), Fixture.Squad->SwitchToSlot(1)
		&& Fixture.Squad->GetMemberState(0) == EGGYGOSquadMemberState::Lingering)) { return false; }
	// 回场后才授予的按下能力，避免与续放能力共享首按。
	FGameplayAbilitySpecHandle PressHandle;
	UGGYGOSquadSwitchTestAbility* Pressed = Fixture.Grant(*this, 0, UGGYGOSquadSwitchTriggeredTestAbility::StaticClass(), true, PressHandle);
	if (!Pressed) { return false; }

	if (!TestTrue(TEXT("Switching back to a lingering member succeeds"), Fixture.Squad->SwitchToSlot(0))) { return false; }
	TestTrue(TEXT("Control moves immediately"), Fixture.IsControlled(0));
	TestTrue(TEXT("Return removes OffField and applies ReturnPending"),
		!Fixture.ASCs[0]->HasMatchingGameplayTag(GGYGOGameplayTags::State_OffField)
		&& !Fixture.ASCs[0]->HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity)
		&& Fixture.ASCs[0]->HasMatchingGameplayTag(GGYGOGameplayTags::State_SwitchReturnPending));
	TestTrue(TEXT("Continuing ability keeps running and is told control returned"), Continue->IsActive() && Continue->ControlRegainedCalls == 1);
	TestEqual(TEXT("Returning to a lingering member plays no SwitchIn"), Enter->ActivationCalls, 0);
	TestTrue(TEXT("Member left without continuing abilities is benched"), Fixture.IsBenched(1));
	// CMC 在 BeginPlay 订阅本地 ASC；本未开局私有世界不覆盖 locomotion 阻止，只核对其读取的 Tag。

	// 原生 UnPossess 销毁了输入组件；私有世界不投递 ClientRestart，显式走原生本地重建。
	if (!Fixture.Pawns[0]->GetInputComponentForTest()) { Fixture.Pawns[0]->PawnClientRestart(); }
	if (!Fixture.Press(*this, 0)) { return false; }
	TestTrue(TEXT("Press during ReturnPending is buffered, not activated"), !Pressed->IsActive() && Pressed->ActivationCalls == 0);
	TestTrue(TEXT("Continue GA ends normally"), Continue->EndForTest());
	TestTrue(TEXT("ReturnPending ends with the last continuing ability"), Fixture.HasNoSwitchTags(0) && Fixture.IsControlled(0));
	Fixture.Input.GetController()->ConsumeInputForTest();
	TestTrue(TEXT("Buffered press activates once after ReturnPending"), Pressed->IsActive() && Pressed->ActivationCalls == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOSquadSwitchThreeMemberSmoke,
	"GGYGO.Teams.SquadSwitch.ThreeMembersOneLingering", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOSquadSwitchThreeMemberSmoke::RunTest(const FString& Parameters)
{
	GGYGOSquadSwitchTests::FFixture Fixture;
	if (!Fixture.Initialize(*this, 3)) { return false; }
	FGameplayAbilitySpecHandle ContinueHandle, EnterHandle;
	UGGYGOSquadSwitchTestAbility* Continue = Fixture.Grant(*this, 0, UGGYGOSquadSwitchContinueTestAbility::StaticClass(), false, ContinueHandle);
	UGGYGOSquadSwitchTestAbility* Enter = Fixture.Grant(*this, 2, UGGYGOSquadSwitchEnterTestAbility::StaticClass(), false, EnterHandle);
	if (!Continue || !Enter || !TestTrue(TEXT("Continue GA activated"), Fixture.ASCs[0]->TryActivateAbility(ContinueHandle))) { return false; }
	if (!TestTrue(TEXT("1 -> 2 succeeds"), Fixture.Squad->SwitchToSlot(1))) { return false; }
	if (!TestTrue(TEXT("2 -> 3 succeeds while 1 lingers"), Fixture.Squad->SwitchToSlot(2))) { return false; }
	TestTrue(TEXT("Member 3 is controlled and its SwitchIn ability ran"), Fixture.IsControlled(2) && Enter->IsActive() && Enter->ActivationCalls == 1);
	TestEqual(TEXT("Member 1 still lingers"), Fixture.Squad->GetMemberState(0), EGGYGOSquadMemberState::Lingering);
	TestTrue(TEXT("Member 1 still on field and immune"), Fixture.IsOnFieldWithoutControl(0) && Fixture.HasOffField(0));
	TestTrue(TEXT("Member 2 had nothing to finish and is benched"), Fixture.IsBenched(1));
	TestTrue(TEXT("Member 1 continuing GA ends"), Continue->EndForTest());
	TestTrue(TEXT("Member 1 benches after its ability ends"), Fixture.IsBenched(0));
	TestTrue(TEXT("Member 3 keeps control"), Fixture.IsControlled(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOSquadSwitchReturnDuringExitSmoke,
	"GGYGO.Teams.SquadSwitch.ReturnDuringExitCancelsExit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOSquadSwitchReturnDuringExitSmoke::RunTest(const FString& Parameters)
{
	GGYGOSquadSwitchTests::FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGameplayAbilitySpecHandle ExitHandle, EnterHandle;
	UGGYGOSquadSwitchTestAbility* Exit = Fixture.Grant(*this, 0, UGGYGOSquadSwitchExitTestAbility::StaticClass(), false, ExitHandle);
	UGGYGOSquadSwitchTestAbility* Enter = Fixture.Grant(*this, 0, UGGYGOSquadSwitchEnterTestAbility::StaticClass(), false, EnterHandle);
	if (!Exit || !Enter || !TestTrue(TEXT("Switch away succeeds"), Fixture.Squad->SwitchToSlot(1))) { return false; }
	TestTrue(TEXT("No continuing ability: exit starts immediately"), Exit->IsActive()
		&& Fixture.Squad->GetMemberState(0) == EGGYGOSquadMemberState::Exiting
		&& Fixture.IsOnFieldWithoutControl(0) && Fixture.HasOffField(0));
	if (!TestTrue(TEXT("Switching back during exit succeeds"), Fixture.Squad->SwitchToSlot(0))) { return false; }
	TestTrue(TEXT("Return cancels the exit ability"), !Exit->IsActive() && Exit->CleanupCalls == 1
		&& Fixture.LastEndedAbility.Get() == Exit && Fixture.bLastEndedCancelled);
	TestTrue(TEXT("Returned member controlled without OffField or ReturnPending"), Fixture.IsControlled(0) && Fixture.HasNoSwitchTags(0)
		&& !Fixture.Pawns[0]->GetCharacterMovement()->bRunPhysicsWithNoController);
	TestEqual(TEXT("Return during exit plays no SwitchIn"), Enter->ActivationCalls, 0);
	TestTrue(TEXT("Other member benched"), Fixture.IsBenched(1));
	return true;
}

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/GameModeBase.h"
#include "GameModes/GGYGOExperienceDefinition.h"
#include "GameModes/GGYGOGameMode.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"

namespace GGYGOSquadSwitchNetTests
{
	/**
	 * 监听服务器＋1 客户端的同进程 PIE：生产 GameMode／Experience／Squad 装配，客户端经 Server RPC 切人。
	 * 编队用 Experience 首位角色的两份实例（测试编队），在 PostLogin 与装配之间写入 Roster；
	 * 续放能力是测试夹具 GA（无生产续放能力），进退场能力缺省时按既定规则立即进场／待命。
	 * 只读复制结果，不驱动输入、动画或镜头；画面、Montage／FX 表现与真实设备不在本例范围。
	 */
	class FListenServerSwitchCommand : public IAutomationLatentCommand
	{
	public:
		explicit FListenServerSwitchCommand(FAutomationTestBase& InTest) : Test(InTest) {}

		virtual ~FListenServerSwitchCommand() override
		{
			FGameModeEvents::GameModePostLoginEvent.Remove(PostLoginHandle);
			if (Stage != EStage::Done)
			{
				Fail(TEXT("automation aborted before owned PIE cleanup"));
				EndOwnedPIE(true);
			}
		}

		virtual bool Update() override
		{
			if (Stage == EStage::Done) { return true; }
			if (Stage == EStage::Start) { StartPIE(); return false; }
			if (Stage == EStage::Ending) { return UpdateEnding(); }
			if (bFailed) { BeginEnding(); return false; }
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Fail(FString::Printf(TEXT("bounded wait timeout; last unmet condition: %s"), *LastUnmet));
				BeginEnding();
				return false;
			}
			if (!ResolveSides()) { return false; }
			switch (Stage)
			{
			case EStage::Ready: ObserveReady(); break;
			case EStage::Lingering: ObserveLingering(); break;
			case EStage::Returned: ObserveReturned(); break;
			case EStage::Released: ObserveReleased(); break;
			case EStage::Rebenched: ObserveRebenched(); break;
			case EStage::HealthDamage: ObserveHealthDamage(); break;
			case EStage::HealthMaxDrop: ObserveHealthMaxDrop(); break;
			case EStage::HealthPoise: ObserveHealthPoise(); break;
			default: break;
			}
			return false;
		}

	private:
		enum class EStage : uint8 { Start, Ready, Lingering, Returned, Released, Rebenched, HealthDamage, HealthMaxDrop, HealthPoise, Ending, Done };

		/** 一侧（服务器或客户端）上的同一名玩家与其编队。 */
		struct FSide
		{
			UWorld* World = nullptr;
			APlayerController* Controller = nullptr;
			UGGYGOSquadComponent* Squad = nullptr;
			AGGYGOCharacterSlot* Slot(int32 Index) const { return Squad ? Squad->GetSlot(Index) : nullptr; }
			/** 客户端因相关性销毁的副本在 GC 前仍被复制属性引用，按无效处理。 */
			APawn* Avatar(int32 Index) const
			{
				const AGGYGOCharacterSlot* S = Slot(Index);
				APawn* Pawn = S ? S->GetAvatarPawn() : nullptr;
				return IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() ? Pawn : nullptr;
			}
			UGGYGOAbilitySystemComponent* ASC(int32 Index) const
			{
				const AGGYGOCharacterSlot* S = Slot(Index);
				return S ? S->GetGGYGOAbilitySystemComponent() : nullptr;
			}
		};

		void Fail(const FString& Reason)
		{
			if (bFailed) { return; }
			bFailed = true;
			Test.AddError(FString::Printf(TEXT("[Teams.SquadSwitch.ListenServer] Stage=%u: %s"), static_cast<uint32>(Stage), *Reason));
		}

		/** 条件未满足时记录原因，用于超时诊断。 */
		bool Require(bool bCondition, const TCHAR* Condition)
		{
			if (!bCondition) { LastUnmet = Condition; }
			return bCondition;
		}

		void Advance(EStage Next, double Seconds = 15.0)
		{
			Stage = Next;
			LastUnmet.Reset();
			Deadline = FPlatformTime::Seconds() + Seconds;
		}

		int32 CountPIEWorlds() const
		{
			int32 Count = 0;
			if (UEditorEngine* Editor = Cast<UEditorEngine>(GEngine))
			{
				for (const FWorldContext& Context : Editor->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE) { ++Count; }
				}
			}
			return Count;
		}

		bool OwnsPIESession() const
		{
			const UEditorEngine* Editor = Cast<UEditorEngine>(GEngine);
			const TOptional<FPlayInEditorSessionInfo> Info = Editor ? Editor->GetPlayInEditorSessionInfo() : TOptional<FPlayInEditorSessionInfo>();
			return Info.IsSet() && RequestSettings.IsValid() && Info->OriginalRequestParams.EditorPlaySettings == RequestSettings.Get();
		}

		void StartPIE()
		{
			Advance(EStage::Ready, 90.0);
			// NullRHI 没有角色渲染需要的 R32f 深度格式，渲染组件按约定拒绝并停用；只属本运行环境，与切人无关。
			Test.AddExpectedError(TEXT("当前RHI不支持配置的R32f深度输出/采样"), EAutomationExpectedErrorFlags::Contains, -1, false);
			UEditorEngine* Editor = Cast<UEditorEngine>(GEngine);
			if (!Editor || Editor != GEditor) { Fail(TEXT("requires the Editor engine")); return; }
			if (Editor->IsPlaySessionInProgress() || Editor->PlayWorld || CountPIEWorlds() != 0 || Editor->GetPlaySessionRequest().IsSet())
			{
				Fail(TEXT("existing or queued PIE is not owned by this test")); return;
			}
			PostLoginHandle = FGameModeEvents::GameModePostLoginEvent.AddLambda(
				[this](AGameModeBase* GameMode, APlayerController* NewPlayer) { AssignTestRoster(GameMode, NewPlayer); });
			RequestSettings.Reset(NewObject<ULevelEditorPlaySettings>(GetTransientPackage()));
			RequestSettings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
			RequestSettings->SetRunUnderOneProcess(true);
			RequestSettings->SetPlayNumberOfClients(2);
			RequestSettings->bLaunchSeparateServer = false;
			RequestSettings->GameGetsMouseControl = false;
			RequestSettings->bShouldMinimizeEditorOnNonVRPIE = false;
			FRequestPlaySessionParams Params;
			Params.EditorPlaySettings = RequestSettings.Get();
			Params.bAllowOnlineSubsystem = false;
			Editor->RequestPlaySession(Params);
			// 请求会复制一份设置对象；会话身份以排队请求实际持有的那份为准。
			const TOptional<FRequestPlaySessionParams> Queued = Editor->GetPlaySessionRequest();
			if (!Queued.IsSet() || !Queued->EditorPlaySettings)
			{
				Fail(TEXT("native PIE request did not retain its settings")); return;
			}
			RequestSettings.Reset(Queued->EditorPlaySettings.Get());
		}

		/** PostLogin 早于 HandleStartingNewPlayer：在装配前写入测试编队，走生产的“已设 Roster”来源。 */
		void AssignTestRoster(AGameModeBase* GameMode, APlayerController* NewPlayer)
		{
			if (!GameMode || !NewPlayer || !GameMode->GetWorld() || GameMode->GetWorld()->WorldType != EWorldType::PIE) { return; }
			const AGGYGOGameMode* ProjectGameMode = Cast<AGGYGOGameMode>(GameMode);
			const UGGYGOExperienceDefinition* Experience = ProjectGameMode ? ProjectGameMode->GetExperience() : nullptr;
			const UGGYGOPawnData* Member = Experience && !Experience->SquadMembers.IsEmpty() ? Experience->SquadMembers[0].Get() : nullptr;
			const AGGYGOPlayerState* PlayerState = NewPlayer->GetPlayerState<AGGYGOPlayerState>();
			UGGYGOSquadComponent* Squad = PlayerState ? PlayerState->GetSquadComponent() : nullptr;
			if (!Member || !Squad)
			{
				Fail(FString::Printf(TEXT("PIE map lacks the project GameMode/Experience member or PlayerState Squad: GameMode=%s Experience=%s"),
					*GetPathNameSafe(GameMode), *GetPathNameSafe(Experience)));
				return;
			}
			UGGYGOPawnData* MutableMember = const_cast<UGGYGOPawnData*>(Member);
			if (!Squad->SetRoster({MutableMember, MutableMember}))
			{
				Fail(FString::Printf(TEXT("SetRoster rejected the two-member test squad for %s"), *GetPathNameSafe(NewPlayer)));
				return;
			}
			++RosterAssignments;
		}

		/** 找到监听服务器世界中远端玩家的编队，以及客户端世界中本地玩家的编队。 */
		bool ResolveSides()
		{
			Server = FSide();
			Client = FSide();
			const UEditorEngine* Editor = Cast<UEditorEngine>(GEngine);
			if (!Editor) { return Require(false, TEXT("Editor")); }
			for (const FWorldContext& Context : Editor->GetWorldContexts())
			{
				UWorld* World = Context.WorldType == EWorldType::PIE ? Context.World() : nullptr;
				if (!World || !World->HasBegunPlay()) { continue; }
				if (World->GetNetMode() == NM_ListenServer) { Server.World = World; }
				else if (World->GetNetMode() == NM_Client) { Client.World = World; }
			}
			if (!Require(Server.World && Client.World, TEXT("listen server and client PIE worlds"))) { return false; }
			for (FConstPlayerControllerIterator It = Server.World->GetPlayerControllerIterator(); It; ++It)
			{
				if (APlayerController* PC = It->Get(); PC && !PC->IsLocalController()) { Server.Controller = PC; }
			}
			const UGameInstance* ClientInstance = Client.World->GetGameInstance();
			const ULocalPlayer* ClientPlayer = ClientInstance ? ClientInstance->GetFirstGamePlayer() : nullptr;
			Client.Controller = ClientPlayer ? ClientPlayer->GetPlayerController(Client.World) : nullptr;
			for (FSide* Side : {&Server, &Client})
			{
				const AGGYGOPlayerState* PlayerState = Side->Controller ? Side->Controller->GetPlayerState<AGGYGOPlayerState>() : nullptr;
				Side->Squad = PlayerState ? PlayerState->GetSquadComponent() : nullptr;
			}
			return Require(Server.Squad && Client.Squad && Server.Slot(0) && Server.Slot(1) && Client.Slot(0) && Client.Slot(1),
				TEXT("remote player's replicated PlayerState squad with two Slots on both sides"));
		}

		static bool IsOnField(const APawn* Pawn) { return Pawn && !Pawn->IsHidden() && Pawn->GetActorEnableCollision(); }

		/** 客户端上的待命队员：不相关时没有副本；相关时（属于本客户端）隐藏且无碰撞。 */
		static bool IsBenchedOnClient(const APawn* Pawn) { return !Pawn || (Pawn->IsHidden() && !Pawn->GetActorEnableCollision()); }

		bool HasTag(const UGGYGOAbilitySystemComponent* ASC, const FGameplayTag& Tag) const
		{
			return ASC && ASC->HasMatchingGameplayTag(Tag);
		}

		bool IsClientControlling(int32 Index) const
		{
			const APawn* Avatar = Client.Avatar(Index);
			return Avatar && Client.Controller->GetPawn() == Avatar && Client.Squad->GetActiveSlot() == Client.Slot(Index)
				&& Client.Controller->GetViewTarget() == Avatar && IsOnField(Avatar);
		}

		void ObserveReady()
		{
			if (!Require(RosterAssignments == 2, TEXT("test roster written for both players before assembly"))
				|| !Require(Server.Squad->GetActiveSlot() == Server.Slot(0)
					&& Server.Squad->GetMemberState(0) == EGGYGOSquadMemberState::Active
					&& Server.Squad->GetMemberState(1) == EGGYGOSquadMemberState::Benched, TEXT("server: first member active, second benched"))
				|| !Require(IsClientControlling(0), TEXT("client: possessed and views visible collidable first member"))
				|| !Require(Client.Avatar(1) && IsBenchedOnClient(Client.Avatar(1)),
					TEXT("client: own benched member replicated hidden without collision")))
			{
				return;
			}
			// 服务器上的续放：夹具 GA 选择 ContinueOnField，服务器执行，客户端只看复制结果。
			UGGYGOAbilitySystemComponent* ServerASC = Server.ASC(0);
			const FGameplayAbilitySpecHandle Handle = ServerASC->GiveAbility(FGameplayAbilitySpec(UGGYGOSquadSwitchContinueTestAbility::StaticClass(), 1));
			const FGameplayAbilitySpec* Spec = ServerASC->FindAbilitySpecFromHandle(Handle);
			LingerAbility = Spec ? Cast<UGGYGOSquadSwitchTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
			if (!LingerAbility.IsValid() || !ServerASC->TryActivateAbility(Handle) || !LingerAbility->IsActive())
			{
				Fail(TEXT("server could not activate the continuing fixture ability")); return;
			}
			Test.AddInfo(TEXT("[Teams.SquadSwitch.ListenServer] Ready: production assembly replicated; client requests switch to slot 1."));
			Client.Squad->ServerRequestSwitchToSlot(1);
			Advance(EStage::Lingering);
		}

		void ObserveLingering()
		{
			if (!Require(Server.Squad->GetActiveSlot() == Server.Slot(1)
					&& Server.Squad->GetMemberState(0) == EGGYGOSquadMemberState::Lingering
					&& Server.Squad->GetMemberState(1) == EGGYGOSquadMemberState::Active
					&& LingerAbility.IsValid() && LingerAbility->IsActive(), TEXT("server: RPC switched, old member lingers with its ability"))
				|| !Require(IsClientControlling(1), TEXT("client: controls and views the new member"))
				|| !Require(IsOnField(Client.Avatar(0)), TEXT("client: lingering member visible and collidable"))
				|| !Require(HasTag(Client.ASC(0), GGYGOGameplayTags::State_OffField)
					&& !HasTag(Client.ASC(0), GGYGOGameplayTags::Gameplay_Damage_Immunity),
					TEXT("client: OffField replicated, server-only immunity not replicated")))
			{
				return;
			}
			Test.AddInfo(TEXT("[Teams.SquadSwitch.ListenServer] Lingering replicated; client requests return to slot 0."));
			Client.Squad->ServerRequestSwitchToSlot(0);
			Advance(EStage::Returned);
		}

		void ObserveReturned()
		{
			if (!Require(Server.Squad->GetActiveSlot() == Server.Slot(0)
					&& Server.Squad->GetMemberState(0) == EGGYGOSquadMemberState::Active
					&& Server.Squad->GetMemberState(1) == EGGYGOSquadMemberState::Benched
					&& HasTag(Server.ASC(0), GGYGOGameplayTags::State_SwitchReturnPending), TEXT("server: returned with ReturnPending, other benched"))
				|| !Require(IsClientControlling(0), TEXT("client: controls the returned member"))
				|| !Require(HasTag(Client.ASC(0), GGYGOGameplayTags::State_SwitchReturnPending)
					&& !HasTag(Client.ASC(0), GGYGOGameplayTags::State_OffField), TEXT("client: ReturnPending replicated, OffField removed"))
				|| !Require(IsBenchedOnClient(Client.Avatar(1)), TEXT("client: other member hidden without collision or not relevant")))
			{
				return;
			}
			if (!LingerAbility.IsValid() || !LingerAbility->EndForTest()) { Fail(TEXT("server could not end the continuing ability")); return; }
			Advance(EStage::Released);
		}

		void ObserveReleased()
		{
			if (!Require(!HasTag(Server.ASC(0), GGYGOGameplayTags::State_SwitchReturnPending), TEXT("server: ReturnPending removed after the ability ended"))
				|| !Require(!HasTag(Client.ASC(0), GGYGOGameplayTags::State_SwitchReturnPending), TEXT("client: ReturnPending removal replicated")))
			{
				return;
			}
			Test.AddInfo(TEXT("[Teams.SquadSwitch.ListenServer] ReturnPending cleared; client requests switch to benched slot 1."));
			Client.Squad->ServerRequestSwitchToSlot(1);
			Advance(EStage::Rebenched);
		}

		void ObserveRebenched()
		{
			if (!Require(Server.Squad->GetActiveSlot() == Server.Slot(1)
					&& Server.Squad->GetMemberState(1) == EGGYGOSquadMemberState::Active
					&& Server.Squad->GetMemberState(0) == EGGYGOSquadMemberState::Benched
					&& IsOnField(Server.Avatar(1)) && !IsOnField(Server.Avatar(0)), TEXT("server: benched member entered, old member benched"))
				|| !Require(IsClientControlling(1), TEXT("client: controls the re-entered member"))
				|| !Require(IsBenchedOnClient(Client.Avatar(0)), TEXT("client: old member hidden without collision or not relevant")))
			{
				return;
			}
			Test.AddInfo(TEXT("[Teams.SquadSwitch.ListenServer] Benched member re-entered; server applies damage to the client's active member."));
			// 生命复制：服务器结算，客户端只经复制收到数值与 HealthComponent 广播。
			ServerHealth = UGGYGOHealthComponent::FindHealthComponent(Server.Avatar(1));
			ClientHealth = UGGYGOHealthComponent::FindHealthComponent(Client.Avatar(1));
			if (!ServerHealth.IsValid() || !ClientHealth.IsValid()) { Fail(TEXT("active member lacks HealthComponent on server or client")); return; }
			ServerObserver.Reset(NewObject<UGGYGOSquadSwitchHealthObserver>(GetTransientPackage()));
			ClientObserver.Reset(NewObject<UGGYGOSquadSwitchHealthObserver>(GetTransientPackage()));
			ServerObserver->Observe(ServerHealth.Get());
			ClientObserver->Observe(ClientHealth.Get());
			const float HealthBefore = Attribute(Server.ASC(1), UGGYGOHealthSet::GetHealthAttribute());
			if (!ApplyInstantMeta(UGGYGOHealthSet::GetDamageAttribute(), 10.0f)) { return; }
			ExpectedHealth = Attribute(Server.ASC(1), UGGYGOHealthSet::GetHealthAttribute());
			if (!FMath::IsNearlyEqual(ExpectedHealth, HealthBefore - 10.0f, 0.01f))
			{
				Fail(FString::Printf(TEXT("server damage did not settle: before=%.3f after=%.3f"), HealthBefore, ExpectedHealth)); return;
			}
			Advance(EStage::HealthDamage);
		}

		static float Attribute(const UGGYGOAbilitySystemComponent* ASC, const FGameplayAttribute& InAttribute)
		{
			return ASC ? ASC->GetNumericAttribute(InAttribute) : -1.0f;
		}

		static bool Same(float A, float B) { return FMath::IsNearlyEqual(A, B, 0.01f); }

		/** 主机的出战队员向客户端出战队员施加一次即时元属性 GE，与命中结算同一 HealthSet 路径。 */
		bool ApplyInstantMeta(const FGameplayAttribute& Meta, float Magnitude)
		{
			UGGYGOAbilitySystemComponent* Source = nullptr;
			for (FConstPlayerControllerIterator It = Server.World->GetPlayerControllerIterator(); It; ++It)
			{
				const APlayerController* PC = It->Get();
				const AGGYGOPlayerState* PlayerState = PC && PC->IsLocalController() ? PC->GetPlayerState<AGGYGOPlayerState>() : nullptr;
				const UGGYGOSquadComponent* HostSquad = PlayerState ? PlayerState->GetSquadComponent() : nullptr;
				const AGGYGOCharacterSlot* HostSlot = HostSquad ? HostSquad->GetActiveSlot() : nullptr;
				Source = HostSlot ? HostSlot->GetGGYGOAbilitySystemComponent() : Source;
			}
			UGGYGOAbilitySystemComponent* Target = Server.ASC(1);
			if (!Source || !Target) { Fail(TEXT("host source or client target ASC missing on server")); return false; }
			UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
			Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
			FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
			Modifier.Attribute = Meta;
			Modifier.ModifierOp = EGameplayModOp::Additive;
			Modifier.ModifierMagnitude = FScalableFloat(Magnitude);
			FGameplayEffectContextHandle Context = Source->MakeEffectContext();
			Context.AddInstigator(Source->GetAvatarActor(), Source->GetAvatarActor());
			Source->ApplyGameplayEffectSpecToTarget(FGameplayEffectSpec(Effect, Context, 1.0f), Target);
			return true;
		}

		bool ClientMatchesServer() const
		{
			const UGGYGOAbilitySystemComponent* ServerASC = Server.ASC(1);
			const UGGYGOAbilitySystemComponent* ClientASC = Client.ASC(1);
			const float Health = Attribute(ServerASC, UGGYGOHealthSet::GetHealthAttribute());
			const float MaxHealth = Attribute(ServerASC, UGGYGOHealthSet::GetMaxHealthAttribute());
			const float Poise = Attribute(ServerASC, UGGYGOHealthSet::GetPoiseAttribute());
			return ClientHealth.IsValid()
				&& Same(Attribute(ClientASC, UGGYGOHealthSet::GetHealthAttribute()), Health)
				&& Same(Attribute(ClientASC, UGGYGOHealthSet::GetMaxHealthAttribute()), MaxHealth)
				&& Same(Attribute(ClientASC, UGGYGOHealthSet::GetPoiseAttribute()), Poise)
				&& Same(ClientHealth->GetHealth(), Health) && Same(ClientHealth->GetMaxHealth(), MaxHealth)
				&& Same(ClientHealth->GetPoise(), Poise);
		}

		void ObserveHealthDamage()
		{
			const float Health = Attribute(Server.ASC(1), UGGYGOHealthSet::GetHealthAttribute());
			if (!Require(Same(Health, ExpectedHealth), TEXT("server: damaged Health stays settled"))
				|| !Require(ClientMatchesServer(), TEXT("client: replicated Health/MaxHealth/Poise and HealthComponent match server"))
				|| !Require(ServerObserver->HealthNotices >= 1 && Same(ServerObserver->LastHealth, Health), TEXT("server: HealthChanged broadcast"))
				|| !Require(ClientObserver->HealthNotices >= 1 && Same(ClientObserver->LastHealth, Health), TEXT("client: HealthChanged broadcast")))
			{
				return;
			}
			// 持续 GE 下调 MaxHealth：分类帧保留的原因（Lyra 写法在服务器不广播 MaxHealthChanged）。
			UGGYGOAbilitySystemComponent* Target = Server.ASC(1);
			const float MaxBefore = Attribute(Target, UGGYGOHealthSet::GetMaxHealthAttribute());
			ServerMaxNoticesBefore = ServerObserver->MaxHealthNotices;
			ClientMaxNoticesBefore = ClientObserver->MaxHealthNotices;
			MaxDrop = Target->ApplyGameplayEffectToSelf(GetDefault<UGGYGOSquadSwitchTestMaxHealthDrop>(), 1.0f, Target->MakeEffectContext());
			ExpectedMaxHealth = Attribute(Target, UGGYGOHealthSet::GetMaxHealthAttribute());
			ExpectedHealth = Attribute(Target, UGGYGOHealthSet::GetHealthAttribute());
			if (!MaxDrop.IsValid() || !Same(ExpectedMaxHealth, MaxBefore - 20.0f) || ExpectedHealth > ExpectedMaxHealth + 0.01f)
			{
				Fail(FString::Printf(TEXT("server MaxHealth drop did not settle: before=%.3f after=%.3f health=%.3f"),
					MaxBefore, ExpectedMaxHealth, ExpectedHealth));
				return;
			}
			Advance(EStage::HealthMaxDrop);
		}

		void ObserveHealthMaxDrop()
		{
			const UGGYGOAbilitySystemComponent* Target = Server.ASC(1);
			if (!Require(Same(Attribute(Target, UGGYGOHealthSet::GetMaxHealthAttribute()), ExpectedMaxHealth)
					&& Same(Attribute(Target, UGGYGOHealthSet::GetHealthAttribute()), ExpectedHealth), TEXT("server: MaxHealth drop and Health clamp settled"))
				|| !Require(ClientMatchesServer(), TEXT("client: replicated MaxHealth drop and clamp match server"))
				|| !Require(ServerObserver->MaxHealthNotices > ServerMaxNoticesBefore && Same(ServerObserver->LastMaxHealth, ExpectedMaxHealth),
					TEXT("server: MaxHealthChanged broadcast for the persistent GE"))
				|| !Require(ClientObserver->MaxHealthNotices > ClientMaxNoticesBefore && Same(ClientObserver->LastMaxHealth, ExpectedMaxHealth),
					TEXT("client: MaxHealthChanged broadcast for the persistent GE")))
			{
				return;
			}
			const float PoiseBefore = Attribute(Target, UGGYGOHealthSet::GetPoiseAttribute());
			if (PoiseBefore <= 0.0f) { Fail(TEXT("active member has no poise to break")); return; }
			if (!ApplyInstantMeta(UGGYGOHealthSet::GetPoiseDamageAttribute(), PoiseBefore)) { return; }
			Advance(EStage::HealthPoise);
		}

		void ObserveHealthPoise()
		{
			const float Poise = Attribute(Server.ASC(1), UGGYGOHealthSet::GetPoiseAttribute());
			if (!Require(Poise <= 0.01f && ServerObserver->PoiseBrokenNotices >= 1, TEXT("server: poise broken and PoiseBroken broadcast"))
				|| !Require(ClientMatchesServer(), TEXT("client: replicated poise matches server"))
				|| !Require(ClientObserver->PoiseNotices >= 1 && Same(ClientObserver->LastPoise, Poise), TEXT("client: PoiseChanged broadcast")))
			{
				return;
			}
			bPassed = true;
			BeginEnding();
		}

		void EndOwnedPIE(bool bSynchronous)
		{
			UEditorEngine* Editor = Cast<UEditorEngine>(GEngine);
			if (!Editor) { return; }
			if (OwnsPIESession())
			{
				if (bSynchronous || !Editor->PlayWorld) { Editor->EndPlayMap(); }
				else { Editor->RequestEndPlayMap(); }
			}
			else
			{
				const TOptional<FRequestPlaySessionParams> Queued = Editor->GetPlaySessionRequest();
				if (Queued.IsSet() && RequestSettings.IsValid() && Queued->EditorPlaySettings == RequestSettings.Get())
				{
					Editor->CancelRequestPlaySession();
				}
			}
		}

		void BeginEnding()
		{
			Stage = EStage::Ending;
			LingerAbility.Reset();
			if (ServerObserver.IsValid()) { ServerObserver->Release(); }
			if (ClientObserver.IsValid()) { ClientObserver->Release(); }
			Deadline = FPlatformTime::Seconds() + 15.0;
			// 无人值守的 -Cmd 会话中排队的 RequestEndPlayMap 不会在本命令等待期间处理；在 latent 更新里同步结束。
			EndOwnedPIE(true);
		}

		bool UpdateEnding()
		{
			if (OwnsPIESession() || CountPIEWorlds() != 0)
			{
				if (FPlatformTime::Seconds() < Deadline) { return false; }
				Fail(TEXT("owned PIE teardown timeout"));
				EndOwnedPIE(true);
			}
			Test.TestTrue(TEXT("listen-server smoke released its PIE session"), !OwnsPIESession() && CountPIEWorlds() == 0);
			if (bPassed && !bFailed)
			{
				Test.AddInfo(TEXT("[Teams.SquadSwitch.ListenServer] Behavior PASS: client RPC switch, linger/return/bench replication, ")
					TEXT("damage/MaxHealth drop/poise break replication with HealthComponent broadcasts, and PIE teardown observed. ")
					TEXT("Production errors logged by other modules stay unfiltered and decide the Automation result."));
			}
			Stage = EStage::Done;
			return true;
		}

		FAutomationTestBase& Test;
		EStage Stage = EStage::Start;
		double Deadline = 0.0;
		bool bFailed = false;
		bool bPassed = false;
		FString LastUnmet;
		int32 RosterAssignments = 0;
		FDelegateHandle PostLoginHandle;
		TStrongObjectPtr<ULevelEditorPlaySettings> RequestSettings;
		TWeakObjectPtr<UGGYGOSquadSwitchTestAbility> LingerAbility;
		TWeakObjectPtr<UGGYGOHealthComponent> ServerHealth;
		TWeakObjectPtr<UGGYGOHealthComponent> ClientHealth;
		TStrongObjectPtr<UGGYGOSquadSwitchHealthObserver> ServerObserver;
		TStrongObjectPtr<UGGYGOSquadSwitchHealthObserver> ClientObserver;
		FActiveGameplayEffectHandle MaxDrop;
		float ExpectedHealth = 0.0f;
		float ExpectedMaxHealth = 0.0f;
		int32 ServerMaxNoticesBefore = 0;
		int32 ClientMaxNoticesBefore = 0;
		FSide Server;
		FSide Client;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOSquadSwitchListenServerSmoke,
	"GGYGO.Teams.SquadSwitch.ListenServerClientRequest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOSquadSwitchListenServerSmoke::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(GGYGOSquadSwitchNetTests::FListenServerSwitchCommand(*this));
	return true;
}
#endif
#endif
