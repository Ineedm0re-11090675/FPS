#pragma once

#include "CoreMinimal.h"
#include "PickUp.h"
#include "ShieldPickUp.generated.h"

UCLASS()
class AShieldPickUp : public APickUp
{
	GENERATED_BODY()
public:
	AShieldPickUp();
protected:
	// Codex: Shield pickup should be represented by Niagara, not the gray placeholder mesh.
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
	float ShieldReplenishAmount=45.f;
	UPROPERTY(EditAnywhere)
	float ShieldReplenishTime = 4.f;
};
