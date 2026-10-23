#pragma once

#include "CoreMinimal.h"
#include "PickUp.h"
#include "Blaster/Weapon/WeaponTypes.h"
#include "AmmoPickUp.generated.h"

UCLASS()
class BLASTER_API AAmmoPickUp : public APickUp
{
	GENERATED_BODY()
public:
	
private:

	UPROPERTY(EditAnywhere)
	EWeaponType WeaponType;
	
	UPROPERTY(EditAnywhere)
	int32 AmmoAmount = 32;
protected:
	virtual void OnSphereOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult 
	) override;
};
