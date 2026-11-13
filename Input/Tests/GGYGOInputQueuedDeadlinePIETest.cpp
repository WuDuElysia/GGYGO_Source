/** @file GGYGOInputQueuedDeadlinePIETest.cpp @brief 生产普攻输入链在临时组占用下的有限重试观察。 */
#include "Input/Tests/GGYGOInputTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "AbilitySystem/Abilities/GGYGOPlayerComboAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tests/GGYGOAbilityAdmissionTestTypes.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Input/GGYGOInputConfig.h"
#include "Input/GGYGOPlayerInput.h"
#include "InputAction.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "System/GGYGOGameplayTags.h"
#include "Editor.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	/** 只拥有本测试新建的 PIE；占组夹具由真实 GAS 授予，输入与被重试能力保留生产配置。 */
	class FQueuedDeadlinePIE : public IAutomationLatentCommand
	{
	public:
		FQueuedDeadlinePIE(FAutomationTestBase& InTest, bool bInExpire) : Test(InTest), bExpire(bInExpire) {}
		virtual ~FQueuedDeadlinePIE() override
		{
			ReleaseObservers();
			if (OwnsPIE()) { GEditor->EndPlayMap(); }
		}

		virtual bool Update() override
		{
			if (Stage == EStage::Start) { Start(); return false; }
			if (Stage == EStage::Closing)
			{
				if (!OwnsPIE()) { return true; }
				if (FPlatformTime::Seconds() > Timeout)
				{
					Test.AddError(TEXT("自有 PIE 退出超时")); GEditor->EndPlayMap(); return true;
				}
				return false;
			}
			if (FPlatformTime::Seconds() > Timeout) { Fail(TEXT("生产输入观察超时")); return false; }
			if (!OwnsPIE()) { Fail(TEXT("本测试的 PIE 会话丢失")); return false; }
			if (Stage == EStage::Ready) { Prepare(); return false; }
			if (!World.IsValid() || GEditor->PlayWorld != World.Get() || !PC.IsValid() || !ASC.IsValid()
				|| !Pawn.IsValid() || PC->GetPawn() != Pawn.Get() || PC->PlayerInput != PlayerInput.Get()
				|| ASC->GetAvatarActor() != Pawn.Get()) { Fail(TEXT("原生产输入装配已变化")); return false; }
			const double Now = World->GetTimeSeconds();
			if (Stage == EStage::Queued)
			{
				if (Notices == 0) { return false; }
				if (bKeyDown) { SendKey(false); }
				if (bExpire && Blocker.IsValid() && Blocker->IsActive() && Now > Original.OriginalDeadline + 0.05)
				{
					Blocker->FinishForTest();
					ReleasedAt = Now;
				}
				if (FreedCount == 0) { return false; }
				if (bExpire)
				{
					if (Now < ReleasedAt + 0.35) { return false; }
					if (Activations != 0) { Fail(TEXT("超过原截止的请求仍激活了生产普攻")); return false; }
					Test.AddInfo(FString::Printf(TEXT("[Input.QueuedDeadlinePIE] Expired OriginalDeadline=%.9f ReleasedAt=%.9f ObservedUntil=%.9f Activations=0"), Original.OriginalDeadline, ReleasedAt, Now));
					Stage = EStage::Control;
					SendKey(true);
				}
				else if (Activations == 1)
				{
					if (!Test.TestTrue(TEXT("生产能力在原截止前真实激活"), ActivatedAt < Original.OriginalDeadline)) { Close(); return false; }
					Stage = EStage::NaturalEnd;
				}
			}
			else if (Stage == EStage::Control && Activations == 1)
			{
				SendKey(false);
				Stage = EStage::NaturalEnd;
			}
			else if (Stage == EStage::NaturalEnd && !Combo->IsActive())
			{
				EndedAt = Now;
				Stage = EStage::Quiet;
			}
			else if (Stage == EStage::Quiet && Now > EndedAt + 0.35)
			{
				Test.TestEqual(TEXT("生产普攻只激活一次，无残留重试"), Activations, 1);
				Test.TestEqual(TEXT("生产组占用只失败一次"), QueuedFailures, 1);
				Test.TestEqual(TEXT("仅原请求产生一次重试通知"), Notices, 1);
				Test.TestEqual(TEXT("无其它激活失败"), OtherFailures, 0);
				Test.TestEqual(TEXT("生产能力发生一次原生结束"), EndCount, 1);
				Test.TestFalse(TEXT("生产能力自然完成而非取消"), bCancelled);
				Test.AddInfo(FString::Printf(TEXT("[Input.QueuedDeadlinePIE] Case=%s Request=%llu Deadline=%.9f ActivatedAt=%.9f Activations=%d QueuedFailures=%d Notices=%d NaturalEnd=1; DefaultPawnInput=1 TemporaryBlocker=1 ProductionGroupUnchanged=1 PhysicalDevice=0"),
					bExpire ? TEXT("Expired") : TEXT("RetryOnce"), static_cast<unsigned long long>(Original.Identity.RequestSerial),
					Original.OriginalDeadline, ActivatedAt, Activations, QueuedFailures, Notices));
				Close();
			}
			return false;
		}

	private:
		enum class EStage { Start, Ready, Queued, Control, NaturalEnd, Quiet, Closing };
		FAutomationTestBase& Test;
		bool bExpire;
		EStage Stage = EStage::Start;
		TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AGGYGOPlayerController> PC;
		TWeakObjectPtr<APawn> Pawn;
		TWeakObjectPtr<UGGYGOPlayerInput> PlayerInput;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakObjectPtr<UGGYGOPlayerComboAbility> Combo;
		TWeakObjectPtr<UGGYGOAbilityAdmissionQueuedTestAbility> Blocker;
		FGameplayAbilitySpecHandle BlockerHandle;
		FDelegateHandle FailedHandle, RetryHandle, FreedHandle, ActivatedHandle, EndHandle;
		FGGYGOAbilityInputRetryRequest Original;
		FKey Key;
		bool bKeyDown = false;
		bool bCancelled = false;
		int32 EndCount = 0;
		int32 Notices = 0, QueuedFailures = 0, OtherFailures = 0, FreedCount = 0, Activations = 0;
		double Timeout = 0, PressedAt = 0, ReleasedAt = 0, ActivatedAt = 0, EndedAt = 0;

		bool OwnsPIE() const
		{
			if (!GEditor || !Settings.IsValid()) { return false; }
			const TOptional<FPlayInEditorSessionInfo> Info = GEditor->GetPlayInEditorSessionInfo();
			if (Info.IsSet()) { return Info->OriginalRequestParams.EditorPlaySettings == Settings.Get(); }
			const TOptional<FRequestPlaySessionParams> Queued = GEditor->GetPlaySessionRequest();
			return Queued.IsSet() && Queued->EditorPlaySettings == Settings.Get();
		}
		void Start()
		{
			if (!GEditor || GEditor->IsPlaySessionInProgress() || GEditor->IsSettingUpPlayWorld()
				|| GEditor->GetPlaySessionRequest().IsSet()) { Fail(TEXT("已有或排队 PIE，不能接管")); return; }
			UWorld* Map = GEditor->GetEditorWorldContext().World();
			if (!Map || Map->GetPackage()->GetName() != TEXT("/Game/Map/L_Movement_Test")) { Fail(TEXT("必须从生产 L_Movement_Test 启动")); return; }
			Settings.Reset(NewObject<ULevelEditorPlaySettings>(GetTransientPackage()));
			Settings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
			Settings->SetRunUnderOneProcess(true);
			Settings->SetPlayNumberOfClients(1);
			Settings->bLaunchSeparateServer = false;
			FRequestPlaySessionParams Params;
			Params.EditorPlaySettings = Settings.Get();
			Params.bAllowOnlineSubsystem = false;
			GEditor->RequestPlaySession(Params);
			const TOptional<FRequestPlaySessionParams> Queued = GEditor->GetPlaySessionRequest();
			if (!Queued.IsSet()) { Fail(TEXT("原生 PIE 请求未建立")); return; }
			Settings.Reset(Queued->EditorPlaySettings.Get());
			Timeout = FPlatformTime::Seconds() + 120.0;
			Stage = EStage::Ready;
		}
		void Prepare()
		{
			UWorld* W = GEditor->PlayWorld;
			if (!W || !W->HasBegunPlay() || !W->GetGameInstance()) { return; }
			ULocalPlayer* LP = W->GetGameInstance()->GetFirstGamePlayer();
			auto* Controller = LP ? Cast<AGGYGOPlayerController>(LP->GetPlayerController(W)) : nullptr;
			APawn* Avatar = Controller ? Controller->GetPawn() : nullptr;
			auto* Extension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Avatar);
			auto* AbilitySystem = Extension ? Extension->GetGGYGOAbilitySystemComponent() : nullptr;
			auto* Source = Controller ? Cast<UGGYGOPlayerInput>(Controller->PlayerInput) : nullptr;
			if (!AbilitySystem || !Source || !Controller->IsLocalController()) { return; }
			FString Reason;
			if (!Extension->AreAbilityInputResourcesReady(GGYGOGameplayTags::InputTag_Attack_Light, Reason)) { return; }
			UGGYGOPlayerComboAbility* Production = nullptr;
			int32 Matches = 0;
			for (const FGameplayAbilitySpec& Spec : AbilitySystem->GetActivatableAbilities())
			{
				if (Spec.GetDynamicSpecSourceTags().HasTagExact(GGYGOGameplayTags::InputTag_Attack_Light))
				{
					++Matches;
					Production = Cast<UGGYGOPlayerComboAbility>(Spec.GetPrimaryInstance());
				}
			}
			if (Matches != 1 || !Production) { Fail(TEXT("默认授予缺少唯一生产普攻")); return; }
			const auto* GroupConfig = AbilitySystem->GetAbilityGroupConfig();
			if (!GroupConfig || Production->GetGroupTag() != GGYGOGameplayTags::AbilityGroup_Attack
				|| GroupConfig->GetRuleForGroup(Production->GetGroupTag()).Rule != EGGYGOAbilityGroupRule::SingleInstanceQueued)
			{
				Fail(TEXT("生产 Attack 组不是 Queued；不修改配置制造场景")); return;
			}
			const UInputAction* Action = LoadObject<UInputAction>(nullptr, TEXT("/Game/Input/Actions/IA_Attack.IA_Attack"));
			auto* Subsystem = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
			if (!Action || !Subsystem) { Fail(TEXT("生产 IA_Attack／Subsystem 缺失")); return; }
			const TArray<FKey> Keys = Subsystem->QueryKeysMappedToAction(Action);
			if (Keys.IsEmpty()) { return; }
			Key = Keys[0];
			if (Key.IsAnalog() || Key.IsGamepadKey() || Source->IsPressed(Key)) { Fail(TEXT("所选生产按键不可用或已由他人按住")); return; }
			World = W; PC = Controller; Pawn = Avatar; PlayerInput = Source; ASC = AbilitySystem; Combo = Production;
			FGameplayAbilitySpec Spec(UGGYGOAbilityAdmissionQueuedTestAbility::StaticClass(), 1);
			BlockerHandle = ASC->GiveAbility(Spec);
			const FGameplayAbilitySpec* Granted = ASC->FindAbilitySpecFromHandle(BlockerHandle);
			Blocker = Granted ? Cast<UGGYGOAbilityAdmissionQueuedTestAbility>(Granted->GetPrimaryInstance()) : nullptr;
			if (!Blocker.IsValid() || !ASC->TryActivateAbility(BlockerHandle)) { Fail(TEXT("临时占用者未真实入场")); return; }
			FailedHandle = ASC->AbilityFailedCallbacks.AddLambda([this](const UGameplayAbility* Failed, const FGameplayTagContainer& Reasons)
			{
				if (Failed != Combo.Get()) { return; }
				if (Reasons.HasTagExact(GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued)) { ++QueuedFailures; }
				else { ++OtherFailures; }
			});
			FreedHandle = ASC->OnAbilityGroupFreed.AddLambda([this](FGameplayTag Group)
			{
				if (Group == GGYGOGameplayTags::AbilityGroup_Attack) { ++FreedCount; }
			});
			ActivatedHandle = ASC->AbilityActivatedCallbacks.AddLambda([this](UGameplayAbility* Ability)
			{
				if (Ability == Combo.Get()) { ++Activations; ActivatedAt = World->GetTimeSeconds(); }
			});
			EndHandle = ASC->OnAbilityEnded.AddLambda([this](const FAbilityEndedData& Data)
			{
				if (Data.AbilityThatEnded == Combo.Get()) { ++EndCount; bCancelled |= Data.bWasCancelled; }
			});
			RetryHandle = ASC->OnAbilityInputRetryable.AddLambda([this](const FGGYGOAbilityInputRetryRequest& Request)
			{
				++Notices; Original = Request;
				Test.TestTrue(TEXT("生产首按有原 ASC 身份及有限截止"), Request.Identity.SourceASC.Get() == ASC.Get()
					&& Request.Identity.IsAssigned() && FMath::IsFinite(Request.OriginalDeadline) && Request.OriginalDeadline > PressedAt);
				if (!bExpire && Blocker->IsActive())
				{
					// 真实 Queued 失败后结束占用者；下一次原生 PostProcessInput 才消费重试。
					Blocker->FinishForTest(); ReleasedAt = World->GetTimeSeconds();
					Test.TestEqual(TEXT("生产组释放不在广播栈激活"), Activations, 0);
				}
			});
			Test.AddInfo(FString::Printf(TEXT("[Input.QueuedDeadlinePIE] Pawn=%s PC=%s PlayerInput=%s GA=%s Key=%s GroupConfig=%s Rule=SingleInstanceQueued TemporaryNoInputTagBlocker=%s"),
				*Avatar->GetPathName(), *Controller->GetPathName(), *Source->GetPathName(), *Production->GetClass()->GetPathName(),
				*Key.ToString(), *GroupConfig->GetPathName(), *Blocker->GetPathName()));
			Stage = EStage::Queued;
			Timeout = FPlatformTime::Seconds() + 20.0;
			PressedAt = W->GetTimeSeconds();
			SendKey(true);
		}
		void SendKey(bool bDown)
		{
			bKeyDown = bDown;
			PC->InputKey(FInputKeyEventArgs(nullptr, FInputDeviceId::CreateFromInternalId(0), Key,
				bDown ? IE_Pressed : IE_Released, bDown ? 1.0f : 0.0f, false, FPlatformTime::Cycles64()));
		}
		void ReleaseObservers()
		{
			if (ASC.IsValid())
			{
				ASC->AbilityFailedCallbacks.Remove(FailedHandle);
				ASC->OnAbilityInputRetryable.Remove(RetryHandle);
				ASC->OnAbilityGroupFreed.Remove(FreedHandle);
				ASC->AbilityActivatedCallbacks.Remove(ActivatedHandle);
				ASC->OnAbilityEnded.Remove(EndHandle);
				if (BlockerHandle.IsValid()) { ASC->ClearAbility(BlockerHandle); BlockerHandle = {}; }
			}
			if (bKeyDown && PC.IsValid() && PC->PlayerInput == PlayerInput.Get()) { SendKey(false); }
		}
		void Close()
		{
			Stage = EStage::Closing;
			ReleaseObservers();
			if (OwnsPIE()) { GEditor->RequestEndPlayMap(); }
			Timeout = FPlatformTime::Seconds() + 15.0;
		}
		void Fail(const FString& Message) { Test.AddError(Message); Close(); }
	};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FGGYGOInputQueuedDeadlinePIETest, "GGYGO.Input.QueuedDeadlinePIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FGGYGOInputQueuedDeadlinePIETest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("GGYGOInputQueuedDeadlinePIE"))) { return; }
	for (const TCHAR* Name : {TEXT("RetryOnce"), TEXT("Expired")}) { Names.Add(Name); Commands.Add(Name); }
}

bool FGGYGOInputQueuedDeadlinePIETest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FQueuedDeadlinePIE(*this, Parameters == TEXT("Expired")));
	return true;
}
#endif
