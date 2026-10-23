#pragma once

#include "CoreMinimal.h"
#include "HitScanWeapon.h"
#include "ShotGun.generated.h"

UCLASS()
class BLASTER_API AShotGun : public AHitScanWeapon
{
	GENERATED_BODY()

public:
	virtual void ShotGunFire(const TArray<FVector_NetQuantize>& TraceHitTargets);
	void ShotGunTraceEndWithScatter(const FVector& HitTarget,TArray<FVector_NetQuantize> &HitTargets);
private:
	UPROPERTY(EditAnywhere,Category="Weapon Scatter")
	uint32 NumberOfPellets = 10;
};
