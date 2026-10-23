#include "PickUpSpawn.h"
#include "PickUp.h"

APickUpSPawn::APickUpSPawn()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates=true;
}

void APickUpSPawn::BeginPlay()
{
	Super::BeginPlay();
	StartSpawnTimer((AActor*)nullptr);
}

void APickUpSPawn::SpawnPickUp()
{
	int32 NumOfClasses =PickupClasses.Num();
	if (NumOfClasses > 0)
	{
		UWorld* World = GetWorld();
		if (World == nullptr) return;

		int32 SelectionIndex = FMath::RandRange(0, NumOfClasses-1);
		SpawnedActor=World->SpawnActor<APickUp>(PickupClasses[SelectionIndex],GetActorTransform());

		if (HasAuthority() && SpawnedActor)
		{
			SpawnedActor->OnDestroyed.AddDynamic(this,& APickUpSPawn::StartSpawnTimer);
		}
	}
}

void APickUpSPawn::SpawnTimerFinish()
{
	if (HasAuthority())
	{
		SpawnPickUp();
	}
}

void APickUpSPawn::StartSpawnTimer(AActor* DestroyedActor)
{
	if (!HasAuthority() || GetWorldTimerManager().IsTimerActive(SpawnTimer)) return;
	const float RespawnTime =FMath::RandRange(ReSpawnTimeMin, ReSpawnTimeMax);
	GetWorldTimerManager().SetTimer(
		SpawnTimer,
		this,
		&APickUpSPawn::SpawnTimerFinish,
		RespawnTime
		);
}

void APickUpSPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}
