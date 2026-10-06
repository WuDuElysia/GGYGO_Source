#include "AbilitySystem/Tests/GGYGOHealthMessageTestTypes.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "Messages/GGYGOVerbMessage.h"
#include "Misc/ScopeExit.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHealthMessageTestTypes)

AGGYGOHealthMessageTestActor::AGGYGOHealthMessageTestActor()
{
	AbilitySystemComponent = CreateDefaultSubobject<UGGYGOAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
}

UAbilitySystemComponent* AGGYGOHealthMessageTestActor::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

UWorld* UGGYGOHealthMessageRepNotifyTestSet::GetWorld() const
{
	return bOverrideMessageWorld ? MessageWorldOverride.Get() : Super::GetWorld();
}

void UGGYGOHealthMessageRepNotifyTestSet::SetMessageWorldOverride(UWorld* World)
{
	bOverrideMessageWorld = true;
	MessageWorldOverride = World;
}

void UGGYGOHealthMessageRepNotifyTestSet::ClearMessageWorldOverride()
{
	bOverrideMessageWorld = false;
	MessageWorldOverride.Reset();
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
	struct FGGYGOHealthMessageTestModifier
	{
		FGameplayAttribute Attribute;
		TEnumAsByte<EGameplayModOp::Type> Operation = EGameplayModOp::Additive;
		float Magnitude = 0.0f;
	};

	struct FGGYGOHealthMessageAttributeRecord
	{
		AActor* Instigator = nullptr;
		AActor* Causer = nullptr;
		bool bHasEffectSpec = false;
		float Magnitude = 0.0f;
		float OldValue = 0.0f;
		float NewValue = 0.0f;
		FGameplayTagContainer SourceTags;
	};

	struct FGGYGOHealthMessageMessageRecord
	{
		FGGYGOVerbMessage Message;
		float DamageMetaAtBroadcast = 0.0f;
		float HealingMetaAtBroadcast = 0.0f;
		float PoiseDamageMetaAtBroadcast = 0.0f;
	};

	struct FGGYGOHealthMessageCombatant
	{
		AGGYGOHealthMessageTestActor* Actor = nullptr;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		UGGYGOHealthMessageRepNotifyTestSet* HealthSet = nullptr;
 	};

	/** Owns an initialized GI world so message routing and GAS world lookups use valid context. */
	struct FGGYGOHealthMessageTestFixture
	{
		UEngine* Engine = nullptr;
		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		UGameplayMessageSubsystem* MessageRouter = nullptr;
		TArray<AActor*> Actors;
		TArray<UGameplayEffect*> RootedEffects;
		TArray<FGameplayMessageListenerHandle> MessageHandles;
		TArray<TFunction<void()>> DelegateCleanup;

		bool Initialize()
		{
			Engine = GEngine;
			if (!Engine)
			{
				return false;
			}

			GameInstance = NewObject<UGameInstance>(Engine);
			if (!GameInstance)
			{
				return false;
			}

			GameInstance->InitializeStandalone(FName(TEXT("GGYGOHealthMessageTestWorld")));
			World = GameInstance->GetWorld();
			if (!World || World->GetGameInstance() != GameInstance)
			{
				return false;
			}

			MessageRouter = UGameInstance::GetSubsystem<UGameplayMessageSubsystem>(GameInstance);
			return MessageRouter != nullptr;
		}

		FGGYGOHealthMessageCombatant CreateCombatant()
		{
			FGGYGOHealthMessageCombatant Result;
			if (!World)
			{
				return Result;
			}

			Result.Actor = World->SpawnActor<AGGYGOHealthMessageTestActor>();
			if (!Result.Actor)
			{
				return Result;
			}
			Actors.Add(Result.Actor);

			Result.ASC = Result.Actor->GetProjectAbilitySystemComponent();
			if (!Result.ASC)
			{
				return Result;
			}
			Result.ASC->InitAbilityActorInfo(Result.Actor, Result.Actor);

			// AttributeSet::GetOwningActor expects its Outer to be the ASC owner actor.
			Result.HealthSet = NewObject<UGGYGOHealthMessageRepNotifyTestSet>(Result.Actor);
			if (!Result.HealthSet)
			{
				return Result;
			}
			Result.ASC->AddAttributeSetSubobject(Result.HealthSet);
			return Result;
		}

		AActor* CreateCauser()
		{
			AActor* Actor = World ? World->SpawnActor<AActor>() : nullptr;
			if (Actor)
			{
				Actors.Add(Actor);
			}
			return Actor;
		}

		UGameplayEffect* CreateEffect(const TArray<FGGYGOHealthMessageTestModifier>& Modifiers,
			EGameplayEffectDurationType DurationPolicy = EGameplayEffectDurationType::Instant)
		{
			UGameplayEffect* Effect = NewObject<UGameplayEffect>(GameInstance);
			if (!Effect)
			{
				return nullptr;
			}

			Effect->AddToRoot();
			RootedEffects.Add(Effect);
			Effect->DurationPolicy = DurationPolicy;
			for (const FGGYGOHealthMessageTestModifier& SourceModifier : Modifiers)
			{
				FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
				Modifier.Attribute = SourceModifier.Attribute;
				Modifier.ModifierOp = SourceModifier.Operation;
				Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(SourceModifier.Magnitude));
			}
			return Effect;
		}

		void ApplyEffect(const FGGYGOHealthMessageCombatant& Source, const FGGYGOHealthMessageCombatant& Target,
			UGameplayEffect* Effect, AActor* Causer, FGameplayTag DynamicAssetTag = FGameplayTag())
		{
			if (!Source.ASC || !Target.ASC || !Effect)
			{
				return;
			}

			FGameplayEffectContextHandle Context = Source.ASC->MakeEffectContext();
			Context.AddInstigator(Source.Actor, Causer ? Causer : Source.Actor);
			FGameplayEffectSpec Spec(Effect, Context, 1.0f);
			if (DynamicAssetTag.IsValid())
			{
				Spec.AddDynamicAssetTag(DynamicAssetTag);
			}
			Target.ASC->ApplyGameplayEffectSpecToSelf(Spec);
		}

		void ApplyModifier(const FGGYGOHealthMessageCombatant& Source, const FGGYGOHealthMessageCombatant& Target,
			FGameplayAttribute Attribute, EGameplayModOp::Type Operation, float Magnitude, AActor* Causer,
			FGameplayTag DynamicAssetTag = FGameplayTag(),
			EGameplayEffectDurationType DurationPolicy = EGameplayEffectDurationType::Instant)
		{
			TArray<FGGYGOHealthMessageTestModifier> Modifiers;
			Modifiers.Add({ Attribute, Operation, Magnitude });
			ApplyEffect(Source, Target, CreateEffect(Modifiers, DurationPolicy), Causer, DynamicAssetTag);
		}

		void ApplyDamage(const FGGYGOHealthMessageCombatant& Source, const FGGYGOHealthMessageCombatant& Target,
			float Magnitude, AActor* Causer)
		{
			TArray<FGGYGOHealthMessageTestModifier> Modifiers;
			Modifiers.Add({ UGGYGOHealthSet::GetDamageAttribute(), EGameplayModOp::Additive, Magnitude });
			ApplyEffect(Source, Target, CreateEffect(Modifiers), Causer);
		}

		void ApplyHealing(const FGGYGOHealthMessageCombatant& Source, const FGGYGOHealthMessageCombatant& Target,
			float Magnitude, AActor* Causer)
		{
			TArray<FGGYGOHealthMessageTestModifier> Modifiers;
			Modifiers.Add({ UGGYGOHealthSet::GetHealingAttribute(), EGameplayModOp::Additive, Magnitude });
			ApplyEffect(Source, Target, CreateEffect(Modifiers), Causer);
		}

		void ApplyPoiseDamage(const FGGYGOHealthMessageCombatant& Source, const FGGYGOHealthMessageCombatant& Target,
			float Magnitude, AActor* Causer)
		{
			TArray<FGGYGOHealthMessageTestModifier> Modifiers;
			Modifiers.Add({ UGGYGOHealthSet::GetPoiseDamageAttribute(), EGameplayModOp::Additive, Magnitude });
			ApplyEffect(Source, Target, CreateEffect(Modifiers), Causer);
		}

		void ApplyDirectPoise(const FGGYGOHealthMessageCombatant& Source, const FGGYGOHealthMessageCombatant& Target,
			EGameplayModOp::Type Operation, float Magnitude, AActor* Causer)
		{
			ApplyModifier(Source, Target, UGGYGOHealthSet::GetPoiseAttribute(), Operation, Magnitude, Causer);
		}

		FGameplayMessageListenerHandle Listen(FGameplayTag Channel,
			TFunction<void(FGameplayTag, const FGGYGOVerbMessage&)>&& Callback)
		{
			if (!MessageRouter)
			{
				return FGameplayMessageListenerHandle();
			}
			FGameplayMessageListenerHandle Handle = MessageRouter->RegisterListener<FGGYGOVerbMessage>(
				Channel, MoveTemp(Callback));
			MessageHandles.Add(Handle);
			return Handle;
		}

		void TrackNativeDelegate(UGGYGOAbilitySystemComponent* ASC, const FGameplayAttribute& Attribute,
			FDelegateHandle Handle)
		{
			DelegateCleanup.Add([ASC, Attribute, Handle]()
			{
				if (ASC && Handle.IsValid())
				{
					ASC->GetGameplayAttributeValueChangeDelegate(Attribute).Remove(Handle);
				}
			});
		}

		void TrackAttributeDelegate(FGGYGOAttributeEvent& Event, FDelegateHandle Handle)
		{
			FGGYGOAttributeEvent* EventPtr = &Event;
			DelegateCleanup.Add([EventPtr, Handle]()
			{
				if (EventPtr && Handle.IsValid())
				{
					EventPtr->Remove(Handle);
				}
			});
		}

		~FGGYGOHealthMessageTestFixture()
		{
			for (FGameplayMessageListenerHandle& Handle : MessageHandles)
			{
				Handle.Unregister();
			}
			for (int32 Index = DelegateCleanup.Num() - 1; Index >= 0; --Index)
			{
				DelegateCleanup[Index]();
			}
			for (UGameplayEffect* Effect : RootedEffects)
			{
				if (Effect && Effect->IsRooted())
				{
					Effect->RemoveFromRoot();
				}
			}
			for (int32 Index = Actors.Num() - 1; Index >= 0; --Index)
			{
				if (World && Actors[Index])
				{
					World->DestroyActor(Actors[Index]);
				}
			}

			if (GameInstance)
			{
				GameInstance->Shutdown();
			}
			if (World)
			{
				World->DestroyWorld(false);
				if (Engine)
				{
					Engine->DestroyWorldContext(World);
				}
				if (UPackage* WorldPackage = World->GetPackage())
				{
					WorldPackage->SetDirtyFlag(false);
				}
			}
			if (GameInstance)
			{
				GameInstance->MarkAsGarbage();
			}
		}
	};

	FGGYGOHealthMessageAttributeRecord MakeAttributeRecord(AActor* Instigator, AActor* Causer,
		const FGameplayEffectSpec* EffectSpec, float Magnitude, float OldValue, float NewValue)
	{
		FGGYGOHealthMessageAttributeRecord Record;
		Record.Instigator = Instigator;
		Record.Causer = Causer;
		Record.bHasEffectSpec = EffectSpec != nullptr;
		Record.Magnitude = Magnitude;
		Record.OldValue = OldValue;
		Record.NewValue = NewValue;
		if (EffectSpec)
		{
			if (const FGameplayTagContainer* SourceTags = EffectSpec->CapturedSourceTags.GetAggregatedTags())
			{
				Record.SourceTags = *SourceTags;
			}
		}
		return Record;
	}

	FGameplayTag GetOnlySourceTag(const FGGYGOHealthMessageAttributeRecord& Record,
		FGameplayTag First, FGameplayTag Second)
	{
		if (Record.SourceTags.HasTagExact(First) && !Record.SourceTags.HasTagExact(Second)) { return First; }
		if (Record.SourceTags.HasTagExact(Second) && !Record.SourceTags.HasTagExact(First)) { return Second; }
		return FGameplayTag();
	}

	FGameplayTag GetOnlySourceTag(const FGGYGOHealthMessageMessageRecord& Record,
		FGameplayTag First, FGameplayTag Second)
	{
		if (Record.Message.InstigatorTags.HasTagExact(First) && !Record.Message.InstigatorTags.HasTagExact(Second)) { return First; }
		if (Record.Message.InstigatorTags.HasTagExact(Second) && !Record.Message.InstigatorTags.HasTagExact(First)) { return Second; }
		return FGameplayTag();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageNativeDamageReentryTest,
	"GGYGO.AbilitySystem.HealthMessage.NativeDamageMetaReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageNativeDamageReentryTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("独立 GameInstance、World 与消息路由初始化"), Fixture.Initialize())) { return false; }

	const FGGYGOHealthMessageCombatant OuterSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant InnerSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	AActor* OuterCauser = Fixture.CreateCauser();
	AActor* InnerCauser = Fixture.CreateCauser();
	if (!TestNotNull(TEXT("外层来源 ASC"), OuterSource.ASC)
		|| !TestNotNull(TEXT("内层来源 ASC"), InnerSource.ASC)
		|| !TestNotNull(TEXT("目标 HealthSet"), Target.HealthSet)
		|| !TestNotNull(TEXT("外层真实 EffectCauser"), OuterCauser)
		|| !TestNotNull(TEXT("内层真实 EffectCauser"), InnerCauser)) { return false; }

	const FGameplayTag OuterSourceTag = GGYGOGameplayTags::Gameplay_Damage;
	const FGameplayTag InnerSourceTag = GGYGOGameplayTags::Gameplay_PoiseDamage;
	OuterSource.ASC->AddLooseGameplayTag(OuterSourceTag);
	InnerSource.ASC->AddLooseGameplayTag(InnerSourceTag);
	Target.HealthSet->InitHealth(100.0f);

	TArray<FGGYGOHealthMessageAttributeRecord> HealthEvents;
	const FDelegateHandle HealthChangedHandle = Target.HealthSet->OnHealthChanged.AddLambda(
		[&HealthEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			HealthEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged, HealthChangedHandle);

	TArray<FGGYGOHealthMessageMessageRecord> DamageMessages;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage,
		[&DamageMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			DamageMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
		Target.HealthSet->GetPoiseDamage() });
		});

	bool bAppliedInnerDamage = false;
	const FDelegateHandle DamageMetaHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(
		UGGYGOHealthSet::GetDamageAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bAppliedInnerDamage && Data.NewValue > 0.0f)
			{
				bAppliedInnerDamage = true;
				Fixture.ApplyDamage(InnerSource, Target, 20.0f, InnerCauser);
			}
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetDamageAttribute(), DamageMetaHandle);

	Fixture.ApplyDamage(OuterSource, Target, 10.0f, OuterCauser);

	TestTrue(TEXT("Damage 元属性回调真实应用内层 GE"), bAppliedInnerDamage);
	TestEqual(TEXT("内外两次 Modifier 累计后生命为 70"), Target.HealthSet->GetHealth(), 70.0f);
	TestEqual(TEXT("Damage meta 在最外 GE 完成时消费清零"), Target.HealthSet->GetDamage(), 0.0f);
	TestEqual(TEXT("Healing meta 保持清零"), Target.HealthSet->GetHealing(), 0.0f);
	TestEqual(TEXT("PoiseDamage meta 保持清零"), Target.HealthSet->GetPoiseDamage(), 0.0f);
	TestEqual(TEXT("每个独立 Damage Modifier 各广播一条属性结果"), HealthEvents.Num(), 2);
	TestEqual(TEXT("每个独立 Damage Modifier 各广播一条 Damage 消息"), DamageMessages.Num(), 2);

	int32 OuterHealthEventCount = 0;
	int32 InnerHealthEventCount = 0;
	for (const FGGYGOHealthMessageAttributeRecord& Event : HealthEvents)
	{
		if (Event.Causer == InnerCauser)
		{
			++InnerHealthEventCount;
			TestTrue(TEXT("内层 Health 事件携带真实内层 Instigator"), Event.Instigator == InnerSource.Actor);
			TestTrue(TEXT("内层 Health 事件保留 EffectSpec"), Event.bHasEffectSpec);
			TestEqual(TEXT("内层 Health 事件保留原始幅度 20"), Event.Magnitude, 20.0f);
			TestEqual(TEXT("内层 Health 事件记录真实旧值"), Event.OldValue, 100.0f);
			TestEqual(TEXT("内层 Health 事件记录真实新值"), Event.NewValue, 80.0f);
			TestTrue(TEXT("内层 Health 事件只带内层来源 Tag"), GetOnlySourceTag(Event, OuterSourceTag, InnerSourceTag) == InnerSourceTag);
		}
		else if (Event.Causer == OuterCauser)
		{
			++OuterHealthEventCount;
			TestTrue(TEXT("外层 Health 事件携带真实外层 Instigator"), Event.Instigator == OuterSource.Actor);
			TestTrue(TEXT("外层 Health 事件保留 EffectSpec"), Event.bHasEffectSpec);
			TestEqual(TEXT("外层 Health 事件保留原始幅度 10"), Event.Magnitude, 10.0f);
			TestEqual(TEXT("外层 Health 事件基于内层结算后的真实旧值"), Event.OldValue, 80.0f);
			TestEqual(TEXT("外层 Health 事件记录真实新值"), Event.NewValue, 70.0f);
			TestTrue(TEXT("外层 Health 事件只带外层来源 Tag"), GetOnlySourceTag(Event, OuterSourceTag, InnerSourceTag) == OuterSourceTag);
		}
		else
		{
			AddError(TEXT("Health 事件带有未知的 Causer"));
		}
	}
	TestEqual(TEXT("外层来源恰好对应一条 Health 事件"), OuterHealthEventCount, 1);
	TestEqual(TEXT("内层来源恰好对应一条 Health 事件"), InnerHealthEventCount, 1);

	int32 OuterMessageCount = 0;
	int32 InnerMessageCount = 0;
	for (const FGGYGOHealthMessageMessageRecord& Record : DamageMessages)
	{
		const FGGYGOVerbMessage& Message = Record.Message;
		TestTrue(TEXT("Damage 消息 Target 为 HealthSet 所属宿主"), Message.Target == Target.Actor);
		TestEqual(TEXT("Damage 消息期间 meta Damage 已清零"), Record.DamageMetaAtBroadcast, 0.0f);
		TestEqual(TEXT("Damage 消息期间 meta Healing 已清零"), Record.HealingMetaAtBroadcast, 0.0f);
		TestEqual(TEXT("Damage 消息期间 meta PoiseDamage 已清零"), Record.PoiseDamageMetaAtBroadcast, 0.0f);
		if (Message.Instigator == OuterCauser)
		{
			++OuterMessageCount;
			TestEqual(TEXT("外层 Damage 消息保留原始幅度 10"), Message.Magnitude, 10.0f);
			TestTrue(TEXT("外层 Damage 消息只保留外层源 Tag"), GetOnlySourceTag(Record, OuterSourceTag, InnerSourceTag) == OuterSourceTag);
		}
		else if (Message.Instigator == InnerCauser)
		{
			++InnerMessageCount;
			TestEqual(TEXT("内层 Damage 消息保留原始幅度 20"), Message.Magnitude, 20.0f);
			TestTrue(TEXT("内层 Damage 消息只保留内层源 Tag"), GetOnlySourceTag(Record, OuterSourceTag, InnerSourceTag) == InnerSourceTag);
		}
		else
		{
			AddError(TEXT("Damage 消息带有未知的 EffectCauser"));
		}
	}
	TestEqual(TEXT("外层来源恰好对应一条 Damage 消息"), OuterMessageCount, 1);
	TestEqual(TEXT("内层来源恰好对应一条 Damage 消息"), InnerMessageCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessagePoiseEdgesTest,
	"GGYGO.AbilitySystem.HealthMessage.PoiseEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessagePoiseEdgesTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("独立 GameInstance、World 与消息路由初始化"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant MetaSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant DirectSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	AActor* MetaCauser = Fixture.CreateCauser();
	AActor* DirectCauser = Fixture.CreateCauser();
	if (!TestNotNull(TEXT("削韧来源"), MetaSource.ASC)
		|| !TestNotNull(TEXT("直接韧性来源"), DirectSource.ASC)
		|| !TestNotNull(TEXT("目标 HealthSet"), Target.HealthSet)
		|| !TestNotNull(TEXT("削韧 EffectCauser"), MetaCauser)
		|| !TestNotNull(TEXT("直接属性 EffectCauser"), DirectCauser)) { return false; }

	Target.HealthSet->InitPoise(10.0f);
	const FGameplayTag MetaSourceTag = GGYGOGameplayTags::Gameplay_PoiseDamage;
	const FGameplayTag DirectSourceTag = GGYGOGameplayTags::Gameplay_Damage;
	MetaSource.ASC->AddLooseGameplayTag(MetaSourceTag);
	DirectSource.ASC->AddLooseGameplayTag(DirectSourceTag);
	TArray<FGGYGOHealthMessageAttributeRecord> PoiseEvents;
	TArray<FGGYGOHealthMessageAttributeRecord> BreakEvents;
	const FDelegateHandle PoiseChangedHandle = Target.HealthSet->OnPoiseChanged.AddLambda(
		[&PoiseEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			PoiseEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseChanged, PoiseChangedHandle);
	const FDelegateHandle PoiseBrokenHandle = Target.HealthSet->OnPoiseBroken.AddLambda(
		[&BreakEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			BreakEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseBroken, PoiseBrokenHandle);
	TArray<FGGYGOHealthMessageMessageRecord> BreakMessages;
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak,
		[&BreakMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			BreakMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});

	Fixture.ApplyPoiseDamage(MetaSource, Target, 4.0f, MetaCauser);
	TestEqual(TEXT("普通削韧后韧性为 6"), Target.HealthSet->GetPoise(), 6.0f);
	TestEqual(TEXT("普通削韧不触发破韧边沿"), BreakEvents.Num(), 0);
	TestEqual(TEXT("普通削韧不发布 PoiseBreak 消息"), BreakMessages.Num(), 0);
	Fixture.ApplyPoiseDamage(MetaSource, Target, 6.0f, MetaCauser);
	TestEqual(TEXT("精确归零后韧性为 0"), Target.HealthSet->GetPoise(), 0.0f);
	TestEqual(TEXT("正值精确归零触发一次边沿"), BreakEvents.Num(), 1);
	TestEqual(TEXT("正值精确归零发布一次消息"), BreakMessages.Num(), 1);
	Fixture.ApplyPoiseDamage(MetaSource, Target, 3.0f, MetaCauser);
	TestEqual(TEXT("零值上的追加削韧仍保持 0"), Target.HealthSet->GetPoise(), 0.0f);
	TestEqual(TEXT("零值追加不重复破韧边沿"), BreakEvents.Num(), 1);
	TestEqual(TEXT("零值追加不发布破韧消息"), BreakMessages.Num(), 1);

	Fixture.ApplyDirectPoise(DirectSource, Target, EGameplayModOp::Additive, 5.0f, DirectCauser);
	TestEqual(TEXT("直接 GE 恢复韧性到 5"), Target.HealthSet->GetPoise(), 5.0f);
	Fixture.ApplyPoiseDamage(MetaSource, Target, 7.0f, MetaCauser);
	TestEqual(TEXT("过量削韧仍 Clamp 到 0"), Target.HealthSet->GetPoise(), 0.0f);
	TestEqual(TEXT("恢复后再次正值到零是新边沿"), BreakEvents.Num(), 2);
	TestEqual(TEXT("恢复后再次破韧发布新消息"), BreakMessages.Num(), 2);

	Fixture.ApplyDirectPoise(DirectSource, Target, EGameplayModOp::Additive, 10.0f, DirectCauser);
	Fixture.ApplyDirectPoise(DirectSource, Target, EGameplayModOp::Override, 0.0f, DirectCauser);
	TestEqual(TEXT("直接韧性 GE 清零"), Target.HealthSet->GetPoise(), 0.0f);
	TestEqual(TEXT("直接清零触发第三个正值到零边沿"), BreakEvents.Num(), 3);
	TestEqual(TEXT("直接清零发布第三条破韧消息"), BreakMessages.Num(), 3);
	TestEqual(TEXT("实际韧性变化各触发一次属性委托"), PoiseEvents.Num(), 6);
	TestEqual(TEXT("PoiseDamage meta 最终清零"), Target.HealthSet->GetPoiseDamage(), 0.0f);

	int32 MetaBreakCount = 0;
	int32 DirectBreakCount = 0;
	for (const FGGYGOHealthMessageAttributeRecord& Event : BreakEvents)
	{
		TestTrue(TEXT("PoiseBroken 事件确实由正值跨到零"), Event.OldValue > 0.0f && Event.NewValue == 0.0f);
		TestTrue(TEXT("PoiseBroken 保留真实 EffectSpec"), Event.bHasEffectSpec);
		if (Event.Causer == MetaCauser)
		{
			++MetaBreakCount;
			TestTrue(TEXT("meta 破韧事件 Instigator 是实际来源"), Event.Instigator == MetaSource.Actor);
			TestTrue(TEXT("meta 破韧事件保留原始 meta 幅度"), Event.Magnitude == 6.0f || Event.Magnitude == 7.0f);
		}
		else if (Event.Causer == DirectCauser)
		{
			++DirectBreakCount;
			TestTrue(TEXT("直接破韧事件 Instigator 是实际来源"), Event.Instigator == DirectSource.Actor);
			TestEqual(TEXT("直接 Override 0 的破韧边沿幅度是实际损失 10"), Event.Magnitude, 10.0f);
			TestTrue(TEXT("直接破韧事件只保留直接来源 Tag"), Event.SourceTags.HasTagExact(DirectSourceTag)
				&& !Event.SourceTags.HasTagExact(MetaSourceTag));
		}
		else
		{
			AddError(TEXT("PoiseBroken 事件带有未知来源"));
		}
	}
	TestEqual(TEXT("两次 meta 破韧事件各自保留来源"), MetaBreakCount, 2);
	TestEqual(TEXT("直接 GE 破韧事件保留来源"), DirectBreakCount, 1);

	int32 MetaMessageCount = 0;
	int32 DirectMessageCount = 0;
	for (const FGGYGOHealthMessageMessageRecord& Record : BreakMessages)
	{
		const FGGYGOVerbMessage& Message = Record.Message;
		TestTrue(TEXT("PoiseBreak 消息 Target 为 HealthSet 宿主"), Message.Target == Target.Actor);
		TestEqual(TEXT("PoiseBreak 广播时 Damage meta 已清零"), Record.DamageMetaAtBroadcast, 0.0f);
		TestEqual(TEXT("PoiseBreak 广播时 Healing meta 已清零"), Record.HealingMetaAtBroadcast, 0.0f);
		TestEqual(TEXT("PoiseBreak 广播时 PoiseDamage meta 已清零"), Record.PoiseDamageMetaAtBroadcast, 0.0f);
		if (Message.Instigator == MetaCauser)
		{
			++MetaMessageCount;
			TestTrue(TEXT("meta 破韧消息幅度是原始削韧量 6 或 7"), Message.Magnitude == 6.0f || Message.Magnitude == 7.0f);
		}
		else if (Message.Instigator == DirectCauser)
		{
			++DirectMessageCount;
			TestEqual(TEXT("直接 Poise GE 消息幅度是 Old-New 实际损失 10"), Message.Magnitude, 10.0f);
		}
		else
		{
			AddError(TEXT("PoiseBreak 消息带有未知 EffectCauser"));
		}
	}
	TestEqual(TEXT("两次 meta PoiseBreak 消息各自保留"), MetaMessageCount, 2);
	TestEqual(TEXT("直接清零的 PoiseBreak 消息各自保留"), DirectMessageCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageDamageImmunityHealingTest,
	"GGYGO.AbilitySystem.HealthMessage.DamageImmunityHealingAndSurvival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageDamageImmunityHealingTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("独立 GameInstance、World 与消息路由初始化"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant Source = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	AActor* Causer = Fixture.CreateCauser();
	if (!TestNotNull(TEXT("伤害来源 ASC"), Source.ASC)
		|| !TestNotNull(TEXT("目标 HealthSet"), Target.HealthSet)
		|| !TestNotNull(TEXT("真实 EffectCauser"), Causer)) { return false; }

	Target.HealthSet->InitHealth(25.0f);
	Target.HealthSet->InitPoise(20.0f);
	const FGameplayTag SourceTag = GGYGOGameplayTags::Gameplay_Damage;
	Source.ASC->AddLooseGameplayTag(SourceTag);
	TArray<FGGYGOHealthMessageMessageRecord> DamageMessages;
	TArray<FGGYGOHealthMessageMessageRecord> BreakMessages;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage,
		[&DamageMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			DamageMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak,
		[&BreakMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			BreakMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});
	TArray<FGGYGOHealthMessageAttributeRecord> DeathEvents;
	const FDelegateHandle OutOfHealthHandle = Target.HealthSet->OnOutOfHealth.AddLambda(
		[&DeathEvents](AActor* Instigator, AActor* EventCauser, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			DeathEvents.Add(MakeAttributeRecord(Instigator, EventCauser, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnOutOfHealth, OutOfHealthHandle);

	Fixture.ApplyDamage(Source, Target, 40.0f, Causer);
	TestEqual(TEXT("过量伤害 Clamp 生命到 0"), Target.HealthSet->GetHealth(), 0.0f);
	TestEqual(TEXT("过量伤害消息数为一"), DamageMessages.Num(), 1);
	if (DamageMessages.IsValidIndex(0))
	{
		TestEqual(TEXT("过量伤害保留原始 Damage 消息幅度"), DamageMessages[0].Message.Magnitude, 40.0f);
		TestTrue(TEXT("过量伤害消息保留真实 EffectCauser"), DamageMessages[0].Message.Instigator == Causer);
		TestTrue(TEXT("过量伤害消息保留真实 Source Tag"), DamageMessages[0].Message.InstigatorTags.HasTagExact(SourceTag));
	}
	TestEqual(TEXT("第一次死亡发布一个边沿"), DeathEvents.Num(), 1);
	Fixture.ApplyHealing(Source, Target, 12.0f, Causer);
	TestEqual(TEXT("治疗从 0 恢复到 12"), Target.HealthSet->GetHealth(), 12.0f);
	TestEqual(TEXT("治疗 meta 消费清零"), Target.HealthSet->GetHealing(), 0.0f);

	Target.ASC->AddLooseGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity);
	TArray<FGGYGOHealthMessageTestModifier> ImmuneHitModifiers;
	ImmuneHitModifiers.Add({ UGGYGOHealthSet::GetDamageAttribute(), EGameplayModOp::Additive, 10.0f });
	ImmuneHitModifiers.Add({ UGGYGOHealthSet::GetPoiseDamageAttribute(), EGameplayModOp::Additive, 8.0f });
	Fixture.ApplyEffect(Source, Target, Fixture.CreateEffect(ImmuneHitModifiers), Causer);
	TestEqual(TEXT("免疫阻断傷害后生命不变"), Target.HealthSet->GetHealth(), 12.0f);
	TestEqual(TEXT("免疫阻断削韧后韧性不变"), Target.HealthSet->GetPoise(), 20.0f);
	TestEqual(TEXT("免疫阻断不新增 Damage 消息"), DamageMessages.Num(), 1);
	TestEqual(TEXT("免疫阻断不新增 PoiseBreak 消息"), BreakMessages.Num(), 0);
	TestEqual(TEXT("免疫后 Damage meta 清零"), Target.HealthSet->GetDamage(), 0.0f);
	TestEqual(TEXT("免疫后 PoiseDamage meta 清零"), Target.HealthSet->GetPoiseDamage(), 0.0f);
	Target.ASC->RemoveLooseGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity);
	Target.ASC->AddLooseGameplayTag(GGYGOGameplayTags::Cheat_GodMode);
	Fixture.ApplyDamage(Source, Target, 99.0f, Causer);
	TestEqual(TEXT("GodMode 在 Pre 阶段免疫普通伤害"), Target.HealthSet->GetHealth(), 12.0f);
	TestEqual(TEXT("GodMode 阻断不发布 Damage 消息"), DamageMessages.Num(), 1);
	Target.ASC->RemoveLooseGameplayTag(GGYGOGameplayTags::Cheat_GodMode);

	Target.ASC->AddLooseGameplayTag(GGYGOGameplayTags::Cheat_UnlimitedHealth);
	Fixture.ApplyDamage(Source, Target, 100.0f, Causer);
	TestEqual(TEXT("UnlimitedHealth 将致死伤害保留 1 点生命"), Target.HealthSet->GetHealth(), 1.0f);
	TestEqual(TEXT("保命规则仍保留原始伤害消息"), DamageMessages.Num(), 2);
	TestEqual(TEXT("保命规则未触发死亡边沿"), DeathEvents.Num(), 1);
	if (DamageMessages.IsValidIndex(1))
	{
		TestEqual(TEXT("UnlimitedHealth 消息保留原始幅度 100"), DamageMessages[1].Message.Magnitude, 100.0f);
		TestTrue(TEXT("UnlimitedHealth 消息保留真实 EffectCauser"), DamageMessages[1].Message.Instigator == Causer);
		TestTrue(TEXT("UnlimitedHealth 消息保留真实 Source Tag"), DamageMessages[1].Message.InstigatorTags.HasTagExact(SourceTag));
	}

	Fixture.ApplyModifier(Source, Target, UGGYGOHealthSet::GetDamageAttribute(), EGameplayModOp::Additive,
		2.0f, Causer, GGYGOGameplayTags::Gameplay_Damage_SelfDestruct);
	TestEqual(TEXT("SelfDestruct 绕过保命规则並致死"), Target.HealthSet->GetHealth(), 0.0f);
	TestEqual(TEXT("自毁伤害仍发原始幅度消息"), DamageMessages.Num(), 3);
	TestEqual(TEXT("治疗后再次死亡形成第二个边沿"), DeathEvents.Num(), 2);
	if (DamageMessages.IsValidIndex(2))
	{
		TestEqual(TEXT("自毁 Damage 消息幅度仍为 2"), DamageMessages[2].Message.Magnitude, 2.0f);
		TestTrue(TEXT("自毁 Damage 消息保留真实 EffectCauser"), DamageMessages[2].Message.Instigator == Causer);
		TestTrue(TEXT("自毁 Damage 消息保留真实 Source Tag"), DamageMessages[2].Message.InstigatorTags.HasTagExact(SourceTag));
	}
	bool bAllDamageMessagesConsumedMeta = true;
	for (const FGGYGOHealthMessageMessageRecord& Record : DamageMessages)
	{
		bAllDamageMessagesConsumedMeta &= Record.DamageMetaAtBroadcast == 0.0f
			&& Record.HealingMetaAtBroadcast == 0.0f && Record.PoiseDamageMetaAtBroadcast == 0.0f;
	}
	TestTrue(TEXT("所有 Damage 消息在广播时 meta 均已消费"), bAllDamageMessagesConsumedMeta);
	bool bAllDeathEventsArePositiveToZero = true;
	for (const FGGYGOHealthMessageAttributeRecord& Event : DeathEvents)
	{
		bAllDeathEventsArePositiveToZero &= Event.OldValue > 0.0f && Event.NewValue == 0.0f && Event.bHasEffectSpec;
	}
	TestTrue(TEXT("每个死亡边沿都记录从正生命到零"), bAllDeathEventsArePositiveToZero);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageDamageMetaClearReentryTest,
	"GGYGO.AbilitySystem.HealthMessage.DamageMetaClearReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageDamageMetaClearReentryTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("独立 GameInstance、World 与消息路由初始化"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant OuterSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant InnerSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	AActor* OuterCauser = Fixture.CreateCauser();
	AActor* InnerCauser = Fixture.CreateCauser();
	if (!TestNotNull(TEXT("外层来源 ASC"), OuterSource.ASC)
		|| !TestNotNull(TEXT("内层来源 ASC"), InnerSource.ASC)
		|| !TestNotNull(TEXT("目标 HealthSet"), Target.HealthSet)
		|| !TestNotNull(TEXT("外层 EffectCauser"), OuterCauser)
		|| !TestNotNull(TEXT("内层 EffectCauser"), InnerCauser)) { return false; }

	const FGameplayTag OuterTag = GGYGOGameplayTags::Gameplay_Damage;
	const FGameplayTag InnerTag = GGYGOGameplayTags::Gameplay_PoiseDamage;
	OuterSource.ASC->AddLooseGameplayTag(OuterTag);
	InnerSource.ASC->AddLooseGameplayTag(InnerTag);
	Target.HealthSet->InitHealth(100.0f);
	TArray<FGGYGOHealthMessageAttributeRecord> HealthEvents;
	const FDelegateHandle HealthHandle = Target.HealthSet->OnHealthChanged.AddLambda(
		[&HealthEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			HealthEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged, HealthHandle);
	TArray<FGGYGOHealthMessageMessageRecord> DamageMessages;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage,
		[&DamageMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			DamageMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});

	bool bAppliedInnerDamage = false;
	const FDelegateHandle DamageMetaHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(
		UGGYGOHealthSet::GetDamageAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bAppliedInnerDamage && Data.OldValue > 0.0f && Data.NewValue == 0.0f)
			{
				bAppliedInnerDamage = true;
				Fixture.ApplyDamage(InnerSource, Target, 20.0f, InnerCauser);
			}
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetDamageAttribute(), DamageMetaHandle);

	Fixture.ApplyDamage(OuterSource, Target, 10.0f, OuterCauser);
	TestTrue(TEXT("Damage meta 清零 setter 回调真实应用内层 GE"), bAppliedInnerDamage);
	TestEqual(TEXT("清 meta 时内层先扣 20，外层再扣 10，最终生命为 70"), Target.HealthSet->GetHealth(), 70.0f);
	TestEqual(TEXT("外层和内层 Damage meta 最终都消费为 0"), Target.HealthSet->GetDamage(), 0.0f);
	TestEqual(TEXT("清零 setter 重入产生两条独立 Health 事件"), HealthEvents.Num(), 2);
	TestEqual(TEXT("清零 setter 重入产生两条独立 Damage 消息"), DamageMessages.Num(), 2);
	int32 OuterEvents = 0;
	int32 InnerEvents = 0;
	for (const FGGYGOHealthMessageAttributeRecord& Event : HealthEvents)
	{
		if (Event.Causer == OuterCauser)
		{
			++OuterEvents;
			TestTrue(TEXT("外层 Health 事件的 Instigator 正确"), Event.Instigator == OuterSource.Actor);
			TestEqual(TEXT("外层 Health 事件幅度 10"), Event.Magnitude, 10.0f);
			TestEqual(TEXT("外层 Health 事件基于内层扣血后的旧值 80"), Event.OldValue, 80.0f);
			TestEqual(TEXT("外层 Health 事件新值 70"), Event.NewValue, 70.0f);
			TestTrue(TEXT("外层 Health 事件只带外层来源 Tag"), GetOnlySourceTag(Event, OuterTag, InnerTag) == OuterTag);
		}
		else if (Event.Causer == InnerCauser)
		{
			++InnerEvents;
			TestTrue(TEXT("内层 Health 事件的 Instigator 正确"), Event.Instigator == InnerSource.Actor);
			TestEqual(TEXT("内层 Health 事件幅度 20"), Event.Magnitude, 20.0f);
			TestEqual(TEXT("内层 Health 事件旧值 100"), Event.OldValue, 100.0f);
			TestEqual(TEXT("内层 Health 事件新值 80"), Event.NewValue, 80.0f);
			TestTrue(TEXT("内层 Health 事件只带内层来源 Tag"), GetOnlySourceTag(Event, OuterTag, InnerTag) == InnerTag);
		}
		else { AddError(TEXT("Health 事件带有未知 Causer")); }
	}
	TestEqual(TEXT("外层来源事件恰好一条"), OuterEvents, 1);
	TestEqual(TEXT("内层来源事件恰好一条"), InnerEvents, 1);
	int32 OuterMessages = 0;
	int32 InnerMessages = 0;
	for (const FGGYGOHealthMessageMessageRecord& Record : DamageMessages)
	{
		TestEqual(TEXT("Damage 消息期间 meta 已消费"), Record.DamageMetaAtBroadcast, 0.0f);
		if (Record.Message.Instigator == OuterCauser)
		{
			++OuterMessages;
			TestEqual(TEXT("外层消息保留原始幅度 10"), Record.Message.Magnitude, 10.0f);
			TestTrue(TEXT("外层消息保留来源 Tag"), Record.Message.InstigatorTags.HasTagExact(OuterTag));
		}
		else if (Record.Message.Instigator == InnerCauser)
		{
			++InnerMessages;
			TestEqual(TEXT("内层消息保留原始幅度 20"), Record.Message.Magnitude, 20.0f);
			TestTrue(TEXT("内层消息保留来源 Tag"), Record.Message.InstigatorTags.HasTagExact(InnerTag));
		}
		else { AddError(TEXT("Damage 消息带有未知 EffectCauser")); }
	}
	TestEqual(TEXT("外层来源 Damage 消息恰好一条"), OuterMessages, 1);
	TestEqual(TEXT("内层来源 Damage 消息恰好一条"), InnerMessages, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageHealthSetterReentryTest,
	"GGYGO.AbilitySystem.HealthMessage.HealthSetterReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageHealthSetterReentryTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("独立 GameInstance、World 与消息路由初始化"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant OuterSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant InnerSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	AActor* OuterCauser = Fixture.CreateCauser();
	AActor* InnerCauser = Fixture.CreateCauser();
	if (!TestNotNull(TEXT("外层来源 ASC"), OuterSource.ASC)
		|| !TestNotNull(TEXT("内层来源 ASC"), InnerSource.ASC)
		|| !TestNotNull(TEXT("目标 HealthSet"), Target.HealthSet)
		|| !TestNotNull(TEXT("外层 EffectCauser"), OuterCauser)
		|| !TestNotNull(TEXT("内层 EffectCauser"), InnerCauser)) { return false; }

	const FGameplayTag OuterTag = GGYGOGameplayTags::Gameplay_Damage;
	const FGameplayTag InnerTag = GGYGOGameplayTags::Gameplay_PoiseDamage;
	OuterSource.ASC->AddLooseGameplayTag(OuterTag);
	InnerSource.ASC->AddLooseGameplayTag(InnerTag);
	Target.HealthSet->InitHealth(80.0f);
	TArray<FGGYGOHealthMessageAttributeRecord> HealthEvents;
	const FDelegateHandle HealthHandle = Target.HealthSet->OnHealthChanged.AddLambda(
		[&HealthEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			HealthEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged, HealthHandle);
	TArray<FGGYGOHealthMessageMessageRecord> DamageMessages;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage,
		[&DamageMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			DamageMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});

	bool bAppliedNestedHealing = false;
	const FDelegateHandle HealthNativeHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(
		UGGYGOHealthSet::GetHealthAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bAppliedNestedHealing && Data.OldValue == 80.0f && Data.NewValue == 70.0f)
			{
				bAppliedNestedHealing = true;
				Fixture.ApplyHealing(InnerSource, Target, 20.0f, InnerCauser);
			}
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetHealthAttribute(), HealthNativeHandle);

	Fixture.ApplyDamage(OuterSource, Target, 10.0f, OuterCauser);
	TestTrue(TEXT("Health setter 的原生属性委托真实应用 nested 治疗"), bAppliedNestedHealing);
	TestEqual(TEXT("Health setter 不以外层旧值覆盖 nested 治疗，最终生命 90"), Target.HealthSet->GetHealth(), 90.0f);
	TestEqual(TEXT("nested Healing meta 已消费"), Target.HealthSet->GetHealing(), 0.0f);
	TestEqual(TEXT("outer Damage meta 已消费"), Target.HealthSet->GetDamage(), 0.0f);
	TestEqual(TEXT("setter 重入保留两条独立 Health 事件"), HealthEvents.Num(), 2);
	TestEqual(TEXT("治疗本身不增加 Damage 消息"), DamageMessages.Num(), 1);
	int32 OuterEvents = 0;
	int32 InnerEvents = 0;
	for (const FGGYGOHealthMessageAttributeRecord& Event : HealthEvents)
	{
		if (Event.Causer == OuterCauser)
		{
			++OuterEvents;
			TestTrue(TEXT("外层 Health 事件 Instigator 正确"), Event.Instigator == OuterSource.Actor);
			TestEqual(TEXT("外层伤害幅度为 10"), Event.Magnitude, 10.0f);
			TestEqual(TEXT("外层旧值为 80"), Event.OldValue, 80.0f);
			TestEqual(TEXT("外层新值为 70"), Event.NewValue, 70.0f);
			TestTrue(TEXT("外层事件只带外层来源 Tag"), GetOnlySourceTag(Event, OuterTag, InnerTag) == OuterTag);
		}
		else if (Event.Causer == InnerCauser)
		{
			++InnerEvents;
			TestTrue(TEXT("内层 Health 事件 Instigator 正确"), Event.Instigator == InnerSource.Actor);
			TestEqual(TEXT("内层治疗幅度为 20"), Event.Magnitude, 20.0f);
			TestEqual(TEXT("内层旧值为 70"), Event.OldValue, 70.0f);
			TestEqual(TEXT("内层新值为 90"), Event.NewValue, 90.0f);
			TestTrue(TEXT("内层事件只带内层来源 Tag"), GetOnlySourceTag(Event, OuterTag, InnerTag) == InnerTag);
		}
		else { AddError(TEXT("Health 事件带有未知 Causer")); }
	}
	TestEqual(TEXT("外层 Health 事件恰好一次"), OuterEvents, 1);
	TestEqual(TEXT("内层 Health 事件恰好一次"), InnerEvents, 1);
	if (DamageMessages.IsValidIndex(0))
	{
		TestTrue(TEXT("Damage 消息只由外层真实 EffectCauser 产生"), DamageMessages[0].Message.Instigator == OuterCauser);
		TestEqual(TEXT("Damage 消息保留外层原始幅度 10"), DamageMessages[0].Message.Magnitude, 10.0f);
		TestTrue(TEXT("Damage 消息保留外层来源 Tag"), DamageMessages[0].Message.InstigatorTags.HasTagExact(OuterTag));
		TestEqual(TEXT("Damage 消息发布时 meta Damage 已清零"), DamageMessages[0].DamageMetaAtBroadcast, 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageRepNotifyNativeReentryTest,
	"GGYGO.AbilitySystem.HealthMessage.RepNotifyNativeReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageRepNotifyNativeReentryTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("独立 GameInstance、World 与消息路由初始化"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant DamageSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant PoiseSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	AActor* DamageCauser = Fixture.CreateCauser();
	AActor* PoiseCauser = Fixture.CreateCauser();
	if (!TestNotNull(TEXT("nested Damage 来源 ASC"), DamageSource.ASC)
		|| !TestNotNull(TEXT("nested Poise 来源 ASC"), PoiseSource.ASC)
		|| !TestNotNull(TEXT("目标 HealthSet"), Target.HealthSet)
		|| !TestNotNull(TEXT("nested Damage EffectCauser"), DamageCauser)
		|| !TestNotNull(TEXT("nested Poise EffectCauser"), PoiseCauser)) { return false; }

	const FGameplayTag DamageSourceTag = GGYGOGameplayTags::Gameplay_Damage;
	const FGameplayTag PoiseSourceTag = GGYGOGameplayTags::Gameplay_PoiseDamage;
	DamageSource.ASC->AddLooseGameplayTag(DamageSourceTag);
	PoiseSource.ASC->AddLooseGameplayTag(PoiseSourceTag);
	Target.HealthSet->InitHealth(100.0f);
	Target.HealthSet->InitPoise(10.0f);
	TArray<FGGYGOHealthMessageAttributeRecord> HealthEvents;
	TArray<FGGYGOHealthMessageAttributeRecord> PoiseEvents;
	TArray<FGGYGOHealthMessageAttributeRecord> DeathEvents;
	TArray<FGGYGOHealthMessageAttributeRecord> BreakEvents;
	const FDelegateHandle HealthHandle = Target.HealthSet->OnHealthChanged.AddLambda(
		[&HealthEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			HealthEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged, HealthHandle);
	const FDelegateHandle PoiseHandle = Target.HealthSet->OnPoiseChanged.AddLambda(
		[&PoiseEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			PoiseEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseChanged, PoiseHandle);
	const FDelegateHandle DeathHandle = Target.HealthSet->OnOutOfHealth.AddLambda(
		[&DeathEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			DeathEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnOutOfHealth, DeathHandle);
	const FDelegateHandle BreakHandle = Target.HealthSet->OnPoiseBroken.AddLambda(
		[&BreakEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			BreakEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseBroken, BreakHandle);

	TArray<FGGYGOHealthMessageMessageRecord> DamageMessages;
	TArray<FGGYGOHealthMessageMessageRecord> BreakMessages;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage,
		[&DamageMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			DamageMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak,
		[&BreakMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			BreakMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});

	bool bAppliedNestedDamage = false;
	const FDelegateHandle HealthNativeHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(
		UGGYGOHealthSet::GetHealthAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bAppliedNestedDamage && Data.OldValue > 0.0f && Data.NewValue == 0.0f)
			{
				bAppliedNestedDamage = true;
				Fixture.ApplyDamage(DamageSource, Target, 1.0f, DamageCauser);
			}
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetHealthAttribute(), HealthNativeHandle);
	bool bAppliedNestedPoiseDamage = false;
	const FDelegateHandle PoiseNativeHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(
		UGGYGOHealthSet::GetPoiseAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bAppliedNestedPoiseDamage && Data.OldValue > 0.0f && Data.NewValue == 0.0f)
			{
				bAppliedNestedPoiseDamage = true;
				Fixture.ApplyPoiseDamage(PoiseSource, Target, 2.0f, PoiseCauser);
			}
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetPoiseAttribute(), PoiseNativeHandle);

	// Contract simulation only: property assignment through Init* followed by the real existing OnRep function.
	Target.HealthSet->SimulateReplicatedHealth(0.0f);
	Target.HealthSet->SimulateReplicatedPoise(0.0f);
	TestTrue(TEXT("Health RepNotify native delegate 已触发 nested Damage GE"), bAppliedNestedDamage);
	TestTrue(TEXT("Poise RepNotify native delegate 已触发 nested PoiseDamage GE"), bAppliedNestedPoiseDamage);
	TestEqual(TEXT("Health RepNotify 后真实值为 0"), Target.HealthSet->GetHealth(), 0.0f);
	TestEqual(TEXT("Poise RepNotify 后真实值为 0"), Target.HealthSet->GetPoise(), 0.0f);
	TestEqual(TEXT("Health RepNotify native nested Damage 不重复死亡边沿"), DeathEvents.Num(), 1);
	TestEqual(TEXT("Poise RepNotify native nested 普通削韧不重复破韧边沿"), BreakEvents.Num(), 1);
	TestEqual(TEXT("只有 nested Damage GE 产生 Damage 消息"), DamageMessages.Num(), 1);
	TestEqual(TEXT("RepNotify 与零值普通削韧不产生 PoiseBreak 消息"), BreakMessages.Num(), 0);

	if (DamageMessages.IsValidIndex(0))
	{
		TestTrue(TEXT("唯一 Damage 消息来自 nested EffectCauser"), DamageMessages[0].Message.Instigator == DamageCauser);
		TestEqual(TEXT("nested Damage 消息保留幅度 1"), DamageMessages[0].Message.Magnitude, 1.0f);
		TestTrue(TEXT("nested Damage 消息保留 Source Tag"), DamageMessages[0].Message.InstigatorTags.HasTagExact(DamageSourceTag));
		TestEqual(TEXT("nested Damage 消息期间 Damage meta 为 0"), DamageMessages[0].DamageMetaAtBroadcast, 0.0f);
	}
	TestTrue(TEXT("Health RepNotify death event 的来源为空且只从正值归零"), DeathEvents.Num() == 1
		&& DeathEvents[0].Instigator == nullptr && DeathEvents[0].Causer == nullptr
		&& DeathEvents[0].OldValue == 100.0f && DeathEvents[0].NewValue == 0.0f
		&& !DeathEvents[0].bHasEffectSpec);
	TestTrue(TEXT("Poise RepNotify break event 的来源为空且只从正值归零"), BreakEvents.Num() == 1
		&& BreakEvents[0].Instigator == nullptr && BreakEvents[0].Causer == nullptr
		&& BreakEvents[0].OldValue == 10.0f && BreakEvents[0].NewValue == 0.0f
		&& !BreakEvents[0].bHasEffectSpec);

	const int32 DamageMessageCount = DamageMessages.Num();
	const int32 BreakMessageCount = BreakMessages.Num();
	Target.HealthSet->SimulateReplicatedHealth(0.0f);
	Target.HealthSet->SimulateReplicatedPoise(0.0f);
	TestEqual(TEXT("重复 Health RepNotify 不重复死亡边沿"), DeathEvents.Num(), 1);
	TestEqual(TEXT("重复 Poise RepNotify 不重复破韧边沿"), BreakEvents.Num(), 1);
	TestEqual(TEXT("重复 RepNotify 不额外发 Damage 消息"), DamageMessages.Num(), DamageMessageCount);
	TestEqual(TEXT("重复 RepNotify 不额外发 PoiseBreak 消息"), BreakMessages.Num(), BreakMessageCount);
	TestEqual(TEXT("Health 属性通知按每次 RepNotify 广播"), HealthEvents.Num(), 2);
	TestEqual(TEXT("Poise 属性通知按每次 RepNotify 广播"), PoiseEvents.Num(), 2);
	TestTrue(TEXT("所有 RepNotify 直接事件的 EffectSpec 均为空"), HealthEvents.IsValidIndex(0) && PoiseEvents.IsValidIndex(0)
		&& HealthEvents[0].Causer == nullptr
		&& HealthEvents[0].Instigator == nullptr && !HealthEvents[0].bHasEffectSpec
		&& PoiseEvents[0].Causer == nullptr && PoiseEvents[0].Instigator == nullptr
		&& !PoiseEvents[0].bHasEffectSpec);
	TestEqual(TEXT("所有三个 meta 最终保持清零"), Target.HealthSet->GetDamage()
		+ Target.HealthSet->GetHealing() + Target.HealthSet->GetPoiseDamage(), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageRepNotifyProjectReentryTest,
	"GGYGO.AbilitySystem.HealthMessage.RepNotifyProjectEventReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageRepNotifyProjectReentryTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("独立 GameInstance、World 与消息路由初始化"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant HealingSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	AActor* HealingCauser = Fixture.CreateCauser();
	if (!TestNotNull(TEXT("nested 治疗来源 ASC"), HealingSource.ASC)
		|| !TestNotNull(TEXT("目标 HealthSet"), Target.HealthSet)
		|| !TestNotNull(TEXT("nested 治疗 EffectCauser"), HealingCauser)) { return false; }
	const FGameplayTag HealingTag = GGYGOGameplayTags::Gameplay_PoiseDamage;
	HealingSource.ASC->AddLooseGameplayTag(HealingTag);
	Target.HealthSet->InitHealth(50.0f);
	TArray<FGGYGOHealthMessageAttributeRecord> HealthEvents;
	bool bDidNestedHealing = false;
	const FDelegateHandle HealthHandle = Target.HealthSet->OnHealthChanged.AddLambda(
		[&](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			HealthEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
			if (!bDidNestedHealing && Spec == nullptr && OldValue == 50.0f && NewValue == 40.0f)
			{
				bDidNestedHealing = true;
				Fixture.ApplyHealing(HealingSource, Target, 10.0f, HealingCauser);
			}
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged, HealthHandle);
	TArray<FGGYGOHealthMessageMessageRecord> DamageMessages;
	TArray<FGGYGOHealthMessageMessageRecord> BreakMessages;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage,
		[&DamageMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			DamageMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak,
		[&BreakMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			BreakMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});
	Target.HealthSet->SimulateReplicatedHealth(40.0f);
	TestTrue(TEXT("OnHealthChanged RepNotify 事件内真实执行 nested Healing GE"), bDidNestedHealing);
	TestEqual(TEXT("项目事件 nested 治疗后的实时 Health 为 50"), Target.HealthSet->GetHealth(), 50.0f);
	TestEqual(TEXT("RepNotify 复制值和 nested 治疗各有一条历史事件"), HealthEvents.Num(), 2);
	TestEqual(TEXT("nested Healing 不伪造 Damage 消息"), DamageMessages.Num(), 0);
	TestEqual(TEXT("Health RepNotify 不发布 PoiseBreak 消息"), BreakMessages.Num(), 0);
	if (HealthEvents.Num() == 2)
	{
		TestTrue(TEXT("复制通知事件的 Instigator/Causer/Spec 均为空"), HealthEvents[0].Instigator == nullptr
			&& HealthEvents[0].Causer == nullptr && !HealthEvents[0].bHasEffectSpec);
		TestEqual(TEXT("复制事件保留原始旧值 50"), HealthEvents[0].OldValue, 50.0f);
		TestEqual(TEXT("复制事件保留本次新值 40，即使实时值已变为 50"), HealthEvents[0].NewValue, 40.0f);
		TestTrue(TEXT("nested 治疗事件携带真实来源"), HealthEvents[1].Instigator == HealingSource.Actor
			&& HealthEvents[1].Causer == HealingCauser && HealthEvents[1].bHasEffectSpec);
		TestEqual(TEXT("nested 治疗事件幅度 10"), HealthEvents[1].Magnitude, 10.0f);
		TestEqual(TEXT("nested 治疗历史旧值 40"), HealthEvents[1].OldValue, 40.0f);
		TestEqual(TEXT("nested 治疗历史新值 50"), HealthEvents[1].NewValue, 50.0f);
		TestTrue(TEXT("nested 治疗事件保留独立源 Tag"), HealthEvents[1].SourceTags.HasTagExact(HealingTag));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageRepNotifyAggregatorRollbackTest,
	"GGYGO.AbilitySystem.HealthMessage.RepNotifyAggregatorRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageRepNotifyAggregatorRollbackTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("独立 GameInstance、World 与消息路由初始化"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant DamageSource = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	AActor* DamageCauser = Fixture.CreateCauser();
	if (!TestNotNull(TEXT("nested Damage 来源 ASC"), DamageSource.ASC)
		|| !TestNotNull(TEXT("目标 HealthSet"), Target.HealthSet)
		|| !TestNotNull(TEXT("nested Damage EffectCauser"), DamageCauser)) { return false; }
	const FGameplayTag DamageTag = GGYGOGameplayTags::Gameplay_Damage;
	DamageSource.ASC->AddLooseGameplayTag(DamageTag);
	Target.HealthSet->InitHealth(10.0f);
	Fixture.ApplyModifier(DamageSource, Target, UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::Additive,
		0.0f, DamageCauser, FGameplayTag(), EGameplayEffectDurationType::Infinite);
	TestEqual(TEXT("Infinite +0 建立聚合器，初始有效 Health 为 10"), Target.HealthSet->GetHealth(), 10.0f);

	TArray<FGGYGOHealthMessageAttributeRecord> NativeEvents;
	TArray<FGGYGOHealthMessageAttributeRecord> HealthEvents;
	TArray<FGGYGOHealthMessageAttributeRecord> DeathEvents;
	const FDelegateHandle NativeHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(
		UGGYGOHealthSet::GetHealthAttribute()).AddLambda(
		[&NativeEvents](const FOnAttributeChangeData& Data)
		{
			NativeEvents.Add(MakeAttributeRecord(nullptr, nullptr, nullptr,
				Data.NewValue - Data.OldValue, Data.OldValue, Data.NewValue));
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetHealthAttribute(), NativeHandle);
	const FDelegateHandle HealthHandle = Target.HealthSet->OnHealthChanged.AddLambda(
		[&HealthEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			HealthEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged, HealthHandle);
	const FDelegateHandle DeathHandle = Target.HealthSet->OnOutOfHealth.AddLambda(
		[&DeathEvents](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
			float Magnitude, float OldValue, float NewValue)
		{
			DeathEvents.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnOutOfHealth, DeathHandle);
	TArray<FGGYGOHealthMessageMessageRecord> DamageMessages;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage,
		[&DamageMessages, Target](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			DamageMessages.Add({ Message, Target.HealthSet->GetDamage(), Target.HealthSet->GetHealing(),
				Target.HealthSet->GetPoiseDamage() });
		});
	bool bAppliedNestedDamage = false;
	const FDelegateHandle NestedHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(
		UGGYGOHealthSet::GetHealthAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bAppliedNestedDamage && Data.OldValue == 10.0f && Data.NewValue == 10.0f)
			{
				bAppliedNestedDamage = true;
				Fixture.ApplyDamage(DamageSource, Target, 1.0f, DamageCauser);
			}
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetHealthAttribute(), NestedHandle);

	// Distinct received base/current values exercise effective-value capture without
	// coupling this regression to nonzero persistent modifiers in the Damage setter.
	FGameplayAttributeData ReceivedHealth(10.0f);
	ReceivedHealth.SetCurrentValue(0.0f);
	Target.HealthSet->SimulateReplicatedHealth(ReceivedHealth);
	TestTrue(TEXT("aggregator-backed RepNotify 原生委托中 nested Damage 已执行"), bAppliedNestedDamage);
	TestEqual(TEXT("nested Damage GE 将有效 Health 从 10 扣到 9"), Target.HealthSet->GetHealth(), 9.0f);
	TestEqual(TEXT("有效 Health 仍为正，不因收到 current 0 误发死亡边沿"), DeathEvents.Num(), 0);
	TestEqual(TEXT("macro 的 incoming 与 nested GE 各有一条 native 通知"), NativeEvents.Num(), 2);
	TestEqual(TEXT("nested GE 与 RepNotify 各有一条项目 Health 历史事件"), HealthEvents.Num(), 2);
	TestEqual(TEXT("只有 nested Damage GE 发出一条消息"), DamageMessages.Num(), 1);
	int32 RepIncomingNativeEvents = 0;
	int32 NestedNativeEvents = 0;
	for (const FGGYGOHealthMessageAttributeRecord& Event : NativeEvents)
	{
		if (Event.OldValue == 10.0f && Event.NewValue == 10.0f) { ++RepIncomingNativeEvents; }
		if (Event.OldValue == 10.0f && Event.NewValue == 9.0f) { ++NestedNativeEvents; }
	}
	TestEqual(TEXT("聚合器按收到的 base 10 重算，有效复制历史保留为 10"), RepIncomingNativeEvents, 1);
	TestEqual(TEXT("native 回调中的 nested Damage 独立改变有效值为 9"), NestedNativeEvents, 1);
	if (DamageMessages.IsValidIndex(0))
	{
		TestTrue(TEXT("aggregator RepNotify 场景消息来自 nested GE 的 Causer"), DamageMessages[0].Message.Instigator == DamageCauser);
		TestEqual(TEXT("nested Damage 消息保留原始幅度 1"), DamageMessages[0].Message.Magnitude, 1.0f);
		TestTrue(TEXT("nested Damage 消息保留真实来源 Tag"), DamageMessages[0].Message.InstigatorTags.HasTagExact(DamageTag));
	}
	int32 RepNotifyProjectEvents = 0;
	int32 NestedProjectEvents = 0;
	for (const FGGYGOHealthMessageAttributeRecord& Event : HealthEvents)
	{
		if (!Event.bHasEffectSpec && Event.Instigator == nullptr && Event.Causer == nullptr)
		{
			++RepNotifyProjectEvents;
			TestEqual(TEXT("RepNotify 项目历史旧有效值是 10"), Event.OldValue, 10.0f);
			TestEqual(TEXT("RepNotify 项目历史新有效值仍是 10"), Event.NewValue, 10.0f);
		}
		else if (Event.Causer == DamageCauser)
		{
			++NestedProjectEvents;
			TestTrue(TEXT("nested Damage 项目事件保留真实来源"), Event.Instigator == DamageSource.Actor && Event.bHasEffectSpec);
			TestEqual(TEXT("nested Damage 项目事件从有效 Health 10 开始"), Event.OldValue, 10.0f);
			TestEqual(TEXT("nested Damage setter 后有效 Health 为 9"), Event.NewValue, 9.0f);
		}
		else { AddError(TEXT("Aggregator RepNotify Health event source mismatch")); }
	}
	TestEqual(TEXT("RepNotify 历史事件恰好一条"), RepNotifyProjectEvents, 1);
	TestEqual(TEXT("nested GE 历史事件恰好一条"), NestedProjectEvents, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageDirectPoiseReentryTest,
	"GGYGO.AbilitySystem.HealthMessage.DirectPoiseNativeReentryAndNullSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageDirectPoiseReentryTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("Initialize fixture"), Fixture.Initialize())) { return false; }
	const auto Source = Fixture.CreateCombatant();
	const auto Target = Fixture.CreateCombatant();
	AActor* OuterCauser = Fixture.CreateCauser();
	AActor* InnerCauser = Fixture.CreateCauser();
	if (!Source.ASC || !Target.HealthSet || !OuterCauser || !InnerCauser) { AddError(TEXT("Missing combatant")); return false; }
	Target.HealthSet->InitPoise(10.0f);
	TArray<FGGYGOHealthMessageAttributeRecord> Edges;
	TArray<FGGYGOVerbMessage> Messages;
	const auto EdgeHandle = Target.HealthSet->OnPoiseBroken.AddLambda(
		[&](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			Edges.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseBroken, EdgeHandle);
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak, [&](FGameplayTag, const FGGYGOVerbMessage& Message)
	{
		Messages.Add(Message);
		TestEqual(TEXT("Poise meta consumed before message"), Target.HealthSet->GetPoiseDamage(), 0.0f);
	});
	bool bRestored = false;
	const auto NativeHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(UGGYGOHealthSet::GetPoiseAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bRestored && Data.OldValue == 10.0f && Data.NewValue == 0.0f)
			{
				bRestored = true;
				Fixture.ApplyDirectPoise(Source, Target, EGameplayModOp::Override, 5.0f, InnerCauser);
				Fixture.ApplyDirectPoise(Source, Target, EGameplayModOp::Override, 0.0f, InnerCauser);
			}
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetPoiseAttribute(), NativeHandle);
	Fixture.ApplyDirectPoise(Source, Target, EGameplayModOp::Override, 0.0f, OuterCauser);
	TestTrue(TEXT("Native callback restored and broke poise again"), bRestored);
	TestEqual(TEXT("Two actual edges survive nested writes"), Edges.Num(), 2);
	TestEqual(TEXT("Two direct-poise messages"), Messages.Num(), 2);
	int32 OuterCount = 0;
	int32 InnerCount = 0;
	for (const auto& Edge : Edges)
	{
		TestTrue(TEXT("Edge retains real spec"), Edge.bHasEffectSpec);
		if (Edge.Causer == OuterCauser && Edge.OldValue == 10.0f && Edge.NewValue == 0.0f && Edge.Magnitude == 10.0f) { ++OuterCount; }
		if (Edge.Causer == InnerCauser && Edge.OldValue == 5.0f && Edge.NewValue == 0.0f && Edge.Magnitude == 5.0f) { ++InnerCount; }
	}
	TestEqual(TEXT("Outer historical edge kept once"), OuterCount, 1);
	TestEqual(TEXT("Inner historical edge kept once"), InnerCount, 1);
	for (const auto& Message : Messages)
	{
		TestTrue(TEXT("Direct poise message keeps actual loss and causer"),
			(Message.Instigator == OuterCauser && Message.Magnitude == 10.0f)
			|| (Message.Instigator == InnerCauser && Message.Magnitude == 5.0f));
	}

	Fixture.ApplyDirectPoise(Source, Target, EGameplayModOp::Override, 7.0f, InnerCauser);
	TArray<FGGYGOHealthMessageTestModifier> Modifiers;
	Modifiers.Add({ UGGYGOHealthSet::GetPoiseAttribute(), EGameplayModOp::Override, 0.0f });
	FGameplayEffectContextHandle EmptySourceContext(new FGameplayEffectContext());
	FGameplayEffectSpec NoSourceSpec(Fixture.CreateEffect(Modifiers), EmptySourceContext, 1.0f);
	Target.ASC->ApplyGameplayEffectSpecToSelf(NoSourceSpec);
	TestEqual(TEXT("Source-less direct GE adds one edge"), Edges.Num(), 3);
	TestEqual(TEXT("Source-less direct GE adds one message"), Messages.Num(), 3);
	if (Edges.IsValidIndex(2) && Messages.IsValidIndex(2))
	{
		TestTrue(TEXT("Do not fabricate actor sources for a valid spec"), Edges[2].bHasEffectSpec
			&& Edges[2].Instigator == nullptr && Edges[2].Causer == nullptr && Messages[2].Instigator == nullptr);
		TestEqual(TEXT("Source-less direct message uses actual loss"), Messages[2].Magnitude, 7.0f);
		TestTrue(TEXT("Target remains the ASC owner"), Messages[2].Target == Target.Actor);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageProjectCallbacksReentryTest,
	"GGYGO.AbilitySystem.HealthMessage.ProjectCallbacksReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageProjectCallbacksReentryTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("Initialize fixture"), Fixture.Initialize())) { return false; }
	const auto Source = Fixture.CreateCombatant();
	const auto Target = Fixture.CreateCombatant();
	AActor* OuterCauser = Fixture.CreateCauser();
	AActor* InnerCauser = Fixture.CreateCauser();
	if (!Source.ASC || !Target.HealthSet || !OuterCauser || !InnerCauser) { AddError(TEXT("Missing combatant")); return false; }
	Target.HealthSet->InitHealth(10.0f);
	Target.HealthSet->InitPoise(10.0f);
	TArray<FGGYGOHealthMessageAttributeRecord> Deaths;
	TArray<FGGYGOHealthMessageAttributeRecord> Breaks;
	TArray<FGGYGOVerbMessage> DamageMessages;
	TArray<FGGYGOVerbMessage> BreakMessages;
	bool bDeathReentry = false;
	bool bBreakReentry = false;
	bool bMessageReentry = false;
	const auto DeathHandle = Target.HealthSet->OnOutOfHealth.AddLambda(
		[&](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			Deaths.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
			if (!bDeathReentry)
			{
				bDeathReentry = true;
				Fixture.ApplyHealing(Source, Target, 5.0f, InnerCauser);
				Fixture.ApplyDamage(Source, Target, 6.0f, InnerCauser);
			}
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnOutOfHealth, DeathHandle);
	const auto BreakHandle = Target.HealthSet->OnPoiseBroken.AddLambda(
		[&](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			Breaks.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
			if (!bBreakReentry)
			{
				bBreakReentry = true;
				Fixture.ApplyDirectPoise(Source, Target, EGameplayModOp::Override, 4.0f, InnerCauser);
				Fixture.ApplyPoiseDamage(Source, Target, 6.0f, InnerCauser);
			}
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseBroken, BreakHandle);
	// Adversarial listener exercises reentry robustness; production observers remain read-only.
	Fixture.Listen(GGYGOGameplayTags::Message_Damage, [&](FGameplayTag, const FGGYGOVerbMessage& Message)
	{
		DamageMessages.Add(Message);
		TestEqual(TEXT("Damage input consumed before listener"), Target.HealthSet->GetDamage(), 0.0f);
		if (!bMessageReentry)
		{
			bMessageReentry = true;
			Fixture.ApplyPoiseDamage(Source, Target, 20.0f, InnerCauser);
		}
	});
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak, [&](FGameplayTag, const FGGYGOVerbMessage& Message)
	{
		BreakMessages.Add(Message);
		TestEqual(TEXT("Poise input consumed before listener"), Target.HealthSet->GetPoiseDamage(), 0.0f);
	});
	Fixture.ApplyDamage(Source, Target, 12.0f, OuterCauser);
	TestTrue(TEXT("All three reentry paths executed"), bDeathReentry && bBreakReentry && bMessageReentry);
	TestEqual(TEXT("Two death edges after a real restoration"), Deaths.Num(), 2);
	TestEqual(TEXT("Two poise edges after a real restoration"), Breaks.Num(), 2);
	TestEqual(TEXT("Each damage modifier produces one message"), DamageMessages.Num(), 2);
	TestEqual(TEXT("Each poise edge produces one message"), BreakMessages.Num(), 2);
	TestEqual(TEXT("Older notifications do not restore Health"), Target.HealthSet->GetHealth(), 0.0f);
	TestEqual(TEXT("Older notifications do not restore Poise"), Target.HealthSet->GetPoise(), 0.0f);
	if (Deaths.Num() == 2 && Breaks.Num() == 2)
	{
		TestTrue(TEXT("Death facts retain both original values and sources"), Deaths[0].OldValue == 10.0f
			&& Deaths[0].NewValue == 0.0f && Deaths[0].Causer == OuterCauser && Deaths[0].bHasEffectSpec
			&& Deaths[1].OldValue == 5.0f && Deaths[1].NewValue == 0.0f && Deaths[1].Causer == InnerCauser && Deaths[1].bHasEffectSpec);
		TestTrue(TEXT("Poise facts retain raw overkill magnitudes"), Breaks[0].OldValue == 10.0f
			&& Breaks[0].NewValue == 0.0f && Breaks[0].Magnitude == 20.0f
			&& Breaks[1].OldValue == 4.0f && Breaks[1].NewValue == 0.0f && Breaks[1].Magnitude == 6.0f);
	}
	Fixture.ApplyDamage(Source, Target, 1.0f, InnerCauser);
	Fixture.ApplyPoiseDamage(Source, Target, 1.0f, InnerCauser);
	TestEqual(TEXT("Damage at zero does not repeat death"), Deaths.Num(), 2);
	TestEqual(TEXT("Poise damage at zero does not repeat break"), Breaks.Num(), 2);
	TestEqual(TEXT("No false poise message at zero"), BreakMessages.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageMultiModifierTest,
	"GGYGO.AbilitySystem.HealthMessage.MultiModifierIntermediateReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageMultiModifierTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("Initialize fixture"), Fixture.Initialize())) { return false; }
	const auto Source = Fixture.CreateCombatant();
	const auto Target = Fixture.CreateCombatant();
	if (!Source.ASC || !Target.HealthSet) { AddError(TEXT("Missing combatant")); return false; }
	Target.HealthSet->InitHealth(20.0f);
	Target.HealthSet->InitPoise(10.0f);
	int32 DamageCount = 0;
	int32 BreakCount = 0;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage, [&](FGameplayTag, const FGGYGOVerbMessage& Message)
	{
		++DamageCount;
		TestEqual(TEXT("First modifier settled health"), Target.HealthSet->GetHealth(), 15.0f);
		TestEqual(TEXT("Second modifier has not settled poise yet"), Target.HealthSet->GetPoise(), 10.0f);
		TestEqual(TEXT("First modifier input consumed"), Target.HealthSet->GetDamage(), 0.0f);
		TestEqual(TEXT("Raw first magnitude"), Message.Magnitude, 5.0f);
		Fixture.ApplyHealing(Source, Target, 2.0f, Source.Actor);
	});
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak, [&](FGameplayTag, const FGGYGOVerbMessage& Message)
	{
		++BreakCount;
		TestEqual(TEXT("Intermediate healing remains committed"), Target.HealthSet->GetHealth(), 17.0f);
		TestEqual(TEXT("Second modifier raw magnitude"), Message.Magnitude, 12.0f);
	});
	TArray<FGGYGOHealthMessageTestModifier> Modifiers;
	Modifiers.Add({ UGGYGOHealthSet::GetDamageAttribute(), EGameplayModOp::Additive, 5.0f });
	Modifiers.Add({ UGGYGOHealthSet::GetPoiseDamageAttribute(), EGameplayModOp::Additive, 12.0f });
	Fixture.ApplyEffect(Source, Target, Fixture.CreateEffect(Modifiers), Source.Actor);
	TestEqual(TEXT("One damage message"), DamageCount, 1);
	TestEqual(TEXT("One real poise break"), BreakCount, 1);
	TestEqual(TEXT("Final health after intermediate GE"), Target.HealthSet->GetHealth(), 17.0f);
	TestEqual(TEXT("Final poise"), Target.HealthSet->GetPoise(), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageRepNotifySetterTest,
	"GGYGO.AbilitySystem.HealthMessage.RepNotifyNativeSetter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageRepNotifySetterTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("Initialize fixture"), Fixture.Initialize())) { return false; }
	const auto Target = Fixture.CreateCombatant();
	if (!Target.HealthSet) { AddError(TEXT("Missing health set")); return false; }
	Target.HealthSet->InitHealth(10.0f);
	int32 DeathCount = 0;
	TArray<FGGYGOHealthMessageAttributeRecord> Changes;
	const auto DeathHandle = Target.HealthSet->OnOutOfHealth.AddLambda(
		[&](AActor*, AActor*, const FGameplayEffectSpec*, float, float, float) { ++DeathCount; });
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnOutOfHealth, DeathHandle);
	const auto ChangeHandle = Target.HealthSet->OnHealthChanged.AddLambda(
		[&](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			Changes.Add(MakeAttributeRecord(Instigator, Causer, Spec, Magnitude, OldValue, NewValue));
		});
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged, ChangeHandle);
	bool bRestored = false;
	const auto NativeHandle = Target.ASC->GetGameplayAttributeValueChangeDelegate(UGGYGOHealthSet::GetHealthAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bRestored && Data.OldValue == 10.0f && Data.NewValue == 0.0f)
			{
				bRestored = true;
				Target.HealthSet->SetHealth(5.0f);
			}
		});
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetHealthAttribute(), NativeHandle);
	Target.HealthSet->SimulateReplicatedHealth(0.0f);
	TestTrue(TEXT("Native callback restored health with ordinary setter"), bRestored);
	TestEqual(TEXT("RepNotify does not overwrite restored health"), Target.HealthSet->GetHealth(), 5.0f);
	TestEqual(TEXT("One historical replicated death"), DeathCount, 1);
	TestEqual(TEXT("Setter and replication keep separate facts"), Changes.Num(), 2);
	int32 RestoreFacts = 0;
	int32 ReplicationFacts = 0;
	for (const auto& Change : Changes)
	{
		if (Change.OldValue == 0.0f && Change.NewValue == 5.0f) { ++RestoreFacts; }
		if (Change.OldValue == 10.0f && Change.NewValue == 0.0f) { ++ReplicationFacts; }
	}
	TestEqual(TEXT("Explicit base write was not suppressed as rewind"), RestoreFacts, 1);
	TestEqual(TEXT("Replicated fact remains zero"), ReplicationFacts, 1);
	Fixture.ApplyDamage(Target, Target, 5.0f, Target.Actor);
	TestEqual(TEXT("Restoration reopened death edge latch"), DeathCount, 2);
	Target.HealthSet->SimulateReplicatedHealth(0.0f);
	TestEqual(TEXT("Duplicate zero does not repeat edge"), DeathCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageDeliveryLifecycleTest,
	"GGYGO.AbilitySystem.HealthMessage.DeliveryLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageDeliveryLifecycleTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("Initialize real GI, world and router"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant Source = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	if (!TestNotNull(TEXT("Source ASC"), Source.ASC) || !TestNotNull(TEXT("Target ASC"), Target.ASC)
		|| !TestNotNull(TEXT("Target HealthSet"), Target.HealthSet)) { return false; }
	Target.HealthSet->InitHealth(100.0f);
	Target.HealthSet->InitPoise(10.0f);
	TestFalse(TEXT("Fixture actor has not begun play"), Target.Actor->HasActorBegunPlay());

	int32 HealthChangedCount = 0;
	int32 DamageMessageCount = 0;
	int32 PoiseChangedCount = 0;
	int32 PoiseBrokenCount = 0;
	int32 BreakMessageCount = 0;
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged,
		Target.HealthSet->OnHealthChanged.AddLambda([&](AActor*, AActor*, const FGameplayEffectSpec*, float, float, float)
		{
			++HealthChangedCount;
			TestEqual(TEXT("Health delegate sees consumed damage meta"), Target.HealthSet->GetDamage(), 0.0f);
		}));
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseChanged,
		Target.HealthSet->OnPoiseChanged.AddLambda([&](AActor*, AActor*, const FGameplayEffectSpec*, float, float, float)
		{
			++PoiseChangedCount;
		}));
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseBroken,
		Target.HealthSet->OnPoiseBroken.AddLambda([&](AActor*, AActor*, const FGameplayEffectSpec*, float, float, float)
		{
			++PoiseBrokenCount;
		}));
	const auto ListenDamage = [&]()
	{
		Fixture.Listen(GGYGOGameplayTags::Message_Damage, [&](FGameplayTag, const FGGYGOVerbMessage& Message)
		{
			++DamageMessageCount;
			TestTrue(TEXT("Damage message retains target"), Message.Target == Target.Actor);
			TestEqual(TEXT("Delivered damage has consumed meta"), Target.HealthSet->GetDamage(), 0.0f);
		});
	};
	ListenDamage();
	Fixture.ApplyDamage(Source, Target, 5.0f, Source.Actor);
	TestEqual(TEXT("PreBegin damage settles"), Target.HealthSet->GetHealth(), 95.0f);
	TestEqual(TEXT("PreBegin delivers with ready dependencies"), DamageMessageCount, 1);
	Fixture.MessageHandles.Last().Unregister();
	Fixture.ApplyDamage(Source, Target, 2.0f, Source.Actor);
	TestEqual(TEXT("No-listener damage still settles"), Target.HealthSet->GetHealth(), 93.0f);
	TestEqual(TEXT("Removed listener receives nothing"), DamageMessageCount, 1);
	ListenDamage();
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak, [&](FGameplayTag, const FGGYGOVerbMessage&)
	{
		++BreakMessageCount;
	});

	AddExpectedErrorPlain(TEXT("[Messages][HealthSet] DeliveryFailed Reason=MissingWorld"), EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedErrorPlain(TEXT("[Messages][HealthSet] DeliveryFailed Reason=MissingGameInstance"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("[Messages][HealthSet] DeliveryFailed Reason=MissingRouter"), EAutomationExpectedErrorFlags::Contains, 2);
	const auto CheckFailedDamage = [&](float Damage, float ExpectedHealth)
	{
		const int32 MessagesBefore = DamageMessageCount;
		const int32 ChangesBefore = HealthChangedCount;
		Fixture.ApplyDamage(Source, Target, Damage, Source.Actor);
		TestEqual(TEXT("Delivery failure retains completed settlement"), Target.HealthSet->GetHealth(), ExpectedHealth);
		TestEqual(TEXT("Delivery failure retains attribute delegate"), HealthChangedCount, ChangesBefore + 1);
		TestEqual(TEXT("Delivery failure consumes meta"), Target.HealthSet->GetDamage(), 0.0f);
		TestEqual(TEXT("Unavailable route does not report delivery"), DamageMessageCount, MessagesBefore);
	};
	Target.HealthSet->SetMessageWorldOverride(nullptr);
	CheckFailedDamage(3.0f, 90.0f);
	CheckFailedDamage(4.0f, 86.0f);
	Target.HealthSet->ClearMessageWorldOverride();
	Fixture.ApplyDamage(Source, Target, 1.0f, Source.Actor);
	TestEqual(TEXT("Recovery delivers only the new result, without replay"), DamageMessageCount, 2);
	Target.HealthSet->SetMessageWorldOverride(nullptr);
	CheckFailedDamage(1.0f, 84.0f);

	// Only HealthSet's message context is overridden; the real ASC keeps its initialized GI world.
	UWorld* DependencyWorld = UWorld::CreateWorld(EWorldType::Game, false,
		FName(TEXT("GGYGOHealthMessageDependencyWorld")));
	if (!TestNotNull(TEXT("Isolated dependency world"), DependencyWorld))
	{
		Target.HealthSet->ClearMessageWorldOverride();
		return false;
	}
	UGameInstance* MissingRouterGameInstance = nullptr;
	ON_SCOPE_EXIT
	{
		Target.HealthSet->ClearMessageWorldOverride();
		DependencyWorld->SetGameInstance(nullptr);
		DependencyWorld->DestroyWorld(false);
		if (UPackage* Package = DependencyWorld->GetPackage()) { Package->SetDirtyFlag(false); }
		if (MissingRouterGameInstance)
		{
			if (MissingRouterGameInstance->IsRooted()) { MissingRouterGameInstance->RemoveFromRoot(); }
			MissingRouterGameInstance->MarkAsGarbage();
		}
	};
	Target.HealthSet->SetMessageWorldOverride(DependencyWorld);
	TestNull(TEXT("Dependency world has no GI"), DependencyWorld->GetGameInstance());
	CheckFailedDamage(2.0f, 82.0f);
	CheckFailedDamage(2.0f, 80.0f);
	MissingRouterGameInstance = NewObject<UGameInstance>(Fixture.Engine);
	if (!TestNotNull(TEXT("GI with intentionally uninitialized subsystems"), MissingRouterGameInstance)) { return false; }
	MissingRouterGameInstance->AddToRoot();
	DependencyWorld->SetGameInstance(MissingRouterGameInstance);
	TestNull(TEXT("Missing router is a real subsystem lookup"),
		UGameInstance::GetSubsystem<UGameplayMessageSubsystem>(MissingRouterGameInstance));
	CheckFailedDamage(2.0f, 78.0f);
	CheckFailedDamage(2.0f, 76.0f);
	Target.HealthSet->ClearMessageWorldOverride();
	Fixture.ApplyDamage(Source, Target, 1.0f, Source.Actor);
	TestEqual(TEXT("Recovered router does not replay discarded messages"), DamageMessageCount, 3);
	Target.HealthSet->SetMessageWorldOverride(DependencyWorld);
	CheckFailedDamage(1.0f, 74.0f);
	TestEqual(TEXT("All damage attribute notifications survive"), HealthChangedCount, 12);

	{
		const bool bWasTearingDown = Fixture.World->bIsTearingDown;
		Fixture.World->bIsTearingDown = true;
		ON_SCOPE_EXIT { Fixture.World->bIsTearingDown = bWasTearingDown; Target.HealthSet->ClearMessageWorldOverride(); };
		Target.HealthSet->SetMessageWorldOverride(nullptr);
		Fixture.ApplyPoiseDamage(Source, Target, 10.0f, Source.Actor);
		TestEqual(TEXT("Teardown retains poise settlement"), Target.HealthSet->GetPoise(), 0.0f);
		TestEqual(TEXT("Teardown retains changed delegate"), PoiseChangedCount, 1);
		TestEqual(TEXT("Teardown retains broken delegate"), PoiseBrokenCount, 1);
		TestEqual(TEXT("Teardown consumes poise meta"), Target.HealthSet->GetPoiseDamage(), 0.0f);
		TestEqual(TEXT("Owner world teardown retires even a missing message world"), BreakMessageCount, 0);
	}

	const FGGYGOHealthMessageCombatant DestroyTarget = Fixture.CreateCombatant();
	if (!TestNotNull(TEXT("Destruction target HealthSet"), DestroyTarget.HealthSet)) { return false; }
	DestroyTarget.HealthSet->InitPoise(10.0f);
	bool bDestroyedDuringChanged = false;
	int32 DestroyedBrokenCount = 0;
	Fixture.TrackAttributeDelegate(DestroyTarget.HealthSet->OnPoiseChanged,
		DestroyTarget.HealthSet->OnPoiseChanged.AddLambda([&](AActor*, AActor*, const FGameplayEffectSpec*, float, float, float)
		{
			bDestroyedDuringChanged = Fixture.World->DestroyActor(DestroyTarget.Actor);
		}));
	Fixture.TrackAttributeDelegate(DestroyTarget.HealthSet->OnPoiseBroken,
		DestroyTarget.HealthSet->OnPoiseBroken.AddLambda([&](AActor*, AActor*, const FGameplayEffectSpec*, float, float, float)
		{
			++DestroyedBrokenCount;
		}));
	Fixture.ApplyPoiseDamage(Source, DestroyTarget, 10.0f, Source.Actor);
	TestTrue(TEXT("Real actor destruction occurs before the queued message"), bDestroyedDuringChanged);
	TestTrue(TEXT("Owner exposes real destruction evidence"), DestroyTarget.Actor->IsActorBeingDestroyed());
	TestEqual(TEXT("Destruction preserves committed poise"), DestroyTarget.HealthSet->GetPoise(), 0.0f);
	TestEqual(TEXT("Destruction preserves edge delegate"), DestroyedBrokenCount, 1);
	TestEqual(TEXT("Destruction consumes poise meta"), DestroyTarget.HealthSet->GetPoiseDamage(), 0.0f);
	TestEqual(TEXT("Destruction retires its remaining message"), BreakMessageCount, 0);
	Fixture.ApplyDirectPoise(Source, Target, EGameplayModOp::Additive, 10.0f, Source.Actor);
	Fixture.ApplyPoiseDamage(Source, Target, 10.0f, Source.Actor);
	TestEqual(TEXT("Only the new live break is delivered after retirement"), BreakMessageCount, 1);
	TestEqual(TEXT("Live restoration reopens the original edge"), PoiseBrokenCount, 2);
	return true;
}

namespace
{
	FActiveGameplayEffectHandle ApplyPersistentNumericModifiers(FGGYGOHealthMessageTestFixture& Fixture,
		const FGGYGOHealthMessageCombatant& Target, const TArray<FGGYGOHealthMessageTestModifier>& Modifiers)
	{
		UGameplayEffect* Effect = Fixture.CreateEffect(Modifiers, EGameplayEffectDurationType::Infinite);
		if (!Effect) { return FActiveGameplayEffectHandle(); }
		FGameplayEffectContextHandle Context = Target.ASC->MakeEffectContext();
		Context.AddInstigator(Target.Actor, Target.Actor);
		return Target.ASC->ApplyGameplayEffectSpecToSelf(FGameplayEffectSpec(Effect, Context, 1.0f));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageCurrentValueSettlementTest,
	"GGYGO.AbilitySystem.HealthMessage.CurrentValueSettlement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageCurrentValueSettlementTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("Numeric fixture initializes"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Offset = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant AboveMax = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Reentry = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant Rejected = Fixture.CreateCombatant();
	const FGGYGOHealthMessageCombatant PostRejected = Fixture.CreateCombatant();
	if (!Target.HealthSet || !Offset.HealthSet || !AboveMax.HealthSet || !Reentry.HealthSet || !Rejected.HealthSet || !PostRejected.HealthSet)
	{
		AddError(TEXT("Numeric combatants were not created"));
		return false;
	}
	TArray<FGGYGOVerbMessage> Messages;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage, [&Messages](FGameplayTag, const FGGYGOVerbMessage& Message)
	{
		Messages.Add(Message);
	});
	Target.HealthSet->SetMaxHealth(500.0f);
	Target.HealthSet->SetMaxPoise(500.0f);
	const FActiveGameplayEffectHandle Multiply = ApplyPersistentNumericModifiers(Fixture, Target,
		{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::MultiplyAdditive, 2.0f },
		 { UGGYGOHealthSet::GetPoiseAttribute(), EGameplayModOp::MultiplyAdditive, 2.0f }});
	TestTrue(TEXT("Persistent multiplier is active"), Multiply.IsValid());
	TestEqual(TEXT("Base100 times2 exposes Current200"), Target.HealthSet->GetHealth(), 200.0f);
	Fixture.ApplyDamage(Target, Target, 10.0f, Target.Actor);
	TestEqual(TEXT("Damage10 consumes Current10 once"), Target.HealthSet->GetHealth(), 190.0f);
	TestEqual(TEXT("Damage10 leaves Base95"), Target.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute()), 95.0f);
	Target.ASC->RemoveActiveGameplayEffect(Multiply);
	TestEqual(TEXT("Removing times2 after Damage10 reveals Current95"), Target.HealthSet->GetHealth(), 95.0f);
	const FActiveGameplayEffectHandle CapMultiply = ApplyPersistentNumericModifiers(Fixture, Target,
		{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::MultiplyAdditive, 2.0f },
		 { UGGYGOHealthSet::GetPoiseAttribute(), EGameplayModOp::MultiplyAdditive, 2.0f }});
	TestTrue(TEXT("Cap fixture multiplier is active"), CapMultiply.IsValid());
	Fixture.ApplyHealing(Target, Target, 10.0f, Target.Actor);
	TestEqual(TEXT("Healing10 restores Current200"), Target.HealthSet->GetHealth(), 200.0f);
	Fixture.ApplyPoiseDamage(Target, Target, 10.0f, Target.Actor);
	TestEqual(TEXT("PoiseDamage10 consumes Current10 once"), Target.HealthSet->GetPoise(), 190.0f);
	TestEqual(TEXT("PoiseDamage10 leaves Base95"), Target.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetPoiseAttribute()), 95.0f);
	Target.HealthSet->SetMaxHealth(150.0f);
	Target.HealthSet->SetMaxPoise(150.0f);
	TestEqual(TEXT("MaxHealth reduction commits Current150"), Target.HealthSet->GetHealth(), 150.0f);
	TestEqual(TEXT("MaxPoise reduction commits Current150"), Target.HealthSet->GetPoise(), 150.0f);
	TestEqual(TEXT("Health cap inversion leaves Base75"), Target.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute()), 75.0f);
	TestEqual(TEXT("Poise cap inversion leaves Base75"), Target.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetPoiseAttribute()), 75.0f);
	TestTrue(TEXT("Settlement retains the actual GE"), Target.ASC->GetActiveGameplayEffect(CapMultiply) != nullptr);
	Target.ASC->RemoveActiveGameplayEffect(CapMultiply);
	TestEqual(TEXT("Removing multiplier reveals settled Health75"), Target.HealthSet->GetHealth(), 75.0f);
	TestEqual(TEXT("Removing multiplier reveals settled Poise75"), Target.HealthSet->GetPoise(), 75.0f);
	TestEqual(TEXT("Cap and removal add no damage messages"), Messages.Num(), 1);
	if (Messages.Num() == 1) { TestEqual(TEXT("Damage message retains raw10"), Messages[0].Magnitude, 10.0f); }

	Offset.HealthSet->SetMaxHealth(500.0f);
	int32 DeathEdges = 0;
	Fixture.TrackAttributeDelegate(Offset.HealthSet->OnOutOfHealth, Offset.HealthSet->OnOutOfHealth.AddLambda(
		[&DeathEdges](AActor*, AActor*, const FGameplayEffectSpec*, float, float, float) { ++DeathEdges; }));
	const FActiveGameplayEffectHandle Add = ApplyPersistentNumericModifiers(Fixture, Offset,
		{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::Additive, 200.0f }});
	TestTrue(TEXT("Persistent additive effect is active"), Add.IsValid());
	Fixture.ApplyDamage(Offset, Offset, 110.0f, Offset.Actor);
	TestEqual(TEXT("Base100 plus200 minus110 is Current190"), Offset.HealthSet->GetHealth(), 190.0f);
	TestEqual(TEXT("Additive settlement permits finite Base minus10"), Offset.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute()), -10.0f);
	Offset.ASC->RemoveActiveGameplayEffect(Add);
	TestEqual(TEXT("Buff removal clamps negative Base to Current0"), Offset.HealthSet->GetHealth(), 0.0f);
	TestEqual(TEXT("Buff removal uses the existing death edge"), DeathEdges, 1);
	TestEqual(TEXT("Buff removal does not invent damage"), Messages.Num(), 2);

	const FActiveGameplayEffectHandle Half = ApplyPersistentNumericModifiers(Fixture, AboveMax,
		{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::MultiplyAdditive, 0.5f }});
	TestTrue(TEXT("Half multiplier is active"), Half.IsValid());
	Fixture.ApplyHealing(AboveMax, AboveMax, 80.0f, AboveMax.Actor);
	TestEqual(TEXT("Healing clamps visible Current to100"), AboveMax.HealthSet->GetHealth(), 100.0f);
	TestEqual(TEXT("Finite Base200 may exceed Max100"), AboveMax.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute()), 200.0f);
	AboveMax.ASC->RemoveActiveGameplayEffect(Half);
	TestEqual(TEXT("Removal keeps visible Current within Max"), AboveMax.HealthSet->GetHealth(), 100.0f);

	Reentry.HealthSet->SetMaxHealth(500.0f);
	const FActiveGameplayEffectHandle ReentryMultiply = ApplyPersistentNumericModifiers(Fixture, Reentry,
		{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::MultiplyAdditive, 2.0f }});
	bool bSwapped = false;
	bool bHealed = false;
	Fixture.TrackNativeDelegate(Reentry.ASC, UGGYGOHealthSet::GetDamageAttribute(),
		Reentry.ASC->GetGameplayAttributeValueChangeDelegate(UGGYGOHealthSet::GetDamageAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bSwapped && Data.OldValue > 0.0f && Data.NewValue == 0.0f)
			{
				bSwapped = true;
				Reentry.ASC->RemoveActiveGameplayEffect(ReentryMultiply);
				ApplyPersistentNumericModifiers(Fixture, Reentry,
					{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::Additive, 200.0f }});
			}
		}));
	Fixture.TrackNativeDelegate(Reentry.ASC, UGGYGOHealthSet::GetHealthAttribute(),
		Reentry.ASC->GetGameplayAttributeValueChangeDelegate(UGGYGOHealthSet::GetHealthAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bHealed && Data.NewValue == 290.0f)
			{
				bHealed = true;
				Fixture.ApplyHealing(Reentry, Reentry, 10.0f, Reentry.Actor);
			}
		}));
	TArray<float> ReentryResults;
	Fixture.TrackAttributeDelegate(Reentry.HealthSet->OnHealthChanged, Reentry.HealthSet->OnHealthChanged.AddLambda(
		[&](AActor*, AActor*, const FGameplayEffectSpec*, float, float, float NewValue) { ReentryResults.Add(NewValue); }));
	Fixture.ApplyDamage(Reentry, Reentry, 10.0f, Reentry.Actor);
	TestTrue(TEXT("Meta-clear callback changed actual aggregation"), bSwapped);
	TestTrue(TEXT("Native callback reentered after original Current290 write"), bHealed);
	TestEqual(TEXT("Post recalculates and nested healing leaves Current300"), Reentry.HealthSet->GetHealth(), 300.0f);
	TestEqual(TEXT("Reentry uses new additive aggregation Base100"), Reentry.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute()), 100.0f);
	TestTrue(TEXT("Original290 remains a recorded fact after later300"), ReentryResults.Contains(290.0f) && ReentryResults.Last() == 300.0f);
	TestEqual(TEXT("Original damage still publishes once after later write"), Messages.Num(), 3);
	TestEqual(TEXT("Reentry consumes Damage meta"), Reentry.HealthSet->GetDamage(), 0.0f);
	TestEqual(TEXT("Reentry consumes Healing meta"), Reentry.HealthSet->GetHealing(), 0.0f);

	const FActiveGameplayEffectHandle Override = ApplyPersistentNumericModifiers(Fixture, Rejected,
		{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::Override, 50.0f }});
	TestTrue(TEXT("Noninvertible effect remains real and active"), Override.IsValid());
	const FGGYGOAttributeBaseCalculationResult RejectCalculation = Rejected.ASC->TryCalculateNumericAttributeBaseForCurrentValue(
		Rejected.HealthSet, UGGYGOHealthSet::GetHealthAttribute(), 50.0f, 40.0f);
	TestTrue(TEXT("Override has an explicit rejection"), RejectCalculation.Outcome == EGGYGOAttributeBaseCalculationOutcome::Rejected
		&& RejectCalculation.Reason == EGGYGOAttributeBaseCalculationReason::NonInvertibleChannel);
	AddExpectedErrorPlain(TEXT("[Messages][HealthSet] MetaRejected"), EAutomationExpectedErrorFlags::Contains, 1);
	Fixture.ApplyDamage(Rejected, Rejected, 10.0f, Rejected.Actor);
	TestEqual(TEXT("Rejected meta never enters storage"), Rejected.HealthSet->GetDamage(), 0.0f);
	TestEqual(TEXT("Rejected inverse never changes Current"), Rejected.HealthSet->GetHealth(), 50.0f);
	TestEqual(TEXT("Rejected inverse never changes Base"), Rejected.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute()), 100.0f);
	TestEqual(TEXT("Rejected damage publishes no success message"), Messages.Num(), 3);
	bool bMadeNoninvertible = false;
	Fixture.TrackNativeDelegate(PostRejected.ASC, UGGYGOHealthSet::GetDamageAttribute(),
		PostRejected.ASC->GetGameplayAttributeValueChangeDelegate(UGGYGOHealthSet::GetDamageAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bMadeNoninvertible && Data.OldValue > 0.0f && Data.NewValue == 0.0f)
			{
				bMadeNoninvertible = true;
				ApplyPersistentNumericModifiers(Fixture, PostRejected,
					{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::Override, 50.0f }});
			}
		}));
	AddExpectedErrorPlain(TEXT("[Messages][HealthSet] CurrentWriteRejected"), EAutomationExpectedErrorFlags::Contains, 1);
	Fixture.ApplyDamage(PostRejected, PostRejected, 10.0f, PostRejected.Actor);
	TestTrue(TEXT("Meta-clear callback can invalidate a previously usable inverse"), bMadeNoninvertible);
	TestEqual(TEXT("Post rejection still retires its consumed meta"), PostRejected.HealthSet->GetDamage(), 0.0f);
	TestEqual(TEXT("Post rejection preserves callback's real Current50"), PostRejected.HealthSet->GetHealth(), 50.0f);
	TestEqual(TEXT("Post rejection removes only its unconfirmed damage candidate"), Messages.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthMessageClientMaxNetReceiveTest,
	"GGYGO.AbilitySystem.HealthMessage.ClientMaxNetReceive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHealthMessageClientMaxNetReceiveTest::RunTest(const FString& Parameters)
{
	FGGYGOHealthMessageTestFixture Fixture;
	if (!TestTrue(TEXT("NetReceive fixture initializes"), Fixture.Initialize())) { return false; }
	const FGGYGOHealthMessageCombatant Target = Fixture.CreateCombatant();
	if (!TestNotNull(TEXT("Client HealthSet exists"), Target.HealthSet)) { return false; }
	Target.HealthSet->SetMaxHealth(500.0f);
	Target.HealthSet->SetMaxPoise(500.0f);
	const FActiveGameplayEffectHandle Persistent = ApplyPersistentNumericModifiers(Fixture, Target,
		{{ UGGYGOHealthSet::GetHealthAttribute(), EGameplayModOp::Additive, 200.0f },
		 { UGGYGOHealthSet::GetPoiseAttribute(), EGameplayModOp::Additive, 200.0f },
		 { UGGYGOHealthSet::GetMaxHealthAttribute(), EGameplayModOp::Additive, 0.0f }});
	TestTrue(TEXT("Client projection retains an actual persistent GE"), Persistent.IsValid());
	Target.Actor->SetTestNetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("Native ASC authority cache reflects simulated role"), Target.ASC->IsOwnerActorAuthoritative());
	int32 DamageMessages = 0;
	int32 BreakMessages = 0;
	int32 HealthChanges = 0;
	int32 PoiseChanges = 0;
	Fixture.Listen(GGYGOGameplayTags::Message_Damage, [&](FGameplayTag, const FGGYGOVerbMessage&) { ++DamageMessages; });
	Fixture.Listen(GGYGOGameplayTags::Message_PoiseBreak, [&](FGameplayTag, const FGGYGOVerbMessage&) { ++BreakMessages; });
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnHealthChanged, Target.HealthSet->OnHealthChanged.AddLambda(
		[&](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float, float OldValue, float NewValue)
		{
			++HealthChanges;
			TestTrue(TEXT("Client cap has no fabricated GE source"), !Instigator && !Causer && !Spec);
			TestEqual(TEXT("Client cap old Health is the real300"), OldValue, 300.0f);
			TestEqual(TEXT("Client cap new Health is150"), NewValue, 150.0f);
		}));
	Fixture.TrackAttributeDelegate(Target.HealthSet->OnPoiseChanged, Target.HealthSet->OnPoiseChanged.AddLambda(
		[&](AActor*, AActor*, const FGameplayEffectSpec*, float, float OldValue, float NewValue)
		{
			++PoiseChanges;
			if (PoiseChanges == 1)
			{
				TestEqual(TEXT("Client cap old Poise is the real300"), OldValue, 300.0f);
				TestEqual(TEXT("Client cap new Poise is150"), NewValue, 150.0f);
			}
		}));
	Target.HealthSet->BeginTestNetReceive();
	Target.HealthSet->SimulateReplicatedMaxHealth(150.0f);
	Target.HealthSet->SimulateReplicatedMaxPoise(150.0f);
	TestEqual(TEXT("Health dependency waits for native batch end"), Target.HealthSet->GetHealth(), 300.0f);
	TestEqual(TEXT("Poise dependency waits for native batch end"), Target.HealthSet->GetPoise(), 300.0f);
	TestEqual(TEXT("No deferred Health project result publishes early"), HealthChanges, 0);
	TestEqual(TEXT("No deferred Poise project result publishes early"), PoiseChanges, 0);
	Target.HealthSet->EndTestNetReceive();
	TestEqual(TEXT("NetReceive end reevaluates Health within Max"), Target.HealthSet->GetHealth(), 150.0f);
	TestEqual(TEXT("NetReceive end reevaluates Poise within Max"), Target.HealthSet->GetPoise(), 150.0f);
	TestEqual(TEXT("Client Health retains real Base100"), Target.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute()), 100.0f);
	TestEqual(TEXT("Client Poise retains real Base100"), Target.ASC->GetNumericAttributeBase(UGGYGOHealthSet::GetPoiseAttribute()), 100.0f);
	TestTrue(TEXT("Client projection leaves GE active"), Target.ASC->GetActiveGameplayEffect(Persistent) != nullptr);
	TestEqual(TEXT("Client Health publishes only the final real change"), HealthChanges, 1);
	TestEqual(TEXT("Client Poise publishes only the final real change"), PoiseChanges, 1);
	TestEqual(TEXT("Client dependency invents no damage message"), DamageMessages, 0);
	TestEqual(TEXT("Client dependency invents no poise message"), BreakMessages, 0);
	bool bNestedDirectWrite = false;
	Fixture.TrackNativeDelegate(Target.ASC, UGGYGOHealthSet::GetPoiseAttribute(),
		Target.ASC->GetGameplayAttributeValueChangeDelegate(UGGYGOHealthSet::GetPoiseAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Data)
		{
			if (!bNestedDirectWrite && Data.NewValue == 100.0f)
			{
				bNestedDirectWrite = true;
				Target.HealthSet->SetPoise(-200.0f);
			}
		}));
	// Also cover the unbatched native OnRep path and a real nested write after its marker.
	Target.HealthSet->SimulateReplicatedMaxPoise(100.0f);
	TestTrue(TEXT("Client cap native callback may make a distinct direct write"), bNestedDirectWrite);
	TestEqual(TEXT("Distinct nested direct write commits Poise0"), Target.HealthSet->GetPoise(), 0.0f);
	TestEqual(TEXT("Only the real nested write publishes PoiseBreak"), BreakMessages, 1);
	TestEqual(TEXT("Cap and real nested write both preserve their changes"), PoiseChanges, 3);
	Target.HealthSet->BeginTestNetReceive();
	Target.HealthSet->EndTestNetReceive();
	TestEqual(TEXT("A later empty receive cannot replay Health request"), HealthChanges, 1);
	TestEqual(TEXT("A later empty receive cannot replay Poise request"), PoiseChanges, 3);
	Target.Actor->SetTestNetRole(ROLE_Authority);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
