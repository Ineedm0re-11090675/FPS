#pragma once
#include "CoreMinimal.h"
#include "MyProjectile.h"
#include "ProjectileGrenade.generated.h"

UCLASS()
class BLASTER_API AProjectileGrenade : public AMyProjectile
{
	GENERATED_BODY()
public:
	AProjectileGrenade();
protected:
	virtual void BeginPlay() override;
	UFUNCTION()
	void OnBounce(const FHitResult& ImpactResult, const FVector& ImpactVelocity);
	virtual void DestroyTimerFinished() override;
	
private:
	UPROPERTY(EditAnywhere)
	USoundCue* BounceSound;

	FVector LastBounceNormal = FVector::UpVector;
};