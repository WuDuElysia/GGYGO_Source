// 编辑器工具：从脚本（Python）向 AnimSequence 写入 AuthoredSyncMarkers。
// 背景：FModel 导出后 .uasset 丢失同步标记；AnimSequence.AuthoredSyncMarkers 未向 Python 反射暴露，
// set_editor_property 失败。故提供本 C++ BlueprintCallable 入口，由 import_anim_syncmarkers.py 调用。
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AnimSyncMarkerTools.generated.h"

class UAnimSequence;

UCLASS()
class GGYGO_API UAnimSyncMarkerTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 用给定的（标记名, 时间）覆盖动画的 AuthoredSyncMarkers。仅编辑器有效。
	 * @param Anim         目标动画序列
	 * @param MarkerNames  标记名数组（如 "Left"/"Right"）
	 * @param Times        对应时间数组（秒），与 MarkerNames 一一对应
	 * @return 实际写入的标记数量（非编辑器构建或 Anim 为空时返回 0）
	 */
	UFUNCTION(BlueprintCallable, Category = "AnimTools")
	static int32 SetAuthoredSyncMarkers(UAnimSequence* Anim, const TArray<FName>& MarkerNames, const TArray<float>& Times);
};
