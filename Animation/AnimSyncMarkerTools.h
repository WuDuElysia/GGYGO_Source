// 编辑器工具：从脚本（Python）向 AnimSequence 写入 AuthoredSyncMarkers，或从固定动画起始根轨迹
// RM_PosX/RM_PosY 烘焙 RM_VelocityDirX/Y 方向 FloatCurve。
// 背景：FModel 导出后 .uasset 丢失同步标记；AnimSequence.AuthoredSyncMarkers 未向 Python 反射暴露，
// set_editor_property 失败。曲线写入通过 UE 5.8 Animation Data Model/Controller 完成，避免直接改 RawCurveTracks。
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

	/**
	 * 从现有 RM_PosX/RM_PosY 累计位置曲线，按 AnimSequence 采样帧生成并覆盖
	 * RM_VelocityDirX/RM_VelocityDirY 两条真实 FloatCurve。RM_PosX/RM_PosY 的原始曲线分量约定为
	 * X=左右、Y=前后；运行时由 MotionDriver 转换为 Bone_Root/UE 局部 X=前、Y=右。
	 * 首帧方向为零，后续帧为水平位置增量的归一化方向；函数只修改并标脏资产，不负责保存包。
	 * @return 写入的采样 key 数量（非编辑器构建、输入为空、无采样帧或写入失败时返回 0；源位置曲线缺失时写入全零方向曲线）
	 */
	UFUNCTION(BlueprintCallable, Category = "AnimTools")
	static int32 BakeVelocityDirectionCurves(UAnimSequence* Anim);
};
