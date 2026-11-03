/**
 * @file GGYGOLocomotionSourceIdentityTest.cpp
 * @brief 用原生 DataModel／平台压缩验证 ACL 曲线映射、完整流身份与缺失配置拒绝。
 */
#include "Character/Data/GGYGOLocomotionEvaluation.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Character/Data/GGYGOAnimationSourceCurveEvaluation.h"
#include "AnimCurveCompressionCodec_ACL.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionCodec_UniformIndexable.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "AnimationUtils.h"
#include "Curves/RichCurve.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Guid.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	TArray<FRichCurveKey> LinearKeys(float First, float Last)
	{
		TArray<FRichCurveKey> Keys{FRichCurveKey(0.0f, First), FRichCurveKey(1.0f, Last)};
		for (FRichCurveKey& Key : Keys) Key.InterpMode = RCIM_Linear;
		return Keys;
	}

	bool CacheNativeStream(FAutomationTestBase& Test, UAnimSequence& Sequence,
		const UClass* ExpectedCodec, TArray<uint8>& OutStream)
	{
		Sequence.CacheDerivedDataForCurrentPlatform();
		Sequence.WaitOnExistingCompression();
		if (!Test.TestTrue(TEXT("原生当前平台压缩已结束且缓存有效"), Sequence.HasCompressedDataForPlatform(nullptr))) return false;
		const UAnimSequence::FScopedCompressedAnimSequence Compressed = Sequence.GetCompressedData(FAnimExtractContext(0.0, false));
		const FCompressedAnimSequence& Platform = Compressed.Get();
		if (!Test.TestTrue(TEXT("实际压缩数据走明确指定的 codec，不能以 raw 或其它 codec 代替"),
			Platform.IsCurveDataValid(&Sequence) && Platform.CurveCompressionCodec
			&& Platform.CurveCompressionCodec->GetClass() == ExpectedCodec && !Platform.CompressedCurveByteStream.IsEmpty())) return false;
		OutStream = Platform.CompressedCurveByteStream;
		return true;
	}

	bool MakeNativeSource(FAutomationTestBase& Test, USkeleton& Skeleton, FName ExtraName,
		FGGYGOLocomotionSequenceSource& OutSource)
	{
		UAnimSequence* Sequence = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
		OutSource.Sequence.Reset(Sequence);
		Sequence->SetSkeleton(&Skeleton);
		Sequence->RateScale = 1.0f;
		Sequence->BoneCompressionSettings = FAnimationUtils::GetDefaultAnimationBoneCompressionSettings();
		Sequence->VariableFrameStrippingSettings = FAnimationUtils::GetDefaultVariableFrameStrippingSettings();
		if (!Test.TestTrue(TEXT("夹具明确取得引擎骨骼／剥帧设置"),
			Sequence->BoneCompressionSettings != nullptr && Sequence->VariableFrameStrippingSettings != nullptr)) return false;
		UAnimCurveCompressionSettings* Settings = NewObject<UAnimCurveCompressionSettings>(Sequence, NAME_None, RF_Transient);
		UAnimCurveCompressionCodec_ACL* Codec = NewObject<UAnimCurveCompressionCodec_ACL>(Settings, NAME_None, RF_Transient);
		Codec->CurvePrecision = 0.0001f;
		Settings->Codec = Codec;
		Sequence->CurveCompressionSettings = Settings;

		IAnimationDataController& Controller = Sequence->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(30), false);
		const FReferenceSkeleton& Reference = Skeleton.GetReferenceSkeleton();
		const FName Root = Reference.GetBoneName(0);
		const FTransform& RootPose = Reference.GetRefBonePose()[0];
		TArray<FVector3f> Positions, Scales;
		TArray<FQuat4f> Rotations;
		Positions.Init(FVector3f(RootPose.GetTranslation()), 31);
		Rotations.Init(FQuat4f(RootPose.GetRotation()), 31);
		Scales.Init(FVector3f(RootPose.GetScale3D()), 31);
		if (!Test.TestTrue(TEXT("夹具原生根骨轨道与真实 Skeleton 一致"), Controller.AddBoneCurve(Root, false)
			&& Controller.SetBoneTrackKeys(Root, Positions, Rotations, Scales, false))) return false;
		const TArray<TPair<FName, TArray<FRichCurveKey>>> Curves{
			{TEXT("RootMotion_Speed"), LinearKeys(100.0f, 200.0f)},
			{TEXT("RootMotion_DirX"), LinearKeys(1.0f, 1.0f)},
			{TEXT("RootMotion_DirY"), LinearKeys(0.25f, 0.25f)},
			{TEXT("RootMotion_Yaw"), LinearKeys(0.0f, 180.0f)},
			{ExtraName, LinearKeys(5.0f, 10.0f)}};
		for (const auto& Curve : Curves)
		{
			const FAnimationCurveIdentifier Id(Curve.Key, ERawCurveTrackTypes::RCT_Float);
			if (!Test.TestTrue(TEXT("夹具通过原生 Controller 写入完整曲线"),
				Controller.AddCurve(Id, 4, false) && Controller.SetCurveKeys(Id, Curve.Value, false))) return false;
		}
		Controller.NotifyPopulated();
		OutSource.MotionType = EGGYGOLocomotionMotionType::WalkStart;
		OutSource.RouteKey = TEXT("ACLSourceIdentityFixture");
		OutSource.bLoop = false;
		OutSource.PlayLength = Sequence->GetPlayLength();
		OutSource.SequenceRateScale = Sequence->RateScale;
		return Test.TestEqual(TEXT("夹具实际原片长度"), OutSource.PlayLength, 1.0f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionSourceIdentityACLTest,
	"GGYGO.Movement.Locomotion.SourceIdentity.ACL",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionSourceIdentityACLTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOLocomotionEvaluation;
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!TestNotNull(TEXT("原生压缩夹具需要真实骨骼 Mesh"), Mesh)
		|| !TestNotNull(TEXT("原生压缩夹具需要真实 Skeleton"), Mesh->GetSkeleton())) return false;
	TStrongObjectPtr<USkeleton> Skeleton(DuplicateObject<USkeleton>(Mesh->GetSkeleton(), GetTransientPackage()));
	if (!TestTrue(TEXT("只使用自有瞬态 Skeleton 副本"), Skeleton.IsValid()
		&& Skeleton.Get() != Mesh->GetSkeleton() && Skeleton->GetReferenceSkeleton().GetNum() > 0)) return false;
	Skeleton->SetFlags(RF_Transient);
	// 新建且字典序在 RootMotion 之前的名称，使原生 FastLess 索引排序与序列化轨道顺序确有不同。
	const FName ExtraName(*FString::Printf(TEXT("AAA_SourceIdentity_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FGGYGOLocomotionSequenceSource Source;
	if (!MakeNativeSource(*this, *Skeleton, ExtraName, Source)) return false;
	UAnimSequence* Sequence = Source.Sequence.Get();
	TArray<uint8> OriginalStream;
	if (!CacheNativeStream(*this, *Sequence, UAnimCurveCompressionCodec_ACL::StaticClass(), OriginalStream)) return false;
	{
		const UAnimSequence::FScopedCompressedAnimSequence Compressed = Sequence->GetCompressedData(FAnimExtractContext(0.0, false));
		const auto& Names = Compressed.Get().IndexedCurveNames;
		bool bHasPermutation = false;
		for (int32 Index = 0; Index < Names.Num(); ++Index) bHasPermutation |= Names[Index].CurveIndex != Index;
		if (!TestTrue(TEXT("夹具实际覆盖非恒等名称排序置换"), Names.Num() == 5 && bHasPermutation)) return false;
	}
	FString Error;
	FGGYGOLocomotionSourceIdentityDigest Initial, Repeated;
	if (!TestTrue(TEXT("真实 ACL 压缩源取得完整内容身份"), ComputeSourceIdentity(Source, Initial, Error) && Error.IsEmpty())
		|| !TestTrue(TEXT("身份明确来自真实压缩流"), Initial.ContentKind == EGGYGOLocomotionCurveContentKind::CompressedCodecStream)
		|| !TestTrue(TEXT("同一原始压缩流的身份稳定"), ComputeSourceIdentity(Source, Repeated, Error)
			&& Repeated.ConfigurationDigest == Initial.ConfigurationDigest && Repeated.CurveContentDigest == Initial.CurveContentDigest)) return false;

	const TArray<FName> Required{TEXT("RootMotion_Speed"), TEXT("RootMotion_DirX"), TEXT("RootMotion_DirY"), TEXT("RootMotion_Yaw")};
	TArray<float> Values;
	if (!TestTrue(TEXT("同一生产批量曲线入口实际解码 ACL 原轨道"),
		GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(Sequence, 0.5, Required, Values, Error))
		|| !TestEqual(TEXT("四条必需曲线均真实返回"), Values.Num(), 4)) return false;
	const TArray<float> Expected{150.0f, 1.0f, 0.25f, 90.0f};
	for (int32 Index = 0; Index < Values.Num(); ++Index)
		if (!TestEqual(TEXT("名称获得其原压缩轨道值"), Values[Index], Expected[Index], 0.02f)) return false;

	// 只改变四条必需曲线以外的内容，证明身份覆盖整个原流，不能用几条移动曲线的采样值冒充。
	const FAnimationCurveIdentifier ExtraId(ExtraName, ERawCurveTrackTypes::RCT_Float);
	if (!TestTrue(TEXT("原生模型修改额外曲线"), Sequence->GetController().SetCurveKeys(ExtraId, LinearKeys(35.0f, 80.0f), false))) return false;
	TArray<uint8> ChangedStream;
	if (!CacheNativeStream(*this, *Sequence, UAnimCurveCompressionCodec_ACL::StaticClass(), ChangedStream)) return false;
	FGGYGOLocomotionSourceIdentityDigest Changed;
	if (!TestTrue(TEXT("额外曲线真实改变压缩流与内容身份，配置身份不变"), OriginalStream != ChangedStream
		&& ComputeSourceIdentity(Source, Changed, Error) && Changed.ConfigurationDigest == Initial.ConfigurationDigest
		&& Changed.CurveContentDigest != Initial.CurveContentDigest)) return false;
	TArray<float> UnchangedValues;
	if (!TestTrue(TEXT("移动曲线输出仍相同，额外流内容变化没有被采样摘要忽略"),
		GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(Sequence, 0.5, Required, UnchangedValues, Error))
		|| !TestEqual(TEXT("修改后四曲线均返回"), UnchangedValues.Num(), Values.Num())) return false;
	for (int32 Index = 0; Index < Values.Num(); ++Index)
		if (!TestEqual(TEXT("四条移动曲线保持原值"), UnchangedValues[Index], Values[Index], 0.02f)) return false;

	Source.RouteKey = TEXT("ACLSourceIdentityOtherRoute");
	FGGYGOLocomotionSourceIdentityDigest Rerouted;
	if (!TestTrue(TEXT("路由配置身份与原流内容身份分列"), ComputeSourceIdentity(Source, Rerouted, Error)
		&& Rerouted.ConfigurationDigest != Changed.ConfigurationDigest && Rerouted.CurveContentDigest == Changed.CurveContentDigest)) return false;
	Source.RouteKey = TEXT("ACLSourceIdentityFixture");
	Sequence->CurveCompressionSettings->Codec = NewObject<UAnimCurveCompressionCodec_UniformIndexable>(
		Sequence->CurveCompressionSettings, NAME_None, RF_Transient);
	TArray<uint8> OtherCodecStream;
	if (!CacheNativeStream(*this, *Sequence, UAnimCurveCompressionCodec_UniformIndexable::StaticClass(), OtherCodecStream)) return false;
	FGGYGOLocomotionSourceIdentityDigest OtherCodec;
	if (!TestTrue(TEXT("真实 codec 表达改变内容身份而不改变源配置身份"), ComputeSourceIdentity(Source, OtherCodec, Error)
		&& OtherCodec.ConfigurationDigest == Changed.ConfigurationDigest && OtherCodec.CurveContentDigest != Changed.CurveContentDigest)) return false;

	const FAnimationCurveIdentifier MissingId(TEXT("RootMotion_DirY"), ERawCurveTrackTypes::RCT_Float);
	if (!TestTrue(TEXT("原生模型移除必需曲线"), Sequence->GetController().RemoveCurve(MissingId, false))) return false;
	if (!CacheNativeStream(*this, *Sequence, UAnimCurveCompressionCodec_UniformIndexable::StaticClass(), OtherCodecStream)) return false;
	OtherCodec = Initial;
	Error = TEXT("先前身份错误");
	if (!TestFalse(TEXT("缺必需曲线拒绝身份"), ComputeSourceIdentity(Source, OtherCodec, Error))) return false;
	return TestTrue(TEXT("失败清空输出并给出可定位诊断"), OtherCodec.ConfigurationDigest == 0 && OtherCodec.CurveContentDigest == 0
		&& OtherCodec.ContentKind == EGGYGOLocomotionCurveContentKind::None && Error.Contains(TEXT("RootMotion_DirY")));
}
#endif
