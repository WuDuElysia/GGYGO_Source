/** @file GGYGOInputQueuedDeadlineTest.cpp @brief 经真实输入入口验证组排队的单次消费与原截止。 */
#include "Input/Tests/GGYGOInputTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tests/GGYGOAbilityAdmissionTestTypes.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FQueuedDeadlineScenario
	{
		FGGYGOInputTestFixture Input;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		TStrongObjectPtr<UGGYGOAbilityAdmissionTestGroupConfig> Config;
		FGameplayAbilitySpecHandle BlockerHandle;
		FGameplayAbilitySpecHandle RequestHandle;
		UGGYGOAbilityAdmissionQueuedTestAbility* Blocker = nullptr;
		UGGYGOAbilityAdmissionQueuedTestAbility* Request = nullptr;
		FDelegateHandle FailedHandle;
		FDelegateHandle RetryHandle;
		FDelegateHandle FreedHandle;
		TArray<FGGYGOAbilityInputRetryRequest> Notices;
		int32 QueuedFailures = 0;
		int32 OtherFailures = 0;
		int32 FreedCount = 0;

		~FQueuedDeadlineScenario()
		{
			if (!ASC) { return; }
			ASC->AbilityFailedCallbacks.Remove(FailedHandle);
			ASC->OnAbilityInputRetryable.Remove(RetryHandle);
			ASC->OnAbilityGroupFreed.Remove(FreedHandle);
			Input.GetPawn()->GetHeroForTest()->ReleasePlayerInput();
			if (Blocker && Blocker->IsActive()) { Blocker->FinishForTest(); }
			if (Request && Request->IsActive()) { Request->FinishForTest(); }
			if (BlockerHandle.IsValid()) { ASC->ClearAbility(BlockerHandle); }
			if (RequestHandle.IsValid()) { ASC->ClearAbility(RequestHandle); }
			ASC->SetAbilityGroupConfig(nullptr);
		}

		bool Initialize(FAutomationTestBase& Test)
		{
			Input.MapAbilityKeyForTest(EKeys::J);
			if (!Input.Initialize(Test) || !Input.CompleteNativeMappingRebuildForTest(Test)) { return false; }
			ASC = Input.GetPawn()->GetASCForTest();
			Config.Reset(NewObject<UGGYGOAbilityAdmissionTestGroupConfig>(ASC));
			Config->SetRuleForTest(GGYGOGameplayTags::AbilityGroup_Attack, EGGYGOAbilityGroupRule::SingleInstanceQueued);
			ASC->SetAbilityGroupConfig(Config.Get());
			FGameplayAbilitySpec BlockerSpec(UGGYGOAbilityAdmissionQueuedTestAbility::StaticClass(), 1);
			BlockerHandle = ASC->GiveAbility(BlockerSpec);
			FGameplayAbilitySpec RequestSpec(UGGYGOAbilityAdmissionQueuedTestAbility::StaticClass(), 1);
			RequestSpec.GetDynamicSpecSourceTags().AddTag(GGYGOGameplayTags::InputTag_Attack_Light);
			RequestHandle = ASC->GiveAbility(RequestSpec);
			const FGameplayAbilitySpec* GrantedBlocker = ASC->FindAbilitySpecFromHandle(BlockerHandle);
			const FGameplayAbilitySpec* GrantedRequest = ASC->FindAbilitySpecFromHandle(RequestHandle);
			Blocker = GrantedBlocker ? Cast<UGGYGOAbilityAdmissionQueuedTestAbility>(GrantedBlocker->GetPrimaryInstance()) : nullptr;
			Request = GrantedRequest ? Cast<UGGYGOAbilityAdmissionQueuedTestAbility>(GrantedRequest->GetPrimaryInstance()) : nullptr;
			if (!Test.TestNotNull(TEXT("占用者 GAS 实例"), Blocker)
				|| !Test.TestNotNull(TEXT("输入能力 GAS 实例"), Request)) { return false; }
			FailedHandle = ASC->AbilityFailedCallbacks.AddLambda([this](const UGameplayAbility* Ability, const FGameplayTagContainer& Reasons)
			{
				if (Ability != Request) { return; }
				if (Reasons.HasTagExact(GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued)) { ++QueuedFailures; }
				else { ++OtherFailures; }
			});
			RetryHandle = ASC->OnAbilityInputRetryable.AddLambda([this](const FGGYGOAbilityInputRetryRequest& Original)
			{
				Notices.Add(Original);
			});
			FreedHandle = ASC->OnAbilityGroupFreed.AddLambda([this](FGameplayTag Group)
			{
				if (Group == GGYGOGameplayTags::AbilityGroup_Attack) { ++FreedCount; }
			});
			return Test.TestTrue(TEXT("占用者经 GAS 入场"), ASC->TryActivateAbility(BlockerHandle));
		}

		void ConsumeFrames(int32 Count = 3)
		{
			for (int32 Frame = 0; Frame < Count; ++Frame) { Input.GetController()->TickInputForTest(1.0f / 60.0f); }
		}

		bool AdvancePastDeadline(FAutomationTestBase& Test, double Deadline)
		{
			// 私有 World 的原生计时推进；不改输入记录，也不手动调用重试或广播组释放。
			Input.GetWorld()->Tick(LEVELTICK_TimeOnly, static_cast<float>(Deadline - Input.GetWorld()->GetTimeSeconds() + 0.05));
			return Test.TestTrue(TEXT("实际 World 时间已超过首按的原截止"),
				static_cast<double>(Input.GetWorld()->GetTimeSeconds()) > Deadline);
		}
	};

	bool RunQueuedDeadlineCase(FAutomationTestBase& Test, const FString& Case)
	{
		const bool bRequeue = Case == TEXT("RequeueKeepsOriginalDeadline");
		const bool bExpireWaiting = Case == TEXT("ExpiredBeforeGroupFreed");
		const bool bExpirePending = Case == TEXT("ExpiredBeforeConsumption");
		const bool bExpires = bExpireWaiting || bExpirePending;
		FQueuedDeadlineScenario S;
		if (!S.Initialize(Test)) { return false; }
		const double PressTime = S.Input.GetWorld()->GetTimeSeconds();
		if (!S.Input.PressAbilityKeyForTest(Test)
			|| !Test.TestEqual(TEXT("真实键评价产生一次 Queued 拒绝"), S.QueuedFailures, 1)
			|| !Test.TestEqual(TEXT("原失败发布一次请求观察"), S.Notices.Num(), 1)) { return false; }
		const FGGYGOAbilityInputRetryRequest Original = S.Notices[0];
		if (!Test.TestTrue(TEXT("原请求身份由当前 ASC 签发"), Original.Identity.IsAssigned() && Original.Identity.SourceASC.Get() == S.ASC)
			|| !Test.TestEqual(TEXT("首按固定绝对截止"), Original.OriginalDeadline,
				PressTime + static_cast<double>(S.Input.GetPawn()->GetHeroForTest()->InputBufferWindow))) { return false; }
		S.ConsumeFrames();
		if (!Test.TestEqual(TEXT("按住不会绕过组释放逐帧重试"), S.QueuedFailures, 1)
			|| !Test.TestEqual(TEXT("占用期间尚未激活"), S.Request->GetBusinessActivationCountForTest(), 0)) { return false; }

		if (bExpireWaiting && !S.AdvancePastDeadline(Test, Original.OriginalDeadline)) { return false; }
		S.Blocker->FinishForTest();
		if (!Test.TestEqual(TEXT("真实 EndAbility 释放原组一次"), S.FreedCount, 1)
			|| !Test.TestEqual(TEXT("组释放广播栈不激活请求"), S.Request->GetBusinessActivationCountForTest(), 0)) { return false; }
		if (bExpirePending && !S.AdvancePastDeadline(Test, Original.OriginalDeadline)) { return false; }
		if (bRequeue)
		{
			if (!Test.TestTrue(TEXT("消费前由真实能力重新占组"), S.ASC->TryActivateAbility(S.BlockerHandle))) { return false; }
			S.ConsumeFrames();
			if (!Test.TestEqual(TEXT("一次唤醒只尝试一次"), S.QueuedFailures, 2)
				|| !Test.TestEqual(TEXT("再次真实 Queued 失败才续排"), S.Notices.Num(), 2)
				|| !Test.TestTrue(TEXT("再次失败保留原请求身份"), S.Notices[1].Identity == Original.Identity)
				|| !Test.TestEqual(TEXT("再次失败不延长原截止"), S.Notices[1].OriginalDeadline, Original.OriginalDeadline)) { return false; }
			S.Blocker->FinishForTest();
		}
		S.ConsumeFrames();
		const int32 ExpectedActivations = bExpires ? 0 : 1;
		if (!Test.TestEqual(TEXT("截止决定本次有限重试是否入场"), S.Request->GetBusinessActivationCountForTest(), ExpectedActivations)
			|| !Test.TestEqual(TEXT("未出现其它失败原因"), S.OtherFailures, 0)
			|| !Test.TestEqual(TEXT("未追加新的输入请求观察"), S.Notices.Num(), bRequeue ? 2 : 1)) { return false; }
		if (S.Request->IsActive()) { S.Request->FinishForTest(); }
		S.ConsumeFrames();
		if (!Test.TestEqual(TEXT("成功消费或过期后不残留重试，按住不生成新请求"),
			S.Request->GetBusinessActivationCountForTest(), ExpectedActivations)) { return false; }
		// 正控同时证明原截止没有伪造松键；必须真实释放再按下才有新激活。
		if (!S.Input.ReleaseAbilityKeyForTest(Test) || !S.Input.PressAbilityKeyForTest(Test)
			|| !Test.TestEqual(TEXT("释放后重新按下仍可正常激活"), S.Request->GetBusinessActivationCountForTest(), ExpectedActivations + 1)) { return false; }
		S.Input.ReleaseAbilityKeyForTest(Test);
		Test.AddInfo(FString::Printf(TEXT("[Input.QueuedDeadline] Case=%s Press=%.9f OriginalDeadline=%.9f Now=%.9f Request=%llu QueuedFailures=%d RetryNotices=%d ExpectedRetryActivations=%d NewPressControl=1; Scope=私有World真实InputKey及PC输入帧，非生产PIE或物理设备"),
			*Case, PressTime, Original.OriginalDeadline, static_cast<double>(S.Input.GetWorld()->GetTimeSeconds()),
			static_cast<unsigned long long>(Original.Identity.RequestSerial), S.QueuedFailures, S.Notices.Num(), ExpectedActivations));
		return true;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FGGYGOInputQueuedDeadlineTest, "GGYGO.Input.QueuedDeadline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FGGYGOInputQueuedDeadlineTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : {TEXT("RetryOnce"), TEXT("RequeueKeepsOriginalDeadline"),
		TEXT("ExpiredBeforeGroupFreed"), TEXT("ExpiredBeforeConsumption")})
	{
		Names.Add(Name);
		Commands.Add(Name);
	}
}

bool FGGYGOInputQueuedDeadlineTest::RunTest(const FString& Parameters)
{
	return RunQueuedDeadlineCase(*this, Parameters);
}
#endif
