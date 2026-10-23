#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PickUpSpawn.generated.h"

UCLASS()
class APickUpSPawn : public AActor
{
	GENERATED_BODY()
protected:
	virtual void BeginPlay() override;
	UPROPERTY(EditAnywhere)
	TArray<TSubclassOf<class APickUp>> PickupClasses;

	void SpawnPickUp();
	void SpawnTimerFinish();
	UFUNCTION()
	void StartSpawnTimer(AActor* DestroyedActor);
	UPROPERTY()
	AActor* SpawnedActor;
private:
	FTimerHandle SpawnTimer;
	UPROPERTY(EditAnywhere)
	float ReSpawnTimeMin;
	UPROPERTY(EditAnywhere)
	float ReSpawnTimeMax;
public:
	APickUpSPawn();
	virtual void Tick(float DeltaTime) override;
};
