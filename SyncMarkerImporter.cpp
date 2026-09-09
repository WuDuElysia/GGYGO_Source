#include "SyncMarkerImporter.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"

// 控制台命令：在编辑器输出日志的命令行输入 SyncMarkerImport 即可执行
static FAutoConsoleCommand CVar_SyncMarkerImport(
	TEXT("SyncMarkerImport"),
	TEXT("从 NTE JSON 批量导入 Sync Marker 到 AnimSequence"),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		FString JsonDir = TEXT("F:/FModel/Output/Exports/HT/Content/Characters/Player/055_kuhara/animation/Movement");
		FString UeDir   = TEXT("/Game/Resourse/Getting/Characters/Player/055_kuhara/animation/Movement");
		FString Group    = TEXT("Locomotion");

		if (Args.Num() >= 1) JsonDir = Args[0];
		if (Args.Num() >= 2) UeDir   = Args[1];
		if (Args.Num() >= 3) Group   = Args[2];

		USyncMarkerImporter::ImportSyncMarkersFromDirectory(JsonDir, UeDir, Group);
	})
);

void USyncMarkerImporter::ImportSyncMarkersFromDirectory(
	const FString& JsonDirectory,
	const FString& UeAnimDirectory,
	const FString& SyncGroupName)
{
	UE_LOG(LogTemp, Log, TEXT("============================================================"));
	UE_LOG(LogTemp, Log, TEXT("NTE Sync Marker 批量导入工具 (C++版)"));
	UE_LOG(LogTemp, Log, TEXT("JSON 目录: %s"), *JsonDirectory);
	UE_LOG(LogTemp, Log, TEXT("UE 目录:  %s"), *UeAnimDirectory);
	UE_LOG(LogTemp, Log, TEXT("SyncGroup: %s"), *SyncGroupName);
	UE_LOG(LogTemp, Log, TEXT("------------------------------------------------------------"));

	TArray<FString> JsonFiles;
	IFileManager::Get().FindFiles(JsonFiles, *(JsonDirectory / TEXT("*.json")), true, false);

	int32 Total = JsonFiles.Num();
	int32 Success = 0;
	int32 SkipNoMarker = 0;
	int32 SkipNotFound = 0;

	UE_LOG(LogTemp, Log, TEXT("找到 %d 个 JSON 文件"), Total);

	for (const FString& JsonFile : JsonFiles)
	{
		FString FullPath = FPaths::Combine(JsonDirectory, JsonFile);
		FString BaseName = FPaths::GetBaseFilename(JsonFile);

		// 解析 Marker
		TArray<TPair<FName, float>> Markers;
		if (!ParseMarkersFromJson(FullPath, Markers))
		{
			SkipNoMarker++;
			continue;
		}

		// 查找 UE 资产
		FString AssetPath = UeAnimDirectory / BaseName;
		UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, *AssetPath);

		if (!AnimSeq)
		{
			SkipNotFound++;
			UE_LOG(LogTemp, Warning, TEXT("[SKIP] 未找到资产: %s"), *BaseName);
			continue;
		}

		// 应用 Marker
		UE_LOG(LogTemp, Log, TEXT("[%d] 处理: %s (%d 个 Marker)"),
			Success + SkipNotFound + 1, *BaseName, Markers.Num());

		// 清除旧 Marker 并写入新的
		AnimSeq->AuthoredSyncMarkers.Empty();

		for (const auto& Pair : Markers)
		{
			FAnimSyncMarker NewMarker;
			NewMarker.MarkerName = Pair.Key;
			NewMarker.Time = Pair.Value;
			AnimSeq->AuthoredSyncMarkers.Add(NewMarker);

			UE_LOG(LogTemp, Log, TEXT("    Marker: %s @ %.4fs"), *Pair.Key.ToString(), Pair.Value);
		}

		// 设置 Sync Group（注意：UE 5.7 中 SyncGroup 在 Montage 上而非 AnimSequence 上，
		// 这里只写入 Marker 数据，SyncGroup 在 NTEAnimInstance 创建动态 Montage 时设置）
		UE_LOG(LogTemp, Log, TEXT("    (SyncGroup 需在动态创建 Montage 时设置: %s)"), *SyncGroupName);

		// 标记修改并保存
		AnimSeq->MarkPackageDirty();
		AnimSeq->GetPackage()->SetDirtyFlag(true);

		Success++;
	}

	UE_LOG(LogTemp, Log, TEXT("------------------------------------------------------------"));
	UE_LOG(LogTemp, Log, TEXT("完成! 总=%d 成功=%d 无Marker=%d 未找到=%d"),
		Total, Success, SkipNoMarker, SkipNotFound);
}

bool USyncMarkerImporter::ImportSingleJson(
	const FString& JsonFilePath,
	const FString& UeAnimBaseDir,
	const FString& SyncGroupName)
{
	TArray<TPair<FName, float>> Markers;
	if (!ParseMarkersFromJson(JsonFilePath, Markers))
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: 无 Marker 数据"), *JsonFilePath);
		return false;
	}

	FString BaseName = FPaths::GetBaseFilename(JsonFilePath);
	FString AssetPath = UeAnimBaseDir / BaseName;

	UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, *AssetPath);
	if (!AnimSeq)
	{
		UE_LOG(LogTemp, Warning, TEXT("未找到资产: %s"), *AssetPath);
		return false;
	}

	AnimSeq->AuthoredSyncMarkers.Empty();
	for (const auto& Pair : Markers)
	{
		FAnimSyncMarker M;
		M.MarkerName = Pair.Key;
		M.Time = Pair.Value;
		AnimSeq->AuthoredSyncMarkers.Add(M);
	}
	// 注意：SyncGroup 在 UE 5.7 中属于 UAnimMontage 而非 UAnimSequence，
	// 需在 NTEAnimInstance 创建动态 Montage 时通过 SetSyncGroup() 设置
	AnimSeq->MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("OK: %s → %d markers, SyncGroup=%s"),
		*BaseName, Markers.Num(), *SyncGroupName);
	return true;
}

bool USyncMarkerImporter::ParseMarkersFromJson(
	const FString& JsonPath,
	TArray<TPair<FName, float>>& OutMarkers)
{
	OutMarkers.Empty();

	FString JsonContent;
	if (!FFileHelper::LoadFileToString(JsonContent, *JsonPath))
	{
		return false;
	}

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
	TArray<TSharedPtr<FJsonValue>> JsonArray;

	if (!FJsonSerializer::Deserialize(Reader, JsonArray) || JsonArray.Num() == 0)
	{
		return false;
	}

	// 遍历数组找 AnimSequence 类型的条目
	for (const auto& EntryVal : JsonArray)
	{
		const TSharedPtr<FJsonObject> EntryObj = EntryVal->AsObject();
		if (!EntryObj) continue;

		FString TypeStr;
		if (!EntryObj->TryGetStringField(TEXT("Type"), TypeStr)) continue;
		if (TypeStr != TEXT("AnimSequence")) continue;

		const TSharedPtr<FJsonObject>* PropsPtr;
		if (!EntryObj->TryGetObjectField(TEXT("Properties"), PropsPtr)) continue;

		const TSharedPtr<FJsonObject> Props = *PropsPtr;

		// 找 AuthoredSyncMarkers 数组
		const TArray<TSharedPtr<FJsonValue>>* MarkerArrayPtr;
		if (!Props->TryGetArrayField(TEXT("AuthoredSyncMarkers"), MarkerArrayPtr))
		{
			return false; // 有 AnimSequence 但没有 Marker 字段
		}

		const TArray<TSharedPtr<FJsonValue>>& MarkerArray = *MarkerArrayPtr;

		for (const auto& MarkerVal : MarkerArray)
		{
			const TSharedPtr<FJsonObject> MarkerObj = MarkerVal->AsObject();
			if (!MarkerObj) continue;

			FString MarkerName;
			float Time = 0.0f;

			if (MarkerObj->TryGetStringField(TEXT("MarkerName"), MarkerName) &&
				MarkerObj->TryGetNumberField(TEXT("Time"), Time))
			{
				OutMarkers.Add(TPair<FName, float>(FName(*MarkerName), Time));
			}
		}

		return OutMarkers.Num() > 0; // 找到就返回
	}

	return false;
}
