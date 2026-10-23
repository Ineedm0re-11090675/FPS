#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PickUp.generated.h"

UCLASS()
class BLASTER_API APickUp : public AActor
{
	GENERATED_BODY()

public:
	APickUp();
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
protected:
	virtual void BeginPlay() override;
	// Codex: Runtime hook keeps Blueprint defaults from re-showing buff placeholder meshes.
	virtual bool ShouldHidePickupMesh() const { return false; }
	// Codex: Buff pickups can opt out of the shared visible mesh while ammo pickups keep using it.
	void SetPickupMeshVisible(bool bVisible);
	UFUNCTION()
	virtual void OnSphereOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult 
	);
	UPROPERTY(EditAnywhere)
	float BaseTurnRate = 45.f;
private:
	UPROPERTY(EditAnywhere)
	class USphereComponent* OverlapSphere;
	UPROPERTY(EditAnywhere)
	USoundCue* PickUpSound;

	UPROPERTY(EditAnywhere)
	UStaticMeshComponent* PickUpMesh;
	UPROPERTY(VisibleAnywhere)
	class UNiagaraComponent* HealthPickUpEffect;
	UPROPERTY(EditAnywhere)
	class UNiagaraSystem* PickUpEffect;

	FTimerHandle OverlayDelayTimerHandle;
	UFUNCTION()
	void OverlayDelayTimerFinish();
	UPROPERTY(EditAnywhere)
	float OverlayDelayTime = 0.25f;
public:  
};
