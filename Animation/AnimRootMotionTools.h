// 编辑器工具：把动画里承载 root motion 的骨骼轨道「原地化」。
//
// 背景：ZZZ 的资产在 Unity 侧靠 Mecanim 处理 —— Animator 从 rootMotionBone 提取整体位移与
// 转身，姿势里只留相对 root 的局部动作，`applyRootMotion=false` 则表示提取出的量交给
// 游戏代码自己消费。AnimeStudio 导出 FBX 时没有这一步：它把**提取前**的完整姿势写进
// `Bip001`，同时把提取出的 root motion 单独写进外部 Null 节点 `Root`（导入 UE 后成为
// 骨架末尾的一根骨骼）。两者携带同一份位移 —— TurnBack 里 `Root` 与 `Bip001` 的
// component 位移逐帧相同，差值只剩身体摆动与骨盆高度。
//
// 后果是 UE 播这段动画时姿势里仍带着整体位移和 180 度转身，而移动层又会按曲线驱动 Actor，
// 两者叠加 —— 转身会转 360 度，位移会走双倍。本工具把 `Root` 承载的那一份从 `Bip001`
// 的轨道里减掉，动画于是变成真正的 in-place，位移与转身只剩「代码按曲线驱动」这一个来源。
//
// 为什么用 `Root` 骨骼而不是 `RootMotion_*` 曲线作为扣除量：
// `Root` 与 `Bip001` 在同一套骨骼 component 空间里，相减是纯 transform 运算，
// 不需要任何手工轴映射。曲线经过了 FBX→UE 的轴变换与 ±180 解折叠，要拿它去扣
// 骨骼轨道就得把那套变换反推回来，而轴向随角色骨架而变（Pyrios 的坐标系转换烘在
// 根骨骼的 Roll 90 度上，前进方向落在 component 空间的 Y 轴而不是 X 轴），
// 手工推映射极易出错且换个角色就失效。
//
// 为什么在资产侧一次性烘焙，而不是运行时抵消：
//   - AnimBP 里用 Transform(Modify)Bone 抵消需要每帧一个骨骼变换节点，且要保证抵消量与
//     移动层消费的量逐帧一致，两处各算一次就多了一个会漂的地方；
//   - 做 In-Place 副本要维护两份资产，原资产重导入后副本就过期了；
//   - 烘焙进轨道后运行时零成本。
//
// 骨骼轨道未向 Python 反射暴露（`AnimSequence` 上没有 `get_controller`，
// `AnimationLibrary` 也没有骨骼轨道写入），所以只能走 C++，与 `UAnimSyncMarkerTools` 同理。
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AnimRootMotionTools.generated.h"

class UAnimSequence;

UCLASS()
class GGYGO_API UAnimRootMotionTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 从 MotionBone 的轨道里扣除 RootBone 承载的 root motion，使动画原地播放。
	 *
	 * 在骨骼 component 空间里做 `NewPose = OldPose * RootMotion.Inverse()`，其中
	 * `RootMotion = RootBone 当前帧的 component 变换 * 首帧的逆`。取相对首帧的增量而不是
	 * 直接用 component 变换，是为了消掉首帧里那份 FBX→UE 的坐标系转换（Pyrios 是 Roll 90 度）
	 * —— 那是坐标系约定，不是 root motion，扣掉会让整个角色躺倒。
	 *
	 * 只扣水平位移与绕竖直轴的转角；高度与 pitch/roll 属于动作本身的起伏，
	 * 扣掉会让角色沉进地面或前后倾倒。
	 *
	 * 幂等性：**本函数不幂等**。重复执行会把同一份 root motion 扣第二次，
	 * 动画会朝反方向漂。调用方要自己记录哪些资产已经处理过。
	 *
	 * @param Anim            目标动画。
	 * @param MotionBoneName  承载整体运动的骨骼。Pyrios 是 `Bip001`。
	 * @param RootBoneName    承载 root motion 参考轨迹的骨骼。Pyrios 是 `Root`。
	 * @param bAlignToOrigin  扣除后再把首帧的水平位置平移到原点。
	 *
	 *   需要它是因为两类动画的 MotionBone 基准不同：走跑循环与转身的首帧本来就在原点附近
	 *   （偏移几厘米），而收招类（`Walk_End` / `Run_End`）是「从远处走过来停在原点」——
	 *   首帧在 -330cm、末帧在 0，而 RootBone 记的是相对位移（0 到 +330）。只做扣除的话
	 *   位移确实归零了，但整段会停在原点后方 3.3 米处。
	 *
	 *   开启后首帧水平位置精确归零，代价是整段被平移了首帧那点摆动量（走跑类是 1~6cm）。
	 *   演出/入场类动画若刻意站在偏离原点的位置，应当关闭。
	 * @param bDryRun         为 true 时只把诊断写进日志、不修改资产，用于核对扣除结果。
	 * @return 实际改写的 key 数量（非编辑器构建、找不到骨骼、无轨道或 dry run 时返回 0）
	 */
	UFUNCTION(BlueprintCallable, Category = "AnimTools")
	static int32 StripRootMotionFromBone(UAnimSequence* Anim, FName MotionBoneName,
		FName RootBoneName, bool bAlignToOrigin, bool bDryRun);
};
