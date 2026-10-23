#pragma once

#include "CoreMinimal.h"
#include "MyProjectile.h"
#include "ProjectileRocket.generated.h"

UCLASS()
class BLASTER_API AProjectileRocket : public AMyProjectile
{
	GENERATED_BODY()

public:
	AProjectileRocket();
protected:
	UPROPERTY(EditAnywhere)
	USoundCue* LoopingSound;
	UPROPERTY()
	UAudioComponent* LoopingAudioComponent;
	UPROPERTY(EditAnywhere)
	USoundAttenuation* LoopingAttenuation;
	virtual void BeginPlay() override;
	virtual void OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit) override;
	virtual void Destroyed() override;
	virtual void HandleImpactEffectsMulticast() override;
private:
	bool bHasImpacted = false;
};
