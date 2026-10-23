#pragma once

#include "CoreMinimal.h"
#include "PickUp.h"
#include "HealthPickUps.generated.h"

UCLASS()
class AHealthPickups : public APickUp
{
	GENERATED_BODY()

public:
	AHealthPickups();
protected:
	// Codex: Health pickup should be represented by Niagara, not the gray placeholder mesh.
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
	float HealthAmount = 50.f;
	UPROPERTY(EditAnywhere)
	float HealthTime = 4.f;
	
};
