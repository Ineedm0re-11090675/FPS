#pragma once
#include "CoreMinimal.h"
#include "PickUp.h"
#include "JumpPickUp.generated.h"

UCLASS()
class AJumpPickUp : public APickUp
{
	GENERATED_BODY()

public:
	AJumpPickUp();

protected:
	// Codex: Jump pickup should be represented by Niagara, not the gray placeholder mesh.
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
	float JumpZVelocityBuff =2000.f;
	UPROPERTY(EditAnywhere)
	float JumpBuffTime = 12.f;
};
