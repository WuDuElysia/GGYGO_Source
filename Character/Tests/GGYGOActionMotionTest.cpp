#if WITH_DEV_AUTOMATION_TESTS
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Character/GGYGOCharacterBase.h"
#include "Character/Data/GGYGOActionMotionProfile.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Character/Data/GGYGOLocomotionMotionProfile.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Curves/CurveVector.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOActionMotionTest, "GGYGO.Movement.ActionMotion.TimingAndOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOActionMotionTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("World"), World)) return false;
	struct FCleanup { UWorld* World; ~FCleanup() { World->DestroyWorld(false); } } Cleanup{World};
	ACharacter* Character = World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("Character"), Character)) return false;
	Character->GetCharacterMovement()->SetComponentTickEnabled(false);
	UGGYGOCharacterMovementComponent* Move = NewObject<UGGYGOCharacterMovementComponent>(Character);
	Move->RegisterComponent();
	Move->SetUpdatedComponent(Character->GetCapsuleComponent());
	Move->MovementMode = MOVE_Walking;
	Move->bRunPhysicsWithNoController = true;
	Move->bOrientRotationToMovement = true;
	Move->RotationRate = FRotator(0., 360., 0.);
	Character->GetMesh()->SetRelativeRotation(FRotator::ZeroRotator);

	UGGYGOMovementSet* FixedSet = NewObject<UGGYGOMovementSet>(Character);
	FixedSet->bUseCurveDrivenSpeed = false;
	FixedSet->RotationYawRate = 360.f;
	UGGYGOLocomotionMotionProfile* TurnBack = NewObject<UGGYGOLocomotionMotionProfile>(FixedSet);
	TurnBack->Duration = 1.f;
	TurnBack->bLoop = false;
	const auto AddTurnBackKeys = [](FRuntimeFloatCurve& Curve, float Start, float End)
	{
		FRichCurve* Rich = Curve.GetRichCurve();
		Rich->SetKeyInterpMode(Rich->AddKey(0.f, Start), RCIM_Linear);
		Rich->SetKeyInterpMode(Rich->AddKey(1.f, End), RCIM_Linear);
	};
	AddTurnBackKeys(TurnBack->SpeedCurve, 500.f, 0.f);
	AddTurnBackKeys(TurnBack->DirectionXCurve, 1.f, 1.f);
	AddTurnBackKeys(TurnBack->DirectionYCurve, 0.f, 0.f);
	AddTurnBackKeys(TurnBack->YawCurve, 0.f, 180.f);
	FixedSet->TurnBackProfile = TurnBack;
	if (!TestTrue(TEXT("Explicit fixed MovementSet binds successfully"), Move->SetMovementSet(FixedSet))) return false;

	UGGYGOActionMotionProfile* Profile = NewObject<UGGYGOActionMotionProfile>(Character);
	Profile->Duration = .23f;
	Profile->TranslationCurve = NewObject<UCurveVector>(Profile);
	const float Times[] = {0.f, .05f, .15f, .23f};
	const float Values[] = {0.f, 0.f, 50.f, 100.f};
	for (int32 Axis = 0; Axis < 3; ++Axis)
		for (int32 Key = 0; Key < 4; ++Key)
		{
			FRichCurve& Curve = Profile->TranslationCurve->FloatCurves[Axis];
			Curve.SetKeyInterpMode(Curve.AddKey(Times[Key], Axis == 0 ? Values[Key] : 0.f), RCIM_Linear);
		}
	FString Error;
	TestTrue(TEXT("Normalized profile valid"), Profile->ValidateMotion(Error));

	for (float Rate : {1.f, 2.f})
	{
		FRootMotionSource_GGYGOActionCurve Source;
		Source.TranslationCurve = Profile->TranslationCurve;
		Source.PlayRate = Rate;
		Source.Duration = Profile->Duration / Rate;
		FVector Integrated = FVector::ZeroVector;
		const float Tick = .04f; // Neither duration is divisible by this tick.
		int32 Iterations = 0;
		while (Source.GetTime() < Source.Duration && ++Iterations < 20)
		{
			const float Simulation = FMath::Min(Tick, Source.Duration - Source.GetTime());
			Source.PrepareRootMotion(Simulation, Tick, *Character, *Move);
			Integrated += Source.RootMotionParams.GetRootMotionTransform().GetTranslation() * Tick;
			if (Rate == 1.f && Iterations == 1)
			{
				TestTrue(TEXT("Zero-speed interval has no displacement"), Integrated.IsNearlyZero());
				TestFalse(TEXT("Zero-speed interval stays active"), Source.Status.HasFlag(ERootMotionSourceStatusFlags::Finished));
			}
		}
		TestTrue(TEXT("Partial final tick preserves total displacement at both rates"), Integrated.Equals(FVector(100., 0., 0.), .001));
		TestTrue(TEXT("Source duration scales inversely with montage rate"), FMath::IsNearlyEqual(Source.GetTime(), .23f / Rate));
		TestTrue(TEXT("Source finishes at duration"), Source.Status.HasFlag(ERootMotionSourceStatusFlags::Finished));
	}

	// The asset pipeline removes source initial offsets; unnormalized profiles must not enter runtime.
	FRichCurve& X = Profile->TranslationCurve->FloatCurves[0];
	for (auto It = X.GetKeyHandleIterator(); It; ++It) X.GetKey(*It).Value += 47.30499f;
	TestFalse(TEXT("Nonzero source origin is rejected"), Profile->ValidateMotion(Error));
	for (auto It = X.GetKeyHandleIterator(); It; ++It) X.GetKey(*It).Value -= 47.30499f;
	TestTrue(TEXT("Subtracting source origin restores valid displacement curve"), Profile->ValidateMotion(Error));

	Character->GetMesh()->SetRelativeScale3D(FVector(2.));
	const int32 First = Move->BeginActionMotion(Profile);
	TestTrue(TEXT("Authority accepts grounded action"), First != INDEX_NONE);
	TestEqual(TEXT("Concurrent action rejected"), Move->BeginActionMotion(Profile), INDEX_NONE);
	const auto FirstSource = Move->GetRootMotionSource(TEXT("GGYGO.ActionCurve"));
	if (!TestTrue(TEXT("Pending RMS immediately visible"), FirstSource.IsValid())) return false;
	TestTrue(TEXT("Mesh scale applied once"), static_cast<FRootMotionSource_GGYGOActionCurve*>(FirstSource.Get())->TranslationScale.Equals(FVector(2.)));
	Move->RequestDirectMove(FVector(0., 100., 0.), true);
	Move->PhysicsRotation(.1f);
	TestTrue(TEXT("Action freezes entry heading"), FMath::IsNearlyZero(Character->GetActorRotation().Yaw));
	Move->EndActionMotion(First);
	TestFalse(TEXT("Cancellation releases active gate immediately"), Move->HasActiveActionMotion());
	if (!TestTrue(TEXT("Cancelled first source is unprepared Pending"),
		Move->CurrentRootMotion.PendingAddRootMotionSources.Contains(FirstSource)
		&& FirstSource->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval)
		&& !FirstSource->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared))) return false;
	if (!TestFalse(TEXT("Legal detach removes the admitted MovementSet"), Move->SetMovementSet(nullptr))) return false;
	if (!TestTrue(TEXT("Cancelled Pending has no native override"),
		Move->GetMovementSet() == nullptr && !Move->CurrentRootMotion.HasOverrideVelocity())) return false;
	const FRotator RejectedHeading = Character->GetActorRotation();
	Move->Velocity = FVector(100., 0., 0.);
	Move->RequestDirectMove(FVector(0., 100., 0.), true);
	const FString ExpectedGroundRejection = FString::Printf(
		TEXT("Movement ground locomotion rejected: Component='%s', Owner='%s', MovementSet='None', ConfigState='unbound', UnsupportedSource='None', SourceType='None', Reason='no accepted MovementSet for ground locomotion'."),
		*Move->GetPathName(), *GetPathNameSafe(Move->GetOwner()));
	AddExpectedMessage(ExpectedGroundRejection, ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1, false);
	Move->ApplyRootMotionToVelocity(.1f);
	TestTrue(TEXT("Cancelled Pending cannot exempt unconfigured ground velocity"), Move->Velocity.IsNearlyZero());
	Move->CalcVelocity(.1f, 0.f, false, 0.f);
	TestTrue(TEXT("Cancelled Pending cannot admit RequestedMove"), Move->Velocity.IsNearlyZero());
	Move->PhysicsRotation(.1f);
	TestTrue(TEXT("Cancelled Pending cannot exempt unconfigured ground rotation"),
		Character->GetActorRotation().Equals(RejectedHeading, .001));
	TestTrue(TEXT("Admission checks do not replace native Pending cleanup"),
		Move->CurrentRootMotion.PendingAddRootMotionSources.Contains(FirstSource));
	if (!TestTrue(TEXT("Explicit fixed MovementSet rebinds successfully"), Move->SetMovementSet(FixedSet))) return false;
	const int32 Second = Move->BeginActionMotion(Profile);
	TestTrue(TEXT("New action uses new ownership token"), Second != INDEX_NONE && Second != First);
	Move->EndActionMotion(First);
	TestTrue(TEXT("Stale token cannot end new action"), Move->HasActiveActionMotion());
	// Find the unremoved source rather than the first (cancelled) source with the same instance name.
	const FRootMotionSourceGroup& Group = Move->CurrentRootMotion;
	TSharedPtr<FRootMotionSource> SecondSource;
	for (const TSharedPtr<FRootMotionSource>& Source : Group.PendingAddRootMotionSources)
		if (Source.IsValid() && !Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval)) SecondSource = Source;
	if (!TestTrue(TEXT("Second action has an actual Pending source"), SecondSource.IsValid())) return false;
	const float NativeTick = Profile->Duration;
	Move->CurrentRootMotion.CleanUpInvalidRootMotion(NativeTick, *Character, *Move);
	TestFalse(TEXT("Native cleanup removes the cancelled Pending source"), Group.PendingAddRootMotionSources.Contains(FirstSource));
	Move->UpdateCharacterStateBeforeMovement(NativeTick);
	Move->CurrentRootMotion.PrepareRootMotion(NativeTick, *Character, *Move, true);
	if (!TestTrue(TEXT("Native last interval is Current Prepared and Finished"),
		Group.RootMotionSources.Contains(SecondSource)
		&& !Group.PendingAddRootMotionSources.Contains(SecondSource)
		&& SecondSource->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared)
		&& SecondSource->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
		&& Group.HasOverrideVelocity())) return false;
	Move->ApplyRootMotionToVelocity(NativeTick);
	TestTrue(TEXT("Native application integrates the scaled complete final interval"),
		(Move->Velocity * NativeTick).Equals(FVector(200., 0., 0.), .001));
	TestFalse(TEXT("Natural expiry is observable without GA timer"), Move->HasActiveActionMotion());
	Move->RequestDirectMove(FVector(0., 100., 0.), true);
	Move->PhysicsRotation(NativeTick);
	TestTrue(TEXT("Native natural-expiry final frame keeps entry heading"),
		FMath::IsNearlyZero(Character->GetActorRotation().Yaw));
	const float NextMoveDelta = .1f;
	Move->CurrentRootMotion.CleanUpInvalidRootMotion(NextMoveDelta, *Character, *Move);
	TestFalse(TEXT("Next native cleanup removes the finished Current"), Group.RootMotionSources.Contains(SecondSource));
	Move->UpdateCharacterStateBeforeMovement(NextMoveDelta);
	Move->CurrentRootMotion.PrepareRootMotion(NextMoveDelta, *Character, *Move, true);
	TestFalse(TEXT("Next native preparation clears the override"), Group.HasOverrideVelocity());
	Move->ApplyRootMotionToVelocity(NextMoveDelta);
	Move->RequestDirectMove(FVector(0., 100., 0.), true);
	Move->PhysicsRotation(.1f);
	TestTrue(TEXT("Natural expiry releases automatic rotation gate"), Character->GetActorRotation().Yaw > 1.f);
	TestTrue(TEXT("Natural expiry permits the next action"), Move->BeginActionMotion(Profile) != INDEX_NONE);

	// Explicit owner cleanup is a separate fixture so it cannot stand in for the natural chain above.
	ACharacter* EndedCharacter = World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("Explicit-owner character"), EndedCharacter)) return false;
	EndedCharacter->GetCharacterMovement()->SetComponentTickEnabled(false);
	UGGYGOCharacterMovementComponent* EndedMove = NewObject<UGGYGOCharacterMovementComponent>(EndedCharacter);
	EndedMove->RegisterComponent();
	EndedMove->SetUpdatedComponent(EndedCharacter->GetCapsuleComponent());
	EndedMove->MovementMode = MOVE_Walking;
	EndedMove->bRunPhysicsWithNoController = true;
	EndedCharacter->GetMesh()->SetRelativeRotation(FRotator::ZeroRotator);
	EndedCharacter->GetMesh()->SetRelativeScale3D(FVector::OneVector);
	if (!TestTrue(TEXT("Explicit-owner fixture binds the same valid configuration"), EndedMove->SetMovementSet(FixedSet))) return false;
	const int32 EndedHandle = EndedMove->BeginActionMotion(Profile);
	if (!TestTrue(TEXT("Explicit-owner fixture starts an actual action"), EndedHandle != INDEX_NONE)) return false;
	const TSharedPtr<FRootMotionSource> EndedSource = EndedMove->GetRootMotionSource(TEXT("GGYGO.ActionCurve"));
	if (!TestTrue(TEXT("Explicit-owner fixture has an actual source"), EndedSource.IsValid())) return false;
	const FRootMotionSourceGroup& EndedGroup = EndedMove->CurrentRootMotion;
	const FRotator EndedHeading = EndedCharacter->GetActorRotation();
	EndedMove->CurrentRootMotion.CleanUpInvalidRootMotion(NativeTick, *EndedCharacter, *EndedMove);
	EndedMove->UpdateCharacterStateBeforeMovement(NativeTick);
	EndedMove->CurrentRootMotion.PrepareRootMotion(NativeTick, *EndedCharacter, *EndedMove, true);
	if (!TestTrue(TEXT("Explicit-owner source is genuinely Prepared and Finished"),
		EndedGroup.RootMotionSources.Contains(EndedSource)
		&& EndedSource->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared)
		&& EndedSource->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
		&& EndedGroup.HasOverrideVelocity())) return false;
	EndedMove->ApplyRootMotionToVelocity(NativeTick);
	TestTrue(TEXT("Explicit-owner native interval applies before owner cleanup"),
		(EndedMove->Velocity * NativeTick).Equals(FVector(100., 0., 0.), .001));
	EndedMove->EndActionMotion(EndedHandle);
	if (!TestTrue(TEXT("Explicit owner cleanup marks the prepared finished Current"),
		EndedGroup.RootMotionSources.Contains(EndedSource)
		&& EndedSource->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared)
		&& EndedSource->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
		&& EndedSource->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval)
		&& EndedGroup.HasOverrideVelocity())) return false;
	EndedMove->RequestDirectMove(FVector(0., 100., 0.), true);
	EndedMove->PhysicsRotation(NativeTick);
	TestTrue(TEXT("Prepared finished marked final frame keeps heading after explicit owner cleanup"),
		EndedCharacter->GetActorRotation().Equals(EndedHeading, .001));

	// The new mode reads the original sequence, including a nonzero cumulative origin and loop boundary.
	// Keep this in the original timing/ownership leaf; none of the legacy final-frame assertions are replaced.
#if WITH_EDITOR
	USkeletalMesh* OriginalLoopFixtureMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!TestNotNull(TEXT("Original sequence fixture requires a real skeletal mesh"), OriginalLoopFixtureMesh)) return false;
	USkeleton* OriginalLoopSourceSkeleton = OriginalLoopFixtureMesh->GetSkeleton();
	if (!TestNotNull(TEXT("Original sequence fixture requires the mesh skeleton"), OriginalLoopSourceSkeleton)) return false;
	// Curve model edits and slot registration must never change the loaded Engine skeleton.
	USkeleton* OriginalLoopFixtureSkeleton = DuplicateObject<USkeleton>(OriginalLoopSourceSkeleton, Character);
	if (!TestNotNull(TEXT("Original sequence fixture owns a skeleton copy"), OriginalLoopFixtureSkeleton)) return false;
	OriginalLoopFixtureSkeleton->SetFlags(RF_Transient);
	if (!TestTrue(TEXT("Original sequence fixture has real reference bones on its private skeleton"),
		OriginalLoopFixtureSkeleton != OriginalLoopSourceSkeleton
		&& OriginalLoopFixtureSkeleton->GetReferenceSkeleton().GetNum() > 0)) return false;
	UAnimSequence* OriginalSequence = NewObject<UAnimSequence>(Character, NAME_None, RF_Transient);
	OriginalSequence->SetSkeleton(OriginalLoopFixtureSkeleton);
	IAnimationDataController& Controller = OriginalSequence->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	const FName OriginalLoopPositionCurveNames[] = {TEXT("RootMotion_PosX"), TEXT("RootMotion_PosY"), TEXT("RootMotion_PosZ")};
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const FAnimationCurveIdentifier Id(OriginalLoopPositionCurveNames[Axis], ERawCurveTrackTypes::RCT_Float);
		FRichCurveKey OriginalPositionStartKey(0.0f, Axis == 0 ? 47.0f : 0.0f);
		FRichCurveKey OriginalPositionEndKey(1.0f, Axis == 0 ? 147.0f : 0.0f);
		OriginalPositionStartKey.InterpMode = OriginalPositionEndKey.InterpMode = RCIM_Linear;
		if (!TestTrue(TEXT("Original cumulative source curve is installed"),
			Controller.AddCurve(Id, 4, false) && Controller.SetCurveKeys(Id, {OriginalPositionStartKey, OriginalPositionEndKey}, false))) return false;
	}
	Controller.NotifyPopulated();
	UAnimMontage* OriginalMontage = NewObject<UAnimMontage>(Character, NAME_None, RF_Transient);
	OriginalMontage->SetSkeleton(OriginalLoopFixtureSkeleton);
	OriginalMontage->SetCompositeLength(2.0f);
	OriginalMontage->SlotAnimTracks.SetNum(1);
	OriginalMontage->SlotAnimTracks[0].SlotName = TEXT("FullBody");
	FAnimSegment Segment;
	Segment.SetAnimReference(OriginalSequence);
	Segment.StartPos = 0.0f;
	Segment.AnimStartTime = 0.0f;
	Segment.AnimEndTime = 1.0f;
	Segment.AnimPlayRate = 1.0f;
	Segment.LoopingCount = 2;
	OriginalMontage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Segment);
	OriginalMontage->AddAnimCompositeSection(TEXT("Main"), 0.0f);
	FGGYGOActionMotionSourceBindingPtr OriginalBinding;
	if (!TestTrue(TEXT("Original looped montage source resolves without a curve copy"),
		GGYGOActionMotionSource::BuildSourceBinding(OriginalMontage, TEXT("FullBody"), TEXT("Main"), OriginalBinding, Error))) return false;
	FVector OriginalDelta;
	TestTrue(TEXT("Original cumulative loop boundary evaluates"),
		GGYGOActionMotionEvaluation::EvaluateInterval(*OriginalBinding, 0.5f, 1.5f, OriginalDelta, Error));
	if (!OriginalDelta.Equals(FVector(100.0, 0.0, 0.0), .001))
	{
		AddInfo(FString::Printf(TEXT("Action loop actual delta=(%.9g,%.9g,%.9g), expected=(100,0,0); evaluation=%s"),
			OriginalDelta.X, OriginalDelta.Y, OriginalDelta.Z, *Error));
		const IAnimationDataModel* OriginalLoopModel = OriginalSequence->GetDataModelInterface().GetInterface();
		for (FName OriginalLoopCurveName : OriginalLoopPositionCurveNames)
		{
			const FFloatCurve* OriginalLoopModelCurve = OriginalLoopModel
				? OriginalLoopModel->FindFloatCurve(FAnimationCurveIdentifier(OriginalLoopCurveName, ERawCurveTrackTypes::RCT_Float)) : nullptr;
			AddInfo(FString::Printf(TEXT("Action loop model=%s curve=%s present=%d"),
				*GetPathNameSafe(OriginalSequence->GetDataModelInterface().GetObject()), *OriginalLoopCurveName.ToString(), OriginalLoopModelCurve != nullptr));
			if (OriginalLoopModelCurve)
				for (const FRichCurveKey& OriginalLoopModelKey : OriginalLoopModelCurve->FloatCurve.GetConstRefOfKeys())
					AddInfo(FString::Printf(TEXT("Action loop curve=%s key time=%.9g value=%.9g interpolation=%d"),
						*OriginalLoopCurveName.ToString(), OriginalLoopModelKey.Time, OriginalLoopModelKey.Value, static_cast<int32>(OriginalLoopModelKey.InterpMode)));
			for (double OriginalLoopSampleTime : {0.0, 0.5, 1.0})
			{
				const float OriginalLoopNativeValue = OriginalSequence->EvaluateCurveData(OriginalLoopCurveName, FAnimExtractContext(OriginalLoopSampleTime, false), false);
				const float OriginalLoopRawValue = OriginalSequence->EvaluateCurveData(OriginalLoopCurveName, FAnimExtractContext(OriginalLoopSampleTime, false), true);
				AddInfo(FString::Printf(TEXT("Action loop curve=%s time=%.9g native=%.9g raw=%.9g model=%s"),
					*OriginalLoopCurveName.ToString(), OriginalLoopSampleTime, OriginalLoopNativeValue, OriginalLoopRawValue,
					OriginalLoopModelCurve ? *FString::Printf(TEXT("%.9g"), OriginalLoopModelCurve->Evaluate(static_cast<float>(OriginalLoopSampleTime))) : TEXT("missing")));
			}
		}
		TArray<FGGYGOActionMotionSourceInterval> OriginalLoopMappedPieces;
		FString OriginalLoopMapError;
		if (GGYGOActionMotionSource::MapMontageInterval(*OriginalBinding, 0.5f, 1.5f, OriginalLoopMappedPieces, OriginalLoopMapError))
			for (const FGGYGOActionMotionSourceInterval& OriginalLoopPiece : OriginalLoopMappedPieces)
				AddInfo(FString::Printf(TEXT("Action loop mapped segment=%d loop=%d montage=[%.9g,%.9g] sequence=[%.9g,%.9g] rate=%.9g"),
					OriginalLoopPiece.BindingSegmentIndex, OriginalLoopPiece.LoopIndex, OriginalLoopPiece.MontageStartSeconds,
					OriginalLoopPiece.MontageEndSeconds, OriginalLoopPiece.SequenceStartSeconds, OriginalLoopPiece.SequenceEndSeconds,
					OriginalLoopPiece.SourceSecondsPerMontageSecond));
		else AddInfo(FString::Printf(TEXT("Action loop mapping failed: %s"), *OriginalLoopMapError));
	}
	TestTrue(TEXT("Per-loop differences preserve displacement and remove original origin only through differencing"),
		OriginalDelta.Equals(FVector(100.0, 0.0, 0.0), .001));
	UAnimMontage* RangeMontage = NewObject<UAnimMontage>(Character, NAME_None, RF_Transient);
	RangeMontage->SetSkeleton(OriginalLoopFixtureSkeleton);
	RangeMontage->SetCompositeLength(2.25f);
	RangeMontage->SlotAnimTracks = OriginalMontage->SlotAnimTracks;
	FAnimSegment RangeEndSegment = Segment;
	RangeEndSegment.StartPos = 2.0f;
	RangeEndSegment.AnimEndTime = 0.25f;
	RangeEndSegment.LoopingCount = 1;
	RangeMontage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(RangeEndSegment);
	RangeMontage->AddAnimCompositeSection(TEXT("Main"), 0.0f);
	RangeMontage->AddAnimCompositeSection(TEXT("End"), 2.0f);
	FGGYGOActionMotionSourceBindingPtr RangeBinding;
	if (!TestTrue(TEXT("Explicit original Main through End range resolves"),
		GGYGOActionMotionSource::BuildSourceBindingRange(RangeMontage, TEXT("FullBody"), TEXT("Main"), TEXT("End"), RangeBinding, Error))) return false;
	TestTrue(TEXT("Range retains both original section boundaries"), RangeBinding->Sections.Num() == 2
		&& RangeBinding->FindSection(TEXT("End")) && RangeBinding->MontageEndSeconds == 2.25f);
	TestTrue(TEXT("Main End crossing differences each reset original segment independently"),
		GGYGOActionMotionEvaluation::EvaluateInterval(*RangeBinding, 1.875f, 2.125f, OriginalDelta, Error)
		&& OriginalDelta.Equals(FVector(25.0, 0.0, 0.0), .001));
	TestTrue(TEXT("Entire natural range includes End displacement despite cumulative curve reset"),
		GGYGOActionMotionEvaluation::EvaluateInterval(*RangeBinding, 0.0f, 2.25f, OriginalDelta, Error)
		&& OriginalDelta.Equals(FVector(225.0, 0.0, 0.0), .001));
	OriginalSequence->bEnableRootMotion = true;
	OriginalDelta = FVector(17.0);
	TestFalse(TEXT("Native animation root motion cannot also execute position curves"),
		GGYGOActionMotionEvaluation::EvaluateInterval(*OriginalBinding, 0.5f, 1.5f, OriginalDelta, Error));
	TestTrue(TEXT("Rejected original interval has no partial output"), OriginalDelta.IsZero());

	// Exercise the same original-source executor through native capsule physics, including nonzero Z.
	// These are transient source/model/collision resources, not replacements for production animations.
	const FVector XYZFloorOrigin(5000.0, 0.0, 0.0);
	const auto MakeXYZCollisionBox = [World](const FVector& Location, const FVector& Extent) -> UBoxComponent*
	{
		AActor* BoxOwner = World->SpawnActor<AActor>();
		if (!BoxOwner) return nullptr;
		UBoxComponent* Box = NewObject<UBoxComponent>(BoxOwner);
		BoxOwner->SetRootComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionProfileName(TEXT("BlockAll"));
		Box->RegisterComponent();
		Box->SetWorldLocation(Location);
		return Box;
	};
	UBoxComponent* XYZFloor = MakeXYZCollisionBox(XYZFloorOrigin - FVector(0.0, 0.0, 20.0), FVector(1000.0, 1000.0, 20.0));
	if (!TestNotNull(TEXT("XYZ fixture has a real blocking floor"), XYZFloor)) return false;
	AGGYGOCharacterBase* XYZCharacter = World->SpawnActor<AGGYGOCharacterBase>(
		XYZFloorOrigin + FVector(0.0, 0.0, 500.0), FRotator(0.0, 90.0, 0.0));
	if (!TestNotNull(TEXT("XYZ capsule character"), XYZCharacter)) return false;
	// Use the Character's actual default CMC so native mode and landing callbacks see the same executor.
	UGGYGOCharacterMovementComponent* XYZMove = XYZCharacter->GetGGYGOMovementComponent();
	if (!TestNotNull(TEXT("XYZ fixture uses the Character's original native CMC"), XYZMove)) return false;
	XYZMove->SetComponentTickEnabled(false);
	XYZMove->bRunPhysicsWithNoController = true;
	if (!TestTrue(TEXT("XYZ fixture binds the original valid MovementSet"), XYZMove->SetMovementSet(FixedSet))) return false;
	const float XYZCapsuleHalfHeight = XYZCharacter->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector XYZEntryLocation = XYZFloorOrigin + FVector(0.0, 0.0, XYZCapsuleHalfHeight + 2.15);
	XYZCharacter->SetActorLocation(XYZEntryLocation);
	XYZMove->SetMovementMode(MOVE_Walking);
	USkeletalMesh* XYZMesh = DuplicateObject<USkeletalMesh>(OriginalLoopFixtureMesh, XYZCharacter);
	if (!TestNotNull(TEXT("XYZ fixture owns its mesh copy"), XYZMesh)) return false;
	XYZMesh->SetFlags(RF_Transient);
	XYZMesh->SetSkeleton(OriginalLoopFixtureSkeleton);
	OriginalLoopFixtureSkeleton->RegisterSlotNode(TEXT("FullBody"));
	XYZCharacter->GetMesh()->SetSkeletalMesh(XYZMesh);
	XYZCharacter->GetMesh()->SetRelativeScale3D(FVector(2.0));
	XYZCharacter->GetMesh()->SetRelativeRotation(FRotator(0.0, 180.0, 0.0));
	XYZCharacter->GetMesh()->SetAnimInstanceClass(UAnimInstance::StaticClass());
	XYZCharacter->GetMesh()->SetComponentTickEnabled(false);
	XYZCharacter->GetMesh()->SetVisibility(false);
	XYZCharacter->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	UAnimInstance* XYZAnim = XYZCharacter->GetMesh()->GetAnimInstance();
	if (!TestNotNull(TEXT("XYZ fixture has its original native AnimInstance"), XYZAnim)) return false;
	UAnimSequence* XYZSequence = NewObject<UAnimSequence>(XYZCharacter, NAME_None, RF_Transient);
	XYZSequence->SetSkeleton(OriginalLoopFixtureSkeleton);
	IAnimationDataController& XYZController = XYZSequence->GetController();
	XYZController.InitializeModel();
	{
		// Publish the complete Model and its target sampling rate before native cache rebuilding.
		IAnimationDataController::FScopedBracket XYZPopulationBracket(
			XYZController, FText::FromString(TEXT("Populate XYZ action fixture")), false);
		XYZController.SetFrameRate(FFrameRate(32, 1), false);
		XYZController.SetNumberOfFrames(FFrameNumber(40), false);
		const float XYZTimes[] = {0.0f, 0.25f, 0.5f, 1.0f, 1.25f};
		const float XYZPositions[3][5] = {{47.0f, 52.0f, 57.0f, 67.0f, 77.0f},
			{13.0f, 15.0f, 17.0f, 21.0f, 25.0f}, {5.0f, 25.0f, 45.0f, 5.0f, 5.0f}};
		for (int32 XYZAxis = 0; XYZAxis < 3; ++XYZAxis)
		{
			TArray<FRichCurveKey> XYZKeys;
			for (int32 XYZKeyIndex = 0; XYZKeyIndex < UE_ARRAY_COUNT(XYZTimes); ++XYZKeyIndex)
			{
				FRichCurveKey& XYZKey = XYZKeys.Emplace_GetRef(XYZTimes[XYZKeyIndex], XYZPositions[XYZAxis][XYZKeyIndex]);
				XYZKey.InterpMode = RCIM_Linear;
			}
			const FAnimationCurveIdentifier XYZCurveId(OriginalLoopPositionCurveNames[XYZAxis], ERawCurveTrackTypes::RCT_Float);
			if (!TestTrue(TEXT("XYZ original Model installs each authored cumulative axis"),
				XYZController.AddCurve(XYZCurveId, 4, false) && XYZController.SetCurveKeys(XYZCurveId, XYZKeys, false))) return false;
		}
		XYZController.NotifyPopulated();
	}
	const IAnimationDataModel* XYZPopulatedModel = XYZSequence->GetDataModelInterface().GetInterface();
	if (!TestTrue(TEXT("XYZ population publishes a coherent native sampling timeline"), XYZPopulatedModel
		&& XYZSequence->GetSamplingFrameRate() == XYZPopulatedModel->GetFrameRate()
		&& FMath::IsNearlyEqual(XYZSequence->GetPlayLength(), 1.25f))) return false;
	UAnimMontage* XYZMontage = NewObject<UAnimMontage>(XYZCharacter, NAME_None, RF_Transient);
	XYZMontage->SetSkeleton(OriginalLoopFixtureSkeleton);
	XYZMontage->SetCompositeLength(1.25f);
	XYZMontage->BlendIn.SetBlendTime(0.0f);
	XYZMontage->BlendOut.SetBlendTime(0.25f);
	XYZMontage->BlendOutTriggerTime = 1.0f / XYZSequence->GetSamplingFrameRate().AsDecimal();
	XYZMontage->SlotAnimTracks.SetNum(1);
	XYZMontage->SlotAnimTracks[0].SlotName = TEXT("FullBody");
	FAnimSegment XYZSegment;
	XYZSegment.SetAnimReference(XYZSequence);
	XYZSegment.AnimStartTime = 0.0f;
	XYZSegment.AnimEndTime = 1.25f;
	XYZSegment.AnimPlayRate = 1.0f;
	XYZSegment.LoopingCount = 1;
	XYZMontage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(XYZSegment);
	XYZMontage->AddAnimCompositeSection(TEXT("Main"), 0.0f);
	XYZMontage->AddAnimCompositeSection(TEXT("End"), 1.0f);
	XYZMontage->CompositeSections[0].NextSectionName = TEXT("End");
	FGGYGOActionMotionSourceBindingPtr XYZBinding;
	if (!TestTrue(TEXT("XYZ original Main through End binding resolves"),
		GGYGOActionMotionSource::BuildSourceBindingRange(XYZMontage, TEXT("FullBody"), TEXT("Main"), TEXT("End"), XYZBinding, Error))) return false;
	int32 XYZMontageInstanceId = INDEX_NONE;
	const auto BeginXYZ = [&](float StartPosition = 0.0f) -> int32
	{
		if (!TestTrue(TEXT("XYZ native Montage playback starts"),
			XYZAnim->Montage_Play(XYZMontage, 1.0f, EMontagePlayReturnType::MontageLength, StartPosition) > 0.0f)) return INDEX_NONE;
		FAnimMontageInstance* XYZInstance = XYZAnim->GetActiveInstanceForMontage(XYZMontage);
		if (!TestNotNull(TEXT("XYZ playback has an actual original instance"), XYZInstance)) return INDEX_NONE;
		int32 XYZHandle = INDEX_NONE;
		if (!TestTrue(TEXT("XYZ execution accepts the original native instance"),
			XYZMove->BeginMontageActionMotion(XYZBinding, XYZInstance->GetInstanceID(), XYZInstance->GetPosition(),
				XYZInstance->GetPlayRate() * XYZMontage->RateScale, 1.0f, XYZHandle, Error))) return INDEX_NONE;
		XYZMontageInstanceId = XYZInstance->GetInstanceID();
		XYZInstance->OnMontageBlendingOutStarted.BindLambda(
			[&, OriginalHandle = XYZHandle, OriginalInstance = XYZMontageInstanceId](UAnimMontage* Montage, bool bInterrupted)
			{
				if (!bInterrupted && Montage == XYZMontage)
					TestTrue(TEXT("Original native non-interrupted BlendOut authenticates only its configured final window"),
						XYZMove->NotifyMontageActionNaturalBlendOut(OriginalHandle, OriginalInstance, Error));
			});
		return XYZHandle;
	};
	const float XYZNativeStep = 0.125f;
	const auto AdvanceXYZ = [&]() -> bool
	{
		FAnimMontageInstance* XYZOriginalInstance = XYZAnim->GetMontageInstanceForID(XYZMontageInstanceId);
		if (!TestNotNull(TEXT("XYZ step retains the exact original Montage instance"), XYZOriginalInstance)) return false;
		if (!TestTrue(TEXT("XYZ original instance remains native and playing"),
			XYZOriginalInstance->Montage == XYZMontage && XYZOriginalInstance->IsActive() && XYZOriginalInstance->IsPlaying())) return false;
		XYZOriginalInstance->UpdateWeight(XYZNativeStep);
		XYZOriginalInstance->Advance(XYZNativeStep, nullptr, false);
		XYZAnim->DispatchQueuedAnimEvents();
		// Public native move entry reaches PerformMovement; the hidden mesh cannot advance twice.
		XYZMove->MoveAutonomous(0.0f, XYZNativeStep, 0, FVector::ZeroVector);
		return true;
	};
	const float XYZOriginalGravityScale = XYZMove->GravityScale;
	const int32 XYZNaturalHandle = BeginXYZ();
	if (!TestTrue(TEXT("XYZ original action receives a real token"), XYZNaturalHandle != INDEX_NONE)) return false;
	int32 XYZNaturalCompletionCount = 0;
	if (!TestTrue(TEXT("Original natural completion observer installs once"),
		XYZMove->ObserveMontageActionMotionCompletion(XYZNaturalHandle,
			FGGYGOActionMotionCompletionDelegate::CreateLambda([&](int32 Handle)
			{
				TestEqual(TEXT("AfterMovement completion preserves its original handle"), Handle, XYZNaturalHandle);
				const FVector ActualDelta = XYZCharacter->GetActorLocation() - XYZEntryLocation;
				TestTrue(TEXT("Completion observes the final End displacement already consumed by the real capsule"),
					FVector(ActualDelta.X, ActualDelta.Y, 0.0).Equals(FVector(-24.0, 60.0, 0.0), 0.05));
				++XYZNaturalCompletionCount;
			}), Error))) return false;
	const TSharedPtr<FRootMotionSource> XYZNaturalSource = XYZMove->GetRootMotionSource(TEXT("GGYGO.ActionCurve"));
	if (!TestTrue(TEXT("XYZ native source overrides Z and enables sensitive liftoff"), XYZNaturalSource.IsValid()
		&& !XYZNaturalSource->Settings.HasFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate)
		&& XYZNaturalSource->Settings.HasFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck))) return false;
	if (!AdvanceXYZ()) return false;
	TestTrue(TEXT("Authored Z causes native Walking to Falling"), XYZMove->IsFalling());
	TestTrue(TEXT("Legal liftoff keeps the same original action"), XYZMove->HasActiveActionMotion());
	for (int32 XYZStepIndex = 1; XYZStepIndex < 4; ++XYZStepIndex) if (!AdvanceXYZ()) return false;
	TestTrue(TEXT("Actual capsule XYZ applies Actor basis and mesh scale exactly once"),
		(XYZCharacter->GetActorLocation() - XYZEntryLocation).Equals(FVector(-8.0, 20.0, 80.0), 0.05));
	for (int32 XYZStepIndex = 4; XYZStepIndex < 8; ++XYZStepIndex) if (!AdvanceXYZ()) return false;
	TestTrue(TEXT("Entering End retains the same original action and native source"), XYZMove->HasActiveActionMotion()
		&& XYZMove->GetRootMotionSource(TEXT("GGYGO.ActionCurve")) == XYZNaturalSource);
	TestEqual(TEXT("Main boundary is not natural range completion"), XYZNaturalCompletionCount, 0);
	for (int32 XYZStepIndex = 8; XYZStepIndex < 10; ++XYZStepIndex) if (!AdvanceXYZ()) return false;
	TestEqual(TEXT("The natural final contribution publishes exactly once"), XYZNaturalCompletionCount, 1);
	TestTrue(TEXT("Original Task completion cleanup preserves the already consumed full range"),
		XYZMove->ReleaseMontageActionMotion(XYZNaturalHandle, EGGYGOActionMotionReleaseReason::Completed, Error));
	TestTrue(TEXT("Original completed cleanup is idempotent"),
		XYZMove->ReleaseMontageActionMotion(XYZNaturalHandle, EGGYGOActionMotionReleaseReason::Completed, Error));
	for (int32 XYZGravityStep = 0; XYZGravityStep < 3; ++XYZGravityStep)
		XYZMove->MoveAutonomous(0.0f, XYZNativeStep, 0, FVector::ZeroVector);
	TestTrue(TEXT("Native collision lands after the finite XYZ source releases gravity"), XYZMove->IsMovingOnGround());
	TestEqual(TEXT("XYZ execution never mutates native GravityScale"), XYZMove->GravityScale, XYZOriginalGravityScale);
	const FVector XYZEndStartLocation = XYZCharacter->GetActorLocation();
	const int32 XYZEndStartHandle = BeginXYZ(1.0625f);
	if (!TestTrue(TEXT("An original correction inside End executes its remaining natural trajectory"), XYZEndStartHandle != INDEX_NONE)) return false;
	if (!AdvanceXYZ() || !AdvanceXYZ()) return false;
	const FVector XYZRemainingEndDelta = XYZCharacter->GetActorLocation() - XYZEndStartLocation;
	TestTrue(TEXT("Remaining End includes its partial final native interval exactly once"),
		FVector(XYZRemainingEndDelta.X, XYZRemainingEndDelta.Y, 0.0).Equals(FVector(-6.0, 15.0, 0.0), 0.05));
	int32 XYZLateCompletionCount = 0;
	TestTrue(TEXT("Late observer replays the exact original consumed completion"),
		XYZMove->ObserveMontageActionMotionCompletion(XYZEndStartHandle,
			FGGYGOActionMotionCompletionDelegate::CreateLambda([&](int32 Handle)
			{ TestEqual(TEXT("Late replay retains the End-start handle"), Handle, XYZEndStartHandle); ++XYZLateCompletionCount; }), Error));
	TestEqual(TEXT("Late replay delivers one original fact"), XYZLateCompletionCount, 1);
	TestFalse(TEXT("The same completed resource cannot install a second completion recipient"),
		XYZMove->ObserveMontageActionMotionCompletion(XYZEndStartHandle,
			FGGYGOActionMotionCompletionDelegate::CreateLambda([](int32) {}), Error));
	TestTrue(TEXT("Remaining End cleanup accepts the original native completion"),
		XYZMove->ReleaseMontageActionMotion(XYZEndStartHandle, EGGYGOActionMotionReleaseReason::Completed, Error));

	// A ceiling constrains the capsule, and landing during Main keeps that original action alive.
	UBoxComponent* XYZCeiling = MakeXYZCollisionBox(
		XYZEntryLocation + FVector(0.0, 0.0, XYZCapsuleHalfHeight + 60.0), FVector(1000.0, 1000.0, 10.0));
	if (!TestNotNull(TEXT("XYZ fixture has a real blocking ceiling"), XYZCeiling)) return false;
	XYZCharacter->SetActorLocation(XYZEntryLocation);
	XYZMove->StopMovementImmediately();
	XYZMove->SetMovementMode(MOVE_Walking);
	const int32 XYZBlockedHandle = BeginXYZ();
	if (!TestTrue(TEXT("Blocked XYZ action receives its own token"), XYZBlockedHandle != INDEX_NONE)) return false;
	for (int32 XYZStepIndex = 0; XYZStepIndex < 4; ++XYZStepIndex) if (!AdvanceXYZ()) return false;
	TestTrue(TEXT("XYZ ascent respects the real capsule ceiling sweep"),
		XYZCharacter->GetActorLocation().Z <= XYZEntryLocation.Z + 50.05);
	TestTrue(TEXT("Blocked trajectory does not fabricate original action failure"), XYZMove->HasActiveActionMotion());
	for (int32 XYZStepIndex = 4; XYZStepIndex < 7; ++XYZStepIndex) if (!AdvanceXYZ()) return false;
	TestTrue(TEXT("Native landing inside Main retains the original resource"),
		XYZMove->IsMovingOnGround() && XYZMove->HasActiveActionMotion());
	TestTrue(TEXT("Original blocked token can cancel without forcing another movement mode"),
		XYZMove->ReleaseMontageActionMotion(XYZBlockedHandle, EGGYGOActionMotionReleaseReason::Cancelled, Error));
	XYZCeiling->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	XYZCharacter->SetActorLocation(XYZEntryLocation);
	XYZMove->StopMovementImmediately();
	XYZMove->SetMovementMode(MOVE_Walking);
	const int32 XYZCancelledHandle = BeginXYZ();
	if (!TestTrue(TEXT("Air cancellation action receives its original token"), XYZCancelledHandle != INDEX_NONE)) return false;
	int32 XYZCancelledCompletionCount = 0;
	TestTrue(TEXT("Cancellation fixture observes only its original natural completion"),
		XYZMove->ObserveMontageActionMotionCompletion(XYZCancelledHandle,
			FGGYGOActionMotionCompletionDelegate::CreateLambda([&](int32) { ++XYZCancelledCompletionCount; }), Error));
	if (!AdvanceXYZ()) return false;
	const FVector XYZVelocityAtCancellation = XYZMove->Velocity;
	TestTrue(TEXT("XYZ original airborne token cancels exactly"),
		XYZMove->ReleaseMontageActionMotion(XYZCancelledHandle, EGGYGOActionMotionReleaseReason::Cancelled, Error));
	TestTrue(TEXT("Air cancellation releases its override and preserves physical mode and momentum"),
		XYZMove->IsFalling() && !XYZMove->HasActiveActionMotion() && !XYZMove->CurrentRootMotion.HasOverrideVelocity()
		&& XYZMove->Velocity.Equals(XYZVelocityAtCancellation));
	XYZMove->MoveAutonomous(0.0f, XYZNativeStep, 0, FVector::ZeroVector);
	TestTrue(TEXT("Native gravity resumes after airborne cancellation"), XYZMove->Velocity.Z < XYZVelocityAtCancellation.Z);
	const int32 XYZSuccessorHandle = BeginXYZ();
	if (!TestTrue(TEXT("An original XYZ successor can enter native Falling"),
		XYZSuccessorHandle != INDEX_NONE && XYZSuccessorHandle != XYZCancelledHandle)) return false;
	TestTrue(TEXT("Retired XYZ cancellation remains idempotent"),
		XYZMove->ReleaseMontageActionMotion(XYZCancelledHandle, EGGYGOActionMotionReleaseReason::Cancelled, Error));
	TestTrue(TEXT("Retired XYZ token does not cancel its airborne successor"), XYZMove->HasActiveActionMotion());
	TestEqual(TEXT("Cancelled original callback cannot become its successor's natural completion"), XYZCancelledCompletionCount, 0);
	TestTrue(TEXT("XYZ fixture releases its exact successor"),
		XYZMove->ReleaseMontageActionMotion(XYZSuccessorHandle, EGGYGOActionMotionReleaseReason::Cancelled, Error));
#endif
	return true;
}
#endif
