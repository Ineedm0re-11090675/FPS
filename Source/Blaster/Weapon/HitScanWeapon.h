#pragma once

#include "CoreMinimal.h"
#include "Weapon.h"
#include "HitScanWeapon.generated.h"

UCLASS()
class BLASTER_API AHitScanWeapon : public AWeapon
{
	GENERATED_BODY()
public:
	virtual void Fire(const FVector& HitTarget) override;
protected:

	void WeaponHitTrace(const FVector& TraceStart,const FVector& HitTarget,FHitResult& OutHit);
	
	UPROPERTY(EditAnywhere)
	USoundCue* HitSound;

	UPROPERTY(EditAnywhere)
	class UNiagaraSystem* ImpactParticle;
private:
	UPROPERTY(EditAnywhere)
	UNiagaraSystem* BeamParticle;
	
	UPROPERTY(EditAnywhere)
	UNiagaraSystem* MuzzleFLash;

	UPROPERTY(EditAnywhere)
	USoundCue* FireSound;
	
};
