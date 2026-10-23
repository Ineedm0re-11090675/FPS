#pragma once

#include "CoreMinimal.h"
#include "Blaster/PickUps/PickUp.h"
#include "SpeedPickUp.generated.h"

UCLASS()
class ASpeedPickUp : public APickUp
{
	GENERATED_BODY()
public:
	ASpeedPickUp();
private:
protected:
	// Codex: Speed pickup should be represented by Niagara, not the gray placeholder mesh.
	virtual bool ShouldHidePickupMesh() const override;
	virtual void OnSphereOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult 
	);
private:
	UPROPERTY(EditAnywhere)
	float BaseSpeedBuff	=1600.f;
	UPROPERTY(EditAnywhere)
	float CrouchSpeedBuff = 850.f;
	UPROPERTY(EditAnywhere)
	float SpeedBuffTime = 10.f;
	
};
