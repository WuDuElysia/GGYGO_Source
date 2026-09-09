#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SyncMarkerImporter.generated.h"

/**
 * 从 NTE FModel 导出的 JSON 中批量导入 Sync Marker 到 AnimSequence 资产
 * 用法: 在编辑器控制台执行: py SyncMarkerImporter.run()
 * 或在 Python 控台: import unreal; unreal.SyncMarkerImporter.import_all()
 */
UCLASS()
class GGYGO_API USyncMarkerImporter : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** 批量导入指定目录下所有 JSON 的 Sync Marker */
	UFUNCTION(BlueprintCallable, Category = "NTE Tools", meta = (DeveloperTool = "true"))
	static void ImportSyncMarkersFromDirectory(
		const FString& JsonDirectory,
		const FString& UeAnimDirectory,
		const FString& SyncGroupName = TEXT("Locomotion"));

	/** 导入单个 JSON 文件的 Sync Marker 到对应 AnimSequence */
	UFUNCTION(BlueprintCallable, Category = "NTE Tools", meta = (DeveloperTool = "true"))
	static bool ImportSingleJson(
		const FString& JsonFilePath,
		const FString& UeAnimBaseDir,
		const FString& SyncGroupName = TEXT("Locomotion"));

private:
	/** 解析 JSON 文件，提取 AuthoredSyncMarkers 数组 */
	static bool ParseMarkersFromJson(const FString& JsonPath,
		TArray<TPair<FName, float>>& OutMarkers);
};
