#include "Teams/Tests/GGYGOSquadSwitchTestTypes.h"

#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
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

UGGYGOSquadSwitchContinueTestAbility::UGGYGOSquadSwitchContinueTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AvatarExitPolicy = EGGYGOAbilityAvatarExitPolicy::ContinueInBackground;
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
		// Reuse only the existing real private GI/LocalPlayer/PC/EnhancedInput birth.
		// The squad below uses real project Slot, Character, Host publication and Possess.
		FGGYGOInputTestFixture Input;
		AGGYGOPlayerState* PlayerState = nullptr;
		UGGYGOSquadComponent* Squad = nullptr;
		AGGYGOSquadSwitchTestPawn* Pawns[2] = {};
		AGGYGOCharacterSlot* Slots[2] = {};
		UGGYGOAbilitySystemComponent* ASCs[2] = {};
		TStrongObjectPtr<UGGYGOPawnData> Data;
		TStrongObjectPtr<UGGYGOMovementSet> Movement;
		TStrongObjectPtr<UGameplayEffect> BuffDefinition;
		TArray<TWeakObjectPtr<UGGYGOSquadSwitchTestAbility>> Abilities;
		FDelegateHandle CompletionObserver;
		int32 CompletionCalls = 0;
		FGGYGOAbilityActivationHandle LastCompletedOriginal;
		EGGYGOAbilityTerminationRequestKind LastCompletedKind = EGGYGOAbilityTerminationRequestKind::End;
		FActiveGameplayEffectHandle Buff;
		FActiveGameplayEffectHandle Cooldown;

		~FFixture()
		{
			if (IsValid(ASCs[0])) { ASCs[0]->OnAbilityTerminationCompleted().Remove(CompletionObserver); }
			for (const auto& WeakAbility : Abilities)
			{
				if (UGGYGOSquadSwitchTestAbility* Ability = WeakAbility.Get())
				{
					Ability->DisarmCleanupForTest();
					if (Ability->IsActive())
					{
						Ability->SetCanBeCanceledForTest(true);
						Ability->RequestAbilityEnd(Ability->ActivatedOriginal, false, false);
					}
				}
			}
			if (IsValid(Squad) && IsValid(Pawns[0]))
			{
				Squad->OnActiveCharacterChanged.RemoveDynamic(Pawns[0], &AGGYGOSquadSwitchTestPawn::ObserveActiveCharacterForTest);
				Squad->DestroySquad(); // Borrowed fixture Actors remain this fixture's responsibility.
			}
			for (int32 Index = 0; Index < 2; ++Index)
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

		bool Initialize(FAutomationTestBase& Test)
		{
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
			for (int32 Index = 0; Index < 2; ++Index)
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
			Pawns[0]->ResetControlObservationsForTest();
			Pawns[1]->ResetControlObservationsForTest();
			CompletionObserver = ASCs[0]->OnAbilityTerminationCompleted().AddLambda([this](const FGGYGOAbilityTerminationCompletedNotice& Notice)
			{
				if (Notice.HasCompletion())
				{
					++CompletionCalls;
					LastCompletedOriginal = Notice.GetOriginal().GetOriginalActivation();
					LastCompletedKind = Notice.GetOriginal().GetRequestKind();
				}
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

		bool PressAndActivate(FAutomationTestBase& Test, UGGYGOSquadSwitchTestAbility* Ability)
		{
			auto Bindings = CapturePressBindings();
			if (!Test.TestEqual(TEXT("One real Hero ability binding"), Bindings.Num(), 1)) { return false; }
			const FInputActionInstance Action(Input.GetAbilityAction());
			for (const auto& Binding : Bindings) { Binding->Execute(Action); }
			Input.GetController()->ConsumeInputForTest();
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

		bool CheckCurrentBindings(FAutomationTestBase& Test, const TCHAR* Stage = TEXT("before switch")) const
		{
			bool bCurrent = true;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const FGGYGOAvatarBindingContext OriginalContext = ASCs[Index]->GetAvatarBindingContext();
				EGGYGOAvatarBindingReason Reason;
				const EGGYGOAvatarBindingOutcome Outcome = ASCs[Index]->CheckAvatarBindingContext(OriginalContext, Reason);
				bCurrent &= Test.TestTrue(FString::Printf(TEXT("Slot %d complete binding is current %s (Outcome=%u Reason=%u)"),
					Index, Stage, static_cast<uint8>(Outcome), static_cast<uint8>(Reason)),
					Outcome == EGGYGOAvatarBindingOutcome::Succeeded && Reason == EGGYGOAvatarBindingReason::None);
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
	const auto OldContext = Fixture.ASCs[0]->GetAvatarBindingContext();
	bool bNestedRejected = false;
	Ability->ArmCleanupForTest(FSimpleDelegate::CreateLambda([&]()
	{
		bNestedRejected = !Fixture.Squad->SwitchToSlot(1);
		TestTrue(TEXT("Cancellation callback still sees the original control/index"), Fixture.Squad->GetActiveSlot() == Fixture.Slots[0]
			&& Fixture.Input.GetController()->GetPawn() == Fixture.Pawns[0]);
	}));
	if (!Fixture.CheckCurrentBindings(*this)) { return false; }
	if (!TestTrue(TEXT("Production double-Slot switch succeeds"), Fixture.Squad->SwitchToSlot(1))) { return false; }
	TestTrue(TEXT("Same synchronous request rejects cancellation reentry"), bNestedRejected);
	TestTrue(TEXT("Original GA really ended once through Cancel Completed"), !Ability->IsActive() && Ability->CleanupCalls == 1
		&& Ability->CleanedOriginal.HasSameActivation(Original) && Fixture.CompletionCalls == 1
		&& Fixture.LastCompletedOriginal.HasSameActivation(Original) && Fixture.LastCompletedKind == EGGYGOAbilityTerminationRequestKind::Cancel);
	TestTrue(TEXT("Committed index matches actual reciprocal native Possess"), Fixture.Squad->GetActiveSlot() == Fixture.Slots[1]
		&& Fixture.Input.GetController()->GetPawn() == Fixture.Pawns[1] && Fixture.Pawns[1]->GetController() == Fixture.Input.GetController()
		&& !Fixture.Pawns[0]->GetController());
	TestTrue(TEXT("Native control changed Pawn Owner without changing persistent Slot ownership"),
		Fixture.Pawns[0]->GetOwner() == nullptr && Fixture.Pawns[1]->GetOwner() == Fixture.Input.GetController()
		&& Fixture.Slots[0]->GetOwner() == Fixture.Input.GetController() && Fixture.Slots[1]->GetOwner() == Fixture.Input.GetController());
	TestTrue(TEXT("Native UnPossess refreshed the original binding without replacing it"),
		Fixture.ASCs[0]->GetAvatarBindingContext().Binding.HasSameIdentity(OldContext.Binding)
		&& !Fixture.ASCs[0]->GetAvatarBindingContext().LastActorInfoWrite.HasSameIdentity(OldContext.LastActorInfoWrite));
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
	auto NewBindings = Fixture.CapturePressBindings(1);
	if (!TestEqual(TEXT("Native new Pawn setup built its real Hero input binding"), NewBindings.Num(), 1)) { return false; }
	const FInputActionInstance NewAction(Fixture.Input.GetAbilityAction());
	for (const auto& Binding : NewBindings) { Binding->Execute(NewAction); }
	Fixture.Input.GetController()->ConsumeInputForTest();
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
	const auto Activation = Fixture.ASCs[0]->TryActivateAbilityWithTerminationBoundary(BlockingHandle);
	if (!TestTrue(TEXT("Second real Cancel-policy GA activated"), Activation.bNativeAccepted && Blocking->IsActive())) { return false; }
	Blocking->SetCanBeCanceledForTest(false);
	if (!TestFalse(TEXT("Native blocker really cannot be cancelled"), Blocking->CanBeCanceled())) { return false; }
	const auto OriginalInput = Fixture.Pawns[0]->GetInputComponentForTest();
	const int32 BindingCount = OriginalInput->GetActionEventBindings().Num();
	const auto OldContext = Fixture.ASCs[0]->GetAvatarBindingContext();
	const auto NewContext = Fixture.ASCs[1]->GetAvatarBindingContext();
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
	const auto IsOriginalCallerCurrent = [&]()
	{
		APlayerController* PC = Fixture.Input.GetController();
		if (PC->GetPlayerState<AGGYGOPlayerState>() != Fixture.PlayerState || Fixture.PlayerState->GetOwner() != PC
			|| Fixture.Squad->GetActiveSlot() != Fixture.Slots[0] || PC->GetPawn() != Fixture.Pawns[0]
			|| Fixture.Pawns[0]->GetController() != PC || Fixture.Pawns[1]->GetController()
			|| Fixture.Pawns[0]->GetInputComponentForTest() != OriginalInput) { return false; }
		for (int32 Index = 0; Index < 2; ++Index)
		{
			EGGYGOAvatarBindingReason Reason;
			const FGGYGOAvatarBindingContext& Expected = Index == 0 ? OldContext : NewContext;
			if (Fixture.ASCs[Index]->CheckAvatarBindingContext(Expected, Reason) != EGGYGOAvatarBindingOutcome::Succeeded
				|| Fixture.Squad->GetSlot(Index) != Fixture.Slots[Index]
				|| Fixture.Slots[Index]->GetAvatarPawn() != Fixture.Pawns[Index]
				|| Fixture.Slots[Index]->GetGGYGOAbilitySystemComponent() != Fixture.ASCs[Index]
				|| Fixture.Slots[Index]->GetPawnData() != Fixture.Data.Get()
				|| Fixture.Pawns[Index]->GetPawnExtensionComponent()->GetPawnData<UGGYGOPawnData>() != Fixture.Data.Get()
				|| Fixture.Pawns[Index]->GetPawnExtensionComponent()->GetGGYGOAbilitySystemComponent() != Fixture.ASCs[Index]) { return false; }
		}
		return true;
	};
	TestTrue(TEXT("Shared exit caller authenticates real original control and published Ready"), IsOriginalCallerCurrent());
	const auto Denial = Fixture.ASCs[0]->TryExitAbilitiesForAvatarSwitch(OldContext, IsOriginalCallerCurrent);
	TestEqual(TEXT("Shared exit rejects the whole Cancel set before effects"), Denial.Outcome, EGGYGOAvatarSwitchAbilityExitOutcome::Rejected);
	TestEqual(TEXT("Shared exit rejection comes from ability preflight"), Denial.Reason, EGGYGOAvatarSwitchAbilityExitReason::AbilityNotReady);
	TestEqual(TEXT("Shared exit identifies the real NotCancelable blocker"), Denial.TerminationReason, EGGYGOAbilityTerminationReason::NotCancelable);
	TestTrue(TEXT("Shared exit reports the exact uncancelable native Spec"), Denial.BlockingSpec == BlockingHandle);
	TestEqual(TEXT("Shared exit did not reject original binding proof"), Denial.BindingReason, EGGYGOAvatarBindingReason::None);
	TestTrue(TEXT("Original caller remains current after side-effect-free preflight rejection"), IsOriginalCallerCurrent());
	TestFalse(TEXT("Production switch rejects the entire pending Cancel set"), Fixture.Squad->SwitchToSlot(1));
	TestTrue(TEXT("Production rejection preserves original complete bindings and published Ready"), IsOriginalCallerCurrent());
	TestTrue(TEXT("No partial cancellation or completion"), Cancel->IsActive() && Blocking->IsActive()
		&& Cancel->CleanupCalls == 0 && Blocking->CleanupCalls == 0 && Fixture.CompletionCalls == 0);
	TestTrue(TEXT("Control and authoritative active Slot unchanged"), Fixture.Squad->GetActiveSlot() == Fixture.Slots[0]
		&& Fixture.Input.GetController()->GetPawn() == Fixture.Pawns[0] && Fixture.Pawns[0]->GetController() == Fixture.Input.GetController()
		&& Fixture.Pawns[0]->UnpossessCalls == 0 && Fixture.Pawns[1]->PossessCalls == 0 && Fixture.Pawns[0]->ActiveCharacterNotices == 0);
	TestTrue(TEXT("Input identity/component and held state not retired"), Fixture.Pawns[0]->GetInputComponentForTest() == OriginalInput
		&& OriginalInput->GetActionEventBindings().Num() == BindingCount
		&& Fixture.ASCs[0]->FindAbilitySpecFromHandle(CancelHandle)->InputPressed);
	const GGYGOSquadSwitchTests::FPresentationSnapshot OldAfter(Fixture.Pawns[0]);
	const GGYGOSquadSwitchTests::FPresentationSnapshot NewAfter(Fixture.Pawns[1]);
	TestTrue(FString::Printf(TEXT("Old presentation exactly unchanged after rejection (before %s; after %s)"),
		*OldPresentation.Describe(), *OldAfter.Describe()), OldAfter.HasSamePresentation(OldPresentation));
	TestTrue(FString::Printf(TEXT("Standby presentation exactly unchanged after rejection (before %s; after %s)"),
		*NewPresentation.Describe(), *NewAfter.Describe()), NewAfter.HasSamePresentation(NewPresentation));
	TestTrue(TEXT("No ActorInfo refresh before rejection"), Fixture.ASCs[0]->GetAvatarBindingContext().LastActorInfoWrite.HasSameIdentity(OldContext.LastActorInfoWrite));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOSquadSwitchContinueSmoke,
	"GGYGO.Teams.SquadSwitch.UncancelableContinueEndsWithOriginal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOSquadSwitchContinueSmoke::RunTest(const FString& Parameters)
{
	GGYGOSquadSwitchTests::FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGameplayAbilitySpecHandle OldHandle, NewHandle;
	UGGYGOSquadSwitchTestAbility* Ability = Fixture.Grant(*this, 0, UGGYGOSquadSwitchContinueTestAbility::StaticClass(), true, OldHandle);
	UGGYGOSquadSwitchTestAbility* NewAbility = Fixture.Grant(*this, 1, UGGYGOSquadSwitchTestAbility::StaticClass(), true, NewHandle);
	if (!Ability || !NewAbility || !Fixture.PressAndActivate(*this, Ability)) { return false; }
	Ability->SetCanBeCanceledForTest(false);
	if (!TestFalse(TEXT("Explicit Continue is a real uncancelable native instance"), Ability->CanBeCanceled())) { return false; }
	const FGGYGOAbilityActivationHandle Original = Ability->ActivatedOriginal;
	const auto OldContext = Fixture.ASCs[0]->GetAvatarBindingContext();
	auto OldPress = Fixture.CapturePressBindings();
	if (!Fixture.CheckCurrentBindings(*this)) { return false; }
	if (!TestTrue(TEXT("Explicit uncancelable Continue does not block switch"), Fixture.Squad->SwitchToSlot(1))) { return false; }
	TestTrue(TEXT("Continue switch preserves native unowned old Pawn and persistent Slot ownership"),
		Fixture.Pawns[0]->GetOwner() == nullptr && Fixture.Pawns[1]->GetOwner() == Fixture.Input.GetController()
		&& Fixture.Slots[0]->GetOwner() == Fixture.Input.GetController() && Fixture.Slots[1]->GetOwner() == Fixture.Input.GetController());
	Fixture.CheckCurrentBindings(*this, TEXT("after native control transfer with Continue"));
	TestTrue(TEXT("Continue did not receive Cancel or Completed"), Ability->IsActive() && Ability->CleanupCalls == 0 && Fixture.CompletionCalls == 0);
	TestTrue(TEXT("Legal UnPossess refreshed same binding write"), Fixture.ASCs[0]->GetAvatarBindingContext().Binding.HasSameIdentity(OldContext.Binding)
		&& !Fixture.ASCs[0]->GetAvatarBindingContext().LastActorInfoWrite.HasSameIdentity(OldContext.LastActorInfoWrite));
	TestFalse(TEXT("Original activation did not renew resource-work qualification"), Ability->CaptureCurrentActivation().HasActivation());
	const FInputActionInstance OldAction(Fixture.Input.GetAbilityAction());
	for (const auto& Binding : OldPress) { Binding->Execute(OldAction); }
	Fixture.ASCs[0]->ProcessAbilityInput(0.0f, false);
	Fixture.Input.GetController()->ConsumeInputForTest();
	TestTrue(TEXT("Retired old binding gives neither old input nor successor permission"),
		!Fixture.ASCs[0]->FindAbilitySpecFromHandle(OldHandle)->InputPressed
		&& !Fixture.ASCs[1]->FindAbilitySpecFromHandle(NewHandle)->InputPressed
		&& Ability->ActivationCalls == 1 && NewAbility->ActivationCalls == 0);
	const auto End = Ability->RequestAbilityEnd(Original, false, false);
	TestEqual(TEXT("Same original GA normal End truly Completed after control refresh"), End.Outcome, EGGYGOAbilityTerminationOutcome::Completed);
	TestTrue(TEXT("Normal End retains original identity and completes once"), !Ability->IsActive() && Ability->CleanupCalls == 1
		&& Ability->CleanedOriginal.HasSameActivation(Original) && Fixture.CompletionCalls == 1
		&& Fixture.LastCompletedOriginal.HasSameActivation(Original) && Fixture.LastCompletedKind == EGGYGOAbilityTerminationRequestKind::End);
	TestTrue(TEXT("New reciprocal control remains intact after background End"), Fixture.Squad->GetActiveSlot() == Fixture.Slots[1]
		&& Fixture.Input.GetController()->GetPawn() == Fixture.Pawns[1] && Fixture.Pawns[1]->GetController() == Fixture.Input.GetController());
	return true;
}
#endif
