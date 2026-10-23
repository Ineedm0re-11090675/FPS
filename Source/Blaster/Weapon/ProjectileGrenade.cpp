#include "ProjectileGrenade.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundCue.h"
AProjectileGrenade::AProjectileGrenade()
{
	ProjectileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Greande Mesh"));
	ProjectileMesh->SetupAttachment(RootComponent);
	ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	MovementComponent = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Grenade Movement Component"));
	MovementComponent->SetUpdatedComponent(CollisionBox);
	MovementComponent->SetIsReplicated(true);
	MovementComponent->bRotationFollowsVelocity = true;
	MovementComponent->bShouldBounce = true;

	bBindHitOnBeginPlay = false;
	bPlayImpactEffectsOnDestroyed = false;
}

void AProjectileGrenade::BeginPlay()
{
	Super::BeginPlay();
	SpawnTrailSystem();
	if (HasAuthority())
	{
		StartDestroyTimer();
	}

	MovementComponent->OnProjectileBounce.AddDynamic(this,&AProjectileGrenade::OnBounce);
}

void AProjectileGrenade::DestroyTimerFinished()
{
	if (!HasAuthority()) return;

	const FVector ExplosionLocation = GetActorLocation();
	const FVector ExplosionNormal = LastBounceNormal.IsNearlyZero()
		? FVector::UpVector
		: LastBounceNormal;

	if (ExplodeDamageAndEffects(ExplosionLocation, ExplosionNormal, GetActorRotation()))
	{
		SetLifeSpan(0.25f);
	}
}

void AProjectileGrenade::OnBounce(const FHitResult& ImpactResult, const FVector& ImpactVelocity)
{
	LastBounceNormal = ImpactResult.ImpactNormal.IsNearlyZero()
	? FVector::UpVector
	: ImpactResult.ImpactNormal.GetSafeNormal();
	if (BounceSound != nullptr)
	{
		UGameplayStatics::PlaySoundAtLocation(
			this,
			BounceSound,
			ImpactResult.ImpactPoint
			);
	}
}
