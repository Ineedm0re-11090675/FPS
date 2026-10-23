#include "PickUp.h"
#include "Blaster/Weapon/WeaponTypes.h"
#include "Components/SphereComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "Sound/SoundCue.h"

APickUp::APickUp()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	OverlapSphere = CreateDefaultSubobject<USphereComponent>(TEXT("OverlapSphere"));
	OverlapSphere->SetupAttachment(RootComponent);
	OverlapSphere->SetSphereRadius(150.f);
	OverlapSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	OverlapSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	OverlapSphere->AddLocalOffset(FVector(0.0f, 0.0f, 85.f));

	PickUpMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PickUpMesh"));
	PickUpMesh->SetupAttachment(RootComponent);
	PickUpMesh->SetRelativeScale3D(FVector(5.f,5.f,5.f));
	//Only Visible
	PickUpMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	PickUpMesh->SetRenderCustomDepth(true);
	PickUpMesh->SetCustomDepthStencilValue(CUSTOM_DEPTH_PURPLE);

	HealthPickUpEffect = CreateDefaultSubobject<UNiagaraComponent>(TEXT("PickUpEffect"));
	HealthPickUpEffect->SetupAttachment(RootComponent);
}

void APickUp::BeginPlay()
{
	Super::BeginPlay();
	if (ShouldHidePickupMesh())
	{
		// Codex: Apply after Blueprint defaults are initialized, so hidden buff meshes stay hidden in PIE.
		SetPickupMeshVisible(false);
	}
	if (HasAuthority())
	{
		GetWorldTimerManager().SetTimer(
			OverlayDelayTimerHandle,
			this,
			&APickUp::OverlayDelayTimerFinish,
			OverlayDelayTime
			);
	}
}

void APickUp::SetPickupMeshVisible(bool bVisible)
{
	if (PickUpMesh == nullptr) return;

	// Codex: Keep the component available for blueprints, but do not render or outline hidden buff meshes.
	PickUpMesh->SetVisibility(bVisible, true);
	PickUpMesh->SetHiddenInGame(!bVisible, true);
	PickUpMesh->SetRenderCustomDepth(bVisible);
}

void APickUp::Destroyed()
{
	Super::Destroyed();
	if (PickUpSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, PickUpSound, GetActorLocation());
	}
	if (PickUpEffect)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			this,
			PickUpEffect,
			GetActorLocation(),
			GetActorRotation()
			);
	}
}

void APickUp::OnSphereOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
                              UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
}

void APickUp::OverlayDelayTimerFinish()
{
	OverlapSphere->OnComponentBeginOverlap.AddDynamic(this,&APickUp::OnSphereOverlap);
}

void APickUp::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (PickUpMesh)
	{
		PickUpMesh->AddLocalRotation(FRotator(0.0f, BaseTurnRate*DeltaTime, 0.0f));
	}
}
