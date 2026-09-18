/**
 * @file GGYGOAnimationStateCapture.h
 * @brief 游戏线程上的动画语义帧唯一抓取入口
 */
#pragma once

class ACharacter;
struct FGGYGOAnimationDebugFrame;
struct FGGYGOAnimationStateFrame;

/** 无状态抓取器。所有 CMC / ASC 跨层读取都收敛在这里。 */
class FGGYGOAnimationStateCapture
{
public:
	/**
	 * 抓取一帧。输出会先整体重置；无 Owner 或非项目 CMC 时保留安全默认值。
	 * 只能在游戏线程调用，结果随后可由动画线程只读消费。
	 */
	void Capture(
		FGGYGOAnimationStateFrame& OutState,
		FGGYGOAnimationDebugFrame& OutDebug,
		const ACharacter* InOwner) const;
};
