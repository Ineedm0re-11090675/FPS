#pragma once

#include "CoreMinimal.h"
#include "MyProjectile.h"
#include "ProjectileBullet.generated.h"

UCLASS()
class BLASTER_API AProjectileBullet : public AMyProjectile
{
	GENERATED_BODY()
public:
	AProjectileBullet();
#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
protected:
	void OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse,
			   const FHitResult& Hit) override;
	void BeginPlay() override;
};
