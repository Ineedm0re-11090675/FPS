#include "ProjectileRocket.h"

#include "NiagaraComponent.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "RocketMovementComponent.h"
#include "Sound/SoundCue.h"

AProjectileRocket::AProjectileRocket()
{
	bPlayImpactEffectsOnDestroyed = false;
	ProjectileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Rocket Mesh"));
	ProjectileMesh->SetupAttachment(RootComponent);
	ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	MovementComponent = CreateDefaultSubobject<URocketMovementComponent>(TEXT("Rocket Movement Component"));
	//from codex
	MovementComponent->SetUpdatedComponent(CollisionBox);
	//这样可以影响速度方向来控制子弹的方向，加重力
	MovementComponent->bRotationFollowsVelocity = true;
	MovementComponent->bShouldBounce = false;
	MovementComponent->Bounciness = 0.f;
	MovementComponent->ProjectileGravityScale = 0.f;
	MovementComponent->SetIsReplicated(true);
}

void AProjectileRocket::BeginPlay()
{
	Super::BeginPlay();
	SpawnTrailSystem();
	if (LoopingSound && LoopingAttenuation)
	{
		LoopingAudioComponent = UGameplayStatics::SpawnSoundAttached(
			LoopingSound,
			GetRootComponent(),
			FName(),
			GetActorLocation(),
			EAttachLocation::KeepWorldPosition,
			false,
			1.f,
			1.f,
			0.f,
			LoopingAttenuation,
			nullptr,
			false
				);
	}
}

void AProjectileRocket::Destroyed()
{
	if (LoopingAudioComponent && LoopingAudioComponent->IsPlaying())
	{
		LoopingAudioComponent->Stop();
	}
	if (RocketTracerComponent)
	{
		RocketTracerComponent->Deactivate();
	}
		
	Super::Destroyed();
}

void AProjectileRocket::HandleImpactEffectsMulticast()
{
	if (LoopingAudioComponent && LoopingAudioComponent->IsPlaying())
	{
		LoopingAudioComponent->Stop();
	}
}

void AProjectileRocket::OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
                              FVector NormalImpulse, const FHitResult& Hit)
{
	if (OtherActor == GetOwner())
	{
		UE_LOG(LogTemp,Verbose,TEXT("Hit Yourself"));
		return;
	}
	if (!HasAuthority() || bHasImpacted) return;
	bHasImpacted = true;
	const FVector ImpactLocation = Hit.bBlockingHit ? FVector(Hit.ImpactPoint) : GetActorLocation();
	const FVector ImpactNormal = Hit.bBlockingHit ? FVector(Hit.ImpactNormal) : GetActorForwardVector();
	const FRotator ImpactRotation = GetActorRotation();

	SetActorLocation(ImpactLocation);

	if (ExplodeDamageAndEffects(ImpactLocation, ImpactNormal, ImpactRotation))
	{
		StartDestroyTimer();
	}
}
