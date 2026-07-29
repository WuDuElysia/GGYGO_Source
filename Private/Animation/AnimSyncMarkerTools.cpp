#include "Animation/AnimSyncMarkerTools.h"
#include "Animation/AnimSequence.h"

int32 UAnimSyncMarkerTools::SetAuthoredSyncMarkers(UAnimSequence* Anim, const TArray<FName>& MarkerNames, const TArray<float>& Times)
{
#if WITH_EDITOR
	if (!Anim)
	{
		return 0;
	}

	const int32 Num = FMath::Min(MarkerNames.Num(), Times.Num());

	// 用传入数据覆盖同步标记。
	Anim->AuthoredSyncMarkers.Empty(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		FAnimSyncMarker Marker;
		Marker.MarkerName = MarkerNames[i];
		Marker.Time = Times[i];
		Anim->AuthoredSyncMarkers.Add(Marker);
	}

	// 运行时同步要求标记按时间升序。
	Anim->AuthoredSyncMarkers.Sort([](const FAnimSyncMarker& A, const FAnimSyncMarker& B)
	{
		return A.Time < B.Time;
	});

	// 重建唯一标记名等缓存，并标脏以便保存。
	Anim->RefreshSyncMarkerDataFromAuthored();
	Anim->MarkPackageDirty();

	return Anim->AuthoredSyncMarkers.Num();
#else
	return 0;
#endif
}
